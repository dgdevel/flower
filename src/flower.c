/* flower.c - command layer: setup and project/task configuration
   editing. grammar: docs/config_cli.md, layout: docs/config_layout.md,
   file format: docs/config_markdown_format.md. */
#define _POSIX_C_SOURCE 200809L
#include "flower.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h> /* realpath, PATH_MAX */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int usage(FILE *out) {
    fputs("usage: flower setup\n"
          "       flower project <P> set <Key> \"<Value>\"\n"
          "       flower project <P> show\n"
          "       flower project <P> details set <Field> \"<text>\"\n"
          "       flower project <P> details remove <Field>\n"
          "       flower project <P> details show [Field]\n"
          "       flower project <P> context add <Prefix> \"<Title>\" "
          "\"<Body>\"\n"
          "       flower project <P> context edit <Prefix> \"<Title>\" "
          "\"<NewBody>\"\n"
          "       flower project <P> context rename <Prefix> \"<Title>\" "
          "\"<NewTitle>\"\n"
          "       flower project <P> context remove <Prefix> \"<Title>\"\n"
          "       flower project <P> context list\n"
          "       flower project <P> resource add \"<Name>\" \"<Pointer>\"\n"
          "       flower project <P> resource set \"<Name>\" \"<Pointer>\"\n"
          "       flower project <P> resource remove \"<Name>\"\n"
          "       flower project <P> resource list\n"
          "       flower project <P> task <T> set <Key> \"<Value>\"\n"
          "       flower project <P> task <T> show\n"
          "       flower project <P> task <T> context <verb> [args]\n"
          "       flower project <P> task <T> action add \"<Title>\" "
          "[--description \"<text>\"]\n"
          "       flower project <P> task <T> action <A> set <Key> "
          "\"<Value>\"\n"
          "       flower project <P> task <T> action <A> remove\n"
          "       flower project <P> task <T> action list "
          "[--pending|--complete]\n"
          "       flower project <P> remove\n"
          "       flower project <P> task <T> remove\n"
          "       flower edit <path>\n",
          out);
    return 1;
}

static int oom(void) {
    fprintf(stderr, "flower: out of memory\n");
    return 1;
}

