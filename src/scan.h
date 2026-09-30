#ifndef FLOWER_SCAN_H
#define FLOWER_SCAN_H

/*
 * scan — the project scan agent: the first consumer of the llmkit
 * process. A scan (POST /api/scan) runs the compiled-in
 * project_scanner agent as an `llmkit runner` child process, which
 * orchestrates the two researcher agents — exposed to it as single
 * `invoke` tools via `llmkit agent-as-tool` stdio servers grown from
 * seed files under {config}/scan/ — and writes its findings back
 * through flower's second mcp surface, POST /scan/mcp:
 *
 *   flower ── stdin: llm + tools + system + user + flush
 *        └─ llmkit runner (project_scanner)
 *             ├─ filesystem_researcher.invoke   (stdio, agent-as-tool)
 *             │    └─ flower /mcp: read_file, list_files, grep, …
 *             ├─ online_researcher.invoke       (stdio, agent-as-tool)
 *             │    └─ flower /mcp: web_search, web_fetch, …
 *             └─ flower /scan/mcp: set_project_details,
 *                add_context_item → the scanned project + save
 *
 * The whole exchange is recorded as conversations (src/conv.c): the
 * scan gets the main one, every researcher invoke its own
 * sub-conversation linked to it. The runner's stdout records feed
 * the main transcript (text blocks, tool calls and results); the
 * researchers' inner tool calls arrive on flower's own /mcp surface
 * while their invoke runs and are folded into the open
 * sub-conversation (the researchers own disjoint tool sets, so the
 * calls attribute unambiguously). Everything is durable and readable
 * under {config}/conversations/ — GET /api/conversations, live via
 * the /api/conversations/stream SSE channel.
 *
 * The runner's stdout is read non-blocking through the server's
 * epoll loop; GET /api/scan reports progress and result (the client
 * polls). One scan at a time; its write-back tools apply to the
 * scan's project and fail when no scan is running.
 */

#include "agents.h"
#include "mcp.h"
#include "projects.h"

#include <stddef.h>

/*
 * Boot wiring (call once, before the event loop starts serving):
 * the live stores the scan writes into (the server's own lists, so
 * tool writes and GETs see the same data), the epoll set the
 * runner's stdout is registered in, the llmkit binary to run (a
 * path or a name found via PATH) and flower's own base url — the
 * tool servers the seeds and records refer to.
 */
void scan_attach(int epfd, projects_t *projects, llms_t *llms,
                 const char *llmkit, const char *base_url);

typedef enum {
    SCAN_START_OK = 0,
    SCAN_START_BUSY = 1,   /* a scan is already running (409) */
    SCAN_START_REJECT = 2, /* bad project/llm reference (422) — err says why */
    SCAN_START_SPAWN = 3   /* spawn or seed failure (500) — err says why */
} scan_start_result_t;

/*
 * Start scanning project `project_id` with endpoint `llm_name`.
 * Writes the researcher seeds, spawns the runner, feeds it the
 * records and closes its stdin (the conversation then runs to its
 * end on its own; progress arrives through scan_on_readable()).
 */
scan_start_result_t scan_start(const char *project_id, const char *llm_name,
                               char *err, size_t err_n);

/* Ask the running scan to stop: SIGINT to the runner's process group
 * first (llmkit stops orderly), SIGKILL when called again. */
void scan_stop(void);

int scan_running(void);

/* The runner's stdout fd while a scan runs, -1 when idle. The event
 * loop routes its events to scan_on_readable(). */
int scan_fd(void);

/* Read whatever the runner wrote, fold it into the conversations; at
 * EOF (the runner exited) reap it and finish the scan. */
void scan_on_readable(void);

/* GET /api/scan: {running, done, ok, error, project, llm, started,
 * ended, writes, conversation} as a malloc'd compact json string —
 * the transcript itself lives in the conversation store. */
char *scan_status_json(void);

/* Kill and reap the child at server exit. */
void scan_shutdown(void);

#endif
