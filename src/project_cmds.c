/* project_cmds.c - `flower project`: show current project infos. */
#include "flower.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* print parsed context items, one "- [type] content" line each. */
static int dump_context_items(const struct flower_project *p)
{
    struct flower_context_item *items = NULL;
    int i, n;

    printf("\n--- context items ---\n");
    if (project_load_context_items(p, &items, &n) != 0) {
        printf("(cannot read)\n");
        return 0;
    }
    if (n == 0) {
        printf("(none)\n");
        return 0;
    }
    for (i = 0; i < n; i++) {
        printf("- [%s] %s\n", items[i].type[0] ? items[i].type : "-",
               items[i].content);
        free(items[i].content);
    }
    free(items);
    return 0;
}

/* print parsed resources, one "- [type] content" line each. */
static int dump_resources(const struct flower_project *p)
{
    struct flower_resource *res = NULL;
    int i, n;

    printf("\n--- resources ---\n");
    if (project_load_resources(p, &res, &n) != 0) {
        printf("(cannot read)\n");
        return 0;
    }
    if (n == 0) {
        printf("(none)\n");
        return 0;
    }
    for (i = 0; i < n; i++) {
        printf("- [%s] %s\n", res[i].type[0] ? res[i].type : "-",
               res[i].content);
        free(res[i].content);
    }
    free(res);
    return 0;
}

/* human output of `flower project`. */
static int project_show(struct flower_project *p)
{
    char current[FLOWER_ID_MAX];
    char *details = NULL;

    printf("project %s (%s)\n", p->id, p->name);
    if (p->description[0])
        printf("  description: %s\n", p->description);
    printf("  path: %s\n", p->path);
    printf("  dir: %s\n", p->dir);
    printf("  tasks: %d\n", project_task_count(p));
    if (task_current_get(p, current, sizeof(current)) == 0)
        printf("  current task: %s\n", current);
    else
        printf("  current task: (none)\n");
    if (project_read_details(p, &details) != 0) {
        fprintf(stderr, "flower project: cannot read details.md\n");
        return 1;
    }
    printf("\n--- details.md ---\n");
    if (details) {
        printf("%s", details);
        if (details[strlen(details) - 1] != '\n')
            printf("\n");
        free(details);
    } else {
        printf("(not set)\n");
    }
    dump_context_items(p);
    dump_resources(p);
    return 0;
}

/* add the context_items array to root. 0 on success. */
static int json_context_items(const struct flower_project *p, cJSON *root)
{
    struct flower_context_item *items = NULL;
    cJSON *arr, *o, *item;
    int i, n;

    if (project_load_context_items(p, &items, &n) != 0)
        n = 0;
    arr = cJSON_CreateArray();
    if (!arr)
        goto fail;
    for (i = 0; i < n; i++) {
        o = cJSON_CreateObject();
        if (!o)
            goto fail;
        item = cJSON_CreateString(items[i].type);
        if (!item || !cJSON_AddItemToObject(o, "type", item))
            goto fail;
        item = cJSON_CreateString(items[i].content);
        if (!item || !cJSON_AddItemToObject(o, "content", item))
            goto fail;
        cJSON_AddItemToArray(arr, o);
    }
    if (!cJSON_AddItemToObject(root, "context_items", arr))
        goto fail;
    for (i = 0; i < n; i++)
        free(items[i].content);
    free(items);
    return 0;

fail:
    cJSON_Delete(arr);
    for (i = 0; i < n; i++)
        free(items[i].content);
    free(items);
    return -1;
}

/* add the resources array to root. 0 on success. */
static int json_resources(const struct flower_project *p, cJSON *root)
{
    struct flower_resource *res = NULL;
    cJSON *arr, *o, *item;
    int i, n;

    if (project_load_resources(p, &res, &n) != 0)
        n = 0;
    arr = cJSON_CreateArray();
    if (!arr)
        goto fail;
    for (i = 0; i < n; i++) {
        o = cJSON_CreateObject();
        if (!o)
            goto fail;
        item = cJSON_CreateString(res[i].type);
        if (!item || !cJSON_AddItemToObject(o, "type", item))
            goto fail;
        item = cJSON_CreateString(res[i].content);
        if (!item || !cJSON_AddItemToObject(o, "content", item))
            goto fail;
        cJSON_AddItemToArray(arr, o);
    }
    if (!cJSON_AddItemToObject(root, "resources", arr))
        goto fail;
    for (i = 0; i < n; i++)
        free(res[i].content);
    free(res);
    return 0;

fail:
    cJSON_Delete(arr);
    for (i = 0; i < n; i++)
        free(res[i].content);
    free(res);
    return -1;
}

/* --json output of `flower project`. */
static int project_show_json(struct flower_project *p)
{
    char current[FLOWER_ID_MAX];
    char *details = NULL;
    cJSON *root, *item;

    root = cJSON_CreateObject();
    if (!root)
        goto fail;
    item = cJSON_CreateString(p->id);
    if (!item || !cJSON_AddItemToObject(root, "id", item))
        goto fail;
    item = cJSON_CreateString(p->name);
    if (!item || !cJSON_AddItemToObject(root, "name", item))
        goto fail;
    item = cJSON_CreateString(p->path);
    if (!item || !cJSON_AddItemToObject(root, "path", item))
        goto fail;
    item = cJSON_CreateString(p->description);
    if (!item || !cJSON_AddItemToObject(root, "description", item))
        goto fail;
    {
        int tasks = project_task_count(p);
        int cur = task_current_get(p, current, sizeof(current));

        if (!cJSON_AddNumberToObject(root, "tasks", tasks < 0 ? 0 : tasks))
            goto fail;
        if (cur == 0) {
            item = cJSON_CreateString(current);
            if (!item || !cJSON_AddItemToObject(root, "current_task", item))
                goto fail;
        } else {
            if (!cJSON_AddNullToObject(root, "current_task"))
                goto fail;
        }
    }
    if (project_read_details(p, &details) != 0)
        goto fail;
    item = cJSON_CreateString(details ? details : "");
    if (!item || !cJSON_AddItemToObject(root, "details", item))
        goto fail;
    free(details);
    details = NULL;
    if (json_context_items(p, root) != 0)
        goto fail;
    if (json_resources(p, root) != 0)
        goto fail;
    {
        char *text = cJSON_Print(root);

        if (!text)
            goto fail;
        fputs(text, stdout);
        fputc('\n', stdout);
        free(text);
    }
    cJSON_Delete(root);
    return 0;

fail:
    free(details);
    cJSON_Delete(root);
    fprintf(stderr, "flower project: out of memory\n");
    return 1;
}

int project_cmd(int argc, char **argv)
{
    struct flower_project p;
    int json = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json"))
            json = 1;
        else {
            fprintf(stderr, "usage: flower project [--json]\n");
            return 2;
        }
    }
    if (project_find_current(&p) != 0) {
        fprintf(stderr, "flower project: no project covers the current "
                        "directory, run 'flower project scan' first\n");
        return 3;
    }
    if (json)
        return project_show_json(&p);
    return project_show(&p);
}
