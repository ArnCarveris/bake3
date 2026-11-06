#include "bake/ps.h"
#include "bake/environment.h"
#include "bake/os.h"
#include "common/json_helpers.h"

#include <flecs.h>
#include <time.h>

#define BAKE_PS_STATE_RUNNING "running"
#define BAKE_PS_STATE_ZOMBIE "zombie"
#define BAKE_PS_STATE_ORPHAN "orphan"
#define BAKE_PS_SOURCE_REGISTRY "registry"
#define BAKE_PS_SOURCE_SCAN "scan"
#define BAKE_PS_LOCAL_ENV_DIR "local_env"
#define BAKE_PS_WORKSPACE_WIDTH (34)
#define BAKE_PS_CMD_WIDTH (56)
#define BAKE_PS_KILL_WAIT_MS (5000)
#define BAKE_PS_KILL_POLL_MS (100)

char* bake_ps_registry_dir(void) {
    const char *override = getenv("BAKE3_PS_DIR");
    if (override && override[0]) {
        return ecs_os_strdup(override);
    }

    char *home = bake_os_home_path();
    if (!home) {
        return NULL;
    }

    char *dir = bake_path_join3(home, ".bake3", "ps");
    ecs_os_free(home);
    return dir;
}

static char* bake_ps_entry_path(const char *dir, int64_t pid) {
    char *name = flecs_asprintf("%lld.json", (long long)pid);
    char *path = bake_path_join(dir, name);
    ecs_os_free(name);
    return path;
}

static char* bake_ps_join_argv(const char *const *argv) {
    if (!argv || !argv[0]) {
        return ecs_os_strdup("");
    }

    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    for (int32_t i = 0; argv[i]; i ++) {
        if (i) {
            ecs_strbuf_appendch(&buf, ' ');
        }
        ecs_strbuf_appendstr(&buf, argv[i]);
    }

    return ecs_strbuf_get(&buf);
}

int64_t bake_ps_parse_etime(const char *etime) {
    if (!etime || !etime[0]) {
        return 0;
    }

    int64_t days = 0;
    const char *cursor = etime;
    const char *dash = strchr(etime, '-');
    if (dash) {
        days = (int64_t)strtoll(etime, NULL, 10);
        cursor = dash + 1;
    }

    int64_t parts[3] = {0, 0, 0};
    int32_t count = 0;
    while (count < 3) {
        char *end = NULL;
        long long value = strtoll(cursor, &end, 10);
        if (end == cursor) {
            break;
        }
        parts[count ++] = (int64_t)value;
        cursor = end;
        if (*cursor != ':') {
            break;
        }
        cursor ++;
    }

    int64_t seconds = 0;
    if (count == 3) {
        seconds = parts[0] * 3600 + parts[1] * 60 + parts[2];
    } else if (count == 2) {
        seconds = parts[0] * 60 + parts[1];
    } else if (count == 1) {
        seconds = parts[0];
    }

    return seconds + days * 86400;
}

void bake_ps_format_elapsed(int64_t seconds, char *buf, size_t size) {
    if (!buf || !size) {
        return;
    }

    if (seconds < 0) {
        seconds = 0;
    }

    int64_t hours = seconds / 3600;
    int64_t minutes = (seconds % 3600) / 60;
    int64_t secs = seconds % 60;
    ecs_os_snprintf(buf, (ecs_size_t)size, "%lld:%02lld:%02lld",
        (long long)hours, (long long)minutes, (long long)secs);
}

static char* bake_ps_truncate(const char *value, int32_t max_len) {
    if (!value) {
        return ecs_os_strdup("-");
    }

    int32_t len = (int32_t)strlen(value);
    if (len <= max_len || max_len < 4) {
        return ecs_os_strdup(value);
    }

    char *result = ecs_os_malloc(max_len + 1);
    ecs_os_memcpy(result, value, max_len - 3);
    result[max_len - 3] = '\0';
    ecs_os_strcat(result, "...");
    return result;
}

