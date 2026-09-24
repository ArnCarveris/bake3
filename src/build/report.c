#include "bake/build_report.h"
#include "bake/environment.h"
#include "bake/os.h"

#include <flecs.h>
#include <math.h>
#include <time.h>

typedef struct bake_report_step_t {
    char *name;
    char *kind;
    char *project;
    char *object;
    char *error;
    int32_t parent;
    double start_sec;
    double duration_sec;
    bool ok;
    bool open;
    bool has_loc;
    int32_t loc_files;
    int32_t loc_code;
    int32_t loc_comment;
    int32_t loc_blank;
    char *loc_by_language;
    char *loc_by_file;
} bake_report_step_t;

struct bake_build_report_t {
    char *path;
    char *timestamp;
    char *cfg;
    char *target;
    char *environment;
    char *git_sha;
    char *git_branch;
    bool git_dirty;
    bool git_present;
    const char *host_os;
    const char *host_arch;
    int32_t cpu_count;
    ecs_time_t start;
    ecs_os_mutex_t lock;
    bake_report_step_t *steps;
    int32_t count;
    int32_t capacity;
    int32_t current;
    bool written;
    bool workspace_has_loc;
    int32_t workspace_loc_files;
    int32_t workspace_loc_code;
    int32_t workspace_loc_comment;
    int32_t workspace_loc_blank;
    char *workspace_loc_by_language;
    char *loc_note;
};

typedef struct bake_report_project_total_t {
    const char *project;
    double total_sec;
    double compile_sec;
    double link_sec;
    int32_t files;
    bool has_loc;
    int32_t loc_files;
    int32_t loc_code;
    int32_t loc_comment;
    int32_t loc_blank;
    const char *loc_by_language;
    const char *loc_by_file;
} bake_report_project_total_t;

static double bake_report_round(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    return floor(value * 1000000.0 + 0.5) / 1000000.0;
}

static double bake_report_elapsed(const ecs_time_t *start) {
    ecs_time_t now = {0};
    ecs_os_get_time(&now);
    int64_t sec = (int64_t)now.sec - (int64_t)start->sec;
    int64_t nanosec = (int64_t)now.nanosec - (int64_t)start->nanosec;
    return (double)sec + (double)nanosec / 1000000000.0;
}

static char* bake_report_timestamp(void) {
    time_t now = time(NULL);
    char stamp[64] = {0};
    struct tm utc;
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return ecs_os_strdup(stamp);
}

static char* bake_report_git(
    const char *workspace,
    const char *scratch_dir,
    const char *const *args)
{
    int32_t arg_count = 0;
    while (args[arg_count]) {
        arg_count++;
    }

    const char **argv = ecs_os_malloc_n(const char*, arg_count + 4);
    argv[0] = "git";
    argv[1] = "-C";
    argv[2] = workspace;
    for (int32_t i = 0; i < arg_count; i++) {
        argv[i + 3] = args[i];
    }
    argv[arg_count + 3] = NULL;

    char *stdout_path = bake_path_join(scratch_dir, ".bake_build_report_git");
    char *stderr_path = bake_path_join(scratch_dir, ".bake_build_report_git_err");
    bake_process_stdio_t stdio_cfg = {
        .stdout_path = stdout_path,
        .stderr_path = stderr_path
    };

    bake_process_result_t result = {0};
    int rc = bake_proc_run(argv, &stdio_cfg, &result);
    char *output = NULL;
    if (rc == 0 && result.exit_code == 0) {
        output = bake_file_read_trimmed(stdout_path);
    }

    bake_remove_file_if_exists(stdout_path);
    bake_remove_file_if_exists(stderr_path);
    ecs_os_free(stdout_path);
    ecs_os_free(stderr_path);
    ecs_os_free(argv);
    return output;
}

