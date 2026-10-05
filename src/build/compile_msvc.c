#include "build_internal.h"
#include "compile_internal.h"
#include "bake/os.h"
#include <ctype.h>
#include <stdlib.h>

/* Map a gcc style language standard (c11, gnu99, c++17, ...) to the value of
 * MSVC's /std flag. MSVC has no C99 or C++11 mode; those map to the oldest
 * mode that includes them. Returns NULL to keep the compiler default. */
static const char* bake_msvc_std_flag(const char *std, bool cpp) {
    if (!std || !std[0]) {
        return NULL;
    }

    const char *digits = std;
    while (*digits && !isdigit((unsigned char)*digits)) {
        digits++;
    }
    if (!digits[0]) {
        return NULL;
    }
    int version = atoi(digits);

    if (cpp) {
        if (version == 98 || version == 3 || version == 11 || version == 14) return "c++14";
        if (version == 17) return "c++17";
        if (version == 20) return "c++20";
        return "c++latest";
    }

    if (version == 89 || version == 90 || version == 99 || version == 11) return "c11";
    if (version == 17 || version == 18) return "c17";
    return "clatest";
}

int bake_compose_compile_command_msvc(const bake_compile_cmd_ctx_t *ctx, ecs_strbuf_t *cmd) {
    const char *compiler = ctx->unit->cpp
        ? (ctx->ctx->opts.cxx ? ctx->ctx->opts.cxx : "cl")
        : (ctx->ctx->opts.cc ? ctx->ctx->opts.cc : "cl");

    /* Link the release DLL runtime in every mode: Rust staticlibs always use
     * it, and bundles are configured to match (see bake_bundle_run_cmake). */
    ecs_strbuf_append(cmd, "%s /nologo /c /MD", compiler);
    for (int32_t i = 0; i < ctx->mode_flags->count; i++) {
        ecs_strbuf_append(cmd, " %s", ctx->mode_flags->items[i]);
    }

    const char *std = bake_msvc_std_flag(ctx->unit->cpp
        ? ctx->lang->cpp_standard
        : ctx->lang->c_standard, ctx->unit->cpp);
    if (std) {
        ecs_strbuf_append(cmd, " /std:%s", std);
    }
    bake_list_append_fmt(cmd, &ctx->lang->cflags, "");
    if (ctx->unit->cpp) {
        bake_list_append_fmt(cmd, &ctx->lang->cxxflags, "");
    }
    for (int32_t i = 0; i < ctx->lang->defines.count; i++) {
        ecs_strbuf_append(cmd, " /D%s", ctx->lang->defines.items[i]);
    }
    ecs_strbuf_append(cmd, " /DBAKE_PROJECT_ID=\\\"%s\\\"", ctx->cfg->id);
    if (ctx->cfg->kind == BAKE_PROJECT_PACKAGE) {
        char *macro = bake_project_id_as_macro(ctx->cfg->id);
        ecs_strbuf_append(cmd, " /D%s_EXPORTS", macro);
        ecs_os_free(macro);
    }

    char *include = bake_path_join(ctx->cfg->path, "include");
    if (bake_path_exists(include)) {
        ecs_strbuf_append(cmd, " /I\"%s\"", include);
    }
    ecs_os_free(include);

    for (int32_t i = 0; i < ctx->lang->include_paths.count; i++) {
        ecs_strbuf_append(cmd, " /I\"%s\"", ctx->lang->include_paths.items[i]);
    }
    for (int32_t i = 0; i < ctx->dep_includes->count; i++) {
        ecs_strbuf_append(cmd, " /I\"%s\"", ctx->dep_includes->items[i]);
    }

    /* Record every header the unit includes, directly or indirectly, so a
     * header change recompiles it. clang-cl has no /sourceDependencies but
     * passes gcc style depfile options through /clang:. */
    if (ctx->unit->dep) {
        if (strstr(compiler, "clang-cl")) {
            ecs_strbuf_append(cmd, " /clang:-MMD \"/clang:-MF%s\"", ctx->unit->dep);
        } else {
            ecs_strbuf_append(cmd, " /sourceDependencies \"%s\"", ctx->unit->dep);
        }
    }

    ecs_strbuf_append(cmd, " /Fo\"%s\" \"%s\"", ctx->unit->obj, ctx->unit->src);
    return 0;
}

/* GNU toolchain runtime libraries that the MSVC runtime already provides. */
static bool bake_msvc_lib_is_implicit(const char *lib) {
    static const char *implicit[] = {
        "m", "c", "c++", "stdc++", "pthread", "dl", "rt", NULL
    };
    for (int32_t i = 0; implicit[i]; i++) {
        if (!strcmp(lib, implicit[i])) {
            return true;
        }
    }
    return false;
}

int bake_compose_link_command_msvc(const bake_link_cmd_ctx_t *ctx, ecs_strbuf_t *cmd) {
    bool is_lib = ctx->cfg->kind == BAKE_PROJECT_PACKAGE;
    bool is_shared = is_lib && ctx->cfg->shared_library;
    if (is_lib && !is_shared) {
        ecs_strbuf_append(cmd, "lib /nologo /OUT:\"%s\"", ctx->artefact);
        for (int32_t i = 0; i < ctx->units->count; i++) {
            ecs_strbuf_append(cmd, " \"%s\"", ctx->units->items[i].obj);
        }
        return 0;
    }

    const char *linker = ctx->use_cpp
        ? (ctx->ctx->opts.cxx ? ctx->ctx->opts.cxx : "cl")
        : (ctx->ctx->opts.cc ? ctx->ctx->opts.cc : "cl");

    ecs_strbuf_append(cmd, "%s /nologo", linker);
    if (is_shared) {
        ecs_strbuf_appendstr(cmd, " /LD");
    }
    for (int32_t i = 0; i < ctx->units->count; i++) {
        ecs_strbuf_append(cmd, " \"%s\"", ctx->units->items[i].obj);
    }
    for (int32_t i = 0; i < ctx->dep_artefacts->count; i++) {
        ecs_strbuf_append(cmd, " \"%s\"", ctx->dep_artefacts->items[i]);
    }
    for (int32_t i = 0; i < ctx->lang->libs.count; i++) {
        if (!bake_msvc_lib_is_implicit(ctx->lang->libs.items[i])) {
            ecs_strbuf_append(cmd, " %s.lib", ctx->lang->libs.items[i]);
        }
    }
    for (int32_t i = 0; i < ctx->dep_libs->count; i++) {
        if (!bake_msvc_lib_is_implicit(ctx->dep_libs->items[i])) {
            ecs_strbuf_append(cmd, " %s.lib", ctx->dep_libs->items[i]);
        }
    }
    ecs_strbuf_append(cmd, " /Fe\"%s\"", ctx->artefact);
    ecs_strbuf_appendstr(cmd, " /link");
    bake_list_append_fmt(cmd, ctx->mode_ldflags, "");
    bake_list_append_fmt(cmd, &ctx->lang->ldflags, "");
    bake_list_append_fmt(cmd, ctx->dep_ldflags, "");
    for (int32_t i = 0; i < ctx->lang->libpaths.count; i++) {
        ecs_strbuf_append(cmd, " /LIBPATH:\"%s\"", ctx->lang->libpaths.items[i]);
    }
    for (int32_t i = 0; i < ctx->dep_libpaths->count; i++) {
        ecs_strbuf_append(cmd, " /LIBPATH:\"%s\"", ctx->dep_libpaths->items[i]);
    }
    return 0;
}
