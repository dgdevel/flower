/*
 * selftest — offline checks for the web-tool machinery: the html
 * tokenizer/readability/markdown pipeline, DuckDuckGo result
 * extraction and prompt templating, all against tests/fixtures/.
 * Run by `make check` before the smoke tests.
 */
#include "html.h"
#include "prompts.h"
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
    test_ddg();
    test_markdown();
    test_malformed();
    test_prompts();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall selftest checks passed\n");
    return 0;
}
