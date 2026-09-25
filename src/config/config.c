#include "bake/config.h"
#include "bake/common.h"
#include "bake/os.h"

#include <ctype.h>

#include "parson.h"
#include "common/json_helpers.h"
#include "common/strutil.h"

static void bake_lang_cfg_init_impl(bake_lang_cfg_t *cfg, bool set_defaults);
static void bake_project_cfg_init_impl(bake_project_cfg_t *cfg, bool init_dependee, bool set_defaults);
static void bake_project_cfg_fini_impl(bake_project_cfg_t *cfg, bool fini_dependee);
static int bake_parse_project_cfg_object(
    const JSON_Object *object,
    bake_project_cfg_t *cfg,
    bool parse_meta,
    bool parse_dependee);
static int bake_parse_rules(
    const JSON_Array *rules,
    bake_rule_list_t *rules_out);
static int bake_parse_bundles_object(
    const JSON_Object *bundles_obj,
    bake_bundle_list_t *bundles);

static const char *bake_cfg_eval_mode = "debug";
static const char *bake_cfg_eval_target = NULL;

void bake_project_cfg_set_eval_context(const char *mode, const char *target) {
    bake_cfg_eval_mode = (mode && mode[0]) ? mode : "debug";
    bake_cfg_eval_target = (target && target[0]) ? target : NULL;
}

static int bake_json_conditional_key_matches(const char *key) {
    if (!key) {
        return 0;
    }

    char *raw = ecs_os_strdup(key);
    size_t len = strlen(raw);
    if (len < 4 || raw[0] != '$' || raw[1] != '{' || raw[len - 1] != '}') {
        ecs_os_free(raw);
        return 0;
    }

    raw[len - 1] = '\0';
    char *expr = bake_ltrim(raw + 2);
    bake_rtrim(expr);

    char *space = expr;
    while (*space && !isspace((unsigned char)*space)) {
        space++;
    }

    if (!*space) {
        ecs_os_free(raw);
        return 0;
    }

    *space = '\0';
    char *kind = expr;
    char *value = bake_ltrim(space + 1);
    bake_rtrim(value);

    int match = 0;
    if (value[0]) {
        if (!strcmp(kind, "os")) {
            match = !strcasecmp(value, bake_target_os());
        } else if (!strcmp(kind, "arch")) {
            match = !strcasecmp(value, bake_target_arch());
        } else if (!strcmp(kind, "cfg")) {
            const char *mode = bake_cfg_eval_mode ? bake_cfg_eval_mode : "debug";
            match = !strcasecmp(value, mode);
        } else if (!strcmp(kind, "target")) {
            match = bake_cfg_eval_target && !strcasecmp(value, bake_cfg_eval_target);
        } else {
            ecs_warn("unknown conditional key kind '%s' in '%s'", kind, key);
        }
    }

    ecs_os_free(raw);
    return match;
}

bool bake_language_is_cpp(const bake_project_cfg_t *cfg) {
    return cfg->language &&
        (!strcmp(cfg->language, "cpp") || !strcmp(cfg->language, "c++"));
}

const char* bake_project_kind_str(bake_project_kind_t kind) {
    switch (kind) {
    case BAKE_PROJECT_APPLICATION: return "application";
    case BAKE_PROJECT_PACKAGE: return "package";
    case BAKE_PROJECT_CONFIG: return "config";
    case BAKE_PROJECT_TEST: return "test";
    case BAKE_PROJECT_BENCH: return "bench";
    case BAKE_PROJECT_TEMPLATE: return "template";
    default: return "application";
    }
}

bake_project_kind_t bake_project_kind_parse(const char *value) {
    if (!value) {
        return BAKE_PROJECT_APPLICATION;
    }
    if (!strcmp(value, "application") || !strcmp(value, "app")) {
        return BAKE_PROJECT_APPLICATION;
    }
    if (!strcmp(value, "package") || !strcmp(value, "lib")) {
        return BAKE_PROJECT_PACKAGE;
    }
    if (!strcmp(value, "config")) {
        return BAKE_PROJECT_CONFIG;
    }
    if (!strcmp(value, "test")) {
        return BAKE_PROJECT_TEST;
    }
    if (!strcmp(value, "bench")) {
        return BAKE_PROJECT_BENCH;
    }
    if (!strcmp(value, "template")) {
        return BAKE_PROJECT_TEMPLATE;
    }
    ecs_warn("unknown project type '%s', defaulting to application", value);
    return BAKE_PROJECT_APPLICATION;
}

bool bake_project_kind_has_artefact(bake_project_kind_t kind) {
    return kind == BAKE_PROJECT_PACKAGE ||
        kind == BAKE_PROJECT_APPLICATION ||
        kind == BAKE_PROJECT_TEST ||
        kind == BAKE_PROJECT_BENCH;
}

bool bake_project_kind_is_harness(bake_project_kind_t kind) {
    return kind == BAKE_PROJECT_TEST || kind == BAKE_PROJECT_BENCH;
}

char* bake_project_run_dir(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->path || !cfg->path[0]) {
        return NULL;
    }

    char *resolved = bake_path_resolve(cfg->path);
    if (resolved && !bake_path_is_dir(resolved)) {
        ecs_os_free(resolved);
        return NULL;
    }

    return resolved;
}

