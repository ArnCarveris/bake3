#include "bake_bench.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define BAKE_BENCH_DEFAULT_TIMEOUT (600.0)
#define BAKE_BENCH_DEFAULT_SAMPLES (100)
#define BAKE_BENCH_DEFAULT_TIME (1.0)
#define BAKE_BENCH_DEFAULT_SAMPLE_TIME (0.010)
#define BAKE_BENCH_DEFAULT_THRESHOLD (0.05)
#define BAKE_BENCH_MAX_WARMUP_ROUNDS (30)
#define BAKE_BENCH_WARMUP_SCALE_MAX (100)
#define BAKE_BENCH_Z95 (1.959964)

typedef struct bake_bench_stats_t {
    double mean;
    double median;
    double stddev;
    double min;
    double max;
    double p95;
    double p99;
    double ci_low;
    double ci_high;
    int32_t outliers;
    int32_t outliers_severe;
} bake_bench_stats_t;

typedef struct bake_bench_result_t {
    char *suite;
    char *name;
    uint64_t iterations;
    uint64_t total_iters;
    int32_t samples;
    double *sample_ns;
    bake_bench_stats_t stats;
    int64_t items;
    char *counter_names[BAKE_BENCH_MAX_COUNTERS];
    double counter_values[BAKE_BENCH_MAX_COUNTERS];
    int32_t counter_count;
    double time_sec;
    double baseline_median_ns;
    double change;
    bool has_baseline;
    bool regressed;
    bool improved;
} bake_bench_result_t;

typedef struct bake_bench_failure_t {
    char *suite;
    char *name;
    const char *status;
    int signal;
    long long exit_code;
    double time_sec;
} bake_bench_failure_t;

typedef struct bake_bench_exit_t {
    bool timed_out;
    bool crashed;
    int signal;
    long long exit_code;
} bake_bench_exit_t;

typedef struct bake_bench_baseline_t {
    char *suite;
    char *name;
    double median_ns;
} bake_bench_baseline_t;

static const char *g_json_path = NULL;
static const char *g_baseline_path = NULL;
static const char *g_filter = NULL;
static double g_time = BAKE_BENCH_DEFAULT_TIME;
static double g_sample_time = BAKE_BENCH_DEFAULT_SAMPLE_TIME;
static double g_threshold = BAKE_BENCH_DEFAULT_THRESHOLD;
static int32_t g_samples = BAKE_BENCH_DEFAULT_SAMPLES;
static double g_timeout = BAKE_BENCH_DEFAULT_TIMEOUT;
static bool g_fail_on_regression = false;
static bool g_in_process = false;
static const char *g_child_path = NULL;

static bake_bench_result_t *g_results = NULL;
static int32_t g_result_count = 0;
static int32_t g_result_cap = 0;

static bake_bench_failure_t *g_failures = NULL;
static int32_t g_failure_count = 0;
static int32_t g_failure_cap = 0;

static bake_bench_baseline_t *g_baseline = NULL;
static int32_t g_baseline_count = 0;

static volatile uint64_t g_bench_sink = 0;

void bake_bench_keep_bytes(const void *ptr, size_t size) {
    const unsigned char *bytes = (const unsigned char*)ptr;
    uint64_t acc = g_bench_sink;
    for (size_t i = 0; i < size; i ++) {
        acc += bytes[i];
    }
    g_bench_sink = acc;
}

void bake_bench_clobber_memory(void) {
    g_bench_sink = g_bench_sink + 1;
}

static uint64_t bake_bench_now_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static char* bake_bench_strdup(const char *str) {
    size_t len = strlen(str ? str : "") + 1;
    char *copy = (char*)malloc(len);
    if (copy) {
        memcpy(copy, str ? str : "", len);
    }
    return copy;
}

static const char* bake_bench_host_os(void) {
#if defined(_WIN32)
    return "Windows";
#elif defined(__EMSCRIPTEN__)
    return "Emscripten";
#elif defined(__APPLE__)
    return "Darwin";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown";
#endif
}

static const char* bake_bench_host_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__wasm32__)
    return "wasm32";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

static int bake_bench_cpu_count(void) {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
#else
    return 1;
#endif
}

static void bake_bench_round_start(bench_t *b, uint64_t iters, uint64_t now) {
    b->round_iters = iters;
    b->iters_left = iters - 1;
    b->paused_ns = 0;
    b->paused = false;
    b->pause_start_ns = 0;
    b->round_start_ns = now;
}

static double bake_bench_round_elapsed(const bench_t *b, uint64_t now) {
    uint64_t paused = b->paused_ns;
    if (b->paused) {
        paused += now - b->pause_start_ns;
    }
    uint64_t total = now - b->round_start_ns;
    if (paused > total) {
        paused = total;
    }
    return (double)(total - paused);
}

static bool bake_bench_sample_append(bench_t *b, double ns_per_iter) {
    if (b->sample_count == b->sample_capacity) {
        int32_t cap = b->sample_capacity ? (b->sample_capacity * 2) : 64;
        double *tmp = (double*)realloc(b->samples, (size_t)cap * sizeof(double));
        if (!tmp) {
            b->out_of_memory = true;
            return false;
        }
        b->samples = tmp;
        b->sample_capacity = cap;
    }
    b->samples[b->sample_count ++] = ns_per_iter;
    return true;
}

static void bake_bench_warmup_done(bench_t *b, double per_iter, uint64_t now) {
    double per = per_iter > 0.0 ? per_iter : 1.0;
    double want = b->sample_target_ns / per;
    uint64_t iters = 1;
    if (want >= 1.0) {
        iters = (uint64_t)(want + 0.5);
    }
    b->iters_per_sample = iters;
    b->phase = BAKE_BENCH_PHASE_SAMPLE;
    b->measuring = true;
    bake_bench_round_start(b, iters, now);
}

