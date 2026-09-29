#ifndef FLOWER_TASKS_H
#define FLOWER_TASKS_H

/*
 * tasks — one directory per task under
 * {config}/tasks/{ID}/, holding task.json with the
 * details: the project it belongs to (by id), a title, the
 * creation time and a list of typed context items (src/context.c).
 *
 * A task is a list of actions to be done. Actions nest without a
 * fixed depth limit (a recursion-depth cap protects the parser);
 * each carries a title, a description, a state and a type — where
 * it sits in the refinement loop (observe, analyze, find_root_
 * cause, act, validate, improve).
 *
 * Same contract as projects.c/agents.c: lenient load (broken entries
 * are skipped, their directories stay on disk), strict validated
 * save via atomic tmp+rename writes. A PUT replaces the whole list;
 * directories of removed tasks are deleted.
 *
 * The hierarchy of the main screen: column one lists projects,
 * column two the tasks of the selected project, column three shows
 * the selected task's action tree. GET reports a live "project_ok"
 * flag for dangling references, like the projects "exists" flag.
 * The list is kept sorted newest-first.
 */

#include "projects.h" /* PROJECT_ID_LEN */
#include "context.h"  /* ctx_item_t */

#include <stddef.h>

#define TASKS_MAX 256
#define TASK_ID_LEN       32  /* hex chars */
#define TASK_TITLE_MAX    96

/* ---------- actions ---------- */

#define ACTION_TITLE_MAX 96
#define ACTION_DESC_MAX  4096
#define ACTION_DEPTH_MAX 64  /* nesting cap: protects the recursive
                              * parser, far past anything sensible */

typedef enum {
    ACTION_ACT = 0,             /* "act" — the default */
    ACTION_OBSERVE = 1,         /* "observe" */
    ACTION_ANALYZE = 2,         /* "analyze" */
    ACTION_FIND_ROOT_CAUSE = 3, /* "find_root_cause" */
    ACTION_VALIDATE = 4,        /* "validate" */
    ACTION_IMPROVE = 5,         /* "improve" */
} action_type_t;

typedef enum {
    ACTION_PENDING = 0,     /* "pending" */
    ACTION_IN_PROGRESS = 1, /* "in_progress" */
    ACTION_COMPLETED = 2,   /* "completed" */
    ACTION_PARTIAL = 3,     /* "partial" — completed partially */
    ACTION_FAILED = 4,      /* "failed" — completed unsuccessfully */
} action_state_t;

/* one action: a first-child/next-sibling tree, heap-allocated so
 * nesting and list length follow the data, not a fixed array */
typedef struct action {
    char title[ACTION_TITLE_MAX];
    char description[ACTION_DESC_MAX];
    int type;                     /* action_type_t */
    int state;                    /* action_state_t */
    struct action *first_child;   /* sub-actions */
    struct action *next_sibling;  /* rest of this list */
} action_t;

typedef struct {
    char id[TASK_ID_LEN + 1]; /* server-assigned, directory name */
    char project[PROJECT_ID_LEN + 1]; /* the owning project, by id */
    char title[TASK_TITLE_MAX];
    long long created;        /* unix seconds */
    action_t *actions;        /* the action list (owned) */
    ctx_item_t *context;      /* the task's context items (owned) */
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

/* Free every task's action tree and empty the list. */
void tasks_clear(tasks_t *c);

/* Load every tasks/{ID}/task.json into *c (empty list on any
 * problem). Returns 0 if the directory was read, 1 if the list
 * started empty. */
int tasks_load(tasks_t *c);

/* Persist as one {ID}/task.json per task (atomic
 * writes) and delete the directories of removed tasks.
 * 0 on success. */
int tasks_save(const tasks_t *c);

/* Strict parse+validate a full task array (PUT body). Project
 * references are checked for format, not existence: a task whose
 * project was deleted stays in the list — GET flags it live via
 * project_ok. On failure fills err_field (e.g.
 * "tasks[1].actions[0].title", "" for whole-document errors) and
 * err_msg. Result is sorted newest-first. */
tasks_parse_result_t tasks_from_json(const char *buf, size_t len,
                                     tasks_t *out,
                                     char *err_field, size_t err_field_n,
                                     char *err_msg, size_t err_msg_n);

/* Serialize as a compact JSON array. with_flags adds a live
 * "project_ok" reference flag per task (checked at call time
 * against projects) — used by GET, omitted on the PUT echo, like
 * the projects "exists" flag. */
char *tasks_to_json(const tasks_t *c, int with_flags,
                    const projects_t *projects);

#endif
