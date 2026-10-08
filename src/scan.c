/*
 * scan — the project scan agent. See scan.h for the process graph;
 * the pieces here are the two write-back mcp tools (MCP_SCAN), the
 * runner child, its stdout feed and the conversation capture around
 * all of it. The researcher seeds and the child plumbing live in
 * src/researchers.c, shared with the task planner (src/plan.c).
 */
#define _POSIX_C_SOURCE 200809L

#include "scan.h"

#include "context.h"
#include "conv.h"
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

/* the orchestrator's runner options: research invokes are whole
 * conversations, so a generous tool timeout and many rounds */
#define SCAN_MAX_ROUNDS      48
#define SCAN_TOOL_TIMEOUT    600

/* how much of a previous run's transcript a follow-up carries */
#define SCAN_PREV_MAX        8192

/* this engine's open sub-conversations, one slot per researcher
 * (the researcher defs themselves live in src/researchers.c) */
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
    /* wiring (scan_attach) */
    int epfd;
    projects_t *projects;
    llms_t *llms;
    char llmkit[512];
    char base_url[CFG_URL_MAX + 32];

    /* the running scan, or the finished one until the next starts */
    int running;
    int done;
    int ok;
    int stop_requested;
    char error[288];
    char last_error[288]; /* last error record the runner printed */
    char project[PROJECT_ID_LEN + 1];
    char llm[CFG_NAME_MAX];
    long long started, ended;
    int writes; /* successful write-back tool calls */

    char conv[CONV_ID_LEN + 1]; /* the scan's main conversation */

    /* a follow-up run: the user's instruction and the conversation
     * it continues (empty for a fresh scan) */
    char note[SCAN_NOTE_MAX];
    char prev[CONV_ID_LEN + 1];

    /* streamed text blocks arrive as partials: accumulate, record
     * once */
    sbuf_t resp_acc, think_acc;

    /* child plumbing */
    pid_t pid; /* -1 when no child */
    int out_fd;
    sbuf_t out; /* the runner's stdout, folded line by line */
} S;

/* ---------- the write-back tools (POST /scan/mcp) ---------- */

/* the project the running scan writes into, or NULL + err */
static project_t *scan_target(char *err, size_t err_n)
{
    if (!S.running) {
        snprintf(err, err_n, "no project scan is running");
        return NULL;
    }
    int i = projects_find_id(S.projects, S.project);
    if (i < 0) {
        snprintf(err, err_n,
                 "the scanned project is no longer in the list");
        return NULL;
    }
    return &S.projects->items[i];
}

/* one of the four free-text detail fields */
static const struct {
    const char *key;
    size_t off;
} DETAIL_FIELDS[] = {
    { "description",  offsetof(project_t, description) },
    { "objectives",   offsetof(project_t, objectives) },
    { "scope",        offsetof(project_t, scope) },
    { "stakeholders", offsetof(project_t, stakeholders) },
};

static char *fn_set_project_details(const cJSON *args, char *err, size_t err_n)
{
    project_t *p = scan_target(err, err_n);
    if (!p) return NULL;

    sbuf_t done = { 0 };
    int n = 0;
    for (size_t i = 0; i < sizeof DETAIL_FIELDS / sizeof DETAIL_FIELDS[0];
         i++) {
        const cJSON *j =
            cJSON_GetObjectItemCaseSensitive(args, DETAIL_FIELDS[i].key);
        if (!j) continue;
        if (!cJSON_IsString(j) || !j->valuestring ||
            strlen(j->valuestring) >= PROJECT_TEXT_MAX ||
            !valid_utf8_text(j->valuestring, strlen(j->valuestring), 1)) {
            free(done.data);
            snprintf(err, err_n,
                     "%s: text, max %d bytes, no control characters",
                     DETAIL_FIELDS[i].key, PROJECT_TEXT_MAX - 1);
            return NULL;
        }
        char *dst = (char *)p + DETAIL_FIELDS[i].off;
        snprintf(dst, PROJECT_TEXT_MAX, "%s", j->valuestring);
        if (done.len) sb_puts(&done, ", ");
        sb_puts(&done, DETAIL_FIELDS[i].key);
        sb_putc(&done, ' ');
        {
            char bytes[32];
            snprintf(bytes, sizeof bytes, "(%zu bytes)",
                     strlen(j->valuestring));
            sb_puts(&done, bytes);
        }
        n++;
    }
    if (!n) {
        free(done.data);
        snprintf(err, err_n,
                 "pass at least one of description, objectives, scope, "
                 "stakeholders");
        return NULL;
    }
    if (projects_save(S.projects) != 0) {
        free(done.data);
        snprintf(err, err_n, "cannot write projects.json");
        return NULL;
    }
    S.writes++;
    char head[48];
    snprintf(head, sizeof head, "saved %d field%s: ", n, n == 1 ? "" : "s");
    char *out = malloc(strlen(head) + done.len + 1);
    if (out) sprintf(out, "%s%s", head, done.data ? done.data : "");
    free(done.data);
    return out ? out : strdup("saved");
}

