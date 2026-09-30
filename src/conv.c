/*
 * conv — the conversation store. See conv.h for the on-disk shape;
 * the pattern follows tasks.c: lenient load (broken entries stay on
 * disk but are not listed), atomic meta writes, append-only records.
 */
#define _POSIX_C_SOURCE 200809L

#include "conv.h"
#include "theme.h" /* theme_dir(): resolved config directory */
#include "util.h"

#include <cJSON.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* ---------- paths & ids ---------- */

static const char *convs_dir(char *buf, size_t n)
{
    snprintf(buf, n, "%s/conversations", theme_dir());
    return buf;
}

static void conv_path(const char *id, const char *file,
                      char *buf, size_t n)
{
    char dir[4352];
    convs_dir(dir, sizeof dir);
    snprintf(buf, n, "%s/%s/%s", dir, id, file);
}

int conv_valid_id(const char *s)
{
    if (strlen(s) != CONV_ID_LEN) return 0;
    for (size_t i = 0; i < CONV_ID_LEN; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}

/* the record kinds, in their natural transcript order */
static const char *const KINDS[] = {
    "user", "thinking", "response", "tool_call", "tool_result", "error",
};
#define KIND_COUNT (sizeof KINDS / sizeof KINDS[0])

static int kind_known(const char *k)
{
    for (size_t i = 0; i < KIND_COUNT; i++)
        if (strcmp(KINDS[i], k) == 0) return 1;
    return 0;
}

static const char *const STATES[] = {
    "running", "completed", "failed", "stopped",
};
#define STATE_COUNT (sizeof STATES / sizeof STATES[0])

static int state_from_name(const char *s)
{
    for (size_t i = 0; i < STATE_COUNT; i++)
        if (strcmp(STATES[i], s) == 0) return (int)i;
    return -1;
}

static const char *state_name(int s)
{
    return (s >= 0 && (size_t)s < STATE_COUNT) ? STATES[s] : STATES[CONV_STOPPED];
}

/* bump on every visible change (SSE clients refetch when it moves) */
static long long g_version = 0;
long long conv_version(void) { return g_version; }

/* ---------- meta ---------- */

typedef struct {
    char id[CONV_ID_LEN + 1];
    char agent[CFG_NAME_MAX];
    char llm[CFG_NAME_MAX];
    char project[PROJECT_ID_LEN + 1];
    char parent[CONV_ID_LEN + 1];
    char title[CONV_TITLE_MAX];
    long long started, ended;
    int state;
} conv_meta_t;

/* lenient parse: whatever is missing stays default, nothing fails */
static void meta_from_json(const cJSON *j, conv_meta_t *m)
{
    const cJSON *v;
    v = cJSON_GetObjectItemCaseSensitive(j, "agent");
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(m->agent, sizeof m->agent, "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(j, "llm");
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(m->llm, sizeof m->llm, "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(j, "project");
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(m->project, sizeof m->project, "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(j, "parent");
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(m->parent, sizeof m->parent, "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(j, "title");
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(m->title, sizeof m->title, "%s", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(j, "started");
    if (cJSON_IsNumber(v)) m->started = (long long)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(j, "ended");
    if (cJSON_IsNumber(v)) m->ended = (long long)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(j, "state");
    if (cJSON_IsString(v) && v->valuestring) {
        int s = state_from_name(v->valuestring);
        if (s >= 0) m->state = s;
    }
}

static int meta_load(const char *id, conv_meta_t *m)
{
    memset(m, 0, sizeof *m);
    snprintf(m->id, sizeof m->id, "%s", id);
    m->state = CONV_STOPPED; /* a meta without a state ended somehow */

    char path[4352 + 64];
    conv_path(id, "meta.json", path, sizeof path);
    char *buf = read_whole_file(path, 64 * 1024);
    if (!buf) return -1;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j || !cJSON_IsObject(j)) { cJSON_Delete(j); return -1; }
    meta_from_json(j, m);
    cJSON_Delete(j);
    return 0;
}

static int meta_save(const conv_meta_t *m)
{
    cJSON *o = cJSON_CreateObject();
    int ok = o &&
        cJSON_AddStringToObject(o, "id", m->id) &&
        cJSON_AddStringToObject(o, "agent", m->agent) &&
        cJSON_AddStringToObject(o, "llm", m->llm) &&
        cJSON_AddStringToObject(o, "project", m->project) &&
        cJSON_AddStringToObject(o, "parent", m->parent) &&
        cJSON_AddStringToObject(o, "title", m->title) &&
        cJSON_AddNumberToObject(o, "started", (double)m->started) &&
        cJSON_AddNumberToObject(o, "ended", (double)m->ended) &&
        cJSON_AddStringToObject(o, "state", state_name(m->state));
    char path[4352 + 64];
    conv_path(m->id, "meta.json", path, sizeof path);
    int rc = ok ? save_json_atomic(path, o) : -1;
    cJSON_Delete(o);
    return rc;
}

static cJSON *meta_to_cjson(const conv_meta_t *m)
{
    cJSON *o = cJSON_CreateObject();
    if (!o ||
        !cJSON_AddStringToObject(o, "id", m->id) ||
        !cJSON_AddStringToObject(o, "agent", m->agent) ||
        !cJSON_AddStringToObject(o, "llm", m->llm) ||
        !cJSON_AddStringToObject(o, "project", m->project) ||
        !cJSON_AddStringToObject(o, "parent", m->parent) ||
        !cJSON_AddStringToObject(o, "title", m->title) ||
        !cJSON_AddNumberToObject(o, "started", (double)m->started) ||
        !cJSON_AddNumberToObject(o, "ended", (double)m->ended) ||
        !cJSON_AddStringToObject(o, "state", state_name(m->state))) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

/* ---------- the directory listing ---------- */

static int strptr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* the id directories, sorted; *names is a malloc'd array of malloc'd
 * strings, *n its length (0 with *names NULL when empty/unreadable) */
static void list_ids(char ***names, size_t *n)
{
    *names = NULL;
    *n = 0;
    char dir[4352];
    convs_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    char **v = calloc(CONV_LIST_MAX, sizeof *v);
    if (!v) { closedir(d); return; }
    const struct dirent *e;
    while ((e = readdir(d)) != NULL && *n < CONV_LIST_MAX) {
        if (!conv_valid_id(e->d_name)) continue;
        char *one = strdup(e->d_name);
        if (!one) break;
        v[(*n)++] = one;
    }
    closedir(d);
    qsort(v, *n, sizeof v[0], strptr_cmp);
    *names = v;
}

static void free_ids(char **names, size_t n)
{
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* newest first, id as the tiebreaker for a stable order */
static int meta_cmp(const void *pa, const void *pb)
{
    const conv_meta_t *a = pa, *b = pb;
    if (a->started != b->started) return a->started < b->started ? 1 : -1;
    return strcmp(b->id, a->id);
}

/* every parseable meta, sorted newest first (malloc'd array) */
static conv_meta_t *load_metas(size_t *count)
{
    char **ids;
    size_t n;
    list_ids(&ids, &n);
    conv_meta_t *all = calloc(n ? n : 1, sizeof *all);
    size_t used = 0;
    if (all)
        for (size_t i = 0; i < n; i++)
            if (meta_load(ids[i], &all[used]) == 0) used++;
    free_ids(ids, n);
    if (all) qsort(all, used, sizeof all[0], meta_cmp);
    *count = used;
    return all;
}

/* ---------- public surface ---------- */

void conv_boot(void)
{
    size_t n;
    conv_meta_t *all = load_metas(&n);
    if (!all) return;
    for (size_t i = 0; i < n; i++) {
        if (all[i].state != CONV_RUNNING) continue;
        conv_add(all[i].id, "error", NULL,
                 "interrupted: flower was restarted while this "
                 "conversation ran", 0);
        conv_finish(all[i].id, CONV_STOPPED);
    }
    free(all);
}

int conv_create(const char *agent, const char *llm, const char *project,
                const char *parent, const char *title, char *id_out)
{
    if (!agent || !agent[0] || !title || !title[0]) return -1;

    conv_meta_t m;
    memset(&m, 0, sizeof m);
    char path[4352 + 64];
    struct stat st;
    do { /* ids are unique against what is already on disk */
        gen_hex_id(m.id, CONV_ID_LEN);
        conv_path(m.id, "meta.json", path, sizeof path);
    } while (stat(path, &st) == 0);

    snprintf(m.agent, sizeof m.agent, "%s", agent);
    snprintf(m.llm, sizeof m.llm, "%s", llm ? llm : "");
    snprintf(m.project, sizeof m.project, "%s", project ? project : "");
    snprintf(m.parent, sizeof m.parent, "%s", parent ? parent : "");
    snprintf(m.title, sizeof m.title, "%s", title);
    m.started = (long long)time(NULL);
    m.ended = 0;
    m.state = CONV_RUNNING;

    char dir[4352 + 64], root[4352];
    convs_dir(root, sizeof root);
    snprintf(dir, sizeof dir, "%s/%s", root, m.id);
    if (mkdir(root, 0700) != 0 && errno != EEXIST) return -1;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    if (meta_save(&m) != 0) return -1;

    if (id_out) snprintf(id_out, CONV_ID_LEN + 1, "%s", m.id);
    g_version++;
    return 0;
}

int conv_add(const char *id, const char *kind, const char *tool,
             const char *text, int is_error)
{
    if (!id || !conv_valid_id(id) || !kind || !kind_known(kind)) return 0;

    char truncated[CONV_TEXT_MAX + 8];
    utf8_trunc(truncated, sizeof truncated, text ? text : "",
               CONV_TEXT_MAX);

    cJSON *o = cJSON_CreateObject();
    if (!o) return -1;
    cJSON *ok = cJSON_AddNumberToObject(o, "t",
                                        (double)(long long)time(NULL));
    if (ok) ok = cJSON_AddStringToObject(o, "k", kind);
    if (ok && tool && tool[0]) {
        char t[CONV_TOOL_MAX + 8];
        utf8_trunc(t, sizeof t, tool, CONV_TOOL_MAX);
        ok = cJSON_AddStringToObject(o, "tool", t);
    }
    if (ok && truncated[0]) ok = cJSON_AddStringToObject(o, "text", truncated);
    if (ok && is_error) ok = cJSON_AddBoolToObject(o, "err", 1);

    char *line = ok ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!line) return -1;

    char path[4352 + 80];
    conv_path(id, "records.jsonl", path, sizeof path);
    FILE *f = fopen(path, "a");
    if (!f) { free(line); return -1; }
    int rc = fputs(line, f) == EOF || fputc('\n', f) == EOF ? -1 : 0;
    free(line);
    if (fclose(f) != 0) rc = -1;
    g_version++;
    return rc;
}

int conv_finish(const char *id, conv_state_t state)
{
    conv_meta_t m;
    if (!id || meta_load(id, &m) != 0) return -1;
    if (m.state != CONV_RUNNING) return 0; /* finished stays finished */
    m.state = state;
    m.ended = (long long)time(NULL);
    if (meta_save(&m) != 0) return -1;
    g_version++;
    return 0;
}

char *conv_list_json(void)
{
    size_t n;
    conv_meta_t *all = load_metas(&n);
    cJSON *j = cJSON_CreateArray();
    if (!j) { free(all); return NULL; }
    for (size_t i = 0; i < n && j; i++) {
        cJSON *o = meta_to_cjson(&all[i]);
        if (!o) { cJSON_Delete(j); j = NULL; break; }
        cJSON_AddItemToArray(j, o);
    }
    free(all);
    char *s = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    return s;
}

/* one record line -> a json object for the detail view (lenient) */
static cJSON *record_to_cjson(const char *line)
{
    cJSON *j = cJSON_Parse(line);
    if (!j || !cJSON_IsObject(j)) { cJSON_Delete(j); return NULL; }
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "k");
    if (!cJSON_IsString(k) || !k->valuestring || !kind_known(k->valuestring)) {
        cJSON_Delete(j);
        return NULL;
    }
    /* keep only the known fields, in a stable order */
    cJSON *o = cJSON_CreateObject();
    cJSON *ok = o ? cJSON_AddStringToObject(o, "k", k->valuestring) : NULL;
    const cJSON *v;
    v = cJSON_GetObjectItemCaseSensitive(j, "t");
    if (ok) ok = cJSON_AddNumberToObject(o, "t",
        cJSON_IsNumber(v) ? v->valuedouble : 0);
    v = cJSON_GetObjectItemCaseSensitive(j, "tool");
    if (ok && cJSON_IsString(v) && v->valuestring)
        ok = cJSON_AddStringToObject(o, "tool", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(j, "text");
    if (ok && cJSON_IsString(v) && v->valuestring)
        ok = cJSON_AddStringToObject(o, "text", v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(j, "err");
    if (ok && cJSON_IsTrue(v)) ok = cJSON_AddBoolToObject(o, "err", 1);
    cJSON_Delete(j);
    if (!ok) { cJSON_Delete(o); return NULL; }
    return o;
}

char *conv_get_json(const char *id)
{
    if (!conv_valid_id(id)) return NULL;
    conv_meta_t m;
    if (meta_load(id, &m) != 0) return NULL;

    cJSON *o = meta_to_cjson(&m);
    cJSON *recs = cJSON_CreateArray();
    if (!o || !recs) { cJSON_Delete(o); cJSON_Delete(recs); return NULL; }

    char path[4352 + 80];
    conv_path(id, "records.jsonl", path, sizeof path);
    char *buf = read_whole_file(path, 64 * 1024 * 1024);
    size_t dropped = 0, kept = 0;
    if (buf) {
        /* collect the record objects first: a too-long transcript is
         * capped at the newest CONV_RECORDS_MAX records */
        cJSON **parsed = calloc(CONV_RECORDS_MAX, sizeof *parsed);
        if (!parsed) { free(buf); buf = NULL; }
        size_t start = 0;
        for (size_t i = 0; buf && buf[i]; i++) {
            if (buf[i] != '\n') continue;
            buf[i] = '\0';
            cJSON *r = record_to_cjson(buf + start);
            if (r) {
                if (kept == CONV_RECORDS_MAX) {
                    cJSON_Delete(parsed[0]);
                    memmove(parsed, parsed + 1,
                            (CONV_RECORDS_MAX - 1) * sizeof parsed[0]);
                    kept--;
                    dropped++;
                }
                parsed[kept++] = r;
            }
            start = i + 1;
        }
        for (size_t i = 0; i < kept; i++)
            cJSON_AddItemToArray(recs, parsed[i]);
        free(parsed);
        free(buf);
    }
    if (dropped)
        cJSON_AddNumberToObject(o, "records_dropped", (double)dropped);
    cJSON_AddItemToObject(o, "records", recs);

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}
