/* mcpserve.c - `flower mcp <P> <area>` and `flower mcp <P> task <T>
   <area>`: stdio json-rpc servers exposing one project area (details,
   context, resource) or one task area (config, context, action) as
   mcp tools. the tool handlers mirror the cli verbs in flower.c;
   stdout carries only json-rpc responses, everything diagnostic goes
   to stderr. */
#define _POSIX_C_SOURCE 200809L
#include "flower.h"
#include "prompts.gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* the served area, fixed once cmd_mcp validated its arguments */
static struct {
    const char *area;  /* "details" | "context" | "resource" | the task
                          areas "config", "context", "action" */
    const char *sname; /* serverInfo name after the "flower-" prefix:
                          the area itself, or "task-<area>" */
    char *rel;         /* the served file under $FLOWER_BASE */
    int kind;          /* FV_FIELDS | FV_CONTEXT | ... */
} g;

/* ---------- json build helpers (json.c is allocation-only) ---------- */

/* object member add: takes ownership of val, drops it on oom */
static void jobj_set(json_t *obj, const char *key, json_t *val) {
    if (!obj || !val) return;
    val->key = str_dup(key);
    if (!val->key) {
        json_free(val);
        return;
    }
    json_add(obj, val);
}

static void jobj_str(json_t *obj, const char *key, const char *s) {
    jobj_set(obj, key, json_str(s));
}

/* ---------- tool results ---------- */

/* tools/call result envelope: one text block plus the error flag */
static json_t *text_result(const char *text, int is_err) {
    json_t *res = json_obj();
    json_t *content = json_arr();
    json_t *item = json_obj();
    jobj_str(item, "type", "text");
    jobj_str(item, "text", text);
    json_add(content, item);
    jobj_set(res, "content", content);
    jobj_set(res, "isError", json_bool(is_err));
    return res;
}

/* ---------- shared handler plumbing ---------- */

/* argument string, or NULL after "missing argument '<name>'" was
   written and *err set. non-string values count as missing: the tools
   stay forgiving, arguments are strings by schema */
static const char *arg_str(const json_t *args, const char *name,
                           buf_t *out, int *err) {
    const char *v = json_str_of(args, name);
    if (!v) {
        buf_appendf(out, "missing argument '%s'", name);
        *err = 1;
    }
    return v;
}

/* load the area file. 0 ok (doc may be empty), 1 after the isError
   text was written (load_rel already said why on stderr). the text
   names the basename of the served file, so the task areas report
   config.md/context.md/actions.md and not the area word */
static int load_area(md_doc_t *doc, buf_t *out, int *err) {
    if (load_rel(g.rel, g.kind, doc)) {
        buf_appendf(out, "cannot load %s.md",
                    strcmp(g.area, "action") ? g.area : "actions");
        *err = 1;
        return 1;
    }
    return 0;
}

/* save the mutated doc and write the success text: the absolute file
   path, exactly what the cli prints. quiet twin of save_rel - the
   stdout channel is reserved for json-rpc. frees doc. */
static void finish_save(md_doc_t *doc, buf_t *out, int *err) {
    char *path = store_path(g.rel);
    const char *base;
    if (!path) {
        buf_appendf(out, "out of memory");
        *err = 1;
        md_free_doc(doc);
        return;
    }
    if (md_save(path, doc) != 0) {
        buf_appendf(out, "cannot save %s", g.rel);
        *err = 1;
    } else if (path[0] == '/') {
        buf_appendf(out, "%s\n", path);
    } else if ((base = store_base()) && base[0] == '/') {
        buf_appendf(out, "%s/%s\n", base, path);
    } else {
        buf_appendf(out, "%s\n", path);
    }
    free(path);
    md_free_doc(doc);
}

/* ---------- details tools ---------- */

static void t_details_get(const json_t *a, buf_t *out, int *err) {
    const char *field = arg_str(a, "area", out, err);
    md_doc_t doc;
    md_chapter_t *c;
    int i;
    if (!field) return;
    if (!key_allowed(store_detail_fields, field)) {
        buf_reset(out);
        buf_appendf(out, "unknown field '%s'", field);
        *err = 1;
        return;
    }
    if (load_area(&doc, out, err)) return;
    c = md_find(&doc, field);
    if (!c) {
        buf_appendf(out, "no such field '%s'", field);
        *err = 1;
    } else {
        for (i = 0; i < c->nbody; i++)
            buf_appendf(out, "%s\n", c->body[i].text);
    }
    md_free_doc(&doc);
}

