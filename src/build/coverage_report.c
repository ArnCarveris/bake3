#include "build_internal.h"
#include "bake/environment.h"
#include "bake/model.h"
#include "bake/os.h"
#include "common/harness_util.h"

#include <time.h>

typedef struct bake_cov_count_t {
    int64_t count;
    int64_t covered;
} bake_cov_count_t;

typedef struct bake_cov_totals_t {
    bake_cov_count_t lines;
    bake_cov_count_t functions;
    bake_cov_count_t branches;
} bake_cov_totals_t;

typedef struct bake_cov_line_t {
    int64_t line;
    uint64_t hits;
} bake_cov_line_t;

typedef struct bake_cov_branch_t {
    int64_t line;
    int32_t taken;
    int32_t total;
} bake_cov_branch_t;

typedef struct bake_cov_fn_t {
    char *name;
    int64_t line;
    uint64_t hits;
} bake_cov_fn_t;

typedef struct bake_cov_file_t {
    char *path;
    char *display;
    bake_cov_totals_t totals;
    ecs_vec_t lines;
    ecs_vec_t branches;
    ecs_vec_t fns;
} bake_cov_file_t;

typedef struct bake_cov_project_t {
    const bake_project_cfg_t *cfg;
    char *exe;
    char *build_root;
    char *resolved_path;
} bake_cov_project_t;

typedef struct bake_cov_report_t {
    bake_context_t *ctx;
    ecs_vec_t projects;
    ecs_vec_t profiles;
    ecs_vec_t files;
    bake_cov_totals_t totals;
    char *out_dir;
    char *title;
} bake_cov_report_t;

char* bake_coverage_report_dir(void) {
    const char *home = bake_env_home();
    if (bake_env_is_local() && home && home[0]) {
        return bake_path_join(home, "coverage_report");
    }

    char *cwd = bake_os_getcwd();
    if (!cwd) {
        return NULL;
    }
    char *dir = bake_path_join3(cwd, ".bake", "coverage_report");
    ecs_os_free(cwd);
    return dir;
}

static void bake_cov_file_free(bake_cov_file_t *file) {
    ecs_os_free(file->path);
    ecs_os_free(file->display);
    ecs_vec_fini_t(NULL, &file->lines, bake_cov_line_t);
    ecs_vec_fini_t(NULL, &file->branches, bake_cov_branch_t);
    int32_t fn_count = ecs_vec_count(&file->fns);
    bake_cov_fn_t *fns = ecs_vec_first_t(&file->fns, bake_cov_fn_t);
    for (int32_t i = 0; i < fn_count; i++) {
        ecs_os_free(fns[i].name);
    }
    ecs_vec_fini_t(NULL, &file->fns, bake_cov_fn_t);
    ecs_os_free(file);
}

static void bake_cov_report_fini(bake_cov_report_t *report) {
    int32_t project_count = ecs_vec_count(&report->projects);
    bake_cov_project_t *projects = ecs_vec_first_t(&report->projects, bake_cov_project_t);
    for (int32_t i = 0; i < project_count; i++) {
        ecs_os_free(projects[i].exe);
        ecs_os_free(projects[i].build_root);
        ecs_os_free(projects[i].resolved_path);
    }
    ecs_vec_fini_t(NULL, &report->projects, bake_cov_project_t);

    int32_t profile_count = ecs_vec_count(&report->profiles);
    char **profiles = ecs_vec_first_t(&report->profiles, char*);
    for (int32_t i = 0; i < profile_count; i++) {
        ecs_os_free(profiles[i]);
    }
    ecs_vec_fini_t(NULL, &report->profiles, char*);

    int32_t file_count = ecs_vec_count(&report->files);
    bake_cov_file_t **files = ecs_vec_first_t(&report->files, bake_cov_file_t*);
    for (int32_t i = 0; i < file_count; i++) {
        bake_cov_file_free(files[i]);
    }
    ecs_vec_fini_t(NULL, &report->files, bake_cov_file_t*);

    ecs_os_free(report->out_dir);
    ecs_os_free(report->title);
}

static bool bake_cov_is_profile(const char *name) {
    size_t len = strlen(name);
    return len > 8 && !strcmp(name + len - 8, ".profraw");
}

static int32_t bake_cov_collect_profiles(
    bake_cov_report_t *report,
    const char *coverage_dir)
{
    if (!bake_path_is_dir(coverage_dir)) {
        return 0;
    }

    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    if (bake_dir_list(coverage_dir, &entries, &entry_count) != 0) {
        return 0;
    }

    int32_t found = 0;
    for (int32_t i = 0; i < entry_count; i++) {
        if (!entries[i].is_dir && bake_cov_is_profile(entries[i].name)) {
            *ecs_vec_append_t(NULL, &report->profiles, char*) =
                ecs_os_strdup(entries[i].path);
            found ++;
        }
    }

    bake_dir_entries_free(entries, entry_count);
    return found;
}

