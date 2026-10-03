/*
 * researchers — the shared researcher/runner plumbing. See
 * researchers.h for the layout; the code here grew out of src/scan.c
 * when the task planner (src/plan.c) needed the same pieces.
 */
#define _POSIX_C_SOURCE 200809L

#include "researchers.h"

#include "prompts.h"
#include "theme.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* syscall(2) is only declared under _GNU_SOURCE in glibc; the
 * close_range fast path below is worth one targeted declaration */
extern long syscall(long number, ...);

/* ---------- the definitions ---------- */

/* online_researcher lists no fs tools: its web calls run inside its
 * own llmkit children (mcp-proxy → builtin-mcp) and never reach
 * flower, so they land in no transcript */
static const researcher_def_t RESEARCHER_DEFS[] = {
    { "filesystem_researcher",
      { "read_file", "list_files", "grep", NULL } },
    { "online_researcher",
      { NULL } },
};

const researcher_def_t *researchers(void) { return RESEARCHER_DEFS; }

size_t researchers_count(void)
{
    return sizeof RESEARCHER_DEFS / sizeof RESEARCHER_DEFS[0];
}

const researcher_def_t *researcher_by_agent(const char *agent)
{
    if (!agent) return NULL;
    for (size_t i = 0; i < researchers_count(); i++)
        if (strcmp(RESEARCHER_DEFS[i].agent, agent) == 0)
            return &RESEARCHER_DEFS[i];
    return NULL;
}

const researcher_def_t *researcher_for_tool(const char *tool)
{
    if (!tool) return NULL;
    for (size_t i = 0; i < researchers_count(); i++)
        for (size_t k = 0; RESEARCHER_DEFS[i].tools[k]; k++)
            if (strcmp(RESEARCHER_DEFS[i].tools[k], tool) == 0)
                return &RESEARCHER_DEFS[i];
    return NULL;
}

/* ---------- runner records ---------- */

int rec_put(sbuf_t *out, const cJSON *rec)
{
    if (!rec) return -1;
    char *line = cJSON_PrintUnformatted(rec);
    if (!line) return -1;
    int rc = sb_puts(out, line) != 0 || sb_putc(out, '\n') != 0 ? -1 : 0;
    free(line);
    return rc;
}

cJSON *rec_text(const char *type, const char *text)
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

/* ---------- scratch files ---------- */

static void seed_path(const char *file, char *buf, size_t n)
{
    snprintf(buf, n, "%s/scan/%s", theme_dir(), file);
}

void researcher_seed_path(const char *tag, const char *agent,
                          char *buf, size_t n)
{
    char file[96];
    snprintf(file, sizeof file, "%s%s.jsonl", tag ? tag : "", agent);
    seed_path(file, buf, n);
}

void researcher_web_tools_path(const char *tag, char *buf, size_t n)
{
    char file[96];
    snprintf(file, sizeof file, "%sweb-tools.jsonl", tag ? tag : "");
    seed_path(file, buf, n);
}

void shell_quote(const char *in, char *out, size_t n)
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

int write_file_atomic(const char *path, const char *buf, size_t len)
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

/* ---------- the online researcher's web tools ---------- */

/* llmkit (>= 1.2.0) ships generic web tools as `llmkit builtin-mcp`.
 * flower hands them to the online researcher through an
 * `llmkit mcp-proxy` config written beside the seeds: only
 * web_search and web_fetch pass the whitelist (the file tools stay
 * out — researchers are read-only), and both carry flower's own
 * texts from prompts/mcp/, so the model sees the same
 * flower.web_search(query) / flower.web_fetch(url) tools as when
 * flower served them itself. The calls run inside the researcher's
 * own llmkit children; they never reach flower, so they land in no
 * transcript */

/* one {"type":"expose",…} record: tool `tool`, its upstream argument
 * `upstream_arg` renamed to flower's `arg` when the two differ */
static int put_web_expose(sbuf_t *b, const char *tool,
                          const char *upstream_arg, const char *arg)
{
    char path[96], sel[64];
    snprintf(sel, sizeof sel, "builtin.%s", tool);

    cJSON *one = cJSON_CreateObject();
    cJSON *args = cJSON_CreateObject();
    cJSON *ex = cJSON_CreateObject();
    int ok = one && args && ex &&
             cJSON_AddStringToObject(ex, "type", "expose") != NULL &&
             cJSON_AddStringToObject(ex, "tool", sel) != NULL &&
             cJSON_AddStringToObject(ex, "name", tool) != NULL;

    snprintf(path, sizeof path, "mcp/%s/description.txt", tool);
    char *d = prompt_text(path);
    if (ok && d && *d)
        ok = cJSON_AddStringToObject(ex, "description", d) != NULL;
    free(d);

    if (ok && strcmp(upstream_arg, arg) != 0)
        ok = cJSON_AddStringToObject(one, "name", arg) != NULL;
    snprintf(path, sizeof path, "mcp/%s/arguments/%s.txt", tool, arg);
    d = prompt_text(path);
    if (ok && d && *d)
        ok = cJSON_AddStringToObject(one, "description", d) != NULL;
    free(d);

    if (ok) {
        cJSON_AddItemToObject(args, upstream_arg, one);
        cJSON_AddItemToObject(ex, "arguments", args);
    } else {
        cJSON_Delete(one);
        cJSON_Delete(args);
    }
    int rc = ok ? rec_put(b, ex) : -1;
    cJSON_Delete(ex);
    return rc;
}

