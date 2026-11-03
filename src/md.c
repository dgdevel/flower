/* md.c - markdown config chapters: parse headers into a flat chapter
   list, keep key-value pairs or bare body lines, serialize back.
   format spec: docs/config_markdown_format.md */
#include "flower.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* strip trailing whitespace in place, returns s. body lines keep their
   leading indentation verbatim, so str_trim is not used there */
static char *rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

/* 1 when every byte of s is whitespace (blank lines are not stored) */
static int is_blank(const char *s) {
    for (; *s; s++)
        if (!isspace((unsigned char)*s)) return 0;
    return 1;
}

/* atx header: 1..6 '#' then space/tab or end of line. sets *level and
   *title (pointer past the hashes, untrimmed). */
static int header_scan(const char *s, int *level, const char **title) {
    int n = 0;
    while (s[n] == '#') n++;
    if (n < 1 || n > 6) return 0;
    if (s[n] && s[n] != ' ' && s[n] != '\t') return 0;
    *level = n;
    *title = s + n;
    return 1;
}

static int key_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-';
}

/* kv line at column 0: key [A-Za-z][A-Za-z0-9 _-]* then ": ". sets
   *klen to the key length and *val past the ": ". */
static int kv_scan(const char *s, size_t *klen, const char **val) {
    size_t i;
    if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return 0;
    for (i = 1; s[i]; i++) {
        char c = s[i];
        if (c == ':') {
            if (s[i + 1] != ' ') return 0; /* "Key:value" is a body line */
            *klen = i;
            *val = s + i + 2;
            return 1;
        }
        if (!key_char(c)) return 0;
    }
    return 0;
}

/* drop all body lines (bare text is discarded once a kv pair exists) */
static void body_clear(md_chapter_t *c) {
    int i;
    for (i = 0; i < c->nbody; i++) free(c->body[i].text);
    free(c->body);
    c->body = NULL;
    c->nbody = 0;
}

int md_parse(const char *content, md_doc_t *out) {
    md_doc_t doc;
    int stack[8]; /* open ancestors; levels strictly increasing */
    int nstack = 0;
    size_t len, pos = 0;
    int rc = 0;

    memset(&doc, 0, sizeof doc);
    if (!content) {
        *out = doc;
        return 0;
    }
    len = strlen(content);
    while (pos < len) {
        const char *ls = content + pos;
        const char *nl = memchr(ls, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - ls) : len - pos;
        char *line = malloc(ll + 1);
        int level;
        const char *rest;

        if (!line) {
            rc = -1;
            goto fail;
        }
        memcpy(line, ls, ll);
        line[ll] = '\0';
        pos = nl ? pos + ll + 1 : len;

        /* indented header is malformed, not a code block */
        {
            const char *lead = line;
            while (*lead == ' ' || *lead == '\t') lead++;
            if (lead != line && header_scan(lead, &level, &rest)) {
                free(line);
                rc = -3;
                goto fail;
            }
        }

        if (header_scan(line, &level, &rest)) {
            char *title;
            int j;

            /* jump deeper than parent+1 (e.g. h1 straight to h3, or the
               very first header deeper than h1) */
            if (level > (nstack ? doc.ch[stack[nstack - 1]].level : 0) + 1) {
                free(line);
                rc = -3;
                goto fail;
            }
            while (nstack && doc.ch[stack[nstack - 1]].level >= level)
                nstack--;

            title = str_dup(str_trim((char *)rest));
            if (!title) {
                free(line);
                rc = -1;
                goto fail;
            }
            /* duplicate chapter name at the same level */
            for (j = 0; j < doc.nch; j++) {
                if (doc.ch[j].level == level &&
                    !strcmp(doc.ch[j].title, title)) {
                    free(title);
                    free(line);
                    rc = -2;
                    goto fail;
                }
            }
            {
                md_chapter_t *g =
                    realloc(doc.ch, (size_t)(doc.nch + 1) * sizeof *g);
                if (!g) {
                    free(title);
                    free(line);
                    rc = -1;
                    goto fail;
                }
                doc.ch = g;
            }
            memset(&doc.ch[doc.nch], 0, sizeof doc.ch[doc.nch]);
            doc.ch[doc.nch].title = title;
            doc.ch[doc.nch].level = level;
            /* the level check above keeps nstack below the array size */
            if (nstack < (int)(sizeof stack / sizeof stack[0]))
                stack[nstack++] = doc.nch;
            doc.nch++;
            free(line);
            continue;
        }

        /* blank lines are not stored; preamble has no chapter to hold it */
        if (is_blank(line) || doc.nch == 0) {
            free(line);
            continue;
        }

        {
            md_chapter_t *c = &doc.ch[doc.nch - 1];
            size_t klen;
            const char *val;

            if (kv_scan(line, &klen, &val)) {
                char *k, *v;
                char **gk, **gv;
                line[klen] = '\0';
                k = str_dup(str_trim(line));
                v = str_dup(str_trim((char *)val));
                if (!k || !v) {
                    free(k);
                    free(v);
                    free(line);
                    rc = -1;
                    goto fail;
                }
                body_clear(c);
                gk = realloc(c->keys, (size_t)(c->nkv + 1) * sizeof *gk);
                if (gk) c->keys = gk;
                gv = realloc(c->vals, (size_t)(c->nkv + 1) * sizeof *gv);
                if (gv) c->vals = gv;
                if (!gk || !gv) {
                    free(k);
                    free(v);
                    free(line);
                    rc = -1;
                    goto fail;
                }
                c->keys[c->nkv] = k;
                c->vals[c->nkv] = v;
                c->nkv++;
            } else if (c->nkv == 0) {
                /* bare line, verbatim minus trailing whitespace */
                char *text = str_dup(rtrim(line));
                md_line_t *g =
                    realloc(c->body, (size_t)(c->nbody + 1) * sizeof *g);
                if (!text || !g) {
                    free(text);
                    free(line);
                    rc = -1;
                    goto fail;
                }
                c->body = g;
                c->body[c->nbody].text = text;
                c->nbody++;
            }
            /* else: bare line in a kv chapter, discarded */
            free(line);
        }
    }
    *out = doc;
    return 0;

fail:
    md_free_doc(&doc);
    memset(out, 0, sizeof *out);
    return rc;
}