char* bake_ps_shorten_path(const char *path, int32_t max_len) {
    if (!path || !path[0]) {
        return ecs_os_strdup("-");
    }

    char *shortened = NULL;
    char *home = bake_os_home_path();
    size_t home_len = home ? strlen(home) : 0;
    if (home && home_len && !strncmp(path, home, home_len) &&
        (path[home_len] == '\0' || bake_path_is_sep(path[home_len])))
    {
        shortened = flecs_asprintf("~%s", path + home_len);
    } else {
        shortened = ecs_os_strdup(path);
    }
    ecs_os_free(home);

    int32_t len = (int32_t)strlen(shortened);
    if (max_len <= 0 || len <= max_len || max_len < 8) {
        return shortened;
    }

    const char *tail = shortened + len - (max_len - 3);
    const char *sep = tail;
    while (*sep && !bake_path_is_sep(*sep)) {
        sep ++;
    }
    if (*sep && (int32_t)strlen(sep) > 4) {
        tail = sep;
    }

    char *result = flecs_asprintf("...%s", tail);
    ecs_os_free(shortened);
    return result;
}

char* bake_ps_render_workspace(const char *workspace, bool full) {
    if (!workspace || !workspace[0]) {
        return ecs_os_strdup("-");
    }
    if (full) {
        return ecs_os_strdup(workspace);
    }
    return bake_ps_shorten_path(workspace, BAKE_PS_WORKSPACE_WIDTH);
}

char* bake_ps_render_cmd(const char *cmd, bool full) {
    if (!cmd || !cmd[0]) {
        return ecs_os_strdup("-");
    }
    if (full) {
        return ecs_os_strdup(cmd);
    }
    return bake_ps_truncate(cmd, BAKE_PS_CMD_WIDTH);
}

const char* bake_ps_env_kind_str(bake_ps_env_kind_t kind) {
    if (kind == BakePsEnvLocal) {
        return BAKE_PS_ENV_LOCAL;
    }
    if (kind == BakePsEnvGlobal) {
        return BAKE_PS_ENV_GLOBAL;
    }
    return "unknown";
}

bake_ps_env_kind_t bake_ps_env_kind_from_str(const char *kind) {
    if (!kind || !kind[0]) {
        return BakePsEnvUnknown;
    }
    if (!strcmp(kind, BAKE_PS_ENV_LOCAL)) {
        return BakePsEnvLocal;
    }
    if (!strcmp(kind, BAKE_PS_ENV_GLOBAL)) {
        return BakePsEnvGlobal;
    }
    return BakePsEnvUnknown;
}

char* bake_ps_env_label(bake_ps_env_kind_t kind, const char *name) {
    if (kind == BakePsEnvLocal) {
        if (name && name[0]) {
            return flecs_asprintf("%s:%s", BAKE_PS_ENV_LOCAL, name);
        }
        return ecs_os_strdup(BAKE_PS_ENV_LOCAL);
    }
    if (kind == BakePsEnvGlobal) {
        return ecs_os_strdup(BAKE_PS_ENV_GLOBAL);
    }
    if (name && name[0]) {
        return ecs_os_strdup(name);
    }
    return ecs_os_strdup(BAKE_PS_ENV_UNKNOWN);
}

static char* bake_ps_cfg_from_triplet(const char *triplet) {
    const char *last = strrchr(triplet, '-');
    if (!last || !last[1]) {
        return NULL;
    }
    return ecs_os_strdup(last + 1);
}

void bake_ps_path_info_fini(bake_ps_path_info_t *info) {
    if (!info) {
        return;
    }

    ecs_os_free(info->env);
    ecs_os_free(info->workspace);
    ecs_os_free(info->cfg);
    ecs_os_free(info->project);
    memset(info, 0, sizeof(*info));
}

static int32_t bake_ps_split_path(const char *path, char ***parts_out) {
    ecs_vec_t vec = {0};
    const char *cursor = path;
    while (*cursor) {
        while (*cursor && bake_path_is_sep(*cursor)) {
            cursor ++;
        }
        const char *start = cursor;
        while (*cursor && !bake_path_is_sep(*cursor)) {
            cursor ++;
        }
        if (cursor == start) {
            continue;
        }
        char *part = ecs_os_malloc((ecs_size_t)(cursor - start) + 1);
        ecs_os_memcpy(part, start, (ecs_size_t)(cursor - start));
        part[cursor - start] = '\0';
        char **slot = ecs_vec_append_t(NULL, &vec, char*);
        *slot = part;
    }

    int32_t count = ecs_vec_count(&vec);
    char **parts = NULL;
    if (count) {
        parts = ecs_os_malloc_n(char*, count);
        ecs_os_memcpy_n(parts, ecs_vec_first_t(&vec, char*), char*, count);
    }
    ecs_vec_fini_t(NULL, &vec, char*);
    *parts_out = parts;
    return count;
}

