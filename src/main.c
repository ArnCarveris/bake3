#include "bake/commands.h"
#include "bake/context.h"
#include "bake/environment.h"
#include "bake/os.h"

int main(int argc, char *argv[]) {
    ecs_os_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int rc = 1;
    bake_options_t opts = { .command = "build", .mode = "debug" };
    const char *local_env_name = NULL;
    char *local_bake_home = NULL;
    bool ctx_initialized = false;
    bake_context_t ctx;

    char *cwd = bake_os_getcwd();
    if (!cwd) {
        ecs_err("failed to get current directory");
        return 1;
    }
    opts.cwd = cwd;

    {
        char *exe_path = bake_os_executable_path();
        if (!exe_path && argv[0] && argv[0][0]) {
            exe_path = bake_path_is_abs(argv[0])
                ? ecs_os_strdup(argv[0])
                : bake_path_join(cwd, argv[0]);
        }
        if (exe_path && bake_path_exists(exe_path)) {
            bake_os_setenv("BAKE3_EXEC_PATH", exe_path);
        } else {
            bake_os_unsetenv("BAKE3_EXEC_PATH");
        }
        ecs_os_free(exe_path);
    }

    bool seen_command = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "--")) {
            opts.run_argc = argc - i - 1;
            opts.run_argv = (const char**)&argv[i + 1];
            break;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            bake_print_help();
            rc = 0;
            goto cleanup;
        }

#define BFLAG(name, field) if (!strcmp(arg, name)) { opts.field = true; continue; }
        BFLAG("-r", recursive)
        BFLAG("--standalone", standalone)
        BFLAG("--strict", strict)
        BFLAG("--coverage", coverage)
        BFLAG("--fix-lint", fix_lint)
        BFLAG("--trace", trace)
        BFLAG("--local", setup_local)
        BFLAG("--json", json)
        BFLAG("--all-users", all_users)
        BFLAG("--full", ps_full)
#undef BFLAG

        if (!strcmp(arg, "--local-env") || !strncmp(arg, "--local-env=", 12)) {
            opts.local_env = true;
            const char *candidate = NULL;
            if (arg[11] == '=') {
                candidate = arg + 12;
                if (!candidate[0]) {
                    ecs_err("missing value for --local-env");
                    goto cleanup;
                }
            } else if ((i + 1) < argc && argv[i + 1][0] && argv[i + 1][0] != '-' &&
                       !bake_is_command(argv[i + 1]))
            {
                if (bake_local_env_name_chars_valid(argv[i + 1]) &&
                    !bake_path_exists(argv[i + 1]))
                {
                    candidate = argv[++i];
                } else if ((i + 2) < argc && bake_is_command(argv[i + 2])) {
                    candidate = argv[++i];
                }
            }
            if (candidate && !bake_local_env_name_chars_valid(candidate)) {
                ecs_err("invalid --local-env name '%s' (use letters, digits, '.', '_' or '-')", candidate);
                goto cleanup;
            }
            if (candidate && bake_local_env_name_reserved(candidate)) {
                ecs_err("reserved --local-env name '%s', pick another name", candidate);
                goto cleanup;
            }
            local_env_name = candidate;
            continue;
        }

        if (!strcmp(arg, "-j")) {
            if ((i + 1) >= argc) {
                ecs_err("missing value for -j");
                goto cleanup;
            }
            char *end = NULL;
            long jobs = strtol(argv[++i], &end, 10);
            if (jobs < 1 || !end || *end) {
                ecs_err("invalid value for -j: %s", argv[i]);
                goto cleanup;
            }
            opts.jobs = (int)jobs;
            continue;
        }

        if (!strcmp(arg, "--port")) {
            if ((i + 1) >= argc) {
                ecs_err("missing value for --port");
                goto cleanup;
            }
            char *end = NULL;
            long port = strtol(argv[++i], &end, 10);
            if (port < 1 || port > 65535 || !end || *end) {
                ecs_err("invalid value for --port: %s", argv[i]);
                goto cleanup;
            }
            opts.port = (int32_t)port;
            continue;
        }

