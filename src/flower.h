/* flower.h - shared declarations. */
#ifndef FLOWER_H
#define FLOWER_H

#include <stddef.h>

#define FLOWER_NAME_MAX  256
#define FLOWER_PATH_MAX  1100
#define FLOWER_ID_MAX    32
#define FLOWER_DESC_MAX  1024

/* ---- config.c: base dir, settings, generic file io ---- */

/* fill buf with the flower base directory ($XDG_CONFIG_DIR/flower,
 * else $HOME/.config/flower). 0 on success, -1 if no dir can be resolved or
 * buf is too small. */
int flower_base_dir(char *buf, size_t size);

/* create path and every missing parent (mkdir -p). 0 on success. */
int flower_ensure_dir(const char *path);

/* settings as written by `flower setup` into flower.json. */
struct flower_settings {
    char protocol[256];   /* openai | openai_responses | anthropic */
    char api_base[512];
    char model[256];      /* "" when absent */
    char api_key[256];    /* "" when absent */
    char user_name[256];
    char user_role[256];
};

/* load $FLOWER_BASE/flower.json into s. 0 on success, -1 if the file is
 * absent or unreadable, -2 if it is invalid json or lacks required fields
 * (llm.endpoint_protocol, llm.api_base, user.name, user.role). */
int flower_settings_load(struct flower_settings *s);

/* read the whole file into a malloc'd nul-terminated buffer.
 * 0 on success, -1 on failure. Caller frees *out. */
int flower_read_file(const char *path, char **out);

/* write text to path, truncating, mode 0644. 0 on success, -1 on failure. */
int flower_write_file(const char *path, const char *text);

/* count markdown list items: lines starting with "- ". 0 if text is NULL. */
int flower_count_md_items(const char *text);

/* ---- project.c: project registry and project file io ---- */

struct flower_project {
    char id[FLOWER_ID_MAX];        /* "P1" */
    char dir[FLOWER_PATH_MAX];     /* $FLOWER_BASE/projects/P1 */
    char name[FLOWER_NAME_MAX];    /* config.json name */
    char path[FLOWER_PATH_MAX];    /* project filesystem path, absolute */
    char description[FLOWER_DESC_MAX];
};

/* list projects in id order. Returns the project count (0 if none), or -1
 * on io error. At most max entries are filled. */
int project_list(struct flower_project *out, int max);

/* resolve the project covering the current working directory. A project
 * covers cwd when its path equals cwd or is a parent of cwd. The deepest
 * (longest) match wins. 0 on success, -1 when no project covers cwd,
 * -2 on io error. */
int project_find_current(struct flower_project *p);

/* load one project by id ("P1"). 0 on success, -1 when no such project,
 * -2 on io error. */
int project_by_id(const char *id, struct flower_project *p);

/* allocate the next project id, create the project directory tree
 * (projects/Pn, projects/Pn/tasks), write a minimal config.json
 * (name from the basename of fs_path, path, empty description).
 * 0 on success, -1 on failure. */
int project_create(const char *fs_path, struct flower_project *p);

/* item count of the project context.md. -1 on read error, 0 when absent. */
int project_count_context_items(const struct flower_project *p);

/* entry count of the project resources.json. -1 on parse error,
 * 0 when absent. */
int project_count_resources(const struct flower_project *p);

/* task directories under the project. -1 on io error, 0 when none. */
int project_task_count(const struct flower_project *p);

/* ---- task.c: task io, current-task pointer, actions io ---- */

struct flower_task {
    char id[FLOWER_ID_MAX];        /* "T1" */
    char dir[FLOWER_PATH_MAX];     /* $FLOWER_BASE/projects/Pn/tasks/Tn */
    char name[FLOWER_NAME_MAX];    /* config.json name */
    char description[FLOWER_DESC_MAX];
};

/* read the current-task pointer file. 0 on success (id holds "T1"),
 * -1 when no pointer is set, -2 on io error. */
int task_current_get(const struct flower_project *p, char *id, size_t size);

/* write the current-task pointer file (id must name an existing task of
 * the project). 0 on success, -1 on failure. */
int task_current_set(const struct flower_project *p, const char *id);

/* list tasks of the project in id order. Returns the task count (0 if
 * none), or -1 on io error. At most max entries are filled. */
int task_list(const struct flower_project *p, struct flower_task *out,
              int max);

/* allocate the next task id, create the task directory under
 * p->dir/tasks, write a minimal config.json (name, description).
 * 0 on success, -1 on failure. */
int task_create(const struct flower_project *p, const char *name,
                const char *description, struct flower_task *t);

/* resolve a task reference. ref is "T1" (task of project p) or "P2-T1"
 * (task of project P2). 0 on success, -1 when the task does not exist,
 * -2 when ref is malformed. */
int task_resolve(const struct flower_project *p, const char *ref,
                 struct flower_task *t);

/* item count of the task context.md. -1 on read error, 0 when absent. */
int task_count_context_items(const struct flower_task *t);

