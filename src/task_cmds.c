/* task_cmds.c - `flower task` and `flower action` commands. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int task_cmd_create(struct flower_project *p, int argc, char **argv);
static int task_cmd_update(struct flower_project *p, const char *ref,
                           int argc, char **argv);

#define EXIT_NO_PROJECT 3
#define EXIT_NO_TASK    4

/* resolve the project for cwd or print the hint and exit. */
static int need_project(struct flower_project *p, const char *cmd)
{
    if (project_find_current(p) != 0) {
        fprintf(stderr, "flower %s: no project covers the current "
                        "directory, run 'flower project scan' first\n",
                cmd);
        return EXIT_NO_PROJECT;
    }
    return 0;
}

/* resolve the current task of p or print the hint and exit. */
static int need_current_task(const struct flower_project *p,
                             struct flower_task *t)
{
    char id[FLOWER_ID_MAX];

    if (task_current_get(p, id, sizeof(id)) != 0) {
        fprintf(stderr, "flower: no current task, run 'flower task create' "
                        "or 'flower task set-current <ID>' first\n");
        return EXIT_NO_TASK;
    }
    if (task_resolve(p, id, t) != 0) {
        fprintf(stderr, "flower: current task %s does not exist, run "
                        "'flower task set-current <ID>'\n", id);
        return EXIT_NO_TASK;
    }
    return 0;
}

static int task_cmd_list(struct flower_project *p)
{
    struct flower_task list[64];
    char current[FLOWER_ID_MAX];
    int count, i, have_cur;

    count = task_list(p, list, 64);
    if (count < 0) {
        fprintf(stderr, "flower task list: cannot read tasks\n");
        return 1;
    }
    have_cur = task_current_get(p, current, sizeof(current)) == 0;
    if (count == 0) {
        printf("no tasks, run 'flower task create' to make one\n");
        return 0;
    }
    for (i = 0; i < count; i++) {
        printf("%s-%s  %s%s\n", p->id, list[i].id, list[i].name,
               (have_cur && !strcmp(current, list[i].id)) ? "  (current)"
                                                          : "");
    }
    return 0;
}

static int task_cmd_show(struct flower_project *p, const char *ref)
{
    struct flower_task t;
    char ctx_path[FLOWER_PATH_MAX];
    char *text = NULL;

    if (task_resolve(p, ref, &t) != 0) {
        fprintf(stderr, "flower task show: no such task: %s\n", ref);
        return 1;
    }
    printf("task %s-%s (%s)\n", p->id, t.id, t.name);
    if (t.description[0])
        printf("  description: %s\n", t.description);
    printf("  dir: %s\n", t.dir);
    printf("  context items: %d\n", task_count_context_items(&t));
    printf("  resources: %d\n", task_count_resources(&t));
    {
        struct flower_action acts[64];
        int n = action_list(&t, acts, 64);

        if (n > 0) {
            int i;

            printf("  actions:\n");
            for (i = 0; i < n; i++)
                printf("    %s  %-10s %-12s %s\n", acts[i].id,
                       acts[i].state, acts[i].type, acts[i].name);
        }
    }
    flower_path_join3(ctx_path, sizeof ctx_path, t.dir, "context", "md");
    if (flower_read_file(ctx_path, &text) == 0) {
        printf("\n--- context.md ---\n%s", text);
        if (text[0] && text[strlen(text) - 1] != '\n')
            printf("\n");
        free(text);
    }
    return 0;
}

static int task_cmd_set_current(struct flower_project *p, const char *ref)
{
    struct flower_task t;
    char full[64];

    if (task_resolve(p, ref, &t) != 0) {
        fprintf(stderr, "flower task set-current: no such task: %s\n", ref);
        return 1;
    }
    snprintf(full, sizeof(full), "%s-%s", p->id, t.id);
    if (task_current_set(p, t.id) != 0) {
        fprintf(stderr, "flower task set-current: cannot write the "
                        "pointer\n");
        return 1;
    }
    printf("current task: %s (%s)\n", full, t.name);
    return 0;
}

