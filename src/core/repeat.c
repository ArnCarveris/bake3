#include "bake/commands.h"
#include "bake/os.h"

#include "parson.h"
#include <math.h>

#define BAKE_REPEAT_LOC_ENV "BAKE_BUILD_REPORT_LOC"

typedef struct bake_repeat_run_t {
    JSON_Value *report;
    int exit_code;
    bool ok;
    double total_sec;
    double loc_sec;
} bake_repeat_run_t;

static bool bake_repeat_is_value_opt(const char *arg) {
    return !strcmp(arg, "--repeat") || !strcmp(arg, "--warmup") ||
        !strcmp(arg, "--build-json");
}

static const char** bake_repeat_child_argv(
    int argc,
    char *argv[],
    const char *exe,
    const char *report_path)
{
    const char **child = ecs_os_calloc_n(const char*, argc + 4);
    int32_t count = 0;
    child[count++] = exe;

    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            break;
        }
        if (bake_repeat_is_value_opt(argv[i])) {
            i++;
            continue;
        }
        child[count++] = argv[i];
    }

    child[count++] = "--build-json";
    child[count++] = report_path;

    for (; i < argc; i++) {
        child[count++] = argv[i];
    }

    child[count] = NULL;
    return child;
}

static double bake_repeat_number(const JSON_Object *object, const char *path) {
    JSON_Value *value = json_object_dotget_value(object, path);
    if (!value || json_value_get_type(value) != JSONNumber) {
        return 0.0;
    }
    return json_value_get_number(value);
}

static double bake_repeat_build_sec(const bake_repeat_run_t *run) {
    return run->total_sec - run->loc_sec;
}

static int bake_repeat_cmp_double(const void *a, const void *b) {
    double lhs = *(const double*)a;
    double rhs = *(const double*)b;
    return (lhs > rhs) - (lhs < rhs);
}

static int32_t bake_repeat_median_run(
    const bake_repeat_run_t *runs,
    int32_t warmup,
    int32_t count,
    double *median_out)
{
    double *values = ecs_os_malloc_n(double, count);
    for (int32_t i = 0; i < count; i++) {
        values[i] = bake_repeat_build_sec(&runs[warmup + i]);
    }
    qsort(values, (size_t)count, sizeof(double), bake_repeat_cmp_double);
    double median = values[(count - 1) / 2];
    ecs_os_free(values);

    *median_out = median;
    for (int32_t i = 0; i < count; i++) {
        if (bake_repeat_build_sec(&runs[warmup + i]) == median) {
            return warmup + i;
        }
    }
    return warmup;
}

static void bake_repeat_copy_loc(JSON_Object *dst, const JSON_Object *src) {
    JSON_Value *workspace = json_object_dotget_value(src, "totals.loc");
    if (workspace && json_value_get_type(workspace) == JSONObject) {
        json_object_dotset_value(dst, "totals.loc", json_value_deep_copy(workspace));
    }

    JSON_Object *src_projects = json_object_dotget_object(src, "totals.project");
    JSON_Object *dst_projects = json_object_dotget_object(dst, "totals.project");
    if (!src_projects || !dst_projects) {
        return;
    }

    size_t count = json_object_get_count(src_projects);
    for (size_t i = 0; i < count; i++) {
        const char *name = json_object_get_name(src_projects, i);
        JSON_Object *src_project = json_object_get_object(src_projects, name);
        JSON_Object *dst_project = json_object_get_object(dst_projects, name);
        JSON_Value *loc = src_project ? json_object_get_value(src_project, "loc") : NULL;
        if (dst_project && loc) {
            json_object_set_value(dst_project, "loc", json_value_deep_copy(loc));
        }
    }
}

static void bake_repeat_add_summary(
    JSON_Object *report,
    const bake_repeat_run_t *runs,
    int32_t run_count,
    int32_t warmup,
    int32_t count,
    int32_t chosen,
    double median)
{
    JSON_Value *summary_value = json_value_init_object();
    JSON_Object *summary = json_value_get_object(summary_value);
    json_object_set_number(summary, "warmup", warmup);
    json_object_set_number(summary, "count", count);
    if (chosen >= 0) {
        json_object_set_number(summary, "chosen_run", chosen);
        json_object_set_number(summary, "median_sec", median);
    } else {
        json_object_set_null(summary, "chosen_run");
        json_object_set_null(summary, "median_sec");
    }

    JSON_Value *list_value = json_value_init_array();
    JSON_Array *list = json_value_get_array(list_value);
    for (int32_t i = 0; i < run_count; i++) {
        JSON_Value *entry_value = json_value_init_object();
        JSON_Object *entry = json_value_get_object(entry_value);
        json_object_set_number(entry, "run", i);
        json_object_set_boolean(entry, "warmup", i < warmup);
        json_object_set_boolean(entry, "ok", runs[i].ok);
        json_object_set_number(entry, "exit_code", runs[i].exit_code);
        if (runs[i].report) {
            json_object_set_number(entry, "total_sec", runs[i].total_sec);
            json_object_set_number(entry, "loc_sec", runs[i].loc_sec);
        } else {
            json_object_set_null(entry, "total_sec");
            json_object_set_null(entry, "loc_sec");
        }
        json_array_append_value(list, entry_value);
    }
    json_object_set_value(summary, "runs", list_value);
    json_object_set_value(report, "repeat", summary_value);
}

