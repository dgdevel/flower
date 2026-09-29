/*
 * html — tolerant tokenizer + DOM + readability-lite + markdown.
 * See html.h for the contract and the deliberate limitations.
 */
#define _POSIX_C_SOURCE 200809L

#include "html.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------- caps ---------- */

#define NODE_MAX  120000  /* nodes kept; further markup is dropped */
#define DEPTH_MAX 400

/* ---------- growable buffer ---------- */

typedef struct {
    char *data;
    size_t len, cap;
} sbuf_t;

static int sb_putn(sbuf_t *b, const char *s, size_t n)
{
    if (!n) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) return -1;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int sb_puts(sbuf_t *b, const char *s)
{
    return sb_putn(b, s ? s : "", s ? strlen(s) : 0);
}

static int sb_putc(sbuf_t *b, char c)
{
    return sb_putn(b, &c, 1);
}

static const char *find_sub(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    if (n < nl) return NULL;
    for (size_t i = 0; i + nl <= n; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nl) == 0)
            return hay + i;
    return NULL;
}

/* ---------- entities ---------- */

static const struct { const char *name; const char *out; } ENTITIES[] = {
    { "amp", "&" },  { "lt", "<" },   { "gt", ">" },  { "quot", "\"" },
    { "apos", "'" }, { "nbsp", " " }, { "copy", "\xc2\xa9" },
    { "reg", "\xc2\xae" }, { "trade", "\xe2\x84\xa2" },
    { "hellip", "\xe2\x80\xa6" }, { "mdash", "\xe2\x80\x94" },
    { "ndash", "\xe2\x80\x93" }, { "lsquo", "\xe2\x80\x98" },
    { "rsquo", "\xe2\x80\x99" }, { "ldquo", "\xe2\x80\x9c" },
    { "rdquo", "\xe2\x80\x9d" }, { "laquo", "\xc2\xab" },
    { "raquo", "\xc2\xbb" }, { "bull", "\xe2\x80\xa2" },
    { "middot", "\xc2\xb7" }, { "deg", "\xc2\xb0" },
    { "sect", "\xc2\xa7" }, { "para", "\xc2\xb6" },
    { "plusmn", "\xc2\xb1" }, { "times", "\xc3\x97" },
    { "divide", "\xc3\xb7" }, { "minus", "\xe2\x88\x92" },
    { "euro", "\xe2\x82\xac" }, { "pound", "\xc2\xa3" },
    { "yen", "\xc2\xa5" }, { "cent", "\xc2\xa2" },
    { "larr", "\xe2\x86\x90" }, { "rarr", "\xe2\x86\x92" },
    { "uarr", "\xe2\x86\x91" }, { "darr", "\xe2\x86\x93" },
};

