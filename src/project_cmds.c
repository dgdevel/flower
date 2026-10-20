/* project_cmds.c - `flower project`: show current project infos. */
#include "flower.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* human output of `flower project`. */
static int project_show(struct flower_project *p)
{
    char current[FLOWER_ID_MAX];
    int ctx, res, tasks;

    printf("project %s (%s)\n", p->id, p->name);
    if (p->description[0])
        printf("  description: %s\n", p->description);
    printf("  path: %s\n", p->path);
    printf("  dir: %s\n", p->dir);
    ctx = project_count_context_items(p);
    res = project_count_resources(p);
    tasks = project_task_count(p);
    printf("  context items: %d\n", ctx < 0 ? 0 : ctx);
    printf("  resources: %d\n", res < 0 ? 0 : res);
    printf("  tasks: %d\n", tasks < 0 ? 0 : tasks);
    if (task_current_get(p, current, sizeof(current)) == 0)
        printf("  current task: %s\n", current);
    else
        printf("  current task: (none)\n");
    return 0;
}

/* --json output of `flower project`. */
static int project_show_json(struct flower_project *p)
{
    char current[FLOWER_ID_MAX];
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
        int ctx = project_count_context_items(p);
        int res = project_count_resources(p);
        int tasks = project_task_count(p);
        int cur = task_current_get(p, current, sizeof(current));

        if (!cJSON_AddNumberToObject(root, "context_items",
                                     ctx < 0 ? 0 : ctx))
            goto fail;
        if (!cJSON_AddNumberToObject(root, "resources", res < 0 ? 0 : res))
            goto fail;
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
