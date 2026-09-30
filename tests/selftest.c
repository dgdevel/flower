/*
 * selftest — offline checks for the web-tool machinery: the html
 * tokenizer/readability/markdown pipeline, DuckDuckGo result
 * extraction and prompt templating, all against tests/fixtures/.
 * Run by `make check` before the smoke tests.
 */
#define _DEFAULT_SOURCE /* mkdtemp */

#include "fs.h"
#include "html.h"
#include "prompts.h"
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void check_contains(const char *hay, const char *needle,
                           const char *what)
{
    check(hay && strstr(hay, needle), what);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len = (size_t)n;
    return buf;
}

/* ---------- ddg result extraction ---------- */

static void test_ddg(void)
{
    size_t n = 0;
    char *html = read_file("tests/fixtures/ddg_epoll.html", &n);
    check(html != NULL, "ddg: fixture readable");
    if (!html) return;

    char *recs = web_ddg_records(html, n);
    check(recs != NULL, "ddg: records rendered");

    int urls = 0;
    for (char *p = recs; (p = strstr(p, "Url: ")); p += 5) urls++;
    check(urls == 10, "ddg: ten records");

    check_contains(recs,
        "Url: https://www.man7.org/linux/man-pages/man7/epoll.7.html",
        "ddg: uddg redirect resolved and percent-decoded");
    check_contains(recs,
        "Description: epoll (7) - Linux manual page - man7.org — Learn how",
        "ddg: title/snippet description");

    /* truncation safety: same page, tiny cap via RESULTS_MAX is not
     * configurable here — feed a truncated buffer instead */
    char *cut = web_ddg_records(html, n / 3);
    check(cut != NULL, "ddg: truncated page still parses");
    free(cut);

    free(recs);
    free(html);
}

/* ---------- readability + markdown ---------- */

static void test_markdown(void)
{
    size_t n = 0;
    char *html = read_file("tests/fixtures/article.html", &n);
    check(html != NULL, "md: fixture readable");
    if (!html) return;

    html_doc_t *doc = html_parse(html, n);
    check(doc != NULL && doc->root != NULL, "md: document parsed");
    char *md = html_to_markdown(doc);
    check(md != NULL, "md: markdown rendered");

    check_contains(md, "# How to grow tomatoes", "md: h1 heading");
    check_contains(md, "## Getting started", "md: h2 heading");
    check_contains(md, "[Old Farmer's Almanac](https://www.almanac.com/gardening/tomatoes)",
                   "md: link with href");
    check_contains(md, "- Sow seeds 5 mm deep", "md: bullet (nbsp decoded)");
    check_contains(md, "  - one plant per 9 cm pot", "md: nested bullet");
    check_contains(md, "1. **Blossom end rot**", "md: ordered list + bold");
    check_contains(md, "> Water deeply twice a week", "md: blockquote");
    check_contains(md, "| March | Sow indoors |", "md: table row");
    check_contains(md, "```", "md: fenced code");
    check_contains(md, "90cm between rows", "md: code content");
    check_contains(md, "6\u201310 days", "md: &ndash; entity");
    check_contains(md, "21 \u00b0C", "md: &deg; entity");
    check_contains(md, "*90 days*", "md: emphasis");

    check(!strstr(md, "premium compost"), "md: sidebar dropped");
    check(!strstr(md, "Subscribe for weekly tips"), "md: footer widget dropped");
    check(!strstr(md, "window.tracker"), "md: script dropped");
    check(!strstr(md, "Privacy"), "md: footer dropped");
    check(!strstr(md, "Home | "), "md: nav dropped");

    char *md2 = html_to_markdown(doc);
    check(md2 && strcmp(md, md2) == 0, "md: deterministic");
    free(md2);
    free(md);
    html_doc_free(doc);
    free(html);
}

/* ---------- malformed input ---------- */

