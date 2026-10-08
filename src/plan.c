/*
 * plan — the task planner (the llm-assisted "New Task Auto"). See
 * plan.h for the process graph; the pieces here are the three
 * write-back mcp tools (MCP_PLAN), the plan's own research surface
 * (POST /plan/research/mcp), the runner child with its held-open
 * stdin, the reply feed and the conversation capture around all of
 * it. The structure follows src/scan.c — the differences are the
 * interactive lifetime and where the write-backs land.
 */
#define _POSIX_C_SOURCE 200809L

#include "plan.h"

#include "conv.h"
#include "context.h"
#include "fs.h"
#include "prompts.h"
#include "researchers.h"
#include "theme.h"
#include "util.h"

#include <cJSON.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* the planner's runner options: research invokes are whole
 * conversations and a plan is built one add_action at a time, so
 * many rounds per turn and a generous tool timeout */
#define PLAN_MAX_ROUNDS      64
#define PLAN_TOOL_TIMEOUT    600

/* a plan is a tree of actions, not a stream of them — the cap keeps
 * a runaway model from turning a task into a saga */
#define PLAN_ACTIONS_MAX     256

/* the replayable transcript a plan keeps for its next turn, and the
 * ceiling past which it asks the user to start a fresh plan */
#define PLAN_TRANSCRIPT_MAX  (4 * 1024 * 1024)

/* the seed tag: {config}/scan/plan-<…>.jsonl, never colliding with
 * the scan's bare names or the researcher subcommand's debug- ones */
#define PLAN_TAG "plan-"

/* this engine's open sub-conversations, one slot per researcher */
static char SUBS[RESEARCHERS_MAX][CONV_ID_LEN + 1];

/* the runner's tool calls, in flight until their response record
 * arrives (a FIFO: llmkit answers calls in order) */
typedef struct {
    char tool[CONV_TOOL_MAX];
    char sub[CONV_ID_LEN + 1]; /* the invoke's conversation, if any */
} pend_t;

#define PEND_MAX 16
static pend_t PEND[PEND_MAX];
static size_t pend_head, pend_n;

static void pend_push(const char *tool, const char *sub)
{
    if (pend_n == PEND_MAX) { /* overflow: forget the oldest */
        pend_head = (pend_head + 1) % PEND_MAX;
        pend_n--;
    }
    pend_t *p = &PEND[(pend_head + pend_n) % PEND_MAX];
    snprintf(p->tool, sizeof p->tool, "%s", tool ? tool : "");
    snprintf(p->sub, sizeof p->sub, "%s", sub ? sub : "");
    pend_n++;
}

static int pend_pop(pend_t *out)
{
    if (!pend_n) return 0;
    *out = PEND[pend_head];
    pend_head = (pend_head + 1) % PEND_MAX;
    pend_n--;
    return 1;
}

static struct {
    /* wiring (plan_attach) */
    int epfd;
    projects_t *projects;
    llms_t *llms;
    tasks_t *tasks;
    char llmkit[512];
    char base_url[CFG_URL_MAX + 32];

    /* the running plan, or the finished one until the next starts */
    int running;      /* the conversation is open (waits between turns) */
    int turn_active;  /* …and a runner child is playing a turn now */
    int done;
    int ok;
    int stop_requested;
    char error[288];
    char last_error[288]; /* last error record the runner printed */
    char project[PROJECT_ID_LEN + 1];
    char llm[CFG_NAME_MAX];
    char task[TASK_ID_LEN + 1];
    long long started, ended;
    int writes; /* successful write-back tool calls */

    char conv[CONV_ID_LEN + 1]; /* the plan's main conversation */

    /* the replayable transcript: every user turn and every runner
     * record of this conversation, verbatim. A turn is one runner
     * child (llmkit ends a conversation with its final response);
     * the next turn's feed is the config + this transcript + the new
     * user record — input + previous output + a new turn, exactly
     * the continuation contract of llmkit's own examples. */
    sbuf_t turns;

    /* streamed text blocks arrive as partials: accumulate, record
     * once */
    sbuf_t resp_acc, think_acc;

    /* child plumbing (one child per turn) */
    pid_t pid; /* -1 when no child */
    int out_fd;
    sbuf_t out; /* the runner's stdout, folded line by line */
} P;

/* ---------- the write-back tools (POST /plan/mcp) ---------- */

/* the task the running plan writes into, or NULL + err */
static task_t *plan_task(char *err, size_t err_n)
{
    if (!P.running) {
        snprintf(err, err_n, "no task plan is running");
        return NULL;
    }
    int i = tasks_find_id(P.tasks, P.task);
    if (i < 0) {
        snprintf(err, err_n,
                 "the planned task is no longer in the list");
        return NULL;
    }
    return &P.tasks->items[i];
}

/* append `s` at line[n..), returning the new length; a string that
 * does not fit whole is cut and marked with an ellipsis */
static size_t line_add(char *dst, size_t size, size_t n, const char *s)
{
    if (n >= size - 1) return n;
    size_t len = strlen(s), room = size - 1 - n;
    if (len > room) {
        size_t cut = room >= 3 ? room - 3 : room;
        memcpy(dst + n, s, cut);
        if (room >= 3) memcpy(dst + n + cut, "…", 3);
        n += cut + (room >= 3 ? 3 : 0);
    } else {
        memcpy(dst + n, s, len);
        n += len;
    }
    dst[n] = '\0';
    return n;
}

/* one level of the task's action tree as "path title — needs …"
 * lines — the model's ground truth for parent paths, dependency
 * links and what it has built */
