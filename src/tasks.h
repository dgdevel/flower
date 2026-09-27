#ifndef FLOWER_TASKS_H
#define FLOWER_TASKS_H

/*
 * tasks — one directory per task under
 * {config}/tasks/{ID}/, holding task.json with the
 * details: the agent it is bound to, an optional llm override, a
 * title and the creation time. Later parts add the message log
 * inside the same directory.
 *
 * Same contract as projects.c/agents.c: lenient load (broken entries
 * are skipped, their directories stay on disk), strict validated
 * save via atomic tmp+rename writes. A PUT replaces the whole list;
 * directories of removed tasks are deleted.
 *
 * A task is bound to one agent (user-defined or builtin).
 * The llm field is an override: "" means "use the agent's llm"; a
 * task whose agent has no llm of its own (the builtins) must
 * select one. GET reports live "agent_ok"/"llm_ok" flags for dangling
 * references, like the projects "exists" flag. The list is kept
 * sorted newest-first.
 */

#include "agents.h" /* CFG_NAME_MAX */

#include <stddef.h>

#define TASKS_MAX 256
#define TASK_ID_LEN       32  /* hex chars */
#define TASK_TITLE_MAX    96

typedef struct {
    char id[TASK_ID_LEN + 1]; /* server-assigned, directory name */
    char title[TASK_TITLE_MAX];
    char agent[CFG_NAME_MAX]; /* reference: user or builtin agent */
    char llm[CFG_NAME_MAX];   /* reference; "" = the agent's llm */
    long long created;        /* unix seconds */
} task_t;

typedef struct {
    task_t items[TASKS_MAX];
    size_t count;
} tasks_t;

typedef enum {
    TASKS_OK = 0,      /* parsed and valid */
    TASKS_E_JSON = 1,  /* not JSON / not an array */
    TASKS_E_FIELD = 2  /* valid JSON, but an entry/value is rejected */
} tasks_parse_result_t;

/* Load every tasks/{ID}/task.json into *c (empty list
 * on any problem). Returns 0 if the directory was read, 1 if the list
 * started empty. */
int tasks_load(tasks_t *c);

/* Persist as one {ID}/task.json per task (atomic
 * writes) and delete the directories of removed tasks.
 * 0 on success. */
int tasks_save(const tasks_t *c);

/* Strict parse+validate a full task array (PUT body). `llms`
 * and `agents` are the current endpoint and agent lists: references
 * are checked against them. On failure fills err_field (e.g.
 * "tasks[1].agent", "" for whole-document errors) and
 * err_msg. Result is sorted newest-first. */
tasks_parse_result_t tasks_from_json(const char *buf, size_t len,
                                             const llms_t *llms,
                                             const agents_t *agents,
                                             tasks_t *out,
                                             char *err_field,
                                             size_t err_field_n,
                                             char *err_msg, size_t err_msg_n);

/* Serialize as a compact JSON array. with_flags adds live
 * "agent_ok"/"llm_ok" reference flags per task (checked at
 * call time against llms/agents) — used by GET, omitted on the PUT
 * echo, like the projects "exists" flag. */
char *tasks_to_json(const tasks_t *c, int with_flags,
                            const llms_t *llms, const agents_t *agents);

#endif
