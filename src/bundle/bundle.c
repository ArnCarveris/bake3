#include "bake/build_report.h"
#include "bake/bundle.h"
#include "bake/discovery.h"
#include "bake/environment.h"
#include "bake/model.h"
#include "bake/os.h"

#include <flecs.h>

#define BAKE_BUNDLE_LOCK_TIMEOUT_SEC (60 * 60)

static char* bake_bundle_ref_segment(const bake_bundle_t *bundle) {
    if (bundle->commit && bundle->commit[0]) {
        return flecs_asprintf("commits/%s", bundle->commit);
    }
    if (bundle->tag && bundle->tag[0]) {
        return flecs_asprintf("tags/%s", bundle->tag);
    }
    if (bundle->branch && bundle->branch[0]) {
        return flecs_asprintf("branches/%s", bundle->branch);
    }
    return ecs_os_strdup("default");
}

static char* bake_bundle_root_dir(const bake_project_cfg_t *cfg, const bake_bundle_t *bundle) {
    if (!cfg || !cfg->path || !bundle || !bundle->id) {
        return NULL;
    }
    char *bake = bake_path_join(cfg->path, ".bake");
    char *bundles = bake_path_join(bake, "bundles");
    ecs_os_free(bake);
    char *id_dir = bake_path_join(bundles, bundle->id);
    ecs_os_free(bundles);
    char *ref = bake_bundle_ref_segment(bundle);
    char *root = bake_path_join(id_dir, ref);
    ecs_os_free(id_dir);
    ecs_os_free(ref);
    return root;
}

static char* bake_bundle_source_dir(const char *root) {
    return root ? bake_path_join(root, "src") : NULL;
}

static char* bake_bundle_build_dir(const char *root, const char *triplet) {
    if (!root || !triplet) return NULL;
    char *base = bake_path_join(root, "build");
    char *path = bake_path_join(base, triplet);
    ecs_os_free(base);
    return path;
}

static char* bake_bundle_install_dir(const char *root, const char *triplet) {
    if (!root || !triplet) return NULL;
    char *base = bake_path_join(root, "install");
    char *path = bake_path_join(base, triplet);
    ecs_os_free(base);
    return path;
}

static char* bake_bundle_install_marker(const char *install_dir) {
    return bake_path_join(install_dir, ".bake_bundle_built");
}

static bool bake_bundle_uses_cargo(const bake_bundle_t *bundle);
static bool bake_bundle_cargo_release(const char *mode);

static char* bake_bundle_fingerprint(
    const bake_bundle_t *bundle,
    const char *bundle_src_dir,
    const char *install_dir,
    const char *mode)
{
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_append(&buf, "build_system=%s\n",
        bundle->build_system ? bundle->build_system : "");
    if (bake_bundle_uses_cargo(bundle)) {
        ecs_strbuf_append(&buf, "cargo_profile=%s\n",
            bake_bundle_cargo_release(mode) ? "release" : "debug");
    }
    ecs_strbuf_append(&buf, "profile=%s\n",
        bundle->profile ? bundle->profile : "");
    ecs_strbuf_append(&buf, "subdir=%s\n", bundle->subdir ? bundle->subdir : "");
    ecs_strbuf_append(&buf, "library=%s\n", bundle->library ? bundle->library : "");
    ecs_strbuf_append(&buf, "header_only=%d\n", bundle->header_only ? 1 : 0);
    ecs_strbuf_append(&buf, "install_dir=%s\n", install_dir ? install_dir : "");
    for (int32_t i = 0; i < bundle->cmake_args.count; i++) {
        ecs_strbuf_append(&buf, "cmake_arg=%s\n", bundle->cmake_args.items[i]);
    }
    for (int32_t i = 0; i < bundle->cargo_args.count; i++) {
        ecs_strbuf_append(&buf, "cargo_arg=%s\n", bundle->cargo_args.items[i]);
    }
    ecs_strbuf_append(&buf, "source_mtime=%lld\n",
        (long long)bake_os_tree_newest_mtime(bundle_src_dir));
    return ecs_strbuf_get(&buf);
}

static const char* bake_bundle_cmake_build_type(const char *mode) {
    if (!mode || !mode[0]) {
        return "Debug";
    }
    if (!strcmp(mode, "release")) return "Release";
    if (!strcmp(mode, "profile")) return "RelWithDebInfo";
    if (!strcmp(mode, "sanitize")) return "Debug";
    return "Debug";
}

