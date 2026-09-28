#include "build_internal.h"
#include "bake/build_report.h"
#include "compile_internal.h"
#include "depcheck_internal.h"
#include "bake/os.h"

#include <ctype.h>

void bake_list_append_fmt(ecs_strbuf_t *buf, const bake_strlist_t *list, const char *prefix) {
    for (int32_t i = 0; i < list->count; i++) {
        ecs_strbuf_append(buf, " %s%s", prefix, list->items[i]);
    }
}

static void bake_append_flags(bake_strlist_t *list, const char *const *flags) {
    for (int32_t i = 0; flags[i]; i++) {
        bake_strlist_append(list, flags[i]);
    }
}

static void bake_append_lang_flags(
    bake_strlist_t *cflags,
    bake_strlist_t *cxxflags,
    const char *const *flags)
{
    bake_append_flags(cflags, flags);
    bake_append_flags(cxxflags, flags);
}

bake_compiler_kind_t bake_detect_compiler_kind(const char *cc, const char *cxx) {
    const char *probe = cxx && cxx[0] ? cxx : cc;
    if (!probe) {
#if defined(_WIN32)
        return BAKE_COMPILER_MSVC;
#else
        return BAKE_COMPILER_GCC;
#endif
    }

    if (strstr(probe, "emcc") || strstr(probe, "em++")) {
        return BAKE_COMPILER_CLANG;
    }
    if (strstr(probe, "clang-cl")) {
        return BAKE_COMPILER_MSVC;
    }
    if (strstr(probe, "clang")) {
        return BAKE_COMPILER_CLANG;
    }
    if (strstr(probe, "gcc") || strstr(probe, "g++")) {
        return BAKE_COMPILER_GCC;
    }
    if (strstr(probe, "cl")) {
        return BAKE_COMPILER_MSVC;
    }
    if (strstr(probe, "cc") || strstr(probe, "c++")) {
        return BAKE_COMPILER_GCC;
    }

    return BAKE_COMPILER_UNKNOWN;
}

void bake_add_mode_flags(const char *mode, bake_compiler_kind_t kind, bake_strlist_t *cflags, bake_strlist_t *cxxflags, bake_strlist_t *ldflags) {
    static const char *msvc_debug[] = {"/Z7", "/Od", NULL};
    static const char *msvc_debug_ld[] = {"/DEBUG", NULL};
    static const char *msvc_release[] = {"/O2", "/GL", NULL};
    static const char *msvc_release_ld[] = {"/LTCG", NULL};
    static const char *msvc_profile[] = {"/O2", NULL};
    static const char *msvc_sanitize[] = {"/fsanitize=address", NULL};
    static const char *gnu_debug[] = {"-g", "-O0", NULL};
    static const char *gnu_release[] = {"-O3", "-DNDEBUG", "-flto", NULL};
    static const char *gnu_release_ld[] = {"-flto", NULL};
    static const char *gnu_profile[] = {"-O2", "-pg", NULL};
    static const char *gnu_sanitize[] = {
        "-O0", "-g", "-fsanitize=address", "-fsanitize=undefined", NULL
    };
    static const char *gnu_sanitize_ld[] = {"-fsanitize=address", "-fsanitize=undefined", NULL};
    static const char *em_profile[] = {"-O2", NULL};
    static const char *em_debug_ld[] = {"-O0", "-g", NULL};
    static const char *em_release_ld[] = {"-O3", "-flto", NULL};
    static const char *em_profile_ld[] = {"-O2", NULL};
    static const char *em_sanitize_ld[] = {
        "-O0", "-g", "-fsanitize=address", "-fsanitize=undefined", NULL
    };

    /* For emcc the link-time optimization level is a semantic setting: it
     * selects which variant of the system libraries is linked and drives the
     * JS optimizer and wasm-opt. Without it a release build links the debug
     * libc into an LTO image. */
    bool is_em = bake_target_is_emscripten();

    if (!mode || !strcmp(mode, "debug")) {
        if (kind == BAKE_COMPILER_MSVC) {
            bake_append_lang_flags(cflags, cxxflags, msvc_debug);
        } else {
            bake_append_lang_flags(cflags, cxxflags, gnu_debug);
            if (is_em) {
                bake_append_flags(ldflags, em_debug_ld);
            }
        }
        return;
    }

    if (!strcmp(mode, "release")) {
        if (kind == BAKE_COMPILER_MSVC) {
            bake_append_lang_flags(cflags, cxxflags, msvc_release);
            bake_append_flags(ldflags, msvc_release_ld);
        } else {
            bake_append_lang_flags(cflags, cxxflags, gnu_release);
            bake_append_flags(ldflags, is_em ? em_release_ld : gnu_release_ld);
        }
        return;
    }

    if (!strcmp(mode, "profile")) {
        if (kind == BAKE_COMPILER_MSVC) {
            bake_append_lang_flags(cflags, cxxflags, msvc_profile);
        } else if (is_em) {
            /* emcc has no gprof instrumentation, -pg is rejected */
            bake_append_lang_flags(cflags, cxxflags, em_profile);
            bake_append_flags(ldflags, em_profile_ld);
        } else {
            bake_append_lang_flags(cflags, cxxflags, gnu_profile);
            bake_strlist_append(ldflags, "-pg");
        }
        return;
    }

    if (!strcmp(mode, "sanitize")) {
        if (kind == BAKE_COMPILER_MSVC) {
            bake_append_lang_flags(cflags, cxxflags, msvc_sanitize);
        } else {
            bake_append_lang_flags(cflags, cxxflags, gnu_sanitize);
            bake_append_flags(ldflags, is_em ? em_sanitize_ld : gnu_sanitize_ld);
        }
    }
}

