/* mcp_scan.c - the project mcp server of a scan: `flower mcp-scan <P-id>`.
 *
 * One tool, `update_project(details, contextItems, resources)`: it fills
 * details.md, context.md, resources.json of the project in one call. The
 * model never sees or writes raw files: this server owns them.
 *
 * json-rpc over stdio (newline-delimited), the surface llmkit speaks:
 * initialize / notifications/initialized / ping / tools/list /
 * tools/call. See llmkit/docs/builtin-mcp.md for the protocol shape. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- json-rpc replies ---- */

/* print one json-rpc result reply; id_raw is the caller's printed id
 * ("null" when the request had none). result is consumed (freed). */
static void send_result(const char *id_raw, cJSON *result)
{
    char *s = cJSON_PrintUnformatted(result);

    cJSON_Delete(result);
    if (!s) {
        fprintf(stderr, "flower mcp-scan: out of memory\n");
        return;
    }
    printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n", id_raw, s);
    fflush(stdout);
    free(s);
}

/* print one json-rpc error reply. code -32601 method not found,
 * -32602 invalid params, -32603 internal. */
static void send_error(const char *id_raw, int code, const char *msg)
{
    printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,"
           "\"message\":\"",
           id_raw, code);
    for (; *msg; msg++) {
        if ((unsigned char)*msg == '"' || (unsigned char)*msg == '\\')
            putchar('\\');
        if ((unsigned char)*msg >= 0x20)
            putchar(*msg);
    }
    printf("\"}}\n");
    fflush(stdout);
}

/* the update_project input schema, from the scan design. */
static const char *schema_json =
    "{\"type\":\"object\",\"required\":[\"details\",\"contextItems\","
    "\"resources\"],"
    "\"properties\":{"
    "\"details\":{\"type\":\"object\",\"description\":\"project "
    "details, one chapter per field\",\"properties\":{"
    "\"Output\":{\"type\":\"string\"},"
    "\"Objectives\":{\"type\":\"string\"},"
    "\"Scope\":{\"type\":\"string\"},"
    "\"Constraints\":{\"type\":\"string\"},"
    "\"Stakeholders\":{\"type\":\"string\"}}},"
    "\"contextItems\":{\"type\":\"array\",\"description\":\"context "
    "items\",\"items\":{\"type\":\"object\","
    "\"required\":[\"type\",\"content\"],\"properties\":{"
    "\"type\":{\"type\":\"string\",\"enum\":[\"Fact\",\"Pattern\","
    "\"Risk\",\"Success metric\",\"Failure sign\","
    "\"Evaluation method\",\"Rule\"]},"
    "\"content\":{\"type\":\"string\"}}}},"
    "\"resources\":{\"type\":\"array\",\"description\":\"resources\","
    "\"items\":{\"type\":\"object\","
    "\"required\":[\"type\",\"content\"],\"properties\":{"
    "\"type\":{\"type\":\"string\",\"enum\":[\"path\",\"url\","
    "\"pointer\"]},"
    "\"content\":{\"type\":\"string\"}}}}}}";

static cJSON *tools_list_result(void)
{
    cJSON *res, *tools, *tool, *schema;

    res = cJSON_CreateObject();
    tools = cJSON_CreateArray();
    tool = cJSON_CreateObject();
    schema = cJSON_Parse(schema_json);
    if (!res || !tools || !tool || !schema)
        goto fail;
    if (!cJSON_AddStringToObject(tool, "name", "update_project"))
        goto fail;
    if (!cJSON_AddStringToObject(tool, "description",
                                 "Write the complete project data set: "
                                 "details, context items, resources. One "
                                 "call ends the scan."))
        goto fail;
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    schema = NULL;
    cJSON_AddItemToArray(tools, tool);
    tool = NULL;
    cJSON_AddItemToObject(res, "tools", tools);
    tools = NULL;
    return res;

fail:
    cJSON_Delete(schema);
    cJSON_Delete(tool);
    cJSON_Delete(tools);
    cJSON_Delete(res);
    return NULL;
}