static void t_details_set(const json_t *a, buf_t *out, int *err) {
    const char *field = arg_str(a, "area", out, err);
    const char *value;
    md_doc_t doc;
    md_chapter_t *c;
    if (!field) return;
    value = arg_str(a, "value", out, err);
    if (!value) return;
    if (!key_allowed(store_detail_fields, field)) {
        buf_reset(out);
        buf_appendf(out, "unknown field '%s'", field);
        *err = 1;
        return;
    }
    if (load_area(&doc, out, err)) return;
    c = md_find(&doc, field);
    if (!c) c = doc_add_chapter(&doc, field, 1);
    if (!c || chapter_set_body(c, value) != 0) {
        md_free_doc(&doc);
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
        return;
    }
    finish_save(&doc, out, err);
}

static void t_details_remove(const json_t *a, buf_t *out, int *err) {
    const char *field = arg_str(a, "area", out, err);
    md_doc_t doc;
    md_chapter_t *c;
    if (!field) return;
    if (!key_allowed(store_detail_fields, field)) {
        buf_reset(out);
        buf_appendf(out, "unknown field '%s'", field);
        *err = 1;
        return;
    }
    if (load_area(&doc, out, err)) return;
    c = md_find(&doc, field);
    if (!c) {
        buf_appendf(out, "no such field '%s'", field);
        *err = 1;
        md_free_doc(&doc);
        return;
    }
    doc_remove_chapter(&doc, (int)(c - doc.ch));
    finish_save(&doc, out, err);
}

/* ---------- context tools ---------- */

/* shared prefix+title validation (cmd_context prologue). returns the
    malloc'd "<Prefix>: <Title>" and sets *title_out, NULL after the
    isError text was written */
static char *ctx_head(const json_t *a, char **title_out, buf_t *out,
                      int *err) {
    const char *pfx_arg = arg_str(a, "prefix", out, err);
    const char *title;
    const char *pfx;
    char *full;
    *title_out = NULL;
    if (!pfx_arg) return NULL;
    title = arg_str(a, "title", out, err);
    if (!title) return NULL;
    pfx = prefix_known(pfx_arg);
    if (!pfx) {
        buf_reset(out);
        buf_appendf(out, "unknown prefix '%s'", pfx_arg);
        *err = 1;
        return NULL;
    }
    if (!title[0]) {
        buf_reset(out);
        buf_appendf(out, "empty title");
        *err = 1;
        return NULL;
    }
    full = store_make_context_title(pfx, title);
    if (!full) {
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
        return NULL;
    }
    *title_out = str_dup(title);
    if (!*title_out) {
        free(full);
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
        return NULL;
    }
    return full;
}

static void t_context_list(const json_t *a, buf_t *out, int *err) {
    md_doc_t doc;
    int i;
    (void)a;
    if (load_area(&doc, out, err)) return;
    for (i = 0; i < doc.nch; i++)
        buf_appendf(out, "%s\n", doc.ch[i].title);
    md_free_doc(&doc);
}

static void t_context_get(const json_t *a, buf_t *out, int *err) {
    char *title = NULL;
    char *full = ctx_head(a, &title, out, err);
    md_doc_t doc;
    md_chapter_t *ch;
    int i;
    if (!full) return;
    if (load_area(&doc, out, err)) goto out;
    ch = md_find(&doc, full);
    if (!ch) {
        buf_appendf(out, "no such context '%s'", full);
        *err = 1;
    } else {
        for (i = 0; i < ch->nbody; i++)
            buf_appendf(out, "%s\n", ch->body[i].text);
    }
    md_free_doc(&doc);
out:
    free(full);
    free(title);
}

static void t_context_add(const json_t *a, buf_t *out, int *err) {
    char *title = NULL;
    char *full = ctx_head(a, &title, out, err);
    const char *body;
    md_doc_t doc;
    md_chapter_t *ch;
    if (!full) return;
    body = arg_str(a, "body", out, err);
    if (!body) goto out;
    if (!body[0]) {
        buf_reset(out);
        buf_appendf(out, "empty body");
        *err = 1;
        goto out;
    }
    if (load_area(&doc, out, err)) goto out;
    ch = md_find(&doc, full);
    if (ch || context_title_taken(&doc, title, NULL)) {
        buf_reset(out);
        buf_appendf(out, "duplicate context title '%s'", title);
        *err = 1;
        md_free_doc(&doc);
        goto out;
    }
    ch = doc_add_chapter(&doc, full, 1);
    if (!ch || chapter_set_body(ch, body) != 0) {
        md_free_doc(&doc);
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
        goto out;
    }
    finish_save(&doc, out, err);
out:
    free(full);
    free(title);
}