static void bake_report_collect_git(
    bake_build_report_t *report,
    const char *workspace,
    const char *scratch_dir)
{
    if (!workspace || !workspace[0]) {
        return;
    }

    static const char *is_repo[] = {"rev-parse", "--is-inside-work-tree", NULL};
    char *inside = bake_report_git(workspace, scratch_dir, is_repo);
    bool in_repo = inside && !strcmp(inside, "true");
    ecs_os_free(inside);
    if (!in_repo) {
        return;
    }

    report->git_present = true;

    static const char *head[] = {"rev-parse", "HEAD", NULL};
    report->git_sha = bake_report_git(workspace, scratch_dir, head);

    static const char *branch[] = {"rev-parse", "--abbrev-ref", "HEAD", NULL};
    report->git_branch = bake_report_git(workspace, scratch_dir, branch);

    static const char *status[] = {"status", "--porcelain", NULL};
    char *changes = bake_report_git(workspace, scratch_dir, status);
    report->git_dirty = changes && changes[0];
    ecs_os_free(changes);
}

bake_build_report_t* bake_report_new(const bake_context_t *ctx, const char *path) {
    if (!ctx || !path || !path[0]) {
        return NULL;
    }

    bake_build_report_t *report = ecs_os_calloc_t(bake_build_report_t);
    report->path = bake_path_is_abs(path)
        ? ecs_os_strdup(path)
        : bake_path_join(ctx->opts.cwd, path);
    report->current = BAKE_REPORT_NO_STEP;
    report->lock = ecs_os_mutex_new();

    char *dir = bake_path_dirname(report->path);
    if (dir && dir[0] && bake_os_mkdirs(dir) != 0) {
        ecs_err("failed to create directory for build report '%s'", report->path);
    }

    report->timestamp = bake_report_timestamp();
    report->cfg = ecs_os_strdup(bake_effective_mode(ctx->opts.mode));
    report->target = bake_target_is_emscripten()
        ? ecs_os_strdup("em")
        : bake_host_platform();
    report->environment = bake_env_name(ctx);
    report->host_os = bake_host_os();
    report->host_arch = bake_host_arch();
    report->cpu_count = bake_os_cpu_count();

    bake_report_collect_git(report, ctx->opts.cwd, (dir && dir[0]) ? dir : ".");

    ecs_os_free(dir);
    ecs_os_get_time(&report->start);
    return report;
}

void bake_report_free(bake_build_report_t *report) {
    if (!report) {
        return;
    }

    for (int32_t i = 0; i < report->count; i++) {
        bake_report_step_t *step = &report->steps[i];
        ecs_os_free(step->name);
        ecs_os_free(step->kind);
        ecs_os_free(step->project);
        ecs_os_free(step->object);
        ecs_os_free(step->error);
        ecs_os_free(step->loc_by_language);
        ecs_os_free(step->loc_by_file);
    }

    if (report->lock) {
        ecs_os_mutex_free(report->lock);
    }

    ecs_os_free(report->steps);
    ecs_os_free(report->path);
    ecs_os_free(report->timestamp);
    ecs_os_free(report->cfg);
    ecs_os_free(report->target);
    ecs_os_free(report->environment);
    ecs_os_free(report->git_sha);
    ecs_os_free(report->git_branch);
    ecs_os_free(report->workspace_loc_by_language);
    ecs_os_free(report->loc_note);
    ecs_os_free(report);
}

int32_t bake_report_current(const bake_build_report_t *report) {
    return report ? report->current : BAKE_REPORT_NO_STEP;
}

static int32_t bake_report_add(
    bake_build_report_t *report,
    int32_t parent,
    const char *kind,
    const char *name,
    const char *project)
{
    if (report->count == report->capacity) {
        int32_t next = report->capacity ? report->capacity * 2 : 32;
        report->steps = ecs_os_realloc_n(report->steps, bake_report_step_t, next);
        report->capacity = next;
    }

    int32_t index = report->count++;
    bake_report_step_t *step = &report->steps[index];
    memset(step, 0, sizeof(*step));
    step->name = ecs_os_strdup(name ? name : "");
    step->kind = ecs_os_strdup(kind ? kind : BAKE_REPORT_KIND_OTHER);
    step->project = project ? ecs_os_strdup(project) : NULL;
    step->parent = parent;
    step->start_sec = bake_report_elapsed(&report->start);
    step->ok = true;
    step->open = true;
    return index;
}

