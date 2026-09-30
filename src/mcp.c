/*
 * mcp — flower's own mcp servers: the json-rpc dispatch behind
 * POST /mcp and POST /scan/mcp. See mcp.h for the surface; the
 * research tools live in src/web.c and src/fs.c, the scan write-back
 * tools in src/scan.c, their prompt texts under prompts/mcp/.
 */
#define _POSIX_C_SOURCE 200809L

#include "mcp.h"

#include "fs.h"
#include "prompts.h"
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROTOCOL_REV "2025-11-25" /* the revision llmkit speaks */

/* ---------- tool implementations (the research set) ---------- */

static char *fn_web_search(const cJSON *args, char *err, size_t err_n)
{
    const cJSON *q = cJSON_GetObjectItemCaseSensitive(args, "query");
    if (!cJSON_IsString(q) || !q->valuestring[0]) {
        snprintf(err, err_n, "missing required string argument 'query'");
        return NULL;
    }
    return web_search(q->valuestring, err, err_n);
}

static char *fn_web_fetch(const cJSON *args, char *err, size_t err_n)
{
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(args, "url");
    if (!cJSON_IsString(u) || !u->valuestring[0]) {
        snprintf(err, err_n, "missing required string argument 'url'");
        return NULL;
    }
    return web_read(u->valuestring, err, err_n);
}

static const mcp_arg_t ARGS_WEB_SEARCH[] = {
    { "query", "string", 1 }, { NULL }
};
static const mcp_arg_t ARGS_WEB_FETCH[] = {
    { "url", "string", 1 }, { NULL }
};
static const mcp_arg_t ARGS_READ_FILE[] = {
    { "path",   "string", 1 },
    { "offset", "number", 0 },
    { "length", "number", 0 },
    { NULL }
};
static const mcp_arg_t ARGS_LIST_FILES[] = {
    { "path", "string", 1 },
    { "glob", "string", 1 },
    { NULL }
};
static const mcp_arg_t ARGS_GREP[] = {
    { "glob",    "string", 1 },
    { "pattern", "string", 1 },
    { NULL }
};

/* the research surface behind POST /mcp. The scan surface (MCP_SCAN)
 * is defined next to its tools, in src/scan.c. */
static const mcp_tool_t RESEARCH_TOOLS[] = {
    { "web_search", ARGS_WEB_SEARCH, fn_web_search },
    { "web_fetch",  ARGS_WEB_FETCH,  fn_web_fetch },
    { "read_file",  ARGS_READ_FILE,  fs_tool_read_file },
    { "list_files", ARGS_LIST_FILES, fs_tool_list_files },
    { "grep",       ARGS_GREP,       fs_tool_grep },
};
mcp_table_t MCP_RESEARCH = {
    RESEARCH_TOOLS, sizeof RESEARCH_TOOLS / sizeof RESEARCH_TOOLS[0], NULL
};

/* ---------- prompt-file lookups with compiled-in fallbacks ---------- */

static char *tool_description(const mcp_tool_t *t)
{
    char path[128];
    snprintf(path, sizeof path, "mcp/%s/description.txt", t->name);
    char *d = prompt_text(path);
    if (d && *d) return d;
    free(d);
    return strdup(t->name);
}

static char *arg_description(const mcp_tool_t *t, const char *arg)
{
    char path[192];
    snprintf(path, sizeof path, "mcp/%s/arguments/%s.txt", t->name, arg);
    char *d = prompt_text(path);
    if (d && *d) return d;
    free(d);
    return strdup(arg);
}

/* ---------- json-rpc helpers ---------- */

static cJSON *id_dup(const cJSON *id)
{
    if (!id || cJSON_IsNull(id)) return cJSON_CreateNull();
    return cJSON_Duplicate((cJSON *)id, 1);
}

static cJSON *rpc_result(const cJSON *id, cJSON *result)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) return NULL;
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id_dup(id));
    cJSON_AddItemToObject(r, "result", result ? result : cJSON_CreateObject());
    return r;
}

static cJSON *rpc_error(const cJSON *id, int code, const char *message)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id_dup(id));
    cJSON_AddItemToObject(r, "error", e);
    return r;
}

static char *json_or_internal(cJSON *reply)
{
    if (!reply)
        return strdup("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
                      "{\"code\":-32603,\"message\":\"internal error\"}}");
    char *json = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    return json;
}

