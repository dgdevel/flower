/*
 * fs — local-filesystem tools for /mcp. See fs.h for the contract.
 */
#define _DEFAULT_SOURCE /* DT_DIR & friends */

#include "fs.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
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