static bool bake_bench_warmup_next(bench_t *b, uint64_t now) {
    double elapsed = bake_bench_round_elapsed(b, now);
    double per_iter = elapsed / (double)b->round_iters;
    double previous = b->warmup_estimate_ns;
    bool stable = previous > 0.0 &&
        fabs(per_iter - previous) <= (0.1 * previous);
    bool long_enough = elapsed >= b->sample_target_ns;
    bool out_of_time = (double)(now - b->run_start_ns) >= b->warmup_budget_ns;

    b->warmup_estimate_ns = per_iter;
    b->warmup_rounds ++;

    if ((long_enough && stable) || out_of_time ||
        b->warmup_rounds >= BAKE_BENCH_MAX_WARMUP_ROUNDS)
    {
        bake_bench_warmup_done(b, per_iter, now);
        return true;
    }

    uint64_t next = b->round_iters;
    if (!long_enough) {
        double scale = elapsed > 0.0 ? (b->sample_target_ns / elapsed) : 0.0;
        if (scale < 2.0) {
            scale = 2.0;
        }
        if (scale > BAKE_BENCH_WARMUP_SCALE_MAX) {
            scale = BAKE_BENCH_WARMUP_SCALE_MAX;
        }
        next = (uint64_t)((double)b->round_iters * scale);
        if (next <= b->round_iters) {
            next = b->round_iters + 1;
        }
    }

    bake_bench_round_start(b, next, bake_bench_now_ns());
    return true;
}

static bool bake_bench_sample_next(bench_t *b, uint64_t now) {
    double elapsed = bake_bench_round_elapsed(b, now);
    b->total_iters += b->round_iters;

    if (!bake_bench_sample_append(b, elapsed / (double)b->round_iters)) {
        b->phase = BAKE_BENCH_PHASE_DONE;
        return false;
    }

    bool budget_spent = (double)(now - b->run_start_ns) >= b->budget_ns;
    if (b->sample_count >= b->target_samples || budget_spent) {
        b->phase = BAKE_BENCH_PHASE_DONE;
        return false;
    }

    bake_bench_round_start(b, b->iters_per_sample, bake_bench_now_ns());
    return true;
}

bool bake_bench_next(bench_t *b) {
    uint64_t now = bake_bench_now_ns();

    if (b->phase == BAKE_BENCH_PHASE_INIT) {
        b->run_start_ns = now;
        b->phase = BAKE_BENCH_PHASE_WARMUP;
        bake_bench_round_start(b, 1, bake_bench_now_ns());
        return true;
    }

    if (b->phase == BAKE_BENCH_PHASE_WARMUP) {
        return bake_bench_warmup_next(b, now);
    }

    if (b->phase == BAKE_BENCH_PHASE_SAMPLE) {
        return bake_bench_sample_next(b, now);
    }

    return false;
}

void bench_pause(bench_t *b) {
    if (b->paused) {
        return;
    }
    b->paused = true;
    b->pause_start_ns = bake_bench_now_ns();
}

void bench_resume(bench_t *b) {
    if (!b->paused) {
        return;
    }
    b->paused_ns += bake_bench_now_ns() - b->pause_start_ns;
    b->paused = false;
}

void bench_counter(bench_t *b, const char *name, double value) {
    if (!b->measuring || !name) {
        return;
    }

    for (int32_t i = 0; i < b->counter_count; i ++) {
        if (!strcmp(b->counters[i].name, name)) {
            b->counters[i].value += value;
            return;
        }
    }

    if (b->counter_count == BAKE_BENCH_MAX_COUNTERS) {
        return;
    }

    b->counters[b->counter_count].name = name;
    b->counters[b->counter_count].value = value;
    b->counter_count ++;
}

void bench_set_items(bench_t *b, int64_t items) {
    b->items = items;
}

uint64_t bench_iterations(const bench_t *b) {
    return b->total_iters;
}

static int bake_bench_cmp_double(const void *a, const void *b) {
    double lhs = *(const double*)a;
    double rhs = *(const double*)b;
    if (lhs < rhs) {
        return -1;
    }
    return lhs > rhs ? 1 : 0;
}

static double bake_bench_percentile(const double *sorted, int32_t count, double p) {
    if (count <= 0) {
        return 0.0;
    }
    if (count == 1) {
        return sorted[0];
    }

    double rank = p * (double)(count - 1);
    int32_t lo = (int32_t)rank;
    if (lo < 0) {
        lo = 0;
    }
    if (lo > (count - 1)) {
        lo = count - 1;
    }
    int32_t hi = (lo + 1) < count ? (lo + 1) : (count - 1);
    double frac = rank - (double)lo;
    return sorted[lo] + ((sorted[hi] - sorted[lo]) * frac);
}

static void bake_bench_stats(
    const double *samples,
    int32_t count,
    bake_bench_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (count <= 0) {
        return;
    }

    double *sorted = (double*)malloc((size_t)count * sizeof(double));
    if (!sorted) {
        return;
    }
    memcpy(sorted, samples, (size_t)count * sizeof(double));
    qsort(sorted, (size_t)count, sizeof(double), bake_bench_cmp_double);

    double sum = 0.0;
    for (int32_t i = 0; i < count; i ++) {
        sum += sorted[i];
    }
    out->mean = sum / (double)count;

    double variance = 0.0;
    for (int32_t i = 0; i < count; i ++) {
        double delta = sorted[i] - out->mean;
        variance += delta * delta;
    }
    if (count > 1) {
        out->stddev = sqrt(variance / (double)(count - 1));
    }

    out->min = sorted[0];
    out->max = sorted[count - 1];
    out->median = bake_bench_percentile(sorted, count, 0.5);
    out->p95 = bake_bench_percentile(sorted, count, 0.95);
    out->p99 = bake_bench_percentile(sorted, count, 0.99);

    double error = BAKE_BENCH_Z95 * out->stddev / sqrt((double)count);
    out->ci_low = out->mean - error;
    out->ci_high = out->mean + error;
    if (out->ci_low < 0.0) {
        out->ci_low = 0.0;
    }

    double q1 = bake_bench_percentile(sorted, count, 0.25);
    double q3 = bake_bench_percentile(sorted, count, 0.75);
    double iqr = q3 - q1;
    double mild_low = q1 - (1.5 * iqr);
    double mild_high = q3 + (1.5 * iqr);
    double severe_low = q1 - (3.0 * iqr);
    double severe_high = q3 + (3.0 * iqr);

    for (int32_t i = 0; i < count; i ++) {
        if (sorted[i] < mild_low || sorted[i] > mild_high) {
            out->outliers ++;
        }
        if (sorted[i] < severe_low || sorted[i] > severe_high) {
            out->outliers_severe ++;
        }
    }

    free(sorted);
}

