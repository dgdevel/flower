/* check.c - file io round trip tests, no network, no llm.
 * run: make check (builds test/check and runs it)
 *
 * each test runs in a fresh $XDG_CONFIG_DIR (temp dir) and a fresh cwd
 * (temp dir), so no real user data is touched. */
#define _POSIX_C_SOURCE 200809L

#include "../src/flower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/* fresh sandbox: $XDG_CONFIG_DIR=<tmp>/cfg, cwd=<tmp>/proj. Returns 0. */
static int sandbox(char *cfg, size_t cfg_size, char *proj,
                   size_t proj_size)
{
    char tmpl[] = "/tmp/flower-check-XXXXXX";
    char *dir = mkdtemp(tmpl);
    char cwd[1024];

    if (!dir)
        return -1;
    snprintf(cfg, cfg_size, "%s/cfg", dir);
    snprintf(proj, proj_size, "%s/proj", dir);
    if (mkdir(cfg, 0755) != 0 || mkdir(proj, 0755) != 0)
        return -1;
    if (setenv("XDG_CONFIG_DIR", cfg, 1) != 0)
        return -1;
    if (chdir(proj) != 0)
        return -1;
    if (!getcwd(cwd, sizeof(cwd)))
        return -1;
    /* project_find_current compares against cwd, keep it absolute */
    snprintf(proj, proj_size, "%s", cwd);
    return 0;
}

static void write_settings(void)
{
    char base[1024], path[1100], cwd[1024];

    if (flower_base_dir(base, sizeof(base)) == 0) {
        snprintf(path, sizeof(path), "%s/flower.json", base);
        if (!getcwd(cwd, sizeof(cwd)))
            cwd[0] = '\0';
        (void)flower_ensure_dir(base);
        (void)flower_write_file(path,
            "{\n"
            "  \"llm\": {\n"
            "    \"endpoint_protocol\": \"openai\",\n"
            "    \"api_base\": \"http://localhost/v1\"\n"
            "  },\n"
            "  \"user\": {\n"
            "    \"name\": \"Tester\",\n"
            "    \"role\": \"QA\"\n"
            "  }\n"
            "}\n");
    }
}

static void test_settings(void)
{
    struct flower_settings s;

    write_settings();
    check(flower_settings_load(&s) == 0, "settings: load valid flower.json");
    check(!strcmp(s.protocol, "openai"), "settings: protocol");
    check(!strcmp(s.api_base, "http://localhost/v1"), "settings: api_base");
    check(!strcmp(s.model, ""), "settings: absent model stays empty");
    check(!strcmp(s.user_name, "Tester"), "settings: user name");
    check(!strcmp(s.user_role, "QA"), "settings: user role");
    {
        char base[1024], path[1100];

        flower_base_dir(base, sizeof(base));
        snprintf(path, sizeof(path), "%s/flower.json", base);
        (void)flower_write_file(path, "{ not json ]");
        check(flower_settings_load(&s) == -2, "settings: invalid json -> -2");
        (void)flower_write_file(path, "{\"llm\":{}}");
        check(flower_settings_load(&s) == -2, "settings: missing fields -> -2");
        unlink(path);
        check(flower_settings_load(&s) == -1, "settings: absent file -> -1");
    }
}

static void test_file_rw(void)
{
    char path[] = "/tmp/flower-check-rw.txt";
    char *text = NULL;

    check(flower_write_file(path, "one\ntwo\n") == 0, "file: write");
    check(flower_read_file(path, &text) == 0 && text
          && !strcmp(text, "one\ntwo\n"), "file: read back");
    free(text);
    text = NULL;
    check(flower_read_file("/tmp/flower-check-absent-xyz", &text) == -1,
          "file: read absent fails");
    check(flower_count_md_items("- a\n- b\n\ntext\n- c\n") == 3,
          "file: count markdown items");
    unlink(path);
}

