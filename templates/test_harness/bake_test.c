#include "bake_test.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <io.h>
#include <windows.h>
#endif

#define BAKE_TEST_PARAM_MAX (128)
#define BAKE_TEST_EMPTY (2)
#define BAKE_TEST_QUARANTINED (3)
#define BAKE_TEST_DEFAULT_TIMEOUT (60.0)
#define BAKE_TEST_WAIT_SLICE_MS (200)
#define BAKE_COLOR_GREEN "\033[32m"
#define BAKE_COLOR_YELLOW "\033[33m"
#define BAKE_COLOR_RED "\033[31m"
#define BAKE_COLOR_RESET "\033[0m"

static bake_test_suite *g_current_suite = NULL;
static bake_test_case *g_current_case = NULL;
static bool g_expect_abort = false;
static bool g_failed = false;
static bool g_flaky = false;
static bool g_trace = false;
static const char *g_cli_params[BAKE_TEST_PARAM_MAX];
static int g_cli_param_count = 0;
static char **g_failed_tests = NULL;
static int g_failed_test_count = 0;
static int g_failed_test_cap = 0;
static const char *g_json_path = NULL;
static double g_cli_timeout = 0;
static bool g_cli_timeout_set = false;
static int g_timeout_count = 0;

typedef struct bake_test_result_t {
    char *suite;
    char *testcase;
    char *params;
    const char *status;
    double elapsed;
} bake_test_result_t;

static bake_test_result_t *g_results = NULL;
static int g_result_count = 0;
static int g_result_cap = 0;
#if !defined(_WIN32)
static pthread_mutex_t g_failed_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void bake_failed_lock(void) {
#if !defined(_WIN32)
    pthread_mutex_lock(&g_failed_lock);
#endif
}

static void bake_failed_unlock(void) {
#if !defined(_WIN32)
    pthread_mutex_unlock(&g_failed_lock);
#endif
}

static bool bake_use_colors(void) {
    static int initialized = 0;
    static bool enabled = false;
    if (initialized) {
        return enabled;
    }

    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0]) {
        initialized = 1;
        return false;
    }

#if defined(_WIN32)
    enabled = _isatty(_fileno(stdout)) != 0;
#else
    enabled = isatty(STDOUT_FILENO) != 0;
#endif
    initialized = 1;
    return enabled;
}

static void bake_print_status(const char *status, const char *color) {
    if (color && bake_use_colors()) {
        printf("%s%s%s", color, status, BAKE_COLOR_RESET);
    } else {
        fputs(status, stdout);
    }
}

static const char* bake_debug_exec(const char *exec) {
    const char *test_path = strstr(exec, "/test/");
    if (test_path) {
        return test_path + 1;
    }
    test_path = strstr(exec, "\\test\\");
    if (test_path) {
        return test_path + 1;
    }
    return exec;
}

static void bake_append_suite_params(char *buf, size_t size, bake_test_suite *suite) {
    if (!suite || !suite->param_count || !size) {
        return;
    }

    size_t off = 0;
    int n = snprintf(buf + off, size - off, " [ ");
    if (n < 0 || (size_t)n >= (size - off)) {
        return;
    }
    off += (size_t)n;

    for (uint32_t i = 0; i < suite->param_count; i++) {
        bake_test_param *param = &suite->params[i];
        const char *value = "";
        if (param->value_cur >= 0 && param->value_cur < param->value_count) {
            value = param->values[param->value_cur];
        }
        n = snprintf(buf + off, size - off, "%s%s: %s",
            i ? ", " : "", param->name ? param->name : "", value ? value : "");
        if (n < 0 || (size_t)n >= (size - off)) {
            return;
        }
        off += (size_t)n;
    }

    snprintf(buf + off, size - off, " ]");
}

static void bake_print_debug_command(const char *exec, bake_test_suite *suite, bake_test_case *tc) {
    printf("To run/debug your test, do:\n");
    printf("export $(bake env)\n");
    printf("%s %s.%s", bake_debug_exec(exec), suite->id, tc->id);
    for (int p = 0; p < g_cli_param_count; p++) {
        printf(" --param %s", g_cli_params[p]);
    }
    for (uint32_t p = 0; p < suite->param_count; p++) {
        bake_test_param *param = &suite->params[p];
        bool cli_override = false;
        for (int i = 0; i < g_cli_param_count; i++) {
            size_t len = strlen(param->name);
            if (!strncmp(g_cli_params[i], param->name, len) && g_cli_params[i][len] == '=') {
                cli_override = true;
                break;
            }
        }
        if (cli_override) {
            continue;
        }
        if (param->value_cur < 0 || param->value_cur >= param->value_count) {
            continue;
        }
        printf(" --param %s=%s", param->name, param->values[param->value_cur]);
    }
    printf("\n\n");
}

static void bake_record_failure(
    const char *suite_id,
    const char *case_id,
    const char *param_str,
    const char *reason)
{
    char name[1024];
    snprintf(name, sizeof(name), "%s.%s%s%s%s%s",
        suite_id, case_id, param_str ? param_str : "",
        reason ? " (" : "", reason ? reason : "", reason ? ")" : "");

    size_t len = strlen(name) + 1;
    char *copy = malloc(len);
    if (!copy) {
        return;
    }
    memcpy(copy, name, len);

    bake_failed_lock();
    if (g_failed_test_count == g_failed_test_cap) {
        int cap = g_failed_test_cap ? (g_failed_test_cap * 2) : 16;
        char **tmp = realloc(g_failed_tests, (size_t)cap * sizeof(char*));
        if (!tmp) {
            bake_failed_unlock();
            free(copy);
            return;
        }
        g_failed_tests = tmp;
        g_failed_test_cap = cap;
    }
    g_failed_tests[g_failed_test_count ++] = copy;
    bake_failed_unlock();
}

static double bake_time_now(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static char* bake_strdup(const char *str) {
    size_t len = strlen(str) + 1;
    char *copy = malloc(len);
    if (copy) {
        memcpy(copy, str, len);
    }
    return copy;
}

static void bake_record_result(
    const char *suite_id,
    const char *case_id,
    const char *param_str,
    const char *status,
    double elapsed)
{
    if (!g_json_path) {
        return;
    }

    bake_failed_lock();
    if (g_result_count == g_result_cap) {
        int cap = g_result_cap ? (g_result_cap * 2) : 64;
        bake_test_result_t *tmp = realloc(g_results, (size_t)cap * sizeof(bake_test_result_t));
        if (!tmp) {
            bake_failed_unlock();
            return;
        }
        g_results = tmp;
        g_result_cap = cap;
    }
    bake_test_result_t *r = &g_results[g_result_count ++];
    r->suite = bake_strdup(suite_id);
    r->testcase = bake_strdup(case_id);
    r->params = bake_strdup(param_str ? param_str : "");
    r->status = status;
    r->elapsed = elapsed;
    bake_failed_unlock();
}

static void bake_json_string(FILE *f, const char *str) {
    fputc('"', f);
    for (const char *p = str ? str : ""; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
            fputc(*p, f);
        } else if ((unsigned char)*p < 0x20) {
            fprintf(f, "\\u%04x", (unsigned char)*p);
        } else {
            fputc(*p, f);
        }
    }
    fputc('"', f);
}

static int bake_write_json_report(
    const char *test_id,
    int pass,
    int fail,
    int empty,
    double elapsed)
{
    if (!g_json_path) {
        return 0;
    }

    FILE *f = fopen(g_json_path, "w");
    if (!f) {
        printf("failed to open json report '%s': %s\n", g_json_path, strerror(errno));
        return -1;
    }

    time_t now = time(NULL);
    char stamp[64] = {0};
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    fputs("{\n", f);
    fputs("  \"project\": ", f); bake_json_string(f, test_id); fputs(",\n", f);
    fputs("  \"timestamp\": ", f); bake_json_string(f, stamp); fputs(",\n", f);
    fprintf(f, "  \"pass\": %d,\n  \"fail\": %d,\n  \"empty\": %d,\n  \"timeout\": %d,\n",
        pass, fail, empty, g_timeout_count);
    fprintf(f, "  \"elapsed\": %.6f,\n", elapsed);
    fputs("  \"tests\": [\n", f);
    for (int i = 0; i < g_result_count; i++) {
        bake_test_result_t *r = &g_results[i];
        fputs("    {\"suite\": ", f); bake_json_string(f, r->suite);
        fputs(", \"case\": ", f); bake_json_string(f, r->testcase);
        if (r->params && r->params[0]) {
            fputs(", \"params\": ", f); bake_json_string(f, r->params);
        }
        fputs(", \"status\": ", f); bake_json_string(f, r->status);
        fprintf(f, ", \"elapsed\": %.6f}%s\n", r->elapsed, (i + 1) < g_result_count ? "," : "");
        free(r->suite);
        free(r->testcase);
        free(r->params);
    }
    fputs("  ]\n}\n", f);
    fclose(f);

    free(g_results);
    g_results = NULL;
    g_result_count = 0;
    g_result_cap = 0;
    return 0;
}

