/* store.c - config directory layout: base dir resolution, path
   building, id allocation, schema tables. layout: docs/config_layout.md */
/* mkdir/opendir/setenv are posix and realpath is xsi, hidden by
   -std=c11 without this */
#define _XOPEN_SOURCE 700
#include "flower.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

const char *store_base(void) {
    const char *base = getenv("FLOWER_BASE");
    if (base && base[0]) return base;
    base = getenv("XDG_CONFIG_HOME");
    if (base && base[0]) {
        static char xdg[4096];
        if (snprintf(xdg, sizeof xdg, "%s/flower", base) < (int)sizeof xdg)
            return xdg;
    }
    base = getenv("HOME");
    if (!base || !base[0]) return NULL;
    {
        static char home[4096];
        if (snprintf(home, sizeof home, "%s/.config/flower", base) >=
            (int)sizeof home)
            return NULL;
        return home;
    }
}

char *store_path(const char *rel) {
    const char *base = store_base();
    size_t blen, rlen;
    char *path;
    if (!base) return NULL;
    blen = strlen(base);
    rlen = rel ? strlen(rel) : 0;
    /* rel == NULL yields base itself with no trailing slash */
    path = malloc(blen + rlen + (rel ? 2 : 1));
    if (!path) return NULL;
    memcpy(path, base, blen);
    if (rel) {
        path[blen] = '/';
        memcpy(path + blen + 1, rel, rlen);
    }
    path[blen + rlen + (rel ? 1 : 0)] = '\0';
    return path;
}

/* mkdir each component of the slash-separated rel_dir path */
int store_mkdir_p(const char *rel_dir) {
    char *path = store_path(rel_dir);
    char *p;
    if (!path) {
        fprintf(stderr, "flower: out of memory\n");
        return -1;
    }
    for (p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (mkdir(path, 0777) != 0 && errno != EEXIST) {
            fprintf(stderr, "flower: cannot create %s: %s\n", path,
                    strerror(errno));
            free(path);
            return -1;
        }
        *p = '/';
    }
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "flower: cannot create %s: %s\n", path,
                strerror(errno));
        free(path);
        return -1;
    }
    free(path);
    return 0;
}

int store_valid_id(const char *id, char kind) {
    char *end;
    long n;
    if (!id || id[0] != kind) return 0;
    if (id[1] < '0' || id[1] > '9') return 0; /* strtol skips spaces/sign */
    errno = 0;
    n = strtol(id + 1, &end, 10);
    if (errno != 0 || *end != '\0') return 0;
    return n > 0;
}

/* largest id number found in dir among entries "<kind><n>...", or 0 */
static long scan_ids(const char *dir, char kind) {
    DIR *d;
    struct dirent *e;
    long max = 0;
    d = opendir(dir);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        char *end;
        long n;
        if (e->d_name[0] != kind) continue;
        errno = 0;
        n = strtol(e->d_name + 1, &end, 10);
        if (errno != 0 || end == e->d_name + 1 || *end != '\0') continue;
        if (n > max) max = n;
    }
    closedir(d);
    return max;
}

/* malloc'd "<kind><n>" with n >= 1, NULL on oom */
static char *make_id(char kind, long n) {
    char *id = malloc(24);
    if (!id) return NULL;
    snprintf(id, 24, "%c%ld", kind, n);
    return id;
}

/* next free id scanning rel_dir under base */
static char *next_dir_id(const char *rel_dir, char kind) {
    char *dir = store_path(rel_dir);
    long max;
    if (!dir) return NULL;
    max = scan_ids(dir, kind);
    free(dir);
    return make_id(kind, max + 1);
}

char *store_next_project_id(void) { return next_dir_id("projects", 'P'); }

/* qsort helper: project ids by numeric part ascending */
static int cmp_proj_id(const void *x, const void *y) {
    const char *a = *(const char *const *)x;
    const char *b = *(const char *const *)y;
    long na = strtol(a + 1, NULL, 10);
    long nb = strtol(b + 1, NULL, 10);
    if (na < nb) return -1;
    if (na > nb) return 1;
    return 0;
}

