/* config.c - doc plumbing: schema tables usage, validation, load/save
   under $FLOWER_BASE. split out of flower.c; format:
   docs/config_markdown_format.md */
#define _POSIX_C_SOURCE 200809L
#include "flower.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

int oom(void) {
    fprintf(stderr, "flower: out of memory\n");
    return 1;
}

/* malloc'd sprintf */
char *xasprintf(const char *fmt, ...) {
    va_list ap;
    int n;
    char *p;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;
    p = malloc((size_t)n + 1);
    if (!p) return NULL;
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

void kind_schema(int kind, const char **chapter,
                 const char *const **keys) {
    *chapter = kind == FV_USER ? "User" : "Project";
    *keys = kind == FV_USER ? store_user_keys : store_project_keys;
    if (kind == FV_TASK) {
        *chapter = "Task";
        *keys = store_task_keys;
    }
}

int key_allowed(const char *const *keys, const char *k) {
    for (; *keys; keys++)
        if (!strcmp(*keys, k)) return 1;
    return 0;
}

const char *prefix_known(const char *p) {
    const char *const *q;
    for (q = store_context_prefixes; *q; q++)
        if (!strcmp(*q, p)) return *q;
    return NULL;
}

/* 1 when s is one or more digits (a positive whole number) */
int llm_digits(const char *s) {
    const char *p;
    if (!s || !s[0]) return 0;
    for (p = s; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* ---------- doc mutation helpers (md.c keeps parsing only) ---------- */

md_chapter_t *doc_add_chapter(md_doc_t *doc, const char *title,
                              int level) {
    md_chapter_t *g = realloc(doc->ch, (size_t)(doc->nch + 1) * sizeof *g);
    md_chapter_t *c;
    char *t;
    if (!g) return NULL;
    doc->ch = g;
    t = str_dup(title);
    if (!t) return NULL; /* tail slot stays garbage, nch unchanged */
    c = &doc->ch[doc->nch];
    memset(c, 0, sizeof *c);
    c->title = t;
    c->level = level;
    doc->nch++;
    return c;
}

void doc_remove_chapter(md_doc_t *doc, int idx) {
    md_chapter_t *c = &doc->ch[idx];
    int i;
    free(c->title);
    for (i = 0; i < c->nkv; i++) {
        free(c->keys[i]);
        free(c->vals[i]);
    }
    free(c->keys);
    free(c->vals);
    for (i = 0; i < c->nbody; i++) free(c->body[i].text);
    free(c->body);
    memmove(&doc->ch[idx], &doc->ch[idx + 1],
            (size_t)(doc->nch - idx - 1) * sizeof *doc->ch);
    doc->nch--;
}

/* append one kv pair at the chapter tail, no duplicate check (md.c is
   parsing-only, so repeatable keys grow here). returns 0 ok, -1 oom */
int doc_append_kv(md_chapter_t *c, const char *key, const char *val) {
    char *k = str_dup(key);
    char *v = str_dup(val);
    char **gk = realloc(c->keys, (size_t)(c->nkv + 1) * sizeof *gk);
    char **gv = realloc(c->vals, (size_t)(c->nkv + 1) * sizeof *gv);
    if (!k || !v || !gk || !gv) {
        free(k);
        free(v);
        if (gk) c->keys = gk;
        if (gv) c->vals = gv;
        return -1;
    }
    c->keys = gk;
    c->vals = gv;
    c->keys[c->nkv] = k;
    c->vals[c->nkv] = v;
    c->nkv++;
    return 0;
}

/* replace the chapter value: kv pairs go away, body becomes text split
   on newlines (blank lines dropped, md_parse drops them anyway). */
int chapter_set_body(md_chapter_t *c, const char *text) {
    const char *p = text;
    md_line_t *body = NULL;
    int n = 0;
    int i;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len > 0 && isspace((unsigned char)p[len - 1])) len--;
        if (len > 0) {
            char *line = malloc(len + 1);
            md_line_t *g;
            if (!line) goto fail;
            memcpy(line, p, len);
            line[len] = '\0';
            g = realloc(body, (size_t)(n + 1) * sizeof *g);
            if (!g) {
                free(line);
                goto fail;
            }
            body = g;
            body[n].text = line;
            n++;
        }
        if (!nl) break;
        p = nl + 1;
    }
    for (i = 0; i < c->nkv; i++) {
        free(c->keys[i]);
        free(c->vals[i]);
    }
    free(c->keys);
    free(c->vals);
    c->keys = NULL;
    c->vals = NULL;
    c->nkv = 0;
    for (i = 0; i < c->nbody; i++) free(c->body[i].text);
    free(c->body);
    c->body = body;
    c->nbody = n;
    return 0;
fail:
    for (i = 0; i < n; i++) free(body[i].text);
    free(body);
    return -1;
}

/* title part of a context chapter, NULL when the title is not a valid
   "<Prefix>: <Title>" */
char *context_title_part(const char *chapter_title) {
    char *pfx, *t;
    if (!store_split_context(chapter_title, &pfx, &t)) return NULL;
    free(pfx);
    return t;
}

/* "A<n>: <Title>" split. 1 ok (malloc'd id + title), 0 bad shape.
   title may come back empty ("A3: ") so callers can reject it. */
int action_split(const char *chapter_title, char **id, char **title) {
    const char *p = chapter_title;
    char *end;
    long n;
    char idbuf[24];
    *id = NULL;
    *title = NULL;
    if (!p || p[0] != 'A') return 0;
    errno = 0;
    n = strtol(p + 1, &end, 10);
    if (errno != 0 || end == p + 1 || n < 1) return 0;
    if (*end != ':' || end[1] != ' ') return 0;
    snprintf(idbuf, sizeof idbuf, "A%ld", n);
    *id = str_dup(idbuf);
    *title = str_dup(end + 2);
    if (!*id || !*title) {
        free(*id);
        free(*title);
        *id = NULL;
        *title = NULL;
        return 0;
    }
    return 1;
}

/* 1 when the chapter title is a valid "# A<n>: <Title>" */
int action_valid_title(const char *t) {
    char *id, *title;
    int ok = action_split(t, &id, &title);
    if (!ok) return 0;
    ok = title[0] != '\0';
    free(id);
    free(title);
    return ok;
}

/* 1 when some chapter is action id ("A1" matching "A1: title") */
int action_exists(const md_doc_t *doc, const char *id) {
    int i;
    for (i = 0; i < doc->nch; i++) {
        char *cid, *t;
        int eq;
        if (!action_split(doc->ch[i].title, &cid, &t)) continue;
        eq = !strcmp(cid, id);
        free(cid);
        free(t);
        if (eq) return 1;
    }
    return 0;
}

/* malloc'd title text of an action chapter, NULL oom */
char *action_title_of(const md_chapter_t *c) {
    char *id, *t;
    if (!action_split(c->title, &id, &t)) return str_dup("");
    free(id);
    return t;
}

/* 1 when some chapter (other than skip) already holds this title */
int context_title_taken(md_doc_t *doc, const char *title,
                        const md_chapter_t *skip) {
    int i;
    for (i = 0; i < doc->nch; i++) {
        char *t;
        int eq;
        if (&doc->ch[i] == skip) continue;
        t = context_title_part(doc->ch[i].title);
        if (!t) continue;
        eq = !strcmp(t, title);
        free(t);
        if (eq) return 1;
    }
    return 0;
}

/* ---------- load/save plumbing ---------- */

/* schema check of an already parsed file; rejects stray chapters, keys
   and key-value pairs where only bare text may live. on failure writes
   the reason into out (snprintf-style, always NUL-terminated) and
   returns 1. */
int doc_check(int kind, const md_doc_t *doc, char *out, size_t outn) {
    int i, j, k;
    for (i = 0; i < doc->nch; i++) {
        md_chapter_t *c = &doc->ch[i];
        if (kind == FV_USER || kind == FV_PROJECT || kind == FV_TASK) {
            const char *chapter = NULL;
            const char *const *keys = NULL;
            kind_schema(kind, &chapter, &keys);
            if (c->level != 1 || strcmp(c->title, chapter) != 0) {
                snprintf(out, outn, "unexpected chapter '%s'", c->title);
                return 1;
            }
            for (j = 0; j < c->nkv; j++) {
                if (!key_allowed(keys, c->keys[j])) {
                    snprintf(out, outn, "unknown key '%s' in chapter '%s'",
                             c->keys[j], c->title);
                    return 1;
                }
                for (k = 0; k < j; k++)
                    if (!strcmp(c->keys[k], c->keys[j])) {
                        snprintf(out, outn,
                                 "duplicate key '%s' in chapter '%s'",
                                 c->keys[j], c->title);
                        return 1;
                    }
            }
        } else {
            int ok = c->level == 1 && c->title[0] != '\0';
            if (ok && kind == FV_FIELDS)
                ok = key_allowed(store_detail_fields, c->title);
            else if (ok && kind == FV_CONTEXT)
                ok = store_is_context_title(c->title);
            else if (ok && kind == FV_ACTIONS)
                ok = action_valid_title(c->title);
            else if (ok && kind == FV_LLM)
                ok = store_valid_llm_name(c->title);
            if (!ok) {
                snprintf(out, outn, "unexpected chapter '%s'", c->title);
                return 1;
            }
            if (kind != FV_ACTIONS && kind != FV_LLM && c->nkv > 0) {
                snprintf(out, outn, "chapter '%s' must be bare text",
                         c->title);
                return 1;
            }
            if (kind == FV_ACTIONS) {
                int nkv_status = 0;
                for (j = 0; j < c->nkv; j++) {
                    if (!key_allowed(store_action_keys, c->keys[j])) {
                        snprintf(out, outn,
                                 "unknown key '%s' in chapter '%s'",
                                 c->keys[j], c->title);
                        return 1;
                    }
                    for (k = 0; k < j; k++)
                        if (!strcmp(c->keys[k], c->keys[j])) {
                            snprintf(out, outn,
                                     "duplicate key '%s' in chapter '%s'",
                                     c->keys[j], c->title);
                            return 1;
                        }
                    if (!strcmp(c->keys[j], "Status")) {
                        nkv_status = 1;
                        if (!store_valid_status(c->vals[j])) {
                            snprintf(out, outn,
                                     "invalid Status '%s' in chapter "
                                     "'%s'",
                                     c->vals[j], c->title);
                            return 1;
                        }
                    }
                }
                if (!nkv_status) {
                    snprintf(out, outn,
                             "chapter '%s' has no Status", c->title);
                    return 1;
                }
            }
            if (kind == FV_LLM) {
                int seen_base = 0, seen_protocol = 0;
                for (j = 0; j < c->nkv; j++) {
                    const char *key = c->keys[j];
                    if (!key_allowed(store_llm_keys, key)) {
                        snprintf(out, outn,
                                 "unknown key '%s' in chapter '%s'",
                                 key, c->title);
                        return 1;
                    }
                    /* Header/Mcp proxy/Terminal tool repeat, the rest
                       must stay single */
                    if (strcmp(key, "Header") && strcmp(key, "Mcp proxy") &&
                        strcmp(key, "Terminal tool")) {
                        for (k = 0; k < j; k++)
                            if (!strcmp(c->keys[k], key)) {
                                snprintf(out, outn,
                                         "duplicate key '%s' in chapter "
                                         "'%s'",
                                         key, c->title);
                                return 1;
                            }
                    }
                    if (!strcmp(key, "Api base")) {
                        seen_base = 1;
                        if (!c->vals[j][0]) {
                            snprintf(out, outn,
                                     "empty Api base in chapter '%s'",
                                     c->title);
                            return 1;
                        }
                    } else if (!strcmp(key, "Protocol")) {
                        seen_protocol = 1;
                        if (!store_valid_llm_protocol(c->vals[j])) {
                            snprintf(out, outn,
                                     "invalid Protocol '%s' in chapter "
                                     "'%s'",
                                     c->vals[j], c->title);
                            return 1;
                        }
                    } else if (!strcmp(key, "Max tokens") &&
                               !llm_digits(c->vals[j])) {
                        snprintf(out, outn,
                                 "invalid Max tokens '%s' in chapter "
                                 "'%s'",
                                 c->vals[j], c->title);
                        return 1;
                    }
                }
                if (!seen_base || !seen_protocol) {
                    snprintf(out, outn,
                             "chapter '%s' needs Api base and Protocol",
                             c->title);
                    return 1;
                }
            }
        }
    }
    if (kind == FV_ACTIONS) {
        /* Depends ids must exist as chapters in this same file */
        for (i = 0; i < doc->nch; i++) {
            const char *dep = md_kv_get((const char **)doc->ch[i].keys,
                                        (const char **)doc->ch[i].vals,
                                        doc->ch[i].nkv, "Depends");
            char **ids = NULL;
            int n, j;
            if (!dep) continue;
            n = store_parse_id_list(dep, &ids);
            if (n < 0) return oom();
            for (j = 0; j < n; j++) {
                if (!action_exists(doc, ids[j])) {
                    snprintf(out, outn, "unknown action id '%s' in "
                                        "Depends of chapter '%s'",
                             ids[j], doc->ch[i].title);
                    store_free_ids(ids, n);
                    return 1;
                }
            }
            store_free_ids(ids, n);
        }
    }
    return 0;
}

int validate_doc(const char *path, int kind, md_doc_t *doc) {
    char reason[512];
    int rc = doc_check(kind, doc, reason, sizeof reason);
    if (rc != 0) fprintf(stderr, "flower: %s: %s\n", path, reason);
    return rc;
}

/* load store_path(rel) when present and validate it. 0 ok (doc may be
   empty), 1 after printing why (doc freed). */
int load_rel(const char *rel, int kind, md_doc_t *doc) {
    char *path = store_path(rel);
    int rc;
    if (!path) return oom();
    rc = md_load(path, doc);
    if (rc == 1) { /* not there yet: fresh empty doc */
        free(path);
        return 0;
    }
    if (rc != 0) {
        fprintf(stderr, "flower: cannot parse %s: %s\n", path,
                rc == -2 ? "duplicate chapter title"
                         : rc == -3 ? "malformed structure" : "read error");
        free(path);
        return 1;
    }
    rc = validate_doc(path, kind, doc);
    free(path);
    if (rc != 0) {
        md_free_doc(doc);
        return 1;
    }
    return 0;
}

/* print path as absolute: prefix it with the base directory when the
   caller passed a relative one. realpath is not used: it would fail on
   the freshly removed files too, not only on the just-written ones */
void print_abs(const char *path) {
    const char *base = store_base();
    if (path[0] == '/') {
        puts(path);
    } else if (base && base[0] == '/') {
        char *abs = xasprintf("%s/%s", base, path);
        if (!abs) {
            puts(path);
            return;
        }
        puts(abs);
        free(abs);
    } else {
        puts(path);
    }
}

/* save doc to store_path(rel), creating mkdir_rel first when given,
   then print the absolute path */
int save_rel(const char *mkdir_rel, const char *rel,
             const md_doc_t *doc) {
    char *path = store_path(rel);
    int rc = 0;
    if (!path) return oom();
    if (mkdir_rel && store_mkdir_p(mkdir_rel) != 0) rc = 1;
    if (rc == 0 && md_save(path, doc) != 0) rc = 1;
    if (rc == 0) print_abs(path);
    free(path);
    return rc;
}

/* dump the raw file to stdout; missing file prints nothing */
int print_rel_file(const char *rel) {
    char *path = store_path(rel);
    FILE *f;
    char chunk[4096];
    size_t n;
    int rc = 0;
    if (!path) return oom();
    f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) {
            fprintf(stderr, "flower: cannot read %s: %s\n", path,
                    strerror(errno));
            rc = 1;
        }
        free(path);
        return rc;
    }
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        fwrite(chunk, 1, n, stdout);
    if (ferror(f) || ferror(stdout)) {
        fprintf(stderr, "flower: read/write error on %s\n", path);
        rc = 1;
    }
    fclose(f);
    free(path);
    return rc;
}

int rel_dir_exists(const char *rel) {
    char *path = store_path(rel);
    struct stat st;
    int ok = path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    free(path);
    return ok;
}

int require_project(const char *pid) {
    char *rel = xasprintf("projects/%s", pid);
    int ok = rel && rel_dir_exists(rel);
    free(rel);
    if (!ok) {
        fprintf(stderr, "flower: no such project %s\n", pid);
        return 0;
    }
    return 1;
}

int require_task(const char *pid, const char *tid) {
    char *rel;
    int ok;
    if (!require_project(pid)) return 0;
    rel = xasprintf("projects/%s/tasks/%s", pid, tid);
    ok = rel && rel_dir_exists(rel);
    free(rel);
    if (!ok) {
        fprintf(stderr, "flower: no such task %s\n", tid);
        return 0;
    }
    return 1;
}