static int bake_bundle_clone(const bake_bundle_t *bundle, const char *dest) {
    if (!bundle->repository || !bundle->repository[0]) {
        ecs_err("bundle '%s' has no repository", bundle->id);
        return -1;
    }

    const char *ref = NULL;
    if (bundle->branch && bundle->branch[0]) ref = bundle->branch;
    else if (bundle->tag && bundle->tag[0]) ref = bundle->tag;

    char *quoted_dest = bake_shell_quote_arg(dest);
    char *quoted_repo = bake_shell_quote_arg(bundle->repository);
    char *quoted_ref = ref ? bake_shell_quote_arg(ref) : NULL;

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&cmd, "git clone --recurse-submodules");
    if (ref) {
        ecs_strbuf_append(&cmd, " --branch %s --depth 1 --shallow-submodules", quoted_ref);
    } else if (!bundle->commit || !bundle->commit[0]) {
        ecs_strbuf_appendstr(&cmd, " --depth 1 --shallow-submodules");
    }
    ecs_strbuf_append(&cmd, " %s %s", quoted_repo, quoted_dest);
    ecs_os_free(quoted_repo);
    ecs_os_free(quoted_ref);

    char *cmd_str = ecs_strbuf_get(&cmd);
    int rc = bake_run_command(cmd_str, true);
    ecs_os_free(cmd_str);
    if (rc != 0) {
        ecs_os_free(quoted_dest);
        return -1;
    }

    if (bundle->commit && bundle->commit[0]) {
        ecs_strbuf_t co = ECS_STRBUF_INIT;
        char *quoted_path = quoted_dest;
        quoted_dest = NULL;
        ecs_strbuf_append(&co, "git -C %s fetch --depth 1 origin %s", quoted_path, bundle->commit);
        char *fetch_cmd = ecs_strbuf_get(&co);
        rc = bake_run_command(fetch_cmd, true);
        ecs_os_free(fetch_cmd);
        if (rc != 0) {
            ecs_os_free(quoted_path);
            return -1;
        }

        ecs_strbuf_t cmd2 = ECS_STRBUF_INIT;
        ecs_strbuf_append(&cmd2, "git -C %s checkout %s", quoted_path, bundle->commit);
        char *checkout_cmd = ecs_strbuf_get(&cmd2);
        rc = bake_run_command(checkout_cmd, true);
        ecs_os_free(checkout_cmd);
        ecs_os_free(quoted_path);
        if (rc != 0) {
            return -1;
        }
    }

    ecs_os_free(quoted_dest);
    return 0;
}

static int bake_bundle_run_git(
    const char *repository,
    const char *scratch_dir,
    const char *const *args,
    char **output_out)
{
    if (output_out) {
        *output_out = NULL;
    }

    int32_t arg_count = 0;
    while (args[arg_count]) {
        arg_count++;
    }

    const char **argv = ecs_os_malloc_n(const char*, arg_count + 4);
    argv[0] = "git";
    argv[1] = "-C";
    argv[2] = repository;
    for (int32_t i = 0; i < arg_count; i++) {
        argv[i + 3] = args[i];
    }
    argv[arg_count + 3] = NULL;

    char *stdout_path = bake_path_join(scratch_dir, ".bake_git_stdout");
    char *stderr_path = bake_path_join(scratch_dir, ".bake_git_stderr");
    bake_remove_file_if_exists(stdout_path);
    bake_remove_file_if_exists(stderr_path);

    bake_process_stdio_t stdio_cfg = {
        .stdout_path = stdout_path,
        .stderr_path = stderr_path
    };
    bake_process_result_t result = {0};
    int spawn_rc = bake_proc_run(argv, &stdio_cfg, &result);
    int rc = spawn_rc == 0 ? result.exit_code : -1;
    if (rc == 0 && output_out) {
        *output_out = bake_file_read_trimmed(stdout_path);
        if (!*output_out) {
            rc = -1;
        }
    }

    bake_remove_file_if_exists(stdout_path);
    bake_remove_file_if_exists(stderr_path);
    ecs_os_free(stdout_path);
    ecs_os_free(stderr_path);
    ecs_os_free(argv);
    return rc;
}

static char* bake_bundle_local_repository(
    const bake_project_cfg_t *cfg,
    const bake_bundle_t *bundle)
{
    if (!cfg || !cfg->path || !bundle || !bundle->repository ||
        !bundle->repository[0])
    {
        return NULL;
    }

    if (bake_path_is_abs(bundle->repository)) {
        return bake_path_is_dir(bundle->repository)
            ? bake_path_resolve(bundle->repository)
            : NULL;
    }

    char *candidate = bake_path_join(cfg->path, bundle->repository);
    if (bake_path_is_dir(candidate)) {
        char *resolved = bake_path_resolve(candidate);
        ecs_os_free(candidate);
        return resolved;
    }
    ecs_os_free(candidate);

    return bake_path_is_dir(bundle->repository)
        ? bake_path_resolve(bundle->repository)
        : NULL;
}

static const char* bake_bundle_short_branch(const char *branch) {
    static const char prefix[] = "refs/heads/";
    if (branch && !strncmp(branch, prefix, sizeof(prefix) - 1)) {
        return branch + sizeof(prefix) - 1;
    }
    return branch;
}

