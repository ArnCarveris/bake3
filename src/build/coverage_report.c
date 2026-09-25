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
    char *root;
    char *root_resolved;
    bake_strlist_t include;
    bake_strlist_t exclude;
    bool keep_empty;
    bool merged;
    bool summary;
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
    ecs_os_free(report->root);
    ecs_os_free(report->root_resolved);
    bake_strlist_fini(&report->include);
    bake_strlist_fini(&report->exclude);
}

static bool bake_cov_has_glob(const char *pattern) {
    return strpbrk(pattern, "*?") != NULL;
}

static bool bake_cov_glob(const char *pattern, const char *path) {
    for (;;) {
        if (!pattern[0]) {
            return !path[0];
        }

        if (pattern[0] == '*' && pattern[1] == '*') {
            const char *rest = pattern + 2;
            if (rest[0] == '/' && bake_cov_glob(rest + 1, path)) {
                return true;
            }
            for (const char *cur = path;; cur++) {
                if (bake_cov_glob(rest, cur)) {
                    return true;
                }
                if (!cur[0]) {
                    return false;
                }
            }
        }

        if (pattern[0] == '*') {
            for (const char *cur = path;; cur++) {
                if (bake_cov_glob(pattern + 1, cur)) {
                    return true;
                }
                if (!cur[0] || cur[0] == '/') {
                    return false;
                }
            }
        }

        if (!path[0]) {
            return false;
        }
        if (pattern[0] == '?' ? path[0] == '/' : pattern[0] != path[0]) {
            return false;
        }
        pattern++;
        path++;
    }
}

bool bake_coverage_path_matches(const char *pattern, const char *path) {
    if (!pattern || !path) {
        return false;
    }
    while (pattern[0] == '.' && pattern[1] == '/') {
        pattern += 2;
    }
    if (bake_cov_has_glob(pattern)) {
        return bake_cov_glob(pattern, path);
    }

    size_t len = strlen(pattern);
    while (len && pattern[len - 1] == '/') {
        len--;
    }
    if (!len) {
        return false;
    }
    return !strncmp(path, pattern, len) && (path[len] == '\0' || path[len] == '/');
}

static void bake_cov_parse_patterns(const char *value, bake_strlist_t *out) {
    bake_strlist_init(out);
    if (!value) {
        return;
    }

    const char *cur = value;
    while (*cur) {
        const char *end = strchr(cur, ',');
        size_t len = end ? (size_t)(end - cur) : strlen(cur);
        while (len && bake_char_is_space(*cur)) {
            cur++;
            len--;
        }
        while (len && bake_char_is_space(cur[len - 1])) {
            len--;
        }
        if (len) {
            char *pattern = ecs_os_malloc((ecs_size_t)len + 1);
            memcpy(pattern, cur, len);
            pattern[len] = '\0';
            bake_strlist_append_owned(out, pattern);
        }
        if (!end) {
            break;
        }
        cur = end + 1;
    }
}

static int bake_cov_report_init(bake_cov_report_t *report, bake_context_t *ctx) {
    memset(report, 0, sizeof(*report));
    report->ctx = ctx;
    report->summary = ctx->opts.coverage_summary;
    ecs_vec_init_t(NULL, &report->projects, bake_cov_project_t, 0);
    ecs_vec_init_t(NULL, &report->profiles, char*, 0);
    ecs_vec_init_t(NULL, &report->files, bake_cov_file_t*, 0);
    bake_cov_parse_patterns(ctx->opts.coverage_include, &report->include);
    bake_cov_parse_patterns(ctx->opts.coverage_exclude, &report->exclude);

    const char *root = ctx->opts.coverage_root;
    if (!root || !root[0]) {
        root = ctx->opts.cwd;
    }
    report->root = bake_path_is_abs(root)
        ? ecs_os_strdup(root)
        : bake_path_join(ctx->opts.cwd, root);
    size_t len = strlen(report->root);
    while (len > 1 && bake_path_is_sep(report->root[len - 1])) {
        report->root[--len] = '\0';
    }

    if (!bake_path_is_dir(report->root)) {
        ecs_err("coverage root '%s' is not a directory", report->root);
        return -1;
    }
    report->root_resolved = bake_path_resolve(report->root);
    return 0;
}

