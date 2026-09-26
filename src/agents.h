#ifndef FLOWER_AGENTS_H
#define FLOWER_AGENTS_H

/*
 * agents — LLM interaction configuration, two stores behind one module:
 *
 *  - llms.json: the named llm endpoints (connection details in one
 *    place: protocol, base url, model, api key, headers)
 *  - agents/<slug>.json: one file per agent — a reference to an llm by
 *    name plus the agent's own settings (inference options, system
 *    prompt, mcp servers)
 *
 * Same contract as theme.c/projects.c: lenient load (broken entries are
 * repaired or dropped, the rest still boots), strict validated save via
 * atomic tmp+rename writes. A PUT replaces the whole list; agent files
 * of removed agents are deleted.
 *
 * The shapes mirror llmkit's records (docs/records.md in the llmkit
 * repo) so an agent + its llm translate mechanically into runner input:
 * the llm record is the llms.json entry's fields plus the agent's
 * inference_options, the tools record is the agent's tools array, and
 * the system prompt becomes the system record's single text block.
 *
 * Identity is the name (unique, case-insensitively) in both stores;
 * agent file names are slugs derived from the agent name. Agents
 * reference llms by name (case-insensitive match); GET /api/agents
 * reports a live "llm_ok" flag per agent, PUT validates references
 * against the current llms list.
 */

#include <stddef.h>

/* shared caps */
#define CFG_NAME_MAX     64   /* llm and agent names */
#define CFG_URL_MAX      512  /* api_base, http/sse tool url */
#define CFG_TEXT_MAX     128  /* model name */
#define CFG_SECRET_MAX   256  /* api_key, header values */
#define CFG_HDR_MAX      16   /* headers per llm / per mcp server */
#define CFG_HDR_NAME_MAX 128

/* one name/value http header (llm requests, http/sse mcp servers) */
typedef struct {
    char name[CFG_HDR_NAME_MAX];
    char value[CFG_SECRET_MAX];
} cfg_kv_t;

/* ---------- llm endpoints (llms.json) ---------- */

#define LLMS_MAX 32

typedef struct {
    char name[CFG_NAME_MAX];
    char endpoint_protocol[24]; /* openai|openai_responses|anthropic */
    char api_base[CFG_URL_MAX];
    char model[CFG_TEXT_MAX];   /* "" = not sent (llama.cpp style) */
    char api_key[CFG_SECRET_MAX]; /* "" = not sent */
    cfg_kv_t headers[CFG_HDR_MAX];
    size_t header_count;
} llm_t;

typedef struct {
    llm_t *items[LLMS_MAX]; /* heap each */
    size_t count;
} llms_t;

typedef enum {
    LLMS_OK = 0,
    LLMS_E_JSON = 1,   /* not JSON / not an array */
    LLMS_E_FIELD = 2   /* valid JSON, but a value/key is rejected */
} llms_parse_result_t;

void llms_free(llms_t *l);

/* Load llms.json into *l (empty list on any problem; entries sorted by
 * name). Returns 0 if the file was read, 1 if the list started empty. */
int llms_load(llms_t *l);

/* Persist as llms.json (atomic write). 0 on success. */
int llms_save(const llms_t *l);

/* Strict parse+validate a full llm array (PUT body). On failure fills
 * err_field ("llms[1].api_base", "" for whole-document errors) and
 * err_msg. Result is sorted by name. */
llms_parse_result_t llms_from_json(const char *buf, size_t len, llms_t *out,
                                   char *err_field, size_t err_field_n,
                                   char *err_msg, size_t err_msg_n);

/* Serialize as a compact sorted JSON array. malloc'd, caller frees. */
char *llms_to_json(const llms_t *l);

/* Case-insensitive name lookup: index, or -1. */
int llms_find(const llms_t *l, const char *name);
const llm_t *llms_get(const llms_t *l, const char *name);

/* ---------- agents (agents/<slug>.json) ---------- */

