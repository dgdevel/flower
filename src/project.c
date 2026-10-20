/* project.c - project registry and project file io. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* project dir id list, numeric order. Caller frees. Count back in *count.
 * 0 on success, -1 on io error. */
static int project_id_list(char ***ids, int *count)
{
    char base[FLOWER_PATH_MAX];
    char path[FLOWER_PATH_MAX];
    DIR *d;
    struct dirent *e;
    char **v = NULL;
    int n = 0, cap = 0, i, j;

    *ids = NULL;
    *count = 0;
    if (flower_base_dir(base, sizeof(base)) != 0)
        return -1;
    flower_path_join(path, sizeof path, base, "projects");
    d = opendir(path);
    if (!d)
        return 0; /* no projects yet */
    while ((e = readdir(d)) != NULL) {
        const char *s = e->d_name;

        if (s[0] != 'P' || !isdigit((unsigned char)s[1]))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            v = realloc(v, (size_t)cap * sizeof(*v));
            if (!v)
                goto fail;
        }
        v[n] = strdup(s);
        if (!v[n])
            goto fail;
        n++;
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof(*v), (int (*)(const void *, const void *))strcmp);
    /* insertion re-sort by the number after 'P': strcmp is wrong across
     * digit counts (P10 before P2). */
    for (i = 1; i < n; i++) {
        char *key = v[i];

        for (j = i - 1; j >= 0 && atoi(v[j] + 1) > atoi(key + 1); j--)
            v[j + 1] = v[j];
        v[j + 1] = key;
    }
    *ids = v;
    *count = n;
    return 0;

fail:
    closedir(d);
    for (i = 0; i < n; i++)
        free(v[i]);
    free(v);
    return -1;
}

/* load config.json of project dir into p->name/path/description.
 * 0 on success, -1 when the file cannot be read or parsed. */
static int project_load_config(struct flower_project *p)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL;
    cJSON *root, *item;
    int rc = -1;

    flower_path_join3(path, sizeof path, p->dir, "config", "json");
    if (flower_read_file(path, &text) != 0)
        return -1;
    root = cJSON_Parse(text);
    free(text);
    if (!root)
        return -1;
    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(item) && item->valuestring[0])
        snprintf(p->name, sizeof(p->name), "%s", item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (cJSON_IsString(item) && item->valuestring[0])
        snprintf(p->path, sizeof(p->path), "%s", item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(item))
        snprintf(p->description, sizeof(p->description), "%s",
                 item->valuestring);
    rc = 0;
    cJSON_Delete(root);
    return rc;
}

