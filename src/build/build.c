#include "build_internal.h"
#include "bake/build_report.h"
#include "bake/bundle.h"
#include "bake/environment.h"
#include "bake/test_harness.h"
#include "bake/bench_harness.h"
#include "bake/os.h"
#include "bake/ps.h"

#include <limits.h>
#include <stdlib.h>

ECS_COMPONENT_DECLARE(BakeBuildRequest);
ECS_COMPONENT_DECLARE(BakeBuildResult);

char* bake_project_build_root(const char *project_path, const char *project_id, const char *mode) {
    if (!project_path || !project_path[0]) {
        return NULL;
    }

    char *triplet = bake_host_triplet(mode);
    const char *bake_home = bake_env_home();
    if (bake_env_is_local() &&
        bake_home &&
        bake_home[0] &&
        project_id &&
        project_id[0])
    {
        char *build_dir = bake_path_join(bake_home, "build");
        char *project_dir = bake_path_join(build_dir, project_id);
        ecs_os_free(build_dir);

        char *root = bake_path_join(project_dir, triplet);
        ecs_os_free(project_dir);
        ecs_os_free(triplet);
        return root;
    }

    char *bake_dir = bake_path_join(project_path, ".bake");
    char *root = bake_path_join(bake_dir, triplet);
    ecs_os_free(triplet);
    ecs_os_free(bake_dir);
    return root;
}

static const char *bake_standalone_deps_marker = ".bake_standalone_deps";

char* bake_display_path(const char *full_path, const char *strip_prefix) {
    if (!full_path) {
        return ecs_os_strdup(".");
    }

    const char *display = full_path;
    size_t prefix_len = 0;
    if (strip_prefix &&
        bake_path_has_prefix_normalized(full_path, strip_prefix, &prefix_len))
    {
        display = full_path + prefix_len;
        while (*display == '/' || *display == '\\') {
            display++;
        }
        if (!display[0]) {
            return ecs_os_strdup(".");
        }
    }

    char *out = ecs_os_strdup(display);

    for (char *ch = out; *ch; ch++) {
        if (*ch == '\\') {
            *ch = '/';
        }
    }

    return out;
}

static void bake_log_build_header(const bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    const char *command = ctx->opts.command ? ctx->opts.command : "build";
    const char *kind = bake_project_kind_str(cfg->kind);
    const char *id = cfg->id ? cfg->id : "<unnamed>";
    char *path = bake_display_path(cfg->path, ctx->opts.cwd);
    ecs_trace("#[green][#[normal]%s#[green]] %s#[normal] %s => '%s'", command, kind, id, path);
    ecs_os_free(path);
}

static char* bake_resolve_target_path(const bake_context_t *ctx, const char *target) {
    if (!ctx || !target || !target[0]) {
        return NULL;
    }

    if (!bake_path_exists(target)) {
        return NULL;
    }

    if (bake_path_is_abs(target)) {
        return bake_path_resolve(target);
    }

    char *joined = bake_path_join(ctx->opts.cwd, target);
    char *normalized = bake_path_resolve(joined);
    ecs_os_free(joined);
    return normalized;
}

static const char* bake_effective_build_target(const bake_context_t *ctx) {
    if (ctx && ctx->opts.target && ctx->opts.target[0]) {
        return ctx->opts.target;
    }
    return ".";
}

static int bake_cleanup_standalone_outputs(
    const char *deps_dir,
    const bake_strlist_t *expected_outputs)
{
    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    if (bake_dir_list(deps_dir, &entries, &entry_count) != 0) {
        return -1;
    }

    int rc = 0;
    for (int32_t i = 0; i < entry_count; i++) {
        if (entries[i].is_dir) {
            continue;
        }

        if (!bake_strlist_contains(expected_outputs, entries[i].name)) {
            if (bake_remove_file(entries[i].path) != 0) {
                rc = -1;
                break;
            }
        }
    }

    bake_dir_entries_free(entries, entry_count);
    return rc;
}

static int64_t bake_standalone_dep_fingerprint(const bake_project_cfg_t *cfg) {
    char *project_json = bake_path_join(cfg->path, "project.json");
    int64_t newest = bake_os_file_mtime(project_json);
    ecs_os_free(project_json);

    static const char *dirs[] = {"include", "src"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char *dir = bake_path_join(cfg->path, dirs[i]);
        int64_t mtime = bake_os_tree_newest_mtime(dir);
        if (mtime > newest) {
            newest = mtime;
        }
        ecs_os_free(dir);
    }

    return newest;
}

static char* bake_standalone_marker_value(const char *marker, const char *dep_id) {
    if (!marker || !dep_id) {
        return NULL;
    }

    size_t id_len = strlen(dep_id);
    const char *line = marker;
    while (line) {
        if (!strncmp(line, dep_id, id_len) && line[id_len] == '=') {
            const char *start = line + id_len + 1;
            const char *end = start;
            while (*end && *end != '\n' && *end != '\r') {
                end++;
            }
            size_t len = (size_t)(end - start);
            char *value = ecs_os_malloc(len + 1);
            memcpy(value, start, len);
            value[len] = '\0';
            return value;
        }
        line = strchr(line, '\n');
        if (line) {
            line++;
        }
    }
    return NULL;
}