const char* bake_project_cfg_shell(const bake_project_cfg_t *cfg) {
    if (!cfg) {
        return NULL;
    }
    if (cfg->c_lang.shell && cfg->c_lang.shell[0]) {
        return cfg->c_lang.shell;
    }
    if (cfg->cpp_lang.shell && cfg->cpp_lang.shell[0]) {
        return cfg->cpp_lang.shell;
    }
    return NULL;
}

char* bake_project_cfg_artefact_name(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->output_name || !bake_project_kind_has_artefact(cfg->kind)) {
        return NULL;
    }

#if defined(_WIN32)
    const char *exe_ext = ".exe";
    const char *lib_ext = ".lib";
    const char *lib_prefix = "";
#else
    const char *exe_ext = "";
    const char *lib_ext = ".a";
    const char *lib_prefix = "lib";
#endif

    const char *target_exe_ext = bake_target_exe_ext();
    if (target_exe_ext && target_exe_ext[0]) {
        exe_ext = target_exe_ext;
    }

    /* A shell file makes emcc emit a page that loads the module */
    if (bake_target_is_emscripten() && bake_project_cfg_shell(cfg)) {
        exe_ext = ".html";
    }

    if (cfg->kind == BAKE_PROJECT_PACKAGE && cfg->shared_library) {
#if defined(_WIN32)
        return flecs_asprintf("%s.dll", cfg->output_name);
#else
        const char *shared_ext =
            !strcmp(bake_target_os(), "Darwin") ? ".dylib" : ".so";
        return flecs_asprintf("lib%s%s", cfg->output_name, shared_ext);
#endif
    }

    if (cfg->kind == BAKE_PROJECT_PACKAGE) {
        return flecs_asprintf("%s%s%s", lib_prefix, cfg->output_name, lib_ext);
    }

    return flecs_asprintf("%s%s", cfg->output_name, exe_ext);
}

void bake_rule_list_init(bake_rule_list_t *list) {
    memset(&list->vec, 0, sizeof(list->vec));
}

void bake_rule_list_fini(bake_rule_list_t *list) {
    bake_rule_t *items = ecs_vec_first_t(&list->vec, bake_rule_t);
    int32_t count = ecs_vec_count(&list->vec);
    for (int32_t i = 0; i < count; i++) {
        ecs_os_free(items[i].ext);
        ecs_os_free(items[i].command);
    }
    ecs_vec_fini_t(NULL, &list->vec, bake_rule_t);
}

int bake_rule_list_append(bake_rule_list_t *list, const char *ext, const char *command) {
    bake_rule_t *rule = ecs_vec_append_t(NULL, &list->vec, bake_rule_t);
    rule->ext = ecs_os_strdup(ext);
    rule->command = ecs_os_strdup(command);
    return 0;
}

void bake_bundle_list_init(bake_bundle_list_t *list) {
    memset(&list->vec, 0, sizeof(list->vec));
}

void bake_bundle_list_fini(bake_bundle_list_t *list) {
    bake_bundle_t *items = ecs_vec_first_t(&list->vec, bake_bundle_t);
    int32_t count = ecs_vec_count(&list->vec);
    for (int32_t i = 0; i < count; i++) {
        ecs_os_free(items[i].id);
        ecs_os_free(items[i].repository);
        ecs_os_free(items[i].branch);
        ecs_os_free(items[i].tag);
        ecs_os_free(items[i].commit);
        ecs_os_free(items[i].subdir);
        ecs_os_free(items[i].library);
        ecs_os_free(items[i].build_system);
        ecs_os_free(items[i].profile);
        bake_strlist_fini(&items[i].includes);
        bake_strlist_fini(&items[i].sources);
        bake_strlist_fini(&items[i].cmake_args);
        bake_strlist_fini(&items[i].cargo_args);
        bake_strlist_fini(&items[i].libs);
        bake_strlist_fini(&items[i].ldflags);
    }
    ecs_vec_fini_t(NULL, &list->vec, bake_bundle_t);
}

bake_bundle_t* bake_bundle_list_append(bake_bundle_list_t *list) {
    bake_bundle_t *item = ecs_vec_append_t(NULL, &list->vec, bake_bundle_t);
    memset(item, 0, sizeof(*item));
    bake_strlist_init(&item->includes);
    bake_strlist_init(&item->sources);
    bake_strlist_init(&item->cmake_args);
    bake_strlist_init(&item->cargo_args);
    bake_strlist_init(&item->libs);
    bake_strlist_init(&item->ldflags);
    return item;
}

int32_t bake_bundle_list_count(const bake_bundle_list_t *list) {
    return ecs_vec_count(&list->vec);
}

bake_bundle_t* bake_bundle_list_get(const bake_bundle_list_t *list, int32_t index) {
    if (index < 0 || index >= ecs_vec_count(&list->vec)) {
        return NULL;
    }
    return &ecs_vec_first_t(&list->vec, bake_bundle_t)[index];
}

const bake_bundle_t* bake_bundle_list_find(const bake_bundle_list_t *list, const char *id) {
    if (!id) {
        return NULL;
    }
    int32_t count = ecs_vec_count(&list->vec);
    bake_bundle_t *items = ecs_vec_first_t(&list->vec, bake_bundle_t);
    for (int32_t i = 0; i < count; i++) {
        if (items[i].id && !strcmp(items[i].id, id)) {
            return &items[i];
        }
    }
    return NULL;
}

