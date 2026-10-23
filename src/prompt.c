/* prompt.c - system prompt assembly for llm commands. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"
#include "prompts.gen.h"

#include <stdarg.h>
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

/* format a bundled prompt template (src/prompts txt files, see
 * tools/gen-prompts.sh) with its runtime values. Returns a malloc'd
 * string, NULL on error. */
char *prompt_format(const char *tmpl, ...)
{
    va_list ap, ap2;
    int n;
    char *out;

    va_start(ap, tmpl);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, tmpl, ap);
    va_end(ap);
    if (n < 0)
        goto fail;
    out = malloc((size_t)n + 1);
    if (!out)
        goto fail;
    vsnprintf(out, (size_t)n + 1, tmpl, ap2);
    va_end(ap2);
    return out;
fail:
    va_end(ap2);
    return NULL;
}

char *prompt_build(const struct flower_settings *s,
                   const struct flower_project *p,
                   const struct flower_task *t,
                   const struct flower_action *a)
{
    struct pbuf b = {0};
    char path[FLOWER_PATH_MAX];
    char *text;

    if (pb_puts(&b, prompt_system_identity) != 0)
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
    if (pb_puts(&b, "\n") != 0
        || pb_puts(&b, prompt_system_flower_block) != 0)
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
