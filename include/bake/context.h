#ifndef BAKE3_CONTEXT_H
#define BAKE3_CONTEXT_H

#include "bake/config.h"

typedef struct bake_build_report_t bake_build_report_t;

typedef struct bake_options_t {
    const char *command;
    const char *target;
    const char *bundle_action;
    const char *bundle_name;
    const char *toolchain;
    const char *mode;
    const char *cwd;
    const char *cc;
    const char *cxx;
    const char *run_prefix;
    const char *ps_kill;
    const char *build_json;
    bool json;
    bool all_users;
    bool ps_full;
    bool recursive;
    bool standalone;
    bool strict;
    bool coverage;
    bool fix_lint;
    bool trace;
    bool setup_local;
    bool local_env;
    int32_t jobs;
    int32_t port;
    int run_argc;
    const char **run_argv;
} bake_options_t;

typedef struct bake_context_t {
    ecs_world_t *world;
    bake_options_t opts;
    char *bake_home;
    bake_build_report_t *report;
    bake_compiler_kind_t compiler_kind;
    char *coverage_profdata;
    char *coverage_cov;
    bool prepare_bundles;
    bool discover_tests;
    int32_t thread_count;
} bake_context_t;

const char* bake_effective_mode(const char *mode);

int bake_context_init(bake_context_t *ctx, const bake_options_t *opts);
void bake_context_fini(bake_context_t *ctx);

#endif
