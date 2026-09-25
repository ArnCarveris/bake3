#ifndef BAKE3_BUILD_H
#define BAKE3_BUILD_H

#include "bake/discovery.h"
#include "bake/build_components.h"

char* bake_project_build_root(const char *project_path, const char *project_id, const char *mode);

int bake_coverage_prepare(bake_context_t *ctx);
int bake_coverage_init_tools(bake_context_t *ctx);
char* bake_coverage_report_dir(void);
int bake_coverage_report_generate(bake_context_t *ctx, const char *target_path);
char* bake_coverage_dir(const bake_project_cfg_t *cfg, const char *mode);
char* bake_coverage_profile_pattern(const char *coverage_dir);
int bake_coverage_export_env(const bake_context_t *ctx, const bake_project_cfg_t *cfg);

int bake_build(bake_context_t *ctx);
int bake_build_clean(bake_context_t *ctx);
int bake_build_rebuild(bake_context_t *ctx);
int bake_build_run(bake_context_t *ctx);
int bake_build_coverage_report(bake_context_t *ctx);

#endif