static void bake_ps_parts_free(char **parts, int32_t count) {
    for (int32_t i = 0; i < count; i ++) {
        ecs_os_free(parts[i]);
    }
    ecs_os_free(parts);
}

static char* bake_ps_join_parts(
    const char *path,
    char **parts,
    int32_t count)
{
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    if (bake_path_is_abs(path)) {
        ecs_strbuf_appendch(&buf, bake_path_sep());
    }
    for (int32_t i = 0; i < count; i ++) {
        if (i) {
            ecs_strbuf_appendch(&buf, bake_path_sep());
        }
        ecs_strbuf_appendstr(&buf, parts[i]);
    }
    return ecs_strbuf_get(&buf);
}

int bake_ps_parse_local_env_path(const char *path, bake_ps_path_info_t *info_out) {
    if (!path || !path[0] || !info_out) {
        return -1;
    }

    memset(info_out, 0, sizeof(*info_out));

    char **parts = NULL;
    int32_t count = bake_ps_split_path(path, &parts);
    int32_t marker = -1;
    bool output_dir = false;
    for (int32_t i = 0; i < (count - 1); i ++) {
        if (strcmp(parts[i + 1], BAKE_PS_LOCAL_ENV_DIR)) {
            continue;
        }
        if (!strcmp(parts[i], ".bake")) {
            marker = i;
            output_dir = false;
            continue;
        }
        char *prefix = bake_ps_join_parts(path, parts, i + 1);
        if (bake_settings_is_output_dir(prefix)) {
            marker = i;
            output_dir = true;
        }
        ecs_os_free(prefix);
    }

    if (marker < 0) {
        bake_ps_parts_free(parts, count);
        return -1;
    }

    info_out->workspace = bake_ps_join_parts(
        path, parts, output_dir ? marker + 1 : marker);

    int32_t i = marker + 2;
    if (i < count && strcmp(parts[i], "build") &&
        !bake_local_env_name_reserved(parts[i]))
    {
        info_out->env = ecs_os_strdup(parts[i]);
        i ++;
    }

    if ((i + 2) < count && !strcmp(parts[i], "build")) {
        info_out->project = ecs_os_strdup(parts[i + 1]);
        info_out->cfg = bake_ps_cfg_from_triplet(parts[i + 2]);
    } else if ((i + 1) < count) {
        info_out->cfg = ecs_os_strdup(parts[i + 1]);
    }

    if (!info_out->project && count) {
        int32_t bin = -1;
        for (int32_t j = i; j < count; j ++) {
            if (!strcmp(parts[j], "bin") || !strcmp(parts[j], "lib")) {
                bin = j;
            }
        }
        if (bin >= 0 && (bin + 2) < count) {
            info_out->project = ecs_os_strdup(parts[bin + 1]);
        } else {
            info_out->project = ecs_os_strdup(parts[count - 1]);
        }
    }

    bake_ps_parts_free(parts, count);
    return 0;
}

bake_ps_env_kind_t bake_ps_env_from_home(const char *bake_home, char **name_out) {
    if (name_out) {
        *name_out = NULL;
    }

    if (!bake_home || !bake_home[0]) {
        return BakePsEnvUnknown;
    }

    char *probe = bake_path_join(bake_home, "bin");
    bake_ps_path_info_t info;
    bake_ps_env_kind_t kind = BakePsEnvGlobal;
    if (bake_ps_parse_local_env_path(probe, &info) == 0) {
        kind = BakePsEnvLocal;
        if (name_out) {
            *name_out = info.env;
            info.env = NULL;
        }
    }
    bake_ps_path_info_fini(&info);
    ecs_os_free(probe);
    return kind;
}