static void bake_print_failed_tests(void) {
    if (!g_failed_test_count) {
        return;
    }

    printf("\n");
    bake_print_status("FAIL", BAKE_COLOR_RED);
    printf(": %d test case%s failed:\n", g_failed_test_count, g_failed_test_count == 1 ? "" : "s");
    for (int i = 0; i < g_failed_test_count; i++) {
        printf(" - %s\n", g_failed_tests[i]);
        free(g_failed_tests[i]);
    }

    free(g_failed_tests);
    g_failed_tests = NULL;
    g_failed_test_count = 0;
    g_failed_test_cap = 0;
}

static void bake_print_trace(const char *suite_id, const char *case_id, const char *param_str) {
    printf("RUN %s.%s%s\n", suite_id, case_id, param_str ? param_str : "");
    fflush(stdout);
}

static void bake_print_report(const char *test_id, const char *suite_id, const char *param_str, int pass, int fail, int empty) {
    bake_print_status("PASS", BAKE_COLOR_GREEN);
    printf(":%3d, ", pass);
    bake_print_status("FAIL", fail ? BAKE_COLOR_RED : NULL);
    printf(":%3d, ", fail);
    bake_print_status("EMPTY", empty ? BAKE_COLOR_YELLOW : NULL);
    printf(":%3d (%s.%s%s)\n", empty, test_id, suite_id, param_str ? param_str : "");
}

static bool bake_char_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void bake_free_argv(char **argv, int argc) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static int bake_parse_cmd(const char *cmd, char ***argv_out, int *argc_out) {
    int argc = 0;
    int cap = 8;
    char **argv = calloc((size_t)cap, sizeof(char*));
    if (!argv) {
        return -1;
    }

    const char *p = cmd;
    while (*p) {
        while (*p && bake_char_is_space(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        int tcap = 128;
        int tlen = 0;
        char *tok = malloc((size_t)tcap);
        if (!tok) {
            bake_free_argv(argv, argc);
            return -1;
        }

        while (*p && !bake_char_is_space(*p)) {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) {
                    if (*p == '\\' && p[1] && quote == '"') {
                        p++;
                    }
                    if ((tlen + 2) >= tcap) {
                        tcap *= 2;
                        char *tmp = realloc(tok, (size_t)tcap);
                        if (!tmp) {
                            free(tok);
                            bake_free_argv(argv, argc);
                            return -1;
                        }
                        tok = tmp;
                    }
                    tok[tlen++] = *p++;
                }
                if (*p == quote) {
                    p++;
                }
                continue;
            }

            if (*p == '\\' && p[1]) {
                p++;
            }

            if ((tlen + 2) >= tcap) {
                tcap *= 2;
                char *tmp = realloc(tok, (size_t)tcap);
                if (!tmp) {
                    free(tok);
                    bake_free_argv(argv, argc);
                    return -1;
                }
                tok = tmp;
            }
            tok[tlen++] = *p++;
        }

        tok[tlen] = '\0';
        if ((argc + 2) >= cap) {
            cap *= 2;
            char **tmp = realloc(argv, (size_t)cap * sizeof(char*));
            if (!tmp) {
                free(tok);
                bake_free_argv(argv, argc);
                return -1;
            }
            argv = tmp;
        }
        argv[argc++] = tok;
    }

    if (!argc) {
        bake_free_argv(argv, argc);
        return -1;
    }
    argv[argc] = NULL;
    *argv_out = argv;
    *argc_out = argc;
    return 0;
}

static double bake_case_timeout(const bake_test_suite *suite) {
    if (g_cli_timeout_set) {
        return g_cli_timeout;
    }
    if (suite && suite->timeout > 0) {
        return suite->timeout;
    }
    return BAKE_TEST_DEFAULT_TIMEOUT;
}

#if !defined(_WIN32)
static int bake_wait_for_child(
    pid_t pid,
    int wait_fd,
    double timeout,
    bool group,
    bool *timed_out,
    int *status_out)
{
    double start = bake_time_now();
    bool killed = false;

    for (;;) {
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            *status_out = status;
            return 0;
        }
        if (rc < 0 && errno != EINTR) {
            return -1;
        }

        int wait_ms = BAKE_TEST_WAIT_SLICE_MS;
        if (timeout > 0 && !killed) {
            double remaining = timeout - (bake_time_now() - start);
            if (remaining <= 0) {
                killed = true;
                if (timed_out) {
                    *timed_out = true;
                }
                if (group) {
                    kill(-pid, SIGKILL);
                } else {
                    kill(pid, SIGKILL);
                }
                continue;
            }
            if ((remaining * 1000.0) < (double)wait_ms) {
                wait_ms = (int)(remaining * 1000.0) + 1;
            }
        }

        struct pollfd pfd = { .fd = wait_fd, .events = POLLIN, .revents = 0 };
        int poll_rc = poll(&pfd, 1, wait_ms);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (!poll_rc) {
            continue;
        }

        char buf[64];
        ssize_t count = read(wait_fd, buf, sizeof(buf));
        if (count > 0) {
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }

        for (;;) {
            pid_t wait_rc = waitpid(pid, &status, 0);
            if (wait_rc == pid) {
                *status_out = status;
                return 0;
            }
            if (wait_rc < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
    }
}
#endif

static int bake_run_subprocess_to_stream(
    const char *cmd,
    FILE *stream,
    double timeout,
    bool *timed_out)
{
#if defined(_WIN32)
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    HANDLE child_input = NULL;
    HANDLE child_output = NULL;
    BOOL inherit_handles = FALSE;
    if (stream) {
        int stream_fd = _fileno(stream);
        intptr_t stream_handle = stream_fd >= 0 ? _get_osfhandle(stream_fd) : -1;
        if (stream_handle == -1 || !DuplicateHandle(
            GetCurrentProcess(), (HANDLE)stream_handle,
            GetCurrentProcess(), &child_output, 0, TRUE,
            DUPLICATE_SAME_ACCESS))
        {
            return -1;
        }
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input == NULL || input == INVALID_HANDLE_VALUE || !DuplicateHandle(
            GetCurrentProcess(), input, GetCurrentProcess(),
            &child_input, 0, TRUE, DUPLICATE_SAME_ACCESS))
        {
            SECURITY_ATTRIBUTES attributes = {
                sizeof(SECURITY_ATTRIBUTES), NULL, TRUE
            };
            child_input = CreateFileA(
                "NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (child_input == INVALID_HANDLE_VALUE) {
                CloseHandle(child_output);
                return -1;
            }
        }
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = child_input;
        si.hStdOutput = child_output;
        si.hStdError = child_output;
        inherit_handles = TRUE;
    }

    char *cmd_copy = _strdup(cmd);
    if (!cmd_copy) {
        if (child_input) {
            CloseHandle(child_input);
        }
        if (child_output) {
            CloseHandle(child_output);
        }
        return -1;
    }

    HANDLE job = CreateJobObjectA(NULL, NULL);
    BOOL ok = CreateProcessA(
        NULL, cmd_copy, NULL, NULL, inherit_handles, CREATE_SUSPENDED,
        NULL, NULL, &si, &pi);
    free(cmd_copy);
    if (child_input) {
        CloseHandle(child_input);
    }
    if (child_output) {
        CloseHandle(child_output);
    }
    if (!ok) {
        if (job) {
            CloseHandle(job);
        }
        return -1;
    }

    if (job) {
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);

    DWORD wait_ms = INFINITE;
    if (timeout > 0) {
        double ms = timeout * 1000.0;
        wait_ms = (ms >= (double)INFINITE) ? (INFINITE - 1) : (DWORD)ms;
    }

    DWORD wait_rc = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        if (timed_out) {
            *timed_out = true;
        }
        if (job) {
            TerminateJobObject(job, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        wait_rc = WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) {
            CloseHandle(job);
        }
        return -1;
    }

    if (wait_rc != WAIT_OBJECT_0) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) {
            CloseHandle(job);
        }
        return -1;
    }

    DWORD exit_code = 0;
    BOOL have_code = GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) {
        CloseHandle(job);
    }
    if (!have_code) {
        return -1;
    }
    return (int)exit_code;
