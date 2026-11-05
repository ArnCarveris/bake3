#include "bake/environment.h"
#include "bake/os.h"
#include "env_internal.h"

#include "parson.h"

#define BAKE_SETTINGS_FILE "bake.json"
#define BAKE_SETTINGS_ID_MARK "\x1f"

static char *bake_settings_output_dir_value = NULL;
static char *bake_settings_base_dir = NULL;
static bool bake_settings_colors_value = true;

static const char *bake_settings_test_log_kinds[] = { "pass", "skip", "fail" };
static const char *bake_settings_test_log_envs[] = {
    "BAKE_TEST_LOG_PASS", "BAKE_TEST_LOG_SKIP", "BAKE_TEST_LOG_FAIL"
};

typedef struct bake_settings_expr_ctx_t {
    const char *id;
    bool used_id;
} bake_settings_expr_ctx_t;

static char* bake_settings_id_underscore(const char *id) {
    char *out = ecs_os_strdup(id);
    for (char *ch = out; *ch; ch++) {
        if (*ch == '.' || bake_path_is_sep(*ch)) {
            *ch = '_';
        }
    }
    return out;
}

static char* bake_settings_resolve_expr(
    const char *kind,
    const char *arg,
    void *ptr)
{
    bake_settings_expr_ctx_t *ctx = ptr;

    if (!strcmp(kind, "id")) {
        ctx->used_id = true;
        if (!arg) {
            return ecs_os_strdup(ctx->id);
        }
        if (!strcmp(arg, "underscore")) {
            return bake_settings_id_underscore(ctx->id);
        }
        ecs_err("unknown modifier '%s' in '${id %s}' (supported: "
            "'${id}', '${id underscore}')", arg, arg);
        return NULL;
    }

    if (!strcmp(kind, "env")) {
        if (!arg) {
            ecs_err("missing variable name in '${env}', use '${env <name>}'");
            return NULL;
        }
        const char *value = getenv(arg);
        if (!value || !value[0]) {
            ecs_err("environment variable '%s' in '${env %s}' is not set",
                arg, arg);
            return NULL;
        }
        return ecs_os_strdup(value);
    }

    ecs_err("unknown expression '${%s%s%s}' (supported: '${id}', "
        "'${id underscore}', '${env <name>}')",
        kind, arg ? " " : "", arg ? arg : "");
    return NULL;
}

char* bake_settings_expand_output_dir(
    const char *output_dir,
    const char *base_dir,
    const char *id,
    bool *used_id_out)
{
    if (used_id_out) {
        *used_id_out = false;
    }

    if (!output_dir || !output_dir[0] || !id || !id[0]) {
        return NULL;
    }

    char *template = NULL;
    if (output_dir[0] == '~' &&
        (!output_dir[1] || bake_path_is_sep(output_dir[1])))
    {
        char *home = bake_os_home_path();
        if (!home) {
            ecs_err("cannot expand '~' in '%s': HOME is not set", output_dir);
            return NULL;
        }
        template = output_dir[1]
            ? bake_path_join(home, output_dir + 2)
            : ecs_os_strdup(home);
        ecs_os_free(home);
    } else {
        template = ecs_os_strdup(output_dir);
    }

    bake_settings_expr_ctx_t ctx = { .id = id };
    char *expanded = bake_expr_expand(template, bake_settings_resolve_expr, &ctx);
    ecs_os_free(template);
    if (!expanded) {
        return NULL;
    }

    if (!expanded[0]) {
        ecs_os_free(expanded);
        return NULL;
    }

    if (!bake_path_is_abs(expanded) && base_dir && base_dir[0]) {
        char *joined = bake_path_join(base_dir, expanded);
        ecs_os_free(expanded);
        expanded = joined;
    }

    if (used_id_out) {
        *used_id_out = ctx.used_id;
    }

    return expanded;
}

int bake_settings_set_output_dir(const char *output_dir, const char *base_dir) {
    ecs_os_free(bake_settings_output_dir_value);
    ecs_os_free(bake_settings_base_dir);
    bake_settings_output_dir_value = NULL;
    bake_settings_base_dir = NULL;

    if (!output_dir) {
        return 0;
    }

    bool used_id = false;
    char *probe = bake_settings_expand_output_dir(
        output_dir, base_dir, BAKE_SETTINGS_ID_MARK, &used_id);
    if (!probe) {
        ecs_err("invalid output_dir '%s'", output_dir);
        return -1;
    }
    ecs_os_free(probe);

    if (!used_id) {
        ecs_err("output_dir '%s' must contain '${id}' or '${id underscore}' "
            "so that each project gets its own directory", output_dir);
        return -1;
    }

    bake_settings_output_dir_value = ecs_os_strdup(output_dir);
    bake_settings_base_dir = base_dir ? ecs_os_strdup(base_dir) : NULL;
    return 0;
}