/* entry count of the task resources.json. -1 on parse error,
 * 0 when absent. */
int task_count_resources(const struct flower_task *t);

struct flower_action {
    char id[FLOWER_ID_MAX];        /* "A1" */
    char name[FLOWER_NAME_MAX];
    char description[FLOWER_DESC_MAX];
    char type[32];                 /* Observe|Analyze|Root Cause|Act|Validate|Improve */
    char state[32];                /* Pending|In Progress|Paused|Completed */
    char outcome[32];              /* Success|Partial|Failure|"" */
    char blocked_by[256];          /* comma-joined blocking action ids, "" if none */
};

/* list actions of the task from actions.json. Returns the action count
 * (0 if none), -1 on io or parse error, -2 when actions.json is absent. */
int action_list(const struct flower_task *t, struct flower_action *out,
                int max);

/* write the full actions array back to actions.json. Array form, fields
 * id, name, description, type, state, outcome, blocked_by (ids split on
 * ','). 0 on success, -1 on failure. */
int action_save(const struct flower_task *t,
                const struct flower_action *acts, int count);

/* update the state and/or the outcome of one action by id in actions.json.
 * NULL keeps the field. 0 on success, -1 when the id is absent or the
 * write fails. */
int action_update(struct flower_task *t, const char *id,
                  const char *state, const char *outcome);

/* ---- llm.c: bridge to llmkit runner ---- */

/* one proposed file write parsed from a flower fenced block. */
struct flower_proposal {
    char target[FLOWER_PATH_MAX]; /* relative to $FLOWER_BASE, e.g.
                                   * projects/P1/details.md */
    char *content;                /* malloc'd, caller frees */
};

/* run one conversation turn. seed is a jsonl string (llm/tools/system/user
 * records, or a full replay transcript plus a new user record). Out:
 *  - final response text into *resp (malloc'd, caller frees)
 *  - full runner stdout into *out_records (malloc'd jsonl, for replay and
 *    transcripts; caller frees)
 *  - proposals from flower fenced blocks into *props (malloc'd array,
 *    caller frees each .content and the array)
 * Renders thinking, tool calls, and the response on the terminal as they
 * arrive. 0 on success, -1 on failure (message already printed). */
int llm_run_turn(const char *seed, char **resp, char **out_records,
                 struct flower_proposal **props, int *prop_count);

/* build a seed jsonl:
 *  - llm record from cfg
 *  - tools record (builtin stdio server, llmkit from $PATH)
 *  - system record assembled from prompt_parts (joined with \n)
 *  - user record from first_user_text (may be NULL)
 * caller frees the returned string. NULL on failure. */
char *llm_build_seed(const struct flower_settings *cfg,
                     const char **prompt_parts, int part_count,
                     const char *first_user_text);

/* return a new malloc'd seed = seed + record + "\n". NULL on failure. */
char *llm_append_record(const char *seed, const char *record);

/* free a proposal array as filled by llm_run_turn. */
void llm_free_proposals(struct flower_proposal *props, int count);

/* hidden smoke command `flower llm-ping`. Returns exit code. */
int llm_ping_cmd(int argc, char **argv);

/* run a full conversation from seed: first turn, then follow-up turns from
 * terminal input. After each turn, flower fenced blocks become proposals:
 * each is shown and applied (written under $FLOWER_BASE) only when the user
 * answers y (or a: all). transcript_path (may be NULL) receives every
 * record of every turn, appended. Empty line or q ends. 0 on success. */
int llm_converse(const struct flower_settings *cfg, const char *seed,
                 const char *transcript_path);

/* ---- scan.c ---- */

/* `flower project scan`. Returns process exit code. */
int scan_cmd(int argc, char **argv);

/* ---- prompt.c ---- */

/* assemble the shared system prompt: user identity, project block
 * (details.md, context.md, resources.json), optional task block, optional
 * action block. Returns a malloc'd string. NULL on failure. */
char *prompt_build(const struct flower_settings *s,
                   const struct flower_project *p,
                   const struct flower_task *t,
                   const struct flower_action *a);

/* ---- setup.c ---- */

/* setup.c: interactive flower.json writer. Returns process exit code. */
int setup_run(void);

/* ---- project_cmds.c ---- */

/* `flower project ...`. Returns process exit code. */
int project_cmd(int argc, char **argv);

/* ---- task_cmds.c ---- */

/* `flower task ...`. Returns process exit code. */
int task_cmd(int argc, char **argv);

/* `flower action ...`. Returns process exit code. */
int action_cmd(int argc, char **argv);

/* ---- config.c: truncation-tolerant formatting ----
 *
 * like snprintf but truncation is allowed (snprintf always nul-
 * terminates; -Wformat-truncation is deliberately silenced inside).
 * format string checked like printf at every call site. */
char *flower_snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
char *flower_path_join(char *buf, size_t size, const char *dir,
                       const char *name);
char *flower_path_join3(char *buf, size_t size, const char *dir,
                        const char *name, const char *ext);

#endif
