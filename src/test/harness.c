#include "bake/test_harness.h"
#include "bake/build.h"
#include "bake/os.h"
#include "bake/ps.h"

#include "harness_internal.h"
#include "common/harness_util.h"

#define BAKE_HARNESS_LOCK_TIMEOUT_SEC (10 * 60)

static bool bake_should_generate_harness(const char *project_json, const char *exe_path) {
    if (!exe_path || !exe_path[0]) {
        return true;
    }

    int64_t project_mtime = bake_os_file_mtime(project_json);
    if (project_mtime < 0) {
        return true;
    }

    int64_t exe_mtime = bake_os_file_mtime(exe_path);
    if (exe_mtime < 0) {
        return true;
    }

    return project_mtime > exe_mtime;
}

int bake_test_generate_harness(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *exe_path)
{
    BAKE_UNUSED(ctx);

    int rc = 0;
    bake_suite_list_t suites = {0};
    char *lock_path = NULL;
    bake_lock_t lock = {0};
    char *project_json = bake_path_join(cfg->path, "project.json");

    if (!bake_path_exists(project_json)) {
        goto cleanup;
    }

    char *main_src = bake_harness_source_path(cfg, "main");
    bool main_missing = !bake_path_exists(main_src);
    ecs_os_free(main_src);

    if (!main_missing && !bake_should_generate_harness(project_json, exe_path)) {
        goto cleanup;
    }

    lock_path = bake_path_join3(cfg->path, ".bake", "harness.lock");
    if (bake_os_lock_acquire(lock_path, BAKE_HARNESS_LOCK_TIMEOUT_SEC, &lock) != 0) {
        rc = -1;
        goto cleanup;
    }

    if (bake_parse_project_tests(project_json, &suites) != 0) {
        rc = -1;
        goto cleanup;
    }

    for (int32_t i = 0; i < suites.count; i++) {
        if (bake_generate_suite_file(cfg, &suites.items[i]) != 0) {
            rc = -1;
            goto cleanup;
        }
    }

    if (suites.count) {
        rc = bake_generate_main(cfg, &suites);
    }

cleanup:
    bake_os_lock_release(&lock);
    ecs_os_free(lock_path);
    bake_suite_list_fini(&suites);
    ecs_os_free(project_json);
    return rc;
}

static char* bake_test_coverage_source(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *tmpl_src)
{
    char *content = bake_file_read(tmpl_src, NULL);
    char *dir = bake_coverage_dir(cfg, ctx->opts.mode);
    char *result = NULL;

    if (!content || !dir || bake_os_mkdirs(dir) != 0) {
        goto cleanup;
    }

    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&buf, "#define BAKE_TEST_COVERAGE_DIR ");
    bake_append_c_literal(&buf, dir);
    ecs_strbuf_appendstr(&buf, "\n");
    ecs_strbuf_appendstr(&buf, content);
    result = ecs_strbuf_get(&buf);

cleanup:
    ecs_os_free(content);
    ecs_os_free(dir);
    return result;
}

static int bake_test_write_source(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *tmpl_src,
    const char *src_path)
{
    if (!ctx->opts.coverage) {
        return bake_os_file_copy(tmpl_src, src_path);
    }

    char *content = bake_test_coverage_source(ctx, cfg, tmpl_src);
    if (!content) {
        ecs_err("failed to generate coverage harness for %s", cfg->id);
        return -1;
    }

    int rc = bake_file_write(src_path, content);
    ecs_os_free(content);
    return rc;
}

int bake_test_generate_builtin_api(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *gen_dir,
    char **src_out)
{
    if (!gen_dir || !src_out) return -1;

    int rc = -1;
    char *hdr_path = bake_path_join(gen_dir, "bake_test.h");
    char *src_path = bake_path_join(gen_dir, "bake_test.c");
    char *tmpl_hdr = bake_harness_template_file(ctx, "bake_test.h");
    char *tmpl_src = bake_harness_template_file(ctx, "bake_test.c");

    if (tmpl_hdr && tmpl_src &&
        bake_os_file_copy(tmpl_hdr, hdr_path) == 0 &&
        bake_test_write_source(ctx, cfg, tmpl_src, src_path) == 0)
    {
        *src_out = src_path;
        src_path = NULL;
        rc = 0;
    }

    ecs_os_free(hdr_path);
    ecs_os_free(src_path);
    ecs_os_free(tmpl_hdr);
    ecs_os_free(tmpl_src);
    return rc;
}

