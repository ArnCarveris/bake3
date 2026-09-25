#ifndef BAKE3_HARNESS_INTERNAL_H
#define BAKE3_HARNESS_INTERNAL_H

#include "bake/test_harness.h"

typedef struct bake_param_spec_t {
    char *name;
    bake_strlist_t values;
} bake_param_spec_t;

typedef struct bake_suite_spec_t {
    char *id;
    bool setup;
    bool teardown;
    double timeout;
    bake_strlist_t testcases;
    bake_param_spec_t *params;
    int32_t param_count;
    int32_t param_capacity;
} bake_suite_spec_t;

typedef struct bake_suite_list_t {
    bake_suite_spec_t *items;
    int32_t count;
    int32_t capacity;
} bake_suite_list_t;

void bake_suite_list_fini(bake_suite_list_t *list);

void bake_append_c_literal(ecs_strbuf_t *out, const char *value);

int bake_parse_project_tests(const char *path, bake_suite_list_t *out);

int bake_generate_suite_file(
    const bake_project_cfg_t *cfg,
    const bake_suite_spec_t *suite);

int bake_generate_main(
    const bake_project_cfg_t *cfg,
    const bake_suite_list_t *suites);

#endif