int task_cmd(int argc, char **argv)
{
    struct flower_project p;
    struct flower_task t;
    int rc;

    if ((rc = need_project(&p, "task")) != 0)
        return rc;
    if (argc < 2) {
        if ((rc = need_current_task(&p, &t)) != 0)
            return rc;
        return task_cmd_show(&p, t.id);
    }
    if (!strcmp(argv[1], "list"))
        return task_cmd_list(&p);
    if (!strcmp(argv[1], "show")) {
        if (argc < 3) {
            if ((rc = need_current_task(&p, &t)) != 0)
                return rc;
            return task_cmd_show(&p, t.id);
        }
        if (argc > 3) {
            fprintf(stderr, "usage: flower task show [ID]\n");
            return 2;
        }
        return task_cmd_show(&p, argv[2]);
    }
    if (!strcmp(argv[1], "set-current")) {
        if (argc != 3) {
            fprintf(stderr, "usage: flower task set-current <ID>\n");
            return 2;
        }
        return task_cmd_set_current(&p, argv[2]);
    }
    if (!strcmp(argv[1], "create"))
        return task_cmd_create(&p, argc - 2, argv + 2);
    if (!strcmp(argv[1], "update")) {
        if (argc >= 3 && !strncmp(argv[2], "--id=", 5)
            && argv[2][5])
            return task_cmd_update(&p, argv[2] + 5, argc - 3, argv + 3);
        return task_cmd_update(&p, "", argc - 2, argv + 2);
    }
    fprintf(stderr, "usage: flower task [list|show|set-current|"
                    "create|update] [args]\n");
    return 2;
}

/* shared llm flow of task create and task update: run the conversation
 * that fills the task files. topic tells the model what to do. 0 on
 * success. */
static int task_llm_flow(const struct flower_settings *s,
                         const struct flower_project *p,
                         const struct flower_task *t, int fresh,
                         const char *user_prompt)
{
    char transcript[FLOWER_PATH_MAX];
    char sessions_dir[FLOWER_PATH_MAX];
    char prompt_text[2048];
    char *seed, *prompt;

    prompt = prompt_build(s, p, t, NULL);
    if (!prompt) {
        fprintf(stderr, "flower task: cannot build the prompt\n");
        return 1;
    }
    if (fresh)
        flower_snprintf(prompt_text, sizeof prompt_text,
                 "Task %s-%s (%s) was just created from this request:\n"
                 "%s\n\nPopulate the task now: read the project directory "
                 "(%s) and the project data with the tools as needed, then "
                 "propose projects/%s/tasks/%s/config.json (name, short "
                 "description), projects/%s/tasks/%s/context.md (list "
                 "items, one per line, of type Fact, Pattern, Risk, "
                 "Success metric, Failure sign, Evaluation method, or "
                 "Rule), projects/%s/tasks/%s/resources.json (json array "
                 "of strings: paths, urls, pointers), and "
                 "projects/%s/tasks/%s/actions.json (json array, each "
                 "action an object with id \"A1\", \"A2\", ... in order, "
                 "name, description, type Observe|Analyze|Root Cause|Act|"
                 "Validate|Improve, state Pending, outcome \"\", "
                 "blocked_by array of ids). Use one flower "
                 "block per file. Ask me questions when in doubt.",
                 p->id, t->id, t->name, user_prompt, p->path, p->id,
                 t->id, p->id, t->id, p->id, t->id, p->id, t->id);
    else
        snprintf(prompt_text, sizeof(prompt_text),
                 "Update task %s-%s (%s) following this request:\n%s\n\n"
                 "The current task files are in the context above. Read "
                 "the project directory (%s) with the tools if the update "
                 "needs it, then propose the changed files as flower "
                 "blocks (full file content, target "
                 "projects/%s/tasks/%s/<file>). Ask me questions when in "
                 "doubt.",
                 p->id, t->id, t->name, user_prompt, p->path, p->id,
                 t->id);
    {
        const char *parts[1];

        parts[0] = prompt;
        seed = llm_build_seed(s, parts, 1, prompt_text);
    }
    free(prompt);
    if (!seed) {
        fprintf(stderr, "flower task: cannot build the seed\n");
        return 1;
    }
    flower_path_join(sessions_dir, sizeof sessions_dir, t->dir, "sessions");
    (void)flower_ensure_dir(sessions_dir);
    flower_snprintf(transcript, sizeof transcript,
             "%s/task-%s.jsonl", sessions_dir, t->id);
    return llm_converse(s, seed, transcript);
}

/* read the user prompt: argv words joined when given, else stdin until
 * eof. Returns a malloc'd string, NULL on error (message printed). */
static char *read_user_prompt(int argc, char **argv, const char *cmd)
{
    struct {
        char *data;
        size_t len, cap;
    } b = {0};
    char *line = NULL;
    size_t cap = 0;
    int i;

    if (argc > 0) {
        size_t need = 1;
        char *out;

        for (i = 0; i < argc; i++)
            need += strlen(argv[i]) + 1;
        out = malloc(need);
        if (!out) {
            fprintf(stderr, "flower: out of memory\n");
            return NULL;
        }
        out[0] = '\0';
        for (i = 0; i < argc; i++) {
            if (i)
                strcat(out, " ");
            strcat(out, argv[i]);
        }
        return out;
    }
    if (isatty(0))
        printf("(end the prompt with ctrl-d)\n");
    while (getline(&line, &cap, stdin) != -1) {
        if (b.len + strlen(line) + 1 > b.cap) {
            while (b.len + strlen(line) + 1 > b.cap)
                b.cap = b.cap ? b.cap * 2 : 512;
            b.data = realloc(b.data, b.cap);
            if (!b.data) {
                fprintf(stderr, "flower: out of memory\n");
                free(line);
                return NULL;
            }
        }
        memcpy(b.data + b.len, line, strlen(line) + 1);
        b.len += strlen(line);
    }
    free(line);
    if (!b.data || !b.data[0]) {
        fprintf(stderr, "flower %s: empty prompt\n", cmd);
        free(b.data);
        return NULL;
    }
    return b.data;
}