static char *fn_add_context_item(const cJSON *args, char *err, size_t err_n)
{
    project_t *p = scan_target(err, err_n);
    if (!p) return NULL;

    const cJSON *tj = cJSON_GetObjectItemCaseSensitive(args, "type");
    int type = cJSON_IsString(tj) && tj->valuestring
                   ? ctx_type_from_name(tj->valuestring) : CTX_FACT;
    if (type < 0) {
        snprintf(err, err_n,
                 "type must be one of fact, pattern, risk, success_metric, "
                 "failure_sign, evaluation_method, rule, resource");
        return NULL;
    }
    /* the resource specification: required on a resource, invalid
     * on anything else */
    const cJSON *sj = cJSON_GetObjectItemCaseSensitive(args, "spec");
    const char *spec =
        cJSON_IsString(sj) && sj->valuestring ? sj->valuestring : "";
    if (type == CTX_RESOURCE) {
        if (!spec[0] || strlen(spec) >= CTX_SPEC_MAX ||
            !valid_utf8_text(spec, strlen(spec), 0)) {
            snprintf(err, err_n,
                     "spec: required on a resource item — a url or "
                     "filesystem path, max %d bytes, no control characters",
                     CTX_SPEC_MAX - 1);
            return NULL;
        }
    } else if (spec[0]) {
        snprintf(err, err_n, "spec is only valid on a resource item");
        return NULL;
    }
    const cJSON *xj = cJSON_GetObjectItemCaseSensitive(args, "text");
    if (!cJSON_IsString(xj) || !xj->valuestring[0] ||
        strlen(xj->valuestring) >= CTX_TEXT_MAX ||
        !valid_utf8_text(xj->valuestring, strlen(xj->valuestring), 1)) {
        snprintf(err, err_n, "text: required, max %d bytes, no control "
                             "characters", CTX_TEXT_MAX - 1);
        return NULL;
    }

    /* count + duplicate check: a re-added item is rejected, the model
     * should not pad the list with what is already there (a resource
     * is its spec — the same text on two paths is two resources) */
    size_t count = 0;
    for (ctx_item_t *it = p->context; it; it = it->next, count++) {
        if (it->type == type && strcmp(it->text, xj->valuestring) == 0 &&
            strcmp(it->spec, spec) == 0) {
            snprintf(err, err_n,
                     "this %s item is already on the project",
                     ctx_type_name(type));
            return NULL;
        }
    }
    if (count >= CONTEXT_MAX) {
        snprintf(err, err_n,
                 "the project already holds %d context items", CONTEXT_MAX);
        return NULL;
    }

    ctx_item_t *item = calloc(1, sizeof *item);
    if (!item) {
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    item->type = type;
    item->updated = (long long)time(NULL);
    snprintf(item->text, sizeof item->text, "%s", xj->valuestring);
    if (spec[0]) snprintf(item->spec, sizeof item->spec, "%s", spec);
    /* append at the tail: the generated ids (P1, P2, …) follow the
     * list order, and a scan's additions read better in order */
    ctx_item_t **tail = &p->context;
    while (*tail) tail = &(*tail)->next;
    *tail = item;

    if (projects_save(S.projects) != 0) {
        *tail = NULL; /* the store stays as it was on disk */
        free(item);
        snprintf(err, err_n, "cannot write projects.json");
        return NULL;
    }
    S.writes++;
    size_t n = 80 + strlen(item->text) + strlen(item->spec);
    char *out = malloc(n);
    if (out) {
        if (item->spec[0]) /* a resource reads best as spec — text */
            snprintf(out, n, "added [%s] %s — %s", ctx_type_name(type),
                     item->spec, item->text);
        else
            snprintf(out, n, "added [%s] %s", ctx_type_name(type),
                     item->text);
    }
    return out ? out : strdup("added");
}

static const mcp_arg_t ARGS_SET_DETAILS[] = {
    { "description",  "string", 0 },
    { "objectives",   "string", 0 },
    { "scope",        "string", 0 },
    { "stakeholders", "string", 0 },
    { NULL }
};
static const mcp_arg_t ARGS_ADD_CONTEXT[] = {
    { "type", "string", 0 },
    { "text", "string", 1 },
    { "spec", "string", 0 },
    { NULL }
};
static const mcp_tool_t SCAN_TOOLS[] = {
    { "set_project_details", ARGS_SET_DETAILS, fn_set_project_details },
    { "add_context_item",    ARGS_ADD_CONTEXT,  fn_add_context_item },
};
mcp_table_t MCP_SCAN = {
    SCAN_TOOLS, sizeof SCAN_TOOLS / sizeof SCAN_TOOLS[0], NULL
};

/* ---------- conversation capture ---------- */

/* a short, readable rendering of a tool call's arguments */
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

/* the researcher def's index into the shared table, -1 when unknown */
static int researcher_index(const researcher_def_t *r)
{
    return r ? (int)(r - researchers()) : -1;
}

/* "<agent>.invoke" — the runner's name for one researcher round */
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

/* the researcher whose sub-conversation `sub` is, -1 when none */
static int sub_index(const char *sub)
{
    if (!sub || !sub[0]) return -1;
    for (size_t i = 0; i < researchers_count(); i++)
        if (strcmp(SUBS[i], sub) == 0) return (int)i;
    return -1;
}

/* the invoke's prompt becomes the sub-conversation's title (first
 * line) and its opening user record */
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
    if (conv_create(researchers()[idx].agent, S.llm, S.project, S.conv,
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

/* the researchers' own tool calls, arriving on flower's /mcp surface
 * while their invoke runs: folded into the open sub-conversation.
 * Calls while nothing is open are somebody else's (another engine's
 * run, or an external mcp client) and are not recorded. */
static void log_research_tool(const char *tool, const cJSON *args,
                              const char *result, int is_error)
{
    if (!S.running) return;
    int idx = researcher_index(researcher_for_tool(tool));
    if (idx < 0 || !SUBS[idx][0]) return;
    char brief[448];
    args_brief(args, brief, sizeof brief);
    conv_add(SUBS[idx], "tool_call", tool, brief, 0);
    conv_add(SUBS[idx], "tool_result", tool, result ? result : "", is_error);
}

/* ---------- prompt rendering ---------- */

/* a compiled-in prompt file, rendered for `proj`, with a fallback */
static char *render_prompt(const char *path, const project_t *proj,
                           const char *fallback)
{
    char *tpl = prompt_text(path);
    char *out = tpl ? prompt_render(tpl, proj) : NULL;
    free(tpl);
    if (out && *out) return out;
    free(out);
    return strdup(fallback);
}

/* render_prompt with caller-supplied {{variables}} (the follow-up's
 * request and previous-run transcript) */
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

/* the opening user record of a run's conversation — and the `user`
 * record the runner is seeded with. A fresh scan asks for the scan;
 * a follow-up hands the model the previous run's transcript and the
 * user's request (S.prev and S.note, captured by scan_start). */
static char *scan_user_prompt(const project_t *proj)
{
    if (!S.note[0])
        return render_prompt("agents/project_scanner/user_prompt.txt",
                             proj, "Scan the project now.");

    char *prev = S.prev[0] ? conv_transcript_text(S.prev, SCAN_PREV_MAX)
                           : NULL;
    prompt_var_t vars[] = {
        { "request", S.note },
        { "previous_run", prev && prev[0] ? prev
              : "No previous run of this project is recorded." },
    };
    char *out = render_prompt_vars(
        "agents/project_scanner/followup_prompt.txt", proj,
        vars, sizeof vars / sizeof vars[0], "Continue the project scan.");
    free(prev);
    return out;
}

/* ---------- the runner's stdin records ---------- */

/* the two researchers as stdio servers, grown from their seeds. The
 * fs tools are scoped to the scanned project — its surface grounds
 * relative paths in the project's directory */
static int add_researcher_servers(cJSON *servers, const llm_t *llm,
                                  const project_t *proj, char *err, size_t err_n)
{
    char fs_url[CFG_URL_MAX + 64];
    snprintf(fs_url, sizeof fs_url, "%s/projects/%lld/mcp", S.base_url,
             proj->seq);
    for (size_t i = 0; i < researchers_count(); i++) {
        if (researcher_add_server(servers, &researchers()[i], llm, proj, "",
                                  S.llmkit, fs_url, err, err_n) != 0)
            return -1;
    }
    return 0;
}

/* the whole stdin feed: header, llm, tools, options, system, user,
 * flush. malloc'd string, NULL + err on failure. */
static char *build_runner_input(const agent_t *scanner, const llm_t *llm,
                                const project_t *proj,
                                char *err, size_t err_n)
{
    sbuf_t b = { 0 };
    int rc = 0;
    cJSON *rec;

    rec = cJSON_Parse("{\"type\":\"header\",\"version\":1}");
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    rec = agents_llm_record(scanner, llm);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    /* tools: the scanner's own (flower /scan/mcp) + the researchers */
    rec = cJSON_CreateObject();
    cJSON *servers = agents_tools_record(scanner, S.base_url, 0);
    if (!rec || !servers ||
        !cJSON_AddStringToObject(rec, "type", "tools")) {
        cJSON_Delete(servers);
        cJSON_Delete(rec);
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    if (add_researcher_servers(servers, llm, proj, err, err_n) != 0) {
        cJSON_Delete(servers);
        cJSON_Delete(rec);
        return NULL;
    }
    cJSON_AddItemToObject(rec, "tools", servers);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    rec = cJSON_CreateObject();
    if (rec && cJSON_AddStringToObject(rec, "type", "options")) {
        cJSON_AddNumberToObject(rec, "max_tool_rounds", SCAN_MAX_ROUNDS);
        cJSON_AddNumberToObject(rec, "tool_call_timeout", SCAN_TOOL_TIMEOUT);
    }
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    char *system = render_prompt("agents/project_scanner/system_prompt.txt",
                                 proj, "You are a project scan agent.");
    rec = rec_text("system", system);
    free(system);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    char *user = scan_user_prompt(proj);
    rec = rec_text("user", user);
    free(user);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    rec = cJSON_Parse("{\"type\":\"flush\"}");
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    if (rc != 0 || !b.data) {
        free(b.data);
        snprintf(err, err_n, "cannot assemble the runner records");
        return NULL;
    }
    return b.data;
}

/* ---------- the child ---------- */

/* fork/exec the runner and hand it the whole feed at once; its stdin
 * closes with the feed (a scan is one turn — the interactive variant
 * that keeps it open lives in src/plan.c) */
static int spawn_runner(const char *input, char *err, size_t err_n)
{
    if (runner_spawn(S.llmkit, input, S.epfd, &S.pid, &S.out_fd,
                     err, err_n) != 0)
        return -1;
    S.out.len = 0; /* fresh buffer for this run */
    if (S.out.data) S.out.data[0] = '\0';
    return 0;
}

/* ---------- stdout: fold records into the conversations ---------- */

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
    /* partial defaults to true on the wire; only a closed block is
     * recorded */
    const cJSON *partial = cJSON_GetObjectItemCaseSensitive(rec, "partial");
    int fin = cJSON_IsBool(partial) && !cJSON_IsTrue(partial);

    if (strcmp(t, "response") == 0) {
        sb_puts(&S.resp_acc, x);
        if (fin) {
            conv_add(S.conv, "response", NULL,
                     S.resp_acc.data ? S.resp_acc.data : "", 0);
            S.resp_acc.len = 0;
            if (S.resp_acc.data) S.resp_acc.data[0] = '\0';
        }
    } else if (strcmp(t, "thinking") == 0) {
        sb_puts(&S.think_acc, x);
        if (fin) {
            conv_add(S.conv, "thinking", NULL,
                     S.think_acc.data ? S.think_acc.data : "", 0);
            S.think_acc.len = 0;
            if (S.think_acc.data) S.think_acc.data[0] = '\0';
        }
    } else if (strcmp(t, "tool_request") == 0) {
        const cJSON *tool = cJSON_GetObjectItemCaseSensitive(rec, "tool");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(rec, "arguments");
        const char *tn = cJSON_IsString(tool) && tool->valuestring
                             ? tool->valuestring : "";
        char brief[448];
        args_brief(args, brief, sizeof brief);
        /* a researcher invoke opens the researcher's own
         * sub-conversation, linked to this scan's */
        int idx = invoke_index(tn);
        if (idx >= 0) {
            const cJSON *input =
                cJSON_GetObjectItemCaseSensitive(args, "input");
            open_sub(idx, cJSON_IsString(input) && input->valuestring
                          ? input->valuestring : "");
        }
        conv_add(S.conv, "tool_call", tn, brief, 0);
        pend_push(tn, idx >= 0 ? SUBS[idx] : "");
    } else if (strcmp(t, "tool_response") == 0) {
        /* answers arrive in call order: the FIFO entry says whether
         * the call was an invoke whose conversation closes here */
        pend_t p;
        if (pend_pop(&p)) {
            if (p.sub[0]) close_sub(p.sub, CONV_COMPLETED, x);
            conv_add(S.conv, "tool_result", p.tool[0] ? p.tool : NULL, x, 0);
        } else {
            conv_add(S.conv, "tool_result", NULL, x, 0);
        }
    } else if (strcmp(t, "error") == 0) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(rec, "code");
        char msg[sizeof S.last_error];
        snprintf(msg, sizeof msg, "%s: %s",
                 cJSON_IsString(code) && code->valuestring
                     ? code->valuestring : "error", x);
        conv_add(S.conv, "error", NULL, msg, 1);
        snprintf(S.last_error, sizeof S.last_error, "%s", msg);
    }
    cJSON_Delete(rec);
}

static void process_lines(void)
{
    if (!S.out.data) return;
    size_t start = 0;
    for (size_t i = 0; i < S.out.len; i++) {
        if (S.out.data[i] != '\n') continue;
        S.out.data[i] = '\0';
        handle_line(S.out.data + start);
        start = i + 1;
    }
    if (start) {
        memmove(S.out.data, S.out.data + start, S.out.len - start);
        S.out.len -= start;
        S.out.data[S.out.len] = '\0';
    }
}

/* ---------- lifecycle ---------- */

static void unlink_seeds(void)
{
    researchers_unlink_seeds("");
}

static void scan_finish(void)
{
    if (S.out_fd >= 0) {
        epoll_ctl(S.epfd, EPOLL_CTL_DEL, S.out_fd, NULL);
        close(S.out_fd);
        S.out_fd = -1;
    }
    process_lines(); /* whatever the last chunk held */
    /* trailing partials mean the turn was cut short — show them */
    if (S.resp_acc.len)
        conv_add(S.conv, "response", NULL, S.resp_acc.data, 0);
    if (S.think_acc.len)
        conv_add(S.conv, "thinking", NULL, S.think_acc.data, 0);
    free(S.resp_acc.data); free(S.think_acc.data);
    S.resp_acc = (sbuf_t){ 0 };
    S.think_acc = (sbuf_t){ 0 };

    int code = 0;
    if (S.pid > 0) {
        int st = 0;
        while (waitpid(S.pid, &st, 0) < 0 && errno == EINTR) { }
        S.pid = -1;
        if (WIFEXITED(st)) code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) code = -WTERMSIG(st);
        else code = -1;
    }

    if (S.stop_requested) {
        S.ok = 0;
        snprintf(S.error, sizeof S.error, "scan stopped");
    } else if (code == 0) {
        S.ok = 1;
        S.error[0] = '\0';
    } else {
        S.ok = 0;
        if (S.last_error[0])
            snprintf(S.error, sizeof S.error, "%s", S.last_error);
        else if (code < 0)
            snprintf(S.error, sizeof S.error,
                     "llmkit runner killed by signal %d", -code);
        else
            snprintf(S.error, sizeof S.error,
                     "llmkit runner exited with code %d", code);
    }

    /* close the conversations: still-open sub-conversations first
     * (children before the parent), then the scan's own */
    for (size_t i = 0; i < researchers_count(); i++)
        if (SUBS[i][0]) {
            conv_add(SUBS[i], "error", NULL,
                     "the scan ended before this conversation finished", 1);
            conv_finish(SUBS[i], CONV_STOPPED);
            SUBS[i][0] = '\0';
        }
    if (S.conv[0]) {
        if (S.error[0]) conv_add(S.conv, "error", NULL, S.error, 1);
        conv_finish(S.conv, S.stop_requested ? CONV_STOPPED
                      : S.ok ? CONV_COMPLETED : CONV_FAILED);
    }
    pend_head = pend_n = 0;

    free(S.out.data);
    S.out.data = NULL;
    S.out.len = S.out.cap = 0;
    unlink_seeds();
    S.stop_requested = 0;
    S.running = 0;
    S.done = 1;
    S.ended = (long long)time(NULL);
}

void scan_on_readable(void)
{
    char chunk[8192];
    for (;;) {
        ssize_t n = read(S.out_fd, chunk, sizeof chunk);
        if (n > 0) {
            if (sb_putn(&S.out, chunk, (size_t)n) != 0)
                break; /* OOM: treat what we have as the end */
            process_lines();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        break; /* EOF or hard error: the runner is done */
    }
    scan_finish();
}

/* ---------- public surface ---------- */

void scan_attach(int epfd, projects_t *projects, llms_t *llms,
                 const char *llmkit, const char *base_url)
{
    S.epfd = epfd;
    S.projects = projects;
    S.llms = llms;
    snprintf(S.llmkit, sizeof S.llmkit, "%s",
             llmkit && llmkit[0] ? llmkit : "llmkit");
    snprintf(S.base_url, sizeof S.base_url, "%s",
             base_url && base_url[0] ? base_url : "http://127.0.0.1:8080");
    S.pid = -1;
    S.out_fd = -1;
    S.conv[0] = '\0';
    /* the researchers' inner tool calls (POST /mcp) are folded into
     * the open sub-conversation while a scan runs */
    MCP_RESEARCH.log = log_research_tool;
}

scan_start_result_t scan_start(const char *project_id, const char *llm_name,
                               const char *note, char *err, size_t err_n)
{
    if (S.running) return SCAN_START_BUSY;
    if (note && note[0] &&
        (strlen(note) >= SCAN_NOTE_MAX ||
         !valid_utf8_text(note, strlen(note), 1))) {
        snprintf(err, err_n,
                 "note: text, max %d bytes, no control characters",
                 SCAN_NOTE_MAX - 1);
        return SCAN_START_REJECT;
    }

    int pi = projects_find_id(S.projects, project_id);
    if (pi < 0) {
        snprintf(err, err_n, "unknown project id");
        return SCAN_START_REJECT;
    }
    project_t *p = &S.projects->items[pi];
    struct stat st;
    if (stat(p->dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, err_n,
                 "the project's working directory is missing on disk");
        return SCAN_START_REJECT;
    }
    const llm_t *llm = llms_get(S.llms, llm_name);
    if (!llm) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "unknown llm '%s' — add it on the config page first",
                 llm_name ? llm_name : "");
        snprintf(err, err_n, "%s", msg);
        return SCAN_START_REJECT;
    }
    const agent_t *scanner = agents_builtin_get("project_scanner");
    if (!scanner) {
        snprintf(err, err_n, "the project_scanner agent is missing");
        return SCAN_START_REJECT;
    }

    /* the run this one continues: whatever the project last recorded
     * (captured before this run's own conversation exists) */
    snprintf(S.note, sizeof S.note, "%s", note ? note : "");
    S.prev[0] = '\0';
    if (S.note[0])
        conv_latest_root_id(project_id, "project_scanner", S.prev);

    /* the conversation this scan is recorded in — created before the
     * child so even a failed spawn leaves a readable trace */
    char title[CONV_TITLE_MAX];
    snprintf(title, sizeof title, "Project scan — %s",
             p->title[0] ? p->title : p->dir);
    if (conv_create("project_scanner", llm_name ? llm_name : "", project_id,
                    "", title, 0, S.conv) != 0) {
        snprintf(err, err_n, "cannot create the conversation record");
        return SCAN_START_SPAWN;
    }
    char *user = scan_user_prompt(p);
    conv_add(S.conv, "user", NULL, user, 0);
    free(user);

    /* fresh state for this run (the previous result is forgotten) */
    S.done = 0;
    S.ok = 0;
    S.error[0] = S.last_error[0] = '\0';
    snprintf(S.project, sizeof S.project, "%s", project_id);
    snprintf(S.llm, sizeof S.llm, "%s", llm_name ? llm_name : "");
    S.started = (long long)time(NULL);
    S.ended = 0;
    S.writes = 0;
    S.stop_requested = 0;
    pend_head = pend_n = 0;

    char *input = build_runner_input(scanner, llm, p, err, err_n);
    if (!input) {
        conv_add(S.conv, "error", NULL, err, 1);
        conv_finish(S.conv, CONV_FAILED);
        return SCAN_START_SPAWN;
    }
    int rc = spawn_runner(input, err, err_n);
    free(input);
    if (rc != 0) {
        conv_add(S.conv, "error", NULL, err, 1);
        conv_finish(S.conv, CONV_FAILED);
        unlink_seeds();
        return SCAN_START_SPAWN;
    }

    S.running = 1;
    return SCAN_START_OK;
}

