#if defined(_WIN32)

#include "bake/os.h"
#include "bake/ps.h"
#include <flecs.h>

#include <windows.h>

static char* bake_proc_quote_arg(const char *arg) {
    if (!arg || !arg[0]) {
        return ecs_os_strdup("\"\"");
    }

    bool needs_quote = false;
    for (const char *p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"' || *p == '\n' || *p == '\v') {
            needs_quote = true;
            break;
        }
    }

    if (!needs_quote) {
        return ecs_os_strdup(arg);
    }

    size_t len = strlen(arg);
    char *out = ecs_os_malloc((int32_t)((len * 2) + 3));
    if (!out) {
        return NULL;
    }

    size_t w = 0;
    out[w++] = '"';
    for (size_t i = 0; i < len; ) {
        size_t bs = 0;
        while (i < len && arg[i] == '\\') {
            bs++;
            i++;
        }

        if (i == len) {
            for (size_t j = 0; j < bs * 2; j++) {
                out[w++] = '\\';
            }
            break;
        } else if (arg[i] == '"') {
            for (size_t j = 0; j < bs * 2 + 1; j++) {
                out[w++] = '\\';
            }
            out[w++] = '"';
            i++;
        } else {
            for (size_t j = 0; j < bs; j++) {
                out[w++] = '\\';
            }
            out[w++] = arg[i++];
        }
    }
    out[w++] = '"';
    out[w] = '\0';
    return out;
}

static char* bake_proc_join_command_line(const char *const *argv) {
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    for (int i = 0; argv[i]; i++) {
        char *q = bake_proc_quote_arg(argv[i]);
        if (!q) {
            char *tmp = ecs_strbuf_get(&buf);
            ecs_os_free(tmp);
            return NULL;
        }
        ecs_strbuf_append(&buf, "%s%s", i ? " " : "", q);
        ecs_os_free(q);
    }
    return ecs_strbuf_get(&buf);
}