static void t_context_edit(const json_t *a, buf_t *out, int *err) {
    char *title = NULL;
    char *full = ctx_head(a, &title, out, err);
    const char *body;
    md_doc_t doc;
    md_chapter_t *ch;
    if (!full) return;
    body = arg_str(a, "body", out, err);
    if (!body) goto out;
    if (load_area(&doc, out, err)) goto out;
    ch = md_find(&doc, full);
    if (!ch) {
        buf_reset(out);
        buf_appendf(out, "no such context '%s'", full);
        *err = 1;
        md_free_doc(&doc);
    } else if (chapter_set_body(ch, body) != 0) {
        md_free_doc(&doc);
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
    } else {
        finish_save(&doc, out, err);
    }
out:
    free(full);
    free(title);
}

static void t_context_remove(const json_t *a, buf_t *out, int *err) {
    char *title = NULL;
    char *full = ctx_head(a, &title, out, err);
    md_doc_t doc;
    md_chapter_t *ch;
    if (!full) return;
    if (load_area(&doc, out, err)) goto out;
    ch = md_find(&doc, full);
    if (!ch) {
        buf_reset(out);
        buf_appendf(out, "no such context '%s'", full);
        *err = 1;
        md_free_doc(&doc);
        goto out;
    }
    doc_remove_chapter(&doc, (int)(ch - doc.ch));
    finish_save(&doc, out, err);
out:
    free(full);
    free(title);
}

/* ---------- resource tools ---------- */

static void t_resource_list(const json_t *a, buf_t *out, int *err) {
    md_doc_t doc;
    int i, j;
    (void)a;
    if (load_area(&doc, out, err)) return;
    for (i = 0; i < doc.nch; i++) {
        buf_appendf(out, "%s\n", doc.ch[i].title);
        for (j = 0; j < doc.ch[i].nbody; j++)
            buf_appendf(out, "  %s\n", doc.ch[i].body[j].text);
    }
    md_free_doc(&doc);
}

static void t_resource_get(const json_t *a, buf_t *out, int *err) {
    const char *name = arg_str(a, "name", out, err);
    md_doc_t doc;
    md_chapter_t *c;
    int i;
    if (!name) return;
    if (load_area(&doc, out, err)) return;
    c = md_find(&doc, name);
    if (!c) {
        buf_appendf(out, "no such resource '%s'", name);
        *err = 1;
    } else {
        for (i = 0; i < c->nbody; i++)
            buf_appendf(out, "%s\n", c->body[i].text);
    }
    md_free_doc(&doc);
}

static void t_resource_add(const json_t *a, buf_t *out, int *err) {
    const char *name = arg_str(a, "name", out, err);
    const char *ptr;
    md_doc_t doc;
    md_chapter_t *c;
    if (!name) return;
    ptr = arg_str(a, "pointer", out, err);
    if (!ptr) return;
    if (!name[0]) {
        buf_reset(out);
        buf_appendf(out, "empty resource name");
        *err = 1;
        return;
    }
    if (!ptr[0]) {
        buf_reset(out);
        buf_appendf(out, "empty pointer");
        *err = 1;
        return;
    }
    if (load_area(&doc, out, err)) return;
    c = md_find(&doc, name);
    if (c) {
        buf_reset(out);
        buf_appendf(out, "duplicate resource '%s'", name);
        *err = 1;
        md_free_doc(&doc);
        return;
    }
    c = doc_add_chapter(&doc, name, 1);
    if (!c || chapter_set_body(c, ptr) != 0) {
        md_free_doc(&doc);
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
        return;
    }
    finish_save(&doc, out, err);
}

static void t_resource_set(const json_t *a, buf_t *out, int *err) {
    const char *name = arg_str(a, "name", out, err);
    const char *ptr;
    md_doc_t doc;
    md_chapter_t *c;
    if (!name) return;
    ptr = arg_str(a, "pointer", out, err);
    if (!ptr) return;
    if (load_area(&doc, out, err)) return;
    c = md_find(&doc, name);
    if (!c) {
        buf_appendf(out, "no such resource '%s'", name);
        *err = 1;
        md_free_doc(&doc);
        return;
    }
    if (chapter_set_body(c, ptr) != 0) {
        md_free_doc(&doc);
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
        return;
    }
    finish_save(&doc, out, err);
}

