/* agents — llm interaction configuration backend, mirroring the
 * theme.c/projects.c pattern: lenient load, strict save, atomic
 * tmp+rename writes. Two stores:
 *
 *   llms.json        named llm endpoints (connection details, one place)
 *   agents/<slug>.json   one file per agent: llm reference by name,
 *                    agent-specific inference options, system prompt,
 *                    mcp servers
 *
 * The shapes mirror llmkit's records (llm payload, tools list, system
 * text) so an agent + its llm translate mechanically into runner input
 * later. */
#define _POSIX_C_SOURCE 200809L

#include "agents.h"
#include "theme.h" /* theme_dir(): resolved config directory */

#include <cJSON.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h> /* getpid */

static cJSON *llm_to_cjson(const llm_t *l);
static cJSON *agent_to_cjson(const agent_t *a);

/* ---------- enums from llmkit's record contract ---------- */

static const char *const ENDPOINT_PROTOCOLS[] = {
    "openai", "openai_responses", "anthropic",
};
static const char *const MCP_REVISIONS[] = {
    "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28",
};
static const char *const TRANSPORTS[] = { "stdio", "http", "sse" };

#define LIST_COUNT(x) (sizeof(x) / sizeof((x)[0]))

static int in_list(const char *const *list, size_t n, const char *s)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(list[i], s) == 0) return 1;
    return 0;
}

static const cJSON *unknown_key(const cJSON *obj, const char *const *keys,
                                size_t n)
{
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, obj) {
        if (!child->string) continue;
        if (!in_list(keys, n, child->string)) return child;
    }
    return NULL;
}

/* ---------- shared validation ---------- */

/* Valid UTF-8 with no control characters; multiline also allows \n, \t
 * (system prompts, stop sequences, multi-line command lines). */
static int valid_utf8(const char *s, size_t n, int multiline)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp;
        int need;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) {
                if (multiline && (c == '\n' || c == '\t')) { i++; continue; }
                return 0;
            }
            i++;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f; need = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f; need = 2;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07; need = 3;
        } else {
            return 0; /* stray continuation / invalid lead */
        }
        if (i + (size_t)need >= n) return 0; /* truncated sequence */
        for (int k = 1; k <= need; k++) {
            unsigned char cc = (unsigned char)s[i + (size_t)k];
            if ((cc & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (need == 1 && cp < 0x80) return 0;            /* overlong */
        if (need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)))
            return 0;                                     /* overlong/surrogate */
        if (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
        i += (size_t)need + 1;
    }
    return 1;
}

static int str_ok(const char *s, size_t cap, int multiline)
{
    return s[0] && strlen(s) < cap && valid_utf8(s, strlen(s), multiline);
}

/* an http(s) URL: scheme, nothing whitespace-ish or control */
static int valid_url(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= CFG_URL_MAX) return 0;
    if (strncmp(s, "http://", 7) != 0 && strncmp(s, "https://", 8) != 0)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

/* http header name: visible ASCII only */
static int valid_header_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= CFG_HDR_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c >= 0x7f) return 0;
    }
    return 1;
}

/* mcp server / tool name: the prefix of the tool names the model sees */
static int valid_toolname(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= AGENT_TOOL_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* record a strict-mode error (lenient mode just fails silently) */
static int vfail(int strict, char *ef, size_t efn, char *em, size_t emn,
                 const char *field, const char *msg)
{
    if (strict && ef && em) {
        snprintf(ef, efn, "%s", field);
        snprintf(em, emn, "%s", msg);
    }
    return -1;
}

/* ---------- shared parsing (strict on PUT, lenient on load) ----------
 *
 * Strict mode records a precise error (JSON paths like
 * "llms[1].api_base", "agents[0].tools[2].url"); lenient mode repairs
 * with defaults, skips bad entries and only fails when the entry is
 * unusable. `prefix` names the JSON path of the object being parsed. */

static int parse_headers(const cJSON *obj, cfg_kv_t *out, size_t *count,
                         int strict, char *ef, size_t efn,
                         char *em, size_t emn, const char *prefix)
{
    char field[200];
    *count = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, obj) {
        if (!child->string) continue;
        if (*count >= CFG_HDR_MAX) {
            snprintf(field, sizeof field, "%s.headers", prefix);
            return vfail(strict, ef, efn, em, emn, field,
                         "too many headers (max 16)");
        }
        if (!valid_header_name(child->string) || !cJSON_IsString(child) ||
            !child->valuestring ||
            !str_ok(child->valuestring, CFG_SECRET_MAX, 0)) {
            if (strict) {
                snprintf(field, sizeof field, "%s.headers.%s", prefix,
                         child->string);
                return vfail(strict, ef, efn, em, emn, field,
                             "header names are visible ASCII, values are "
                             "single-line text (max 255 bytes)");
            }
            continue; /* lenient: skip the bad header */
        }
        snprintf(out[*count].name, sizeof out[*count].name, "%s",
                 child->string);
        snprintf(out[*count].value, sizeof out[*count].value, "%s",
                 child->valuestring);
        (*count)++;
    }
    return 0;
}

static cJSON *headers_to_cjson(const cfg_kv_t *kv, size_t count)
{
    if (!count) return NULL;
    cJSON *h = cJSON_CreateObject();
    if (!h) return NULL;
    for (size_t i = 0; i < count; i++)
        if (!cJSON_AddStringToObject(h, kv[i].name, kv[i].value)) {
            cJSON_Delete(h);
            return NULL;
        }
    return h;
}

/* ============================================================
 * llm endpoints — llms.json
 * ============================================================ */