static int bake_prepare_standalone_sources(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg)
{
    int rc = -1;
    char *deps_dir = bake_path_join(cfg->path, "deps");
    char *marker_path = bake_path_join(deps_dir, bake_standalone_deps_marker);
    char *prev_marker = NULL;
    bake_strlist_t expected_outputs = {0};
    ecs_strbuf_t marker_buf = ECS_STRBUF_INIT;
    bake_strlist_init(&expected_outputs);

    if (bake_os_mkdirs(deps_dir) != 0) {
        goto cleanup;
    }

    prev_marker = bake_file_read(marker_path, NULL);
    ecs_strbuf_appendstr(&marker_buf, "generated by bake\n");

    const BakeResolvedDeps *resolved =
        ecs_get(ctx->world, project_entity, BakeResolvedDeps);
    int32_t dep_count = resolved ? resolved->dep_count : 0;

    for (int32_t i = 0; i < dep_count; i++) {
        const BakeProject *dep_project =
            ecs_get(ctx->world, resolved->deps[i], BakeProject);
        if (!dep_project || !dep_project->cfg || !dep_project->cfg->id) {
            continue;
        }

        const bake_project_cfg_t *dep_cfg = dep_project->cfg;
        if (dep_cfg->kind == BAKE_PROJECT_CONFIG ||
            dep_cfg->kind == BAKE_PROJECT_TEMPLATE)
        {
            continue;
        }

        char *base = bake_project_id_as_macro(dep_cfg->id);
        char *h_name = flecs_asprintf("%s.h", base);
        char *c_name = flecs_asprintf("%s.c", base);
        char *cpp_name = flecs_asprintf("%s.cpp", base);
        char *objc_name = flecs_asprintf("%s_objc.m", base);
        char *h_path = bake_path_join(deps_dir, h_name);
        char *c_path = bake_path_join(deps_dir, c_name);
        char *cpp_path = bake_path_join(deps_dir, cpp_name);
        char *prev_value = bake_standalone_marker_value(prev_marker, dep_cfg->id);

        /* Amalgamation splits a dependency into <base>.c and/or <base>.cpp;
         * either may be absent, so the header plus at least one source is the
         * signal that usable standalone sources already exist. */
        bool have_files = bake_path_exists(h_path) &&
            (bake_path_exists(c_path) || bake_path_exists(cpp_path));
        bool dep_available = dep_cfg->path && bake_path_exists(dep_cfg->path);
        int dep_rc = 0;

        if (dep_available) {
            int64_t fingerprint = bake_standalone_dep_fingerprint(dep_cfg);
            char *value = flecs_asprintf("%lld", (long long)fingerprint);
            if (!have_files || !prev_value || strcmp(prev_value, value)) {
                ecs_trace(
                    "#[green][#[normal]  amalg#[green]]#[normal] %s => '%s/deps'",
                    dep_cfg->id, cfg->id);
                dep_rc = bake_amalgamate_project(dep_cfg, deps_dir);
            }
            if (dep_rc == 0) {
                ecs_strbuf_append(&marker_buf, "%s=%s\n", dep_cfg->id, value);
            }
            ecs_os_free(value);
        } else if (have_files) {
            ecs_trace(
                "using existing standalone sources for '%s': dependency '%s' "
                "is not available", cfg->id, dep_cfg->id);
            ecs_strbuf_append(&marker_buf, "%s=%s\n",
                dep_cfg->id, prev_value ? prev_value : "0");
        } else {
            if (dep_cfg->path) {
                ecs_err(
                    "cannot generate standalone sources for '%s': source path "
                    "'%s' of dependency '%s' not found",
                    cfg->id, dep_cfg->path, dep_cfg->id);
            } else {
                ecs_err(
                    "cannot generate standalone sources for '%s': dependency "
                    "'%s' not found", cfg->id, dep_cfg->id);
            }
            dep_rc = -1;
        }

        if (dep_rc == 0) {
            bake_strlist_append_unique(&expected_outputs, h_name);
            bake_strlist_append_unique(&expected_outputs, c_name);
            bake_strlist_append_unique(&expected_outputs, cpp_name);
            bake_strlist_append_unique(&expected_outputs, objc_name);
        }

        ecs_os_free(prev_value);
        ecs_os_free(base);
        ecs_os_free(h_name);
        ecs_os_free(c_name);
        ecs_os_free(cpp_name);
        ecs_os_free(objc_name);
        ecs_os_free(h_path);
        ecs_os_free(c_path);
        ecs_os_free(cpp_path);

        if (dep_rc != 0) {
            goto cleanup;
        }
    }

    bake_strlist_append_unique(&expected_outputs, bake_standalone_deps_marker);

    {
        char *marker_content = ecs_strbuf_get(&marker_buf);
        int write_rc = bake_file_write(marker_path, marker_content);
        ecs_os_free(marker_content);
        if (write_rc != 0) {
            goto cleanup;
        }
    }

    /* Only prune unexpected files from a deps directory bake created earlier;
     * a pre-existing deps directory without marker may hold user files. */
    if (prev_marker &&
        bake_cleanup_standalone_outputs(deps_dir, &expected_outputs) != 0)
    {
        goto cleanup;
    }

    rc = 0;
cleanup:
    ecs_strbuf_reset(&marker_buf);
    ecs_os_free(prev_marker);
    ecs_os_free(marker_path);
    ecs_os_free(deps_dir);
    bake_strlist_fini(&expected_outputs);
    return rc;
}

/* A standalone build compiles each dependency's amalgamated source into the
 * application's own binary, using the application's compile flags. A dependency
 * may rely on defines from its own project.json (e.g. GLFW_EXPOSE_NATIVE_COCOA)
 * that are normally applied only when the dependency is built on its own. Carry
 * those defines onto the standalone compile so the amalgamated source sees the
 * same macros it would during a regular build. */