static void test_malformed(void)
{
    html_doc_t *doc = html_parse("<p>a<b>c<<dd<<<", 15);
    check(doc != NULL, "bad: unclosed tags parse");
    char *md = html_to_markdown(doc);
    check(md != NULL, "bad: markdown survives");
    check_contains(md, "a", "bad: text kept");
    free(md);
    html_doc_free(doc);

    doc = html_parse("", 0);
    check(doc != NULL, "bad: empty document parses");
    md = html_to_markdown(doc);
    check(md && strcmp(md, "") == 0, "bad: empty markdown");
    free(md);
    html_doc_free(doc);

    /* deeply nested input stays within the depth cap */
    char deep[8192];
    size_t w = 0;
    for (int i = 0; i < 900 && w < sizeof deep - 128; i++)
        w += (size_t)snprintf(deep + w, sizeof deep - w, "<div>");
    snprintf(deep + w, sizeof deep - w, "<p>bottom</p>");
    doc = html_parse(deep, strlen(deep));
    check(doc != NULL && doc->nodes > 0, "bad: deep nesting capped");
    md = html_to_markdown(doc);
    check_contains(md, "bottom", "bad: deep text survives");
    free(md);
    html_doc_free(doc);
}

/* ---------- prompt templating ---------- */

static void test_prompts(void)
{
    char *p = prompt_text("agents/assistant/system_prompt.txt");
    check(p && strcmp(p, "You are a helpful assistant. Answer clearly and concisely.") == 0,
          "prompts: assistant prompt embedded and trimmed");
    free(p);
    check(prompt_text("agents/nope/system_prompt.txt") == NULL,
          "prompts: missing file is NULL");

    project_t proj;
    memset(&proj, 0, sizeof proj);
    strcpy(proj.dir, "/home/coder/flower");
    strcpy(proj.title, "flower");
    strcpy(proj.description, "A single-binary webapp.");
    strcpy(proj.objectives, "Ship part 4\nKeep it small");
    ctx_item_t risk = { .type = CTX_RISK, .text = "DDG markup may change" };
    ctx_item_t fact = { .type = CTX_FACT, .text = "libcurl handles tls" };
    risk.next = NULL;
    fact.next = &risk;
    proj.context = &fact;

    char *out = prompt_render(
        "path={{project_path}} name={{project_name}}\n"
        "{{project_attributes}}\n"
        "{{project_context}}\n"
        "keep {{unknown_var}} and {{ spaced }} as-is", &proj);
    check(out != NULL, "tpl: rendered");
    check_contains(out, "path=/home/coder/flower name=flower", "tpl: path+name");
    check_contains(out, "Description: A single-binary webapp.\n", "tpl: attribute");
    check_contains(out, "Objectives: Ship part 4\n  Keep it small\n",
                   "tpl: multi-line indent");
    check_contains(out, "- [fact] libcurl handles tls\n", "tpl: context item");
    check_contains(out, "- [risk] DDG markup may change\n", "tpl: typed item");
    check_contains(out, "{{unknown_var}}", "tpl: unknown kept verbatim");
    check_contains(out, "{{ spaced }}", "tpl: spaced token kept verbatim");
    check(!strstr(out, "Scope:"), "tpl: empty field omitted");
    free(out);

    /* no project: known vars empty, structure survives */
    out = prompt_render("a={{project_path}}b={{project_name}}c", NULL);
    check(out && strcmp(out, "a=b=c") == 0, "tpl: NULL project empties vars");
    free(out);
}

/* ---------- filesystem tools ---------- */

static void mkfile(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/* call an fs tool dispatcher with a one- or two-string argument
 * object — the per-project surface tests go through the
 * dispatchers, because that is where path resolution lives */
static char *fs_tool_ss(char *(*fn)(const cJSON *, char *, size_t),
                        const char *k1, const char *v1,
                        const char *k2, const char *v2,
                        char *err, size_t err_n)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, k1, v1);
    if (k2) cJSON_AddStringToObject(a, k2, v2);
    char *r = fn(a, err, err_n);
    cJSON_Delete(a);
    return r;
}

