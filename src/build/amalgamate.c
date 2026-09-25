#include "build_internal.h"
#include "bake/os.h"

#include <ctype.h>

#define BAKE_AMALG_MAX_LINE (4096)

typedef struct bake_amalgamate_ctx_t {
    const bake_project_cfg_t *cfg;
    const char *include_name;
    const char *include_path;
    const bake_strlist_t *disable;
} bake_amalgamate_ctx_t;

typedef struct bake_collect_sources_ctx_t {
    bake_strlist_t *sources;
    const char *suffix;
} bake_collect_sources_ctx_t;

static const char* bake_skip_ws(const char *ptr) {
    while (ptr && *ptr && isspace((unsigned char)*ptr)) {
        ptr++;
    }
    return ptr;
}

static char* bake_parse_include_file(const char *line, bool *relative_out) {
    if (relative_out) {
        *relative_out = false;
    }

    const char *p = bake_skip_ws(line);
    if (!p || p[0] != '#') {
        return NULL;
    }

    p = bake_skip_ws(p + 1);
    if (strncmp(p, "include", 7)) {
        return NULL;
    }

    p = bake_skip_ws(p + 7);
    if (!p || !p[0]) {
        return NULL;
    }

    char end = '>';
    if (p[0] == '"') {
        end = '"';
        if (relative_out) {
            *relative_out = true;
        }
    } else if (p[0] != '<') {
        return NULL;
    }

    p++;
    const char *end_ptr = strchr(p, end);
    if (!end_ptr) {
        return NULL;
    }

    size_t len = (size_t)(end_ptr - p);
    char *out = ecs_os_malloc(len + 1);
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

static bool bake_path_mark_parsed(bake_strlist_t *parsed, const char *path) {
    char *normalized = bake_path_resolve(path);
    if (!normalized) {
        return false;
    }

    if (bake_strlist_contains(parsed, normalized)) {
        ecs_os_free(normalized);
        return false;
    }

    bake_strlist_append_owned(parsed, normalized);
    return true;
}

static int bake_source_depth(const char *path) {
    int depth = 0;
    for (const char *p = path; p && *p; p++) {
        if (*p == '/' || *p == '\\') {
            depth++;
        }
    }
    return depth;
}

static int bake_source_path_compare(const void *ptr1, const void *ptr2) {
    const char *path1 = *(const char* const*)ptr1;
    const char *path2 = *(const char* const*)ptr2;

    int depth1 = bake_source_depth(path1);
    int depth2 = bake_source_depth(path2);
    if (depth1 != depth2) {
        return depth1 - depth2;
    }
    return strcmp(path1, path2);
}

#define BAKE_AMALG_MAX_COND (256)

typedef enum bake_cpp_directive_kind_t {
    BAKE_CPP_NONE,
    BAKE_CPP_IF,
    BAKE_CPP_IFDEF,
    BAKE_CPP_IFNDEF,
    BAKE_CPP_ELIF,
    BAKE_CPP_ELSE,
    BAKE_CPP_ENDIF
} bake_cpp_directive_kind_t;

typedef struct bake_cond_frame_t {
    bool managed;
    bool emit;
    bool taken;
} bake_cond_frame_t;

static bool bake_is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static bake_cpp_directive_kind_t bake_parse_cpp_directive(
    const char *line,
    const char **arg_out)
{
    if (arg_out) {
        *arg_out = NULL;
    }

    const char *p = bake_skip_ws(line);
    if (!p || p[0] != '#') {
        return BAKE_CPP_NONE;
    }

    p = bake_skip_ws(p + 1);
    const char *word = p;
    while (bake_is_ident_char(*p)) {
        p++;
    }

    size_t len = (size_t)(p - word);
    bake_cpp_directive_kind_t kind = BAKE_CPP_NONE;
    if (len == 2 && !strncmp(word, "if", 2)) {
        kind = BAKE_CPP_IF;
    } else if (len == 5 && !strncmp(word, "ifdef", 5)) {
        kind = BAKE_CPP_IFDEF;
    } else if (len == 6 && !strncmp(word, "ifndef", 6)) {
        kind = BAKE_CPP_IFNDEF;
    } else if (len == 4 && !strncmp(word, "elif", 4)) {
        kind = BAKE_CPP_ELIF;
    } else if (len == 4 && !strncmp(word, "else", 4)) {
        kind = BAKE_CPP_ELSE;
    } else if (len == 5 && !strncmp(word, "endif", 5)) {
        kind = BAKE_CPP_ENDIF;
    }

    if (kind != BAKE_CPP_NONE && arg_out) {
        *arg_out = p;
    }
    return kind;
}

static bool bake_disable_contains(const bake_strlist_t *disable, const char *name, size_t len) {
    if (!disable || len == 0 || len >= 256) {
        return false;
    }
    char buf[256];
    memcpy(buf, name, len);
    buf[len] = '\0';
    return bake_strlist_contains(disable, buf) != 0;
}

static bool bake_arg_is_disabled_macro(const char *arg, const bake_strlist_t *disable) {
    const char *p = bake_skip_ws(arg);
    const char *name = p;
    while (bake_is_ident_char(*p)) {
        p++;
    }
    return bake_disable_contains(disable, name, (size_t)(p - name));
}

static bool bake_line_continues(const char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    return len > 0 && line[len - 1] == '\\';
}

static bool bake_is_disabled_define(const char *line, const bake_strlist_t *disable) {
    const char *p = bake_skip_ws(line);
    if (p[0] != '#') {
        return false;
    }
    p = bake_skip_ws(p + 1);
    if (strncmp(p, "define", 6) || bake_is_ident_char(p[6])) {
        return false;
    }
    p = bake_skip_ws(p + 6);
    const char *name = p;
    while (bake_is_ident_char(*p)) {
        p++;
    }
    if (p[0] == '(') {
        return false;
    }
    return bake_disable_contains(disable, name, (size_t)(p - name));
}

static bool bake_eval_defined_expr(
    const char *expr,
    const bake_strlist_t *disable,
    bool *value_out)
{
    const char *p = bake_skip_ws(expr);
    bool negate = false;
    if (p[0] == '!') {
        negate = true;
        p = bake_skip_ws(p + 1);
    }

    if (strncmp(p, "defined", 7) || bake_is_ident_char(p[7])) {
        return false;
    }
    p += 7;

    bool had_paren = false;
    p = bake_skip_ws(p);
    if (p[0] == '(') {
        had_paren = true;
        p = bake_skip_ws(p + 1);
    } else if (bake_is_ident_char(p[0]) == false) {
        return false;
    }

    const char *name = p;
    while (bake_is_ident_char(*p)) {
        p++;
    }
    size_t name_len = (size_t)(p - name);

    p = bake_skip_ws(p);
    if (had_paren) {
        if (p[0] != ')') {
            return false;
        }
        p = bake_skip_ws(p + 1);
    }

    if (p[0] != '\0' && p[0] != '\n' && p[0] != '\r') {
        return false;
    }

    if (!bake_disable_contains(disable, name, name_len)) {
        return false;
    }

    *value_out = negate ? true : false;
    return true;
}

static char* bake_condition_text(const char *arg) {
    const char *start = bake_skip_ws(arg);
    size_t len = strlen(start);
    char *text = ecs_os_malloc(len + 1);
    memcpy(text, start, len);
    text[len] = '\0';

    for (char *p = text; *p; p++) {
        if (p[0] == '/' && (p[1] == '/' || p[1] == '*')) {
            *p = '\0';
            break;
        }
        if (p[0] == '\n' || p[0] == '\r') {
            *p = '\0';
            break;
        }
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *(--end) = '\0';
    }

    return text;
}

static bool bake_cond_eval_managed(
    bake_cpp_directive_kind_t kind,
    const char *arg,
    const bake_strlist_t *disable,
    bool *body_emit_out)
{
    if (kind == BAKE_CPP_IFDEF) {
        if (bake_arg_is_disabled_macro(arg, disable)) {
            *body_emit_out = false;
            return true;
        }
        return false;
    }

    if (kind == BAKE_CPP_IFNDEF) {
        if (bake_arg_is_disabled_macro(arg, disable)) {
            *body_emit_out = true;
            return true;
        }
        return false;
    }

    if (kind == BAKE_CPP_IF) {
        char *cond = bake_condition_text(arg);
        bool value = false;
        bool managed = bake_eval_defined_expr(cond, disable, &value);
        ecs_os_free(cond);
        if (managed) {
            *body_emit_out = value;
            return true;
        }
        return false;
    }

    return false;
}

/* Records a verbatim (non-inlined) include and reports whether it should be
 * emitted. Duplicate includes are dropped only at the top level of a
 * translation unit (if_depth 0); includes guarded by #if are always kept since
 * the guard may select between alternatives. emitted is NULL for outputs that
 * are not deduplicated (e.g. the amalgamated header). */
static bool bake_mark_verbatim_include(
    bake_strlist_t *emitted,
    int32_t if_depth,
    const char *include,
    bool relative)
{
    if (!emitted || if_depth != 0) {
        return true;
    }

    char *key = flecs_asprintf("%c%s", relative ? '"' : '<', include);
    if (bake_strlist_contains(emitted, key)) {
        ecs_os_free(key);
        return false;
    }

    bake_strlist_append_owned(emitted, key);
    return true;
}

static int bake_amalgamate_file(
    const bake_amalgamate_ctx_t *ctx,
    FILE *out,
    bool is_include,
    const char *file,
    const char *src_file,
    int32_t src_line,
    bake_strlist_t *parsed,
    bool *main_included,
    bake_strlist_t *emitted_includes,
    int32_t if_depth_in)
{
    if (!bake_path_mark_parsed(parsed, file)) {
        return 0;
    }

    FILE *in = fopen(file, "rb");
    if (!in) {
        ecs_err(
            "cannot read file '%s' while amalgamating '%s' (from '%s:%d')",
            file, ctx->cfg->id, src_file, src_line);
        return -1;
    }

    char *cur_path = bake_path_dirname(file);
    char *base_name = bake_path_basename(file);
    bool bake_config_h = base_name && !strcmp(base_name, "bake_config.h");
    ecs_os_free(base_name);

    char line[BAKE_AMALG_MAX_LINE];
    int32_t line_count = 0;
    bool in_block_comment = false;
    bool skip_continuation = false;
    bool has_disable = ctx->disable && ctx->disable->count > 0;
    bake_cond_frame_t cond_stack[BAKE_AMALG_MAX_COND];
    int32_t cond_depth = 0;
    int32_t suppressed = 0;
    int32_t if_depth = if_depth_in;
    while (fgets(line, BAKE_AMALG_MAX_LINE, in)) {
        line_count++;

        if (skip_continuation) {
            skip_continuation = bake_line_continues(line);
            continue;
        }

        bool line_in_block_comment_at_start = in_block_comment;
        const char *scan = line;
        while (*scan) {
            if (in_block_comment) {
                if (scan[0] == '*' && scan[1] == '/') {
                    in_block_comment = false;
                    scan += 2;
                    continue;
                }
                scan++;
                continue;
            }
            if (scan[0] == '/' && scan[1] == '/') {
                break;
            }
            if (scan[0] == '/' && scan[1] == '*') {
                in_block_comment = true;
                scan += 2;
                continue;
            }
            if (scan[0] == '"' || scan[0] == '\'') {
                char quote = *scan++;
                while (*scan && *scan != quote) {
                    if (scan[0] == '\\' && scan[1]) {
                        scan++;
                    }
                    scan++;
                }
                if (*scan) {
                    scan++;
                }
                continue;
            }
            scan++;
        }

        /* Track preprocessor conditional nesting so verbatim includes can be
         * deduplicated only when they sit at the top level of the unit. */
        if (!line_in_block_comment_at_start) {
            const char *depth_arg = NULL;
            bake_cpp_directive_kind_t depth_dir =
                bake_parse_cpp_directive(line, &depth_arg);
            if (depth_dir == BAKE_CPP_IF ||
                depth_dir == BAKE_CPP_IFDEF ||
                depth_dir == BAKE_CPP_IFNDEF)
            {
                if_depth++;
            } else if (depth_dir == BAKE_CPP_ENDIF && if_depth > 0) {
                if_depth--;
            }
        }

        if (has_disable && !line_in_block_comment_at_start) {
            const char *arg = NULL;
            bake_cpp_directive_kind_t directive =
                bake_parse_cpp_directive(line, &arg);

            if (directive == BAKE_CPP_IF ||
                directive == BAKE_CPP_IFDEF ||
                directive == BAKE_CPP_IFNDEF)
            {
                if (cond_depth >= BAKE_AMALG_MAX_COND) {
                    ecs_err(
                        "preprocessor nesting too deep while amalgamating '%s'",
                        file);
                    ecs_os_free(cur_path);
                    fclose(in);
                    return -1;
                }

                bool body_emit = true;
                bool managed = bake_cond_eval_managed(
                    directive, arg, ctx->disable, &body_emit);

                bake_cond_frame_t *frame = &cond_stack[cond_depth++];
                frame->managed = managed;
                if (managed) {
                    frame->emit = body_emit;
                    frame->taken = body_emit;
                    if (!body_emit) {
                        suppressed++;
                    }
                } else {
                    frame->emit = true;
                    frame->taken = false;
                    if (suppressed == 0) {
                        fprintf(out, "%s", line);
                    }
                }
                continue;
            }

            if (directive == BAKE_CPP_ELSE && cond_depth > 0) {
                bake_cond_frame_t *frame = &cond_stack[cond_depth - 1];
                if (frame->managed) {
                    if (frame->taken) {
                        if (frame->emit) {
                            frame->emit = false;
                            suppressed++;
                        }
                    } else {
                        if (!frame->emit) {
                            suppressed--;
                        }
                        frame->emit = true;
                        frame->taken = true;
                    }
                } else if (suppressed == 0) {
                    fprintf(out, "%s", line);
                }
                continue;
            }

            if (directive == BAKE_CPP_ELIF && cond_depth > 0) {
                bake_cond_frame_t *frame = &cond_stack[cond_depth - 1];
                if (frame->managed) {
                    if (frame->taken) {
                        if (frame->emit) {
                            frame->emit = false;
                            suppressed++;
                        }
                    } else {
                        bool value = false;
                        char *cond = bake_condition_text(arg);
                        bool managed_elif = bake_eval_defined_expr(
                            cond, ctx->disable, &value);
                        ecs_os_free(cond);

                        if (managed_elif) {
                            /* The elif condition itself references a disabled
                             * flag; keep managing this frame. */
                            if (value) {
                                if (!frame->emit) {
                                    suppressed--;
                                }
                                frame->emit = true;
                                frame->taken = true;
                            }
                        } else {
                            if (!frame->emit) {
                                suppressed--;
                            }
                            frame->emit = true;
                            frame->managed = false;
                            if (suppressed == 0) {
                                fprintf(out, "#if%s", arg);
                            }
                        }
                    }
                } else if (suppressed == 0) {
                    fprintf(out, "%s", line);
                }
                continue;
            }

            if (directive == BAKE_CPP_ENDIF && cond_depth > 0) {
                bake_cond_frame_t *frame = &cond_stack[--cond_depth];
                if (frame->managed) {
                    if (!frame->emit) {
                        suppressed--;
                    }
                } else if (suppressed == 0) {
                    fprintf(out, "%s", line);
                }
                continue;
            }
        }

        if (suppressed > 0) {
            continue;
        }

        if (has_disable && !line_in_block_comment_at_start &&
            bake_is_disabled_define(line, ctx->disable))
        {
            skip_continuation = bake_line_continues(line);
            continue;
        }

        bool include_relative = false;
        char *include = NULL;
        if (!line_in_block_comment_at_start) {
            include = bake_parse_include_file(line, &include_relative);
        }
        if (!include) {
            fprintf(out, "%s", line);
            continue;
        }

        if (!is_include && main_included && !main_included[0]) {
            fprintf(out, "#include \"%s.h\"\n", ctx->include_name);
            main_included[0] = true;
        }

        bool recurse = false;
        char *include_path = NULL;

        if (!include_relative) {
            if (bake_config_h) {
                fprintf(out, "#include \"%s\"\n", include);
                ecs_os_free(include);
                continue;
            }

            include_path = bake_path_join(ctx->include_path, include);
            if (include_path && bake_path_exists(include_path)) {
                recurse = true;
            }
        } else {
            include_path = cur_path ? bake_path_join(cur_path, include) : NULL;
            if (!include_path || !bake_path_exists(include_path)) {
                ecs_os_free(include_path);
                include_path = bake_path_join(ctx->include_path, include);
                if (include_path && bake_path_exists(include_path)) {
                    recurse = true;
                } else {
                    ecs_os_free(include_path);
                    include_path = NULL;
                }
            } else {
                recurse = true;
            }
        }

        if (recurse) {
            if (bake_amalgamate_file(
                ctx,
                out,
                is_include,
                include_path,
                file,
                line_count,
                parsed,
                main_included,
                emitted_includes,
                if_depth) != 0)
            {
                ecs_os_free(include_path);
                ecs_os_free(include);
                ecs_os_free(cur_path);
                fclose(in);
                return -1;
            }
        } else if (bake_mark_verbatim_include(
            emitted_includes, if_depth, include, include_relative))
        {
            fprintf(out, "%s", line);
        }

        ecs_os_free(include_path);
        ecs_os_free(include);
    }

    fprintf(out, "\n");
    ecs_os_free(cur_path);

    if (ferror(in)) {
        ecs_err("read error while amalgamating '%s'", file);
        fclose(in);
        return -1;
    }

    if (ferror(out)) {
        ecs_err("write error while amalgamating '%s'", file);
        fclose(in);
        return -1;
    }

    fclose(in);
    return 0;
}

static void bake_try_source_name(
    const char *src_dir,
    const char *name,
    const char *ext,
    char **out)
{
    if (*out) {
        return;
    }

    char *path = flecs_asprintf("%s/%s.%s", src_dir, name, ext);
    if (bake_path_exists(path)) {
        *out = path;
        return;
    }

    ecs_os_free(path);
}

static char* bake_find_main_src_file(
    const bake_project_cfg_t *cfg,
    const char *src_dir,
    const char *project_id)
{
    static const char *c_exts[] = {"c", NULL};
    static const char *cpp_exts[] = {"cpp", "cc", "cxx", "C", NULL};
    const char *const *exts = bake_language_is_cpp(cfg) ? cpp_exts : c_exts;

    char *id_base = bake_project_id_base(cfg->id);
    char *found = NULL;
    for (int i = 0; exts[i] && !found; i++) {
        bake_try_source_name(src_dir, "main", exts[i], &found);
        if (!found) {
            bake_try_source_name(src_dir, project_id, exts[i], &found);
        }
        if (!found) {
            bake_try_source_name(src_dir, id_base, exts[i], &found);
        }
    }

    ecs_os_free(id_base);
    return found;
}

static bool bake_is_supported_source(const char *path) {
    return bake_has_suffix(path, ".c") ||
        bake_has_suffix(path, ".cpp") ||
        bake_has_suffix(path, ".cc") ||
        bake_has_suffix(path, ".cxx") ||
        bake_has_suffix(path, ".C");
}

static bool bake_source_is_cpp(const char *path) {
    return bake_has_suffix(path, ".cpp") ||
        bake_has_suffix(path, ".cc") ||
        bake_has_suffix(path, ".cxx") ||
        bake_has_suffix(path, ".C");
}

static int bake_collect_source_files_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_collect_sources_ctx_t *ctx = ctx_ptr;
    if (entry->is_dir) {
        if (bake_is_dot_dir(entry->name) || entry->name[0] == '.') {
            return 1;
        }
        return 0;
    }

    if (ctx->suffix) {
        if (!bake_has_suffix(entry->path, ctx->suffix)) {
            return 0;
        }
    } else if (!bake_is_supported_source(entry->path)) {
        return 0;
    }

    return bake_strlist_append(ctx->sources, entry->path);
}