/* ---- validation + rendering ---- */

static const char *const details_order[] = {
    "Output", "Objectives", "Scope", "Constraints", "Stakeholders", NULL
};

static const char *const context_types[] = {
    "Fact", "Pattern", "Risk", "Success metric", "Failure sign",
    "Evaluation method", "Rule", NULL
};

static const char *const resource_types[] = {
    "path", "url", "pointer", NULL
};

static int type_ok(const char *const *list, const char *v)
{
    int i;

    for (i = 0; list[i]; i++)
        if (!strcmp(list[i], v))
            return 1;
    return 0;
}

/* growable text for rendering the files. */
struct rbuf {
    char *data;
    size_t len, cap;
};

static int rputs(struct rbuf *b, const char *s)
{
    size_t n = strlen(s);
    char *p;

    if (b->len + n + 1 > b->cap) {
        b->cap = b->cap ? b->cap : 256;
        while (b->cap < b->len + n + 1)
            b->cap *= 2;
        p = realloc(b->data, b->cap);
        if (!p)
            return -1;
        b->data = p;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

/* validate the update_project arguments and render the three files.
 * Returns 0 and fills det/ctx/res (malloc'd, caller frees each) on
 * success, -1 on invalid arguments (err filled). */
static int validate_and_render(const cJSON *args, char **det, char **ctx,
                               char **res, char *err, size_t errsz)
{
    const cJSON *details, *items, *resources, *it;
    struct rbuf b = {0};
    int i;

    if (!cJSON_IsObject(args)) {
        snprintf(err, errsz, "arguments must be an object");
        return -1;
    }
    details = cJSON_GetObjectItemCaseSensitive(args, "details");
    items = cJSON_GetObjectItemCaseSensitive(args, "contextItems");
    resources = cJSON_GetObjectItemCaseSensitive(args, "resources");
    if (!cJSON_IsObject(details) || !cJSON_IsArray(items)
        || !cJSON_IsArray(resources)) {
        snprintf(err, errsz,
                 "details must be an object, contextItems and resources "
                 "arrays");
        return -1;
    }

    /* details.md: one chapter per nonempty field, fixed order */
    for (i = 0; details_order[i]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(details,
                                                          details_order[i]);

        if (!cJSON_IsString(v) || !v->valuestring[0])
            continue;
        if (rputs(&b, "# ") != 0 || rputs(&b, details_order[i]) != 0
            || rputs(&b, "\n\n") != 0 || rputs(&b, v->valuestring) != 0
            || rputs(&b, "\n\n") != 0)
            goto oom;
    }
    *det = b.data ? b.data : strdup("");
    if (!*det)
        goto oom;

    /* context.md: one list item per entry */
    b.data = NULL;
    b.len = b.cap = 0;
    cJSON_ArrayForEach(it, items) {
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(it, "type");
        const cJSON *co = cJSON_GetObjectItemCaseSensitive(it, "content");

        if (!cJSON_IsString(ty) || !cJSON_IsString(co)
            || !ty->valuestring || !co->valuestring
            || !type_ok(context_types, ty->valuestring)) {
            snprintf(err, errsz,
                     "contextItems entries need type (one of Fact, "
                     "Pattern, Risk, Success metric, Failure sign, "
                     "Evaluation method, Rule) and content strings");
            free(b.data);
            free(*det);
            return -1;
        }
        if (rputs(&b, "- [") != 0 || rputs(&b, ty->valuestring) != 0
            || rputs(&b, "] ") != 0 || rputs(&b, co->valuestring) != 0
            || rputs(&b, "\n") != 0)
            goto oom_ctx;
    }
    *ctx = b.data ? b.data : strdup("");
    if (!*ctx)
        goto oom_ctx;

    /* resources.json: array of {type, content} objects */
    {
        cJSON *arr = cJSON_CreateArray();
        char *text;

        if (!arr)
            goto oom_ctx;
        cJSON_ArrayForEach(it, resources) {
            const cJSON *ty = cJSON_GetObjectItemCaseSensitive(it, "type");
            const cJSON *co = cJSON_GetObjectItemCaseSensitive(it,
                                                               "content");
            cJSON *o;

            if (!cJSON_IsString(ty) || !cJSON_IsString(co)
                || !ty->valuestring || !co->valuestring
                || !type_ok(resource_types, ty->valuestring)) {
                snprintf(err, errsz,
                         "resources entries need type (path, url, or "
                         "pointer) and content strings");
                cJSON_Delete(arr);
                free(*det);
                free(*ctx);
                return -1;
            }
            o = cJSON_CreateObject();
            if (!o
                || !cJSON_AddStringToObject(o, "type", ty->valuestring)
                || !cJSON_AddStringToObject(o, "content",
                                            co->valuestring)) {
                cJSON_Delete(o);
                cJSON_Delete(arr);
                goto oom_ctx;
            }
            cJSON_AddItemToArray(arr, o);
        }
        text = cJSON_Print(arr);
        cJSON_Delete(arr);
        if (!text)
            goto oom_ctx;
        *res = text;
    }
    return 0;

oom_ctx:
    free(b.data);
    free(*det);
    free(*ctx);
    goto oom_common;
oom:
    free(b.data);
oom_common:
    snprintf(err, errsz, "out of memory");
    return -1;
}

int mcp_scan_apply(const char *project_dir, const cJSON *args,
                   char *err, size_t errsz)
{
    char *det = NULL, *ctx = NULL, *res = NULL;
    char path[FLOWER_PATH_MAX];
    int rc;

    if (validate_and_render(args, &det, &ctx, &res, err, errsz) != 0)
        return -1;
    rc = -2;
    flower_path_join3(path, sizeof path, project_dir, "details", "md");
    if (flower_write_file(path, det) != 0) {
        snprintf(err, errsz, "cannot write %s", path);
        goto out;
    }
    flower_path_join3(path, sizeof path, project_dir, "context", "md");
    if (flower_write_file(path, ctx) != 0) {
        snprintf(err, errsz, "cannot write %s", path);
        goto out;
    }
    flower_path_join3(path, sizeof path, project_dir, "resources", "json");
    if (flower_write_file(path, res) != 0) {
        snprintf(err, errsz, "cannot write %s", path);
        goto out;
    }
    rc = 0;

out:
    free(det);
    free(ctx);
    free(res);
    return rc;
}

/* ---- request routing ---- */

/* initialize reply: echo the client's protocol version. */
static void reply_initialize(const cJSON *root, const char *id_raw)
{
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const cJSON *pv = params
        ? cJSON_GetObjectItemCaseSensitive(params, "protocolVersion")
        : NULL;
    const char *proto = (cJSON_IsString(pv) && pv->valuestring[0])
        ? pv->valuestring
        : "2025-11-25";
    cJSON *res = cJSON_CreateObject();
    cJSON *info;

    if (!res)
        goto fail;
    if (!cJSON_AddStringToObject(res, "protocolVersion", proto))
        goto fail;
    if (!cJSON_AddObjectToObject(res, "capabilities"))
        goto fail;
    info = cJSON_AddObjectToObject(res, "serverInfo");
    if (!info)
        goto fail;
    if (!cJSON_AddStringToObject(info, "name", "flower"))
        goto fail;
    if (!cJSON_AddStringToObject(info, "version", FLOWER_VERSION))
        goto fail;
    send_result(id_raw, res);
    return;

fail:
    cJSON_Delete(res);
    send_error(id_raw, -32603, "out of memory");
}

/* tools/call update_project: apply, reply, signal exit. */
static int call_update_project(const cJSON *params,
                               const struct flower_project *p,
                               const char *id_raw)
{
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params,
                                                         "arguments");
    char err[512];
    cJSON *res, *content, *text;

    if (!cJSON_IsObject(args)) {
        send_error(id_raw, -32602, "arguments must be an object");
        return 0;
    }
    if (mcp_scan_apply(p->dir, args, err, sizeof err) == 0) {
        res = cJSON_CreateObject();
        content = cJSON_CreateArray();
        text = cJSON_CreateObject();
        if (res && content && text
            && cJSON_AddStringToObject(text, "type", "text")
            && cJSON_AddStringToObject(text, "text", "project data written")
            && cJSON_AddItemToArray(content, text)
            && cJSON_AddItemToObject(res, "content", content)) {
            text = NULL;
            content = NULL;
            send_result(id_raw, res);
            res = NULL;
            return 1; /* applied: the server exits after the reply */
        }
        cJSON_Delete(text);
        cJSON_Delete(content);
        cJSON_Delete(res);
        send_error(id_raw, -32603, "out of memory");
        return 0;
    }
    send_error(id_raw, -32602, err);
    return 0;
}

/* one json-rpc request: route and reply. Returns 0 to continue, 1 when
 * update_project was applied (the server exits after the reply). */
static int handle_request(const char *line, const struct flower_project *p,
                          char *id_raw, size_t idsz)
{
    cJSON *root;
    const cJSON *method, *id, *params, *name;

    root = cJSON_Parse(line);
    if (!root)
        return 0; /* ignore garbage: the client owns the protocol */
    method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (!cJSON_IsString(method)) {
        cJSON_Delete(root);
        return 0; /* a reply line on our stdin: ignore */
    }
    id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (id) {
        char *s = cJSON_PrintUnformatted(id);

        if (s) {
            snprintf(id_raw, idsz, "%.*s", (int)(idsz - 1), s);
            free(s);
        }
    }

    if (!strcmp(method->valuestring, "initialize")) {
        reply_initialize(root, id_raw);
    } else if (!strcmp(method->valuestring, "notifications/initialized")) {
        /* notification: no reply */
    } else if (!strcmp(method->valuestring, "ping")) {
        send_result(id_raw, cJSON_CreateObject());
    } else if (!strcmp(method->valuestring, "tools/list")) {
        cJSON *res = tools_list_result();

        if (res)
            send_result(id_raw, res);
        else
            send_error(id_raw, -32603, "out of memory");
    } else if (!strcmp(method->valuestring, "tools/call")) {
        params = cJSON_GetObjectItemCaseSensitive(root, "params");
        name = params
            ? cJSON_GetObjectItemCaseSensitive(params, "name")
            : NULL;
        if (cJSON_IsString(name)
            && !strcmp(name->valuestring, "update_project")) {
            int rc = call_update_project(params, p, id_raw);

            cJSON_Delete(root);
            return rc;
        }
        send_error(id_raw, -32601, "unknown tool");
    } else {
        send_error(id_raw, -32601, "method not found");
    }
    cJSON_Delete(root);
    return 0;
}

int mcp_scan_cmd(int argc, char **argv)
{
    struct flower_project p;
    char *line = NULL;
    size_t cap = 0;

    if (argc != 1 || !argv[0]) {
        fprintf(stderr, "usage: flower mcp-scan <P-id>\n");
        return 2;
    }
    if (project_by_id(argv[0], &p) != 0) {
        fprintf(stderr, "flower mcp-scan: no project %s\n", argv[0]);
        return 1;
    }

    while (getline(&line, &cap, stdin) != -1) {
        char id_raw[128] = "null";
        int rc = handle_request(line, &p, id_raw, sizeof id_raw);

        if (rc != 0) {
            free(line);
            return rc > 0 ? 0 : 1;
        }
    }
    free(line);
    return 0;
}
