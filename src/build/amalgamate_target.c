#include "build_internal.h"
#include "bake/build_report.h"
#include "bake/os.h"

#include "parson.h"

static const char *bake_amalgamate_dll_marker =
    "// Comment out this line when using as DLL\n";

static const char *bake_amalgamate_stage_exts[] = {
    ".h", ".c", ".cpp", "_objc.m"
};

bool bake_amalgamate_prefix_valid(const char *prefix) {
    if (!prefix || !prefix[0] || prefix[0] == '.') {
        return false;
    }

    for (const char *ptr = prefix; *ptr; ptr++) {
        char ch = *ptr;
        bool ok =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '.' ||
            ch == '_' ||
            ch == '-';
        if (!ok) {
            return false;
        }
    }

    return true;
}

static bool bake_amalgamate_file_exists(
    const char *dir,
    const char *prefix,
    const char *ext)
{
    char *name = flecs_asprintf("%s%s", prefix, ext);
    char *path = bake_path_join(dir, name);
    bool exists = bake_path_exists(path) != 0;
    ecs_os_free(path);
    ecs_os_free(name);
    return exists;
}

static bool bake_amalgamate_files_exist(const char *dir, const char *prefix) {
    return bake_amalgamate_file_exists(dir, prefix, ".h") &&
        (bake_amalgamate_file_exists(dir, prefix, ".c") ||
         bake_amalgamate_file_exists(dir, prefix, ".cpp"));
}

static char* bake_amalgamate_find_config(
    const bake_project_cfg_t *parent,
    const char *prefix,
    bake_strlist_t *prefixes)
{
    char *dir = NULL;
    int32_t count = bake_amalgamate_list_count(&parent->amalgamate);
    for (int32_t i = 0; i < count; i++) {
        const bake_amalgamate_cfg_t *amalg =
            bake_amalgamate_list_get(&parent->amalgamate, i);
        char *base = bake_amalgamate_output_base(parent, amalg);
        bake_strlist_append_unique(prefixes, base);
        if (!dir && !strcmp(base, prefix)) {
            dir = (amalg->path && amalg->path[0])
                ? bake_path_join(parent->path, amalg->path)
                : ecs_os_strdup(parent->path);
        }
        ecs_os_free(base);
    }
    return dir;
}

static void bake_amalgamate_report_missing(
    const bake_project_cfg_t *parent,
    const char *prefix,
    const bake_strlist_t *prefixes)
{
    if (prefixes->count) {
        char *joined = bake_strlist_join(prefixes, ", ");
        ecs_err("cannot build amalgamation '%s': project '%s' has no "
            "amalgamation with prefix '%s' and '%s' has no %s.h with %s.c or "
            "%s.cpp (available prefixes: %s)",
            prefix, parent->id, prefix, parent->path, prefix, prefix, prefix,
            joined);
        ecs_os_free(joined);
    } else {
        ecs_err("cannot build amalgamation '%s': project '%s' does not "
            "configure amalgamation and '%s' has no %s.h with %s.c or %s.cpp",
            prefix, parent->id, parent->path, prefix, prefix, prefix);
    }
}

static bake_project_cfg_t* bake_amalgamate_new_project(
    const char *prefix,
    const char *gen_dir,
    const bake_project_cfg_t *parent)
{
    bake_project_cfg_t *cfg = ecs_os_calloc_t(bake_project_cfg_t);
    bake_project_cfg_init(cfg);

    ecs_os_free(cfg->id);
    cfg->id = ecs_os_strdup(prefix);
    cfg->path = ecs_os_strdup(gen_dir);
    cfg->build_dir = ecs_os_strdup(gen_dir);
    cfg->output_name = ecs_os_strdup(prefix);
    cfg->kind = BAKE_PROJECT_PACKAGE;
    cfg->public_project = false;
    cfg->shared_library = true;

    if (parent->language) {
        ecs_os_free(cfg->language);
        cfg->language = ecs_os_strdup(parent->language);
    }

    bake_strlist_merge_unique(&cfg->use, &parent->use);
    bake_strlist_merge_unique(&cfg->use_private, &parent->use_private);

    bake_lang_cfg_t *langs[] = { &cfg->c_lang, &cfg->cpp_lang };
    const bake_lang_cfg_t *parent_langs[] = { &parent->c_lang, &parent->cpp_lang };
    for (int32_t i = 0; i < 2; i++) {
        bake_lang_cfg_t *lang = langs[i];
        const bake_lang_cfg_t *parent_lang = parent_langs[i];
        bake_strlist_merge_unique(&lang->libs, &parent_lang->libs);
        bake_strlist_merge_unique(&lang->ldflags, &parent_lang->ldflags);
        bake_strlist_merge_unique(&lang->libpaths, &parent_lang->libpaths);
        if (parent_lang->c_standard) {
            ecs_os_free(lang->c_standard);
            lang->c_standard = ecs_os_strdup(parent_lang->c_standard);
        }
        if (parent_lang->cpp_standard) {
            ecs_os_free(lang->cpp_standard);
            lang->cpp_standard = ecs_os_strdup(parent_lang->cpp_standard);
        }
    }

    return cfg;
}

