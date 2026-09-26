/*
 * embed — flower's build-time asset compiler.
 *
 * Recursively scans a directory (web/) and emits:
 *   - a C source file holding every file's bytes as string literals
 *   - a header declaring the lookup API
 * so the server serves everything from memory and ships as one binary.
 *
 * Usage: embed <out.c> <out.h> <dir>
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct {
    char *fs_path;  /* file on disk: web/index.html */
    char *url_path; /* served as:     /index.html   */
    const char *mime;
    size_t size;
    unsigned char *data;
} entry_t;

static entry_t *entries;
static size_t entry_count, entry_cap;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "embed: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s);
    if (!p) die("out of memory");
    return p;
}

static char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *s = xmalloc(la + 1 + lb + 1);
    memcpy(s, a, la);
    s[la] = '/';
    memcpy(s + la + 1, b, lb + 1);
    return s;
}

static const char *mime_for(const char *name)
{
    static const struct { const char *ext, *mime; } table[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "text/javascript" },
        { "mjs",  "text/javascript" },
        { "json", "application/json" },
        { "svg",  "image/svg+xml" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "webp", "image/webp" },
        { "ico",  "image/x-icon" },
        { "txt",  "text/plain; charset=utf-8" },
        { "woff", "font/woff" },
        { "woff2","font/woff2" },
        { "wasm", "application/wasm" },
    };
    const char *ext = strrchr(name, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcasecmp(ext, table[i].ext) == 0) return table[i].mime;
    return "application/octet-stream";
}

/* Only allow characters that survive being emitted into C string literals
 * and URLs verbatim. */
static int safe_url(const char *s)
{
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '/'))
            return 0;
    }
    return 1;
}

static void add_entry(const char *fs_path, const char *url_path)
{
    if (!safe_url(url_path))
        die("filename has unsupported characters: %s", fs_path);

    FILE *f = fopen(fs_path, "rb");
    if (!f) die("open %s: %s", fs_path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0) die("seek %s", fs_path);
    long sz = ftell(f);
    if (sz < 0) die("tell %s", fs_path);
    rewind(f);
    unsigned char *data = xmalloc((size_t)sz);
    if (sz > 0 && fread(data, 1, (size_t)sz, f) != (size_t)sz)
        die("read %s", fs_path);
    fclose(f);

    if (entry_count == entry_cap) {
        entry_cap = entry_cap ? entry_cap * 2 : 16;
        entries = realloc(entries, entry_cap * sizeof *entries);
        if (!entries) die("out of memory");
    }
    entries[entry_count].fs_path = xstrdup(fs_path);
    entries[entry_count].url_path = xstrdup(url_path);
    entries[entry_count].mime = mime_for(fs_path);
    entries[entry_count].size = (size_t)sz;
    entries[entry_count].data = data;
    entry_count++;
}

static int cmp_url(const void *a, const void *b)
{
    const entry_t *ea = a, *eb = b;
    return strcmp(ea->url_path, eb->url_path);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* fs_dir: path on disk; url_prefix: served path prefix ("", "css/", ...) */
static void scan_dir(const char *fs_dir, const char *url_prefix)
{
    DIR *d = opendir(fs_dir);
    if (!d) die("opendir %s: %s", fs_dir, strerror(errno));

    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue; /* ".", "..", dotfiles */
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            names = realloc(names, cap * sizeof *names);
            if (!names) die("out of memory");
        }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str); /* deterministic output */

    for (size_t i = 0; i < n; i++) {
        char *fs = path_join(fs_dir, names[i]);
        struct stat st;
        if (stat(fs, &st) != 0) die("stat %s: %s", fs, strerror(errno));
        if (S_ISDIR(st.st_mode)) {
            char *prefix = xmalloc(strlen(url_prefix) + strlen(names[i]) + 2);
            sprintf(prefix, "%s%s/", url_prefix, names[i]);
            scan_dir(fs, prefix);
            free(prefix);
        } else if (S_ISREG(st.st_mode)) {
            char *url = xmalloc(1 + strlen(url_prefix) + strlen(names[i]) + 1);
            sprintf(url, "/%s%s", url_prefix, names[i]);
            add_entry(fs, url);
            free(url);
        }
        free(fs);
        free(names[i]);
    }
    free(names);
}

