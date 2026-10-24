/* llm.c - bridge to the llmkit runner child process. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* growable byte buffer for record accumulation. */
struct buf {
    char *data;
    size_t len, cap;
};

static int buf_reserve(struct buf *b, size_t extra)
{
    char *p;

    if (b->len + extra + 1 <= b->cap)
        return 0;
    b->cap = b->cap ? b->cap : 4096;
    while (b->cap < b->len + extra + 1)
        b->cap *= 2;
    p = realloc(b->data, b->cap);
    if (!p)
        return -1;
    b->data = p;
    return 0;
}

static int buf_put(struct buf *b, const char *s, size_t n)
{
    if (buf_reserve(b, n) != 0)
        return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int buf_puts(struct buf *b, const char *s)
{
    return buf_put(b, s, strlen(s));
}

/* json-escape s into b (no surrounding quotes). */
static int buf_put_json_escaped(struct buf *b, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[8];

        switch (c) {
        case '"':
            if (buf_puts(b, "\\\""))
                return -1;
            break;
        case '\\':
            if (buf_puts(b, "\\\\"))
                return -1;
            break;
        case '\n':
            if (buf_puts(b, "\\n"))
                return -1;
            break;
        case '\r':
            if (buf_puts(b, "\\r"))
                return -1;
            break;
        case '\t':
            if (buf_puts(b, "\\t"))
                return -1;
            break;
        default:
            if (c < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                if (buf_puts(b, tmp))
                    return -1;
            } else {
                if (buf_put(b, (const char *)&c, 1))
                    return -1;
            }
        }
    }
    return 0;
}

/* append one record built as {"type":...,json body} to seed. json_body may
 * be NULL. Returns 0 on success. */
static int seed_add(struct buf *b, const char *type, const char *json_body)
{
    if (buf_puts(b, "{\"type\":\""))
        return -1;
    if (buf_puts(b, type))
        return -1;
    if (json_body && *json_body) {
        if (buf_puts(b, "\","))
            return -1;
        if (buf_puts(b, json_body))
            return -1;
    }
    if (buf_puts(b, "}\n"))
        return -1;
    return 0;
}

/* locate the llmkit binary in $PATH. Returns a malloc'd path or NULL. */
static char *find_llmkit(void)
{
    const char *path = getenv("PATH");
    const char *p;
    char try[FLOWER_PATH_MAX];

    if (!path)
        return NULL;
    p = path;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);

        if (len > 0) {
            snprintf(try, sizeof(try), "%.*s/llmkit", (int)len, p);
            if (access(try, X_OK) == 0)
                return strdup(try);
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return NULL;
}

char *llm_build_seed(const struct flower_settings *cfg,
                     const char **prompt_parts, int part_count,
                     const char *first_user_text)
{
    struct buf b = {0};
    char *llmkit_path;
    int i;

    /* llm record */
    {
        char body[2048];
        int n = snprintf(body, sizeof(body),
                         "\"endpoint_protocol\":\"%s\",\"api_base\":\"%s\"",
                         cfg->protocol, cfg->api_base);

        if (n < 0)
            goto fail;
        if (cfg->model[0] || cfg->api_key[0]) {
            char more[1024];
            int m = 0;

            more[0] = '\0';
            if (cfg->model[0])
                m += snprintf(more + m, sizeof(more) - (size_t)m,
                              ",\"model\":\"%s\"", cfg->model);
            if (cfg->api_key[0])
                m += snprintf(more + m, sizeof(more) - (size_t)m,
                              ",\"api_key\":\"%s\"", cfg->api_key);
            if (m < 0 || (size_t)(n + m) >= sizeof(body))
                goto fail;
            strcat(body, more);
        }
        if (seed_add(&b, "llm", body) != 0)
            goto fail;
    }

    /* tools record: builtin server via llmkit builtin-mcp */
    llmkit_path = find_llmkit();
    if (!llmkit_path) {
        fprintf(stderr, "flower: llmkit binary not found in PATH\n");
        goto fail;
    }
    {
        struct buf t = {0};

        if (buf_puts(&t, "\"tools\":[{\"type\":\"stdio\",\"name\":\"builtin\","
                         "\"command_line\":\""))
            goto tools_fail;
        if (buf_put_json_escaped(&t, llmkit_path) != 0)
            goto tools_fail;
        if (buf_puts(&t, " builtin-mcp\"}]"))
            goto tools_fail;
        if (seed_add(&b, "tools", t.data) != 0)
            goto tools_fail;
        free(t.data);
    }
    free(llmkit_path);

    /* system record */
    if (part_count > 0) {
        struct buf s = {0};
        int first = 1;

        if (buf_puts(&s, "\"content\":[{\"type\":\"text\",\"text\":\""))
            goto sys_fail;
        for (i = 0; i < part_count; i++) {
            if (!first && buf_puts(&s, "\\n") != 0)
                goto sys_fail;
            if (buf_put_json_escaped(&s, prompt_parts[i]) != 0)
                goto sys_fail;
            first = 0;
        }
        if (buf_puts(&s, "\"}]"))
            goto sys_fail;
        if (seed_add(&b, "system", s.data) != 0)
            goto sys_fail;
        free(s.data);
    }

    /* first user record */
    if (first_user_text && first_user_text[0]) {
        struct buf u = {0};

        if (buf_puts(&u, "\"content\":[{\"type\":\"text\",\"text\":\""))
            goto user_fail;
        if (buf_put_json_escaped(&u, first_user_text) != 0)
            goto user_fail;
        if (buf_puts(&u, "\"}]"))
            goto user_fail;
        if (seed_add(&b, "user", u.data) != 0)
            goto user_fail;
        free(u.data);
    }

    return b.data;

tools_fail:
    free(llmkit_path);
    goto fail;
sys_fail:
    free(b.data);
    return NULL;
user_fail:
    free(b.data);
    return NULL;
fail:
    free(b.data);
    return NULL;
}