static void t_resource_remove(const json_t *a, buf_t *out, int *err) {
    const char *name = arg_str(a, "name", out, err);
    md_doc_t doc;
    md_chapter_t *c;
    if (!name) return;
    if (load_area(&doc, out, err)) return;
    c = md_find(&doc, name);
    if (!c) {
        buf_appendf(out, "no such resource '%s'", name);
        *err = 1;
        md_free_doc(&doc);
        return;
    }
    doc_remove_chapter(&doc, (int)(c - doc.ch));
    finish_save(&doc, out, err);
}

/* ---------- task config tools ---------- */

/* cmd_config_set semantics on tasks/<T>/config.md: one "# Task"
   chapter, keys Name and Description only. load_rel treats a missing
   file as an empty doc, so this creates it (require_task made sure
   the directory exists) */
static void t_task_set(const json_t *a, buf_t *out, int *err) {
    const char *key = arg_str(a, "key", out, err);
    const char *val;
    md_doc_t doc;
    md_chapter_t *c;
    if (!key) return;
    val = arg_str(a, "value", out, err);
    if (!val) return;
    if (!key_allowed(store_task_keys, key)) {
        buf_reset(out);
        buf_appendf(out, "unknown key '%s'", key);
        *err = 1;
        return;
    }
    if (load_area(&doc, out, err)) return;
    c = md_find(&doc, "Task");
    if (!c) c = doc_add_chapter(&doc, "Task", 1);
    if (!c || md_kv_set(c, key, val) != 0) {
        md_free_doc(&doc);
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
        return;
    }
    finish_save(&doc, out, err);
}

/* ---------- action tools ---------- */

/* the given action id, or NULL after the "invalid action id '<id>'"
   or "no such action <id>" isError text was written */
static md_chapter_t *action_arg(const char *id, md_doc_t *doc,
                                buf_t *out, int *err) {
    md_chapter_t *c;
    if (!store_valid_id(id, 'A')) {
        buf_reset(out);
        buf_appendf(out, "invalid action id '%s'", id);
        *err = 1;
        return NULL;
    }
    c = action_find(doc, id);
    if (!c) {
        buf_reset(out);
        buf_appendf(out, "no such action %s", id);
        *err = 1;
    }
    return c;
}

/* the chapter's Status value, "?" when it has none - the cli list
   prints the same stand-in */
static const char *action_status(const md_chapter_t *c) {
    const char *s = md_kv_get((const char **)c->keys,
                              (const char **)c->vals, c->nkv, "Status");
    return s ? s : "?";
}

/* cmd_action add: duplicate title guard across the chapters, id from
   store_next_action_id, header "<id>: <title>", Description kv when
   given, Status pending */
static void t_action_add(const json_t *a, buf_t *out, int *err) {
    const char *title = arg_str(a, "title", out, err);
    const char *desc;
    md_doc_t doc;
    md_chapter_t *c;
    char *id;
    int i;
    if (!title) return;
    desc = json_str_of(a, "description");
    if (!title[0]) {
        buf_reset(out);
        buf_appendf(out, "empty action title");
        *err = 1;
        return;
    }
    if (load_area(&doc, out, err)) return;
    for (i = 0; i < doc.nch; i++) {
        char *t = action_title_of(&doc.ch[i]);
        int dup = t && !strcmp(t, title);
        free(t);
        if (dup) {
            buf_reset(out);
            buf_appendf(out, "duplicate action title '%s' (%s)", title,
                        doc.ch[i].title);
            *err = 1;
            md_free_doc(&doc);
            return;
        }
    }
    id = store_next_action_id(&doc);
    if (id) {
        char *hdr = xasprintf("%s: %s", id, title);
        c = hdr ? doc_add_chapter(&doc, hdr, 1) : NULL;
        free(hdr);
        free(id);
    } else {
        c = NULL;
    }
    if (!c || (desc && md_kv_set(c, "Description", desc) != 0) ||
        md_kv_set(c, "Status", "pending") != 0) {
        md_free_doc(&doc);
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
        return;
    }
    finish_save(&doc, out, err);
}

/* cmd_action set: key in store_action_keys, Status validated, Depends
   parsed with self-reference and unknown id guards */
