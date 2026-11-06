/* flower.c - command layer: setup and project/task configuration
   editing. grammar: docs/config_cli.md, layout: docs/config_layout.md,
   file format: docs/config_markdown_format.md. */
#define _POSIX_C_SOURCE 200809L
#include "flower.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h> /* realpath, PATH_MAX */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int usage(FILE *out) {
    fputs("usage: flower setup\n"
          "       flower project [<P>] set <Key> \"<Value>\"\n"
          "       flower project [<P>] show\n"
          "       flower project [<P>] details set <Field> \"<text>\"\n"
          "       flower project [<P>] details remove <Field>\n"
          "       flower project [<P>] details show [Field]\n"
          "       flower project [<P>] context add <Prefix> \"<Title>\" "
          "\"<Body>\"\n"
          "       flower project [<P>] context edit <Prefix> \"<Title>\" "
          "\"<NewBody>\"\n"
          "       flower project [<P>] context rename <Prefix> \"<Title>\" "
          "\"<NewTitle>\"\n"
          "       flower project [<P>] context remove <Prefix> \"<Title>\"\n"
          "       flower project [<P>] context list\n"
          "       flower project [<P>] resource add \"<Name>\" \"<Pointer>\"\n"
          "       flower project [<P>] resource set \"<Name>\" \"<Pointer>\"\n"
          "       flower project [<P>] resource remove \"<Name>\"\n"
          "       flower project [<P>] resource list\n"
          "       flower project [<P>] task <T> set <Key> \"<Value>\"\n"
          "       flower project [<P>] task <T> show\n"
          "       flower project [<P>] task <T> context <verb> [args]\n"
          "       flower project [<P>] task <T> action add \"<Title>\" "
          "[--description \"<text>\"]\n"
          "       flower project [<P>] task <T> action <A> set <Key> "
          "\"<Value>\"\n"
          "       flower project [<P>] task <T> action <A> remove\n"
          "       flower project [<P>] task <T> action list "
          "[--pending|--complete]\n"
          "       flower project [<P>] remove\n"
          "       flower project [<P>] task <T> remove\n"
          "       flower llm add <name> --base <url> --protocol <p> "
          "[--key <k>]\n"
          "                          [--model <m>] [--max-tokens <n>] "
          "[--reasoning-effort <v>]\n"
          "                          [--header <n=v>]... [--mcp-proxy "
          "<cfg>]...\n"
          "                          [--terminal-tool <t>]...\n"
          "       flower llm <name> set <Key> \"<Value>\"\n"
          "       flower llm <name> unset <Key>\n"
          "       flower llm <name> remove\n"
          "       flower llm <name> show\n"
          "       flower llm list\n"
          "       flower project [<P>] scan <profile> [--dry-run]\n"
          "       flower edit <path>\n"
          "       flower mcp [<P>] <area>\n",
          out);
    return 1;
}


/* strip one pair of surrounding double quotes. returns NULL after
   complaining when the quotes are unbalanced. mutates s in place. */
static char *arg_val(char *s) {
    size_t n = strlen(s);
    int open = n > 0 && s[0] == '"';
    int close = n > 0 && s[n - 1] == '"';
    if (open != close) {
        fprintf(stderr, "flower: unbalanced quotes in '%s'\n", s);
        return NULL;
    }
    if (open && n >= 2) {
        s[n - 1] = '\0';
        return s + 1;
    }
    if (open) { /* the string is one lone quote char: empty value */
        s[0] = '\0';
        return s;
    }
    return s;
}

/* ---------- setup ---------- */

/* prompt for one key; empty line or eof keeps the current value */
static int prompt_kv(md_chapter_t *c, const char *key) {
    char line[4096];
    const char *cur =
        md_kv_get((const char **)c->keys, (const char **)c->vals, c->nkv, key);
    if (cur && cur[0]) printf("%s [%s]: ", key, cur);
    else printf("%s: ", key);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return 0;
    str_trim(line);
    if (!line[0]) return 0;
    return md_kv_set(c, key, line);
}

/* one yes/no prompt. 1 yes, 0 no; empty line answers def_yes, eof is
   always no so piping /dev/null never invents profiles. */
static int ask_yes(const char *prompt, int def_yes) {
    char line[64];
    printf("%s ", prompt);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return 0;
    str_trim(line);
    if (!line[0]) return def_yes;
    if (line[0] == 'y' || line[0] == 'Y') return 1;
    return 0;
}

/* one text prompt with a "[current]" hint when cur is set. out gets
   the trimmed answer; *eof_out flags eof. returns 1 for non-empty
   text, 0 for empty line or eof. */
static int ask_line(const char *prompt, const char *cur, char *out,
                    size_t outn, int *eof_out) {
    *eof_out = 0;
    if (cur && cur[0]) printf("%s [%s]: ", prompt, cur);
    else printf("%s: ", prompt);
    fflush(stdout);
    if (!fgets(out, (int)outn, stdin)) {
        *eof_out = 1;
        out[0] = '\0';
        return 0;
    }
    str_trim(out);
    return out[0] != '\0';
}

/* one llm profile walk. *c_io names the existing chapter or comes in
   NULL to create it. keys asked in store_llm_keys order: Api base
   (required once the chapter is empty), Protocol (validated, retried),
   Api key, Model, Max tokens (digits, retried), Reasoning effort.
   name is fixed, repeatable Header/Mcp proxy/Terminal tool stay with
   `flower llm <name> set`. empty input keeps the current value, eof
   stops. *changed reports any write to the doc. 0 ok, 1 oom. */
static int prompt_llm_chapter(md_doc_t *doc, md_chapter_t **c_io,
                              const char *name, int *changed, int *eof) {
    static const struct {
        const char *key;
        int digits; /* 1: llm_digits validated, retried */
    } walk[] = {{"Api base", 0},  {"Protocol", 0},   {"Api key", 0},
                {"Model", 0},     {"Max tokens", 1}, {"Reasoning effort", 0}};
    md_chapter_t *c = *c_io;
    int fresh = c == NULL;
    int i;
    if (fresh) {
        c = doc_add_chapter(doc, name, 1);
        if (!c) return oom();
        *c_io = c;
        *changed = 1;
    }
    for (i = 0; i < (int)(sizeof walk / sizeof walk[0]); i++) {
        const char *key = walk[i].key;
        char line[4096];
        int e = 0, want = 0;
        const char *cur = md_kv_get((const char **)c->keys,
                                    (const char **)c->vals, c->nkv, key);
        for (;;) {
            if (!ask_line(key, cur, line, sizeof line, &e)) {
                if (e) { /* eof: keep what was collected so far */
                    *eof = 1;
                    return 0;
                }
                if (fresh || i != 0) break; /* keep current value */
                fprintf(stderr, "flower: %s is required\n", key);
                continue; /* first key of a new chapter: retry */
            }
            if (walk[i].digits && !llm_digits(line)) {
                fprintf(stderr, "flower: invalid %s '%s'\n", key, line);
                continue;
            }
            if (!strcmp(key, "Protocol") &&
                !store_valid_llm_protocol(line)) {
                fprintf(stderr, "flower: invalid Protocol '%s'\n", line);
                continue;
            }
            want = 1;
            break;
        }
        if (want && md_kv_set(c, key, line) != 0) return oom();
        if (want) *changed = 1;
    }
    return 0;
}

