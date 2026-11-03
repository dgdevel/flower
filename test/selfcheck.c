/* selfcheck.c - self-contained test harness, no framework. exits 1 on
   any failure so make check can gate on it. */
/* mkdtemp/setenv/unsetenv are posix, hidden by -std=c11 without this */
#define _POSIX_C_SOURCE 200809L
#include "flower.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run, tests_failed;

/* one check = one line: prints ok/fail and counts both */
static void check(int cond, const char *name) {
    tests_run++;
    if (cond) {
        printf("  [ ok ] %s\n", name);
    } else {
        tests_failed++;
        printf("  [FAIL] %s\n", name);
    }
}

static void check_int(int got, int want, const char *name) {
    tests_run++;
    if (got == want) {
        printf("  [ ok ] %s\n", name);
    } else {
        tests_failed++;
        printf("  [FAIL] %s: got %d want %d\n", name, got, want);
    }
}

static void check_str(const char *got, const char *want, const char *name) {
    tests_run++;
    if (got && want && !strcmp(got, want)) {
        printf("  [ ok ] %s\n", name);
    } else {
        tests_failed++;
        printf("  [FAIL] %s: got '%s' want '%s'\n",
               name, got ? got : "(null)", want ? want : "(null)");
    }
}

static void test_buf(void) {
    buf_t b;
    char big[65536];
    memset(big, 'x', sizeof big);
    printf("buf:\n");
    buf_init(&b);
    check_int((int)b.len, 0, "buf_init len 0");
    check(b.data == NULL, "buf_init data null");
    check_int(buf_append(&b, "hello", 5), 0, "buf_append hello");
    check_int(buf_append(&b, " world", 6), 0, "buf_append world");
    check_str(b.data, "hello world", "buf_append content");
    check_int((int)b.len, 11, "buf_append len");
    check_int(buf_appendf(&b, "%d-%s", 42, "x"), 0, "buf_appendf");
    check_str(b.data, "hello world42-x", "buf_appendf content");
    buf_reset(&b);
    check_int((int)b.len, 0, "buf_reset len 0");
    check(b.data && b.data[0] == '\0', "buf_reset nul");
    /* big append crosses the doubling path without oom */
    check_int(buf_append(&b, big, sizeof big), 0, "buf_append 64k");
    check_int((int)b.len, (int)sizeof big, "buf_append 64k len");
    check(b.data[b.len] == '\0', "nul kept after append");
    buf_free(&b);
    check(b.data == NULL, "buf_free nulls data");
}

static void test_strutil(void) {
    char t1[] = "  hi  ";
    char t2[] = "";
    char t3[] = "\t\n mid \r\n";
    printf("strutil:\n");
    check_str(str_trim(t1), "hi", "str_trim spaces");
    check_str(str_trim(t2), "", "str_trim empty");
    check_str(str_trim(t3), "mid", "str_trim mixed space");
    check_int(str_starts("hello", "hel"), 1, "str_starts yes");
    check_int(str_starts("he", "hello"), 0, "str_starts short");
    check_int(str_starts("hello", ""), 1, "str_starts empty pfx");
}

static void test_cli(void) {
    struct cli_opt opts[] = {{"verbose", 0}, {"name", 1}};
    struct cli_args out;
    char *a1[] = {"prog", "--verbose", "--name", "val", "pos",
                  "--name=other", NULL};
    char *a2[] = {"prog", "--help", NULL};
    char *a3[] = {"prog", "--nope", NULL};
    char *a4[] = {"prog", "--name", NULL};
    char *a5[] = {"prog", "a", "--", "--notopt", "b", NULL};
    printf("cli:\n");
    check_int(cli_parse(6, a1, opts, 2, &out), 0, "cli_parse ok");
    check_int(out.flags & 1, 0, "help bit clear");
    check_str(out.name_value, "other", "name_value last wins");
    check_int(out.npos, 1, "npos 1");
    check_str(out.pos[0], "pos", "pos[0]");
    free((void *)out.pos);
    check_int(cli_parse(2, a2, opts, 2, &out), 0, "cli_parse --help");
    check_int(out.flags & 1, 1, "help bit set");
    free((void *)out.pos);
    check_int(cli_parse(2, a3, opts, 2, &out), -1, "unknown option -1");
    check_int(cli_parse(2, a4, opts, 2, &out), -2, "missing value -2");
    check_int(cli_parse(5, a5, opts, 2, &out), 0, "cli_parse -- marker");
    check_int(out.npos, 3, "post-marker positionals");
    check_str(out.pos[1], "--notopt", "-- becomes positional");
    free((void *)out.pos);
}