int bake_amalgamate_target_add(
    bake_context_t *ctx,
    const char *root,
    char **target_out)
{
    const char *prefix = ctx->opts.amalgamate;
    int rc = -1;
    char *project_json = bake_path_join(root, "project.json");
    char *dir = NULL;
    char *gen_dir = NULL;
    bool generate = false;
    bake_project_cfg_t *parent = NULL;
    bake_strlist_t prefixes;
    bake_strlist_init(&prefixes);

    if (!bake_path_exists(project_json)) {
        ecs_err("cannot build amalgamation '%s': no project.json in '%s'",
            prefix, root);
        goto cleanup;
    }

    parent = ecs_os_calloc_t(bake_project_cfg_t);
    bake_project_cfg_init(parent);
    if (bake_project_cfg_load_file(project_json, parent) != 0) {
        ecs_err("failed to parse %s", project_json);
        goto cleanup;
    }

    dir = bake_amalgamate_find_config(parent, prefix, &prefixes);
    generate = dir != NULL;
    if (!dir && bake_amalgamate_files_exist(parent->path, prefix)) {
        dir = ecs_os_strdup(parent->path);
    }

    if (!dir) {
        bake_amalgamate_report_missing(parent, prefix, &prefixes);
        goto cleanup;
    }

    char *base = bake_project_build_base(parent->path, parent->id);
    char *amalg_root = bake_path_join(base, "amalgamate");
    gen_dir = bake_path_join(amalg_root, prefix);
    ecs_os_free(amalg_root);
    ecs_os_free(base);

    bake_project_cfg_t *cfg = bake_amalgamate_new_project(prefix, gen_dir, parent);
    cfg->amalgamate_src = ecs_os_calloc_t(bake_amalgamate_src_t);
    cfg->amalgamate_src->project = parent;
    cfg->amalgamate_src->prefix = ecs_os_strdup(prefix);
    cfg->amalgamate_src->dir = dir;
    cfg->amalgamate_src->generate = generate;
    parent = NULL;
    dir = NULL;

    ecs_entity_t entity = bake_model_add_project(ctx->world, cfg, false);
    const BakeProject *project = entity
        ? ecs_get(ctx->world, entity, BakeProject)
        : NULL;
    if (!project || project->cfg != cfg) {
        ecs_err("cannot build amalgamation '%s': project id conflict", prefix);
        goto cleanup;
    }

    *target_out = gen_dir;
    gen_dir = NULL;
    rc = 0;

cleanup:
    if (parent) {
        bake_project_cfg_fini(parent);
        ecs_os_free(parent);
    }
    bake_strlist_fini(&prefixes);
    ecs_os_free(project_json);
    ecs_os_free(dir);
    ecs_os_free(gen_dir);
    return rc;
}

static char* bake_amalgamate_dll_header(const char *content, char **macro_out) {
    size_t marker_len = strlen(bake_amalgamate_dll_marker);
    if (strncmp(content, bake_amalgamate_dll_marker, marker_len)) {
        return NULL;
    }

    const char *define = content + marker_len;
    if (strncmp(define, "#define ", 8)) {
        return NULL;
    }

    const char *name = define + 8;
    const char *end = name;
    while ((*end >= 'a' && *end <= 'z') || (*end >= 'A' && *end <= 'Z') ||
        (*end >= '0' && *end <= '9') || *end == '_')
    {
        end++;
    }

    size_t len = (size_t)(end - name);
    if (len <= 7 || strncmp(end - 7, "_STATIC", 7) ||
        (*end != '\n' && *end != '\r'))
    {
        return NULL;
    }

    char *macro = ecs_os_malloc(len - 7 + 1);
    memcpy(macro, name, len - 7);
    macro[len - 7] = '\0';
    *macro_out = macro;

    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_appendstrn(&buf, content, (int32_t)marker_len);
    ecs_strbuf_appendstr(&buf, "// ");
    ecs_strbuf_appendstr(&buf, define);
    return ecs_strbuf_get(&buf);
}