#define VARG(name, field) \
        if (!strcmp(arg, name)) { \
            if (i + 1 >= argc) { \
                ecs_err("missing value for %s", name); \
                goto cleanup; \
            } \
            opts.field = argv[++i]; \
            continue; \
        }
        VARG("--cfg", mode)
        VARG("--cc", cc)
        VARG("--cxx", cxx)
        VARG("--target", toolchain)
        VARG("--run-prefix", run_prefix)
        VARG("--kill", ps_kill)
        VARG("--build-json", build_json)
#undef VARG

        if (arg[0] == '-') {
            ecs_err("unknown option: %s", arg);
            goto cleanup;
        }
        if (!seen_command && bake_is_command(arg)) {
            opts.command = arg;
            seen_command = true;
            continue;
        }
        if (seen_command && !strcmp(opts.command, "bundle")) {
            if (!opts.bundle_action) {
                opts.bundle_action = arg;
                continue;
            }
            if (!opts.bundle_name) {
                opts.bundle_name = arg;
                continue;
            }
            ecs_err("unexpected argument: %s", arg);
            goto cleanup;
        }
        if (!opts.target) {
            opts.target = arg;
            continue;
        }
        ecs_err("unexpected argument: %s", arg);
        goto cleanup;
    }

    if (!opts.cc) {
        const char *env_cc = getenv("BAKE_CC");
        if (env_cc && env_cc[0]) {
            opts.cc = env_cc;
        }
    }
    if (!opts.cxx) {
        const char *env_cxx = getenv("BAKE_CXX");
        if (env_cxx && env_cxx[0]) {
            opts.cxx = env_cxx;
        }
    }

    if (bake_target_name_is_em(opts.toolchain)) {
        if (!opts.cc) opts.cc = "emcc";
        if (!opts.cxx) opts.cxx = "em++";
    }

    if (opts.local_env) {
        const char *existing_bake_home = getenv("BAKE_HOME");
        if (existing_bake_home && existing_bake_home[0]) {
            bake_os_setenv("BAKE_GLOBAL_HOME", existing_bake_home);
        } else {
            bake_os_unsetenv("BAKE_GLOBAL_HOME");
        }

        local_bake_home = bake_local_env_home(cwd, local_env_name);
        if (!local_bake_home) {
            ecs_err("failed to resolve local bake environment path");
            goto cleanup;
        }
        bake_os_setenv("BAKE_HOME", local_bake_home);
        bake_os_setenv("BAKE_LOCAL_ENV", "1");
    } else {
        bake_os_setenv("BAKE_LOCAL_ENV", "0");
        bake_os_unsetenv("BAKE_GLOBAL_HOME");
    }

    if (opts.setup_local && strcmp(opts.command, "setup")) {
        ecs_err("--local can only be used with the setup command");
        goto cleanup;
    }

    if ((opts.json || opts.all_users || opts.ps_full || opts.ps_kill) &&
        strcmp(opts.command, "ps"))
    {
        ecs_err("--json, --all-users, --full and --kill can only be used with "
            "the ps command");
        goto cleanup;
    }

    if (opts.build_json && !bake_command_builds(opts.command)) {
        ecs_err("--build-json can only be used with the build, rebuild, run, "
            "test and bench commands");
        goto cleanup;
    }

    if (opts.coverage && !bake_command_builds(opts.command)) {
        ecs_err("--coverage can only be used with the build, rebuild, run, "
            "test and bench commands");
        goto cleanup;
    }

    if (bake_context_init(&ctx, &opts) != 0) {
        ecs_err("failed to initialize bake context");
        goto cleanup;
    }
    ctx_initialized = true;

    rc = bake_execute(&ctx, argv[0]) == 0 ? 0 : 1;

cleanup:
    if (ctx_initialized) bake_context_fini(&ctx);
    ecs_os_free(local_bake_home);
    ecs_os_free(cwd);
    return rc;
}