void scan_stop(void)
{
    if (!S.running || S.pid <= 0) return;
    if (!S.stop_requested) {
        S.stop_requested = 1;
        kill(-S.pid, SIGINT); /* orderly: llmkit flushes and exits */
    } else {
        kill(-S.pid, SIGKILL); /* second ask: immediate */
    }
}

int scan_running(void) { return S.running; }

int scan_fd(void) { return S.running ? S.out_fd : -1; }

char *scan_status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddBoolToObject(o, "running", S.running);
    cJSON_AddBoolToObject(o, "done", S.done);
    cJSON_AddStringToObject(o, "project", S.project);
    cJSON_AddStringToObject(o, "llm", S.llm);
    if (S.conv[0])
        cJSON_AddStringToObject(o, "conversation", S.conv);
    if (S.started) cJSON_AddNumberToObject(o, "started", (double)S.started);
    if (S.ended) cJSON_AddNumberToObject(o, "ended", (double)S.ended);
    if (S.done) cJSON_AddBoolToObject(o, "ok", S.ok);
    if (S.done && S.error[0]) cJSON_AddStringToObject(o, "error", S.error);
    cJSON_AddNumberToObject(o, "writes", S.writes);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json;
}

void scan_shutdown(void)
{
    if (S.pid > 0) {
        kill(-S.pid, SIGKILL);
        waitpid(S.pid, NULL, 0);
        S.pid = -1;
    }
    if (S.out_fd >= 0) {
        close(S.out_fd);
        S.out_fd = -1;
    }
    free(S.out.data);
    S.out.data = NULL;
    S.out.len = S.out.cap = 0;
    unlink_seeds();
}