static void test_project(void)
{
    struct flower_project p1, p2, found;
    struct flower_project list[8];
    char path[2048], *text = NULL;
    int n;

    check(project_create("/tmp/flower-check-alpha", &p1) == 0,
          "project: create first -> P1");
    check(!strcmp(p1.id, "P1") && !strcmp(p1.name, "flower-check-alpha"),
          "project: id and name from path basename");
    check(project_create("/tmp/flower-check-beta", &p2) == 0,
          "project: create second -> P2");
    check(!strcmp(p2.id, "P2"), "project: id sequence grows");

    /* config round trip through files */
    snprintf(path, sizeof(path), "%s/config.json", p2.dir);
    check(flower_read_file(path, &text) == 0 && strstr(text, "beta"),
          "project: config.json written on disk");
    free(text);
    check(project_by_id("P2", &found) == 0 && !strcmp(found.name,
          "flower-check-beta"), "project: by id");
    check(project_by_id("P9", &found) == -1, "project: by id absent fails");

    n = project_list(list, 8);
    check(n == 2 && !strcmp(list[0].id, "P1") && !strcmp(list[1].id, "P2"),
          "project: list in id order");

    /* counts */
    snprintf(path, sizeof(path), "%s/context.md", p1.dir);
    (void)flower_write_file(path, "- Fact: a\n- Risk: b\n");
    check(project_count_context_items(&p1) == 2,
          "project: count context items");
    snprintf(path, sizeof(path), "%s/resources.json", p1.dir);
    (void)flower_write_file(path, "[\"x\", \"y\"]");
    check(project_count_resources(&p1) == 2, "project: count resources");
    (void)flower_write_file(path, "[]");
    check(project_count_resources(&p1) == 0,
          "project: empty array counts zero");
    check(project_task_count(&p1) == 0, "project: task count zero");

    /* details + parsed loaders */
    snprintf(path, sizeof(path), "%s/details.md", p1.dir);
    (void)flower_write_file(path, "# Output\n\nstuff\n");
    {
        char *det = NULL;

        check(project_read_details(&p1, &det) == 0 && det
              && !strcmp(det, "# Output\n\nstuff\n"),
              "project: read details");
        free(det);
    }
    snprintf(path, sizeof(path), "%s/context.md", p1.dir);
    (void)flower_write_file(path, "- [Fact] a\n- [Risk] b\n- plain\n");
    {
        struct flower_context_item *items = NULL;
        int n = 0;

        check(project_load_context_items(&p1, &items, &n) == 0 && n == 3,
              "project: load context items");
        check(n == 3 && !strcmp(items[0].type, "Fact")
              && !strcmp(items[0].content, "a")
              && !strcmp(items[2].type, "") && !strcmp(items[2].content,
              "plain"), "project: context item fields");
        for (int i = 0; i < n; i++)
            free(items[i].content);
        free(items);
    }
    snprintf(path, sizeof(path), "%s/resources.json", p1.dir);
    (void)flower_write_file(path,
                            "[{\"type\":\"path\",\"content\":\"src/a.c\"},"
                            "\"bare\"]");
    {
        struct flower_resource *res = NULL;
        int n = 0;

        check(project_load_resources(&p1, &res, &n) == 0 && n == 2,
              "project: load resources");
        check(n == 2 && !strcmp(res[0].type, "path")
              && !strcmp(res[0].content, "src/a.c")
              && !strcmp(res[1].type, "") && !strcmp(res[1].content,
              "bare"), "project: resource fields");
        for (int i = 0; i < n; i++)
            free(res[i].content);
        free(res);
    }

    /* find_current: sandbox cwd is <tmp>/proj, make P2 cover it */
    {
        struct flower_project tmp_p;
        char cwd[1024], cfg2[1024];
        char *body;

        /* rewrite P2 config path to the sandbox cwd */
        if (getcwd(cwd, sizeof(cwd))
            && flower_base_dir(cfg2, sizeof(cfg2)) == 0) {
            snprintf(path, sizeof(path), "%s/projects/P2/config.json", cfg2);
            body = malloc(strlen(cwd) + 64);
            if (body) {
                sprintf(body, "{\"name\":\"beta\",\"path\":\"%s\","
                        "\"description\":\"\"}", cwd);
                (void)flower_write_file(path, body);
                free(body);
            }
            check(project_find_current(&tmp_p) == 0
                  && !strcmp(tmp_p.id, "P2"),
                  "project: find current by cwd");
            /* subdirectory coverage */
            snprintf(path, sizeof(path), "%s/sub", cwd);
            (void)mkdir(path, 0755);
            if (chdir(path) == 0) {
                check(project_find_current(&tmp_p) == 0
                      && !strcmp(tmp_p.id, "P2"),
                      "project: find current covers subdirectory");
                (void)chdir(cwd);
            }
        }
    }
}

