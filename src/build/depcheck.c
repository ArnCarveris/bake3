#include "build_internal.h"
#include "depcheck_internal.h"
#include "bake/os.h"
#include "parson.h"

static bool bake_dep_token_outdated(const char *token, int64_t obj_mtime) {
    if (!token[0]) {
        return false;
    }

    int64_t mtime = bake_os_file_mtime(token);
    if (mtime < 0) {
        return true;
    }

    return mtime > obj_mtime;
}

static char* bake_dep_token_reserve(char *token, size_t token_len, size_t *token_cap) {
    if ((token_len + 2) < *token_cap) {
        return token;
    }
    size_t next_cap = *token_cap * 2;
    if (next_cap <= *token_cap) {
        ecs_os_free(token);
        return NULL;
    }
    *token_cap = next_cap;
    return ecs_os_realloc_n(token, char, next_cap);
}

/* MSVC /sourceDependencies output: {"Data": {"Includes": ["c:\...h", ...]}}.
 * Includes lists every header the unit pulled in, directly or indirectly. */
static bool bake_source_dependencies_outdated(const char *content, int64_t obj_mtime) {
    JSON_Value *root = json_parse_string(content);
    if (!root) {
        return true;
    }

    JSON_Array *includes = json_object_dotget_array(
        json_value_get_object(root), "Data.Includes");
    if (!includes) {
        json_value_free(root);
        return true;
    }

    bool outdated = false;
    size_t count = json_array_get_count(includes);
    for (size_t i = 0; i < count && !outdated; i++) {
        const char *path = json_array_get_string(includes, i);
        if (!path || bake_dep_token_outdated(path, obj_mtime)) {
            outdated = true;
        }
    }

    json_value_free(root);
    return outdated;
}

bool bake_depfile_outdated(const char *dep_path, int64_t obj_mtime) {
    size_t len = 0;
    char *content = bake_file_read(dep_path, &len);
    if (!content) {
        return true;
    }

    size_t first = 0;
    while (first < len && bake_char_is_space(content[first])) {
        first++;
    }
    if (first < len && content[first] == '{') {
        bool outdated = bake_source_dependencies_outdated(content, obj_mtime);
        ecs_os_free(content);
        return outdated;
    }

    bool seen_colon = false;
    bool outdated = false;
    size_t token_cap = 256;
    size_t token_len = 0;
    char *token = ecs_os_malloc(token_cap);

    for (size_t i = 0; i < len; i++) {
        char ch = content[i];

        if (!seen_colon) {
            /* Only a colon followed by whitespace ends the target; a bare
             * colon can be part of a Windows drive-letter path. */
            if (ch == ':' &&
                ((i + 1) >= len || bake_char_is_space(content[i + 1])))
            {
                seen_colon = true;
            }
            continue;
        }

        if (ch == '\\' && (i + 1) < len && content[i + 1] == '\n') {
            i++;
            continue;
        }

        if (ch == '\\' && (i + 2) < len && content[i + 1] == '\r' && content[i + 2] == '\n') {
            i += 2;
            continue;
        }

        if (ch == '\\' && (i + 1) < len) {
            char next_ch = content[i + 1];
            bool is_escape = next_ch == ' ' || next_ch == '\t' ||
                next_ch == '\\' || next_ch == '#' || next_ch == '$' ||
                next_ch == ':';
            token = bake_dep_token_reserve(token, token_len, &token_cap);
            if (!token) {
                ecs_os_free(content);
                return true;
            }
            if (is_escape) {
                token[token_len++] = content[++i];
            } else {
                token[token_len++] = ch;
            }
            continue;
        }

        if (bake_char_is_space(ch)) {
            token[token_len] = '\0';
            if (token_len && bake_dep_token_outdated(token, obj_mtime)) {
                outdated = true;
            }
            token_len = 0;
            if (outdated) {
                break;
            }
            continue;
        }

        token = bake_dep_token_reserve(token, token_len, &token_cap);
        if (!token) {
            ecs_os_free(content);
            return true;
        }
        token[token_len++] = ch;
    }

    if (!outdated) {
        token[token_len] = '\0';
        if (token_len && bake_dep_token_outdated(token, obj_mtime)) {
            outdated = true;
        }
    }

    ecs_os_free(token);
    ecs_os_free(content);
    return outdated;
}

int64_t bake_project_json_mtime(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->path || !cfg->path[0]) {
        return -1;
    }

    char *project_json = bake_path_join(cfg->path, "project.json");
    int64_t mtime = bake_os_file_mtime(project_json);
    ecs_os_free(project_json);
    return mtime;
}

bool bake_compile_unit_outdated(
    const bake_compile_unit_t *unit,
    int64_t project_json_mtime)
{
    if (!unit || !unit->src || !unit->obj) {
        return true;
    }

    int64_t obj_mtime = bake_os_file_mtime(unit->obj);
    int64_t src_mtime = bake_os_file_mtime(unit->src);
    if (obj_mtime < 0 || src_mtime < 0) {
        return true;
    }

    if (src_mtime > obj_mtime) {
        return true;
    }

    if (project_json_mtime < 0 || project_json_mtime > obj_mtime) {
        return true;
    }

    if (unit->dep) {
        int64_t dep_mtime = bake_os_file_mtime(unit->dep);
        if (dep_mtime < 0 || dep_mtime < src_mtime) {
            return true;
        }
        if (bake_depfile_outdated(unit->dep, obj_mtime)) {
            return true;
        }
    }

    return false;
}

bool bake_project_json_outdated(
    const bake_project_cfg_t *cfg,
    int64_t artefact_mtime)
{
    int64_t pj_mtime = bake_project_json_mtime(cfg);
    if (pj_mtime < 0) {
        return true;
    }

    return pj_mtime > artefact_mtime;
}

char* bake_library_name_from_artefact(const char *artefact) {
    char *name = bake_path_basename(artefact);
    if (!name) {
        return NULL;
    }

#if defined(_WIN32)
    const char *suffix = ".lib";
    size_t len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (len > suffix_len && !strcmp(name + len - suffix_len, suffix)) {
        name[len - suffix_len] = '\0';
    }
#else
    const char *suffix = ".a";
    size_t len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (len > suffix_len && !strcmp(name + len - suffix_len, suffix)) {
        name[len - suffix_len] = '\0';
    }
    if (!strncmp(name, "lib", 3)) {
        memmove(name, name + 3, strlen(name + 3) + 1);
    }
#endif

    return name;
}

bool bake_has_dep_artefact_for_lib(
    const bake_strlist_t *artefacts,
    const char *lib)
{
    if (!artefacts || !lib || !lib[0]) {
        return false;
    }

    for (int32_t i = 0; i < artefacts->count; i++) {
        char *name = bake_library_name_from_artefact(artefacts->items[i]);
        bool match = !strcmp(name, lib);
        ecs_os_free(name);
        if (match) {
            return true;
        }
    }

    return false;
}