/* ---------- the `flower researcher` subcommand ---------- */

/* One of the two researchers, run on its own — the debug path. The
 * same seed a scan writes (same prompts, same tools, same project
 * grounding) goes to {config}/scan/debug-<agent>.jsonl and llmkit
 * agent-as-tool is exec'd over it, so this process becomes the same
 * stdio mcp server the scanner's runner talks to: one tool, invoke.
 * No flower surface is involved in serving it (the researcher's own
 * tools call out like they always do — the fs researcher back into
 * the running flower at --base-url, the online one into its own
 * llmkit children), so nothing here runs through the scan state. */

static void researcher_usage(FILE *out)
{
    fprintf(out,
        "flower researcher — run one of the scan's researcher agents\n"
        "directly, as the stdio mcp server exposing its invoke tool\n"
        "\n"
        "usage: flower researcher <agent> --project <id> [--llm <name>]\n"
        "                          [--base-url <url>] [-c DIR] [-l BIN]\n"
        "\n"
        "  <agent>           filesystem_researcher | online_researcher\n"
        "  --project <id>    the project to research (ids: GET /api/projects)\n"
        "  --llm <name>      the llm endpoint to run it with (default:\n"
        "                    the only one in the config, else the first —\n"
        "                    the scan dialog's own fallback)\n"
        "  --base-url <url>  the running flower serving the filesystem\n"
        "                    researcher's tools (default\n"
        "                    http://127.0.0.1:8080; the online researcher\n"
        "                    never calls back)\n"
        "  -c DIR            config directory (as the server's -c)\n"
        "  -l BIN            the llmkit binary (as the server's -l)\n"
        "  -h, --help        show this help\n"
        "\n"
        "The seed is written to {config}/scan/debug-<agent>.jsonl and\n"
        "llmkit agent-as-tool is exec'd over it, so the session speaks\n"
        "mcp json-rpc on stdin/stdout. Point `llmkit mcp-repl --stdio`\n"
        "(or an `llmkit mcp-proxy` config, for `llmkit repl\n"
        "--mcp-proxy`) at this command to debug a researcher in\n"
        "isolation from the scanner — the seed is the very one a scan\n"
        "of that project would run.\n");
}

