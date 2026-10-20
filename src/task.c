/* task.c - task io, current-task pointer, actions io. */
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

/* true when s is "P" or "T" followed by digits only. */
static int id_ok(const char *s, char prefix)
{
    if (!s || s[0] != prefix || !isdigit((unsigned char)s[1]))
        return 0;
    s += 2;
    while (*s) {
        if (!isdigit((unsigned char)*s))
            return 0;
        s++;
    }
    return 1;
}

/* read the whole current-task pointer. 0 on success with id filled. */
static int task_ptr_read(const struct flower_project *p, char *id, size_t size)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL;
    size_t len;

    snprintf(path, sizeof(path), "%s/current-task", p->dir);
    if (flower_read_file(path, &text) != 0)
        return -1;
    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'
                       || text[len - 1] == ' ' || text[len - 1] == '\t'))
        text[--len] = '\0';
    if (len == 0) {
        free(text);
        return -1;
    }
    snprintf(id, size, "%s", text);
    free(text);
    return 0;
}

int task_current_get(const struct flower_project *p, char *id, size_t size)
{
    return task_ptr_read(p, id, size);
}

int task_current_set(const struct flower_project *p, const char *id)
{
    char path[FLOWER_PATH_MAX];
    struct stat st;
    char text[FLOWER_ID_MAX + 2];

    snprintf(path, sizeof(path), "%s/tasks/%s", p->dir, id);
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    snprintf(text, sizeof(text), "%s\n", id);
    snprintf(path, sizeof(path), "%s/current-task", p->dir);
    return flower_write_file(path, text);
}

/* load config.json of a task dir into t->name/description. */
static int task_load_config(struct flower_task *t)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL;
    cJSON *root, *item;

    snprintf(path, sizeof(path), "%s/config.json", t->dir);
    if (flower_read_file(path, &text) != 0)
        return -1;
    root = cJSON_Parse(text);
    free(text);
    if (!root)
        return -1;
    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(item) && item->valuestring[0])
        snprintf(t->name, sizeof(t->name), "%s", item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(item))
        snprintf(t->description, sizeof(t->description), "%s",
                 item->valuestring);
    cJSON_Delete(root);
    return 0;
}

/* fill t from an id. 0 on success. */
static int task_fill(const struct flower_project *p, const char *id,
                     struct flower_task *t)
{
    struct stat st;

    memset(t, 0, sizeof(*t));
    snprintf(t->id, sizeof(t->id), "%s", id);
    snprintf(t->dir, sizeof(t->dir), "%s/tasks/%s", p->dir, id);
    if (stat(t->dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    (void)task_load_config(t); /* lenient: keep empty fields */
    return 0;
}

int task_list(const struct flower_project *p, struct flower_task *out,
              int max)
{
    char path[FLOWER_PATH_MAX];
    DIR *d;
    struct dirent *e;
    struct stat st;
    char *ids[128];
    int n = 0, i, j, total = 0;

    snprintf(path, sizeof(path), "%s/tasks", p->dir);
    d = opendir(path);
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL && n < 128) {
        if (!id_ok(e->d_name, 'T'))
            continue;
        ids[n] = strdup(e->d_name);
        if (!ids[n])
            break;
        n++;
    }
    closedir(d);
    for (i = 1; i < n; i++) {
        char *key = ids[i];

        for (j = i - 1; j >= 0 && atoi(ids[j] + 1) > atoi(key + 1); j--)
            ids[j + 1] = ids[j];
        ids[j + 1] = key;
    }
    for (i = 0; i < n && total < max; i++) {
        snprintf(path, sizeof(path), "%s/tasks/%s", p->dir, ids[i]);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            task_fill(p, ids[i], &out[total]);
            total++;
        }
    }
    for (i = 0; i < n; i++)
        free(ids[i]);
    return total;
}