static void test_semver(void) {
    flower_semver_t v;
    printf("update_parse_ver:\n");
    v = update_parse_ver("v1.2.3");
    check_int(v.ok, 1, "v1.2.3 ok");
    check(v.ok && v.maj == 1 && v.min == 2 && v.pat == 3, "v1.2.3 fields");
    v = update_parse_ver("1.2");
    check(v.ok && v.maj == 1 && v.min == 2 && v.pat == 0, "1.2 -> 1.2.0");
    v = update_parse_ver("v3");
    check(v.ok && v.maj == 3 && v.min == 0 && v.pat == 0, "v3 -> 3.0.0");
    v = update_parse_ver("x9");
    check_int(v.ok, 0, "x9 junk");
    v = update_parse_ver("");
    check_int(v.ok, 0, "empty junk");
    v = update_parse_ver("1.2.x");
    check_int(v.ok, 0, "1.2.x junk");
}

static void test_md(void) {
    /* sample from docs/config_markdown_format.md */
    static const char *sample =
        "# Chapter 1\n"
        "Key1: value1\n"
        "\n"
        "## Subchapter 1.1\n"
        "Bare value 1.1\n"
        "\n"
        "## Subchapter 1.2\n"
        "Key2: value2\n";
    static const char *serialized =
        "# Chapter 1\n"
        "Key1: changed\n"
        "Key3: value3\n"
        "\n"
        "## Subchapter 1.1\n"
        "Bare value 1.1\n"
        "\n"
        "## Subchapter 1.2\n"
        "Key2: value2\n";
    md_doc_t doc, doc2, empty;
    md_chapter_t *c;
    buf_t b, b2;

    printf("md:\n");
    check_int(md_parse(sample, &doc), 0, "md_parse sample ok");
    check_int(doc.nch, 3, "md_parse 3 chapters");

    c = md_find(&doc, "Chapter 1");
    check(c != NULL, "md_find Chapter 1");
    check(c && c->level == 1, "chapter 1 level 1");
    check(c && c->nkv == 1 && c->nbody == 0, "chapter 1 is kv");
    check_str(c ? md_kv_get((const char **)c->keys, (const char **)c->vals,
                            c->nkv, "Key1")
                : NULL,
              "value1", "md_kv_get Key1");

    c = md_find(&doc, "Subchapter 1.1");
    check(c != NULL, "md_find Subchapter 1.1");
    check(c && c->level == 2, "subchapter 1.1 level 2");
    check(c && c->nkv == 0 && c->nbody == 1, "subchapter 1.1 is bare");
    check_str(c && c->nbody ? c->body[0].text : NULL, "Bare value 1.1",
              "subchapter 1.1 body");

    c = md_find(&doc, "Subchapter 1.2");
    check(c && c->nkv == 1 && c->nbody == 0, "subchapter 1.2 is kv");
    check_str(c ? md_kv_get((const char **)c->keys, (const char **)c->vals,
                            c->nkv, "Key2")
                : NULL,
              "value2", "md_kv_get Key2");
    check(md_find(&doc, "Nope") == NULL, "md_find missing null");

    /* add + overwrite, key order kept */
    c = md_find(&doc, "Chapter 1");
    check_int(md_kv_set(c, "Key1", "changed"), 0, "md_kv_set overwrite");
    check_int(md_kv_set(c, "Key3", "value3"), 0, "md_kv_set append");
    check(c && c->nkv == 2, "md_kv_set nkv 2");
    check_str(c && c->nkv == 2 ? c->keys[0] : NULL, "Key1",
              "md_kv_set order key0");
    check_str(c && c->nkv == 2 ? c->vals[0] : NULL, "changed",
              "md_kv_set order val0");

    buf_init(&b);
    check_int(md_serialize(&doc, &b), 0, "md_serialize ok");
    check_str(b.data, serialized, "md_serialize format");
    check_int(md_parse(b.data, &doc2), 0, "md_parse round trip ok");
    buf_init(&b2);
    check_int(md_serialize(&doc2, &b2), 0, "md_serialize round trip");
    check(b2.data && b.data && !strcmp(b2.data, b.data),
          "round trip identical");
    buf_free(&b2);
    buf_free(&b);
    md_free_doc(&doc2);
    md_free_doc(&doc);
    check(doc.nch == 0 && doc.ch == NULL, "md_free_doc zeroed");

    /* errors */
    check_int(md_parse("# A\n## B\n# A\n", &doc), -2, "dup chapter -2");
    check(doc.nch == 0, "doc zeroed on -2");
    check_int(md_parse("# A\n### C\n", &doc), -3, "level jump -3");
    check_int(md_parse("  # indented\n", &doc), -3, "indented header -3");
    check_int(md_parse("## orphan\n", &doc), -3, "first header h2 -3");
    check_int(md_parse("# A\n## A\n", &doc), 0, "same title other level ok");
    md_free_doc(&doc);
    check_int(md_parse("text before any header\n# A\nv\n", &doc), 0,
              "preamble skipped");
    check(doc.nch == 1 && doc.ch[0].nbody == 1, "chapter body only");
    md_free_doc(&doc);

    /* empty doc serializes to empty buffer */
    memset(&empty, 0, sizeof empty);
    buf_init(&b);
    check_int(md_serialize(&empty, &b), 0, "md_serialize empty ok");
    check(b.len == 0, "empty doc empty buffer");
    buf_free(&b);

    /* file load/save */
    check_int(md_parse(sample, &doc), 0, "reparse sample");
    check_int(md_save("test/md-selfcheck.tmp", &doc), 0, "md_save ok");
    check_int(md_load("test/md-selfcheck.tmp", &doc2), 0, "md_load ok");
    check(doc2.nch == doc.nch, "md_load chapter count");
    /* md_kv_set on a bare chapter converts it to kv and drops the body */
    c = md_find(&doc2, "Subchapter 1.1");
    check_int(md_kv_set(c, "Key", "val"), 0, "md_kv_set on body chapter");
    check(c && c->nkv == 1 && c->nbody == 0, "body chapter now kv");
    md_free_doc(&doc2);
    md_free_doc(&doc);
    check_int(md_load("test/md-no-such.md", &doc), 1, "md_load missing 1");
    check(doc.nch == 0 && doc.ch == NULL, "md_load missing zeroed");
    check_int(md_save("/no-such-dir/x.md", &doc), -1, "md_save io -1");
    remove("test/md-selfcheck.tmp");
}