#else
    char **argv = NULL;
    int argc = 0;
    if (bake_parse_cmd(cmd, &argv, &argc) != 0) {
        return -1;
    }

    int stream_fd = -1;
    if (stream) {
        stream_fd = fileno(stream);
        if (stream_fd < 0) {
            bake_free_argv(argv, argc);
            return -1;
        }
    }

    int wait_pipe[2];
    if (pipe(wait_pipe) != 0) {
        bake_free_argv(argv, argc);
        return -1;
    }

    fcntl(wait_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(wait_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(wait_pipe[0]);
        close(wait_pipe[1]);
        bake_free_argv(argv, argc);
        return -1;
    }

    if (pid == 0) {
        close(wait_pipe[0]);
        setpgid(0, 0);
        fcntl(wait_pipe[1], F_SETFD, 0);
        if (stream_fd >= 0) {
            if (dup2(stream_fd, STDOUT_FILENO) < 0 ||
                dup2(stream_fd, STDERR_FILENO) < 0)
            {
                _exit(127);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    bake_free_argv(argv, argc);
    close(wait_pipe[1]);

    bool group = setpgid(pid, pid) == 0 || errno == EACCES;

    int status = 0;
    int wait_rc = bake_wait_for_child(
        pid, wait_pipe[0], timeout, group, timed_out, &status);
    close(wait_pipe[0]);

    if (wait_rc != 0) {
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

static int bake_run_subprocess(const char *cmd, double timeout, bool *timed_out) {
    return bake_run_subprocess_to_stream(cmd, NULL, timeout, timed_out);
}

static void bake_set_failure(const char *file, int line, const char *msg) {
    g_failed = true;
    if (g_current_suite && g_current_case) {
        bake_print_status("FAIL", BAKE_COLOR_RED);
        printf(": %s.%s:%d: %s\n", g_current_suite->id, g_current_case->id, line, msg);
    } else {
        bake_print_status("FAIL", BAKE_COLOR_RED);
        printf(": %s:%d: %s\n", file, line, msg);
    }
}

static const char* bake_lookup_param(const char *name) {
    size_t len = strlen(name);
    for (int i = 0; i < g_cli_param_count; i++) {
        const char *p = g_cli_params[i];
        if (!strncmp(p, name, len) && p[len] == '=') {
            return p + len + 1;
        }
    }

    if (g_current_suite) {
        for (uint32_t i = 0; i < g_current_suite->param_count; i++) {
            bake_test_param *param = &g_current_suite->params[i];
            if (!strcmp(param->name, name)) {
                if (param->value_cur >= 0 && param->value_cur < param->value_count) {
                    return param->values[param->value_cur];
                }
            }
        }
    }

    return NULL;
}

const char* test_param(const char *name) {
    return bake_lookup_param(name);
}

static void bake_test_exit(void) {
    exit(g_flaky ? 0 : -1);
}

void test_is_flaky(void) {
    g_flaky = true;
}

void test_quarantine(const char *date) {
    const char *quarantine_date = date ? date : "unknown";
    bake_print_status("SKIP", BAKE_COLOR_YELLOW);
    if (g_current_suite && g_current_case) {
        printf(": %s.%s: test was quarantined on %s\n",
            g_current_suite->id, g_current_case->id, quarantine_date);
    } else {
        printf(": test was quarantined on %s\n", quarantine_date);
    }
    exit(BAKE_TEST_QUARANTINED);
}

void test_expect_abort(void) {
    g_expect_abort = true;
}

void test_abort(void) {
    if (g_expect_abort) {
        exit(0);
    }

    bake_set_failure(__FILE__, __LINE__, "unexpected abort");
    bake_test_exit();
}

bool _if_test_assert(bool cond, const char *cond_str, const char *file, int line) {
    if (g_current_suite) {
        g_current_suite->assert_count ++;
    }
    if (!cond) {
        char msg[512];
        snprintf(msg, sizeof(msg), "assert(%s)", cond_str);
        bake_set_failure(file, line, msg);
        return false;
    }
    return true;
}

bool _if_test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v1 != v2) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%lld) != %s (%lld)", str_v1, (long long)v1, str_v2, (long long)v2);
        bake_set_failure(file, line, msg);
        return false;
    }
    return true;
}

bool _if_test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v1 != v2) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%llu) != %s (%llu)", str_v1, (unsigned long long)v1, str_v2, (unsigned long long)v2);
        bake_set_failure(file, line, msg);
        return false;
    }
    return true;
}

bool _if_test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v1 != v2) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%s) != %s (%s)", str_v1, v1 ? "true" : "false", str_v2, v2 ? "true" : "false");
        bake_set_failure(file, line, msg);
        return false;
    }
    return true;
}

bool _if_test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    double d = fabs(v1 - v2);
    if (d > 0.000001) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%f) != %s (%f)", str_v1, v1, str_v2, v2);
        bake_set_failure(file, line, msg);
        return false;
    }
    return true;
}

bool _if_test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    bool equal = false;
    if (!v1 && !v2) {
        equal = true;
    } else if (v1 && v2 && !strcmp(v1, v2)) {
        equal = true;
    }

    if (!equal) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "%s (%s) != %s (%s)", str_v1, v1 ? v1 : "NULL", str_v2, v2 ? v2 : "NULL");
        bake_set_failure(file, line, msg);
        return false;
    }

    return true;
}

bool _if_test_null(void *v, const char *str_v, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v != NULL) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s is not NULL", str_v);
        bake_set_failure(file, line, msg);
        return false;
    }
    return true;
}

bool _if_test_not_null(void *v, const char *str_v, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v == NULL) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s is NULL", str_v);
        bake_set_failure(file, line, msg);
        return false;
    }
    return true;
}

bool _if_test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v1 != v2) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%p) != %s (%p)", str_v1, v1, str_v2, v2);
        bake_set_failure(file, line, msg);
        return false;
    }
    return true;
}

void _test_assert(bool cond, const char *cond_str, const char *file, int line) {
    if (!_if_test_assert(cond, cond_str, file, line)) { bake_test_exit(); }
}
void _test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_int(v1, v2, str_v1, str_v2, file, line)) { bake_test_exit(); }
}
void _test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_uint(v1, v2, str_v1, str_v2, file, line)) { bake_test_exit(); }
}
void _test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_bool(v1, v2, str_v1, str_v2, file, line)) { bake_test_exit(); }
}
void _test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_flt(v1, v2, str_v1, str_v2, file, line)) { bake_test_exit(); }
}
void _test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_str(v1, v2, str_v1, str_v2, file, line)) { bake_test_exit(); }
}
void _test_null(void *v, const char *str_v, const char *file, int line) {
    if (!_if_test_null(v, str_v, file, line)) { bake_test_exit(); }
}
void _test_not_null(void *v, const char *str_v, const char *file, int line) {
    if (!_if_test_not_null(v, str_v, file, line)) { bake_test_exit(); }
}
void _test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_ptr(v1, v2, str_v1, str_v2, file, line)) { bake_test_exit(); }
}

static int bake_run_case(bake_test_suite *suite, bake_test_case *tc) {
    g_current_suite = suite;
    g_current_case = tc;
    suite->assert_count = 0;
    g_expect_abort = false;
    g_failed = false;
    g_flaky = false;

    if (suite->setup) {
        suite->setup();
    }

    tc->function();

    if (suite->teardown) {
        suite->teardown();
    }

    if (g_expect_abort) {
        bake_set_failure(__FILE__, __LINE__, "expected abort");
    }

    if (g_failed) {
        if (g_flaky) {
            printf("FLAKY %s.%s\n", suite->id, tc->id);
            return 0;
        }
        return -1;
    }

    if (!suite->assert_count) {
        return BAKE_TEST_EMPTY;
    }

    return 0;
}