static char* bake_bundle_branch_ref(const char *branch) {
    if (!branch || !branch[0]) {
        return NULL;
    }
    if (!strncmp(branch, "refs/", 5)) {
        return ecs_os_strdup(branch);
    }
    return flecs_asprintf("refs/heads/%s", branch);
}

static char* bake_bundle_current_branch(
    const char *src_dir,
    const char *scratch_dir)
{
    const char *args[] = {
        "symbolic-ref", "--quiet", "--short", "HEAD", NULL
    };
    char *branch = NULL;
    if (bake_bundle_run_git(src_dir, scratch_dir, args, &branch) != 0) {
        ecs_os_free(branch);
        return NULL;
    }
    return branch;
}

static void bake_bundle_notice_if_behind(
    const bake_project_cfg_t *cfg,
    const bake_bundle_t *bundle,
    const char *root_dir,
    const char *src_dir)
{
    if ((bundle->commit && bundle->commit[0]) ||
        (bundle->tag && bundle->tag[0]))
    {
        return;
    }

    char *repository = bake_bundle_local_repository(cfg, bundle);
    if (!repository) {
        return;
    }

    char *current_branch = NULL;
    const char *branch = bake_bundle_short_branch(bundle->branch);
    if (!branch || !branch[0]) {
        current_branch = bake_bundle_current_branch(src_dir, root_dir);
        branch = current_branch;
    }

    char *branch_ref = bake_bundle_branch_ref(branch);
    char *remote_head = NULL;
    char *checkout_head = NULL;
    if (!branch_ref) {
        goto cleanup;
    }

    const char *remote_args[] = {
        "rev-parse", "--verify", branch_ref, NULL
    };
    if (bake_bundle_run_git(
        repository, root_dir, remote_args, &remote_head) != 0)
    {
        goto cleanup;
    }

    const char *checkout_args[] = {
        "rev-parse", "--verify", "HEAD", NULL
    };
    if (bake_bundle_run_git(
        src_dir, root_dir, checkout_args, &checkout_head) != 0)
    {
        goto cleanup;
    }

    if (!strcmp(remote_head, checkout_head)) {
        goto cleanup;
    }

    const char *ancestor_args[] = {
        "merge-base", "--is-ancestor", checkout_head, remote_head, NULL
    };
    if (bake_bundle_run_git(
        repository, root_dir, ancestor_args, NULL) == 0)
    {
        ecs_warn(
            "bundle '%s' checkout is behind its remote branch '%s'; "
            "run bake3 bundle update %s",
            bundle->id, branch, bundle->id);
    }

cleanup:
    ecs_os_free(repository);
    ecs_os_free(current_branch);
    ecs_os_free(branch_ref);
    ecs_os_free(remote_head);
    ecs_os_free(checkout_head);
}

