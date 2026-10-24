/* scan.c - `flower project scan`: llm-guided project data collection. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"
#include "prompts.gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the readonly tools the scan conversation offers, through the
 * llmkit mcp-proxy whitelist config. */
static const char *const scan_readonly_tools[] = {
    "files_list", "files_search", "file_read", "file_analyze", NULL
};

/* resolve the flower binary path for spawning `flower mcp-scan`.
 * /proc/self/exe first (no PATH ambiguity), argv[0] fallback. */
static char *self_exe_path(void)
{
    char *buf = malloc(FLOWER_PATH_MAX);
    const char *env;
    ssize_t n;

    if (!buf)
        return NULL;
    n = readlink("/proc/self/exe", buf, FLOWER_PATH_MAX - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
    /* non-linux (macos): the launcher exports _ when a shell ran us */
    env = getenv("_");
    if (env && *env && env[0] == '/') {
        snprintf(buf, FLOWER_PATH_MAX, "%s", env);
        return buf;
    }
    free(buf);
    return NULL;
}

/* write projects/<id>/mcp-scan.jsonl: the mcp-proxy config exposing the
 * readonly builtin tools (whitelist). flower's path is found the same
 * way llm.c finds llmkit: $PATH scan. */
static int write_proxy_config(char *path_out, size_t pathsz,
                              const struct flower_project *p)
{
    char path[FLOWER_PATH_MAX];
    char body[4096];
    int off, i;

    flower_path_join(path, sizeof path, p->dir, "mcp-scan.jsonl");
    {
        const char *path_env = getenv("PATH");
        char try[FLOWER_PATH_MAX];
        int found = 0;
        const char *q;

        if (!path_env)
            return -1;
        for (q = path_env; ; ) {
            const char *colon = strchr(q, ':');
            size_t len = colon ? (size_t)(colon - q) : strlen(q);

            if (len > 0) {
                snprintf(try, sizeof(try), "%.*s/llmkit", (int)len, q);
                if (access(try, X_OK) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!colon)
                break;
            q = colon + 1;
        }
        if (!found)
            return -1;

        off = snprintf(body, sizeof(body),
                       "{\"type\":\"header\",\"version\":1}\n"
                       "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\","
                       "\"name\":\"builtin\",\"command_line\":\"%s "
                       "builtin-mcp\"}]}\n",
                       try);
        if (off < 0 || (size_t)off >= sizeof(body))
            return -1;
    }
    for (i = 0; scan_readonly_tools[i]; i++) {
        int n = snprintf(body + off, sizeof(body) - (size_t)off,
                         "{\"type\":\"expose\",\"tool\":\"builtin.%s\","
                         "\"name\":\"%s\"}\n",
                         scan_readonly_tools[i], scan_readonly_tools[i]);

        if (n < 0 || (size_t)n >= sizeof(body) - (size_t)off)
            return -1;
        off += n;
    }
    if (flower_write_file(path, body) != 0)
        return -1;
    snprintf(path_out, pathsz, "%s", path);
    return 0;
}

/* read a project data file for the current-data-set block. NULL-safe:
 * absent file -> "(not set)". */
static char *read_or_placeholder(const struct flower_project *p,
                                 const char *name, const char *ext)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL;

    flower_path_join3(path, sizeof path, p->dir, name, ext);
    if (flower_read_file(path, &text) != 0 || !text[0]) {
        free(text);
        return strdup("(not set)");
    }
    return text;
}

/* textual representation of resources.json: one "- [type] content" line
 * per entry, or "(not set)". */
static char *render_resources(const struct flower_project *p)
{
    char path[FLOWER_PATH_MAX];
    char *text = NULL, *out = NULL;
    cJSON *root, *arr, *it;
    size_t cap = 256, len = 0;

    flower_path_join3(path, sizeof path, p->dir, "resources", "json");
    if (flower_read_file(path, &text) != 0 || !text[0])
        return strdup("(not set)");
    root = cJSON_Parse(text);
    free(text);
    if (!root)
        return strdup("(not set)");
    arr = cJSON_IsArray(root)
        ? root
        : cJSON_GetObjectItemCaseSensitive(root, "resources");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return strdup("(not set)");
    }
    out = malloc(cap);
    if (!out) {
        cJSON_Delete(root);
        return NULL;
    }
    out[0] = '\0';
    cJSON_ArrayForEach(it, arr) {
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(it, "type");
        const cJSON *co = cJSON_GetObjectItemCaseSensitive(it, "content");
        const char *t = cJSON_IsString(ty) ? ty->valuestring : "?";
        const char *c = cJSON_IsString(co) ? co->valuestring : "?";
        const char *extra;

        /* a bare-string entry keeps the string alone */
        extra = cJSON_IsString(it) ? it->valuestring : NULL;
        while (len + strlen(extra ? extra : c) + strlen(t) + 8 > cap) {
            char *nb;

            cap *= 2;
            nb = realloc(out, cap);
            if (!nb) {
                free(out);
                cJSON_Delete(root);
                return NULL;
            }
            out = nb;
        }
        if (extra)
            len += (size_t)snprintf(out + len, cap - len, "- %s\n", extra);
        else
            len += (size_t)snprintf(out + len, cap - len, "- [%s] %s\n",
                                    t, c);
    }
    cJSON_Delete(root);
    if (!out[0]) {
        free(out);
        return strdup("(not set)");
    }
    return out;
}

