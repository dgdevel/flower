/*
 * fs — local-filesystem tools for /mcp. See fs.h for the contract.
 */
#define _POSIX_C_SOURCE 200809L /* getline */
#define _DEFAULT_SOURCE /* DT_DIR & friends */

#include "fs.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define READ_DEFAULT  (64 * 1024) /* bytes when length is omitted   */
#define READ_MAX      (64 * 1024) /* hard clamp: the reply must fit */
#define LIST_MAX      2000        /* lines before "[truncated]"     */
#define LIST_DEPTH    48          /* recursion cap for **           */

/* ---------- read_file ---------- */

/* number of bytes a utf-8 lead byte promises (0 for a continuation
 * or invalid byte) */
static int utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 0;
}

char *fs_read_path(const char *path, long long offset, long long length,
                   char *err, size_t err_n)
{
    if (!path || !*path) {
        snprintf(err, err_n, "missing required string argument 'path'");
        return NULL;
    }
    if (offset < 0) {
        snprintf(err, err_n, "offset must be 0 or more");
        return NULL;
    }
    if (length <= 0) {
        snprintf(err, err_n, "length must be 1 or more");
        return NULL;
    }
    if (length > READ_MAX) length = READ_MAX;

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_n, "cannot open %s: %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        snprintf(err, err_n, "cannot stat %s: %s", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    if (S_ISDIR(st.st_mode)) {
        snprintf(err, err_n, "%s is a directory (list_files lists those)",
                 path);
        fclose(f);
        return NULL;
    }
    if ((long long)st.st_size > 0 && offset >= (long long)st.st_size) {
        snprintf(err, err_n, "offset %lld is past the end of %s (%lld bytes)",
                 offset, path, (long long)st.st_size);
        fclose(f);
        return NULL;
    }

    if (fseeko(f, offset, SEEK_SET) != 0) {
        snprintf(err, err_n, "cannot seek %s: %s", path, strerror(errno));
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)length + 1);
    if (!buf) {
        snprintf(err, err_n, "out of memory");
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)length, f);
    fclose(f);

    /* a NUL byte means this is not text the model can use */
    if (memchr(buf, '\0', n)) {
        free(buf);
        snprintf(err, err_n, "%s looks like a binary file", path);
        return NULL;
    }

    /* keep utf-8 sequences whole: skip into a cut one at the start,
     * trim a partial one at the end */
    size_t start = 0;
    while (start < n && utf8_len((unsigned char)buf[start]) == 0)
        start++; /* continuation bytes: the tail of a cut character */
    size_t end = n;
    for (int back = 1; back <= 3 && end - start >= (size_t)back; back++) {
        unsigned char c = (unsigned char)buf[end - back];
        if (utf8_len(c) > 0) {
            if (utf8_len(c) > back) end -= (size_t)back; /* incomplete */
            break;
        }
        /* still continuation bytes, keep walking back */
    }

    size_t keep = end - start;
    memmove(buf, buf + start, keep);
    buf[keep] = '\0';
    return buf;
}

/* ---------- list_files ---------- */

typedef struct {
    char **v;
    size_t n, cap;
    int truncated;
} lines_t;

static int lines_add(lines_t *l, char *line)
{
    if (l->n >= LIST_MAX) {
        l->truncated = 1;
        free(line);
        return 1;
    }
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        char **v = realloc(l->v, cap * sizeof *v);
        if (!v) return -1;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = line;
    return 0;
}