/* guided llms.md walk after flower.md: list or offer to add, then edit
   or create profiles by name. eof anywhere just stops. saves only when
   something changed. 0 ok, 1 error. */
static int setup_llm_walk(void) {
    const char *file = "llms.md";
    md_doc_t doc;
    int changed = 0, eof = 0, rc = 0, i;
    if (load_rel(file, FV_LLM, &doc)) return 1;
    if (doc.nch == 0) {
        if (!ask_yes("no llm profiles configured. add one? [Y/n]", 1))
            goto out;
    } else {
        for (i = 0; i < doc.nch; i++)
            printf("  %d. %s\n", i + 1, doc.ch[i].title);
        if (!ask_yes("edit llm profiles? [y/N]", 0)) goto out;
    }
    while (!eof) {
        md_chapter_t *c = NULL;
        char name[4096];
        int e = 0;
        if (doc.nch > 0) {
            if (!ask_line("profile to edit (name, Enter to skip)", NULL,
                          name, sizeof name, &e)) {
                if (e) break;
                goto another;
            }
        } else {
            if (!ask_line("profile name (Enter to stop)", NULL, name,
                          sizeof name, &e))
                break;
        }
        if (!store_valid_llm_name(name)) {
            fprintf(stderr, "flower: invalid profile name '%s'\n", name);
            goto another;
        }
        c = md_find(&doc, name);
        if (!c && doc.nch > 0 &&
            !ask_yes("no profile. create? [y/N]", 0))
            goto another;
        while (!c && md_find(&doc, name)) {
            /* duplicate title: md would parse-refuse it, so retry */
            char dup[4096];
            int de = 0;
            fprintf(stderr, "flower: duplicate profile '%s'\n", name);
            if (!ask_line("profile name (Enter to stop)", NULL, dup,
                          sizeof dup, &de)) {
                if (de) goto out; /* eof: keep what we have */
                goto another;
            }
            if (!store_valid_llm_name(dup)) {
                fprintf(stderr, "flower: invalid profile name '%s'\n",
                        dup);
                continue;
            }
            strcpy(name, dup);
        }
        if (prompt_llm_chapter(&doc, &c, name, &changed, &eof) != 0) {
            rc = 1;
            goto out;
        }
        if (eof) break;
    another:
        if (!ask_yes("add another llm profile? [y/N]", 0)) break;
    }
out:
    if (!rc && changed) {
        if (store_mkdir_p(NULL) != 0) rc = 1;
        else rc = save_rel(NULL, file, &doc);
    }
    md_free_doc(&doc);
    return rc;
}

static int cmd_setup(void) {
    char *rel = "flower.md";
    md_doc_t doc;
    md_chapter_t *u;
    int rc = 1;
    if (load_rel(rel, FV_USER, &doc)) return 1;
    u = md_find(&doc, "User");
    if (!u) u = doc_add_chapter(&doc, "User", 1);
    if (!u) goto out;
    if (prompt_kv(u, "Name") != 0) goto out;
    if (prompt_kv(u, "Role") != 0) goto out;
    if (store_mkdir_p(NULL) != 0) goto out;
    rc = save_rel(NULL, rel, &doc);
    if (rc == 0) rc = setup_llm_walk();
out:
    md_free_doc(&doc);
    return rc;
}

/* ---------- config.md (user/project/task share the shape) ---------- */

static int cmd_config_set(int kind, const char *dir_rel, const char *file_rel,
                          char *key, char *val) {
    const char *chapter = NULL;
    const char *const *keys = NULL;
    md_doc_t doc;
    md_chapter_t *c;
    int rc;
    kind_schema(kind, &chapter, &keys);
    if (!key_allowed(keys, key)) {
        fprintf(stderr, "flower: unknown key '%s'\n", key);
        return 1;
    }
    if (load_rel(file_rel, kind, &doc)) return 1;
    c = md_find(&doc, chapter);
    if (!c) c = doc_add_chapter(&doc, chapter, 1);
    if (!c) {
        md_free_doc(&doc);
        return oom();
    }
    if (md_kv_set(c, key, val) != 0) {
        md_free_doc(&doc);
        return oom();
    }
    rc = save_rel(dir_rel, file_rel, &doc);
    md_free_doc(&doc);
    return rc;
}

/* ---------- details ---------- */