static const char* bake_lookup_cli_param_only(const char *name) {
    size_t len = strlen(name);
    for (int i = 0; i < g_cli_param_count; i++) {
        const char *p = g_cli_params[i];
        if (!strncmp(p, name, len) && p[len] == '=') {
            return p + len + 1;
        }
    }
    return NULL;
}

static int bake_build_case_command(
    char *cmd,
    size_t cmd_size,
    const char *exec,
    bake_test_suite *suite,
    bake_test_case *tc,
    const int32_t *param_values)
{
    int written = snprintf(cmd, cmd_size, "\"%s\" \"%s.%s\"", exec, suite->id, tc->id);
    if (written < 0 || (size_t)written >= cmd_size) {
        return -1;
    }

    for (int p = 0; p < g_cli_param_count; p++) {
        size_t used = strlen(cmd);
        if ((used + strlen(g_cli_params[p]) + 12) >= cmd_size) {
            break;
        }
        strcat(cmd, " --param ");
        strcat(cmd, g_cli_params[p]);
    }

    for (uint32_t p = 0; p < suite->param_count; p++) {
        bake_test_param *param = &suite->params[p];
        if (bake_lookup_cli_param_only(param->name)) {
            continue;
        }
        int32_t value_cur = param_values ? param_values[p] : param->value_cur;
        if (value_cur < 0 || value_cur >= param->value_count) {
            continue;
        }
        const char *value = param->values[value_cur];
        size_t used = strlen(cmd);
        if ((used + strlen(param->name) + strlen(value) + 16) >= cmd_size) {
            break;
        }
        strcat(cmd, " --param ");
        strcat(cmd, param->name);
        strcat(cmd, "=");
        strcat(cmd, value);
    }

    return 0;
}

static int bake_run_suite(const char *test_id, const char *exec, bake_test_suite *suite, int *pass_out, int *fail_out, int *empty_out) {
    int rc = 0;
    int pass = 0;
    int fail = 0;
    int empty = 0;
    char param_str[512] = {0};
    bake_append_suite_params(param_str, sizeof(param_str), suite);

    for (uint32_t i = 0; i < suite->testcase_count; i++) {
        if (g_trace) {
            bake_print_trace(suite->id, suite->testcases[i].id, param_str);
        }

        char cmd[4096];
        if (bake_build_case_command(cmd, sizeof(cmd), exec, suite, &suite->testcases[i], NULL) != 0) {
            fail ++;
            rc = -1;
            bake_record_failure(suite->id, suite->testcases[i].id, param_str, NULL);
            continue;
        }

        double timeout = bake_case_timeout(suite);
        bool timed_out = false;
        double start = bake_time_now();
        int test_rc = bake_run_subprocess(cmd, timeout, &timed_out);
        double elapsed = bake_time_now() - start;
        if (timed_out) {
            fail ++;
            rc = -1;
            g_timeout_count ++;
            bake_record_result(suite->id, suite->testcases[i].id, param_str, "timeout", elapsed);
            bake_record_failure(suite->id, suite->testcases[i].id, param_str, "timeout");
            bake_print_status("TIMEOUT", BAKE_COLOR_RED);
            printf(" %s.%s (exceeded %g seconds)\n", suite->id, suite->testcases[i].id, timeout);
            bake_print_debug_command(exec, suite, &suite->testcases[i]);
            continue;
        }

        if (test_rc == 0) {
            pass ++;
            bake_record_result(suite->id, suite->testcases[i].id, param_str, "pass", elapsed);
            continue;
        }

        if (test_rc == BAKE_TEST_QUARANTINED) {
            bake_record_result(suite->id, suite->testcases[i].id, param_str, "quarantined", elapsed);
            continue;
        }

        if (test_rc == BAKE_TEST_EMPTY) {
            empty ++;
            bake_record_result(suite->id, suite->testcases[i].id, param_str, "empty", elapsed);
            bake_print_status("EMPTY", BAKE_COLOR_YELLOW);
            printf(" %s.%s (add test statements)\n", suite->id, suite->testcases[i].id);
            bake_print_debug_command(exec, suite, &suite->testcases[i]);
            continue;
        }

        fail ++;
        rc = -1;
        bake_record_result(suite->id, suite->testcases[i].id, param_str, "fail", elapsed);
        bake_record_failure(suite->id, suite->testcases[i].id, param_str, NULL);
        bake_print_debug_command(exec, suite, &suite->testcases[i]);
    }

    bake_print_report(test_id, suite->id, param_str, pass, fail, empty);
    if (fail || empty) {
        printf("\n");
    }

    if (pass_out) {
        *pass_out += pass;
    }
    if (fail_out) {
        *fail_out += fail;
    }
    if (empty_out) {
        *empty_out += empty;
    }
    return rc;
}

static int bake_run_suite_for_params(const char *test_id, const char *exec, bake_test_suite *suite, uint32_t param, int *pass, int *fail, int *empty) {
    if (!suite->param_count || param >= suite->param_count) {
        return bake_run_suite(test_id, exec, suite, pass, fail, empty);
    }

    int rc = 0;
    bake_test_param *p = &suite->params[param];
    for (int32_t i = 0; i < p->value_count; i++) {
        p->value_cur = i;
        int suite_rc = bake_run_suite_for_params(test_id, exec, suite, param + 1, pass, fail, empty);
        if (suite_rc != 0) {
            rc = -1;
        }
    }
    return rc;
}

typedef struct bake_case_job_t {
    size_t run_index;
    bake_test_case *testcase;
    char cmd[4096];
    char *output;
    size_t output_size;
    int command_rc;
    int test_rc;
    double timeout;
    bool timed_out;
    double elapsed;
} bake_case_job_t;

typedef struct bake_suite_run_t {
    bake_test_suite *suite;
    int32_t *param_values;
    char param_str[512];
    size_t first_job;
    size_t job_count;
} bake_suite_run_t;

typedef struct bake_case_plan_t {
    bake_suite_run_t *runs;
    size_t run_count;
    size_t run_cap;
    bake_case_job_t *jobs;
    size_t job_count;
    size_t job_cap;
} bake_case_plan_t;

#if defined(_WIN32)
typedef CRITICAL_SECTION bake_jobs_mutex_t;
typedef HANDLE bake_worker_thread_t;
#else
typedef pthread_mutex_t bake_jobs_mutex_t;
typedef pthread_t bake_worker_thread_t;
#endif

typedef struct bake_case_jobs_t {
    bake_case_plan_t *plan;
    size_t next_job;
    bake_jobs_mutex_t lock;
} bake_case_jobs_t;

static void bake_jobs_mutex_init(bake_jobs_mutex_t *mutex) {
#if defined(_WIN32)
    InitializeCriticalSection(mutex);
#else
    pthread_mutex_init(mutex, NULL);
#endif
}

