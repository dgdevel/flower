/* config.c - flower base directory resolution and directory helpers. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int flower_base_dir(char *buf, size_t size)
{
    const char *env = getenv("XDG_CONFIG_DIR");
    const char *home;
    int n;

    if (env && *env) {
        n = snprintf(buf, size, "%s/flower", env);
        if (n < 0 || (size_t)n >= size)
            return -1;
        return 0;
    }
    home = getenv("HOME");
    if (!home || !*home)
        return -1;
    n = snprintf(buf, size, "%s/.config/flower", home);
    if (n < 0 || (size_t)n >= size)
        return -1;
    return 0;
}

int flower_ensure_dir(const char *path)
{
    char tmp[1024];
    char *p;
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* load $FLOWER_BASE/flower.json into s. 0 on success, -1 if the file is
 * absent or unreadable, -2 on invalid json or a missing/invalid required
 * field (llm.endpoint_protocol, llm.api_base, user.name, user.role).
 * model and api_key are optional and stay "" when absent. */
int flower_settings_load(struct flower_settings *s)
{
    char base[FLOWER_PATH_MAX];
    char path[FLOWER_PATH_MAX + 32];
    char *text = NULL;
    cJSON *root, *llm, *user, *item;
    int n;
    int rc = -2;

    memset(s, 0, sizeof(*s));
    if (flower_base_dir(base, sizeof(base)) != 0)
        return -1;
    n = snprintf(path, sizeof(path), "%s/flower.json", base);
    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;
    if (flower_read_file(path, &text) != 0)
        return -1;

    root = cJSON_Parse(text);
    if (!root)
        goto out;
    llm = cJSON_GetObjectItemCaseSensitive(root, "llm");
    if (!cJSON_IsObject(llm))
        goto out;
    user = cJSON_GetObjectItemCaseSensitive(root, "user");
    if (!cJSON_IsObject(user))
        goto out;

    item = cJSON_GetObjectItemCaseSensitive(llm, "endpoint_protocol");
    if (!cJSON_IsString(item) || !item->valuestring[0])
        goto out;
    item = cJSON_GetObjectItemCaseSensitive(llm, "api_base");
    if (!cJSON_IsString(item) || !item->valuestring[0])
        goto out;
    item = cJSON_GetObjectItemCaseSensitive(user, "name");
    if (!cJSON_IsString(item) || !item->valuestring[0])
        goto out;
    item = cJSON_GetObjectItemCaseSensitive(user, "role");
    if (!cJSON_IsString(item) || !item->valuestring[0])
        goto out;

    item = cJSON_GetObjectItemCaseSensitive(llm, "endpoint_protocol");
    if (cJSON_IsString(item))
        snprintf(s->protocol, sizeof(s->protocol), "%s",
                 item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(llm, "api_base");
    if (cJSON_IsString(item))
        snprintf(s->api_base, sizeof(s->api_base), "%s",
                 item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(llm, "model");
    if (cJSON_IsString(item))
        snprintf(s->model, sizeof(s->model), "%s", item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(llm, "api_key");
    if (cJSON_IsString(item))
        snprintf(s->api_key, sizeof(s->api_key), "%s",
                 item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(user, "name");
    if (cJSON_IsString(item))
        snprintf(s->user_name, sizeof(s->user_name), "%s",
                 item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(user, "role");
    if (cJSON_IsString(item))
        snprintf(s->user_role, sizeof(s->user_role), "%s",
                 item->valuestring);
    rc = 0;

out:
    cJSON_Delete(root);
    free(text);
    return rc;
}

/* read the whole file into a malloc'd nul-terminated buffer.
 * 0 on success, -1 on any failure. Caller frees *out. */
int flower_read_file(const char *path, char **out)
{
    FILE *f;
    char *buf;
    long len;
    size_t n;

    *out = NULL;
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0
        || fseek(f, 0, SEEK_SET) != 0 || len == LONG_MAX) {
        fclose(f);
        return -1;
    }
    buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (n != (size_t)len) {
        free(buf);
        return -1;
    }
    buf[len] = '\0';
    *out = buf;
    return 0;
}

/* write text to path, truncating, mode 0644. 0 on success, -1 on
 * failure. Writes everything in one pass and fails on short writes. */
int flower_write_file(const char *path, const char *text)
{
    int fd;
    size_t len = strlen(text);
    ssize_t n;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    n = write(fd, text, len);
    if (n != (ssize_t)len || close(fd) != 0) {
        if (n != (ssize_t)len)
            close(fd);
        return -1;
    }
    return 0;
}

/* count markdown list items: lines starting with "- ". 0 if text is
 * NULL. Walks lines with strchr over '\n' only. */
int flower_count_md_items(const char *text)
{
    const char *p = text;
    int count = 0;

    if (!p)
        return 0;
    for (;;) {
        if (p[0] == '-' && p[1] == ' ')
            count++;
        p = strchr(p, '\n');
        if (!p)
            break;
        p++;
    }
    return count;
}
