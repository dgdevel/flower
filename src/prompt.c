/* prompt.c - system prompt assembly for llm commands. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pbuf {
    char *data;
    size_t len, cap;
};

static int pb_reserve(struct pbuf *b, size_t extra)
{
    char *p;

    if (b->len + extra + 1 <= b->cap)
        return 0;
    b->cap = b->cap ? b->cap : 1024;
    while (b->cap < b->len + extra + 1)
        b->cap *= 2;
    p = realloc(b->data, b->cap);
    if (!p)
        return -1;
    b->data = p;
    return 0;
}

static int pb_puts(struct pbuf *b, const char *s)
{
    size_t n = strlen(s);

    if (pb_reserve(b, n) != 0)
        return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

/* read file into b under the label heading. Missing files are skipped. */
static int pb_file(struct pbuf *b, const char *path, const char *label)
{
    char *text = NULL;

    if (flower_read_file(path, &text) != 0)
        return 0; /* absent: fine */
    pb_puts(b, label);
    pb_puts(b, "\n");
    pb_puts(b, text);
    if (!text[0] || text[strlen(text) - 1] != '\n')
        pb_puts(b, "\n");
    pb_puts(b, "\n");
    free(text);
    return 0;
}

char *prompt_build(const struct flower_settings *s,
                   const struct flower_project *p,
                   const struct flower_task *t,
                   const struct flower_action *a)
{
    struct pbuf b = {0};
    char path[FLOWER_PATH_MAX];
    char *text;

    if (pb_puts(&b, "You are flower, an assistant for project management.")
        != 0)
        goto fail;
    if (s->user_name[0] || s->user_role[0])
        if (pb_puts(&b, "\nYou work with ")
            || (s->user_name[0] ? pb_puts(&b, s->user_name)
                                : pb_puts(&b, "the user"))
            || (s->user_role[0]
                    ? (pb_puts(&b, ", ") || pb_puts(&b, s->user_role))
                    : 0)
            || pb_puts(&b, "."))
            goto fail;
    if (pb_puts(&b,
                "\nWhen you propose changes to flower data files (config.json,"
                " details.md, context.md, resources.json, actions.json),"
                " write each file as a fenced block: a line with ```flower,"
                " then the target path relative to the flower base directory"
                " (for example projects/P1/details.md), then the full file"
                " content, then a closing fence.\n")
        != 0)
        goto fail;

    if (p) {
        char head[512];

        flower_snprintf(head, sizeof head,
                 "\n== project %s: %s ==\npath on disk: %s\n", p->id,
                 p->name, p->path);
        if (pb_puts(&b, head) != 0)
            goto fail;
        flower_path_join3(path, sizeof path, p->dir, "details", "md");
        if (pb_file(&b, path, "-- details.md --") != 0)
            goto fail;
        flower_path_join3(path, sizeof path, p->dir, "context", "md");
        if (pb_file(&b, path, "-- context.md --") != 0)
            goto fail;
        flower_path_join3(path, sizeof path, p->dir, "resources", "json");
        if (pb_file(&b, path, "-- resources.json --") != 0)
            goto fail;
    }

    if (t) {
        char head[512];

        snprintf(head, sizeof(head), "\n== task %s: %s ==\n", t->id, t->name);
        if (pb_puts(&b, head) != 0)
            goto fail;
        if (t->description[0]) {
            if (pb_puts(&b, "description: ") || pb_puts(&b, t->description)
                || pb_puts(&b, "\n"))
                goto fail;
        }
        flower_path_join3(path, sizeof path, t->dir, "context", "md");
        if (pb_file(&b, path, "-- task context.md --") != 0)
            goto fail;
        flower_path_join3(path, sizeof path, t->dir, "resources", "json");
        if (pb_file(&b, path, "-- task resources.json --") != 0)
            goto fail;
    }

    if (a) {
        char head[512];

        flower_snprintf(head, sizeof head, "\n== action %s: %s ==\ntype: %s\n"
                 "state: %s\noutcome: %s\nblocked by: %s\n",
                 a->id, a->name, a->type, a->state,
                 a->outcome[0] ? a->outcome : "(none)",
                 a->blocked_by[0] ? a->blocked_by : "(none)");
        if (pb_puts(&b, head) != 0)
            goto fail;
        if (a->description[0]) {
            if (pb_puts(&b, "description: ") || pb_puts(&b, a->description)
                || pb_puts(&b, "\n"))
                goto fail;
        }
    }

    text = b.data;
    return text;

fail:
    free(b.data);
    return NULL;
}