static int bake_bundle_update_one(
    const bake_project_cfg_t *cfg,
    const bake_bundle_t *bundle)
{
    if (bundle->commit && bundle->commit[0]) {
        printf("[bake] bundle '%s' is pinned to commit '%s'; nothing to update\n",
            bundle->id, bundle->commit);
        return 0;
    }
    if (bundle->tag && bundle->tag[0]) {
        printf("[bake] bundle '%s' is pinned to tag '%s'; nothing to update\n",
            bundle->id, bundle->tag);
        return 0;
    }

    int rc = -1;
    char *root_dir = bake_bundle_root_dir(cfg, bundle);
    char *src_dir = bake_bundle_source_dir(root_dir);
    char *lock_path = NULL;
    char *git_dir = NULL;
    char *current_branch = NULL;
    char *repository = NULL;
    char *branch_ref = NULL;
    char *status = NULL;
    char *old_head = NULL;
    char *new_head = NULL;
    bake_lock_t lock = {0};

    if (!root_dir || bake_os_mkdirs(root_dir) != 0) {
        goto cleanup;
    }

    lock_path = bake_path_join(root_dir, ".lock");
    if (bake_os_lock_acquire(
        lock_path, BAKE_BUNDLE_LOCK_TIMEOUT_SEC, &lock) != 0)
    {
        ecs_err("failed to lock bundle '%s'", bundle->id);
        goto cleanup;
    }

    git_dir = bake_path_join(src_dir, ".git");
    if (!bake_path_exists(src_dir) || !bake_path_exists(git_dir)) {
        ecs_err(
            "bundle '%s' has no checkout to update; build the project first",
            bundle->id);
        goto cleanup;
    }

    const char *status_args[] = {
        "status", "--porcelain=v1", "--untracked-files=all",
        "--ignore-submodules=none", NULL
    };
    if (bake_bundle_run_git(src_dir, root_dir, status_args, &status) != 0) {
        ecs_err("failed to inspect bundle '%s' checkout", bundle->id);
        goto cleanup;
    }
    if (status[0]) {
        ecs_err(
            "bundle '%s' checkout has local modifications; refusing to update",
            bundle->id);
        goto cleanup;
    }

    current_branch = bake_bundle_current_branch(src_dir, root_dir);
    if (!current_branch || !current_branch[0]) {
        ecs_err(
            "bundle '%s' checkout is not on a branch; refusing to update",
            bundle->id);
        goto cleanup;
    }

    const char *configured_branch = bake_bundle_short_branch(bundle->branch);
    if (configured_branch && configured_branch[0] &&
        strcmp(configured_branch, current_branch))
    {
        ecs_err(
            "bundle '%s' checkout is on branch '%s', expected '%s'; "
            "refusing to update",
            bundle->id, current_branch, configured_branch);
        goto cleanup;
    }
    const char *branch = configured_branch && configured_branch[0]
        ? configured_branch
        : current_branch;

    repository = bake_bundle_local_repository(cfg, bundle);
    const char *fetch_repository = repository
        ? repository
        : bundle->repository;
    branch_ref = bake_bundle_branch_ref(branch);
    if (!fetch_repository || !fetch_repository[0] || !branch_ref) {
        ecs_err("bundle '%s' has no repository or branch to update", bundle->id);
        goto cleanup;
    }

    const char *head_args[] = {
        "rev-parse", "--verify", "HEAD", NULL
    };
    if (bake_bundle_run_git(src_dir, root_dir, head_args, &old_head) != 0) {
        ecs_err("failed to read bundle '%s' checkout revision", bundle->id);
        goto cleanup;
    }

    const char *fetch_args[] = {
        "fetch", "--quiet", "--recurse-submodules=on-demand", "--",
        fetch_repository, branch_ref, NULL
    };
    if (bake_bundle_run_git(src_dir, root_dir, fetch_args, NULL) != 0) {
        ecs_err(
            "failed to fetch branch '%s' for bundle '%s' from '%s'",
            branch, bundle->id, fetch_repository);
        goto cleanup;
    }

    const char *fetch_head_args[] = {
        "rev-parse", "--verify", "FETCH_HEAD", NULL
    };
    if (bake_bundle_run_git(
        src_dir, root_dir, fetch_head_args, &new_head) != 0)
    {
        ecs_err("failed to read fetched revision for bundle '%s'", bundle->id);
        goto cleanup;
    }

    if (!strcmp(old_head, new_head)) {
        printf("[bake] bundle '%s' is already up to date on branch '%s'\n",
            bundle->id, branch);
        rc = 0;
        goto cleanup;
    }

    const char *ancestor_args[] = {
        "merge-base", "--is-ancestor", old_head, new_head, NULL
    };
    if (bake_bundle_run_git(src_dir, root_dir, ancestor_args, NULL) != 0) {
        ecs_err(
            "bundle '%s' checkout has diverged from branch '%s' and cannot "
            "be fast-forwarded; refusing to update",
            bundle->id, branch);
        goto cleanup;
    }

    const char *merge_args[] = {
        "merge", "--ff-only", "--quiet", "FETCH_HEAD", NULL
    };
    if (bake_bundle_run_git(src_dir, root_dir, merge_args, NULL) != 0) {
        ecs_err(
            "bundle '%s' checkout could not be fast-forwarded to branch '%s'",
            bundle->id, branch);
        goto cleanup;
    }

    const char *submodule_args[] = {
        "submodule", "update", "--init", "--recursive", NULL
    };
    if (bake_bundle_run_git(src_dir, root_dir, submodule_args, NULL) != 0) {
        ecs_err("failed to update submodules for bundle '%s'", bundle->id);
        goto cleanup;
    }

    printf("[bake] bundle '%s' updated %.12s -> %.12s on branch '%s'\n",
        bundle->id, old_head, new_head, branch);
    rc = 0;

cleanup:
    bake_os_lock_release(&lock);
    ecs_os_free(root_dir);
    ecs_os_free(src_dir);
    ecs_os_free(lock_path);
    ecs_os_free(git_dir);
    ecs_os_free(current_branch);
    ecs_os_free(repository);
    ecs_os_free(branch_ref);
    ecs_os_free(status);
    ecs_os_free(old_head);
    ecs_os_free(new_head);
    return rc;
}