static void bake_bench_result_fini(bake_bench_result_t *result) {
    free(result->suite);
    free(result->name);
    free(result->sample_ns);
    for (int32_t i = 0; i < result->counter_count; i ++) {
        free(result->counter_names[i]);
    }
    memset(result, 0, sizeof(*result));
}

static void bake_bench_results_fini(void) {
    for (int32_t i = 0; i < g_result_count; i ++) {
        bake_bench_result_fini(&g_results[i]);
    }
    free(g_results);
    g_results = NULL;
    g_result_count = 0;
    g_result_cap = 0;
}

static bake_bench_result_t* bake_bench_result_append(void) {
    if (g_result_count == g_result_cap) {
        int32_t cap = g_result_cap ? (g_result_cap * 2) : 16;
        bake_bench_result_t *tmp = (bake_bench_result_t*)realloc(
            g_results, (size_t)cap * sizeof(bake_bench_result_t));
        if (!tmp) {
            return NULL;
        }
        g_results = tmp;
        g_result_cap = cap;
    }

    bake_bench_result_t *result = &g_results[g_result_count ++];
    memset(result, 0, sizeof(*result));
    return result;
}

static const char* bake_bench_scan_value(const char *ptr) {
    const char *colon = strchr(ptr, ':');
    if (!colon) {
        return NULL;
    }
    colon ++;
    while (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r') {
        colon ++;
    }
    return colon;
}

static char* bake_bench_scan_string(const char *ptr) {
    const char *value = bake_bench_scan_value(ptr);
    if (!value || *value != '"') {
        return NULL;
    }
    value ++;

    const char *end = value;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1]) {
            end += 2;
            continue;
        }
        end ++;
    }

    size_t len = (size_t)(end - value);
    char *out = (char*)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, value, len);
    out[len] = '\0';
    return out;
}

static double bake_bench_scan_number(const char *ptr) {
    const char *value = bake_bench_scan_value(ptr);
    if (!value) {
        return -1.0;
    }
    return strtod(value, NULL);
}

static char* bake_bench_file_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    char *text = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size >= 0) {
            rewind(f);
            text = (char*)malloc((size_t)size + 1);
            if (text) {
                size_t read = fread(text, 1, (size_t)size, f);
                text[read] = '\0';
            }
        }
    }

    fclose(f);
    return text;
}

static int bake_bench_baseline_load(const char *path) {
    char *text = bake_bench_file_read(path);
    if (!text) {
        printf("failed to read baseline '%s': %s\n", path, strerror(errno));
        return -1;
    }

    int32_t capacity = 16;
    g_baseline = (bake_bench_baseline_t*)malloc(
        (size_t)capacity * sizeof(bake_bench_baseline_t));
    if (!g_baseline) {
        free(text);
        return -1;
    }

    const char *ptr = text;
    while ((ptr = strstr(ptr, "\"suite\"")) != NULL) {
        const char *case_key = strstr(ptr, "\"case\"");
        const char *median_key = strstr(ptr, "\"median_ns\"");
        if (!case_key || !median_key) {
            break;
        }

        char *suite = bake_bench_scan_string(ptr);
        char *name = bake_bench_scan_string(case_key);
        double median = bake_bench_scan_number(median_key);
        ptr = median_key + 1;

        if (!suite || !name || median < 0.0) {
            free(suite);
            free(name);
            continue;
        }

        if (g_baseline_count == capacity) {
            capacity *= 2;
            bake_bench_baseline_t *tmp = (bake_bench_baseline_t*)realloc(
                g_baseline, (size_t)capacity * sizeof(bake_bench_baseline_t));
            if (!tmp) {
                free(suite);
                free(name);
                break;
            }
            g_baseline = tmp;
        }

        g_baseline[g_baseline_count].suite = suite;
        g_baseline[g_baseline_count].name = name;
        g_baseline[g_baseline_count].median_ns = median;
        g_baseline_count ++;
    }

    free(text);
    return 0;
}

static void bake_bench_baseline_fini(void) {
    for (int32_t i = 0; i < g_baseline_count; i ++) {
        free(g_baseline[i].suite);
        free(g_baseline[i].name);
    }
    free(g_baseline);
    g_baseline = NULL;
    g_baseline_count = 0;
}

static const bake_bench_baseline_t* bake_bench_baseline_find(
    const char *suite,
    const char *name)
{
    for (int32_t i = 0; i < g_baseline_count; i ++) {
        if (!strcmp(g_baseline[i].suite, suite) &&
            !strcmp(g_baseline[i].name, name))
        {
            return &g_baseline[i];
        }
    }
    return NULL;
}

static void bake_bench_apply_baseline(bake_bench_result_t *result) {
    const bake_bench_baseline_t *entry =
        bake_bench_baseline_find(result->suite, result->name);
    if (!entry || entry->median_ns <= 0.0) {
        return;
    }

    result->has_baseline = true;
    result->baseline_median_ns = entry->median_ns;
    result->change = (result->stats.median - entry->median_ns) / entry->median_ns;
    if (result->change > g_threshold) {
        result->regressed = true;
    } else if (result->change < -g_threshold) {
        result->improved = true;
    }
}

static void bake_bench_print_ns(double value) {
    if (value < 1000.0) {
        printf("%.3f", value);
    } else if (value < 1000000.0) {
        printf("%.1f", value);
    } else {
        printf("%.0f", value);
    }
}

static void bake_bench_print_result(const bake_bench_result_t *result) {
    char label[256];
    snprintf(label, sizeof(label), "%s.%s", result->suite, result->name);
    printf("%-38s ", label);
    bake_bench_print_ns(result->stats.median);
    printf(" ns/iter  ci95 [");
    bake_bench_print_ns(result->stats.ci_low);
    printf(", ");
    bake_bench_print_ns(result->stats.ci_high);
    printf("]  iters %llu  samples %d  outliers %d",
        (unsigned long long)result->iterations,
        result->samples,
        result->stats.outliers);

    if (result->items > 0 && result->stats.mean > 0.0) {
        double per_sec = (double)result->items * 1e9 / result->stats.mean;
        printf("  %.3f M items/s", per_sec / 1e6);
    }

    if (result->has_baseline) {
        printf("  %+.1f%% vs baseline", result->change * 100.0);
        if (result->regressed) {
            printf(" (regression)");
        } else if (result->improved) {
            printf(" (improvement)");
        }
    }

    printf("\n");

    for (int32_t i = 0; i < result->counter_count; i ++) {
        double total = result->counter_values[i];
        double per_iter = result->total_iters ?
            (total / (double)result->total_iters) : 0.0;
        printf("%-38s   %s: %.6g total, %.6g per iter\n",
            "", result->counter_names[i], total, per_iter);
    }
}

