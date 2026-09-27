/* projects — project list backend, mirroring the theme.c pattern:
 * lenient load, strict save, atomic tmp+rename write. */
#define _POSIX_C_SOURCE 200809L

#include "projects.h"
#include "theme.h" /* theme_dir(): resolved config directory */

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* stat: working dirs must exist on save */
#include <unistd.h> /* getpid */

/* ---------- defaults & limits ---------- */

/* palette cycled through when a project carries no color of its own */
static const char *const DEFAULT_COLORS[] = {
    "#58a6ff", "#ff7b72", "#3fb950", "#d29922",
    "#bc8cff", "#39c5cf", "#f778ba", "#7ee787",
};
#define PALETTE_N (sizeof DEFAULT_COLORS / sizeof DEFAULT_COLORS[0])
#define DEFAULT_EMOJI "\xf0\x9f\x8c\xb8" /* 🌸 */

/* ---------- validation ---------- */

/* #rrggbb */
static int valid_color(const char *s)
{
    if (s[0] != '#' || strlen(s) != 7) return 0;
    for (int i = 1; i < 7; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

/* Valid UTF-8 with no control characters (C0, DEL); n is the byte
 * length. Keeps emoji/titles/path names safe to store and re-emit.
 * multiline additionally allows \n and \t (the free-text detail
 * fields are multi-line by nature). */
static int valid_text(const char *s, size_t n, int multiline)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp;
        int need;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) {
                if (multiline && (c == '\n' || c == '\t')) {
                    i++;
                    continue;
                }
                return 0;
            }
            i++;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f; need = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f; need = 2;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07; need = 3;
        } else {
            return 0; /* stray continuation / invalid lead */
        }
        if (i + (size_t)need >= n) return 0; /* truncated sequence */
        for (int k = 1; k <= need; k++) {
            unsigned char cc = (unsigned char)s[i + (size_t)k];
            if ((cc & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (need == 1 && cp < 0x80) return 0;            /* overlong */
        if (need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)))
            return 0;                                     /* overlong/surrogate */
        if (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
        i += (size_t)need + 1;
    }
    return 1;
}

/* absolute path (starts with '/'), sane length, no control characters */
static int valid_dir(const char *s)
{
    size_t n = strlen(s);
    return n >= 1 && n < PROJECT_DIR_MAX && s[0] == '/' && valid_text(s, n, 0);
}

/* "last component" of a directory path, for the default title */
static const char *dir_basename(const char *dir)
{
    size_t end = strlen(dir);
    while (end > 1 && dir[end - 1] == '/') end--;
    for (size_t i = end; i > 0; i--)
        if (dir[i - 1] == '/' && i < end) return dir + i;
    return dir; /* no slash (or all slashes) -> the whole string */
}

/* ---------- entry parsing (shared by load & PUT) ---------- */

/* the free-text detail fields: json key + offset into project_t */
static const struct { const char *key; size_t off; } DETAIL_FIELDS[] = {
    { "description",  offsetof(project_t, description) },
    { "objectives",   offsetof(project_t, objectives) },
    { "scope",        offsetof(project_t, scope) },
    { "stakeholders", offsetof(project_t, stakeholders) },
};
#define DETAIL_N (sizeof DETAIL_FIELDS / sizeof DETAIL_FIELDS[0])

/*
 * Parse one project object into *out. Strict mode (PUT) rejects any
 * problem into err_field/err_msg; lenient mode (load) repairs missing
 * or broken values with defaults and only fails on an unusable dir.
 * `index` selects the fallback palette color. Returns 0 on success.
 */
