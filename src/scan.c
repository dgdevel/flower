/*
 * scan — the project scan agent. See scan.h for the process graph;
 * the pieces here are the two write-back mcp tools (MCP_SCAN), the
 * researcher seed files, the runner child and its stdout feed.
 */
#define _POSIX_C_SOURCE 200809L

#include "scan.h"

#include "context.h"
#include "prompts.h"
#include "theme.h"
#include "util.h"

#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* syscall(2) is only declared under _GNU_SOURCE in glibc; the
 * close_range fast path below is worth one targeted declaration */
extern long syscall(long number, ...);

/* the orchestrator's runner options: research invokes are whole
 * conversations, so a generous tool timeout and many rounds */
#define SCAN_MAX_ROUNDS      48
#define SCAN_TOOL_TIMEOUT    600

#define SCAN_LOG_MAX         240 /* entries kept (oldest half drops) */
#define SCAN_LOG_TEXT        400 /* bytes of text per entry */

typedef struct {
    char kind[12];  /* start|thinking|response|tool|tool_result|error */
    char tool[80];  /* tool requests: the full tool name */
    char text[SCAN_LOG_TEXT + 8];
} scan_log_t;

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

    scan_log_t log[SCAN_LOG_MAX];
    size_t log_n, log_dropped;

    /* streamed text blocks arrive as partials: accumulate, log once */
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
                 "failure_sign, evaluation_method, rule");
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
     * should not pad the list with what is already there */
    size_t count = 0;
    for (ctx_item_t *it = p->context; it; it = it->next, count++) {
        if (it->type == type && strcmp(it->text, xj->valuestring) == 0) {
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
    size_t n = 80 + strlen(item->text);
    char *out = malloc(n);
    if (out)
        snprintf(out, n, "added [%s] %s", ctx_type_name(type), item->text);
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
    { NULL }
};
static const mcp_tool_t SCAN_TOOLS[] = {
    { "set_project_details", ARGS_SET_DETAILS, fn_set_project_details },
    { "add_context_item",    ARGS_ADD_CONTEXT,  fn_add_context_item },
};
const mcp_table_t MCP_SCAN = {
    SCAN_TOOLS, sizeof SCAN_TOOLS / sizeof SCAN_TOOLS[0]
};

/* ---------- log ---------- */

/* copy at most SCAN_LOG_TEXT bytes, cut on a utf-8 boundary, mark
 * the cut with an ellipsis */
static void trunc_text(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n <= SCAN_LOG_TEXT) {
        memcpy(dst, src, n);
        dst[n] = '\0';
        return;
    }
    size_t cut = SCAN_LOG_TEXT;
    while (cut > 0 && ((unsigned char)src[cut] & 0xc0) == 0x80) cut--;
    memcpy(dst, src, cut);
    memcpy(dst + cut, "\xE2\x80\xA6", 3); /* … */
    dst[cut + 3] = '\0';
}

static void log_add(const char *kind, const char *tool, const char *text)
{
    if (S.log_n == SCAN_LOG_MAX) { /* keep the newest half */
        size_t drop = SCAN_LOG_MAX / 2;
        memmove(S.log, S.log + drop,
                (SCAN_LOG_MAX - drop) * sizeof S.log[0]);
        S.log_n -= drop;
        S.log_dropped += drop;
    }
    scan_log_t *e = &S.log[S.log_n++];
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->tool, sizeof e->tool, "%s", tool ? tool : "");
    trunc_text(e->text, text ? text : "");
}

/* ---------- record plumbing (cJSON -> one jsonl line) ---------- */

static int put_record(sbuf_t *out, const cJSON *rec)
{
    if (!rec) return -1;
    char *line = cJSON_PrintUnformatted(rec);
    if (!line) return -1;
    int rc = sb_puts(out, line) != 0 || sb_putc(out, '\n') != 0 ? -1 : 0;
    free(line);
    return rc;
}

static cJSON *text_record(const char *type, const char *text)
{
    cJSON *rec = cJSON_CreateObject();
    cJSON *block = cJSON_CreateObject();
    cJSON *blocks = cJSON_CreateArray();
    if (!rec || !block || !blocks ||
        !cJSON_AddStringToObject(rec, "type", type) ||
        !cJSON_AddStringToObject(block, "type", "text") ||
        !cJSON_AddStringToObject(block, "text", text ? text : ""))
        goto fail;
    cJSON_AddItemToArray(blocks, block);
    cJSON_AddItemToObject(rec, "content", blocks);
    return rec;
fail:
    cJSON_Delete(rec);
    cJSON_Delete(block);
    cJSON_Delete(blocks);
    return NULL;
}

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