static int bake_collect_source_files_w_suffix(
    const char *src_dir,
    const char *suffix,
    bake_strlist_t *sources)
{
    bake_collect_sources_ctx_t ctx = {
        .sources = sources,
        .suffix = suffix
    };

    if (!bake_path_exists(src_dir)) {
        return 0;
    }

    if (bake_dir_walk_recursive(src_dir, bake_collect_source_files_visit, &ctx) != 0) {
        return -1;
    }

    if (sources->count > 1) {
        qsort(
            sources->items,
            (size_t)sources->count,
            sizeof(char*),
            bake_source_path_compare);
    }

    return 0;
}

static bool bake_comment_has_file_directive(const char *start, const char *end) {
    for (const char *q = start; q + 5 <= end; q++) {
        if ((q[0] == '@' || q[0] == '\\') &&
            q[1] == 'f' && q[2] == 'i' && q[3] == 'l' && q[4] == 'e' &&
            (q + 5 == end || !bake_is_ident_char(q[5])))
        {
            return true;
        }
    }
    return false;
}

static char* bake_clean_amalgamation(const char *in, size_t in_len) {
    char *out = ecs_os_malloc(in_len + 1);
    size_t w = 0;
    int newline_run = 2;
    const char *p = in;
    const char *in_end = in + in_len;

    while (*p) {
        char c = *p;

        if (c == '"' || c == '\'') {
            char quote = c;
            out[w++] = *p++;
            while (*p) {
                if (*p == '\\' && p[1]) {
                    out[w++] = *p++;
                    out[w++] = *p++;
                    continue;
                }
                char s = *p;
                out[w++] = *p++;
                if (s == quote) {
                    break;
                }
            }
            newline_run = 0;
            continue;
        }

        if (c == '/' && p[1] == '/') {
            while (*p && *p != '\n') {
                out[w++] = *p++;
            }
            newline_run = 0;
            continue;
        }

        if (c == '/' && p[1] == '*') {
            const char *end = strstr(p + 2, "*/");
            const char *comment_end = end ? end + 2 : in_end;
            if (bake_comment_has_file_directive(p, comment_end)) {
                p = comment_end;
                continue;
            }
            while (p < comment_end) {
                out[w++] = *p++;
            }
            newline_run = 0;
            continue;
        }

        if (c == '\n') {
            if (newline_run >= 2) {
                p++;
                continue;
            }
            out[w++] = '\n';
            newline_run++;
            p++;
            continue;
        }

        out[w++] = c;
        newline_run = 0;
        p++;
    }

    out[w] = '\0';
    return out;
}