const char* bake_settings_output_dir(void) {
    return bake_settings_output_dir_value;
}

void bake_settings_set_colors(bool colors) {
    bake_settings_colors_value = colors;
    ecs_log_enable_colors(colors);
    if (!colors) {
        bake_os_setenv("NO_COLOR", "1");
    }
}

bool bake_settings_colors(void) {
    return bake_settings_colors_value;
}

static int bake_settings_parse_test(
    JSON_Value *value,
    const char *path,
    int *log_out)
{
    const JSON_Object *test = json_value_get_object(value);
    if (!test) {
        ecs_err("test in %s must be an object", path);
        return -1;
    }

    size_t count = json_object_get_count(test);
    for (size_t i = 0; i < count; i++) {
        const char *key = json_object_get_name(test, i);
        JSON_Value *member = json_object_get_value_at(test, i);
        if (strcmp(key, "log")) {
            ecs_warn("unknown setting 'test.%s' in %s", key, path);
            continue;
        }

        const JSON_Object *log = json_value_get_object(member);
        if (!log) {
            ecs_err("test.log in %s must be an object", path);
            return -1;
        }

        size_t log_count = json_object_get_count(log);
        for (size_t l = 0; l < log_count; l++) {
            const char *kind = json_object_get_name(log, l);
            JSON_Value *flag = json_object_get_value_at(log, l);
            int index = -1;
            for (int k = 0; k < 3; k++) {
                if (!strcmp(kind, bake_settings_test_log_kinds[k])) {
                    index = k;
                }
            }
            if (index < 0) {
                ecs_warn("unknown setting 'test.log.%s' in %s", kind, path);
                continue;
            }
            if (json_value_get_type(flag) != JSONBoolean) {
                ecs_err("test.log.%s in %s must be a boolean", kind, path);
                return -1;
            }
            log_out[index] = json_value_get_boolean(flag) ? 1 : 0;
        }
    }

    return 0;
}

char* bake_settings_home(void) {
    const char *local_env = getenv("BAKE_LOCAL_ENV");
    const char *env_home = (local_env && !strcmp(local_env, "1"))
        ? getenv("BAKE_GLOBAL_HOME")
        : getenv("BAKE_HOME");
    if (env_home && env_home[0]) {
        return bake_env_resolve_home_path(env_home);
    }

    char *home = bake_os_home_path();
    if (!home) {
        return NULL;
    }
    char *result = bake_path_join(home, "bake3");
    ecs_os_free(home);
    return result;
}

int bake_settings_load(void) {
    bake_settings_fini();

    char *home = bake_settings_home();
    if (!home) {
        return 0;
    }

    int rc = -1;
    char *path = bake_path_join(home, BAKE_SETTINGS_FILE);
    JSON_Value *root = NULL;

    if (!bake_path_exists(path)) {
        rc = 0;
        goto cleanup;
    }

    root = json_parse_file_with_comments(path);
    const JSON_Object *object = json_value_get_object(root);
    if (!object) {
        ecs_err("failed to parse %s (expected a JSON object)", path);
        goto cleanup;
    }

    const char *output_dir = NULL;
    bool colors = true;
    int test_log[3] = { -1, -1, -1 };
    size_t count = json_object_get_count(object);
    for (size_t i = 0; i < count; i++) {
        const char *key = json_object_get_name(object, i);
        JSON_Value *value = json_object_get_value_at(object, i);
        if (!strcmp(key, "output_dir")) {
            output_dir = json_value_get_string(value);
            if (!output_dir || !output_dir[0]) {
                ecs_err("output_dir in %s must be a non-empty string", path);
                goto cleanup;
            }
        } else if (!strcmp(key, "colors")) {
            if (json_value_get_type(value) != JSONBoolean) {
                ecs_err("colors in %s must be a boolean", path);
                goto cleanup;
            }
            colors = json_value_get_boolean(value);
        } else if (!strcmp(key, "test")) {
            if (bake_settings_parse_test(value, path, test_log) != 0) {
                goto cleanup;
            }
        } else {
            ecs_warn("unknown setting '%s' in %s", key, path);
        }
    }

    if (bake_settings_set_output_dir(output_dir, home) != 0) {
        ecs_err("invalid settings in %s", path);
        goto cleanup;
    }

    if (!colors) {
        bake_settings_set_colors(false);
    }

    for (int k = 0; k < 3; k++) {
        if (test_log[k] >= 0) {
            bake_os_setenv(bake_settings_test_log_envs[k], test_log[k] ? "1" : "0");
        }
    }

    rc = 0;

cleanup:
    if (root) {
        json_value_free(root);
    }
    ecs_os_free(path);
    ecs_os_free(home);
    return rc;
}

