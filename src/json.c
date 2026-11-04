/* json.c - minimal json tree: recursive-descent parser and compact
   serializer, no external deps. rfc 8259 subset: objects, arrays,
   strings with \uXXXX escapes (surrogate pairs fold to one utf-8
   sequence, lone surrogates are errors), numbers via strtod, literals
   true/false/null. nesting deeper than 64 is an error. */
#include "flower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

typedef struct {
    const char *p; /* cursor into the document */
    int depth;
} jparse_t;

/* ---------------- helpers ---------------- */

static json_t *jnew(json_type_t t) {
    json_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->type = t;
    if (t == JSON_STR) {
        n->str = calloc(1, 1);
        if (!n->str) {
            free(n);
            return NULL;
        }
    }
    return n;
}

json_t *json_new(json_type_t t) { return jnew(t); }

json_t *json_str(const char *s) {
    json_t *n = jnew(JSON_STR);
    if (!n) return NULL;
    free(n->str);
    n->str = str_dup(s);
    if (!n->str) {
        free(n);
        return NULL;
    }
    return n;
}

json_t *json_num(double v) {
    json_t *n = jnew(JSON_NUM);
    if (n) n->num = v;
    return n;
}

json_t *json_bool(int v) {
    json_t *n = jnew(JSON_BOOL);
    if (n) n->num = v ? 1 : 0;
    return n;
}

json_t *json_arr(void) { return jnew(JSON_ARR); }
json_t *json_obj(void) { return jnew(JSON_OBJ); }

void json_add(json_t *parent, json_t *child) {
    json_t **slot;
    if (!parent || !child) return;
    if (!parent->child) {
        parent->child = child;
        return;
    }
    for (slot = &parent->child; *slot; slot = &(*slot)->next);
    *slot = child;
}

void json_free(json_t *node) {
    while (node) {
        json_t *next = node->next;
        json_free(node->child);
        free(node->key);
        free(node->str);
        free(node);
        node = next;
    }
}