static int sb_put_utf8(sbuf_t *b, unsigned cp)
{
    if (cp < 0x80) return sb_putc(b, (char)cp);
    if (cp > 0x10ffff) cp = 0xfffd;
    char t[4];
    if (cp < 0x800) {
        t[0] = (char)(0xc0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3f));
        return sb_putn(b, t, 2);
    }
    if (cp < 0x10000) {
        t[0] = (char)(0xe0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        t[2] = (char)(0x80 | (cp & 0x3f));
        return sb_putn(b, t, 3);
    }
    t[0] = (char)(0xf0 | (cp >> 18));
    t[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    t[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    t[3] = (char)(0x80 | (cp & 0x3f));
    return sb_putn(b, t, 4);
}

/* Decode entities in s[0..n) into a malloc'd string. Unknown entities
 * stay literal; a lone "&" survives. */
static char *decode_entities(const char *s, size_t n)
{
    sbuf_t b = { 0 };
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') {
            if (sb_putn(&b, s + i, 1) != 0) { free(b.data); return NULL; }
            continue;
        }
        size_t j = i + 1, max = i + 12;
        for (; j < n && j < max && s[j] != ';'; j++) {}
        if (j >= n || s[j] != ';') {
            if (sb_putn(&b, s + i, 1) != 0) { free(b.data); return NULL; }
            continue;
        }
        size_t elen = j - i - 1;
        if (elen == 0) {
            if (sb_putn(&b, s + i, 1) != 0) { free(b.data); return NULL; }
            continue;
        }
        if (s[i + 1] == '#') {
            unsigned cp = 0;
            int ok = 0;
            if (elen >= 2 && (s[i + 2] == 'x' || s[i + 2] == 'X')) {
                for (size_t k = i + 3; k < j; k++) {
                    int d;
                    if (isdigit((unsigned char)s[k]))
                        d = s[k] - '0';
                    else if (isxdigit((unsigned char)s[k]))
                        d = tolower((unsigned char)s[k]) - 'a' + 10;
                    else { d = -1; }
                    if (d < 0) { ok = 0; break; }
                    cp = cp * 16 + (unsigned)d;
                    ok = 1;
                }
            } else {
                for (size_t k = i + 2; k < j; k++) {
                    if (!isdigit((unsigned char)s[k])) { ok = 0; break; }
                    cp = cp * 10 + (unsigned)(s[k] - '0');
                    ok = 1;
                }
            }
            if (ok) {
                if (sb_put_utf8(&b, cp) != 0) { free(b.data); return NULL; }
                i = j;
                continue;
            }
            if (sb_putn(&b, s + i, 1) != 0) { free(b.data); return NULL; }
            continue;
        }
        const char *lit = NULL;
        for (size_t k = 0; k < sizeof ENTITIES / sizeof ENTITIES[0]; k++) {
            if (strlen(ENTITIES[k].name) == elen &&
                strncmp(ENTITIES[k].name, s + i + 1, elen) == 0) {
                lit = ENTITIES[k].out;
                break;
            }
        }
        if (lit) {
            if (sb_puts(&b, lit) != 0) { free(b.data); return NULL; }
            i = j;
        } else {
            if (sb_putn(&b, s + i, 1) != 0) { free(b.data); return NULL; }
        }
    }
    if (!b.data) {
        b.data = malloc(1);
        if (b.data) b.data[0] = '\0';
    }
    return b.data;
}

/* ---------- parser ---------- */

static int lc(int c)
{
    return tolower((unsigned char)c);
}

static void node_append(html_node_t *parent, html_node_t *n)
{
    n->parent = parent;
    if (parent->last_child)
        parent->last_child->next = n;
    else
        parent->child = n;
    parent->last_child = n;
}

static html_node_t *node_new(html_doc_t *doc)
{
    if (doc->nodes >= NODE_MAX) return NULL;
    html_node_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    doc->nodes++;
    return n;
}

static int is_void_tag(const char *tag)
{
    static const char *voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL,
    };
    for (int i = 0; voids[i]; i++)
        if (strcmp(tag, voids[i]) == 0) return 1;
    return 0;
}

static int is_raw_tag(const char *tag)
{
    return strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0;
}

static void tag_copy(char *dst, size_t dstn, const char *s, size_t n)
{
    if (n >= dstn) n = dstn - 1;
    for (size_t i = 0; i < n; i++) dst[i] = (char)lc((unsigned char)s[i]);
    dst[n] = '\0';
}

static char *attr_dup(const char *s, size_t n)
{
    if (!s) return NULL;
    if (n > 2047) n = 2047;
    char tmp[2048];
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    return decode_entities(tmp, n);
}

typedef struct {
    html_doc_t *doc;
    html_node_t *stack[DEPTH_MAX];
    size_t depth;
} parser_t;

static void add_text(parser_t *p, const char *s, size_t n)
{
    html_node_t *t = node_new(p->doc);
    if (!t) return;
    t->text = decode_entities(s, n);
    if (!t->text) {
        t->text = calloc(1, 1);
        if (!t->text) { free(t); return; }
    }
    node_append(p->stack[p->depth], t);
}