static int bake_cov_project_compare(const void *a, const void *b) {
    const bake_cov_project_t *pa = a;
    const bake_cov_project_t *pb = b;
    return strcmp(pa->cfg->id, pb->cfg->id);
}

static bool bake_cov_project_selected(
    const bake_project_cfg_t *cfg,
    const char *root,
    const char *target_id)
{
    if (root) {
        char *resolved = bake_path_resolve(cfg->path);
        bool selected = bake_path_has_prefix_normalized(
            resolved ? resolved : cfg->path, root, NULL);
        ecs_os_free(resolved);
        return selected;
    }
    return target_id && !strcmp(cfg->id, target_id);
}

static int bake_cov_collect_projects(
    bake_cov_report_t *report,
    const char *root,
    const char *target_id)
{
    ecs_world_t *world = report->ctx->world;
    const char *mode = report->ctx->opts.mode;

    ecs_iter_t it = ecs_each_id(world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        const BakeProject *projects = ecs_field(&it, BakeProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const bake_project_cfg_t *cfg = projects[i].cfg;
            if (projects[i].external || !cfg || !cfg->path ||
                cfg->kind != BAKE_PROJECT_TEST ||
                !bake_cov_project_selected(cfg, root, target_id))
            {
                continue;
            }

            char *coverage_dir = bake_coverage_dir(cfg, mode);
            int32_t found = coverage_dir
                ? bake_cov_collect_profiles(report, coverage_dir)
                : 0;
            ecs_os_free(coverage_dir);
            if (!found) {
                continue;
            }

            char *build_root = bake_project_build_root(cfg->path, cfg->id, mode);
            char *artefact = bake_project_cfg_artefact_name(cfg);
            char *exe = build_root && artefact
                ? bake_path_join(build_root, artefact)
                : NULL;
            ecs_os_free(artefact);

            if (!exe || !bake_path_exists(exe)) {
                ecs_warn("skipping coverage data of %s: test binary %s not found",
                    cfg->id, exe ? exe : "<unknown>");
                ecs_os_free(exe);
                ecs_os_free(build_root);
                continue;
            }

            bake_cov_project_t *project = ecs_vec_append_t(
                NULL, &report->projects, bake_cov_project_t);
            project->cfg = cfg;
            project->exe = exe;
            project->build_root = build_root;
            project->resolved_path = bake_path_resolve(cfg->path);
        }
    }

    int32_t count = ecs_vec_count(&report->projects);
    if (count > 1) {
        qsort(ecs_vec_first(&report->projects), (size_t)count,
            sizeof(bake_cov_project_t), bake_cov_project_compare);
    }
    return count;
}

static int bake_cov_run_tool(
    const char *const *argv,
    const char *stdout_path)
{
    bake_process_stdio_t stdio_cfg = { .stdout_path = stdout_path };
    bake_process_result_t result = {0};
    if (bake_proc_run(argv, &stdio_cfg, &result) != 0) {
        ecs_err("failed to start %s", argv[0]);
        return -1;
    }
    if (result.exit_code != 0) {
        ecs_err("%s exited with code %d", argv[0], result.exit_code);
        return -1;
    }
    return 0;
}

