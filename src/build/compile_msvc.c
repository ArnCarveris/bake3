#include "build_internal.h"
#include "compile_internal.h"
#include "bake/os.h"

int bake_compose_compile_command_msvc(const bake_compile_cmd_ctx_t *ctx, ecs_strbuf_t *cmd) {
    const char *compiler = ctx->unit->cpp
        ? (ctx->ctx->opts.cxx ? ctx->ctx->opts.cxx : "cl")
        : (ctx->ctx->opts.cc ? ctx->ctx->opts.cc : "cl");

    ecs_strbuf_append(cmd, "%s /nologo /c", compiler);
    for (int32_t i = 0; i < ctx->mode_flags->count; i++) {
        ecs_strbuf_append(cmd, " %s", ctx->mode_flags->items[i]);
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

    ecs_strbuf_append(cmd, " /Fo\"%s\" \"%s\"", ctx->unit->obj, ctx->unit->src);
    return 0;
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
        ecs_strbuf_append(cmd, " %s.lib", ctx->lang->libs.items[i]);
    }
    for (int32_t i = 0; i < ctx->dep_libs->count; i++) {
        ecs_strbuf_append(cmd, " %s.lib", ctx->dep_libs->items[i]);
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