static void bake_apply_standalone_dep_defines(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    bake_lang_cfg_t *c_lang,
    bake_lang_cfg_t *cpp_lang)
{
    const BakeResolvedDeps *resolved =
        ecs_get(ctx->world, project_entity, BakeResolvedDeps);
    int32_t dep_count = resolved ? resolved->dep_count : 0;

    for (int32_t i = 0; i < dep_count; i++) {
        const BakeProject *dep_project =
            ecs_get(ctx->world, resolved->deps[i], BakeProject);
        if (!dep_project || !dep_project->cfg) {
            continue;
        }

        const bake_project_cfg_t *dep_cfg = dep_project->cfg;
        if (dep_cfg->kind == BAKE_PROJECT_CONFIG ||
            dep_cfg->kind == BAKE_PROJECT_TEMPLATE)
        {
            continue;
        }

        bake_strlist_merge_unique(&c_lang->defines, &dep_cfg->c_lang.defines);
        bake_strlist_merge_unique(&c_lang->defines, &dep_cfg->cpp_lang.defines);
        bake_strlist_merge_unique(&cpp_lang->defines, &dep_cfg->c_lang.defines);
        bake_strlist_merge_unique(&cpp_lang->defines, &dep_cfg->cpp_lang.defines);
    }
}

static void bake_fingerprint_append_list(
    ecs_strbuf_t *buf,
    const char *key,
    const bake_strlist_t *list)
{
    ecs_strbuf_append(buf, "%s=", key);
    for (int32_t i = 0; i < list->count; i++) {
        ecs_strbuf_append(buf, "%s;", list->items[i]);
    }
    ecs_strbuf_appendch(buf, '\n');
}

static void bake_fingerprint_append_lang(
    ecs_strbuf_t *buf,
    const char *prefix,
    const bake_lang_cfg_t *lang)
{
    ecs_strbuf_append(buf, "%s.std=%s/%s\n", prefix,
        lang->c_standard ? lang->c_standard : "",
        lang->cpp_standard ? lang->cpp_standard : "");
#define L(f) bake_fingerprint_append_list(buf, prefix, &lang->f)
    L(cflags); L(cxxflags); L(defines); L(ldflags); L(libs);
    L(libpaths); L(include_paths); L(embed);
#undef L
}

/* Captures the configuration that shapes compile and link commands. When it
 * differs from the stored value, all objects are stale even if their mtimes
 * are not: flag changes and bake upgrades do not touch source files.
 * Dependency-derived paths are deliberately not included: they vary between
 * a cold and a warm build (install dirs appear after the first pass), and
 * dependency changes are already tracked through project.json and artefact
 * timestamps. */
static char* bake_compose_build_fingerprint(
    const bake_context_t *ctx,
    const BakeBuildRequest *request,
    const bake_lang_cfg_t *c_lang,
    const bake_lang_cfg_t *cpp_lang,
    const bake_strlist_t *mode_cflags,
    const bake_strlist_t *mode_cxxflags,
    const bake_strlist_t *mode_ldflags)
{
    ecs_strbuf_t buf = ECS_STRBUF_INIT;

    char *exe = bake_os_executable_path();
    ecs_strbuf_append(&buf, "bake=%lld\n",
        exe ? (long long)bake_os_file_mtime(exe) : 0);
    ecs_os_free(exe);

    ecs_strbuf_append(&buf, "cc=%s\ncxx=%s\nkind=%d\nmode=%s\nstrict=%d\ntarget=%s-%s\n",
        ctx->opts.cc ? ctx->opts.cc : "",
        ctx->opts.cxx ? ctx->opts.cxx : "",
        (int)ctx->compiler_kind,
        bake_effective_mode(request->mode),
        ctx->opts.strict ? 1 : 0,
        bake_target_arch(),
        bake_target_os());

    bake_fingerprint_append_lang(&buf, "c", c_lang);
    bake_fingerprint_append_lang(&buf, "cpp", cpp_lang);
    bake_fingerprint_append_list(&buf, "mode_cflags", mode_cflags);
    bake_fingerprint_append_list(&buf, "mode_cxxflags", mode_cxxflags);
    bake_fingerprint_append_list(&buf, "mode_ldflags", mode_ldflags);

    return ecs_strbuf_get(&buf);
}