char *llm_build_scan_seed(const struct flower_settings *cfg,
                          const char *self_exe, const char *proxy_cfg,
                          const char *project_id,
                          const char **prompt_parts, int part_count,
                          const char *first_user_text)
{
    struct buf b = {0};
    char *llmkit_path;
    int i;

    /* llm record: same shape as llm_build_seed */
    {
        char body[2048];
        int n = snprintf(body, sizeof(body),
                         "\"endpoint_protocol\":\"%s\",\"api_base\":\"%s\"",
                         cfg->protocol, cfg->api_base);

        if (n < 0)
            return NULL;
        if (cfg->model[0] || cfg->api_key[0]) {
            char more[1024];
            int m = 0;

            more[0] = '\0';
            if (cfg->model[0])
                m += snprintf(more + m, sizeof(more) - (size_t)m,
                              ",\"model\":\"%s\"", cfg->model);
            if (cfg->api_key[0])
                m += snprintf(more + m, sizeof(more) - (size_t)m,
                              ",\"api_key\":\"%s\"", cfg->api_key);
            if (m < 0 || (size_t)(n + m) >= sizeof(body))
                return NULL;
            strcat(body, more);
        }
        if (seed_add(&b, "llm", body) != 0)
            goto fail;
    }

    /* tools record: project server (terminal update_project) + readonly
     * scan server (mcp-proxy over the whitelist config) */
    llmkit_path = find_llmkit();
    if (!llmkit_path) {
        fprintf(stderr, "flower: llmkit binary not found in PATH\n");
        goto fail;
    }
    {
        struct buf t = {0};
        char cmd[FLOWER_PATH_MAX * 2 + 64];
        int n = snprintf(cmd, sizeof(cmd), "%s mcp-scan %s", self_exe,
                         project_id);

        if (n < 0 || (size_t)n >= sizeof(cmd))
            goto tools_fail;
        if (buf_puts(&t, "\"tools\":[") != 0
            || buf_puts(&t, "{\"type\":\"stdio\",\"name\":\"project\","
                            "\"command_line\":\"") != 0
            || buf_put_json_escaped(&t, cmd) != 0
            || buf_puts(&t, "\",\"terminal_tools\":[\"update_project\"],"
                            "\"required\":true},{\"type\":\"stdio\","
                            "\"name\":\"scan\",\"command_line\":\"") != 0
            || buf_puts(&t, llmkit_path) != 0
            || buf_puts(&t, " mcp-proxy ") != 0
            || buf_put_json_escaped(&t, proxy_cfg) != 0
            || buf_puts(&t, "\",\"required\":true}]") != 0)
            goto tools_fail;
        if (seed_add(&b, "tools", t.data) != 0)
            goto tools_fail;
        free(t.data);
    }
    free(llmkit_path);

    /* options: bound the readonly scanning rounds */
    if (seed_add(&b, "options", "\"max_tool_rounds\":40") != 0)
        goto fail;

    /* system record */
    if (part_count > 0) {
        struct buf s = {0};
        int first = 1;

        if (buf_puts(&s, "\"content\":[{\"type\":\"text\",\"text\":\""))
            goto fail;
        for (i = 0; i < part_count; i++) {
            if (!first && buf_puts(&s, "\\n") != 0)
                goto fail;
            if (buf_put_json_escaped(&s, prompt_parts[i]) != 0)
                goto fail;
            first = 0;
        }
        if (buf_puts(&s, "\"}]"))
            goto fail;
        if (seed_add(&b, "system", s.data) != 0)
            goto fail;
        free(s.data);
    }

    /* first user record */
    if (first_user_text && first_user_text[0]) {
        struct buf u = {0};

        if (buf_puts(&u, "\"content\":[{\"type\":\"text\",\"text\":\""))
            goto fail;
        if (buf_put_json_escaped(&u, first_user_text) != 0)
            goto fail;
        if (buf_puts(&u, "\"}]"))
            goto fail;
        if (seed_add(&b, "user", u.data) != 0)
            goto fail;
        free(u.data);
    }

    return b.data;

tools_fail:
    free(llmkit_path);
fail:
    free(b.data);
    return NULL;
}