static void bake_bench_json_string(FILE *f, const char *str) {
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

static void bake_bench_write_counters(FILE *f, const bake_bench_result_t *r) {
    fputs("      \"counters\": [", f);
    for (int32_t i = 0; i < r->counter_count; i ++) {
        double per_iter = r->total_iters ?
            (r->counter_values[i] / (double)r->total_iters) : 0.0;
        fputs(i ? ", {\"name\": " : "{\"name\": ", f);
        bake_bench_json_string(f, r->counter_names[i]);
        fprintf(f, ", \"total\": %.6f, \"per_iter\": %.6f}",
            r->counter_values[i], per_iter);
    }
    fputs("],\n", f);
}

static void bake_bench_write_samples(FILE *f, const bake_bench_result_t *r) {
    fputs("      \"sample_ns\": [", f);
    for (int32_t i = 0; i < r->samples; i ++) {
        fprintf(f, "%s%.6f", i ? ", " : "", r->sample_ns[i]);
    }
    fputs("]\n", f);
}

static void bake_bench_write_result(FILE *f, const bake_bench_result_t *r, bool last) {
    const bake_bench_stats_t *s = &r->stats;

    fputs("    {\n", f);
    fputs("      \"suite\": ", f); bake_bench_json_string(f, r->suite); fputs(",\n", f);
    fputs("      \"case\": ", f); bake_bench_json_string(f, r->name); fputs(",\n", f);
    fputs("      \"status\": \"ok\",\n", f);
    fprintf(f, "      \"iterations\": %llu,\n", (unsigned long long)r->iterations);
    fprintf(f, "      \"samples\": %d,\n", r->samples);
    fprintf(f, "      \"total_iterations\": %llu,\n", (unsigned long long)r->total_iters);
    fprintf(f, "      \"mean_ns\": %.6f,\n", s->mean);
    fprintf(f, "      \"median_ns\": %.6f,\n", s->median);
    fprintf(f, "      \"stddev_ns\": %.6f,\n", s->stddev);
    fprintf(f, "      \"min_ns\": %.6f,\n", s->min);
    fprintf(f, "      \"max_ns\": %.6f,\n", s->max);
    fprintf(f, "      \"p95_ns\": %.6f,\n", s->p95);
    fprintf(f, "      \"p99_ns\": %.6f,\n", s->p99);
    fprintf(f, "      \"ci_low_ns\": %.6f,\n", s->ci_low);
    fprintf(f, "      \"ci_high_ns\": %.6f,\n", s->ci_high);
    fputs("      \"ci_level\": 0.95,\n", f);
    fputs("      \"ci_method\": \"normal-approx-stddev\",\n", f);
    fprintf(f, "      \"outliers\": %d,\n", s->outliers);
    fprintf(f, "      \"outliers_severe\": %d,\n", s->outliers_severe);
    fprintf(f, "      \"items_per_iter\": %lld,\n", (long long)r->items);
    if (r->items > 0 && s->mean > 0.0) {
        fprintf(f, "      \"items_per_sec\": %.6f,\n",
            (double)r->items * 1e9 / s->mean);
    }
    if (r->has_baseline) {
        fprintf(f, "      \"baseline_median_ns\": %.6f,\n", r->baseline_median_ns);
        fprintf(f, "      \"change\": %.6f,\n", r->change);
    }
    fprintf(f, "      \"time_sec\": %.6f,\n", r->time_sec);
    bake_bench_write_counters(f, r);
    bake_bench_write_samples(f, r);
    fprintf(f, "    }%s\n", last ? "" : ",");
}

static void bake_bench_write_failure(FILE *f, const bake_bench_failure_t *r, bool last) {
    fputs("    {\"suite\": ", f); bake_bench_json_string(f, r->suite);
    fputs(", \"case\": ", f); bake_bench_json_string(f, r->name);
    fputs(", \"status\": ", f); bake_bench_json_string(f, r->status);
    if (r->signal) {
        fprintf(f, ", \"signal\": %d", r->signal);
    }
    if (r->exit_code) {
        fprintf(f, ", \"exit_code\": %lld", r->exit_code);
    }
    fprintf(f, ", \"time_sec\": %.6f}%s\n", r->time_sec, last ? "" : ",");
}

static int bake_bench_write_json(const char *bench_id, double elapsed) {
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
    fputs("  \"project\": ", f); bake_bench_json_string(f, bench_id); fputs(",\n", f);
    fputs("  \"tool\": \"bake3\",\n", f);
    fputs("  \"tool_version\": ", f);
    bake_bench_json_string(f, BAKE_BENCH_VERSION);
    fputs(",\n", f);
    fputs("  \"timestamp\": ", f); bake_bench_json_string(f, stamp); fputs(",\n", f);
    fputs("  \"host\": {\"os\": ", f);
    bake_bench_json_string(f, bake_bench_host_os());
    fputs(", \"arch\": ", f);
    bake_bench_json_string(f, bake_bench_host_arch());
    fprintf(f, ", \"cpu_count\": %d},\n", bake_bench_cpu_count());
    fprintf(f, "  \"samples\": %d,\n", g_samples);
    fprintf(f, "  \"time_budget_sec\": %.6f,\n", g_time);
    fprintf(f, "  \"sample_target_sec\": %.6f,\n", g_sample_time);
    fputs("  \"isolation\": ", f);
    bake_bench_json_string(f, g_in_process ? "none" : "process");
    fputs(",\n", f);
    fprintf(f, "  \"cases\": %d,\n", g_result_count);
    fprintf(f, "  \"failed\": %d,\n", g_failure_count);
    fprintf(f, "  \"time_sec\": %.6f,\n", elapsed);
    fputs("  \"benchmarks\": [\n", f);
    for (int32_t i = 0; i < g_result_count; i ++) {
        bake_bench_write_result(f, &g_results[i], (i + 1) == g_result_count);
    }
    fputs("  ],\n", f);
    fputs("  \"failures\": [\n", f);
    for (int32_t i = 0; i < g_failure_count; i ++) {
        bake_bench_write_failure(f, &g_failures[i], (i + 1) == g_failure_count);
    }
    fputs("  ]\n}\n", f);
    fclose(f);
    return 0;
}

static bool bake_bench_case_selected(
    const char *suite,
    const char *name,
    const char *suite_filter,
    const char *single)
{
    if (suite_filter && strcmp(suite_filter, suite)) {
        return false;
    }

    char label[256];
    snprintf(label, sizeof(label), "%s.%s", suite, name);

    if (single && strcmp(single, label)) {
        return false;
    }

    if (g_filter && !strstr(label, g_filter)) {
        return false;
    }

    return true;
}

static char g_timeout_msg[256];
static size_t g_timeout_msg_len = 0;

#if defined(_WIN32)
static HANDLE g_timeout_timer = NULL;

static VOID CALLBACK bake_bench_timeout_fired(PVOID arg, BOOLEAN fired) {
    (void)arg;
    (void)fired;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), g_timeout_msg,
        (DWORD)g_timeout_msg_len, &written, NULL);
    ExitProcess(1);
}
#else
static void bake_bench_timeout_fired(int sig) {
    (void)sig;
    ssize_t written = write(STDOUT_FILENO, g_timeout_msg, g_timeout_msg_len);
    (void)written;
    _exit(1);
}
#endif

