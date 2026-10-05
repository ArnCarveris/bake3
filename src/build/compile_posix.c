#include "build_internal.h"
#include "compile_internal.h"
#include "bake/build_report.h"
#include "bake/os.h"

#include <string.h>

static void bake_strbuf_append_quoted_path(
    ecs_strbuf_t *cmd, const char *prefix, const char *path)
{
    ecs_strbuf_append(cmd, "%s\"", prefix);
#if defined(_WIN32)
    for (const char *p = path; *p; p++) {
        ecs_strbuf_appendch(cmd, (*p == '\\') ? '/' : *p);
    }
#else
    ecs_strbuf_appendstr(cmd, path);
#endif
    ecs_strbuf_appendstr(cmd, "\"");
}

static bool bake_lang_sets_feature_macro(const bake_lang_cfg_t *lang, bool cpp) {
    static const char *macros[] = {
        "_GNU_SOURCE", "_DEFAULT_SOURCE", "_BSD_SOURCE", "_SVID_SOURCE",
        "_POSIX_C_SOURCE", "_XOPEN_SOURCE", "_ANSI_SOURCE", NULL
    };

    const bake_strlist_t *lists[3];
    int32_t list_count = 0;
    lists[list_count ++] = &lang->defines;
    lists[list_count ++] = &lang->cflags;
    if (cpp) {
        lists[list_count ++] = &lang->cxxflags;
    }

    for (int32_t l = 0; l < list_count; l ++) {
        for (int32_t i = 0; i < lists[l]->count; i ++) {
            const char *item = lists[l]->items[i];
            if (!item) {
                continue;
            }
            if (item[0] == '-' && item[1] == 'D') {
                item += 2;
            }
            for (int32_t m = 0; macros[m]; m ++) {
                size_t len = strlen(macros[m]);
                if (!strncmp(item, macros[m], len) &&
                    (item[len] == '\0' || item[len] == '='))
                {
                    return true;
                }
            }
        }
    }

    return false;
}

static bool bake_needs_default_source(const char *std, const bake_lang_cfg_t *lang, bool cpp) {
    if (!std || !strncmp(std, "gnu", 3)) {
        return false;
    }
    if (strcmp(bake_target_os(), "Linux")) {
        return false;
    }
    return !bake_lang_sets_feature_macro(lang, cpp);
}

int bake_compose_compile_command_posix(const bake_compile_cmd_ctx_t *ctx, ecs_strbuf_t *cmd) {
    const char *compiler = ctx->unit->cpp
        ? (ctx->ctx->opts.cxx ? ctx->ctx->opts.cxx : "c++")
        : (ctx->ctx->opts.cc ? ctx->ctx->opts.cc : "cc");

    ecs_strbuf_append(cmd, "%s -c", compiler);
    for (int32_t i = 0; i < ctx->mode_flags->count; i++) {
        ecs_strbuf_append(cmd, " %s", ctx->mode_flags->items[i]);
    }

    const char *std = ctx->unit->cpp
        ? ctx->lang->cpp_standard
        : ctx->lang->c_standard;
    if (std) {
        ecs_strbuf_append(cmd, " -std=%s", std);
    }

    if (bake_needs_default_source(std, ctx->lang, ctx->unit->cpp)) {
        ecs_strbuf_appendstr(cmd, " -D_DEFAULT_SOURCE");
    }

    bake_list_append_fmt(cmd, &ctx->lang->cflags, "");
    if (ctx->unit->cpp) {
        bake_list_append_fmt(cmd, &ctx->lang->cxxflags, "");
    }
    bake_list_append_fmt(cmd, &ctx->lang->defines, "-D");
    ecs_strbuf_append(cmd, " -DBAKE_PROJECT_ID=\\\"%s\\\"", ctx->cfg->id);
    if (ctx->cfg->kind == BAKE_PROJECT_PACKAGE) {
        char *macro = bake_project_id_as_macro(ctx->cfg->id);
        ecs_strbuf_append(cmd, " -D%s_EXPORTS", macro);
        ecs_os_free(macro);
    }

    char *include = bake_path_join(ctx->cfg->path, "include");
    if (bake_path_exists(include)) {
        bake_strbuf_append_quoted_path(cmd, " -I", include);
    }
    ecs_os_free(include);

    for (int32_t i = 0; i < ctx->lang->include_paths.count; i++) {
        bake_strbuf_append_quoted_path(cmd, " -I", ctx->lang->include_paths.items[i]);
    }
    for (int32_t i = 0; i < ctx->dep_includes->count; i++) {
        bake_strbuf_append_quoted_path(cmd, " -I", ctx->dep_includes->items[i]);
    }

    if (ctx->unit->dep) {
        bake_strbuf_append_quoted_path(cmd, " -MMD -MF ", ctx->unit->dep);
    }

    bake_strbuf_append_quoted_path(cmd, " -o ", ctx->unit->obj);
    bake_strbuf_append_quoted_path(cmd, " ", ctx->unit->src);
    return 0;
}