static int bake_finalize_amalgamation(
    const char *tmp_file,
    const char *out_file)
{
    size_t len = 0;
    char *raw = bake_file_read(tmp_file, &len);
    remove(tmp_file);
    if (!raw) {
        return -1;
    }

    char *cleaned = bake_clean_amalgamation(raw, len);
    ecs_os_free(raw);
    int rc = bake_file_write(out_file, cleaned);
    ecs_os_free(cleaned);
    return rc;
}

/* Amalgamates the sources of a single language (C or C++) into one translation
 * unit. C and C++ sources go to separate files -- like Objective-C does -- so a
 * project that mixes them (e.g. a C++ package with C sources) does not force its
 * C code to be compiled as C++, which would fail on C-only idioms. Each unit
 * starts from a copy of the header's parsed set so it includes <base>.h once and
 * skips the headers already inlined there. Sets *created when a file is written
 * (a language with no sources produces nothing). */
static int bake_amalgamate_source_group(
    const bake_amalgamate_ctx_t *ctx,
    const char *out_tmp,
    bool want_cpp,
    const bake_strlist_t *sources,
    const char *main_src,
    const bake_strlist_t *base_parsed,
    bool *created)
{
    *created = false;

    bool main_in_group =
        main_src && (bake_source_is_cpp(main_src) == want_cpp);
    bool any = main_in_group;
    for (int32_t i = 0; i < sources->count && !any; i++) {
        if (main_src && !strcmp(sources->items[i], main_src)) {
            continue;
        }
        if (bake_source_is_cpp(sources->items[i]) == want_cpp) {
            any = true;
        }
    }
    if (!any) {
        return 0;
    }

    FILE *fp = fopen(out_tmp, "wb");
    if (!fp) {
        return -1;
    }

    bake_strlist_t parsed = {0};
    bake_strlist_init(&parsed);
    bake_strlist_copy(&parsed, base_parsed);
    bake_strlist_t emitted = {0};
    bake_strlist_init(&emitted);
    bool main_included = false;
    int rc = -1;

    if (main_in_group && bake_amalgamate_file(
        ctx, fp, false, main_src, "(main source)", 0,
        &parsed, &main_included, &emitted, 0) != 0)
    {
        goto done;
    }

    for (int32_t i = 0; i < sources->count; i++) {
        if (main_src && !strcmp(sources->items[i], main_src)) {
            continue;
        }
        if (bake_source_is_cpp(sources->items[i]) != want_cpp) {
            continue;
        }
        if (bake_amalgamate_file(
            ctx, fp, false, sources->items[i], "(source)", 0,
            &parsed, &main_included, &emitted, 0) != 0)
        {
            goto done;
        }
    }

    if (!main_included) {
        fprintf(fp, "#include \"%s.h\"\n", ctx->include_name);
    }

    rc = 0;
done:
    fclose(fp);
    bake_strlist_fini(&parsed);
    bake_strlist_fini(&emitted);
    if (rc == 0) {
        *created = true;
    }
    return rc;
}