/* does cwd match path? path with trailing '/' stripped: equal, or cwd
   starts with path + "/" */
static int path_contains(const char *cwd, const char *path) {
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') n--;
    if (n == 0) return 0;
    if (strncmp(cwd, path, n) != 0) return 0;
    return cwd[n] == '\0' || cwd[n] == '/';
}

/* malloc'd project id detected from cwd via Path keys, NULL when none */
char *store_detect_project(void) {
    char cwd[4096];
    char *dir, *rel, *path, **ids = NULL, *hit = NULL;
    DIR *d;
    struct dirent *e;
    int n = 0;
    if (!realpath(".", cwd)) return NULL;
    dir = store_path("projects");
    if (!dir) return NULL;
    d = opendir(dir);
    free(dir);
    if (!d) return NULL;
    while ((e = readdir(d)) != NULL) {
        char **grown;
        if (!store_valid_id(e->d_name, 'P')) continue;
        grown = realloc(ids, (size_t)(n + 1) * sizeof *ids);
        if (!grown) break; /* oom: keep what we have */
        ids = grown;
        ids[n] = str_dup(e->d_name);
        if (!ids[n]) break;
        n++;
    }
    closedir(d);
    qsort(ids, (size_t)n, sizeof *ids, cmp_proj_id);
    for (int i = 0; i < n; i++) {
        md_doc_t doc;
        md_chapter_t *ch;
        const char *val;
        rel = xasprintf("projects/%s/config.md", ids[i]);
        if (!rel) continue;
        path = store_path(rel);
        free(rel);
        if (!path) continue;
        if (md_load(path, &doc) != 0) {
            free(path);
            continue;
        }
        free(path);
        ch = md_find(&doc, "Project");
        val = ch ? md_kv_get((const char **)ch->keys,
                             (const char **)ch->vals, ch->nkv, "Path")
                 : NULL;
        if (val && val[0] && path_contains(cwd, val)) {
            hit = str_dup(ids[i]);
            md_free_doc(&doc);
            break;
        }
        md_free_doc(&doc);
    }
    for (int i = 0; i < n; i++) free(ids[i]);
    free(ids);
    return hit;
}

char *store_next_task_id(const char *pid) {
    size_t n = strlen(pid) + 64;
    char *rel = malloc(n);
    char *id;
    if (!rel) return NULL;
    snprintf(rel, n, "projects/%s/tasks", pid);
    id = next_dir_id(rel, 'T');
    free(rel);
    return id;
}

char *store_next_action_id(const md_doc_t *actions_doc) {
    long max = 0;
    int i;
    for (i = 0; actions_doc && i < actions_doc->nch; i++) {
        const char *title = actions_doc->ch[i].title;
        char *end;
        long n;
        if (!title || title[0] != 'A') continue;
        errno = 0;
        n = strtol(title + 1, &end, 10);
        if (errno != 0 || end == title + 1 || end[0] != ':' || end[1] != ' ')
            continue;
        if (n > max) max = n;
    }
    return make_id('A', max + 1);
}

/* allowed keys for config.md chapters. NULL-terminated lists. */
const char *const store_user_keys[] = {"Name", "Role", NULL};
const char *const store_project_keys[] = {"Name", "Path", "Description", NULL};
const char *const store_task_keys[] = {"Name", "Description", NULL};
const char *const store_action_keys[] = {"Description", "Status", "Depends",
                                         NULL};
const char *const store_detail_fields[] = {"Output", "Objectives", "Scope",
                                           "Constraints", "Stakeholders",
                                           NULL};
const char *const store_context_prefixes[] = {
    "Fact",       "Pattern",  "Risk",
    "Success metric", "Failure sign", "Evaluation method",
    "Rule",       NULL};
/* keys for llms.md profile chapters. Header, Mcp proxy and Terminal
   tool may repeat inside one chapter, the rest are single-valued. */