void bake_amalgamate_list_init(bake_amalgamate_list_t *list) {
    memset(&list->vec, 0, sizeof(list->vec));
}

void bake_amalgamate_list_fini(bake_amalgamate_list_t *list) {
    bake_amalgamate_cfg_t *items = ecs_vec_first_t(&list->vec, bake_amalgamate_cfg_t);
    int32_t count = ecs_vec_count(&list->vec);
    for (int32_t i = 0; i < count; i++) {
        ecs_os_free(items[i].path);
        ecs_os_free(items[i].prefix);
        bake_strlist_fini(&items[i].disable_flags);
    }
    ecs_vec_fini_t(NULL, &list->vec, bake_amalgamate_cfg_t);
}

bake_amalgamate_cfg_t* bake_amalgamate_list_append(bake_amalgamate_list_t *list) {
    bake_amalgamate_cfg_t *item = ecs_vec_append_t(NULL, &list->vec, bake_amalgamate_cfg_t);
    memset(item, 0, sizeof(*item));
    bake_strlist_init(&item->disable_flags);
    return item;
}

int32_t bake_amalgamate_list_count(const bake_amalgamate_list_t *list) {
    return ecs_vec_count(&list->vec);
}

bake_amalgamate_cfg_t* bake_amalgamate_list_get(const bake_amalgamate_list_t *list, int32_t index) {
    if (index < 0 || index >= ecs_vec_count(&list->vec)) {
        return NULL;
    }
    return &ecs_vec_first_t(&list->vec, bake_amalgamate_cfg_t)[index];
}

static void bake_lang_cfg_init_impl(bake_lang_cfg_t *cfg, bool set_defaults) {
#define F(n) bake_strlist_init(&cfg->n)
    F(cflags); F(cxxflags); F(defines); F(ldflags); F(libs);
    F(libpaths); F(include_paths); F(embed);
#undef F
    cfg->c_standard = set_defaults ? ecs_os_strdup("c99") : NULL;
    cfg->cpp_standard = set_defaults ? ecs_os_strdup("c++17") : NULL;
    cfg->shell = NULL;
}

void bake_lang_cfg_init(bake_lang_cfg_t *cfg) {
    bake_lang_cfg_init_impl(cfg, true);
}

void bake_lang_cfg_copy(bake_lang_cfg_t *dst, const bake_lang_cfg_t *src) {
    bake_lang_cfg_init_impl(dst, false);

    dst->c_standard = ecs_os_strdup(src->c_standard);
    dst->cpp_standard = ecs_os_strdup(src->cpp_standard);
    dst->shell = ecs_os_strdup(src->shell);

#define CP(f) bake_strlist_copy(&dst->f, &src->f)
    CP(cflags); CP(cxxflags); CP(defines); CP(ldflags); CP(libs);
    CP(libpaths); CP(include_paths); CP(embed);
#undef CP
}

void bake_lang_cfg_fini(bake_lang_cfg_t *cfg) {
#define F(n) bake_strlist_fini(&cfg->n)
    F(cflags); F(cxxflags); F(defines); F(ldflags); F(libs);
    F(libpaths); F(include_paths); F(embed);
#undef F
    ecs_os_free(cfg->c_standard);
    ecs_os_free(cfg->cpp_standard);
    ecs_os_free(cfg->shell);
    cfg->c_standard = NULL;
    cfg->cpp_standard = NULL;
    cfg->shell = NULL;
}

void bake_dependee_cfg_init(bake_dependee_cfg_t *cfg) {
    cfg->cfg = ecs_os_calloc_t(bake_project_cfg_t);
    bake_project_cfg_init_impl(cfg->cfg, false, false);
    cfg->json = NULL;
}

void bake_dependee_cfg_fini(bake_dependee_cfg_t *cfg) {
    if (cfg->cfg) {
        bake_project_cfg_fini_impl(cfg->cfg, false);
        ecs_os_free(cfg->cfg);
    }
    ecs_os_free(cfg->json);
    memset(cfg, 0, sizeof(*cfg));
}

static void bake_project_cfg_init_impl(bake_project_cfg_t *cfg, bool init_dependee, bool set_defaults) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind = BAKE_PROJECT_APPLICATION;
    cfg->has_test_spec = false;
    cfg->has_bench_spec = false;
    cfg->public_project = set_defaults;
    cfg->language = set_defaults ? ecs_os_strdup("c") : NULL;

#define F(n) bake_strlist_init(&cfg->n)
    F(use); F(use_private);
    F(bundle_includes); F(bundle_libpaths); F(bundle_libs); F(bundle_ldflags);
    F(bundle_sources);
#undef F

    if (init_dependee) {
        bake_dependee_cfg_init(&cfg->dependee);
    } else {
        memset(&cfg->dependee, 0, sizeof(cfg->dependee));
    }
    bake_lang_cfg_init_impl(&cfg->c_lang, set_defaults);
    bake_lang_cfg_init_impl(&cfg->cpp_lang, set_defaults);
    bake_rule_list_init(&cfg->rules);
    bake_bundle_list_init(&cfg->bundles);
    bake_amalgamate_list_init(&cfg->amalgamate);
}

void bake_project_cfg_init(bake_project_cfg_t *cfg) {
    bake_project_cfg_init_impl(cfg, true, true);
}