/* print the project state after a scan, same shape as `flower project`. */
static void print_state(const struct flower_project *p)
{
    int ctx = project_count_context_items(p);
    int res = project_count_resources(p);
    int tasks = project_task_count(p);

    printf("project %s (%s)\npath %s\ncontext items %d\nresources %d\n"
           "tasks %d\n",
           p->id, p->name, p->path, ctx, res, tasks);
}

/* scan flow:
 * 1. resolve project for cwd. none yet -> project_create(cwd).
 * 2. write the mcp-proxy whitelist config (readonly builtin tools).
 * 3. seed: system prompt (scan instructions), user text (template from
 *    the scan design: context, current data set). The tools record
 *    carries the project server (update_project, terminal) and the
 *    readonly scan server. flower chdirs to the project directory so
 *    the readonly tools start from there, never the raw fs root.
 * 4. conversation via llm_converse: update_project writes the data and
 *    ends the scan (terminal exit 9). transcript under
 *    projects/<id>/sessions/scan-<pid>.jsonl. */
int scan_cmd(int argc, char **argv)
{
    struct flower_settings s;
    struct flower_project p;
    char transcript[FLOWER_PATH_MAX];
    char sessions_dir[FLOWER_PATH_MAX];
    char proxy_cfg[FLOWER_PATH_MAX];
    char *seed, *self_exe;

    (void)argc;
    (void)argv;
    if (flower_settings_load(&s) != 0) {
        fprintf(stderr, "flower project scan: run 'flower setup' first\n");
        return 1;
    }
    if (project_find_current(&p) == 0) {
        printf("project %s (%s) covers this directory\n", p.id, p.name);
    } else {
        char cwd[FLOWER_PATH_MAX];

        if (!getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "flower project scan: cannot resolve the "
                            "current directory\n");
            return 1;
        }
        if (project_create(cwd, &p) != 0) {
            fprintf(stderr, "flower project scan: cannot create the "
                            "project\n");
            return 1;
        }
        printf("created project %s (%s) for %s\n", p.id, p.name, p.path);
    }

    if (write_proxy_config(proxy_cfg, sizeof proxy_cfg, &p) != 0) {
        fprintf(stderr, "flower project scan: cannot write the scan tool "
                        "config (llmkit in PATH?)\n");
        return 1;
    }
    self_exe = self_exe_path();
    if (!self_exe) {
        fprintf(stderr, "flower project scan: cannot resolve the flower "
                        "binary path\n");
        return 1;
    }

    /* system text: identity, user identity, the scan design prompt
     * (context + current data set), tool guidance. */
    {
        char *det = read_or_placeholder(&p, "details", "md");
        char *ctx = read_or_placeholder(&p, "context", "md");
        char *res = render_resources(&p);
        char *tmpl, *sys;
        size_t len;
        const char *parts[1];

        if (!det || !ctx || !res) {
            free(det);
            free(ctx);
            free(res);
            goto oom;
        }
        tmpl = prompt_format(prompt_scan_user, p.name,
                             p.description[0] ? p.description : "(not set)",
                             det, ctx, res);
        free(det);
        free(ctx);
        free(res);
        if (!tmpl) {
            fprintf(stderr, "flower project scan: cannot build the "
                            "prompt\n");
            free(self_exe);
            return 1;
        }
        len = strlen(prompt_system_identity) + strlen(tmpl)
              + strlen(prompt_scan_system) + strlen(s.user_name)
              + strlen(s.user_role) + 48;
        sys = malloc(len);
        if (!sys) {
            free(tmpl);
            goto oom;
        }
        {
            int off = snprintf(sys, len, "%s\n%s\n%s", prompt_system_identity,
                               tmpl, prompt_scan_system);

            if (s.user_name[0] || s.user_role[0])
                off += snprintf(sys + off, len - (size_t)off,
                                "\nYou work with %s%s%s.",
                                s.user_name[0] ? s.user_name : "the user",
                                s.user_role[0] ? ", " : "",
                                s.user_role[0] ? s.user_role : "");
            (void)off;
        }
        free(tmpl);
        parts[0] = sys;
        seed = llm_build_scan_seed(&s, self_exe, proxy_cfg, p.id,
                                   parts, 1, "Run the scan now.");
        free(sys);
    }
    free(self_exe);
    if (!seed) {
        fprintf(stderr, "flower project scan: cannot build the seed\n");
        return 1;
    }

    flower_path_join(sessions_dir, sizeof sessions_dir, p.dir, "sessions");
    (void)flower_ensure_dir(sessions_dir);
    flower_snprintf(transcript, sizeof transcript,
             "%s/scan-%d.jsonl", sessions_dir, (int)getpid());

    /* root the readonly tools at the project directory */
    if (chdir(p.path) != 0) {
        fprintf(stderr, "flower project scan: cannot enter %s\n", p.path);
        free(seed);
        return 1;
    }
    {
        int rc = llm_converse(&s, seed, transcript, 1,
                              "project.update_project");

        free(seed);
        if (rc != 0)
            return 1;
    }

    print_state(&p);
    return 0;

oom:
    free(self_exe);
    fprintf(stderr, "flower project scan: out of memory\n");
    return 1;
}