int researcher_main(int argc, char **argv)
{
    const char *agent = NULL, *project = NULL, *llm = NULL;
    const char *cfg_dir = NULL, *llmkit = NULL, *base_url = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            researcher_usage(stdout);
            return 0;
        } else if (strcmp(a, "--project") == 0 && i + 1 < argc) {
            project = argv[++i];
        } else if (strcmp(a, "--llm") == 0 && i + 1 < argc) {
            llm = argv[++i];
        } else if (strcmp(a, "--base-url") == 0 && i + 1 < argc) {
            base_url = argv[++i];
        } else if (strcmp(a, "-c") == 0 && i + 1 < argc) {
            cfg_dir = argv[++i];
        } else if (strcmp(a, "-l") == 0 && i + 1 < argc) {
            llmkit = argv[++i];
        } else if (!agent && a[0] != '-') {
            agent = a;
        } else {
            fprintf(stderr, "flower researcher: unknown or incomplete"
                            " argument '%s'\n", a);
            researcher_usage(stderr);
            return 2;
        }
    }
    if (!agent || !project) {
        fprintf(stderr, "flower researcher: <agent> and --project are"
                        " required\n");
        researcher_usage(stderr);
        return 2;
    }

    /* only the two researchers run here — the scanner and the
     * planner have no life outside their engines */
    if (!researcher_by_agent(agent)) {
        fprintf(stderr, "flower researcher: '%s' is not a researcher agent"
                        " (filesystem_researcher | online_researcher)\n",
                agent);
        return 2;
    }

    if (theme_init(cfg_dir) != 0) {
        fprintf(stderr,
                "flower: cannot resolve or create config directory "
                "(set HOME, XDG_CONFIG_HOME, or use -c DIR)\n");
        return 1;
    }
    if (!llmkit) llmkit = getenv("FLOWER_LLMKIT"); /* may stay NULL */

    projects_t projects;
    llms_t llms;
    projects_load(&projects);
    llms_load(&llms);

    const agent_t *ag = agents_builtin_get(agent);
    int pi = projects_find_id(&projects, project);
    if (pi < 0) {
        fprintf(stderr, "flower researcher: unknown project id '%s'\n",
                project);
        return 2;
    }
    project_t *p = &projects.items[pi];
    struct stat st;
    if (stat(p->dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "flower researcher: the project's working directory"
                        " is missing on disk\n");
        return 2;
    }
    const llm_t *l = llm ? llms_get(&llms, llm) : NULL;
    if (llm && !l) {
        fprintf(stderr, "flower researcher: unknown llm '%s' — add it on"
                        " the config page first\n", llm);
        return 2;
    }
    if (!l) {
        /* no --llm: the config's own pick — the only endpoint there
         * is, else the first of the list (the scan dialog's own
         * fallback; llms.json loads name-sorted) */
        if (llms.count == 0) {
            fprintf(stderr, "flower researcher: no llm endpoints in the"
                            " config — add one on the config page first,"
                            " or pass --llm\n");
            return 2;
        }
        l = llms.items[0];
        fprintf(stderr, "flower researcher: no --llm given — using '%s'"
                        " (%s)\n", l->name,
                llms.count == 1 ? "the only llm in the config"
                               : "first of the config's list");
    }
    if (!ag) {
        fprintf(stderr, "flower researcher: the %s agent is missing\n",
                agent);
        return 1;
    }

    /* the seed writer needs the wiring the server gets from
     * scan_attach; a debug run needs only these three of it */
    char lk[512], base[CFG_URL_MAX + 32];
    snprintf(lk, sizeof lk, "%s", llmkit && llmkit[0] ? llmkit : "llmkit");
    snprintf(base, sizeof base, "%s",
             base_url && base_url[0] ? base_url : "http://127.0.0.1:8080");

    const researcher_def_t *def = researcher_by_agent(agent);
    char fs_url[CFG_URL_MAX + 64];
    snprintf(fs_url, sizeof fs_url, "%s/projects/%lld/mcp", base, p->seq);
    char err[192] = "";
    if (!def ||
        researcher_write_seed(def, l, p, "debug-", lk, fs_url,
                              err, sizeof err) != 0) {
        fprintf(stderr, "flower researcher: %s\n",
                def ? err : "the researcher definition is missing");
        return 1;
    }

    char seed[4352];
    researcher_seed_path("debug-", agent, seed, sizeof seed);
    if (strchr(lk, '/'))
        execl(lk, "llmkit", "agent-as-tool", seed, (char *)NULL);
    else
        execlp(lk, "llmkit", "agent-as-tool", seed, (char *)NULL);
    fprintf(stderr, "flower researcher: cannot run %s: %s\n", lk,
            strerror(errno));
    return 127;
}
