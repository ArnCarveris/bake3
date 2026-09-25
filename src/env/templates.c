#include "bake/environment.h"
#include "bake/os.h"
#include "env_internal.h"

static const char *bake_env_required_test_templates[] = {
    "bake_test.h",
    "bake_test.c",
    "bake_test_runtime.h",
    "bake_test_runtime.c",
    "bake_bench.h",
    "bake_bench.c",
    "coverage_report.html"
};

bool bake_env_has_required_test_templates(const char *dir, const char **missing_out) {
    if (missing_out) {
        *missing_out = NULL;
    }

    if (!dir || !bake_path_exists(dir) || !bake_path_is_dir(dir)) {
        if (missing_out) {
            *missing_out = "<directory>";
        }
        return false;
    }

    size_t template_count =
        sizeof(bake_env_required_test_templates) / sizeof(bake_env_required_test_templates[0]);
    for (size_t i = 0; i < template_count; i++) {
        const char *name = bake_env_required_test_templates[i];
        char *path = bake_path_join(dir, name);
        bool exists = bake_path_exists(path);
        ecs_os_free(path);

        if (!exists) {
            if (missing_out) {
                *missing_out = name;
            }
            return false;
        }
    }

    return true;
}

int bake_env_copy_tree_recursive(const char *src, const char *dst) {
    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(src, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        bake_dir_entry_t *entry = &entries[i];
        if (bake_is_dot_dir(entry->name)) {
            continue;
        }

        char *dst_path = bake_path_join(dst, entry->name);
        int rc = 0;
        if (entry->is_dir) {
            rc = bake_os_mkdirs(dst_path);
            if (rc == 0) {
                rc = bake_env_copy_tree_recursive(entry->path, dst_path);
            }
        } else {
            rc = bake_os_file_copy(entry->path, dst_path);
        }

        ecs_os_free(dst_path);
        if (rc != 0) {
            bake_dir_entries_free(entries, count);
            return -1;
        }
    }

    bake_dir_entries_free(entries, count);
    return 0;
}

bool bake_env_paths_overlap(const char *src, const char *dst) {
    if (!src || !dst || !src[0] || !dst[0]) {
        return false;
    }

    /* Compare raw paths, not realpath-resolved paths: a destination that is
     * a symlink resolves through the symlink and would falsely overlap with
     * the source. bake_os_rmtree handles symlinks safely by unlinking them
     * rather than recursing into the target. The check here is for the case
     * where dst literally lives inside src (or vice versa) on disk, e.g.
     * when bake_home is misconfigured to the project root and dst becomes
     * <project>/include/<id>. */
    return bake_path_equal_normalized(src, dst) ||
        bake_path_has_prefix_normalized(src, dst, NULL) ||
        bake_path_has_prefix_normalized(dst, src, NULL);
}

int bake_env_copy_tree_exact(const char *src, const char *dst) {
    if (bake_env_paths_overlap(src, dst)) {
        ecs_err(
            "refusing to sync tree: src and dst overlap "
            "(src='%s', dst='%s'); aborting to avoid deleting source files",
            src ? src : "(null)", dst ? dst : "(null)");
        return -1;
    }

    if (bake_os_rmtree(dst) != 0) {
        return -1;
    }

    if (!src || !bake_path_exists(src) || !bake_path_is_dir(src)) {
        return 0;
    }

    if (bake_os_mkdirs(dst) != 0) {
        return -1;
    }

    return bake_env_copy_tree_recursive(src, dst);
}

static char* bake_env_test_templates_from_home(const char *home) {
    if (!home || !home[0]) {
        return NULL;
    }

    char *test_dir = bake_path_join(home, "test");
    if (bake_env_has_required_test_templates(test_dir, NULL)) {
        return test_dir;
    }

    ecs_os_free(test_dir);
    return NULL;
}

char* bake_env_find_test_template_source(void) {
    char *cwd = bake_os_getcwd();
    char *cwd_templates = cwd ? bake_path_join(cwd, "templates/test_harness") : NULL;
    ecs_os_free(cwd);
    if (cwd_templates && bake_env_has_required_test_templates(cwd_templates, NULL)) {
        return cwd_templates;
    }
    ecs_os_free(cwd_templates);

    const char *exe_path = getenv("BAKE3_EXEC_PATH");
    if (exe_path && exe_path[0]) {
        char *exe_dir = bake_path_dirname(exe_path);
        char *exe_test_templates = bake_env_test_templates_from_home(exe_dir);
        if (exe_test_templates) {
            ecs_os_free(exe_dir);
            return exe_test_templates;
        }

        char *root_dir = bake_path_dirname(exe_dir);
        char *root_templates = bake_path_join(root_dir, "templates/test_harness");

        ecs_os_free(exe_dir);
        ecs_os_free(root_dir);

        if (bake_env_has_required_test_templates(root_templates, NULL)) {
            return root_templates;
        }
        ecs_os_free(root_templates);
    }

    const char *global_home = getenv("BAKE_GLOBAL_HOME");
    char *global_templates = bake_env_test_templates_from_home(global_home);
    if (global_templates) {
        return global_templates;
    }

    char *home = bake_os_home_path();
    char *default_home = home ? bake_path_join(home, "bake3") : NULL;
    ecs_os_free(home);

    char *default_templates = bake_env_test_templates_from_home(default_home);
    ecs_os_free(default_home);
    if (default_templates) {
        return default_templates;
    }

    return NULL;
}

static bool bake_env_test_templates_stale(const char *src, const char *dst) {
    if (!src) {
        return false;
    }

    size_t template_count =
        sizeof(bake_env_required_test_templates) / sizeof(bake_env_required_test_templates[0]);
    for (size_t i = 0; i < template_count; i++) {
        const char *name = bake_env_required_test_templates[i];
        char *src_path = bake_path_join(src, name);
        char *dst_path = bake_path_join(dst, name);
        int64_t src_mtime = bake_os_file_mtime(src_path);
        int64_t dst_mtime = bake_os_file_mtime(dst_path);
        ecs_os_free(src_path);
        ecs_os_free(dst_path);
        if (src_mtime > dst_mtime) {
            return true;
        }
    }

    return false;
}

int bake_env_ensure_local_test_templates(const bake_context_t *ctx) {
    if (!(ctx && ctx->opts.local_env) && !bake_env_is_local()) {
        return 0;
    }

    char *test_dst = bake_path_join(ctx->bake_home, "test");
    char *test_src = bake_env_find_test_template_source();
    if (bake_env_has_required_test_templates(test_dst, NULL) &&
        !bake_env_test_templates_stale(test_src, test_dst))
    {
        ecs_os_free(test_src);
        ecs_os_free(test_dst);
        return 0;
    }

    if (!test_src) {
        ecs_err(
            "failed to initialize local test harness templates at %s; "
            "expected templates/test_harness in current working directory, "
            "test templates next to bake executable, BAKE_GLOBAL_HOME/test, "
            "or ~/bake3/test",
            test_dst);
        ecs_os_free(test_dst);
        return -1;
    }

    int rc = bake_env_copy_tree_exact(test_src, test_dst);
    if (rc != 0) {
        ecs_err("failed to install local test harness templates from %s to %s", test_src, test_dst);
    }

    ecs_os_free(test_src);
    ecs_os_free(test_dst);
    return rc;
}