void bake_add_strict_flags(
    bool strict,
    bake_compiler_kind_t kind,
    bake_strlist_t *cflags,
    bake_strlist_t *cxxflags,
    bake_strlist_t *ldflags)
{
    static const char *msvc[] = {
        "/W4", "/WX",
        "/w14242", "/w14254", "/w14263",
        "/w14265", "/w14287", "/we4289",
        "/w14296", "/w14311", "/w14545",
        "/w14546", "/w14547", "/w14549",
        "/w14555", "/w14619", "/w14640",
        "/w14826", "/w14905", "/w14906",
        "/w14928", NULL
    };
    static const char *gnu_common[] = {
        "-Wall", "-Wextra", "-Werror",
        "-Wcast-align", "-Wcast-qual", "-Wconversion",
        "-Wdouble-promotion", "-Wfloat-conversion", "-Wformat=2",
        "-Wimplicit-fallthrough", "-Wnull-dereference", "-Wpedantic",
        "-Wshadow", "-Wsign-conversion", "-Wswitch-enum",
        "-Wundef", NULL
    };
    static const char *gnu_c[] = {
        "-Wbad-function-cast", "-Wmissing-declarations",
        "-Wmissing-prototypes", "-Wold-style-definition",
        "-Wstrict-prototypes", NULL
    };
    static const char *gnu_cxx[] = {
        "-Wmissing-declarations", "-Wnon-virtual-dtor",
        "-Wold-style-cast", "-Woverloaded-virtual",
        "-Wzero-as-null-pointer-constant", NULL
    };
#if defined(__APPLE__)
    static const char *gnu_ld[] = {"-Werror", "-Wl,-fatal_warnings", NULL};
#else
    static const char *gnu_ld[] = {"-Werror", "-Wl,--fatal-warnings", NULL};
#endif

    if (!strict) {
        return;
    }

    if (kind == BAKE_COMPILER_MSVC) {
        bake_append_lang_flags(cflags, cxxflags, msvc);
        bake_strlist_append(ldflags, "/WX");
        return;
    }

    bake_append_lang_flags(cflags, cxxflags, gnu_common);
    bake_append_flags(cflags, gnu_c);
    bake_append_flags(cxxflags, gnu_cxx);
    bake_append_flags(ldflags, gnu_ld);
}

