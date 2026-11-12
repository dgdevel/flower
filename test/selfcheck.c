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
    /* hash set grows past initial capacity and still catches dups */
    {
        buf_t big;
        int i;
        buf_init(&big);
        buf_appendf(&big, "# R\n");
        for (i = 0; i < 300; i++) buf_appendf(&big, "## S%d\n", i);
        buf_appendf(&big, "## S42\ndup\n");
        check_int(md_parse(big.data, &doc), -2, "dup after 300 grow");
        buf_free(&big);
    }
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

static void test_store_llm(void) {
    static const char *const want_keys[] = {
        "Api base", "Api key", "Protocol", "Model", "Max tokens",
        "Reasoning effort", "Header", "Mcp proxy", "Terminal tool",
    };
    static const char *const want_protos[] = {
        "anthropic", "openai", "openai-responses",
    };
    static const char *const want_user[] = {"Name", "Role",
                                            "Scout profile"};
    size_t i;
    const char *const *p;

    printf("store_llm:\n");

    /* store_llm_keys table: exact contents, NULL-terminated */
    for (i = 0; i < sizeof want_keys / sizeof want_keys[0]; i++)
        check(store_llm_keys[i] && !strcmp(store_llm_keys[i],
                                           want_keys[i]),
              "llm_keys slot matches");
    check(store_llm_keys[sizeof want_keys / sizeof want_keys[0]] == NULL,
          "llm_keys null terminated");

    /* store_user_keys table */
    for (i = 0; i < sizeof want_user / sizeof want_user[0]; i++)
        check(store_user_keys[i] && !strcmp(store_user_keys[i],
                                            want_user[i]),
              "user_keys slot matches");
    check(store_user_keys[sizeof want_user / sizeof want_user[0]] == NULL,
          "user_keys null terminated");
    for (p = store_llm_keys; *p; p++)
        for (i = 0; i < sizeof want_keys / sizeof want_keys[0]; i++)
            if (!strcmp(*p, want_keys[i])) break;
    check_int((int)(p - store_llm_keys),
              (int)(sizeof want_keys / sizeof want_keys[0]),
              "llm_keys has no extras");

    /* store_llm_protocols table */
    for (i = 0; i < sizeof want_protos / sizeof want_protos[0]; i++)
        check(store_llm_protocols[i] && !strcmp(store_llm_protocols[i],
                                                want_protos[i]),
              "llm_protocols slot matches");
    check(store_llm_protocols[3] == NULL, "llm_protocols null terminated");

    /* store_valid_llm_protocol matrix */
    check_int(store_valid_llm_protocol("anthropic"), 1,
              "protocol anthropic ok");
    check_int(store_valid_llm_protocol("openai"), 1, "protocol openai ok");
    check_int(store_valid_llm_protocol("openai-responses"), 1,
              "protocol openai-responses ok");
    check_int(store_valid_llm_protocol("bogus"), 0,
              "protocol bogus bad");
    check_int(store_valid_llm_protocol(""), 0, "protocol empty bad");
    check_int(store_valid_llm_protocol("Anthropic"), 0,
              "protocol case sensitive");
    check_int(store_valid_llm_protocol(NULL), 0, "protocol null bad");

    /* store_valid_llm_name matrix */
    check_int(store_valid_llm_name("gpt"), 1, "name gpt ok");
    check_int(store_valid_llm_name("claude sonnet"), 1,
              "name with space ok");
    check_int(store_valid_llm_name("my-prod 2"), 1,
              "name mixed space dash digit ok");
    check_int(store_valid_llm_name("A"), 1, "name single letter ok");
    check_int(store_valid_llm_name("gpt-4_turbo x"), 1,
              "name dash underscore space ok");
    check_int(store_valid_llm_name(NULL), 0, "name null bad");
    check_int(store_valid_llm_name(""), 0, "name empty bad");
    check_int(store_valid_llm_name("9gpt"), 0, "name digit first bad");
    check_int(store_valid_llm_name("claude/sonnet"), 0,
              "name slash bad");
    check_int(store_valid_llm_name("claude@x"), 0, "name at bad");
    check_int(store_valid_llm_name(" claude"), 0, "name leading space bad");
}

