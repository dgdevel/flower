/*
 * analyze — the structure analyzers (see analyze.h for the
 * contract). One analyzer per file type; the registry at the
 * bottom is the extension point: one function plus one entry per
 * type, picked by extension.
 */
#define _POSIX_C_SOURCE 200809L

#include "analyze.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FILE_MAX   (8 * 1024 * 1024) /* larger files are refused */
#define NODES_MAX  (1000)            /* nodes before truncation  */
#define DEPTH_MAX  (30)              /* indent levels rendered   */
#define LABEL_MAX  (200)             /* bytes of one node label  */
#define ACC_MAX    (1024)            /* bytes of one c decl head */

/* ---------- shared plumbing ---------- */

/* one line at a time (trailing \r stripped): line/len point into
 * the text, cursor moves past the newline; 0 at the end. */
static int next_line(const char **cursor, const char **line, size_t *len)
{
    const char *s = *cursor;
    if (!*s) return 0;
    const char *nl = strchr(s, '\n');
    *line = s;
    if (nl) {
        *len = (size_t)(nl - s);
        *cursor = nl + 1;
    } else {
        *len = strlen(s);
        *cursor = s + *len;
    }
    while (*len > 0 && (*line)[*len - 1] == '\r') (*len)--;
    return 1;
}

/* a %.*s precision that fits a label buffer (and -Wformat-truncation) */
static int ncl(size_t n)
{
    return n > LABEL_MAX ? LABEL_MAX : (int)n;
}

/* copy s[0..n) into dst; past LABEL_MAX bytes the copy is cut on a
 * utf-8 character boundary and closed with an ellipsis */
static void label_copy(char *dst, size_t dstn, const char *s, size_t n)
{
    size_t take = n > LABEL_MAX ? LABEL_MAX : n;
    if (take < n) {
        /* back off partial utf-8 sequences at the cut */
        for (int back = 1; back <= 3 && take >= (size_t)back; back++) {
            unsigned char c = (unsigned char)s[take - back];
            int len = c < 0x80 ? 1
                    : c >= 0xc2 && c <= 0xdf ? 2
                    : c >= 0xe0 && c <= 0xef ? 3
                    : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            if (len > 0) {
                if (len > back) take -= (size_t)back;
                break;
            }
        }
    }
    if (take + 8 > dstn) take = dstn > 8 ? dstn - 8 : 0;
    memcpy(dst, s, take);
    if (take < n) memcpy(dst + take, "\xE2\x80\xA6", 4); /* … */
    else dst[take] = '\0';
}

/* one node line: "%5ld", two spaces, two per depth, the label.
 * 0 ok, -1 out of memory; past NODES_MAX only the truncation
 * flag is set. */
static int node_add(nodes_t *ns, long lineno, int depth, const char *label)
{
    if (ns->truncated) return 0;
    if (ns->count >= NODES_MAX) {
        ns->truncated = 1;
        return 0;
    }
    if (depth > DEPTH_MAX) depth = DEPTH_MAX;
    if (depth < 0) depth = 0;
    char num[24];
    snprintf(num, sizeof num, "%5ld", lineno);
    if (sb_puts(&ns->out, num) != 0) return -1;
    if (sb_puts(&ns->out, "  ") != 0) return -1;
    for (int i = 0; i < depth; i++)
        if (sb_puts(&ns->out, "  ") != 0) return -1;
    if (sb_puts(&ns->out, label) != 0 ||
        sb_putc(&ns->out, '\n') != 0) return -1;
    ns->count++;
    return 0;
}

/* ---------- markdown: the heading tree ---------- */

static int analyze_markdown(const char *text, nodes_t *ns)
{
    const char *p = text, *line;
    size_t len;
    long lineno = 0;
    int fence = 0; /* inside a fenced code block */

    while (next_line(&p, &line, &len)) {
        lineno++;
        size_t i = 0;
        while (i < len && i < 3 && isspace((unsigned char)line[i])) i++;

        /* a fence line opens or closes a code block (any 3+ run of
         * ` or ~ at up to three spaces of indentation) */
        if (i < len && (line[i] == '`' || line[i] == '~')) {
            size_t run = 1;
            while (i + run < len && line[i + run] == line[i]) run++;
            if (run >= 3) {
                fence = !fence;
                continue;
            }
        }
        if (fence) continue;

        /* an atx heading: 1-6 '#', whitespace, then the title */
        int level = 0;
        while (i + (size_t)level < len && line[i + level] == '#') level++;
        if (level < 1 || level > 6) continue;
        i += (size_t)level;
        if (i >= len || !isspace((unsigned char)line[i])) continue;
        while (i < len && isspace((unsigned char)line[i])) i++;

        /* the title, minus trailing space and one trailing run of
         * '#'s (the prototype's \s*#*\s*$) */
        size_t end = len;
        while (end > i && isspace((unsigned char)line[end - 1])) end--;
        if (end > i && line[end - 1] == '#') {
            end--;
            while (end > i && line[end - 1] == '#') end--;
            while (end > i && isspace((unsigned char)line[end - 1])) end--;
        }
        char label[LABEL_MAX + 8];
        label_copy(label, sizeof label, line + i, end > i ? end - i : 0);
        if (node_add(ns, lineno, level - 1, label) != 0) return -1;
    }
    return 0;
}