static int bake_cov_merge_and_export(
    bake_cov_report_t *report,
    const char *profdata,
    const char *lcov)
{
    int32_t profile_count = ecs_vec_count(&report->profiles);
    char **profiles = ecs_vec_first_t(&report->profiles, char*);

    ecs_vec_t argv;
    ecs_vec_init_t(NULL, &argv, const char*, profile_count + 8);
    *ecs_vec_append_t(NULL, &argv, const char*) = report->ctx->coverage_profdata;
    *ecs_vec_append_t(NULL, &argv, const char*) = "merge";
    *ecs_vec_append_t(NULL, &argv, const char*) = "-sparse";
    *ecs_vec_append_t(NULL, &argv, const char*) = "-failure-mode=all";
    *ecs_vec_append_t(NULL, &argv, const char*) = "-o";
    *ecs_vec_append_t(NULL, &argv, const char*) = profdata;
    for (int32_t i = 0; i < profile_count; i++) {
        *ecs_vec_append_t(NULL, &argv, const char*) = profiles[i];
    }
    *ecs_vec_append_t(NULL, &argv, const char*) = NULL;

    int rc = bake_cov_run_tool(ecs_vec_first_t(&argv, const char*), NULL);
    ecs_vec_clear(&argv);
    if (rc != 0) {
        ecs_vec_fini_t(NULL, &argv, const char*);
        return -1;
    }

    char *profile_arg = flecs_asprintf("-instr-profile=%s", profdata);
    *ecs_vec_append_t(NULL, &argv, const char*) = report->ctx->coverage_cov;
    *ecs_vec_append_t(NULL, &argv, const char*) = "export";
    *ecs_vec_append_t(NULL, &argv, const char*) = "-format=lcov";
    *ecs_vec_append_t(NULL, &argv, const char*) = profile_arg;

    int32_t project_count = ecs_vec_count(&report->projects);
    bake_cov_project_t *projects = ecs_vec_first_t(&report->projects, bake_cov_project_t);
    for (int32_t i = 0; i < project_count; i++) {
        if (i) {
            *ecs_vec_append_t(NULL, &argv, const char*) = "-object";
        }
        *ecs_vec_append_t(NULL, &argv, const char*) = projects[i].exe;
    }
    *ecs_vec_append_t(NULL, &argv, const char*) = NULL;

    rc = bake_cov_run_tool(ecs_vec_first_t(&argv, const char*), lcov);
    ecs_os_free(profile_arg);
    ecs_vec_fini_t(NULL, &argv, const char*);
    return rc;
}

static bool bake_cov_excluded(const bake_cov_report_t *report, const char *path) {
    int32_t count = ecs_vec_count(&report->projects);
    const bake_cov_project_t *projects = ecs_vec_first_t(&report->projects, bake_cov_project_t);
    for (int32_t i = 0; i < count; i++) {
        const bake_cov_project_t *p = &projects[i];
        if (bake_path_has_prefix_normalized(path, p->cfg->path, NULL) ||
            (p->resolved_path && bake_path_has_prefix_normalized(path, p->resolved_path, NULL)) ||
            (p->build_root && bake_path_has_prefix_normalized(path, p->build_root, NULL)))
        {
            return true;
        }
    }
    return false;
}

static const char* bake_cov_fn_name(const char *name) {
    const char *colon = strrchr(name, ':');
    const char *semi = strrchr(name, ';');
    const char *sep = colon;
    if (semi && (!sep || semi > sep)) {
        sep = semi;
    }
    return sep ? sep + 1 : name;
}

static char* bake_cov_display_path(const char *path, const char *cwd) {
    size_t len = 0;
    if (cwd && bake_path_has_prefix_normalized(path, cwd, &len)) {
        const char *rel = path + len;
        while (*rel && bake_path_is_sep(*rel)) {
            rel ++;
        }
        if (*rel) {
            return ecs_os_strdup(rel);
        }
    }
    return ecs_os_strdup(path);
}

static void bake_cov_add_totals(bake_cov_totals_t *dst, const bake_cov_totals_t *src) {
    dst->lines.count += src->lines.count;
    dst->lines.covered += src->lines.covered;
    dst->functions.count += src->functions.count;
    dst->functions.covered += src->functions.covered;
    dst->branches.count += src->branches.count;
    dst->branches.covered += src->branches.covered;
}