static int bake_generate_amalgamation_to_dir(
    const bake_project_cfg_t *cfg,
    const char *project_id,
    const char *include_path,
    const char *src_path,
    const char *main_header,
    const char *output_path,
    const char *output_base,
    const bake_strlist_t *disable_flags)
{
    int rc = -1;
    char *include_out = NULL;
    char *include_tmp = NULL;
    char *c_out = NULL;
    char *c_tmp = NULL;
    char *cpp_out = NULL;
    char *cpp_tmp = NULL;
    char *objc_out = NULL;
    char *objc_tmp = NULL;
    char *main_src = NULL;
    FILE *include_fp = NULL;
    FILE *objc_fp = NULL;
    bool main_included = false;
    bool c_created = false;
    bool cpp_created = false;
    bool objc_created = false;
    bake_strlist_t parsed = {0};
    bake_strlist_t objc_parsed = {0};
    bake_strlist_t objc_emitted = {0};
    bake_strlist_t sources = {0};
    bake_strlist_t objc_sources = {0};

    if (bake_os_mkdirs(output_path) != 0) {
        goto cleanup;
    }

    include_out = flecs_asprintf("%s/%s.h", output_path, output_base);
    include_tmp = flecs_asprintf("%s/%s.h.tmp", output_path, output_base);
    c_out = flecs_asprintf("%s/%s.c", output_path, output_base);
    c_tmp = flecs_asprintf("%s/%s.c.tmp", output_path, output_base);
    cpp_out = flecs_asprintf("%s/%s.cpp", output_path, output_base);
    cpp_tmp = flecs_asprintf("%s/%s.cpp.tmp", output_path, output_base);
    objc_out = flecs_asprintf("%s/%s_objc.m", output_path, output_base);
    objc_tmp = flecs_asprintf("%s/%s_objc.m.tmp", output_path, output_base);

    bake_amalgamate_ctx_t ctx = {
        .cfg = cfg,
        .include_name = output_base,
        .include_path = include_path,
        .disable = disable_flags
    };

    bake_strlist_init(&parsed);

    include_fp = fopen(include_tmp, "wb");
    if (!include_fp) {
        goto cleanup;
    }

    fprintf(include_fp, "// Comment out this line when using as DLL\n");
    fprintf(include_fp, "#define %s_STATIC\n", project_id);
    if (bake_amalgamate_file(
        &ctx, include_fp, true, main_header, "(main header)", 0,
        &parsed, NULL, NULL, 0) != 0)
    {
        goto cleanup;
    }
    fclose(include_fp);
    include_fp = NULL;

    main_src = bake_find_main_src_file(cfg, src_path, project_id);

    bake_strlist_init(&sources);
    if (bake_collect_source_files_w_suffix(src_path, NULL, &sources) != 0) {
        goto cleanup;
    }

    /* C and C++ sources are amalgamated into separate <base>.c and <base>.cpp
     * files so that C code is never compiled as C++. Either file is omitted
     * when the project has no sources of that language. */
    if (bake_amalgamate_source_group(
        &ctx, c_tmp, false, &sources, main_src, &parsed, &c_created) != 0)
    {
        goto cleanup;
    }
    if (bake_amalgamate_source_group(
        &ctx, cpp_tmp, true, &sources, main_src, &parsed, &cpp_created) != 0)
    {
        goto cleanup;
    }

    /* Objective-C sources are amalgamated into a separate <base>_objc.m file
     * (only when the project has any), as they cannot live in the .c output. */
    bake_strlist_init(&objc_sources);
    if (bake_collect_source_files_w_suffix(src_path, ".m", &objc_sources) != 0) {
        goto cleanup;
    }

    if (objc_sources.count) {
        objc_fp = fopen(objc_tmp, "wb");
        if (!objc_fp) {
            goto cleanup;
        }
        objc_created = true;

        /* When the project also has C/C++ sources they already pull in the
         * amalgamated header, so the Objective-C unit -- which inlines the
         * headers it includes directly -- does not re-emit it, matching the
         * output produced before C and C++ were split into separate units. */
        main_included = c_created || cpp_created;
        bake_strlist_init(&objc_parsed);
        bake_strlist_init(&objc_emitted);
        for (int32_t i = 0; i < objc_sources.count; i++) {
            if (bake_amalgamate_file(
                &ctx, objc_fp, false, objc_sources.items[i], "(obj-C source)",
                0, &objc_parsed, &main_included, &objc_emitted, 0) != 0)
            {
                goto cleanup;
            }
        }
        fclose(objc_fp);
        objc_fp = NULL;
    }

    if (bake_finalize_amalgamation(include_tmp, include_out) != 0) {
        goto cleanup;
    }

    if (c_created && bake_finalize_amalgamation(c_tmp, c_out) != 0) {
        goto cleanup;
    }

    if (cpp_created && bake_finalize_amalgamation(cpp_tmp, cpp_out) != 0) {
        goto cleanup;
    }

    if (objc_created && bake_finalize_amalgamation(objc_tmp, objc_out) != 0) {
        goto cleanup;
    }

    if (!c_created) {
        remove(c_out);
    }
    if (!cpp_created) {
        remove(cpp_out);
    }
    if (!objc_created) {
        remove(objc_out);
    }

    rc = 0;
cleanup:
    if (include_fp) {
        fclose(include_fp);
    }
    if (objc_fp) {
        fclose(objc_fp);
    }
    if (rc != 0) {
        if (include_tmp) {
            remove(include_tmp);
        }
        if (c_tmp) {
            remove(c_tmp);
        }
        if (cpp_tmp) {
            remove(cpp_tmp);
        }
        if (objc_tmp) {
            remove(objc_tmp);
        }
    }
    bake_strlist_fini(&sources);
    bake_strlist_fini(&objc_sources);
    bake_strlist_fini(&parsed);
    bake_strlist_fini(&objc_parsed);
    bake_strlist_fini(&objc_emitted);
    ecs_os_free(include_out);
    ecs_os_free(include_tmp);
    ecs_os_free(c_out);
    ecs_os_free(c_tmp);
    ecs_os_free(cpp_out);
    ecs_os_free(cpp_tmp);
    ecs_os_free(objc_out);
    ecs_os_free(objc_tmp);
    ecs_os_free(main_src);
    return rc;
}