/* a[0]=verb, n=args after it */
static int cmd_details(const char *pid, char **a, int n) {
    const char *verb = a[0];
    char *file = xasprintf("projects/%s/details.md", pid);
    md_doc_t doc;
    int rc = 1;
    if (!file) return oom();

    if (!strcmp(verb, "set") && n == 3) {
        char *field = arg_val(a[1]);
        char *text = arg_val(a[2]);
        md_chapter_t *c;
        if (!field || !text) goto usage_out;
        if (!key_allowed(store_detail_fields, field)) {
            fprintf(stderr, "flower: unknown field '%s'\n", field);
            goto out;
        }
        if (load_rel(file, FV_FIELDS, &doc)) goto out;
        c = md_find(&doc, field);
        if (!c) c = doc_add_chapter(&doc, field, 1);
        if (!c || chapter_set_body(c, text) != 0) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

    if (!strcmp(verb, "remove") && n == 2) {
        char *field = arg_val(a[1]);
        md_chapter_t *c;
        if (!field) goto usage_out;
        if (!key_allowed(store_detail_fields, field)) {
            fprintf(stderr, "flower: unknown field '%s'\n", field);
            goto out;
        }
        if (load_rel(file, FV_FIELDS, &doc)) goto out;
        c = md_find(&doc, field);
        if (!c) {
            fprintf(stderr, "flower: no such field '%s'\n", field);
            md_free_doc(&doc);
            goto out;
        }
        doc_remove_chapter(&doc, (int)(c - doc.ch));
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

    if (!strcmp(verb, "show") && (n == 1 || n == 2)) {
        if (n == 1) {
            rc = print_rel_file(file);
            goto out;
        }
        {
            char *field = arg_val(a[1]);
            md_chapter_t *c;
            int i;
            if (!field) goto usage_out;
            if (!key_allowed(store_detail_fields, field)) {
                fprintf(stderr, "flower: unknown field '%s'\n", field);
                goto out;
            }
            if (load_rel(file, FV_FIELDS, &doc)) goto out;
            c = md_find(&doc, field);
            if (!c) {
                fprintf(stderr, "flower: no such field '%s'\n", field);
                md_free_doc(&doc);
                goto out;
            }
            for (i = 0; i < c->nbody; i++)
                printf("%s\n", c->body[i].text);
            md_free_doc(&doc);
            rc = ferror(stdout) ? 1 : 0;
            goto out;
        }
    }

usage_out:
    rc = usage(stderr);
out:
    free(file);
    return rc;
}

/* ---------- context (project and task files share the shape) ---------- */

/* a[0]=verb, n=args after it */
static int cmd_context(const char *file_rel, char **a, int n) {
    const char *verb;
    char *pfx_arg, *title_arg;
    const char *pfx;
    char *full;
    md_doc_t doc;
    md_chapter_t *ch;
    int rc = 1;

    if (n < 1) return usage(stderr);
    verb = a[0];

    if (!strcmp(verb, "list")) {
        int i;
        if (n != 1) return usage(stderr);
        if (load_rel(file_rel, FV_CONTEXT, &doc)) return 1;
        for (i = 0; i < doc.nch; i++) printf("%s\n", doc.ch[i].title);
        md_free_doc(&doc);
        return ferror(stdout) ? 1 : 0;
    }

    if (strcmp(verb, "add") && strcmp(verb, "edit") && strcmp(verb, "rename") &&
        strcmp(verb, "remove"))
        return usage(stderr);
    if ((!strcmp(verb, "add") || !strcmp(verb, "edit") ||
         !strcmp(verb, "rename")) &&
        n != 4)
        return usage(stderr);
    if (!strcmp(verb, "remove") && n != 3) return usage(stderr);

    pfx_arg = arg_val(a[1]);
    title_arg = arg_val(a[2]);
    if (!pfx_arg || !title_arg) return usage(stderr);
    pfx = prefix_known(pfx_arg);
    if (!pfx) {
        fprintf(stderr, "flower: unknown prefix '%s'\n", pfx_arg);
        return 1;
    }
    if (!title_arg[0]) {
        fprintf(stderr, "flower: empty title\n");
        return 1;
    }
    full = store_make_context_title(pfx, title_arg);
    if (!full) return oom();
    if (load_rel(file_rel, FV_CONTEXT, &doc)) {
        free(full);
        return 1;
    }
    ch = md_find(&doc, full);

    if (!strcmp(verb, "add")) {
        char *body = arg_val(a[3]);
        if (!body) {
            rc = usage(stderr);
            goto out;
        }
        if (ch || context_title_taken(&doc, title_arg, NULL)) {
            fprintf(stderr, "flower: duplicate context title '%s'\n",
                    title_arg);
            goto out;
        }
        ch = doc_add_chapter(&doc, full, 1);
        if (!ch || chapter_set_body(ch, body) != 0) {
            rc = oom();
            goto out;
        }
    } else if (!strcmp(verb, "edit")) {
        char *body = arg_val(a[3]);
        if (!body) {
            rc = usage(stderr);
            goto out;
        }
        if (!ch) {
            fprintf(stderr, "flower: no such context '%s'\n", full);
            goto out;
        }
        if (chapter_set_body(ch, body) != 0) {
            rc = oom();
            goto out;
        }
    } else if (!strcmp(verb, "rename")) {
        char *nt = arg_val(a[3]);
        char *nf;
        if (!nt) {
            rc = usage(stderr);
            goto out;
        }
        if (!nt[0]) {
            fprintf(stderr, "flower: empty title\n");
            goto out;
        }
        if (!ch) {
            fprintf(stderr, "flower: no such context '%s'\n", full);
            goto out;
        }
        if (context_title_taken(&doc, nt, ch)) {
            fprintf(stderr, "flower: duplicate context title '%s'\n", nt);
            goto out;
        }
        nf = store_make_context_title(pfx, nt);
        if (!nf) {
            rc = oom();
            goto out;
        }
        free(ch->title);
        ch->title = nf;
    } else { /* remove */
        if (!ch) {
            fprintf(stderr, "flower: no such context '%s'\n", full);
            goto out;
        }
        doc_remove_chapter(&doc, (int)(ch - doc.ch));
    }

    rc = save_rel(NULL, file_rel, &doc);
out:
    md_free_doc(&doc);
    free(full);
    return rc;
}

/* ---------- resources ---------- */

/* a[0]=verb, n=args after it */
static int cmd_resource(const char *pid, char **a, int n) {
    const char *verb = a[0];
    char *file = xasprintf("projects/%s/resources.md", pid);
    md_doc_t doc;
    int rc = 1;
    if (!file) return oom();

    if (!strcmp(verb, "list") && n == 1) {
        int i, j;
        if (load_rel(file, FV_RESOURCE, &doc)) {
            free(file);
            return 1;
        }
        for (i = 0; i < doc.nch; i++) {
            printf("%s\n", doc.ch[i].title);
            for (j = 0; j < doc.ch[i].nbody; j++)
                printf("  %s\n", doc.ch[i].body[j].text);
        }
        md_free_doc(&doc);
        free(file);
        return ferror(stdout) ? 1 : 0;
    }

    if ((!strcmp(verb, "add") || !strcmp(verb, "set")) && n == 3) {
        char *name = arg_val(a[1]);
        char *ptr = arg_val(a[2]);
        md_chapter_t *c;
        if (!name || !ptr) goto usage_out;
        if (!name[0]) {
            fprintf(stderr, "flower: empty resource name\n");
            goto out;
        }
        if (load_rel(file, FV_RESOURCE, &doc)) goto out;
        c = md_find(&doc, name);
        if (!strcmp(verb, "add")) {
            if (c) {
                fprintf(stderr, "flower: duplicate resource '%s'\n", name);
                goto out_doc;
            }
            c = doc_add_chapter(&doc, name, 1);
        } else if (!c) {
            fprintf(stderr, "flower: no such resource '%s'\n", name);
            goto out_doc;
        }
        if (!c || chapter_set_body(c, ptr) != 0) {
            rc = oom();
            goto out_doc;
        }
        rc = save_rel(NULL, file, &doc);
        goto out_doc;
    }

    if (!strcmp(verb, "remove") && n == 2) {
        char *name = arg_val(a[1]);
        md_chapter_t *c;
        if (!name) goto usage_out;
        if (load_rel(file, FV_RESOURCE, &doc)) goto out;
        c = md_find(&doc, name);
        if (!c) {
            fprintf(stderr, "flower: no such resource '%s'\n", name);
            goto out_doc;
        }
        doc_remove_chapter(&doc, (int)(c - doc.ch));
        rc = save_rel(NULL, file, &doc);
        goto out_doc;
    }

usage_out:
    rc = usage(stderr);
    goto out;
out_doc:
    md_free_doc(&doc);
out:
    free(file);
    return rc;
}

/* ---------- actions ---------- */

/* chapter for action id, NULL when absent. id must be validated. */
static md_chapter_t *action_find(md_doc_t *doc, const char *id) {
    char want[64];
    int i;
    snprintf(want, sizeof want, "%s: ", id);
    for (i = 0; i < doc->nch; i++)
        if (str_starts(doc->ch[i].title, want) &&
            action_valid_title(doc->ch[i].title))
            return &doc->ch[i];
    return NULL;
}

/* 1 when some action's Depends mentions id (skip's own Depends is
   ignored: self-reference is rejected at set time, not here) */
static char *action_depended_on(md_doc_t *doc, const char *id,
                                md_chapter_t *skip) {
    int i;
    for (i = 0; i < doc->nch; i++) {
        md_chapter_t *c = &doc->ch[i];
        const char *dep = md_kv_get((const char **)c->keys,
                                    (const char **)c->vals, c->nkv,
                                    "Depends");
        char **ids = NULL;
        int n, j;
        if (!dep) continue;
        n = store_parse_id_list(dep, &ids);
        if (n < 0) return NULL; /* oom marker: callers see no clash */
        for (j = 0; j < n; j++) {
            if (!strcmp(ids[j], id) && c != skip) {
                store_free_ids(ids, n);
                return c->title;
            }
        }
        store_free_ids(ids, n);
    }
    return NULL;
}

/* a[0]=verb or action id, n=args after it */
static int cmd_action(const char *pid, const char *tid, char **a, int n) {
    char *file = xasprintf("projects/%s/tasks/%s/actions.md", pid, tid);
    md_doc_t doc;
    int rc = 1;
    if (!file) return oom();

    if (!strcmp(a[0], "add")) {
        char *title = NULL;
        char *desc = NULL;
        char *id = NULL;
        md_chapter_t *c;
        int i;
        if (n < 2) goto usage_out;
        title = arg_val(a[1]);
        if (!title) goto usage_out;
        if (!title[0]) {
            fprintf(stderr, "flower: empty action title\n");
            goto out;
        }
        for (i = 2; i < n; i++) {
            if (!strcmp(a[i], "--description") && i + 1 < n) {
                desc = arg_val(a[i + 1]);
                if (!desc) goto usage_out;
                i++;
            } else
                goto usage_out;
        }
        if (load_rel(file, FV_ACTIONS, &doc)) goto out;
        /* duplicate title guard across existing chapters */
        {
            char *dup = NULL;
            for (i = 0; i < doc.nch && !dup; i++) {
                char *t = action_title_of(&doc.ch[i]);
                if (t && !strcmp(t, title)) dup = doc.ch[i].title;
                free(t);
            }
            if (dup) {
                fprintf(stderr, "flower: duplicate action title '%s' "
                                "(%s)\n",
                        title, dup);
                md_free_doc(&doc);
                goto out;
            }
        }
        id = store_next_action_id(&doc);
        if (!id) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        {
            char *hdr = xasprintf("%s: %s", id, title);
            c = hdr ? doc_add_chapter(&doc, hdr, 1) : NULL;
            free(hdr);
        }
        if (!c) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        if (desc && md_kv_set(c, "Description", desc) != 0) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        if (md_kv_set(c, "Status", "pending") != 0) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

    if (!strcmp(a[0], "list")) {
        const char *want = NULL;
        int i;
        if (n > 2) goto usage_out;
        if (n == 2) {
            if (!strcmp(a[1], "--pending")) want = "pending";
            else if (!strcmp(a[1], "--complete")) want = "complete";
            else goto usage_out;
        }
        if (load_rel(file, FV_ACTIONS, &doc)) goto out;
        for (i = 0; i < doc.nch; i++) {
            char *id, *title;
            const char *status = md_kv_get((const char **)doc.ch[i].keys,
                                           (const char **)doc.ch[i].vals,
                                           doc.ch[i].nkv, "Status");
            if (!action_split(doc.ch[i].title, &id, &title)) continue;
            if (want && status && strcmp(status, want)) {
                free(id);
                free(title);
                continue;
            }
            printf("%s: %s [%s]\n", id, title, status ? status : "?");
            free(id);
            free(title);
        }
        md_free_doc(&doc);
        rc = ferror(stdout) ? 1 : 0;
        goto out;
    }

    /* remaining verbs address one action id */
    if (!store_valid_id(a[0], 'A')) {
        fprintf(stderr, "flower: invalid action id '%s'\n", a[0]);
        goto out;
    }
    if (n < 2) goto usage_out;

    if (!strcmp(a[1], "set") && n == 4) {
        char *key = arg_val(a[2]);
        char *val = arg_val(a[3]);
        md_chapter_t *c;
        if (!key || !val) goto usage_out;
        if (!key_allowed(store_action_keys, key)) {
            fprintf(stderr, "flower: unknown key '%s'\n", key);
            goto out;
        }
        if (!strcmp(key, "Status") && !store_valid_status(val)) {
            fprintf(stderr,
                    "flower: invalid Status '%s' (pending, complete)\n",
                    val);
            goto out;
        }
        if (load_rel(file, FV_ACTIONS, &doc)) goto out;
        c = action_find(&doc, a[0]);
        if (!c) {
            fprintf(stderr, "flower: no such action %s\n", a[0]);
            md_free_doc(&doc);
            goto out;
        }
        if (!strcmp(key, "Depends")) {
            char **ids = NULL;
            int cnt, j;
            cnt = store_parse_id_list(val, &ids);
            if (cnt < 0) {
                md_free_doc(&doc);
                rc = oom();
                goto out;
            }
            for (j = 0; j < cnt; j++) {
                if (!strcmp(ids[j], a[0])) {
                    fprintf(stderr, "flower: action %s cannot depend on "
                                    "itself\n",
                            a[0]);
                    store_free_ids(ids, cnt);
                    md_free_doc(&doc);
                    goto out;
                }
                if (!action_find(&doc, ids[j])) {
                    fprintf(stderr, "flower: unknown action %s\n", ids[j]);
                    store_free_ids(ids, cnt);
                    md_free_doc(&doc);
                    goto out;
                }
            }
            store_free_ids(ids, cnt);
        }
        if (md_kv_set(c, key, val) != 0) {
            md_free_doc(&doc);
            rc = oom();
            goto out;
        }
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

    if (!strcmp(a[1], "remove") && n == 2) {
        md_chapter_t *c;
        char *dep;
        if (load_rel(file, FV_ACTIONS, &doc)) goto out;
        c = action_find(&doc, a[0]);
        if (!c) {
            fprintf(stderr, "flower: no such action %s\n", a[0]);
            md_free_doc(&doc);
            goto out;
        }
        dep = action_depended_on(&doc, a[0], c);
        if (dep) {
            char *id, *title;
            if (action_split(dep, &id, &title)) {
                fprintf(stderr, "flower: action %s depends on %s; remove "
                                "or edit dependents first\n",
                        id, a[0]);
                free(id);
                free(title);
            } else {
                fprintf(stderr, "flower: action %s depends on %s; remove "
                                "or edit dependents first\n",
                        dep, a[0]);
            }
            md_free_doc(&doc);
            goto out;
        }
        doc_remove_chapter(&doc, (int)(c - doc.ch));
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        goto out;
    }

usage_out:
    rc = usage(stderr);
out:
    free(file);
    return rc;
}

/* ---------- llm profiles (llms.md) ---------- */

/* argv[1]="llm". verbs: add, list, or "<name> set|unset|remove|show".
   add scans its flags by hand like cmd_action's --description loop
   because Header/Mcp proxy/Terminal tool repeat and cli_parse keeps
   only the last value of an option. */
static int cmd_llm(int argc, char **argv) {
    const char *file = "llms.md";
    const char *verb;
    md_doc_t doc;
    md_chapter_t *c;
    int rc;

    if (argc < 3) return usage(stderr);
    verb = argv[2];

    if (!strcmp(verb, "list")) {
        int i;
        if (argc != 3) return usage(stderr);
        if (load_rel(file, FV_LLM, &doc)) return 1;
        for (i = 0; i < doc.nch; i++) printf("%s\n", doc.ch[i].title);
        md_free_doc(&doc);
        return ferror(stdout) ? 1 : 0;
    }

    if (!strcmp(verb, "add")) {
        char *name;
        char *base = NULL, *proto = NULL, *apikey = NULL;
        char *model = NULL, *maxtok = NULL, *reason = NULL;
        struct {
            const char *key; /* store key of a repeatable flag */
            char *val;
        } rep[64];
        int nrep = 0;
        int i;

        if (argc < 4) return usage(stderr);
        name = arg_val(argv[3]);
        if (!name) return usage(stderr);
        for (i = 4; i < argc; i++) {
            const char *rkey = NULL;
            char **slot = NULL;
            char *v;
            if (i + 1 >= argc) return usage(stderr);
            if (!strcmp(argv[i], "--base"))
                slot = &base;
            else if (!strcmp(argv[i], "--protocol"))
                slot = &proto;
            else if (!strcmp(argv[i], "--key"))
                slot = &apikey;
            else if (!strcmp(argv[i], "--model"))
                slot = &model;
            else if (!strcmp(argv[i], "--max-tokens"))
                slot = &maxtok;
            else if (!strcmp(argv[i], "--reasoning-effort"))
                slot = &reason;
            else if (!strcmp(argv[i], "--header"))
                rkey = "Header";
            else if (!strcmp(argv[i], "--mcp-proxy"))
                rkey = "Mcp proxy";
            else if (!strcmp(argv[i], "--terminal-tool"))
                rkey = "Terminal tool";
            else
                return usage(stderr);
            v = arg_val(argv[i + 1]);
            if (!v) return usage(stderr);
            if (slot) {
                *slot = v;
            } else if (nrep < (int)(sizeof rep / sizeof rep[0])) {
                rep[nrep].key = rkey;
                rep[nrep].val = v;
                nrep++;
            } else {
                fprintf(stderr, "flower: too many repeatable llm "
                                "options\n");
                return 1;
            }
            i++;
        }
        if (!store_valid_llm_name(name)) {
            fprintf(stderr, "flower: invalid profile name '%s'\n", name);
            return 1;
        }
        if (!base || !base[0]) {
            fprintf(stderr, "flower: --base <url> is required\n");
            return 1;
        }
        if (!proto || !store_valid_llm_protocol(proto)) {
            fprintf(stderr, "flower: --protocol must be anthropic, "
                            "openai or openai-responses\n");
            return 1;
        }
        if (maxtok && !llm_digits(maxtok)) {
            fprintf(stderr, "flower: invalid --max-tokens '%s'\n", maxtok);
            return 1;
        }
        if (load_rel(file, FV_LLM, &doc)) return 1;
        if (md_find(&doc, name)) {
            fprintf(stderr, "flower: duplicate profile '%s'\n", name);
            md_free_doc(&doc);
            return 1;
        }
        c = doc_add_chapter(&doc, name, 1);
        if (!c) {
            md_free_doc(&doc);
            return oom();
        }
        if (md_kv_set(c, "Api base", base) != 0 ||
            md_kv_set(c, "Protocol", proto) != 0 ||
            (apikey && md_kv_set(c, "Api key", apikey) != 0) ||
            (model && md_kv_set(c, "Model", model) != 0) ||
            (maxtok && md_kv_set(c, "Max tokens", maxtok) != 0) ||
            (reason && md_kv_set(c, "Reasoning effort", reason) != 0)) {
            md_free_doc(&doc);
            return oom();
        }
        for (i = 0; i < nrep; i++)
            if (doc_append_kv(c, rep[i].key, rep[i].val) != 0) {
                md_free_doc(&doc);
                return oom();
            }
        if (store_mkdir_p(NULL) != 0) {
            md_free_doc(&doc);
            return 1;
        }
        rc = save_rel(NULL, file, &doc);
        md_free_doc(&doc);
        return rc;
    }

    /* remaining verbs address one profile */
    {
        const char *name = argv[2];
        const char *v = argv[3];

        if (argc < 4) return usage(stderr);
        if (!store_valid_llm_name(name)) {
            fprintf(stderr, "flower: invalid profile name '%s'\n", name);
            return 1;
        }

        if (!strcmp(v, "set") && argc == 6) {
            char *key = arg_val(argv[4]);
            char *val = arg_val(argv[5]);
            if (!key || !val) return usage(stderr);
            if (!key_allowed(store_llm_keys, key)) {
                fprintf(stderr, "flower: unknown key '%s'\n", key);
                return 1;
            }
            if (!strcmp(key, "Api base") && !val[0]) {
                fprintf(stderr, "flower: empty Api base\n");
                return 1;
            }
            if (!strcmp(key, "Protocol") && !store_valid_llm_protocol(val)) {
                fprintf(stderr, "flower: invalid Protocol '%s'\n", val);
                return 1;
            }
            if (!strcmp(key, "Max tokens") && !llm_digits(val)) {
                fprintf(stderr, "flower: invalid Max tokens '%s'\n", val);
                return 1;
            }
            if (load_rel(file, FV_LLM, &doc)) return 1;
            c = md_find(&doc, name);
            if (!c) {
                fprintf(stderr, "flower: no such profile '%s'\n", name);
                md_free_doc(&doc);
                return 1;
            }
            if (!strcmp(key, "Header") || !strcmp(key, "Mcp proxy") ||
                !strcmp(key, "Terminal tool")) {
                rc = doc_append_kv(c, key, val) != 0 ? oom() : 0;
            } else {
                rc = md_kv_set(c, key, val) != 0 ? oom() : 0;
            }
            if (rc != 0) {
                md_free_doc(&doc);
                return rc;
            }
            rc = save_rel(NULL, file, &doc);
            md_free_doc(&doc);
            return rc;
        }

        if (!strcmp(v, "unset") && argc == 5) {
            char *key = arg_val(argv[4]);
            int j, kept = 0, found = 0;
            if (!key) return usage(stderr);
            if (!key_allowed(store_llm_keys, key)) {
                fprintf(stderr, "flower: unknown key '%s'\n", key);
                return 1;
            }
            /* Api base and Protocol are required, dropping either
               would leave an invalid file */
            if (!strcmp(key, "Api base") || !strcmp(key, "Protocol")) {
                fprintf(stderr, "flower: %s cannot be unset\n", key);
                return 1;
            }
            if (load_rel(file, FV_LLM, &doc)) return 1;
            c = md_find(&doc, name);
            if (!c) {
                fprintf(stderr, "flower: no such profile '%s'\n", name);
                md_free_doc(&doc);
                return 1;
            }
            for (j = 0; j < c->nkv; j++) {
                if (!strcmp(c->keys[j], key)) {
                    free(c->keys[j]);
                    free(c->vals[j]);
                    found = 1;
                    continue;
                }
                c->keys[kept] = c->keys[j];
                c->vals[kept] = c->vals[j];
                kept++;
            }
            if (!found) {
                fprintf(stderr, "flower: no such key '%s' in profile "
                                "'%s'\n",
                        key, name);
                md_free_doc(&doc);
                return 1;
            }
            c->nkv = kept;
            rc = save_rel(NULL, file, &doc);
            md_free_doc(&doc);
            return rc;
        }

        if (!strcmp(v, "remove") && argc == 4) {
            if (load_rel(file, FV_LLM, &doc)) return 1;
            c = md_find(&doc, name);
            if (!c) {
                fprintf(stderr, "flower: no such profile '%s'\n", name);
                md_free_doc(&doc);
                return 1;
            }
            doc_remove_chapter(&doc, (int)(c - doc.ch));
            rc = save_rel(NULL, file, &doc);
            md_free_doc(&doc);
            return rc;
        }

        if (!strcmp(v, "show") && argc == 4) {
            int i;
            if (load_rel(file, FV_LLM, &doc)) return 1;
            c = md_find(&doc, name);
            if (!c) {
                fprintf(stderr, "flower: no such profile '%s'\n", name);
                md_free_doc(&doc);
                return 1;
            }
            for (i = 0; i < c->nkv; i++)
                printf("%s: %s\n", c->keys[i], c->vals[i]);
            for (i = 0; i < c->nbody; i++) printf("%s\n", c->body[i].text);
            md_free_doc(&doc);
            return ferror(stdout) ? 1 : 0;
        }
    }

    return usage(stderr);
}

/* ---------- recursive delete ---------- */

/* rm -rf by opendir recursion, no system(). 0 ok, -1 io error. */
static int rm_rf(const char *path) {
    DIR *d;
    struct dirent *e;
    struct stat st;
    int rc = 0;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "flower: cannot stat %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0) {
            fprintf(stderr, "flower: cannot remove %s: %s\n", path,
                    strerror(errno));
            return -1;
        }
        return 0;
    }
    d = opendir(path);
    if (!d) {
        fprintf(stderr, "flower: cannot open %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    while (rc == 0 && (e = readdir(d)) != NULL) {
        char *child;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        child = xasprintf("%s/%s", path, e->d_name);
        if (!child) {
            rc = -1;
            continue;
        }
        rc = rm_rf(child);
        free(child);
    }
    closedir(d);
    if (rc == 0 && rmdir(path) != 0) {
        fprintf(stderr, "flower: cannot remove %s: %s\n", path,
                strerror(errno));
        rc = -1;
    }
    return rc;
}

/* prompt + rm_rf for a directory under the base */
static int cmd_remove_tree(const char *rel) {
    char *path = store_path(rel);
    struct stat st;
    char line[32];
    if (!path) return oom();
    if (stat(path, &st) != 0) {
        fprintf(stderr, "flower: no such %s\n", path);
        free(path);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "flower: not a directory: %s\n", path);
        free(path);
        return 1;
    }
    printf("remove %s? [y/N] ", path);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin) ||
        (line[0] != 'y' && line[0] != 'Y')) {
        printf("aborted\n");
        free(path);
        return 1;
    }
    if (rm_rf(path) != 0) {
        free(path);
        return 1;
    }
    print_abs(path);
    free(path);
    return 0;
}