static void lines_free(lines_t *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* is this entry a directory we may descend into? symlinked
 * directories are never entered (no loops); DT_UNKNOWN filesystems
 * pay one lstat */
static int entry_is_dir(const char *base, const struct dirent *de)
{
    if (de->d_type == DT_DIR) return 1;
    if (de->d_type != DT_UNKNOWN) return 0;
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", base, de->d_name);
    struct stat st;
    return lstat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

/* one directory level: entries matching `leaf` are added as
 * "base/name" (directories with a trailing "/"). Returns 0 ok,
 * 1 cap reached, -1 io error. */
static int list_level(const char *base, const char *leaf, lines_t *out)
{
    DIR *d = opendir(base[0] ? base : ".");
    if (!d) return -1;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (fnmatch(leaf, de->d_name, 0) != 0) continue;

        int is_dir = entry_is_dir(base, de);
        char *line = malloc(strlen(base) + strlen(de->d_name) + 3);
        if (!line) continue;
        sprintf(line, "%s/%s%s", base, de->d_name, is_dir ? "/" : "");
        if (lines_add(out, line) != 0) { /* cap reached */
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* recursive walk for `**`-prefixed patterns: match `leaf` at every
 * level (the base directory included), depth-capped */
static void walk_level(const char *base, const char *leaf, lines_t *out,
                       int depth)
{
    if (depth > LIST_DEPTH) return;
    if (list_level(base, leaf, out) != 0) return; /* cap or io error */
    if (out->truncated) return;

    DIR *d = opendir(base[0] ? base : ".");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && !out->truncated) {
        if (de->d_name[0] == '.') continue;
        if (!entry_is_dir(base, de)) continue;
        char *sub = malloc(strlen(base) + strlen(de->d_name) + 2);
        if (!sub) continue;
        sprintf(sub, "%s/%s", base, de->d_name);
        walk_level(sub, leaf, out, depth + 1);
        free(sub);
    }
    closedir(d);
}

char *fs_glob(const char *dir, const char *pattern,
              char *err, size_t err_n)
{
    if (!dir || !*dir) {
        snprintf(err, err_n, "missing required string argument 'path'");
        return NULL;
    }
    if (!pattern || !*pattern) {
        snprintf(err, err_n, "missing required string argument 'glob'");
        return NULL;
    }

    struct stat st;
    if (stat(dir, &st) != 0) {
        snprintf(err, err_n, "cannot stat %s: %s", dir, strerror(errno));
        return NULL;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(err, err_n, "%s is not a directory", dir);
        return NULL;
    }

    const char *leaf = pattern;
    int recursive = 0;
    if (strncmp(pattern, "**/", 3) == 0) {
        recursive = 1;
        leaf = pattern + 3;
        if (!*leaf) leaf = "*";
    }

    lines_t out = { 0 };
    if (recursive)
        walk_level(dir, leaf, &out, 0);
    else
        list_level(dir, leaf, &out);

    qsort(out.v, out.n, sizeof out.v[0], cmp_str);

    /* assemble: one path per line (+ a truncation notice) */
    size_t total = 1;
    for (size_t i = 0; i < out.n; i++) total += strlen(out.v[i]) + 1;
    if (out.truncated) total += strlen("[truncated]\n");
    char *text = malloc(total + 1);
    if (!text) {
        lines_free(&out);
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    size_t w = 0;
    for (size_t i = 0; i < out.n; i++)
        w += (size_t)snprintf(text + w, total + 1 - w, "%s\n", out.v[i]);
    if (out.truncated)
        snprintf(text + w, total + 1 - w, "[truncated]\n");
    lines_free(&out);
    return text;
}

/* ---------- grep ---------- */

#define GREP_MAX_MATCHES 200               /* matches before stopping */
#define GREP_MAX_LINE    240               /* bytes shown of one line */
#define GREP_FILE_MAX    (8 * 1024 * 1024) /* larger files are skipped */
#define GREP_OUT_MAX     (64 * 1024)       /* reply bytes, then stop  */

/* s appended whole, or cut to at most max bytes on a utf-8 boundary
 * and marked with an ellipsis. 0 ok, -1 out of memory. */
static int sb_puts_cut(sbuf_t *b, const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max) return sb_putn(b, s, n);
    size_t cut = max;
    while (cut > 0 && ((unsigned char)s[cut] & 0xc0) == 0x80) cut--;
    if (sb_putn(b, s, cut) != 0) return -1;
    return sb_puts(b, "\xE2\x80\xA6"); /* … */
}

/* split a filepath glob into the directory to search and the leaf
 * pattern in the list_files language ("*.c", or the recursive **
 * form). A glob without a metacharacter is one file — or a
 * directory, which means every file under it. 0 ok, -1 with err
 * otherwise. */
static int split_glob(const char *glob, char *dir, size_t dir_n,
                      char *leaf, size_t leaf_n, char *err, size_t err_n)
{
    const char *meta = strpbrk(glob, "*?[");
    const char *dp, *lp; /* directory bytes, leaf string */
    size_t dn;

    if (meta) {
        /* the directory: everything up to the last '/' that
         * precedes the first metacharacter */
        const char *slash = NULL;
        for (const char *q = meta; q > glob; q--)
            if (q[-1] == '/') { slash = q - 1; break; }
        if (!slash)             { dp = "."; dn = 1; lp = glob; }
        else if (slash == glob) { dp = "/"; dn = 1; lp = slash + 1; }
        else { dp = glob; dn = (size_t)(slash - glob); lp = slash + 1; }

        /* the leaf: one component, with or without the recursive
         ** prefix */
        int recursive = strncmp(lp, "**/", 3) == 0 || strcmp(lp, "**") == 0;
        if (recursive) {
            lp += lp[2] == '/' ? 3 : 2;
            if (!*lp) lp = "*";
        }
        if (strchr(lp, '/')) {
            snprintf(err, err_n, "the glob may only pattern its last "
                                 "path component (like dir/**/*.c)");
            return -1;
        }
        if (dn >= dir_n || strlen(lp) + (size_t)recursive * 3 >= leaf_n)
            goto too_long;
        memcpy(dir, dp, dn);
        dir[dn] = '\0';
        if (recursive) sprintf(leaf, "**/%s", lp);
        else           sprintf(leaf, "%s", lp);
        return 0;
    }

    /* no metacharacter: a directory means every file under it */
    struct stat st;
    if (stat(glob, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (strlen(glob) >= dir_n || 5 >= leaf_n) goto too_long;
        strcpy(dir, glob);
        strcpy(leaf, "**/*");
        return 0;
    }
    /* one file: split at its last '/' */
    const char *slash = strrchr(glob, '/');
    if (!slash)             { dp = "."; dn = 1; lp = glob; }
    else if (slash == glob) { dp = "/"; dn = 1; lp = slash + 1; }
    else { dp = glob; dn = (size_t)(slash - glob); lp = slash + 1; }
    if (dn >= dir_n || strlen(lp) >= leaf_n) goto too_long;
    memcpy(dir, dp, dn);
    dir[dn] = '\0';
    strcpy(leaf, lp);
    return 0;

too_long:
    snprintf(err, err_n, "glob is too long");
    return -1;
}

char *fs_grep(const char *glob, const char *pattern,
              char *err, size_t err_n)
{
    if (!glob || !*glob) {
        snprintf(err, err_n, "missing required string argument 'glob'");
        return NULL;
    }
    if (!pattern || !*pattern) {
        snprintf(err, err_n, "missing required string argument 'pattern'");
        return NULL;
    }

    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char why[128];
        regerror(rc, &re, why, sizeof why);
        snprintf(err, err_n, "invalid pattern: %s", why);
        return NULL;
    }

    char dir[4096], leaf[512];
    if (split_glob(glob, dir, sizeof dir, leaf, sizeof leaf,
                   err, err_n) != 0) {
        regfree(&re);
        return NULL;
    }

    struct stat st;
    if (stat(dir, &st) != 0) {
        snprintf(err, err_n, "cannot stat %s: %s", dir, strerror(errno));
        regfree(&re);
        return NULL;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(err, err_n, "%s is not a directory", dir);
        regfree(&re);
        return NULL;
    }

    /* the same enumeration list_files does */
    const char *leafpat = leaf;
    int recursive = 0;
    if (strncmp(leaf, "**/", 3) == 0) {
        recursive = 1;
        leafpat = leaf + 3;
        if (!*leafpat) leafpat = "*";
    }
    lines_t files = { 0 };
    if (recursive)
        walk_level(dir, leafpat, &files, 0);
    else
        list_level(dir, leafpat, &files);
    int list_capped = files.truncated;
    qsort(files.v, files.n, sizeof files.v[0], cmp_str);

    sbuf_t out = { 0 };
    size_t matched = 0, with_hits = 0, matches = 0, skipped = 0;
    int capped = 0, oom = 0;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t n;

    for (size_t i = 0; i < files.n && !capped && !oom; i++) {
        size_t plen = strlen(files.v[i]);
        if (plen && files.v[i][plen - 1] == '/') continue; /* directory */
        matched++;

        if (stat(files.v[i], &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size > (off_t)GREP_FILE_MAX) {
            skipped++;
            continue;
        }
        FILE *f = fopen(files.v[i], "rb");
        if (!f) {
            skipped++;
            continue;
        }

        int hits_here = 0;
        size_t lineno = 0;
        while ((n = getline(&line, &line_cap, f)) > 0) {
            lineno++;
            if (memchr(line, '\0', (size_t)n)) { /* not text */
                skipped++;
                break;
            }
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                n--;
            line[n] = '\0';
            if (regexec(&re, line, 0, NULL, 0) != 0) continue;

            matches++;
            if (++hits_here == 1) with_hits++;
            char num[32];
            snprintf(num, sizeof num, ":%zu:", lineno);
            if (sb_puts(&out, files.v[i]) != 0 ||
                sb_puts(&out, num) != 0 ||
                sb_puts_cut(&out, line, GREP_MAX_LINE) != 0 ||
                sb_putc(&out, '\n') != 0) {
                oom = 1;
                break;
            }
            if (matches >= GREP_MAX_MATCHES || out.len > GREP_OUT_MAX) {
                capped = 1;
                break;
            }
        }
        fclose(f);
    }
    free(line);
    lines_free(&files);
    regfree(&re);

    if (oom) {
        free(out.data);
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    if (!matched) {
        free(out.data);
        char *r = malloc(strlen(glob) + 32);
        if (!r) {
            snprintf(err, err_n, "out of memory");
            return NULL;
        }
        sprintf(r, "no files match %s\n", glob);
        return r;
    }

    /* the caps first, then the tally */
    char num[160];
    if (capped) {
        snprintf(num, sizeof num, "[stopped after %zu matches]\n", matches);
        if (sb_puts(&out, num) != 0) oom = 1;
    }
    if (!oom && list_capped) {
        snprintf(num, sizeof num, "[stopped after %d files]\n", LIST_MAX);
        if (sb_puts(&out, num) != 0) oom = 1;
    }
    if (!oom) {
        snprintf(num, sizeof num, "[%zu match%s in %zu of %zu file%s",
                 matches, matches == 1 ? "" : "es",
                 with_hits, matched, matched == 1 ? "" : "s");
        if (sb_puts(&out, num) != 0) oom = 1;
    }
    if (!oom && skipped) {
        snprintf(num, sizeof num,
                 ", %zu skipped: binary, unreadable or over 8 MB", skipped);
        if (sb_puts(&out, num) != 0) oom = 1;
    }
    if (!oom && sb_puts(&out, "]\n") != 0) oom = 1;
    if (oom) {
        free(out.data);
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    return out.data;
}

/* ---------- mcp dispatchers ---------- */

static int arg_number(const cJSON *args, const char *name, long long *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);
    if (!v || cJSON_IsNull(v)) {
        *out = -1; /* absent */
        return 0;
    }
    if (!cJSON_IsNumber(v)) {
        char msg[128];
        snprintf(msg, sizeof msg, "argument '%s' must be a number", name);
        return -1;
    }
    *out = (long long)v->valuedouble;
    return 1;
}

char *fs_tool_read_file(const cJSON *args, char *err, size_t err_n)
{
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!cJSON_IsString(p) || !p->valuestring[0]) {
        snprintf(err, err_n, "missing required string argument 'path'");
        return NULL;
    }
    long long offset = 0, length = READ_DEFAULT;
    long long v;
    int r = arg_number(args, "offset", &v);
    if (r < 0) {
        snprintf(err, err_n, "argument 'offset' must be a number");
        return NULL;
    }
    if (r > 0) offset = v;
    r = arg_number(args, "length", &v);
    if (r < 0) {
        snprintf(err, err_n, "argument 'length' must be a number");
        return NULL;
    }
    if (r > 0) length = v;
    return fs_read_path(p->valuestring, offset, length, err, err_n);
}

char *fs_tool_list_files(const cJSON *args, char *err, size_t err_n)
{
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!cJSON_IsString(p) || !p->valuestring[0]) {
        snprintf(err, err_n, "missing required string argument 'path'");
        return NULL;
    }
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(args, "glob");
    if (!cJSON_IsString(g) || !g->valuestring[0]) {
        snprintf(err, err_n, "missing required string argument 'glob'");
        return NULL;
    }
    return fs_glob(p->valuestring, g->valuestring, err, err_n);
}

char *fs_tool_grep(const cJSON *args, char *err, size_t err_n)
{
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(args, "glob");
    if (!cJSON_IsString(g) || !g->valuestring[0]) {
        snprintf(err, err_n, "missing required string argument 'glob'");
        return NULL;
    }
    const cJSON *pj = cJSON_GetObjectItemCaseSensitive(args, "pattern");
    if (!cJSON_IsString(pj) || !pj->valuestring[0]) {
        snprintf(err, err_n, "missing required string argument 'pattern'");
        return NULL;
    }
    return fs_grep(g->valuestring, pj->valuestring, err, err_n);
}