static int parse_entry(const cJSON *obj, size_t index, project_t *out,
                       int strict,
                       char *err_field, size_t err_field_n,
                       char *err_msg, size_t err_msg_n)
{
    memset(out, 0, sizeof *out);
    const cJSON *j;

    j = cJSON_GetObjectItemCaseSensitive(obj, "dir");
    if (!cJSON_IsString(j) || !j->valuestring || !valid_dir(j->valuestring)) {
        if (strict) {
            snprintf(err_field, err_field_n, "projects[%zu].dir", index);
            snprintf(err_msg, err_msg_n,
                     "required, an absolute path like /home/you/project");
            return -1;
        }
        return -1; /* lenient: a project without a usable dir is dropped */
    }
    snprintf(out->dir, sizeof out->dir, "%s", j->valuestring);

    /* strict (saving): the working directory must exist on disk. Lenient
     * (loading) keeps the entry — GET reports it with exists:false so
     * the UI can warn and refuse to work until it is fixed. */
    if (strict) {
        struct stat st;
        if (stat(out->dir, &st) != 0) {
            snprintf(err_field, err_field_n, "projects[%zu].dir", index);
            snprintf(err_msg, err_msg_n, "directory does not exist");
            return -1;
        }
        if (!S_ISDIR(st.st_mode)) {
            snprintf(err_field, err_field_n, "projects[%zu].dir", index);
            snprintf(err_msg, err_msg_n, "not a directory");
            return -1;
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "title");
    if (cJSON_IsString(j) && j->valuestring) {
        size_t n = strlen(j->valuestring);
        if (n >= sizeof out->title || !valid_text(j->valuestring, n, 0)) {
            if (strict) {
                snprintf(err_field, err_field_n, "projects[%zu].title", index);
                snprintf(err_msg, err_msg_n, "text, max %d bytes, no control characters",
                         PROJECT_TITLE_MAX - 1);
                return -1;
            }
            snprintf(out->title, sizeof out->title, "%s", dir_basename(out->dir));
        } else {
            snprintf(out->title, sizeof out->title, "%s", j->valuestring);
        }
    }
    if (!out->title[0]) /* missing or empty -> directory name */
        snprintf(out->title, sizeof out->title, "%s", dir_basename(out->dir));

    j = cJSON_GetObjectItemCaseSensitive(obj, "color");
    if (cJSON_IsString(j) && j->valuestring && valid_color(j->valuestring)) {
        snprintf(out->color, sizeof out->color, "%s", j->valuestring);
    } else if (j && strict) {
        snprintf(err_field, err_field_n, "projects[%zu].color", index);
        snprintf(err_msg, err_msg_n, "must be a #rrggbb hex color");
        return -1;
    } else {
        snprintf(out->color, sizeof out->color, "%s",
                 DEFAULT_COLORS[index % PALETTE_N]);
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "emoji");
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0] &&
        strlen(j->valuestring) < sizeof out->emoji &&
        valid_text(j->valuestring, strlen(j->valuestring), 0)) {
        snprintf(out->emoji, sizeof out->emoji, "%s", j->valuestring);
    } else if (j && strict) {
        snprintf(err_field, err_field_n, "projects[%zu].emoji", index);
        snprintf(err_msg, err_msg_n, "one emoji, max %d bytes",
                 PROJECT_EMOJI_MAX - 1);
        return -1;
    } else {
        snprintf(out->emoji, sizeof out->emoji, "%s", DEFAULT_EMOJI);
    }

    /* free-text detail fields: optional, multi-line, capped. Strict
     * (PUT) rejects bad ones; lenient (load) keeps them empty. */
    for (size_t k = 0; k < DETAIL_N; k++) {
        j = cJSON_GetObjectItemCaseSensitive(obj, DETAIL_FIELDS[k].key);
        if (!j) continue; /* missing -> stays empty */
        const char *sv = cJSON_IsString(j) ? j->valuestring : NULL;
        size_t n = sv ? strlen(sv) : 0;
        if (!sv || n >= PROJECT_TEXT_MAX || !valid_text(sv, n, 1)) {
            if (strict) {
                snprintf(err_field, err_field_n, "projects[%zu].%s", index,
                         DETAIL_FIELDS[k].key);
                snprintf(err_msg, err_msg_n,
                         "text, max %d bytes, no control characters "
                         "except newlines", PROJECT_TEXT_MAX - 1);
                return -1;
            }
            continue;
        }
        memcpy((char *)out + DETAIL_FIELDS[k].off, sv, n + 1);
    }

    if (strict) { /* unknown keys are typos -> reject (like theme.c) */
        cJSON_ArrayForEach(j, obj) {
            int known = j->string && (strcmp(j->string, "dir") == 0 ||
                strcmp(j->string, "title") == 0 ||
                strcmp(j->string, "color") == 0 ||
                strcmp(j->string, "emoji") == 0);
            for (size_t k = 0; !known && k < DETAIL_N; k++)
                if (strcmp(j->string, DETAIL_FIELDS[k].key) == 0) known = 1;
            if (!known) {
                snprintf(err_field, err_field_n, "projects[%zu].%s", index,
                         j->string ? j->string : "");
                snprintf(err_msg, err_msg_n, "unknown setting");
                return -1;
            }
        }
    }
    return 0;
}

static int dir_used(const projects_t *p, size_t upto, const char *dir)
{
    for (size_t i = 0; i < upto; i++)
        if (strcmp(p->items[i].dir, dir) == 0) return 1;
    return 0;
}

/* add the detail fields of one project to json object o; 0 on success */
static int add_details(cJSON *o, const project_t *p)
{
    for (size_t k = 0; k < DETAIL_N; k++)
        if (!cJSON_AddStringToObject(o, DETAIL_FIELDS[k].key,
                                     (const char *)p + DETAIL_FIELDS[k].off))
            return -1;
    return 0;
}

/* ---------- load / save ---------- */

static const char *projects_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/projects.json", theme_dir());
    return buf;
}

