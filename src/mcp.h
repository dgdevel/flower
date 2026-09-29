#ifndef FLOWER_MCP_H
#define FLOWER_MCP_H

/*
 * mcp — flower's own mcp server, the tools it offers to its agents
 * over POST /mcp (streamable-http transport, plain json replies).
 * Any mcp client — the llmkit runner included — registers
 * {"type":"http","name":"flower","url":"http://host:port/mcp"} as a
 * tool server and sees the tools below as flower.<tool>.
 *
 * The wire contract is mcp's json-rpc subset: initialize, the
 * initialized notification, tools/list, tools/call (see the mcp
 * revision 2025-11-25). Tool descriptions and per-argument hints are
 * prompt files (prompts/mcp/<tool>/…), so they are tweakable without
 * touching C.
 */

#include <stddef.h>

/*
 * Handle one POST /mcp body (a single json-rpc message; llmkit sends
 * them one per request). Returns a malloc'd application/json reply,
 * or NULL when the message was a notification (no "id" — answer
 * 202 with an empty body; *is_notification is then 1). Unparsable
 * input is answered with a -32700 error reply like the spec says.
 */
char *mcp_handle_post(const char *body, size_t len, int *is_notification);

/* how many tools flower offers (for tests) */
size_t mcp_tool_count(void);

#endif