char *llm_append_record(const char *seed, const char *record)
{
    struct buf b = {0};

    if (!seed || !record)
        return NULL;
    if (buf_puts(&b, seed) != 0)
        return NULL;
    if (b.len > 0 && b.data[b.len - 1] != '\n'
        && buf_puts(&b, "\n") != 0)
        return NULL;
    if (buf_puts(&b, record) != 0) {
        free(b.data);
        return NULL;
    }
    if (b.data[b.len - 1] != '\n' && buf_puts(&b, "\n") != 0) {
        free(b.data);
        return NULL;
    }
    return b.data;
}

/* parse one runner stdout line: route records to rendering and outputs. */
static int handle_record(char *line, struct buf *resp, struct buf *records,
                         int *final_seen)
{
    cJSON *root, *item;
    const char *type;

    if (buf_puts(records, line) != 0 || buf_puts(records, "\n") != 0)
        return -1;
    root = cJSON_Parse(line);
    if (!root)
        return 0; /* not json: ignore */
    item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(item)) {
        cJSON_Delete(root);
        return 0;
    }
    type = item->valuestring;

    if (!strcmp(type, "thinking")) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");

        if (cJSON_IsString(t) && t->valuestring[0]) {
            printf("[thinking] %s\n", t->valuestring);
            fflush(stdout);
        }
    } else if (!strcmp(type, "tool_request")) {
        cJSON *tool = cJSON_GetObjectItemCaseSensitive(root, "tool");
        cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "arguments");

        printf("[tool] %s", cJSON_IsString(tool) ? tool->valuestring : "?");
        if (args) {
            char *s = cJSON_PrintUnformatted(args);

            if (s) {
                printf(" %s", s);
                free(s);
            }
        }
        printf("\n");
        fflush(stdout);
    } else if (!strcmp(type, "response")) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");
        cJSON *partial = cJSON_GetObjectItemCaseSensitive(root, "partial");

        if (cJSON_IsString(t) && t->valuestring[0]) {
            fputs(t->valuestring, stdout);
            fflush(stdout);
            if (buf_puts(resp, t->valuestring) != 0) {
                cJSON_Delete(root);
                return -1;
            }
        }
        if (!cJSON_IsTrue(partial) && *final_seen == 0) {
            /* final response of a block: newline separator */
            printf("\n");
            *final_seen = 1;
        }
    } else if (!strcmp(type, "error")) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");

        fprintf(stderr, "flower: llm error (%s): %s\n",
                cJSON_IsString(code) ? code->valuestring : "?",
                cJSON_IsString(msg) ? msg->valuestring : "?");
        fflush(stderr);
    } else if (!strcmp(type, "tool_response")) {
        /* tool traffic already shown at request time; keep quiet */
    }
    cJSON_Delete(root);
    return 0;
}

/* scan text for ```flower fenced blocks, parse each into a proposal.
 * block format: first line = target path, rest = content. Returns 0 ok,
 * -1 on parse failure (message printed). Proposals appended to *props. */