html_doc_t *html_parse(const char *buf, size_t len)
{
    html_doc_t *doc = calloc(1, sizeof *doc);
    if (!doc) return NULL;
    html_node_t *root = node_new(doc);
    if (!root) { free(doc); return NULL; }
    strcpy(root->tag, "#root");
    doc->root = root;

    parser_t p;
    memset(&p, 0, sizeof p);
    p.doc = doc;
    p.stack[0] = root;

    size_t i = 0;
    while (i < len) {
        const char *lt = memchr(buf + i, '<', len - i);
        if (!lt) {
            add_text(&p, buf + i, len - i);
            break;
        }
        size_t off = (size_t)(lt - buf);
        if (off > i) add_text(&p, buf + i, off - i);
        i = off;

        /* comments */
        if (len - i >= 4 && memcmp(buf + i, "<!--", 4) == 0) {
            const char *end = find_sub(buf + i + 4, len - i - 4, "-->");
            i = end ? (size_t)(end - buf) + 3 : len;
            continue;
        }
        /* doctype / markup declarations / processing instructions */
        if (len - i >= 2 && (buf[i + 1] == '!' || buf[i + 1] == '?')) {
            const char *end = memchr(buf + i + 2, '>', len - i - 2);
            i = end ? (size_t)(end - buf) + 1 : len;
            continue;
        }
        /* closing tag */
        if (len - i >= 3 && buf[i + 1] == '/' &&
            isalpha((unsigned char)buf[i + 2])) {
            size_t j = i + 2;
            while (j < len && (isalnum((unsigned char)buf[j]) || buf[j] == '-'))
                j++;
            char tag[12];
            tag_copy(tag, sizeof tag, buf + i + 2, j - (i + 2));
            for (size_t d = p.depth; d > 0; d--) {
                if (strcmp(p.stack[d]->tag, tag) == 0) {
                    p.depth = d - 1;
                    break;
                }
            }
            const char *end = memchr(buf + j, '>', len - j);
            i = end ? (size_t)(end - buf) + 1 : len;
            continue;
        }
        /* opening tag */
        if (len - i >= 2 && isalpha((unsigned char)buf[i + 1])) {
            if (doc->nodes >= NODE_MAX) { /* cap reached: skip markup */
                const char *end = memchr(buf + i, '>', len - i);
                i = end ? (size_t)(end - buf) + 1 : len;
                continue;
            }
            size_t j = i + 1;
            while (j < len && (isalnum((unsigned char)buf[j]) || buf[j] == '-'))
                j++;
            html_node_t *n = node_new(doc);
            if (!n) break;
            tag_copy(n->tag, sizeof n->tag, buf + i + 1, j - (i + 1));

            int self_closing = 0;
            while (j < len) {
                if (buf[j] == '>') break;
                if (buf[j] == '/') {
                    if (j + 1 < len && buf[j + 1] == '>') { self_closing = 1; break; }
                    j++;
                    continue;
                }
                if (isspace((unsigned char)buf[j])) { j++; continue; }
                size_t an = j;
                while (j < len && !isspace((unsigned char)buf[j]) &&
                       buf[j] != '=' && buf[j] != '>' && buf[j] != '/')
                    j++;
                if (j == an) { j++; continue; } /* stray char */
                char aname[16];
                tag_copy(aname, sizeof aname, buf + an, j - an);
                const char *aval = NULL;
                size_t alen = 0;
                size_t k = j;
                while (k < len && isspace((unsigned char)buf[k])) k++;
                if (k < len && buf[k] == '=') {
                    k++;
                    while (k < len && isspace((unsigned char)buf[k])) k++;
                    if (k < len && (buf[k] == '"' || buf[k] == '\'')) {
                        char q = buf[k++];
                        const char *ve = memchr(buf + k, q, len - k);
                        aval = buf + k;
                        alen = ve ? (size_t)(ve - (buf + k)) : len - k;
                        j = ve ? (size_t)(ve - buf) + 1 : len;
                    } else {
                        size_t e = k;
                        while (e < len && !isspace((unsigned char)buf[e]) &&
                               buf[e] != '>')
                            e++;
                        aval = buf + k;
                        alen = e - k;
                        j = e;
                    }
                }
                if (strcmp(aname, "class") == 0 && !n->class_)
                    n->class_ = attr_dup(aval, alen);
                else if (strcmp(aname, "id") == 0 && !n->id)
                    n->id = attr_dup(aval, alen);
                else if (strcmp(aname, "href") == 0 && !n->href)
                    n->href = attr_dup(aval, alen);
            }
            if (self_closing) /* j sits on '/', j+1 on '>' */
                i = j + 2 <= len ? j + 2 : len;
            else
                i = (j < len && buf[j] == '>') ? j + 1 : len;

            int push = !self_closing && !is_void_tag(n->tag);
            node_append(p.stack[p.depth], n);

            if (is_raw_tag(n->tag)) {
                /* content is not markup: skip to the matching close */
                char close[16];
                snprintf(close, sizeof close, "</%s", n->tag);
                const char *rest = buf + i;
                size_t rn = len - i;
                const char *found = NULL;
                for (size_t k = 0; k + 3 <= rn; k++) {
                    if (rest[k] == '<' &&
                        strncasecmp(rest + k, close, strlen(close)) == 0) {
                        found = rest + k;
                        break;
                    }
                }
                if (found) {
                    const char *end = memchr(found, '>', len - (size_t)(found - buf));
                    i = end ? (size_t)(end - buf) + 1 : len;
                } else {
                    i = len;
                }
                push = 0;
            }
            if (push && p.depth + 1 < DEPTH_MAX)
                p.stack[++p.depth] = n;
            continue;
        }
        /* a lone '<' that starts nothing */
        add_text(&p, "<", 1);
        i++;
    }
    return doc;
}

