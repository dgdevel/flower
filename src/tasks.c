/* tasks — task store backend, mirroring the
 * projects.c/agents.c pattern: lenient load, strict save, atomic
 * tmp+rename writes, one directory per task. */
#define _POSIX_C_SOURCE 200809L

#include "tasks.h"
#include "agents.h"
#include "theme.h" /* theme_dir(): resolved config directory */

#include <cJSON.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------- helpers ---------- */

static const char *tasks_dir(char *buf, size_t n)
{
    snprintf(buf, n, "%s/tasks", theme_dir());
    return buf;
}

/* random lowercase-hex id, n chars (buf holds n+1); uniqueness, not
 * secrecy, is the goal (same helper as projects.c, longer id). */
static void gen_hex_id(char *buf, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    size_t nb = (n + 1) / 2;
    int ok = nb <= sizeof raw;
    if (ok) {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            ok = fread(raw, 1, nb, f) == nb;
            fclose(f);
        }
    }
    if (!ok) {
        unsigned seed = (unsigned)getpid() ^ (unsigned)time(NULL);
        for (size_t i = 0; i < nb && i < sizeof raw; i++) {
            seed = seed * 1103515245u + 12345u;
            raw[i] = (unsigned char)(seed >> 16);
        }
    }
    for (size_t i = 0; i < n; i++)
        buf[i] = hex[(raw[i / 2] >> (i % 2 ? 0 : 4)) & 0xf];
    buf[n] = '\0';
}

static int valid_id(const char *s)
{
    if (strlen(s) != TASK_ID_LEN) return 0;
    for (size_t i = 0; i < TASK_ID_LEN; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}

/* valid UTF-8 with no control characters (single-line titles) */
static int valid_text(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp;
        int need;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) return 0;
            i++;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f; need = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f; need = 2;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07; need = 3;
        } else {
            return 0;
        }
        if (i + (size_t)need >= n) return 0;
        for (int k = 1; k <= need; k++) {
            unsigned char cc = (unsigned char)s[i + (size_t)k];
            if ((cc & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (need == 1 && cp < 0x80) return 0;
        if (need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)))
            return 0;
        if (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
        i += (size_t)need + 1;
    }
    return 1;
}

/* the bound agent: user-defined or builtin, NULL when dangling */
static const agent_t *find_agent(const agents_t *agents, const char *name)
{
    int i = agents_find(agents, name);
    if (i >= 0) return agents->items[i];
    i = agents_builtin_find(name);
    return i >= 0 ? agents_builtin((size_t)i) : NULL;
}

/* the task's effective llm: its own override, else the bound
 * agent's; "" when neither has one */
static void effective_llm(const task_t *c, const agents_t *agents,
                          char *out, size_t out_n)
{
    if (c->llm[0]) {
        snprintf(out, out_n, "%s", c->llm);
        return;
    }
    const agent_t *a = agents ? find_agent(agents, c->agent) : NULL;
    snprintf(out, out_n, "%s", a ? a->llm : "");
}

static int id_used(const tasks_t *c, size_t upto, const char *id)
{
    for (size_t i = 0; i < upto; i++)
        if (strcmp(c->items[i].id, id) == 0) return 1;
    return 0;
}

static int strptr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* newest first, id as the tiebreaker for a stable order */
static int task_cmp(const void *pa, const void *pb)
{
    const task_t *a = pa, *b = pb;
    if (a->created != b->created) return a->created < b->created ? 1 : -1;
    return strcmp(a->id, b->id);
}

/* ---------- entry parsing (shared by load & PUT) ---------- */

/*
 * Parse one task object into *out. Strict mode (PUT) rejects
 * any problem into err_field/err_msg; lenient mode (load) repairs
 * what it can and skips nothing short of an unusable agent name.
 * `seen` is the list being built (ids are generated to avoid the ids
 * already in it). Returns 0 on success.
 */