static void bake_project_cfg_fini_impl(bake_project_cfg_t *cfg, bool fini_dependee) {
#define F(n) ecs_os_free(cfg->n)
    F(id); F(path); F(language); F(output_name); F(lint.command);
    F(build_dir);
#undef F

    if (cfg->amalgamate_src) {
        if (cfg->amalgamate_src->project) {
            bake_project_cfg_fini(cfg->amalgamate_src->project);
            ecs_os_free(cfg->amalgamate_src->project);
        }
        ecs_os_free(cfg->amalgamate_src->prefix);
        ecs_os_free(cfg->amalgamate_src->dir);
        ecs_os_free(cfg->amalgamate_src);
    }

#define F(n) bake_strlist_fini(&cfg->n)
    F(use); F(use_private);
    F(bundle_includes); F(bundle_libpaths); F(bundle_libs); F(bundle_ldflags);
    F(bundle_sources);
#undef F
    bake_amalgamate_list_fini(&cfg->amalgamate);

    if (fini_dependee) {
        bake_dependee_cfg_fini(&cfg->dependee);
    } else {
        if (cfg->dependee.cfg) {
            bake_project_cfg_fini_impl(cfg->dependee.cfg, false);
            ecs_os_free(cfg->dependee.cfg);
        }
        ecs_os_free(cfg->dependee.json);
    }
    bake_lang_cfg_fini(&cfg->c_lang);
    bake_lang_cfg_fini(&cfg->cpp_lang);
    bake_rule_list_fini(&cfg->rules);
    bake_bundle_list_fini(&cfg->bundles);

    memset(cfg, 0, sizeof(*cfg));
}

void bake_project_cfg_fini(bake_project_cfg_t *cfg) {
    bake_project_cfg_fini_impl(cfg, true);
}

/* Single source of truth for the language config keys; consumed by both the
 * lang.c/lang.cpp section parser and the project-level parser. */
#define BAKE_LANG_ARRAY_KEYS(X) \
    X("cflags", cflags) \
    X("cxxflags", cxxflags) \
    X("defines", defines) \
    X("ldflags", ldflags) \
    X("lib", libs) \
    X("libpath", libpaths) \
    X("include", include_paths) \
    X("embed", embed)

#define BAKE_PROJECT_META_KEYS(X) \
    X("id") X("type") X("value") X("test") X("bench") X("rules") X("bundle") \
    X("lint") X("dependee") X("lang") X("lang.c") X("lang.cpp")

#define BAKE_PROJECT_VALUE_KEYS(X) \
    X("language") X("output") X("private") X("public") X("standalone") \
    X("amalgamate") X("amalgamate-path") X("use") X("use-private") \
    X("c-standard") X("cpp-standard")

#define BAKE_KEY(key) key,
#define BAKE_KEY_ARR(key, field) key,

static const char *bake_project_object_keys[] = {
    BAKE_PROJECT_META_KEYS(BAKE_KEY)
    BAKE_PROJECT_VALUE_KEYS(BAKE_KEY)
    BAKE_LANG_ARRAY_KEYS(BAKE_KEY_ARR)
    NULL
};

static const char *bake_lang_object_keys[] = {
    BAKE_LANG_ARRAY_KEYS(BAKE_KEY_ARR)
    "c-standard", "cpp-standard", "shell",
    NULL
};

static const char *bake_bundle_object_keys[] = {
    "repository", "branch", "tag", "commit", "subdir", "library",
    "build-system", "profile", "header-only", "include", "sources",
    "cmake-args", "cargo-args", "lib", "ldflags",
    NULL
};

static const char *bake_lint_object_keys[] = {
    "command", "action",
    NULL
};

static const char *bake_amalgamate_object_keys[] = {
    "path", "prefix", "disable-flags",
    NULL
};

#undef BAKE_KEY
#undef BAKE_KEY_ARR

static bool bake_json_key_is_conditional(const char *key) {
    size_t len = key ? strlen(key) : 0;
    return len >= 4 && key[0] == '$' && key[1] == '{' && key[len - 1] == '}';
}

static void bake_key_normalize(const char *key, char *out, size_t size) {
    size_t w = 0;
    for (const char *p = key; *p && w + 1 < size; p++) {
        if (*p == '-' || *p == '_') {
            continue;
        }
        out[w++] = (char)tolower((unsigned char)*p);
    }
    out[w] = '\0';
}

static const char* bake_key_find_similar(const char *key, const char **known) {
    char norm_key[128];
    char norm_known[128];
    bake_key_normalize(key, norm_key, sizeof(norm_key));
    if (strlen(norm_key) < 3) {
        return NULL;
    }

    for (int32_t i = 0; known[i]; i++) {
        bake_key_normalize(known[i], norm_known, sizeof(norm_known));
        if (!strcmp(norm_key, norm_known)) {
            return known[i];
        }
    }

    for (int32_t i = 0; known[i]; i++) {
        bake_key_normalize(known[i], norm_known, sizeof(norm_known));
        if (strlen(norm_known) < 3) {
            continue;
        }
        if (strstr(norm_key, norm_known) || strstr(norm_known, norm_key)) {
            return known[i];
        }
    }

    return NULL;
}