static int bake_build_one(bake_context_t *ctx, ecs_entity_t project_entity, const BakeBuildRequest *request) {
    const BakeProject *project = ecs_get(ctx->world, project_entity, BakeProject);
    if (!project || !project->cfg) {
        const char *name = ecs_get_name(ctx->world, project_entity);
        ecs_err("cannot build '%s': not a known project", name ? name : "<unnamed>");
        return -1;
    }

    const bake_project_cfg_t *cfg = project->cfg;
    char *builtin_test_src = NULL;
    char *test_exe_path = NULL;
    if (project->external) {
        return 0;
    }

    if (cfg->kind == BAKE_PROJECT_CONFIG || cfg->kind == BAKE_PROJECT_TEMPLATE) {
        const BakeBuildResult *prev_result = ecs_get(ctx->world, project_entity, BakeBuildResult);
        if (prev_result && prev_result->artefact) {
            ecs_os_free((char*)prev_result->artefact);
        }

        BakeBuildResult result = { .status = 0, .artefact = NULL };
        ecs_set_ptr(ctx->world, project_entity, BakeBuildResult, &result);
        if (bake_env_sync_project(ctx, project_entity, &result, request, false) != 0) {
            return -1;
        }
        return 0;
    }

    int rc = -1;
    char *fingerprint = NULL;
    char *fingerprint_path = NULL;
    bake_build_paths_t paths = {0};
    bake_lang_cfg_t c_lang = {0};
    bake_lang_cfg_t cpp_lang = {0};
    bake_strlist_t mode_cflags = {0};
    bake_strlist_t mode_cxxflags = {0};
    bake_strlist_t mode_ldflags = {0};
    bake_compile_list_t units = {0};

    if (bake_build_paths_init(cfg, request->mode, &paths) != 0) {
        ecs_err("failed to initialize build paths for %s (path=%s)", cfg->id, cfg->path ? cfg->path : "<null>");
        goto cleanup;
    }

    if (bake_project_kind_is_harness(cfg->kind)) {
        char *artefact_name = bake_project_cfg_artefact_name(cfg);
        test_exe_path = artefact_name ? bake_path_join(paths.bin_dir, artefact_name) : NULL;
        ecs_os_free(artefact_name);
        if (!test_exe_path) {
            ecs_err("failed to resolve harness executable path for %s", cfg->id);
            goto cleanup;
        }

        bool is_bench = cfg->kind == BAKE_PROJECT_BENCH;
        int32_t harness_step = bake_report_open(ctx->report,
            BAKE_REPORT_KIND_GENERATE,
            is_bench ? "bench harness main" : "test harness main", cfg->id);

        if (is_bench) {
            if (bake_bench_generate_harness(ctx, cfg, test_exe_path) != 0) {
                bake_report_close(ctx->report, harness_step, false,
                    "bench harness generation failed");
                ecs_err("bench harness generation failed for %s", cfg->id);
                goto cleanup;
            }
            bake_report_close(ctx->report, harness_step, true, NULL);

            if (cfg->has_bench_spec) {
                int32_t api_step = bake_report_open(ctx->report,
                    BAKE_REPORT_KIND_GENERATE, "bench api", cfg->id);
                if (bake_bench_generate_builtin_api(ctx, cfg, paths.gen_dir, &builtin_test_src) != 0) {
                    bake_report_close(ctx->report, api_step, false,
                        "bench api generation failed");
                    ecs_err("failed to generate bench API for %s", cfg->id);
                    goto cleanup;
                }
                bake_report_close(ctx->report, api_step, true, NULL);
            }
        } else {
            if (bake_test_generate_harness(ctx, cfg, test_exe_path) != 0) {
                bake_report_close(ctx->report, harness_step, false,
                    "test harness generation failed");
                ecs_err("test harness generation failed for %s", cfg->id);
                goto cleanup;
            }
            bake_report_close(ctx->report, harness_step, true, NULL);

            if (cfg->has_test_spec) {
                int32_t api_step = bake_report_open(ctx->report,
                    BAKE_REPORT_KIND_GENERATE, "test api", cfg->id);
                if (bake_test_generate_builtin_api(ctx, cfg, paths.gen_dir, &builtin_test_src) != 0) {
                    bake_report_close(ctx->report, api_step, false,
                        "test api generation failed");
                    ecs_err("failed to generate test API for %s", cfg->id);
                    goto cleanup;
                }
                bake_report_close(ctx->report, api_step, true, NULL);
            }
        }
    }

    if (ecs_vec_count(&cfg->rules.vec)) {
        int32_t rules_step = bake_report_open(ctx->report,
            BAKE_REPORT_KIND_GENERATE, "rules", cfg->id);
        int rules_rc = bake_execute_rules(ctx->world, project_entity, cfg, &paths);
        bake_report_close(ctx->report, rules_step, rules_rc == 0,
            rules_rc == 0 ? NULL : "rule execution failed");
        if (rules_rc != 0) {
            ecs_err("rule execution failed for %s", cfg->id);
            goto cleanup;
        }
    }

    if (bake_amalgamate_list_count(&cfg->amalgamate) > 0) {
        int32_t amalg_step = bake_report_open(ctx->report,
            BAKE_REPORT_KIND_GENERATE, "amalgamate", cfg->id);
        int amalg_rc = bake_generate_project_amalgamation(cfg);
        bake_report_close(ctx->report, amalg_step, amalg_rc == 0,
            amalg_rc == 0 ? NULL : "amalgamation failed");
        if (amalg_rc != 0) {
            ecs_err("amalgamation failed for %s", cfg->id);
            goto cleanup;
        }
    }

    {
        int32_t header_step = bake_report_open(ctx->report,
            BAKE_REPORT_KIND_GENERATE, "bake_config.h", cfg->id);
        int header_rc = bake_generate_config_header(ctx->world, cfg);
        bake_report_close(ctx->report, header_step, header_rc == 0,
            header_rc == 0 ? NULL : "bake_config.h generation failed");
        if (header_rc != 0) {
            ecs_err("bake_config.h generation failed for %s", cfg->id);
            goto cleanup;
        }
    }

    bool standalone = (request->standalone || cfg->standalone) &&
        (cfg->kind == BAKE_PROJECT_APPLICATION || bake_project_kind_is_harness(cfg->kind));

    if (standalone) {
        int32_t deps_step = bake_report_open(ctx->report,
            BAKE_REPORT_KIND_GENERATE, "standalone deps", cfg->id);
        int deps_rc = bake_prepare_standalone_sources(ctx, project_entity, cfg);
        bake_report_close(ctx->report, deps_step, deps_rc == 0,
            deps_rc == 0 ? NULL : "standalone amalgamation failed");
        if (deps_rc != 0) {
            ecs_err("standalone amalgamation failed for %s", cfg->id);
            goto cleanup;
        }
    }

    bake_lang_cfg_copy(&c_lang, &cfg->c_lang);
    bake_lang_cfg_copy(&cpp_lang, &cfg->cpp_lang);

    bake_apply_dependee_cfg(ctx->world, project_entity, &c_lang, false);
    bake_apply_dependee_cfg(ctx->world, project_entity, &cpp_lang, true);

    if (standalone) {
        char *deps_dir = bake_path_join(cfg->path, "deps");
        bake_strlist_append_unique(&c_lang.include_paths, deps_dir);
        bake_strlist_append_unique(&cpp_lang.include_paths, deps_dir);
        ecs_os_free(deps_dir);

        bake_apply_standalone_dep_defines(ctx, project_entity, &c_lang, &cpp_lang);
    }

    /* Link uses a single language config: fold the C++ link inputs into the C
     * config so that link flags declared under either lang.c or lang.cpp are
     * applied to the project's own binary. */
    bake_strlist_merge_unique(&c_lang.ldflags, &cpp_lang.ldflags);
    bake_strlist_merge_unique(&c_lang.libs, &cpp_lang.libs);
    bake_strlist_merge_unique(&c_lang.libpaths, &cpp_lang.libpaths);
    bake_strlist_merge_unique(&c_lang.embed, &cpp_lang.embed);

    if (bake_project_kind_is_harness(cfg->kind)) {
        bake_strlist_append_unique(&c_lang.include_paths, paths.gen_dir);
        bake_strlist_append_unique(&cpp_lang.include_paths, paths.gen_dir);
    }

    bake_strlist_init(&mode_cflags);
    bake_strlist_init(&mode_cxxflags);
    bake_strlist_init(&mode_ldflags);
    bake_add_mode_flags(request->mode, ctx->compiler_kind, &mode_cflags, &mode_cxxflags, &mode_ldflags);
    bake_add_strict_flags(ctx->opts.strict, ctx->compiler_kind, &mode_cflags, &mode_cxxflags, &mode_ldflags);
    bake_add_coverage_flags(ctx->opts.coverage, &mode_cflags, &mode_cxxflags, &mode_ldflags);

    if (bake_project_kind_is_harness(cfg->kind) &&
        ctx->compiler_kind != BAKE_COMPILER_MSVC &&
        !bake_target_is_emscripten())
    {
        bake_strlist_append(&mode_cflags, "-pthread");
        bake_strlist_append(&mode_cxxflags, "-pthread");
        bake_strlist_append(&mode_ldflags, "-pthread");
    }

    bake_compile_list_init(&units);
    bool include_deps = true;
    if (cfg->kind == BAKE_PROJECT_APPLICATION || bake_project_kind_is_harness(cfg->kind)) {
        include_deps = standalone;
    }

    if (bake_collect_compile_units(
        cfg,
        &paths,
        bake_project_kind_is_harness(cfg->kind),
        include_deps,
        ctx->compiler_kind,
        &units) != 0)
    {
        ecs_err("failed to collect source files for %s", cfg->id);
        goto cleanup;
    }

    if (builtin_test_src) {
#if defined(_WIN32)
        const char *obj_ext = ".obj";
#else
        const char *obj_ext = ".o";
#endif
        const char *builtin_name = cfg->kind == BAKE_PROJECT_BENCH ?
            "generated_bake_bench" : "generated_bake_test";
        char *obj_name = flecs_asprintf("%s%s", builtin_name, obj_ext);
        char *obj_path = bake_path_join(paths.obj_dir, obj_name);
        if (bake_os_mkdirs(paths.obj_dir) != 0) {
            ecs_os_free(obj_name);
            ecs_os_free(obj_path);
            ecs_err("failed to add generated harness API source for %s", cfg->id);
            goto cleanup;
        }
        bake_compile_list_append(&units, builtin_test_src, obj_path, NULL, false);
        ecs_os_free(obj_name);
        ecs_os_free(obj_path);
    }

    fingerprint = bake_compose_build_fingerprint(
        ctx, request, &c_lang, &cpp_lang,
        &mode_cflags, &mode_cxxflags, &mode_ldflags);
    fingerprint_path = bake_path_join(paths.build_root, ".bake_cmd");
    char *prev_fingerprint = bake_file_read(fingerprint_path, NULL);
    bool flags_changed = !prev_fingerprint ||
        strcmp(prev_fingerprint, fingerprint) != 0;
    ecs_os_free(prev_fingerprint);

    int32_t compiled_count = 0;
    if (bake_compile_units_parallel(
        ctx, project_entity, cfg, &units, &c_lang, &cpp_lang,
        &mode_cflags, &mode_cxxflags, flags_changed, &compiled_count) != 0)
    {
        ecs_err("compilation failed for %s", cfg->id);
        goto cleanup;
    }

    char *artefact = NULL;
    bool linked = false;
    int32_t link_step = bake_report_open(ctx->report, BAKE_REPORT_KIND_LINK,
        "link", cfg->id);
    int link_rc = bake_link_project_binary(
        ctx, project_entity, cfg, &paths, &units, &c_lang, &mode_ldflags,
        flags_changed, standalone, &artefact, &linked);
    bake_report_close(ctx->report, link_step, link_rc == 0,
        link_rc == 0 ? NULL : "link command failed");
    if (link_rc != 0) {
        ecs_err("link failed for %s", cfg->id);
        goto cleanup;
    }

    if (flags_changed && bake_file_write(fingerprint_path, fingerprint) != 0) {
        ecs_os_free(artefact);
        goto cleanup;
    }

    const BakeBuildResult *prev_result = ecs_get(ctx->world, project_entity, BakeBuildResult);
    if (prev_result && prev_result->artefact) {
        ecs_os_free((char*)prev_result->artefact);
    }

    BakeBuildResult result = {
        .status = 0,
        .artefact = artefact
    };
    ecs_set_ptr(ctx->world, project_entity, BakeBuildResult, &result);

    bool rebuilt = compiled_count > 0 || linked;
    if (bake_env_sync_project(ctx, project_entity, &result, request, rebuilt) != 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
    ecs_os_free(fingerprint);
    ecs_os_free(fingerprint_path);
    bake_compile_list_fini(&units);
    bake_strlist_fini(&mode_cflags);
    bake_strlist_fini(&mode_cxxflags);
    bake_strlist_fini(&mode_ldflags);
    bake_lang_cfg_fini(&c_lang);
    bake_lang_cfg_fini(&cpp_lang);
    ecs_os_free(test_exe_path);
    ecs_os_free(builtin_test_src);
    bake_build_paths_fini(&paths);
    return rc;
}

static bool bake_is_unresolved_external_dependency(const BakeProject *project) {
    if (!project || !project->external) {
        return false;
    }
    return bake_project_is_placeholder(project);
}

static int bake_validate_build_graph_dependencies(const ecs_world_t *world, ecs_entity_t *order, int32_t count) {
    int32_t unresolved_count = 0;

    for (int32_t i = 0; i < count; i++) {
        const BakeProject *project = ecs_get(world, order[i], BakeProject);
        if (!bake_is_unresolved_external_dependency(project)) {
            continue;
        }

        ecs_err("unresolved dependency: %s", project->cfg->id);
        unresolved_count++;
    }

    if (unresolved_count) {
        return -1;
    }

    return 0;
}

static int bake_prepare_bundles_recursive(
    bake_context_t *ctx,
    ecs_entity_t entity,
    ecs_map_t *visited)
{
    if (ecs_map_get(visited, (ecs_map_key_t)entity)) {
        return 0;
    }
    ecs_map_insert(visited, (ecs_map_key_t)entity, 0);

    const BakeProject *project = ecs_get(ctx->world, entity, BakeProject);
    if (!project || !project->cfg) {
        return 0;
    }

    if (bake_bundle_prepare_for_project(ctx, project->cfg) != 0) {
        return -1;
    }

    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(ctx->world, entity, BakeDependsOn, i);
        if (!dep) {
            break;
        }
        if (bake_prepare_bundles_recursive(ctx, dep, visited) != 0) {
            return -1;
        }
    }

    return 0;
}