static int parse_llm_entry(const cJSON *obj, llm_t *out, size_t index,
                           int strict, char *ef, size_t efn,
                           char *em, size_t emn)
{
    static const char *const keys[] = {
        "name", "endpoint_protocol", "api_base", "model", "api_key",
        "headers",
    };
    const size_t nkeys = sizeof keys / sizeof keys[0];
    memset(out, 0, sizeof *out);
    char prefix[48], field[176];
    const cJSON *j;

    snprintf(prefix, sizeof prefix, "llms[%zu]", index);

    j = cJSON_GetObjectItemCaseSensitive(obj, "name");
    if (!cJSON_IsString(j) || !str_ok(cJSON_IsString(j) ? j->valuestring : "",
                                      CFG_NAME_MAX, 0)) {
        snprintf(field, sizeof field, "%s.name", prefix);
        vfail(strict, ef, efn, em, emn, field, "required, 1-63 bytes of text");
        return -1; /* lenient: an entry without a name is dropped */
    }
    snprintf(out->name, sizeof out->name, "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(obj, "endpoint_protocol");
    if (!cJSON_IsString(j) || !j->valuestring ||
        !in_list(ENDPOINT_PROTOCOLS, LIST_COUNT(ENDPOINT_PROTOCOLS),
                 j->valuestring)) {
        snprintf(field, sizeof field, "%s.endpoint_protocol", prefix);
        vfail(strict, ef, efn, em, emn, field,
              "openai, openai_responses or anthropic");
        return -1; /* lenient: dropped */
    }
    snprintf(out->endpoint_protocol, sizeof out->endpoint_protocol, "%s",
             j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(obj, "api_base");
    if (!cJSON_IsString(j) || !valid_url(cJSON_IsString(j) ? j->valuestring : "")) {
        snprintf(field, sizeof field, "%s.api_base", prefix);
        vfail(strict, ef, efn, em, emn, field,
              "required, an http(s) URL like http://localhost:11434/v1");
        return -1; /* lenient: dropped */
    }
    snprintf(out->api_base, sizeof out->api_base, "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(obj, "model");
    if (j && !cJSON_IsString(j)) {
        snprintf(field, sizeof field, "%s.model", prefix);
        if (strict) return vfail(strict, ef, efn, em, emn, field, "must be a string");
    } else if (j && j->valuestring[0] &&
               !str_ok(j->valuestring, CFG_TEXT_MAX, 0)) {
        snprintf(field, sizeof field, "%s.model", prefix);
        if (strict)
            return vfail(strict, ef, efn, em, emn, field,
                         "text, max 127 bytes, single line");
        /* lenient: keep the empty default */
    } else if (j && j->valuestring) {
        snprintf(out->model, sizeof out->model, "%s", j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "api_key");
    if (j && !cJSON_IsString(j)) {
        snprintf(field, sizeof field, "%s.api_key", prefix);
        if (strict) return vfail(strict, ef, efn, em, emn, field, "must be a string");
    } else if (j && j->valuestring[0] &&
               !str_ok(j->valuestring, CFG_SECRET_MAX, 0)) {
        snprintf(field, sizeof field, "%s.api_key", prefix);
        if (strict)
            return vfail(strict, ef, efn, em, emn, field,
                         "text, max 255 bytes, must not contain line breaks");
    } else if (j && j->valuestring) {
        snprintf(out->api_key, sizeof out->api_key, "%s", j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "headers");
    if (j) {
        if (!cJSON_IsObject(j)) {
            snprintf(field, sizeof field, "%s.headers", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "must be an object of name/value pairs");
        } else if (parse_headers(j, out->headers, &out->header_count, strict,
                                 ef, efn, em, emn, prefix) != 0 && strict) {
            return -1;
        }
    }

    j = unknown_key(obj, keys, nkeys);
    if (j && strict) {
        snprintf(field, sizeof field, "%s.%s", prefix, j->string);
        return vfail(strict, ef, efn, em, emn, field, "unknown setting");
    }
    return 0;
}

static int llm_name_cmp(const void *pa, const void *pb)
{
    const llm_t *a = *(llm_t *const *)pa;
    const llm_t *b = *(llm_t *const *)pb;
    int r = strcasecmp(a->name, b->name);
    return r ? r : strcmp(a->name, b->name);
}

void llms_free(llms_t *l)
{
    for (size_t i = 0; i < l->count; i++) free(l->items[i]);
    l->count = 0;
}

static char *read_file(const char *path, long cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0 && sz < cap) {
            rewind(f);
            buf = malloc((size_t)sz + 1);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
                buf[sz] = '\0';
            else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}

/* parse a whole llm array; strict = PUT (errors), lenient = load */
static llms_parse_result_t llms_parse(const char *buf, size_t len,
                                      int strict, llms_t *out,
                                      char *ef, size_t efn,
                                      char *em, size_t emn)
{
    memset(out, 0, sizeof *out);
    char *copy = malloc(len + 1);
    if (!copy) {
        if (strict) snprintf(em, emn, "out of memory");
        return LLMS_E_JSON;
    }
    memcpy(copy, buf, len);
    copy[len] = '\0';
    cJSON *j = cJSON_Parse(copy);
    free(copy);
    if (!j || !cJSON_IsArray(j)) {
        cJSON_Delete(j);
        if (strict) snprintf(em, emn, "expected a JSON array of llms");
        return LLMS_E_JSON;
    }
    if (cJSON_GetArraySize(j) > LLMS_MAX) {
        cJSON_Delete(j);
        if (strict)
            snprintf(em, emn, "too many llms (max %d)", LLMS_MAX);
        return LLMS_E_FIELD;
    }

    size_t i = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, j) {
        if (strict && !cJSON_IsObject(child)) {
            snprintf(ef, efn, "llms[%zu]", i);
            snprintf(em, emn, "must be an object");
            llms_free(out);
            cJSON_Delete(j);
            return LLMS_E_FIELD;
        }
        if (!cJSON_IsObject(child)) { i++; continue; } /* lenient: skip */

        llm_t *one = calloc(1, sizeof *one);
        if (!one) {
            if (strict) snprintf(em, emn, "out of memory");
            llms_free(out);
            cJSON_Delete(j);
            return LLMS_E_FIELD;
        }
        if (parse_llm_entry(child, one, i, strict, ef, efn, em, emn) != 0) {
            free(one);
            if (strict) { llms_free(out); cJSON_Delete(j); return LLMS_E_FIELD; }
            i++;
            continue; /* lenient: skip the broken entry */
        }
        for (size_t k = 0; k < out->count; k++)
            if (strcasecmp(out->items[k]->name, one->name) == 0) {
                if (strict) {
                    snprintf(ef, efn, "llms[%zu].name", i);
                    snprintf(em, emn,
                             "duplicate name — llm names are unique");
                }
                free(one);
                if (strict) { llms_free(out); cJSON_Delete(j); return LLMS_E_FIELD; }
                goto next;
            }
        out->items[out->count++] = one;
    next:
        i++;
    }
    cJSON_Delete(j);

    /* sorted by name for a stable order */
    qsort(out->items, out->count, sizeof out->items[0], llm_name_cmp);
    return LLMS_OK;
}

int llms_load(llms_t *l)
{
    memset(l, 0, sizeof *l);
    char path[4352];
    snprintf(path, sizeof path, "%s/llms.json", theme_dir());
    char *buf = read_file(path, 1024 * 1024);
    if (!buf) return 1;
    llms_parse(buf, strlen(buf), 0, l, NULL, 0, NULL, 0); /* lenient */
    free(buf);
    return 0;
}

int llms_save(const llms_t *l)
{
    char path[4352], tmp[4400];
    snprintf(path, sizeof path, "%s/llms.json", theme_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    cJSON *j = cJSON_CreateArray();
    if (j)
        for (size_t i = 0; i < l->count; i++) {
            cJSON *o = llm_to_cjson(l->items[i]);
            if (!o) { cJSON_Delete(j); j = NULL; break; }
            cJSON_AddItemToArray(j, o);
        }
    char *out = j ? cJSON_Print(j) : NULL; /* pretty-printed */
    cJSON_Delete(j);
    int ok = out && fputs(out, f) != EOF && fputc('\n', f) != EOF &&
             fclose(f) == 0;
    free(out);
    if (!ok) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

llms_parse_result_t llms_from_json(const char *buf, size_t len, llms_t *out,
                                   char *err_field, size_t err_field_n,
                                   char *err_msg, size_t err_msg_n)
{
    err_field[0] = '\0';
    err_msg[0] = '\0';
    return llms_parse(buf, len, 1, out, err_field, err_field_n,
                      err_msg, err_msg_n);
}

int llms_find(const llms_t *l, const char *name)
{
    if (!name || !name[0]) return -1;
    for (size_t i = 0; i < l->count; i++)
        if (strcasecmp(l->items[i]->name, name) == 0) return (int)i;
    return -1;
}

const llm_t *llms_get(const llms_t *l, const char *name)
{
    int i = llms_find(l, name);
    return i < 0 ? NULL : l->items[i];
}

/* ============================================================
 * agents — agents/<slug>.json
 * ============================================================ */

static int parse_inference(const cJSON *obj, agent_inference_t *out,
                           int strict, char *ef, size_t efn,
                           char *em, size_t emn, const char *prefix)
{
    static const char *const keys[] = {
        "temperature", "top_p", "max_tokens", "stop", "top_k",
        "thinking_budget", "reasoning_effort", "presence_penalty",
        "frequency_penalty", "seed", "stream",
    };
    const size_t nkeys = sizeof keys / sizeof keys[0];
    memset(out, 0, sizeof *out);
    char field[232];
    const cJSON *j;

    j = unknown_key(obj, keys, nkeys);
    if (j && strict) {
        snprintf(field, sizeof field, "%s.%s", prefix, j->string);
        return vfail(strict, ef, efn, em, emn, field, "unknown setting");
    }

    struct { const char *key; agent_num_t *dst; } nums[] = {
        { "temperature", &out->temperature },
        { "top_p", &out->top_p },
        { "top_k", &out->top_k },
        { "max_tokens", &out->max_tokens },
        { "thinking_budget", &out->thinking_budget },
        { "presence_penalty", &out->presence_penalty },
        { "frequency_penalty", &out->frequency_penalty },
        { "seed", &out->seed },
    };
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        j = cJSON_GetObjectItemCaseSensitive(obj, nums[i].key);
        if (!j) continue;
        if (!cJSON_IsNumber(j) || !isfinite(j->valuedouble)) {
            snprintf(field, sizeof field, "%s.%s", prefix, nums[i].key);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "must be a number");
            continue;
        }
        nums[i].dst->set = 1;
        nums[i].dst->v = j->valuedouble;
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "reasoning_effort");
    if (j && (!cJSON_IsString(j) ||
              !str_ok(cJSON_IsString(j) ? j->valuestring : "",
                      AGENT_EFFORT_MAX, 0))) {
        snprintf(field, sizeof field, "%s.reasoning_effort", prefix);
        if (strict)
            return vfail(strict, ef, efn, em, emn, field,
                         "text, max 23 bytes (e.g. low, medium, high)");
    } else if (j && cJSON_IsString(j)) {
        snprintf(out->reasoning_effort, sizeof out->reasoning_effort, "%s",
                 j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "stop");
    if (j) {
        const cJSON *one = NULL;
        size_t n = 0;
        if (cJSON_IsString(j)) {
            one = j; /* single-string form: one iteration below */
        } else if (cJSON_IsArray(j)) {
            one = j->child;
        } else {
            snprintf(field, sizeof field, "%s.stop", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "must be a string or an array of strings");
        }
        for (; one; one = (one == j) ? NULL : one->next) {
            if (n >= AGENT_STOP_MAX) {
                snprintf(field, sizeof field, "%s.stop", prefix);
                if (strict)
                    return vfail(strict, ef, efn, em, emn, field,
                                 "too many stop sequences (max 8)");
                break;
            }
            if (!cJSON_IsString(one) || !one->valuestring ||
                !str_ok(one->valuestring, AGENT_STOP_LEN_MAX, 1)) {
                snprintf(field, sizeof field, "%s.stop", prefix);
                if (strict)
                    return vfail(strict, ef, efn, em, emn, field,
                                 "stop entries are text, max 127 bytes");
                continue;
            }
            snprintf(out->stop[n], sizeof out->stop[n], "%s", one->valuestring);
            n++;
        }
        out->stop_count = n;
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "stream");
    if (j) {
        if (!cJSON_IsBool(j)) {
            snprintf(field, sizeof field, "%s.stream", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "must be true or false");
        } else {
            out->stream.set = 1;
            out->stream.v = cJSON_IsTrue(j);
        }
    }
    return 0;
}

static int parse_tool(const cJSON *obj, agent_tool_t *out, int strict,
                      char *ef, size_t efn, char *em, size_t emn,
                      const char *prefix)
{
    static const char *const keys[] = {
        "type", "name", "command_line", "url", "headers", "protocol",
        "required", "terminal_tools",
    };
    const size_t nkeys = sizeof keys / sizeof keys[0];
    memset(out, 0, sizeof *out);
    char field[216];
    const cJSON *j;

    j = cJSON_GetObjectItemCaseSensitive(obj, "type");
    if (!cJSON_IsString(j) || !j->valuestring ||
        !in_list(TRANSPORTS, LIST_COUNT(TRANSPORTS), j->valuestring)) {
        snprintf(field, sizeof field, "%s.type", prefix);
        vfail(strict, ef, efn, em, emn, field, "stdio, http or sse");
        return -1; /* lenient: unusable server entry is skipped */
    }
    snprintf(out->type, sizeof out->type, "%s", j->valuestring);
    int is_stdio = strcmp(out->type, "stdio") == 0;

    j = cJSON_GetObjectItemCaseSensitive(obj, "name");
    if (!cJSON_IsString(j) || !j->valuestring ||
        !valid_toolname(j->valuestring)) {
        snprintf(field, sizeof field, "%s.name", prefix);
        vfail(strict, ef, efn, em, emn, field,
              "required, letters/digits/dot/underscore/hyphen (e.g. fs)");
        return -1; /* lenient: skipped */
    }
    snprintf(out->name, sizeof out->name, "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(obj, "command_line");
    if (j && !cJSON_IsString(j)) {
        snprintf(field, sizeof field, "%s.command_line", prefix);
        if (strict) return vfail(strict, ef, efn, em, emn, field, "must be a string");
        j = NULL;
    } else if (j && is_stdio &&
               !str_ok(cJSON_IsString(j) ? j->valuestring : "",
                       AGENT_CMD_MAX, 1)) {
        snprintf(field, sizeof field, "%s.command_line", prefix);
        if (strict)
            return vfail(strict, ef, efn, em, emn, field,
                         "the full shell command, max 1023 bytes");
        j = NULL;
    }
    if (j && j->valuestring && j->valuestring[0]) {
        if (!is_stdio) {
            snprintf(field, sizeof field, "%s.command_line", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "only stdio servers have a command_line");
        } else {
            snprintf(out->command_line, sizeof out->command_line, "%s",
                     j->valuestring);
        }
    } else if (is_stdio && strict) {
        snprintf(field, sizeof field, "%s.command_line", prefix);
        return vfail(strict, ef, efn, em, emn, field,
                     "required for stdio servers");
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "url");
    if (j && !cJSON_IsString(j)) {
        snprintf(field, sizeof field, "%s.url", prefix);
        if (strict) return vfail(strict, ef, efn, em, emn, field, "must be a string");
        j = NULL;
    } else if (j && !is_stdio &&
               !valid_url(cJSON_IsString(j) ? j->valuestring : "")) {
        snprintf(field, sizeof field, "%s.url", prefix);
        if (strict)
            return vfail(strict, ef, efn, em, emn, field,
                         "required, an http(s) URL");
        j = NULL;
    }
    if (j && j->valuestring && j->valuestring[0]) {
        if (is_stdio) {
            snprintf(field, sizeof field, "%s.url", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "only http/sse servers have a url");
        } else {
            snprintf(out->url, sizeof out->url, "%s", j->valuestring);
        }
    } else if (!is_stdio && strict) {
        snprintf(field, sizeof field, "%s.url", prefix);
        return vfail(strict, ef, efn, em, emn, field,
                     "required for http/sse servers");
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "headers");
    if (j) {
        if (is_stdio) {
            snprintf(field, sizeof field, "%s.headers", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "only http/sse servers have headers");
        } else if (!cJSON_IsObject(j)) {
            snprintf(field, sizeof field, "%s.headers", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "must be an object of name/value pairs");
        } else if (parse_headers(j, out->headers, &out->header_count, strict,
                                 ef, efn, em, emn, prefix) != 0 && strict) {
            return -1;
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "protocol");
    if (j && (!cJSON_IsString(j) || !j->valuestring ||
              !in_list(MCP_REVISIONS, LIST_COUNT(MCP_REVISIONS),
                       j->valuestring))) {
        snprintf(field, sizeof field, "%s.protocol", prefix);
        if (strict)
            return vfail(strict, ef, efn, em, emn, field,
                         "an mcp revision, e.g. 2025-11-25");
    } else if (j && j->valuestring) {
        snprintf(out->protocol, sizeof out->protocol, "%s", j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "required");
    if (j) {
        if (!cJSON_IsBool(j)) {
            snprintf(field, sizeof field, "%s.required", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "must be true or false");
        } else {
            out->required = cJSON_IsTrue(j);
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "terminal_tools");
    if (j) {
        if (!cJSON_IsArray(j)) {
            snprintf(field, sizeof field, "%s.terminal_tools", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "must be an array of tool names");
        } else {
            const cJSON *one = NULL;
            cJSON_ArrayForEach(one, j) {
                if (out->term_count >= AGENT_TERM_MAX) {
                    snprintf(field, sizeof field, "%s.terminal_tools", prefix);
                    if (strict)
                        return vfail(strict, ef, efn, em, emn, field,
                                     "too many (max 16)");
                    break;
                }
                if (!cJSON_IsString(one) || !one->valuestring ||
                    !valid_toolname(one->valuestring)) {
                    snprintf(field, sizeof field, "%s.terminal_tools", prefix);
                    if (strict)
                        return vfail(strict, ef, efn, em, emn, field,
                                     "tool names as the server lists them");
                    continue;
                }
                snprintf(out->terminal_tools[out->term_count],
                         sizeof out->terminal_tools[out->term_count], "%s",
                         one->valuestring);
                out->term_count++;
            }
        }
    }

    j = unknown_key(obj, keys, nkeys);
    if (j && strict) {
        snprintf(field, sizeof field, "%s.%s", prefix, j->string);
        return vfail(strict, ef, efn, em, emn, field, "unknown setting");
    }
    return 0;
}

static int parse_agent(const cJSON *obj, agent_t *out, size_t index,
                       const llms_t *llms, int strict,
                       char *ef, size_t efn, char *em, size_t emn)
{
    static const char *const keys[] = {
        "name", "llm", "inference_options", "system_prompt", "tools",
    };
    const size_t nkeys = sizeof keys / sizeof keys[0];
    memset(out, 0, sizeof *out);
    char prefix[48], field[232];
    const cJSON *j;

    snprintf(prefix, sizeof prefix, "agents[%zu]", index);

    j = cJSON_GetObjectItemCaseSensitive(obj, "name");
    if (!cJSON_IsString(j) || !str_ok(cJSON_IsString(j) ? j->valuestring : "",
                                      CFG_NAME_MAX, 0)) {
        snprintf(field, sizeof field, "%s.name", prefix);
        vfail(strict, ef, efn, em, emn, field, "required, 1-63 bytes of text");
        return -1; /* lenient: an agent without a name is dropped */
    }
    snprintf(out->name, sizeof out->name, "%s", j->valuestring);

    /* the llm reference: checked against the endpoint list when known */
    j = cJSON_GetObjectItemCaseSensitive(obj, "llm");
    if (!cJSON_IsString(j) || !str_ok(cJSON_IsString(j) ? j->valuestring : "",
                                      CFG_NAME_MAX, 0)) {
        snprintf(field, sizeof field, "%s.llm", prefix);
        vfail(strict, ef, efn, em, emn, field,
              "required, the name of an llm from the llm section");
        return -1; /* lenient: dropped */
    }
    snprintf(out->llm, sizeof out->llm, "%s", j->valuestring);
    if (strict && llms && llms_find(llms, out->llm) < 0) {
        snprintf(field, sizeof field, "%s.llm", prefix);
        char msg[160];
        snprintf(msg, sizeof msg,
                 "unknown llm '%s' — add it to the llm section first",
                 out->llm);
        return vfail(strict, ef, efn, em, emn, field, msg);
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "inference_options");
    if (j) {
        char ipref[88];
        snprintf(ipref, sizeof ipref, "%s.inference_options", prefix);
        if (!cJSON_IsObject(j)) {
            snprintf(field, sizeof field, "%s", ipref);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "must be an object");
        } else if (parse_inference(j, &out->infer, strict, ef, efn, em, emn,
                                   ipref) != 0 && strict) {
            return -1;
        }
    }

    /* anthropic requires max_tokens, and thinking must fit inside it —
     * llmkit rejects the record at run time; catch it at save time */
    if (strict && llms) {
        const llm_t *ref = llms_get(llms, out->llm);
        if (ref && strcmp(ref->endpoint_protocol, "anthropic") == 0) {
            if (!out->infer.max_tokens.set) {
                snprintf(field, sizeof field,
                         "%s.inference_options.max_tokens", prefix);
                return vfail(strict, ef, efn, em, emn, field,
                             "anthropic endpoints require max_tokens");
            }
            if (out->infer.thinking_budget.set &&
                out->infer.thinking_budget.v >= out->infer.max_tokens.v) {
                snprintf(field, sizeof field,
                         "%s.inference_options.thinking_budget", prefix);
                return vfail(strict, ef, efn, em, emn, field,
                             "must be lower than max_tokens");
            }
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "system_prompt");
    if (j && (!cJSON_IsString(j) ||
              strlen(cJSON_IsString(j) ? j->valuestring : "") >=
                  AGENT_PROMPT_MAX ||
              !valid_utf8(j->valuestring, strlen(j->valuestring), 1))) {
        snprintf(field, sizeof field, "%s.system_prompt", prefix);
        if (strict)
            return vfail(strict, ef, efn, em, emn, field,
                         "text, max 16383 bytes");
    } else if (j) {
        snprintf(out->system_prompt, sizeof out->system_prompt, "%s",
                 j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "tools");
    if (j) {
        if (!cJSON_IsArray(j)) {
            snprintf(field, sizeof field, "%s.tools", prefix);
            if (strict)
                return vfail(strict, ef, efn, em, emn, field,
                             "must be an array of mcp servers");
        } else {
            size_t i = 0;
            const cJSON *child = NULL;
            cJSON_ArrayForEach(child, j) {
                if (out->tool_count >= AGENT_TOOLS_MAX) {
                    snprintf(field, sizeof field, "%s.tools", prefix);
                    if (strict)
                        return vfail(strict, ef, efn, em, emn, field,
                                     "too many mcp servers (max 16)");
                    break;
                }
                if (!cJSON_IsObject(child)) {
                    snprintf(field, sizeof field, "%s.tools[%zu]", prefix, i);
                    if (strict)
                        return vfail(strict, ef, efn, em, emn, field,
                                     "must be an object");
                    i++;
                    continue;
                }
                char tpref[88];
                snprintf(tpref, sizeof tpref, "%s.tools[%zu]", prefix, i);
                agent_tool_t one;
                if (parse_tool(child, &one, strict, ef, efn, em, emn,
                               tpref) != 0) {
                    if (strict) return -1;
                    i++;
                    continue; /* lenient: skip the broken server */
                }
                int dup = 0;
                for (size_t k = 0; k < out->tool_count; k++)
                    if (strcmp(out->tools[k].name, one.name) == 0) {
                        dup = 1;
                        if (strict) {
                            snprintf(field, sizeof field, "%s.name", tpref);
                            return vfail(strict, ef, efn, em, emn, field,
                                         "duplicate server name in this agent");
                        }
                        break;
                    }
                if (!dup) out->tools[out->tool_count++] = one;
                i++;
            }
        }
    }

    j = unknown_key(obj, keys, nkeys);
    if (j && strict) {
        snprintf(field, sizeof field, "%s.%s", prefix, j->string);
        return vfail(strict, ef, efn, em, emn, field, "unknown setting");
    }
    return 0;
}

static int agent_name_cmp(const void *pa, const void *pb)
{
    const agent_t *a = *(agent_t *const *)pa;
    const agent_t *b = *(agent_t *const *)pb;
    int r = strcasecmp(a->name, b->name);
    return r ? r : strcmp(a->name, b->name);
}

void agents_free(agents_t *a)
{
    for (size_t i = 0; i < a->count; i++) free(a->items[i]);
    a->count = 0;
}

/* ---------- builtin agents ---------- */

/* Compiled in, shipped with flower, never written to agents/. The llm
 * reference is "" by design: tasks bound to a builtin select
 * their llm themselves (the task's llm field). */
static const agent_t BUILTIN_AGENTS[] = {
    {
        .name = "assistant",
        .llm = "",
        .system_prompt =
            "You are a helpful assistant. Answer clearly and concisely.",
    },
};
#define BUILTIN_N (sizeof BUILTIN_AGENTS / sizeof BUILTIN_AGENTS[0])

const agent_t *agents_builtin(size_t i)
{
    return i < BUILTIN_N ? &BUILTIN_AGENTS[i] : NULL;
}

size_t agents_builtin_count(void)
{
    return BUILTIN_N;
}

int agents_builtin_find(const char *name)
{
    if (!name) return -1;
    for (size_t i = 0; i < BUILTIN_N; i++)
        if (strcasecmp(BUILTIN_AGENTS[i].name, name) == 0) return (int)i;
    return -1;
}

static int is_builtin(const agent_t *a)
{
    return a >= BUILTIN_AGENTS && a < BUILTIN_AGENTS + BUILTIN_N;
}

int agents_find(const agents_t *a, const char *name)
{
    if (!name) return -1;
    for (size_t i = 0; i < a->count; i++)
        if (strcasecmp(a->items[i]->name, name) == 0) return (int)i;
    return -1;
}

static const char *agents_dir(char *buf, size_t n)
{
    snprintf(buf, n, "%s/agents", theme_dir());
    return buf;
}

static int strptr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int agents_load(agents_t *a)
{
    memset(a, 0, sizeof *a);
    char dir[4352];
    agents_dir(dir, sizeof dir);

    DIR *d = opendir(dir);
    if (!d) return 1; /* no agents yet */

    /* collect agent file names, sorted for a stable order */
    enum { MAX_FILES = 256 };
    char *names[MAX_FILES];
    size_t nn = 0;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 6 || n > 128) continue;        /* shortest name: "a.json" */
        if (e->d_name[0] == '.') continue;     /* dotfiles, leftover tmps */
        if (strcmp(e->d_name + n - 5, ".json") != 0) continue;
        if (nn >= MAX_FILES) break;
        names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof names[0], strptr_cmp);

    for (size_t i = 0; i < nn && a->count < AGENTS_MAX; i++) {
        char path[4352 + 160];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        free(names[i]);
        char *buf = read_file(path, 1024 * 1024);
        if (!buf) continue;
        cJSON *j = cJSON_Parse(buf);
        free(buf);
        if (!j || !cJSON_IsObject(j)) { cJSON_Delete(j); continue; }
        agent_t *one = calloc(1, sizeof *one);
        if (!one) { cJSON_Delete(j); continue; }
        /* lenient: no llms list here — dangling references are kept and
         * flagged by GET (llm_ok), like a vanished project directory */
        if (parse_agent(j, one, a->count, NULL, 0, NULL, 0, NULL, 0) != 0) {
            free(one);
            cJSON_Delete(j);
            continue; /* broken file stays on disk, just not listed */
        }
        cJSON_Delete(j);
        a->items[a->count++] = one;
    }
    qsort(a->items, a->count, sizeof a->items[0], agent_name_cmp);
    return 0;
}

/* [a-z0-9-] derived from the agent name; runs of anything else collapse
 * into one '-', bounded to 64 chars. Never empty. */
static void slug_of(const char *name, char *out, size_t n)
{
    char base[CFG_NAME_MAX * 2];
    size_t o = 0;
    for (size_t i = 0; name[i] && o < 64; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            base[o++] = (char)c;
        else if (c >= 'A' && c <= 'Z')
            base[o++] = (char)(c - 'A' + 'a');
        else if (o > 0 && base[o - 1] != '-')
            base[o++] = '-';
    }
    while (o > 0 && base[o - 1] == '-') o--;
    if (o == 0) { base[o++] = 'a'; base[o++] = 'g'; base[o++] = 'e'; base[o++] = 'n'; base[o++] = 't'; }
    base[o] = '\0';
    snprintf(out, n, "%s", base);
}

/* the agent's file name: "<slug>.json", unique across the list — each
 * earlier agent's already-assigned name is compared in full, so suffixed
 * slugs cannot collide either */
static void agent_file(const agents_t *a, size_t idx,
                       const char files[][96], char *out, size_t n)
{
    char base[80];
    slug_of(a->items[idx]->name, base, sizeof base);
    for (int k = 0;; k++) {
        if (k == 0) snprintf(out, n, "%s.json", base);
        else snprintf(out, n, "%s-%d.json", base, k + 1);
        int clash = 0;
        for (size_t j = 0; j < idx; j++)
            if (strcmp(files[j], out) == 0) { clash = 1; break; }
        if (!clash || k > AGENTS_MAX + 1) break;
    }
}

int agents_save(const agents_t *a)
{
    char dir[4352];
    agents_dir(dir, sizeof dir);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;

    char files[AGENTS_MAX][96];
    for (size_t i = 0; i < a->count; i++)
        agent_file(a, i, (const char (*)[96])files, files[i], sizeof files[i]);

    for (size_t i = 0; i < a->count; i++) {
        char path[8704], tmp[8704 + 32];
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());

        FILE *f = fopen(tmp, "w");
        if (!f) return -1;
        cJSON *j = agent_to_cjson(a->items[i]);
        char *out = j ? cJSON_Print(j) : NULL; /* pretty-printed */
        cJSON_Delete(j);
        int ok = out && fputs(out, f) != EOF && fputc('\n', f) != EOF &&
                 fclose(f) == 0;
        free(out);
        if (!ok) { remove(tmp); return -1; }
        if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    }

    /* delete agent files that are no longer in the list */
    DIR *d = opendir(dir);
    if (!d) return 0; /* nothing saved, nothing to clean */
    char gone[8704];
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 6 || n > 128) continue;
        if (e->d_name[0] == '.') continue;
        if (strcmp(e->d_name + n - 5, ".json") != 0) continue;
        int keep = 0;
        for (size_t i = 0; i < a->count && !keep; i++)
            if (strcmp(e->d_name, files[i]) == 0) keep = 1;
        if (keep) continue;
        snprintf(gone, sizeof gone, "%s/%s", dir, e->d_name);
        remove(gone);
    }
    closedir(d);
    return 0;
}

agents_parse_result_t agents_from_json(const char *buf, size_t len,
                                       const llms_t *llms, agents_t *out,
                                       char *err_field, size_t err_field_n,
                                       char *err_msg, size_t err_msg_n)
{
    err_field[0] = '\0';
    err_msg[0] = '\0';
    memset(out, 0, sizeof *out);

    char *copy = malloc(len + 1);
    if (!copy) {
        snprintf(err_msg, err_msg_n, "out of memory");
        return AGENTS_E_JSON;
    }
    memcpy(copy, buf, len);
    copy[len] = '\0';

    cJSON *j = cJSON_Parse(copy);
    free(copy);
    if (!j || !cJSON_IsArray(j)) {
        cJSON_Delete(j);
        snprintf(err_msg, err_msg_n, "expected a JSON array of agents");
        return AGENTS_E_JSON;
    }

    int n = cJSON_GetArraySize(j);
    if (n > AGENTS_MAX) {
        cJSON_Delete(j);
        snprintf(err_msg, err_msg_n, "too many agents (max %d)", AGENTS_MAX);
        return AGENTS_E_FIELD;
    }

    size_t i = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, j) {
        if (!cJSON_IsObject(child)) {
            snprintf(err_field, err_field_n, "agents[%zu]", i);
            snprintf(err_msg, err_msg_n, "must be an object");
            agents_free(out);
            cJSON_Delete(j);
            return AGENTS_E_FIELD;
        }
        agent_t *one = calloc(1, sizeof *one);
        if (!one) {
            snprintf(err_msg, err_msg_n, "out of memory");
            agents_free(out);
            cJSON_Delete(j);
            return AGENTS_E_FIELD;
        }
        if (parse_agent(child, one, i, llms, 1,
                        err_field, err_field_n, err_msg, err_msg_n) != 0) {
            free(one);
            agents_free(out);
            cJSON_Delete(j);
            return AGENTS_E_FIELD;
        }
        for (size_t k = 0; k < out->count; k++)
            if (strcasecmp(out->items[k]->name, one->name) == 0) {
                snprintf(err_field, err_field_n, "agents[%zu].name", i);
                snprintf(err_msg, err_msg_n,
                         "duplicate name — agent names are unique");
                free(one);
                agents_free(out);
                cJSON_Delete(j);
                return AGENTS_E_FIELD;
            }
        if (agents_builtin_find(one->name) >= 0) {
            snprintf(err_field, err_field_n, "agents[%zu].name", i);
            snprintf(err_msg, err_msg_n,
                     "a builtin agent already uses this name");
            free(one);
            agents_free(out);
            cJSON_Delete(j);
            return AGENTS_E_FIELD;
        }
        out->items[out->count++] = one;
        i++;
    }
    cJSON_Delete(j);
    qsort(out->items, out->count, sizeof out->items[0], agent_name_cmp);
    return AGENTS_OK;
}

/* ---------- serialization ---------- */

/* numbers as raw items: cJSON's double printer would spell 0.7 as
 * 0.69999999999999996; %g keeps the files human-readable */
static void add_num(cJSON *obj, const char *key, double v)
{
    char buf[40];
    if (v >= -9.0e15 && v <= 9.0e15 && v == (double)(long long)v)
        snprintf(buf, sizeof buf, "%lld", (long long)v);
    else
        snprintf(buf, sizeof buf, "%.*g", 10, v);
    cJSON_AddItemToObject(obj, key, cJSON_CreateRaw(buf));
}

static cJSON *llm_to_cjson(const llm_t *l)
{
    cJSON *o = cJSON_CreateObject();
    if (!o ||
        !cJSON_AddStringToObject(o, "name", l->name) ||
        !cJSON_AddStringToObject(o, "endpoint_protocol", l->endpoint_protocol) ||
        !cJSON_AddStringToObject(o, "api_base", l->api_base))
        goto fail;
    if (l->model[0] && !cJSON_AddStringToObject(o, "model", l->model))
        goto fail;
    if (l->api_key[0] && !cJSON_AddStringToObject(o, "api_key", l->api_key))
        goto fail;
    {
        cJSON *h = headers_to_cjson(l->headers, l->header_count);
        if (h) cJSON_AddItemToObject(o, "headers", h);
    }
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

char *llms_to_json(const llms_t *l)
{
    cJSON *j = cJSON_CreateArray();
    if (!j) return NULL;
    for (size_t i = 0; i < l->count; i++) {
        cJSON *o = llm_to_cjson(l->items[i]);
        if (!o) { cJSON_Delete(j); return NULL; }
        cJSON_AddItemToArray(j, o);
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}

static cJSON *inference_to_cjson(const agent_inference_t *io)
{
    const struct { const char *key; const agent_num_t *num; } nums[] = {
        { "temperature", &io->temperature },
        { "top_p", &io->top_p },
        { "top_k", &io->top_k },
        { "thinking_budget", &io->thinking_budget },
        { "max_tokens", &io->max_tokens },
        { "presence_penalty", &io->presence_penalty },
        { "frequency_penalty", &io->frequency_penalty },
        { "seed", &io->seed },
    };
    int any = io->stop_count > 0 || io->reasoning_effort[0] || io->stream.set;
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++)
        any = any || nums[i].num->set;
    if (!any) return NULL;

    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++)
        if (nums[i].num->set) add_num(o, nums[i].key, nums[i].num->v);
    if (io->reasoning_effort[0] &&
        !cJSON_AddStringToObject(o, "reasoning_effort", io->reasoning_effort))
        goto fail;
    if (io->stop_count) {
        cJSON *arr = cJSON_CreateArray();
        if (!arr) goto fail;
        for (size_t i = 0; i < io->stop_count; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(io->stop[i]));
        cJSON_AddItemToObject(o, "stop", arr);
    }
    if (io->stream.set && !cJSON_AddBoolToObject(o, "stream", io->stream.v))
        goto fail;
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

static cJSON *tool_to_cjson(const agent_tool_t *t)
{
    cJSON *o = cJSON_CreateObject();
    int is_stdio = strcmp(t->type, "stdio") == 0;
    if (!o ||
        !cJSON_AddStringToObject(o, "type", t->type) ||
        !cJSON_AddStringToObject(o, "name", t->name) ||
        (is_stdio &&
         !cJSON_AddStringToObject(o, "command_line", t->command_line)))
        goto fail;
    if (!is_stdio) {
        if (!cJSON_AddStringToObject(o, "url", t->url)) goto fail;
        cJSON *h = headers_to_cjson(t->headers, t->header_count);
        if (h) cJSON_AddItemToObject(o, "headers", h);
    }
    if (t->protocol[0] && !cJSON_AddStringToObject(o, "protocol", t->protocol))
        goto fail;
    if (t->required && !cJSON_AddBoolToObject(o, "required", 1)) goto fail;
    if (t->term_count) {
        cJSON *arr = cJSON_CreateArray();
        if (!arr) goto fail;
        for (size_t i = 0; i < t->term_count; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(t->terminal_tools[i]));
        cJSON_AddItemToObject(o, "terminal_tools", arr);
    }
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

static cJSON *agent_to_cjson(const agent_t *a)
{
    cJSON *o = cJSON_CreateObject();
    if (!o ||
        !cJSON_AddStringToObject(o, "name", a->name) ||
        !cJSON_AddStringToObject(o, "llm", a->llm))
        goto fail;
    {
        cJSON *io = inference_to_cjson(&a->infer);
        if (io) cJSON_AddItemToObject(o, "inference_options", io);
    }
    if (a->system_prompt[0] &&
        !cJSON_AddStringToObject(o, "system_prompt", a->system_prompt))
        goto fail;
    if (a->tool_count) {
        cJSON *arr = cJSON_CreateArray();
        if (!arr) goto fail;
        for (size_t i = 0; i < a->tool_count; i++) {
            cJSON *t = tool_to_cjson(&a->tools[i]);
            if (!t) { cJSON_Delete(arr); goto fail; }
            cJSON_AddItemToArray(arr, t);
        }
        cJSON_AddItemToObject(o, "tools", arr);
    }
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

char *agents_to_json(const agents_t *a, const llms_t *llms, int with_builtins)
{
    /* user agents (and the builtins when asked), sorted by name */
    const agent_t *all[AGENTS_MAX + BUILTIN_N];
    size_t n = 0;
    for (size_t i = 0; i < a->count; i++) all[n++] = a->items[i];
    if (with_builtins)
        for (size_t i = 0; i < BUILTIN_N; i++) all[n++] = &BUILTIN_AGENTS[i];
    qsort(all, n, sizeof all[0], agent_name_cmp);

    cJSON *j = cJSON_CreateArray();
    if (!j) return NULL;
    for (size_t i = 0; i < n; i++) {
        cJSON *o = agent_to_cjson(all[i]);
        if (!o) { cJSON_Delete(j); return NULL; }
        if (is_builtin(all[i])) {
            if (!cJSON_AddBoolToObject(o, "builtin", 1)) {
                cJSON_Delete(o);
                cJSON_Delete(j);
                return NULL;
            }
        } else if (llms &&
            !cJSON_AddBoolToObject(o, "llm_ok",
                                   llms_find(llms, all[i]->llm) >= 0)) {
            cJSON_Delete(o);
            cJSON_Delete(j);
            return NULL;
        }
        cJSON_AddItemToArray(j, o);
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}