static char* bake_test_coverage_json_path(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    char **test_json_out)
{
    const char *json = NULL;
    for (int i = 0; i < ctx->opts.run_argc; i++) {
        const char *arg = ctx->opts.run_argv[i];
        bool has_value = !strcmp(arg, "--json") || !strcmp(arg, "-j") ||
            !strcmp(arg, "--timeout") || !strcmp(arg, "--param");
        if (!has_value || (i + 1) >= ctx->opts.run_argc) {
            continue;
        }
        if (!strcmp(arg, "--json")) {
            json = ctx->opts.run_argv[i + 1];
        }
        i++;
    }
    if (!json || !json[0]) {
        return NULL;
    }

    char *run_dir = bake_project_run_dir(cfg);
    char *test_json = bake_path_is_abs(json) || !run_dir
        ? ecs_os_strdup(json)
        : bake_path_join(run_dir, json);
    ecs_os_free(run_dir);

    size_t len = strlen(test_json);
    size_t base_len = bake_has_suffix(test_json, ".json") ? len - 5 : len;
    char *coverage_json = flecs_asprintf("%.*s.coverage.json", (int)base_len, test_json);
    *test_json_out = test_json;
    return coverage_json;
}

int bake_test_run_project(bake_context_t *ctx, const bake_project_cfg_t *cfg, const char *exe_path) {
    char *old_threads = NULL;
    const char *old_env = getenv("BAKE_TEST_THREADS");
    if (old_env) {
        old_threads = ecs_os_strdup(old_env);
    }

    if (ctx && ctx->opts.jobs > 0) {
        char jobs_str[32];
        ecs_os_snprintf(jobs_str, sizeof(jobs_str), "%d", ctx->opts.jobs);
        bake_os_setenv("BAKE_TEST_THREADS", jobs_str);
    }

    if (ctx && bake_coverage_export_env(ctx, cfg) != 0) {
        ecs_os_free(old_threads);
        return -1;
    }

    char *test_json = NULL;
    char *coverage_json = ctx && ctx->opts.coverage
        ? bake_test_coverage_json_path(ctx, cfg, &test_json)
        : NULL;
    int64_t test_json_mtime = test_json ? bake_os_file_mtime(test_json) : -1;

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    if (ctx && ctx->opts.run_prefix) {
        ecs_strbuf_append(&cmd, "%s ", ctx->opts.run_prefix);
    }
    char *abs_exe = bake_path_resolve(exe_path);
    char *quoted_exe = bake_shell_quote_arg(abs_exe ? abs_exe : exe_path);
    ecs_strbuf_appendstr(&cmd, quoted_exe);
    ecs_os_free(quoted_exe);
    ecs_os_free(abs_exe);
    if (ctx && ctx->opts.jobs > 0) {
        ecs_strbuf_append(&cmd, " -j %d", ctx->opts.jobs);
    }
    for (int i = 0; ctx && i < ctx->opts.run_argc; i++) {
        char *quoted_arg = bake_shell_quote_arg(ctx->opts.run_argv[i]);
        ecs_strbuf_append(&cmd, " %s", quoted_arg);
        ecs_os_free(quoted_arg);
    }

    char *cmd_str = ecs_strbuf_get(&cmd);
    char *run_dir = bake_project_run_dir(cfg);
    char *env_name = NULL;
    bake_ps_env_kind_t env_kind = ctx ?
        bake_ps_env_from_home(ctx->bake_home, &env_name) : BakePsEnvUnknown;
    bake_ps_info_t ps = {
        .project = cfg->id,
        .cfg = ctx ? bake_effective_mode(ctx->opts.mode) : NULL,
        .env = env_name,
        .env_kind = env_kind,
        .bake_home = ctx ? ctx->bake_home : NULL,
        .workspace = ctx ? ctx->opts.cwd : NULL,
        .kind = "test"
    };
    int rc = bake_run_command_tracked(cmd_str, false, run_dir, &ps);
    ecs_os_free(env_name);
    ecs_os_free(run_dir);
    ecs_os_free(cmd_str);

    if (coverage_json) {
        int64_t mtime = bake_os_file_mtime(test_json);
        if (mtime >= 0 && mtime != test_json_mtime &&
            bake_coverage_project_report(ctx, cfg, exe_path, coverage_json) != 0)
        {
            rc = -1;
        }
    }
    ecs_os_free(test_json);
    ecs_os_free(coverage_json);

    if (ctx && ctx->opts.jobs > 0) {
        if (old_threads) {
            bake_os_setenv("BAKE_TEST_THREADS", old_threads);
        } else {
            bake_os_unsetenv("BAKE_TEST_THREADS");
        }
    }
    ecs_os_free(old_threads);

    return rc;
}