static void tree_lines(const action_t *list, size_t depth, size_t *idx,
                       sbuf_t *b)
{
    char line[ACTION_TITLE_MAX + 6 * ACTION_DEPTH_MAX +
              (ACTION_DEP_PATH_MAX + 2) * ACTION_DEPS_MAX + 32];
    char piece[ACTION_DEP_PATH_MAX + 8];
    for (size_t i = 0; list; list = list->next_sibling, i++) {
        idx[depth] = i;
        size_t n = 0;
        line[0] = '\0';
        for (size_t d = 0; d <= depth; d++) {
            snprintf(piece, sizeof piece, d ? ".%zu" : "%zu", idx[d]);
            n = line_add(line, sizeof line, n, piece);
        }
        snprintf(piece, sizeof piece, " %s", list->title);
        n = line_add(line, sizeof line, n, piece);
        for (int k = 0; k < list->dep_count; k++) {
            snprintf(piece, sizeof piece, "%s%s",
                     k ? ", " : " — needs ", list->depends_on[k]);
            n = line_add(line, sizeof line, n, piece);
        }
        n = line_add(line, sizeof line, n, "\n");
        sb_puts(b, line);
        if (list->first_child && depth + 1 < ACTION_DEPTH_MAX)
            tree_lines(list->first_child, depth + 1, idx, b);
    }
}

static void tree_summary(const task_t *t, sbuf_t *b)
{
    size_t idx[ACTION_DEPTH_MAX];
    tree_lines(t->actions, 0, idx, b);
}

/* a tool result: "<head>\n<tree>", the tree truncated on a utf-8
 * boundary (a big plan should not swamp the model's context) */
static char *plan_result(const char *head, const task_t *t)
{
    sbuf_t b = { 0 };
    sb_puts(&b, head);
    sb_putc(&b, '\n');
    tree_summary(t, &b);
    if (!b.data) return strdup(head);
    char out[2048 + 8];
    utf8_trunc(out, sizeof out, b.data, 2048);
    free(b.data);
    return strdup(out);
}

static char *fn_set_task_title(const cJSON *args, char *err, size_t err_n)
{
    task_t *t = plan_task(err, err_n);
    if (!t) return NULL;
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(args, "title");
    if (!cJSON_IsString(j) || !j->valuestring || !j->valuestring[0] ||
        strlen(j->valuestring) >= sizeof t->title ||
        !valid_utf8_text(j->valuestring, strlen(j->valuestring), 0)) {
        snprintf(err, err_n, "title: required, max %d bytes, no control "
                             "characters", TASK_TITLE_MAX - 1);
        return NULL;
    }
    snprintf(t->title, sizeof t->title, "%s", j->valuestring);
    if (tasks_save(P.tasks) != 0) {
        snprintf(err, err_n, "cannot write the task");
        return NULL;
    }
    P.writes++;
    return plan_result("title set", t);
}

/* "0.2" -> the sibling list that path names, walking from the root;
 * NULL + err when the path names nothing */
static action_t **find_parent_list(task_t *t, const char *parent,
                                   char *err, size_t err_n)
{
    if (!parent || !parent[0]) return &t->actions;
    action_t **list = &t->actions;
    const char *p = parent;
    while (*p) {
        if (!(*p >= '0' && *p <= '9')) {
            snprintf(err, err_n, "parent: dot-separated action indexes, "
                                 "like \"0\" or \"0.2\" (empty = the root)");
            return NULL;
        }
        size_t idx = 0;
        while (*p >= '0' && *p <= '9') {
            idx = idx * 10 + (size_t)(*p - '0');
            p++;
        }
        action_t *a = *list;
        for (size_t k = 0; a; a = a->next_sibling, k++)
            if (k == idx) break;
        if (!a) {
            snprintf(err, err_n, "parent: no action at path \"%s\" — "
                                 "the tree follows", parent);
            return NULL;
        }
        list = &a->first_child;
        if (*p == '.') p++;
        else if (*p) {
            snprintf(err, err_n, "parent: dot-separated action indexes, "
                                 "like \"0\" or \"0.2\" (empty = the root)");
            return NULL;
        }
    }
    return list;
}