static HANDLE bake_proc_dup_inheritable(HANDLE src) {
    if (!src || src == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    HANDLE out = NULL;
    if (!DuplicateHandle(
        GetCurrentProcess(),
        src,
        GetCurrentProcess(),
        &out,
        0,
        TRUE,
        DUPLICATE_SAME_ACCESS))
    {
        return NULL;
    }

    return out;
}

static HANDLE bake_proc_open_redirect(
    const char *path,
    bool write_mode,
    bool append_mode)
{
    if (!path || !path[0]) {
        return NULL;
    }

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    DWORD access = write_mode ? GENERIC_WRITE : GENERIC_READ;
    DWORD creation = write_mode
        ? (append_mode ? OPEN_ALWAYS : CREATE_ALWAYS)
        : OPEN_EXISTING;

    HANDLE h = CreateFileA(
        path,
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        creation,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        bake_log_win_error_last("open redirected stdio file", path);
        return NULL;
    }

    if (write_mode && append_mode) {
        DWORD rc = SetFilePointer(h, 0, NULL, FILE_END);
        if (rc == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
            bake_log_win_error_last("seek redirected stdio file", path);
            CloseHandle(h);
            return NULL;
        }
    }

    return h;
}

int bake_proc_run(
    const char *const *argv,
    const bake_process_stdio_t *stdio_cfg,
    bake_process_result_t *result)
{
    if (!argv || !argv[0]) {
        return -1;
    }

    char *cmd_line = bake_proc_join_command_line(argv);
    if (!cmd_line) {
        return -1;
    }

    STARTUPINFOEXA six;
    PROCESS_INFORMATION pi;
    memset(&six, 0, sizeof(six));
    memset(&pi, 0, sizeof(pi));
    six.StartupInfo.cb = sizeof(six.StartupInfo);

    HANDLE std_in = NULL;
    HANDLE std_out = NULL;
    HANDLE std_err = NULL;
    BOOL inherit_handles = FALSE;

    if (stdio_cfg) {
        if (stdio_cfg->stdin_path && stdio_cfg->stdin_path[0]) {
            std_in = bake_proc_open_redirect(stdio_cfg->stdin_path, false, false);
            if (!std_in) {
                ecs_os_free(cmd_line);
                return -1;
            }
        } else {
            std_in = bake_proc_dup_inheritable(GetStdHandle(STD_INPUT_HANDLE));
        }

        if (stdio_cfg->stdout_path && stdio_cfg->stdout_path[0]) {
            std_out = bake_proc_open_redirect(
                stdio_cfg->stdout_path,
                true,
                stdio_cfg->stdout_append);
            if (!std_out) {
                ecs_os_free(cmd_line);
                if (std_in) CloseHandle(std_in);
                return -1;
            }
        } else {
            std_out = bake_proc_dup_inheritable(GetStdHandle(STD_OUTPUT_HANDLE));
        }

        if (stdio_cfg->stderr_to_stdout) {
            std_err = bake_proc_dup_inheritable(std_out);
            if (!std_err) {
                ecs_os_free(cmd_line);
                if (std_in) CloseHandle(std_in);
                if (std_out) CloseHandle(std_out);
                return -1;
            }
        } else if (stdio_cfg->stderr_path && stdio_cfg->stderr_path[0]) {
            std_err = bake_proc_open_redirect(
                stdio_cfg->stderr_path,
                true,
                stdio_cfg->stderr_append);
            if (!std_err) {
                ecs_os_free(cmd_line);
                if (std_in) CloseHandle(std_in);
                if (std_out) CloseHandle(std_out);
                return -1;
            }
        } else {
            std_err = bake_proc_dup_inheritable(GetStdHandle(STD_ERROR_HANDLE));
        }

        /* Absent std handles (no console) are passed through as NULL;
         * only an explicitly requested redirect may fail the spawn. */
        six.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        six.StartupInfo.hStdInput = std_in;
        six.StartupInfo.hStdOutput = std_out;
        six.StartupInfo.hStdError = std_err;
        inherit_handles = TRUE;
    }

    const char *cwd = NULL;
    if (stdio_cfg && stdio_cfg->cwd && stdio_cfg->cwd[0]) {
        cwd = stdio_cfg->cwd;
    }

    /* Only let the child inherit its own std handles. Without a handle list
     * a process spawned concurrently from another thread inherits every
     * inheritable handle of this one, keeping redirect files open (and not
     * removable) after this child exits. */
    HANDLE inherit_list[3];
    DWORD inherit_count = 0;
    HANDLE std_handles[3] = { std_in, std_out, std_err };
    for (int i = 0; i < 3; i++) {
        HANDLE h = std_handles[i];
        bool dup = !h;
        for (DWORD j = 0; j < inherit_count && !dup; j++) {
            dup = inherit_list[j] == h;
        }
        if (!dup) {
            inherit_list[inherit_count++] = h;
        }
    }

    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    DWORD create_flags = 0;
    if (inherit_handles && inherit_count) {
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
        attrs = ecs_os_malloc((ecs_size_t)attr_size);
        if (attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
            UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherit_list, inherit_count * sizeof(HANDLE), NULL, NULL))
        {
            six.lpAttributeList = attrs;
            six.StartupInfo.cb = sizeof(six);
            create_flags |= EXTENDED_STARTUPINFO_PRESENT;
        }
    }

    BOOL ok = CreateProcessA(
        NULL,
        cmd_line,
        NULL,
        NULL,
        inherit_handles,
        create_flags,
        NULL,
        cwd,
        &six.StartupInfo,
        &pi);
    ecs_os_free(cmd_line);
    if (attrs) {
        if (six.lpAttributeList) {
            DeleteProcThreadAttributeList(attrs);
        }
        ecs_os_free(attrs);
    }

    if (std_in) CloseHandle(std_in);
    if (std_out) CloseHandle(std_out);
    if (std_err) CloseHandle(std_err);

    if (!ok) {
        bake_log_win_error_last("start command", argv[0]);
        return -1;
    }

    bool registered = false;
    if (stdio_cfg && stdio_cfg->ps) {
        registered = bake_ps_register(
            (int64_t)pi.dwProcessId, argv, (const bake_ps_info_t*)stdio_cfg->ps) == 0;
    }

    if (WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0) {
        if (registered) {
            bake_ps_unregister((int64_t)pi.dwProcessId);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return -1;
    }

    if (registered) {
        bake_ps_unregister((int64_t)pi.dwProcessId);
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return -1;
    }

    if (result) {
        result->exit_code = (int)exit_code;
        result->term_signal = 0;
        result->interrupted = exit_code == STATUS_CONTROL_C_EXIT;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

int bake_proc_run_argv(const char *const *argv, bake_process_result_t *result) {
    return bake_proc_run(argv, NULL, result);
}

int bake_proc_snapshot(bake_proc_info_t **procs_out, int32_t *count_out) {
    if (procs_out) *procs_out = NULL;
    if (count_out) *count_out = 0;
    return -1;
}

void bake_proc_snapshot_free(bake_proc_info_t *procs, int32_t count) {
    BAKE_UNUSED(count);
    BAKE_UNUSED(procs);
}

#endif

#if !defined(_WIN32)
typedef int bake_win_proc_dummy_t;
#endif