void bake_settings_fini(void) {
    bake_settings_set_output_dir(NULL, NULL);
    bake_settings_colors_value = true;
    ecs_log_enable_colors(true);
}

static char* bake_settings_dir_name(const char *path) {
    char *name = bake_path_basename(path);
    if (name && name[0]) {
        return name;
    }
    ecs_os_free(name);
    return ecs_os_strdup("root");
}

char* bake_project_output_dir(const char *project_path, const char *project_id) {
    if (!project_path || !project_path[0]) {
        return NULL;
    }

    if (!bake_settings_output_dir_value) {
        return bake_path_join(project_path, ".bake");
    }

    char *id = (project_id && project_id[0])
        ? ecs_os_strdup(project_id)
        : bake_settings_dir_name(project_path);
    char *result = bake_settings_expand_output_dir(
        bake_settings_output_dir_value, bake_settings_base_dir, id, NULL);
    ecs_os_free(id);
    return result;
}

static char* bake_settings_workspace_id(const char *cwd) {
    char *project_json = bake_path_join(cwd, "project.json");
    char *id = NULL;
    if (bake_path_exists(project_json)) {
        JSON_Value *root = json_parse_file_with_comments(project_json);
        const char *value = json_object_get_string(
            json_value_get_object(root), "id");
        if (value && value[0]) {
            id = ecs_os_strdup(value);
        }
        if (root) {
            json_value_free(root);
        }
    }
    ecs_os_free(project_json);
    return id;
}

char* bake_workspace_output_dir(const char *cwd) {
    if (!cwd || !cwd[0]) {
        return NULL;
    }

    if (!bake_settings_output_dir_value) {
        return bake_path_join(cwd, ".bake");
    }

    char *id = bake_settings_workspace_id(cwd);
    char *result = bake_project_output_dir(cwd, id);
    ecs_os_free(id);
    return result;
}

static int32_t bake_settings_split(const char *path, char ***parts_out) {
    ecs_vec_t vec = {0};
    ecs_vec_init_t(NULL, &vec, char*, 0);

    const char *start = path;
    for (const char *ptr = path; ; ptr++) {
        if (*ptr && !bake_path_is_sep(*ptr)) {
            continue;
        }
        size_t len = (size_t)(ptr - start);
        if (len && !(len == 1 && start[0] == '.')) {
            char *part = ecs_os_malloc((ecs_size_t)len + 1);
            memcpy(part, start, len);
            part[len] = '\0';
            ecs_vec_append_t(NULL, &vec, char*)[0] = part;
        }
        if (!*ptr) {
            break;
        }
        start = ptr + 1;
    }

    int32_t count = ecs_vec_count(&vec);
    *parts_out = count ? ecs_os_malloc_n(char*, count) : NULL;
    if (count) {
        ecs_os_memcpy_n(*parts_out, ecs_vec_first_t(&vec, char*), char*, count);
    }
    ecs_vec_fini_t(NULL, &vec, char*);
    return count;
}

static void bake_settings_parts_free(char **parts, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        ecs_os_free(parts[i]);
    }
    ecs_os_free(parts);
}

static bool bake_settings_segment_match(const char *pattern, const char *value) {
    if (!pattern[0]) {
        return !value[0];
    }

    if (pattern[0] == BAKE_SETTINGS_ID_MARK[0]) {
        if (!value[0]) {
            return false;
        }
        for (const char *ptr = value + 1; ; ptr++) {
            if (bake_settings_segment_match(pattern + 1, ptr)) {
                return true;
            }
            if (!ptr[0]) {
                return false;
            }
        }
    }

    return pattern[0] == value[0] &&
        bake_settings_segment_match(pattern + 1, value + 1);
}

bool bake_settings_is_output_dir(const char *path) {
    if (!path || !path[0] || !bake_settings_output_dir_value) {
        return false;
    }

    char *pattern = bake_settings_expand_output_dir(
        bake_settings_output_dir_value, bake_settings_base_dir,
        BAKE_SETTINGS_ID_MARK, NULL);
    if (!pattern) {
        return false;
    }

    char **pattern_parts = NULL;
    char **path_parts = NULL;
    int32_t pattern_count = bake_settings_split(pattern, &pattern_parts);
    int32_t path_count = bake_settings_split(path, &path_parts);
    ecs_os_free(pattern);

    bool match = pattern_count == path_count &&
        bake_path_is_abs(path);
    for (int32_t i = 0; match && i < pattern_count; i++) {
        match = bake_settings_segment_match(pattern_parts[i], path_parts[i]);
    }

    bake_settings_parts_free(pattern_parts, pattern_count);
    bake_settings_parts_free(path_parts, path_count);
    return match;
}