static void html_node_free(html_node_t *n)
{
    while (n) {
        html_node_t *next = n->next;
        if (n->child) html_node_free(n->child);
        free(n->class_);
        free(n->id);
        free(n->href);
        free(n->text);
        free(n);
        n = next;
    }
}

void html_doc_free(html_doc_t *doc)
{
    if (!doc) return;
    html_node_free(doc->root);
    free(doc);
}

/* ---------- queries ---------- */

int html_walk(html_node_t *n, int (*cb)(html_node_t *, void *), void *ctx)
{
    for (html_node_t *c = n; c; c = c->next) {
        int r = cb(c, ctx);
        if (r) return r;
        if (c->child) {
            r = html_walk(c->child, cb, ctx);
            if (r) return r;
        }
    }
    return 0;
}

int html_is(const html_node_t *n, const char *tag)
{
    return n && n->tag[0] && strcmp(n->tag, tag) == 0;
}

int html_has_class(const html_node_t *n, const char *cls)
{
    if (!n || !n->class_) return 0;
    size_t cl = strlen(cls);
    const char *c = n->class_;
    while (*c) {
        while (*c == ' ' || *c == '\t') c++;
        if (!*c) break;
        const char *e = c;
        while (*e && *e != ' ' && *e != '\t') e++;
        if ((size_t)(e - c) == cl && strncmp(c, cls, cl) == 0) return 1;
        c = e;
    }
    return 0;
}

/* ---------- text extraction ---------- */

/* the subtree of one node only — never the node's siblings */
static void text_subtree(const html_node_t *n, sbuf_t *b)
{
    for (const html_node_t *c = n->child; c; c = c->next) {
        if (c->text) {
            for (const char *p = c->text; *p; p++) {
                char ch = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
                if (ch == ' ' && b->len && b->data[b->len - 1] == ' ')
                    continue;
                sb_putc(b, ch);
            }
        }
        if (c->child) text_subtree(c, b);
    }
}

char *html_text(const html_node_t *n)
{
    if (!n) return NULL;
    sbuf_t b = { 0 };
    if (n->text) {
        for (const char *p = n->text; *p; p++) {
            char ch = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
            if (ch == ' ' && b.len && b.data[b.len - 1] == ' ')
                continue;
            sb_putc(&b, ch);
        }
    }
    text_subtree(n, &b);
    if (!b.data) {
        b.data = malloc(1);
        if (!b.data) return NULL;
        b.data[0] = '\0';
    }
    char *out = b.data;
    /* trim both ends */
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
    char *start = out;
    while (*start == ' ') start++;
    if (start != out) memmove(out, start, strlen(start) + 1);
    return out;
}

