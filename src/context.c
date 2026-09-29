/* context — typed context items, shared by the projects and tasks
 * stores (both carry the same shape, so the parse/emit rules live
 * once). Lenient load, strict save, like everywhere else. */
#define _POSIX_C_SOURCE 200809L

#include "context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- names ---------- */

static const char *const CTX_TYPE_NAMES[] = {
    "fact", "pattern", "risk", "success_metric",
    "failure_sign", "evaluation_method", "rule",
};
#define TYPE_COUNT (sizeof CTX_TYPE_NAMES / sizeof CTX_TYPE_NAMES[0])

static int type_from_name(const char *s)
{
    for (size_t i = 0; i < TYPE_COUNT; i++)
        if (strcmp(CTX_TYPE_NAMES[i], s) == 0) return (int)i;
    return -1;
}

const char *ctx_type_name(int t)
{
    return (t >= 0 && (size_t)t < TYPE_COUNT) ? CTX_TYPE_NAMES[t]
                                              : CTX_TYPE_NAMES[CTX_FACT];
}

/* valid UTF-8 with no control characters; the text is multi-line */
static int valid_text(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp;
        int need;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) {
                if (c == '\n' || c == '\t') { i++; continue; }
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
            return 0;
        }
        if (i + (size_t)need >= n) return 0;
        for (int k = 1; k <= need; k++) {
            unsigned char cc = (unsigned char)s[i + (size_t)k];
            if ((cc & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (need == 1 && cp < 0x80) return 0;
        if (need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)))
            return 0;
        if (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
        i += (size_t)need + 1;
    }
    return 1;
}

/* ---------- json error paths (same clamp-not-warn trick as
 * tasks.c: memcpy paths cannot trip -Wformat-truncation) ---------- */

static void path_set(char *dst, size_t n, const char *src)
{
    size_t l = strnlen(src, n - 1);
    memcpy(dst, src, l);
    dst[l] = '\0';
}

static void path_add(char *dst, size_t n, const char *suffix)
{
    size_t l = strnlen(dst, n - 1);
    size_t m = strnlen(suffix, n - 1 - l);
    memcpy(dst + l, suffix, m);
    dst[l + m] = '\0';
}

static void path_add_index(char *dst, size_t n, size_t i)
{
    char idx[24];
    snprintf(idx, sizeof idx, "[%zu]", i);
    path_add(dst, n, idx);
}

/* ---------- list ---------- */

void context_free(ctx_item_t *head)
{
    while (head) {
        ctx_item_t *next = head->next;
        free(head);
        head = next;
    }
}

/*
 * Parse one item object into *out (already allocated, zeroed on
 * entry by the calloc in the caller). Strict rejects into
 * err_field/err_msg; lenient fails only on an unusable text (the
 * item is dropped by the caller). Returns 0 on success.
 */