static int bake_repeat_serialize_number(double num, char *buf) {
    char tmp[64];
    int written;
    if (num == floor(num) && fabs(num) < 1e15) {
        written = snprintf(tmp, sizeof(tmp), "%lld", (long long)num);
    } else {
        written = snprintf(tmp, sizeof(tmp), "%.6f", num);
    }
    if (buf) {
        memcpy(buf, tmp, (size_t)written + 1);
    }
    return written;
}

static int bake_repeat_write_report(JSON_Value *report, const char *path) {
    json_set_escape_slashes(0);
    json_set_number_serialization_function(bake_repeat_serialize_number);
    char *content = json_serialize_to_string_pretty(report);
    json_set_number_serialization_function(NULL);
    json_set_escape_slashes(1);
    if (!content) {
        ecs_err("failed to serialize build report '%s'", path);
        return -1;
    }

    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&buf, content);
    ecs_strbuf_appendch(&buf, '\n');
    json_free_serialized_string(content);

    char *text = ecs_strbuf_get(&buf);
    int rc = bake_file_write(path, text);
    ecs_os_free(text);
    if (rc != 0) {
        ecs_err("failed to write build report '%s'", path);
    }
    return rc;
}

static char* bake_repeat_exe(char *argv[]) {
    const char *exe = getenv("BAKE3_EXEC_PATH");
    if (exe && exe[0]) {
        return ecs_os_strdup(exe);
    }
    return ecs_os_strdup(argv[0]);
}

int bake_repeat_build(const bake_options_t *opts, int argc, char *argv[]) {
    int32_t warmup = opts->warmup;
    int32_t count = opts->repeat > 0 ? opts->repeat : 1;
    int32_t run_count = warmup + count;

    char *report_path = bake_path_is_abs(opts->build_json)
        ? ecs_os_strdup(opts->build_json)
        : bake_path_join(opts->cwd, opts->build_json);
    char *exe = bake_repeat_exe(argv);
    const char *loc_env = getenv(BAKE_REPEAT_LOC_ENV);
    char *loc_env_saved = loc_env ? ecs_os_strdup(loc_env) : NULL;

    bake_repeat_run_t *runs = ecs_os_calloc_n(bake_repeat_run_t, run_count);
    int32_t done = 0;
    int32_t failed = -1;
    int rc = -1;

    bake_remove_file_if_exists(report_path);

    for (int32_t i = 0; i < run_count; i++) {
        bool is_warmup = i < warmup;
        char *run_path = flecs_asprintf("%s.run%d.tmp", report_path, i);
        const char **child = bake_repeat_child_argv(argc, argv, exe, run_path);

        if (i == 1) {
            bake_os_setenv(BAKE_REPEAT_LOC_ENV, "0");
        }

        printf("[ repeat] run %d/%d%s\n", i + 1, run_count,
            is_warmup ? " (warm-up)" : "");

        bake_process_result_t result = {0};
        int spawn_rc = bake_proc_run_argv(child, &result);
        ecs_os_free(child);

        bake_repeat_run_t *run = &runs[i];
        run->exit_code = spawn_rc == 0 ? result.exit_code : -1;
        run->report = json_parse_file(run_path);
        bake_remove_file_if_exists(run_path);
        ecs_os_free(run_path);

        JSON_Object *report = json_value_get_object(run->report);
        if (report) {
            run->total_sec = bake_repeat_number(report, "total_sec");
            run->loc_sec = bake_repeat_number(report, "totals.kind.loc");
            run->ok = json_object_get_boolean(report, "ok") == 1;
        }
        run->ok = run->ok && spawn_rc == 0 && result.exit_code == 0 &&
            !result.interrupted;
        done = i + 1;

        if (!run->ok) {
            printf("[ repeat] run %d/%d failed\n", i + 1, run_count);
            failed = i;
            break;
        }

        printf("[ repeat] run %d/%d: %.3fs\n", i + 1, run_count,
            bake_repeat_build_sec(run));
    }

    if (loc_env_saved) {
        bake_os_setenv(BAKE_REPEAT_LOC_ENV, loc_env_saved);
    } else {
        bake_os_unsetenv(BAKE_REPEAT_LOC_ENV);
    }

    if (failed >= 0) {
        JSON_Value *report = runs[failed].report;
        if (report && json_value_get_type(report) == JSONObject) {
            bake_repeat_add_summary(json_value_get_object(report), runs, done,
                warmup, count, -1, 0.0);
            bake_repeat_write_report(report, report_path);
        }
        goto cleanup;
    }

    double median = 0.0;
    int32_t chosen = bake_repeat_median_run(runs, warmup, count, &median);
    JSON_Object *report = json_value_get_object(runs[chosen].report);
    if (chosen != 0) {
        bake_repeat_copy_loc(report, json_value_get_object(runs[0].report));
    }
    bake_repeat_add_summary(report, runs, done, warmup, count, chosen, median);

    printf("[ repeat] median of %d run%s: %.3fs (run %d)\n",
        count, count == 1 ? "" : "s", median, chosen + 1);

    if (bake_repeat_write_report(runs[chosen].report, report_path) == 0) {
        rc = 0;
    }

cleanup:
    for (int32_t i = 0; i < run_count; i++) {
        if (runs[i].report) {
            json_value_free(runs[i].report);
        }
    }
    ecs_os_free(runs);
    ecs_os_free(loc_env_saved);
    ecs_os_free(exe);
    ecs_os_free(report_path);
    return rc;
}