static void bake_cov_parse_record_line(bake_cov_file_t *file, char *line) {
    char *end = NULL;
    if (!strncmp(line, "FN:", 3)) {
        int64_t fn_line = strtoll(line + 3, &end, 10);
        if (end && *end == ',') {
            bake_cov_fn_t *fn = ecs_vec_append_t(NULL, &file->fns, bake_cov_fn_t);
            fn->name = ecs_os_strdup(bake_cov_fn_name(end + 1));
            fn->line = fn_line;
            fn->hits = 0;
        }
    } else if (!strncmp(line, "FNDA:", 5)) {
        uint64_t hits = strtoull(line + 5, &end, 10);
        if (end && *end == ',') {
            const char *name = bake_cov_fn_name(end + 1);
            int32_t count = ecs_vec_count(&file->fns);
            bake_cov_fn_t *fns = ecs_vec_first_t(&file->fns, bake_cov_fn_t);
            for (int32_t i = 0; i < count; i++) {
                if (!strcmp(fns[i].name, name)) {
                    fns[i].hits += hits;
                    break;
                }
            }
        }
    } else if (!strncmp(line, "DA:", 3)) {
        int64_t da_line = strtoll(line + 3, &end, 10);
        if (end && *end == ',') {
            bake_cov_line_t *l = ecs_vec_append_t(NULL, &file->lines, bake_cov_line_t);
            l->line = da_line;
            l->hits = strtoull(end + 1, NULL, 10);
        }
    } else if (!strncmp(line, "BRDA:", 5)) {
        int64_t br_line = strtoll(line + 5, &end, 10);
        const char *taken = strrchr(line, ',');
        if (end && *end == ',' && taken) {
            bool hit = taken[1] != '-' && strtoull(taken + 1, NULL, 10) > 0;
            int32_t count = ecs_vec_count(&file->branches);
            bake_cov_branch_t *last = count
                ? ecs_vec_get_t(&file->branches, bake_cov_branch_t, count - 1)
                : NULL;
            if (!last || last->line != br_line) {
                last = ecs_vec_append_t(NULL, &file->branches, bake_cov_branch_t);
                last->line = br_line;
                last->taken = 0;
                last->total = 0;
            }
            last->total ++;
            last->taken += hit ? 1 : 0;
        }
    } else if (!strncmp(line, "LF:", 3)) {
        file->totals.lines.count = strtoll(line + 3, NULL, 10);
    } else if (!strncmp(line, "LH:", 3)) {
        file->totals.lines.covered = strtoll(line + 3, NULL, 10);
    } else if (!strncmp(line, "FNF:", 4)) {
        file->totals.functions.count = strtoll(line + 4, NULL, 10);
    } else if (!strncmp(line, "FNH:", 4)) {
        file->totals.functions.covered = strtoll(line + 4, NULL, 10);
    } else if (!strncmp(line, "BRF:", 4)) {
        file->totals.branches.count = strtoll(line + 4, NULL, 10);
    } else if (!strncmp(line, "BRH:", 4)) {
        file->totals.branches.covered = strtoll(line + 4, NULL, 10);
    }
}

static int bake_cov_file_compare(const void *a, const void *b) {
    const bake_cov_file_t *fa = *(bake_cov_file_t* const*)a;
    const bake_cov_file_t *fb = *(bake_cov_file_t* const*)b;
    return strcmp(fa->display, fb->display);
}

static int bake_cov_parse_lcov(bake_cov_report_t *report, const char *lcov_path) {
    char *content = bake_file_read(lcov_path, NULL);
    if (!content) {
        ecs_err("failed to read %s", lcov_path);
        return -1;
    }

    bake_cov_file_t *file = NULL;
    char *cursor = content;
    while (*cursor) {
        char *line = cursor;
        char *nl = strchr(cursor, '\n');
        if (nl) {
            *nl = '\0';
            cursor = nl + 1;
        } else {
            cursor += strlen(cursor);
        }
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }

        if (!strncmp(line, "SF:", 3)) {
            if (file) {
                bake_cov_file_free(file);
            }
            file = ecs_os_calloc_t(bake_cov_file_t);
            file->path = ecs_os_strdup(line + 3);
            ecs_vec_init_t(NULL, &file->lines, bake_cov_line_t, 0);
            ecs_vec_init_t(NULL, &file->branches, bake_cov_branch_t, 0);
            ecs_vec_init_t(NULL, &file->fns, bake_cov_fn_t, 0);
        } else if (!strcmp(line, "end_of_record")) {
            if (file && !bake_cov_excluded(report, file->path) &&
                (file->totals.lines.count || file->totals.functions.count))
            {
                file->display = bake_cov_display_path(file->path, report->ctx->opts.cwd);
                bake_cov_add_totals(&report->totals, &file->totals);
                *ecs_vec_append_t(NULL, &report->files, bake_cov_file_t*) = file;
            } else if (file) {
                bake_cov_file_free(file);
            }
            file = NULL;
        } else if (file) {
            bake_cov_parse_record_line(file, line);
        }
    }

    if (file) {
        bake_cov_file_free(file);
    }
    ecs_os_free(content);

    int32_t count = ecs_vec_count(&report->files);
    if (count > 1) {
        qsort(ecs_vec_first(&report->files), (size_t)count,
            sizeof(bake_cov_file_t*), bake_cov_file_compare);
    }
    return 0;
}

static void bake_cov_json_str(ecs_strbuf_t *buf, const char *str, size_t len) {
    ecs_strbuf_appendch(buf, '"');
    const char *run = str;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)str[i];
        const char *esc = NULL;
        char tmp[8];
        if (ch == '"') esc = "\\\"";
        else if (ch == '\\') esc = "\\\\";
        else if (ch == '\n') esc = "\\n";
        else if (ch == '\t') esc = "\\t";
        else if (ch == '\r') esc = "\\r";
        else if (ch == '/' && i && str[i - 1] == '<') esc = "\\/";
        else if (ch < 0x20) {
            ecs_os_snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
            esc = tmp;
        }
        if (esc) {
            ecs_strbuf_appendstrn(buf, run, (int32_t)(&str[i] - run));
            ecs_strbuf_appendstr(buf, esc);
            run = &str[i + 1];
        }
    }
    ecs_strbuf_appendstrn(buf, run, (int32_t)(&str[len] - run));
    ecs_strbuf_appendch(buf, '"');
}

