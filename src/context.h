#ifndef FLOWER_CONTEXT_H
#define FLOWER_CONTEXT_H

/*
 * context — the typed context items a project or a task carries.
 * An item is one of seven types (fact, pattern, risk, success_
 * metric, failure_sign, evaluation_method, rule), a free-text
 * description and its last-update time. Items live in a singly-
 * linked heap list (like actions), capped per owner. Each item also
 * gets a generated id on the wire — "P<n>" in a project's list,
 * "T<n>" in a task's — numbered by position and regenerated on
 * every read, so it identifies "the third item right now", not a
 * stable row across edits.
 *
 * Parsing follows the store contract: strict mode (PUT) rejects the
 * first problem into err_field/err_msg; lenient mode (load) keeps
 * the good items and repairs defaults. Emission omits defaults
 * (type "fact"), mirroring the action state/type rules. A "created"
 * key is the retired name of "updated" — the lenient loader honors
 * it so pre-update files keep their stamps; an "id" key is accepted
 * and ignored (the id is always regenerated).
 */

#include <cJSON.h>
#include <stddef.h>

#define CONTEXT_MAX   64   /* items per project/task */
#define CTX_TEXT_MAX  4096 /* bytes of text per item */

typedef enum {
    CTX_FACT = 0,              /* "fact" — the default */
    CTX_PATTERN = 1,           /* "pattern" */
    CTX_RISK = 2,              /* "risk" */
    CTX_SUCCESS_METRIC = 3,    /* "success_metric" */
    CTX_FAILURE_SIGN = 4,      /* "failure_sign" */
    CTX_EVALUATION_METHOD = 5, /* "evaluation_method" */
    CTX_RULE = 6,              /* "rule" */
} ctx_type_t;

typedef struct ctx_item {
    struct ctx_item *next; /* rest of the list */
    long long updated;     /* unix seconds of the last edit */
    int type;              /* ctx_type_t */
    char text[CTX_TEXT_MAX];
} ctx_item_t;

/* Free an item list. */
void context_free(ctx_item_t *head);

/*
 * Parse the value of a "context" key into *head (owned by the
 * caller, freed via context_free). `j` may be NULL (nothing there).
 * Strict mode rejects any problem into err_field/err_msg — with
 * `prefix` as the JSON path ("projects[0].context",
 * "tasks[1].context") — and frees whatever it built; lenient mode
 * keeps the parseable items (defaulting type to fact, updated to
 * now) and never fails. 0 on success, -1 on a strict rejection.
 */
int context_from_json(const cJSON *j, ctx_item_t **head, int strict,
                      char *err_field, size_t err_field_n,
                      char *err_msg, size_t err_msg_n,
                      const char *prefix);

/* The item list as a json array (NULL for an empty list); each item
 * emits its text and update time, plus its type when not "fact".
 * id_prefix 'P' or 'T' adds the generated positional id ("P1",
 * "T2", …) — pass 0 to emit none, for the copy on disk. */
cJSON *context_to_cjson(const ctx_item_t *head, char id_prefix);

/* The wire name of a ctx_type_t ("fact" for anything out of range).
 * Shared by the stores and the prompt renderer. */
const char *ctx_type_name(int type);

#endif