int32_t bake_report_open(
    bake_build_report_t *report,
    const char *kind,
    const char *name,
    const char *project)
{
    if (!report) {
        return BAKE_REPORT_NO_STEP;
    }

    ecs_os_mutex_lock(report->lock);
    int32_t index = bake_report_add(report, report->current, kind, name, project);
    report->current = index;
    ecs_os_mutex_unlock(report->lock);
    return index;
}

int32_t bake_report_open_under(
    bake_build_report_t *report,
    int32_t parent,
    const char *kind,
    const char *name,
    const char *project)
{
    if (!report) {
        return BAKE_REPORT_NO_STEP;
    }

    ecs_os_mutex_lock(report->lock);
    int32_t index = bake_report_add(report, parent, kind, name, project);
    ecs_os_mutex_unlock(report->lock);
    return index;
}

void bake_report_set_object(
    bake_build_report_t *report,
    int32_t step,
    const char *object)
{
    if (!report || step < 0 || step >= report->count || !object) {
        return;
    }

    ecs_os_mutex_lock(report->lock);
    ecs_os_free(report->steps[step].object);
    report->steps[step].object = ecs_os_strdup(object);
    ecs_os_mutex_unlock(report->lock);
}

void bake_report_close(
    bake_build_report_t *report,
    int32_t step,
    bool ok,
    const char *error)
{
    if (!report || step < 0 || step >= report->count) {
        return;
    }

    ecs_os_mutex_lock(report->lock);
    bake_report_step_t *entry = &report->steps[step];
    if (entry->open) {
        entry->duration_sec = bake_report_elapsed(&report->start) - entry->start_sec;
        entry->open = false;
    }
    entry->ok = ok;
    if (error && error[0] && !entry->error) {
        entry->error = ecs_os_strdup(error);
    }
    if (report->current == step) {
        report->current = entry->parent;
    }
    ecs_os_mutex_unlock(report->lock);
}

void bake_report_set_loc(
    bake_build_report_t *report,
    int32_t step,
    int32_t files,
    int32_t code,
    int32_t comment,
    int32_t blank,
    const char *by_language_json,
    const char *by_file_json)
{
    if (!report || step < 0 || step >= report->count) {
        return;
    }

    ecs_os_mutex_lock(report->lock);
    bake_report_step_t *entry = &report->steps[step];
    entry->has_loc = true;
    entry->loc_files = files;
    entry->loc_code = code;
    entry->loc_comment = comment;
    entry->loc_blank = blank;
    ecs_os_free(entry->loc_by_language);
    entry->loc_by_language = ecs_os_strdup(by_language_json ? by_language_json : "{}");
    ecs_os_free(entry->loc_by_file);
    entry->loc_by_file = by_file_json ? ecs_os_strdup(by_file_json) : NULL;
    ecs_os_mutex_unlock(report->lock);
}

void bake_report_set_workspace_loc(
    bake_build_report_t *report,
    int32_t files,
    int32_t code,
    int32_t comment,
    int32_t blank,
    const char *by_language_json)
{
    if (!report) {
        return;
    }

    ecs_os_mutex_lock(report->lock);
    report->workspace_has_loc = true;
    report->workspace_loc_files = files;
    report->workspace_loc_code = code;
    report->workspace_loc_comment = comment;
    report->workspace_loc_blank = blank;
    ecs_os_free(report->workspace_loc_by_language);
    report->workspace_loc_by_language = ecs_os_strdup(by_language_json ? by_language_json : "{}");
    ecs_os_mutex_unlock(report->lock);
}