/* ---------- method handlers ---------- */

static cJSON *handle_initialize(void)
{
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "protocolVersion", PROTOCOL_REV);
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(res, "capabilities", caps);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "flower");
    cJSON_AddStringToObject(info, "version", "1");
    cJSON_AddItemToObject(res, "serverInfo", info);
    return res;
}

static cJSON *handle_tools_list(const mcp_table_t *t)
{
    cJSON *tools = cJSON_CreateArray();
    for (size_t i = 0; i < t->count; i++) {
        const mcp_tool_t *one = &t->tools[i];
        cJSON *props = cJSON_CreateObject();
        cJSON *required = cJSON_CreateArray();
        for (size_t a = 0; one->args[a].name; a++) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "type", one->args[a].type);
            char *d = arg_description(one, one->args[a].name);
            cJSON_AddStringToObject(p, "description",
                                     d ? d : one->args[a].name);
            free(d);
            cJSON_AddItemToObject(props, one->args[a].name, p);
            if (one->args[a].required)
                cJSON_AddItemToArray(required,
                                     cJSON_CreateString(one->args[a].name));
        }
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON_AddItemToObject(schema, "required", required);

        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", one->name);
        char *d = tool_description(one);
        cJSON_AddStringToObject(tool, "description", d ? d : one->name);
        free(d);
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "tools", tools);
    return res;
}

static cJSON *handle_tools_call(const mcp_table_t *t, const cJSON *params,
                                const cJSON *id)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name) || !name->valuestring[0])
        return rpc_error(id, -32602, "tools/call needs a tool name");

    const mcp_tool_t *one = NULL;
    for (size_t i = 0; i < t->count; i++)
        if (strcmp(t->tools[i].name, name->valuestring) == 0) {
            one = &t->tools[i];
            break;
        }
    if (!one)
        return rpc_error(id, -32602, "unknown tool");

    const cJSON *arguments =
        cJSON_GetObjectItemCaseSensitive(params, "arguments");

    char err[512] = "";
    char *out = one->fn(arguments, err, sizeof err);

    /* the surface's recorder (if any) folds the call into the
     * conversation transcript this tool call belongs to */
    if (t->log)
        t->log(name->valuestring, arguments, out ? out : err, out == NULL);

    /* one text block either way; a tool failure is content the model
     * can read, flagged isError — the run keeps going (llmkit maps
     * it to a tool-error turn) */
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", out ? out : err);
    free(out);
    cJSON *content = cJSON_CreateArray();
    cJSON_AddItemToArray(content, block);

    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "content", content);
    cJSON_AddBoolToObject(res, "isError", out == NULL);
    return rpc_result(id, res);
}

/* ---------- entry point ---------- */

char *mcp_handle_post(const mcp_table_t *t, const char *body, size_t len,
                      int *is_notification)
{
    *is_notification = 0;
    cJSON *msg = cJSON_ParseWithLength(body, len);
    if (!msg)
        return json_or_internal(rpc_error(NULL, -32700, "parse error"));
    if (!cJSON_IsObject(msg)) {
        cJSON_Delete(msg);
        return json_or_internal(rpc_error(NULL, -32600, "invalid request"));
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    if (!cJSON_IsString(method) || !method->valuestring[0]) {
        cJSON_Delete(msg);
        return json_or_internal(rpc_error(id, -32600, "invalid request"));
    }

    /* notifications (no id member) get no reply */
    if (!id) {
        cJSON_Delete(msg);
        *is_notification = 1;
        return NULL;
    }

    const cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    const char *m = method->valuestring;

    cJSON *reply;
    if (strcmp(m, "initialize") == 0) {
        reply = rpc_result(id, handle_initialize());
    } else if (strcmp(m, "tools/list") == 0) {
        reply = rpc_result(id, handle_tools_list(t));
    } else if (strcmp(m, "tools/call") == 0) {
        reply = handle_tools_call(t, params, id);
    } else if (strcmp(m, "ping") == 0) {
        reply = rpc_result(id, cJSON_CreateObject());
    } else {
        reply = rpc_error(id, -32601, "method not found");
    }

    cJSON_Delete(msg);
    return json_or_internal(reply);
}