int bake_ps_register(
    int64_t pid,
    const char *const *argv,
    const bake_ps_info_t *info)
{
    if (pid <= 0 || !info) {
        return -1;
    }

    char *dir = bake_ps_registry_dir();
    if (!dir) {
        return -1;
    }

    if (bake_os_mkdirs(dir) != 0) {
        ecs_os_free(dir);
        return -1;
    }

    char *cmd = bake_ps_join_argv(argv);
    JSON_Value *value = json_value_init_object();
    JSON_Object *object = json_value_get_object(value);
    json_object_set_number(object, "pid", (double)pid);
    json_object_set_number(object, "parent_pid", (double)bake_os_pid());
    json_object_set_number(object, "start_time", (double)time(NULL));
    json_object_set_string(object, "cmd", cmd);
    if (info->project) json_object_set_string(object, "project", info->project);
    if (info->cfg) json_object_set_string(object, "cfg", info->cfg);
    if (info->env) json_object_set_string(object, "env", info->env);
    json_object_set_string(object, "env_kind", bake_ps_env_kind_str(info->env_kind));
    if (info->bake_home) json_object_set_string(object, "bake_home", info->bake_home);
    if (info->workspace) json_object_set_string(object, "workspace", info->workspace);
    if (info->kind) json_object_set_string(object, "kind", info->kind);

    char *serialized = json_serialize_to_string_pretty(value);
    char *path = bake_ps_entry_path(dir, pid);
    int rc = bake_file_write(path, serialized ? serialized : "{}");

    json_free_serialized_string(serialized);
    json_value_free(value);
    ecs_os_free(path);
    ecs_os_free(cmd);
    ecs_os_free(dir);

    if (!rc && info->announce) {
        char *label = bake_ps_env_label(info->env_kind, info->env);
        ecs_trace("[bake] started %s (pid %lld, env %s) - run 'bake3 ps' to "
            "see running processes",
            info->project ? info->project : "process",
            (long long)pid,
            label);
        ecs_os_free(label);
    }

    return rc;
}

void bake_ps_unregister(int64_t pid) {
    if (pid <= 0) {
        return;
    }

    char *dir = bake_ps_registry_dir();
    if (!dir) {
        return;
    }

    char *path = bake_ps_entry_path(dir, pid);
    bake_remove_file_if_exists(path);
    ecs_os_free(path);
    ecs_os_free(dir);
}

static void bake_ps_entry_fini(bake_ps_entry_t *entry) {
    ecs_os_free(entry->project);
    ecs_os_free(entry->cfg);
    ecs_os_free(entry->env);
    ecs_os_free(entry->bake_home);
    ecs_os_free(entry->workspace);
    ecs_os_free(entry->cmd);
    ecs_os_free(entry->kind);
}

void bake_ps_list_fini(bake_ps_list_t *list) {
    if (!list) {
        return;
    }

    for (int32_t i = 0; i < list->count; i ++) {
        bake_ps_entry_fini(&list->items[i]);
    }
    ecs_os_free(list->items);
    list->items = NULL;
    list->count = 0;
}

static char* bake_ps_json_string(const JSON_Object *object, const char *key) {
    const char *value = json_object_get_string(object, key);
    return value ? ecs_os_strdup(value) : NULL;
}

static int64_t bake_ps_json_number(const JSON_Object *object, const char *key) {
    JSON_Value *value = json_object_get_value(object, key);
    if (!value || json_value_get_type(value) != JSONNumber) {
        return 0;
    }
    return (int64_t)json_value_get_number(value);
}

static char* bake_ps_exe_from_cmd(const char *cmd) {
    if (!cmd || !cmd[0]) {
        return NULL;
    }

    const char *start = cmd;
    while (*start == ' ') {
        start ++;
    }

    char quote = '\0';
    if (*start == '"' || *start == '\'') {
        quote = *start;
        start ++;
    }

    const char *stop = start;
    while (*stop && ((quote && *stop != quote) || (!quote && *stop != ' '))) {
        stop ++;
    }

    size_t len = (size_t)(stop - start);
    if (!len) {
        return NULL;
    }

    char *exe = ecs_os_malloc((ecs_size_t)len + 1);
    ecs_os_memcpy(exe, start, (ecs_size_t)len);
    exe[len] = '\0';
    return exe;
}