#define AGENTS_MAX          32
#define AGENT_PROMPT_MAX    16384
#define AGENT_STOP_MAX      8    /* stop sequences */
#define AGENT_STOP_LEN_MAX  128
#define AGENT_TOOLS_MAX     16   /* mcp servers per agent */
#define AGENT_TOOL_NAME_MAX 64
#define AGENT_CMD_MAX       1024 /* stdio command line */
#define AGENT_TERM_MAX      16   /* terminal tool names per server */
#define AGENT_TERM_NAME_MAX 64
#define AGENT_PROTO_MAX     24   /* mcp revision */
#define AGENT_EFFORT_MAX    24   /* reasoning effort */

/* optional JSON number/boolean: `set` distinguishes absent from 0/false */
typedef struct { int set; double v; } agent_num_t;
typedef struct { int set; int v; } agent_bool_t;

typedef struct {
    /* the llmkit inference_options, same names for every protocol;
     * agent-specific (shared endpoints are not forced to share them) */
    agent_num_t temperature, top_p, top_k, thinking_budget, max_tokens,
                presence_penalty, frequency_penalty, seed;
    char reasoning_effort[AGENT_EFFORT_MAX];          /* "" = unset */
    char stop[AGENT_STOP_MAX][AGENT_STOP_LEN_MAX];    /* stop_count used */
    size_t stop_count;
    agent_bool_t stream;                              /* default true */
} agent_inference_t;

typedef struct {
    char type[8];                        /* stdio | http | sse */
    char name[AGENT_TOOL_NAME_MAX];      /* tool-name prefix the model sees */
    char command_line[AGENT_CMD_MAX];    /* stdio only */
    char url[CFG_URL_MAX];               /* http/sse only */
    cfg_kv_t headers[CFG_HDR_MAX];       /* http/sse only */
    size_t header_count;
    char protocol[AGENT_PROTO_MAX];      /* mcp revision, "" = default */
    int required;                        /* connect failure stops the run */
    char terminal_tools[AGENT_TERM_MAX][AGENT_TERM_NAME_MAX];
    size_t term_count;
} agent_tool_t;

typedef struct {
    char name[CFG_NAME_MAX];
    char llm[CFG_NAME_MAX];              /* reference into llms.json */
    agent_inference_t infer;
    char system_prompt[AGENT_PROMPT_MAX]; /* "" = none */
    agent_tool_t tools[AGENT_TOOLS_MAX];
    size_t tool_count;
} agent_t;

typedef struct {
    agent_t *items[AGENTS_MAX]; /* heap each: too big by value */
    size_t count;
} agents_t;

typedef enum {
    AGENTS_OK = 0,
    AGENTS_E_JSON = 1, /* not JSON / not an array */
    AGENTS_E_FIELD = 2 /* valid JSON, but a value/key is rejected */
} agents_parse_result_t;

void agents_free(agents_t *a);

/* Load every agent json file under agents/ into *a (empty list on any
 * problem; broken agents are dropped; entries sorted by name). Returns
 * 0 if the directory was read, 1 if the list started empty. */
int agents_load(agents_t *a);

/* Persist as one agents/<slug>.json per agent (atomic writes) and
 * delete agent files no longer in the list. 0 on success. */
int agents_save(const agents_t *a);

/* Strict parse+validate a full agent array (PUT body). `llms` is the
 * current endpoint list: references are checked against it (pass NULL
 * to skip reference checks) and anthropic-specific inference rules are
 * enforced per referenced protocol. On failure fills err_field
 * (e.g. "agents[1].tools[0].url", "" for whole-document errors) and
 * err_msg. Result is sorted by name. */
agents_parse_result_t agents_from_json(const char *buf, size_t len,
                                       const llms_t *llms, agents_t *out,
                                       char *err_field, size_t err_field_n,
                                       char *err_msg, size_t err_msg_n);

/* Serialize as a compact sorted JSON array. With `llms` non-NULL every
 * agent carries a live "llm_ok":true/false reference flag (used by GET,
 * like the projects "exists" flag); NULL omits it (PUT echo). */
char *agents_to_json(const agents_t *a, const llms_t *llms);

#endif