static char *fn_add_action(const cJSON *args, char *err, size_t err_n)
{
    task_t *t = plan_task(err, err_n);
    if (!t) return NULL;

    const cJSON *j = cJSON_GetObjectItemCaseSensitive(args, "title");
    if (!cJSON_IsString(j) || !j->valuestring || !j->valuestring[0] ||
        strlen(j->valuestring) >= ACTION_TITLE_MAX ||
        !valid_utf8_text(j->valuestring, strlen(j->valuestring), 0)) {
        snprintf(err, err_n, "title: required, max %d bytes, no control "
                             "characters", ACTION_TITLE_MAX - 1);
        return NULL;
    }
    const char *title = j->valuestring;

    const char *desc = "";
    j = cJSON_GetObjectItemCaseSensitive(args, "description");
    if (cJSON_IsString(j) && j->valuestring) desc = j->valuestring;
    if (strlen(desc) >= ACTION_DESC_MAX ||
        !valid_utf8_text(desc, strlen(desc), 1)) {
        snprintf(err, err_n, "description: max %d bytes, no control "
                             "characters except newlines",
                 ACTION_DESC_MAX - 1);
        return NULL;
    }

    int type = ACTION_ACT;
    j = cJSON_GetObjectItemCaseSensitive(args, "type");
    if (cJSON_IsString(j) && j->valuestring) {
        type = action_type_from_name(j->valuestring);
        if (type < 0) {
            snprintf(err, err_n, "type: observe, analyze, find_root_cause, "
                                 "act, validate or improve");
            return NULL;
        }
    }

    const char *parent = "";
    j = cJSON_GetObjectItemCaseSensitive(args, "parent");
    if (cJSON_IsString(j) && j->valuestring) parent = j->valuestring;
    else if (j && !cJSON_IsString(j)) {
        snprintf(err, err_n, "parent: dot-separated action indexes, like "
                             "\"0\" or \"0.2\" (empty = the root)");
        return NULL;
    }

    /* dependency links: paths of actions this one waits for, the
     * same paths the tree listings print. Each must name an action
     * that already exists — checked before the append, so a path
     * equal to the new action's own future path fails as "names
     * nothing" (self-reference included). Links to existing
     * actions cannot form a circle: nothing depends on the new
     * action yet */
    char deps[ACTION_DEPS_MAX][ACTION_DEP_PATH_MAX];
    int dep_count = 0;
    j = cJSON_GetObjectItemCaseSensitive(args, "depends_on");
    if (j && !cJSON_IsArray(j)) {
        snprintf(err, err_n, "depends_on: an array of action paths, "
                             "like [\"0\", \"1.2\"]");
        return NULL;
    }
    const cJSON *dep = NULL;
    cJSON_ArrayForEach(dep, j) {
        if (!cJSON_IsString(dep) || !dep->valuestring ||
            !action_dep_path_ok(dep->valuestring)) {
            snprintf(err, err_n, "depends_on: dot-separated action "
                                 "indexes from the tree listing, like "
                                 "\"0\" or \"1.2\"");
            return NULL;
        }
        if (dep_count >= ACTION_DEPS_MAX) {
            snprintf(err, err_n, "depends_on: at most %d links",
                     ACTION_DEPS_MAX);
            return NULL;
        }
        int dup = 0;
        for (int k = 0; k < dep_count; k++)
            if (strcmp(deps[k], dep->valuestring) == 0) dup = 1;
        if (dup) continue; /* one copy is enough */
        snprintf(deps[dep_count++], ACTION_DEP_PATH_MAX, "%s",
                 dep->valuestring);
    }

    if (task_count_actions(t) >= PLAN_ACTIONS_MAX) {
        snprintf(err, err_n, "the task already holds %d actions",
                 PLAN_ACTIONS_MAX);
        return NULL;
    }

    action_t **list = find_parent_list(t, parent, err, err_n);
    if (!list) {
        /* the path names nothing: hand the model the tree it missed */
        sbuf_t b = { 0 };
        sb_puts(&b, err);
        sb_putc(&b, '\n');
        tree_summary(t, &b);
        if (b.data) {
            char out[2048 + 8];
            utf8_trunc(out, sizeof out, b.data, 2048);
            free(b.data);
            return strdup(out);
        }
        return NULL;
    }
    for (int k = 0; k < dep_count; k++)
        if (!actions_resolve(t->actions, deps[k])) {
            /* a dependency path names nothing: the tree follows,
             * like a bad parent path */
            snprintf(err, err_n, "depends_on: no action at path \"%s\" "
                                 "— the tree follows", deps[k]);
            return plan_result(err, t);
        }
    while (*list) list = &(*list)->next_sibling; /* append at the tail */

    action_t *a = calloc(1, sizeof *a);
    if (!a) {
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    snprintf(a->title, sizeof a->title, "%s", title);
    snprintf(a->description, sizeof a->description, "%s", desc);
    a->type = type;
    a->state = ACTION_PENDING; /* the states are the user's */
    a->dep_count = dep_count;
    memcpy(a->depends_on, deps, (size_t)dep_count * ACTION_DEP_PATH_MAX);
    *list = a;

    if (tasks_save(P.tasks) != 0) {
        *list = NULL; /* the store stays as it was on disk */
        free(a);
        snprintf(err, err_n, "cannot write the task");
        return NULL;
    }
    P.writes++;
    char head[192];
    snprintf(head, sizeof head, "added action %s: %s",
             parent && parent[0] ? parent : "root", a->title);
    return plan_result(head, t);
}

static char *fn_clear_actions(const cJSON *args, char *err, size_t err_n)
{
    task_t *t = plan_task(err, err_n);
    if (!t) return NULL;
    (void)args; /* nothing to read: the whole tree goes */
    size_t n = task_count_actions(t);
    task_clear_actions(t);
    if (tasks_save(P.tasks) != 0) {
        snprintf(err, err_n, "cannot write the task");
        return NULL;
    }
    P.writes++;
    char head[64];
    snprintf(head, sizeof head, "cleared %zu action%s", n, n == 1 ? "" : "s");
    return plan_result(head, t);
}

static const mcp_arg_t ARGS_SET_TITLE[] = {
    { "title", "string", 1 },
    { NULL }
};
static const mcp_arg_t ARGS_ADD_ACTION[] = {
    { "title",       "string", 1 },
    { "description", "string", 0 },
    { "type",        "string", 0 },
    { "parent",      "string", 0 },
    { "depends_on",  "array",  0 },
    { NULL }
};
static const mcp_tool_t PLAN_TOOLS[] = {
    { "set_task_title", ARGS_SET_TITLE,  fn_set_task_title },
    { "add_action",     ARGS_ADD_ACTION, fn_add_action },
    { "clear_actions",  NULL,            fn_clear_actions },
};
mcp_table_t MCP_PLAN = {
    PLAN_TOOLS, sizeof PLAN_TOOLS / sizeof PLAN_TOOLS[0], NULL
};

/* ---------- the plan's research surface (POST /plan/research/mcp) */

static void args_brief(const cJSON *args, char *dst, size_t n)
{
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(args, "input");
    if (cJSON_IsString(input) && input->valuestring) {
        utf8_trunc(dst, n, input->valuestring, 400);
        return;
    }
    char *j = cJSON_PrintUnformatted(args);
    if (!j) {
        dst[0] = '\0';
        return;
    }
    utf8_trunc(dst, n, j, 400);
    free(j);
}

static int researcher_index(const researcher_def_t *r)
{
    return r ? (int)(r - researchers()) : -1;
}

static int invoke_index(const char *tool)
{
    if (!tool) return -1;
    for (size_t i = 0; i < researchers_count(); i++) {
        const char *agent = researchers()[i].agent;
        size_t alen = strlen(agent);
        if (strncmp(tool, agent, alen) == 0 &&
            strcmp(tool + alen, ".invoke") == 0)
            return (int)i;
    }
    return -1;
}

static int sub_index(const char *sub)
{
    if (!sub || !sub[0]) return -1;
    for (size_t i = 0; i < researchers_count(); i++)
        if (strcmp(SUBS[i], sub) == 0) return (int)i;
    return -1;
}

static void open_sub(int idx, const char *input)
{
    input = input ? input : "";
    char line[256], title[CONV_TITLE_MAX];
    size_t i = 0;
    while (input[i] && input[i] != '\n' && i < sizeof line - 1) {
        line[i] = input[i];
        i++;
    }
    line[i] = '\0';
    utf8_trunc(title, sizeof title, line, 96);
    if (!title[0])
        snprintf(title, sizeof title, "%s round",
                 researchers()[idx].agent);

    char id[CONV_ID_LEN + 1];
    if (conv_create(researchers()[idx].agent, P.llm, P.project, P.conv,
                    title, 0, id) != 0)
        return; /* the call still lands in the main conversation */
    snprintf(SUBS[idx], sizeof SUBS[idx], "%s", id);
    conv_add(id, "user", NULL, input, 0);
}

static void close_sub(const char *sub, int state, const char *answer)
{
    conv_add(sub, "response", NULL, answer ? answer : "", 0);
    conv_finish(sub, (conv_state_t)state);
    int idx = sub_index(sub);
    if (idx >= 0) SUBS[idx][0] = '\0';
}

/* the fs researcher's own tool calls, folded into its open
 * sub-conversation — the plan's private surface keeps a concurrent
 * scan's transcripts untangled */
static void plan_log_research(const char *tool, const cJSON *args,
                              const char *result, int is_error)
{
    if (!P.running) return;
    int idx = researcher_index(researcher_for_tool(tool));
    if (idx < 0 || !SUBS[idx][0]) return;
    char brief[448];
    args_brief(args, brief, sizeof brief);
    conv_add(SUBS[idx], "tool_call", tool, brief, 0);
    conv_add(SUBS[idx], "tool_result", tool, result ? result : "", is_error);
}

char *plan_research_post(const char *body, size_t len, int *is_notification)
{
    if (!P.running) return NULL;
    int pi = projects_find_id(P.projects, P.project);
    if (pi < 0) return NULL;
    fs_set_root(P.projects->items[pi].dir);
    mcp_table_t t = MCP_RESEARCH; /* the tools, the plan's own logger */
    t.log = plan_log_research;
    char *j = mcp_handle_post(&t, body, len, is_notification);
    fs_set_root(NULL);
    return j;
}

/* ---------- the runner's stdin records ---------- */

static char *render_prompt_vars(const char *path, const project_t *proj,
                                const prompt_var_t *vars, size_t n,
                                const char *fallback)
{
    char *tpl = prompt_text(path);
    char *out = tpl ? prompt_render_vars(tpl, proj, vars, n) : NULL;
    free(tpl);
    if (out && *out) return out;
    free(out);
    return strdup(fallback);
}

/* the opening user record: the planning prompt wrapped in the
 * project's context (a missing template degrades to the raw prompt) */
static char *plan_user_prompt(const project_t *proj, const char *prompt)
{
    prompt_var_t vars[] = { { "request", prompt } };
    return render_prompt_vars("agents/task_planner/user_prompt.txt", proj,
                              vars, sizeof vars / sizeof vars[0], prompt);
}

/* one user turn into the replayable transcript (the exact line the
 * runner gets, so the next turn replays it verbatim) */
static int append_user_turn(const char *text)
{
    cJSON *rec = rec_text("user", text);
    int rc = rec_put(&P.turns, rec);
    cJSON_Delete(rec);
    return rc;
}

/* one turn's feed: header, llm, the tools (the write-back surface
 * plus the two researchers, their seeds written here), options,
 * system — then the transcript so far and the flush that starts the
 * turn. The new user record is already in P.turns. */
static char *build_runner_input(const agent_t *planner, const llm_t *llm,
                                const project_t *proj,
                                char *err, size_t err_n)
{
    sbuf_t b = { 0 };
    int rc = 0;
    cJSON *rec;

    rec = cJSON_Parse("{\"type\":\"header\",\"version\":1}");
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    rec = agents_llm_record(planner, llm);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    rec = cJSON_CreateObject();
    cJSON *servers = agents_tools_record(planner, P.base_url, 0);
    if (!rec || !servers ||
        !cJSON_AddStringToObject(rec, "type", "tools")) {
        cJSON_Delete(servers);
        cJSON_Delete(rec);
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    /* the write-back surface: this plan's own flower server */
    cJSON *srv = cJSON_CreateObject();
    char url[CFG_URL_MAX + 64];
    snprintf(url, sizeof url, "%s/plan/mcp", P.base_url);
    if (srv &&
        cJSON_AddStringToObject(srv, "type", "http") &&
        cJSON_AddStringToObject(srv, "name", "flower") &&
        cJSON_AddStringToObject(srv, "url", url) &&
        cJSON_AddBoolToObject(srv, "required", 1)) {
        cJSON_AddItemToArray(servers, srv);
    } else {
        cJSON_Delete(srv);
        cJSON_Delete(servers);
        cJSON_Delete(rec);
        snprintf(err, err_n, "cannot assemble the write-back server");
        return NULL;
    }
    /* the researchers, their fs tools grounded in the project through
     * the plan's own research surface */
    char fs_url[CFG_URL_MAX + 64];
    snprintf(fs_url, sizeof fs_url, "%s/plan/research/mcp", P.base_url);
    for (size_t i = 0; i < researchers_count(); i++) {
        if (researcher_add_server(servers, &researchers()[i], llm, proj,
                                  PLAN_TAG, P.llmkit, fs_url,
                                  err, err_n) != 0) {
            cJSON_Delete(servers);
            cJSON_Delete(rec);
            return NULL;
        }
    }
    cJSON_AddItemToObject(rec, "tools", servers);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    rec = cJSON_CreateObject();
    if (rec && cJSON_AddStringToObject(rec, "type", "options")) {
        cJSON_AddNumberToObject(rec, "max_tool_rounds", PLAN_MAX_ROUNDS);
        cJSON_AddNumberToObject(rec, "tool_call_timeout",
                                PLAN_TOOL_TIMEOUT);
    }
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    char *system = prompt_text("agents/task_planner/system_prompt.txt");
    char *rendered = system ? prompt_render(system, proj) : NULL;
    free(system);
    if (!rendered || !rendered[0]) {
        free(rendered);
        rendered = strdup("You are a task planning agent.");
    }
    rec = rec_text("system", rendered);
    free(rendered);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    /* the conversation so far — and the flush that starts the turn */
    if (P.turns.data) {
        rc |= sb_putn(&b, P.turns.data, P.turns.len);
    }
    rec = cJSON_Parse("{\"type\":\"flush\"}");
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    if (rc != 0 || !b.data) {
        free(b.data);
        snprintf(err, err_n, "cannot assemble the runner records");
        return NULL;
    }
    return b.data;
}

/* ---------- stdout: fold records into the conversations ---------- */

/* the runner records that belong to the conversation's replayable
 * transcript: what the next turn's feed carries back to the model */
static int replayable(const char *t)
{
    return strcmp(t, "thinking") == 0 || strcmp(t, "response") == 0 ||
           strcmp(t, "tool_request") == 0 || strcmp(t, "tool_response") == 0;
}

static void handle_line(char *line)
{
    if (!line[0]) return;
    cJSON *rec = cJSON_Parse(line);
    if (!rec) return; /* not a record: nothing to fold in */

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(rec, "type");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(rec, "text");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    const char *x = cJSON_IsString(text) && text->valuestring
                        ? text->valuestring : "";
    /* the conversation keeps going across turns: every record of the
     * model's side goes into the transcript verbatim (header, start
     * and error records are not part of the replay) */
    if (replayable(t))
        if (sb_puts(&P.turns, line) != 0 || sb_putc(&P.turns, '\n') != 0) {
            cJSON_Delete(rec);
            return; /* OOM: the turn still lands in the conversation */
        }
    /* partial defaults to true on the wire; only a closed block is
     * recorded */
    const cJSON *partial = cJSON_GetObjectItemCaseSensitive(rec, "partial");
    int fin = cJSON_IsBool(partial) && !cJSON_IsTrue(partial);

    if (strcmp(t, "response") == 0) {
        sb_puts(&P.resp_acc, x);
        if (fin) {
            conv_add(P.conv, "response", NULL,
                     P.resp_acc.data ? P.resp_acc.data : "", 0);
            P.resp_acc.len = 0;
            if (P.resp_acc.data) P.resp_acc.data[0] = '\0';
        }
    } else if (strcmp(t, "thinking") == 0) {
        sb_puts(&P.think_acc, x);
        if (fin) {
            conv_add(P.conv, "thinking", NULL,
                     P.think_acc.data ? P.think_acc.data : "", 0);
            P.think_acc.len = 0;
            if (P.think_acc.data) P.think_acc.data[0] = '\0';
        }
    } else if (strcmp(t, "tool_request") == 0) {
        const cJSON *tool = cJSON_GetObjectItemCaseSensitive(rec, "tool");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(rec, "arguments");
        const char *tn = cJSON_IsString(tool) && tool->valuestring
                             ? tool->valuestring : "";
        char brief[448];
        args_brief(args, brief, sizeof brief);
        /* a researcher invoke opens the researcher's own
         * sub-conversation, linked to this plan's */
        int idx = invoke_index(tn);
        if (idx >= 0) {
            const cJSON *input =
                cJSON_GetObjectItemCaseSensitive(args, "input");
            open_sub(idx, cJSON_IsString(input) && input->valuestring
                          ? input->valuestring : "");
        }
        conv_add(P.conv, "tool_call", tn, brief, 0);
        pend_push(tn, idx >= 0 ? SUBS[idx] : "");
    } else if (strcmp(t, "tool_response") == 0) {
        /* answers arrive in call order: the FIFO entry says whether
         * the call was an invoke whose conversation closes here */
        pend_t p;
        if (pend_pop(&p)) {
            if (p.sub[0]) close_sub(p.sub, CONV_COMPLETED, x);
            conv_add(P.conv, "tool_result", p.tool[0] ? p.tool : NULL, x, 0);
        } else {
            conv_add(P.conv, "tool_result", NULL, x, 0);
        }
    } else if (strcmp(t, "error") == 0) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(rec, "code");
        char msg[sizeof P.last_error];
        snprintf(msg, sizeof msg, "%s: %s",
                 cJSON_IsString(code) && code->valuestring
                     ? code->valuestring : "error", x);
        conv_add(P.conv, "error", NULL, msg, 1);
        snprintf(P.last_error, sizeof P.last_error, "%s", msg);
    }
    cJSON_Delete(rec);
}

static void process_lines(void)
{
    if (!P.out.data) return;
    size_t start = 0;
    for (size_t i = 0; i < P.out.len; i++) {
        if (P.out.data[i] != '\n') continue;
        P.out.data[i] = '\0';
        handle_line(P.out.data + start);
        start = i + 1;
    }
    if (start) {
        memmove(P.out.data, P.out.data + start, P.out.len - start);
        P.out.len -= start;
        P.out.data[P.out.len] = '\0';
    }
}

/* ---------- lifecycle ---------- */

/* the conversation is over: close every open researcher
 * sub-conversation (children first), then the plan's own, and drop
 * the run's state (the seeds go too — the next turn rewrites them) */
static void plan_close(conv_state_t state, const char *reason)
{
    for (size_t i = 0; i < researchers_count(); i++)
        if (SUBS[i][0]) {
            conv_add(SUBS[i], "error", NULL,
                     "the plan ended before this conversation finished", 1);
            conv_finish(SUBS[i], CONV_STOPPED);
            SUBS[i][0] = '\0';
        }
    if (P.conv[0]) {
        if (reason && reason[0]) conv_add(P.conv, "error", NULL, reason, 1);
        conv_finish(P.conv, state);
    }
    pend_head = pend_n = 0;
    free(P.out.data);
    P.out.data = NULL;
    P.out.len = P.out.cap = 0;
    free(P.turns.data);
    P.turns = (sbuf_t){ 0 };
    researchers_unlink_seeds(PLAN_TAG);
    P.ok = state == CONV_COMPLETED;
    if (reason && reason[0])
        snprintf(P.error, sizeof P.error, "%s", reason);
    else
        P.error[0] = '\0';
    P.stop_requested = 0;
    P.turn_active = 0;
    P.pid = -1;
    P.out_fd = -1;
    P.running = 0;
    P.done = 1;
    P.ended = (long long)time(NULL);
}

/* A turn's child reached EOF. A clean turn (exit 0) ends the
 * conversation *turn* — llmkit stops at the final response — but the
 * plan stays open and interactive: the next reply replays this
 * transcript into a fresh child. A failure or a stop closes the
 * whole conversation. */
static void plan_turn_end(void)
{
    if (P.out_fd >= 0) {
        epoll_ctl(P.epfd, EPOLL_CTL_DEL, P.out_fd, NULL);
        close(P.out_fd);
        P.out_fd = -1;
    }
    process_lines(); /* whatever the last chunk held */
    /* trailing partials mean the turn was cut short — show them */
    if (P.resp_acc.len) {
        conv_add(P.conv, "response", NULL, P.resp_acc.data, 0);
        /* …and keep them replayable, as the stream denoted them */
        cJSON *rec = rec_text("response", P.resp_acc.data);
        if (rec) {
            char *line = cJSON_PrintUnformatted(rec);
            if (line) {
                sb_puts(&P.turns, line);
                sb_putc(&P.turns, '\n');
                free(line);
            }
            cJSON_Delete(rec);
        }
    }
    if (P.think_acc.len) {
        conv_add(P.conv, "thinking", NULL, P.think_acc.data, 0);
        cJSON *rec = rec_text("thinking", P.think_acc.data);
        if (rec) {
            char *line = cJSON_PrintUnformatted(rec);
            if (line) {
                sb_puts(&P.turns, line);
                sb_putc(&P.turns, '\n');
                free(line);
            }
            cJSON_Delete(rec);
        }
    }
    free(P.resp_acc.data); free(P.think_acc.data);
    P.resp_acc = (sbuf_t){ 0 };
    P.think_acc = (sbuf_t){ 0 };

    int code = 0;
    if (P.pid > 0) {
        int st = 0;
        while (waitpid(P.pid, &st, 0) < 0 && errno == EINTR) { }
        P.pid = -1;
        if (WIFEXITED(st)) code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) code = -WTERMSIG(st);
        else code = -1;
    }

    /* still-open researcher sub-conversations end with the turn */
    for (size_t i = 0; i < researchers_count(); i++)
        if (SUBS[i][0]) {
            conv_add(SUBS[i], "response", NULL,
                     "(the turn ended before this research round did)", 0);
            conv_finish(SUBS[i], CONV_COMPLETED);
            SUBS[i][0] = '\0';
        }
    pend_head = pend_n = 0;

    if (P.stop_requested) {
        plan_close(CONV_STOPPED, "plan stopped");
        return;
    }
    if (code == 0) {
        /* the turn is done; the conversation waits for the next reply */
        P.turn_active = 0;
        P.done = 0;
        return;
    }

    char msg[sizeof P.error];
    if (P.last_error[0])
        snprintf(msg, sizeof msg, "%s", P.last_error);
    else if (code < 0)
        snprintf(msg, sizeof msg, "llmkit runner killed by signal %d", -code);
    else
        snprintf(msg, sizeof msg, "llmkit runner exited with code %d", code);
    plan_close(CONV_FAILED, msg);
}