static void bake_jobs_mutex_lock(bake_jobs_mutex_t *mutex) {
#if defined(_WIN32)
    EnterCriticalSection(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

static void bake_jobs_mutex_unlock(bake_jobs_mutex_t *mutex) {
#if defined(_WIN32)
    LeaveCriticalSection(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

static void bake_jobs_mutex_fini(bake_jobs_mutex_t *mutex) {
#if defined(_WIN32)
    DeleteCriticalSection(mutex);
#else
    pthread_mutex_destroy(mutex);
#endif
}

static int bake_parallel_plan_counts(
    bake_test_suite *suites,
    uint32_t suite_count,
    size_t *run_count_out,
    size_t *job_count_out)
{
    size_t run_count = 0;
    size_t job_count = 0;
    size_t size_max = (size_t)-1;

    for (uint32_t s = 0; s < suite_count; s++) {
        bake_test_suite *suite = &suites[s];
        size_t suite_run_count = 1;
        for (uint32_t p = 0; p < suite->param_count; p++) {
            int32_t value_count = suite->params[p].value_count;
            if (value_count <= 0) {
                suite_run_count = 0;
                break;
            }
            if (suite_run_count > size_max / (size_t)value_count) {
                return -1;
            }
            suite_run_count *= (size_t)value_count;
        }

        if (run_count > size_max - suite_run_count) {
            return -1;
        }
        run_count += suite_run_count;

        if (suite->testcase_count &&
            suite_run_count > size_max / (size_t)suite->testcase_count)
        {
            return -1;
        }
        size_t suite_job_count = suite_run_count * (size_t)suite->testcase_count;
        if (job_count > size_max - suite_job_count) {
            return -1;
        }
        job_count += suite_job_count;
    }

    *run_count_out = run_count;
    *job_count_out = job_count;
    return 0;
}

static void bake_parallel_plan_fini(bake_case_plan_t *plan) {
    if (plan->jobs) {
        for (size_t i = 0; i < plan->job_count; i++) {
            free(plan->jobs[i].output);
        }
    }
    if (plan->runs) {
        for (size_t i = 0; i < plan->run_count; i++) {
            free(plan->runs[i].param_values);
        }
    }
    free(plan->runs);
    free(plan->jobs);
    memset(plan, 0, sizeof(*plan));
}

static int bake_parallel_plan_add_run(
    bake_case_plan_t *plan,
    const char *exec,
    bake_test_suite *suite)
{
    if (plan->run_count >= plan->run_cap) {
        return -1;
    }

    bake_suite_run_t *run = &plan->runs[plan->run_count];
    run->suite = suite;
    run->first_job = plan->job_count;
    run->job_count = suite->testcase_count;
    bake_append_suite_params(run->param_str, sizeof(run->param_str), suite);

    if (suite->param_count) {
        if ((size_t)suite->param_count > ((size_t)-1) / sizeof(int32_t)) {
            return -1;
        }
        run->param_values = malloc((size_t)suite->param_count * sizeof(int32_t));
        if (!run->param_values) {
            return -1;
        }
        for (uint32_t p = 0; p < suite->param_count; p++) {
            run->param_values[p] = suite->params[p].value_cur;
        }
    }

    size_t run_index = plan->run_count ++;
    for (uint32_t t = 0; t < suite->testcase_count; t++) {
        if (plan->job_count >= plan->job_cap) {
            return -1;
        }
        bake_case_job_t *job = &plan->jobs[plan->job_count ++];
        job->run_index = run_index;
        job->testcase = &suite->testcases[t];
        job->command_rc = bake_build_case_command(
            job->cmd, sizeof(job->cmd), exec, suite, job->testcase,
            run->param_values);
        job->test_rc = -1;
        job->timeout = bake_case_timeout(suite);
    }

    return 0;
}

static int bake_parallel_plan_add_suite(
    bake_case_plan_t *plan,
    const char *exec,
    bake_test_suite *suite,
    uint32_t param)
{
    if (!suite->param_count || param >= suite->param_count) {
        return bake_parallel_plan_add_run(plan, exec, suite);
    }

    bake_test_param *p = &suite->params[param];
    for (int32_t i = 0; i < p->value_count; i++) {
        p->value_cur = i;
        if (bake_parallel_plan_add_suite(plan, exec, suite, param + 1) != 0) {
            return -1;
        }
    }
    return 0;
}

static int bake_parallel_plan_init(
    bake_case_plan_t *plan,
    const char *exec,
    bake_test_suite *suites,
    uint32_t suite_count)
{
    memset(plan, 0, sizeof(*plan));
    if (bake_parallel_plan_counts(
        suites, suite_count, &plan->run_cap, &plan->job_cap) != 0)
    {
        return -1;
    }

    if (plan->run_cap) {
        plan->runs = calloc(plan->run_cap, sizeof(bake_suite_run_t));
        if (!plan->runs) {
            bake_parallel_plan_fini(plan);
            return -1;
        }
    }
    if (plan->job_cap) {
        plan->jobs = calloc(plan->job_cap, sizeof(bake_case_job_t));
        if (!plan->jobs) {
            bake_parallel_plan_fini(plan);
            return -1;
        }
    }

    for (uint32_t s = 0; s < suite_count; s++) {
        if (bake_parallel_plan_add_suite(plan, exec, &suites[s], 0) != 0) {
            bake_parallel_plan_fini(plan);
            return -1;
        }
    }

    if (plan->run_count != plan->run_cap || plan->job_count != plan->job_cap) {
        bake_parallel_plan_fini(plan);
        return -1;
    }
    return 0;
}

static void bake_case_job_execute(bake_case_job_t *job) {
    double start = bake_time_now();
    FILE *stream = tmpfile();
    if (!stream) {
        job->test_rc = bake_run_subprocess(job->cmd, job->timeout, &job->timed_out);
        job->elapsed = bake_time_now() - start;
        return;
    }

    job->test_rc = bake_run_subprocess_to_stream(
        job->cmd, stream, job->timeout, &job->timed_out);
    job->elapsed = bake_time_now() - start;
    fflush(stream);
    if (fseek(stream, 0, SEEK_END) == 0) {
        long end = ftell(stream);
        if (end > 0 && (long)(size_t)end == end) {
            size_t size = (size_t)end;
            job->output = malloc(size);
            if (job->output) {
                rewind(stream);
                job->output_size = fread(job->output, 1, size, stream);
            }
        }
    }
    fclose(stream);
}

static void bake_case_worker_run(bake_case_jobs_t *jobs) {
    for (;;) {
        bake_jobs_mutex_lock(&jobs->lock);
        size_t job_index = jobs->next_job;
        if (job_index < jobs->plan->job_count) {
            jobs->next_job ++;
        }
        bake_jobs_mutex_unlock(&jobs->lock);

        if (job_index >= jobs->plan->job_count) {
            break;
        }

        bake_case_job_t *job = &jobs->plan->jobs[job_index];
        if (job->command_rc == 0) {
            bake_case_job_execute(job);
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI bake_case_worker(void *arg) {
    bake_case_worker_run(arg);
    return 0;
}
#else
static void* bake_case_worker(void *arg) {
    bake_case_worker_run(arg);
    return NULL;
}
#endif

static int bake_worker_start(
    bake_worker_thread_t *thread,
    bake_case_jobs_t *jobs)
{
#if defined(_WIN32)
    *thread = CreateThread(NULL, 0, bake_case_worker, jobs, 0, NULL);
    return *thread ? 0 : -1;
#else
    return pthread_create(thread, NULL, bake_case_worker, jobs);
#endif
}

static int bake_worker_join(bake_worker_thread_t thread) {
#if defined(_WIN32)
    DWORD rc = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return rc == WAIT_OBJECT_0 ? 0 : -1;
#else
    return pthread_join(thread, NULL);
#endif
}

static void bake_parallel_apply_params(bake_suite_run_t *run) {
    for (uint32_t p = 0; p < run->suite->param_count; p++) {
        run->suite->params[p].value_cur = run->param_values[p];
    }
}

static int bake_parallel_report(
    bake_case_plan_t *plan,
    const char *test_id,
    const char *exec,
    int *pass_out,
    int *fail_out,
    int *empty_out)
{
    int rc = 0;

    for (size_t r = 0; r < plan->run_count; r++) {
        bake_suite_run_t *run = &plan->runs[r];
        bake_test_suite *suite = run->suite;
        int pass = 0;
        int fail = 0;
        int empty = 0;
        bake_parallel_apply_params(run);

        for (size_t j = 0; j < run->job_count; j++) {
            bake_case_job_t *job = &plan->jobs[run->first_job + j];
            if (g_trace) {
                bake_print_trace(suite->id, job->testcase->id, run->param_str);
            }
            if (job->output_size) {
                fwrite(job->output, 1, job->output_size, stdout);
            }
            if (job->command_rc != 0) {
                fail ++;
                rc = -1;
                bake_record_result(suite->id, job->testcase->id, run->param_str, "error", job->elapsed);
                bake_record_failure(suite->id, job->testcase->id, run->param_str, NULL);
                continue;
            }

            if (job->timed_out) {
                fail ++;
                rc = -1;
                g_timeout_count ++;
                bake_record_result(suite->id, job->testcase->id, run->param_str, "timeout", job->elapsed);
                bake_record_failure(suite->id, job->testcase->id, run->param_str, "timeout");
                bake_print_status("TIMEOUT", BAKE_COLOR_RED);
                printf(" %s.%s (exceeded %g seconds)\n",
                    suite->id, job->testcase->id, job->timeout);
                bake_print_debug_command(exec, suite, job->testcase);
                continue;
            }

            if (job->test_rc == 0) {
                pass ++;
                bake_record_result(suite->id, job->testcase->id, run->param_str, "pass", job->elapsed);
                continue;
            }
            if (job->test_rc == BAKE_TEST_QUARANTINED) {
                bake_record_result(suite->id, job->testcase->id, run->param_str, "quarantined", job->elapsed);
                continue;
            }
            if (job->test_rc == BAKE_TEST_EMPTY) {
                empty ++;
                bake_record_result(suite->id, job->testcase->id, run->param_str, "empty", job->elapsed);
                bake_print_status("EMPTY", BAKE_COLOR_YELLOW);
                printf(" %s.%s (add test statements)\n", suite->id, job->testcase->id);
                bake_print_debug_command(exec, suite, job->testcase);
                continue;
            }

            fail ++;
            rc = -1;
            bake_record_result(suite->id, job->testcase->id, run->param_str, "fail", job->elapsed);
            bake_record_failure(suite->id, job->testcase->id, run->param_str, NULL);
            bake_print_debug_command(exec, suite, job->testcase);
        }

        bake_print_report(test_id, suite->id, run->param_str, pass, fail, empty);
        if (fail || empty) {
            printf("\n");
        }
        *pass_out += pass;
        *fail_out += fail;
        *empty_out += empty;
    }

    return rc;
}

static int bake_run_cases_parallel(
    const char *test_id,
    const char *exec,
    bake_test_suite *suites,
    uint32_t suite_count,
    int jobs_count,
    int *pass,
    int *fail,
    int *empty)
{
    if (jobs_count < 2) {
        return 1;
    }

    bake_case_plan_t plan;
    if (bake_parallel_plan_init(&plan, exec, suites, suite_count) != 0) {
        return 1;
    }
    if (plan.job_count < 2) {
        bake_parallel_plan_fini(&plan);
        return 1;
    }
    if ((size_t)jobs_count > plan.job_count) {
        jobs_count = (int)plan.job_count;
    }

    bake_case_jobs_t jobs = {
        .plan = &plan,
        .next_job = 0
    };
    bake_jobs_mutex_init(&jobs.lock);

    bake_worker_thread_t *threads = calloc(
        (size_t)jobs_count, sizeof(bake_worker_thread_t));
    if (!threads) {
        bake_jobs_mutex_fini(&jobs.lock);
        bake_parallel_plan_fini(&plan);
        return 1;
    }

    int started = 0;
    for (int i = 0; i < jobs_count; i++) {
        if (bake_worker_start(&threads[i], &jobs) != 0) {
            break;
        }
        started ++;
    }
    if (!started) {
        free(threads);
        bake_jobs_mutex_fini(&jobs.lock);
        bake_parallel_plan_fini(&plan);
        return 1;
    }

    int join_rc = 0;
    for (int i = 0; i < started; i++) {
        if (bake_worker_join(threads[i]) != 0) {
            join_rc = -1;
        }
    }

    free(threads);
    bake_jobs_mutex_fini(&jobs.lock);

    int rc = bake_parallel_report(
        &plan, test_id, exec, pass, fail, empty);
    bake_parallel_plan_fini(&plan);
    if (join_rc != 0) {
        return -1;
    }
    return rc;
}

static void bake_list_tests(bake_test_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s++) {
        for (uint32_t t = 0; t < suites[s].testcase_count; t++) {
            printf("%s.%s\n", suites[s].id, suites[s].testcases[t].id);
        }
    }
}

static void bake_list_suites(bake_test_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s++) {
        printf("%s\n", suites[s].id);
    }
}

static void bake_list_commands(const char *exec, bake_test_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s++) {
        for (uint32_t t = 0; t < suites[s].testcase_count; t++) {
            printf("%s %s.%s\n", exec, suites[s].id, suites[s].testcases[t].id);
        }
    }
}

static bake_test_suite* bake_find_suite(bake_test_suite *suites, uint32_t suite_count, const char *id) {
    for (uint32_t i = 0; i < suite_count; i++) {
        if (!strcmp(suites[i].id, id)) {
            return &suites[i];
        }
    }
    return NULL;
}

static int bake_run_single_test(bake_test_suite *suites, uint32_t suite_count, const char *id) {
    const char *dot = strchr(id, '.');
    if (!dot) {
        return -1;
    }

    char suite_id[256];
    size_t suite_len = (size_t)(dot - id);
    if (suite_len >= sizeof(suite_id)) {
        return -1;
    }
    memcpy(suite_id, id, suite_len);
    suite_id[suite_len] = '\0';

    const char *case_id = dot + 1;
    bake_test_suite *suite = bake_find_suite(suites, suite_count, suite_id);
    if (!suite) {
        printf("test suite '%s' not found\n", suite_id);
        return -1;
    }

    for (uint32_t i = 0; i < suite->testcase_count; i++) {
        if (!strcmp(suite->testcases[i].id, case_id)) {
            return bake_run_case(suite, &suite->testcases[i]);
        }
    }

    printf("testcase '%s' not found\n", id);
    return -1;
}

#ifdef BAKE_TEST_COVERAGE_DIR
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

void __llvm_profile_set_filename(const char *name);

#define BAKE_TEST_COVERAGE_PATTERN BAKE_TEST_COVERAGE_DIR "/profile-%8m.profraw"

static const char *g_coverage_exclude[] = BAKE_TEST_COVERAGE_EXCLUDE;

typedef struct bake_coverage_count_t {
    long count;
    long covered;
} bake_coverage_count_t;

typedef struct bake_coverage_totals_t {
    bake_coverage_count_t lines;
    bake_coverage_count_t functions;
    bake_coverage_count_t branches;
} bake_coverage_totals_t;

typedef struct bake_coverage_fn_t {
    char *name;
    long line;
    bool uncovered;
} bake_coverage_fn_t;

typedef struct bake_coverage_file_t {
    char *path;
    bake_coverage_totals_t totals;
    bake_coverage_fn_t *fns;
    int fn_count;
    int fn_cap;
    long *ranges;
    int range_count;
    int range_cap;
    bool in_range;
} bake_coverage_file_t;

static bool bake_coverage_is_profile(const char *name) {
    size_t len = strlen(name);
    return len > 8 && !strcmp(name + len - 8, ".profraw");
}

static void bake_coverage_init(bool reset) {
    mkdir(BAKE_TEST_COVERAGE_DIR, 0755);

    DIR *dir = reset ? opendir(BAKE_TEST_COVERAGE_DIR) : NULL;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (bake_coverage_is_profile(entry->d_name)) {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s",
                    BAKE_TEST_COVERAGE_DIR, entry->d_name);
                unlink(path);
            }
        }
        closedir(dir);
    }

    setenv("LLVM_PROFILE_FILE", BAKE_TEST_COVERAGE_PATTERN, 1);
    __llvm_profile_set_filename(BAKE_TEST_COVERAGE_PATTERN);
}

static char* bake_coverage_merge_cmd(const char *profdata) {
    DIR *dir = opendir(BAKE_TEST_COVERAGE_DIR);
    if (!dir) {
        return NULL;
    }

    size_t cap = strlen(BAKE_TEST_COVERAGE_PROFDATA) + strlen(profdata) + 128;
    char *cmd = malloc(cap);
    if (!cmd) {
        closedir(dir);
        return NULL;
    }

    size_t len = (size_t)snprintf(cmd, cap,
        "\"%s\" merge -sparse -failure-mode=all -o \"%s\"",
        BAKE_TEST_COVERAGE_PROFDATA, profdata);

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!bake_coverage_is_profile(entry->d_name)) {
            continue;
        }
        size_t need = len + strlen(BAKE_TEST_COVERAGE_DIR) +
            strlen(entry->d_name) + 8;
        if (need >= cap) {
            cap = need * 2;
            char *tmp = realloc(cmd, cap);
            if (!tmp) {
                free(cmd);
                closedir(dir);
                return NULL;
            }
            cmd = tmp;
        }
        len += (size_t)snprintf(cmd + len, cap - len, " \"%s/%s\"",
            BAKE_TEST_COVERAGE_DIR, entry->d_name);
        count ++;
    }
    closedir(dir);

    if (!count) {
        free(cmd);
        return NULL;
    }
    return cmd;
}