static char* bake_cov_relative_path(const bake_cov_report_t *report, const char *path) {
    const char *roots[] = { report->root, report->root_resolved };
    for (int32_t i = 0; i < 2; i++) {
        size_t len = 0;
        if (!roots[i] || !bake_path_has_prefix_normalized(path, roots[i], &len)) {
            continue;
        }
        const char *rel = path + len;
        while (*rel && bake_path_is_sep(*rel)) {
            rel++;
        }
        if (*rel) {
            return ecs_os_strdup(rel);
        }
    }
    return ecs_os_strdup(path);
}

static bool bake_cov_path_selected(const bake_cov_report_t *report, const char *path) {
    bool included = report->include.count == 0;
    for (int32_t i = 0; !included && i < report->include.count; i++) {
        included = bake_coverage_path_matches(report->include.items[i], path);
    }
    if (!included) {
        return false;
    }
    for (int32_t i = 0; i < report->exclude.count; i++) {
        if (bake_coverage_path_matches(report->exclude.items[i], path)) {
            return false;
        }
    }
    return true;
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

            char *build_root = bake_project_cfg_build_root(cfg, mode);
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
                (report->keep_empty || file->totals.lines.count ||
                    file->totals.functions.count))
            {
                file->display = bake_cov_relative_path(report, file->path);
            }
            if (file && file->display && bake_cov_path_selected(report, file->display)) {
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

static const char *bake_cov_cxx_operators[] = {
    "<=>", "<<=", ">>=", "->*", "()", "[]", "<<", ">>", "<=", ">=", "==", "!=",
    "&&", "||", "++", "--", "->", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "+", "-", "*", "/", "%", "^", "&", "|", "~", "!", "=", "<", ">", ",", NULL
};

#define BAKE_COV_ANON_NS "(anonymous namespace)"

char* bake_coverage_simplify_name(const char *name, bool strip_params) {
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    size_t anon_len = strlen(BAKE_COV_ANON_NS);
    int32_t depth = 0;
    const char *ch = name;
    while (*ch) {
        bool ident_start = ch == name || !bake_harness_char_is_ident(ch[-1]);
        if (!depth && ident_start && !strncmp(ch, "operator", 8)) {
            const char *end = ch + 8;
            for (int32_t i = 0; bake_cov_cxx_operators[i]; i++) {
                size_t len = strlen(bake_cov_cxx_operators[i]);
                if (!strncmp(end, bake_cov_cxx_operators[i], len)) {
                    end += len;
                    break;
                }
            }
            ecs_strbuf_appendstrn(&buf, ch, (int32_t)(end - ch));
            ch = end;
            continue;
        }
        if (!depth && !strncmp(ch, BAKE_COV_ANON_NS, anon_len)) {
            ecs_strbuf_appendstrn(&buf, ch, (int32_t)anon_len);
            ch += anon_len;
            continue;
        }
        if (*ch == '<') {
            depth++;
        } else if (*ch == '>' && depth) {
            depth--;
        } else if (!depth) {
            if (strip_params && *ch == '(') {
                break;
            }
            ecs_strbuf_appendch(&buf, *ch);
        }
        ch++;
    }
    char *result = ecs_strbuf_get(&buf);
    return result ? result : ecs_os_strdup("");
}

static char* bake_cov_find_program(const char *name) {
    if (!name || !name[0]) {
        return NULL;
    }
    if (strchr(name, '/')) {
        return bake_path_exists(name) ? ecs_os_strdup(name) : NULL;
    }

    const char *path = getenv("PATH");
    while (path && *path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        if (len) {
            char *dir = ecs_os_malloc((ecs_size_t)len + 1);
            memcpy(dir, path, len);
            dir[len] = '\0';
            char *candidate = bake_path_join(dir, name);
            ecs_os_free(dir);
            if (bake_path_exists(candidate) && !bake_path_is_dir(candidate)) {
                return candidate;
            }
            ecs_os_free(candidate);
        }
        if (!end) {
            break;
        }
        path = end + 1;
    }
    return NULL;
}

static void bake_cov_demangler_candidates(
    const bake_cov_report_t *report,
    bake_strlist_t *out)
{
    bake_strlist_init(out);
    char *toolchain = bake_coverage_tool(report->ctx, "llvm-cxxfilt");
    const char *names[] = { toolchain, "llvm-cxxfilt", "c++filt" };
    for (int32_t i = 0; i < 3; i++) {
        char *found = bake_cov_find_program(names[i]);
        if (found && !bake_strlist_contains(out, found)) {
            bake_strlist_append_owned(out, found);
        } else {
            ecs_os_free(found);
        }
    }
    ecs_os_free(toolchain);
}

static char** bake_cov_run_demangler(
    const char *tool,
    bool no_params,
    const char *in_path,
    const char *out_path,
    const char *err_path,
    int32_t count)
{
    const char *argv[] = { tool, "-n", no_params ? "-p" : NULL, NULL };
    bake_process_stdio_t stdio_cfg = {
        .stdin_path = in_path,
        .stdout_path = out_path,
        .stderr_path = err_path
    };
    bake_process_result_t result = {0};
    if (bake_proc_run(argv, &stdio_cfg, &result) != 0 || result.exit_code != 0) {
        return NULL;
    }

    char *content = bake_file_read(out_path, NULL);
    if (!content) {
        return NULL;
    }

    char **names = ecs_os_calloc_n(char*, count);
    char *cur = content;
    int32_t i = 0;
    for (; i < count && *cur; i++) {
        char *nl = strchr(cur, '\n');
        size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
        names[i] = ecs_os_malloc((ecs_size_t)len + 1);
        memcpy(names[i], cur, len);
        names[i][len] = '\0';
        cur = nl ? nl + 1 : cur + len;
    }
    ecs_os_free(content);

    if (i < count) {
        for (int32_t j = 0; j < i; j++) {
            ecs_os_free(names[j]);
        }
        ecs_os_free(names);
        return NULL;
    }
    return names;
}

static int bake_cov_name_compare(const void *a, const void *b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static void bake_cov_demangle(bake_cov_report_t *report, const char *work_dir) {
    bake_strlist_t mangled;
    bake_strlist_init(&mangled);
    int32_t file_count = ecs_vec_count(&report->files);
    bake_cov_file_t **files = ecs_vec_first_t(&report->files, bake_cov_file_t*);
    for (int32_t i = 0; i < file_count; i++) {
        int32_t fn_count = ecs_vec_count(&files[i]->fns);
        bake_cov_fn_t *fns = ecs_vec_first_t(&files[i]->fns, bake_cov_fn_t);
        for (int32_t f = 0; f < fn_count; f++) {
            if (!strncmp(fns[f].name, "_Z", 2)) {
                bake_strlist_append(&mangled, fns[f].name);
            }
        }
    }

    char **demangled = NULL;
    bool strip_params = false;
    if (mangled.count) {
        qsort(mangled.items, (size_t)mangled.count, sizeof(char*), bake_cov_name_compare);
        int32_t unique = 0;
        for (int32_t i = 0; i < mangled.count; i++) {
            if (unique && !strcmp(mangled.items[unique - 1], mangled.items[i])) {
                ecs_os_free(mangled.items[i]);
                continue;
            }
            mangled.items[unique++] = mangled.items[i];
        }
        mangled.count = unique;

        char *joined = bake_strlist_join(&mangled, "\n");
        char *in_path = bake_path_join(work_dir, "demangle.in");
        char *out_path = bake_path_join(work_dir, "demangle.out");
        char *err_path = bake_path_join(work_dir, "demangle.err");
        ecs_strbuf_t input = ECS_STRBUF_INIT;
        ecs_strbuf_appendstr(&input, joined);
        ecs_strbuf_appendch(&input, '\n');
        char *input_str = ecs_strbuf_get(&input);

        if (bake_file_write(in_path, input_str) == 0) {
            bake_strlist_t tools;
            bake_cov_demangler_candidates(report, &tools);
            for (int32_t pass = 0; pass < 2 && !demangled; pass++) {
                for (int32_t t = 0; t < tools.count && !demangled; t++) {
                    demangled = bake_cov_run_demangler(tools.items[t], pass == 0,
                        in_path, out_path, err_path, mangled.count);
                }
                strip_params = pass == 1;
            }
            if (!demangled) {
                ecs_warn("no working llvm-cxxfilt or c++filt found, coverage "
                    "reports use mangled function names");
            }
            bake_strlist_fini(&tools);
        }

        bake_remove_file_if_exists(in_path);
        bake_remove_file_if_exists(out_path);
        bake_remove_file_if_exists(err_path);
        ecs_os_free(err_path);
        ecs_os_free(input_str);
        ecs_os_free(joined);
        ecs_os_free(in_path);
        ecs_os_free(out_path);
    }

    for (int32_t i = 0; i < file_count; i++) {
        int32_t fn_count = ecs_vec_count(&files[i]->fns);
        bake_cov_fn_t *fns = ecs_vec_first_t(&files[i]->fns, bake_cov_fn_t);
        for (int32_t f = 0; f < fn_count; f++) {
            const char *name = fns[f].name;
            bool is_mangled = !strncmp(name, "_Z", 2);
            if (is_mangled && demangled) {
                char **match = bsearch(&name, mangled.items, (size_t)mangled.count,
                    sizeof(char*), bake_cov_name_compare);
                const char *full = match ? demangled[match - mangled.items] : NULL;
                if (full && full[0]) {
                    name = full;
                }
            }
            if (!is_mangled || demangled) {
                char *simple = bake_coverage_simplify_name(name, is_mangled && strip_params);
                if (simple[0]) {
                    ecs_os_free(fns[f].name);
                    fns[f].name = simple;
                } else {
                    ecs_os_free(simple);
                }
            }
        }
    }

    if (demangled) {
        for (int32_t i = 0; i < mangled.count; i++) {
            ecs_os_free(demangled[i]);
        }
        ecs_os_free(demangled);
    }
    bake_strlist_fini(&mangled);
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

typedef struct bake_cov_range_t {
    int64_t first;
    int64_t last;
} bake_cov_range_t;

static void bake_cov_uncovered_ranges(const bake_cov_file_t *file, ecs_vec_t *out) {
    int32_t count = ecs_vec_count(&file->lines);
    const bake_cov_line_t *lines = ecs_vec_first_t(&file->lines, bake_cov_line_t);
    bake_cov_range_t *range = NULL;
    for (int32_t i = 0; i < count; i++) {
        if (lines[i].hits) {
            range = NULL;
            continue;
        }
        if (!range) {
            range = ecs_vec_append_t(NULL, out, bake_cov_range_t);
            range->first = lines[i].line;
        }
        range->last = lines[i].line;
    }
}

static bool bake_cov_in_ranges(const ecs_vec_t *ranges, int64_t line) {
    int32_t count = ecs_vec_count(ranges);
    const bake_cov_range_t *items = ecs_vec_first_t(ranges, bake_cov_range_t);
    for (int32_t i = 0; i < count; i++) {
        if (line >= items[i].first && line <= items[i].last) {
            return true;
        }
    }
    return false;
}

static int bake_cov_fn_line_compare(const void *a, const void *b) {
    const bake_cov_fn_t *fa = *(const bake_cov_fn_t* const*)a;
    const bake_cov_fn_t *fb = *(const bake_cov_fn_t* const*)b;
    if (fa->line != fb->line) {
        return fa->line < fb->line ? -1 : 1;
    }
    return fa < fb ? -1 : (fa > fb);
}

static void bake_cov_json_uncovered(ecs_strbuf_t *buf, const bake_cov_file_t *file) {
    ecs_vec_t ranges;
    ecs_vec_init_t(NULL, &ranges, bake_cov_range_t, 0);
    bake_cov_uncovered_ranges(file, &ranges);

    ecs_strbuf_appendstr(buf, ",\n     \"uncovered_lines\": [");
    int32_t range_count = ecs_vec_count(&ranges);
    const bake_cov_range_t *range_items = ecs_vec_first_t(&ranges, bake_cov_range_t);
    for (int32_t i = 0; i < range_count; i++) {
        ecs_strbuf_append(buf, "%s[%lld, %lld]", i ? ", " : "",
            (long long)range_items[i].first, (long long)range_items[i].last);
    }

    int32_t fn_count = ecs_vec_count(&file->fns);
    const bake_cov_fn_t *fns = ecs_vec_first_t(&file->fns, bake_cov_fn_t);
    const bake_cov_fn_t **uncovered = ecs_os_malloc_n(const bake_cov_fn_t*, fn_count + 1);
    int32_t uncovered_count = 0;
    for (int32_t f = 0; f < fn_count; f++) {
        if (!fns[f].hits && bake_cov_in_ranges(&ranges, fns[f].line)) {
            uncovered[uncovered_count++] = &fns[f];
        }
    }
    if (uncovered_count > 1) {
        qsort(uncovered, (size_t)uncovered_count, sizeof(bake_cov_fn_t*),
            bake_cov_fn_line_compare);
    }

    ecs_strbuf_appendstr(buf, "],\n     \"uncovered_functions\": [");
    int64_t prev_line = -1;
    bool first = true;
    for (int32_t f = 0; f < uncovered_count; f++) {
        if (uncovered[f]->line == prev_line) {
            continue;
        }
        prev_line = uncovered[f]->line;
        ecs_strbuf_appendstr(buf, first ? "{\"name\": " : ", {\"name\": ");
        bake_cov_json_cstr(buf, uncovered[f]->name);
        ecs_strbuf_append(buf, ", \"line\": %lld}", (long long)uncovered[f]->line);
        first = false;
    }
    ecs_strbuf_appendstr(buf, "]");

    ecs_os_free(uncovered);
    ecs_vec_fini_t(NULL, &ranges, bake_cov_range_t);
}

static int bake_cov_write_json(
    const bake_cov_report_t *report,
    const char *stamp,
    const char *path)
{
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&buf, "{\n  \"project\": ");
    bake_cov_json_cstr(&buf, report->title);
    ecs_strbuf_appendstr(&buf, ",\n  \"timestamp\": ");
    bake_cov_json_cstr(&buf, stamp);
    ecs_strbuf_appendstr(&buf, ",\n");
    if (report->merged) {
        ecs_strbuf_appendstr(&buf, "  \"projects\": [");
        int32_t project_count = ecs_vec_count(&report->projects);
        const bake_cov_project_t *projects = ecs_vec_first_t(
            &report->projects, bake_cov_project_t);
        for (int32_t i = 0; i < project_count; i++) {
            if (i) ecs_strbuf_appendstr(&buf, ", ");
            bake_cov_json_cstr(&buf, projects[i].cfg->id);
        }
        ecs_strbuf_appendstr(&buf, "],\n");
    }
    bake_cov_json_totals(&buf, "  ", &report->totals);
    ecs_strbuf_appendstr(&buf, ",\n  \"files\": [");

    int32_t file_count = ecs_vec_count(&report->files);
    bake_cov_file_t **files = ecs_vec_first_t(&report->files, bake_cov_file_t*);
    for (int32_t i = 0; i < file_count; i++) {
        const bake_cov_file_t *file = files[i];
        ecs_strbuf_appendstr(&buf, i ? ",\n    {\"file\": " : "\n    {\"file\": ");
        bake_cov_json_cstr(&buf, file->display);
        ecs_strbuf_appendstr(&buf, ",\n");
        bake_cov_json_totals(&buf, "     ", &file->totals);
        if (!report->summary) {
            bake_cov_json_uncovered(&buf, file);
        }
        ecs_strbuf_appendstr(&buf, "}");
    }
    ecs_strbuf_appendstr(&buf, file_count ? "\n  ]\n}\n" : "]\n}\n");

    char *content = ecs_strbuf_get(&buf);
    int rc = bake_file_write(path, content);
    if (rc != 0) {
        ecs_err("failed to write coverage report '%s'", path);
    }
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
    bake_cov_report_t report;
    int rc = -1;
    char *root = NULL;
    char *profdata = NULL;
    char *lcov = NULL;
    char *files_dir = NULL;
    char *json_path = NULL;
    const char *target_id = NULL;

    if (bake_cov_report_init(&report, ctx) != 0) {
        goto cleanup;
    }
    report.merged = true;

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
    bake_cov_demangle(&report, report.out_dir);

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

    json_path = bake_path_join(report.out_dir, "coverage.json");
    if (bake_cov_write_index(&report, stamp) != 0 ||
        bake_cov_write_json(&report, stamp, json_path) != 0)
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
    ecs_os_free(json_path);
    bake_cov_report_fini(&report);
    return rc;
}

static bool bake_cov_add_project(
    bake_cov_report_t *report,
    const bake_project_cfg_t *cfg,
    const char *exe)
{
    const char *mode = report->ctx->opts.mode;
    char *coverage_dir = bake_coverage_dir(cfg, mode);
    int32_t found = coverage_dir
        ? bake_cov_collect_profiles(report, coverage_dir)
        : 0;
    if (!found) {
        ecs_err("no coverage data found in %s", coverage_dir ? coverage_dir : cfg->id);
        ecs_os_free(coverage_dir);
        return false;
    }
    ecs_os_free(coverage_dir);

    bake_cov_project_t *project = ecs_vec_append_t(
        NULL, &report->projects, bake_cov_project_t);
    project->cfg = cfg;
    project->exe = bake_path_resolve(exe);
    if (!project->exe) {
        project->exe = ecs_os_strdup(exe);
    }
    project->build_root = bake_project_cfg_build_root(cfg, mode);
    project->resolved_path = bake_path_resolve(cfg->path);
    return true;
}

int bake_coverage_project_report(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *exe,
    const char *json_path)
{
    bake_cov_report_t report;
    int rc = -1;
    char *coverage_dir = NULL;
    char *profdata = NULL;
    char *lcov = NULL;

    if (bake_cov_report_init(&report, ctx) != 0) {
        goto cleanup;
    }
    report.keep_empty = true;
    report.title = ecs_os_strdup(cfg->id);

    if (!bake_cov_add_project(&report, cfg, exe) ||
        bake_coverage_init_tools(ctx) != 0)
    {
        goto cleanup;
    }

    coverage_dir = bake_coverage_dir(cfg, ctx->opts.mode);
    profdata = bake_path_join(coverage_dir, "coverage.profdata");
    lcov = bake_path_join(coverage_dir, "coverage.lcov");
    if (bake_cov_merge_and_export(&report, profdata, lcov) != 0 ||
        bake_cov_parse_lcov(&report, lcov) != 0)
    {
        goto cleanup;
    }
    bake_cov_demangle(&report, coverage_dir);

    char stamp[64] = {0};
    bake_cov_timestamp(stamp, sizeof(stamp));
    if (bake_cov_write_json(&report, stamp, json_path) != 0) {
        goto cleanup;
    }

    const bake_cov_totals_t *t = &report.totals;
    printf("coverage: %.2f%% lines (%lld/%lld), %.2f%% functions (%lld/%lld), "
        "%.2f%% branches (%lld/%lld)\n",
        bake_cov_percent(&t->lines),
        (long long)t->lines.covered, (long long)t->lines.count,
        bake_cov_percent(&t->functions),
        (long long)t->functions.covered, (long long)t->functions.count,
        bake_cov_percent(&t->branches),
        (long long)t->branches.covered, (long long)t->branches.count);
    rc = 0;

cleanup:
    ecs_os_free(coverage_dir);
    ecs_os_free(profdata);
    ecs_os_free(lcov);
    bake_cov_report_fini(&report);
    return rc;
}
