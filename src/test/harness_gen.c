#include "harness_internal.h"
#include "bake/os.h"
#include "common/harness_util.h"

void bake_append_c_literal(ecs_strbuf_t *out, const char *value) {
    ecs_strbuf_appendch(out, '"');
    for (const char *p = value; *p; p++) {
        if (*p == '"' || *p == '\\') {
            ecs_strbuf_appendch(out, '\\');
        }
        ecs_strbuf_appendch(out, *p);
    }
    ecs_strbuf_appendch(out, '"');
}

static char* bake_symbol_sanitize(const char *name) {
    if (!name || !name[0]) {
        return ecs_os_strdup("_");
    }

    size_t len = strlen(name);
    char *result = ecs_os_malloc(len + 2);

    size_t out = 0;
    if (name[0] >= '0' && name[0] <= '9') {
        result[out++] = '_';
    }

    for (size_t i = 0; i < len; i++) {
        char ch = name[i];
        if (!bake_harness_char_is_ident(ch)) {
            ch = '_';
        }
        result[out++] = ch;
    }

    result[out] = '\0';
    return result;
}

static void bake_append_empty_function(
    ecs_strbuf_t *out,
    const char *suite,
    const char *suffix,
    const char *existing,
    bool *appended)
{
    bake_harness_append_separator(out, existing, appended);
    ecs_strbuf_append(out,
        "void %s_%s(void) {\n"
        "}\n\n",
        suite,
        suffix);
}

int bake_generate_suite_file(const bake_project_cfg_t *cfg, const bake_suite_spec_t *suite) {
    char *suite_file = bake_harness_source_path(cfg, suite->id);

    char *existing = NULL;
    if (bake_path_exists(suite_file)) {
        existing = bake_file_read(suite_file, NULL);
    }

    ecs_strbuf_t out = ECS_STRBUF_INIT;
    if (existing) {
        ecs_strbuf_appendstr(&out, existing);
    }

    bool appended = false;

    if (suite->setup && !bake_harness_suite_has_function(existing, suite->id, "setup")) {
        bake_append_empty_function(&out, suite->id, "setup", existing, &appended);
    }

    if (suite->teardown && !bake_harness_suite_has_function(existing, suite->id, "teardown")) {
        bake_append_empty_function(&out, suite->id, "teardown", existing, &appended);
    }

    for (int32_t i = 0; i < suite->testcases.count; i++) {
        const char *testcase = suite->testcases.items[i];
        if (bake_harness_suite_has_function(existing, suite->id, testcase)) {
            continue;
        }

        bake_append_empty_function(&out, suite->id, testcase, existing, &appended);
    }

    int rc = 0;
    if (appended) {
        char *content = ecs_strbuf_get(&out);
        rc = bake_file_write(suite_file, content);
        ecs_os_free(content);
    } else {
        ecs_strbuf_reset(&out);
    }

    ecs_os_free(existing);
    ecs_os_free(suite_file);
    return rc;
}

static void bake_generate_main_suite_params(
    ecs_strbuf_t *out,
    const bake_suite_spec_t *suite)
{
    if (!suite->param_count) {
        return;
    }

    char **value_names = ecs_os_calloc_n(char*, suite->param_count);

    for (int32_t p = 0; p < suite->param_count; p++) {
        const bake_param_spec_t *param = &suite->params[p];
        char *param_symbol = bake_symbol_sanitize(param->name);
        value_names[p] = flecs_asprintf("%s_%s_param", suite->id, param_symbol);
        ecs_os_free(param_symbol);

        ecs_strbuf_append(out, "const char* %s[] = {", value_names[p]);
        for (int32_t v = 0; v < param->values.count; v++) {
            if (v) {
                ecs_strbuf_appendstr(out, ", ");
            }
            bake_append_c_literal(out, param->values.items[v]);
        }
        ecs_strbuf_appendstr(out, "};\n");
    }

    ecs_strbuf_append(out, "bake_test_param %s_params[] = {\n", suite->id);
    for (int32_t p = 0; p < suite->param_count; p++) {
        const bake_param_spec_t *param = &suite->params[p];
        ecs_strbuf_appendstr(out, "    {");
        bake_append_c_literal(out, param->name);
        ecs_strbuf_append(out,
            ", (char**)%s, %d}%s\n",
            value_names[p],
            param->values.count,
            (p + 1) < suite->param_count ? "," : "");
    }
    ecs_strbuf_appendstr(out, "};\n\n");

    for (int32_t p = 0; p < suite->param_count; p++) {
        ecs_os_free(value_names[p]);
    }
    ecs_os_free(value_names);
}