static void bake_cov_json_cstr(ecs_strbuf_t *buf, const char *str) {
    bake_cov_json_str(buf, str ? str : "", str ? strlen(str) : 0);
}

static double bake_cov_percent(const bake_cov_count_t *c) {
    return c->count ? ((double)c->covered * 100.0) / (double)c->count : 100.0;
}

static void bake_cov_json_count(ecs_strbuf_t *buf, const bake_cov_count_t *c) {
    ecs_strbuf_append(buf, "{\"count\": %lld, \"covered\": %lld, \"percent\": %.2f}",
        (long long)c->count, (long long)c->covered, bake_cov_percent(c));
}

static void bake_cov_json_totals(
    ecs_strbuf_t *buf,
    const char *indent,
    const bake_cov_totals_t *t)
{
    ecs_strbuf_append(buf, "%s\"lines\": ", indent);
    bake_cov_json_count(buf, &t->lines);
    ecs_strbuf_append(buf, ",\n%s\"functions\": ", indent);
    bake_cov_json_count(buf, &t->functions);
    ecs_strbuf_append(buf, ",\n%s\"branches\": ", indent);
    bake_cov_json_count(buf, &t->branches);
}

static void bake_cov_json_pair(ecs_strbuf_t *buf, const bake_cov_count_t *c) {
    ecs_strbuf_append(buf, "[%lld,%lld]", (long long)c->covered, (long long)c->count);
}