static size_t text_len(const html_node_t *n)
{
    char *t = html_text(n);
    if (!t) return 0;
    size_t l = strlen(t);
    free(t);
    return l;
}

/* ---------- readability-lite ---------- */

static int is_junk_tag(const char *tag)
{
    static const char *junk[] = {
        "script", "style", "noscript", "template", "svg", "iframe",
        "form", "nav", "aside", "button", "select", "label", "input",
        "option", "datalist", NULL,
    };
    for (int i = 0; junk[i]; i++)
        if (strcmp(tag, junk[i]) == 0) return 1;
    return 0;
}

static int boilerplate_hint(const html_node_t *n)
{
    static const char *hints[] = {
        "nav", "menu", "sidebar", "comment", "promo", "sponsor",
        "advert", "widget", "share", "related", "footer", "banner",
        "social", "popup", "modal", "cookie", "newsletter", NULL,
    };
    for (int pass = 0; pass < 2; pass++) {
        const char *s = pass == 0 ? n->class_ : n->id;
        if (!s) continue;
        for (int i = 0; hints[i]; i++)
            if (strstr(s, hints[i])) return 1;
    }
    return 0;
}

static int is_container(const html_node_t *n)
{
    return strcmp(n->tag, "div") == 0 || strcmp(n->tag, "section") == 0 ||
           strcmp(n->tag, "article") == 0 || strcmp(n->tag, "main") == 0 ||
           strcmp(n->tag, "td") == 0 || strcmp(n->tag, "th") == 0 ||
           strcmp(n->tag, "body") == 0 || strcmp(n->tag, "#root") == 0;
}

static int is_paragraphish(const html_node_t *n)
{
    return strcmp(n->tag, "p") == 0 || strcmp(n->tag, "li") == 0 ||
           strcmp(n->tag, "blockquote") == 0 ||
           strcmp(n->tag, "pre") == 0 || strcmp(n->tag, "h2") == 0 ||
           strcmp(n->tag, "h3") == 0;
}

typedef struct {
    html_node_t *node;
    double score;
} scored_t;

typedef struct {
    scored_t *v;
    size_t n, cap;
} scorevec_t;

static scored_t *scorevec_find(scorevec_t *sv, html_node_t *n)
{
    for (size_t i = 0; i < sv->n; i++)
        if (sv->v[i].node == n) return &sv->v[i];
    return NULL;
}

static int scorevec_add(scorevec_t *sv, html_node_t *n, double s)
{
    scored_t *e = scorevec_find(sv, n);
    if (e) {
        e->score += s;
        return 0;
    }
    if (sv->n == sv->cap) {
        size_t cap = sv->cap ? sv->cap * 2 : 64;
        scored_t *v = realloc(sv->v, cap * sizeof *v);
        if (!v) return -1;
        sv->v = v;
        sv->cap = cap;
    }
    sv->v[sv->n].node = n;
    sv->v[sv->n].score = s;
    sv->n++;
    return 0;
}

/* collect containers (pre-registration) */
static int collect_cb(html_node_t *n, void *ctx)
{
    scorevec_t *sv = ctx;
    if (!n->tag[0] || is_junk_tag(n->tag)) return 0;
    if (is_container(n))
        if (scorevec_add(sv, n, 0.0) != 0) return 1;
    return 0;
}

static double score_text(const char *t)
{
    size_t len = strlen(t);
    if (len < 25) return 0;
    size_t commas = 0;
    for (const char *p = t; *p; p++)
        if (*p == ',') commas++;
    size_t hundreds = len / 100;
    if (hundreds > 3) hundreds = 3;
    if (commas > 3) commas = 3;
    return 1.0 + (double)hundreds + (double)commas;
}

/* credit the candidate's nearest two container ancestors, like
 * classic readability (parent full, grandparent half) */