static void bake_generate_main_suite_decls(
    ecs_strbuf_t *out,
    const bake_suite_spec_t *suite)
{
    ecs_strbuf_append(out, "// Testsuite '%s'\n", suite->id);

    if (suite->setup) {
        ecs_strbuf_append(out, "void %s_setup(void);\n", suite->id);
    }
    if (suite->teardown) {
        ecs_strbuf_append(out, "void %s_teardown(void);\n", suite->id);
    }

    for (int32_t t = 0; t < suite->testcases.count; t++) {
        ecs_strbuf_append(out, "void %s_%s(void);\n", suite->id, suite->testcases.items[t]);
    }

    ecs_strbuf_appendstr(out, "\n");
}

int bake_generate_main(const bake_project_cfg_t *cfg, const bake_suite_list_t *suites) {
    char *main_path = bake_harness_source_path(cfg, "main");
    char *project_header = bake_harness_project_header(cfg);
    const char *header_include = project_header ? project_header : "bake_test.h";

    ecs_strbuf_t out = ECS_STRBUF_INIT;

    ecs_strbuf_appendstr(&out,
        "\n"
        "/* A friendly warning from bake.test\n"
        " * ----------------------------------------------------------------------------\n"
        " * This file is generated. To add/remove testcases modify the 'project.json' of\n"
        " * the test project. ANY CHANGE TO THIS FILE IS LOST AFTER (RE)BUILDING!\n"
        " * ----------------------------------------------------------------------------\n"
        " */\n\n");
    ecs_strbuf_append(&out, "#include <%s>\n\n", header_include);

    for (int32_t i = 0; i < suites->count; i++) {
        bake_generate_main_suite_decls(&out, &suites->items[i]);
    }

    for (int32_t i = 0; i < suites->count; i++) {
        const bake_suite_spec_t *suite = &suites->items[i];

        ecs_strbuf_append(&out, "bake_test_case %s_testcases[] = {\n", suite->id);
        for (int32_t t = 0; t < suite->testcases.count; t++) {
            const char *testcase = suite->testcases.items[t];
            ecs_strbuf_append(&out,
                "    {\n"
                "        \"%s\",\n"
                "        %s_%s\n"
                "    }%s\n",
                testcase,
                suite->id,
                testcase,
                (t + 1) < suite->testcases.count ? "," : "");
        }
        ecs_strbuf_appendstr(&out, "};\n\n");
    }

    for (int32_t i = 0; i < suites->count; i++) {
        bake_generate_main_suite_params(&out, &suites->items[i]);
    }

    ecs_strbuf_appendstr(&out, "static bake_test_suite suites[] = {\n");
    for (int32_t i = 0; i < suites->count; i++) {
        const bake_suite_spec_t *suite = &suites->items[i];

        char *setup_name = suite->setup ? flecs_asprintf("%s_setup", suite->id) : NULL;
        char *teardown_name = suite->teardown ? flecs_asprintf("%s_teardown", suite->id) : NULL;

        ecs_strbuf_append(&out,
            "    {\n"
            "        \"%s\",\n"
            "        %s,\n"
            "        %s,\n"
            "        %d,\n"
            "        %s_testcases",
            suite->id,
            setup_name ? setup_name : "NULL",
            teardown_name ? teardown_name : "NULL",
            suite->testcases.count,
            suite->id);

        if (suite->param_count) {
            ecs_strbuf_append(&out,
                ",\n"
                "        %d,\n"
                "        %s_params",
                suite->param_count,
                suite->id);
        } else if (suite->timeout > 0) {
            ecs_strbuf_appendstr(&out,
                ",\n"
                "        0,\n"
                "        NULL");
        }

        if (suite->timeout > 0) {
            ecs_strbuf_append(&out, ",\n        %g", suite->timeout);
        }

        ecs_strbuf_append(&out, "\n    }%s\n",
            (i + 1) < suites->count ? "," : "");

        ecs_os_free(setup_name);
        ecs_os_free(teardown_name);
    }
    ecs_strbuf_appendstr(&out, "};\n\n");

    ecs_strbuf_append(&out,
        "int main(int argc, char *argv[]) {\n"
        "    return bake_test_run(\"%s\", argc, argv, suites, %d);\n"
        "}\n",
        cfg->id,
        suites->count);

    char *content = ecs_strbuf_get(&out);
    int rc = bake_file_write(main_path, content);

    ecs_os_free(content);
    ecs_os_free(project_header);
    ecs_os_free(main_path);
    return rc;
}