static int parse_task(const cJSON *obj, size_t index, task_t *out,
                      const tasks_t *seen,
                      const llms_t *llms, const agents_t *agents,
                      int strict,
                      char *err_field, size_t err_field_n,
                      char *err_msg, size_t err_msg_n)
{
    memset(out, 0, sizeof *out);
    char prefix[48];
    const cJSON *j;
    snprintf(prefix, sizeof prefix, "tasks[%zu]", index);

    /* the id: optional on the wire (new tasks), never
     * rewritten once assigned */
    j = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (cJSON_IsString(j) && j->valuestring && valid_id(j->valuestring)) {
        snprintf(out->id, sizeof out->id, "%s", j->valuestring);
    } else if (j && strict) {
        snprintf(err_field, err_field_n, "%s.id", prefix);
        snprintf(err_msg, err_msg_n,
                 "must be %d lowercase hex characters", TASK_ID_LEN);
        return -1;
    } else {
        do gen_hex_id(out->id, TASK_ID_LEN);
        while (seen && id_used(seen, seen->count, out->id));
    }

    /* the agent binding: required, must exist (user or builtin) */
    j = cJSON_GetObjectItemCaseSensitive(obj, "agent");
    if (!cJSON_IsString(j) || !j->valuestring || !j->valuestring[0] ||
        strlen(j->valuestring) >= sizeof out->agent ||
        !valid_text(j->valuestring, strlen(j->valuestring))) {
        if (strict) {
            snprintf(err_field, err_field_n, "%s.agent", prefix);
            snprintf(err_msg, err_msg_n,
                     "required, the name of an agent (user or builtin)");
            return -1;
        }
        return -1; /* lenient: no usable binding -> unlistable */
    }
    snprintf(out->agent, sizeof out->agent, "%s", j->valuestring);
    if (agents && strict && !find_agent(agents, out->agent)) {
        snprintf(err_field, err_field_n, "%s.agent", prefix);
        snprintf(err_msg, err_msg_n, "unknown agent");
        return -1;
    }

    /* the llm override: "" (or absent) = the agent's own llm */
    j = cJSON_GetObjectItemCaseSensitive(obj, "llm");
    if (cJSON_IsString(j) && j->valuestring &&
        strlen(j->valuestring) < sizeof out->llm &&
        valid_text(j->valuestring, strlen(j->valuestring))) {
        snprintf(out->llm, sizeof out->llm, "%s", j->valuestring);
    } else if (j && strict) {
        snprintf(err_field, err_field_n, "%s.llm", prefix);
        snprintf(err_msg, err_msg_n,
                 "must be the name of an llm, or empty for the agent's");
        return -1;
    }
    if (llms && out->llm[0] && strict && llms_find(llms, out->llm) < 0) {
        snprintf(err_field, err_field_n, "%s.llm", prefix);
        snprintf(err_msg, err_msg_n, "unknown llm");
        return -1;
    }

    /* a task must resolve to some llm: the builtin agents
     * carry none of their own, so theirs select one explicitly */
    if (strict) {
        char eff[CFG_NAME_MAX];
        effective_llm(out, agents, eff, sizeof eff);
        if (!eff[0]) {
            snprintf(err_field, err_field_n, "%s.llm", prefix);
            snprintf(err_msg, err_msg_n,
                     "select an llm — agent \"%s\" has none of its own",
                     out->agent);
            return -1;
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "title");
    if (cJSON_IsString(j) && j->valuestring &&
        strlen(j->valuestring) < sizeof out->title &&
        valid_text(j->valuestring, strlen(j->valuestring))) {
        snprintf(out->title, sizeof out->title, "%s", j->valuestring);
    } else if (j && strict) {
        snprintf(err_field, err_field_n, "%s.title", prefix);
        snprintf(err_msg, err_msg_n, "text, max %d bytes, no control characters",
                 TASK_TITLE_MAX - 1);
        return -1;
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "created");
    if (cJSON_IsNumber(j) && j->valuedouble >= 0 &&
        j->valuedouble == (double)(long long)j->valuedouble) {
        out->created = (long long)j->valuedouble;
    } else if (j && strict) {
        snprintf(err_field, err_field_n, "%s.created", prefix);
        snprintf(err_msg, err_msg_n, "must be a unix timestamp in seconds");
        return -1;
    } else if (!j) {
        out->created = (long long)time(NULL);
    } else {
        out->created = (long long)time(NULL); /* lenient repair */
    }

    if (strict) { /* unknown keys are typos -> reject (like theme.c) */
        static const char *const keys[] = {
            "id", "title", "agent", "llm", "created",
        };
        cJSON_ArrayForEach(j, obj) {
            int known = 0;
            for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
                if (j->string && strcmp(j->string, keys[k]) == 0) known = 1;
            if (!known) {
                snprintf(err_field, err_field_n, "%s.%s", prefix,
                         j->string ? j->string : "");
                snprintf(err_msg, err_msg_n, "unknown setting");
                return -1;
            }
        }
    }
    return 0;
}

/* ---------- load / save ---------- */

static char *read_file(const char *path, long cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0 && sz < cap) {
            rewind(f);
            buf = malloc((size_t)sz + 1);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
                buf[sz] = '\0';
            else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}

int tasks_load(tasks_t *c)
{
    memset(c, 0, sizeof *c);
    char dir[4352];
    tasks_dir(dir, sizeof dir);

    DIR *d = opendir(dir);
    if (!d) return 1; /* no tasks yet */

    /* collect candidate directory names, sorted for a stable order */
    enum { MAX_ENTRIES = 1024 };
    char *names[MAX_ENTRIES];
    size_t nn = 0;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!valid_id(e->d_name)) continue; /* only {ID} directories */
        if (nn >= MAX_ENTRIES) break;
        names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof names[0], strptr_cmp);

    for (size_t i = 0; i < nn && c->count < TASKS_MAX; i++) {
        char name[TASK_ID_LEN + 1];
        snprintf(name, sizeof name, "%s", names[i]);
        free(names[i]);
        char path[4352 + 64];
        snprintf(path, sizeof path, "%s/%s/task.json", dir, name);
        char *buf = read_file(path, 1024 * 1024);
        if (!buf) continue; /* directory without details: not listed */
        cJSON *j = cJSON_Parse(buf);
        free(buf);
        if (!j || !cJSON_IsObject(j)) { cJSON_Delete(j); continue; }
        task_t one;
        /* lenient: no llms/agents context here — dangling references
         * are kept and flagged by GET, like a vanished project dir */
        if (parse_task(j, c->count, &one, c, NULL, NULL, 0,
                       NULL, 0, NULL, 0) != 0) {
            cJSON_Delete(j);
            continue;
        }
        /* the directory is the identity, whatever the file claims */
        snprintf(one.id, sizeof one.id, "%s", name);
        cJSON_Delete(j);
        if (id_used(c, c->count, one.id)) continue;
        c->items[c->count++] = one;
    }
    qsort(c->items, c->count, sizeof c->items[0], task_cmp);
    return 0;
}