/* ---------- python: classes and defs ---------- */

/* no python parser here: nesting comes from indentation, so a def
 * or class one level deeper than its enclosing def or class lands
 * under it (one hidden inside an if-block shows up a level down
 * instead of not at all) */

/* does `word` (lowercase, len wl) sit at line[pos..], followed by
 * whitespace (or the end of the line)? */
static int word_at(const char *line, size_t len, size_t pos,
                   const char *word, size_t wl)
{
    if (pos + wl > len) return 0; /* must fit before eol */
    if (strncmp(line + pos, word, wl) != 0) return 0;
    if (pos + wl == len) return 1;     /* keyword alone on the line */
    return line[pos + wl] == ' ' || line[pos + wl] == '\t';
}

static int analyze_python(const char *text, nodes_t *ns)
{
    const char *p = text, *line;
    size_t len;
    long lineno = 0;
    int indents[DEPTH_MAX + 2]; /* enclosing def/class indents; [0]
                                 * is the module level, always 0 */
    int depth_n = 1;
    indents[0] = 0;

    while (next_line(&p, &line, &len)) {
        lineno++;
        size_t i = 0;
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= len || line[i] == '#') continue;

        const char *kw = NULL;
        size_t j = i;
        if (word_at(line, len, j, "class", 5)) {
            kw = "class";
            j += 5;
        } else if (word_at(line, len, j, "def", 3)) {
            kw = "def";
            j += 3;
        } else if (word_at(line, len, j, "async", 5)) {
            size_t k = j + 5;
            while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
            if (word_at(line, len, k, "def", 3)) {
                kw = "async def";
                j = k + 3;
            }
        }
        if (!kw) continue;

        while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
        size_t nend = j;
        while (nend < len && (isalnum((unsigned char)line[nend]) ||
                              line[nend] == '_'))
            nend++;
        if (nend == j) continue; /* no name after the keyword */

        /* the signature: the parenthesized group after the name —
         * bases for a class, parameters for a def — when the line
         * closes it (a multi-line parameter list keeps the bare
         * name, parens empty) */
        size_t po = nend, pc = 0;
        while (po < len && (line[po] == ' ' || line[po] == '\t')) po++;
        if (po < len && line[po] == '(') {
            int pd = 0;
            for (size_t m = po; m < len; m++) {
                if (line[m] == '(') pd++;
                else if (line[m] == ')' && --pd == 0) { pc = m; break; }
            }
        }

        /* dedent: pop the defs/classes this one closes */
        while (depth_n > 1 && indents[depth_n - 1] >= (int)i) depth_n--;
        char label[512];
        if (pc) {
            size_t a = po + 1, b = pc; /* the group, ws-trimmed */
            while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
            while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
            snprintf(label, sizeof label, "%s %.*s(%.*s)",
                     strcmp(kw, "class") == 0 ? "class" : kw,
                     ncl(nend - j), line + j, ncl(b - a), line + a);
        } else if (strcmp(kw, "class") == 0) {
            snprintf(label, sizeof label, "class %.*s",
                     ncl(nend - j), line + j);
        } else {
            snprintf(label, sizeof label, "%s %.*s()",
                     kw, ncl(nend - j), line + j);
        }
        if (node_add(ns, lineno, depth_n - 1, label) != 0) return -1;
        if (depth_n < (int)(sizeof indents / sizeof indents[0]))
            indents[depth_n++] = (int)i;
    }
    return 0;
}

/* ---------- the C family (C/C++/Java/C#/JS-ish) ---------- */