/* malloc'd sprintf */
static char *xasprintf(const char *fmt, ...) {
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

/* strip one pair of surrounding double quotes. returns NULL after
   complaining when the quotes are unbalanced. mutates s in place. */
static char *arg_val(char *s) {
    size_t n = strlen(s);
    int open = n > 0 && s[0] == '"';
    int close = n > 0 && s[n - 1] == '"';
    if (open != close) {
        fprintf(stderr, "flower: unbalanced quotes in '%s'\n", s);
        return NULL;
    }
    if (open && n >= 2) {
        s[n - 1] = '\0';
        return s + 1;
    }
    if (open) { /* the string is one lone quote char: empty value */
        s[0] = '\0';
        return s;
    }
    return s;
}

/* file kinds for load-time validation */
enum {
    FV_USER,     /* flower.md: one "# User" kv chapter */
    FV_PROJECT,  /* config.md: one "# Project" kv chapter */
    FV_TASK,     /* config.md: one "# Task" kv chapter */
    FV_FIELDS,   /* details.md: chapters from store_detail_fields */
    FV_CONTEXT,  /* context.md: "<Prefix>: <Title>" bare chapters */
    FV_RESOURCE, /* resources.md: any name, bare value */
    FV_ACTIONS   /* actions.md: "# A<n>: <Title>" kv chapters */
};

static void kind_schema(int kind, const char **chapter,
                        const char *const **keys) {
    *chapter = kind == FV_USER ? "User" : "Project";
    *keys = kind == FV_USER ? store_user_keys : store_project_keys;
    if (kind == FV_TASK) {
        *chapter = "Task";
        *keys = store_task_keys;
    }
}

static int key_allowed(const char *const *keys, const char *k) {
    for (; *keys; keys++)
        if (!strcmp(*keys, k)) return 1;
    return 0;
}

static const char *prefix_known(const char *p) {
    const char *const *q;
    for (q = store_context_prefixes; *q; q++)
        if (!strcmp(*q, p)) return *q;
    return NULL;
}

/* ---------- doc mutation helpers (md.c keeps parsing only) ---------- */

static md_chapter_t *doc_add_chapter(md_doc_t *doc, const char *title,
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

static void doc_remove_chapter(md_doc_t *doc, int idx) {
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

/* replace the chapter value: kv pairs go away, body becomes text split
   on newlines (blank lines dropped, md_parse drops them anyway). */
static int chapter_set_body(md_chapter_t *c, const char *text) {
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
static char *context_title_part(const char *chapter_title) {
    char *pfx, *t;
    if (!store_split_context(chapter_title, &pfx, &t)) return NULL;
    free(pfx);
    return t;
}

/* "A<n>: <Title>" split. 1 ok (malloc'd id + title), 0 bad shape.
   title may come back empty ("A3: ") so callers can reject it. */
static int action_split(const char *chapter_title, char **id, char **title) {
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
static int action_valid_title(const char *t) {
    char *id, *title;
    int ok = action_split(t, &id, &title);
    if (!ok) return 0;
    ok = title[0] != '\0';
    free(id);
    free(title);
    return ok;
}

/* 1 when some chapter is action id ("A1" matching "A1: title") */
static int action_exists(const md_doc_t *doc, const char *id) {
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

/* 1 when some chapter (other than skip) already holds this title */
static int context_title_taken(md_doc_t *doc, const char *title,
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

/* free one store_parse_id_list result */
static void store_free_ids(char **ids, int n) {
    int i;
    if (!ids) return;
    for (i = 0; i < n; i++) free(ids[i]);
    free(ids);
}

/* ---------- load/save plumbing ---------- */

/* schema check of an already parsed file; rejects stray chapters, keys
   and key-value pairs where only bare text may live. on failure writes
   the reason into out (snprintf-style, always NUL-terminated) and
   returns 1. */
static int doc_check(int kind, const md_doc_t *doc, char *out, size_t outn) {
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
            if (!ok) {
                snprintf(out, outn, "unexpected chapter '%s'", c->title);
                return 1;
            }
            if (kind != FV_ACTIONS && c->nkv > 0) {
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

static int validate_doc(const char *path, int kind, md_doc_t *doc) {
    char reason[512];
    int rc = doc_check(kind, doc, reason, sizeof reason);
    if (rc != 0) fprintf(stderr, "flower: %s: %s\n", path, reason);
    return rc;
}

/* load store_path(rel) when present and validate it. 0 ok (doc may be
   empty), 1 after printing why (doc freed). */
static int load_rel(const char *rel, int kind, md_doc_t *doc) {
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
static void print_abs(const char *path) {
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
static int save_rel(const char *mkdir_rel, const char *rel,
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
static int print_rel_file(const char *rel) {
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

static int rel_dir_exists(const char *rel) {
    char *path = store_path(rel);
    struct stat st;
    int ok = path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    free(path);
    return ok;
}

static int require_project(const char *pid) {
    char *rel = xasprintf("projects/%s", pid);
    int ok = rel && rel_dir_exists(rel);
    free(rel);
    if (!ok) {
        fprintf(stderr, "flower: no such project %s\n", pid);
        return 0;
    }
    return 1;
}

static int require_task(const char *pid, const char *tid) {
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

/* ---------- setup ---------- */

/* prompt for one key; empty line or eof keeps the current value */
static int prompt_kv(md_chapter_t *c, const char *key) {
    char line[4096];
    const char *cur =
        md_kv_get((const char **)c->keys, (const char **)c->vals, c->nkv, key);
    if (cur && cur[0]) printf("%s [%s]: ", key, cur);
    else printf("%s: ", key);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return 0;
    str_trim(line);
    if (!line[0]) return 0;
    return md_kv_set(c, key, line);
}

static int cmd_setup(void) {
    char *rel = "flower.md";
    md_doc_t doc;
    md_chapter_t *u;
    int rc = 1;
    if (load_rel(rel, FV_USER, &doc)) return 1;
    u = md_find(&doc, "User");
    if (!u) u = doc_add_chapter(&doc, "User", 1);
    if (!u) goto out;
    if (prompt_kv(u, "Name") != 0) goto out;
    if (prompt_kv(u, "Role") != 0) goto out;
    if (store_mkdir_p(NULL) != 0) goto out;
    rc = save_rel(NULL, rel, &doc);
out:
    md_free_doc(&doc);
    return rc;
}

/* ---------- config.md (user/project/task share the shape) ---------- */

static int cmd_config_set(int kind, const char *dir_rel, const char *file_rel,
                          char *key, char *val) {
    const char *chapter = NULL;
    const char *const *keys = NULL;
    md_doc_t doc;
    md_chapter_t *c;
    int rc;
    kind_schema(kind, &chapter, &keys);
    if (!key_allowed(keys, key)) {
        fprintf(stderr, "flower: unknown key '%s'\n", key);
        return 1;
    }
    if (load_rel(file_rel, kind, &doc)) return 1;
    c = md_find(&doc, chapter);
    if (!c) c = doc_add_chapter(&doc, chapter, 1);
    if (!c) {
        md_free_doc(&doc);
        return oom();
    }
    if (md_kv_set(c, key, val) != 0) {
        md_free_doc(&doc);
        return oom();
    }
    rc = save_rel(dir_rel, file_rel, &doc);
    md_free_doc(&doc);
    return rc;
}

/* ---------- details ---------- */

/* a[0]=verb, n=args after it */
static int cmd_details(const char *pid, char **a, int n) {
    const char *verb = a[0];
    char *file = xasprintf("projects/%s/details.md", pid);
    md_doc_t doc;
    int rc = 1;
    if (!file) return oom();

    if (!strcmp(verb, "set") && n == 3) {
        char *field = arg_val(a[1]);
        char *text = arg_val(a[2]);
        md_chapter_t *c;
        if (!field || !text) goto usage_out;
        if (!key_allowed(store_detail_fields, field)) {
            fprintf(stderr, "flower: unknown field '%s'\n", field);
            goto out;
        }
        if (load_rel(file, FV_FIELDS, &doc)) goto out;
        c = md_find(&doc, field);
        if (!c) c = doc_add_chapter(&doc, field, 1);
        if (!c || chapter_set_body(c, text) != 0) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

    if (!strcmp(verb, "remove") && n == 2) {
        char *field = arg_val(a[1]);
        md_chapter_t *c;
        if (!field) goto usage_out;
        if (!key_allowed(store_detail_fields, field)) {
            fprintf(stderr, "flower: unknown field '%s'\n", field);
            goto out;
        }
        if (load_rel(file, FV_FIELDS, &doc)) goto out;
        c = md_find(&doc, field);
        if (!c) {
            fprintf(stderr, "flower: no such field '%s'\n", field);
            md_free_doc(&doc);
            goto out;
        }
        doc_remove_chapter(&doc, (int)(c - doc.ch));
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

    if (!strcmp(verb, "show") && (n == 1 || n == 2)) {
        if (n == 1) {
            rc = print_rel_file(file);
            goto out;
        }
        {
            char *field = arg_val(a[1]);
            md_chapter_t *c;
            int i;
            if (!field) goto usage_out;
            if (!key_allowed(store_detail_fields, field)) {
                fprintf(stderr, "flower: unknown field '%s'\n", field);
                goto out;
            }
            if (load_rel(file, FV_FIELDS, &doc)) goto out;
            c = md_find(&doc, field);
            if (!c) {
                fprintf(stderr, "flower: no such field '%s'\n", field);
                md_free_doc(&doc);
                goto out;
            }
            for (i = 0; i < c->nbody; i++)
                printf("%s\n", c->body[i].text);
            md_free_doc(&doc);
            rc = ferror(stdout) ? 1 : 0;
            goto out;
        }
    }

usage_out:
    rc = usage(stderr);
out:
    free(file);
    return rc;
}

/* ---------- context (project and task files share the shape) ---------- */

/* a[0]=verb, n=args after it */
static int cmd_context(const char *file_rel, char **a, int n) {
    const char *verb;
    char *pfx_arg, *title_arg;
    const char *pfx;
    char *full;
    md_doc_t doc;
    md_chapter_t *ch;
    int rc = 1;

    if (n < 1) return usage(stderr);
    verb = a[0];

    if (!strcmp(verb, "list")) {
        int i;
        if (n != 1) return usage(stderr);
        if (load_rel(file_rel, FV_CONTEXT, &doc)) return 1;
        for (i = 0; i < doc.nch; i++) printf("%s\n", doc.ch[i].title);
        md_free_doc(&doc);
        return ferror(stdout) ? 1 : 0;
    }

    if (strcmp(verb, "add") && strcmp(verb, "edit") && strcmp(verb, "rename") &&
        strcmp(verb, "remove"))
        return usage(stderr);
    if ((!strcmp(verb, "add") || !strcmp(verb, "edit") ||
         !strcmp(verb, "rename")) &&
        n != 4)
        return usage(stderr);
    if (!strcmp(verb, "remove") && n != 3) return usage(stderr);

    pfx_arg = arg_val(a[1]);
    title_arg = arg_val(a[2]);
    if (!pfx_arg || !title_arg) return usage(stderr);
    pfx = prefix_known(pfx_arg);
    if (!pfx) {
        fprintf(stderr, "flower: unknown prefix '%s'\n", pfx_arg);
        return 1;
    }
    if (!title_arg[0]) {
        fprintf(stderr, "flower: empty title\n");
        return 1;
    }
    full = store_make_context_title(pfx, title_arg);
    if (!full) return oom();
    if (load_rel(file_rel, FV_CONTEXT, &doc)) {
        free(full);
        return 1;
    }
    ch = md_find(&doc, full);

    if (!strcmp(verb, "add")) {
        char *body = arg_val(a[3]);
        if (!body) {
            rc = usage(stderr);
            goto out;
        }
        if (ch || context_title_taken(&doc, title_arg, NULL)) {
            fprintf(stderr, "flower: duplicate context title '%s'\n",
                    title_arg);
            goto out;
        }
        ch = doc_add_chapter(&doc, full, 1);
        if (!ch || chapter_set_body(ch, body) != 0) {
            rc = oom();
            goto out;
        }
    } else if (!strcmp(verb, "edit")) {
        char *body = arg_val(a[3]);
        if (!body) {
            rc = usage(stderr);
            goto out;
        }
        if (!ch) {
            fprintf(stderr, "flower: no such context '%s'\n", full);
            goto out;
        }
        if (chapter_set_body(ch, body) != 0) {
            rc = oom();
            goto out;
        }
    } else if (!strcmp(verb, "rename")) {
        char *nt = arg_val(a[3]);
        char *nf;
        if (!nt) {
            rc = usage(stderr);
            goto out;
        }
        if (!nt[0]) {
            fprintf(stderr, "flower: empty title\n");
            goto out;
        }
        if (!ch) {
            fprintf(stderr, "flower: no such context '%s'\n", full);
            goto out;
        }
        if (context_title_taken(&doc, nt, ch)) {
            fprintf(stderr, "flower: duplicate context title '%s'\n", nt);
            goto out;
        }
        nf = store_make_context_title(pfx, nt);
        if (!nf) {
            rc = oom();
            goto out;
        }
        free(ch->title);
        ch->title = nf;
    } else { /* remove */
        if (!ch) {
            fprintf(stderr, "flower: no such context '%s'\n", full);
            goto out;
        }
        doc_remove_chapter(&doc, (int)(ch - doc.ch));
    }

    rc = save_rel(NULL, file_rel, &doc);
out:
    md_free_doc(&doc);
    free(full);
    return rc;
}

/* ---------- resources ---------- */

/* a[0]=verb, n=args after it */
static int cmd_resource(const char *pid, char **a, int n) {
    const char *verb = a[0];
    char *file = xasprintf("projects/%s/resources.md", pid);
    md_doc_t doc;
    int rc = 1;
    if (!file) return oom();

    if (!strcmp(verb, "list") && n == 1) {
        int i, j;
        if (load_rel(file, FV_RESOURCE, &doc)) {
            free(file);
            return 1;
        }
        for (i = 0; i < doc.nch; i++) {
            printf("%s\n", doc.ch[i].title);
            for (j = 0; j < doc.ch[i].nbody; j++)
                printf("  %s\n", doc.ch[i].body[j].text);
        }
        md_free_doc(&doc);
        free(file);
        return ferror(stdout) ? 1 : 0;
    }

    if ((!strcmp(verb, "add") || !strcmp(verb, "set")) && n == 3) {
        char *name = arg_val(a[1]);
        char *ptr = arg_val(a[2]);
        md_chapter_t *c;
        if (!name || !ptr) goto usage_out;
        if (!name[0]) {
            fprintf(stderr, "flower: empty resource name\n");
            goto out;
        }
        if (load_rel(file, FV_RESOURCE, &doc)) goto out;
        c = md_find(&doc, name);
        if (!strcmp(verb, "add")) {
            if (c) {
                fprintf(stderr, "flower: duplicate resource '%s'\n", name);
                goto out_doc;
            }
            c = doc_add_chapter(&doc, name, 1);
        } else if (!c) {
            fprintf(stderr, "flower: no such resource '%s'\n", name);
            goto out_doc;
        }
        if (!c || chapter_set_body(c, ptr) != 0) {
            rc = oom();
            goto out_doc;
        }
        rc = save_rel(NULL, file, &doc);
        goto out_doc;
    }

    if (!strcmp(verb, "remove") && n == 2) {
        char *name = arg_val(a[1]);
        md_chapter_t *c;
        if (!name) goto usage_out;
        if (load_rel(file, FV_RESOURCE, &doc)) goto out;
        c = md_find(&doc, name);
        if (!c) {
            fprintf(stderr, "flower: no such resource '%s'\n", name);
            goto out_doc;
        }
        doc_remove_chapter(&doc, (int)(c - doc.ch));
        rc = save_rel(NULL, file, &doc);
        goto out_doc;
    }

usage_out:
    rc = usage(stderr);
    goto out;
out_doc:
    md_free_doc(&doc);
out:
    free(file);
    return rc;
}

/* ---------- actions ---------- */

/* chapter for action id, NULL when absent. id must be validated. */
static md_chapter_t *action_find(md_doc_t *doc, const char *id) {
    char want[64];
    int i;
    snprintf(want, sizeof want, "%s: ", id);
    for (i = 0; i < doc->nch; i++)
        if (str_starts(doc->ch[i].title, want) &&
            action_valid_title(doc->ch[i].title))
            return &doc->ch[i];
    return NULL;
}

/* malloc'd title text of an action chapter, NULL oom */
static char *action_title_of(const md_chapter_t *c) {
    char *id, *t;
    if (!action_split(c->title, &id, &t)) return str_dup("");
    free(id);
    return t;
}

/* 1 when some action's Depends mentions id (skip's own Depends is
   ignored: self-reference is rejected at set time, not here) */
static char *action_depended_on(md_doc_t *doc, const char *id,
                                md_chapter_t *skip) {
    int i;
    for (i = 0; i < doc->nch; i++) {
        md_chapter_t *c = &doc->ch[i];
        const char *dep = md_kv_get((const char **)c->keys,
                                    (const char **)c->vals, c->nkv,
                                    "Depends");
        char **ids = NULL;
        int n, j;
        if (!dep) continue;
        n = store_parse_id_list(dep, &ids);
        if (n < 0) return NULL; /* oom marker: callers see no clash */
        for (j = 0; j < n; j++) {
            if (!strcmp(ids[j], id) && c != skip) {
                store_free_ids(ids, n);
                return c->title;
            }
        }
        store_free_ids(ids, n);
    }
    return NULL;
}

/* a[0]=verb or action id, n=args after it */
static int cmd_action(const char *pid, const char *tid, char **a, int n) {
    char *file = xasprintf("projects/%s/tasks/%s/actions.md", pid, tid);
    md_doc_t doc;
    int rc = 1;
    if (!file) return oom();

    if (!strcmp(a[0], "add")) {
        char *title = NULL;
        char *desc = NULL;
        char *id = NULL;
        md_chapter_t *c;
        int i;
        if (n < 2) goto usage_out;
        title = arg_val(a[1]);
        if (!title) goto usage_out;
        if (!title[0]) {
            fprintf(stderr, "flower: empty action title\n");
            goto out;
        }
        for (i = 2; i < n; i++) {
            if (!strcmp(a[i], "--description") && i + 1 < n) {
                desc = arg_val(a[i + 1]);
                if (!desc) goto usage_out;
                i++;
            } else
                goto usage_out;
        }
        if (load_rel(file, FV_ACTIONS, &doc)) goto out;
        /* duplicate title guard across existing chapters */
        {
            char *dup = NULL;
            for (i = 0; i < doc.nch && !dup; i++) {
                char *t = action_title_of(&doc.ch[i]);
                if (t && !strcmp(t, title)) dup = doc.ch[i].title;
                free(t);
            }
            if (dup) {
                fprintf(stderr, "flower: duplicate action title '%s' "
                                "(%s)\n",
                        title, dup);
                md_free_doc(&doc);
                goto out;
            }
        }
        id = store_next_action_id(&doc);
        if (!id) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        {
            char *hdr = xasprintf("%s: %s", id, title);
            c = hdr ? doc_add_chapter(&doc, hdr, 1) : NULL;
            free(hdr);
        }
        if (!c) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        if (desc && md_kv_set(c, "Description", desc) != 0) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        if (md_kv_set(c, "Status", "pending") != 0) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

    if (!strcmp(a[0], "list")) {
        const char *want = NULL;
        int i;
        if (n > 2) goto usage_out;
        if (n == 2) {
            if (!strcmp(a[1], "--pending")) want = "pending";
            else if (!strcmp(a[1], "--complete")) want = "complete";
            else goto usage_out;
        }
        if (load_rel(file, FV_ACTIONS, &doc)) goto out;
        for (i = 0; i < doc.nch; i++) {
            char *id, *title;
            const char *status = md_kv_get((const char **)doc.ch[i].keys,
                                           (const char **)doc.ch[i].vals,
                                           doc.ch[i].nkv, "Status");
            if (!action_split(doc.ch[i].title, &id, &title)) continue;
            if (want && status && strcmp(status, want)) {
                free(id);
                free(title);
                continue;
            }
            printf("%s: %s [%s]\n", id, title, status ? status : "?");
            free(id);
            free(title);
        }
        md_free_doc(&doc);
        rc = ferror(stdout) ? 1 : 0;
        goto out;
    }

    /* remaining verbs address one action id */
    if (!store_valid_id(a[0], 'A')) {
        fprintf(stderr, "flower: invalid action id '%s'\n", a[0]);
        goto out;
    }
    if (n < 2) goto usage_out;

    if (!strcmp(a[1], "set") && n == 4) {
        char *key = arg_val(a[2]);
        char *val = arg_val(a[3]);
        md_chapter_t *c;
        if (!key || !val) goto usage_out;
        if (!key_allowed(store_action_keys, key)) {
            fprintf(stderr, "flower: unknown key '%s'\n", key);
            goto out;
        }
        if (!strcmp(key, "Status") && !store_valid_status(val)) {
            fprintf(stderr,
                    "flower: invalid Status '%s' (pending, complete)\n",
                    val);
            goto out;
        }
        if (load_rel(file, FV_ACTIONS, &doc)) goto out;
        c = action_find(&doc, a[0]);
        if (!c) {
            fprintf(stderr, "flower: no such action %s\n", a[0]);
            md_free_doc(&doc);
            goto out;
        }
        if (!strcmp(key, "Depends")) {
            char **ids = NULL;
            int cnt, j;
            cnt = store_parse_id_list(val, &ids);
            if (cnt < 0) {
                md_free_doc(&doc);
                rc = oom();
                goto out;
            }
            for (j = 0; j < cnt; j++) {
                if (!strcmp(ids[j], a[0])) {
                    fprintf(stderr, "flower: action %s cannot depend on "
                                    "itself\n",
                            a[0]);
                    store_free_ids(ids, cnt);
                    md_free_doc(&doc);
                    goto out;
                }
                if (!action_find(&doc, ids[j])) {
                    fprintf(stderr, "flower: unknown action %s\n", ids[j]);
                    store_free_ids(ids, cnt);
                    md_free_doc(&doc);
                    goto out;
                }
            }
            store_free_ids(ids, cnt);
        }
        if (md_kv_set(c, key, val) != 0) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

    if (!strcmp(a[1], "remove") && n == 2) {
        md_chapter_t *c;
        char *dep;
        if (load_rel(file, FV_ACTIONS, &doc)) goto out;
        c = action_find(&doc, a[0]);
        if (!c) {
            fprintf(stderr, "flower: no such action %s\n", a[0]);
            md_free_doc(&doc);
            goto out;
        }
        dep = action_depended_on(&doc, a[0], c);
        if (dep) {
            char *id, *title;
            if (action_split(dep, &id, &title)) {
                fprintf(stderr, "flower: action %s depends on %s; remove "
                                "or edit dependents first\n",
                        id, a[0]);
                free(id);
                free(title);
            } else {
                fprintf(stderr, "flower: action %s depends on %s; remove "
                                "or edit dependents first\n",
                        dep, a[0]);
            }
            md_free_doc(&doc);
            goto out;
        }
        doc_remove_chapter(&doc, (int)(c - doc.ch));
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

usage_out:
    rc = usage(stderr);
out:
    free(file);
    return rc;
}

/* ---------- recursive delete ---------- */

/* rm -rf by opendir recursion, no system(). 0 ok, -1 io error. */
static int rm_rf(const char *path) {
    DIR *d;
    struct dirent *e;
    struct stat st;
    int rc = 0;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "flower: cannot stat %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0) {
            fprintf(stderr, "flower: cannot remove %s: %s\n", path,
                    strerror(errno));
            return -1;
        }
        return 0;
    }
    d = opendir(path);
    if (!d) {
        fprintf(stderr, "flower: cannot open %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    while (rc == 0 && (e = readdir(d)) != NULL) {
        char *child;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        child = xasprintf("%s/%s", path, e->d_name);
        if (!child) {
            rc = -1;
            continue;
        }
        rc = rm_rf(child);
        free(child);
    }
    closedir(d);
    if (rc == 0 && rmdir(path) != 0) {
        fprintf(stderr, "flower: cannot remove %s: %s\n", path,
                strerror(errno));
        rc = -1;
    }
    return rc;
}

/* prompt + rm_rf for a directory under the base */
static int cmd_remove_tree(const char *rel) {
    char *path = store_path(rel);
    struct stat st;
    char line[32];
    if (!path) return oom();
    if (stat(path, &st) != 0) {
        fprintf(stderr, "flower: no such %s\n", path);
        free(path);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "flower: not a directory: %s\n", path);
        free(path);
        return 1;
    }
    printf("remove %s? [y/N] ", path);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin) ||
        (line[0] != 'y' && line[0] != 'Y')) {
        printf("aborted\n");
        free(path);
        return 1;
    }
    if (rm_rf(path) != 0) {
        free(path);
        return 1;
    }
    print_abs(path);
    free(path);
    return 0;
}

/* ---------- edit escape hatch ---------- */

/* file kind by relative path under base, -1 when unrecognized */
static int kind_for_rel(const char *rel) {
    if (!strcmp(rel, "flower.md")) return FV_USER;
    if (str_starts(rel, "projects/")) {
        const char *p = rel + strlen("projects/");
        const char *slash = strchr(p, '/');
        const char *rest;
        char idbuf[32];
        size_t idlen;
        if (!slash) return -1;
        idlen = (size_t)(slash - p);
        if (idlen == 0 || idlen >= sizeof idbuf) return -1;
        memcpy(idbuf, p, idlen);
        idbuf[idlen] = '\0';
        if (!store_valid_id(idbuf, 'P')) return -1;
        rest = slash + 1;
        if (!strcmp(rest, "config.md")) return FV_PROJECT;
        if (!strcmp(rest, "details.md")) return FV_FIELDS;
        if (!strcmp(rest, "context.md")) return FV_CONTEXT;
        if (!strcmp(rest, "resources.md")) return FV_RESOURCE;
        if (str_starts(rest, "tasks/")) {
            const char *tp = rest + strlen("tasks/");
            const char *tslash = strchr(tp, '/');
            size_t tlen;
            if (!tslash) return -1;
            tlen = (size_t)(tslash - tp);
            if (tlen == 0 || tlen >= sizeof idbuf) return -1;
            memcpy(idbuf, tp, tlen);
            idbuf[tlen] = '\0';
            if (!store_valid_id(idbuf, 'T')) return -1;
            if (!strcmp(tslash + 1, "config.md")) return FV_TASK;
            if (!strcmp(tslash + 1, "context.md")) return FV_CONTEXT;
            if (!strcmp(tslash + 1, "actions.md")) return FV_ACTIONS;
        }
    }
    return -1;
}

static int cmd_edit(char *rel) {
    const char *base = store_base();
    const char *editor;
    char *path = NULL;
    char *tmp = NULL;
    char reason[512];
    char line[4096];
    size_t n;
    FILE *f;
    md_doc_t doc;
    int kind;
    int rc = 1;
    pid_t pid;
    int st;

    if (!base) {
        fprintf(stderr, "flower: FLOWER_BASE not resolvable\n");
        return 1;
    }
    if (rel[0] == '\0') {
        fprintf(stderr, "flower: empty path\n");
        return 1;
    }
    if (rel[0] == '/' || strstr(rel, "..")) {
        fprintf(stderr,
                "flower: path must stay inside the config base: %s\n",
                rel);
        return 1;
    }
    kind = kind_for_rel(rel);
    if (kind < 0) {
        fprintf(stderr, "flower: not a known config file: %s\n", rel);
        return 1;
    }
    path = store_path(rel);
    if (!path) return oom();

    /* copy the original (empty when missing) into the temp file */
    tmp = xasprintf("%s.tmp", path);
    if (!tmp) {
        rc = oom();
        goto out;
    }
    f = fopen(path, "rb");
    if (f) {
        size_t got;
        FILE *o = fopen(tmp, "wb");
        if (!o) {
            fprintf(stderr, "flower: cannot write %s: %s\n", tmp,
                    strerror(errno));
            fclose(f);
            goto out;
        }
        while ((got = fread(line, 1, sizeof line, f)) > 0)
            if (fwrite(line, 1, got, o) != got) {
                fprintf(stderr, "flower: write error on %s\n", tmp);
                fclose(f);
                fclose(o);
                goto out;
            }
        if (ferror(f)) {
            fprintf(stderr, "flower: read error on %s\n", path);
            fclose(f);
            fclose(o);
            goto out;
        }
        fclose(f);
        if (fclose(o) != 0) {
            fprintf(stderr, "flower: write error on %s\n", tmp);
            goto out;
        }
    } else {
        FILE *o = fopen(tmp, "wb");
        if (!o) {
            fprintf(stderr, "flower: cannot write %s: %s\n", tmp,
                    strerror(errno));
            goto out;
        }
        fclose(o);
    }

    editor = getenv("EDITOR");
    if (!editor || !editor[0]) editor = "vi";
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "flower: fork: %s\n", strerror(errno));
        goto out;
    }
    if (pid == 0) {
        execlp(editor, editor, tmp, (char *)NULL);
        fprintf(stderr, "flower: cannot run %s: %s\n", editor,
                strerror(errno));
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0) {
        fprintf(stderr, "flower: waitpid: %s\n", strerror(errno));
        goto out;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "flower: editor %s failed, %s unchanged\n",
                editor, path);
        goto out;
    }

    /* validate the edited temp file */
    memset(&doc, 0, sizeof doc);
    f = fopen(tmp, "rb");
    if (!f) {
        fprintf(stderr, "flower: cannot read %s: %s\n", tmp,
                strerror(errno));
        goto out;
    }
    {
        buf_t b;
        int prc;
        buf_init(&b);
        while ((n = fread(line, 1, sizeof line, f)) > 0)
            if (buf_append(&b, line, n)) {
                fclose(f);
                buf_free(&b);
                rc = oom();
                goto out;
            }
        if (ferror(f)) {
            fprintf(stderr, "flower: read error on %s\n", tmp);
            fclose(f);
            buf_free(&b);
            goto out;
        }
        fclose(f);
        prc = md_parse(b.data ? b.data : "", &doc);
        buf_free(&b);
        if (prc != 0) {
            snprintf(reason, sizeof reason, "%s",
                     prc == -2 ? "duplicate chapter title"
                               : prc == -3 ? "malformed structure"
                                           : "parse error");
            fprintf(stderr, "flower: invalid: %s (kept %s)\n", reason,
                    tmp);
            goto out;
        }
    }
    if (doc_check(kind, &doc, reason, sizeof reason) != 0) {
        fprintf(stderr, "flower: invalid: %s (kept %s)\n", reason, tmp);
        md_free_doc(&doc);
        goto out;
    }
    md_free_doc(&doc);

    /* atomic replace */
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "flower: cannot save %s: %s\n", path,
                strerror(errno));
        goto out;
    }
    free(tmp);
    tmp = NULL;
    print_abs(path);
    rc = 0;
out:
    free(tmp);
    free(path);
    return rc;
}

/* ---------- project dispatch ---------- */

static int cmd_project(int argc, char **argv) {
    const char *pid = argv[2];
    const char *area;
    if (argc < 4) return usage(stderr);
    if (!store_valid_id(pid, 'P')) {
        fprintf(stderr, "flower: invalid project id '%s'\n", pid);
        return 1;
    }
    area = argv[3];

    if (!strcmp(area, "set")) {
        char *dir, *file, *key, *val;
        int rc;
        if (argc != 6) return usage(stderr);
        key = arg_val(argv[4]);
        val = arg_val(argv[5]);
        if (!key || !val) return usage(stderr);
        dir = xasprintf("projects/%s", pid);
        file = xasprintf("projects/%s/config.md", pid);
        if (!dir || !file) {
            free(dir);
            free(file);
            return oom();
        }
        rc = cmd_config_set(FV_PROJECT, dir, file, key, val);
        free(dir);
        free(file);
        return rc;
    }

    if (!strcmp(area, "show")) {
        char *file;
        int rc;
        if (argc != 4) return usage(stderr);
        if (!require_project(pid)) return 1;
        file = xasprintf("projects/%s/config.md", pid);
        if (!file) return oom();
        rc = print_rel_file(file);
        free(file);
        return rc;
    }

    if (!strcmp(area, "details")) {
        if (argc < 5) return usage(stderr);
        if (!require_project(pid)) return 1;
        return cmd_details(pid, &argv[4], argc - 4);
    }

    if (!strcmp(area, "context")) {
        char *file;
        int rc;
        if (argc < 5) return usage(stderr);
        if (!require_project(pid)) return 1;
        file = xasprintf("projects/%s/context.md", pid);
        if (!file) return oom();
        rc = cmd_context(file, &argv[4], argc - 4);
        free(file);
        return rc;
    }

    if (!strcmp(area, "resource")) {
        if (argc < 5) return usage(stderr);
        if (!require_project(pid)) return 1;
        return cmd_resource(pid, &argv[4], argc - 4);
    }

    if (!strcmp(area, "task")) {
        const char *tid;
        if (argc < 6) return usage(stderr);
        tid = argv[4];
        if (!store_valid_id(tid, 'T')) {
            fprintf(stderr, "flower: invalid task id '%s'\n", tid);
            return 1;
        }
        if (!strcmp(argv[5], "set") && argc == 8) {
            char *dir, *file, *key, *val;
            int rc;
            key = arg_val(argv[6]);
            val = arg_val(argv[7]);
            if (!key || !val) return usage(stderr);
            dir = xasprintf("projects/%s/tasks/%s", pid, tid);
            file = xasprintf("projects/%s/tasks/%s/config.md", pid, tid);
            if (!dir || !file) {
                free(dir);
                free(file);
                return oom();
            }
            rc = cmd_config_set(FV_TASK, dir, file, key, val);
            free(dir);
            free(file);
            return rc;
        }
        if (!strcmp(argv[5], "show") && argc == 6) {
            char *file;
            int rc;
            if (!require_task(pid, tid)) return 1;
            file = xasprintf("projects/%s/tasks/%s/config.md", pid, tid);
            if (!file) return oom();
            rc = print_rel_file(file);
            free(file);
            return rc;
        }
        if (!strcmp(argv[5], "context")) {
            char *file;
            int rc;
            if (argc < 7) return usage(stderr);
            if (!require_task(pid, tid)) return 1;
            file = xasprintf("projects/%s/tasks/%s/context.md", pid, tid);
            if (!file) return oom();
            rc = cmd_context(file, &argv[6], argc - 6);
            free(file);
            return rc;
        }
        if (!strcmp(argv[5], "action")) {
            char *file;
            int rc;
            const char *verb = argv[6];
            if (argc < 7) return usage(stderr);
            /* add and list need an existing task dir: add creates it */
            if (!strcmp(verb, "add")) {
                char *dir = xasprintf("projects/%s/tasks/%s", pid, tid);
                if (!dir) return oom();
                if (store_mkdir_p(dir) != 0) {
                    free(dir);
                    return 1;
                }
                free(dir);
            } else if (!require_task(pid, tid)) {
                return 1;
            }
            file =
                xasprintf("projects/%s/tasks/%s/actions.md", pid, tid);
            if (!file) return oom();
            rc = cmd_action(pid, tid, &argv[6], argc - 6);
            free(file);
            return rc;
        }
        if (!strcmp(argv[5], "remove") && argc == 6) {
            char *rel;
            int rc;
            if (!require_task(pid, tid)) return 1;
            rel = xasprintf("projects/%s/tasks/%s", pid, tid);
            if (!rel) return oom();
            rc = cmd_remove_tree(rel);
            free(rel);
            return rc;
        }
        return usage(stderr);
    }

    if (!strcmp(area, "remove") && argc == 4) {
        char *rel;
        int rc;
        if (!require_project(pid)) return 1;
        rel = xasprintf("projects/%s", pid);
        if (!rel) return oom();
        rc = cmd_remove_tree(rel);
        free(rel);
        return rc;
    }

    return usage(stderr);
}

int flower_main(int argc, char **argv) {
    const char *cmd = argv[1];
    (void)argc;
    if (!strcmp(cmd, "setup")) {
        if (argc != 2) return usage(stderr);
        return cmd_setup();
    }
    if (!strcmp(cmd, "project")) return cmd_project(argc, argv);
    if (!strcmp(cmd, "edit")) {
        if (argc != 3) return usage(stderr);
        return cmd_edit(argv[2]);
    }
    fprintf(stderr, "flower: unknown command '%s'\n", cmd);
    return usage(stderr);
}