static void bake_warn_similar_keys(
    const JSON_Object *object,
    const char **known,
    const char *context)
{
    if (!object) {
        return;
    }

    size_t key_count = json_object_get_count(object);
    for (size_t i = 0; i < key_count; i++) {
        const char *key = json_object_get_name(object, i);
        if (!key || bake_json_key_is_conditional(key)) {
            continue;
        }

        bool known_key = false;
        for (int32_t k = 0; known[k]; k++) {
            if (!strcmp(key, known[k])) {
                known_key = true;
                break;
            }
        }
        if (known_key) {
            continue;
        }

        const char *similar = bake_key_find_similar(key, known);
        if (similar) {
            ecs_warn("unknown %s key '%s', did you mean '%s'?",
                context, key, similar);
        }
    }
}

static int bake_parse_lang_cfg(const JSON_Object *object, bake_lang_cfg_t *cfg) {
    if (!object) {
        return 0;
    }

    bake_warn_similar_keys(object, bake_lang_object_keys, "language config");

#define G(key, field) \
    if (bake_json_get_array(object, key, &cfg->field) < 0) return -1;
    BAKE_LANG_ARRAY_KEYS(G)
#undef G

    if (bake_json_get_string(object, "c-standard", &cfg->c_standard) < 0) return -1;
    if (bake_json_get_string(object, "cpp-standard", &cfg->cpp_standard) < 0) return -1;
    if (bake_json_get_string(object, "shell", &cfg->shell) < 0) return -1;

    size_t key_count = json_object_get_count(object);
    for (size_t i = 0; i < key_count; i++) {
        const char *key = json_object_get_name(object, i);
        if (bake_json_conditional_key_matches(key)) {
            JSON_Value *val = json_object_get_value_at(object, i);
            const JSON_Object *nested = json_value_get_object(val);
            if (!nested || bake_parse_lang_cfg(nested, cfg) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int bake_parse_amalgamate_item(
    const JSON_Object *item,
    bake_amalgamate_cfg_t *amalg)
{
    bake_warn_similar_keys(item, bake_amalgamate_object_keys, "amalgamate");
    if (bake_json_get_string(item, "path", &amalg->path) < 0) return -1;
    if (bake_json_get_string(item, "prefix", &amalg->prefix) < 0) return -1;
    if (bake_json_get_array(item, "disable-flags", &amalg->disable_flags) < 0) return -1;
    return 0;
}

static int bake_parse_amalgamate(
    const JSON_Object *object,
    bake_project_cfg_t *cfg)
{
    JSON_Value *value = json_object_get_value(object, "amalgamate");
    if (!value) {
        return 0;
    }

    JSON_Value_Type type = json_value_get_type(value);

    if (type == JSONBoolean) {
        if (json_value_get_boolean(value)) {
            bake_amalgamate_cfg_t *amalg = bake_amalgamate_list_append(&cfg->amalgamate);
            if (bake_json_get_string(object, "amalgamate-path", &amalg->path) < 0) return -1;
        }
        return 0;
    }

    if (type == JSONObject) {
        bake_amalgamate_cfg_t *amalg = bake_amalgamate_list_append(&cfg->amalgamate);
        return bake_parse_amalgamate_item(json_value_get_object(value), amalg);
    }

    if (type == JSONArray) {
        JSON_Array *array = json_value_get_array(value);
        size_t count = json_array_get_count(array);
        for (size_t i = 0; i < count; i++) {
            const JSON_Object *item = json_array_get_object(array, i);
            if (!item) {
                ecs_err("each 'amalgamate' entry must be an object");
                return -1;
            }
            bake_amalgamate_cfg_t *amalg = bake_amalgamate_list_append(&cfg->amalgamate);
            if (bake_parse_amalgamate_item(item, amalg) != 0) return -1;
        }
        return 0;
    }

    ecs_err("'amalgamate' must be a boolean, object or array");
    return -1;
}

const char* bake_lint_action_str(bake_lint_action_t action) {
    switch (action) {
    case BAKE_LINT_LOG: return "log";
    case BAKE_LINT_AUTOFIX: return "autofix";
    case BAKE_LINT_ERROR: break;
    }

    return "error";
}

static int bake_parse_lint(
    const JSON_Object *object,
    bake_lint_cfg_t *lint)
{
    JSON_Value *value = json_object_get_value(object, "lint");
    if (!value) {
        return 0;
    }

    const JSON_Object *lint_obj = json_value_get_object(value);
    if (!lint_obj) {
        ecs_err("'lint' must be an object");
        return -1;
    }

    bake_warn_similar_keys(lint_obj, bake_lint_object_keys, "lint");

    ecs_os_free(lint->command);
    lint->command = NULL;
    if (bake_json_get_string(lint_obj, "command", &lint->command) < 0) return -1;
    if (!lint->command || !lint->command[0]) {
        ecs_err("'lint' requires a non-empty 'command'");
        return -1;
    }

    char *action = NULL;
    if (bake_json_get_string(lint_obj, "action", &action) < 0) return -1;
    lint->action = BAKE_LINT_ERROR;
    if (action) {
        if (!strcmp(action, "log")) {
            lint->action = BAKE_LINT_LOG;
        } else if (!strcmp(action, "autofix")) {
            lint->action = BAKE_LINT_AUTOFIX;
        } else if (strcmp(action, "error")) {
            ecs_err("invalid lint action '%s', expected log, error or autofix", action);
            ecs_os_free(action);
            return -1;
        }
        ecs_os_free(action);
    }

    return 0;
}

static int bake_parse_project_value_cfg(
    const JSON_Object *object,
    bake_project_cfg_t *cfg)
{
    if (!object) {
        return 0;
    }

    if (bake_json_get_string(object, "language", &cfg->language) < 0) return -1;
    if (bake_json_get_string(object, "output", &cfg->output_name) < 0) return -1;

    if (bake_json_get_bool(object, "private", &cfg->private_project) < 0) return -1;
    if (bake_json_get_bool(object, "public", &cfg->public_project) < 0) return -1;
    if (bake_json_get_bool(object, "standalone", &cfg->standalone) < 0) return -1;
    if (bake_parse_amalgamate(object, cfg) < 0) return -1;

    if (bake_json_get_array(object, "use", &cfg->use) < 0) return -1;
    if (bake_json_get_array(object, "use-private", &cfg->use_private) < 0) return -1;

#define ARR(key, field) \
    if (bake_json_get_array(object, key, &cfg->c_lang.field) < 0) return -1; \
    if (bake_json_get_array(object, key, &cfg->cpp_lang.field) < 0) return -1;
    BAKE_LANG_ARRAY_KEYS(ARR)
#undef ARR

    if (bake_json_get_string(object, "c-standard", &cfg->c_lang.c_standard) < 0) return -1;
    if (bake_json_get_string(object, "cpp-standard", &cfg->cpp_lang.cpp_standard) < 0) return -1;

    return 0;
}

static int bake_parse_project_lang_cfg(
    const JSON_Object *object,
    bake_project_cfg_t *cfg)
{
    const JSON_Object *lang_c = NULL;
    if (bake_json_get_object_optional(object, "lang.c", &lang_c) != 0) return -1;
    if (bake_parse_lang_cfg(lang_c, &cfg->c_lang) != 0) return -1;

    const JSON_Object *lang_cpp = NULL;
    if (bake_json_get_object_optional(object, "lang.cpp", &lang_cpp) != 0) return -1;
    if (bake_parse_lang_cfg(lang_cpp, &cfg->cpp_lang) != 0) return -1;

    const JSON_Object *lang = NULL;
    if (bake_json_get_object_optional(object, "lang", &lang) != 0) return -1;
    if (lang) {
        const JSON_Object *lang_c_obj = NULL;
        if (bake_json_get_object_optional(lang, "c", &lang_c_obj) != 0) return -1;
        if (bake_parse_lang_cfg(lang_c_obj, &cfg->c_lang) != 0) return -1;

        const JSON_Object *lang_cpp_obj = NULL;
        if (bake_json_get_object_optional(lang, "cpp", &lang_cpp_obj) != 0) return -1;
        if (bake_parse_lang_cfg(lang_cpp_obj, &cfg->cpp_lang) != 0) return -1;
    }

    return 0;
}

static int bake_parse_dependee_cfg(
    const JSON_Object *object,
    bake_dependee_cfg_t *cfg);

static int bake_parse_project_cfg_conditionals(
    const JSON_Object *object,
    bake_project_cfg_t *cfg,
    bool parse_dependee)
{
    if (!object) {
        return 0;
    }

    size_t key_count = json_object_get_count(object);
    for (size_t i = 0; i < key_count; i++) {
        const char *key = json_object_get_name(object, i);
        if (bake_json_conditional_key_matches(key)) {
            JSON_Value *val = json_object_get_value_at(object, i);
            const JSON_Object *nested = json_value_get_object(val);
            if (!nested) {
                return -1;
            }
            if (bake_parse_project_cfg_object(nested, cfg, false, parse_dependee) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int bake_parse_project_cfg_object(
    const JSON_Object *object,
    bake_project_cfg_t *cfg,
    bool parse_meta,
    bool parse_dependee)
{
    if (!object) {
        return 0;
    }

    bake_warn_similar_keys(object, bake_project_object_keys, "project");

    {
        const JSON_Object *bundles_obj = NULL;
        if (bake_json_get_object_optional(object, "bundle", &bundles_obj) != 0) return -1;
        if (bake_parse_bundles_object(bundles_obj, &cfg->bundles) != 0) return -1;

    }

    if (parse_meta) {
        if (bake_json_get_string(object, "id", &cfg->id) < 0) return -1;

        char *type = NULL;
        if (bake_json_get_string(object, "type", &type) < 0) return -1;
        if (type) {
            cfg->kind = bake_project_kind_parse(type);
            ecs_os_free(type);
        }

        JSON_Value *test_value = json_object_get_value(object, "test");
        if (test_value) {
            if (json_value_get_type(test_value) != JSONObject) {
                return -1;
            }
            cfg->has_test_spec = true;
            if (cfg->kind == BAKE_PROJECT_APPLICATION) {
                cfg->kind = BAKE_PROJECT_TEST;
            }
        }

        JSON_Value *bench_value = json_object_get_value(object, "bench");
        if (bench_value) {
            if (json_value_get_type(bench_value) != JSONObject) {
                return -1;
            }
            if (cfg->has_test_spec) {
                ecs_err("project declares both a 'test' and a 'bench' section, "
                    "use separate projects for tests and benchmarks");
                return -1;
            }
            cfg->has_bench_spec = true;
            if (cfg->kind == BAKE_PROJECT_APPLICATION) {
                cfg->kind = BAKE_PROJECT_BENCH;
            }
        }

        JSON_Value *rules_value = json_object_get_value(object, "rules");
        if (rules_value) {
            if (json_value_get_type(rules_value) != JSONArray) {
                return -1;
            }
            if (bake_parse_rules(json_value_get_array(rules_value), &cfg->rules) != 0) return -1;
        }

        if (bake_parse_lint(object, &cfg->lint) != 0) return -1;
    }

    if (bake_parse_project_value_cfg(object, cfg) != 0) return -1;

    const JSON_Object *value_obj = NULL;
    if (bake_json_get_object_optional(object, "value", &value_obj) != 0) return -1;
    bake_warn_similar_keys(value_obj, bake_project_object_keys, "project");
    if (bake_parse_project_value_cfg(value_obj, cfg) != 0) return -1;

    if (bake_parse_project_lang_cfg(object, cfg) != 0) return -1;

    if (parse_dependee) {
        const JSON_Object *value_dependee_obj = NULL;
        if (value_obj && bake_json_get_object_optional(value_obj, "dependee", &value_dependee_obj) != 0) {
            return -1;
        }
        if (bake_parse_dependee_cfg(value_dependee_obj, &cfg->dependee) != 0) {
            return -1;
        }

        const JSON_Object *root_dependee_obj = NULL;
        if (bake_json_get_object_optional(object, "dependee", &root_dependee_obj) != 0) {
            return -1;
        }
        if (bake_parse_dependee_cfg(root_dependee_obj, &cfg->dependee) != 0) {
            return -1;
        }
    }

    if (bake_parse_project_cfg_conditionals(object, cfg, parse_dependee) != 0) {
        return -1;
    }

    if (bake_parse_project_cfg_conditionals(value_obj, cfg, parse_dependee) != 0) {
        return -1;
    }

    return 0;
}

static int bake_json_object_merge(JSON_Object *dst, const JSON_Object *src) {
    size_t count = json_object_get_count(src);
    for (size_t i = 0; i < count; i++) {
        const char *key = json_object_get_name(src, i);
        JSON_Value *src_val = json_object_get_value_at(src, i);
        JSON_Value *dst_val = json_object_get_value(dst, key);

        if (dst_val && json_value_get_type(dst_val) == JSONObject &&
            json_value_get_type(src_val) == JSONObject)
        {
            if (bake_json_object_merge(
                json_value_get_object(dst_val),
                json_value_get_object(src_val)) != 0)
            {
                return -1;
            }
            continue;
        }

        if (dst_val && json_value_get_type(dst_val) == JSONArray &&
            json_value_get_type(src_val) == JSONArray)
        {
            JSON_Array *dst_arr = json_value_get_array(dst_val);
            JSON_Array *src_arr = json_value_get_array(src_val);
            size_t n = json_array_get_count(src_arr);
            for (size_t j = 0; j < n; j++) {
                JSON_Value *item = json_value_deep_copy(json_array_get_value(src_arr, j));
                if (!item || json_array_append_value(dst_arr, item) != JSONSuccess) {
                    json_value_free(item);
                    return -1;
                }
            }
            continue;
        }

        JSON_Value *copy = json_value_deep_copy(src_val);
        if (!copy || json_object_set_value(dst, key, copy) != JSONSuccess) {
            json_value_free(copy);
            return -1;
        }
    }

    return 0;
}

static int bake_parse_dependee_cfg(
    const JSON_Object *object,
    bake_dependee_cfg_t *cfg)
{
    if (!object) {
        return 0;
    }

    if (!cfg->cfg) {
        cfg->cfg = ecs_os_calloc_t(bake_project_cfg_t);
        bake_project_cfg_init_impl(cfg->cfg, false, false);
    }

    if (bake_parse_project_cfg_object(object, cfg->cfg, false, false) != 0) {
        return -1;
    }

    /* Merge into the previously captured fragment so dependee sections from
     * the root, value and conditional objects all end up in dependee.json. */
    JSON_Value *merged = NULL;
    if (cfg->json && cfg->json[0]) {
        merged = json_parse_string(cfg->json);
    }
    if (!merged) {
        merged = json_value_init_object();
    }
    if (!merged) {
        return -1;
    }

    if (bake_json_object_merge(json_value_get_object(merged), object) != 0) {
        json_value_free(merged);
        return -1;
    }

    char *serialized = json_serialize_to_string(merged);
    json_value_free(merged);
    if (!serialized) {
        return -1;
    }

    char *dependee_json = ecs_os_strdup(serialized);
    json_free_serialized_string(serialized);

    ecs_os_free(cfg->json);
    cfg->json = dependee_json;
    return 0;
}

static int bake_parse_bundle_entry(
    const char *id,
    const JSON_Object *object,
    bake_bundle_list_t *bundles)
{
    if (!id || !id[0] || !object) {
        return -1;
    }

    if (bake_bundle_list_find(bundles, id)) {
        ecs_warn("ignoring duplicate bundle entry '%s'", id);
        return 0;
    }

    bake_warn_similar_keys(object, bake_bundle_object_keys, "bundle");

    bake_bundle_t *bundle = bake_bundle_list_append(bundles);
    bundle->id = ecs_os_strdup(id);

    if (bake_json_get_string(object, "repository", &bundle->repository) < 0) return -1;
    if (bake_json_get_string(object, "branch", &bundle->branch) < 0) return -1;
    if (bake_json_get_string(object, "tag", &bundle->tag) < 0) return -1;
    if (bake_json_get_string(object, "commit", &bundle->commit) < 0) return -1;
    if (bake_json_get_string(object, "subdir", &bundle->subdir) < 0) return -1;
    if (bake_json_get_string(object, "library", &bundle->library) < 0) return -1;
    if (bake_json_get_string(object, "build-system", &bundle->build_system) < 0) return -1;
    if (bake_json_get_string(object, "profile", &bundle->profile) < 0) return -1;

    if (bake_json_get_bool(object, "header-only", &bundle->header_only) < 0) return -1;
    if (bake_json_get_array(object, "include", &bundle->includes) < 0) return -1;
    if (bake_json_get_array(object, "sources", &bundle->sources) < 0) return -1;
    if (bake_json_get_array(object, "cmake-args", &bundle->cmake_args) < 0) return -1;
    if (bake_json_get_array(object, "cargo-args", &bundle->cargo_args) < 0) return -1;
    if (bake_json_get_array(object, "lib", &bundle->libs) < 0) return -1;
    if (bake_json_get_array(object, "ldflags", &bundle->ldflags) < 0) return -1;

    return 0;
}

static int bake_parse_bundles_object(
    const JSON_Object *bundles_obj,
    bake_bundle_list_t *bundles)
{
    if (!bundles_obj) {
        return 0;
    }

    size_t key_count = json_object_get_count(bundles_obj);
    for (size_t i = 0; i < key_count; i++) {
        const char *key = json_object_get_name(bundles_obj, i);
        JSON_Value *val = json_object_get_value_at(bundles_obj, i);
        if (!val || json_value_get_type(val) != JSONObject) {
            return -1;
        }
        if (bake_parse_bundle_entry(key, json_value_get_object(val), bundles) != 0) {
            return -1;
        }
    }

    return 0;
}

static int bake_parse_rules(const JSON_Array *rules, bake_rule_list_t *rules_out) {
    if (!rules) {
        return 0;
    }

    size_t count = json_array_get_count(rules);
    for (size_t i = 0; i < count; i++) {
        JSON_Value *rule_value = json_array_get_value(rules, i);
        const JSON_Object *rule = json_value_get_object(rule_value);
        if (!rule) {
            return -1;
        }

        const char *ext = json_object_get_string(rule, "ext");
        const char *cmd = json_object_get_string(rule, "command");

        if (!ext || !cmd) {
            ecs_warn("ignoring rule without '%s' attribute",
                ext ? "command" : "ext");
            continue;
        }

        bake_rule_list_append(rules_out, ext, cmd);
    }

    return 0;
}

static void bake_lang_resolve_include_paths(
    bake_lang_cfg_t *lang,
    const char *base)
{
    for (int32_t i = 0; i < lang->include_paths.count; i++) {
        char *path = lang->include_paths.items[i];
        if (!path || !path[0] || bake_path_is_abs(path)) {
            continue;
        }
        lang->include_paths.items[i] = bake_path_join(base, path);
        ecs_os_free(path);
    }
}

static void bake_project_cfg_finalize_defaults(const char *project_json_path, bake_project_cfg_t *cfg) {
    if (bake_project_kind_is_harness(cfg->kind)) {
        cfg->public_project = false;
    }

    if (!cfg->path) {
        cfg->path = bake_path_dirname(project_json_path);
    }

    bake_lang_resolve_include_paths(&cfg->c_lang, cfg->path);
    bake_lang_resolve_include_paths(&cfg->cpp_lang, cfg->path);

    if (!cfg->id) {
        cfg->id = ecs_os_strdup(cfg->path);

        for (char *p = cfg->id; *p; p++) {
            if (*p == '/' || *p == '\\' || *p == ':') {
                *p = '.';
            }
        }

        const char *trimmed = cfg->id;
        while (trimmed[0] == '.') {
            trimmed++;
        }
        if (trimmed != cfg->id) {
            memmove(cfg->id, trimmed, strlen(trimmed) + 1);
        }
    }

    if (!cfg->output_name) {
        const char *last_dot = strrchr(cfg->id, '.');
        cfg->output_name = ecs_os_strdup(
            (last_dot && last_dot[1]) ? last_dot + 1 : cfg->id);
    }
}

int bake_project_cfg_load_file(const char *project_json_path, bake_project_cfg_t *cfg) {
    size_t len = 0;
    char *json = bake_file_read(project_json_path, &len);
    if (!json) {
        return -1;
    }

    JSON_Value *root_value = json_parse_string_with_comments(json);
    const JSON_Object *root = root_value ? json_value_get_object(root_value) : NULL;
    if (!root) {
        json_value_free(root_value);
        ecs_os_free(json);
        return -1;
    }

    if (bake_parse_project_cfg_object(root, cfg, true, true) != 0) {
        goto error;
    }

    bake_project_cfg_finalize_defaults(project_json_path, cfg);

    json_value_free(root_value);
    ecs_os_free(json);
    return 0;

error:
    json_value_free(root_value);
    ecs_os_free(json);
    return -1;
}