static void bake_collect_dependency_link_inputs(
    ecs_world_t *world,
    const BakeResolvedDeps *resolved,
    bool include_artefacts,
    bake_strlist_t *artefacts,
    bake_strlist_t *libpaths,
    bake_strlist_t *libs,
    bake_strlist_t *ldflags)
{
    if (!resolved) {
        return;
    }

    bake_strlist_merge_unique(libpaths, &resolved->build_libpaths);
    bake_strlist_merge_unique(ldflags, &resolved->ldflags);

    for (int32_t i = 0; include_artefacts && i < resolved->dep_count; i++) {
        const BakeBuildResult *result = ecs_get(world, resolved->deps[i], BakeBuildResult);
        if (!result || !result->artefact ||
            bake_strlist_contains(artefacts, result->artefact))
        {
            continue;
        }

        bake_strlist_append(artefacts, result->artefact);
        char *libdir = bake_path_dirname(result->artefact);
        bake_strlist_append_unique(libpaths, libdir);
        ecs_os_free(libdir);
    }

    for (int32_t i = 0; i < resolved->libs.count; i++) {
        const char *lib = resolved->libs.items[i];
        if (bake_has_dep_artefact_for_lib(artefacts, lib) ||
            bake_strlist_contains(libs, lib))
        {
            continue;
        }

        bake_strlist_append(libs, lib);
    }
}

typedef struct bake_compile_ctx_t {
    bake_context_t *ctx;
    const bake_project_cfg_t *cfg;
    const bake_compile_list_t *units;
    const bake_lang_cfg_t *c_lang;
    const bake_lang_cfg_t *cpp_lang;
    const bake_strlist_t *mode_cflags;
    const bake_strlist_t *mode_cxxflags;
    bake_strlist_t dep_includes;
    bool *compile_mask;
    int32_t report_parent;
    int32_t compile_total;
    int32_t compile_done;
    ecs_os_mutex_t print_lock;
    int32_t cursor;
    int32_t failed;
} bake_compile_ctx_t;

static int bake_run_compiler_command(
    const bake_context_t *ctx,
    ecs_os_mutex_t print_lock,
    const char *command)
{
    if (ctx->opts.trace) {
        if (print_lock) {
            ecs_os_mutex_lock(print_lock);
        }
        ecs_trace("%s", command);
        if (print_lock) {
            ecs_os_mutex_unlock(print_lock);
        }
    }

    return bake_run_command(command, false);
}

static char* bake_compile_display_path(const bake_project_cfg_t *cfg, const char *src) {
    if (!src) {
        return NULL;
    }

    char *path = bake_display_path(src, cfg ? cfg->path : NULL);

    /* Strip leading "src/" from compile display paths */
    if (!strncmp(path, "src/", 4) || !strncmp(path, "src\\", 4)) {
        char *trimmed = ecs_os_strdup(path + 4);
        ecs_os_free(path);
        return trimmed;
    }

    if (!path[0] || !strcmp(path, ".")) {
        ecs_os_free(path);
        return bake_path_basename(src);
    }

    return path;
}

static void bake_lint_print_line(const char *line, const char *color) {
    const char *colon = strchr(line, ':');
    const char *num = colon ? colon + 1 : NULL;
    const char *p = num;
    while (p && *p >= '0' && *p <= '9') {
        p++;
    }

    if (num && p != num && *p == ':') {
        int loc_len = (int)(p - line + 1);
        ecs_trace("#[%s]lint: #[bold]%.*s#[normal]%s", color, loc_len, line, p + 1);
    } else {
        ecs_trace("#[%s]lint: #[normal]%s", color, line);
    }
}

static void bake_lint_print_output(
    bake_compile_ctx_t *ctx,
    const char *output_path,
    const char *color)
{
    char *output = bake_file_read(output_path, NULL);
    bake_remove_file_if_exists(output_path);
    if (!output) {
        return;
    }

    if (ctx->print_lock) {
        ecs_os_mutex_lock(ctx->print_lock);
    }

    char *line = output;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }
        if (line[0]) {
            bake_lint_print_line(line, color);
        }
        line = next;
    }

    if (ctx->print_lock) {
        ecs_os_mutex_unlock(ctx->print_lock);
    }

    ecs_os_free(output);
}

