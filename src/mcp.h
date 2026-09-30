#ifndef FLOWER_MCP_H
#define FLOWER_MCP_H

/*
 * mcp — flower's own mcp servers, the json-rpc dispatch behind POST
 * /mcp and POST /scan/mcp (streamable-http transport, plain json
 * replies). Any mcp client — the llmkit runner included — registers
 * {"type":"http","name":"flower","url":"http://host:port/mcp"} as a
 * tool server and sees the tools of that surface as flower.<tool>.
 *
 * Two surfaces, two tool tables:
 *
 *   MCP_RESEARCH  POST /mcp — the research tools (web_search,
 *                 web_fetch, read_file, list_files, grep). What the
 *                 researcher agents and any external client get.
 *   MCP_SCAN      POST /scan/mcp — the project scan's write-back
 *                 tools (set_project_details, add_context_item,
 *                 defined in src/scan.c). They apply to the project
 *                 of the running scan and fail when none is.
 *
 * The wire contract is mcp's json-rpc subset: initialize, the
 * initialized notification, tools/list, tools/call (see the mcp
 * revision 2025-11-25). Tool descriptions and per-argument hints are
 * prompt files (prompts/mcp/<tool>/…), so they are tweakable without
 * touching C.
 */

#include <cJSON.h>
#include <stddef.h>

/* one tool argument: json-schema type and the required flag */
typedef struct {
    const char *name;
    const char *type; /* "string" | "number" */
    int required;
} mcp_arg_t;

/* a tool implementation: the parsed arguments in, a malloc'd text
 * result out. NULL + an err message is a tool failure — returned to
 * the model as isError content, not an rpc error. */
typedef char *(*mcp_tool_fn)(const cJSON *args, char *err, size_t err_n);

/* optional per-surface logger: called once a tools/call finished
 * (result is the tool's text, is_error flags a failed call). The
 * conversation recorder behind the project scan uses it to fold
 * every tool use into the conversation's transcript. */
typedef void (*mcp_log_fn)(const char *tool, const cJSON *args,
                           const char *result, int is_error);

typedef struct {
    const char *name;
    const mcp_arg_t *args; /* NULL-name terminated */
    mcp_tool_fn fn;
} mcp_tool_t;

typedef struct {
    const mcp_tool_t *tools;
    size_t count;
    mcp_log_fn log; /* optional, may be NULL */
} mcp_table_t;

/* the research tool set behind POST /mcp (defined in mcp.c) */
extern mcp_table_t MCP_RESEARCH;
/* the scan write-back tool set behind POST /scan/mcp (scan.c) */
extern mcp_table_t MCP_SCAN;

/*
 * Handle one POST /mcp-style body (a single json-rpc message; llmkit
 * sends them one per request) against `t`'s tools. Returns a malloc'd
 * application/json reply, or NULL when the message was a notification
 * (no "id" — answer 202 with an empty body; *is_notification is
 * then 1). Unparsable input is answered with a -32700 error reply
 * like the spec says.
 */
char *mcp_handle_post(const mcp_table_t *t, const char *body, size_t len,
                      int *is_notification);

#endif
