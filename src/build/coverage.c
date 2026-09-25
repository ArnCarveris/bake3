#include "build_internal.h"
#include "bake/environment.h"
#include "bake/os.h"

static const char* bake_coverage_cc(const bake_context_t *ctx) {
    return ctx->opts.cc ? ctx->opts.cc : "cc";
}

static char* bake_coverage_capture(
    const bake_context_t *ctx,
    const char *const *argv)
{
    char *name = flecs_asprintf(".bake_coverage_probe_%lld",
        (long long)bake_os_pid());
    char *stdout_path = bake_path_join(ctx->bake_home, name);
    ecs_os_free(name);

    bake_process_stdio_t stdio_cfg = {
        .stdout_path = stdout_path,
        .stderr_to_stdout = true
    };
    bake_process_result_t result = {0};
    char *output = NULL;
    if (bake_proc_run(argv, &stdio_cfg, &result) == 0 &&
        result.exit_code == 0)
    {
        output = bake_file_read_trimmed(stdout_path);
    }

    bake_remove_file_if_exists(stdout_path);
    ecs_os_free(stdout_path);
    return output;
}

static char* bake_coverage_tool(const bake_context_t *ctx, const char *tool) {
    char *arg = flecs_asprintf("-print-prog-name=%s", tool);
    const char *argv[] = { bake_coverage_cc(ctx), arg, NULL };
    char *path = bake_coverage_capture(ctx, argv);
    ecs_os_free(arg);
    if (!path || !path[0]) {
        ecs_os_free(path);
        return ecs_os_strdup(tool);
    }
    return path;
}

int bake_coverage_init_tools(bake_context_t *ctx) {
    if (ctx->coverage_profdata) {
        return 0;
    }

#if defined(_WIN32)
    ecs_err("--coverage is not supported on Windows");
    return -1;
#else
    if (bake_target_is_emscripten()) {
        ecs_err("--coverage is not supported for the emscripten target");
        return -1;
    }

    const char *cc = bake_coverage_cc(ctx);
    bool is_clang = ctx->compiler_kind == BAKE_COMPILER_CLANG;
    if (!is_clang && ctx->compiler_kind != BAKE_COMPILER_MSVC) {
        const char *argv[] = { cc, "--version", NULL };
        char *version = bake_coverage_capture(ctx, argv);
        is_clang = version && strstr(version, "clang");
        ecs_os_free(version);
    }

    if (!is_clang) {
        ecs_err("--coverage requires clang, '%s' is not clang "
            "(use --cc clang --cxx clang++)", cc);
        return -1;
    }

    ctx->coverage_profdata = bake_coverage_tool(ctx, "llvm-profdata");
    ctx->coverage_cov = bake_coverage_tool(ctx, "llvm-cov");
    return 0;
#endif
}

int bake_coverage_prepare(bake_context_t *ctx) {
    if (!ctx->opts.coverage) {
        return 0;
    }
    return bake_coverage_init_tools(ctx);
}

void bake_add_coverage_flags(
    bool coverage,
    bake_strlist_t *cflags,
    bake_strlist_t *cxxflags,
    bake_strlist_t *ldflags)
{
    if (!coverage) {
        return;
    }

    bake_strlist_append(cflags, "-fprofile-instr-generate");
    bake_strlist_append(cflags, "-fcoverage-mapping");
    bake_strlist_append(cxxflags, "-fprofile-instr-generate");
    bake_strlist_append(cxxflags, "-fcoverage-mapping");
    bake_strlist_append(ldflags, "-fprofile-instr-generate");
}

char* bake_coverage_dir(const bake_project_cfg_t *cfg, const char *mode) {
    char *build_root = bake_project_build_root(cfg->path, cfg->id, mode);
    if (!build_root) {
        return NULL;
    }
    char *dir = bake_path_join(build_root, "coverage");
    ecs_os_free(build_root);
    return dir;
}

char* bake_coverage_profile_pattern(const char *coverage_dir) {
    return bake_path_join(coverage_dir, "profile-%8m.profraw");
}

int bake_coverage_export_env(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg)
{
    if (!ctx->opts.coverage) {
        return 0;
    }

    char *dir = bake_coverage_dir(cfg, ctx->opts.mode);
    if (!dir || bake_os_mkdirs(dir) != 0) {
        ecs_err("failed to create coverage directory for %s", cfg->id);
        ecs_os_free(dir);
        return -1;
    }

    char *pattern = bake_coverage_profile_pattern(dir);
    int rc = bake_os_setenv("LLVM_PROFILE_FILE", pattern);
    ecs_os_free(pattern);
    ecs_os_free(dir);
    return rc;
}
