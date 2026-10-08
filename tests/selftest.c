/*
 * selftest — offline checks for the fs tools and prompt templating
 * (the web-tool pipeline moved into llmkit's builtin-mcp). Run by
 * `make check` before the smoke tests.
 */
#define _DEFAULT_SOURCE /* mkdtemp */

#include "fs.h"
#include "analyze.h"
#include "prompts.h"
#include "tasks.h"
#include "theme.h"

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
    ctx_item_t fact = { .type = CTX_FACT, .text = "cJSON parses json" };
    ctx_item_t res = { .type = CTX_RESOURCE, .text = "the project schema" };
    strcpy(res.spec, "docs/schema.md");
    res.next = NULL;
    risk.next = &res;
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
    check_contains(out, "- [fact] cJSON parses json\n", "tpl: context item");
    check_contains(out, "- [risk] DDG markup may change\n", "tpl: typed item");
    check_contains(out, "- [resource] docs/schema.md — the project schema\n",
                   "tpl: resource item leads with its spec");
    check_contains(out, "{{unknown_var}}", "tpl: unknown kept verbatim");
    check_contains(out, "{{ spaced }}", "tpl: spaced token kept verbatim");
    check(!strstr(out, "Scope:"), "tpl: empty field omitted");
    free(out);

    /* no project: known vars empty, structure survives */
    out = prompt_render("a={{project_path}}b={{project_name}}c", NULL);
    check(out && strcmp(out, "a=b=c") == 0, "tpl: NULL project empties vars");
    free(out);

    /* caller-supplied variables (the scan follow-up's {{request}} and
     * {{previous_run}}): consulted after the fixed set, made of plain
     * text, never re-scanned for tokens */
    prompt_var_t vars[] = {
        { "request", "find the tests {{project_name}}" },
        { "previous_run", NULL },
        { "project_name", "shadowed" },
    };
    out = prompt_render_vars(
        "req={{request}} prev=[{{previous_run}}] "
        "name={{project_name}} keep={{spaced }}", &proj,
        vars, sizeof vars / sizeof vars[0]);
    check_contains(out, "req=find the tests {{project_name}}",
                   "tpl: extra var value verbatim");
    check_contains(out, "prev=[]", "tpl: NULL extra var renders empty");
    check_contains(out, "name=flower", "tpl: fixed set wins over extras");
    check_contains(out, "{{spaced }}", "tpl: unknown stays after extras");
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

    /* read_file: offset is the 1-based start line, length the
     * number of lines */
    snprintf(p, sizeof p, "%s/notes.txt", root);
    r = fs_read_path(p, 1, 64, err, sizeof err);
    check(r && strcmp(r, "one\ntwo\nthree\n") == 0, "fs: read whole file");
    free(r);
    r = fs_read_path(p, 2, 1, err, sizeof err);
    check(r && strcmp(r, "two\n") == 0, "fs: read with offset+length");
    free(r);
    r = fs_read_path(p, 2, 1000000, err, sizeof err);
    check(r && strcmp(r, "two\nthree\n") == 0, "fs: length clamped to rest");
    free(r);
    r = fs_read_path(p, 100, 4, err, sizeof err);
    check(!r && strstr(err, "past the end"), "fs: offset past eof errors");
    r = fs_read_path(p, 0, 4, err, sizeof err);
    check(!r && strstr(err, "1 or more"), "fs: offset 0 rejected");
    snprintf(p, sizeof p, "%s/binary.bin", root);
    r = fs_read_path(p, 1, 8, err, sizeof err);
    check(!r && strstr(err, "binary"), "fs: NUL byte detected as binary");
    snprintf(p, sizeof p, "%s/src", root);
    r = fs_read_path(p, 1, 8, err, sizeof err);
    check(!r && strstr(err, "directory"), "fs: directory read refused");
    r = fs_read_path("/nonexistent-file-xyz", 1, 8, err, sizeof err);
    check(!r && err[0], "fs: missing file errors");

    /* whole lines keep multibyte characters intact */
    snprintf(p, sizeof p, "%s/utf8.txt", root);
    mkfile(p, "a\xc3\xa9" "bcdef ghij \xe2\x82\xac""end\nnext\n");
    r = fs_read_path(p, 1, 1, err, sizeof err);
    check(r && strstr(r, "\xc3\xa9") && strstr(r, "\xe2\x82\xac"),
          "fs: multibyte characters survive whole");
    free(r);

    /* the byte budget cuts an oversized line on a utf-8 boundary */
    snprintf(p, sizeof p, "%s/huge.txt", root);
    {
        enum { BUDGET = 64 * 1024, PAD = BUDGET - 1 };
        char *big = malloc(PAD + 8);
        if (!big) exit(2);
        memset(big, 'x', PAD);
        memcpy(big + PAD, "\xc3\xa9" "tail\n", 6); /* é straddles the cut */
        FILE *hf = fopen(p, "wb");
        if (!hf) { perror(p); exit(2); }
        fwrite(big, 1, PAD + 6, hf);
        fclose(hf);
        free(big);
        r = fs_read_path(p, 1, 10, err, sizeof err);
        check(r && (unsigned char)r[PAD - 1] == 'x' && r[PAD] == '[',
              "fs: byte-budget cut keeps utf-8 whole");
        check_contains(r, "[truncated]\n", "fs: byte-budget cut is marked");
        free(r);
    }

    /* analyze: the structure of a known-type file */
    snprintf(p, sizeof p, "%s/doc.md", root);
    mkfile(p, "# Title\n## A\n```sh\n# no heading in fences\n```\n"
               "### B ###\nplain\n");
    r = analyze_file(p, err, sizeof err);
    check_contains(r, "  [markdown]  7 lines\n", "analyze: markdown header");
    check_contains(r, "    1  Title\n", "analyze: h1 node");
    check_contains(r, "    2    A\n", "analyze: h2 nests under h1");
    check_contains(r, "    6      B\n", "analyze: trailing #'s stripped");
    check(r && !strstr(r, "fences"), "analyze: fenced content skipped");
    free(r);

    snprintf(p, sizeof p, "%s/prog.py", root);
    mkfile(p, "class Foo:\n"
              "    def __init__(self):\n"
              "        pass\n"
              "\n"
              "    async def run(self):\n"
              "        pass\n"
              "\n"
              "def top():\n"
              "    pass\n"
              "\n"
              "class Sub(Foo):\n"
              "    def helper(self, a=1):\n"
              "        return a\n");
    r = analyze_file(p, err, sizeof err);
    check_contains(r, "  [python]  13 lines\n", "analyze: python header");
    check_contains(r, "    1  class Foo\n", "analyze: python class");
    check_contains(r, "    2    def __init__(self)\n",
                   "analyze: python method signature nests");
    check_contains(r, "    5    async def run(self)\n",
                   "analyze: python async def signature");
    check_contains(r, "    8  def top()\n",
                   "analyze: dedent back to the top level");
    check_contains(r, "   11  class Sub(Foo)\n",
                   "analyze: python class bases");
    check_contains(r, "   12    def helper(self, a=1)\n",
                   "analyze: python default parameter");
    check(r && !strstr(r, "pass") && !strstr(r, "return"),
          "analyze: plain lines are not nodes");
    free(r);

    snprintf(p, sizeof p, "%s/code.c", root);
    mkfile(p, "/* struct fake { */\n"
              "typedef unsigned long u64;\n"
              "struct point {\n"
              "    int x; /* member */\n"
              "    char *name;\n"
              "    struct point *next;\n"
              "    int axes[3];\n"
              "    unsigned bits : 4;\n"
              "};\n"
              "static int helper(int a) { return a; }\n"
              "char *s = \"struct fake { int x; }\";\n");
    r = analyze_file(p, err, sizeof err);
    check_contains(r, "  [c]  11 lines\n", "analyze: c header");
    check_contains(r, "    2  typedef unsigned long u64\n",
                   "analyze: typedef keeps its type");
    check_contains(r, "    3  struct point\n", "analyze: c struct");
    check_contains(r, "    4    int x\n", "analyze: struct member nests");
    check_contains(r, "    5    char *name\n",
                   "analyze: member keeps its type");
    check_contains(r, "    6    struct point *next\n",
                   "analyze: self-referential member type");
    check_contains(r, "    7    int axes[3]\n",
                   "analyze: array member keeps its size");
    check_contains(r, "    8    unsigned bits : 4\n",
                   "analyze: bitfield member keeps its width");
    check_contains(r, "   10  static int helper(int a)\n",
                   "analyze: c function signature");
    check(r && !strstr(r, "fake"), "analyze: comments and strings skipped");
    free(r);

    snprintf(p, sizeof p, "%s/web.js", root);
    mkfile(p, "function greet(name) {\n"
              "  return el(\"div\",\n"
              "    el(\"b\", { text: name }),\n"
              "    input,\n"
              "    el(\"i\", { text: \"hi\" }));\n"
              "}\n");
    r = analyze_file(p, err, sizeof err);
    check_contains(r, "  [c]  6 lines\n", "analyze: js header");
    check_contains(r, "    1  function greet(name)\n",
                   "analyze: js function signature");
    check_contains(r, "    3  el()\n",
                   "analyze: js call artifact keeps the bare name");
    free(r);

    snprintf(p, sizeof p, "%s/empty.md", root);
    mkfile(p, "");
    r = analyze_file(p, err, sizeof err);
    check_contains(r, "(nothing found)", "analyze: no nodes reported");
    free(r);

    /* refusals */
    snprintf(p, sizeof p, "%s/notes.txt", root);
    r = analyze_file(p, err, sizeof err);
    check(!r && strstr(err, "unknown file type"),
          "analyze: unknown type reported");
    check(!r && strstr(err, "markdown") && strstr(err, ".py"),
          "analyze: unknown type lists the known ones");
    snprintf(p, sizeof p, "%s/null.c", root);
    {
        FILE *f = fopen(p, "wb");
        if (f) { fwrite("int x;\0y\n", 1, 9, f); fclose(f); }
    }
    r = analyze_file(p, err, sizeof err);
    check(!r && strstr(err, "binary"), "analyze: binary content refused");
    r = analyze_file(root, err, sizeof err);
    check(!r && strstr(err, "directory"), "analyze: directory refused");
    r = analyze_file("/nonexistent-file-xyz.c", err, sizeof err);
    check(!r && err[0], "analyze: missing file errors");

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

    r = fs_tool_ss(fs_tool_analyze, "path", "src/main.c",
                   NULL, NULL, err, sizeof err);
    check_contains(r, "src/main.c  [c]  1 lines",
                   "fs: rooted analyze is project-relative");
    check_contains(r, "int main()",
                   "fs: rooted analyze shows the signature");
    check(r && !strstr(r, root), "fs: rooted analyze hides the root path");
    free(r);
    r = fs_tool_ss(fs_tool_analyze, "path", "notes.txt",
                   NULL, NULL, err, sizeof err);
    check(!r && strstr(err, "unknown file type") && !strstr(err, root),
          "fs: rooted analyze unknown type stays project-relative");

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
    snprintf(p, sizeof p, "%s/doc.md", root); remove(p);
    snprintf(p, sizeof p, "%s/prog.py", root); remove(p);
    snprintf(p, sizeof p, "%s/code.c", root); remove(p);
    snprintf(p, sizeof p, "%s/web.js", root); remove(p);
    snprintf(p, sizeof p, "%s/null.c", root); remove(p);
    snprintf(p, sizeof p, "%s/empty.md", root); remove(p);
    snprintf(p, sizeof p, "%s/Makefile", root); remove(p);
    snprintf(p, sizeof p, "%s/photo.png", root); remove(p);
    snprintf(p, sizeof p, "%s/binary.bin", root); remove(p);
    snprintf(p, sizeof p, "%s/cap.txt", root); remove(p);
    snprintf(p, sizeof p, "%s/long.txt", root); remove(p);
    rmdir(root);
}

