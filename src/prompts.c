/*
 * prompts — lookup and templating for the compiled-in prompt texts.
 * See prompts.h for the layout and src/prompts_gen.[ch] for the
 * generated table (built by tools/embed from prompts/).
 */
#define _POSIX_C_SOURCE 200809L

#include "prompts.h"

#include "context.h"
#include "prompts_gen.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *prompt_text(const char *name)
{
    if (!name || !*name) return NULL;

    char path[512];
    snprintf(path, sizeof path, "/%s", name);
    const prompt_t *p = prompt_find(path);
    if (!p) return NULL;

    /* not NUL-terminated in the table; copy with room for one */
    char *s = malloc(p->size + 1);
    if (!s) return NULL;
    memcpy(s, p->data, p->size);
    s[p->size] = '\0';

    /* trim trailing whitespace (authors keep a final newline) */
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' ' ||
                     s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

/* "Field: text" with multi-line text indented two spaces; skipped
 * entirely when the field is empty */
static int put_field(sbuf_t *out, const char *field, const char *text)
{
    if (!text || !*text) return 0;
    if (sb_puts(out, field) != 0) return -1;
    if (sb_puts(out, ": ") != 0) return -1;
    for (const char *p = text; *p; p++) {
        char c[2] = { *p, '\0' };
        if (*p == '\n') {
            if (sb_puts(out, "\n  ") != 0) return -1;
        } else if (sb_putn(out, c, 1) != 0) {
            return -1;
        }
    }
    if (sb_puts(out, "\n") != 0) return -1;
    return 0;
}

static int put_attributes(sbuf_t *out, const project_t *p)
{
    if (!p) return 0;
    if (put_field(out, "Description", p->description) != 0 ||
        put_field(out, "Objectives", p->objectives) != 0 ||
        put_field(out, "Scope", p->scope) != 0 ||
        put_field(out, "Stakeholders", p->stakeholders) != 0)
        return -1;
    return 0;
}

static int put_context(sbuf_t *out, const project_t *p)
{
    if (!p) return 0;
    for (const ctx_item_t *it = p->context; it; it = it->next) {
        if (sb_puts(out, "- [") != 0) return -1;
        if (sb_puts(out, ctx_type_name(it->type)) != 0) return -1;
        if (sb_puts(out, "] ") != 0) return -1;
        /* a resource leads with its spec — the location is the
         * item's identity, the text only annotates it */
        if (it->type == CTX_RESOURCE && it->spec[0]) {
            if (sb_puts(out, it->spec) != 0) return -1;
            if (sb_puts(out, " — ") != 0) return -1;
        }
        /* indent continuation lines of multi-line item text */
        for (const char *c = it->text; *c; c++) {
            if (*c == '\n') {
                if (sb_puts(out, "\n  ") != 0) return -1;
            } else {
                char one[2] = { *c, '\0' };
                if (sb_putn(out, one, 1) != 0) return -1;
            }
        }
        if (sb_puts(out, "\n") != 0) return -1;
    }
    return 0;
}

/* the value of a known variable, rendered into a scratch sbuf the
 * caller resets per use; 0 when the name is known, 1 when it is not
 * (caller-supplied variables are consulted after the fixed set) */
static int render_var(sbuf_t *out, const char *name, size_t name_len,
                      const project_t *p, const prompt_var_t *extra,
                      size_t extra_n)
{
    if (name_len == 12 && strncmp(name, "project_path", 12) == 0)
        return p ? sb_puts(out, p->dir) : 0;
    if (name_len == 12 && strncmp(name, "project_name", 12) == 0)
        return p ? sb_puts(out, p->title) : 0;
    if (name_len == 18 && strncmp(name, "project_attributes", 18) == 0)
        return put_attributes(out, p);
    if (name_len == 15 && strncmp(name, "project_context", 15) == 0)
        return put_context(out, p);
    for (size_t i = 0; i < extra_n; i++) {
        const prompt_var_t *v = &extra[i];
        if (!v->name || strlen(v->name) != name_len) continue;
        if (strncmp(v->name, name, name_len) != 0) continue;
        return sb_puts(out, v->value ? v->value : "");
    }
    return 1; /* not a known variable */
}

char *prompt_render_vars(const char *tpl, const project_t *proj,
                         const prompt_var_t *extra, size_t extra_n)
{
    if (!tpl) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    sbuf_t out = { 0 };
    const char *p = tpl;
    while (*p) {
        const char *open = strstr(p, "{{");
        if (!open) {
            if (sb_puts(&out, p) != 0) goto oom;
            break;
        }
        if (sb_putn(&out, p, (size_t)(open - p)) != 0) goto oom;
        const char *close = strstr(open + 2, "}}");
        if (!close) { /* unterminated: keep the rest verbatim */
            if (sb_puts(&out, open) != 0) goto oom;
            break;
        }
        const char *name = open + 2;
        size_t name_len = (size_t)(close - name);

        /* allow no spaces around the name: "{{ var }}" stays verbatim */
        sbuf_t val = { 0 };
        int known = render_var(&val, name, name_len, proj, extra, extra_n);
        if (known == 0) {
            if (sb_putn(&out, val.data ? val.data : "",
                          val.len) != 0) {
                free(val.data);
                goto oom;
            }
            free(val.data);
        } else { /* unknown token: emit verbatim, typos stay visible */
            if (sb_putn(&out, open, (size_t)(close + 2 - open)) != 0)
                goto oom;
        }
        p = close + 2;
    }

    if (!out.data) { /* nothing rendered: return the empty string */
        out.data = malloc(1);
        if (!out.data) return NULL;
        out.data[0] = '\0';
    }
    return out.data;

oom:
    free(out.data);
    {
        char *copy = strdup(tpl);
        return copy; /* out of memory: degrade to the raw template */
    }
}

char *prompt_render(const char *tpl, const project_t *proj)
{
    return prompt_render_vars(tpl, proj, NULL, 0);
}