static int parse_item(const cJSON *obj, ctx_item_t *out, int strict,
                      char *ef, size_t efn, char *em, size_t emn,
                      const char *prefix)
{
    static const char *const keys[] = { "type", "text", "updated", "id" };
    char field[256];
    const cJSON *j;

    j = cJSON_GetObjectItemCaseSensitive(obj, "text");
    if (!cJSON_IsString(j) || !j->valuestring || !j->valuestring[0] ||
        strlen(j->valuestring) >= sizeof out->text ||
        !valid_text(j->valuestring, strlen(j->valuestring))) {
        path_set(field, sizeof field, prefix);
        path_add(field, sizeof field, ".text");
        snprintf(ef, efn, "%s", field);
        snprintf(em, emn, "required, text, max %d bytes, no control "
                          "characters except newlines", CTX_TEXT_MAX - 1);
        return -1; /* strict and lenient: no text, no item */
    }
    snprintf(out->text, sizeof out->text, "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(obj, "type");
    if (j && (!cJSON_IsString(j) || type_from_name(j->valuestring) < 0)) {
        if (strict) {
            path_set(field, sizeof field, prefix);
            path_add(field, sizeof field, ".type");
            snprintf(ef, efn, "%s", field);
            snprintf(em, emn, "fact, pattern, risk, success_metric, "
                              "failure_sign, evaluation_method or rule");
            return -1;
        } /* lenient: stays "fact" */
    } else if (j && cJSON_IsString(j)) {
        out->type = type_from_name(j->valuestring);
    }

    /* the update time; "created" is the retired name, still honored
     * by the lenient loader so pre-update files keep their stamps */
    j = cJSON_GetObjectItemCaseSensitive(obj, "updated");
    if (!j && !strict)
        j = cJSON_GetObjectItemCaseSensitive(obj, "created");
    if (cJSON_IsNumber(j) && j->valuedouble >= 0 &&
        j->valuedouble == (double)(long long)j->valuedouble) {
        out->updated = (long long)j->valuedouble;
    } else if (j && strict) {
        path_set(field, sizeof field, prefix);
        path_add(field, sizeof field, ".updated");
        snprintf(ef, efn, "%s", field);
        snprintf(em, emn, "must be a unix timestamp in seconds");
        return -1;
    } else {
        out->updated = (long long)time(NULL); /* missing, or lenient repair */
    }

    if (strict) { /* unknown keys are typos -> reject (like theme.c);
                   * "id" is known but ignored — it is positional and
                   * regenerated on every read, so a stored value is
                   * meaningless */
        cJSON_ArrayForEach(j, obj) {
            int known = 0;
            for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
                if (j->string && strcmp(j->string, keys[k]) == 0) known = 1;
            if (!known) {
                path_set(field, sizeof field, prefix);
                path_add(field, sizeof field, ".");
                path_add(field, sizeof field, j->string ? j->string : "");
                snprintf(ef, efn, "%s", field);
                snprintf(em, emn, "unknown setting");
                return -1;
            }
        }
    }
    return 0;
}

int context_from_json(const cJSON *j, ctx_item_t **head, int strict,
                      char *ef, size_t efn, char *em, size_t emn,
                      const char *prefix)
{
    *head = NULL;
    if (!j) return 0;
    if (!cJSON_IsArray(j)) {
        if (strict) {
            snprintf(ef, efn, "%s", prefix);
            snprintf(em, emn, "must be an array of context items");
            return -1;
        }
        return 0; /* lenient: not a list -> no items */
    }

    ctx_item_t **tail = head;
    size_t n = 0, i = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, j) {
        if (n >= CONTEXT_MAX) {
            if (strict) {
                snprintf(ef, efn, "%s", prefix);
                snprintf(em, emn, "too many context items (max %d)",
                         CONTEXT_MAX);
                context_free(*head);
                *head = NULL;
                return -1;
            }
            break; /* lenient: keep the first CONTEXT_MAX */
        }
        if (!cJSON_IsObject(child)) {
            if (strict) {
                char field[256];
                path_set(field, sizeof field, prefix);
                path_add_index(field, sizeof field, i);
                snprintf(ef, efn, "%s", field);
                snprintf(em, emn, "must be an object");
                context_free(*head);
                *head = NULL;
                return -1;
            }
            i++;
            continue;
        }
        ctx_item_t *one = calloc(1, sizeof *one);
        if (!one) {
            snprintf(em, emn, "out of memory");
            if (strict) {
                context_free(*head);
                *head = NULL;
                return -1;
            }
            i++;
            continue;
        }
        char item_prefix[1024];
        path_set(item_prefix, sizeof item_prefix, prefix);
        path_add_index(item_prefix, sizeof item_prefix, i);
        if (parse_item(child, one, strict, ef, efn, em, emn,
                       item_prefix) != 0) {
            free(one);
            if (strict) {
                context_free(*head);
                *head = NULL;
                return -1;
            }
            i++;
            continue; /* lenient: skip the broken item */
        }
        *tail = one;
        tail = &one->next;
        n++;
        i++;
    }
    return 0;
}

cJSON *context_to_cjson(const ctx_item_t *head, char id_prefix)
{
    if (!head) return NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    size_t i = 0;
    for (const ctx_item_t *p = head; p; p = p->next, i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            cJSON_Delete(arr);
            return NULL;
        }
        if (id_prefix) { /* the generated positional id, "P1", "T2"… */
            char id[8]; /* "P64" at most */
            snprintf(id, sizeof id, "%c%zu", id_prefix, i + 1);
            if (!cJSON_AddStringToObject(o, "id", id)) {
                cJSON_Delete(o);
                cJSON_Delete(arr);
                return NULL;
            }
        }
        if (!cJSON_AddStringToObject(o, "text", p->text)) {
            cJSON_Delete(o);
            cJSON_Delete(arr);
            return NULL;
        }
        if (p->type != CTX_FACT &&
            !cJSON_AddStringToObject(o, "type", ctx_type_name(p->type))) {
            cJSON_Delete(o);
            cJSON_Delete(arr);
            return NULL;
        }
        if (!cJSON_AddNumberToObject(o, "updated", (double)p->updated)) {
            cJSON_Delete(o);
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}