static void test_store_scout_profile(void) {
    char tmpl[] = "/tmp/flower-selfcheck-scp-XXXXXX";
    char *tmp, *p;
    char path[4200];
    FILE *f;

    printf("store_scout_profile:\n");
    tmp = mkdtemp(tmpl);
    if (!tmp) {
        printf("  [SKIP] store_scout_profile (/tmp unwritable)\n");
        return;
    }
    setenv("FLOWER_BASE", tmp, 1);

    /* 1. no flower.md, no llms.md */
    p = store_scout_profile();
    check(p == NULL, "no files -> null");
    free(p);

    /* 2. explicit key wins, even with 2+ llm profiles */
    snprintf(path, sizeof path, "%s/llms.md", tmp);
    f = fopen(path, "w");
    fputs("# claude\nApi base: http://x\nProtocol: anthropic\n"
          "\n# gpt\nApi base: http://y\nProtocol: openai\n", f);
    fclose(f);
    p = store_scout_profile();
    check(p == NULL, "no flower key, 2 llms -> null");
    free(p);
    snprintf(path, sizeof path, "%s/flower.md", tmp);
    f = fopen(path, "w");
    fputs("# User\nName: n\nRole: r\nScout profile: gpt\n", f);
    fclose(f);
    p = store_scout_profile();
    check_str(p, "gpt", "explicit key wins");
    free(p);

    /* 3. no key, single llm profile -> default */
    snprintf(path, sizeof path, "%s/flower.md", tmp);
    f = fopen(path, "w");
    fputs("# User\nName: n\nRole: r\n", f);
    fclose(f);
    snprintf(path, sizeof path, "%s/llms.md", tmp);
    f = fopen(path, "w");
    fputs("# claude\nApi base: http://x\nProtocol: anthropic\n", f);
    fclose(f);
    p = store_scout_profile();
    check_str(p, "claude", "single llm profile defaults");
    free(p);

    /* 4. empty key value falls through to the llms.md rule */
    snprintf(path, sizeof path, "%s/flower.md", tmp);
    f = fopen(path, "w");
    fputs("# User\nName: n\nRole: r\nScout profile: \n", f);
    fclose(f);
    p = store_scout_profile();
    check_str(p, "claude", "empty key falls through");
    free(p);

    /* cleanup */
    {
        char cmd[4200];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
        if (system(cmd) != 0)
            printf("  [WARN] could not remove %s\n", tmp);
    }
    unsetenv("FLOWER_BASE");
}