static int write_web_tools_config(const char *path, const char *llmkit,
                                  char *err, size_t err_n)
{
    char q_llmkit[1100], cmd[1200];
    shell_quote(llmkit, q_llmkit, sizeof q_llmkit);
    snprintf(cmd, sizeof cmd, "%s builtin-mcp", q_llmkit);

    sbuf_t b = { 0 };
    int rc = 0;

    /* the upstream: llmkit's own generic web tools */
    cJSON *srv = cJSON_CreateObject();
    cJSON *servers = cJSON_CreateArray();
    cJSON *rec = cJSON_CreateObject();
    if (srv) {
        cJSON_AddStringToObject(srv, "type", "stdio");
        cJSON_AddStringToObject(srv, "name", "builtin");
        cJSON_AddStringToObject(srv, "command_line", cmd);
        if (servers) cJSON_AddItemToArray(servers, srv);
        else cJSON_Delete(srv);
    }
    if (rec) {
        cJSON_AddStringToObject(rec, "type", "tools");
        cJSON_AddItemToObject(rec, "tools", servers);
    } else {
        cJSON_Delete(servers);
    }
    rc |= rec_put(&b, rec);
    cJSON_Delete(rec);

    /* flower's view: the two web tools, none of the file tools */
    rc |= put_web_expose(&b, "web_search", "keywords", "query");
    rc |= put_web_expose(&b, "web_fetch", "url", "url");

    if (rc != 0 || !b.data) {
        free(b.data);
        snprintf(err, err_n, "cannot assemble the web tools config");
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

/* the researcher's web tool server: the proxy grown from that config,
 * named "flower" so the model keeps calling flower.web_search /
 * flower.web_fetch — the system prompt already speaks those names */
static int add_web_tools_server(cJSON *servers, const char *llmkit,
                                const char *cfg)
{
    char q_llmkit[1100], q_cfg[4400], cmd[5600];
    shell_quote(llmkit, q_llmkit, sizeof q_llmkit);
    shell_quote(cfg, q_cfg, sizeof q_cfg);
    snprintf(cmd, sizeof cmd, "%s mcp-proxy %s", q_llmkit, q_cfg);

    cJSON *srv = cJSON_CreateObject();
    if (!srv ||
        !cJSON_AddStringToObject(srv, "type", "stdio") ||
        !cJSON_AddStringToObject(srv, "name", "flower") ||
        !cJSON_AddStringToObject(srv, "command_line", cmd) ||
        !cJSON_AddBoolToObject(srv, "required", 1)) {
        cJSON_Delete(srv);
        return -1;
    }
    cJSON_AddItemToArray(servers, srv);
    return 0;
}

/* ---------- the researcher's seed ---------- */

/* the seed for one researcher: header, the chosen llm, the research
 * tools (flower's fs surface at fs_url, or llmkit's builtin web
 * tools for the online researcher), the rendered system prompt and
 * the agent-as-tool presentation of its single invoke tool */
int researcher_write_seed(const researcher_def_t *r, const llm_t *llm,
                          const project_t *proj, const char *tag,
                          const char *llmkit, const char *fs_url,
                          char *err, size_t err_n)
{
    char path[4352], dpath[4352];
    researcher_seed_path(tag, r->agent, path, sizeof path);
    snprintf(dpath, sizeof dpath, "%s/scan", theme_dir());
    mkdir(dpath, 0700);

    const agent_t *agent = agents_builtin_get(r->agent);
    if (!agent) {
        snprintf(err, err_n, "builtin agent %s is missing", r->agent);
        return -1;
    }

    sbuf_t b = { 0 };
    int rc = 0;
    cJSON *rec;

    rec = cJSON_Parse("{\"type\":\"header\",\"version\":1}");
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    rec = agents_llm_record(agent, llm);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    /* the researcher's tools: the fs surface this run grounds it in
     * (only for a researcher that reads files — the online one's
     * tool set is the web proxy below, and it never sees fs_url) */
    rec = cJSON_CreateObject();
    cJSON *servers = cJSON_CreateArray();
    if (r->tools[0]) {
        cJSON *fsrv = cJSON_CreateObject();
        if (fsrv &&
            cJSON_AddStringToObject(fsrv, "type", "http") &&
            cJSON_AddStringToObject(fsrv, "name", "flower") &&
            cJSON_AddStringToObject(fsrv, "url", fs_url) &&
            cJSON_AddBoolToObject(fsrv, "required", 1)) {
            cJSON_AddItemToArray(servers, fsrv);
        } else {
            cJSON_Delete(fsrv);
            rc = -1;
        }
    } else {
        /* the online researcher reads the web: llmkit's builtin web
         * tools, curated by the proxy config written beside this
         * seed */
        char cfg[4352];
        researcher_web_tools_path(tag, cfg, sizeof cfg);
        if (write_web_tools_config(cfg, llmkit, err, err_n) != 0 ||
            add_web_tools_server(servers, llmkit, cfg) != 0) {
            cJSON_Delete(servers);
            free(b.data);
            if (err[0]) return -1;
            snprintf(err, err_n, "cannot assemble the %s web tools server",
                     r->agent);
            return -1;
        }
    }
    if (rec && servers && cJSON_AddStringToObject(rec, "type", "tools"))
        cJSON_AddItemToObject(rec, "tools", servers);
    else cJSON_Delete(servers);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    /* the researcher's prompt, rendered for this project (the
     * {{project_*}} variables are the run's whole brief) */
    char *system = prompt_render(agent->system_prompt, proj);
    rec = rec_text("system", system);
    free(system);
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    /* the invoke presentation: prompt files, like every other text */
    char ppath[160];
    char *d;
    snprintf(ppath, sizeof ppath, "agents/%s/tool_description.txt",
             r->agent);
    d = prompt_text(ppath);
    rec = cJSON_CreateObject();
    if (rec && cJSON_AddStringToObject(rec, "type", "agent-as-tool")) {
        if (d && *d) cJSON_AddStringToObject(rec, "tool_description", d);
        free(d);
        snprintf(ppath, sizeof ppath, "agents/%s/input_description.txt",
                 r->agent);
        d = prompt_text(ppath);
        if (d && *d) cJSON_AddStringToObject(rec, "input_description", d);
        free(d);
    } else {
        free(d);
    }
    rc |= rec_put(&b, rec); cJSON_Delete(rec);

    if (rc != 0 || !b.data) {
        free(b.data);
        snprintf(err, err_n, "cannot assemble the %s seed", r->agent);
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

int researcher_add_server(cJSON *servers, const researcher_def_t *r,
                          const llm_t *llm, const project_t *proj,
                          const char *tag, const char *llmkit,
                          const char *fs_url, char *err, size_t err_n)
{
    if (researcher_write_seed(r, llm, proj, tag, llmkit,
                              fs_url, err, err_n) != 0)
        return -1;

    char seed[4352];
    researcher_seed_path(tag, r->agent, seed, sizeof seed);
    char q_llmkit[1100], q_seed[4400];
    shell_quote(llmkit, q_llmkit, sizeof q_llmkit);
    shell_quote(seed, q_seed, sizeof q_seed);
    char cmd[5600];
    snprintf(cmd, sizeof cmd, "%s agent-as-tool %s", q_llmkit, q_seed);

    cJSON *srv = cJSON_CreateObject();
    if (!srv ||
        !cJSON_AddStringToObject(srv, "type", "stdio") ||
        !cJSON_AddStringToObject(srv, "name", r->agent) ||
        !cJSON_AddStringToObject(srv, "command_line", cmd) ||
        !cJSON_AddBoolToObject(srv, "required", 1)) {
        cJSON_Delete(srv);
        snprintf(err, err_n, "out of memory");
        return -1;
    }
    cJSON_AddItemToArray(servers, srv);
    return 0;
}

void researchers_unlink_seeds(const char *tag)
{
    char path[4352];
    for (size_t i = 0; i < researchers_count(); i++) {
        researcher_seed_path(tag, RESEARCHER_DEFS[i].agent,
                             path, sizeof path);
        unlink(path);
    }
    researcher_web_tools_path(tag, path, sizeof path);
    unlink(path);
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
 * back non-blocking through the epoll loop. */
int runner_spawn(const char *llmkit, const char *input, int epfd,
                 pid_t *pid_out, int *out_fd, char *err, size_t err_n)
{
    *out_fd = -1;
    int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 };
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
        if (strchr(llmkit, '/'))
            execl(llmkit, "llmkit", "runner", (char *)NULL);
        else
            execlp(llmkit, "llmkit", "runner", (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    setpgid(pid, pid); /* both sides race to set it; either winning is fine */

    int rc = write_all(in_pipe[1], input, strlen(input));
    int e = errno;
    close(in_pipe[1]); /* the feed is the turn: the child runs to its end */
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
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, out_pipe[0], &ev) != 0) {
        snprintf(err, err_n, "epoll_ctl: %s", strerror(errno));
        close(out_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    *pid_out = pid;
    *out_fd = out_pipe[0];
    return 0;
}