static bool bake_coverage_excluded(const char *path) {
    for (int i = 0; g_coverage_exclude[i]; i++) {
        const char *prefix = g_coverage_exclude[i];
        size_t len = strlen(prefix);
        if (len && !strncmp(path, prefix, len) &&
            (path[len] == '/' || path[len] == '\0' || prefix[len - 1] == '/'))
        {
            return true;
        }
    }
    return false;
}

static const char* bake_coverage_fn_name(const char *name) {
    const char *colon = strrchr(name, ':');
    const char *semi = strrchr(name, ';');
    const char *sep = colon;
    if (semi && (!sep || semi > sep)) {
        sep = semi;
    }
    return sep ? sep + 1 : name;
}

static double bake_coverage_percent(const bake_coverage_count_t *c) {
    return c->count ? ((double)c->covered * 100.0) / (double)c->count : 100.0;
}

static void bake_coverage_write_count(
    FILE *f,
    const char *name,
    const bake_coverage_count_t *c)
{
    fprintf(f, "\"%s\": {\"count\": %ld, \"covered\": %ld, \"percent\": %.2f}",
        name, c->count, c->covered, bake_coverage_percent(c));
}

static void bake_coverage_write_totals(
    FILE *f,
    const char *indent,
    const bake_coverage_totals_t *t)
{
    fputs(indent, f);
    bake_coverage_write_count(f, "lines", &t->lines);
    fprintf(f, ",\n%s", indent);
    bake_coverage_write_count(f, "functions", &t->functions);
    fprintf(f, ",\n%s", indent);
    bake_coverage_write_count(f, "branches", &t->branches);
}

