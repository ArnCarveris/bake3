#include "bake/build_report.h"
#include "bake/discovery.h"
#include "bake/environment.h"
#include "bake/os.h"

static int bake_should_skip_dir(const char *name, bool skip_special_dirs, bool include_tests) {
    if (bake_is_dot_dir(name) || name[0] == '.') return 1;
    static const char *always[] = {"tmp", "target", "out", "build", "template", "templates"};
    for (size_t i = 0; i < sizeof(always) / sizeof(always[0]); i++) {
        if (!strcmp(name, always[i])) return 1;
    }
    if (skip_special_dirs) {
        if (include_tests && (!strcmp(name, "test") || !strcmp(name, "tests"))) {
            return 0;
        }
        static const char *special[] = {"test", "tests", "bench", "benchmarks", "example", "examples"};
        for (size_t i = 0; i < sizeof(special) / sizeof(special[0]); i++) {
            if (!strcmp(name, special[i])) return 1;
        }
    }
    return 0;
}

typedef struct bake_discovery_ctx_t {
    bake_context_t *ctx;
    int32_t discovered;
    bool skip_special_dirs;
} bake_discovery_ctx_t;

static int bake_discovery_add_project_file(
    bake_context_t *ctx,
    const char *project_json_path)
{
    bake_project_cfg_t *cfg = ecs_os_calloc_t(bake_project_cfg_t);
    bake_project_cfg_init(cfg);

    if (bake_project_cfg_load_file(project_json_path, cfg) != 0) {
        bake_project_cfg_fini(cfg);
        ecs_os_free(cfg);
        ecs_err("failed to parse %s", project_json_path);
        return 0;
    }

    if (!bake_model_add_project(ctx->world, cfg, false)) {
        return -1;
    }
    return 1;
}

static int bake_discovery_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_discovery_ctx_t *ctx = ctx_ptr;

    if (entry->is_dir) {
        if (bake_should_skip_dir(entry->name, ctx->skip_special_dirs,
            ctx->ctx->discover_tests))
        {
            return 1;
        }
        char *marker = bake_path_join(entry->path, ".bake-skip");
        bool skip = bake_path_exists(marker);
        ecs_os_free(marker);
        if (skip) {
            return 1;
        }
        return 0;
    }

    if (strcmp(entry->name, "project.json")) {
        return 0;
    }

    int rc = bake_discovery_add_project_file(ctx->ctx, entry->path);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        ctx->discovered++;
    }
    return 0;
}

static int bake_discovery_resolve_dependencies(bake_context_t *ctx) {
    int32_t step = bake_report_open(ctx->report, BAKE_REPORT_KIND_DISCOVERY,
        "resolve dependencies", NULL);
    int rc = -1;

    if (bake_env_import_dependency_closure(ctx) < 0) {
        goto done;
    }

    bake_model_link_dependencies(ctx->world);

    if (bake_env_resolve_external_dependency_binaries(ctx) < 0) {
        goto done;
    }

    if (bake_model_refresh_resolved_deps(ctx->world, ctx->opts.mode) != 0) {
        goto done;
    }

    rc = 0;
done:
    bake_report_close(ctx->report, step, rc == 0,
        rc == 0 ? NULL : "dependency resolution failed");
    return rc;
}

int bake_discover_projects(
    bake_context_t *ctx,
    const char *start_path,
    bool skip_special_dirs)
{
    bake_discovery_ctx_t discovery = {
        .ctx = ctx,
        .discovered = 0,
        .skip_special_dirs = skip_special_dirs
    };

    int32_t step = bake_report_open(ctx->report, BAKE_REPORT_KIND_DISCOVERY,
        start_path, NULL);
    int walk_rc = bake_dir_walk_recursive(start_path, bake_discovery_visit, &discovery);
    bake_report_close(ctx->report, step, walk_rc == 0,
        walk_rc == 0 ? NULL : "project scan failed");

    if (walk_rc != 0) {
        return -1;
    }

    if (bake_discovery_resolve_dependencies(ctx) != 0) {
        return -1;
    }

    return discovery.discovered;
}

/* Promotes external dependencies that were resolved from the bake
 * environment to regular projects when their recorded source location still
 * exists, so recursive commands can clean and build them from source. */
int bake_discover_dependency_sources(bake_context_t *ctx) {
    int32_t promoted = 0;
    bake_strlist_t attempted;
    bake_strlist_init(&attempted);

    int rc = -1;
    bool dirty = true;
    while (dirty) {
        dirty = false;

        /* Collect candidates first: promoting projects mutates the world,
         * which is not allowed while iterating it. */
        bake_strlist_t candidates;
        bake_strlist_init(&candidates);
        ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeProject));
        while (ecs_each_next(&it)) {
            const BakeProject *projects = ecs_field(&it, BakeProject, 0);
            for (int32_t i = 0; i < it.count; i++) {
                const BakeProject *project = &projects[i];
                if (!project->external || !project->cfg || !project->cfg->path) {
                    continue;
                }

                char *project_json = bake_path_join(project->cfg->path, "project.json");
                if (bake_path_exists(project_json) &&
                    !bake_strlist_contains(&attempted, project_json))
                {
                    bake_strlist_append(&candidates, project_json);
                }
                ecs_os_free(project_json);
            }
        }

        for (int32_t i = 0; i < candidates.count; i++) {
            const char *project_json = candidates.items[i];
            bake_strlist_append(&attempted, project_json);

            int add_rc = bake_discovery_add_project_file(ctx, project_json);
            if (add_rc < 0) {
                bake_strlist_fini(&candidates);
                goto cleanup;
            }
            if (add_rc > 0) {
                promoted++;
                dirty = true;
            }
        }
        bake_strlist_fini(&candidates);

        if (dirty && bake_discovery_resolve_dependencies(ctx) != 0) {
            goto cleanup;
        }
    }

    rc = promoted;
cleanup:
    bake_strlist_fini(&attempted);
    return rc;
}