static void t_action_set(const json_t *a, buf_t *out, int *err) {
    const char *id = arg_str(a, "id", out, err);
    const char *key, *val;
    md_doc_t doc;
    md_chapter_t *c;
    if (!id) return;
    key = arg_str(a, "key", out, err);
    if (!key) return;
    val = arg_str(a, "value", out, err);
    if (!val) return;
    if (!key_allowed(store_action_keys, key)) {
        buf_reset(out);
        buf_appendf(out, "unknown key '%s'", key);
        *err = 1;
        return;
    }
    if (!strcmp(key, "Status") && !store_valid_status(val)) {
        buf_reset(out);
        buf_appendf(out, "invalid Status '%s' (pending, complete)", val);
        *err = 1;
        return;
    }
    if (load_area(&doc, out, err)) return;
    c = action_arg(id, &doc, out, err);
    if (!c) {
        md_free_doc(&doc);
        return;
    }
    if (!strcmp(key, "Depends")) {
        char **ids = NULL;
        int cnt = store_parse_id_list(val, &ids), j;
        if (cnt < 0) {
            md_free_doc(&doc);
            buf_reset(out);
            buf_appendf(out, "out of memory");
            *err = 1;
            return;
        }
        for (j = 0; j < cnt; j++) {
            if (!strcmp(ids[j], id)) {
                buf_reset(out);
                buf_appendf(out, "action %s cannot depend on itself", id);
                *err = 1;
                store_free_ids(ids, cnt);
                md_free_doc(&doc);
                return;
            }
            if (!action_find(&doc, ids[j])) {
                buf_reset(out);
                buf_appendf(out, "unknown action %s", ids[j]);
                *err = 1;
                store_free_ids(ids, cnt);
                md_free_doc(&doc);
                return;
            }
        }
        store_free_ids(ids, cnt);
    }
    if (md_kv_set(c, key, val) != 0) {
        md_free_doc(&doc);
        buf_reset(out);
        buf_appendf(out, "out of memory");
        *err = 1;
        return;
    }
    finish_save(&doc, out, err);
}

/* cmd_action remove: blocked while another action depends on this one */
static void t_action_remove(const json_t *a, buf_t *out, int *err) {
    const char *id = arg_str(a, "id", out, err);
    md_doc_t doc;
    md_chapter_t *c;
    char *dep;
    if (!id) return;
    if (load_area(&doc, out, err)) return;
    c = action_arg(id, &doc, out, err);
    if (!c) {
        md_free_doc(&doc);
        return;
    }
    dep = action_depended_on(&doc, id, c);
    if (dep) {
        char *rid = NULL, *rtitle = NULL;
        action_split(dep, &rid, &rtitle);
        buf_reset(out);
        buf_appendf(out, "action %s depends on %s; remove or edit "
                         "dependents first",
                    rid ? rid : dep, id);
        free(rid);
        free(rtitle);
        *err = 1;
        md_free_doc(&doc);
        return;
    }
    doc_remove_chapter(&doc, (int)(c - doc.ch));
    finish_save(&doc, out, err);
}

/* cmd_action list: one "<id>: <title> [status]" line per action */
static void t_action_list(const json_t *a, buf_t *out, int *err) {
    md_doc_t doc;
    int i;
    (void)a;
    if (load_area(&doc, out, err)) return;
    for (i = 0; i < doc.nch; i++) {
        char *id, *title;
        if (!action_split(doc.ch[i].title, &id, &title)) continue;
        buf_appendf(out, "%s: %s [%s]\n", id, title,
                    action_status(&doc.ch[i]));
        free(id);
        free(title);
    }
    md_free_doc(&doc);
}

/* ---------- tool tables ---------- */

/* one string argument of the input schema: name plus prompt text */
typedef struct {
    const char *name;
    const char *desc;
} arg_spec_t;

typedef struct {
    const char *name;
    const char *desc;
    const arg_spec_t *args; /* NULL-terminated, may be empty */
    void (*fn)(const json_t *args, buf_t *out, int *err);
} tool_t;

static const arg_spec_t a_details_get[] = {
    {"area", prompt_mcp_details_get_arguments_area}, {NULL, NULL}};
static const arg_spec_t a_details_set[] = {
    {"area", prompt_mcp_details_set_arguments_area},
    {"value", prompt_mcp_details_set_arguments_value}, {NULL, NULL}};
static const arg_spec_t a_details_remove[] = {
    {"area", prompt_mcp_details_remove_arguments_area}, {NULL, NULL}};
static const arg_spec_t a_none[] = {{NULL, NULL}};
static const arg_spec_t a_context_get[] = {
    {"prefix", prompt_mcp_context_get_arguments_prefix},
    {"title", prompt_mcp_context_get_arguments_title}, {NULL, NULL}};
static const arg_spec_t a_context_add[] = {
    {"prefix", prompt_mcp_context_add_arguments_prefix},
    {"title", prompt_mcp_context_add_arguments_title},
    {"body", prompt_mcp_context_add_arguments_body}, {NULL, NULL}};
static const arg_spec_t a_context_edit[] = {
    {"prefix", prompt_mcp_context_edit_arguments_prefix},
    {"title", prompt_mcp_context_edit_arguments_title},
    {"body", prompt_mcp_context_edit_arguments_body}, {NULL, NULL}};