static int bake_prepare_build_graph_bundles(
    bake_context_t *ctx,
    const ecs_entity_t *order,
    int32_t count)
{
    if (!ctx->prepare_bundles) {
        return 0;
    }

    ecs_map_t visited = {0};
    ecs_map_init(&visited, NULL);

    int rc = 0;
    for (int32_t i = 0; i < count; i++) {
        if (bake_prepare_bundles_recursive(ctx, order[i], &visited) != 0) {
            rc = -1;
            break;
        }
    }

    ecs_map_fini(&visited);

    if (rc == 0 && bake_model_refresh_resolved_deps(ctx->world, ctx->opts.mode) != 0) {
        rc = -1;
    }

    return rc;
}

static int bake_execute_build_graph(bake_context_t *ctx, const char *target, bool recursive, bool standalone) {
    bake_model_mark_build_targets(ctx->world, target, ctx->opts.mode, recursive, standalone);

    int rc = -1;
    ecs_entity_t *order = NULL;
    int32_t count = 0;
    if (bake_model_build_order(ctx->world, &order, &count) != 0) goto cleanup;

    if (target && target[0] && count == 0) {
        ecs_err("target not found: %s", target);
        goto cleanup;
    }

    if (bake_prepare_build_graph_bundles(ctx, order, count) != 0) goto cleanup;

    if (bake_validate_build_graph_dependencies(ctx->world, order, count) != 0) goto cleanup;

    bake_report_collect_loc(ctx, order, count);

    for (int32_t i = 0; i < count; i++) {
        const BakeBuildRequest *req = ecs_get(ctx->world, order[i], BakeBuildRequest);
        if (!req) continue;

        const BakeProject *project = ecs_get(ctx->world, order[i], BakeProject);
        bool own_project = project && project->cfg && !project->external;
        if (own_project) {
            bake_log_build_header(ctx, project->cfg);
        }

        int32_t project_step = own_project
            ? bake_report_open(ctx->report, BAKE_REPORT_KIND_PROJECT,
                project->cfg->id, project->cfg->id)
            : BAKE_REPORT_NO_STEP;

        int build_rc = bake_build_one(ctx, order[i], req);
        bake_report_close(ctx->report, project_step, build_rc == 0, NULL);

        if (build_rc != 0) {
            goto cleanup;
        }
    }
    rc = 0;

cleanup:
    ecs_os_free(order);
    return rc;
}