static int bake_lint_single(bake_compile_ctx_t *ctx, const bake_compile_unit_t *unit) {
    const bake_lint_cfg_t *lint = &ctx->cfg->lint;
    bake_lint_action_t action = ctx->ctx->opts.fix_lint ? BAKE_LINT_AUTOFIX : lint->action;
    char *file = bake_shell_quote_arg(unit->src);
    char *command = flecs_asprintf("%s %s %s",
        lint->command, file, bake_lint_action_str(action));
    ecs_os_free(file);

    if (ctx->ctx->opts.trace) {
        if (ctx->print_lock) {
            ecs_os_mutex_lock(ctx->print_lock);
        }
        ecs_trace("%s", command);
        if (ctx->print_lock) {
            ecs_os_mutex_unlock(ctx->print_lock);
        }
    }

    char *output_path = flecs_asprintf("%s.lint", unit->obj);
    int exit_code = 0;
    int rc = bake_run_command_status(command, ctx->cfg->path, output_path, &exit_code);
    ecs_os_free(command);

    bool failed = rc == 0 && exit_code != 0 && action == BAKE_LINT_ERROR;
    bake_lint_print_output(ctx, output_path, failed ? "red" : "yellow");
    ecs_os_free(output_path);

    if (rc != 0) {
        return -1;
    }

    if (exit_code == 0) {
        return 0;
    }

    char *display_path = bake_display_path(unit->src, ctx->cfg->path);
    if (failed) {
        ecs_err("lint failed for %s (exit code %d)", display_path, exit_code);
        rc = -1;
    } else {
        ecs_warn("lint reported problems in %s (exit code %d)", display_path, exit_code);
    }
    ecs_os_free(display_path);

    return rc;
}

static int bake_compile_single(bake_compile_ctx_t *ctx, const bake_compile_unit_t *unit) {
    if (ctx->print_lock) {
        ecs_os_mutex_lock(ctx->print_lock);
    }
    int32_t done = ecs_os_ainc(&ctx->compile_done);
    int32_t pct = (done * 100) / ctx->compile_total;
    char *display_path = bake_compile_display_path(ctx->cfg, unit->src);
    ecs_trace("#[green][#[normal]%6d%%#[green]]#[normal] %s", pct, display_path ? display_path : unit->src);
    if (ctx->print_lock) {
        ecs_os_mutex_unlock(ctx->print_lock);
    }
    ecs_os_free(display_path);

    char *source_path = bake_display_path(unit->src, ctx->cfg ? ctx->cfg->path : NULL);
    int32_t step = bake_report_open_under(ctx->ctx->report, ctx->report_parent,
        BAKE_REPORT_KIND_COMPILE, source_path, ctx->cfg ? ctx->cfg->id : NULL);
    bake_report_set_object(ctx->ctx->report, step, unit->obj);
    ecs_os_free(source_path);

    if (unit->lint && bake_lint_single(ctx, unit) != 0) {
        bake_report_close(ctx->ctx->report, step, false, "lint failed");
        return -1;
    }

    const bake_lang_cfg_t *lang = unit->cpp ? ctx->cpp_lang : ctx->c_lang;
    const bake_strlist_t *mode_flags = unit->cpp ? ctx->mode_cxxflags : ctx->mode_cflags;

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    bake_compile_cmd_ctx_t cmd_ctx = {
        .ctx = ctx->ctx,
        .cfg = ctx->cfg,
        .unit = unit,
        .lang = lang,
        .mode_flags = mode_flags,
        .dep_includes = &ctx->dep_includes
    };

    if (ctx->ctx->compiler_kind == BAKE_COMPILER_MSVC) {
        bake_compose_compile_command_msvc(&cmd_ctx, &cmd);
    } else {
        bake_compose_compile_command_posix(&cmd_ctx, &cmd);
    }

    char *command = ecs_strbuf_get(&cmd);
    int rc = bake_run_compiler_command(ctx->ctx, ctx->print_lock, command);
    ecs_os_free(command);

    bake_report_close(ctx->ctx->report, step, rc == 0,
        rc == 0 ? NULL : "compile command failed");
    return rc;
}

static void* bake_compile_worker(void *arg) {
    bake_compile_ctx_t *ctx = arg;

    for (;;) {
        if (ctx->failed) {
            break;
        }

        int32_t index = ecs_os_ainc(&ctx->cursor) - 1;
        if (index >= ctx->units->count) {
            break;
        }

        if (!ctx->compile_mask[index]) {
            continue;
        }

        if (bake_compile_single(ctx, &ctx->units->items[index]) != 0) {
            ecs_os_ainc(&ctx->failed);
            break;
        }
    }

    return NULL;
}