static const arg_spec_t a_context_remove[] = {
    {"prefix", prompt_mcp_context_remove_arguments_prefix},
    {"title", prompt_mcp_context_remove_arguments_title}, {NULL, NULL}};
static const arg_spec_t a_resource_get[] = {
    {"name", prompt_mcp_resource_get_arguments_name}, {NULL, NULL}};
static const arg_spec_t a_resource_add[] = {
    {"name", prompt_mcp_resource_add_arguments_name},
    {"pointer", prompt_mcp_resource_add_arguments_pointer}, {NULL, NULL}};
static const arg_spec_t a_resource_set[] = {
    {"name", prompt_mcp_resource_set_arguments_name},
    {"pointer", prompt_mcp_resource_set_arguments_pointer}, {NULL, NULL}};
static const arg_spec_t a_resource_remove[] = {
    {"name", prompt_mcp_resource_remove_arguments_name}, {NULL, NULL}};
static const arg_spec_t a_task_set[] = {
    {"key", prompt_mcp_task_set_arguments_key},
    {"value", prompt_mcp_task_set_arguments_value}, {NULL, NULL}};
static const arg_spec_t a_action_add[] = {
    {"title", prompt_mcp_action_add_arguments_title},
    {"description", prompt_mcp_action_add_arguments_description},
    {NULL, NULL}};
static const arg_spec_t a_action_set[] = {
    {"id", prompt_mcp_action_set_arguments_id},
    {"key", prompt_mcp_action_set_arguments_key},
    {"value", prompt_mcp_action_set_arguments_value}, {NULL, NULL}};
static const arg_spec_t a_action_remove[] = {
    {"id", prompt_mcp_action_remove_arguments_id}, {NULL, NULL}};

static const tool_t tools_details[] = {
    {"details_get", prompt_mcp_details_get_description, a_details_get,
     t_details_get},
    {"details_set", prompt_mcp_details_set_description, a_details_set,
     t_details_set},
    {"details_remove", prompt_mcp_details_remove_description,
     a_details_remove, t_details_remove},
    {NULL, NULL, NULL, NULL}};

static const tool_t tools_context[] = {
    {"context_list", prompt_mcp_context_list_description, a_none,
     t_context_list},
    {"context_get", prompt_mcp_context_get_description, a_context_get,
     t_context_get},
    {"context_add", prompt_mcp_context_add_description, a_context_add,
     t_context_add},
    {"context_edit", prompt_mcp_context_edit_description, a_context_edit,
     t_context_edit},
    {"context_remove", prompt_mcp_context_remove_description,
     a_context_remove, t_context_remove},
    {NULL, NULL, NULL, NULL}};

static const tool_t tools_resource[] = {
    {"resource_list", prompt_mcp_resource_list_description, a_none,
     t_resource_list},
    {"resource_get", prompt_mcp_resource_get_description, a_resource_get,
     t_resource_get},
    {"resource_add", prompt_mcp_resource_add_description, a_resource_add,
     t_resource_add},
    {"resource_set", prompt_mcp_resource_set_description, a_resource_set,
     t_resource_set},
    {"resource_remove", prompt_mcp_resource_remove_description,
     a_resource_remove, t_resource_remove},
    {NULL, NULL, NULL, NULL}};

static const tool_t tools_task_config[] = {
    {"task_set", prompt_mcp_task_set_description, a_task_set,
     t_task_set},
    {NULL, NULL, NULL, NULL}};

static const tool_t tools_action[] = {
    {"action_add", prompt_mcp_action_add_description, a_action_add,
     t_action_add},
    {"action_set", prompt_mcp_action_set_description, a_action_set,
     t_action_set},
    {"action_remove", prompt_mcp_action_remove_description,
     a_action_remove, t_action_remove},
    {"action_list", prompt_mcp_action_list_description, a_none,
     t_action_list},
    {NULL, NULL, NULL, NULL}};

/* ---------- json-rpc plumbing ---------- */

/* copy of the request id: string, number or bool, NULL otherwise */
static json_t *id_copy(const json_t *id) {
    if (!id) return NULL;
    if (id->type == JSON_STR) return json_str(id->str);
    if (id->type == JSON_NUM) return json_num(id->num);
    if (id->type == JSON_BOOL) return json_bool((int)id->num);
    return NULL;
}

/* one response line on stdout: {"jsonrpc":"2.0","id":...,"<member>":...}
   takes ownership of id and payload */