static int bake_generate_one_amalgamation(
    const bake_project_cfg_t *cfg,
    const char *project_id,
    const char *include_path,
    const char *src_path,
    const char *main_header,
    const bake_amalgamate_cfg_t *amalg)
{
    const char *output_base =
        (amalg->prefix && amalg->prefix[0]) ? amalg->prefix : project_id;

    char *output_path = (amalg->path && amalg->path[0]) ?
        bake_path_join(cfg->path, amalg->path) : ecs_os_strdup(cfg->path);

    int rc = bake_generate_amalgamation_to_dir(
        cfg, project_id, include_path, src_path, main_header,
        output_path, output_base, &amalg->disable_flags);

    ecs_os_free(output_path);
    return rc;
}

/* Resolves the project's main header (include/<id>.h, falling back to the
 * last id segment) and returns the matching macro id through project_id_out. */
static char* bake_amalgamate_resolve_main_header(
    const bake_project_cfg_t *cfg,
    const char *include_path,
    char **project_id_out)
{
    char *project_id = bake_project_id_as_macro(cfg->id);
    char *main_header = flecs_asprintf("%s/%s.h", include_path, project_id);

    if (!bake_path_exists(main_header)) {
        char *id_base = bake_project_id_base(cfg->id);
        char *base_project_id = bake_project_id_as_macro(id_base);
        char *base_header = flecs_asprintf("%s/%s.h", include_path, base_project_id);
        if (bake_path_exists(base_header)) {
            ecs_os_free(project_id);
            project_id = base_project_id;
            base_project_id = NULL;
            ecs_os_free(main_header);
            main_header = base_header;
            base_header = NULL;
        }
        ecs_os_free(id_base);
        ecs_os_free(base_project_id);
        ecs_os_free(base_header);
    }

    if (!bake_path_exists(main_header)) {
        ecs_os_free(main_header);
        ecs_os_free(project_id);
        return NULL;
    }

    *project_id_out = project_id;
    return main_header;
}