/* fill p from an id: id, dir, then config.json fields. 0 on success. */
static int project_fill(const char *id, struct flower_project *p)
{
    char base[FLOWER_PATH_MAX];
    struct stat st;

    if (flower_base_dir(base, sizeof(base)) != 0)
        return -1;
    memset(p, 0, sizeof(*p));
    snprintf(p->id, sizeof(p->id), "%s", id);
    flower_snprintf(p->dir, sizeof p->dir, "%s/projects/%s", base, id);
    if (stat(p->dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    if (project_load_config(p) != 0)
        return -1;
    return 0;
}

int project_list(struct flower_project *out, int max)
{
    char **ids = NULL;
    int count = 0, i, n = 0;

    if (project_id_list(&ids, &count) != 0)
        return -1;
    for (i = 0; i < count && n < max; i++) {
        if (project_fill(ids[i], &out[n]) == 0)
            n++;
    }
    for (i = 0; i < count; i++)
        free(ids[i]);
    free(ids);
    return n;
}

int project_find_current(struct flower_project *p)
{
    struct flower_project list[64];
    char cwd[FLOWER_PATH_MAX];
    int count, i, best = -1;
    size_t bestlen = 0;

    if (!getcwd(cwd, sizeof(cwd)))
        return -2;
    count = project_list(list, 64);
    if (count < 0)
        return -2;
    for (i = 0; i < count; i++) {
        size_t len = strlen(list[i].path);

        if (len == 0)
            continue;
        if (strcmp(list[i].path, cwd) == 0
            || (strncmp(list[i].path, cwd, len) == 0 && cwd[len] == '/')) {
            if ((int)len > best) {
                best = i;
                bestlen = len;
            }
        }
    }
    (void)bestlen;
    if (best < 0)
        return -1;
    *p = list[best];
    return 0;
}

int project_by_id(const char *id, struct flower_project *p)
{
    const char *s;

    if (!id || id[0] != 'P' || !isdigit((unsigned char)id[1]))
        return -1;
    for (s = id + 1; *s; s++)
        if (!isdigit((unsigned char)*s))
            return -1;
    if (project_fill(id, p) != 0)
        return -1;
    return 0;
}

int project_create(const char *fs_path, struct flower_project *p)
{
    struct flower_project list[64];
    char base[FLOWER_PATH_MAX];
    char path[FLOWER_PATH_MAX];
    char name[FLOWER_NAME_MAX];
    const char *b, *end;
    cJSON *root, *item;
    char *text;
    int count, i, max = 0, rc = -1;

    count = project_list(list, 64);
    if (count < 0)
        return -1;
    for (i = 0; i < count; i++) {
        int num = atoi(list[i].id + 1);

        if (num > max)
            max = num;
    }

    /* name: basename of fs_path, trailing slashes stripped. */
    b = fs_path;
    end = b + strlen(b);
    while (end > b + 1 && end[-1] == '/')
        end--;
    if (end == b + 1 && *b == '/') {
        snprintf(name, sizeof(name), "root");
    } else {
        const char *slash = NULL;
        const char *q;

        for (q = b; q < end; q++)
            if (*q == '/')
                slash = q;
        snprintf(name, sizeof(name), "%.*s", (int)(end - (slash ? slash + 1 : b)),
                 slash ? slash + 1 : b);
    }

    if (flower_base_dir(base, sizeof(base)) != 0)
        return -1;
    memset(p, 0, sizeof(*p));
    snprintf(p->id, sizeof(p->id), "P%d", max + 1);
    flower_snprintf(p->dir, sizeof p->dir, "%s/projects/%s", base, p->id);
    flower_path_join(path, sizeof path, p->dir, "tasks");
    if (flower_ensure_dir(path) != 0)
        return -1;
    snprintf(p->name, sizeof(p->name), "%s", name);
    snprintf(p->path, sizeof(p->path), "%s", fs_path);

    root = cJSON_CreateObject();
    if (!root)
        return -1;
    item = cJSON_CreateString(p->name);
    if (!item || !cJSON_AddItemToObject(root, "name", item))
        goto out;
    item = cJSON_CreateString(p->path);
    if (!item || !cJSON_AddItemToObject(root, "path", item))
        goto out;
    item = cJSON_CreateString("");
    if (!item || !cJSON_AddItemToObject(root, "description", item))
        goto out;
    text = cJSON_Print(root);
    if (!text)
        goto out;
    flower_path_join3(path, sizeof path, p->dir, "config", "json");
    rc = flower_write_file(path, text);
    free(text);

out:
    cJSON_Delete(root);
    return rc;
}

int project_count_context_items(const struct flower_project *p)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL;
    int n;

    flower_path_join3(path, sizeof path, p->dir, "context", "md");
    if (flower_read_file(path, &text) != 0)
        return 0;
    n = flower_count_md_items(text);
    free(text);
    return n;
}

int project_count_resources(const struct flower_project *p)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL;
    cJSON *root, *arr;
    int n = -1;

    flower_path_join3(path, sizeof path, p->dir, "resources", "json");
    if (flower_read_file(path, &text) != 0)
        return 0;
    root = cJSON_Parse(text);
    free(text);
    if (!root)
        return -1;
    if (cJSON_IsArray(root)) {
        arr = root;
    } else {
        arr = cJSON_GetObjectItemCaseSensitive(root, "resources");
    }
    if (arr && cJSON_IsArray(arr))
        n = cJSON_GetArraySize(arr);
    cJSON_Delete(root);
    return n;
}

int project_task_count(const struct flower_project *p)
{
    char path[FLOWER_PATH_MAX];
    DIR *d;
    struct dirent *e;
    struct stat st;
    int n = 0;

    flower_path_join(path, sizeof path, p->dir, "tasks");
    d = opendir(path);
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char sub[FLOWER_PATH_MAX];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        flower_path_join(sub, sizeof sub, path, e->d_name);
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
            n++;
    }
    closedir(d);
    return n;
}