static const bake_proc_info_t* bake_ps_find_proc(
    const bake_proc_info_t *procs,
    int32_t count,
    int64_t pid)
{
    for (int32_t i = 0; i < count; i ++) {
        if (procs[i].pid == pid) {
            return &procs[i];
        }
    }
    return NULL;
}

static bool bake_ps_cmd_matches(const char *registered, const char *observed) {
    if (!registered || !registered[0] || !observed || !observed[0]) {
        return true;
    }

    char *exe = bake_ps_exe_from_cmd(registered);
    if (!exe) {
        return true;
    }

    char *base = bake_path_basename(exe);
    bool match = base && base[0] && strstr(observed, base) != NULL;
    ecs_os_free(base);
    ecs_os_free(exe);
    return match;
}

static int bake_ps_cmp_entry(const void *a, const void *b) {
    const bake_ps_entry_t *lhs = a;
    const bake_ps_entry_t *rhs = b;
    if (lhs->elapsed_sec != rhs->elapsed_sec) {
        return lhs->elapsed_sec > rhs->elapsed_sec ? -1 : 1;
    }
    if (lhs->pid != rhs->pid) {
        return lhs->pid < rhs->pid ? -1 : 1;
    }
    return 0;
}

static void bake_ps_collect_registry(
    ecs_vec_t *vec,
    const bake_proc_info_t *procs,
    int32_t proc_count,
    bool have_procs)
{
    char *dir = bake_ps_registry_dir();
    if (!dir || !bake_path_exists(dir)) {
        ecs_os_free(dir);
        return;
    }

    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    if (bake_dir_list(dir, &entries, &entry_count) != 0) {
        ecs_os_free(dir);
        return;
    }

    int64_t now = (int64_t)time(NULL);

    for (int32_t i = 0; i < entry_count; i ++) {
        const bake_dir_entry_t *file = &entries[i];
        if (file->is_dir || !bake_has_suffix(file->name, ".json")) {
            continue;
        }

        JSON_Value *value = json_parse_file_with_comments(file->path);
        JSON_Object *object = value ? json_value_get_object(value) : NULL;
        if (!object) {
            json_value_free(value);
            bake_remove_file_if_exists(file->path);
            continue;
        }

        int64_t pid = bake_ps_json_number(object, "pid");
        char *cmd = bake_ps_json_string(object, "cmd");
        const bake_proc_info_t *proc = have_procs ?
            bake_ps_find_proc(procs, proc_count, pid) : NULL;

        bool alive = proc != NULL || bake_os_pid_alive(pid);
        if (alive && proc && !bake_ps_cmd_matches(cmd, proc->cmd)) {
            alive = false;
        }

        if (pid <= 0 || !alive) {
            bake_remove_file_if_exists(file->path);
            ecs_os_free(cmd);
            json_value_free(value);
            continue;
        }

        bake_ps_entry_t *entry = ecs_vec_append_t(NULL, vec, bake_ps_entry_t);
        memset(entry, 0, sizeof(*entry));
        entry->pid = pid;
        entry->parent_pid = bake_ps_json_number(object, "parent_pid");
        entry->start_time = bake_ps_json_number(object, "start_time");
        entry->elapsed_sec = entry->start_time > 0 ? now - entry->start_time : 0;
        if (entry->elapsed_sec < 0) {
            entry->elapsed_sec = 0;
        }
        entry->project = bake_ps_json_string(object, "project");
        entry->cfg = bake_ps_json_string(object, "cfg");
        entry->env = bake_ps_json_string(object, "env");
        entry->bake_home = bake_ps_json_string(object, "bake_home");
        const char *env_kind = json_object_get_string(object, "env_kind");
        if (env_kind) {
            entry->env_kind = bake_ps_env_kind_from_str(env_kind);
        } else {
            entry->env_kind = bake_ps_env_from_home(entry->bake_home, NULL);
        }
        entry->workspace = bake_ps_json_string(object, "workspace");
        entry->kind = bake_ps_json_string(object, "kind");
        entry->cmd = cmd;
        entry->source = BAKE_PS_SOURCE_REGISTRY;

        if (proc && proc->state == 'Z') {
            entry->state = BAKE_PS_STATE_ZOMBIE;
        } else if (entry->parent_pid > 0 && !bake_os_pid_alive(entry->parent_pid)) {
            entry->state = BAKE_PS_STATE_ORPHAN;
        } else {
            entry->state = BAKE_PS_STATE_RUNNING;
        }

        json_value_free(value);
    }

    bake_dir_entries_free(entries, entry_count);
    ecs_os_free(dir);
}