void bake_report_set_loc_note(bake_build_report_t *report, const char *note) {
    if (!report || !note) {
        return;
    }

    ecs_os_mutex_lock(report->lock);
    ecs_os_free(report->loc_note);
    report->loc_note = ecs_os_strdup(note);
    ecs_os_mutex_unlock(report->lock);
}

static void bake_report_append_string(ecs_strbuf_t *buf, const char *value) {
    if (!value) {
        ecs_strbuf_appendstr(buf, "null");
        return;
    }

    ecs_strbuf_appendch(buf, '"');
    for (const char *ch = value; *ch; ch++) {
        unsigned char c = (unsigned char)*ch;
        if (c == '"' || c == '\\') {
            ecs_strbuf_appendch(buf, '\\');
            ecs_strbuf_appendch(buf, (char)c);
        } else if (c == '\n') {
            ecs_strbuf_appendstr(buf, "\\n");
        } else if (c == '\r') {
            ecs_strbuf_appendstr(buf, "\\r");
        } else if (c == '\t') {
            ecs_strbuf_appendstr(buf, "\\t");
        } else if (c < 0x20) {
            ecs_strbuf_append(buf, "\\u%04x", (int)c);
        } else {
            ecs_strbuf_appendch(buf, (char)c);
        }
    }
    ecs_strbuf_appendch(buf, '"');
}

static void bake_report_append_indent(ecs_strbuf_t *buf, int32_t depth) {
    for (int32_t i = 0; i < depth; i++) {
        ecs_strbuf_appendstr(buf, "  ");
    }
}

static bool bake_report_has_children(const bake_build_report_t *report, int32_t step) {
    for (int32_t i = step + 1; i < report->count; i++) {
        if (report->steps[i].parent == step) {
            return true;
        }
    }
    return false;
}

static void bake_report_append_steps(
    const bake_build_report_t *report,
    ecs_strbuf_t *buf,
    int32_t parent,
    int32_t depth);

static void bake_report_append_step(
    const bake_build_report_t *report,
    ecs_strbuf_t *buf,
    int32_t index,
    int32_t depth)
{
    const bake_report_step_t *step = &report->steps[index];

    bake_report_append_indent(buf, depth);
    ecs_strbuf_appendstr(buf, "{\n");

    bake_report_append_indent(buf, depth + 1);
    ecs_strbuf_appendstr(buf, "\"name\": ");
    bake_report_append_string(buf, step->name);
    ecs_strbuf_appendstr(buf, ",\n");

    bake_report_append_indent(buf, depth + 1);
    ecs_strbuf_appendstr(buf, "\"kind\": ");
    bake_report_append_string(buf, step->kind);
    ecs_strbuf_appendstr(buf, ",\n");

    bake_report_append_indent(buf, depth + 1);
    ecs_strbuf_appendstr(buf, "\"project\": ");
    bake_report_append_string(buf, step->project);
    ecs_strbuf_appendstr(buf, ",\n");

    bake_report_append_indent(buf, depth + 1);
    ecs_strbuf_append(buf, "\"start_sec\": %.6f,\n", bake_report_round(step->start_sec));

    bake_report_append_indent(buf, depth + 1);
    ecs_strbuf_append(buf, "\"duration_sec\": %.6f,\n",
        bake_report_round(step->duration_sec));

    bake_report_append_indent(buf, depth + 1);
    ecs_strbuf_append(buf, "\"ok\": %s,\n", step->ok ? "true" : "false");

    if (step->object) {
        bake_report_append_indent(buf, depth + 1);
        ecs_strbuf_appendstr(buf, "\"object\": ");
        bake_report_append_string(buf, step->object);
        ecs_strbuf_appendstr(buf, ",\n");
    }

    if (step->error) {
        bake_report_append_indent(buf, depth + 1);
        ecs_strbuf_appendstr(buf, "\"error\": ");
        bake_report_append_string(buf, step->error);
        ecs_strbuf_appendstr(buf, ",\n");
    }

    bake_report_append_indent(buf, depth + 1);
    if (!bake_report_has_children(report, index)) {
        ecs_strbuf_appendstr(buf, "\"children\": []\n");
    } else {
        ecs_strbuf_appendstr(buf, "\"children\": [\n");
        bake_report_append_steps(report, buf, index, depth + 2);
        bake_report_append_indent(buf, depth + 1);
        ecs_strbuf_appendstr(buf, "]\n");
    }

    bake_report_append_indent(buf, depth);
    ecs_strbuf_appendstr(buf, "}");
}