static void bake_coverage_add(
    bake_coverage_totals_t *dst,
    const bake_coverage_totals_t *src)
{
    dst->lines.count += src->lines.count;
    dst->lines.covered += src->lines.covered;
    dst->functions.count += src->functions.count;
    dst->functions.covered += src->functions.covered;
    dst->branches.count += src->branches.count;
    dst->branches.covered += src->branches.covered;
}

static void bake_coverage_file_reset(bake_coverage_file_t *file) {
    free(file->path);
    for (int i = 0; i < file->fn_count; i++) {
        free(file->fns[i].name);
    }
    free(file->fns);
    free(file->ranges);
    memset(file, 0, sizeof(*file));
}

static void bake_coverage_add_fn(
    bake_coverage_file_t *file,
    const char *name,
    long line)
{
    if (file->fn_count == file->fn_cap) {
        int cap = file->fn_cap ? file->fn_cap * 2 : 32;
        bake_coverage_fn_t *tmp = realloc(
            file->fns, (size_t)cap * sizeof(bake_coverage_fn_t));
        if (!tmp) {
            return;
        }
        file->fns = tmp;
        file->fn_cap = cap;
    }
    bake_coverage_fn_t *fn = &file->fns[file->fn_count ++];
    fn->name = bake_strdup(bake_coverage_fn_name(name));
    fn->line = line;
    fn->uncovered = false;
}

static void bake_coverage_mark_fn(
    bake_coverage_file_t *file,
    const char *name,
    unsigned long long hits)
{
    if (hits) {
        return;
    }
    name = bake_coverage_fn_name(name);
    for (int i = 0; i < file->fn_count; i++) {
        if (file->fns[i].name && !strcmp(file->fns[i].name, name)) {
            file->fns[i].uncovered = true;
            return;
        }
    }
}

static void bake_coverage_add_line(
    bake_coverage_file_t *file,
    long line,
    unsigned long long hits)
{
    if (hits) {
        file->in_range = false;
        return;
    }

    if (file->in_range) {
        file->ranges[file->range_count * 2 - 1] = line;
        return;
    }

    if (file->range_count == file->range_cap) {
        int cap = file->range_cap ? file->range_cap * 2 : 32;
        long *tmp = realloc(file->ranges, (size_t)cap * 2 * sizeof(long));
        if (!tmp) {
            return;
        }
        file->ranges = tmp;
        file->range_cap = cap;
    }
    file->ranges[file->range_count * 2] = line;
    file->ranges[file->range_count * 2 + 1] = line;
    file->range_count ++;
    file->in_range = true;
}

static void bake_coverage_write_file(
    FILE *f,
    const bake_coverage_file_t *file,
    bool first)
{
    fputs(first ? "\n    {\"file\": " : ",\n    {\"file\": ", f);
    bake_json_string(f, file->path);
    fputs(",\n", f);
    bake_coverage_write_totals(f, "     ", &file->totals);
    fputs(",\n     \"uncovered_lines\": [", f);
    for (int i = 0; i < file->range_count; i++) {
        fprintf(f, "%s[%ld, %ld]", i ? ", " : "",
            file->ranges[i * 2], file->ranges[i * 2 + 1]);
    }
    fputs("],\n     \"uncovered_functions\": [", f);
    bool first_fn = true;
    for (int i = 0; i < file->fn_count; i++) {
        const bake_coverage_fn_t *fn = &file->fns[i];
        if (!fn->uncovered || !fn->name) {
            continue;
        }
        fputs(first_fn ? "{\"name\": " : ", {\"name\": ", f);
        bake_json_string(f, fn->name);
        fprintf(f, ", \"line\": %ld}", fn->line);
        first_fn = false;
    }
    fputs("]}", f);
}

static int bake_coverage_parse_lcov(
    FILE *lcov,
    FILE *files_out,
    bake_coverage_totals_t *totals,
    int *file_count)
{
    bake_coverage_file_t file = {0};
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while ((line_len = getline(&line, &line_cap, lcov)) >= 0) {
        while (line_len && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[-- line_len] = '\0';
        }

        if (!strncmp(line, "SF:", 3)) {
            bake_coverage_file_reset(&file);
            file.path = bake_strdup(line + 3);
        } else if (!strncmp(line, "FN:", 3)) {
            char *end = NULL;
            long fn_line = strtol(line + 3, &end, 10);
            if (end && *end == ',') {
                bake_coverage_add_fn(&file, end + 1, fn_line);
            }
        } else if (!strncmp(line, "FNDA:", 5)) {
            char *end = NULL;
            unsigned long long hits = strtoull(line + 5, &end, 10);
            if (end && *end == ',') {
                bake_coverage_mark_fn(&file, end + 1, hits);
            }
        } else if (!strncmp(line, "DA:", 3)) {
            char *end = NULL;
            long da_line = strtol(line + 3, &end, 10);
            if (end && *end == ',') {
                bake_coverage_add_line(&file, da_line, strtoull(end + 1, NULL, 10));
            }
        } else if (!strncmp(line, "LF:", 3)) {
            file.totals.lines.count = strtol(line + 3, NULL, 10);
        } else if (!strncmp(line, "LH:", 3)) {
            file.totals.lines.covered = strtol(line + 3, NULL, 10);
        } else if (!strncmp(line, "FNF:", 4)) {
            file.totals.functions.count = strtol(line + 4, NULL, 10);
        } else if (!strncmp(line, "FNH:", 4)) {
            file.totals.functions.covered = strtol(line + 4, NULL, 10);
        } else if (!strncmp(line, "BRF:", 4)) {
            file.totals.branches.count = strtol(line + 4, NULL, 10);
        } else if (!strncmp(line, "BRH:", 4)) {
            file.totals.branches.covered = strtol(line + 4, NULL, 10);
        } else if (!strcmp(line, "end_of_record")) {
            if (file.path && !bake_coverage_excluded(file.path)) {
                bake_coverage_write_file(files_out, &file, *file_count == 0);
                bake_coverage_add(totals, &file.totals);
                (*file_count) ++;
            }
            bake_coverage_file_reset(&file);
        }
    }

    free(line);
    bake_coverage_file_reset(&file);
    return 0;
}

static char* bake_coverage_json_path(void) {
    size_t len = strlen(g_json_path);
    size_t base_len = len;
    if (len >= 5 && !strcmp(g_json_path + len - 5, ".json")) {
        base_len = len - 5;
    }
    size_t size = base_len + sizeof(".coverage.json");
    char *path = malloc(size);
    if (path) {
        snprintf(path, size, "%.*s.coverage.json", (int)base_len, g_json_path);
    }
    return path;
}