static void test_task(void)
{
    struct flower_project p;
    struct flower_task t1, t2, got;
    struct flower_task list[8];
    char path[2048];
    int n;

    project_by_id("P1", &p);
    check(task_create(&p, "first task", "do things", &t1) == 0,
          "task: create first -> T1");
    check(!strcmp(t1.id, "T1") && !strcmp(t1.name, "first task"),
          "task: id and name");
    check(task_create(&p, "second task", NULL, &t2) == 0
          && !strcmp(t2.id, "T2"), "task: create second -> T2");

    n = task_list(&p, list, 8);
    check(n == 2 && !strcmp(list[0].id, "T1") && !strcmp(list[1].id, "T2"),
          "task: list in id order");

    snprintf(path, sizeof(path), "%s/config.json", t1.dir);
    check(access(path, F_OK) == 0, "task: config.json on disk");

    /* current pointer round trip */
    check(task_current_get(&p, path, sizeof(path)) == -1,
          "task: no pointer before set");
    check(task_current_set(&p, "T2") == 0, "task: set current");
    check(task_current_get(&p, path, sizeof(path)) == 0
          && !strcmp(path, "T2"), "task: get current");
    check(task_current_set(&p, "T9") == -1, "task: set absent fails");

    /* resolve: bare and cross-project forms */
    check(task_resolve(&p, "T1", &got) == 0 && !strcmp(got.id, "T1"),
          "task: resolve bare id");
    check(task_resolve(&p, "P1-T2", &got) == 0 && !strcmp(got.id, "T2"),
          "task: resolve P-T form");
    check(task_resolve(&p, "T9", &got) == -1, "task: resolve absent");
    check(task_resolve(&p, "X1", &got) == -2, "task: resolve malformed");
    check(task_resolve(&p, "P1-X", &got) == -2,
          "task: resolve malformed P-T");

    /* counts */
    snprintf(path, sizeof(path), "%s/context.md", t1.dir);
    (void)flower_write_file(path, "- Fact: a\n- Fact: b\n- Rule: c\n");
    check(task_count_context_items(&t1) == 3, "task: count context items");
    snprintf(path, sizeof(path), "%s/resources.json", t1.dir);
    (void)flower_write_file(path, "[\"r1\"]");
    check(task_count_resources(&t1) == 1, "task: count resources");
}

static void test_actions(void)
{
    struct flower_project p;
    struct flower_task t;
    struct flower_action acts[2], got[8];
    int n;

    project_by_id("P1", &p);
    task_resolve(&p, "T1", &t);

    check(action_list(&t, got, 8) == -2, "actions: absent file -> -2");

    memset(acts, 0, sizeof(acts));
    strcpy(acts[0].id, "A1");
    strcpy(acts[0].name, "first");
    strcpy(acts[0].description, "d1");
    strcpy(acts[0].type, "Act");
    strcpy(acts[0].state, "Pending");
    strcpy(acts[1].id, "A2");
    strcpy(acts[1].name, "second");
    strcpy(acts[1].description, "d2");
    strcpy(acts[1].type, "Validate");
    strcpy(acts[1].state, "Pending");
    strcpy(acts[1].blocked_by, "A1");
    check(action_save(&t, acts, 2) == 0, "actions: save");

    n = action_list(&t, got, 8);
    check(n == 2, "actions: list count");
    check(n > 0 && !strcmp(got[0].id, "A1") && !strcmp(got[0].type, "Act"),
          "actions: fields round trip");
    check(n > 1 && !strcmp(got[1].blocked_by, "A1"),
          "actions: blocked_by round trip");

    check(action_update(&t, "A1", "Completed", "Success") == 0,
          "actions: update state and outcome");
    n = action_list(&t, got, 8);
    check(n > 0 && !strcmp(got[0].state, "Completed")
          && !strcmp(got[0].outcome, "Success"),
          "actions: update persisted");
    check(action_update(&t, "A9", "Paused", NULL) == -1,
          "actions: update absent id fails");
    check(action_update(&t, "A2", "In Progress", NULL) == 0
          && action_list(&t, got, 8) == 2
          && !strcmp(got[1].state, "In Progress"),
          "actions: state only update keeps outcome");
}