/* ---------- edit escape hatch ---------- */

/* file kind by relative path under base, -1 when unrecognized */
static int kind_for_rel(const char *rel) {
    if (!strcmp(rel, "flower.md")) return FV_USER;
    if (!strcmp(rel, "llms.md")) return FV_LLM;
    if (str_starts(rel, "projects/")) {
        const char *p = rel + strlen("projects/");
        const char *slash = strchr(p, '/');
        const char *rest;
        char idbuf[32];
        size_t idlen;
        if (!slash) return -1;
        idlen = (size_t)(slash - p);
        if (idlen == 0 || idlen >= sizeof idbuf) return -1;
        memcpy(idbuf, p, idlen);
        idbuf[idlen] = '\0';
        if (!store_valid_id(idbuf, 'P')) return -1;
        rest = slash + 1;
        if (!strcmp(rest, "config.md")) return FV_PROJECT;
        if (!strcmp(rest, "details.md")) return FV_FIELDS;
        if (!strcmp(rest, "context.md")) return FV_CONTEXT;
        if (!strcmp(rest, "resources.md")) return FV_RESOURCE;
        if (str_starts(rest, "tasks/")) {
            const char *tp = rest + strlen("tasks/");
            const char *tslash = strchr(tp, '/');
            size_t tlen;
            if (!tslash) return -1;
            tlen = (size_t)(tslash - tp);
            if (tlen == 0 || tlen >= sizeof idbuf) return -1;
            memcpy(idbuf, tp, tlen);
            idbuf[tlen] = '\0';
            if (!store_valid_id(idbuf, 'T')) return -1;
            if (!strcmp(tslash + 1, "config.md")) return FV_TASK;
            if (!strcmp(tslash + 1, "context.md")) return FV_CONTEXT;
            if (!strcmp(tslash + 1, "actions.md")) return FV_ACTIONS;
        }
    }
    return -1;
}

