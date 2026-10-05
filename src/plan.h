#ifndef FLOWER_PLAN_H
#define FLOWER_PLAN_H

/*
 * plan — the task planner: the llm-assisted "New Task Auto". A plan
 * (POST /api/plan) creates the task, then runs the compiled-in
 * task_planner agent as an `llmkit runner` child that orchestrates
 * the two researchers — exposed to it as single `invoke` tools via
 * `llmkit agent-as-tool` stdio servers grown from seed files under
 * {config}/scan/plan-* — and fills the task through flower's third
 * mcp surface, POST /plan/mcp:
 *
 *   flower ── stdin: llm + tools + system + transcript + flush
 *        └─ llmkit runner (task_planner)    one child per turn
 *             ├─ filesystem_researcher.invoke   (stdio, agent-as-tool)
 *             │    └─ flower /plan/research/mcp: read_file,
 *             │       list_files, grep, analyze — grounded in the
 *             │       plan's project directory
 *             ├─ online_researcher.invoke       (stdio, agent-as-tool)
 *             │    └─ llmkit mcp-proxy → builtin-mcp: web_search,
 *             │       web_fetch (inside the researcher's own children)
 *             └─ flower /plan/mcp: set_task_title, add_action,
 *                clear_actions → the planned task + save
 *
 * Unlike the scan, the conversation outlives its turn. llmkit ends a
 * conversation at the final response (stdin open or not), so a turn
 * is one runner child and the interactive lifetime is flower's: the
 * conversation stays open after a clean turn, and every user reply
 * (POST /api/conversations/{id}/reply) starts the next turn as a
 * fresh child fed the replayed transcript plus the new user record —
 * llmkit's own continuation contract (input + previous output + a new
 * turn). The user and the model refine the task title and the action
 * plan together until the user stops the conversation. Actions the
 * tools add always start pending — the states are the user's.
 *
 * Everything is recorded like a scan's run (src/conv.c): the main
 * conversation carries "interactive":true, every researcher invoke
 * its own sub-conversation. One plan at a time (the reply owns the
 * single stdin); the researchers' fs calls arrive on the plan's own
 * /plan/research/mcp surface, so a concurrent scan's transcripts
 * never mix with a plan's.
 */

#include "agents.h"
#include "mcp.h"
#include "projects.h"
#include "tasks.h"

#include <stddef.h>

/*
 * Boot wiring (call once, before the event loop starts serving):
 * the live stores the plan writes into (the server's own lists, so
 * tool writes and GETs see the same data), the epoll set the
 * runner's stdout is registered in, the llmkit binary to run and
 * flower's own base url.
 */
void plan_attach(int epfd, projects_t *projects, llms_t *llms,
                 tasks_t *tasks, const char *llmkit, const char *base_url);

typedef enum {
    PLAN_START_OK = 0,
    PLAN_START_BUSY = 1,   /* a plan is already running (409) */
    PLAN_START_REJECT = 2, /* bad project/llm/prompt (422) — err says why */
    PLAN_START_SPAWN = 3   /* spawn or seed failure (500) — err says why */
} plan_start_result_t;

/* the longest planning prompt accepted, NUL included */
#define PLAN_PROMPT_MAX 4096

/* the longest reply accepted in the conversation, NUL included */
#define PLAN_REPLY_MAX 4096

/*
 * Start planning a task for project `project_id` with endpoint
 * `llm_name`: creates the task (title seeded from the prompt's first
 * line, empty tree — the agent fills both), creates its interactive
 * conversation, writes the researcher seeds and plays the first turn.
 * The conversation stays open until the user stops it (plan_stop) or
 * a turn fails.
 *
 * On success the ids are copied out (task: TASK_ID_LEN + 1 bytes,
 * conv: CONV_ID_LEN + 1 — the "New Task Auto" button links to the
 * conversation).
 */
plan_start_result_t plan_start(const char *project_id, const char *llm_name,
                               const char *prompt,
                               char *task_out, char *conv_out,
                               char *err, size_t err_n);

typedef enum {
    PLAN_REPLY_OK = 0,
    PLAN_REPLY_MISSING = 1, /* no conversation with this id (404) */
    PLAN_REPLY_IDLE = 2,    /* it is not the running plan (409) */
    PLAN_REPLY_REJECT = 3   /* bad text (422) — err says why */
} plan_reply_result_t;

/*
 * The user's next turn in the running plan's conversation: recorded
 * as a user record and played as a fresh child seeded with the whole
 * replayed transcript plus it (the turn's records stream back through
 * plan_on_readable). One turn at a time: while the model is still
 * answering, a reply is refused (409).
 */
plan_reply_result_t plan_reply(const char *conv_id, const char *text,
                               char *err, size_t err_n);

/* Stop the plan: SIGINT to the current turn's process group first
 * (llmkit stops orderly), SIGKILL when called again; a plan idle
 * between turns closes at once. */
void plan_stop(void);

int plan_running(void);

/* The id of the running plan's conversation, "" when idle. */
const char *plan_conv_id(void);

/* The runner's stdout fd while a plan runs, -1 when idle. The event
 * loop routes its events to plan_on_readable(). */
int plan_fd(void);

/* Read whatever the current turn's runner wrote, fold it into the
 * conversations and the replayable transcript; at EOF reap the child
 * and end the turn (a clean one leaves the conversation open). */
void plan_on_readable(void);

/* Serve POST /plan/research/mcp: the research table scoped to the
 * running plan's project, folding the fs researcher's calls into its
 * open sub-conversation. NULL + 404 semantics when no plan runs. */
char *plan_research_post(const char *body, size_t len, int *is_notification);

/* GET /api/plan: {running, awaiting_reply, done, ok, error,
 * project, llm, task, started, ended, writes, conversation} as a
 * malloc'd compact json string. */
char *plan_status_json(void);

/* Kill and reap the child at server exit. */
void plan_shutdown(void);

/* the plan write-back tool set behind POST /plan/mcp (plan.c) */
extern mcp_table_t MCP_PLAN;

#endif