static void bake_cov_timestamp(char *stamp, size_t size) {
    time_t now = time(NULL);
    struct tm tm_utc;
#if defined(_WIN32)
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    strftime(stamp, size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static int bake_cov_write_file_data(
    const bake_cov_report_t *report,
    const bake_cov_file_t *file,
    int32_t index,
    const char *files_dir)
{
    BAKE_UNUSED(report);
    size_t source_len = 0;
    char *source = bake_file_read(file->path, &source_len);

    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_append(&buf, "bakeCoverageFile(%d, {\"source\": ", index);
    if (source) {
        bake_cov_json_str(&buf, source, source_len);
    } else {
        ecs_strbuf_appendstr(&buf, "null");
    }

    ecs_strbuf_appendstr(&buf, ",\n\"lines\": [");
    int32_t count = ecs_vec_count(&file->lines);
    const bake_cov_line_t *lines = ecs_vec_first_t(&file->lines, bake_cov_line_t);
    for (int32_t i = 0; i < count; i++) {
        ecs_strbuf_append(&buf, "%s[%lld,%llu]", i ? "," : "",
            (long long)lines[i].line, (unsigned long long)lines[i].hits);
    }

    ecs_strbuf_appendstr(&buf, "],\n\"branches\": [");
    count = ecs_vec_count(&file->branches);
    const bake_cov_branch_t *branches = ecs_vec_first_t(&file->branches, bake_cov_branch_t);
    for (int32_t i = 0; i < count; i++) {
        ecs_strbuf_append(&buf, "%s[%lld,%d,%d]", i ? "," : "",
            (long long)branches[i].line, branches[i].taken, branches[i].total);
    }

    ecs_strbuf_appendstr(&buf, "],\n\"functions\": [");
    count = ecs_vec_count(&file->fns);
    const bake_cov_fn_t *fns = ecs_vec_first_t(&file->fns, bake_cov_fn_t);
    for (int32_t i = 0; i < count; i++) {
        ecs_strbuf_appendstr(&buf, i ? ",[" : "[");
        bake_cov_json_cstr(&buf, fns[i].name);
        ecs_strbuf_append(&buf, ",%lld,%llu]",
            (long long)fns[i].line, (unsigned long long)fns[i].hits);
    }
    ecs_strbuf_appendstr(&buf, "]});\n");

    char *name = flecs_asprintf("%d.js", index);
    char *path = bake_path_join(files_dir, name);
    char *content = ecs_strbuf_get(&buf);
    int rc = bake_file_write(path, content);
    ecs_os_free(content);
    ecs_os_free(path);
    ecs_os_free(name);
    ecs_os_free(source);
    return rc;
}

static char* bake_cov_index_data(const bake_cov_report_t *report, const char *stamp) {
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&buf, "{\"title\": ");
    bake_cov_json_cstr(&buf, report->title);
    ecs_strbuf_appendstr(&buf, ", \"timestamp\": ");
    bake_cov_json_cstr(&buf, stamp);
    ecs_strbuf_appendstr(&buf, ", \"projects\": [");
    int32_t project_count = ecs_vec_count(&report->projects);
    const bake_cov_project_t *projects = ecs_vec_first_t(&report->projects, bake_cov_project_t);
    for (int32_t i = 0; i < project_count; i++) {
        if (i) ecs_strbuf_appendstr(&buf, ", ");
        bake_cov_json_cstr(&buf, projects[i].cfg->id);
    }

    ecs_strbuf_appendstr(&buf, "],\n\"files\": [");
    int32_t file_count = ecs_vec_count(&report->files);
    bake_cov_file_t **files = ecs_vec_first_t(&report->files, bake_cov_file_t*);
    for (int32_t i = 0; i < file_count; i++) {
        const bake_cov_file_t *file = files[i];
        ecs_strbuf_appendstr(&buf, i ? ",\n[" : "\n[");
        bake_cov_json_cstr(&buf, file->display);
        ecs_strbuf_appendstr(&buf, ",");
        bake_cov_json_pair(&buf, &file->totals.lines);
        ecs_strbuf_appendstr(&buf, ",");
        bake_cov_json_pair(&buf, &file->totals.functions);
        ecs_strbuf_appendstr(&buf, ",");
        bake_cov_json_pair(&buf, &file->totals.branches);
        ecs_strbuf_appendstr(&buf, "]");
    }
    ecs_strbuf_appendstr(&buf, "]}");
    return ecs_strbuf_get(&buf);
}

static int bake_cov_write_index(
    const bake_cov_report_t *report,
    const char *stamp)
{
    char *tmpl_path = bake_harness_template_file(report->ctx, "coverage_report.html");
    if (!tmpl_path) {
        return -1;
    }
    char *tmpl = bake_file_read(tmpl_path, NULL);
    ecs_os_free(tmpl_path);
    if (!tmpl) {
        ecs_err("failed to read coverage report template");
        return -1;
    }

    static const char *placeholder = "/*BAKE_COVERAGE_DATA*/null";
    char *at = strstr(tmpl, placeholder);
    if (!at) {
        ecs_err("coverage report template has no data placeholder");
        ecs_os_free(tmpl);
        return -1;
    }

    char *data = bake_cov_index_data(report, stamp);
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_appendstrn(&buf, tmpl, (int32_t)(at - tmpl));
    ecs_strbuf_appendstr(&buf, data);
    ecs_strbuf_appendstr(&buf, at + strlen(placeholder));
    char *content = ecs_strbuf_get(&buf);

    char *path = bake_path_join(report->out_dir, "index.html");
    int rc = bake_file_write(path, content);
    ecs_os_free(path);
    ecs_os_free(content);
    ecs_os_free(data);
    ecs_os_free(tmpl);
    return rc;
}

static void bake_cov_json_ranges(ecs_strbuf_t *buf, const bake_cov_file_t *file) {
    int32_t count = ecs_vec_count(&file->lines);
    const bake_cov_line_t *lines = ecs_vec_first_t(&file->lines, bake_cov_line_t);
    bool in_range = false;
    bool first = true;
    int64_t start = 0, end = 0;
    for (int32_t i = 0; i <= count; i++) {
        bool uncovered = i < count && !lines[i].hits;
        if (uncovered) {
            if (!in_range) {
                start = lines[i].line;
                in_range = true;
            }
            end = lines[i].line;
            continue;
        }
        if (in_range) {
            ecs_strbuf_append(buf, "%s[%lld, %lld]", first ? "" : ", ",
                (long long)start, (long long)end);
            first = false;
            in_range = false;
        }
    }
}

static int bake_cov_write_json(const bake_cov_report_t *report, const char *stamp) {
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&buf, "{\n  \"project\": ");
    bake_cov_json_cstr(&buf, report->title);
    ecs_strbuf_appendstr(&buf, ",\n  \"timestamp\": ");
    bake_cov_json_cstr(&buf, stamp);
    ecs_strbuf_appendstr(&buf, ",\n  \"projects\": [");
    int32_t project_count = ecs_vec_count(&report->projects);
    const bake_cov_project_t *projects = ecs_vec_first_t(&report->projects, bake_cov_project_t);
    for (int32_t i = 0; i < project_count; i++) {
        if (i) ecs_strbuf_appendstr(&buf, ", ");
        bake_cov_json_cstr(&buf, projects[i].cfg->id);
    }
    ecs_strbuf_appendstr(&buf, "],\n");
    bake_cov_json_totals(&buf, "  ", &report->totals);
    ecs_strbuf_appendstr(&buf, ",\n  \"files\": [");

    int32_t file_count = ecs_vec_count(&report->files);
    bake_cov_file_t **files = ecs_vec_first_t(&report->files, bake_cov_file_t*);
    for (int32_t i = 0; i < file_count; i++) {
        const bake_cov_file_t *file = files[i];
        ecs_strbuf_appendstr(&buf, i ? ",\n    {\"file\": " : "\n    {\"file\": ");
        bake_cov_json_cstr(&buf, file->path);
        ecs_strbuf_appendstr(&buf, ",\n");
        bake_cov_json_totals(&buf, "     ", &file->totals);
        ecs_strbuf_appendstr(&buf, ",\n     \"uncovered_lines\": [");
        bake_cov_json_ranges(&buf, file);
        ecs_strbuf_appendstr(&buf, "],\n     \"uncovered_functions\": [");
        int32_t fn_count = ecs_vec_count(&file->fns);
        const bake_cov_fn_t *fns = ecs_vec_first_t(&file->fns, bake_cov_fn_t);
        bool first = true;
        for (int32_t f = 0; f < fn_count; f++) {
            if (fns[f].hits) {
                continue;
            }
            ecs_strbuf_appendstr(&buf, first ? "{\"name\": " : ", {\"name\": ");
            bake_cov_json_cstr(&buf, fns[f].name);
            ecs_strbuf_append(&buf, ", \"line\": %lld}", (long long)fns[f].line);
            first = false;
        }
        ecs_strbuf_appendstr(&buf, "]}");
    }
    ecs_strbuf_appendstr(&buf, file_count ? "\n  ]\n}\n" : "]\n}\n");

    char *content = ecs_strbuf_get(&buf);
    char *path = bake_path_join(report->out_dir, "coverage.json");
    int rc = bake_file_write(path, content);
    ecs_os_free(path);
    ecs_os_free(content);
    return rc;
}