/* Emit data as one (possibly multi-line, concatenated) C string literal. */
static void emit_literal(FILE *f, const unsigned char *data, size_t n)
{
    fputc('"', f);
    int col = 1;
    for (size_t i = 0; i < n; i++) {
        char esc[8];
        const char *s;
        unsigned char c = data[i];
        if (c == '"') s = "\\\"";
        else if (c == '\\') s = "\\\\";
        else if (c == '\n') s = "\\n";
        else if (c == '\r') s = "\\r";
        else if (c == '\t') s = "\\t";
        else if (c >= 0x20 && c <= 0x7e) { esc[0] = (char)c; esc[1] = '\0'; s = esc; }
        else { sprintf(esc, "\\%03o", c); s = esc; }
        size_t sl = strlen(s);
        if (col + sl > 72) { fputs("\"\n\"", f); col = 1; }
        fputs(s, f);
        col += (int)sl;
    }
    fputc('"', f);
}

static void write_header(FILE *f)
{
    fprintf(f,
        "/* Generated by tools/embed — DO NOT EDIT.\n"
        " * Inputs: everything under web/ at build time. */\n"
        "#ifndef FLOWER_ASSETS_GEN_H\n"
        "#define FLOWER_ASSETS_GEN_H\n\n"
        "#include <stddef.h>\n\n"
        "typedef struct {\n"
        "    const char *path;          /* served path, e.g. \"/index.html\" */\n"
        "    const char *mime;          /* content type sent to clients */\n"
        "    const unsigned char *data; /* file bytes, not NUL-terminated */\n"
        "    size_t size;\n"
        "} asset_t;\n\n"
        "const asset_t *assets_all(size_t *count);\n"
        "const asset_t *asset_find(const char *path); /* exact match, NULL if absent */\n\n"
        "#endif\n");
}

static void write_source(FILE *f)
{
    fprintf(f, "/* Generated by tools/embed — DO NOT EDIT. */\n");
    fprintf(f, "#include \"assets_gen.h\"\n\n#include <string.h>\n\n");
    for (size_t i = 0; i < entry_count; i++) {
        fprintf(f, "/* %s (%zu bytes) */\n", entries[i].fs_path, entries[i].size);
        fprintf(f, "static const unsigned char asset_data_%zu[] = ", i);
        emit_literal(f, entries[i].data, entries[i].size);
        fprintf(f, ";\n\n");
    }
    fprintf(f, "static const asset_t asset_table[] = {\n");
    for (size_t i = 0; i < entry_count; i++)
        fprintf(f, "    { \"%s\", \"%s\", asset_data_%zu, sizeof asset_data_%zu - 1 },\n",
                entries[i].url_path, entries[i].mime, i, i);
    fprintf(f, "};\n\n");
    fprintf(f,
        "const asset_t *assets_all(size_t *count)\n"
        "{\n"
        "    if (count) *count = sizeof asset_table / sizeof asset_table[0];\n"
        "    return asset_table;\n"
        "}\n\n"
        "const asset_t *asset_find(const char *path)\n"
        "{\n"
        "    for (size_t i = 0; i < sizeof asset_table / sizeof asset_table[0]; i++)\n"
        "        if (strcmp(asset_table[i].path, path) == 0)\n"
        "            return &asset_table[i];\n"
        "    return NULL;\n"
        "}\n");
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <out.c> <out.h> <dir>\n", argv[0]);
        return 2;
    }
    scan_dir(argv[3], "");
    if (entry_count == 0)
        die("no assets found under %s", argv[3]);
    qsort(entries, entry_count, sizeof *entries, cmp_url);

    size_t total = 0;
    for (size_t i = 0; i < entry_count; i++) total += entries[i].size;

    FILE *out_h = fopen(argv[2], "w");
    if (!out_h) die("open %s: %s", argv[2], strerror(errno));
    write_header(out_h);
    if (fclose(out_h) != 0) die("write %s", argv[2]);

    FILE *out_c = fopen(argv[1], "w");
    if (!out_c) die("open %s: %s", argv[1], strerror(errno));
    write_source(out_c);
    if (fclose(out_c) != 0) die("write %s", argv[1]);

    printf("embed: %zu asset%s, %zu bytes -> %s, %s\n",
           entry_count, entry_count == 1 ? "" : "s", total, argv[1], argv[2]);
    return 0;
}
