#include "build_internal.h"
#include "bake/os.h"

#include <ctype.h>

static int bake_is_compile_source(const char *path, bool *cpp_out) {
    if (bake_has_suffix(path, ".c")) {
        *cpp_out = false;
        return 1;
    }

    if (bake_has_suffix(path, ".cpp") || bake_has_suffix(path, ".cc") || bake_has_suffix(path, ".cxx") || bake_has_suffix(path, ".C")) {
        *cpp_out = true;
        return 1;
    }

#if defined(__APPLE__)
    /* Objective-C sources only compile for Apple targets; skip them when
       cross-compiling (e.g. to emscripten). */
    if (!strcmp(bake_target_os(), "Darwin")) {
        if (bake_has_suffix(path, ".m")) {
            *cpp_out = false;
            return 1;
        }

        if (bake_has_suffix(path, ".mm")) {
            *cpp_out = true;
            return 1;
        }
    }
#endif

    return 0;
}

static char* bake_rel_path(const char *base, const char *path) {
    size_t base_len = strlen(base);
    if (!strncmp(base, path, base_len) && (path[base_len] == bake_path_sep() || path[base_len] == '/')) {
        return ecs_os_strdup(path + base_len + 1);
    }
    return bake_path_basename(path);
}

void bake_compile_list_init(bake_compile_list_t *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void bake_compile_list_fini(bake_compile_list_t *list) {
    for (int32_t i = 0; i < list->count; i++) {
        ecs_os_free(list->items[i].src);
        ecs_os_free(list->items[i].obj);
        ecs_os_free(list->items[i].dep);
    }
    ecs_os_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int bake_compile_list_append(
    bake_compile_list_t *list,
    const char *src,
    const char *obj,
    const char *dep,
    bool cpp)
{
    if (list->count == list->capacity) {
        int32_t next = list->capacity ? list->capacity * 2 : 32;
        list->items = ecs_os_realloc_n(list->items, bake_compile_unit_t, next);
        list->capacity = next;
    }

    bake_compile_unit_t *unit = &list->items[list->count];
    unit->src = ecs_os_strdup(src);
    unit->obj = ecs_os_strdup(obj);
    unit->dep = dep ? ecs_os_strdup(dep) : NULL;
    unit->cpp = cpp;
    unit->lint = false;
    list->count++;
    return 0;
}

void bake_build_paths_fini(bake_build_paths_t *paths) {
#define F(n) ecs_os_free(paths->n)
    F(build_root); F(obj_dir); F(bin_dir); F(lib_dir); F(gen_dir);
#undef F
    memset(paths, 0, sizeof(*paths));
}

int bake_build_paths_init(const bake_project_cfg_t *cfg, const char *mode, bake_build_paths_t *paths) {
    memset(paths, 0, sizeof(*paths));

    paths->build_root = bake_project_build_root(cfg->path, cfg->id, mode);
    if (!paths->build_root) {
        ecs_err("bake_build_paths_init: failed to build build_root");
        return -1;
    }

    paths->obj_dir = bake_path_join(paths->build_root, "obj");
    paths->bin_dir = ecs_os_strdup(paths->build_root);
    paths->lib_dir = ecs_os_strdup(paths->build_root);
    paths->gen_dir = bake_path_join(paths->build_root, "generated");

    if (bake_os_mkdirs(paths->build_root) != 0 ||
        bake_os_mkdirs(paths->obj_dir) != 0 ||
        bake_os_mkdirs(paths->gen_dir) != 0)
    {
        ecs_err("bake_build_paths_init: mkdir failed for %s", paths->build_root);
        bake_build_paths_fini(paths);
        return -1;
    }

    return 0;
}

typedef struct bake_collect_ctx_t {
    const bake_project_cfg_t *cfg;
    const bake_build_paths_t *paths;
    bake_compile_list_t *units;
    bake_compiler_kind_t compiler_kind;
    bool lint;
} bake_collect_ctx_t;

static int bake_collect_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_collect_ctx_t *ctx = ctx_ptr;

    if (entry->is_dir) {
        if (bake_is_dot_dir(entry->name)) {
            return 1;
        }
        if (entry->name[0] == '.') {
            return 1;
        }
        return 0;
    }

    bool cpp = false;
    if (!bake_is_compile_source(entry->path, &cpp)) {
        return 0;
    }

    char *rel = bake_rel_path(ctx->cfg->path, entry->path);
    for (char *p = rel; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        } else if (*p == ':') {
            *p = '_';
        }
    }

    bool lint = ctx->lint && ctx->cfg->lint.command &&
        !(bake_project_kind_is_harness(ctx->cfg->kind) && !strcmp(rel, "src/main.c"));

#if defined(_WIN32)
    const char *obj_ext = ".obj";
#else
    const char *obj_ext = ".o";
#endif

    char *obj_file = flecs_asprintf("%s%s", rel, obj_ext);
    ecs_os_free(rel);

    char *obj_path = bake_path_join(ctx->paths->obj_dir, obj_file);
    ecs_os_free(obj_file);

    char *obj_parent = bake_path_dirname(obj_path);
    if (bake_os_mkdirs(obj_parent) != 0) {
        ecs_os_free(obj_parent);
        ecs_os_free(obj_path);
        return -1;
    }
    ecs_os_free(obj_parent);

    char *dep_path = NULL;
    if (ctx->compiler_kind != BAKE_COMPILER_MSVC) {
        dep_path = flecs_asprintf("%s.d", obj_path);
    }

    int rc = bake_compile_list_append(ctx->units, entry->path, obj_path, dep_path, cpp);
    if (rc == 0) {
        ctx->units->items[ctx->units->count - 1].lint = lint;
    }
    ecs_os_free(dep_path);
    ecs_os_free(obj_path);
    return rc;
}