void plan_on_readable(void)
{
    char chunk[8192];
    for (;;) {
        ssize_t n = read(P.out_fd, chunk, sizeof chunk);
        if (n > 0) {
            if (sb_putn(&P.out, chunk, (size_t)n) != 0)
                break; /* OOM: treat what we have as the end */
            process_lines();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        break; /* EOF or hard error: the runner is done */
    }
    plan_turn_end();
}

/* ---------- public surface ---------- */

void plan_attach(int epfd, projects_t *projects, llms_t *llms,
                 tasks_t *tasks, const char *llmkit, const char *base_url)
{
    P.epfd = epfd;
    P.projects = projects;
    P.llms = llms;
    P.tasks = tasks;
    snprintf(P.llmkit, sizeof P.llmkit, "%s",
             llmkit && llmkit[0] ? llmkit : "llmkit");
    snprintf(P.base_url, sizeof P.base_url, "%s",
             base_url && base_url[0] ? base_url : "http://127.0.0.1:8080");
    P.pid = -1;
    P.out_fd = -1;
    P.conv[0] = '\0';
}

/* the task a plan starts as: title from the prompt's first line, an
 * empty tree — the agent fills both through the write-back tools */
static int add_plan_task(const char *project_id, const char *prompt,
                         char *id_out, char *err, size_t err_n)
{
    if (P.tasks->count >= TASKS_MAX) {
        snprintf(err, err_n, "the task list is full (%d)", TASKS_MAX);
        return -1;
    }

    task_t *t = &P.tasks->items[0]; /* newest-first: the front */
    memmove(P.tasks->items + 1, P.tasks->items,
            P.tasks->count * sizeof *P.tasks->items);
    P.tasks->count++;
    memset(t, 0, sizeof *t);

    char line[TASK_TITLE_MAX + 8];
    utf8_trunc(line, sizeof line, prompt, TASK_TITLE_MAX - 1);
    size_t i = 0;
    while (line[i] && line[i] != '\n' && i < sizeof line - 1) i++;
    line[i] = '\0';
    if (!valid_utf8_text(line, strlen(line), 0))
        line[0] = '\0'; /* controls (a tab): not a title */
    snprintf(t->title, sizeof t->title, "%s", line[0] ? line : "New task");
    snprintf(t->project, sizeof t->project, "%s", project_id);
    t->created = (long long)time(NULL);

    char dir[4352 + 64], root[4352];
    snprintf(root, sizeof root, "%s/tasks", theme_dir());
    do {
        gen_hex_id(t->id, TASK_ID_LEN);
        snprintf(dir, sizeof dir, "%s/%s", root, t->id);
        struct stat st;
        if (stat(dir, &st) == 0) continue; /* taken: another draw */
        int clash = 0;
        for (size_t k = 1; k < P.tasks->count; k++)
            if (strcmp(P.tasks->items[k].id, t->id) == 0) clash = 1;
        if (!clash) break;
    } while (1);

    if (mkdir(root, 0700) != 0 && errno != EEXIST) {
        P.tasks->count--;
        memmove(P.tasks->items, P.tasks->items + 1,
                P.tasks->count * sizeof *P.tasks->items);
        snprintf(err, err_n, "cannot create the tasks directory");
        return -1;
    }
    if (tasks_save(P.tasks) != 0) {
        P.tasks->count--;
        memmove(P.tasks->items, P.tasks->items + 1,
                P.tasks->count * sizeof *P.tasks->items);
        snprintf(err, err_n, "cannot write the task");
        return -1;
    }
    snprintf(id_out, TASK_ID_LEN + 1, "%s", t->id);
    return 0;
}

/* the task a failed spawn leaves behind goes away again: the store
 * without it, its directory too */
static void drop_plan_task(const char *task_id)
{
    int i = tasks_find_id(P.tasks, task_id);
    if (i < 0) return;
    task_clear_actions(&P.tasks->items[i]); /* the heap goes with it */
    context_free(P.tasks->items[i].context);
    P.tasks->items[i].context = NULL;
    char dir[4352 + 64];
    snprintf(dir, sizeof dir, "%s/tasks/%s/task.json", theme_dir(),
             task_id);
    unlink(dir);
    snprintf(dir, sizeof dir, "%s/tasks/%s", theme_dir(), task_id);
    rmdir(dir);
    memmove(P.tasks->items + i, P.tasks->items + i + 1,
            (P.tasks->count - (size_t)i - 1) * sizeof *P.tasks->items);
    P.tasks->count--;
    tasks_save(P.tasks);
}

plan_start_result_t plan_start(const char *project_id, const char *llm_name,
                               const char *prompt,
                               char *task_out, char *conv_out,
                               char *err, size_t err_n)
{
    if (P.running) return PLAN_START_BUSY;
    if (!prompt || !prompt[0] ||
        strlen(prompt) >= PLAN_PROMPT_MAX ||
        !valid_utf8_text(prompt, strlen(prompt), 1)) {
        snprintf(err, err_n,
                 "prompt: required, max %d bytes, no control characters "
                 "except newlines", PLAN_PROMPT_MAX - 1);
        return PLAN_START_REJECT;
    }

    int pi = projects_find_id(P.projects, project_id);
    if (pi < 0) {
        snprintf(err, err_n, "unknown project id");
        return PLAN_START_REJECT;
    }
    project_t *p = &P.projects->items[pi];
    struct stat st;
    if (stat(p->dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, err_n,
                 "the project's working directory is missing on disk");
        return PLAN_START_REJECT;
    }
    const llm_t *llm = llms_get(P.llms, llm_name);
    if (!llm) {
        snprintf(err, err_n,
                 "unknown llm '%s' — add it on the config page first",
                 llm_name ? llm_name : "");
        return PLAN_START_REJECT;
    }
    const agent_t *planner = agents_builtin_get("task_planner");
    if (!planner) {
        snprintf(err, err_n, "the task_planner agent is missing");
        return PLAN_START_REJECT;
    }

    /* the task this plan fills, created before anything spawns so a
     * failure leaves nothing behind */
    char task_id[TASK_ID_LEN + 1];
    if (add_plan_task(project_id, prompt, task_id, err, err_n) != 0)
        return PLAN_START_REJECT;

    /* the conversation the plan is recorded in — interactive, so the
     * Conversations page keeps offering the reply box while it runs */
    char title[CONV_TITLE_MAX];
    char line[96];
    utf8_trunc(line, sizeof line, prompt, 80);
    size_t i = 0;
    while (line[i] && line[i] != '\n' && i < sizeof line - 1) i++;
    line[i] = '\0';
    snprintf(title, sizeof title, "Task plan — %s",
             line[0] ? line : p->title);
    if (conv_create("task_planner", llm_name ? llm_name : "", project_id,
                    "", title, 1, P.conv) != 0) {
        drop_plan_task(task_id);
        snprintf(err, err_n, "cannot create the conversation record");
        return PLAN_START_SPAWN;
    }
    char *user = plan_user_prompt(p, prompt);
    conv_add(P.conv, "user", NULL, user, 0);

    /* fresh state for this run (the previous result is forgotten),
     * then the opening turn: the transcript starts with the user
     * record the runner is fed — and will replay */
    P.done = 0;
    P.ok = 0;
    P.error[0] = P.last_error[0] = '\0';
    snprintf(P.project, sizeof P.project, "%s", project_id);
    snprintf(P.llm, sizeof P.llm, "%s", llm_name ? llm_name : "");
    snprintf(P.task, sizeof P.task, "%s", task_id);
    P.started = (long long)time(NULL);
    P.ended = 0;
    P.writes = 0;
    P.stop_requested = 0;
    P.turn_active = 0;
    free(P.turns.data);
    P.turns = (sbuf_t){ 0 };
    pend_head = pend_n = 0;

    int failed = append_user_turn(user) != 0;
    free(user);
    char *input = failed ? NULL
                         : build_runner_input(planner, llm, p, err, err_n);
    if (!input) {
        if (!err[0]) snprintf(err, err_n, "cannot assemble the runner records");
        conv_add(P.conv, "error", NULL, err, 1);
        conv_finish(P.conv, CONV_FAILED);
        drop_plan_task(task_id);
        P.conv[0] = '\0';
        free(P.turns.data);
        P.turns = (sbuf_t){ 0 };
        return PLAN_START_SPAWN;
    }
    /* one child per turn (see plan_turn_end): the child plays this
     * turn and exits, the conversation stays open for replies */
    if (runner_spawn(P.llmkit, input, P.epfd, &P.pid, &P.out_fd,
                     err, err_n) != 0) {
        free(input);
        conv_add(P.conv, "error", NULL, err, 1);
        conv_finish(P.conv, CONV_FAILED);
        drop_plan_task(task_id);
        P.conv[0] = '\0';
        free(P.turns.data);
        P.turns = (sbuf_t){ 0 };
        return PLAN_START_SPAWN;
    }
    free(input);
    P.out.len = 0;
    if (P.out.data) P.out.data[0] = '\0';

    if (task_out) snprintf(task_out, TASK_ID_LEN + 1, "%s", task_id);
    if (conv_out) snprintf(conv_out, CONV_ID_LEN + 1, "%s", P.conv);
    P.running = 1;
    P.turn_active = 1;
    return PLAN_START_OK;
}

plan_reply_result_t plan_reply(const char *conv_id, const char *text,
                               char *err, size_t err_n)
{
    if (!conv_id || !conv_valid_id(conv_id) || !conv_exists(conv_id))
        return PLAN_REPLY_MISSING;
    if (!P.running || strcmp(P.conv, conv_id) != 0) {
        snprintf(err, err_n, "this conversation is not running");
        return PLAN_REPLY_IDLE;
    }
    if (!text || !text[0] || strlen(text) >= PLAN_REPLY_MAX ||
        !valid_utf8_text(text, strlen(text), 1)) {
        snprintf(err, err_n,
                 "text: required, max %d bytes, no control characters "
                 "except newlines", PLAN_REPLY_MAX - 1);
        return PLAN_REPLY_REJECT;
    }
    if (P.turn_active) {
        /* the model is still answering: one turn at a time */
        snprintf(err, err_n, "the planner is still answering this turn");
        return PLAN_REPLY_IDLE;
    }
    if (P.turns.len > PLAN_TRANSCRIPT_MAX) {
        snprintf(err, err_n,
                 "this plan's transcript is full — stop it and start a "
                 "new one");
        return PLAN_REPLY_REJECT;
    }

    const agent_t *planner = agents_builtin_get("task_planner");
    int pi = projects_find_id(P.projects, P.project);
    const llm_t *llm = llms_get(P.llms, P.llm);
    if (!planner || pi < 0 || !llm) {
        plan_close(CONV_FAILED, "the plan's project, llm or agent is gone");
        snprintf(err, err_n, "the plan can no longer run");
        return PLAN_REPLY_REJECT;
    }

    /* the user's turn joins the transcript, and a fresh child replays
     * it all: input + previous output + the new user record */
    conv_add(P.conv, "user", NULL, text, 0);
    if (append_user_turn(text) != 0) {
        plan_close(CONV_FAILED, "out of memory");
        snprintf(err, err_n, "out of memory");
        return PLAN_REPLY_REJECT;
    }
    P.last_error[0] = '\0'; /* this turn's errors are its own */
    char *input = build_runner_input(planner, llm, &P.projects->items[pi],
                                     err, err_n);
    if (!input) {
        plan_close(CONV_FAILED, err);
        return PLAN_REPLY_REJECT;
    }
    if (runner_spawn(P.llmkit, input, P.epfd, &P.pid, &P.out_fd,
                     err, err_n) != 0) {
        free(input);
        plan_close(CONV_FAILED, err);
        return PLAN_REPLY_REJECT;
    }
    free(input);
    P.out.len = 0;
    if (P.out.data) P.out.data[0] = '\0';
    P.turn_active = 1;
    return PLAN_REPLY_OK;
}

void plan_stop(void)
{
    if (!P.running) return;
    if (P.pid <= 0) { /* idle between turns: nothing to signal */
        plan_close(CONV_STOPPED, "plan stopped");
        return;
    }
    if (!P.stop_requested) {
        P.stop_requested = 1;
        kill(-P.pid, SIGINT); /* orderly: llmkit flushes and exits */
    } else {
        kill(-P.pid, SIGKILL); /* second ask: immediate */
    }
}

int plan_running(void) { return P.running; }

const char *plan_conv_id(void) { return P.conv; }

int plan_fd(void) { return P.running ? P.out_fd : -1; }

char *plan_status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddBoolToObject(o, "running", P.running);
    /* running but no child: the turn is done, a reply starts the next */
    cJSON_AddBoolToObject(o, "awaiting_reply",
                          P.running && !P.turn_active);
    cJSON_AddBoolToObject(o, "done", P.done);
    cJSON_AddStringToObject(o, "project", P.project);
    cJSON_AddStringToObject(o, "llm", P.llm);
    cJSON_AddStringToObject(o, "task", P.task);
    if (P.conv[0])
        cJSON_AddStringToObject(o, "conversation", P.conv);
    if (P.started) cJSON_AddNumberToObject(o, "started", (double)P.started);
    if (P.ended) cJSON_AddNumberToObject(o, "ended", (double)P.ended);
    if (P.done) cJSON_AddBoolToObject(o, "ok", P.ok);
    if (P.done && P.error[0]) cJSON_AddStringToObject(o, "error", P.error);
    cJSON_AddNumberToObject(o, "writes", P.writes);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json;
}

void plan_shutdown(void)
{
    if (P.pid > 0) {
        kill(-P.pid, SIGKILL);
        waitpid(P.pid, NULL, 0);
        P.pid = -1;
    }
    if (P.out_fd >= 0) {
        close(P.out_fd);
        P.out_fd = -1;
    }
    free(P.out.data);
    P.out.data = NULL;
    P.out.len = P.out.cap = 0;
    free(P.turns.data);
    P.turns = (sbuf_t){ 0 };
    researchers_unlink_seeds(PLAN_TAG);
}