static void respond(json_t *id, const char *member, json_t *payload) {
    json_t *resp = json_obj();
    char *out;
    jobj_str(resp, "jsonrpc", "2.0");
    jobj_set(resp, "id", id ? id : json_new(JSON_NULL));
    jobj_set(resp, member, payload);
    out = json_print(resp);
    if (out) {
        fputs(out, stdout);
        fputc('\n', stdout);
        free(out);
    }
    fflush(stdout);
    json_free(resp);
}

static void respond_err(json_t *id, int code, const char *msg) {
    json_t *err = json_obj();
    jobj_set(err, "code", json_num(code));
    jobj_str(err, "message", msg);
    respond(id, "error", err);
}

/* 1 when v is a protocol version this server speaks */
static int proto_supported(const char *v) {
    static const char *const ok[] = {"2024-11-05", "2025-03-26",
                                     "2025-06-18", "2025-11-25", NULL};
    const char *const *p;
    for (p = ok; *p; p++)
        if (!strcmp(*p, v)) return 1;
    return 0;
}

/* initialize result: echo the requested version when supported */
static json_t *init_result(const json_t *params) {
    const char *req = json_str_of(params, "protocolVersion");
    json_t *res = json_obj();
    json_t *caps = json_obj();
    json_t *si = json_obj();
    char *name = xasprintf("flower-%s", g.sname);
    jobj_str(res, "protocolVersion",
             req && proto_supported(req) ? req : "2025-11-25");
    jobj_set(caps, "tools", json_obj());
    jobj_set(res, "capabilities", caps);
    jobj_str(si, "name", name ? name : "flower");
    jobj_str(si, "version", FLOWER_VERSION);
    jobj_set(res, "serverInfo", si);
    free(name);
    return res;
}

/* 1 when an argument is optional (stays out of the required array).
   only action_add's description so far; the rest mirror the cli,
   which demands every argument */
static int arg_optional(const tool_t *tool, const arg_spec_t *arg) {
    return tool->fn == t_action_add && !strcmp(arg->name, "description");
}

/* tools/list result built from a static table */
static json_t *tools_result(const tool_t *tools) {
    json_t *res = json_obj();
    json_t *list = json_arr();
    const tool_t *t;
    for (t = tools; t->name; t++) {
        json_t *tool = json_obj();
        json_t *schema = json_obj();
        json_t *props = json_obj();
        json_t *req = json_arr();
        const arg_spec_t *as;
        jobj_str(tool, "name", t->name);
        jobj_str(tool, "description", t->desc);
        jobj_str(schema, "type", "object");
        for (as = t->args; as->name; as++) {
            json_t *p = json_obj();
            jobj_str(p, "type", "string");
            jobj_str(p, "description", as->desc);
            jobj_set(props, as->name, p);
            if (!arg_optional(t, as)) json_add(req, json_str(as->name));
        }
        jobj_set(schema, "properties", props);
        jobj_set(schema, "required", req);
        jobj_set(tool, "inputSchema", schema);
        json_add(list, tool);
    }
    jobj_set(res, "tools", list);
    return res;
}

/* tools/call: unknown tool is a json-rpc error, bad arguments are an
   isError result so an agent can read the reason and retry */
static void handle_call(const json_t *id, const json_t *params,
                        const tool_t *tools) {
    const char *name = json_str_of(params, "name");
    const json_t *args = json_get(params, "arguments");
    const tool_t *t;
    buf_t text;
    int err = 0;
    if (!name) {
        respond_err(id_copy(id), -32602, "tools/call needs a tool name");
        return;
    }
    for (t = tools; t->name && strcmp(t->name, name); t++);
    if (!t->name) {
        char *msg = xasprintf("unknown tool '%s'", name);
        respond_err(id_copy(id), -32602, msg ? msg : "unknown tool");
        free(msg);
        return;
    }
    if (args && args->type != JSON_OBJ) args = NULL;
    buf_init(&text);
    t->fn(args, &text, &err);
    respond(id_copy(id), "result",
            text_result(text.data ? text.data : "", err));
    buf_free(&text);
}