static void bake_ps_collect_scan(
    ecs_vec_t *vec,
    const bake_proc_info_t *procs,
    int32_t proc_count,
    bool all_users)
{
    int64_t uid = bake_os_uid();
    int64_t self = bake_os_pid();

    for (int32_t i = 0; i < proc_count; i ++) {
        const bake_proc_info_t *proc = &procs[i];
        if (proc->pid == self || proc->pid <= 0) {
            continue;
        }

        if (!all_users && proc->uid != uid) {
            continue;
        }

        if (!strstr(proc->cmd, BAKE_PS_LOCAL_ENV_DIR)) {
            continue;
        }

        char *exe = bake_ps_exe_from_cmd(proc->cmd);
        bake_ps_path_info_t info;
        if (!exe || !strstr(exe, BAKE_PS_LOCAL_ENV_DIR) ||
            bake_ps_parse_local_env_path(exe, &info) != 0)
        {
            ecs_os_free(exe);
            continue;
        }
        ecs_os_free(exe);

        bool known = false;
        bake_ps_entry_t *existing = ecs_vec_first_t(vec, bake_ps_entry_t);
        for (int32_t j = 0; j < ecs_vec_count(vec); j ++) {
            if (existing[j].pid == proc->pid) {
                known = true;
                break;
            }
        }

        if (known) {
            bake_ps_path_info_fini(&info);
            continue;
        }

        bake_ps_entry_t *entry = ecs_vec_append_t(NULL, vec, bake_ps_entry_t);
        memset(entry, 0, sizeof(*entry));
        entry->pid = proc->pid;
        entry->parent_pid = proc->parent_pid;
        entry->elapsed_sec = proc->elapsed_sec;
        entry->start_time = (int64_t)time(NULL) - proc->elapsed_sec;
        entry->project = info.project;
        entry->cfg = info.cfg;
        entry->env = info.env;
        entry->env_kind = BakePsEnvLocal;
        entry->workspace = info.workspace;
        entry->cmd = ecs_os_strdup(proc->cmd);
        entry->kind = ecs_os_strdup("exec");
        entry->source = BAKE_PS_SOURCE_SCAN;
        entry->state = proc->state == 'Z' ?
            BAKE_PS_STATE_ZOMBIE : BAKE_PS_STATE_RUNNING;
        memset(&info, 0, sizeof(info));
    }
}

int bake_ps_collect(bake_ps_list_t *list_out, bool all_users) {
    if (!list_out) {
        return -1;
    }

    list_out->items = NULL;
    list_out->count = 0;

    bake_proc_info_t *procs = NULL;
    int32_t proc_count = 0;
    bool have_procs = bake_proc_snapshot(&procs, &proc_count) == 0;

    ecs_vec_t vec = {0};
    bake_ps_collect_registry(&vec, procs, proc_count, have_procs);
    if (have_procs) {
        bake_ps_collect_scan(&vec, procs, proc_count, all_users);
    }
    bake_proc_snapshot_free(procs, proc_count);

    int32_t count = ecs_vec_count(&vec);
    if (count) {
        bake_ps_entry_t *items = ecs_os_malloc_n(bake_ps_entry_t, count);
        ecs_os_memcpy_n(items, ecs_vec_first_t(&vec, bake_ps_entry_t),
            bake_ps_entry_t, count);
        if (count > 1) {
            qsort(items, (size_t)count, sizeof(bake_ps_entry_t), bake_ps_cmp_entry);
        }
        list_out->items = items;
    }

    ecs_vec_fini_t(NULL, &vec, bake_ps_entry_t);
    list_out->count = count;
    return have_procs ? 0 : 1;
}

static const char* bake_ps_value(const char *value) {
    return (value && value[0]) ? value : "-";
}