static void test_store(void) {
    char *p, *pre, *tit, **ids;
    char tmpl[] = "/tmp/flower-store-test-XXXXXX";
    char *tmp;
    int n, i;
    md_doc_t doc;
    const char *base;

    printf("store:\n");

    /* store_valid_id matrix */
    check_int(store_valid_id("P1", 'P'), 1, "valid_id P1 ok");
    check_int(store_valid_id("p1", 'P'), 0, "valid_id p1 bad");
    check_int(store_valid_id("P0", 'P'), 0, "valid_id P0 bad");
    check_int(store_valid_id("P", 'P'), 0, "valid_id P bad");
    check_int(store_valid_id("P1x", 'P'), 0, "valid_id P1x bad");
    check_int(store_valid_id("P1", 'T'), 0, "valid_id kind mismatch");
    check_int(store_valid_id("T3", 'T'), 1, "valid_id T3 ok");
    check_int(store_valid_id("A12", 'A'), 1, "valid_id A12 ok");
    check_int(store_valid_id("P 1", 'P'), 0, "valid_id P 1 bad");
    check_int(store_valid_id("P-1", 'P'), 0, "valid_id P-1 bad");
    check_int(store_valid_id(NULL, 'P'), 0, "valid_id null bad");

    /* context title split round trip */
    check_int(store_split_context("Fact: configuration location", &pre, &tit),
              1, "split Fact ok");
    check_str(pre, "Fact", "split prefix Fact");
    check_str(tit, "configuration location", "split title");
    free(pre);
    free(tit);
    check_int(store_split_context("Fact:nospace", &pre, &tit), 0,
              "split no-colon-space fails");
    check(pre == NULL && tit == NULL, "split fail leaves nulls");
    check_int(store_split_context("Rule: temporary files", &pre, &tit), 1,
              "split Rule ok");
    check_str(pre, "Rule", "split prefix Rule");
    check_str(tit, "temporary files", "split title Rule");
    free(pre);
    free(tit);
    check_int(store_split_context("Success metric: fast", &pre, &tit), 1,
              "split two-word prefix ok");
    check_str(pre, "Success metric", "split prefix two-word");
    free(pre);
    free(tit);
    check_int(store_split_context("Random: x", &pre, &tit), 0,
              "split unknown prefix fails");
    check_int(store_split_context("", &pre, &tit), 0, "split empty fails");

    /* store_is_context_title */
    check_int(store_is_context_title("Fact: configuration location"), 1,
              "is_context Fact yes");
    check_int(store_is_context_title("Rule: temporary files"), 1,
              "is_context Rule yes");
    check_int(store_is_context_title("Risk: it breaks"), 1,
              "is_context Risk yes");
    check_int(store_is_context_title("Random: x"), 0,
              "is_context Random no");
    check_int(store_is_context_title("Fact:nospace"), 0,
              "is_context no space no");
    check_int(store_is_context_title("Fact: "), 1,
              "is_context empty title yes");

    /* make + split round trip */
    p = store_make_context_title("Pattern", "divide and conquer");
    check_str(p, "Pattern: divide and conquer", "make_context_title");
    check_int(store_split_context(p, &pre, &tit), 1, "split make ok");
    check_str(pre, "Pattern", "split make prefix");
    check_str(tit, "divide and conquer", "split make title");
    free(pre);
    free(tit);
    free(p);

    /* status */
    check_int(store_valid_status("pending"), 1, "status pending ok");
    check_int(store_valid_status("complete"), 1, "status complete ok");
    check_int(store_valid_status("done"), 0, "status done bad");
    check_int(store_valid_status(""), 0, "status empty bad");

    /* id list parse */
    n = store_parse_id_list("A1, A2", &ids);
    check_int(n, 2, "parse A1, A2 count 2");
    check(n == 2 && !strcmp(ids[0], "A1") && !strcmp(ids[1], "A2"),
          "parse A1, A2 ids");
    for (i = 0; i < n; i++) free(ids[i]);
    free(ids);
    n = store_parse_id_list("A1,A2,", &ids);
    check_int(n, 2, "parse trailing comma drops empty");
    check(n == 2 && !strcmp(ids[0], "A1") && !strcmp(ids[1], "A2"),
          "parse A1,A2, ids");
    for (i = 0; i < n; i++) free(ids[i]);
    free(ids);
    n = store_parse_id_list("  A1 , B2 ", &ids);
    check_int(n, 2, "parse trims spaces");
    check(n == 2 && !strcmp(ids[0], "A1") && !strcmp(ids[1], "B2"),
          "parse trimmed ids");
    for (i = 0; i < n; i++) free(ids[i]);
    free(ids);
    n = store_parse_id_list("", &ids);
    check_int(n, 0, "parse empty 0");
    check(ids == NULL, "parse empty null ids");
    n = store_parse_id_list(",", &ids);
    check_int(n, 0, "parse lone comma 0");
    check(ids == NULL, "parse lone comma null");
    n = store_parse_id_list(" , , ", &ids);
    check_int(n, 0, "parse only spaces 0");
    n = store_parse_id_list("A1", &ids);
    check_int(n, 1, "parse single 1");
    check(n == 1 && !strcmp(ids[0], "A1"), "parse single id");
    for (i = 0; i < n; i++) free(ids[i]);
    free(ids);

    /* next action id over crafted docs */
    memset(&doc, 0, sizeof doc);
    p = store_next_action_id(&doc);
    check_str(p, "A1", "next_action empty doc A1");
    free(p);
    check_int(md_parse("# A1: take water\nDescription: x\n"
                       "\n"
                       "# A3: mix\nDescription: y\n",
                       &doc), 0, "parse actions doc ok");
    check_int(doc.nch, 2, "actions doc 2 chapters");
    p = store_next_action_id(&doc);
    check_str(p, "A4", "next_action A1,A3 -> A4");
    free(p);
    md_free_doc(&doc);
    check_int(md_parse("# Not an action\nDescription: x\n", &doc), 0,
              "parse non-action doc ok");
    p = store_next_action_id(&doc);
    check_str(p, "A1", "next_action non-action -> A1");
    free(p);
    md_free_doc(&doc);
    check_int(md_parse("# A2: only\nDescription: x\n", &doc), 0,
              "parse A2 doc ok");
    p = store_next_action_id(&doc);
    check_str(p, "A3", "next_action A2 -> A3");
    free(p);
    md_free_doc(&doc);
    p = store_next_action_id(NULL);
    check_str(p, "A1", "next_action null doc A1");
    free(p);

    /* disk tests: temp FLOWER_BASE */
    tmp = mkdtemp(tmpl);
    if (!tmp) {
        printf("  [SKIP] store disk tests (/tmp unwritable)\n");
    } else {
        size_t blen = strlen(tmp);
        setenv("FLOWER_BASE", tmp, 1);
        base = store_base();
        check(base && !strcmp(base, tmp), "store_base respects FLOWER_BASE");

        /* store_path */
        p = store_path("projects/P1/config.md");
        check(p && !strncmp(p, tmp, blen), "store_path starts with base");
        if (p) {
            size_t plen = strlen(p);
            check(plen == blen + 1 + strlen("projects/P1/config.md") &&
                      !strcmp(p + blen + 1, "projects/P1/config.md"),
                  "store_path joins rel");
        }
        free(p);
        p = store_path(NULL);
        check_str(p, tmp, "store_path null rel = base");
        free(p);

        /* mkdir -p then next id */
        check_int(store_mkdir_p("projects/P1"), 0, "mkdir_p projects/P1 ok");
        check_int(store_mkdir_p("projects/P2"), 0, "mkdir_p projects/P2 ok");
        p = store_next_project_id();
        check_str(p, "P3", "next_project P1,P2 -> P3");
        free(p);
        p = store_next_task_id("P1");
        check_str(p, "T1", "next_task empty -> T1");
        free(p);
        check_int(store_mkdir_p("projects/P1/tasks/T1"), 0,
                  "mkdir_p nested ok");
        p = store_next_task_id("P1");
        check_str(p, "T2", "next_task T1 -> T2");
        free(p);
        check_int(store_mkdir_p("projects/P3/nested/deep"), 0,
                  "mkdir_p deep ok");
        p = store_next_project_id();
        check_str(p, "P4", "next_project P1..P3 -> P4");
        free(p);
        check_int(store_mkdir_p("projects/P1"), 0, "mkdir_p existing ok");

        /* cleanup: rm -rf tmp */
        {
            char cmd[4200];
            snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
            if (system(cmd) != 0)
                printf("  [WARN] could not remove %s\n", tmp);
        }
        unsetenv("FLOWER_BASE");
    }
}

int main(void) {
    test_buf();
    test_strutil();
    test_md();
    test_store();
    test_cli();
    test_semver();
    printf("selfcheck: %d passed, %d failed\n", tests_run - tests_failed,
           tests_failed);
    return tests_failed ? 1 : 0;
}