static int bake_bundle_run_cmake(
    const bake_bundle_t *bundle,
    const char *src_dir,
    const char *build_dir,
    const char *install_dir,
    const char *mode)
{
    char *quoted_src = bake_shell_quote_arg(src_dir);
    char *quoted_build = bake_shell_quote_arg(build_dir);
    char *quoted_install = bake_shell_quote_arg(install_dir);
    int rc;

    const char *build_type = bake_bundle_cmake_build_type(mode);
    const char *cmake_launcher = bake_target_is_emscripten() ? "emcmake cmake" : "cmake";

    ecs_strbuf_t configure = ECS_STRBUF_INIT;
    ecs_strbuf_append(&configure,
        "%s -S %s -B %s -DCMAKE_INSTALL_PREFIX=%s -DCMAKE_BUILD_TYPE=%s -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        cmake_launcher, quoted_src, quoted_build, quoted_install, build_type);
#if defined(_WIN32)
    /* Match the /MD runtime bake uses for MSVC projects; ignored by MinGW. */
    if (!bake_target_is_emscripten()) {
        ecs_strbuf_appendstr(&configure,
            " -DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
            " -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL");
    }
#endif
    for (int32_t i = 0; i < bundle->cmake_args.count; i++) {
        char *q = bake_shell_quote_arg(bundle->cmake_args.items[i]);
        ecs_strbuf_append(&configure, " %s", q);
        ecs_os_free(q);
    }
    char *cfg_cmd = ecs_strbuf_get(&configure);
    rc = bake_run_command(cfg_cmd, true);
    ecs_os_free(cfg_cmd);
    if (rc != 0) goto cleanup;

    ecs_strbuf_t build = ECS_STRBUF_INIT;
    ecs_strbuf_append(&build, "cmake --build %s --config %s", quoted_build, build_type);
    char *build_cmd = ecs_strbuf_get(&build);
    rc = bake_run_command(build_cmd, true);
    ecs_os_free(build_cmd);
    if (rc != 0) goto cleanup;

    ecs_strbuf_t install = ECS_STRBUF_INIT;
    ecs_strbuf_append(&install, "cmake --install %s --config %s", quoted_build, build_type);
    char *install_cmd = ecs_strbuf_get(&install);
    rc = bake_run_command(install_cmd, true);
    ecs_os_free(install_cmd);

cleanup:
    ecs_os_free(quoted_src);
    ecs_os_free(quoted_build);
    ecs_os_free(quoted_install);
    return rc;
}

static int bake_bundle_resolve_lib(
    const bake_bundle_t *bundle,
    const char *base_dir,
    const char *const *libdirs,
    char **libdir_out)
{
    const char *libname = (bundle->library && bundle->library[0]) ? bundle->library : bundle->id;

#if defined(_WIN32)
    /* MSVC emits <name>.lib; MinGW (used on the Windows CI runner) emits
     * lib<name>.a or <name>.dll.a. */
    const char *prefixes[] = { "", "lib", NULL };
    const char *exts[] = { ".lib", ".a", ".dll.a", NULL };
#elif defined(__APPLE__)
    const char *prefixes[] = { "lib", NULL };
    const char *exts[] = { ".a", ".dylib", NULL };
#else
    const char *prefixes[] = { "lib", NULL };
    const char *exts[] = { ".a", ".so", NULL };
#endif

    for (int32_t d = 0; libdirs[d]; d++) {
        char *libdir = libdirs[d][0]
            ? bake_path_join(base_dir, libdirs[d])
            : ecs_os_strdup(base_dir);
        if (!bake_path_exists(libdir)) {
            ecs_os_free(libdir);
            continue;
        }
        for (int32_t pi = 0; prefixes[pi]; pi++) {
            for (int32_t ei = 0; exts[ei]; ei++) {
                char *file = flecs_asprintf("%s%s%s", prefixes[pi], libname, exts[ei]);
                char *full = bake_path_join(libdir, file);
                ecs_os_free(file);
                if (bake_path_exists(full)) {
                    *libdir_out = libdir;
                    ecs_os_free(full);
                    return 0;
                }
                ecs_os_free(full);
            }
        }
        ecs_os_free(libdir);
    }

    return -1;
}

static bool bake_bundle_uses_cargo(const bake_bundle_t *bundle) {
    return bundle->build_system && !strcmp(bundle->build_system, "cargo");
}

static bool bake_bundle_cargo_release(const char *mode) {
    return !mode || strcmp(mode, "sanitize") != 0;
}

static const char* bake_bundle_cargo_target(void) {
    return bake_target_is_emscripten()
        ? "wasm32-unknown-emscripten"
        : NULL;
}

char* bake_bundle_cargo_command(
    const bake_bundle_t *bundle,
    const char *src_dir,
    const char *build_dir,
    const char *mode)
{
    char *manifest = bake_path_join(src_dir, "Cargo.toml");
    char *quoted_manifest = bake_shell_quote_arg(manifest);
    char *quoted_build = bake_shell_quote_arg(build_dir);
    const char *target = bake_bundle_cargo_target();

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&cmd,
        bake_target_is_emscripten() ? "cargo rustc" : "cargo build");
    if (bake_bundle_cargo_release(mode)) {
        ecs_strbuf_appendstr(&cmd, " --release");
    }
    if (target) {
        ecs_strbuf_append(&cmd, " --target %s", target);
    }
    if (bake_target_is_emscripten()) {
        ecs_strbuf_appendstr(&cmd, " --crate-type staticlib");
    }
    ecs_strbuf_append(&cmd, " --manifest-path %s --target-dir %s",
        quoted_manifest, quoted_build);

    if (bundle) {
        for (int32_t i = 0; i < bundle->cargo_args.count; i++) {
            char *q = bake_shell_quote_arg(bundle->cargo_args.items[i]);
            ecs_strbuf_append(&cmd, " %s", q);
            ecs_os_free(q);
        }
    }

    ecs_os_free(manifest);
    ecs_os_free(quoted_manifest);
    ecs_os_free(quoted_build);
    return ecs_strbuf_get(&cmd);
}