int bake_collect_compile_units(
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths,
    bool include_tests,
    bool include_deps,
    bake_compiler_kind_t compiler_kind,
    bake_compile_list_t *units)
{
    bake_collect_ctx_t ctx = {
        .cfg = cfg,
        .paths = paths,
        .units = units,
        .compiler_kind = compiler_kind
    };

    struct { const char *name; bool enabled; bool lint; } dirs[] = {
        { "src", true, true },
        { "deps", include_deps, false },
        { "test", include_tests, true }
    };

    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        if (!dirs[d].enabled) {
            continue;
        }
        ctx.lint = dirs[d].lint;
        char *dir = bake_path_join(cfg->path, dirs[d].name);
        if (bake_path_exists(dir)) {
            if (bake_dir_walk_recursive(dir, bake_collect_visit, &ctx) != 0) {
                ecs_os_free(dir);
                return -1;
            }
        }
        ecs_os_free(dir);
    }

    ctx.lint = false;
    for (int32_t i = 0; i < cfg->bundle_sources.count; i++) {
        const char *path = cfg->bundle_sources.items[i];
        char *base = bake_path_basename(path);
        bake_dir_entry_t entry = {
            .name = base,
            .path = (char*)path,
            .is_dir = false
        };
        int rc = bake_collect_visit(&entry, &ctx);
        ecs_os_free(base);
        if (rc != 0) {
            return -1;
        }
    }

    return 0;
}

typedef struct bake_rule_exec_ctx_t {
    const bake_project_cfg_t *cfg;
    const bake_build_paths_t *paths;
    const char *ext;
    const char *command;
} bake_rule_exec_ctx_t;

static int bake_rule_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_rule_exec_ctx_t *ctx = ctx_ptr;
    if (entry->is_dir) {
        if (bake_is_dot_dir(entry->name) || entry->name[0] == '.') {
            return 1;
        }
        return 0;
    }

    if (!bake_has_suffix(entry->path, ctx->ext)) {
        return 0;
    }

    char *stem = bake_path_stem(entry->path);
    const struct { const char *needle; const char *value; } subs[] = {
        {"{input}",   entry->path},
        {"{project}", ctx->cfg->path},
        {"{out_dir}", ctx->paths->gen_dir},
        {"{stem}",    stem},
    };
    char *cmd = ecs_os_strdup(ctx->command);
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        char *next = bake_text_replace(cmd, subs[i].needle, subs[i].value);
        ecs_os_free(cmd);
        cmd = next;
    }
    ecs_os_free(stem);

    int rc = bake_run_command(cmd, true);
    ecs_os_free(cmd);
    return rc;
}