int bake_amalgamate_project(const bake_project_cfg_t *cfg, const char *dst_dir) {
    if (!cfg || !cfg->path) {
        return -1;
    }

    int rc = -1;
    char *project_id = NULL;
    char *output_base = bake_project_id_as_macro(cfg->id);
    char *include_path = bake_path_join(cfg->path, "include");
    char *src_path = bake_path_join(cfg->path, "src");
    char *main_header = bake_amalgamate_resolve_main_header(
        cfg, include_path, &project_id);

    if (!main_header) {
        ecs_err(
            "cannot amalgamate '%s': no main header '%s.h' in '%s'",
            cfg->id, output_base, include_path);
        goto cleanup;
    }

    rc = bake_generate_amalgamation_to_dir(
        cfg, project_id, include_path, src_path, main_header,
        dst_dir, output_base, NULL);

cleanup:
    ecs_os_free(project_id);
    ecs_os_free(output_base);
    ecs_os_free(include_path);
    ecs_os_free(src_path);
    ecs_os_free(main_header);
    return rc;
}

char* bake_amalgamate_output_base(
    const bake_project_cfg_t *cfg,
    const bake_amalgamate_cfg_t *amalg)
{
    if (amalg->prefix && amalg->prefix[0]) {
        return ecs_os_strdup(amalg->prefix);
    }

    char *project_id = NULL;
    char *include_path = bake_path_join(cfg->path, "include");
    char *main_header = bake_amalgamate_resolve_main_header(
        cfg, include_path, &project_id);
    ecs_os_free(main_header);
    ecs_os_free(include_path);

    return project_id ? project_id : bake_project_id_as_macro(cfg->id);
}

