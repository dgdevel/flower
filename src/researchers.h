#ifndef FLOWER_RESEARCHERS_H
#define FLOWER_RESEARCHERS_H

/*
 * researchers — the researcher agents the llm engines (the project
 * scan and the task planner) orchestrate as agent-as-tool stdio
 * servers, plus the `llmkit runner` plumbing every engine shares:
 * the jsonl record helpers, the seed files and the child spawn.
 *
 * The researchers themselves are builtin agents (agents.c); what
 * lives here is their presentation to a runner: the seed each
 * `llmkit agent-as-tool` server grows from (the chosen llm, the
 * research tools — flower's fs surface for filesystem_researcher,
 * llmkit's builtin web tools behind an mcp-proxy for
 * online_researcher — and the rendered system prompt), and the
 * stdio server entry the orchestrating runner gets for them.
 *
 * Engines own their run state themselves: which researcher's invoke
 * is currently open (its sub-conversation) is per-engine, so two
 * engines can run researchers side by side without their fs tool
 * calls bleeding into each other's transcripts.
 */

#include "agents.h"
#include "conv.h"     /* CONV_ID_LEN */
#include "projects.h"
#include "util.h"     /* sbuf_t */

#include <cJSON.h>
#include <stddef.h>
#include <sys/types.h>

/* the researchers flower ships (see researchers()); the cap bounds
 * the per-engine open-sub-conversation tables */
#define RESEARCHERS_MAX 4

typedef struct {
    const char *agent;
    const char *tools[8]; /* NULL-terminated: the fs tools this
                           * researcher calls on flower's surface —
                           * what attributes an incoming call to it */
} researcher_def_t;

/* the compiled-in researcher definitions, in a stable order */
const researcher_def_t *researchers(void);
size_t researchers_count(void);

/* def lookups: by agent name (NULL when unknown), and by the fs tool
 * name an incoming /mcp call carries */
const researcher_def_t *researcher_by_agent(const char *agent);
const researcher_def_t *researcher_for_tool(const char *tool);

/* ---------- runner records (jsonl) ---------- */

/* one record, compact, plus '\n' into the buffer (0 on success) */
int rec_put(sbuf_t *out, const cJSON *rec);

/* a {"type":…,"content":[{type:"text",text}]} record (malloc'd) */
cJSON *rec_text(const char *type, const char *text);

/* ---------- seeds ---------- */

/* the seed's home: {config}/scan/<tag><agent>.jsonl — the tag
 * namespaces the scratch files so engines never collide ("" for the
 * scan, "debug-" for the researcher subcommand, …) */
void researcher_seed_path(const char *tag, const char *agent,
                          char *buf, size_t n);
/* the web tools proxy config beside them: <tag>web-tools.jsonl */
void researcher_web_tools_path(const char *tag, char *buf, size_t n);

/* Write one researcher's seed (and, for the online researcher, the
 * web tools proxy config beside it). `fs_url` is the url the
 * researcher's own flower server points at — the fs tools surface
 * grounded in the run's project. 0 on success, err says why not. */
int researcher_write_seed(const researcher_def_t *r, const llm_t *llm,
                          const project_t *proj, const char *tag,
                          const char *llmkit, const char *fs_url,
                          char *err, size_t err_n);

/* Append the researcher's stdio agent-as-tool server to `servers`
 * (the runner's tool entry — the seed is written first). */
int researcher_add_server(cJSON *servers, const researcher_def_t *r,
                          const llm_t *llm, const project_t *proj,
                          const char *tag, const char *llmkit,
                          const char *fs_url, char *err, size_t err_n);

/* drop this tag's seed and web tools files (scratch cleanup) */
void researchers_unlink_seeds(const char *tag);

/* ---------- the child ---------- */

/* fork/exec `llmkit runner`, feed it `input` (one turn: header …
 * flush) and bring its stdout back non-blocking through epfd. The
 * feed closes the child's stdin: the runner plays the conversation to
 * its final response and exits there (that is where llmkit ends a
 * conversation — an interactive engine starts the next turn as a new
 * child fed the replayed transcript, see src/plan.c). 0 on success,
 * err says why not. */
int runner_spawn(const char *llmkit, const char *input, int epfd,
                 pid_t *pid_out, int *out_fd, char *err, size_t err_n);

/* sh-safe: 'foo' with embedded quotes escaped */
void shell_quote(const char *in, char *out, size_t n);

/* atomic write of raw bytes (tmp + rename) */
int write_file_atomic(const char *path, const char *buf, size_t len);

#endif
