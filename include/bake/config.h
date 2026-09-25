#ifndef BAKE3_CONFIG_H
#define BAKE3_CONFIG_H

#include "bake/strlist.h"
#include <flecs.h>

typedef enum bake_project_kind_t {
    BAKE_PROJECT_APPLICATION = 0,
    BAKE_PROJECT_PACKAGE,
    BAKE_PROJECT_CONFIG,
    BAKE_PROJECT_TEST,
    BAKE_PROJECT_BENCH,
    BAKE_PROJECT_TEMPLATE
} bake_project_kind_t;

typedef enum bake_compiler_kind_t {
    BAKE_COMPILER_GCC = 0,
    BAKE_COMPILER_CLANG,
    BAKE_COMPILER_MSVC,
    BAKE_COMPILER_UNKNOWN
} bake_compiler_kind_t;

typedef struct bake_rule_t {
    char *ext;
    char *command;
} bake_rule_t;

typedef struct bake_rule_list_t {
    ecs_vec_t vec;
} bake_rule_list_t;

typedef struct bake_bundle_t {
    char *id;
    char *repository;
    char *branch;
    char *tag;
    char *commit;
    char *subdir;
    char *library;
    char *build_system;
    char *profile;
    bool header_only;
    bake_strlist_t includes;
    bake_strlist_t sources;
    bake_strlist_t cmake_args;
    bake_strlist_t cargo_args;
    bake_strlist_t libs;
    bake_strlist_t ldflags;
} bake_bundle_t;

typedef struct bake_bundle_list_t {
    ecs_vec_t vec;
} bake_bundle_list_t;

typedef struct bake_amalgamate_cfg_t {
    char *path;
    char *prefix;
    bake_strlist_t disable_flags;
} bake_amalgamate_cfg_t;

typedef struct bake_amalgamate_list_t {
    ecs_vec_t vec;
} bake_amalgamate_list_t;

typedef enum bake_lint_action_t {
    BAKE_LINT_ERROR = 0,
    BAKE_LINT_LOG,
    BAKE_LINT_AUTOFIX
} bake_lint_action_t;

typedef struct bake_lint_cfg_t {
    char *command;
    bake_lint_action_t action;
} bake_lint_cfg_t;

typedef struct bake_project_cfg_t bake_project_cfg_t;

typedef struct bake_amalgamate_src_t {
    bake_project_cfg_t *project;
    char *prefix;
    char *dir;
    bool generate;
} bake_amalgamate_src_t;

typedef struct bake_dependee_cfg_t {
    bake_project_cfg_t *cfg;
    char *json;
} bake_dependee_cfg_t;

typedef struct bake_lang_cfg_t {
    bake_strlist_t cflags;
    bake_strlist_t cxxflags;
    bake_strlist_t defines;
    bake_strlist_t ldflags;
    bake_strlist_t libs;
    bake_strlist_t libpaths;
    bake_strlist_t include_paths;
    bake_strlist_t embed;
    char *c_standard;
    char *cpp_standard;
    char *shell;
} bake_lang_cfg_t;

struct bake_project_cfg_t {
    char *id;
    char *path;
    bake_project_kind_t kind;
    bool has_test_spec;
    bool has_bench_spec;
    bool public_project;
    char *language;
    char *output_name;

    bool private_project;
    bool standalone;
    bool shared_library;
    char *build_dir;
    bake_amalgamate_list_t amalgamate;
    bake_amalgamate_src_t *amalgamate_src;

    bake_strlist_t use;
    bake_strlist_t use_private;

    bake_dependee_cfg_t dependee;
    bake_lang_cfg_t c_lang;
    bake_lang_cfg_t cpp_lang;

    bake_rule_list_t rules;
    bake_bundle_list_t bundles;
    bake_lint_cfg_t lint;

    bake_strlist_t bundle_includes;
    bake_strlist_t bundle_libpaths;
    bake_strlist_t bundle_libs;
    bake_strlist_t bundle_ldflags;
    bake_strlist_t bundle_sources;
};

const char* bake_project_kind_str(bake_project_kind_t kind);
bool bake_project_kind_has_artefact(bake_project_kind_t kind);
bool bake_project_kind_is_harness(bake_project_kind_t kind);
bake_project_kind_t bake_project_kind_parse(const char *value);
char* bake_project_cfg_artefact_name(const bake_project_cfg_t *cfg);
const char* bake_project_cfg_shell(const bake_project_cfg_t *cfg);
char* bake_project_run_dir(const bake_project_cfg_t *cfg);

void bake_rule_list_init(bake_rule_list_t *list);
void bake_rule_list_fini(bake_rule_list_t *list);
int bake_rule_list_append(bake_rule_list_t *list, const char *ext, const char *command);

void bake_bundle_list_init(bake_bundle_list_t *list);
void bake_bundle_list_fini(bake_bundle_list_t *list);
bake_bundle_t* bake_bundle_list_append(bake_bundle_list_t *list);
int32_t bake_bundle_list_count(const bake_bundle_list_t *list);
bake_bundle_t* bake_bundle_list_get(const bake_bundle_list_t *list, int32_t index);
const bake_bundle_t* bake_bundle_list_find(const bake_bundle_list_t *list, const char *id);

void bake_amalgamate_list_init(bake_amalgamate_list_t *list);
void bake_amalgamate_list_fini(bake_amalgamate_list_t *list);
bake_amalgamate_cfg_t* bake_amalgamate_list_append(bake_amalgamate_list_t *list);
int32_t bake_amalgamate_list_count(const bake_amalgamate_list_t *list);
bake_amalgamate_cfg_t* bake_amalgamate_list_get(const bake_amalgamate_list_t *list, int32_t index);

void bake_lang_cfg_init(bake_lang_cfg_t *cfg);
void bake_lang_cfg_copy(bake_lang_cfg_t *dst, const bake_lang_cfg_t *src);
void bake_lang_cfg_fini(bake_lang_cfg_t *cfg);

void bake_dependee_cfg_init(bake_dependee_cfg_t *cfg);
void bake_dependee_cfg_fini(bake_dependee_cfg_t *cfg);

void bake_project_cfg_init(bake_project_cfg_t *cfg);
void bake_project_cfg_fini(bake_project_cfg_t *cfg);
int bake_project_cfg_load_file(const char *project_json_path, bake_project_cfg_t *cfg);
void bake_project_cfg_set_eval_context(const char *mode, const char *target);

const char* bake_lint_action_str(bake_lint_action_t action);

bool bake_language_is_cpp(const bake_project_cfg_t *cfg);

#endif