int projects_load(projects_t *p)
{
    memset(p, 0, sizeof *p);

    char path[4352], *buf = NULL;
    FILE *f = fopen(projects_path(path, sizeof path), "rb");
    if (!f) return 1; /* no file yet -> empty list */
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0 && sz < 1024 * 1024) {
            rewind(f);
            buf = malloc((size_t)sz + 1);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
                buf[sz] = '\0';
            else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    if (!buf) return 1;

    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j || !cJSON_IsArray(j)) {
        cJSON_Delete(j);
        return 1; /* corrupt file -> empty list */
    }

    /* lenient: keep the good entries, drop/repair the rest */
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, j) {
        if (p->count >= PROJECTS_MAX) break;
        project_t one;
        if (!cJSON_IsObject(child)) continue;
        if (parse_entry(child, p->count, &one, 0, NULL, 0, NULL, 0) != 0)
            continue;
        if (dir_used(p, p->count, one.dir)) continue;
        p->items[p->count++] = one;
    }
    cJSON_Delete(j);
    return 0;
}

int projects_save(const projects_t *p)
{
    char path[4352], tmp[4400];
    projects_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    cJSON *j = cJSON_CreateArray();
    if (j)
        for (size_t i = 0; i < p->count; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o ||
                !cJSON_AddStringToObject(o, "dir", p->items[i].dir) ||
                !cJSON_AddStringToObject(o, "title", p->items[i].title) ||
                !cJSON_AddStringToObject(o, "color", p->items[i].color) ||
                !cJSON_AddStringToObject(o, "emoji", p->items[i].emoji) ||
                add_details(o, &p->items[i]) != 0) {
                cJSON_Delete(o);
                cJSON_Delete(j);
                j = NULL;
                break;
            }
            cJSON_AddItemToArray(j, o);
        }
    char *out = j ? cJSON_Print(j) : NULL; /* pretty-printed */
    cJSON_Delete(j);

    int ok = out && fputs(out, f) != EOF && fputc('\n', f) != EOF &&
             fclose(f) == 0;
    free(out);
    if (!ok) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

/* ---------- JSON in (strict) / out ---------- */

projects_parse_result_t projects_from_json(const char *buf, size_t len,
                                           projects_t *out,
                                           char *err_field, size_t err_field_n,
                                           char *err_msg, size_t err_msg_n)
{
    err_field[0] = '\0';
    err_msg[0] = '\0';
    memset(out, 0, sizeof *out);

    char *copy = malloc(len + 1);
    if (!copy) {
        snprintf(err_msg, err_msg_n, "out of memory");
        return PROJECTS_E_JSON;
    }
    memcpy(copy, buf, len);
    copy[len] = '\0';

    cJSON *j = cJSON_Parse(copy);
    free(copy);
    if (!j || !cJSON_IsArray(j)) {
        cJSON_Delete(j);
        snprintf(err_msg, err_msg_n, "expected a JSON array of projects");
        return PROJECTS_E_JSON;
    }

    int n = cJSON_GetArraySize(j);
    if (n > PROJECTS_MAX) {
        cJSON_Delete(j);
        snprintf(err_msg, err_msg_n, "too many projects (max %d)", PROJECTS_MAX);
        return PROJECTS_E_FIELD;
    }

    size_t i = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, j) {
        if (!cJSON_IsObject(child)) {
            snprintf(err_field, err_field_n, "projects[%zu]", i);
            snprintf(err_msg, err_msg_n, "must be an object");
            cJSON_Delete(j);
            return PROJECTS_E_FIELD;
        }
        project_t one;
        if (parse_entry(child, i, &one, 1, err_field, err_field_n,
                        err_msg, err_msg_n) != 0) {
            cJSON_Delete(j);
            return PROJECTS_E_FIELD;
        }
        if (dir_used(out, out->count, one.dir)) {
            snprintf(err_field, err_field_n, "projects[%zu].dir", i);
            snprintf(err_msg, err_msg_n, "duplicate directory");
            cJSON_Delete(j);
            return PROJECTS_E_FIELD;
        }
        out->items[out->count++] = one;
        i++;
    }

    cJSON_Delete(j);
    return PROJECTS_OK;
}

static int dir_on_disk(const char *dir)
{
    struct stat st;
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

/* with_exists adds a live "exists" flag per project (checked at call
 * time) — used by GET so the UI can flag projects whose working
 * directory vanished after saving. */
char *projects_to_json(const projects_t *p, int with_exists)
{
    cJSON *j = cJSON_CreateArray();
    if (!j) return NULL;
    for (size_t i = 0; i < p->count; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o ||
            !cJSON_AddStringToObject(o, "dir", p->items[i].dir) ||
            !cJSON_AddStringToObject(o, "title", p->items[i].title) ||
            !cJSON_AddStringToObject(o, "color", p->items[i].color) ||
            !cJSON_AddStringToObject(o, "emoji", p->items[i].emoji) ||
            add_details(o, &p->items[i]) != 0 ||
            (with_exists &&
             !cJSON_AddBoolToObject(o, "exists", dir_on_disk(p->items[i].dir)))) {
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