static int task_cmd_create(struct flower_project *p, int argc, char **argv)
{
    struct flower_settings s;
    struct flower_task t;
    char *prompt_text, *name;
    int rc;

    if (flower_settings_load(&s) != 0) {
        fprintf(stderr, "flower task create: run 'flower setup' first\n");
        return 1;
    }
    prompt_text = read_user_prompt(argc, argv, "task create");
    if (!prompt_text)
        return 1;
    /* short name from the first words of the prompt */
    {
        char first[80];
        size_t n = 0;

        while (prompt_text[n] && prompt_text[n] != '\n'
               && n < sizeof(first) - 1) {
            first[n] = prompt_text[n];
            n++;
        }
        first[n] = '\0';
        {
            char *dot = strchr(first, '.');

            if (dot && (size_t)(dot - first) < 40)
                *dot = '\0';
        }
        name = strdup(first[0] ? first : "new task");
        if (!name) {
            free(prompt_text);
            fprintf(stderr, "flower: out of memory\n");
            return 1;
        }
    }
    if (task_create(p, name, prompt_text, &t) != 0) {
        fprintf(stderr, "flower task create: cannot create the task\n");
        free(prompt_text);
        free(name);
        return 1;
    }
    free(name);
    printf("created task %s-%s (%s)\n", p->id, t.id, t.name);
    if (task_current_set(p, t.id) != 0)
        fprintf(stderr, "flower task create: cannot set the current task\n");
    else
        printf("current task: %s-%s\n", p->id, t.id);
    rc = task_llm_flow(&s, p, &t, 1, prompt_text);
    free(prompt_text);
    return rc;
}

static int task_cmd_update(struct flower_project *p, const char *ref,
                           int argc, char **argv)
{
    struct flower_settings s;
    struct flower_task t;
    char *prompt_text;
    int rc;

    if (flower_settings_load(&s) != 0) {
        fprintf(stderr, "flower task update: run 'flower setup' first\n");
        return 1;
    }
    if (ref[0]) {
        if (task_resolve(p, ref, &t) != 0) {
            fprintf(stderr, "flower task update: no such task: %s\n", ref);
            return 1;
        }
    } else if ((rc = need_current_task(p, &t)) != 0) {
        return rc;
    }
    prompt_text = read_user_prompt(argc, argv, "task update");
    if (!prompt_text)
        return 1;
    printf("updating task %s-%s (%s)\n", p->id, t.id, t.name);
    rc = task_llm_flow(&s, p, &t, 0, prompt_text);
    free(prompt_text);
    return rc;
}

static int action_cmd_list(struct flower_project *p)
{
    struct flower_task t;
    struct flower_action acts[64];
    int n, i, rc;

    if ((rc = need_current_task(p, &t)) != 0)
        return rc;
    n = action_list(&t, acts, 64);
    if (n == -2) {
        printf("no actions\n");
        return 0;
    }
    if (n < 0) {
        fprintf(stderr, "flower action list: cannot read actions.json\n");
        return 1;
    }
    if (n == 0) {
        printf("no actions\n");
        return 0;
    }
    for (i = 0; i < n; i++) {
        printf("%s  %-12s %-14s %-8s %s\n", acts[i].id, acts[i].state,
               acts[i].type, acts[i].outcome[0] ? acts[i].outcome : "-",
               acts[i].name);
        if (acts[i].blocked_by[0])
            printf("      blocked by: %s\n", acts[i].blocked_by);
    }
    return 0;
}