int task_create(const struct flower_project *p, const char *name,
                const char *description, struct flower_task *t)
{
    struct flower_task list[64];
    char path[FLOWER_PATH_MAX];
    cJSON *root, *item;
    char *text;
    int count, i, max = 0, rc = -1;

    count = task_list(p, list, 64);
    if (count < 0)
        return -1;
    for (i = 0; i < count; i++) {
        int num = atoi(list[i].id + 1);

        if (num > max)
            max = num;
    }
    memset(t, 0, sizeof(*t));
    snprintf(t->id, sizeof(t->id), "T%d", max + 1);
    snprintf(t->dir, sizeof(t->dir), "%s/tasks/%s", p->dir, t->id);
    if (flower_ensure_dir(t->dir) != 0)
        return -1;
    snprintf(t->name, sizeof(t->name), "%s", name);
    snprintf(t->description, sizeof(t->description), "%s",
             description ? description : "");

    root = cJSON_CreateObject();
    if (!root)
        return -1;
    item = cJSON_CreateString(t->name);
    if (!item || !cJSON_AddItemToObject(root, "name", item))
        goto out;
    item = cJSON_CreateString(t->description);
    if (!item || !cJSON_AddItemToObject(root, "description", item))
        goto out;
    text = cJSON_Print(root);
    if (!text)
        goto out;
    snprintf(path, sizeof(path), "%s/config.json", t->dir);
    rc = flower_write_file(path, text);
    free(text);

out:
    cJSON_Delete(root);
    return rc;
}

int task_resolve(const struct flower_project *p, const char *ref,
                 struct flower_task *t)
{
    struct flower_project proj = *p;
    const char *dash, *tid;
    struct flower_task list[64];
    int count, i;

    dash = strchr(ref, '-');
    if (dash) {
        char pid[FLOWER_ID_MAX];
        size_t len = (size_t)(dash - ref);

        if (len == 0 || len >= sizeof(pid))
            return -2;
        memcpy(pid, ref, len);
        pid[len] = '\0';
        if (!id_ok(pid, 'P') || !id_ok(dash + 1, 'T'))
            return -2;
        if (project_by_id(pid, &proj) != 0)
            return -1;
        tid = dash + 1;
    } else {
        if (!id_ok(ref, 'T'))
            return -2;
        tid = ref;
    }
    count = task_list(&proj, list, 64);
    if (count < 0)
        return -1;
    for (i = 0; i < count; i++)
        if (!strcmp(list[i].id, tid)) {
            *t = list[i];
            return 0;
        }
    return -1;
}

int task_count_context_items(const struct flower_task *t)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL;
    int n;

    snprintf(path, sizeof(path), "%s/context.md", t->dir);
    if (flower_read_file(path, &text) != 0)
        return 0;
    n = flower_count_md_items(text);
    free(text);
    return n;
}

int task_count_resources(const struct flower_task *t)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL;
    cJSON *root, *arr;
    int n = -1;

    snprintf(path, sizeof(path), "%s/resources.json", t->dir);
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

/* copy a json string field into out when present and non-empty. */
static void get_str(cJSON *obj, const char *name, char *out, size_t size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);

    if (cJSON_IsString(item) && item->valuestring[0])
        snprintf(out, size, "%s", item->valuestring);
}

/* fill blocked_by from a string or an array of strings. */
static void get_blocked_by(cJSON *obj, char *out, size_t size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, "blocked_by");

    if (cJSON_IsString(item)) {
        snprintf(out, size, "%s", item->valuestring);
        return;
    }
    if (cJSON_IsArray(item)) {
        size_t used = 0;
        cJSON *e;

        cJSON_ArrayForEach(e, item) {
            if (!cJSON_IsString(e))
                continue;
            if (used && used + 1 < size) {
                out[used] = ',';
                out[used + 1] = '\0';
                used += 1;
            }
            if (used + strlen(e->valuestring) >= size)
                break;
            memcpy(out + used, e->valuestring, strlen(e->valuestring));
            used += strlen(e->valuestring);
            out[used] = '\0';
        }
    }
}