/* ---------- action dependencies ---------- */

/* the strict/lenient contract for depends_on: links round-trip,
 * bad ones reject a PUT, and the lenient loader repairs a file
 * another writer could have left behind */
static void test_task_deps(void)
{
    tasks_t t;
    char ef[256], em[256];

    /* strict: good links parse, store and re-emit in order */
    const char *good =
        "[{\"project\":\"9f2c4a1b0d7e6358\",\"title\":\"t\",\"actions\":["
        "{\"title\":\"a\"},"
        "{\"title\":\"b\",\"depends_on\":[\"0\"]},"
        "{\"title\":\"c\",\"depends_on\":[\"0\",\"1\"]}]}]";
    check(tasks_from_json(good, strlen(good), &t,
                          ef, sizeof ef, em, sizeof em) == TASKS_OK,
          "deps: strict parse accepts links");
    check(t.count == 1 && t.items[0].actions &&
          t.items[0].actions->next_sibling->dep_count == 1 &&
          strcmp(t.items[0].actions->next_sibling->depends_on[0], "0") == 0,
          "deps: the link is stored");
    char *j = tasks_to_json(&t, 0, NULL);
    check_contains(j, "\"depends_on\":[\"0\",\"1\"]",
                   "deps: emitted on the wire, in order");
    int n = 0; /* the two linked actions emit it, the plain one doesn't */
    for (const char *p = j; (p = strstr(p, "\"depends_on\"")); p += 12) n++;
    check(n == 2, "deps: only when set");
    free(j);
    tasks_clear(&t);

    /* strict: every way a link can be wrong */
    static const struct { const char *body; const char *what; } bad[] = {
        { "[{\"project\":\"9f2c4a1b0d7e6358\",\"actions\":["
          "{\"title\":\"a\"},{\"title\":\"b\",\"depends_on\":[\"7\"]}]}]",
          "deps: dangling link rejected" },
        { "[{\"project\":\"9f2c4a1b0d7e6358\",\"actions\":["
          "{\"title\":\"a\",\"depends_on\":[\"0\"]}]}]",
          "deps: self link rejected" },
        { "[{\"project\":\"9f2c4a1b0d7e6358\",\"actions\":["
          "{\"title\":\"a\",\"depends_on\":[\"1\"]},"
          "{\"title\":\"b\",\"depends_on\":[\"0\"]}]}]",
          "deps: a two-action circle rejected" },
        { "[{\"project\":\"9f2c4a1b0d7e6358\",\"actions\":["
          "{\"title\":\"a\"},{\"title\":\"b\",\"depends_on\":[\"0.x\"]}]}]",
          "deps: malformed path rejected" },
        { "[{\"project\":\"9f2c4a1b0d7e6358\",\"actions\":["
          "{\"title\":\"a\"},{\"title\":\"b\",\"depends_on\":[\"0\",\"0\"]}]}]",
          "deps: duplicate link rejected" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        check(tasks_from_json(bad[i].body, strlen(bad[i].body), &t,
                              ef, sizeof ef, em, sizeof em) == TASKS_E_FIELD,
              bad[i].what);
        tasks_clear(&t);
    }

    /* lenient load: a self link and a dangling one drop, a circle
     * breaks at its first edge — the file stays usable */
    char dir[] = "/tmp/flower-deptest-XXXXXX";
    check(mkdtemp(dir) != NULL, "deps: temp config dir");
    check(theme_init(dir) == 0, "deps: config dir resolved");
    char tdir[256], idir[320], path[384];
    snprintf(tdir, sizeof tdir, "%s/tasks", dir);
    snprintf(idir, sizeof idir, "%s/00000000000000000000000000000000", tdir);
    mkdir(tdir, 0700);
    mkdir(idir, 0700);
    snprintf(path, sizeof path, "%s/task.json", idir);
    mkfile(path,
           "{\"project\":\"9f2c4a1b0d7e6358\",\"title\":\"t\",\"actions\":["
           "{\"title\":\"a\",\"depends_on\":[\"0\"]},"
           "{\"title\":\"b\",\"depends_on\":[\"3\"]},"
           "{\"title\":\"c\",\"depends_on\":[\"3\"]},"
           "{\"title\":\"d\",\"depends_on\":[\"2\"]}]}]");
    check(tasks_load(&t) == 0 && t.count == 1,
          "deps: lenient load keeps the task");
    const action_t *a = t.items[0].actions;
    check(a && a->dep_count == 0, "deps: self link dropped");
    check(a && a->next_sibling && a->next_sibling->dep_count == 0,
          "deps: dangling link dropped");
    check(a && a->next_sibling && a->next_sibling->next_sibling &&
          a->next_sibling->next_sibling->dep_count == 0,
          "deps: the circle broke at its first edge");
    check(a && a->next_sibling && a->next_sibling->next_sibling &&
          a->next_sibling->next_sibling->next_sibling &&
          a->next_sibling->next_sibling->next_sibling->dep_count == 1 &&
          strcmp(a->next_sibling->next_sibling->next_sibling->depends_on[0],
                 "2") == 0,
          "deps: the edge outside the circle stays");
    tasks_clear(&t);
    remove(path);
    rmdir(idir);
    rmdir(tdir);
    rmdir(dir);
}

int main(void)
{
    test_prompts();
    test_fs();
    test_task_deps();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall selftest checks passed\n");
    return 0;
}