static int action_finish(struct flower_project *p, struct flower_task *t,
                         const struct flower_action *a)
{
    char line[32];
    char state[32], outcome[32];

    printf("\nrecord the result of %s %s (%s)\n", p->id, t->id, a->id);
    printf("state (Enter keeps %s) [Pending/InProgress/Paused/Completed]: ",
           a->state);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin))
        return 0;
    line[strcspn(line, "\n")] = '\0';
    if (!line[0])
        snprintf(state, sizeof(state), "%s", a->state);
    else
        snprintf(state, sizeof(state), "%s", line);
    printf("outcome (Enter keeps %s) [Success/Partial/Failure/-]: ",
           a->outcome[0] ? a->outcome : "(none)");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin))
        return 0;
    line[strcspn(line, "\n")] = '\0';
    if (!line[0])
        snprintf(outcome, sizeof(outcome), "%s",
                 a->outcome[0] ? a->outcome : "");
    else if (!strcmp(line, "-"))
        outcome[0] = '\0';
    else
        snprintf(outcome, sizeof(outcome), "%s", line);
    if (action_update(t, a->id, state, outcome) != 0) {
        fprintf(stderr, "flower action resume: cannot write actions.json\n");
        return 1;
    }
    printf("%s-%s %s: state %s, outcome %s\n", p->id, t->id, a->id, state,
           outcome[0] ? outcome : "(none)");
    return 0;
}

static int action_cmd_resume(struct flower_project *p, const char *ref)
{
    struct flower_task t;
    struct flower_action acts[64];
    struct flower_settings s;
    struct flower_action *a = NULL;
    char transcript[FLOWER_PATH_MAX];
    char sessions_dir[FLOWER_PATH_MAX];
    char prompt_text[2048];
    char *seed, *prompt;
    int n, i, rc;

    if ((rc = need_current_task(p, &t)) != 0)
        return rc;
    n = action_list(&t, acts, 64);
    if (n == -2) {
        fprintf(stderr, "flower action resume: no actions in this task\n");
        return 1;
    }
    if (n < 0) {
        fprintf(stderr, "flower action resume: cannot read actions.json\n");
        return 1;
    }
    if (ref[0]) {
        for (i = 0; i < n; i++)
            if (!strcmp(acts[i].id, ref))
                a = &acts[i];
        if (!a) {
            fprintf(stderr, "flower action resume: no such action: %s\n",
                    ref);
            return 1;
        }
    } else {
        a = &acts[0];
        for (i = 1; i < n; i++)
            if (strcmp(acts[i].state, "Completed"))
                a = &acts[i];
        printf("resuming %s: %s\n", a->id, a->name);
    }
    if (a->blocked_by[0] && strcmp(a->state, "Completed")) {
        int open = 0;

        for (i = 0; i < n; i++)
            if (strstr(a->blocked_by, acts[i].id)
                && strcmp(acts[i].state, "Completed"))
                open = 1;
        if (open)
            printf("note: %s is blocked by an unfinished action (%s)\n",
                   a->id, a->blocked_by);
    }
    if (flower_settings_load(&s) != 0) {
        fprintf(stderr, "flower action resume: run 'flower setup' first\n");
        return 1;
    }
    prompt = prompt_build(&s, p, &t, a);
    if (!prompt) {
        fprintf(stderr, "flower action resume: cannot build the prompt\n");
        return 1;
    }
    flower_snprintf(prompt_text, sizeof prompt_text,
             "Resume action %s (%s, type %s, state %s) of task %s-%s. Work "
             "the action now with the tools: read the task and project "
             "data, do the work the description asks for, and report what "
             "you did. When the work changes flower data files, write "
             "them as flower blocks. When done, tell me the new state "
             "(Pending, In Progress, Paused, Completed) and outcome "
             "(Success, Partial, Failure) to record.",
             a->id, a->name, a->type, a->state, p->id, t.id);
    {
        const char *parts[1];

        parts[0] = prompt;
        seed = llm_build_seed(&s, parts, 1, prompt_text);
    }
    free(prompt);
    if (!seed) {
        fprintf(stderr, "flower action resume: cannot build the seed\n");
        return 1;
    }
    flower_path_join(sessions_dir, sizeof sessions_dir, t.dir, "sessions");
    (void)flower_ensure_dir(sessions_dir);
    flower_snprintf(transcript, sizeof transcript, "%s/action-%s.jsonl",
             sessions_dir, a->id);
    rc = llm_converse(&s, seed, transcript);
    free(seed);
    if (rc != 0)
        return rc;
    return action_finish(p, &t, a);
}

int action_cmd(int argc, char **argv)
{
    struct flower_project p;
    int rc;

    if ((rc = need_project(&p, "action")) != 0)
        return rc;
    if (argc < 2) {
        fprintf(stderr, "usage: flower action <list|resume> [args]\n");
        return 2;
    }
    if (!strcmp(argv[1], "list"))
        return action_cmd_list(&p);
    if (!strcmp(argv[1], "resume")) {
        if (argc > 3) {
            fprintf(stderr, "usage: flower action resume [ID]\n");
            return 2;
        }
        return action_cmd_resume(&p, argc == 3 ? argv[2] : "");
    }
    fprintf(stderr, "usage: flower action <list|resume> [args]\n");
    return 2;
}
