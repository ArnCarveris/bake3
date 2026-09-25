#ifndef BAKE_BUILD_INTERNAL_H
#define BAKE_BUILD_INTERNAL_H

#include "bake/build.h"

typedef struct bake_compile_unit_t {
    char *src;
    char *obj;
    char *dep;
    bool cpp;
    bool lint;
} bake_compile_unit_t;

typedef struct bake_compile_list_t {
    bake_compile_unit_t *items;
    int32_t count;
    int32_t capacity;
} bake_compile_list_t;

typedef struct bake_build_paths_t {
    char *build_root;
    char *obj_dir;
    char *bin_dir;
    char *lib_dir;
    char *gen_dir;
} bake_build_paths_t;


void bake_compile_list_init(bake_compile_list_t *list);
void bake_compile_list_fini(bake_compile_list_t *list);
int bake_compile_list_append(
    bake_compile_list_t *list,
    const char *src,
    const char *obj,
    const char *dep,
    bool cpp);

void bake_build_paths_fini(bake_build_paths_t *paths);
int bake_build_paths_init(const bake_project_cfg_t *cfg, const char *mode, bake_build_paths_t *paths);

int bake_collect_compile_units(
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths,
    bool include_tests,
    bool include_deps,
    bake_compiler_kind_t compiler_kind,
    bake_compile_list_t *units);
int bake_execute_rules(
    ecs_world_t *world,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths);
int bake_generate_config_header(ecs_world_t *world, const bake_project_cfg_t *cfg);
int bake_apply_dependee_cfg(
    ecs_world_t *world,
    ecs_entity_t project_entity,
    bake_lang_cfg_t *dst,
    bool cpp_lang);

/* Local web server for "run --target em". The requested port is the first one
 * tried; busy ports are skipped up to BAKE_EM_SERVE_PORT_SCAN entries later. */
#define BAKE_EM_SERVE_PORT_DEFAULT 8080
#define BAKE_EM_SERVE_PORT_SCAN 32
int32_t bake_em_serve_first_port(int32_t requested);

bake_compiler_kind_t bake_detect_compiler_kind(const char *cc, const char *cxx);
void bake_add_mode_flags(const char *mode, bake_compiler_kind_t kind, bake_strlist_t *cflags, bake_strlist_t *cxxflags, bake_strlist_t *ldflags);
void bake_add_strict_flags(bool strict, bake_compiler_kind_t kind, bake_strlist_t *cflags, bake_strlist_t *cxxflags, bake_strlist_t *ldflags);
void bake_add_coverage_flags(bool coverage, bake_strlist_t *cflags, bake_strlist_t *cxxflags, bake_strlist_t *ldflags);
void bake_list_append_fmt(ecs_strbuf_t *buf, const bake_strlist_t *list, const char *prefix);
char* bake_display_path(const char *full_path, const char *strip_prefix);

int bake_compile_units_parallel(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    const bake_compile_list_t *units,
    const bake_lang_cfg_t *lang,
    const bake_lang_cfg_t *cpp_lang,
    const bake_strlist_t *mode_cflags,
    const bake_strlist_t *mode_cxxflags,
    bool force_rebuild,
    int32_t *compiled_count_out);

int bake_link_project_binary(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths,
    const bake_compile_list_t *units,
    const bake_lang_cfg_t *lang,
    const bake_strlist_t *mode_ldflags,
    bool force_relink,
    bool standalone,
    char **artefact_out,
    bool *linked_out);

int bake_amalgamate_project(const bake_project_cfg_t *cfg, const char *dst_dir);
int bake_generate_project_amalgamation(const bake_project_cfg_t *cfg);

void bake_report_collect_loc(
    bake_context_t *ctx,
    const ecs_entity_t *order,
    int32_t count);

#endif