/* one parsed stdin line: request or notification */
static void serve_line(const char *line, const tool_t *tools) {
    json_t *msg = json_parse(line);
    const char *method;
    json_t *id, *params;
    if (!msg || msg->type != JSON_OBJ) {
        json_free(msg);
        return;
    }
    method = json_str_of(msg, "method");
    if (!method) {
        json_free(msg);
        return;
    }
    id = json_get(msg, "id");
    if (id && id->type == JSON_NULL) id = NULL; /* notification tier */
    params = json_get(msg, "params");

    if (!strcmp(method, "initialize")) {
        if (id) respond(id_copy(id), "result", init_result(params));
    } else if (!strcmp(method, "notifications/initialized")) {
        /* no reply */
    } else if (!strcmp(method, "ping")) {
        if (id) respond(id_copy(id), "result", json_obj());
    } else if (!strcmp(method, "tools/list")) {
        if (id) respond(id_copy(id), "result", tools_result(tools));
    } else if (!strcmp(method, "tools/call")) {
        if (id) handle_call(id, params, tools);
    } else if (id) {
        respond_err(id_copy(id), -32601, "method not found");
    }
    json_free(msg);
}

static void serve(const tool_t *tools) {
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) != -1) {
        str_trim(line);
        if (line[0]) serve_line(line, tools);
    }
    free(line);
}

/* ---------- entry ---------- */

static int mcp_usage(void) {
    fputs("usage: flower mcp [<P>] <area>\n"
          "       flower mcp [<P>] task <T> <area>\n"
          "       area: details, context or resource\n"
          "       task area: config, context or action\n",
          stderr);
    return 1;
}

/* argv: prog "mcp" [<pid>] <area> | prog "mcp" [<pid>] "task" <tid>
   <area>. stdin/stdout carry the json-rpc session, stderr carries
   diagnostics. pid optional: detected from cwd via project Path keys
   when omitted */
int cmd_mcp(int argc, char **argv) {
    char *detected = NULL;
    const char *pid, *area, *tid = NULL;
    const tool_t *tools;
    int area_off = 3;
    if (argc >= 3 && store_valid_id(argv[2], 'P')) {
        pid = argv[2];
    } else {
        detected = store_detect_project();
        if (!detected) {
            if (argc >= 4) {
                /* explicit but malformed id (e.g. p1, X1): keep the
                   old error instead of blaming the directory */
                fprintf(stderr, "flower: invalid project id '%s'\n",
                        argv[2]);
                return 1;
            }
            fprintf(stderr, "flower: no project found for this directory "
                            "(set project Path or pass <P>)\n");
            return 1;
        }
        pid = detected;
        area_off = 2;
    }
    if (argc > area_off && !strcmp(argv[area_off], "task")) {
        /* task-scoped form: the area sits one slot further right */
        if (argc != area_off + 3) {
            free(detected);
            return mcp_usage();
        }
        tid = argv[area_off + 1];
        if (!store_valid_id(tid, 'T')) {
            fprintf(stderr, "flower: invalid task id '%s'\n", tid);
            free(detected);
            return 1;
        }
        area = argv[area_off + 2];
    } else {
        if (argc != area_off + 1) {
            free(detected);
            return mcp_usage();
        }
        area = argv[area_off];
    }
    if (!tid) {
        if (!strcmp(area, "details")) {
            tools = tools_details;
            g.kind = FV_FIELDS;
        } else if (!strcmp(area, "context")) {
            tools = tools_context;
            g.kind = FV_CONTEXT;
        } else if (!strcmp(area, "resource")) {
            tools = tools_resource;
            g.kind = FV_RESOURCE;
        } else {
            free(detected);
            return mcp_usage();
        }
    } else if (!strcmp(area, "config")) {
        tools = tools_task_config;
        g.kind = FV_TASK;
    } else if (!strcmp(area, "context")) {
        tools = tools_context;
        g.kind = FV_CONTEXT;
    } else if (!strcmp(area, "action")) {
        tools = tools_action;
        g.kind = FV_ACTIONS;
    } else {
        free(detected);
        return mcp_usage();
    }
    if (!store_valid_id(pid, 'P')) {
        fprintf(stderr, "flower: invalid project id '%s'\n", pid);
        free(detected);
        return 1;
    }
    if (!tid) {
        if (!require_project(pid)) {
            free(detected);
            return 1;
        }
    } else if (!require_task(pid, tid)) {
        free(detected);
        return 1;
    }
    g.area = area;
    g.sname = tid ? xasprintf("task-%s", area) : area;
    if (!g.sname) {
        free(detected);
        return oom();
    }
    if (tid)
        g.rel = xasprintf("projects/%s/tasks/%s/%s.md", pid, tid,
                          strcmp(area, "action") ? area : "actions");
    else
        g.rel = xasprintf("projects/%s/%s.md", pid,
                          strcmp(area, "resource") ? area : "resources");
    if (!g.rel) {
        if (tid) free((char *)g.sname);
        free(detected);
        return oom();
    }
    serve(tools);
    free(g.rel);
    if (tid) free((char *)g.sname);
    free(detected);
    return 0;
}