static int parse_proposals(const char *text, struct flower_proposal **props,
                           int *prop_count)
{
    const char *p = text;
    int alloc = 0;

    while ((p = strstr(p, "```flower")) != NULL) {
        const char *nl = strchr(p, '\n');   /* end of fence open line */
        const char *tnl;                    /* end of target line */
        const char *content;                /* first byte of content */
        const char *end;
        const char *start;                  /* first target byte */

        if (!nl) {
            fprintf(stderr, "flower: unterminated flower block\n");
            return -1;
        }
        /* target may sit on the fence line itself (```flower path) */
        start = p + 9;
        while (start < nl && (*start == ' ' || *start == '\t'))
            start++;
        if (start < nl) {
            tnl = nl;
            content = nl + 1;
        } else {
            tnl = strchr(nl + 1, '\n');
            if (!tnl) {
                fprintf(stderr, "flower: flower block has no content\n");
                return -1;
            }
            start = nl + 1;
            content = tnl + 1;
        }
        end = strstr(content, "```");
        if (!end) {
            fprintf(stderr, "flower: unterminated flower block\n");
            return -1;
        }
        {
            char target[FLOWER_PATH_MAX];
            size_t tlen = (size_t)(tnl - start);
            struct flower_proposal *np;
            size_t clen = (size_t)(end - content);

            if (tlen == 0 || tlen >= sizeof(target)) {
                fprintf(stderr, "flower: bad flower block target line\n");
                return -1;
            }
            memcpy(target, start, tlen);
            target[tlen] = '\0';
            /* trim trailing spaces of target */
            while (tlen > 0 && (target[tlen - 1] == ' '
                                || target[tlen - 1] == '\r'))
                target[--tlen] = '\0';

            if (*prop_count == alloc) {
                alloc = alloc ? alloc * 2 : 4;
                np = realloc(*props, (size_t)alloc * sizeof(**props));
                if (!np) {
                    fprintf(stderr, "flower: out of memory\n");
                    return -1;
                }
                *props = np;
            }
            snprintf((*props)[*prop_count].target, FLOWER_PATH_MAX, "%s",
                     target);
            (*props)[*prop_count].content = malloc(clen + 1);
            if (!(*props)[*prop_count].content) {
                fprintf(stderr, "flower: out of memory\n");
                return -1;
            }
            if (clen > 0)
                memcpy((*props)[*prop_count].content, content, clen);
            (*props)[*prop_count].content[clen] = '\0';
            /* trim one trailing newline: fences usually close on their
             * own line */
            if (clen > 0
                && (*props)[*prop_count].content[clen - 1] == '\n')
                (*props)[*prop_count].content[clen - 1] = '\0';
            (*prop_count)++;
        }
        p = end + 3;
    }
    return 0;
}

int llm_run_turn(const char *seed,
                 char **resp, char **out_records,
                 struct flower_proposal **props, int *prop_count)
{
    char *llmkit_path;
    int in_pipe[2], out_pipe[2];
    pid_t pid;
    struct buf resp_b = {0}, rec_b = {0};
    struct flower_proposal *prop_v = NULL;
    int prop_n = 0, final_seen = 0;
    int status;
    FILE *child_out;
    char *line = NULL;
    size_t linecap = 0;
    ssize_t nread;
    int rc = -1;

    *resp = NULL;
    if (out_records)
        *out_records = NULL;
    *props = NULL;
    *prop_count = 0;