static void credit(scorevec_t *sv, html_node_t *cand, double s)
{
    int level = 0;
    for (html_node_t *a = cand->parent; a && level < 2; a = a->parent) {
        scored_t *e = scorevec_find(sv, a);
        if (e) {
            e->score += level == 0 ? s : s / 2.0;
            level++;
        }
    }
}

static void score_tree(html_node_t *n, scorevec_t *sv)
{
    for (html_node_t *c = n->child; c; c = c->next) {
        if (!c->tag[0] || is_junk_tag(c->tag)) continue;
        if (is_paragraphish(c) && !boilerplate_hint(c)) {
            char *t = html_text(c);
            if (t) {
                double s = score_text(t);
                if (s > 0) credit(sv, c, s);
                free(t);
            }
        }
        score_tree(c, sv);
    }
}

static html_node_t *readability_pick(html_doc_t *doc)
{
    scorevec_t sv = { 0 };
    html_walk(doc->root, collect_cb, &sv);
    if (!sv.v) return doc->root;

    /* fast path: a substantial <article> or <main> */
    for (size_t i = 0; i < sv.n; i++) {
        html_node_t *n = sv.v[i].node;
        if ((strcmp(n->tag, "article") == 0 || strcmp(n->tag, "main") == 0) &&
            !boilerplate_hint(n) && text_len(n) >= 250) {
            free(sv.v);
            return n;
        }
    }

    score_tree(doc->root, &sv);

    html_node_t *best = NULL;
    double best_score = 0;
    for (size_t i = 0; i < sv.n; i++) {
        double s = sv.v[i].score;
        if (boilerplate_hint(sv.v[i].node)) s *= 0.2;
        if (s > best_score) {
            best_score = s;
            best = sv.v[i].node;
        }
    }
    free(sv.v);
    /* nothing scored meaningfully: keep the whole document */
    if (best_score < 4.0 || strcmp(best->tag, "#root") == 0)
        return doc->root;
    return best;
}

/* ---------- markdown emitter ---------- */

#define MD_MAX (256 * 1024)

typedef struct {
    sbuf_t b;
    int pending_sep; /* one blank line owed before the next block */
} md_t;

static void md_sep(md_t *m)
{
    if (m->pending_sep && m->b.len < MD_MAX) {
        sb_puts(&m->b, "\n\n");
        m->pending_sep = 0;
    }
}

static void md_one(html_node_t *c, md_t *m);
static void md_block(html_node_t *n, md_t *m);

/* one text node's content, whitespace collapsed (inline rules) */
static void emit_inline_text(const char *s, md_t *m)
{
    for (const char *p = s; *p; p++) {
        char ch = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
        if (ch == ' ' && m->b.len && m->b.data[m->b.len - 1] == ' ')
            continue;
        sb_putc(&m->b, ch);
    }
}

/* collapsed inline text of the subtree; markdown spans applied */
static void md_inline(html_node_t *n, md_t *m)
{
    for (html_node_t *c = n; c; c = c->next) {
        if (m->b.len >= MD_MAX) return;
        if (c->text) {
            emit_inline_text(c->text, m);
        } else if (c->tag[0]) {
            if (is_junk_tag(c->tag)) continue;
            if (strcmp(c->tag, "br") == 0) {
                sb_putc(&m->b, '\n');
            } else if (strcmp(c->tag, "a") == 0) {
                sb_putc(&m->b, '[');
                md_inline(c->child, m);
                sb_puts(&m->b, "](");
                sb_puts(&m->b, c->href ? c->href : "");
                sb_putc(&m->b, ')');
            } else if (strcmp(c->tag, "strong") == 0 ||
                       strcmp(c->tag, "b") == 0) {
                sb_puts(&m->b, "**");
                md_inline(c->child, m);
                sb_puts(&m->b, "**");
            } else if (strcmp(c->tag, "em") == 0 ||
                       strcmp(c->tag, "i") == 0) {
                sb_putc(&m->b, '*');
                md_inline(c->child, m);
                sb_putc(&m->b, '*');
            } else if (strcmp(c->tag, "code") == 0) {
                sb_putc(&m->b, '`');
                md_inline(c->child, m);
                sb_putc(&m->b, '`');
            } else {
                md_inline(c->child, m); /* transparent: span, u, small… */
            }
        }
    }
}