static void bake_ps_print_json(const bake_ps_list_t *list) {
    JSON_Value *root = json_value_init_array();
    JSON_Array *array = json_value_get_array(root);

    for (int32_t i = 0; i < list->count; i ++) {
        const bake_ps_entry_t *entry = &list->items[i];
        char elapsed[32];
        bake_ps_format_elapsed(entry->elapsed_sec, elapsed, sizeof(elapsed));

        JSON_Value *value = json_value_init_object();
        JSON_Object *object = json_value_get_object(value);
        json_object_set_number(object, "pid", (double)entry->pid);
        json_object_set_number(object, "parent_pid", (double)entry->parent_pid);
        json_object_set_number(object, "start_time", (double)entry->start_time);
        json_object_set_number(object, "elapsed_sec", (double)entry->elapsed_sec);
        json_object_set_string(object, "elapsed", elapsed);
        char *label = bake_ps_env_label(entry->env_kind, entry->env);
        json_object_set_string(object, "env", label);
        json_object_set_string(object, "env_kind",
            bake_ps_env_kind_str(entry->env_kind));
        json_object_set_string(object, "env_name", bake_ps_value(entry->env));
        ecs_os_free(label);
        json_object_set_string(object, "cfg", bake_ps_value(entry->cfg));
        json_object_set_string(object, "project", bake_ps_value(entry->project));
        json_object_set_string(object, "kind", bake_ps_value(entry->kind));
        json_object_set_string(object, "state", entry->state);
        json_object_set_string(object, "source", entry->source);
        json_object_set_string(object, "workspace", bake_ps_value(entry->workspace));
        json_object_set_string(object, "bake_home", bake_ps_value(entry->bake_home));
        json_object_set_string(object, "cmd", bake_ps_value(entry->cmd));
        json_array_append_value(array, value);
    }

    char *serialized = json_serialize_to_string_pretty(root);
    printf("%s\n", serialized ? serialized : "[]");
    json_free_serialized_string(serialized);
    json_value_free(root);
}

#define BAKE_PS_COLUMNS (9)

static void bake_ps_print_table(const bake_ps_list_t *list, bool full) {
    static const char *headers[BAKE_PS_COLUMNS] = {
        "PID", "ELAPSED", "ENV", "CFG", "PROJECT",
        "STATE", "SOURCE", "WORKSPACE", "CMD"
    };

    char ***rows = ecs_os_calloc_n(char**, list->count);
    int32_t widths[BAKE_PS_COLUMNS];
    for (int32_t c = 0; c < BAKE_PS_COLUMNS; c ++) {
        widths[c] = (int32_t)strlen(headers[c]);
    }

    for (int32_t i = 0; i < list->count; i ++) {
        const bake_ps_entry_t *entry = &list->items[i];
        char elapsed[32];
        bake_ps_format_elapsed(entry->elapsed_sec, elapsed, sizeof(elapsed));

        char **row = ecs_os_calloc_n(char*, BAKE_PS_COLUMNS);
        row[0] = flecs_asprintf("%lld", (long long)entry->pid);
        row[1] = ecs_os_strdup(elapsed);
        row[2] = bake_ps_env_label(entry->env_kind, entry->env);
        row[3] = ecs_os_strdup(bake_ps_value(entry->cfg));
        row[4] = ecs_os_strdup(bake_ps_value(entry->project));
        row[5] = ecs_os_strdup(entry->state);
        row[6] = ecs_os_strdup(entry->source);
        row[7] = bake_ps_render_workspace(entry->workspace, full);
        row[8] = bake_ps_render_cmd(entry->cmd, full);

        for (int32_t c = 0; c < BAKE_PS_COLUMNS; c ++) {
            int32_t len = (int32_t)strlen(row[c]);
            if (len > widths[c]) {
                widths[c] = len;
            }
        }

        rows[i] = row;
    }

    for (int32_t c = 0; c < BAKE_PS_COLUMNS; c ++) {
        if (c == (BAKE_PS_COLUMNS - 1)) {
            printf("%s\n", headers[c]);
        } else {
            printf("%-*s  ", widths[c], headers[c]);
        }
    }

    for (int32_t i = 0; i < list->count; i ++) {
        for (int32_t c = 0; c < BAKE_PS_COLUMNS; c ++) {
            if (c == (BAKE_PS_COLUMNS - 1)) {
                printf("%s\n", rows[i][c]);
            } else {
                printf("%-*s  ", widths[c], rows[i][c]);
            }
            ecs_os_free(rows[i][c]);
        }
        ecs_os_free(rows[i]);
    }

    ecs_os_free(rows);
}