static void bake_bench_timeout_arm(const char *suite, const char *name) {
    if (g_timeout <= 0) {
        return;
    }

    int len = snprintf(g_timeout_msg, sizeof(g_timeout_msg),
        "TIMEOUT %s.%s (exceeded %g seconds)\n", suite, name, g_timeout);
    g_timeout_msg_len = (len > 0) ? (size_t)len : 0;
    fflush(stdout);

#if defined(_WIN32)
    CreateTimerQueueTimer(&g_timeout_timer, NULL, bake_bench_timeout_fired,
        NULL, (DWORD)(g_timeout * 1000.0), 0, WT_EXECUTEDEFAULT);
#else
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = bake_bench_timeout_fired;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);

    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_sec = (time_t)g_timeout;
    timer.it_value.tv_usec = (suseconds_t)(
        (g_timeout - (double)(time_t)g_timeout) * 1e6);
    setitimer(ITIMER_REAL, &timer, NULL);
#endif
}

static void bake_bench_timeout_disarm(void) {
    if (g_timeout <= 0) {
        return;
    }

#if defined(_WIN32)
    if (g_timeout_timer) {
        DeleteTimerQueueTimer(NULL, g_timeout_timer, NULL);
        g_timeout_timer = NULL;
    }
#else
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_REAL, &timer, NULL);
#endif
}

static double bake_bench_measure(
    bake_bench_suite *suite,
    bake_bench_case *benchcase,
    bench_t *b)
{
    memset(b, 0, sizeof(*b));
    b->phase = BAKE_BENCH_PHASE_INIT;
    b->target_samples = g_samples;
    b->sample_target_ns = g_sample_time * 1e9;
    b->budget_ns = g_time * 1e9;
    b->warmup_budget_ns = b->budget_ns * 0.25;
    if (b->warmup_budget_ns > 1e8) {
        b->warmup_budget_ns = 1e8;
    }
    if (b->warmup_budget_ns < (b->sample_target_ns * 3.0)) {
        b->warmup_budget_ns = b->sample_target_ns * 3.0;
    }

    uint64_t start = bake_bench_now_ns();
    if (suite->setup) {
        suite->setup();
    }
    benchcase->function(b);
    if (suite->teardown) {
        suite->teardown();
    }
    return (double)(bake_bench_now_ns() - start) / 1e9;
}

static int bake_bench_check(
    const char *suite,
    const char *name,
    const bench_t *b)
{
    if (b->out_of_memory) {
        printf("%s.%s: out of memory while collecting samples\n", suite, name);
        return -1;
    }
    if (!b->sample_count) {
        printf("%s.%s: no samples collected (add a 'while (bench_iter(b))' loop)\n",
            suite, name);
        return -1;
    }
    return 0;
}

static void bake_bench_result_finish(bake_bench_result_t *result) {
    bake_bench_stats(result->sample_ns, result->samples, &result->stats);
    bake_bench_apply_baseline(result);
    bake_bench_print_result(result);
}

static int bake_bench_run_case_in_process(
    bake_bench_suite *suite,
    bake_bench_case *benchcase)
{
    bench_t b;
    bake_bench_timeout_arm(suite->id, benchcase->id);
    double elapsed = bake_bench_measure(suite, benchcase, &b);
    bake_bench_timeout_disarm();

    int rc = bake_bench_check(suite->id, benchcase->id, &b);
    if (!rc) {
        bake_bench_result_t *result = bake_bench_result_append();
        if (!result) {
            rc = -1;
        } else {
            result->suite = bake_bench_strdup(suite->id);
            result->name = bake_bench_strdup(benchcase->id);
            result->iterations = b.iters_per_sample;
            result->total_iters = b.total_iters;
            result->samples = b.sample_count;
            result->sample_ns = b.samples;
            result->items = b.items;
            result->time_sec = elapsed;
            b.samples = NULL;
            for (int32_t i = 0; i < b.counter_count; i ++) {
                result->counter_names[i] = bake_bench_strdup(b.counters[i].name);
                result->counter_values[i] = b.counters[i].value;
            }
            result->counter_count = b.counter_count;
            bake_bench_result_finish(result);
        }
    }

    free(b.samples);
    return rc;
}

static int bake_bench_result_write(
    const char *path,
    const bench_t *b,
    double elapsed)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("failed to write benchcase result '%s': %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(f, "iterations %llu\n", (unsigned long long)b->iters_per_sample);
    fprintf(f, "total_iterations %llu\n", (unsigned long long)b->total_iters);
    fprintf(f, "items %lld\n", (long long)b->items);
    fprintf(f, "time_sec %.17g\n", elapsed);
    for (int32_t i = 0; i < b->counter_count; i ++) {
        fprintf(f, "counter %.17g ", b->counters[i].value);
        for (const char *p = b->counters[i].name; *p; p ++) {
            fputc((*p == '\n' || *p == '\r') ? ' ' : *p, f);
        }
        fputc('\n', f);
    }
    for (int32_t i = 0; i < b->sample_count; i ++) {
        fprintf(f, "sample %.17g\n", b->samples[i]);
    }
    fputs("end\n", f);

    int rc = ferror(f) ? -1 : 0;
    if (fclose(f) != 0) {
        rc = -1;
    }
    return rc;
}