/* Discovers projects for the effective target and returns the resolved
 * target path (NULL when the target is not an existing path) through
 * target_path_out so commands do not have to resolve it again. */
static int bake_prepare_discovery(bake_context_t *ctx, char **target_path_out) {
    int rc = -1;
    int32_t step = bake_report_open(ctx->report, BAKE_REPORT_KIND_DISCOVERY,
        "discovery", NULL);
    const char *target = bake_effective_build_target(ctx);
    char *target_path = bake_resolve_target_path(ctx, target);
    char *target_root = NULL;
    if (target_path) {
        target_root = bake_path_is_dir(target_path)
            ? ecs_os_strdup(target_path)
            : bake_path_dirname(target_path);
    }

    if (target_root && bake_discover_projects(ctx, target_root, true) < 0) {
        goto cleanup;
    }

    bool discover_cwd = !(target_root && bake_path_equal_normalized(target_root, ctx->opts.cwd));
    if (discover_cwd && bake_discover_projects(ctx, ctx->opts.cwd, true) < 0) {
        goto cleanup;
    }

    if (ctx->opts.recursive && bake_discover_dependency_sources(ctx) < 0) {
        goto cleanup;
    }

    ctx->compiler_kind = bake_detect_compiler_kind(ctx->opts.cc, ctx->opts.cxx);
    if (bake_coverage_prepare(ctx) != 0) {
        goto cleanup;
    }
    rc = 0;

cleanup:
    bake_report_close(ctx->report, step, rc == 0,
        rc == 0 ? NULL : "project discovery failed");
    if (rc == 0 && target_path_out) {
        *target_path_out = target_path;
        target_path = NULL;
    }
    ecs_os_free(target_path);
    ecs_os_free(target_root);
    return rc;
}