typedef struct bake_cov_dir_t {
    char *path;
    bake_cov_totals_t totals;
} bake_cov_dir_t;

static int bake_cov_dir_compare(const void *a, const void *b) {
    return strcmp(((const bake_cov_dir_t*)a)->path, ((const bake_cov_dir_t*)b)->path);
}

static void bake_cov_print_row(int width, const char *name, const bake_cov_totals_t *t) {
    char lines[48], fns[48], branches[48];
    ecs_os_snprintf(lines, sizeof(lines), "%6.2f%% %7lld/%-7lld",
        bake_cov_percent(&t->lines), (long long)t->lines.covered, (long long)t->lines.count);
    ecs_os_snprintf(fns, sizeof(fns), "%6.2f%% %5lld/%-5lld",
        bake_cov_percent(&t->functions), (long long)t->functions.covered, (long long)t->functions.count);
    ecs_os_snprintf(branches, sizeof(branches), "%6.2f%% %7lld/%-7lld",
        bake_cov_percent(&t->branches), (long long)t->branches.covered, (long long)t->branches.count);
    printf("  %-*s  %s  %s  %s\n", width, name, lines, fns, branches);
}

static void bake_cov_print_summary(const bake_cov_report_t *report) {
    int32_t project_count = ecs_vec_count(&report->projects);
    const bake_cov_project_t *projects = ecs_vec_first_t(&report->projects, bake_cov_project_t);
    printf("coverage of %d test project%s:", project_count, project_count == 1 ? "" : "s");
    for (int32_t i = 0; i < project_count; i++) {
        printf("%s %s", i ? "," : "", projects[i].cfg->id);
    }
    printf("\n\n");

    ecs_vec_t dirs;
    ecs_vec_init_t(NULL, &dirs, bake_cov_dir_t, 0);
    int32_t file_count = ecs_vec_count(&report->files);
    bake_cov_file_t **files = ecs_vec_first_t(&report->files, bake_cov_file_t*);
    for (int32_t i = 0; i < file_count; i++) {
        char *path = bake_path_dirname(files[i]->display);
        if (!path[0]) {
            ecs_os_free(path);
            path = ecs_os_strdup(".");
        }
        int32_t dir_count = ecs_vec_count(&dirs);
        bake_cov_dir_t *dir = NULL;
        for (int32_t d = 0; d < dir_count; d++) {
            bake_cov_dir_t *cur = ecs_vec_get_t(&dirs, bake_cov_dir_t, d);
            if (!strcmp(cur->path, path)) {
                dir = cur;
                break;
            }
        }
        if (dir) {
            ecs_os_free(path);
        } else {
            dir = ecs_vec_append_t(NULL, &dirs, bake_cov_dir_t);
            dir->path = path;
            memset(&dir->totals, 0, sizeof(dir->totals));
        }
        bake_cov_add_totals(&dir->totals, &files[i]->totals);
    }

    int32_t dir_count = ecs_vec_count(&dirs);
    bake_cov_dir_t *dir_array = ecs_vec_first_t(&dirs, bake_cov_dir_t);
    if (dir_count > 1) {
        qsort(dir_array, (size_t)dir_count, sizeof(bake_cov_dir_t), bake_cov_dir_compare);
    }

    int width = (int)strlen("directory");
    for (int32_t d = 0; d < dir_count; d++) {
        int len = (int)strlen(dir_array[d].path);
        width = len > width ? len : width;
    }

    printf("  %-*s  %-23s  %-19s  %-23s\n", width, "directory", "lines", "functions", "branches");
    for (int32_t d = 0; d < dir_count; d++) {
        bake_cov_print_row(width, dir_array[d].path, &dir_array[d].totals);
        ecs_os_free(dir_array[d].path);
    }
    ecs_vec_fini_t(NULL, &dirs, bake_cov_dir_t);

    printf("\n");
    bake_cov_print_row(width, "total", &report->totals);
    printf("\n");
}