/* delete a directory tree (a task's); only ever called on
 * paths inside tasks/ whose name is a valid id */
static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[4352 + 128];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(child);
        else remove(child);
    }
    closedir(d);
    rmdir(path);
}

int tasks_save(const tasks_t *c)
{
    char dir[4352];
    tasks_dir(dir, sizeof dir);
    mkdir(dir, 0700); /* first task creates the store */

    for (size_t i = 0; i < c->count; i++) {
        const task_t *tk = &c->items[i];
        char cdir[4352 + 64], path[4352 + 96], tmp[4352 + 160];
        snprintf(cdir, sizeof cdir, "%s/%s", dir, tk->id);
        snprintf(path, sizeof path, "%s/task.json", cdir);
        snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
        mkdir(cdir, 0700); /* exists for already-saved tasks */

        FILE *f = fopen(tmp, "w");
        if (!f) return -1;
        cJSON *o = cJSON_CreateObject();
        cJSON *ok = o;
        if (ok) ok = cJSON_AddStringToObject(o, "id", tk->id);
        if (ok) ok = cJSON_AddStringToObject(o, "title", tk->title);
        if (ok) ok = cJSON_AddStringToObject(o, "agent", tk->agent);
        if (ok) ok = cJSON_AddStringToObject(o, "llm", tk->llm);
        if (ok) ok = cJSON_AddNumberToObject(o, "created", (double)tk->created);
        char *out = ok ? cJSON_Print(o) : NULL; /* pretty-printed */
        cJSON_Delete(o);

        int wr = out && fputs(out, f) != EOF && fputc('\n', f) != EOF &&
                 fclose(f) == 0;
        free(out);
        if (!wr) {
            remove(tmp);
            return -1;
        }
        if (rename(tmp, path) != 0) {
            remove(tmp);
            return -1;
        }
    }

    /* delete the directories of removed tasks */
    DIR *d = opendir(dir);
    if (!d) return 0;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!valid_id(e->d_name)) continue; /* never touch unknowns */
        char full[4352 + 64];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (id_used(c, c->count, e->d_name)) continue;
        rm_rf(full);
    }
    closedir(d);
    return 0;
}