    llmkit_path = find_llmkit();
    if (!llmkit_path) {
        fprintf(stderr, "flower: llmkit binary not found in PATH\n");
        return -1;
    }
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        fprintf(stderr, "flower: pipe failed: %s\n", strerror(errno));
        free(llmkit_path);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "flower: fork failed: %s\n", strerror(errno));
        free(llmkit_path);
        return -1;
    }
    if (pid == 0) {
        /* child: llmkit runner, stdin from pipe, stdout to pipe */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execl(llmkit_path, "llmkit", "runner", (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    free(llmkit_path);

    /* write seed then close stdin */
    {
        size_t off = 0, len = strlen(seed);

        while (off < len) {
            ssize_t w = write(in_pipe[1], seed + off, len - off);

            if (w <= 0)
                break;
            off += (size_t)w;
        }
    }
    close(in_pipe[1]);

    child_out = fdopen(out_pipe[0], "r");
    if (!child_out) {
        fprintf(stderr, "flower: fdopen failed\n");
        close(out_pipe[0]);
        goto out_wait;
    }
    while ((nread = getline(&line, &linecap, child_out)) != -1) {
        if (nread > 0 && line[nread - 1] == '\n')
            line[nread - 1] = '\0';
        if (handle_record(line, &resp_b, &rec_b, &final_seen) != 0)
            goto out_close;
    }
    rc = 0;

out_close:
    free(line);
    fclose(child_out);
out_wait:
    waitpid(pid, &status, 0);
    if (rc == 0 && !WIFEXITED(status))
        rc = -1;
    if (rc == 0 && WEXITSTATUS(status) == 9) {
        /* exit 9: a terminal tool fired. A requested end, not a failure:
         * the caller decides whether that ends the conversation (scan)
         * or is unexpected (flower blocks). */
        rc = 9;
    } else if (rc == 0 && WEXITSTATUS(status) != 0) {
        fprintf(stderr, "flower: llmkit runner exited with code %d\n",
                WEXITSTATUS(status));
        rc = -1;
    }
    if (rc == 0) {
        if (parse_proposals(resp_b.data, &prop_v, &prop_n) != 0)
            rc = -1;
    }
    if (rc != 0 && rc != 9) {
        free(resp_b.data);
        free(rec_b.data);
        llm_free_proposals(prop_v, prop_n);
        return rc;
    }
    *resp = resp_b.data ? resp_b.data : strdup("");
    if (out_records)
        *out_records = rec_b.data;
    else
        free(rec_b.data);
    *props = prop_v;
    *prop_count = prop_n;
    return rc;
}

void llm_free_proposals(struct flower_proposal *props, int count)
{
    int i;

    for (i = 0; i < count; i++)
        free(props[i].content);
    free(props);
}

/* apply one proposal: show target, ask, write under $FLOWER_BASE on yes.
 * Returns 1 applied, 0 skipped, -1 error. */
static int proposal_apply(const struct flower_proposal *pr, int auto_yes)
{
    char base[FLOWER_PATH_MAX];
    char path[FLOWER_PATH_MAX];
    char ans[32];
    size_t len;
    char *slash;

    if (flower_base_dir(base, sizeof(base)) != 0)
        return -1;
    if (pr->target[0] == '/' || strstr(pr->target, ".."))
        return -1; /* refuse absolute or escaping targets */
    flower_path_join(path, sizeof path, base, pr->target);

    printf("\nproposed write: %s\n", pr->target);
    printf("---- begin ----\n%s\n---- end ----\n", pr->content);
    if (auto_yes) {
        ans[0] = 'y';
        ans[1] = '\0';
    } else {
        printf("write this file? [y/n] ");
        fflush(stdout);
        if (!fgets(ans, sizeof(ans), stdin))
            return 0;
        ans[0] = (char)tolower((unsigned char)ans[0]);
    }
    if (ans[0] != 'y')
        return 0;

    /* ensure parent dirs of the target exist */
    len = strlen(path);
    {
        char tmp[FLOWER_PATH_MAX];

        memcpy(tmp, path, len + 1);
        slash = strrchr(tmp, '/');
        if (slash) {
            *slash = '\0';
            if (flower_ensure_dir(tmp) != 0) {
                fprintf(stderr, "flower: cannot create directory %s\n", tmp);
                return -1;
            }
        }
    }
    if (flower_write_file(path, pr->content) != 0) {
        fprintf(stderr, "flower: cannot write %s\n", path);
        return -1;
    }
    printf("written %s\n", path);
    return 1;
}

/* trim trailing newline and spaces of s, in place. */
static void len_trim(char *s)
{
    size_t len = strlen(s);

    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'
                       || s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = '\0';
}

int llm_converse(const struct flower_settings *cfg, const char *seed,
                 const char *transcript_path, int terminal_ok)
{
    char *current_seed = strdup(seed);
    char *head; /* seed part before the first user record (llm, tools,
                 * system): every replay needs it, runner stdout does not
                 * carry it */

    (void)cfg;
    if (!current_seed)
        return 1;
    {
        const char *first_user = strstr(seed, "{\"type\":\"user\"");
        size_t head_len = first_user ? (size_t)(first_user - seed)
                                     : strlen(seed);
        char *h = malloc(head_len + 1);

        if (!h) {
            free(current_seed);
            return 1;
        }
        memcpy(h, seed, head_len);
        h[head_len] = '\0';
        head = h;
    }
    if (transcript_path && transcript_path[0])
        flower_write_file(transcript_path, "");
    for (;;) {
        char *resp, *records, *user_line = NULL;
        size_t cap = 0;
        struct flower_proposal *props;
        int prop_count, i, all = 0, rc, terminal_hit = 0;

        {
            int trc = llm_run_turn(current_seed, &resp, &records, &props,
                                   &prop_count);
            int terminal = (trc == 9);

            if (trc != 0 && !(terminal && terminal_ok)) {
                free(current_seed);
                free(head);
                return 1;
            }
            terminal_hit = terminal;
        }
        if (transcript_path && transcript_path[0]) {
            char *old = NULL;

            if (flower_read_file(transcript_path, &old) == 0 && old) {
                char *joined = llm_append_record(old, records);

                free(old);
                if (joined) {
                    flower_write_file(transcript_path, joined);
                    free(joined);
                }
            } else {
                flower_write_file(transcript_path, records);
            }
        }
        if (terminal_hit) {
            /* the terminal tool already delivered the data; finish the
             * conversation without a follow-up prompt */
            free(resp);
            free(records);
            llm_free_proposals(props, prop_count);
            free(current_seed);
            free(head);
            return 0;
        }
        for (i = 0; i < prop_count; i++) {
            rc = proposal_apply(&props[i], all);
            if (rc < 0) {
                fprintf(stderr, "flower: proposal rejected: bad target\n");
                continue;
            }
            if (rc == 1)
                all = 1;
        }
        llm_free_proposals(props, prop_count);

        fputs("> ", stdout);
        fflush(stdout);
        if (getline(&user_line, &cap, stdin) == -1) {
            printf("\n");
            free(user_line);
            free(resp);
            free(records);
            free(current_seed);
            free(head);
            return 0;
        }
        len_trim(user_line);
        if (user_line[0] == '\0' || !strcmp(user_line, "q")) {
            free(user_line);
            free(resp);
            free(records);
            free(current_seed);
            free(head);
            return 0;
        }
        {
            struct buf u = {0};

            if (buf_puts(&u, "{\"type\":\"user\",\"content\":[{\"type\":"
                             "\"text\",\"text\":\"") != 0
                || buf_put_json_escaped(&u, user_line) != 0
                || buf_puts(&u, "\"}]}") != 0) {
                free(user_line);
                free(resp);
                free(records);
                free(current_seed);
                free(head);
                free(u.data);
                return 1;
            }
            {
                char *tmp = llm_append_record(head, records);
                char *next;

                if (!tmp) {
                    free(user_line);
                    free(resp);
                    free(records);
                    free(current_seed);
                    free(u.data);
                    return 1;
                }
                next = llm_append_record(tmp, u.data);
                free(tmp);
                free(u.data);
                free(current_seed);
                current_seed = next;
            }
        }
        free(user_line);
        free(resp);
        free(records);
    }
}

/* hidden smoke command: one turn, no tools needed context. */
int llm_ping_cmd(int argc, char **argv)
{
    struct flower_settings s;
    const char *prompt = (argc > 1) ? argv[1] : "reply with exactly: pong";
    const char *parts[] = {"You are a terse assistant."};
    char *seed;

    if (flower_settings_load(&s) != 0) {
        fprintf(stderr, "flower llm-ping: run 'flower setup' first\n");
        return 1;
    }
    seed = llm_build_seed(&s, parts, 1, prompt);
    if (!seed) {
        fprintf(stderr, "flower: cannot build seed\n");
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "converse"))
        return llm_converse(&s, llm_build_seed(&s, parts, 1,
                                               argc > 2 ? argv[2] : "hello"),
                            NULL, 0);
    {
        char *resp, *records;
        struct flower_proposal *props;
        int prop_count;

        if (llm_run_turn(seed, &resp, &records, &props, &prop_count) != 0) {
            free(seed);
            return 1;
        }
        free(seed);
        free(records);
        llm_free_proposals(props, prop_count);
        if (!resp || !resp[0]) {
            fprintf(stderr, "flower: empty response\n");
            free(resp);
            return 1;
        }
        free(resp);
    }
    return 0;
}