/* does the subtree hold any inline text at all? */
static int has_text(const html_node_t *n)
{
    for (const html_node_t *c = n; c; c = c->next) {
        if (c->text && c->text[0]) return 1;
        if (c->child && has_text(c->child)) return 1;
    }
    return 0;
}

static void md_list(html_node_t *list, md_t *m, int indent, int ordered)
{
    int idx = 1;
    for (html_node_t *li = list->child; li; li = li->next) {
        if (m->b.len >= MD_MAX) return;
        if (!html_is(li, "li")) {
            if (html_is(li, "ul") || html_is(li, "ol"))
                md_list(li, m, indent + 2, html_is(li, "ol"));
            continue;
        }
        md_sep(m);
        for (int k = 0; k < indent; k++) sb_putc(&m->b, ' ');
        if (ordered) {
            char num[16];
            snprintf(num, sizeof num, "%d. ", idx++);
            sb_puts(&m->b, num);
        } else {
            sb_puts(&m->b, "- ");
        }
        /* inline content minus nested lists */
        for (html_node_t *c = li->child; c; c = c->next) {
            if (html_is(c, "ul") || html_is(c, "ol")) continue;
            if (c->text && !c->text[0]) continue;
            if (html_is(c, "p")) {
                md_inline(c->child, m);
            } else {
                md_inline(c, m);
            }
        }
        sb_putc(&m->b, '\n');
        m->pending_sep = 0;
        /* nested lists under this item */
        for (html_node_t *c = li->child; c; c = c->next)
            if (html_is(c, "ul") || html_is(c, "ol"))
                md_list(c, m, indent + 2, html_is(c, "ol"));
    }
    m->pending_sep = 1;
}

/* raw text of one <pre> subtree, whitespace preserved — the node's
 * subtree only, never its siblings */
static void pre_collect(const html_node_t *n, md_t *m)
{
    if (n->text) sb_putn(&m->b, n->text, strlen(n->text));
    if (html_is(n, "br")) sb_putc(&m->b, '\n');
    for (const html_node_t *c = n->child; c; c = c->next)
        pre_collect(c, m);
}

static void md_blockquote(html_node_t *bq, md_t *m)
{
    md_t sub;
    memset(&sub, 0, sizeof sub);
    sub.pending_sep = 0;
    for (html_node_t *c = bq->child; c; c = c->next)
        md_one(c, &sub);
    if (!sub.b.data) return;
    md_sep(m);
    for (char *line = strtok(sub.b.data, "\n"); line;
         line = strtok(NULL, "\n")) {
        if (!*line) { sb_puts(&m->b, ">\n"); continue; }
        sb_puts(&m->b, "> ");
        sb_puts(&m->b, line);
        sb_putc(&m->b, '\n');
    }
    m->pending_sep = 1;
    free(sub.b.data);
}

/* one table section: <tr> children, or thead/tbody/tfoot wrappers */
static void md_rows(html_node_t *first, md_t *m)
{
    for (html_node_t *tr = first; tr; tr = tr->next) {
        if (html_is(tr, "thead") || html_is(tr, "tbody") ||
            html_is(tr, "tfoot")) {
            md_rows(tr->child, m);
            continue;
        }
        if (!html_is(tr, "tr")) continue;
        sb_putc(&m->b, '|');
        for (html_node_t *cell = tr->child; cell; cell = cell->next) {
            if (!html_is(cell, "td") && !html_is(cell, "th")) continue;
            sb_putc(&m->b, ' ');
            md_inline(cell->child, m);
            sb_puts(&m->b, " |");
        }
        sb_putc(&m->b, '\n');
    }
}

static void md_table(html_node_t *table, md_t *m)
{
    md_sep(m);
    md_rows(table->child, m);
    m->pending_sep = 1;
}