static void bake_report_append_steps(
    const bake_build_report_t *report,
    ecs_strbuf_t *buf,
    int32_t parent,
    int32_t depth)
{
    bool first = true;
    for (int32_t i = 0; i < report->count; i++) {
        if (report->steps[i].parent != parent) {
            continue;
        }
        if (!first) {
            ecs_strbuf_appendstr(buf, ",\n");
        }
        first = false;
        bake_report_append_step(report, buf, i, depth);
    }
    if (!first) {
        ecs_strbuf_appendstr(buf, "\n");
    }
}

static const char* bake_report_total_kinds[] = {
    BAKE_REPORT_KIND_COMPILE,
    BAKE_REPORT_KIND_LINK,
    BAKE_REPORT_KIND_BUNDLE,
    BAKE_REPORT_KIND_DISCOVERY,
    BAKE_REPORT_KIND_GENERATE,
    BAKE_REPORT_KIND_ETC,
    BAKE_REPORT_KIND_LOC,
    BAKE_REPORT_KIND_OTHER
};

static void bake_report_append_kind_totals(
    const bake_build_report_t *report,
    ecs_strbuf_t *buf)
{
    size_t kind_count = sizeof(bake_report_total_kinds) /
        sizeof(bake_report_total_kinds[0]);
    double *totals = ecs_os_calloc_n(double, (int32_t)kind_count);

    for (int32_t i = 0; i < report->count; i++) {
        const bake_report_step_t *step = &report->steps[i];
        if (bake_report_has_children(report, i) ||
            !strcmp(step->kind, BAKE_REPORT_KIND_PROJECT))
        {
            continue;
        }

        size_t slot = kind_count - 1;
        for (size_t k = 0; k < kind_count; k++) {
            if (!strcmp(step->kind, bake_report_total_kinds[k])) {
                slot = k;
                break;
            }
        }

        totals[slot] += step->duration_sec;
    }

    ecs_strbuf_appendstr(buf, "    \"kind\": {\n");
    for (size_t k = 0; k < kind_count; k++) {
        ecs_strbuf_append(buf, "      \"%s\": %.6f%s\n",
            bake_report_total_kinds[k],
            bake_report_round(totals[k]),
            (k + 1) == kind_count ? "" : ",");
    }
    ecs_strbuf_appendstr(buf, "    },\n");

    ecs_os_free(totals);
}

static void bake_report_append_loc_value(
    ecs_strbuf_t *buf,
    int32_t files,
    int32_t code,
    int32_t comment,
    int32_t blank,
    const char *by_language_json,
    const char *by_file_json)
{
    ecs_strbuf_append(buf, "{\"files\": %d, \"code\": %d, \"comment\": %d, "
        "\"blank\": %d, \"by_language\": ", files, code, comment, blank);
    ecs_strbuf_appendstr(buf,
        (by_language_json && by_language_json[0]) ? by_language_json : "{}");
    if (by_file_json && by_file_json[0]) {
        ecs_strbuf_appendstr(buf, ", \"by_file\": ");
        ecs_strbuf_appendstr(buf, by_file_json);
    }
    ecs_strbuf_appendstr(buf, "}");
}