const char *const store_llm_keys[] = {"Api base", "Api key", "Protocol",
                                      "Model", "Max tokens",
                                      "Reasoning effort", "Header",
                                      "Mcp proxy", "Terminal tool", NULL};
/* protocols inherited from llmkit */
const char *const store_llm_protocols[] = {"anthropic", "openai",
                                           "openai-responses", NULL};

/* longest prefix from store_context_prefixes that s starts with
   followed by ": ", NULL when none */
static const char *match_prefix(const char *s) {
    const char *const *p;
    const char *found = NULL;
    size_t foundlen = 0;
    if (!s) return NULL;
    for (p = store_context_prefixes; *p; p++) {
        size_t n = strlen(*p);
        if (strncmp(s, *p, n) == 0 && s[n] == ':' && s[n + 1] == ' ' &&
            n > foundlen) {
            found = *p;
            foundlen = n;
        }
    }
    return found;
}

int store_is_context_title(const char *chapter_title) {
    return match_prefix(chapter_title) != NULL;
}

int store_split_context(const char *chapter_title, char **prefix,
                        char **title) {
    const char *p;
    *prefix = NULL;
    *title = NULL;
    p = match_prefix(chapter_title);
    if (!p) return 0;
    *prefix = str_dup(p);
    *title = str_dup(chapter_title + strlen(p) + 2);
    if (!*prefix || !*title) {
        free(*prefix);
        free(*title);
        *prefix = NULL;
        *title = NULL;
        return 0;
    }
    return 1;
}

char *store_make_context_title(const char *prefix, const char *title) {
    size_t plen = strlen(prefix), tlen = strlen(title);
    char *out = malloc(plen + 2 + tlen + 1);
    if (!out) return NULL;
    memcpy(out, prefix, plen);
    out[plen] = ':';
    out[plen + 1] = ' ';
    memcpy(out + plen + 2, title, tlen);
    out[plen + 2 + tlen] = '\0';
    return out;
}

int store_valid_status(const char *status) {
    if (!status) return 0;
    return !strcmp(status, "pending") || !strcmp(status, "complete");
}

/* 1 when protocol value is one of store_llm_protocols */
int store_valid_llm_protocol(const char *protocol) {
    const char *const *p;
    if (!protocol) return 0;
    for (p = store_llm_protocols; *p; p++)
        if (!strcmp(*p, protocol)) return 1;
    return 0;
}

/* 1 when profile name is non-empty and matches the md key charset:
   [A-Za-z][A-Za-z0-9 _-]*. uniqueness is md_parse's duplicate-title
   check, not this. */
int store_valid_llm_name(const char *name) {
    size_t i;
    if (!name || name[0] == '\0') return 0;
    if (!((name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= 'a' && name[0] <= 'z')))
        return 0;
    for (i = 1; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int store_parse_id_list(const char *s, char ***out_ids) {
    const char *p = s;
    char **ids = NULL;
    int n = 0;
    *out_ids = NULL;
    for (;;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        const char *start = p;
        size_t end = len;
        while (end > 0 && (start[end - 1] == ' ' || start[end - 1] == '\t'))
            end--;
        while (start < p + end && (*start == ' ' || *start == '\t')) start++;
        if (end > 0) {
            char **g = realloc(ids, (size_t)(n + 1) * sizeof *g);
            char *id;
            size_t idlen = (size_t)(p + end - start);
            if (!g) goto oom;
            ids = g;
            id = malloc(idlen + 1);
            if (!id) goto oom;
            memcpy(id, start, idlen);
            id[idlen] = '\0';
            ids[n++] = id;
        }
        if (!comma) break;
        p = comma + 1;
    }
    *out_ids = ids;
    return n;

oom:
    while (n > 0) free(ids[--n]);
    free(ids);
    return -1;
}

/* free one store_parse_id_list result */
void store_free_ids(char **ids, int n) {
    int i;
    if (!ids) return;
    for (i = 0; i < n; i++) free(ids[i]);
    free(ids);
}
