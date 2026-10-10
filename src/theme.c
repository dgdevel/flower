#define _POSIX_C_SOURCE 200809L

#include "theme.h"
#include "util.h"

#include <cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---------- schema table ---------- */

typedef enum { TF_COLOR, TF_FONT, TF_SIZE } field_kind_t;

typedef struct {
    const char *key;
    size_t offset;
    field_kind_t kind;
    size_t cap;
    const char *def;
} field_t;

#define F(key, field, kind, def) \
    { key, offsetof(theme_t, field), kind, sizeof(((theme_t *)0)->field), def }

/* These twelve are the whole theme. The look itself lives one layer
 * further out, in the derived tokens web/style.css:root builds on top
 * of them (--line, --hover, --sel, --font-mono, the size steps) — so
 * keep the two files in step when either moves. */
static const field_t fields[] = {
    F("background_primary",   background_primary,   TF_COLOR, "#0a0d10"),
    F("background_secondary", background_secondary, TF_COLOR, "#151e23"),
    F("background_tertiary",  background_tertiary,  TF_COLOR, "#1f2a30"),
    F("text_primary_color",   text_primary_color,   TF_COLOR, "#dbe3e7"),
    F("text_primary_font",    text_primary_font,    TF_FONT,  "system-ui, -apple-system, 'Segoe UI', Roboto, 'Noto Sans', sans-serif"),
    F("text_primary_size",    text_primary_size,    TF_SIZE,  "16px"),
    F("text_secondary_color", text_secondary_color, TF_COLOR, "#8a9aa4"),
    F("text_secondary_font",  text_secondary_font,  TF_FONT,  "system-ui, -apple-system, 'Segoe UI', Roboto, 'Noto Sans', sans-serif"),
    F("text_secondary_size",  text_secondary_size,  TF_SIZE,  "13px"),
    F("accent",               accent,               TF_COLOR, "#46b8c4"),
    F("success",              success,              TF_COLOR, "#5fae74"),
    F("warning",              warning,              TF_COLOR, "#d3a13f"),
};

#undef F

#define FIELD_COUNT (sizeof fields / sizeof fields[0])

static const field_t *field_by_key(const char *key)
{
    for (size_t i = 0; i < FIELD_COUNT; i++)
        if (strcmp(fields[i].key, key) == 0) return &fields[i];
    return NULL;
}

/* ---------- validation ---------- */

/* #rrggbb */
static int valid_color(const char *s)
{
    if (s[0] != '#' || strlen(s) != 7) return 0;
    for (int i = 1; i < 7; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

/* 1-3 digits, optional 1-2 fraction digits, unit px|rem|em|% */
static int valid_size(const char *s)
{
    size_t i = 0, digits = 0;
    while (s[i] >= '0' && s[i] <= '9' && digits < 3) { i++; digits++; }
    if (digits == 0) return 0;
    if (s[i] == '.') {
        i++;
        size_t frac = 0;
        while (s[i] >= '0' && s[i] <= '9' && frac < 2) { i++; frac++; }
        if (frac == 0) return 0;
    }
    const char *u = s + i;
    return strcmp(u, "px") == 0 || strcmp(u, "rem") == 0 ||
           strcmp(u, "em") == 0 || strcmp(u, "%") == 0;
}

/* ASCII letters/digits + space , - _ ' — deliberately excludes anything
 * that could break out of a CSS custom property value */
static int valid_font(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= THEME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) return 0;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == ',' || c == '-' ||
              c == '_' || c == '\''))
            return 0;
    }
    return 1;
}

static const char *validate(const field_t *f, const char *v)
{
    switch (f->kind) {
    case TF_COLOR: return valid_color(v) ? NULL : "must be a #rrggbb hex color";
    case TF_SIZE:  return valid_size(v)  ? NULL : "must look like 16px (px, rem, em, %)";
    case TF_FONT:  return valid_font(v)  ? NULL : "letters, digits, spaces and , - _ ' only";
    }
    return "invalid";
}

/* ---------- config directory ---------- */

static char g_dir[4096];

static int mkdir_p(const char *path)
{
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return -1;
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

int theme_init(const char *override)
{
    if (override && override[0]) {
        if (snprintf(g_dir, sizeof g_dir, "%s", override) >= (int)sizeof g_dir)
            return -1;
        return mkdir_p(g_dir);
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0])
        snprintf(g_dir, sizeof g_dir, "%s/flower", xdg);
    else if (home && home[0])
        snprintf(g_dir, sizeof g_dir, "%s/.config/flower", home);
    else
        return -1;
    return mkdir_p(g_dir);
}

const char *theme_dir(void)
{
    return g_dir;
}

static const char *theme_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/theme.json", g_dir);
    return buf;
}

