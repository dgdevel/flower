#ifndef FLOWER_CONV_H
#define FLOWER_CONV_H

/*
 * conv — the conversation store: a durable, readable transcript of
 * every llm conversation flower runs.
 *
 * One directory per conversation under {config}/conversations/{ID}/:
 *
 *   meta.json     identity and state — id, agent, llm, project
 *                 reference, parent conversation (sub-agents), title,
 *                 started/ended, state
 *   records.jsonl the transcript, one record per line, appended as
 *                 the conversation happens:
 *
 *                 {"t":1790000000,"k":"user","text":"scan the project"}
 *                 {"t":1790000001,"k":"tool_call","tool":"grep",
 *                  "text":"{\"glob\":\"src/…\",…}"}
 *                 {"t":1790000002,"k":"tool_result","tool":"grep",
 *                  "text":"src/main.c:12:…"}
 *
 * Record kinds: user (a prompt handed to the model), thinking and
 * response (model text blocks), tool_call / tool_result (a tool use
 * and its outcome, `err` flags a failed result), error. Sub-agent
 * conversations (an agent spawned as a tool by another) carry the
 * spawning conversation's id in `parent` — the Conversations page
 * links both ways.
 *
 * Server-owned store (no PUT API): records are appended by the
 * conversation runners (the project scan, the task planner), and by
 * the user of an interactive conversation (the reply API); meta.json
 * is rewritten atomically on state changes. Loading is lenient like
 * every store: broken metas are skipped, unparsable record lines
 * dropped. A restart closes conversations still marked running
 * (conv_boot) so the list never shows zombies.
 *
 * An interactive conversation (the task planner's) carries
 * "interactive":true — the user may keep replying while it runs, so
 * it ends only when its runner exits or the user stops it. It can be
 * deleted, like any conversation, once nothing in its tree runs.
 */

#include "agents.h"    /* CFG_NAME_MAX */
#include "projects.h"  /* PROJECT_ID_LEN */

#include <stddef.h>

#define CONV_ID_LEN     32   /* hex chars, like task ids */
#define CONV_TITLE_MAX  128
#define CONV_TEXT_MAX   8192 /* stored bytes per record text */
#define CONV_TOOL_MAX   128  /* stored bytes per tool name */
#define CONV_LIST_MAX   256  /* newest conversations listed */
#define CONV_RECORDS_MAX 4096 /* records served per conversation */

typedef enum {
    CONV_RUNNING = 0,   /* started, records may still appear */
    CONV_COMPLETED = 1,
    CONV_FAILED = 2,    /* ended unsuccessfully */
    CONV_STOPPED = 3,   /* ended by the user or a restart */
} conv_state_t;

/* Close conversations still marked running (a restart interrupted
 * them): each gets an error record and state "stopped". Call once at
 * boot, before anything can start a new one. */
void conv_boot(void);

/* Create a running conversation; agent and title required, llm,
 * project id and parent id may be empty; interactive marks a
 * conversation the user can reply to while it runs (the task
 * planner's). The generated id is copied to id_out (CONV_ID_LEN + 1
 * bytes). 0 on success. */
int conv_create(const char *agent, const char *llm, const char *project,
                const char *parent, const char *title, int interactive,
                char *id_out);

/* Append one record (kind one of user, thinking, response, tool_call,
 * tool_result, error; tool may be NULL). Text is truncated on a
 * utf-8 boundary to CONV_TEXT_MAX bytes. Unknown ids are ignored
 * (0 returned). is_error marks failed tool results. */
int conv_add(const char *id, const char *kind, const char *tool,
             const char *text, int is_error);

/* Set the end state (and ended timestamp), rewriting meta.json. */
int conv_finish(const char *id, conv_state_t state);

/* Monotonic counter, bumped by every create/append/finish — the SSE
 * stream tells clients to refetch when it moves. */
long long conv_version(void);

/* 32 lowercase hex chars? */
int conv_valid_id(const char *s);

/* The newest CONV_LIST_MAX conversations as a compact JSON array
 * (malloc'd): the meta fields with state as a name. NULL on OOM. */
char *conv_list_json(void);

/* One conversation as a compact JSON object — the meta fields plus a
 * "records" array (t, k, tool?, text?, err?). NULL when the id is
 * unknown or nothing could be read. */
char *conv_get_json(const char *id);

/* The id of the newest root (non sub-agent) conversation of `project`
 * run by `agent` — a scan's main conversation, a task plan — or ""
 * when there is none. id_out holds CONV_ID_LEN + 1 bytes. */
void conv_latest_root_id(const char *project, const char *agent,
                         char *id_out);

/* A conversation with this id exists on disk? (a reply to a finished
 * or unknown one is answered before an engine ever sees it) */
int conv_exists(const char *id);

typedef enum {
    CONV_DEL_OK = 0,
    CONV_DEL_MISSING = 1, /* no such conversation */
    CONV_DEL_RUNNING = 2, /* it or a sub-agent of it still runs */
    CONV_DEL_IO = 3       /* the directory could not be removed */
} conv_del_result_t;

/* Delete a conversation and every sub-agent conversation under it
 * (the whole tree — a sub-agent's transcript has no life without its
 * parent). Refused while anything in the tree is still running; err
 * then names it. Bumps the store version either way clients notice. */
conv_del_result_t conv_delete(const char *id, char *err, size_t err_n);

/* One conversation as plain text, for handing a later run the context
 * of an earlier one: an "kind[tool]: text" line per record, oldest
 * first, with the newest lines kept when the whole exceeds max_bytes
 * (the cut is marked). malloc'd, "" when the id is unknown or nothing
 * could be read. */
char *conv_transcript_text(const char *id, size_t max_bytes);

#endif