int bake_coverage_report_generate(bake_context_t *ctx, const char *target_path) {
    bake_cov_report_t report = { .ctx = ctx };
    ecs_vec_init_t(NULL, &report.projects, bake_cov_project_t, 0);
    ecs_vec_init_t(NULL, &report.profiles, char*, 0);
    ecs_vec_init_t(NULL, &report.files, bake_cov_file_t*, 0);

    int rc = -1;
    char *root = NULL;
    char *profdata = NULL;
    char *lcov = NULL;
    char *files_dir = NULL;
    const char *target_id = NULL;

    if (target_path) {
        root = bake_path_resolve(target_path);
    } else if (ctx->opts.target && ctx->opts.target[0]) {
        target_id = ctx->opts.target;
    } else {
        root = bake_path_resolve(ctx->opts.cwd);
    }

    if (!bake_cov_collect_projects(&report, root, target_id)) {
        ecs_err("no coverage data found for %s, run tests of a project "
            "built with --coverage first",
            ctx->opts.target ? ctx->opts.target : "current directory");
        goto cleanup;
    }

    if (bake_coverage_init_tools(ctx) != 0) {
        goto cleanup;
    }

    report.out_dir = bake_coverage_report_dir();
    if (!report.out_dir || bake_os_mkdirs(report.out_dir) != 0) {
        ecs_err("failed to create coverage report directory");
        goto cleanup;
    }

    const char *title_src = target_id ? target_id : root;
    report.title = target_id ? ecs_os_strdup(target_id) : bake_path_basename(title_src);

    profdata = bake_path_join(report.out_dir, "coverage.profdata");
    lcov = bake_path_join(report.out_dir, "coverage.lcov");
    if (bake_cov_merge_and_export(&report, profdata, lcov) != 0) {
        goto cleanup;
    }

    if (bake_cov_parse_lcov(&report, lcov) != 0) {
        goto cleanup;
    }

    files_dir = bake_path_join(report.out_dir, "files");
    if (bake_path_exists(files_dir) && bake_os_rmtree(files_dir) != 0) {
        goto cleanup;
    }
    if (bake_os_mkdirs(files_dir) != 0) {
        goto cleanup;
    }

    char stamp[64] = {0};
    bake_cov_timestamp(stamp, sizeof(stamp));

    int32_t file_count = ecs_vec_count(&report.files);
    bake_cov_file_t **files = ecs_vec_first_t(&report.files, bake_cov_file_t*);
    for (int32_t i = 0; i < file_count; i++) {
        if (bake_cov_write_file_data(&report, files[i], i, files_dir) != 0) {
            goto cleanup;
        }
    }

    if (bake_cov_write_index(&report, stamp) != 0 ||
        bake_cov_write_json(&report, stamp) != 0)
    {
        goto cleanup;
    }

    bake_cov_print_summary(&report);
    char *index = bake_path_join(report.out_dir, "index.html");
    printf("report: %s\n", index);
    ecs_os_free(index);
    rc = 0;

cleanup:
    ecs_os_free(root);
    ecs_os_free(profdata);
    ecs_os_free(lcov);
    ecs_os_free(files_dir);
    bake_cov_report_fini(&report);
    return rc;
}