static int bake_bench_result_read(const char *path, bake_bench_result_t *result) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    char line[1024];
    int32_t capacity = 0;
    bool complete = false;
    bool ok = true;

    while (ok && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[-- len] = '\0';
        }

        if (!strncmp(line, "sample ", 7)) {
            if (result->samples == capacity) {
                capacity = capacity ? (capacity * 2) : 64;
                double *tmp = (double*)realloc(
                    result->sample_ns, (size_t)capacity * sizeof(double));
                if (!tmp) {
                    ok = false;
                    break;
                }
                result->sample_ns = tmp;
            }
            result->sample_ns[result->samples ++] = strtod(line + 7, NULL);
        } else if (!strncmp(line, "counter ", 8)) {
            char *name = NULL;
            double value = strtod(line + 8, &name);
            if (name && *name == ' ') {
                name ++;
            }
            if (result->counter_count < BAKE_BENCH_MAX_COUNTERS) {
                result->counter_names[result->counter_count] =
                    bake_bench_strdup(name ? name : "");
                result->counter_values[result->counter_count] = value;
                result->counter_count ++;
            }
        } else if (!strncmp(line, "iterations ", 11)) {
            result->iterations = strtoull(line + 11, NULL, 10);
        } else if (!strncmp(line, "total_iterations ", 17)) {
            result->total_iters = strtoull(line + 17, NULL, 10);
        } else if (!strncmp(line, "items ", 6)) {
            result->items = strtoll(line + 6, NULL, 10);
        } else if (!strncmp(line, "time_sec ", 9)) {
            result->time_sec = strtod(line + 9, NULL);
        } else if (!strcmp(line, "end")) {
            complete = true;
        }
    }

    fclose(f);
    return (ok && complete && result->samples > 0) ? 0 : -1;
}

static int bake_bench_run_child(
    bake_bench_suite *suite,
    bake_bench_case *benchcase)
{
    bench_t b;
    double elapsed = bake_bench_measure(suite, benchcase, &b);
    int rc = bake_bench_check(suite->id, benchcase->id, &b);
    if (!rc) {
        rc = bake_bench_result_write(g_child_path, &b, elapsed);
    }
    free(b.samples);
    fflush(stdout);
    return rc;
}

static char* bake_bench_temp_path(void) {
#if defined(_WIN32)
    char dir[MAX_PATH];
    char path[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, dir);
    if (!len || len >= MAX_PATH) {
        return NULL;
    }
    if (!GetTempFileNameA(dir, "bkb", 0, path)) {
        return NULL;
    }
    return bake_bench_strdup(path);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) {
        dir = "/tmp";
    }
    size_t dir_len = strlen(dir);
    while (dir_len > 1 && dir[dir_len - 1] == '/') {
        dir_len --;
    }
    size_t size = dir_len + 32;
    char *path = (char*)malloc(size);
    if (!path) {
        return NULL;
    }
    snprintf(path, size, "%.*s/bake_bench_XXXXXX", (int)dir_len, dir);
    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }
    close(fd);
    return path;
#endif
}

static void bake_bench_temp_remove(const char *path) {
#if defined(_WIN32)
    DeleteFileA(path);
#else
    unlink(path);
#endif
}

#if defined(_WIN32)
static void bake_bench_cmdline_append(char *buf, size_t size, const char *arg) {
    size_t len = strlen(buf);
    if (len && (len + 1) < size) {
        buf[len ++] = ' ';
        buf[len] = '\0';
    }
    if ((len + 1) < size) {
        buf[len ++] = '"';
    }
    for (const char *p = arg; *p && (len + 2) < size; p ++) {
        if (*p == '"') {
            buf[len ++] = '\\';
        }
        buf[len ++] = *p;
    }
    if ((len + 1) < size) {
        buf[len ++] = '"';
    }
    buf[len < size ? len : (size - 1)] = '\0';
}