int action_list(const struct flower_task *t, struct flower_action *out,
                int max)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL;
    cJSON *root, *arr, *e;
    int count, i = 0;

    snprintf(path, sizeof(path), "%s/actions.json", t->dir);
    if (flower_read_file(path, &text) != 0)
        return -2;
    root = cJSON_Parse(text);
    free(text);
    if (!root)
        return -1;
    if (cJSON_IsArray(root)) {
        arr = root;
    } else {
        arr = cJSON_GetObjectItemCaseSensitive(root, "actions");
    }
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return -1;
    }
    count = cJSON_GetArraySize(arr);
    cJSON_ArrayForEach(e, arr) {
        if (i >= max || i >= count)
            break;
        memset(&out[i], 0, sizeof(out[i]));
        get_str(e, "id", out[i].id, sizeof(out[i].id));
        get_str(e, "name", out[i].name, sizeof(out[i].name));
        get_str(e, "description", out[i].description,
                sizeof(out[i].description));
        get_str(e, "type", out[i].type, sizeof(out[i].type));
        get_str(e, "state", out[i].state, sizeof(out[i].state));
        get_str(e, "outcome", out[i].outcome, sizeof(out[i].outcome));
        get_blocked_by(e, out[i].blocked_by, sizeof(out[i].blocked_by));
        i++;
    }
    cJSON_Delete(root);
    return i;
}

/* write one action object into the array. 0 on success. */
static int action_put(cJSON *arr, const struct flower_action *a)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *item;
    char *dup, *tok, *save;

    if (!o)
        return -1;
    item = cJSON_CreateString(a->id);
    if (!item || !cJSON_AddItemToObject(o, "id", item))
        goto fail;
    item = cJSON_CreateString(a->name);
    if (!item || !cJSON_AddItemToObject(o, "name", item))
        goto fail;
    item = cJSON_CreateString(a->description);
    if (!item || !cJSON_AddItemToObject(o, "description", item))
        goto fail;
    item = cJSON_CreateString(a->type);
    if (!item || !cJSON_AddItemToObject(o, "type", item))
        goto fail;
    item = cJSON_CreateString(a->state);
    if (!item || !cJSON_AddItemToObject(o, "state", item))
        goto fail;
    item = cJSON_CreateString(a->outcome);
    if (!item || !cJSON_AddItemToObject(o, "outcome", item))
        goto fail;
    if (a->blocked_by[0]) {
        cJSON *bl = cJSON_CreateArray();

        if (!bl)
            goto fail;
        dup = strdup(a->blocked_by);
        if (!dup) {
            cJSON_Delete(bl);
            goto fail;
        }
        for (tok = strtok_r(dup, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            cJSON *bi = cJSON_CreateString(tok);

            if (!bi || !cJSON_AddItemToArray(bl, bi)) {
                if (bi)
                    cJSON_Delete(bi);
                free(dup);
                cJSON_Delete(bl);
                goto fail;
            }
        }
        free(dup);
        if (!cJSON_AddItemToObject(o, "blocked_by", bl)) {
            cJSON_Delete(bl);
            goto fail;
        }
    }
    if (!cJSON_AddItemToArray(arr, o))
        goto fail;
    return 0;

fail:
    cJSON_Delete(o);
    return -1;
}

int action_save(const struct flower_task *t,
                const struct flower_action *acts, int count)
{
    char path[FLOWER_PATH_MAX];
    cJSON *arr;
    char *text;
    int i, rc = -1;

    arr = cJSON_CreateArray();
    if (!arr)
        return -1;
    for (i = 0; i < count; i++)
        if (action_put(arr, &acts[i]) != 0)
            goto out;
    text = cJSON_Print(arr);
    if (!text)
        goto out;
    snprintf(path, sizeof(path), "%s/actions.json", t->dir);
    rc = flower_write_file(path, text);
    free(text);

out:
    cJSON_Delete(arr);
    return rc;
}

int action_update(struct flower_task *t, const char *id,
                  const char *state, const char *outcome)
{
    struct flower_action acts[64];
    int n, i;

    n = action_list(t, acts, 64);
    if (n < 0)
        return -1;
    for (i = 0; i < n; i++) {
        if (strcmp(acts[i].id, id))
            continue;
        if (state)
            snprintf(acts[i].state, sizeof(acts[i].state), "%s", state);
        if (outcome)
            snprintf(acts[i].outcome, sizeof(acts[i].outcome), "%s",
                     outcome);
        return action_save(t, acts, n);
    }
    return -1;
}