int bake_compile_units_parallel(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    const bake_compile_list_t *units,
    const bake_lang_cfg_t *lang,
    const bake_lang_cfg_t *cpp_lang,
    const bake_strlist_t *mode_cflags,
    const bake_strlist_t *mode_cxxflags,
    bool force_rebuild,
    int32_t *compiled_count_out)
{
    if (compiled_count_out) {
        *compiled_count_out = 0;
    }

    if (units->count == 0) {
        return 0;
    }

    const BakeResolvedDeps *resolved =
        project_entity ? ecs_get(ctx->world, project_entity, BakeResolvedDeps) : NULL;

    bake_compile_ctx_t compile_ctx = {
        .ctx = ctx,
        .cfg = cfg,
        .units = units,
        .c_lang = lang,
        .cpp_lang = cpp_lang,
        .mode_cflags = mode_cflags,
        .mode_cxxflags = mode_cxxflags,
        .report_parent = bake_report_current(ctx->report),
    };

    int rc = -1;
    ecs_os_thread_t *threads = NULL;
    compile_ctx.compile_mask = ecs_os_calloc_n(bool, units->count);

    int64_t project_json_mtime = bake_project_json_mtime(cfg);
    for (int32_t i = 0; i < units->count; i++) {
        compile_ctx.compile_mask[i] = force_rebuild || bake_compile_unit_outdated(
            &units->items[i],
            project_json_mtime);
        if (compile_ctx.compile_mask[i]) {
            compile_ctx.compile_total++;
        }
    }

    if (compile_ctx.compile_total == 0) {
        rc = 0;
        goto cleanup;
    }

    bake_strlist_init(&compile_ctx.dep_includes);
    if (resolved) {
        bake_strlist_merge_unique(&compile_ctx.dep_includes, &resolved->include_paths);
    }
    compile_ctx.print_lock = ecs_os_mutex_new();

    int32_t workers = ctx->thread_count;
    if (workers < 1) {
        workers = 1;
    }
    if (workers > compile_ctx.compile_total) {
        workers = compile_ctx.compile_total;
    }

    threads = ecs_os_malloc_n(ecs_os_thread_t, workers);
    int32_t started = 0;
    for (int32_t i = 0; i < workers; i++) {
        threads[i] = ecs_os_thread_new(bake_compile_worker, &compile_ctx);
        if (!threads[i]) {
            ecs_os_ainc(&compile_ctx.failed);
            break;
        }
        started++;
    }

    if (!started) {
        goto cleanup;
    }

    for (int32_t i = 0; i < started; i++) {
        ecs_os_thread_join(threads[i]);
    }

    if (!compile_ctx.failed && compiled_count_out) {
        *compiled_count_out = compile_ctx.compile_total;
    }

    rc = compile_ctx.failed ? -1 : 0;

cleanup:
    ecs_os_free(threads);
    bake_strlist_fini(&compile_ctx.dep_includes);
    if (compile_ctx.print_lock) {
        ecs_os_mutex_free(compile_ctx.print_lock);
    }
    ecs_os_free(compile_ctx.compile_mask);
    return rc;
}

static bool bake_link_inputs_outdated(
    const bake_project_cfg_t *cfg,
    const char *artefact,
    const bake_compile_list_t *units,
    const bake_strlist_t *dep_artefacts)
{
    if (!artefact || !bake_path_exists(artefact)) {
        return true;
    }

    int64_t artefact_mtime = bake_os_file_mtime(artefact);
    if (artefact_mtime < 0) {
        return true;
    }

    if (bake_project_json_outdated(cfg, artefact_mtime)) {
        return true;
    }

    for (int32_t i = 0; i < units->count; i++) {
        int64_t mtime = bake_os_file_mtime(units->items[i].obj);
        if (mtime < 0 || mtime > artefact_mtime) {
            return true;
        }
    }

    for (int32_t i = 0; i < dep_artefacts->count; i++) {
        int64_t mtime = bake_os_file_mtime(dep_artefacts->items[i]);
        if (mtime < 0 || mtime > artefact_mtime) {
            return true;
        }
    }

    return false;
}

