#ifndef FLOWER_CONVERSATIONS_H
#define FLOWER_CONVERSATIONS_H

/*
 * conversations — one directory per conversation under
 * {config}/conversations/{ID}/, holding conversation.json with the
 * details: the agent it is bound to, an optional llm override, a
 * title and the creation time. Later parts add the message log
 * inside the same directory.
 *
 * Same contract as projects.c/agents.c: lenient load (broken entries
 * are skipped, their directories stay on disk), strict validated
 * save via atomic tmp+rename writes. A PUT replaces the whole list;
 * directories of removed conversations are deleted.
 *
 * A conversation is bound to one agent (user-defined or builtin).
 * The llm field is an override: "" means "use the agent's llm"; a
 * conversation whose agent has no llm of its own (the builtins) must
 * select one. GET reports live "agent_ok"/"llm_ok" flags for dangling
 * references, like the projects "exists" flag. The list is kept
 * sorted newest-first.
 */

#include "agents.h" /* CFG_NAME_MAX */

#include <stddef.h>

#define CONVERSATIONS_MAX 256
#define CONV_ID_LEN       32  /* hex chars */
#define CONV_TITLE_MAX    96

typedef struct {
    char id[CONV_ID_LEN + 1]; /* server-assigned, directory name */
    char title[CONV_TITLE_MAX];
    char agent[CFG_NAME_MAX]; /* reference: user or builtin agent */
    char llm[CFG_NAME_MAX];   /* reference; "" = the agent's llm */
    long long created;        /* unix seconds */
} conv_t;

typedef struct {
    conv_t items[CONVERSATIONS_MAX];
    size_t count;
} conversations_t;

typedef enum {
    CONVS_OK = 0,      /* parsed and valid */
    CONVS_E_JSON = 1,  /* not JSON / not an array */
    CONVS_E_FIELD = 2  /* valid JSON, but an entry/value is rejected */
} convs_parse_result_t;

/* Load every conversations/{ID}/conversation.json into *c (empty list
 * on any problem). Returns 0 if the directory was read, 1 if the list
 * started empty. */
int conversations_load(conversations_t *c);

/* Persist as one {ID}/conversation.json per conversation (atomic
 * writes) and delete the directories of removed conversations.
 * 0 on success. */
int conversations_save(const conversations_t *c);

/* Strict parse+validate a full conversation array (PUT body). `llms`
 * and `agents` are the current endpoint and agent lists: references
 * are checked against them. On failure fills err_field (e.g.
 * "conversations[1].agent", "" for whole-document errors) and
 * err_msg. Result is sorted newest-first. */
convs_parse_result_t conversations_from_json(const char *buf, size_t len,
                                             const llms_t *llms,
                                             const agents_t *agents,
                                             conversations_t *out,
                                             char *err_field,
                                             size_t err_field_n,
                                             char *err_msg, size_t err_msg_n);

/* Serialize as a compact JSON array. with_flags adds live
 * "agent_ok"/"llm_ok" reference flags per conversation (checked at
 * call time against llms/agents) — used by GET, omitted on the PUT
 * echo, like the projects "exists" flag. */
char *conversations_to_json(const conversations_t *c, int with_flags,
                            const llms_t *llms, const agents_t *agents);

#endif