static int bake_clean_project(const bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    char *bake_dir = NULL;
    if (ctx && ctx->opts.local_env && ctx->bake_home && cfg->id && cfg->id[0]) {
        char *build_root = bake_path_join(ctx->bake_home, "build");
        bake_dir = bake_path_join(build_root, cfg->id);
        ecs_os_free(build_root);
    } else {
        bake_dir = bake_path_join(cfg->path, ".bake");
    }

    int rc = 0;
    if (bake_path_exists(bake_dir) && bake_path_is_dir(bake_dir)) {
        bake_dir_entry_t *entries = NULL;
        int32_t entry_count = 0;
        if (bake_dir_list(bake_dir, &entries, &entry_count) != 0) {
            ecs_os_free(bake_dir);
            return -1;
        }

        bool kept_bundles = false;
        for (int32_t i = 0; i < entry_count && rc == 0; i++) {
            const bake_dir_entry_t *entry = &entries[i];
            if (bake_is_dot_dir(entry->name)) {
                continue;
            }
            /* Preserve fetched/built bundle artefacts so a clean does not
             * trigger expensive re-clones and re-builds. */
            if (!strcmp(entry->name, "bundles")) {
                kept_bundles = true;
                continue;
            }
            if (entry->is_dir) {
                rc = bake_os_rmtree(entry->path);
            } else {
                rc = bake_remove_file_if_exists(entry->path);
            }
        }
        bake_dir_entries_free(entries, entry_count);

        if (rc == 0 && !kept_bundles) {
            /* Match the historical clean behaviour: leave nothing behind when
             * the project has no bundle artefacts to preserve. */
            bake_os_rmdir(bake_dir);
        }
    }

    ecs_os_free(bake_dir);

    char *profraw = bake_path_join(cfg->path, "default.profraw");
    if (rc == 0) {
        rc = bake_remove_file_if_exists(profraw);
    }
    ecs_os_free(profraw);

    /* Generated standalone sources in deps/ are preserved, like bundle
     * artefacts: they are refreshed from dependency fingerprints on build and
     * may not be regenerable when the dependency source is not available. */
    return rc;
}

static int bake_build_clean_prepared(bake_context_t *ctx, const char *target) {
    int rc = -1;
    bake_model_mark_build_targets(ctx->world, target, ctx->opts.mode, ctx->opts.recursive, ctx->opts.standalone);

    ecs_entity_t *order = NULL;
    int32_t count = 0;
    if (bake_model_build_order(ctx->world, &order, &count) != 0) {
        goto cleanup;
    }

    if (count == 0) {
        rc = 0;
        goto cleanup;
    }

    for (int32_t i = 0; i < count; i++) {
        const BakeProject *project = ecs_get(ctx->world, order[i], BakeProject);
        if (!project || !project->cfg || !project->cfg->path || project->external) {
            continue;
        }

        ecs_trace("#[green][#[normal]  clean#[green]]#[normal] %s", project->cfg->id);
        if (bake_clean_project(ctx, project->cfg) != 0) {
            goto cleanup;
        }
    }

    char *report_dir = bake_coverage_report_dir();
    if (report_dir && bake_path_is_dir(report_dir) && bake_os_rmtree(report_dir) != 0) {
        ecs_os_free(report_dir);
        goto cleanup;
    }
    ecs_os_free(report_dir);

    rc = 0;
cleanup:
    ecs_os_free(order);
    return rc;
}

int bake_build_clean(bake_context_t *ctx) {
    char *target_path = NULL;
    if (bake_prepare_discovery(ctx, &target_path) != 0) {
        return -1;
    }

    const char *target = target_path
        ? target_path
        : bake_effective_build_target(ctx);
    int rc = bake_build_clean_prepared(ctx, target);
    ecs_os_free(target_path);
    return rc;
}

int bake_build_coverage_report(bake_context_t *ctx) {
    char *target_path = NULL;
    ctx->discover_tests = true;
    if (bake_prepare_discovery(ctx, &target_path) != 0) {
        return -1;
    }

    int rc = bake_coverage_report_generate(ctx, target_path);
    ecs_os_free(target_path);
    return rc;
}

int bake_build(bake_context_t *ctx) {
    char *target_path = NULL;
    if (bake_prepare_discovery(ctx, &target_path) != 0) {
        return -1;
    }

    const char *target_resolved = target_path
        ? target_path
        : bake_effective_build_target(ctx);

    int rc = 0;
    if (bake_execute_build_graph(ctx, target_resolved, true, ctx->opts.standalone) != 0) {
        rc = -1;
    }

    ecs_os_free(target_path);
    return rc;
}

int bake_build_rebuild(bake_context_t *ctx) {
    char *target_path = NULL;
    if (bake_prepare_discovery(ctx, &target_path) != 0) {
        return -1;
    }

    const char *target = target_path
        ? target_path
        : bake_effective_build_target(ctx);

    int rc = bake_build_clean_prepared(ctx, target);
    if (rc == 0 && bake_execute_build_graph(ctx, target, true, ctx->opts.standalone) != 0) {
        rc = -1;
    }

    ecs_os_free(target_path);
    return rc;
}

/* A wasm artefact is not an executable, it is a page a browser has to fetch
 * over http. Serve the directory it was linked into and open it. */
static const char *bake_em_serve_script =
    "import functools, http.server, socketserver, sys, threading, webbrowser\n"
    "directory, first_port, page = sys.argv[1], int(sys.argv[2]), sys.argv[3]\n"
    "scan = int(sys.argv[4])\n"
    "handler = functools.partial(\n"
    "    http.server.SimpleHTTPRequestHandler, directory=directory)\n"
    "socketserver.TCPServer.allow_reuse_address = True\n"
    "server = None\n"
    "for port in range(first_port, first_port + scan):\n"
    "    try:\n"
    "        server = socketserver.TCPServer(('127.0.0.1', port), handler)\n"
    "        break\n"
    "    except OSError:\n"
    "        continue\n"
    "if server is None:\n"
    "    sys.exit('no free port in %d..%d' % (first_port, first_port + scan - 1))\n"
    "url = 'http://localhost:%d/%s' % (server.server_address[1], page)\n"
    "print('serving %s at %s (ctrl-c to stop)' % (directory, url), flush=True)\n"
    "threading.Timer(0.3, webbrowser.open, [url]).start()\n"
    "try:\n"
    "    server.serve_forever()\n"
    "except KeyboardInterrupt:\n"
    "    pass\n";