static void test_json(void) {
    static const char *const doc =
        "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"fs\","
        "\"command_line\":\"npx -y x\",\"required\":false,\"n\":3,"
        "\"nest\":{\"a\":[1,2,{\"b\":null}]}}]}";
    json_t *j, *j2, *n, *tools, *first;
    char *p1, *p2, *p3;

    printf("json:\n");

    /* roundtrip of a representative mcp-ish document */
    j = json_parse(doc);
    check(j != NULL, "parse mcp doc ok");
    p1 = j ? json_print(j) : NULL;
    check_str(p1, doc, "print mcp doc compact");
    j2 = p1 ? json_parse(p1) : NULL;
    check(j2 != NULL, "reparse printed doc");
    p2 = j2 ? json_print(j2) : NULL;
    check(p1 && p2 && !strcmp(p1, p2), "roundtrip identical");
    check_str(json_str_of(j, "type"), "tools", "str_of type");
    tools = json_get(j, "tools");
    check(tools && tools->type == JSON_ARR, "get tools is arr");
    first = tools && tools->child ? tools->child : NULL;
    check(first && first->type == JSON_OBJ, "tools[0] is obj");
    check_str(json_str_of(first, "command_line"), "npx -y x",
              "str_of nested");
    n = json_get(first, "required");
    check(n && n->type == JSON_BOOL && !(int)n->num, "required false");
    n = json_get(first, "n");
    check(n && n->type == JSON_NUM && n->num == 3.0, "n == 3.0");
    n = json_get(json_get(json_get(first, "nest"), "a"), "x");
    check(n == NULL, "get missing key null");
    check(json_get(j, "nope") == NULL, "get missing top null");
    check(json_get(NULL, "a") == NULL, "get null obj null");
    check(json_str_of(NULL, "a") == NULL, "str_of null obj null");
    free(p2);
    free(p1);
    json_free(j2);
    json_free(j);

    /* escapes: quote, backslash, newline, tab, \u00e9, surrogate pair */
    j = json_parse("\"a\\\"b\\\\c\\nd\\te\\u00e9f\\ud83d\\ude00g\"");
    check(j != NULL, "parse escape string ok");
    p1 = j ? json_print(j) : NULL;
    check_str(p1, "\"a\\\"b\\\\c\\nd\\te\xC3\xA9" "f\xF0\x9F\x98\x80" "g\"",
              "escapes decoded to utf8");
    j2 = p1 ? json_parse(p1) : NULL;
    check(j2 != NULL, "reparse escaped string");
    p2 = j2 ? json_print(j2) : NULL;
    check(p1 && p2 && !strcmp(p1, p2), "escape roundtrip identical");
    free(p2);
    free(p1);
    json_free(j2);
    json_free(j);

    /* control char escapes on serialize: \u0001 */
    j = json_str("a\001b");
    check(j != NULL, "json_str ok");
    p1 = j ? json_print(j) : NULL;
    check_str(p1, "\"a\\u0001b\"", "control char escaped");
    free(p1);
    json_free(j);

    /* errors: NULL on all of these */
    {
        static const char *const bad[] = {
            "", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "nul",
            "\"unterminated", "\"\\q\"", "\"\\u00\"", "\"\\ud83d\"",
            "\"\\udc00x\"", "-", "01x", "1 2", "[] extra", "{} trailing",
            "[}", "{\"a\"}", "{1:2}",
        };
        size_t i;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            char name[64];
            snprintf(name, sizeof name, "parse error case %d",
                     (int)i + 1);
            check(json_parse(bad[i]) == NULL, name);
        }
    }
    {
        /* deep nesting: 70 levels of [ fails, 10 passes */
        char deep[200];
        int i;
        for (i = 0; i < 70; i++) deep[i] = '[';
        for (i = 70; i < 140; i++) deep[i] = ']';
        deep[140] = '\0';
        check(json_parse(deep) == NULL, "70 nested arrays rejected");
        for (i = 0; i < 10; i++) deep[i] = '[';
        for (i = 10; i < 20; i++) deep[i] = ']';
        deep[20] = '\0';
        j = json_parse(deep);
        check(j != NULL, "10 nested arrays ok");
        json_free(j);
    }
    {
        /* deep nesting via objects too */
        char deep[256];
        int i;
        for (i = 0; i < 64; i++) {
            deep[i * 2] = '{';
            deep[i * 2 + 1] = '"';
        }
        deep[128] = '1';
        for (i = 129; i < 193; i++) deep[i] = '}';
        deep[193] = '\0';
        check(json_parse(deep) == NULL, "65 nested objects rejected");
    }

    /* builders: json_new/json_str/json_num/json_bool/json_arr/json_obj */
    j = json_obj();
    n = json_str("v1");
    n->key = str_dup("k1");
    json_add(j, n);
    n = json_num(2.5);
    n->key = str_dup("k2");
    json_add(j, n);
    n = json_bool(1);
    n->key = str_dup("k3");
    json_add(j, n);
    n = json_arr();
    n->key = str_dup("k4");
    json_add(j, n);
    json_add(n, json_num(1));
    json_add(n, json_num(2));
    n = json_new(JSON_NULL);
    n->key = str_dup("k5");
    json_add(j, n);
    p1 = json_print(j);
    check_str(p1, "{\"k1\":\"v1\",\"k2\":2.5,\"k3\":true,"
                  "\"k4\":[1,2],\"k5\":null}",
              "build + print tree");
    free(p1);
    json_free(j);

    /* number formatting: integral no .0, big/frac via %g */
    j = json_num(1.0);
    p1 = json_print(j);
    check_str(p1, "1", "integral prints without .0");
    free(p1);
    json_free(j);
    j = json_num(-42.0);
    p1 = json_print(j);
    check_str(p1, "-42", "negative integral");
    free(p1);
    json_free(j);
    j = json_num(0.1);
    p1 = json_print(j);
    check_str(p1, "0.10000000000000001", "fraction %.17g");
    free(p1);
    json_free(j);
    j = json_num(1e300);
    p1 = json_print(j);
    check_str(p1, "1.0000000000000001e+300", "huge exponent");
    free(p1);
    json_free(j);

    /* parse of the printed numbers roundtrips */
    j = json_parse("{\"a\":1,\"b\":0.1,\"c\":-42,\"d\":1e300}");
    check(j != NULL, "parse numbers doc");
    p1 = j ? json_print(j) : NULL;
    j2 = p1 ? json_parse(p1) : NULL;
    p2 = j2 ? json_print(j2) : NULL;
    p3 = p2 ? json_print(j2) : NULL;
    check(p2 && p3 && !strcmp(p2, p3), "number print idempotent");
    check(j2 && json_get(j2, "c") && json_get(j2, "c")->num == -42.0,
          "c parsed -42");
    check(j2 && json_get(j2, "d") && json_get(j2, "d")->num == 1e300,
          "d parsed 1e300");
    free(p3);
    free(p2);
    free(p1);
    json_free(j2);
    json_free(j);

    /* json_free(NULL) and empty containers */
    json_free(NULL);
    j = json_parse("{}");
    check(j != NULL && j->child == NULL, "empty object");
    p1 = j ? json_print(j) : NULL;
    check_str(p1, "{}", "empty object prints {}");
    free(p1);
    json_free(j);
    j = json_parse("[]");
    check(j != NULL && j->child == NULL, "empty array");
    p1 = j ? json_print(j) : NULL;
    check_str(p1, "[]", "empty array prints []");
    free(p1);
    json_free(j);
    j = json_parse("  null  ");
    check(j != NULL && j->type == JSON_NULL, "ws-wrapped null");
    json_free(j);

    /* siblings survive a nested free (leak check: run under valgrind) */
    j = json_parse("[[1],[2,[3]],[4]]");
    check(j != NULL, "sibling tree parse");
    if (j && j->child) {
        json_t *mid = j->child->next;
        json_free(mid->child); /* free [2,[3]] contents, keep [4] */
        j->child->next->child = NULL;
    }
    p1 = json_print(j);
    check_str(p1, "[[1],[],[4]]", "sibling free keeps neighbors");
    free(p1);
    json_free(j);
}

int main(void) {
    test_buf();
    test_strutil();
    test_md();
    test_store();
    test_store_llm();
    test_store_scout_profile();
    test_cli();
    test_semver();
    test_json();
    printf("selfcheck: %d passed, %d failed\n", tests_run - tests_failed,
           tests_failed);
    return tests_failed ? 1 : 0;
}