static bool bake_ps_is_number(const char *value) {
    if (!value || !value[0]) {
        return false;
    }
    for (const char *ptr = value; *ptr; ptr ++) {
        if (*ptr < '0' || *ptr > '9') {
            return false;
        }
    }
    return true;
}

bool bake_ps_entry_matches(const bake_ps_entry_t *entry, const char *target) {
    if (!entry || !target || !target[0]) {
        return false;
    }

    if (bake_ps_is_number(target)) {
        return entry->pid == (int64_t)strtoll(target, NULL, 10);
    }

    if (entry->env && !strcmp(entry->env, target)) {
        return true;
    }

    bake_ps_env_kind_t kind = bake_ps_env_kind_from_str(target);
    if (kind != BakePsEnvUnknown && kind == entry->env_kind) {
        return true;
    }

    char *label = bake_ps_env_label(entry->env_kind, entry->env);
    bool match = !strcmp(label, target);
    ecs_os_free(label);
    return match;
}

static int bake_ps_kill(const bake_ps_list_t *list, const char *target) {
    int32_t matched = 0;
    for (int32_t i = 0; i < list->count; i ++) {
        const bake_ps_entry_t *entry = &list->items[i];
        if (!bake_ps_entry_matches(entry, target)) {
            continue;
        }

        matched ++;
        if (bake_os_pid_kill(entry->pid, false) != 0) {
            ecs_err("failed to terminate process %lld", (long long)entry->pid);
            continue;
        }

        char *label = bake_ps_env_label(entry->env_kind, entry->env);
        ecs_trace("terminated %s (pid %lld, env %s)",
            bake_ps_value(entry->project),
            (long long)entry->pid,
            label);
        ecs_os_free(label);
    }

    if (!matched) {
        ecs_err("no bake process matches '%s'", target);
        return -1;
    }

    int64_t waited = 0;
    while (waited < BAKE_PS_KILL_WAIT_MS) {
        bool any_alive = false;
        for (int32_t i = 0; i < list->count; i ++) {
            const bake_ps_entry_t *entry = &list->items[i];
            if (bake_ps_entry_matches(entry, target) &&
                bake_os_pid_alive(entry->pid))
            {
                any_alive = true;
            }
        }

        if (!any_alive) {
            break;
        }

        ecs_os_sleep(0, BAKE_PS_KILL_POLL_MS * 1000 * 1000);
        waited += BAKE_PS_KILL_POLL_MS;
    }

    for (int32_t i = 0; i < list->count; i ++) {
        const bake_ps_entry_t *entry = &list->items[i];
        if (!bake_ps_entry_matches(entry, target)) {
            continue;
        }
        if (bake_os_pid_alive(entry->pid)) {
            ecs_warn("process %lld did not stop, sending kill signal",
                (long long)entry->pid);
            bake_os_pid_kill(entry->pid, true);
        }
        if (!bake_os_pid_alive(entry->pid)) {
            bake_ps_unregister(entry->pid);
        }
    }

    return 0;
}

int bake_ps_command(bake_context_t *ctx) {
    bake_ps_list_t list;
    int scan_rc = bake_ps_collect(&list, ctx->opts.all_users);

    int rc = 0;
    if (ctx->opts.ps_kill) {
        rc = bake_ps_kill(&list, ctx->opts.ps_kill);
        bake_ps_list_fini(&list);
        return rc;
    }

    if (ctx->opts.json) {
        bake_ps_print_json(&list);
        bake_ps_list_fini(&list);
        return 0;
    }

    if (scan_rc > 0) {
        ecs_warn("process table scan is not supported on this platform, "
            "listing registered processes only");
    }

    if (!list.count) {
        printf("no processes started by bake are running\n");
    } else {
        bake_ps_print_table(&list, ctx->opts.ps_full);
        if (!ctx->opts.ps_full) {
            printf("run 'bake3 ps --full' to see full paths\n");
        }
    }

    bake_ps_list_fini(&list);
    return rc;
}