/* ---------- load / save ---------- */

void theme_defaults(theme_t *t)
{
    memset(t, 0, sizeof *t);
    for (size_t i = 0; i < FIELD_COUNT; i++)
        memcpy((char *)t + fields[i].offset, fields[i].def,
               strlen(fields[i].def) + 1);
}

int theme_load(theme_t *t)
{
    theme_defaults(t);

    char path[4352];
    char *buf = read_whole_file(theme_path(path, sizeof path),
                                1024 * 1024);
    if (!buf) return 1; /* no file (or unreadable) -> defaults */

    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return 1; /* corrupt file -> defaults */

    /* lenient: keep defaults for missing/invalid individual values */
    if (cJSON_IsObject(j)) {
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            const cJSON *item =
                cJSON_GetObjectItemCaseSensitive(j, fields[i].key);
            if (cJSON_IsString(item) && item->valuestring &&
                validate(&fields[i], item->valuestring) == NULL) {
                memcpy((char *)t + fields[i].offset, item->valuestring,
                       strlen(item->valuestring) + 1);
            }
        }
    }
    cJSON_Delete(j);
    return 0;
}

int theme_save(const theme_t *t)
{
    char path[4352];
    theme_path(path, sizeof path);

    cJSON *j = cJSON_CreateObject();
    for (size_t i = 0; i < FIELD_COUNT; i++)
        cJSON_AddStringToObject(j, fields[i].key,
                                (const char *)t + fields[i].offset);
    int rc = save_json_atomic(path, j);
    cJSON_Delete(j);
    return rc;
}

/* ---------- JSON in (strict) / out ---------- */

theme_parse_result_t theme_from_json(const char *buf, size_t len,
                                     theme_t *out,
                                     char *err_field, size_t err_field_n,
                                     char *err_msg, size_t err_msg_n)
{
    err_field[0] = '\0';
    err_msg[0] = '\0';

    char *copy = malloc(len + 1);
    if (!copy) {
        snprintf(err_msg, err_msg_n, "out of memory");
        return THEME_E_JSON;
    }
    memcpy(copy, buf, len);
    copy[len] = '\0';

    cJSON *j = cJSON_Parse(copy);
    free(copy);
    if (!j || !cJSON_IsObject(j)) {
        cJSON_Delete(j);
        snprintf(err_msg, err_msg_n, "invalid JSON object");
        return THEME_E_JSON;
    }

    theme_defaults(out);
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        const cJSON *item =
            cJSON_GetObjectItemCaseSensitive(j, fields[i].key);
        if (!item) continue; /* missing keys keep their defaults */
        if (!cJSON_IsString(item) || !item->valuestring ||
            !item->valuestring[0]) {
            snprintf(err_field, err_field_n, "%s", fields[i].key);
            snprintf(err_msg, err_msg_n, "must be a non-empty string");
            cJSON_Delete(j);
            return THEME_E_FIELD;
        }
        const char *v = item->valuestring;
        if (strlen(v) >= fields[i].cap) {
            snprintf(err_field, err_field_n, "%s", fields[i].key);
            snprintf(err_msg, err_msg_n, "too long");
            cJSON_Delete(j);
            return THEME_E_FIELD;
        }
        const char *err = validate(&fields[i], v);
        if (err) {
            snprintf(err_field, err_field_n, "%s", fields[i].key);
            snprintf(err_msg, err_msg_n, "%s", err);
            cJSON_Delete(j);
            return THEME_E_FIELD;
        }
        memcpy((char *)out + fields[i].offset, v, strlen(v) + 1);
    }

    /* reject unknown keys (typo protection) */
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, j) {
        if (child->string && !field_by_key(child->string)) {
            snprintf(err_field, err_field_n, "%s", child->string);
            snprintf(err_msg, err_msg_n, "unknown setting");
            cJSON_Delete(j);
            return THEME_E_FIELD;
        }
    }

    cJSON_Delete(j);
    return THEME_OK;
}

static cJSON *theme_to_cjson(const theme_t *t)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    for (size_t i = 0; i < FIELD_COUNT; i++)
        if (!cJSON_AddStringToObject(j, fields[i].key,
                                     (const char *)t + fields[i].offset)) {
            cJSON_Delete(j);
            return NULL;
        }
    return j;
}

char *theme_to_json(const theme_t *t)
{
    cJSON *j = theme_to_cjson(t);
    if (!j) return NULL;
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}

char *theme_error_json(const char *msg, const char *field)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    if (!cJSON_AddStringToObject(j, "error", msg) ||
        (field && field[0] && !cJSON_AddStringToObject(j, "field", field))) {
        cJSON_Delete(j);
        return NULL;
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}