static int bake_bench_spawn(
    char *const argv[],
    double timeout,
    bake_bench_exit_t *out)
{
    char cmd[8192] = {0};
    for (int i = 0; argv[i]; i ++) {
        bake_bench_cmdline_append(cmd, sizeof(cmd), argv[i]);
    }

    fflush(stdout);
    fflush(stderr);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED,
        NULL, NULL, &si, &pi))
    {
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
        out->timed_out = true;
        if (job) {
            TerminateJobObject(job, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
    }

    DWORD exit_code = 0;
    BOOL have_code = GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) {
        CloseHandle(job);
    }

    if (out->timed_out) {
        return 0;
    }
    if (!have_code) {
        return -1;
    }

    out->exit_code = (long long)exit_code;
    if (exit_code >= 0xC0000000u) {
        out->crashed = true;
    }
    return 0;
}
#else
static int bake_bench_spawn(
    char *const argv[],
    double timeout,
    bake_bench_exit_t *out)
{
    fflush(stdout);
    fflush(stderr);

    int wait_pipe[2];
    if (pipe(wait_pipe) != 0) {
        return -1;
    }
    fcntl(wait_pipe[0], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(wait_pipe[0]);
        close(wait_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(wait_pipe[0]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(wait_pipe[1]);

    uint64_t start = bake_bench_now_ns();
    for (;;) {
        int wait_ms = -1;
        if (timeout > 0) {
            double remaining = timeout -
                ((double)(bake_bench_now_ns() - start) / 1e9);
            if (remaining <= 0) {
                out->timed_out = true;
                kill(pid, SIGKILL);
                break;
            }
            double ms = remaining * 1000.0 + 1.0;
            wait_ms = ms > (double)INT_MAX ? INT_MAX : (int)ms;
        }

        struct pollfd pfd = { .fd = wait_pipe[0], .events = POLLIN, .revents = 0 };
        int poll_rc = poll(&pfd, 1, wait_ms);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (!poll_rc) {
            continue;
        }

        char buf[64];
        ssize_t count = read(wait_pipe[0], buf, sizeof(buf));
        if (count > 0) {
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        break;
    }
    close(wait_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    if (out->timed_out) {
        return 0;
    }
    if (WIFSIGNALED(status)) {
        out->crashed = true;
        out->signal = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        out->exit_code = WEXITSTATUS(status);
    } else {
        out->crashed = true;
    }
    return 0;
}
#endif

static void bake_bench_failure_add(
    const char *suite,
    const char *name,
    const char *status,
    const bake_bench_exit_t *exit_info,
    double time_sec)
{
    if (g_failure_count == g_failure_cap) {
        int32_t cap = g_failure_cap ? (g_failure_cap * 2) : 8;
        bake_bench_failure_t *tmp = (bake_bench_failure_t*)realloc(
            g_failures, (size_t)cap * sizeof(bake_bench_failure_t));
        if (!tmp) {
            return;
        }
        g_failures = tmp;
        g_failure_cap = cap;
    }

    bake_bench_failure_t *failure = &g_failures[g_failure_count ++];
    memset(failure, 0, sizeof(*failure));
    failure->suite = bake_bench_strdup(suite);
    failure->name = bake_bench_strdup(name);
    failure->status = status;
    failure->time_sec = time_sec;
    if (exit_info) {
        failure->signal = exit_info->signal;
        failure->exit_code = exit_info->exit_code;
    }
}

static void bake_bench_failures_fini(void) {
    for (int32_t i = 0; i < g_failure_count; i ++) {
        free(g_failures[i].suite);
        free(g_failures[i].name);
    }
    free(g_failures);
    g_failures = NULL;
    g_failure_count = 0;
    g_failure_cap = 0;
}

static void bake_bench_print_crash(
    const char *suite,
    const char *name,
    const bake_bench_exit_t *exit_info)
{
#if defined(_WIN32)
    printf("CRASH %s.%s (exit code 0x%08llX)\n", suite, name,
        (unsigned long long)exit_info->exit_code);
#else
    if (exit_info->signal) {
        const char *desc = strsignal(exit_info->signal);
        printf("CRASH %s.%s (signal %d: %s)\n", suite, name,
            exit_info->signal, desc ? desc : "unknown");
    } else {
        printf("CRASH %s.%s\n", suite, name);
    }
#endif
}

static int bake_bench_run_case_isolated(
    const char *exec,
    bake_bench_suite *suite,
    bake_bench_case *benchcase)
{
    char label[512];
    char time_str[64];
    char samples_str[32];
    char sample_time_str[64];
    snprintf(label, sizeof(label), "%s.%s", suite->id, benchcase->id);
    snprintf(time_str, sizeof(time_str), "%.17g", g_time);
    snprintf(samples_str, sizeof(samples_str), "%d", g_samples);
    snprintf(sample_time_str, sizeof(sample_time_str), "%.17g", g_sample_time);

    char *result_path = bake_bench_temp_path();
    if (!result_path) {
        printf("ERROR %s (failed to create temporary result file)\n", label);
        bake_bench_failure_add(suite->id, benchcase->id, "error", NULL, 0.0);
        return -1;
    }

    char *argv[] = {
        (char*)exec, label,
        (char*)"--bench-child", result_path,
        (char*)"--time", time_str,
        (char*)"--samples", samples_str,
        (char*)"--sample-time", sample_time_str,
        NULL
    };

    bake_bench_exit_t exit_info;
    memset(&exit_info, 0, sizeof(exit_info));
    uint64_t start = bake_bench_now_ns();
    int spawn_rc = bake_bench_spawn(argv, g_timeout, &exit_info);
    double elapsed = (double)(bake_bench_now_ns() - start) / 1e9;

    int rc = -1;
    if (spawn_rc != 0) {
        printf("ERROR %s (failed to start benchcase process)\n", label);
        bake_bench_failure_add(suite->id, benchcase->id, "error", NULL, elapsed);
    } else if (exit_info.timed_out) {
        printf("TIMEOUT %s (exceeded %g seconds)\n", label, g_timeout);
        bake_bench_failure_add(
            suite->id, benchcase->id, "timeout", &exit_info, elapsed);
    } else if (exit_info.crashed) {
        bake_bench_print_crash(suite->id, benchcase->id, &exit_info);
        bake_bench_failure_add(
            suite->id, benchcase->id, "crash", &exit_info, elapsed);
    } else if (exit_info.exit_code != 0) {
        printf("ERROR %s (exit code %lld)\n", label, exit_info.exit_code);
        bake_bench_failure_add(
            suite->id, benchcase->id, "error", &exit_info, elapsed);
    } else {
        bake_bench_result_t *result = bake_bench_result_append();
        if (result) {
            if (bake_bench_result_read(result_path, result) == 0) {
                result->suite = bake_bench_strdup(suite->id);
                result->name = bake_bench_strdup(benchcase->id);
                bake_bench_result_finish(result);
                rc = 0;
            } else {
                bake_bench_result_fini(result);
                g_result_count --;
            }
        }
        if (rc) {
            printf("ERROR %s (benchcase process did not report a result)\n", label);
            bake_bench_failure_add(
                suite->id, benchcase->id, "error", &exit_info, elapsed);
        }
    }

    fflush(stdout);
    bake_bench_temp_remove(result_path);
    free(result_path);
    return rc;
}

static bake_bench_suite* bake_bench_find_suite(
    bake_bench_suite *suites,
    uint32_t suite_count,
    const char *id)
{
    for (uint32_t i = 0; i < suite_count; i ++) {
        if (!strcmp(suites[i].id, id)) {
            return &suites[i];
        }
    }
    return NULL;
}

static void bake_bench_list_cases(bake_bench_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s ++) {
        for (uint32_t c = 0; c < suites[s].benchcase_count; c ++) {
            printf("%s.%s\n", suites[s].id, suites[s].benchcases[c].id);
        }
    }
}

static void bake_bench_list_suites(bake_bench_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s ++) {
        printf("%s\n", suites[s].id);
    }
}

static int bake_bench_print_summary(const char *bench_id, double elapsed) {
    int regressions = 0;
    int improvements = 0;
    for (int32_t i = 0; i < g_result_count; i ++) {
        regressions += g_results[i].regressed ? 1 : 0;
        improvements += g_results[i].improved ? 1 : 0;
    }

    printf("-----------------------------\n");
    printf("%s: %d benchmark(s) in %.3fs\n", bench_id, g_result_count, elapsed);

    if (g_failure_count) {
        printf("%d benchmark(s) failed:\n", g_failure_count);
        for (int32_t i = 0; i < g_failure_count; i ++) {
            printf("FAILED %s.%s (%s)\n",
                g_failures[i].suite, g_failures[i].name, g_failures[i].status);
        }
    }

    if (g_baseline_path) {
        printf("baseline %s: %d regression(s), %d improvement(s) beyond %.1f%%\n",
            g_baseline_path, regressions, improvements, g_threshold * 100.0);
        for (int32_t i = 0; i < g_result_count; i ++) {
            if (g_results[i].regressed) {
                printf("REGRESSION %s.%s %+.1f%%\n",
                    g_results[i].suite, g_results[i].name,
                    g_results[i].change * 100.0);
            }
        }
    }

    return regressions;
}

static int bake_bench_parse_args(int argc, char *argv[], const char **single, const char **suite_filter) {
    for (int i = 1; i < argc; i ++) {
        const char *arg = argv[i];
        const char *value = (i + 1) < argc ? argv[i + 1] : NULL;

        if (!strcmp(arg, "--json") || !strcmp(arg, "--baseline") ||
            !strcmp(arg, "--filter") || !strcmp(arg, "--time") ||
            !strcmp(arg, "--sample-time") || !strcmp(arg, "--samples") ||
            !strcmp(arg, "--threshold") || !strcmp(arg, "--timeout") ||
            !strcmp(arg, "--bench-child") || !strcmp(arg, "-j"))
        {
            if (!value) {
                printf("missing value for %s\n", arg);
                return -1;
            }

            if (!strcmp(arg, "--json")) {
                g_json_path = value;
            } else if (!strcmp(arg, "--baseline")) {
                g_baseline_path = value;
            } else if (!strcmp(arg, "--filter")) {
                g_filter = value;
            } else if (!strcmp(arg, "--time")) {
                g_time = atof(value);
            } else if (!strcmp(arg, "--sample-time")) {
                g_sample_time = atof(value);
            } else if (!strcmp(arg, "--samples")) {
                g_samples = atoi(value);
            } else if (!strcmp(arg, "--threshold")) {
                g_threshold = atof(value);
            } else if (!strcmp(arg, "--timeout")) {
                g_timeout = atof(value);
            } else if (!strcmp(arg, "--bench-child")) {
                g_child_path = value;
            }

            i ++;
            continue;
        }

        if (!strcmp(arg, "--fail-on-regression")) {
            g_fail_on_regression = true;
            continue;
        }

        if (!strcmp(arg, "--in-process")) {
            g_in_process = true;
            continue;
        }

        if (arg[0] == '-') {
            printf("unknown option '%s'\n", arg);
            return -1;
        }

        if (strchr(arg, '.')) {
            *single = arg;
        } else {
            *suite_filter = arg;
        }
    }

    if (g_samples < 1) {
        g_samples = 1;
    }
    if (g_time <= 0.0) {
        g_time = BAKE_BENCH_DEFAULT_TIME;
    }
    if (g_sample_time <= 0.0) {
        g_sample_time = BAKE_BENCH_DEFAULT_SAMPLE_TIME;
    }
    if (g_threshold < 0.0) {
        g_threshold = BAKE_BENCH_DEFAULT_THRESHOLD;
    }
    if (g_timeout < 0.0) {
        g_timeout = BAKE_BENCH_DEFAULT_TIMEOUT;
    }

    return 0;
}

static int bake_bench_child_main(
    bake_bench_suite *suites,
    uint32_t suite_count,
    const char *single)
{
    if (!single) {
        printf("--bench-child requires a <Suite>.<case> argument\n");
        return 2;
    }

    for (uint32_t s = 0; s < suite_count; s ++) {
        bake_bench_suite *suite = &suites[s];
        for (uint32_t c = 0; c < suite->benchcase_count; c ++) {
            char label[512];
            snprintf(label, sizeof(label), "%s.%s",
                suite->id, suite->benchcases[c].id);
            if (!strcmp(label, single)) {
                return bake_bench_run_child(suite, &suite->benchcases[c]) ? 1 : 0;
            }
        }
    }

    printf("benchcase '%s' not found\n", single);
    return 2;
}

int bake_bench_run(
    const char *bench_id,
    int argc,
    char *argv[],
    bake_bench_suite *suites,
    uint32_t suite_count)
{
    if (!bench_id || !bench_id[0]) {
        bench_id = "bench";
    }

    for (int i = 1; i < argc; i ++) {
        if (!strcmp(argv[i], "--list-benches")) {
            bake_bench_list_cases(suites, suite_count);
            return 0;
        }
        if (!strcmp(argv[i], "--list-suites")) {
            bake_bench_list_suites(suites, suite_count);
            return 0;
        }
    }

    const char *single = NULL;
    const char *suite_filter = NULL;
    if (bake_bench_parse_args(argc, argv, &single, &suite_filter) != 0) {
        return -1;
    }

    if (g_child_path) {
        return bake_bench_child_main(suites, suite_count, single);
    }

    if (suite_filter && !bake_bench_find_suite(suites, suite_count, suite_filter)) {
        printf("bench suite '%s' not found\n", suite_filter);
        return -1;
    }

    if (g_baseline_path && bake_bench_baseline_load(g_baseline_path) != 0) {
        return -1;
    }

    int rc = 0;
    int32_t selected = 0;
    uint64_t start = bake_bench_now_ns();

    for (uint32_t s = 0; s < suite_count; s ++) {
        bake_bench_suite *suite = &suites[s];
        for (uint32_t c = 0; c < suite->benchcase_count; c ++) {
            bake_bench_case *benchcase = &suite->benchcases[c];
            if (!bake_bench_case_selected(
                suite->id, benchcase->id, suite_filter, single))
            {
                continue;
            }
            selected ++;
            int case_rc = g_in_process ?
                bake_bench_run_case_in_process(suite, benchcase) :
                bake_bench_run_case_isolated(argv[0], suite, benchcase);
            if (case_rc != 0) {
                rc = -1;
            }
        }
    }

    double elapsed = (double)(bake_bench_now_ns() - start) / 1e9;

    if (!selected) {
        printf("no benchmarks matched\n");
        rc = -1;
    }

    int regressions = bake_bench_print_summary(bench_id, elapsed);

    if (bake_bench_write_json(bench_id, elapsed) != 0) {
        rc = -1;
    }

    if (regressions && g_fail_on_regression) {
        rc = -1;
    }

    bake_bench_results_fini();
    bake_bench_failures_fini();
    bake_bench_baseline_fini();

    return rc;
}