static void bake_report_append_project_totals(
    const bake_build_report_t *report,
    ecs_strbuf_t *buf)
{
    bake_report_project_total_t *totals =
        ecs_os_calloc_n(bake_report_project_total_t, report->count + 1);
    int32_t total_count = 0;

    for (int32_t i = 0; i < report->count; i++) {
        const bake_report_step_t *step = &report->steps[i];
        if (!step->project || !step->project[0]) {
            continue;
        }

        bake_report_project_total_t *entry = NULL;
        for (int32_t t = 0; t < total_count; t++) {
            if (!strcmp(totals[t].project, step->project)) {
                entry = &totals[t];
                break;
            }
        }
        if (!entry) {
            entry = &totals[total_count++];
            entry->project = step->project;
        }

        if (!strcmp(step->kind, BAKE_REPORT_KIND_LOC)) {
            if (step->has_loc) {
                entry->has_loc = true;
                entry->loc_files = step->loc_files;
                entry->loc_code = step->loc_code;
                entry->loc_comment = step->loc_comment;
                entry->loc_blank = step->loc_blank;
                entry->loc_by_language = step->loc_by_language;
                entry->loc_by_file = step->loc_by_file;
            }

            continue;
        }

        const bake_report_step_t *parent = step->parent >= 0
            ? &report->steps[step->parent]
            : NULL;
        bool owns_time = !parent || !parent->project ||
            strcmp(parent->project, step->project);
        if (owns_time) {
            entry->total_sec += step->duration_sec;
        }

        if (!strcmp(step->kind, BAKE_REPORT_KIND_COMPILE)) {
            entry->compile_sec += step->duration_sec;
            entry->files++;
        } else if (!strcmp(step->kind, BAKE_REPORT_KIND_LINK)) {
            entry->link_sec += step->duration_sec;
        }
    }

    ecs_strbuf_appendstr(buf, "    \"project\": {");
    for (int32_t t = 0; t < total_count; t++) {
        const bake_report_project_total_t *entry = &totals[t];
        ecs_strbuf_appendstr(buf, t ? ",\n" : "\n");
        ecs_strbuf_appendstr(buf, "      ");
        bake_report_append_string(buf, entry->project);
        ecs_strbuf_append(buf,
            ": {\"total_sec\": %.6f, \"compile_sec\": %.6f, "
            "\"link_sec\": %.6f, \"files\": %d",
            bake_report_round(entry->total_sec),
            bake_report_round(entry->compile_sec),
            bake_report_round(entry->link_sec),
            entry->files);
        if (entry->has_loc) {
            ecs_strbuf_appendstr(buf, ", \"loc\": ");
            bake_report_append_loc_value(buf, entry->loc_files, entry->loc_code,
                entry->loc_comment, entry->loc_blank, entry->loc_by_language,
                entry->loc_by_file);
        }
        ecs_strbuf_appendstr(buf, "}");
    }
    if (total_count) {
        ecs_strbuf_appendstr(buf, "\n    ");
    }
    ecs_strbuf_appendstr(buf, "},\n");

    ecs_os_free(totals);
}

static void bake_report_append_workspace_loc(
    const bake_build_report_t *report,
    ecs_strbuf_t *buf)
{
    ecs_strbuf_appendstr(buf, "    \"loc\": ");

    if (report->workspace_has_loc) {
        bake_report_append_loc_value(buf,
            report->workspace_loc_files, report->workspace_loc_code,
            report->workspace_loc_comment, report->workspace_loc_blank,
            report->workspace_loc_by_language, NULL);
    } else if (report->loc_note) {
        ecs_strbuf_appendstr(buf, "{\"note\": ");
        bake_report_append_string(buf, report->loc_note);
        ecs_strbuf_appendstr(buf, "}");
    } else {
        ecs_strbuf_appendstr(buf, "null");
    }

    ecs_strbuf_appendstr(buf, "\n");
}