int bake_link_project_binary(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths,
    const bake_compile_list_t *units,
    const bake_lang_cfg_t *lang,
    const bake_strlist_t *mode_ldflags,
    bool force_relink,
    bool standalone,
    char **artefact_out,
    bool *linked_out)
{
    int rc = -1;
    char *artefact = NULL;
    char *file_name = NULL;
    if (linked_out) {
        *linked_out = false;
    }

    bake_strlist_t dep_artefacts;
    bake_strlist_t dep_libpaths;
    bake_strlist_t dep_libs;
    bake_strlist_t dep_ldflags;
    bake_strlist_init(&dep_artefacts);
    bake_strlist_init(&dep_libpaths);
    bake_strlist_init(&dep_libs);
    bake_strlist_init(&dep_ldflags);

    const BakeResolvedDeps *resolved = ecs_get(ctx->world, project_entity, BakeResolvedDeps);

    /* Standalone builds compile dependency sources from deps/ into the
     * project's own objects; dependency binaries are not linked. */
    bake_collect_dependency_link_inputs(
        ctx->world,
        resolved,
        !standalone,
        &dep_artefacts,
        &dep_libpaths,
        &dep_libs,
        &dep_ldflags);

    bool is_lib = cfg->kind == BAKE_PROJECT_PACKAGE;
    file_name = bake_project_cfg_artefact_name(cfg);

    if (!file_name) {
        goto cleanup;
    }

    artefact = bake_path_join(is_lib ? paths->lib_dir : paths->bin_dir, file_name);
    ecs_os_free(file_name);
    file_name = NULL;

    if (!force_relink &&
        !bake_link_inputs_outdated(cfg, artefact, units, &dep_artefacts))
    {
        *artefact_out = artefact;
        artefact = NULL;
        rc = 0;
        goto cleanup;
    }

    /* Rebuild static libraries from scratch: ar only adds/replaces members,
     * so objects of deleted sources would otherwise linger in the archive. */
    if (cfg->kind == BAKE_PROJECT_PACKAGE &&
        bake_remove_file_if_exists(artefact) != 0)
    {
        goto cleanup;
    }

    bool use_cpp = bake_language_is_cpp(cfg);
    for (int32_t i = 0; i < units->count; i++) {
        if (units->items[i].cpp) {
            use_cpp = true;
            break;
        }
    }
    if (!use_cpp && resolved) {
        for (int32_t i = 0; i < resolved->dep_count; i++) {
            const BakeProject *dep = ecs_get(ctx->world, resolved->deps[i], BakeProject);
            if (dep && dep->cfg && bake_language_is_cpp(dep->cfg)) {
                use_cpp = true;
                break;
            }
        }
    }

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    bake_link_cmd_ctx_t cmd_ctx = {
        .ctx = ctx,
        .cfg = cfg,
        .units = units,
        .lang = lang,
        .mode_ldflags = mode_ldflags,
        .dep_artefacts = &dep_artefacts,
        .dep_libpaths = &dep_libpaths,
        .dep_libs = &dep_libs,
        .dep_ldflags = &dep_ldflags,
        .artefact = artefact,
        .use_cpp = use_cpp
    };

    if (ctx->compiler_kind == BAKE_COMPILER_MSVC) {
        bake_compose_link_command_msvc(&cmd_ctx, &cmd);
    } else {
        bake_compose_link_command_posix(&cmd_ctx, &cmd);
    }

    char *command = ecs_strbuf_get(&cmd);
    rc = bake_run_compiler_command(ctx, 0, command);
    ecs_os_free(command);

    if (rc != 0) {
        goto cleanup;
    }

    if (linked_out) {
        *linked_out = true;
    }
    *artefact_out = artefact;
    artefact = NULL;
    rc = 0;

cleanup:
    bake_strlist_fini(&dep_artefacts);
    bake_strlist_fini(&dep_libpaths);
    bake_strlist_fini(&dep_libs);
    bake_strlist_fini(&dep_ldflags);
    ecs_os_free(file_name);
    ecs_os_free(artefact);
    return rc;
}