int bake_generate_project_amalgamation(
    const bake_project_cfg_t *cfg,
    const char *prefix)
{
    if (!cfg) {
        return 0;
    }

    int32_t count = bake_amalgamate_list_count(&cfg->amalgamate);
    if (count == 0) {
        return 0;
    }

    int rc = -1;
    char *project_id = NULL;
    char *include_path = bake_path_join(cfg->path, "include");
    char *src_path = bake_path_join(cfg->path, "src");
    char *main_header = bake_amalgamate_resolve_main_header(
        cfg, include_path, &project_id);

    if (!main_header) {
        char *macro_id = bake_project_id_as_macro(cfg->id);
        ecs_err("cannot find include file '%s/%s.h' for amalgamation",
            include_path, macro_id);
        ecs_os_free(macro_id);
        goto cleanup;
    }

    rc = 0;
    for (int32_t i = 0; i < count; i++) {
        const bake_amalgamate_cfg_t *amalg = bake_amalgamate_list_get(&cfg->amalgamate, i);
        if (prefix) {
            const char *output_base =
                (amalg->prefix && amalg->prefix[0]) ? amalg->prefix : project_id;
            if (strcmp(output_base, prefix)) {
                continue;
            }
        }

        if (bake_generate_one_amalgamation(
            cfg, project_id, include_path, src_path, main_header, amalg) != 0)
        {
            rc = -1;
            goto cleanup;
        }
    }

cleanup:
    ecs_os_free(project_id);
    ecs_os_free(include_path);
    ecs_os_free(src_path);
    ecs_os_free(main_header);
    return rc;
}