int bake_compose_link_command_posix(const bake_link_cmd_ctx_t *ctx, ecs_strbuf_t *cmd) {
    bool is_lib = ctx->cfg->kind == BAKE_PROJECT_PACKAGE;
    bool is_shared = is_lib && ctx->cfg->shared_library;
    if (is_lib && !is_shared) {
        const char *ar_prefix = bake_target_is_emscripten() ? "emar rcs " : "ar rcs ";
        bake_strbuf_append_quoted_path(cmd, ar_prefix, ctx->artefact);
        for (int32_t i = 0; i < ctx->units->count; i++) {
            bake_strbuf_append_quoted_path(cmd, " ", ctx->units->items[i].obj);
        }
        return 0;
    }

    const char *linker = ctx->use_cpp
        ? (ctx->ctx->opts.cxx ? ctx->ctx->opts.cxx : "c++")
        : (ctx->ctx->opts.cc ? ctx->ctx->opts.cc : "cc");

    ecs_strbuf_append(cmd, "%s", linker);
    if (is_shared) {
        char *file_name = bake_path_basename(ctx->artefact);
        if (!strcmp(bake_target_os(), "Darwin")) {
            ecs_strbuf_append(cmd,
                " -dynamiclib -install_name \"@rpath/%s\"", file_name);
        } else if (!strcmp(bake_target_os(), "Linux")) {
            ecs_strbuf_append(cmd, " -shared -Wl,-soname,\"%s\"", file_name);
        } else {
            ecs_strbuf_appendstr(cmd, " -shared");
        }
        ecs_os_free(file_name);
    }
    for (int32_t i = 0; i < ctx->units->count; i++) {
        bake_strbuf_append_quoted_path(cmd, " ", ctx->units->items[i].obj);
    }
#if !defined(__APPLE__)
    if (ctx->dep_artefacts->count > 0) {
        ecs_strbuf_appendstr(cmd, " -Wl,--start-group");
    }
#endif
    for (int32_t i = 0; i < ctx->dep_artefacts->count; i++) {
        bake_strbuf_append_quoted_path(cmd, " ", ctx->dep_artefacts->items[i]);
    }
#if !defined(__APPLE__)
    if (ctx->dep_artefacts->count > 0) {
        ecs_strbuf_appendstr(cmd, " -Wl,--end-group");
    }
#endif
    /* Emitted before the project flags so a project can override them */
    if (bake_target_is_emscripten()) {
        ecs_strbuf_appendstr(cmd, " -s ALLOW_MEMORY_GROWTH=1");
        ecs_strbuf_appendstr(cmd, " -s EXPORTED_RUNTIME_METHODS=cwrap");
        ecs_strbuf_appendstr(cmd, " -s MODULARIZE=1");

        const char *export_name = ctx->cfg->output_name;
        char *export_name_alloc = NULL;
        if (!export_name || !export_name[0]) {
            export_name_alloc = bake_project_id_as_macro(ctx->cfg->id);
            export_name = export_name_alloc;
        }
        ecs_strbuf_append(cmd, " -s EXPORT_NAME=\"%s\"", export_name);
        ecs_os_free(export_name_alloc);
    }

    bake_list_append_fmt(cmd, ctx->mode_ldflags, "");
    bake_list_append_fmt(cmd, &ctx->lang->ldflags, "");
    bake_list_append_fmt(cmd, ctx->dep_ldflags, "");
    for (int32_t i = 0; i < ctx->lang->libpaths.count; i++) {
        bake_strbuf_append_quoted_path(cmd, " -L", ctx->lang->libpaths.items[i]);
    }
    for (int32_t i = 0; i < ctx->dep_libpaths->count; i++) {
        bake_strbuf_append_quoted_path(cmd, " -L", ctx->dep_libpaths->items[i]);
    }
    for (int32_t i = 0; i < ctx->lang->libs.count; i++) {
        ecs_strbuf_append(cmd, " -l%s", ctx->lang->libs.items[i]);
    }
    for (int32_t i = 0; i < ctx->dep_libs->count; i++) {
        ecs_strbuf_append(cmd, " -l%s", ctx->dep_libs->items[i]);
    }

    if (bake_target_is_emscripten()) {
        const char *shell = bake_project_cfg_shell(ctx->cfg);
        if (shell) {
            if (bake_path_is_abs(shell)) {
                bake_strbuf_append_quoted_path(cmd, " --shell-file ", shell);
            } else {
                char *shell_path = bake_path_join(ctx->cfg->path, shell);
                bake_strbuf_append_quoted_path(cmd, " --shell-file ", shell_path);
                ecs_os_free(shell_path);
            }
        }

        int32_t embed_step = ctx->lang->embed.count
            ? bake_report_open(ctx->ctx->report, BAKE_REPORT_KIND_OTHER,
                "embed", ctx->cfg->id)
            : BAKE_REPORT_NO_STEP;

        for (int32_t i = 0; i < ctx->lang->embed.count; i++) {
            const char *entry = ctx->lang->embed.items[i];
            if (bake_path_is_abs(entry) || strchr(entry, '@')) {
                bake_strbuf_append_quoted_path(cmd, " --embed-file ", entry);
            } else {
                char *src = bake_path_join(ctx->cfg->path, entry);
                char *mapped = flecs_asprintf("%s@%s", src, entry);
                bake_strbuf_append_quoted_path(cmd, " --embed-file ", mapped);
                ecs_os_free(mapped);
                ecs_os_free(src);
            }
        }

        bake_report_close(ctx->ctx->report, embed_step, true, NULL);
    }

    bake_strbuf_append_quoted_path(cmd, " -o ", ctx->artefact);
    return 0;
}