char* bake_bundle_cargo_profile_dir(const char *mode) {
    const char *profile = bake_bundle_cargo_release(mode)
        ? "release"
        : "debug";
    const char *target = bake_bundle_cargo_target();
    return target
        ? bake_path_join(target, profile)
        : ecs_os_strdup(profile);
}

static int bake_bundle_run_cargo(
    const bake_bundle_t *bundle,
    const char *src_dir,
    const char *build_dir,
    const char *mode)
{
    char *cmd_str = bake_bundle_cargo_command(bundle, src_dir, build_dir, mode);
    int rc = bake_run_command(cmd_str, true);
    ecs_os_free(cmd_str);
    return rc;
}

static void bake_bundle_add_include(bake_project_cfg_t *cfg, const char *path) {
    if (!path || !path[0] || !bake_path_exists(path)) {
        return;
    }
    bake_strlist_append_unique(&cfg->bundle_includes, path);
}

static int bake_bundle_apply_to_project(
    bake_project_cfg_t *cfg,
    const bake_bundle_t *bundle,
    const char *src_root,
    const char *install_dir,
    const char *mode)
{
    /* Default include dir: install/include for cmake builds, src for header-only */
    if (bundle->header_only) {
        bake_bundle_add_include(cfg, src_root);
    } else {
        char *include_dir = bake_path_join(install_dir, "include");
        bake_bundle_add_include(cfg, include_dir);
        ecs_os_free(include_dir);
    }

    /* User-specified extra include subdirs (relative to src_root) */
    for (int32_t i = 0; i < bundle->includes.count; i++) {
        const char *rel = bundle->includes.items[i];
        char *full = (rel[0] == '/' || rel[0] == '\\')
            ? ecs_os_strdup(rel)
            : bake_path_join(src_root, rel);
        bake_bundle_add_include(cfg, full);
        ecs_os_free(full);
    }

    /* Extra source files compiled into the consuming project */
    for (int32_t i = 0; i < bundle->sources.count; i++) {
        const char *rel = bundle->sources.items[i];
        char *full = (rel[0] == '/' || rel[0] == '\\')
            ? ecs_os_strdup(rel)
            : bake_path_join(src_root, rel);
        if (!bake_path_exists(full)) {
            ecs_err("bundle '%s': source file not found: %s", bundle->id, full);
            ecs_os_free(full);
            return -1;
        }
        bake_strlist_append_unique(&cfg->bundle_sources, full);
        ecs_os_free(full);
    }

    if (!bundle->header_only) {
        static const char *cmake_libdirs[] = { "lib", "lib64", "lib32", NULL };
        const char *const *libdirs = cmake_libdirs;
        const char *cargo_libdirs[2] = { NULL, NULL };
        char *cargo_profile_dir = NULL;
        if (bake_bundle_uses_cargo(bundle)) {
            cargo_profile_dir = bake_bundle_cargo_profile_dir(mode);
            cargo_libdirs[0] = cargo_profile_dir;
            libdirs = cargo_libdirs;
        }
        const char *base = install_dir;
        char *libpath = NULL;
        if (bake_bundle_resolve_lib(bundle, base, libdirs, &libpath) != 0) {
            const char *libname = (bundle->library && bundle->library[0]) ? bundle->library : bundle->id;
            ecs_err("bundle '%s': could not locate library '%s' under %s", bundle->id, libname, base);
            ecs_os_free(cargo_profile_dir);
            return -1;
        }
        ecs_os_free(cargo_profile_dir);

        bake_strlist_append_unique(&cfg->bundle_libpaths, libpath);

#if !defined(_WIN32)
        if (bake_bundle_uses_cargo(bundle) && !bake_target_is_emscripten()) {
            char *rpath = flecs_asprintf("-Wl,-rpath,%s", libpath);
            bake_strlist_append_unique(&cfg->bundle_ldflags, rpath);
            ecs_os_free(rpath);
        }
#endif

        ecs_os_free(libpath);

        const char *libname = (bundle->library && bundle->library[0]) ? bundle->library : bundle->id;
        bake_strlist_append_unique(&cfg->bundle_libs, libname);
    }

    bake_strlist_merge_unique(&cfg->bundle_libs, &bundle->libs);

    bake_strlist_merge_unique(&cfg->bundle_ldflags, &bundle->ldflags);

    return 0;
}