static void test_fs(void)
{
    char root[] = "/tmp/flower-fstest-XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    char p[512];
    snprintf(p, sizeof p, "%s/notes.txt", root); mkfile(p, "one\ntwo\nthree\n");
    snprintf(p, sizeof p, "%s/Makefile", root); mkfile(p, "all:\n");
    snprintf(p, sizeof p, "%s/photo.png", root); mkfile(p, "\x89PNG");
    snprintf(p, sizeof p, "%s/binary.bin", root);
    {
        FILE *f = fopen(p, "wb");
        if (f) { fwrite("a\0b", 1, 4, f); fclose(f); }  /* real NUL byte */
    }
    snprintf(p, sizeof p, "%s/src", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/src/main.c", root); mkfile(p, "int main(){}\n");
    snprintf(p, sizeof p, "%s/src/util.js", root); mkfile(p, "x;\n");
    snprintf(p, sizeof p, "%s/src/deep", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/src/deep/edge.c", root); mkfile(p, "deep\n");

    char err[256];
    char *r;

    /* the six glob forms from the spec */
    r = fs_glob(root, "*", err, sizeof err);
    check_contains(r, "Makefile\n", "fs: * lists files");
    check_contains(r, "notes.txt\n", "fs: * lists extensioned files");
    check_contains(r, "photo.png\n", "fs: * lists by extension");
    check_contains(r, "src/\n", "fs: * lists directories");
    check(r && !strstr(r, "main.c"), "fs: * stays in one directory");
    free(r);

    r = fs_glob(root, "*.*", err, sizeof err);
    check_contains(r, "notes.txt\n", "fs: *.* matches an extension");
    check_contains(r, "photo.png\n", "fs: *.* matches any extension");
    check(r && !strstr(r, "Makefile") && !strstr(r, "src/"),
          "fs: *.* requires a dot");
    free(r);

    r = fs_glob(root, "*.png", err, sizeof err);
    check_contains(r, "photo.png\n", "fs: *.png matches one extension");
    check(r && !strstr(r, "notes.txt"), "fs: *.png filters others out");
    free(r);

    r = fs_glob(root, "**/*.c", err, sizeof err);
    check_contains(r, "src/main.c\n", "fs: **/*.c finds nested c files");
    check_contains(r, "src/deep/edge.c\n", "fs: **/*.c walks deep");
    check(r && !strstr(r, "util.js") && !strstr(r, "Makefile"),
          "fs: **/*.c filters other extensions");
    free(r);

    r = fs_glob(root, "**/*.*", err, sizeof err);
    check_contains(r, "src/util.js\n", "fs: **/*.* matches nested ext files");
    check_contains(r, "photo.png\n", "fs: **/*.* includes the top level");
    check(r && !strstr(r, "Makefile") && !strstr(r, "src/deep/\n"),
          "fs: **/*.* skips extensionless entries");
    free(r);

    r = fs_glob(root, "**/*", err, sizeof err);
    check_contains(r, "src/deep/edge.c\n", "fs: **/* lists everything nested");
    check_contains(r, "src/\n", "fs: **/* lists directories too");
    free(r);

    /* errors */
    r = fs_glob("/nonexistent-dir-xyz", "*", err, sizeof err);
    check(!r && err[0], "fs: bad path errors");
    snprintf(p, sizeof p, "%s/notes.txt", root);
    r = fs_glob(p, "*", err, sizeof err);
    check(!r && strstr(err, "not a directory"), "fs: file path errors");
    r = fs_glob(root, "", err, sizeof err);
    check(!r && err[0], "fs: empty glob errors");

    /* grep */
    snprintf(p, sizeof p, "%s/**/*.c", root);
    r = fs_grep(p, "main", err, sizeof err);
    check_contains(r, "src/main.c:1:int main(){}",
                   "fs: grep matches file content");
    check(r && !strstr(r, "util.js") && !strstr(r, "edge.c"),
          "fs: grep stays inside the glob");
    check_contains(r, "[1 match in 1 of 2 files]", "fs: grep tally");
    free(r);

    r = fs_grep(root, "^deep$", err, sizeof err); /* bare dir walks */
    check_contains(r, "src/deep/edge.c:1:deep",
                   "fs: grep bare directory walks subdirectories");
    free(r);

    snprintf(p, sizeof p, "%s/notes.txt", root);
    r = fs_grep(p, "two", err, sizeof err);
    check_contains(r, "notes.txt:2:two", "fs: grep a single file");
    check_contains(r, "[1 match in 1 of 1 file]", "fs: grep single tally");
    free(r);

    snprintf(p, sizeof p, "%s/*.c", root);
    r = fs_grep(p, "x", err, sizeof err);
    check(r && strncmp(r, "no files match", 14) == 0,
          "fs: grep reports a glob that selects nothing");
    free(r);

    r = fs_grep(root, "b", err, sizeof err); /* only binary.bin has one */
    check_contains(r, "[0 matches in 0 of 7 files, 1 skipped",
                   "fs: grep skips binary files");
    free(r);

    r = fs_grep("/nonexistent-dir-xyz/*", "x", err, sizeof err);
    check(!r && err[0], "fs: grep bad path errors");
    r = fs_grep(p, "(open", err, sizeof err);
    check(!r && strstr(err, "invalid pattern"), "fs: grep bad regex errors");
    snprintf(p, sizeof p, "%s/*/*.c", root);
    r = fs_grep(p, "x", err, sizeof err);
    check(!r && strstr(err, "last path component"),
          "fs: grep rejects a multi-component glob");

    /* the caps: 300 hits stop at 200, an overlong line is cut */
    snprintf(p, sizeof p, "%s/cap.txt", root);
    {
        FILE *f = fopen(p, "w");
        if (!f) { perror(p); exit(2); }
        for (int i = 0; i < 300; i++) fputs("hit\n", f);
        fclose(f);
    }
    r = fs_grep(root, "hit", err, sizeof err);
    check_contains(r, "cap.txt:1:hit", "fs: grep matches before the cap");
    check_contains(r, "[stopped after 200 matches]", "fs: grep match cap");
    free(r);

    snprintf(p, sizeof p, "%s/long.txt", root);
    {
        FILE *f = fopen(p, "w");
        if (!f) { perror(p); exit(2); }
        fputs("needle", f);
        for (int i = 0; i < 500; i++) putc('x', f);
        fputs("\n", f);
        fclose(f);
    }
    r = fs_grep(root, "needle", err, sizeof err);
    check_contains(r, "long.txt:1:needle", "fs: grep long line matched");
    check_contains(r, "\xE2\x80\xA6", "fs: grep long line cut with …");
    free(r);

    /* read_file */
    snprintf(p, sizeof p, "%s/notes.txt", root);
    r = fs_read_path(p, 0, 64, err, sizeof err);
    check(r && strcmp(r, "one\ntwo\nthree\n") == 0, "fs: read whole file");
    free(r);
    r = fs_read_path(p, 4, 4, err, sizeof err);
    check(r && strcmp(r, "two\n") == 0, "fs: read with offset+length");
    free(r);
    r = fs_read_path(p, 4, 1000000, err, sizeof err);
    check(r && strcmp(r, "two\nthree\n") == 0, "fs: length clamped to rest");
    free(r);
    r = fs_read_path(p, 100, 4, err, sizeof err);
    check(!r && strstr(err, "past the end"), "fs: offset past eof errors");
    snprintf(p, sizeof p, "%s/binary.bin", root);
    r = fs_read_path(p, 0, 8, err, sizeof err);
    check(!r && strstr(err, "binary"), "fs: NUL byte detected as binary");
    snprintf(p, sizeof p, "%s/src", root);
    r = fs_read_path(p, 0, 8, err, sizeof err);
    check(!r && strstr(err, "directory"), "fs: directory read refused");
    r = fs_read_path("/nonexistent-file-xyz", 0, 8, err, sizeof err);
    check(!r && err[0], "fs: missing file errors");

    /* utf-8 slices stay whole */
    snprintf(p, sizeof p, "%s/utf8.txt", root);
    mkfile(p, "a\xc3\xa9" "bcdef ghij \xe2\x82\xac""end");
    r = fs_read_path(p, 0, 4, err, sizeof err); /* cuts inside é */
    check(r && (unsigned char)r[strlen(r) - 1] != 0xa9,
          "fs: partial utf-8 trimmed at the end");
    free(r);
    r = fs_read_path(p, 1, 4, err, sizeof err); /* starts inside é */
    check(r && (unsigned char)r[0] != 0xa9,
          "fs: partial utf-8 skipped at the start");
    free(r);

    /* the per-project surface: relative paths under a root, a jail,
     * project-relative replies (the mcp dispatchers resolve) */
    fs_set_root(root);
    check(fs_rooted(), "fs: root set");

    r = fs_tool_ss(fs_tool_read_file, "path", "notes.txt",
                   NULL, NULL, err, sizeof err);
    check(r && strcmp(r, "one\ntwo\nthree\n") == 0,
          "fs: rooted read takes relative paths");
    free(r);
    r = fs_tool_ss(fs_tool_read_file, "path", "./src/main.c",
                   NULL, NULL, err, sizeof err);
    check(r && strcmp(r, "int main(){}\n") == 0,
          "fs: rooted read accepts a ./ prefix");
    free(r);
    r = fs_tool_ss(fs_tool_list_files, "path", ".", "glob", "**/*.c",
                   err, sizeof err);
    check_contains(r, "src/main.c\n", "fs: rooted listing is project-relative");
    check(r && !strstr(r, root), "fs: rooted listing hides the root path");
    free(r);
    r = fs_tool_ss(fs_tool_grep, "glob", "**/*.c", "pattern", "main",
                   err, sizeof err);
    check_contains(r, "src/main.c:1:int main(){}",
                   "fs: rooted grep is project-relative");
    check(r && !strstr(r, root), "fs: rooted grep hides the root path");
    free(r);
    r = fs_tool_ss(fs_tool_grep, "glob", ".", "pattern", "deep",
                   err, sizeof err);
    check_contains(r, "src/deep/edge.c:1:deep",
                   "fs: rooted grep takes . as the project root");
    free(r);

    r = fs_tool_ss(fs_tool_read_file, "path", "/etc/hostname",
                   NULL, NULL, err, sizeof err);
    check(!r && strstr(err, "relative to the project"),
          "fs: rooted surface refuses absolute paths");
    r = fs_tool_ss(fs_tool_read_file, "path", "../escape",
                   NULL, NULL, err, sizeof err);
    check(!r && strstr(err, "escapes the project directory"),
          "fs: rooted surface refuses .. escapes");
    r = fs_tool_ss(fs_tool_read_file, "path", "src/../notes.txt",
                   NULL, NULL, err, sizeof err);
    check(r && strcmp(r, "one\ntwo\nthree\n") == 0,
          "fs: rooted .. inside the project is fine");
    free(r);
    r = fs_tool_ss(fs_tool_read_file, "path", "missing.txt",
                   NULL, NULL, err, sizeof err);
    check(!r && strstr(err, "missing.txt") && !strstr(err, root),
          "fs: rooted errors stay project-relative");

    fs_set_root(NULL);
    check(!fs_rooted(), "fs: root cleared");
    r = fs_tool_ss(fs_tool_read_file, "path", "notes.txt",
                   NULL, NULL, err, sizeof err);
    check(!r && strstr(err, "absolute"),
          "fs: bare surface demands absolute paths");
    snprintf(p, sizeof p, "%s/notes.txt", root);
    r = fs_tool_ss(fs_tool_read_file, "path", p,
                   NULL, NULL, err, sizeof err);
    check(r && strcmp(r, "one\ntwo\nthree\n") == 0,
          "fs: bare surface keeps absolute paths");
    free(r);

    /* cleanup */
    snprintf(p, sizeof p, "%s/src/deep/edge.c", root); remove(p);
    snprintf(p, sizeof p, "%s/src/deep", root); rmdir(p);
    snprintf(p, sizeof p, "%s/src/main.c", root); remove(p);
    snprintf(p, sizeof p, "%s/src/util.js", root); remove(p);
    snprintf(p, sizeof p, "%s/utf8.txt", root); remove(p);
    snprintf(p, sizeof p, "%s/src", root); rmdir(p);
    snprintf(p, sizeof p, "%s/notes.txt", root); remove(p);
    snprintf(p, sizeof p, "%s/Makefile", root); remove(p);
    snprintf(p, sizeof p, "%s/photo.png", root); remove(p);
    snprintf(p, sizeof p, "%s/binary.bin", root); remove(p);
    snprintf(p, sizeof p, "%s/cap.txt", root); remove(p);
    snprintf(p, sizeof p, "%s/long.txt", root); remove(p);
    rmdir(root);
}

int main(void)
{
    test_ddg();
    test_markdown();
    test_malformed();
    test_prompts();
    test_fs();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall selftest checks passed\n");
    return 0;
}