int bake_execute_rules(
    ecs_world_t *world,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths)
{
    ecs_iter_t children = ecs_children(world, project_entity);
    while (ecs_children_next(&children)) {
        for (int32_t i = 0; i < children.count; i++) {
            const BakeBuildRule *rule = ecs_get(world, children.entities[i], BakeBuildRule);
            if (!rule || !rule->ext || !rule->command) {
                continue;
            }

            bake_rule_exec_ctx_t ctx = {
                .cfg = cfg,
                .paths = paths,
                .ext = rule->ext,
                .command = rule->command
            };
            if (bake_dir_walk_recursive(cfg->path, bake_rule_visit, &ctx) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static void bake_append_dep_include(
    ecs_strbuf_t *header,
    const char *dep_id,
    bool standalone_local_headers)
{
    char *dep_macro = bake_project_id_as_macro(dep_id);
    if (standalone_local_headers) {
        ecs_strbuf_append(header, "#include \"../../deps/%s.h\"\n", dep_macro);
    } else {
        ecs_strbuf_append(header, "#include <%s.h>\n", dep_macro);
    }

    ecs_os_free(dep_macro);
}

static void bake_append_dep_includes(
    ecs_strbuf_t *header,
    const bake_strlist_t *deps,
    bool standalone_local_headers)
{
    if (!deps || deps->count == 0) {
        ecs_strbuf_appendstr(header, "/* No dependencies */\n");
        return;
    }

    for (int32_t i = 0; i < deps->count; i++) {
        bake_append_dep_include(header, deps->items[i], standalone_local_headers);
    }
}

int bake_generate_config_header(ecs_world_t *world, const bake_project_cfg_t *cfg) {
    int rc = -1;
    ecs_entity_t project = 0;
    const BakeProject *p = bake_model_find_project(world, cfg->id, &project);
    if (!p || !project) {
        return -1;
    }

    char *include_root = NULL;
    char *project_dir = NULL;
    char *header_path = NULL;
    char *project_macro = NULL;
    char *project_macro_upper = NULL;
    char *guard_macro = NULL;
    char *api_macro = NULL;
    char *content = NULL;
    bake_strlist_t public_deps = {0};
    ecs_strbuf_t header = ECS_STRBUF_INIT;

    if (!cfg || !cfg->id || !cfg->path) {
        goto cleanup;
    }

    include_root = bake_path_join(cfg->path, "include");
    if (!bake_path_exists(include_root)) {
        rc = 0;
        goto cleanup;
    }

    project_dir = bake_project_id_as_dash(cfg->id);
    char *tmp = bake_path_join(include_root, project_dir);
    ecs_os_free(project_dir);
    project_dir = tmp;
    tmp = NULL;

    if (bake_os_mkdirs(project_dir) != 0) {
        goto cleanup;
    }

    header_path = bake_path_join(project_dir, "bake_config.h");
    project_macro = bake_project_id_as_macro(cfg->id);
    project_macro_upper = bake_macro_upper(project_macro);
    guard_macro = flecs_asprintf("%s_BAKE_CONFIG_H", project_macro_upper);
    api_macro = flecs_asprintf("%s_API", project_macro_upper);

    bool standalone_local_headers =
        cfg->standalone &&
        (cfg->kind == BAKE_PROJECT_APPLICATION || bake_project_kind_is_harness(cfg->kind));

    bake_strlist_init(&public_deps);

    for (int32_t i = 0; i < cfg->use.count; i++) {
        bake_strlist_append_unique(&public_deps, cfg->use.items[i]);
    }

    for (int32_t i = 0; i < cfg->use.count; i++) {
        const BakeProject *dep_p = bake_model_find_project(world, cfg->use.items[i], NULL);
        if (!dep_p || !dep_p->cfg || !dep_p->cfg->dependee.cfg) {
            continue;
        }

        const bake_strlist_t *dependee_use = &dep_p->cfg->dependee.cfg->use;
        for (int32_t d = 0; d < dependee_use->count; d++) {
            const char *dep_id = dependee_use->items[d];
            if (!dep_id) {
                continue;
            }

            if (bake_strlist_contains(&cfg->use_private, dep_id)) {
                continue;
            }
            bake_strlist_append_unique(&public_deps, dep_id);
        }
    }

    ecs_strbuf_appendstr(&header,
        "/*\n"
        "                                   )\n"
        "                                  (.)\n"
        "                                  .|.\n"
        "                                  | |\n"
        "                              _.--| |--._\n"
        "                           .-';  ;`-'& ; `&.\n"
        "                          \\   &  ;    &   &_/\n"
        "                           |\"\"\"---...---\"\"\"|\n"
        "                           \\ | | | | | | | /\n"
        "                            `---.|.|.|.---'\n"
        "\n"
        " * This file is generated by bake.lang.c for your convenience. Headers of\n"
        " * dependencies will automatically show up in this file. Include bake_config.h\n"
        " * in your main project file. Do not edit! */\n\n");

    ecs_strbuf_append(&header, "#ifndef %s\n", guard_macro);
    ecs_strbuf_append(&header, "#define %s\n\n", guard_macro);

    ecs_strbuf_appendstr(&header, "/* Headers of public dependencies */\n");
    bake_append_dep_includes(&header, &public_deps, standalone_local_headers);
    if (cfg->kind == BAKE_PROJECT_TEST && cfg->has_test_spec) {
        ecs_strbuf_appendstr(&header, "#include <bake_test.h>\n");
    }
    if (cfg->kind == BAKE_PROJECT_BENCH && cfg->has_bench_spec) {
        ecs_strbuf_appendstr(&header, "#include <bake_bench.h>\n");
    }
    ecs_strbuf_appendstr(&header, "\n");

    if (cfg->use_private.count) {
        ecs_strbuf_appendstr(&header, "/* Headers of private dependencies */\n");
        if (cfg->kind == BAKE_PROJECT_PACKAGE) {
            ecs_strbuf_append(&header, "#ifdef %s_EXPORTS\n", project_macro);
            bake_append_dep_includes(&header, &cfg->use_private, standalone_local_headers);
            ecs_strbuf_appendstr(&header, "#endif\n\n");
        } else {
            bake_append_dep_includes(&header, &cfg->use_private, standalone_local_headers);
            ecs_strbuf_appendstr(&header, "\n");
        }
    }

    if (cfg->kind == BAKE_PROJECT_PACKAGE) {
        ecs_strbuf_appendstr(&header, "/* Convenience macro for exporting symbols */\n");
        ecs_strbuf_append(&header, "#ifndef %s_STATIC\n", project_macro);
        ecs_strbuf_append(&header, "#if defined(%s_EXPORTS) && (defined(_MSC_VER) || defined(__MINGW32__))\n", project_macro);
        ecs_strbuf_append(&header, "  #define %s __declspec(dllexport)\n", api_macro);
        ecs_strbuf_append(&header, "#elif defined(%s_EXPORTS)\n", project_macro);
        ecs_strbuf_append(&header, "  #define %s __attribute__((__visibility__(\"default\")))\n", api_macro);
        /* No dllimport branch: with MSVC bake links packages as static
         * libraries, so dependees must reference their symbols directly. */
        ecs_strbuf_appendstr(&header, "#else\n");
        ecs_strbuf_append(&header, "  #define %s\n", api_macro);
        ecs_strbuf_appendstr(&header, "#endif\n");
        ecs_strbuf_appendstr(&header, "#else\n");
        ecs_strbuf_append(&header, "  #define %s\n", api_macro);
        ecs_strbuf_appendstr(&header, "#endif\n\n");
    }

    ecs_strbuf_appendstr(&header, "#endif\n\n");

    content = ecs_strbuf_get(&header);
    if (bake_file_write(header_path, content) != 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (!content) {
        content = ecs_strbuf_get(&header);
    }
    ecs_os_free(content);
    ecs_os_free(include_root);
    ecs_os_free(project_dir);
    ecs_os_free(header_path);
    ecs_os_free(project_macro);
    ecs_os_free(project_macro_upper);
    ecs_os_free(guard_macro);
    ecs_os_free(api_macro);
    bake_strlist_fini(&public_deps);
    return rc;
}

static void bake_merge_lang_cfg_unique(
    bake_lang_cfg_t *dst,
    const bake_lang_cfg_t *src)
{
    bake_strlist_merge_unique(&dst->cflags, &src->cflags);
    bake_strlist_merge_unique(&dst->cxxflags, &src->cxxflags);
    bake_strlist_merge_unique(&dst->defines, &src->defines);
    bake_strlist_merge_unique(&dst->ldflags, &src->ldflags);
    bake_strlist_merge_unique(&dst->libs, &src->libs);
    bake_strlist_merge_unique(&dst->libpaths, &src->libpaths);
    bake_strlist_merge_unique(&dst->include_paths, &src->include_paths);
}

int bake_apply_dependee_cfg(
    ecs_world_t *world,
    ecs_entity_t project_entity,
    bake_lang_cfg_t *dst,
    bool cpp_lang)
{
    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, project_entity, BakeDependsOn, i);
        if (!dep) {
            break;
        }

        const BakeProject *dep_p = ecs_get(world, dep, BakeProject);
        if (!dep_p || !dep_p->cfg) {
            continue;
        }

        if (!dep_p->cfg->dependee.cfg) {
            continue;
        }

        const bake_lang_cfg_t *dep_lang = cpp_lang
            ? &dep_p->cfg->dependee.cfg->cpp_lang
            : &dep_p->cfg->dependee.cfg->c_lang;
        bake_merge_lang_cfg_unique(dst, dep_lang);
    }

    return 0;
}