int32_t bake_em_serve_first_port(int32_t requested) {
    if (requested < 1 || requested > 65535) {
        return BAKE_EM_SERVE_PORT_DEFAULT;
    }
    return requested;
}

static int bake_run_em_artefact(const char *artefact, int32_t requested_port) {
    char *dir = bake_path_dirname(artefact);
    char *page = bake_path_basename(artefact);

    /* emcc only emits a page when a shell file is configured. Fall back to a
     * hand written index.html, and to the directory listing otherwise. */
    size_t page_len = strlen(page);
    if (page_len < 5 || strcmp(page + page_len - 5, ".html")) {
        char *index = bake_path_join(dir, "index.html");
        bool has_index = bake_path_exists(index) != 0;
        ecs_os_free(index);

        if (has_index) {
            ecs_os_free(page);
            page = ecs_os_strdup("index.html");
        } else {
            ecs_warn("no html page next to %s: set \"shell\" in the language "
                "config or add an index.html that loads %s", artefact, page);
            ecs_os_free(page);
            page = ecs_os_strdup("");
        }
    }

    char port[16];
    ecs_os_snprintf(port, sizeof(port), "%d",
        bake_em_serve_first_port(requested_port));

    char scan[16];
    ecs_os_snprintf(scan, sizeof(scan), "%d", BAKE_EM_SERVE_PORT_SCAN);

    ecs_trace("#[green][#[normal]    run#[green]]#[normal] "
        "python3 -m http.server %s -d %s", port, dir);

    const char *argv[] = {
        "python3", "-c", bake_em_serve_script, dir, port, page, scan, NULL
    };

    bake_process_result_t result = {0};
    int rc = bake_proc_run_argv(argv, &result);
    if (rc != 0) {
        ecs_err("failed to start a web server for %s, serve it with: "
            "python3 -m http.server %s -d %s", artefact, port, dir);
    }

    ecs_os_free(page);
    ecs_os_free(dir);
    return rc;
}

int bake_build_run(bake_context_t *ctx) {
    int rc = 0;

    if (bake_env_export_vars(ctx) != 0) {
        return -1;
    }

    char *target_path = NULL;
    if (bake_prepare_discovery(ctx, &target_path) != 0) {
        bake_report_finish(ctx->report, false);
        return -1;
    }

    const char *effective_target = bake_effective_build_target(ctx);
    const char *target_resolved = target_path ? target_path : effective_target;

    int build_rc = bake_execute_build_graph(
        ctx, target_resolved, true, ctx->opts.standalone);
    bake_report_finish(ctx->report, build_rc == 0);
    if (build_rc != 0) {
        rc = -1;
        goto cleanup;
    }

    const char *target = effective_target;

    ecs_entity_t project_entity = 0;
    const BakeProject *project = bake_model_find_project(ctx->world, target, &project_entity);
    if ((!project || !project_entity) && target_path) {
        project = bake_model_find_project_by_path(ctx->world, target_path, &project_entity);
    }

    if (!project || !project_entity) {
        ecs_err("target not found: %s", target);
        rc = -1;
        goto cleanup;
    }

    const BakeBuildResult *result = ecs_get(ctx->world, project_entity, BakeBuildResult);
    if (!result || !result->artefact) {
        rc = -1;
        goto cleanup;
    }

    if (!strcmp(ctx->opts.command, "bench") || project->cfg->kind == BAKE_PROJECT_BENCH) {
        rc = bake_bench_run_project(ctx, project->cfg, result->artefact);
        goto cleanup;
    }

    if (!strcmp(ctx->opts.command, "test") || project->cfg->kind == BAKE_PROJECT_TEST) {
        rc = bake_test_run_project(ctx, project->cfg, result->artefact);
        goto cleanup;
    }

    if (!strcmp(ctx->opts.command, "run")) {
        if (bake_target_is_emscripten()) {
            rc = bake_run_em_artefact(result->artefact, ctx->opts.port);
            goto cleanup;
        }

        if (bake_coverage_export_env(ctx, project->cfg) != 0) {
            rc = -1;
            goto cleanup;
        }

        char *exe_path = bake_path_resolve(result->artefact);
        char *run_dir = bake_project_run_dir(project->cfg);

        ecs_strbuf_t cmd = ECS_STRBUF_INIT;
        if (ctx->opts.run_prefix) {
            ecs_strbuf_append(&cmd, "%s ", ctx->opts.run_prefix);
        }

        ecs_strbuf_append(&cmd, "\"%s\"", exe_path ? exe_path : result->artefact);
        for (int i = 0; i < ctx->opts.run_argc; i++) {
            ecs_strbuf_append(&cmd, " \"%s\"", ctx->opts.run_argv[i]);
        }

        char *env_name = NULL;
        bake_ps_env_kind_t env_kind = bake_ps_env_from_home(
            ctx->bake_home, &env_name);
        bake_ps_info_t ps = {
            .project = project->cfg->id,
            .cfg = bake_effective_mode(ctx->opts.mode),
            .env = env_name,
            .env_kind = env_kind,
            .bake_home = ctx->bake_home,
            .workspace = ctx->opts.cwd,
            .kind = "run",
            .announce = true
        };

        char *cmd_str = ecs_strbuf_get(&cmd);
        rc = bake_run_command_tracked(cmd_str, true, run_dir, &ps);
        ecs_os_free(env_name);
        ecs_os_free(cmd_str);
        ecs_os_free(exe_path);
        ecs_os_free(run_dir);
        goto cleanup;
    }

cleanup:
    ecs_os_free(target_path);
    return rc;
}
