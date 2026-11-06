#include "bake/os.h"
#include "bake/ps.h"
#include "build/build_internal.h"
#include "bake/build.h"
#include "bake/bundle.h"
#include "bake/environment.h"
#include "bake/strlist.h"
#include "bake/common.h"
#include "common/strutil.h"

#include <flecs.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void check(bool cond, const char *expr, const char *file, int line) {
    checks ++;
    if (!cond) {
        failures ++;
        printf("FAIL %s:%d: %s\n", file, line, expr);
    }
}

static void check_str(
    const char *got,
    const char *expect,
    const char *expr,
    const char *file,
    int line)
{
    checks ++;
    if (!got || !expect || strcmp(got, expect)) {
        failures ++;
        printf("FAIL %s:%d: %s\n  got:    %s\n  expect: %s\n",
            file, line, expr, got ? got : "(null)", expect);
    }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_STR(got, expect) check_str(got, expect, #got, __FILE__, __LINE__)

static void test_trim(void) {
    char buf[] = "  \t value \n ";
    char *trimmed = bake_ltrim(buf);
    bake_rtrim(trimmed);
    CHECK_STR(trimmed, "value");

    char only_space[] = "   ";
    char *empty = bake_ltrim(only_space);
    bake_rtrim(empty);
    CHECK_STR(empty, "");

    char none[] = "value";
    CHECK_STR(bake_ltrim(none), "value");

    bake_rtrim(NULL);
    CHECK(bake_ltrim(NULL) == NULL);
}

static void test_errno_message(void) {
    char buf[256];
    CHECK_STR(bake_errno_message(EACCES, buf, sizeof(buf)), strerror(EACCES));
    CHECK_STR(bake_errno_message(ENOENT, buf, sizeof(buf)), strerror(ENOENT));
    CHECK(bake_errno_message(EACCES, buf, sizeof(buf)) == buf);
}

static void test_path_join(void) {
    char sep = bake_path_sep();
    char expect[64];

    char *joined = bake_path_join("a", "b");
    snprintf(expect, sizeof(expect), "a%cb", sep);
    CHECK_STR(joined, expect);
    ecs_os_free(joined);

    joined = bake_path_join("a/", "b");
    CHECK_STR(joined, "a/b");
    ecs_os_free(joined);

    joined = bake_path_join("", "b");
    CHECK_STR(joined, "b");
    ecs_os_free(joined);

    joined = bake_path_join("a", "");
    CHECK_STR(joined, "a");
    ecs_os_free(joined);

    joined = bake_path_join3("a", "b", "c");
    snprintf(expect, sizeof(expect), "a%cb%cc", sep, sep);
    CHECK_STR(joined, expect);
    ecs_os_free(joined);
}

static void test_path_compare(void) {
    CHECK(bake_path_equal_normalized("a/b", "a\\b"));
    CHECK(bake_path_equal_normalized("a/b/", "a/b"));
    CHECK(bake_path_equal_normalized("a/b///", "a/b"));
    CHECK(!bake_path_equal_normalized("a/b", "a/c"));
    CHECK(!bake_path_equal_normalized("a/b", "a/bc"));
    CHECK(!bake_path_equal_normalized(NULL, "a"));

    size_t prefix_len = 0;
    CHECK(bake_path_has_prefix_normalized("a/b/c", "a/b", &prefix_len));
    CHECK(prefix_len == 3);
    CHECK(bake_path_has_prefix_normalized("a/b", "a/b/", NULL));
    CHECK(!bake_path_has_prefix_normalized("a/bc", "a/b", NULL));
    CHECK(!bake_path_has_prefix_normalized("a/b", "a/b/c", NULL));
    CHECK(!bake_path_has_prefix_normalized("a/b", "", NULL));
}

static void test_path_is_abs(void) {
#if defined(_WIN32)
    CHECK(bake_path_is_abs("C:\\x"));
    CHECK(bake_path_is_abs("C:/x"));
#endif
    CHECK(bake_path_is_abs("/x"));
    CHECK(!bake_path_is_abs("x"));
    CHECK(!bake_path_is_abs("./x"));
    CHECK(!bake_path_is_abs(""));
}

static void test_project_id_as_macro(void) {
    char *macro = bake_project_id_as_macro("my.project-name");
    CHECK_STR(macro, "my_project_name");
    ecs_os_free(macro);

    macro = bake_project_id_as_macro("Mixed.Case9");
    CHECK_STR(macro, "mixed_case9");
    ecs_os_free(macro);

    CHECK(bake_project_id_as_macro(NULL) == NULL);
}

static void test_shell_quote(void) {
    char *quoted = bake_shell_quote_arg("plain");
    CHECK_STR(quoted, "\"plain\"");
    ecs_os_free(quoted);

    quoted = bake_shell_quote_arg("with space");
    CHECK_STR(quoted, "\"with space\"");
    ecs_os_free(quoted);

    quoted = bake_shell_quote_arg("has\"quote");
    CHECK_STR(quoted, "\"has\\\"quote\"");
    ecs_os_free(quoted);

    quoted = bake_shell_quote_arg("back\\slash");
    CHECK_STR(quoted, "\"back\\\\slash\"");
    ecs_os_free(quoted);

    quoted = bake_shell_quote_arg(NULL);
    CHECK_STR(quoted, "\"\"");
    ecs_os_free(quoted);
}

static void test_strlist(void) {
    bake_strlist_t list;
    bake_strlist_init(&list);

    CHECK(bake_strlist_append(&list, "a") == 0);
    CHECK(bake_strlist_append(&list, "b") == 0);
    CHECK(list.count == 2);
    CHECK(bake_strlist_contains(&list, "a"));
    CHECK(!bake_strlist_contains(&list, "c"));

    CHECK(bake_strlist_append_unique(&list, "a") == 0);
    CHECK(list.count == 2);
    CHECK(bake_strlist_append_unique(&list, "c") == 0);
    CHECK(list.count == 3);

    char *joined = bake_strlist_join(&list, ",");
    CHECK_STR(joined, "a,b,c");
    ecs_os_free(joined);

    bake_strlist_t copy;
    bake_strlist_init(&copy);
    CHECK(bake_strlist_copy(&copy, &list) == 0);
    CHECK(copy.count == 3);
    CHECK(copy.items[0] != list.items[0]);
    CHECK_STR(copy.items[0], "a");

    bake_strlist_t merged;
    bake_strlist_init(&merged);
    CHECK(bake_strlist_append(&merged, "b") == 0);
    CHECK(bake_strlist_merge_unique(&merged, &list) == 0);
    CHECK(merged.count == 3);
    CHECK_STR(merged.items[0], "b");

    bake_strlist_fini(&merged);
    bake_strlist_fini(&copy);
    bake_strlist_fini(&list);
}

static void test_bundle_cargo_native_paths(void) {
    bake_set_build_target(NULL);

    char *command = bake_bundle_cargo_command(
        NULL, "source dir", "build dir", "debug");
    char *manifest = bake_path_join("source dir", "Cargo.toml");
    char *expected = flecs_asprintf(
        "cargo build --release --manifest-path \"%s\" --target-dir \"build dir\"",
        manifest);
    CHECK_STR(command, expected);
    ecs_os_free(expected);
    ecs_os_free(manifest);
    ecs_os_free(command);

    char *profile = bake_bundle_cargo_profile_dir("debug");
    CHECK_STR(profile, "release");
    ecs_os_free(profile);
    profile = bake_bundle_cargo_profile_dir("sanitize");
    CHECK_STR(profile, "debug");
    ecs_os_free(profile);
}

static void test_bundle_cargo_emscripten_paths(void) {
    bake_set_build_target("em");

    char *command = bake_bundle_cargo_command(
        NULL, "source dir", "build dir", "profile");
    char *manifest = bake_path_join("source dir", "Cargo.toml");
    char *expected_command = flecs_asprintf(
        "cargo rustc --release --target wasm32-unknown-emscripten "
        "--crate-type staticlib "
        "--manifest-path \"%s\" --target-dir \"build dir\"",
        manifest);
    CHECK_STR(command, expected_command);
    ecs_os_free(expected_command);
    ecs_os_free(manifest);
    ecs_os_free(command);

    char *profile = bake_bundle_cargo_profile_dir("profile");
    char *expected_profile = bake_path_join(
        "wasm32-unknown-emscripten", "release");
    CHECK_STR(profile, expected_profile);
    ecs_os_free(expected_profile);
    ecs_os_free(profile);

    bake_set_build_target(NULL);
}

static void test_bundle_cargo_args(void) {
    bake_set_build_target(NULL);

    bake_bundle_t bundle = {0};
    bake_strlist_init(&bundle.cargo_args);
    bake_strlist_append(&bundle.cargo_args, "--no-default-features");
    bake_strlist_append(&bundle.cargo_args, "--features=raster-images");

    char *command = bake_bundle_cargo_command(
        &bundle, "source dir", "build dir", "debug");
    char *manifest = bake_path_join("source dir", "Cargo.toml");
    char *expected = flecs_asprintf(
        "cargo build --release --manifest-path \"%s\" --target-dir \"build dir\" "
        "\"--no-default-features\" \"--features=raster-images\"",
        manifest);
    CHECK_STR(command, expected);
    ecs_os_free(expected);
    ecs_os_free(manifest);
    ecs_os_free(command);

    bake_strlist_fini(&bundle.cargo_args);
}

static bool strlist_has(const bake_strlist_t *list, const char *value) {
    for (int32_t i = 0; i < list->count; i++) {
        if (!strcmp(list->items[i], value)) {
            return true;
        }
    }
    return false;
}

static void mode_flags(
    const char *mode,
    bake_strlist_t *cflags,
    bake_strlist_t *ldflags)
{
    bake_strlist_t cxxflags;
    bake_strlist_init(cflags);
    bake_strlist_init(&cxxflags);
    bake_strlist_init(ldflags);
    bake_add_mode_flags(mode, BAKE_COMPILER_CLANG, cflags, &cxxflags, ldflags);
    bake_strlist_fini(&cxxflags);
}

static void test_mode_flags_native(void) {
    bake_set_build_target(NULL);

    bake_strlist_t cflags, ldflags;

    mode_flags("release", &cflags, &ldflags);
    CHECK(strlist_has(&cflags, "-flto=thin"));
    CHECK(strlist_has(&ldflags, "-flto=thin"));
    CHECK(!strlist_has(&ldflags, "-flto"));
    CHECK(!strlist_has(&ldflags, "-O3"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    bake_strlist_t cxxflags;
    bake_strlist_init(&cflags);
    bake_strlist_init(&cxxflags);
    bake_strlist_init(&ldflags);
    bake_add_mode_flags("release", BAKE_COMPILER_GCC, &cflags, &cxxflags, &ldflags);
    CHECK(strlist_has(&cflags, "-flto"));
    CHECK(strlist_has(&ldflags, "-flto"));
    CHECK(!strlist_has(&ldflags, "-flto=thin"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&cxxflags); bake_strlist_fini(&ldflags);

    mode_flags("debug", &cflags, &ldflags);
    CHECK(ldflags.count == 0);
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    mode_flags("profile", &cflags, &ldflags);
    CHECK(strlist_has(&cflags, "-pg"));
    CHECK(strlist_has(&ldflags, "-pg"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);
}

static void test_mode_flags_emscripten(void) {
    bake_set_build_target("em");

    bake_strlist_t cflags, ldflags;

    mode_flags("release", &cflags, &ldflags);
    CHECK(strlist_has(&ldflags, "-O3"));
    CHECK(strlist_has(&ldflags, "-flto"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    mode_flags("debug", &cflags, &ldflags);
    CHECK(strlist_has(&ldflags, "-O0"));
    CHECK(strlist_has(&ldflags, "-g"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    mode_flags("profile", &cflags, &ldflags);
    CHECK(strlist_has(&cflags, "-O2"));
    CHECK(!strlist_has(&cflags, "-pg"));
    CHECK(strlist_has(&ldflags, "-O2"));
    CHECK(!strlist_has(&ldflags, "-pg"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    mode_flags("sanitize", &cflags, &ldflags);
    CHECK(strlist_has(&ldflags, "-O0"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    bake_set_build_target(NULL);
}

static void test_em_serve_first_port(void) {
    CHECK(bake_em_serve_first_port(0) == BAKE_EM_SERVE_PORT_DEFAULT);
    CHECK(bake_em_serve_first_port(-1) == BAKE_EM_SERVE_PORT_DEFAULT);
    CHECK(bake_em_serve_first_port(65536) == BAKE_EM_SERVE_PORT_DEFAULT);
    CHECK(bake_em_serve_first_port(1) == 1);
    CHECK(bake_em_serve_first_port(8123) == 8123);
    CHECK(bake_em_serve_first_port(65535) == 65535);
    CHECK(BAKE_EM_SERVE_PORT_SCAN > 0);
}

static void test_local_env_name_validation(void) {
    CHECK(bake_local_env_name_chars_valid("agent1"));
    CHECK(bake_local_env_name_chars_valid("Agent-1.2_x"));
    CHECK(!bake_local_env_name_chars_valid(NULL));
    CHECK(!bake_local_env_name_chars_valid(""));
    CHECK(!bake_local_env_name_chars_valid("."));
    CHECK(!bake_local_env_name_chars_valid(".."));
    CHECK(!bake_local_env_name_chars_valid("a/b"));
    CHECK(!bake_local_env_name_chars_valid("a\\b"));
    CHECK(!bake_local_env_name_chars_valid("a b"));
    CHECK(!bake_local_env_name_chars_valid("a:b"));

    CHECK(bake_local_env_name_reserved("build"));
    CHECK(bake_local_env_name_reserved("meta"));
    CHECK(bake_local_env_name_reserved("include"));
    CHECK(bake_local_env_name_reserved("test"));
    CHECK(bake_local_env_name_reserved("bin"));
    CHECK(bake_local_env_name_reserved("lib"));
    CHECK(bake_local_env_name_reserved("arm64-Darwin"));
    CHECK(bake_local_env_name_reserved("x64-Linux"));
    CHECK(bake_local_env_name_reserved("wasm32-Emscripten"));
    CHECK(!bake_local_env_name_reserved("agent1"));
    CHECK(!bake_local_env_name_reserved("builder"));

    CHECK(bake_local_env_name_valid("agent1"));
    CHECK(!bake_local_env_name_valid("build"));
    CHECK(!bake_local_env_name_valid("a b"));
}

static void test_local_env_home(void) {
    char *unnamed = bake_local_env_home("/ws", NULL);
    char *expect_unnamed = bake_path_join3("/ws", ".bake", "local_env");
    CHECK_STR(unnamed, expect_unnamed);

    char *named = bake_local_env_home("/ws", "agent1");
    char *expect_named = bake_path_join(expect_unnamed, "agent1");
    CHECK_STR(named, expect_named);

    CHECK(bake_local_env_home("/ws", "build") == NULL);
    CHECK(bake_local_env_home("/ws", "a/b") == NULL);
    CHECK(bake_local_env_home("/ws", "") == NULL);
    CHECK(bake_local_env_home(NULL, "agent1") == NULL);

    ecs_os_free(unnamed);
    ecs_os_free(named);
    ecs_os_free(expect_unnamed);
    ecs_os_free(expect_named);
}

static void test_local_env_build_root(void) {
    bake_set_build_target(NULL);

    char *home = bake_local_env_home("/ws", "agent1");
    bake_os_setenv("BAKE_HOME", home);
    bake_os_setenv("BAKE_LOCAL_ENV", "1");

    char *triplet = bake_host_triplet("debug");
    char *build_dir = bake_path_join(home, "build");
    char *id_dir = bake_path_join(build_dir, "my_app");
    char *expect_root = bake_path_join(id_dir, triplet);

    char *root = bake_project_build_root("/ws/projects/app", "my_app", "debug");
    CHECK_STR(root, expect_root);
    ecs_os_free(root);

    char *other_home = bake_local_env_home("/ws", "agent2");
    bake_os_setenv("BAKE_HOME", other_home);
    char *other_root = bake_project_build_root("/ws/projects/app", "my_app", "debug");
    CHECK(other_root && strcmp(other_root, expect_root) != 0);
    ecs_os_free(other_root);

    bake_os_setenv("BAKE_LOCAL_ENV", "0");
    char *project_root = bake_project_build_root("/ws/projects/app", "my_app", "debug");
    char *expect_project = bake_path_join3("/ws/projects/app", ".bake", triplet);
    CHECK_STR(project_root, expect_project);
    ecs_os_free(project_root);
    ecs_os_free(expect_project);

    bake_os_unsetenv("BAKE_HOME");
    bake_os_unsetenv("BAKE_LOCAL_ENV");

    ecs_os_free(home);
    ecs_os_free(other_home);
    ecs_os_free(triplet);
    ecs_os_free(build_dir);
    ecs_os_free(id_dir);
    ecs_os_free(expect_root);
}

static char* test_expr_resolve(const char *kind, const char *arg, void *ctx) {
    BAKE_UNUSED(ctx);
    if (!strcmp(kind, "fail")) {
        return NULL;
    }
    return flecs_asprintf("<%s|%s>", kind, arg ? arg : "");
}

static void test_expr_split(void) {
    char *kind = NULL;
    char *arg = NULL;
    const char *expr = "  os   Darwin  ";
    CHECK(bake_expr_split(expr, strlen(expr), &kind, &arg));
    CHECK_STR(kind, "os");
    CHECK_STR(arg, "Darwin");
    ecs_os_free(kind);
    ecs_os_free(arg);

    CHECK(bake_expr_split("id", 2, &kind, &arg));
    CHECK_STR(kind, "id");
    CHECK(arg == NULL);
    ecs_os_free(kind);

    CHECK(bake_expr_split("id underscore}", 13, &kind, &arg));
    CHECK_STR(kind, "id");
    CHECK_STR(arg, "underscore");
    ecs_os_free(kind);
    ecs_os_free(arg);

    CHECK(!bake_expr_split("   ", 3, &kind, &arg));
    CHECK(kind == NULL);
    CHECK(arg == NULL);
}

static void test_expr_expand(void) {
    char *out = bake_expr_expand("/a/${id}/b/${env HOME}x", test_expr_resolve, NULL);
    CHECK_STR(out, "/a/<id|>/b/<env|HOME>x");
    ecs_os_free(out);

    out = bake_expr_expand("plain/path", test_expr_resolve, NULL);
    CHECK_STR(out, "plain/path");
    ecs_os_free(out);

    out = bake_expr_expand("", test_expr_resolve, NULL);
    CHECK_STR(out, "");
    ecs_os_free(out);

    CHECK(bake_expr_expand("/a/${id", test_expr_resolve, NULL) == NULL);
    CHECK(bake_expr_expand("/a/${ }", test_expr_resolve, NULL) == NULL);
    CHECK(bake_expr_expand("/a/${fail}", test_expr_resolve, NULL) == NULL);
    CHECK(bake_expr_expand(NULL, test_expr_resolve, NULL) == NULL);
}

static void test_settings_expand_output_dir(void) {
    bool used_id = false;
    char *out = bake_settings_expand_output_dir(
        "/tmp/bake3/${id underscore}", NULL, "foo.bar", &used_id);
    CHECK_STR(out, "/tmp/bake3/foo_bar");
    CHECK(used_id);
    ecs_os_free(out);

    out = bake_settings_expand_output_dir(
        "/tmp/bake3/${id underscore}", NULL, "box", NULL);
    CHECK_STR(out, "/tmp/bake3/box");
    ecs_os_free(out);

    out = bake_settings_expand_output_dir(
        "/tmp/bake3/${id}", NULL, "foo.bar", NULL);
    CHECK_STR(out, "/tmp/bake3/foo.bar");
    ecs_os_free(out);

    out = bake_settings_expand_output_dir(
        "/tmp/bake3/fixed", NULL, "foo.bar", &used_id);
    CHECK_STR(out, "/tmp/bake3/fixed");
    CHECK(!used_id);
    ecs_os_free(out);

    out = bake_settings_expand_output_dir(
        "out/${id underscore}", "/home/me/bake3", "a.b.c", NULL);
    char *expect = bake_path_join("/home/me/bake3", "out/a_b_c");
    CHECK_STR(out, expect);
    ecs_os_free(expect);
    ecs_os_free(out);

    bake_os_setenv("BAKE_UNIT_OUTPUT_ROOT", "/scratch/out");
    out = bake_settings_expand_output_dir(
        "${env BAKE_UNIT_OUTPUT_ROOT}/${id underscore}", "/ignored", "x.y", NULL);
    CHECK_STR(out, "/scratch/out/x_y");
    ecs_os_free(out);
    bake_os_unsetenv("BAKE_UNIT_OUTPUT_ROOT");

    CHECK(bake_settings_expand_output_dir(
        "${env BAKE_UNIT_OUTPUT_ROOT}/${id}", NULL, "x", NULL) == NULL);

    char *home = bake_os_home_path();
    if (home) {
        out = bake_settings_expand_output_dir("~/out/${id}", NULL, "x", NULL);
        expect = bake_path_join(home, "out/x");
        CHECK_STR(out, expect);
        ecs_os_free(expect);
        ecs_os_free(out);
        ecs_os_free(home);
    }

    CHECK(bake_settings_expand_output_dir("/o/${id dash}", NULL, "x", NULL) == NULL);
    CHECK(bake_settings_expand_output_dir("/o/${os}/${id}", NULL, "x", NULL) == NULL);
    CHECK(bake_settings_expand_output_dir("/o/${id}", NULL, NULL, NULL) == NULL);
    CHECK(bake_settings_expand_output_dir(NULL, NULL, "x", NULL) == NULL);
}

static void test_settings_project_output_dir(void) {
    char *out = bake_project_output_dir("/ws/app", "my.app");
    char *expect = bake_path_join("/ws/app", ".bake");
    CHECK_STR(out, expect);
    ecs_os_free(out);
    ecs_os_free(expect);

    CHECK(bake_settings_set_output_dir("/o/fixed", NULL) != 0);
    CHECK(bake_settings_output_dir() == NULL);
    CHECK(bake_settings_set_output_dir("/o/${nope}", NULL) != 0);
    CHECK(bake_settings_output_dir() == NULL);

    CHECK(bake_settings_set_output_dir("/o/${id underscore}", NULL) == 0);
    CHECK_STR(bake_settings_output_dir(), "/o/${id underscore}");

    out = bake_project_output_dir("/ws/app", "my.app");
    CHECK_STR(out, "/o/my_app");
    ecs_os_free(out);

    out = bake_project_output_dir("/ws/unnamed", NULL);
    CHECK_STR(out, "/o/unnamed");
    ecs_os_free(out);

    CHECK(bake_project_output_dir(NULL, "my.app") == NULL);

    bake_os_setenv("BAKE_LOCAL_ENV", "0");
    char *triplet = bake_host_triplet("debug");
    char *root = bake_project_build_root("/ws/app", "my.app", "debug");
    expect = bake_path_join("/o/my_app", triplet);
    CHECK_STR(root, expect);
    ecs_os_free(root);
    ecs_os_free(expect);
    ecs_os_free(triplet);
    bake_os_unsetenv("BAKE_LOCAL_ENV");

    char *home = bake_local_env_home("/no/such/workspace", "agent1");
    expect = bake_path_join3("/o/workspace", "local_env", "agent1");
    CHECK_STR(home, expect);
    ecs_os_free(expect);
    ecs_os_free(home);

    CHECK(bake_settings_is_output_dir("/o/my_app"));
    CHECK(bake_settings_is_output_dir("/o//my_app/"));
    CHECK(!bake_settings_is_output_dir("/o"));
    CHECK(!bake_settings_is_output_dir("/o/my_app/sub"));
    CHECK(!bake_settings_is_output_dir("/other/my_app"));

    bake_ps_path_info_t info;
    CHECK(bake_ps_parse_local_env_path(
        "/o/my_app/local_env/agent_a/build/my.app/arm64-Darwin-debug/bin/my_app",
        &info) == 0);
    CHECK_STR(info.env, "agent_a");
    CHECK_STR(info.workspace, "/o/my_app");
    CHECK_STR(info.cfg, "debug");
    CHECK_STR(info.project, "my.app");
    bake_ps_path_info_fini(&info);

    CHECK(bake_ps_parse_local_env_path(
        "/other/my_app/local_env/agent_a/arm64-Darwin/debug/bin/my_app", &info) != 0);
    bake_ps_path_info_fini(&info);

    char *env = NULL;
    CHECK(bake_ps_env_from_home("/o/my_app/local_env/agent_b", &env) == BakePsEnvLocal);
    CHECK_STR(env, "agent_b");
    ecs_os_free(env);

    CHECK(bake_ps_env_from_home("/o/my_app/local_env", &env) == BakePsEnvLocal);
    CHECK(env == NULL);

    CHECK(bake_ps_env_from_home("/ws/.bake/local_env/agent_c", &env) == BakePsEnvLocal);
    CHECK_STR(env, "agent_c");
    ecs_os_free(env);

    bake_settings_fini();
    CHECK(bake_settings_output_dir() == NULL);
    CHECK(!bake_settings_is_output_dir("/o/my_app"));
}

static void test_settings_colors(void) {
    bake_os_unsetenv("NO_COLOR");
    CHECK(bake_settings_colors());
    CHECK(ecs_os_api.flags_ & EcsOsApiLogWithColors);

    bake_settings_set_colors(false);
    CHECK(!bake_settings_colors());
    CHECK(!(ecs_os_api.flags_ & EcsOsApiLogWithColors));
    CHECK_STR(getenv("NO_COLOR"), "1");

    bake_settings_fini();
    CHECK(bake_settings_colors());
    CHECK(ecs_os_api.flags_ & EcsOsApiLogWithColors);
    bake_os_unsetenv("NO_COLOR");
}

static void test_lock_excludes_second_holder(void) {
    char *dir = flecs_asprintf("bake_unit_lock_%lld", (long long)bake_os_pid());
    char *lock_path = bake_path_join(dir, "test.lock");

    bake_lock_t first = {0};
    CHECK(bake_os_lock_acquire(lock_path, 5, &first) == 0);
    CHECK(first.held);
    CHECK(bake_path_exists(lock_path));

    bake_lock_t second = {0};
    CHECK(bake_os_lock_acquire(lock_path, 1, &second) != 0);
    CHECK(!second.held);

    bake_os_lock_release(&first);
    CHECK(!bake_path_exists(lock_path));

    bake_lock_t third = {0};
    CHECK(bake_os_lock_acquire(lock_path, 5, &third) == 0);
    bake_os_lock_release(&third);

    CHECK(bake_os_pid() > 0);
    CHECK(bake_os_pid_alive(bake_os_pid()));
    CHECK(!bake_os_pid_alive(-1));

    bake_os_rmtree(dir);
    ecs_os_free(lock_path);
    ecs_os_free(dir);
}


static void test_ps_elapsed_formatting(void) {
    char buf[32];

    bake_ps_format_elapsed(0, buf, sizeof(buf));
    CHECK_STR(buf, "0:00:00");

    bake_ps_format_elapsed(59, buf, sizeof(buf));
    CHECK_STR(buf, "0:00:59");

    bake_ps_format_elapsed(61, buf, sizeof(buf));
    CHECK_STR(buf, "0:01:01");

    bake_ps_format_elapsed(3661, buf, sizeof(buf));
    CHECK_STR(buf, "1:01:01");

    bake_ps_format_elapsed(90061, buf, sizeof(buf));
    CHECK_STR(buf, "25:01:01");

    bake_ps_format_elapsed(-10, buf, sizeof(buf));
    CHECK_STR(buf, "0:00:00");

    CHECK(bake_ps_parse_etime("05") == 5);
    CHECK(bake_ps_parse_etime("01:05") == 65);
    CHECK(bake_ps_parse_etime("1:02:03") == 3723);
    CHECK(bake_ps_parse_etime("2-03:04:05") == 2 * 86400 + 3 * 3600 + 4 * 60 + 5);
    CHECK(bake_ps_parse_etime("") == 0);
    CHECK(bake_ps_parse_etime(NULL) == 0);
}

static void test_ps_parse_local_env_path(void) {
    bake_ps_path_info_t info;

    CHECK(bake_ps_parse_local_env_path(
        "/ws/.bake/local_env/agent_a/arm64-Darwin/debug/bin/night_shift", &info) == 0);
    CHECK_STR(info.env, "agent_a");
    CHECK_STR(info.workspace, "/ws");
    CHECK_STR(info.cfg, "debug");
    CHECK_STR(info.project, "night_shift");
    bake_ps_path_info_fini(&info);

    CHECK(bake_ps_parse_local_env_path(
        "/ws/.bake/local_env/arm64-Darwin/release/bin/night_shift", &info) == 0);
    CHECK(info.env == NULL);
    CHECK_STR(info.workspace, "/ws");
    CHECK_STR(info.cfg, "release");
    CHECK_STR(info.project, "night_shift");
    bake_ps_path_info_fini(&info);

    CHECK(bake_ps_parse_local_env_path(
        "/ws/.bake/local_env/agent_a/build/my.app/arm64-Darwin-debug/bin/my_app", &info) == 0);
    CHECK_STR(info.env, "agent_a");
    CHECK_STR(info.cfg, "debug");
    CHECK_STR(info.project, "my.app");
    bake_ps_path_info_fini(&info);

    CHECK(bake_ps_parse_local_env_path("/ws/.bake/arm64-Darwin-debug/bin/app", &info) != 0);
    bake_ps_path_info_fini(&info);

    char *env = NULL;
    CHECK(bake_ps_env_from_home("/ws/.bake/local_env/agent_b", &env) == BakePsEnvLocal);
    CHECK_STR(env, "agent_b");
    ecs_os_free(env);

    CHECK(bake_ps_env_from_home("/ws/.bake/local_env", &env) == BakePsEnvLocal);
    CHECK(env == NULL);

    CHECK(bake_ps_env_from_home("/Users/me/bake3", &env) == BakePsEnvGlobal);
    CHECK(env == NULL);

    CHECK(bake_ps_env_from_home(NULL, &env) == BakePsEnvUnknown);
    CHECK(env == NULL);

    CHECK(bake_ps_env_from_home("", &env) == BakePsEnvUnknown);
    CHECK(env == NULL);
}

static void test_ps_env_labels(void) {
    char *label = bake_ps_env_label(BakePsEnvLocal, "agent_a");
    CHECK_STR(label, "local:agent_a");
    ecs_os_free(label);

    label = bake_ps_env_label(BakePsEnvLocal, NULL);
    CHECK_STR(label, "local");
    ecs_os_free(label);

    label = bake_ps_env_label(BakePsEnvLocal, "");
    CHECK_STR(label, "local");
    ecs_os_free(label);

    label = bake_ps_env_label(BakePsEnvGlobal, NULL);
    CHECK_STR(label, "global");
    ecs_os_free(label);

    label = bake_ps_env_label(BakePsEnvGlobal, "ignored");
    CHECK_STR(label, "global");
    ecs_os_free(label);

    label = bake_ps_env_label(BakePsEnvUnknown, NULL);
    CHECK_STR(label, "?");
    ecs_os_free(label);

    label = bake_ps_env_label(BakePsEnvUnknown, "leftover");
    CHECK_STR(label, "leftover");
    ecs_os_free(label);

    CHECK_STR(bake_ps_env_kind_str(BakePsEnvLocal), "local");
    CHECK_STR(bake_ps_env_kind_str(BakePsEnvGlobal), "global");
    CHECK_STR(bake_ps_env_kind_str(BakePsEnvUnknown), "unknown");

    CHECK(bake_ps_env_kind_from_str("local") == BakePsEnvLocal);
    CHECK(bake_ps_env_kind_from_str("global") == BakePsEnvGlobal);
    CHECK(bake_ps_env_kind_from_str("unknown") == BakePsEnvUnknown);
    CHECK(bake_ps_env_kind_from_str("agent_a") == BakePsEnvUnknown);
    CHECK(bake_ps_env_kind_from_str(NULL) == BakePsEnvUnknown);
}

static void test_ps_kill_target_matching(void) {
    bake_ps_entry_t named = {
        .pid = 4242, .env = (char*)"agent_a", .env_kind = BakePsEnvLocal };
    bake_ps_entry_t unnamed = { .pid = 4243, .env_kind = BakePsEnvLocal };
    bake_ps_entry_t global = { .pid = 4244, .env_kind = BakePsEnvGlobal };

    CHECK(bake_ps_entry_matches(&named, "4242"));
    CHECK(!bake_ps_entry_matches(&named, "4243"));
    CHECK(bake_ps_entry_matches(&named, "agent_a"));
    CHECK(bake_ps_entry_matches(&named, "local:agent_a"));
    CHECK(bake_ps_entry_matches(&named, "local"));
    CHECK(!bake_ps_entry_matches(&named, "global"));
    CHECK(!bake_ps_entry_matches(&named, "agent_b"));

    CHECK(bake_ps_entry_matches(&unnamed, "local"));
    CHECK(!bake_ps_entry_matches(&unnamed, "local:agent_a"));
    CHECK(!bake_ps_entry_matches(&unnamed, "global"));

    CHECK(bake_ps_entry_matches(&global, "global"));
    CHECK(!bake_ps_entry_matches(&global, "local"));
    CHECK(!bake_ps_entry_matches(&global, ""));
    CHECK(!bake_ps_entry_matches(&global, NULL));
}

static void test_ps_render_columns(void) {
    const char *workspace = "/Users/someone/very/long/path/to/a/workspace/that/does/not/fit";
    char *rendered = bake_ps_render_workspace(workspace, false);
    CHECK(strlen(rendered) < strlen(workspace));
    CHECK(strstr(rendered, "...") != NULL);
    ecs_os_free(rendered);

    rendered = bake_ps_render_workspace(workspace, true);
    CHECK_STR(rendered, workspace);
    ecs_os_free(rendered);

    rendered = bake_ps_render_workspace(NULL, true);
    CHECK_STR(rendered, "-");
    ecs_os_free(rendered);

    rendered = bake_ps_render_workspace("", false);
    CHECK_STR(rendered, "-");
    ecs_os_free(rendered);

    char *home = bake_os_home_path();
    if (home) {
        char *path = bake_path_join(home, "ws");
        rendered = bake_ps_render_workspace(path, false);
        CHECK_STR(rendered, "~/ws");
        ecs_os_free(rendered);

        rendered = bake_ps_render_workspace(path, true);
        CHECK_STR(rendered, path);
        ecs_os_free(rendered);
        ecs_os_free(path);
        ecs_os_free(home);
    }

    const char *cmd =
        "/ws/.bake/local_env/agent_a/arm64-Darwin/debug/bin/night_shift "
        "--headless --scene /ws/etc/scenes/a/very/long/scene/path.flecs";
    rendered = bake_ps_render_cmd(cmd, false);
    CHECK(strlen(rendered) < strlen(cmd));
    CHECK(bake_has_suffix(rendered, "..."));
    ecs_os_free(rendered);

    rendered = bake_ps_render_cmd(cmd, true);
    CHECK_STR(rendered, cmd);
    ecs_os_free(rendered);

    rendered = bake_ps_render_cmd("app -a", false);
    CHECK_STR(rendered, "app -a");
    ecs_os_free(rendered);

    rendered = bake_ps_render_cmd(NULL, false);
    CHECK_STR(rendered, "-");
    ecs_os_free(rendered);
}

static void test_ps_registry_roundtrip(void) {
    char *dir = flecs_asprintf("bake_unit_ps_%lld", (long long)bake_os_pid());
    bake_os_setenv("BAKE3_PS_DIR", dir);

    char *registry = bake_ps_registry_dir();
    CHECK_STR(registry, dir);
    ecs_os_free(registry);

    char *exe = bake_os_executable_path();
    const char *argv[] = {NULL, "--headless", NULL};
    argv[0] = exe;

    bake_ps_info_t info = {
        .project = "unit_app",
        .cfg = "debug",
        .env = "unit_env",
        .env_kind = BakePsEnvLocal,
        .bake_home = "/ws/.bake/local_env/unit_env",
        .workspace = "/ws",
        .kind = "run"
    };

    CHECK(bake_ps_register(bake_os_pid(), argv, &info) == 0);

    char *entry_file = flecs_asprintf("%s/%lld.json", dir, (long long)bake_os_pid());
    CHECK(bake_path_exists(entry_file));

    bake_ps_info_t stale = { .project = "gone", .cfg = "debug", .env = "unit_env" };
    const char *stale_argv[] = {"/does/not/exist/gone", NULL};
    CHECK(bake_ps_register(2147483646, stale_argv, &stale) == 0);
    char *stale_file = flecs_asprintf("%s/2147483646.json", dir);
    CHECK(bake_path_exists(stale_file));

    bake_ps_list_t list;
    CHECK(bake_ps_collect(&list, false) >= 0);

    const bake_ps_entry_t *found = NULL;
    for (int32_t i = 0; i < list.count; i ++) {
        if (list.items[i].pid == bake_os_pid()) {
            found = &list.items[i];
        }
        CHECK(list.items[i].pid != 2147483646);
    }

    CHECK(found != NULL);
    if (found) {
        CHECK_STR(found->project, "unit_app");
        CHECK_STR(found->cfg, "debug");
        CHECK_STR(found->env, "unit_env");
        CHECK(found->env_kind == BakePsEnvLocal);
        char *label = bake_ps_env_label(found->env_kind, found->env);
        CHECK_STR(label, "local:unit_env");
        ecs_os_free(label);
        CHECK_STR(found->workspace, "/ws");
        CHECK_STR(found->kind, "run");
        CHECK_STR(found->source, "registry");
        CHECK(found->elapsed_sec >= 0);
        CHECK(found->cmd && strstr(found->cmd, "--headless") != NULL);
    }

    bake_ps_list_fini(&list);

    CHECK(!bake_path_exists(stale_file));

    bake_ps_unregister(bake_os_pid());
    CHECK(!bake_path_exists(entry_file));

    bake_os_rmtree(dir);
    bake_os_unsetenv("BAKE3_PS_DIR");
    ecs_os_free(stale_file);
    ecs_os_free(entry_file);
    ecs_os_free(exe);
    ecs_os_free(dir);
}

static void test_ps_shorten_path(void) {
    char *shortened = bake_ps_shorten_path("/ws/app", 32);
    CHECK_STR(shortened, "/ws/app");
    ecs_os_free(shortened);

    shortened = bake_ps_shorten_path(
        "/Users/someone/very/long/path/to/a/workspace/that/does/not/fit", 24);
    CHECK(strlen(shortened) <= 24);
    CHECK(!strncmp(shortened, "...", 3));
    ecs_os_free(shortened);

    char *home = bake_os_home_path();
    if (home) {
        char *path = bake_path_join(home, "ws");
        shortened = bake_ps_shorten_path(path, 64);
        CHECK_STR(shortened, "~/ws");
        ecs_os_free(shortened);
        ecs_os_free(path);
        ecs_os_free(home);
    }
}

static void test_coverage_path_matches(void) {
    CHECK(bake_coverage_path_matches("src", "src/a.c"));
    CHECK(bake_coverage_path_matches("src/", "src/a/b.c"));
    CHECK(bake_coverage_path_matches("./src", "src/a.c"));
    CHECK(bake_coverage_path_matches("src/a.c", "src/a.c"));
    CHECK(!bake_coverage_path_matches("src", "srcx/a.c"));
    CHECK(!bake_coverage_path_matches("src", "include/src/a.c"));
    CHECK(!bake_coverage_path_matches("", "src/a.c"));

    CHECK(bake_coverage_path_matches("src/*.c", "src/a.c"));
    CHECK(!bake_coverage_path_matches("src/*.c", "src/sub/a.c"));
    CHECK(bake_coverage_path_matches("src/**", "src/sub/a.c"));
    CHECK(bake_coverage_path_matches("src/**/*.c", "src/a.c"));
    CHECK(bake_coverage_path_matches("src/**/*.c", "src/x/y/a.c"));
    CHECK(!bake_coverage_path_matches("src/**/*.c", "src/x/a.h"));
    CHECK(bake_coverage_path_matches("**/*.h", "include/a/b.h"));
    CHECK(bake_coverage_path_matches("*/lib?.c", "src/lib1.c"));
    CHECK(!bake_coverage_path_matches("*/lib?.c", "src/lib12.c"));
    CHECK(!bake_coverage_path_matches("*.c", "src/a.c"));
}

static void test_coverage_simplify_name(void) {
    const char *cases[][3] = {
        {"flecs::world::get_ref<Position>", "0", "flecs::world::get_ref"},
        {"flecs::box<flecs::pair<int, char>>::get", "0", "flecs::box::get"},
        {"ns::operator<<", "0", "ns::operator<<"},
        {"ns::type<int>::operator<", "0", "ns::type::operator<"},
        {"ns::type<int>::operator()", "0", "ns::type::operator()"},
        {"ns::type<int>::operator->", "0", "ns::type::operator->"},
        {"cooperator<int>::run", "0", "cooperator::run"},
        {"(anonymous namespace)::helper<int>", "0", "(anonymous namespace)::helper"},
        {"covlib_unused", "0", "covlib_unused"},
        {"ns::f<int>(int, char) const", "1", "ns::f"},
        {"ns::type<int>::operator()(int)", "1", "ns::type::operator()"},
        {"(anonymous namespace)::g(int)", "1", "(anonymous namespace)::g"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *result = bake_coverage_simplify_name(cases[i][0], cases[i][1][0] == '1');
        CHECK_STR(result, cases[i][2]);
        ecs_os_free(result);
    }
}

int main(void) {
    ecs_os_init();

    test_trim();
    test_errno_message();
    test_path_join();
    test_path_compare();
    test_path_is_abs();
    test_project_id_as_macro();
    test_shell_quote();
    test_strlist();
    test_bundle_cargo_native_paths();
    test_bundle_cargo_emscripten_paths();
    test_bundle_cargo_args();
    test_mode_flags_native();
    test_mode_flags_emscripten();
    test_em_serve_first_port();
    test_local_env_name_validation();
    test_local_env_home();
    test_local_env_build_root();
    test_expr_split();
    test_expr_expand();
    test_settings_expand_output_dir();
    test_settings_project_output_dir();
    test_settings_colors();
    test_lock_excludes_second_holder();
    test_ps_elapsed_formatting();
    test_ps_parse_local_env_path();
    test_ps_env_labels();
    test_ps_render_columns();
    test_ps_kill_target_matching();
    test_ps_registry_roundtrip();
    test_ps_shorten_path();
    test_coverage_path_matches();
    test_coverage_simplify_name();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