static int bake_coverage_write_report(
    const char *test_id,
    const char *lcov_path,
    const char *json_path)
{
    FILE *lcov = fopen(lcov_path, "r");
    if (!lcov) {
        printf("failed to open coverage data '%s': %s\n", lcov_path, strerror(errno));
        return -1;
    }

    FILE *files = tmpfile();
    if (!files) {
        fclose(lcov);
        printf("failed to create temporary file for coverage report\n");
        return -1;
    }

    bake_coverage_totals_t totals = {0};
    int file_count = 0;
    bake_coverage_parse_lcov(lcov, files, &totals, &file_count);
    fclose(lcov);

    FILE *f = fopen(json_path, "w");
    if (!f) {
        fclose(files);
        printf("failed to open coverage report '%s': %s\n", json_path, strerror(errno));
        return -1;
    }

    time_t now = time(NULL);
    char stamp[64] = {0};
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    fputs("{\n", f);
    fputs("  \"project\": ", f); bake_json_string(f, test_id); fputs(",\n", f);
    fputs("  \"timestamp\": ", f); bake_json_string(f, stamp); fputs(",\n", f);
    bake_coverage_write_totals(f, "  ", &totals);
    fputs(",\n  \"files\": [", f);

    rewind(files);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), files)) > 0) {
        fwrite(buf, 1, n, f);
    }
    fclose(files);

    fputs(file_count ? "\n  ]\n}\n" : "]\n}\n", f);
    fclose(f);

    printf("coverage: %.2f%% lines (%ld/%ld), %.2f%% functions (%ld/%ld), "
        "%.2f%% branches (%ld/%ld)\n",
        bake_coverage_percent(&totals.lines),
        totals.lines.covered, totals.lines.count,
        bake_coverage_percent(&totals.functions),
        totals.functions.covered, totals.functions.count,
        bake_coverage_percent(&totals.branches),
        totals.branches.covered, totals.branches.count);
    return 0;
}

static int bake_coverage_report(const char *test_id, const char *exec) {
    if (!g_json_path) {
        return 0;
    }

    int rc = -1;
    char profdata[PATH_MAX];
    char lcov_path[PATH_MAX];
    char exe[PATH_MAX];
    char *merge_cmd = NULL;
    char *export_cmd = NULL;
    char *json_path = NULL;

    snprintf(profdata, sizeof(profdata), "%s/coverage.profdata", BAKE_TEST_COVERAGE_DIR);
    snprintf(lcov_path, sizeof(lcov_path), "%s/coverage.lcov", BAKE_TEST_COVERAGE_DIR);
    if (!strchr(exec, '/') || !realpath(exec, exe)) {
        snprintf(exe, sizeof(exe), "%s", exec);
    }

    merge_cmd = bake_coverage_merge_cmd(profdata);
    if (!merge_cmd) {
        printf("no coverage data found in %s\n", BAKE_TEST_COVERAGE_DIR);
        goto cleanup;
    }

    if (bake_run_subprocess(merge_cmd, 0, NULL) != 0) {
        printf("failed to merge coverage data: %s\n", merge_cmd);
        goto cleanup;
    }

    size_t export_size = strlen(BAKE_TEST_COVERAGE_COV) + strlen(profdata) +
        strlen(exe) + 64;
    export_cmd = malloc(export_size);
    if (!export_cmd) {
        goto cleanup;
    }
    snprintf(export_cmd, export_size,
        "\"%s\" export -format=lcov -instr-profile=\"%s\" \"%s\"",
        BAKE_TEST_COVERAGE_COV, profdata, exe);

    FILE *lcov = fopen(lcov_path, "w");
    if (!lcov) {
        printf("failed to open '%s': %s\n", lcov_path, strerror(errno));
        goto cleanup;
    }
    int export_rc = bake_run_subprocess_to_stream(export_cmd, lcov, 0, NULL);
    fclose(lcov);
    if (export_rc != 0) {
        printf("failed to export coverage data: %s\n", export_cmd);
        goto cleanup;
    }

    json_path = bake_coverage_json_path();
    if (!json_path) {
        goto cleanup;
    }

    rc = bake_coverage_write_report(test_id, lcov_path, json_path);

cleanup:
    free(merge_cmd);
    free(export_cmd);
    free(json_path);
    return rc;
}
#endif

int bake_test_run(const char *test_id, int argc, char *argv[], bake_test_suite *suites, uint32_t suite_count) {
    if (!test_id || !test_id[0]) {
        test_id = "test";
    }

    const char *single_test = NULL;
    const char *suite_filter = NULL;
    int job_count = 1;

    g_cli_param_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--list-tests")) {
            bake_list_tests(suites, suite_count);
            return 0;
        }
        if (!strcmp(arg, "--list-suites")) {
            bake_list_suites(suites, suite_count);
            return 0;
        }
        if (!strcmp(arg, "--list-commands")) {
            bake_list_commands(argv[0], suites, suite_count);
            return 0;
        }
        if (!strcmp(arg, "--trace")) {
            g_trace = true;
            continue;
        }
        if (!strcmp(arg, "--param")) {
            if ((i + 1) < argc && strchr(argv[i + 1], '=')) {
                if (g_cli_param_count < BAKE_TEST_PARAM_MAX) {
                    g_cli_params[g_cli_param_count ++] = argv[i + 1];
                }
                i ++;
                continue;
            }
            printf("invalid --param argument\n");
            return -1;
        }
        if (!strcmp(arg, "-j")) {
            if ((i + 1) < argc) {
                int parsed_jobs = atoi(argv[i + 1]);
                if (parsed_jobs > 0) {
                    job_count = parsed_jobs;
                }
                i ++;
                continue;
            }
            printf("missing value for -j\n");
            return -1;
        }
        if (!strcmp(arg, "--timeout")) {
            if ((i + 1) < argc) {
                char *end = NULL;
                double parsed = strtod(argv[i + 1], &end);
                if (end && end != argv[i + 1] && !end[0] && parsed >= 0) {
                    g_cli_timeout = parsed;
                    g_cli_timeout_set = true;
                    i ++;
                    continue;
                }
                printf("invalid value for --timeout\n");
                return -1;
            }
            printf("missing value for --timeout\n");
            return -1;
        }
        if (!strcmp(arg, "--json")) {
            if ((i + 1) < argc) {
                g_json_path = argv[i + 1];
                i ++;
                continue;
            }
            printf("missing value for --json\n");
            return -1;
        }

        if (strchr(arg, '.')) {
            single_test = arg;
        } else {
            suite_filter = arg;
        }
    }

#ifdef BAKE_TEST_COVERAGE_DIR
    bake_coverage_init(!single_test);
#endif

    if (single_test) {
        return bake_run_single_test(suites, suite_count, single_test);
    }

    int pass = 0;
    int fail = 0;
    int empty = 0;
    int rc = 0;
    double start = bake_time_now();

    if (suite_filter) {
        bake_test_suite *suite = bake_find_suite(suites, suite_count, suite_filter);
        if (!suite) {
            printf("test suite '%s' not found\n", suite_filter);
            return -1;
        }
        int parallel_rc = bake_run_cases_parallel(
            test_id, argv[0], suite, 1, job_count,
            &pass, &fail, &empty);
        if (parallel_rc == 1) {
            int suite_rc = bake_run_suite_for_params(
                test_id, argv[0], suite, 0, &pass, &fail, &empty);
            if (suite_rc != 0) {
                rc = -1;
            }
        } else if (parallel_rc != 0) {
            rc = -1;
        }
        bake_print_failed_tests();
    } else {
        int parallel_rc = bake_run_cases_parallel(
            test_id, argv[0], suites, suite_count, job_count,
            &pass, &fail, &empty);
        if (parallel_rc == 1) {
            for (uint32_t s = 0; s < suite_count; s++) {
                int suite_rc = bake_run_suite_for_params(test_id, argv[0], &suites[s], 0, &pass, &fail, &empty);
                if (suite_rc != 0) {
                    rc = -1;
                }
            }
        } else if (parallel_rc != 0) {
            rc = -1;
        }
        printf("-----------------------------\n");
        bake_print_report(test_id, "all", "", pass, fail, empty);
        bake_print_failed_tests();
    }

    if (bake_write_json_report(test_id, pass, fail, empty, bake_time_now() - start) != 0) {
        rc = -1;
    }

#ifdef BAKE_TEST_COVERAGE_DIR
    if (bake_coverage_report(test_id, argv[0]) != 0) {
        rc = -1;
    }
#endif

    return rc;
}