static char* bake_report_serialize(const bake_build_report_t *report, bool ok) {
    ecs_strbuf_t buf = ECS_STRBUF_INIT;

    ecs_strbuf_appendstr(&buf, "{\n");
    ecs_strbuf_appendstr(&buf, "  \"tool\": \"bake3\",\n");
    ecs_strbuf_appendstr(&buf, "  \"tool_version\": \"" BAKE_VERSION "\",\n");
    ecs_strbuf_appendstr(&buf, "  \"timestamp\": ");
    bake_report_append_string(&buf, report->timestamp);
    ecs_strbuf_appendstr(&buf, ",\n");

    ecs_strbuf_appendstr(&buf, "  \"host\": {\"os\": ");
    bake_report_append_string(&buf, report->host_os);
    ecs_strbuf_appendstr(&buf, ", \"arch\": ");
    bake_report_append_string(&buf, report->host_arch);
    ecs_strbuf_append(&buf, ", \"cpu_count\": %d},\n", report->cpu_count);

    ecs_strbuf_appendstr(&buf, "  \"cfg\": ");
    bake_report_append_string(&buf, report->cfg);
    ecs_strbuf_appendstr(&buf, ",\n");

    ecs_strbuf_appendstr(&buf, "  \"target\": ");
    bake_report_append_string(&buf, report->target);
    ecs_strbuf_appendstr(&buf, ",\n");

    ecs_strbuf_appendstr(&buf, "  \"environment\": ");
    bake_report_append_string(&buf, report->environment);
    ecs_strbuf_appendstr(&buf, ",\n");

    ecs_strbuf_appendstr(&buf, "  \"git\": ");
    if (!report->git_present) {
        ecs_strbuf_appendstr(&buf, "null");
    } else {
        ecs_strbuf_appendstr(&buf, "{\"sha\": ");
        bake_report_append_string(&buf, report->git_sha);
        ecs_strbuf_append(&buf, ", \"dirty\": %s, \"branch\": ",
            report->git_dirty ? "true" : "false");
        bake_report_append_string(&buf, report->git_branch);
        ecs_strbuf_appendstr(&buf, "}");
    }
    ecs_strbuf_appendstr(&buf, ",\n");

    ecs_strbuf_append(&buf, "  \"total_sec\": %.6f,\n",
        bake_report_round(bake_report_elapsed(&report->start)));
    ecs_strbuf_append(&buf, "  \"ok\": %s,\n", ok ? "true" : "false");

    ecs_strbuf_appendstr(&buf, "  \"totals\": {\n");
    bake_report_append_kind_totals(report, &buf);
    bake_report_append_project_totals(report, &buf);
    bake_report_append_workspace_loc(report, &buf);
    ecs_strbuf_appendstr(&buf, "  },\n");

    ecs_strbuf_appendstr(&buf, "  \"steps\": [");
    if (report->count) {
        ecs_strbuf_appendstr(&buf, "\n");
        bake_report_append_steps(report, &buf, BAKE_REPORT_NO_STEP, 2);
        ecs_strbuf_appendstr(&buf, "  ]\n");
    } else {
        ecs_strbuf_appendstr(&buf, "]\n");
    }
    ecs_strbuf_appendstr(&buf, "}\n");

    return ecs_strbuf_get(&buf);
}

int bake_report_finish(bake_build_report_t *report, bool ok) {
    if (!report || report->written) {
        return 0;
    }

    for (int32_t i = 0; i < report->count; i++) {
        if (report->steps[i].open) {
            bake_report_close(report, i, false, NULL);
        }
    }

    report->written = true;
    report->current = BAKE_REPORT_NO_STEP;

    char *content = bake_report_serialize(report, ok);
    int rc = bake_file_write(report->path, content);
    ecs_os_free(content);

    if (rc != 0) {
        ecs_err("failed to write build report '%s'", report->path);
        return -1;
    }

    ecs_trace("#[green][#[normal] report#[green]]#[normal] %s", report->path);
    return 0;
}