static int bake_bundle_prepare_one(
    bake_context_t *ctx,
    bake_project_cfg_t *cfg,
    const bake_bundle_t *bundle)
{
    if (!bundle || !bundle->id || !bundle->id[0]) {
        return -1;
    }

    /* A bundle can pin its build profile (e.g. "release") so debug builds
     * of the consuming project still link an optimized dependency. */
    const char *mode = (bundle->profile && bundle->profile[0])
        ? bundle->profile
        : ctx->opts.mode;

    int rc = -1;
    int32_t step = bake_report_open(ctx->report, BAKE_REPORT_KIND_BUNDLE,
        bundle->id, cfg->id);
    const char *error = NULL;
    char *triplet = bake_host_triplet(ctx->opts.mode);
    char *root_dir = bake_bundle_root_dir(cfg, bundle);
    char *src_dir = bake_bundle_source_dir(root_dir);
    char *build_dir = bake_bundle_build_dir(root_dir, triplet);
    char *install_dir = bake_bundle_install_dir(root_dir, triplet);
    char *marker = install_dir ? bake_bundle_install_marker(install_dir) : NULL;
    char *bundle_src_dir = NULL;
    char *lock_path = NULL;
    bake_lock_t lock = {0};

    if (!root_dir) {
        goto cleanup;
    }

    if (bake_os_mkdirs(root_dir) != 0) {
        goto cleanup;
    }

    lock_path = bake_path_join(root_dir, ".lock");
    if (bake_os_lock_acquire(lock_path, BAKE_BUNDLE_LOCK_TIMEOUT_SEC, &lock) != 0) {
        ecs_err("failed to lock bundle '%s'", bundle->id);
        goto cleanup;
    }

    bool need_clone = !bake_path_exists(src_dir);
    if (!need_clone) {
        char *git_dir = bake_path_join(src_dir, ".git");
        if (!bake_path_exists(git_dir)) {
            ecs_warn("bundle '%s' has an incomplete checkout at %s, refetching",
                bundle->id, src_dir);
            if (bake_os_rmtree(src_dir) != 0) {
                ecs_os_free(git_dir);
                goto cleanup;
            }
            need_clone = true;
        }
        ecs_os_free(git_dir);
    }

    if (need_clone) {
        char *src_root = bake_path_dirname(src_dir);
        if (bake_os_mkdirs(src_root) != 0) {
            ecs_os_free(src_root);
            goto cleanup;
        }
        ecs_os_free(src_root);

        ecs_trace("#[green][#[normal] bundle#[green]]#[normal] fetching %s", bundle->id);
        int32_t fetch_step = bake_report_open(ctx->report,
            BAKE_REPORT_KIND_BUNDLE, "fetch", cfg->id);
        int clone_rc = bake_bundle_clone(bundle, src_dir);
        bake_report_close(ctx->report, fetch_step, clone_rc == 0,
            clone_rc == 0 ? NULL : "bundle fetch failed");
        if (clone_rc != 0) {
            ecs_err("failed to clone bundle '%s'", bundle->id);
            error = "bundle fetch failed";
            goto cleanup;
        }
    }

    if (!need_clone) {
        bake_bundle_notice_if_behind(cfg, bundle, root_dir, src_dir);
    }

    bundle_src_dir = (bundle->subdir && bundle->subdir[0])
        ? bake_path_join(src_dir, bundle->subdir)
        : ecs_os_strdup(src_dir);

    if (!bundle->header_only) {
        bool uses_cargo = bake_bundle_uses_cargo(bundle);

        if (!uses_cargo) {
            char *cmakelists = bake_path_join(bundle_src_dir, "CMakeLists.txt");
            bool has_cmake = bake_path_exists(cmakelists);
            ecs_os_free(cmakelists);

            if (!has_cmake) {
                ecs_err("bundle '%s' has no CMakeLists.txt at %s", bundle->id, bundle_src_dir);
                goto cleanup;
            }
        } else {
            char *cargo_toml = bake_path_join(bundle_src_dir, "Cargo.toml");
            bool has_cargo = bake_path_exists(cargo_toml);
            ecs_os_free(cargo_toml);

            if (!has_cargo) {
                ecs_err("bundle '%s' has no Cargo.toml at %s", bundle->id, bundle_src_dir);
                goto cleanup;
            }
        }

        char *fingerprint = bake_bundle_fingerprint(
            bundle, bundle_src_dir, install_dir, mode);

        char *built = bake_file_read(marker, NULL);
        bool up_to_date = built && !strcmp(built, fingerprint);
        bool was_built = built != NULL;
        ecs_os_free(built);

        if (!up_to_date) {
            if (was_built) {
                if (bake_os_rmtree(build_dir) != 0 ||
                    bake_os_rmtree(install_dir) != 0)
                {
                    ecs_os_free(fingerprint);
                    goto cleanup;
                }
            }

            if (bake_os_mkdirs(build_dir) != 0 || bake_os_mkdirs(install_dir) != 0) {
                ecs_os_free(fingerprint);
                goto cleanup;
            }

            ecs_trace("#[green][#[normal] bundle#[green]]#[normal] building %s", bundle->id);
            int32_t build_step = bake_report_open(ctx->report,
                BAKE_REPORT_KIND_BUNDLE, "build", cfg->id);
            int build_rc = uses_cargo
                ? bake_bundle_run_cargo(bundle, bundle_src_dir, install_dir, mode)
                : bake_bundle_run_cmake(bundle, bundle_src_dir, build_dir, install_dir, mode);
            bake_report_close(ctx->report, build_step, build_rc == 0,
                build_rc == 0 ? NULL : "bundle build failed");
            if (build_rc != 0) {
                ecs_err("failed to build bundle '%s'", bundle->id);
                error = "bundle build failed";
                ecs_os_free(fingerprint);
                goto cleanup;
            }

            if (bake_file_write(marker, fingerprint) != 0) {
                ecs_os_free(fingerprint);
                goto cleanup;
            }
        }
        ecs_os_free(fingerprint);
    }

    if (bake_bundle_apply_to_project(cfg, bundle, bundle_src_dir, install_dir, mode) != 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
    bake_report_close(ctx->report, step, rc == 0,
        rc == 0 ? NULL : (error ? error : "bundle preparation failed"));
    bake_os_lock_release(&lock);
    ecs_os_free(lock_path);
    ecs_os_free(triplet);
    ecs_os_free(root_dir);
    ecs_os_free(src_dir);
    ecs_os_free(build_dir);
    ecs_os_free(install_dir);
    ecs_os_free(marker);
    ecs_os_free(bundle_src_dir);
    return rc;
}

bool bake_bundle_is_declared(const bake_project_cfg_t *cfg, const char *id) {
    if (!cfg || !id) {
        return false;
    }
    return bake_bundle_list_find(&cfg->bundles, id) != NULL;
}

int bake_bundle_prepare_for_project(bake_context_t *ctx, bake_project_cfg_t *cfg) {
    if (!ctx || !cfg) {
        return 0;
    }

    if (!ctx->prepare_bundles) {
        return 0;
    }

    int32_t count = bake_bundle_list_count(&cfg->bundles);
    for (int32_t i = 0; i < count; i++) {
        const bake_bundle_t *bundle = bake_bundle_list_get(&cfg->bundles, i);
        if (!bundle) continue;
        if (bake_bundle_prepare_one(ctx, cfg, bundle) != 0) {
            return -1;
        }
    }

    return 0;
}

int bake_bundle_update_command(bake_context_t *ctx) {
    if (!ctx->opts.bundle_action || !ctx->opts.bundle_action[0]) {
        ecs_err("bundle command requires an action; use "
            "'bake3 bundle update [<name>]'");
        return -1;
    }
    if (strcmp(ctx->opts.bundle_action, "update")) {
        ecs_err("unknown bundle action: %s", ctx->opts.bundle_action);
        return -1;
    }

    if (bake_discover_projects(ctx, ctx->opts.cwd, false) < 0) {
        return -1;
    }

    int32_t matched = 0;
    int32_t failed = 0;
    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        const BakeProject *projects = ecs_field(&it, BakeProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const BakeProject *project = &projects[i];
            if (project->external || !project->cfg) {
                continue;
            }

            int32_t count = bake_bundle_list_count(&project->cfg->bundles);
            for (int32_t j = 0; j < count; j++) {
                const bake_bundle_t *bundle =
                    bake_bundle_list_get(&project->cfg->bundles, j);
                if (!bundle || !bundle->id) {
                    continue;
                }
                if (ctx->opts.bundle_name &&
                    strcmp(ctx->opts.bundle_name, bundle->id))
                {
                    continue;
                }

                matched++;
                if (bake_bundle_update_one(project->cfg, bundle) != 0) {
                    failed++;
                }
            }
        }
    }

    if (!matched) {
        if (ctx->opts.bundle_name) {
            ecs_err("bundle '%s' is not declared by a project under %s",
                ctx->opts.bundle_name, ctx->opts.cwd);
            return -1;
        }
        printf("[bake] no bundles declared by projects under %s\n", ctx->opts.cwd);
    }

    return failed ? -1 : 0;
}