static void test_prompt(void)
{
    struct flower_settings s;
    struct flower_project p;
    char *out;

    strcpy(s.user_name, "Tester");
    strcpy(s.user_role, "QA");
    s.user_name[0] = '\0';
    s.user_role[0] = '\0';
    project_by_id("P1", &p);
    out = prompt_build(&s, &p, NULL, NULL);
    check(out != NULL, "prompt: build with project only");
    if (out) {
        check(strstr(out, "fenced block") != NULL,
              "prompt: fenced block contract present");
        check(strstr(out, "projects") != NULL || strstr(out, "P1") != NULL,
              "prompt: project block present");
        free(out);
    }
}

static void test_mcp_scan_apply(void)
{
    struct flower_project p;
    char path[1100];
    char *text = NULL;
    cJSON *args;

    project_by_id("P1", &p);

    /* invalid: no required fields */
    check(mcp_scan_apply(p.dir, cJSON_Parse("{\"x\":1}"), NULL, 0) == -1,
          "mcp_scan: args without required fields rejected");

    /* minimal valid call */
    args = cJSON_Parse(
        "{\"details\":{\"Output\":\"docs\",\"Scope\":\"src only\"},"
        "\"contextItems\":[{\"type\":\"Fact\",\"content\":\"c code\"},"
        "{\"type\":\"Rule\",\"content\":\"c11\"}],"
        "\"resources\":[{\"type\":\"path\",\"content\":\"src\"},"
        "{\"type\":\"url\",\"content\":\"https://x\"}]}");
    check(args && mcp_scan_apply(p.dir, args, NULL, 0) == 0,
          "mcp_scan: valid args applied");
    cJSON_Delete(args);

    snprintf(path, sizeof(path), "%s/details.md", p.dir);
    check(flower_read_file(path, &text) == 0 && strstr(text, "# Output")
          && strstr(text, "docs") && strstr(text, "# Scope"),
          "mcp_scan: details.md chapters");
    free(text);
    text = NULL;
    snprintf(path, sizeof(path), "%s/context.md", p.dir);
    check(flower_read_file(path, &text) == 0
          && strstr(text, "- [Fact] c code")
          && strstr(text, "- [Rule] c11"),
          "mcp_scan: context.md items");
    free(text);
    text = NULL;
    snprintf(path, sizeof(path), "%s/resources.json", p.dir);
    check(flower_read_file(path, &text) == 0
          && project_count_resources(&p) == 2,
          "mcp_scan: resources.json entries countable");
    free(text);
    text = NULL;

    /* invalid enum rejected, nothing rewritten */
    args = cJSON_Parse(
        "{\"details\":{},\"contextItems\":[{\"type\":\"Nope\","
        "\"content\":\"x\"}],\"resources\":[]}");
    check(args && mcp_scan_apply(p.dir, args, NULL, 0) == -1,
          "mcp_scan: bad context type rejected");
    cJSON_Delete(args);
    snprintf(path, sizeof(path), "%s/context.md", p.dir);
    check(flower_read_file(path, &text) == 0
          && strstr(text, "[Fact] c code") != NULL,
          "mcp_scan: rejected call left files intact");
    free(text);
}

int main(void)
{
    char cfg[1024], proj[1024];

    if (sandbox(cfg, sizeof(cfg), proj, sizeof(proj)) != 0) {
        fprintf(stderr, "check: cannot create the sandbox\n");
        return 1;
    }
    test_settings();
    test_file_rw();
    test_project();
    test_task();
    test_actions();
    test_prompt();
    test_mcp_scan_apply();
    printf("\n%s\n", failures ? "FAILURES" : "all ok");
    return failures ? 1 : 0;
}