/* ---------- JSON in (strict) / out ---------- */

tasks_parse_result_t tasks_from_json(const char *buf, size_t len,
                                             const llms_t *llms,
                                             const agents_t *agents,
                                             tasks_t *out,
                                             char *err_field,
                                             size_t err_field_n,
                                             char *err_msg, size_t err_msg_n)
{
    err_field[0] = '\0';
    err_msg[0] = '\0';
    memset(out, 0, sizeof *out);

    char *copy = malloc(len + 1);
    if (!copy) {
        snprintf(err_msg, err_msg_n, "out of memory");
        return TASKS_E_JSON;
    }
    memcpy(copy, buf, len);
    copy[len] = '\0';

    cJSON *j = cJSON_Parse(copy);
    free(copy);
    if (!j || !cJSON_IsArray(j)) {
        cJSON_Delete(j);
        snprintf(err_msg, err_msg_n, "expected a JSON array of tasks");
        return TASKS_E_JSON;
    }

    int n = cJSON_GetArraySize(j);
    if (n > TASKS_MAX) {
        cJSON_Delete(j);
        snprintf(err_msg, err_msg_n, "too many tasks (max %d)",
                 TASKS_MAX);
        return TASKS_E_FIELD;
    }

    size_t i = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, j) {
        if (!cJSON_IsObject(child)) {
            snprintf(err_field, err_field_n, "tasks[%zu]", i);
            snprintf(err_msg, err_msg_n, "must be an object");
            cJSON_Delete(j);
            return TASKS_E_FIELD;
        }
        task_t one;
        if (parse_task(child, i, &one, out, llms, agents, 1,
                       err_field, err_field_n, err_msg, err_msg_n) != 0) {
            cJSON_Delete(j);
            return TASKS_E_FIELD;
        }
        if (id_used(out, out->count, one.id)) {
            snprintf(err_field, err_field_n, "tasks[%zu].id", i);
            snprintf(err_msg, err_msg_n, "duplicate id");
            cJSON_Delete(j);
            return TASKS_E_FIELD;
        }
        out->items[out->count++] = one;
        i++;
    }

    cJSON_Delete(j);
    qsort(out->items, out->count, sizeof out->items[0], task_cmp);
    return TASKS_OK;
}

char *tasks_to_json(const tasks_t *c, int with_flags,
                            const llms_t *llms, const agents_t *agents)
{
    cJSON *j = cJSON_CreateArray();
    if (!j) return NULL;
    for (size_t i = 0; i < c->count; i++) {
        const task_t *tk = &c->items[i];
        cJSON *o = cJSON_CreateObject();
        int ok = o &&
            cJSON_AddStringToObject(o, "id", tk->id) &&
            cJSON_AddStringToObject(o, "title", tk->title) &&
            cJSON_AddStringToObject(o, "agent", tk->agent) &&
            cJSON_AddStringToObject(o, "llm", tk->llm) &&
            cJSON_AddNumberToObject(o, "created", (double)tk->created);
        if (ok && with_flags) {
            char eff[CFG_NAME_MAX];
            effective_llm(tk, agents, eff, sizeof eff);
            ok = cJSON_AddBoolToObject(o, "agent_ok",
                                       agents && find_agent(agents, tk->agent)) &&
                 cJSON_AddBoolToObject(
                     o, "llm_ok",
                     eff[0] && llms && llms_find(llms, eff) >= 0);
        }
        if (!ok) {
            cJSON_Delete(o);
            cJSON_Delete(j);
            return NULL;
        }
        cJSON_AddItemToArray(j, o);
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}