static const char *const C_KEYWORDS[] = {
    "abstract", "async", "await", "auto", "bool", "break", "case",
    "catch", "char", "class", "const", "constexpr", "continue",
    "default", "delete", "do", "double", "else", "enum", "export",
    "extends", "extern", "false", "final", "float", "for", "friend",
    "function", "goto", "if", "implements", "import", "inline",
    "instanceof", "int", "interface", "let", "long", "namespace",
    "native", "new", "noexcept", "nullptr", "operator", "override",
    "package", "private", "protected", "public", "register",
    "restrict", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "synchronized", "template", "this", "throw",
    "true", "try", "typedef", "typename", "typeof", "union",
    "unsigned", "using", "var", "virtual", "void", "volatile",
    "while", "yield",
};

static int is_c_keyword(const char *s, size_t n)
{
    for (size_t k = 0;
         k < sizeof C_KEYWORDS / sizeof C_KEYWORDS[0]; k++)
        if (strlen(C_KEYWORDS[k]) == n &&
            strncmp(C_KEYWORDS[k], s, n) == 0)
            return 1;
    return 0;
}

/* identifiers, the prototype's [A-Za-z_~]\w* runs */

static int id_start(unsigned char c)
{
    return c == '_' || c == '~' ||
           (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

typedef struct {
    const char *p;
    size_t n;
} tok_t;

/* the identifier runs in s[0..n): at most cap stored, the number
 * stored returned */
static size_t scan_idents(const char *s, size_t n, tok_t *out,
                          size_t cap)
{
    size_t count = 0;
    for (size_t i = 0; i < n; ) {
        if (id_start((unsigned char)s[i])) {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_'))
                j++;
            if (count < cap) out[count] = (tok_t){ s + i, j - i };
            count++;
            i = j;
        } else {
            i++;
        }
    }
    return count < cap ? count : cap;
}

/* the last identifier run in s[0..n), if any */
static int last_ident(const char *s, size_t n, tok_t *out)
{
    tok_t id = { NULL, 0 };
    size_t count = 0;
    for (size_t i = 0; i < n; ) {
        if (id_start((unsigned char)s[i])) {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_'))
                j++;
            id = (tok_t){ s + i, j - i };
            count++;
            i = j;
        } else {
            i++;
        }
    }
    if (!count) return 0;
    *out = id;
    return 1;
}

/* _func_name: the identifier just before the first '(' of a
 * declaration head (any ')' anywhere in the head makes it look
 * like a call or prototype, not a stray paren) */
static int func_name(const char *head, tok_t *out)
{
    const char *paren = strchr(head, '(');
    if (!paren || paren == head) return 0;
    if (!strchr(head, ')')) return 0;
    size_t before = (size_t)(paren - head);
    if (memchr(head, '=', before)) return 0;
    tok_t id;
    if (!last_ident(head, before, &id)) return 0;
    if (is_c_keyword(id.p, id.n)) return 0;
    *out = id;
    return 1;
}

/* _decl_name: array brackets and initializers dropped, the last
 * identifier that is not a keyword — copied into name (the scan
 * runs on a scratch buffer, so the result cannot point into it) */
static int decl_name(const char *head, char *name, size_t name_n)
{
    char buf[ACC_MAX];
    size_t w = 0;
    int bracket = 0;
    for (const char *q = head; *q && w + 1 < sizeof buf; q++) {
        if (*q == '[') { bracket = 1; continue; }
        if (*q == ']') { bracket = 0; continue; }
        if (bracket) continue;
        if (*q == '=') break;
        buf[w++] = *q;
    }
    buf[w] = '\0';
    tok_t ids[64];
    size_t n = scan_idents(buf, w, ids, sizeof ids / sizeof ids[0]);
    for (size_t k = n; k > 0; k--)
        if (!is_c_keyword(ids[k - 1].p, ids[k - 1].n)) {
            snprintf(name, name_n, "%.*s", ncl(ids[k - 1].n),
                     ids[k - 1].p);
            return 1;
        }
    return 0;
}

/* a member or typedef's label: the declaration head as it stands —
 * the type stays, so do array sizes and bitfield widths — minus
 * any initializer */
static void decl_label(char *dst, size_t dstn, const char *head)
{
    const char *eq = strchr(head, '=');
    size_t n = eq ? (size_t)(eq - head) : strlen(head);
    while (n > 0 && head[n - 1] == ' ') n--;
    label_copy(dst, dstn, head, n);
}

/* the next space-separated word of a collapsed head, or NULL */
static const char *next_word(const char **cursor, size_t *len)
{
    const char *p = *cursor;
    while (*p == ' ') p++;
    if (!*p) return NULL;
    const char *start = p;
    while (*p && *p != ' ') p++;
    *len = (size_t)(p - start);
    *cursor = p;
    return start;
}

static int eq_word(const char *w, size_t n, const char *lit)
{
    return strlen(lit) == n && strncmp(w, lit, n) == 0;
}

/* an aggregate head: [typedef] (struct|union|enum|class|
 * interface) [class] [name] [: bases] — and nothing else; the
 * whole head must be exactly that */
static int match_agg(const char *head, tok_t *kind, tok_t *name)
{
    const char *cur = head;
    const char *w;
    size_t n;

    name->p = NULL;
    name->n = 0;

    w = next_word(&cur, &n);
    if (!w) return 0;
    if (eq_word(w, n, "typedef")) {
        w = next_word(&cur, &n);
        if (!w) return 0;
    }
    if (!eq_word(w, n, "struct") && !eq_word(w, n, "union") &&
        !eq_word(w, n, "enum") && !eq_word(w, n, "class") &&
        !eq_word(w, n, "interface"))
        return 0;
    *kind = (tok_t){ w, n };

    if ((w = next_word(&cur, &n)) && eq_word(w, n, "class"))
        w = next_word(&cur, &n);

    /* the regex needs \s+ after the kind word: an anonymous
     * "struct {" (nothing but the kind) is not an aggregate */
    if (!w) return 0;

    if (w[0] != ':') {
        /* a plain identifier: [A-Za-z_]\w* */
        if (!(w[0] == '_' || (w[0] >= 'A' && w[0] <= 'Z') ||
              (w[0] >= 'a' && w[0] <= 'z')))
            return 0;
        for (size_t k = 1; k < n; k++)
            if (!(isalnum((unsigned char)w[k]) || w[k] == '_'))
                return 0;
        *name = (tok_t){ w, n };
        w = next_word(&cur, &n);
    }
    /* nothing, or a ':' inheritance tail */
    return !w || w[0] == ':';
}

/* blank_comments: comments and string/char literals become
 * spaces (newlines survive), so nothing inside them can look
 * like structure. Returns a malloc'd same-length copy, or NULL. */
static char *blank_comments(const char *text)
{
    size_t n = strlen(text);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t i = 0;
    int state = 0; /* 0 code 1 // 2 block 3 str 4 char */
    while (i < n) {
        char c = text[i];
        char nxt = i + 1 < n ? text[i + 1] : '\0';
        if (state == 0) {
            if (c == '/' && nxt == '/') {
                out[i++] = ' '; out[i++] = ' '; state = 1;
            } else if (c == '/' && nxt == '*') {
                out[i++] = ' '; out[i++] = ' '; state = 2;
            } else if (c == '"') {
                out[i++] = ' '; state = 3;
            } else if (c == '\'') {
                out[i++] = ' '; state = 4;
            } else {
                out[i++] = c;
            }
        } else if (state == 1) {
            out[i] = c == '\n' ? '\n' : ' ';
            if (c == '\n') state = 0;
            i++;
        } else if (state == 2) {
            if (c == '*' && nxt == '/') {
                out[i++] = ' '; out[i++] = ' '; state = 0;
            } else {
                out[i++] = c == '\n' ? '\n' : ' ';
            }
        } else { /* string or char literal */
            if (c == '\\' && nxt) {
                out[i++] = ' ';
                out[i++] = nxt == '\n' ? '\n' : ' ';
            } else if ((state == 3 && c == '"') ||
                       (state == 4 && c == '\'')) {
                out[i++] = ' ';
                state = 0;
            } else {
                out[i++] = c == '\n' ? '\n' : ' ';
            }
        }
    }
    out[n] = '\0';
    return out;
}

/* collapse a run of whitespace to single spaces */
static void collapse_ws(char *dst, size_t dstn, const char *src)
{
    size_t r = 0, w = 0;
    int pending = 0;
    while (src[r] && w + 1 < dstn) {
        if (src[r] == ' ' || src[r] == '\t') {
            pending = 1;
            r++;
            continue;
        }
        if (pending && w) dst[w++] = ' ';
        pending = 0;
        dst[w++] = src[r++];
    }
    dst[w] = '\0';
}

/* does a declaration head open with the `typedef` word? */
static int starts_typedef(const char *head)
{
    return strncmp(head, "typedef", 7) == 0 &&
           (head[7] == '\0' || head[7] == ' ');
}

static int analyze_c(const char *text, nodes_t *ns)
{
    char *src = blank_comments(text);
    if (!src) return -1;

    int stack[64];        /* brace depth at each open aggregate */
    size_t stack_n = 0;
    int depth = 0;
    char acc[ACC_MAX];    /* the declaration head being built */
    size_t acc_len = 0;
    long acc_line = -1;
    int acc_blank = 1;    /* acc still all whitespace */
    int acc_bad = 0;      /* head overran the cap: no node from it */

    const char *p = src, *line;
    size_t len;
    long lineno = 0;
    int rc = 0;

    while (next_line(&p, &line, &len) && rc == 0) {
        lineno++;
        size_t i = 0;
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i < len && line[i] == '#') { /* preprocessor line */
            acc_len = 0;
            acc_bad = 0;
            acc_blank = 1;
            acc_line = -1;
            continue;
        }

        for (size_t k = 0; k < len && rc == 0; k++) {
            char ch = line[k];

            if (ch != '{' && ch != '}' && ch != ';') {
                if (!acc_bad) {
                    if (acc_blank && ch != ' ' && ch != '\t') {
                        acc_line = lineno;
                        acc_blank = 0;
                    }
                    if (acc_len + 1 < sizeof acc)
                        acc[acc_len++] = ch;
                    else
                        acc_bad = 1;
                }
                continue;
            }

            acc[acc_len] = '\0';
            char head[ACC_MAX];
            collapse_ws(head, sizeof head, acc);

            if (ch == '{') {
                if (!acc_bad && head[0]) {
                    tok_t kind, name;
                    if (match_agg(head, &kind, &name)) {
                        char label[LABEL_MAX + 16];
                        if (name.n)
                            snprintf(label, sizeof label, "%.*s %.*s",
                                     ncl(kind.n), kind.p,
                                     ncl(name.n), name.p);
                        else
                            snprintf(label, sizeof label,
                                     "%.*s {{...}}", ncl(kind.n), kind.p);
                        rc = node_add(ns, acc_line, (int)stack_n, label);
                        if (stack_n < sizeof stack / sizeof stack[0])
                            stack[stack_n++] = depth;
                    } else {
                        tok_t fn;
                        if (stack_n == 0 && func_name(head, &fn)) {
                            char label[LABEL_MAX + 8];
                            size_t hlen = strlen(head);
                            if (hlen && head[hlen - 1] == ')')
                                /* the whole head is the signature */
                                label_copy(label, sizeof label,
                                           head, hlen);
                            else
                                snprintf(label, sizeof label,
                                         "%.*s()", ncl(fn.n), fn.p);
                            rc = node_add(ns, acc_line, 0, label);
                        }
                    }
                }
                depth++;
            } else if (ch == '}') {
                if (depth > 0) depth--;
                if (stack_n && stack[stack_n - 1] == depth) stack_n--;
            } else { /* ';' */
                if (!acc_bad) {
                    char name[LABEL_MAX + 8];
                    char label[LABEL_MAX + 8];
                    if (stack_n && depth == stack[stack_n - 1] + 1) {
                        /* a member: its declaration, type included */
                        if (decl_name(head, name, sizeof name)) {
                            decl_label(label, sizeof label, head);
                            rc = node_add(ns, acc_line, (int)stack_n,
                                          label);
                        }
                    } else if (depth == 0 && starts_typedef(head)) {
                        if (decl_name(head, name, sizeof name)) {
                            decl_label(label, sizeof label, head);
                            rc = node_add(ns, acc_line, 0, label);
                        }
                    }
                }
            }

            acc_len = 0;
            acc_bad = 0;
            acc_blank = 1;
            acc_line = -1;
        }
    }
    free(src);
    return rc;
}

/* ---------- the registry ---------- */

static const char *const EXTS_MARKDOWN[] = {
    ".md", ".markdown", ".mdown", NULL
};
static const char *const EXTS_C[] = {
    ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx",
    ".java", ".cs", ".js", ".jsx", ".ts", ".tsx", NULL
};
static const char *const EXTS_PYTHON[] = {
    ".py", ".pyi", NULL
};

static const analyzer_t ANALYZERS[] = {
    { "markdown", EXTS_MARKDOWN, analyze_markdown },
    { "c",        EXTS_C,        analyze_c },
    { "python",   EXTS_PYTHON,   analyze_python },
};

const analyzer_t *analyzers(void) { return ANALYZERS; }

size_t analyzers_count(void)
{
    return sizeof ANALYZERS / sizeof ANALYZERS[0];
}

const analyzer_t *analyzer_for_ext(const char *path)
{
    if (!path) return NULL;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return NULL; /* no (real) extension */

    char ext[32];
    size_t n = strlen(dot);
    if (n >= sizeof ext) return NULL;
    for (size_t i = 0; i <= n; i++)
        ext[i] = (char)tolower((unsigned char)dot[i]);

    for (size_t i = 0; i < analyzers_count(); i++)
        for (size_t k = 0; ANALYZERS[i].exts[k]; k++)
            if (strcmp(ANALYZERS[i].exts[k], ext) == 0)
                return &ANALYZERS[i];
    return NULL;
}

/* ---------- the tool body ---------- */

/* "path: unknown file type; known: markdown (.md …), c (…),
 * python (.py .pyi)" — what analyze can do, so the caller can
 * pick its next move */
static void unknown_type_err(const char *path, char *err, size_t err_n)
{
    int w = snprintf(err, err_n, "%s: unknown file type; known:",
                     path);
    if (w < 0) return;
    if ((size_t)w >= err_n) w = (int)err_n - 1;
    for (size_t i = 0; i < analyzers_count(); i++) {
        int n = snprintf(err + w, (size_t)(err_n - w), "%s %s (",
                         i ? "," : "", ANALYZERS[i].name);
        if (n < 0 || (size_t)n >= err_n - w) break;
        w += n;
        for (size_t k = 0; ANALYZERS[i].exts[k]; k++) {
            n = snprintf(err + w, (size_t)(err_n - w), "%s%s",
                         k ? " " : "", ANALYZERS[i].exts[k]);
            if (n < 0 || (size_t)n >= err_n - w) return;
            w += n;
        }
        n = snprintf(err + w, (size_t)(err_n - w), ")");
        if (n < 0 || (size_t)n >= err_n - w) break;
        w += n;
    }
}

char *analyze_file(const char *path, char *err, size_t err_n)
{
    if (!path || !*path) {
        snprintf(err, err_n, "missing required string argument 'path'");
        return NULL;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(err, err_n, "cannot stat %s: %s", path,
                 strerror(errno));
        return NULL;
    }
    if (S_ISDIR(st.st_mode)) {
        snprintf(err, err_n, "%s is a directory (list_files lists those)",
                 path);
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(err, err_n, "%s is not a regular file", path);
        return NULL;
    }
    if (st.st_size > FILE_MAX) {
        snprintf(err, err_n, "%s is over %d MiB", path,
                 FILE_MAX / (1024 * 1024));
        return NULL;
    }

    const analyzer_t *an = analyzer_for_ext(path);
    if (!an) {
        unknown_type_err(path, err, err_n);
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_n, "cannot open %s: %s", path,
                 strerror(errno));
        return NULL;
    }
    size_t cap = (size_t)st.st_size;
    char *text = malloc(cap + 1);
    if (!text) {
        fclose(f);
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    size_t got = fread(text, 1, cap, f);
    fclose(f);
    if (got != cap) {
        free(text);
        snprintf(err, err_n, "cannot read %s", path);
        return NULL;
    }
    text[cap] = '\0';
    if (memchr(text, '\0', cap)) {
        free(text);
        snprintf(err, err_n, "%s looks like a binary file", path);
        return NULL;
    }

    long nlines = 0;
    for (size_t i = 0; i < cap; i++)
        if (text[i] == '\n') nlines++;
    if (cap && text[cap - 1] != '\n') nlines++;

    nodes_t ns = { 0 };
    int rc = an->fn(text, &ns);
    free(text);

    /* render: header, the nodes, the caps */
    sbuf_t out = { 0 };
    char hdr[96];
    snprintf(hdr, sizeof hdr, "  [%s]  %ld lines\n", an->name, nlines);
    int ok = rc == 0 &&
             sb_puts(&out, path) == 0 &&
             sb_puts(&out, hdr) == 0 &&
             (ns.count || ns.truncated
                  ? sb_puts(&out, ns.out.data ? ns.out.data : "") == 0
                  : sb_puts(&out, "  (nothing found)\n") == 0) &&
             (!ns.truncated ||
              sb_puts(&out, "... [truncated]\n") == 0);
    free(ns.out.data);
    if (!ok) {
        free(out.data);
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    return out.data;
}