md_chapter_t *md_find(md_doc_t *doc, const char *title) {
    int i;
    for (i = 0; i < doc->nch; i++)
        if (!strcmp(doc->ch[i].title, title)) return &doc->ch[i];
    return NULL;
}

int md_kv_set(md_chapter_t *c, const char *key, const char *val) {
    int i;
    for (i = 0; i < c->nkv; i++) {
        if (!strcmp(c->keys[i], key)) {
            char *v = str_dup(val);
            if (!v) return -1;
            free(c->vals[i]);
            c->vals[i] = v;
            return 0;
        }
    }
    {
        char *k = str_dup(key);
        char *v = str_dup(val);
        char **gk = realloc(c->keys, (size_t)(c->nkv + 1) * sizeof *gk);
        char **gv = realloc(c->vals, (size_t)(c->nkv + 1) * sizeof *gv);
        if (!k || !v || !gk || !gv) {
            /* keep any grown array, nkv is unchanged so it stays valid */
            free(k);
            free(v);
            if (gk) c->keys = gk;
            if (gv) c->vals = gv;
            return -1;
        }
        c->keys = gk;
        c->vals = gv;
        /* first kv pair turns the chapter from bare body to kv */
        if (c->nkv == 0) body_clear(c);
        c->keys[c->nkv] = k;
        c->vals[c->nkv] = v;
        c->nkv++;
    }
    return 0;
}

const char *md_kv_get(const char **keys, const char **vals, int nkv,
                      const char *key) {
    int i;
    for (i = 0; i < nkv; i++)
        if (!strcmp(keys[i], key)) return vals[i];
    return NULL;
}

void md_free_doc(md_doc_t *doc) {
    int i, j;
    if (!doc) return;
    for (i = 0; i < doc->nch; i++) {
        md_chapter_t *c = &doc->ch[i];
        free(c->title);
        for (j = 0; j < c->nkv; j++) {
            free(c->keys[j]);
            free(c->vals[j]);
        }
        free(c->keys);
        free(c->vals);
        body_clear(c);
    }
    free(doc->ch);
    memset(doc, 0, sizeof *doc);
}

int md_serialize(const md_doc_t *doc, buf_t *out) {
    int i, j;
    buf_reset(out);
    for (i = 0; i < doc->nch; i++) {
        const md_chapter_t *c = &doc->ch[i];
        if (i > 0 && buf_append(out, "\n", 1)) return -1;
        if (buf_appendf(out, "%.*s %s\n", c->level, "######", c->title))
            return -1;
        for (j = 0; j < c->nkv; j++)
            if (buf_appendf(out, "%s: %s\n", c->keys[j], c->vals[j]))
                return -1;
        if (c->nkv == 0)
            for (j = 0; j < c->nbody; j++)
                if (buf_appendf(out, "%s\n", c->body[j].text)) return -1;
    }
    return 0;
}

int md_load(const char *path, md_doc_t *doc) {
    FILE *f;
    buf_t b;
    char chunk[4096];
    size_t n;
    int rc;

    memset(doc, 0, sizeof *doc);
    f = fopen(path, "rb");
    if (!f) return 1; /* caller treats as a fresh file */
    buf_init(&b);
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (buf_append(&b, chunk, n)) {
            fclose(f);
            buf_free(&b);
            return -1;
        }
    }
    if (ferror(f)) {
        fclose(f);
        buf_free(&b);
        return -1;
    }
    fclose(f);
    rc = md_parse(b.data ? b.data : "", doc);
    buf_free(&b);
    return rc;
}

int md_save(const char *path, const md_doc_t *doc) {
    buf_t b;
    char *tmp;
    FILE *f;

    buf_init(&b);
    if (md_serialize(doc, &b)) {
        fprintf(stderr, "flower: out of memory writing %s\n", path);
        buf_free(&b);
        return -1;
    }
    tmp = malloc(strlen(path) + 5); /* room for ".tmp" */
    if (!tmp) {
        fprintf(stderr, "flower: out of memory writing %s\n", path);
        buf_free(&b);
        return -1;
    }
    sprintf(tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "flower: cannot write %s: %s\n", tmp,
                strerror(errno));
        free(tmp);
        buf_free(&b);
        return -1;
    }
    if ((b.len && fwrite(b.data, 1, b.len, f) != b.len) || fclose(f) != 0) {
        fprintf(stderr, "flower: cannot write %s: %s\n", tmp,
                strerror(errno));
        remove(tmp);
        free(tmp);
        buf_free(&b);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "flower: cannot write %s: %s\n", path,
                strerror(errno));
        remove(tmp);
        free(tmp);
        buf_free(&b);
        return -1;
    }
    free(tmp);
    buf_free(&b);
    return 0;
}