/* ---------- researcher seeds ---------- */

static void seed_path(const char *file, char *buf, size_t n)
{
    snprintf(buf, n, "%s/scan/%s", theme_dir(), file);
}

/* sh-safe: 'foo' with embedded quotes escaped */
static void shell_quote(const char *in, char *out, size_t n)
{
    size_t o = 0;
    if (o < n - 1) out[o++] = '\'';
    for (const char *p = in; *p && o < n - 4; p++) {
        if (*p == '\'') {
            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            out[o++] = *p;
        }
    }
    if (o < n - 1) out[o++] = '\'';
    out[o] = '\0';
}

/* atomic write of raw bytes (the seeds are jsonl, not one json doc) */
static int write_file_atomic(const char *path, const char *buf, size_t len)
{
    char tmp[4352 + 16];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, len, f);
    if (w != len || fflush(f) != 0 || fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* the seed for one researcher: header, the chosen llm, the research
 * tools (flower /mcp), the rendered system prompt and the
 * agent-as-tool presentation of its single invoke tool */
static int write_researcher_seed(const agent_t *researcher, const llm_t *llm,
                                 const project_t *proj, const char *file,
                                 char *err, size_t err_n)
{
    char path[4352], dpath[4352];
    seed_path(file, path, sizeof path);
    snprintf(dpath, sizeof dpath, "%s/scan", theme_dir());
    mkdir(dpath, 0700);

    sbuf_t b = { 0 };
    int rc = 0;
    cJSON *rec;

    rec = cJSON_Parse("{\"type\":\"header\",\"version\":1}");
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    rec = agents_llm_record(researcher, llm);
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    rec = cJSON_CreateObject();
    cJSON *servers = agents_tools_record(researcher, S.base_url);
    if (rec && servers && cJSON_AddStringToObject(rec, "type", "tools"))
        cJSON_AddItemToObject(rec, "tools", servers);
    else cJSON_Delete(servers);
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    /* the researcher's prompt, rendered for this project (the
     * {{project_*}} variables are the scan's whole brief) */
    char *system = prompt_render(researcher->system_prompt, proj);
    rec = text_record("system", system);
    free(system);
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    /* the invoke presentation: prompt files, like every other text */
    char ppath[160];
    char *d;
    snprintf(ppath, sizeof ppath, "agents/%s/tool_description.txt",
             researcher->name);
    d = prompt_text(ppath);
    rec = cJSON_CreateObject();
    if (rec && cJSON_AddStringToObject(rec, "type", "agent-as-tool")) {
        if (d && *d) cJSON_AddStringToObject(rec, "tool_description", d);
        free(d);
        snprintf(ppath, sizeof ppath, "agents/%s/input_description.txt",
                 researcher->name);
        d = prompt_text(ppath);
        if (d && *d) cJSON_AddStringToObject(rec, "input_description", d);
        free(d);
    } else {
        free(d);
    }
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    if (rc != 0 || !b.data) {
        free(b.data);
        snprintf(err, err_n, "cannot assemble the %s seed", researcher->name);
        return -1;
    }
    if (write_file_atomic(path, b.data, b.len) != 0) {
        free(b.data);
        snprintf(err, err_n, "cannot write %s", path);
        return -1;
    }
    free(b.data);
    return 0;
}

/* ---------- the runner's stdin records ---------- */

/* the two researchers as stdio servers, grown from their seeds */
static int add_researcher_servers(cJSON *servers, const llm_t *llm,
                                  const project_t *proj, char *err, size_t err_n)
{
    static const char *const NAMES[] = {
        "filesystem_researcher", "online_researcher",
    };
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        const agent_t *ag = agents_builtin_get(NAMES[i]);
        if (!ag) {
            snprintf(err, err_n, "builtin agent %s is missing", NAMES[i]);
            return -1;
        }
        char file[64], seed[4352];
        snprintf(file, sizeof file, "%s.jsonl", NAMES[i]);
        seed_path(file, seed, sizeof seed);
        if (write_researcher_seed(ag, llm, proj, file, err, err_n) != 0)
            return -1;

        char q_llmkit[1100], q_seed[4400];
        shell_quote(S.llmkit, q_llmkit, sizeof q_llmkit);
        shell_quote(seed, q_seed, sizeof q_seed);
        char cmd[5600];
        snprintf(cmd, sizeof cmd, "%s agent-as-tool %s", q_llmkit, q_seed);

        cJSON *srv = cJSON_CreateObject();
        if (!srv ||
            !cJSON_AddStringToObject(srv, "type", "stdio") ||
            !cJSON_AddStringToObject(srv, "name", NAMES[i]) ||
            !cJSON_AddStringToObject(srv, "command_line", cmd) ||
            !cJSON_AddBoolToObject(srv, "required", 1)) {
            cJSON_Delete(srv);
            snprintf(err, err_n, "out of memory");
            return -1;
        }
        cJSON_AddItemToArray(servers, srv);
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
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    rec = agents_llm_record(scanner, llm);
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    /* tools: the scanner's own (flower /scan/mcp) + the researchers */
    rec = cJSON_CreateObject();
    cJSON *servers = agents_tools_record(scanner, S.base_url);
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
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    rec = cJSON_CreateObject();
    if (rec && cJSON_AddStringToObject(rec, "type", "options")) {
        cJSON_AddNumberToObject(rec, "max_tool_rounds", SCAN_MAX_ROUNDS);
        cJSON_AddNumberToObject(rec, "tool_call_timeout", SCAN_TOOL_TIMEOUT);
    }
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    char *system = render_prompt("agents/project_scanner/system_prompt.txt",
                                 proj, "You are a project scan agent.");
    rec = text_record("system", system);
    free(system);
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    char *user = render_prompt("agents/project_scanner/user_prompt.txt",
                               proj, "Scan the project now.");
    rec = text_record("user", user);
    free(user);
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    rec = cJSON_Parse("{\"type\":\"flush\"}");
    rc |= put_record(&b, rec); cJSON_Delete(rec);

    if (rc != 0 || !b.data) {
        free(b.data);
        snprintf(err, err_n, "cannot assemble the runner records");
        return NULL;
    }
    return b.data;
}

/* ---------- the child ---------- */

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* fork/exec `llmkit runner`; the seed goes to its stdin (it drains
 * eagerly, so the blocking write cannot stall), its stdout comes
 * back non-blocking through the epoll loop. Returns 0 on success. */
static int spawn_runner(const char *input, char *err, size_t err_n)
{
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        snprintf(err, err_n, "pipe: %s", strerror(errno));
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        if (out_pipe[0] >= 0) { close(out_pipe[0]); close(out_pipe[1]); }
        return -1;
    }

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_n, "fork: %s", strerror(errno));
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        /* the child: jsonl on stdin/stdout, stderr passes through */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        setpgid(0, 0); /* its own group: stopping it reaches its own
                          stdio servers, never flower */
        /* flower's stop handlers must not survive into the exec
         * window: reset them, or a stop signal arrives while this is
         * still the flower image and gets swallowed by the handler */
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        /* nothing else is inherited: other servers' pipes and any
         * open sockets belong to the parent, not to the runner */
        int closed = 0;
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3, ~0U, 0) == 0) closed = 1;
#endif
        if (!closed) {
            long maxfd = sysconf(_SC_OPEN_MAX);
            if (maxfd < 0) maxfd = 16384;
            for (int fd = 3; fd < maxfd; fd++) close(fd);
        }
        if (strchr(S.llmkit, '/'))
            execl(S.llmkit, "llmkit", "runner", (char *)NULL);
        else
            execlp(S.llmkit, "llmkit", "runner", (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    setpgid(pid, pid); /* both sides race to set it; either winning is fine */

    int rc = write_all(in_pipe[1], input, strlen(input));
    int e = errno;
    close(in_pipe[1]);
    if (rc != 0) {
        snprintf(err, err_n, "cannot feed the runner: %s", strerror(e));
        close(out_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    int fl = fcntl(out_pipe[0], F_GETFL, 0);
    if (fl >= 0) fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = out_pipe[0] };
    if (epoll_ctl(S.epfd, EPOLL_CTL_ADD, out_pipe[0], &ev) != 0) {
        snprintf(err, err_n, "epoll_ctl: %s", strerror(errno));
        close(out_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    S.pid = pid;
    S.out_fd = out_pipe[0];
    S.out.len = 0; /* fresh buffer for this run */
    if (S.out.data) S.out.data[0] = '\0';
    return 0;
}

/* ---------- stdout: fold records into the log ---------- */

/* a short, readable rendering of a tool call's arguments */
static void brief_args(const cJSON *args, char *dst)
{
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(args, "input");
    if (cJSON_IsString(input) && input->valuestring) {
        trunc_text(dst, input->valuestring);
        return;
    }
    char *j = cJSON_PrintUnformatted(args);
    if (!j) {
        dst[0] = '\0';
        return;
    }
    trunc_text(dst, j);
    free(j);
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
    /* partial defaults to true on the wire; only a closed block logs */
    const cJSON *partial = cJSON_GetObjectItemCaseSensitive(rec, "partial");
    int fin = cJSON_IsBool(partial) && !cJSON_IsTrue(partial);

    if (strcmp(t, "response") == 0) {
        sb_puts(&S.resp_acc, x);
        if (fin) {
            log_add("response", NULL, S.resp_acc.data ? S.resp_acc.data : "");
            S.resp_acc.len = 0;
            if (S.resp_acc.data) S.resp_acc.data[0] = '\0';
        }
    } else if (strcmp(t, "thinking") == 0) {
        sb_puts(&S.think_acc, x);
        if (fin) {
            log_add("thinking", NULL,
                    S.think_acc.data ? S.think_acc.data : "");
            S.think_acc.len = 0;
            if (S.think_acc.data) S.think_acc.data[0] = '\0';
        }
    } else if (strcmp(t, "tool_request") == 0) {
        const cJSON *tool = cJSON_GetObjectItemCaseSensitive(rec, "tool");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(rec, "arguments");
        char brief[SCAN_LOG_TEXT + 8];
        brief_args(args, brief);
        log_add("tool", cJSON_IsString(tool) ? tool->valuestring : "", brief);
    } else if (strcmp(t, "tool_response") == 0) {
        log_add("tool_result", NULL, x);
    } else if (strcmp(t, "error") == 0) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(rec, "code");
        char msg[sizeof S.last_error];
        snprintf(msg, sizeof msg, "%s: %s",
                 cJSON_IsString(code) && code->valuestring
                     ? code->valuestring : "error", x);
        log_add("error", NULL, msg);
        snprintf(S.last_error, sizeof S.last_error, "%s", msg);
    } else if (strcmp(t, "start") == 0) {
        log_add("start", NULL, "");
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
    char path[4352];
    seed_path("filesystem_researcher.jsonl", path, sizeof path);
    unlink(path);
    seed_path("online_researcher.jsonl", path, sizeof path);
    unlink(path);
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
    if (S.resp_acc.len) log_add("response", NULL, S.resp_acc.data);
    if (S.think_acc.len) log_add("thinking", NULL, S.think_acc.data);
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
}

scan_start_result_t scan_start(const char *project_id, const char *llm_name,
                               char *err, size_t err_n)
{
    if (S.running) return SCAN_START_BUSY;

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

    /* fresh state for this run (the previous result is forgotten) */
    S.done = 0;
    S.ok = 0;
    S.error[0] = S.last_error[0] = '\0';
    snprintf(S.project, sizeof S.project, "%s", project_id);
    snprintf(S.llm, sizeof S.llm, "%s", llm_name ? llm_name : "");
    S.started = (long long)time(NULL);
    S.ended = 0;
    S.writes = 0;
    S.log_n = S.log_dropped = 0;
    S.stop_requested = 0;

    char *input = build_runner_input(scanner, llm, p, err, err_n);
    if (!input) return SCAN_START_SPAWN;
    int rc = spawn_runner(input, err, err_n);
    free(input);
    if (rc != 0) {
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
    if (S.started) cJSON_AddNumberToObject(o, "started", (double)S.started);
    if (S.ended) cJSON_AddNumberToObject(o, "ended", (double)S.ended);
    if (S.done) cJSON_AddBoolToObject(o, "ok", S.ok);
    if (S.done && S.error[0]) cJSON_AddStringToObject(o, "error", S.error);
    cJSON_AddNumberToObject(o, "writes", S.writes);

    cJSON *log = cJSON_CreateArray();
    if (log) {
        for (size_t i = 0; i < S.log_n; i++) {
            cJSON *e = cJSON_CreateObject();
            if (!e) break;
            cJSON_AddStringToObject(e, "k", S.log[i].kind);
            if (S.log[i].tool[0])
                cJSON_AddStringToObject(e, "tool", S.log[i].tool);
            if (S.log[i].text[0])
                cJSON_AddStringToObject(e, "text", S.log[i].text);
            cJSON_AddItemToArray(log, e);
        }
        cJSON_AddItemToObject(o, "log", log);
    }
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