static int bake_amalgamate_stage_file(
    const char *src_path,
    const char *dst_path,
    bool header,
    char **macro_out)
{
    char *content = bake_file_read(src_path, NULL);
    if (!content) {
        ecs_err("failed to read %s", src_path);
        return -1;
    }

    if (header) {
        char *dll = bake_amalgamate_dll_header(content, macro_out);
        if (dll) {
            ecs_os_free(content);
            content = dll;
        }
    }

    int rc = bake_file_write(dst_path, content);
    ecs_os_free(content);
    return rc;
}

static JSON_Value* bake_amalgamate_json_list(const bake_strlist_t *list) {
    JSON_Value *value = json_value_init_array();
    JSON_Array *array = json_value_get_array(value);
    for (int32_t i = 0; i < list->count; i++) {
        json_array_append_string(array, list->items[i]);
    }
    return value;
}

static int bake_amalgamate_write_project_json(const bake_project_cfg_t *cfg) {
    JSON_Value *root_value = json_value_init_object();
    JSON_Object *root = json_value_get_object(root_value);
    json_object_set_string(root, "id", cfg->id);
    json_object_set_string(root, "type", "package");

    JSON_Value *value = json_value_init_object();
    JSON_Object *value_obj = json_value_get_object(value);
    json_object_set_boolean(value_obj, "public", false);
    if (cfg->language) {
        json_object_set_string(value_obj, "language", cfg->language);
    }
    json_object_set_value(value_obj, "use",
        bake_amalgamate_json_list(&cfg->use));
    json_object_set_value(value_obj, "use-private",
        bake_amalgamate_json_list(&cfg->use_private));
    json_object_set_value(root, "value", value);

    char *json = json_serialize_to_string_pretty(root_value);
    json_value_free(root_value);
    if (!json) {
        return -1;
    }

    char *path = bake_path_join(cfg->path, "project.json");
    int rc = bake_file_write(path, json);
    ecs_os_free(path);
    json_free_serialized_string(json);
    return rc;
}

static int bake_amalgamate_stage_files(bake_project_cfg_t *cfg) {
    const bake_amalgamate_src_t *src = cfg->amalgamate_src;
    int rc = -1;
    char *macro = NULL;
    char *src_dir = bake_path_join(cfg->path, "src");

    if (!bake_amalgamate_files_exist(src->dir, src->prefix)) {
        ecs_err("cannot build amalgamation '%s': no %s.h with %s.c or %s.cpp "
            "in '%s'", src->prefix, src->prefix, src->prefix, src->prefix,
            src->dir);
        goto cleanup;
    }

    int32_t ext_count = (int32_t)(sizeof(bake_amalgamate_stage_exts) /
        sizeof(bake_amalgamate_stage_exts[0]));
    for (int32_t i = 0; i < ext_count; i++) {
        const char *ext = bake_amalgamate_stage_exts[i];
        char *name = flecs_asprintf("%s%s", src->prefix, ext);
        char *from = bake_path_join(src->dir, name);
        char *to = bake_path_join(src_dir, name);
        int file_rc = bake_path_exists(from)
            ? bake_amalgamate_stage_file(from, to, i == 0, &macro)
            : bake_remove_file_if_exists(to);
        ecs_os_free(to);
        ecs_os_free(from);
        ecs_os_free(name);
        if (file_rc != 0) {
            goto cleanup;
        }
    }

    if (bake_amalgamate_write_project_json(cfg) != 0) {
        goto cleanup;
    }

    if (macro) {
        char *define = flecs_asprintf("%s_EXPORTS", macro);
        bake_strlist_append_unique(&cfg->c_lang.defines, define);
        bake_strlist_append_unique(&cfg->cpp_lang.defines, define);
        ecs_os_free(define);
    }

    rc = 0;

cleanup:
    ecs_os_free(macro);
    ecs_os_free(src_dir);
    return rc;
}

int bake_amalgamate_target_stage(
    bake_context_t *ctx,
    bake_project_cfg_t *cfg)
{
    const bake_amalgamate_src_t *src = cfg->amalgamate_src;
    int32_t step = bake_report_open(ctx->report,
        BAKE_REPORT_KIND_GENERATE, "amalgamate", cfg->id);

    int rc = 0;
    if (src->generate) {
        char *display = bake_display_path(src->dir, ctx->opts.cwd);
        ecs_trace("#[green][#[normal]  amalg#[green]]#[normal] %s => '%s/%s'",
            src->project->id, display, src->prefix);
        ecs_os_free(display);
        rc = bake_generate_project_amalgamation(src->project, src->prefix);
    }

    if (rc == 0) {
        rc = bake_amalgamate_stage_files(cfg);
    }

    bake_report_close(ctx->report, step, rc == 0,
        rc == 0 ? NULL : "amalgamation failed");
    if (rc != 0) {
        ecs_err("amalgamation failed for %s", cfg->id);
    }
    return rc;
}
