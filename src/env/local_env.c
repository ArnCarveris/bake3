#include "bake/environment.h"
#include "bake/os.h"
#include "env_internal.h"

static const char *bake_local_env_reserved[] = {
    "bin",
    "build",
    "coverage_report",
    "etc",
    "include",
    "lib",
    "meta",
    "src",
    "test",
    NULL
};

static const char *bake_local_env_platform_suffix[] = {
    "-Darwin",
    "-Emscripten",
    "-Linux",
    "-Windows",
    NULL
};

static bool bake_local_env_has_suffix(const char *name, const char *suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > name_len) {
        return false;
    }
    return !strcmp(name + name_len - suffix_len, suffix);
}

bool bake_local_env_name_chars_valid(const char *name) {
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, "..")) {
        return false;
    }

    for (const char *ptr = name; *ptr; ptr ++) {
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

bool bake_local_env_name_reserved(const char *name) {
    if (!name || !name[0]) {
        return false;
    }

    for (int32_t i = 0; bake_local_env_reserved[i]; i ++) {
        if (!strcmp(name, bake_local_env_reserved[i])) {
            return true;
        }
    }

    for (int32_t i = 0; bake_local_env_platform_suffix[i]; i ++) {
        if (bake_local_env_has_suffix(name, bake_local_env_platform_suffix[i])) {
            return true;
        }
    }

    return false;
}

bool bake_local_env_name_valid(const char *name) {
    return bake_local_env_name_chars_valid(name) &&
        !bake_local_env_name_reserved(name);
}

char* bake_local_env_home(const char *cwd, const char *name) {
    if (!cwd || !cwd[0]) {
        return NULL;
    }

    char *root = bake_path_join3(cwd, ".bake", "local_env");
    if (!name) {
        return root;
    }

    if (!bake_local_env_name_valid(name)) {
        ecs_os_free(root);
        return NULL;
    }

    char *home = root ? bake_path_join(root, name) : NULL;
    ecs_os_free(root);
    return home;
}