json_t *json_get(const json_t *obj, const char *key) {
    json_t *c;
    if (!obj || !key || obj->type != JSON_OBJ) return NULL;
    for (c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

const char *json_str_of(const json_t *obj, const char *key) {
    json_t *c = json_get(obj, key);
    return (c && c->type == JSON_STR) ? c->str : NULL;
}

/* ---------------- parser ---------------- */

static void jskip_ws(jparse_t *j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
        j->p++;
}

/* append one utf-8 sequence for cp to buf. cp < 0x10000, no surrogate.
   returns 0 ok, -1 oom. */
static int jput_utf8(buf_t *b, unsigned cp) {
    if (cp < 0x80) {
        char c = (char)cp;
        return buf_append(b, &c, 1);
    }
    if (cp < 0x800) {
        char s[2];
        s[0] = (char)(0xC0 | (cp >> 6));
        s[1] = (char)(0x80 | (cp & 0x3F));
        return buf_append(b, s, 2);
    }
    {
        char s[3];
        s[0] = (char)(0xE0 | (cp >> 12));
        s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        s[2] = (char)(0x80 | (cp & 0x3F));
        return buf_append(b, s, 3);
    }
}

/* parse \uXXXX at j->p (already past the backslash-u). returns 0 ok
   with *out set, -1 on bad hex. */
static int jhex4(jparse_t *j, unsigned *out) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = *j->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* parse a quoted string (j->p at the opening quote). returns a
   malloc'd decoded string, NULL on error/oom. */
static char *jstring(jparse_t *j) {
    buf_t b;
    char *out;
    j->p++; /* opening quote */
    buf_init(&b);
    for (;;) {
        unsigned char c = (unsigned char)*j->p;
        if (c == '"') {
            j->p++;
            break;
        }
        if (c == '\0') goto fail; /* unterminated */
        if (c < 0x20) goto fail;  /* raw control char */
        if (c == '\\') {
            char e = *++j->p;
            static const char from[] = {'"',  '\\', '/', 'b', 'f',
                                        'n',  'r',  't'};
            static const char to[] = {'"', '\\', '/', '\b', '\f',
                                      '\n', '\r', '\t'};
            size_t k;
            j->p++;
            if (e == 'u') {
                unsigned cp;
                if (jhex4(j, &cp)) goto fail;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (j->p[0] != '\\' || j->p[1] != 'u')
                        goto fail; /* lone high surrogate */
                    j->p += 2; /* step past \u of the low half */
                    if (jhex4(j, &lo) || lo < 0xDC00 || lo > 0xDFFF)
                        goto fail;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    goto fail; /* lone low surrogate */
                }
                if (cp < 0x10000) {
                    if (jput_utf8(&b, cp)) goto fail;
                } else {
                    char s[4];
                    s[0] = (char)(0xF0 | (cp >> 18));
                    s[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    s[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    s[3] = (char)(0x80 | (cp & 0x3F));
                    if (buf_append(&b, s, 4)) goto fail;
                }
            } else {
                for (k = 0; k < sizeof from; k++)
                    if (from[k] == e) break;
                if (k == sizeof from) goto fail;
                if (buf_append(&b, &to[k], 1)) goto fail;
            }
            continue;
        }
        if (buf_append(&b, j->p, 1)) goto fail;
        j->p++;
    }
    out = b.data ? str_dup(b.data) : str_dup("");
    buf_free(&b);
    return out;
fail:
    buf_free(&b);
    return NULL;
}

static json_t *jvalue(jparse_t *j);

/* array/object bodies: j->p at the opening bracket. the shared loop
   avoids duplicating the comma logic; objects parse "key": value and
   hand the member name to the child. */
static json_t *jcontainer(jparse_t *j, json_type_t t) {
    char close = t == JSON_ARR ? ']' : '}';
    json_t *node;
    if (++j->depth > JSON_MAX_DEPTH) {
        j->depth--;
        return NULL;
    }
    node = jnew(t);
    if (!node) {
        j->depth--;
        return NULL;
    }
    j->p++; /* opening bracket */
    jskip_ws(j);
    if (*j->p == close) {
        j->p++;
        j->depth--;
        return node;
    }
    for (;;) {
        json_t *val;
        char *key = NULL;
        if (t == JSON_OBJ) {
            if (*j->p != '"') goto fail;
            key = jstring(j);
            if (!key) goto fail;
            jskip_ws(j);
            if (*j->p != ':') {
                free(key);
                goto fail;
            }
            j->p++;
            jskip_ws(j);
        }
        val = jvalue(j);
        if (!val) {
            free(key);
            goto fail;
        }
        val->key = key;
        json_add(node, val);
        jskip_ws(j);
        if (*j->p == ',') {
            j->p++;
            jskip_ws(j);
            continue;
        }
        if (*j->p == close) {
            j->p++;
            break;
        }
        goto fail;
    }
    j->depth--;
    return node;
fail:
    json_free(node);
    j->depth--;
    return NULL;
}

/* number: strtod with a leading-minus guard plus at least one digit so
   "-" and ".5" fail. */
static json_t *jnumber(jparse_t *j) {
    char *end;
    double v;
    const char *start = j->p;
    if (*j->p == '-') j->p++;
    if (*j->p < '0' || *j->p > '9') return NULL;
    v = strtod(start, &end);
    if (end == start) return NULL;
    j->p = end;
    return json_num(v);
}

static json_t *jvalue(jparse_t *j) {
    jskip_ws(j);
    switch (*j->p) {
    case '{':
        return jcontainer(j, JSON_OBJ);
    case '[':
        return jcontainer(j, JSON_ARR);
    case '"': {
        char *s = jstring(j);
        json_t *n = s ? jnew(JSON_STR) : NULL;
        if (s && !n) {
            free(s);
            return NULL;
        }
        if (n) {
            free(n->str);
            n->str = s;
        }
        return n;
    }
    case 't':
        if (strncmp(j->p, "true", 4)) return NULL;
        j->p += 4;
        return json_bool(1);
    case 'f':
        if (strncmp(j->p, "false", 5)) return NULL;
        j->p += 5;
        return json_bool(0);
    case 'n':
        if (strncmp(j->p, "null", 4)) return NULL;
        j->p += 4;
        return jnew(JSON_NULL);
    default:
        return jnumber(j);
    }
}

json_t *json_parse(const char *s) {
    jparse_t j;
    json_t *root;
    if (!s) return NULL;
    j.p = s;
    j.depth = 0;
    root = jvalue(&j);
    if (!root) return NULL;
    jskip_ws(&j);
    if (*j.p != '\0') { /* trailing junk */
        json_free(root);
        return NULL;
    }
    return root;
}

/* ---------------- serializer ---------------- */

static int jesc_str(buf_t *b, const char *s) {
    size_t i, n = strlen(s);
    if (buf_append(b, "\"", 1)) return -1;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            if (buf_append(b, "\\\"", 2)) return -1;
            break;
        case '\\':
            if (buf_append(b, "\\\\", 2)) return -1;
            break;
        case '\b':
            if (buf_append(b, "\\b", 2)) return -1;
            break;
        case '\f':
            if (buf_append(b, "\\f", 2)) return -1;
            break;
        case '\n':
            if (buf_append(b, "\\n", 2)) return -1;
            break;
        case '\r':
            if (buf_append(b, "\\r", 2)) return -1;
            break;
        case '\t':
            if (buf_append(b, "\\t", 2)) return -1;
            break;
        default:
            if (c < 0x20) {
                if (buf_appendf(b, "\\u%04x", (unsigned)c)) return -1;
            } else if (buf_append(b, s + i, 1)) {
                return -1;
            }
        }
    }
    return buf_append(b, "\"", 1);
}

static int jnum_str(buf_t *b, double v) {
    double ab = v < 0 ? -v : v;
    if (v == (double)(long long)v && ab < 1e15)
        return buf_appendf(b, "%lld", (long long)v);
    return buf_appendf(b, "%.17g", v);
}

static int jprint(buf_t *b, const json_t *n) {
    const json_t *c;
    if (!n) return buf_append(b, "null", 4);
    switch (n->type) {
    case JSON_NULL:
        return buf_append(b, "null", 4);
    case JSON_BOOL:
        return buf_append(b, n->num ? "true" : "false",
                          n->num ? 4 : 5);
    case JSON_NUM:
        return jnum_str(b, n->num);
    case JSON_STR:
        return jesc_str(b, n->str ? n->str : "");
    case JSON_ARR:
        if (buf_append(b, "[", 1)) return -1;
        for (c = n->child; c; c = c->next) {
            if (c != n->child && buf_append(b, ",", 1)) return -1;
            if (jprint(b, c)) return -1;
        }
        return buf_append(b, "]", 1);
    case JSON_OBJ:
        if (buf_append(b, "{", 1)) return -1;
        for (c = n->child; c; c = c->next) {
            if (c != n->child && buf_append(b, ",", 1)) return -1;
            if (jesc_str(b, c->key ? c->key : "")) return -1;
            if (buf_append(b, ":", 1)) return -1;
            if (jprint(b, c)) return -1;
        }
        return buf_append(b, "}", 1);
    }
    return -1;
}

char *json_print(const json_t *node) {
    buf_t b;
    char *out;
    buf_init(&b);
    if (jprint(&b, node)) {
        buf_free(&b);
        return NULL;
    }
    out = b.data ? b.data : str_dup("");
    if (!out) buf_free(&b);
    return out;
}