static int cmd_edit(char *rel) {
    const char *base = store_base();
    const char *editor;
    char *path = NULL;
    char *tmp = NULL;
    char reason[512];
    char line[4096];
    size_t n;
    FILE *f;
    md_doc_t doc;
    int kind;
    int rc = 1;
    pid_t pid;
    int st;

    if (!base) {
        fprintf(stderr, "flower: FLOWER_BASE not resolvable\n");
        return 1;
    }
    if (rel[0] == '\0') {
        fprintf(stderr, "flower: empty path\n");
        return 1;
    }
    if (rel[0] == '/' || strstr(rel, "..")) {
        fprintf(stderr,
                "flower: path must stay inside the config base: %s\n",
                rel);
        return 1;
    }
    kind = kind_for_rel(rel);
    if (kind < 0) {
        fprintf(stderr, "flower: not a known config file: %s\n", rel);
        return 1;
    }
    path = store_path(rel);
    if (!path) return oom();

    /* copy the original (empty when missing) into the temp file */
    tmp = xasprintf("%s.tmp", path);
    if (!tmp) {
        rc = oom();
        goto out;
    }
    f = fopen(path, "rb");
    if (f) {
        size_t got;
        FILE *o = fopen(tmp, "wb");
        if (!o) {
            fprintf(stderr, "flower: cannot write %s: %s\n", tmp,
                    strerror(errno));
            fclose(f);
            goto out;
        }
        while ((got = fread(line, 1, sizeof line, f)) > 0)
            if (fwrite(line, 1, got, o) != got) {
                fprintf(stderr, "flower: write error on %s\n", tmp);
                fclose(f);
                fclose(o);
                goto out;
            }
        if (ferror(f)) {
            fprintf(stderr, "flower: read error on %s\n", path);
            fclose(f);
            fclose(o);
            goto out;
        }
        fclose(f);
        if (fclose(o) != 0) {
            fprintf(stderr, "flower: write error on %s\n", tmp);
            goto out;
        }
    } else {
        FILE *o = fopen(tmp, "wb");
        if (!o) {
            fprintf(stderr, "flower: cannot write %s: %s\n", tmp,
                    strerror(errno));
            goto out;
        }
        fclose(o);
    }

    editor = getenv("EDITOR");
    if (!editor || !editor[0]) editor = "vi";
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "flower: fork: %s\n", strerror(errno));
        goto out;
    }
    if (pid == 0) {
        execlp(editor, editor, tmp, (char *)NULL);
        fprintf(stderr, "flower: cannot run %s: %s\n", editor,
                strerror(errno));
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0) {
        fprintf(stderr, "flower: waitpid: %s\n", strerror(errno));
        goto out;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "flower: editor %s failed, %s unchanged\n",
                editor, path);
        goto out;
    }

    /* validate the edited temp file */
    memset(&doc, 0, sizeof doc);
    f = fopen(tmp, "rb");
    if (!f) {
        fprintf(stderr, "flower: cannot read %s: %s\n", tmp,
                strerror(errno));
        goto out;
    }
    {
        buf_t b;
        int prc;
        buf_init(&b);
        while ((n = fread(line, 1, sizeof line, f)) > 0)
            if (buf_append(&b, line, n)) {
                fclose(f);
                buf_free(&b);
                rc = oom();
                goto out;
            }
        if (ferror(f)) {
            fprintf(stderr, "flower: read error on %s\n", tmp);
            fclose(f);
            buf_free(&b);
            goto out;
        }
        fclose(f);
        prc = md_parse(b.data ? b.data : "", &doc);
        buf_free(&b);
        if (prc != 0) {
            snprintf(reason, sizeof reason, "%s",
                     prc == -2 ? "duplicate chapter title"
                               : prc == -3 ? "malformed structure"
                                           : "parse error");
            fprintf(stderr, "flower: invalid: %s (kept %s)\n", reason,
                    tmp);
            goto out;
        }
    }
    if (doc_check(kind, &doc, reason, sizeof reason) != 0) {
        fprintf(stderr, "flower: invalid: %s (kept %s)\n", reason, tmp);
        md_free_doc(&doc);
        goto out;
    }
    md_free_doc(&doc);

    /* atomic replace */
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "flower: cannot save %s: %s\n", path,
                strerror(errno));
        goto out;
    }
    free(tmp);
    tmp = NULL;
    print_abs(path);
    rc = 0;