/* one element as a block; md_block runs it over a child list */
static void md_one(html_node_t *c, md_t *m)
{
    if (m->b.len >= MD_MAX) {
        if (m->pending_sep != 2) { /* truncation notice once */
            sb_puts(&m->b, "\n\n[truncated]\n");
            m->pending_sep = 2;
        }
        return;
    }
    if (!c->tag[0]) { /* stray text outside a block: this node only,
                      * never its siblings (md_inline is list-shaped) */
        if (!c->text || !c->text[0]) return;
        int blank = 1;
        for (const char *p = c->text; *p; p++)
            if (!isspace((unsigned char)*p)) { blank = 0; break; }
        if (blank) return; /* inter-block whitespace */
        md_sep(m);
        emit_inline_text(c->text, m);
        sb_putc(&m->b, '\n');
        m->pending_sep = 1;
        return;
    }
    const char *t = c->tag;
    if (is_junk_tag(t) || strcmp(t, "head") == 0 ||
        strcmp(t, "title") == 0 || strcmp(t, "img") == 0)
        return;

    if (strcmp(t, "h1") == 0 || strcmp(t, "h2") == 0 ||
        strcmp(t, "h3") == 0 || strcmp(t, "h4") == 0 ||
        strcmp(t, "h5") == 0 || strcmp(t, "h6") == 0) {
        if (!has_text(c->child)) return;
        md_sep(m);
        int level = t[1] - '0';
        for (int k = 0; k < level; k++) sb_putc(&m->b, '#');
        sb_putc(&m->b, ' ');
        md_inline(c->child, m);
        m->pending_sep = 1;
    } else if (strcmp(t, "p") == 0) {
        if (!c->child) return;
        md_sep(m);
        md_inline(c->child, m);
        m->pending_sep = 1;
    } else if (strcmp(t, "pre") == 0) {
        md_sep(m);
        sb_puts(&m->b, "```\n");
        pre_collect(c, m);
        sb_puts(&m->b, "\n```");
        m->pending_sep = 1;
    } else if (strcmp(t, "blockquote") == 0) {
        md_blockquote(c, m);
    } else if (strcmp(t, "ul") == 0 || strcmp(t, "ol") == 0) {
        md_list(c, m, 0, t[0] == 'o');
    } else if (strcmp(t, "table") == 0) {
        md_table(c, m);
    } else if (strcmp(t, "hr") == 0) {
        md_sep(m);
        sb_puts(&m->b, "---");
        m->pending_sep = 1;
    } else if (strcmp(t, "dl") == 0) {
        for (html_node_t *e = c->child; e; e = e->next) {
            if (html_is(e, "dt")) {
                md_sep(m);
                sb_puts(&m->b, "**");
                md_inline(e->child, m);
                sb_puts(&m->b, "** — ");
                m->pending_sep = 0;
            } else if (html_is(e, "dd")) {
                md_inline(e->child, m);
                sb_putc(&m->b, '\n');
                m->pending_sep = 1;
            }
        }
    } else {
        /* transparent block containers: div, section, article,
         * main, header, footer, body, figure, span-at-block… */
        md_block(c->child, m);
    }
}

static void md_block(html_node_t *n, md_t *m)
{
    for (html_node_t *c = n; c; c = c->next) {
        md_one(c, m);
        if (m->pending_sep == 2) return; /* truncated */
    }
}

char *html_to_markdown(html_doc_t *doc)
{
    if (!doc || !doc->root) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    html_node_t *pick = readability_pick(doc);
    md_t m;
    memset(&m, 0, sizeof m);
    md_one(pick, &m);
    if (!m.b.data) {
        m.b.data = malloc(1);
        if (!m.b.data) return NULL;
        m.b.data[0] = '\0';
    } else {
        /* strip a trailing blank line pair if the emitter owes one */
        size_t len = strlen(m.b.data);
        while (len > 0 && (m.b.data[len - 1] == '\n' || m.b.data[len - 1] == ' '))
            m.b.data[--len] = '\0';
    }
    return m.b.data;
}