out:
    free(tmp);
    free(path);
    return rc;
}

/* ---------- project dispatch ---------- */

static int cmd_project(int argc, char **argv) {
    char *detected = NULL;
    char **a;
    const char *pid, *area;
    int n, explicit_pid;
    if (argc < 3) return usage(stderr);
    explicit_pid = store_valid_id(argv[2], 'P');
    if (explicit_pid) {
        pid = argv[2];
    } else {
        detected = store_detect_project();
        if (!detected) {
            if (argc >= 4) {
                /* explicit but malformed id (e.g. p1, X1): keep the
                   old error instead of blaming the directory */
                fprintf(stderr, "flower: invalid project id '%s'\n",
                        argv[2]);
                return 1;
            }
            fprintf(stderr, "flower: no project found for this directory "
                            "(set project Path or pass <P>)\n");
            return 1;
        }
        pid = detected;
    }
    a = argv + (explicit_pid ? 3 : 2);
    n = argc - (explicit_pid ? 3 : 2);
    if (n < 1) {
        free(detected);
        return usage(stderr);
    }
    area = a[0];

    if (!strcmp(area, "set")) {
        char *dir, *file, *key, *val;
        int rc;
        if (n != 3) {
            free(detected);
            return usage(stderr);
        }
        key = arg_val(a[1]);
        val = arg_val(a[2]);
        if (!key || !val) {
            free(detected);
            return usage(stderr);
        }
        dir = xasprintf("projects/%s", pid);
        file = xasprintf("projects/%s/config.md", pid);
        if (!dir || !file) {
            free(dir);
            free(file);
            free(detected);
            return oom();
        }
        rc = cmd_config_set(FV_PROJECT, dir, file, key, val);
        free(dir);
        free(file);
        free(detected);
        return rc;
    }

    if (!strcmp(area, "show")) {
        char *file;
        int rc;
        if (n != 1) {
            free(detected);
            return usage(stderr);
        }
        if (!require_project(pid)) {
            free(detected);
            return 1;
        }
        file = xasprintf("projects/%s/config.md", pid);
        if (!file) {
            free(detected);
            return oom();
        }
        rc = print_rel_file(file);
        free(file);
        free(detected);
        return rc;
    }

    if (!strcmp(area, "details")) {
        int rc;
        if (n < 2) {
            free(detected);
            return usage(stderr);
        }
        if (!require_project(pid)) {
            free(detected);
            return 1;
        }
        rc = cmd_details(pid, &a[1], n - 1);
        free(detected);
        return rc;
    }

    if (!strcmp(area, "context")) {
        char *file;
        int rc;
        if (n < 2) {
            free(detected);
            return usage(stderr);
        }
        if (!require_project(pid)) {
            free(detected);
            return 1;
        }
        file = xasprintf("projects/%s/context.md", pid);
        if (!file) {
            free(detected);
            return oom();
        }
        rc = cmd_context(file, &a[1], n - 1);
        free(file);
        free(detected);
        return rc;
    }

    if (!strcmp(area, "resource")) {
        int rc;
        if (n < 2) {
            free(detected);
            return usage(stderr);
        }
        if (!require_project(pid)) {
            free(detected);
            return 1;
        }
        rc = cmd_resource(pid, &a[1], n - 1);
        free(detected);
        return rc;
    }

    if (!strcmp(area, "task")) {
        const char *tid;
        int rc;
        if (n < 3) {
            free(detected);
            return usage(stderr);
        }
        tid = a[1];
        if (!store_valid_id(tid, 'T')) {
            fprintf(stderr, "flower: invalid task id '%s'\n", tid);
            free(detected);
            return 1;
        }
        if (!strcmp(a[2], "set") && n == 5) {
            char *dir, *file, *key, *val;
            key = arg_val(a[3]);
            val = arg_val(a[4]);
            if (!key || !val) {
                free(detected);
                return usage(stderr);
            }
            dir = xasprintf("projects/%s/tasks/%s", pid, tid);
            file = xasprintf("projects/%s/tasks/%s/config.md", pid, tid);
            if (!dir || !file) {
                free(dir);
                free(file);
                free(detected);
                return oom();
            }
            rc = cmd_config_set(FV_TASK, dir, file, key, val);
            free(dir);
            free(file);
            free(detected);
            return rc;
        }
        if (!strcmp(a[2], "show") && n == 3) {
            char *file;
            if (!require_task(pid, tid)) {
                free(detected);
                return 1;
            }
            file = xasprintf("projects/%s/tasks/%s/config.md", pid, tid);
            if (!file) {
                free(detected);
                return oom();
            }
            rc = print_rel_file(file);
            free(file);
            free(detected);
            return rc;
        }
        if (!strcmp(a[2], "context")) {
            char *file;
            if (n < 4) {
                free(detected);
                return usage(stderr);
            }
            if (!require_task(pid, tid)) {
                free(detected);
                return 1;
            }
            file = xasprintf("projects/%s/tasks/%s/context.md", pid, tid);
            if (!file) {
                free(detected);
                return oom();
            }
            rc = cmd_context(file, &a[3], n - 3);
            free(file);
            free(detected);
            return rc;
        }
        if (!strcmp(a[2], "action")) {
            char *file;
            const char *verb = a[3];
            /* add and list need an existing task dir: add creates it */
            if (n < 4) {
                free(detected);
                return usage(stderr);
            }
            if (!strcmp(verb, "add")) {
                char *dir = xasprintf("projects/%s/tasks/%s", pid, tid);
                if (!dir) {
                    free(detected);
                    return oom();
                }
                if (store_mkdir_p(dir) != 0) {
                    free(dir);
                    free(detected);
                    return 1;
                }
                free(dir);
            } else if (!require_task(pid, tid)) {
                free(detected);
                return 1;
            }
            file =
                xasprintf("projects/%s/tasks/%s/actions.md", pid, tid);
            if (!file) {
                free(detected);
                return oom();
            }
            rc = cmd_action(pid, tid, &a[3], n - 3);
            free(file);
            free(detected);
            return rc;
        }
        if (!strcmp(a[2], "remove") && n == 3) {
            char *rel;
            if (!require_task(pid, tid)) {
                free(detected);
                return 1;
            }
            rel = xasprintf("projects/%s/tasks/%s", pid, tid);
            if (!rel) {
                free(detected);
                return oom();
            }
            rc = cmd_remove_tree(rel);
            free(rel);
            free(detected);
            return rc;
        }
        free(detected);
        return usage(stderr);
    }

    if (!strcmp(area, "scan") && (n == 2 ||
                                   (n == 3 &&
                                    !strcmp(a[2], "--dry-run")))) {
        int rc = cmd_scan(pid, a[1], n == 3, argv[0]);
        free(detected);
        return rc;
    }

    if (!strcmp(area, "remove") && n == 1) {
        char *rel;
        int rc;
        if (!require_project(pid)) {
            free(detected);
            return 1;
        }
        rel = xasprintf("projects/%s", pid);
        if (!rel) {
            free(detected);
            return oom();
        }
        rc = cmd_remove_tree(rel);
        free(rel);
        free(detected);
        return rc;
    }

    free(detected);
    return usage(stderr);
}

int flower_main(int argc, char **argv) {
    const char *cmd = argv[1];
    (void)argc;
    if (!strcmp(cmd, "setup")) {
        if (argc != 2) return usage(stderr);
        return cmd_setup();
    }
    if (!strcmp(cmd, "project")) return cmd_project(argc, argv);
    if (!strcmp(cmd, "llm")) return cmd_llm(argc, argv);
    if (!strcmp(cmd, "edit")) {
        if (argc != 3) return usage(stderr);
        return cmd_edit(argv[2]);
    }
    fprintf(stderr, "flower: unknown command '%s'\n", cmd);
    return usage(stderr);
}
