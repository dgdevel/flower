/* llmrun.c - shared llmkit runner session plumbing: the llms.md
   profile -> llm record, the jsonl record builders (system/user/nudge
   text records, stdio server entries), the session transcript files
   and the runner turn loop. shared by scan.c and taskcreate.c; split
   out of scan.c, behavior unchanged. design: plan.md, llmkit records:
   llmkit/docs/records.md. */
#define _POSIX_C_SOURCE 200809L
#include "flower.h"
#include "prompts.gen.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------- small helpers ---------- */

/* shell-quote s into b: single quotes, embedded quote closed+reopened */
int sh_quote(buf_t *b, const char *s) {
    if (buf_append(b, "'", 1) != 0) return -1;
    for (; *s; s++) {
        if (*s == '\'') {
            if (buf_append(b, "'\\''", 4) != 0) return -1;
        } else if (buf_append(b, s, 1) != 0) {
            return -1;
        }
    }
    return buf_append(b, "'", 1);
}

/* append one json node to b as a jsonl line, then free the node */
int emit_record(buf_t *b, json_t *rec) {
    char *line;
    if (!rec) return -1;
    line = json_print(rec);
    json_free(rec);
    if (!line) return -1;
    if (buf_append(b, line, strlen(line)) != 0 ||
        buf_append(b, "\n", 1) != 0) {
        free(line);
        return -1;
    }
    free(line);
    return 0;
}

/* add a string member to an object node, freeing v on oom */
int obj_str(json_t *o, const char *key, const char *v) {
    json_t *n = json_str(v);
    if (!n) return -1;
    n->key = str_dup(key);
    if (!n->key) {
        json_free(n);
        return -1;
    }
    json_add(o, n);
    return 0;
}

/* ---------- profile -> llm record ---------- */

void profile_free(profile_t *p) {
    int i;
    for (i = 0; i < p->nhdr; i++) {
        free(p->hdr_names[i]);
        free(p->hdr_vals[i]);
    }
    free(p->hdr_names);
    free(p->hdr_vals);
    for (i = 0; i < p->nproxy; i++) free(p->proxies[i]);
    free(p->proxies);
    for (i = 0; i < p->nterminal; i++) free(p->terminals[i]);
    free(p->terminals);
    memset(p, 0, sizeof *p);
}

/* load one profile chapter from llms.md into p; *doc_out receives the
   loaded doc (p borrows its strings - free with md_free_doc when done).
   0 ok, 1 error printed */
int profile_load(const char *name, profile_t *p, md_doc_t *doc_out) {
    md_chapter_t *c;
    int i;
    memset(p, 0, sizeof *p);
    if (load_rel("llms.md", FV_LLM, doc_out)) return 1;
    c = md_find(doc_out, name);
    if (!c) {
        fprintf(stderr, "flower: no such profile '%s'\n", name);
        md_free_doc(doc_out);
        return 1;
    }
    for (i = 0; i < c->nkv; i++) {
        const char *k = c->keys[i], *v = c->vals[i];
        if (!strcmp(k, "Api base")) p->base = v;
        else if (!strcmp(k, "Protocol")) p->protocol = v;
        else if (!strcmp(k, "Api key")) p->key = v;
        else if (!strcmp(k, "Model")) p->model = v;
        else if (!strcmp(k, "Max tokens")) p->max_tokens = v;
        else if (!strcmp(k, "Reasoning effort")) p->effort = v;
        else if (!strcmp(k, "Header")) {
            const char *eq = strchr(v, '=');
            char **gn, **gv;
            char *nm, *val;
            if (!eq || eq == v || !eq[1]) {
                fprintf(stderr, "flower: profile '%s': Header '%s' is "
                                "not <name>=<value>\n",
                        name, v);
                md_free_doc(doc_out);
                return 1;
            }
            nm = strndup(v, (size_t)(eq - v));
            val = str_dup(eq + 1);
            if (!nm || !val) {
                free(nm);
                free(val);
                md_free_doc(doc_out);
                profile_free(p);
                return oom();
            }
            gn = realloc(p->hdr_names, (size_t)(p->nhdr + 1) * sizeof *gn);
            gv = realloc(p->hdr_vals, (size_t)(p->nhdr + 1) * sizeof *gv);
            if (!gn || !gv) {
                free(nm);
                free(val);
                md_free_doc(doc_out);
                profile_free(p);
                return oom();
            }
            p->hdr_names = gn;
            p->hdr_vals = gv;
            p->hdr_names[p->nhdr] = nm;
            p->hdr_vals[p->nhdr] = val;
            p->nhdr++;
        } else if (!strcmp(k, "Mcp proxy")) {
            char **gp = realloc(p->proxies, (size_t)(p->nproxy + 1) *
                                 sizeof *gp);
            char *val;
            if (!gp) {
                md_free_doc(doc_out);
                profile_free(p);
                return oom();
            }
            p->proxies = gp;
            val = str_dup(v);
            if (!val) {
                md_free_doc(doc_out);
                profile_free(p);
                return oom();
            }
            p->proxies[p->nproxy++] = val;
        } else if (!strcmp(k, "Terminal tool")) {
            char **gt = realloc(p->terminals, (size_t)(p->nterminal + 1) *
                                 sizeof *gt);
            char *val;
            if (!gt) {
                md_free_doc(doc_out);
                profile_free(p);
                return oom();
            }
            p->terminals = gt;
            val = str_dup(v);
            if (!val) {
                md_free_doc(doc_out);
                profile_free(p);
                return oom();
            }
            p->terminals[p->nterminal++] = val;
        }
        /* Api base/Protocol etc. values stay borrowed from doc: doc is
           kept alive for the whole scan call */
    }
    if (!p->base || !p->base[0] || !p->protocol) {
        fprintf(stderr, "flower: profile '%s': Api base and Protocol "
                        "are required\n",
                name);
        md_free_doc(doc_out);
        profile_free(p);
        return 1;
    }
    if (!store_valid_llm_protocol(p->protocol)) {
        fprintf(stderr, "flower: profile '%s': invalid Protocol '%s'\n",
                name, p->protocol);
        md_free_doc(doc_out);
        profile_free(p);
        return 1;
    }
    if (!strcmp(p->protocol, "anthropic") &&
        (!p->max_tokens || !p->max_tokens[0])) {
        fprintf(stderr, "flower: profile '%s': anthropic protocol "
                        "requires Max tokens\n",
                name);
        md_free_doc(doc_out);
        profile_free(p);
        return 1;
    }
    if (p->max_tokens && !llm_digits(p->max_tokens)) {
        fprintf(stderr, "flower: profile '%s': Max tokens must be "
                        "digits\n",
                name);
        md_free_doc(doc_out);
        profile_free(p);
        return 1;
    }
    /* keep the doc alive: p borrows its strings */
    return 0;
}

/* build the runner/seed llm record from the profile. NULL on oom */
json_t *llm_record(const profile_t *p) {
    json_t *o = json_obj();
    const char *proto = !strcmp(p->protocol, "openai-responses")
                            ? "openai_responses"
                            : p->protocol;
    if (!o) return NULL;
    if (obj_str(o, "type", "llm") != 0 ||
        obj_str(o, "endpoint_protocol", proto) != 0 ||
        obj_str(o, "api_base", p->base) != 0)
        goto fail;
    if (p->model && obj_str(o, "model", p->model) != 0) goto fail;
    if (p->key && obj_str(o, "api_key", p->key) != 0) goto fail;
    if (p->nhdr > 0) {
        int i;
        json_t *h = json_obj();
        if (!h) goto fail;
        for (i = 0; i < p->nhdr; i++)
            if (obj_str(h, p->hdr_names[i], p->hdr_vals[i]) != 0) {
                json_free(h);
                goto fail;
            }
        h->key = str_dup("headers");
        if (!h->key) {
            json_free(h);
            goto fail;
        }
        json_add(o, h);
    }
    if (p->max_tokens || p->effort) {
        json_t *io = json_obj();
        if (!io) goto fail;
        if (p->max_tokens) {
            json_t *n = json_num(strtol(p->max_tokens, NULL, 10));
            if (!n) {
                json_free(io);
                goto fail;
            }
            n->key = str_dup("max_tokens");
            if (!n->key) {
                json_free(n);
                json_free(io);
                goto fail;
            }
            json_add(io, n);
        }
        if (p->effort && obj_str(io, "reasoning_effort", p->effort) != 0) {
            json_free(io);
            goto fail;
        }
        io->key = str_dup("inference_options");
        if (!io->key) {
            json_free(io);
            goto fail;
        }
        json_add(o, io);
    }
    return o;
fail:
    json_free(o);
    return NULL;
}

/* ---------- seed + runner jsonl ---------- */

/* write buf to path with 0600. 0 ok, 1 error printed */
int write_private(const char *path, const buf_t *b) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "flower: cannot write %s: %s\n", path,
                strerror(errno));
        return 1;
    }
    if (b->len > 0 && fwrite(b->data, 1, b->len, f) != b->len) {
        fprintf(stderr, "flower: write error on %s\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);
    if (chmod(path, 0600) != 0) {
        fprintf(stderr, "flower: cannot chmod %s: %s\n", path,
                strerror(errno));
        return 1;
    }
    return 0;
}

/* ---------- llm session transcripts ---------- */

/* open projects/<pid>/llm-sessions/<prefix>-<stamp><suffix>.jsonl,
   unique name. prefix names the command ("scan", "taskcreate");
   suffix "" is the main session, "-scout" the subagent tee. malloc'd
   path, NULL on io error (printed). mode 0600 from birth. */
char *session_open(const char *prefix, const char *pid,
                  const char *suffix) {
    char dir_rel[128];
    char *dir, *path = NULL;
    char stamp[64];
    time_t t = time(NULL);
    struct tm tmv;
    int attempt;
    if (!localtime_r(&t, &tmv)) {
        fprintf(stderr, "flower: cannot get local time\n");
        return NULL;
    }
    if (strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv) == 0) {
        fprintf(stderr, "flower: cannot format timestamp\n");
        return NULL;
    }
    snprintf(dir_rel, sizeof dir_rel, "projects/%s/llm-sessions", pid);
    if (store_mkdir_p(dir_rel) != 0) return NULL;
    dir = store_path(dir_rel);
    if (!dir) {
        oom();
        return NULL;
    }
    for (attempt = 0; attempt < 100; attempt++) {
        char name[96];
        struct stat st;
        if (attempt == 0)
            snprintf(name, sizeof name, "%s-%s%s.jsonl", prefix, stamp,
                     suffix);
        else
            snprintf(name, sizeof name, "%s-%s-%d%s.jsonl", prefix,
                     stamp, attempt, suffix);
        path = xasprintf("%s/%s", dir, name);
        if (!path) {
            free(dir);
            oom();
            return NULL;
        }
        if (stat(path, &st) != 0 && errno == ENOENT) break;
        free(path);
        path = NULL;
    }
    free(dir);
    if (!path) {
        fprintf(stderr, "flower: cannot pick a session file name\n");
        return NULL;
    }
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            fprintf(stderr, "flower: cannot create %s: %s\n", path,
                    strerror(errno));
            free(path);
            return NULL;
        }
        close(fd);
    }
    return path;
}

/* one line of the runner's output, mirrored to stdout and the session
   file. skips the runner's leading header record: the session starts
   with the input records, and the runner always re-emits its own
   {"type":"header"} as the first output line, which would duplicate
   the input's. returns 0 ok, -1 io error (printed). */
int session_line(const char *line, size_t len, FILE *sess,
                        int *skipped_header, buf_t *hist) {
    static const char hdr[] = "{\"type\":\"header\",\"version\":1}";
    if (!*skipped_header && len == strlen(hdr) &&
        memcmp(line, hdr, len) == 0) {
        *skipped_header = 1;
        return 0;
    }
    if (fwrite(line, 1, len, sess) != len || fputc('\n', sess) == EOF) {
        fprintf(stderr, "flower: session write error\n");
        return -1;
    }
    if (fwrite(line, 1, len, stdout) != len || fputc('\n', stdout) == EOF) {
        fprintf(stderr, "flower: stdout write error\n");
        return -1;
    }
    if (hist && (buf_append(hist, line, len) != 0 ||
                 buf_append(hist, "\n", 1) != 0)) {
        fprintf(stderr, "flower: out of memory\n");
        return -1;
    }
    return 0;
}

/* write every byte of data to fd, retrying on eintr. -1 io error.
   EPIPE counts as success: the writer child shares SIGPIPE
   disposition, so a dead reader shows up as write() returning -1 with
   EPIPE rather than a signal - the runner's own exit code reports it */
int write_all_fd(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EPIPE) return 0;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* run one runner turn: a fresh <llmkit> runner gets input (a full
   transcript ending in a flush) on stdin followed by eof, because the
   real runner exits after a final response instead of waiting for
   steering. its stdout lines are teed to flower's stdout, the session
   file and hist. returns the runner wait status (>= 0), -1 on a
   flower-side io error, -2 when the runner could not run (127). */
int run_turn(const char *llmkit, const buf_t *input, FILE *sess,
                    buf_t *hist) {
    pid_t kid, wr;
    int inpipe[2], outp[2];
    int st, wst;
    FILE *rp;
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    int skipped_header = 0;
    int rc = 0;

    if (pipe(inpipe) != 0 || pipe(outp) != 0) {
        fprintf(stderr, "flower: pipe: %s\n", strerror(errno));
        if (inpipe[0] >= 0) {
            close(inpipe[0]);
            close(inpipe[1]);
        }
        if (outp[0] >= 0) {
            close(outp[0]);
            close(outp[1]);
        }
        return -1;
    }
    fflush(stdout);
    kid = fork();
    if (kid < 0) {
        fprintf(stderr, "flower: fork: %s\n", strerror(errno));
        close(inpipe[0]); close(inpipe[1]);
        close(outp[0]); close(outp[1]);
        return -1;
    }
    if (kid == 0) {
        if (dup2(inpipe[0], STDIN_FILENO) < 0 ||
            dup2(outp[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(inpipe[0]); close(inpipe[1]);
        close(outp[0]); close(outp[1]);
        /* execlp: FLOWER_LLMKIT may be bare "llmkit" or a path - both
           work, PATH search happens for the bare name */
        execlp(llmkit, "llmkit", "runner", (char *)NULL);
        _exit(127);
    }
    close(inpipe[0]);
    close(outp[1]);

    /* writer child: feeds the transcript then closes the pipe (eof).
       separate process because the runner emits output while it still
       reads, and a big transcript written from the parent would
       deadlock the pipe */
    wr = fork();
    if (wr < 0) {
        fprintf(stderr, "flower: fork: %s\n", strerror(errno));
        close(inpipe[1]);
        close(outp[0]);
        kill(kid, SIGTERM);
        waitpid(kid, &st, 0);
        return -1;
    }
    if (wr == 0) {
        signal(SIGPIPE, SIG_IGN);
        _exit(write_all_fd(inpipe[1], input->data, input->len) == 0
                  ? 0
                  : 1);
    }
    close(inpipe[1]);

    rp = fdopen(outp[0], "r");
    if (!rp) {
        fprintf(stderr, "flower: cannot read runner output: %s\n",
                strerror(errno));
        close(outp[0]);
        kill(kid, SIGTERM);
        waitpid(kid, &st, 0);
        waitpid(wr, &st, 0);
        return -1;
    }
    for (;;) {
        got = getline(&line, &cap, rp);
        if (got < 0) {
            if (ferror(rp)) {
                fprintf(stderr, "flower: session read error\n");
                rc = -1;
            }
            break;
        }
        if (!line) continue;
        while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r'))
            got--;
        if (got <= 0) continue;
        if (session_line(line, (size_t)got, sess, &skipped_header,
                         hist) != 0) {
            rc = -1;
            break;
        }
    }
    free(line);
    fclose(rp);
    if (waitpid(wr, &st, 0) < 0 || (WIFEXITED(st) && WEXITSTATUS(st) != 0)) {
        fprintf(stderr, "flower: runner stdin write error\n");
        rc = -1;
    }
    if (waitpid(kid, &wst, 0) < 0) {
        fprintf(stderr, "flower: waitpid: %s\n", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(wst)) return -1;
    if (WEXITSTATUS(wst) == 127) return -2;
    return rc < 0 ? -1 : wst;
}

/* ---------- prompt text files ---------- */

/* shift every markdown header one level deeper in a serialized doc:
   "# x" -> "## x" (md.c header_scan rule: run of 1..6 '#', then ' '
   or tab or end of line). returns malloc'd copy, NULL oom */
char *shift_headers(const char *s) {
    buf_t b;
    const char *p = s;
    buf_init(&b);
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        int n = 0;
        while (n < (int)len && p[n] == '#') n++;
        if (n >= 1 && n <= 6 &&
            (n == (int)len || p[n] == ' ' || p[n] == '\t')) {
            if (buf_append(&b, "#", 1) != 0) goto fail;
        }
        if (buf_append(&b, p, len) != 0) goto fail;
        if (!nl) break;
        if (buf_append(&b, "\n", 1) != 0) goto fail;
        p = nl + 1;
    }
    return b.data;
fail:
    buf_free(&b);
    return NULL;
}

/* append one project file (shifted) to the system prompt text */
int append_file(buf_t *b, const char *rel) {
    char *path = store_path(rel);
    char *content = NULL;
    char *shifted;
    FILE *f;
    long n;
    if (!path) return -1;
    f = fopen(path, "rb");
    if (!f) {
        free(path);
        return errno == ENOENT ? 0 : -1; /* missing file: skip */
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n >= 0) {
        content = malloc((size_t)n + 1);
        if (content &&
            fread(content, 1, (size_t)n, f) == (size_t)n) {
            content[n] = '\0';
        } else {
            free(content);
            content = NULL;
        }
    }
    fclose(f);
    free(path);
    if (!content) return -1;
    shifted = shift_headers(content);
    free(content);
    if (!shifted) return -1;
    if (buf_append(b, shifted, strlen(shifted)) != 0) {
        free(shifted);
        return -1;
    }
    free(shifted);
    return 0;
}

/* ---------- text records ---------- */

/* one stage's system prompt record. NULL oom */
json_t *llmrun_system_record(const char *text) {
    json_t *o = json_obj();
    json_t *content = json_arr();
    json_t *blk = json_obj();
    json_t *txt = json_str(text);
    if (!o || !content || !blk || !txt) goto fail;
    txt->key = str_dup("text");
    if (!txt->key) goto fail;
    json_add(blk, txt);
    if (obj_str(blk, "type", "text") != 0) goto fail;
    json_add(content, blk);
    if (obj_str(o, "type", "system") != 0) goto fail;
    content->key = str_dup("content");
    if (!content->key) goto fail;
    json_add(o, content);
    return o;
fail:
    json_free(txt);
    json_free(blk);
    json_free(content);
    json_free(o);
    return NULL;
}

/* one user record. NULL oom */
json_t *llmrun_user_record(const char *text) {
    json_t *o = json_obj();
    json_t *content = json_arr();
    json_t *blk = json_obj();
    json_t *txt = json_str(text);
    if (!o || !content || !blk || !txt) goto fail;
    txt->key = str_dup("text");
    if (!txt->key) goto fail;
    json_add(blk, txt);
    if (obj_str(blk, "type", "text") != 0) goto fail;
    json_add(content, blk);
    if (obj_str(o, "type", "user") != 0) goto fail;
    content->key = str_dup("content");
    if (!content->key) goto fail;
    json_add(o, content);
    return o;
fail:
    json_free(txt);
    json_free(blk);
    json_free(content);
    json_free(o);
    return NULL;
}

/* append the nudge user record + flush: the turn ended without the
   stage marker. 0 ok, -1 oom */
int llmrun_nudge_records(buf_t *out) {
    json_t *rec = llmrun_user_record(prompt_system_prompts_scan_nudge);
    if (!rec) return -1;
    if (emit_record(out, rec) != 0) return -1;
    if (emit_record(out, json_parse("{\"type\":\"flush\"}")) != 0)
        return -1;
    return 0;
}

/* classify one runner output line for stage tracking: 1 a final
   response ending in [STEP_COMPLETE], 2 a final response without the
   marker, 0 anything else, -1 unparseable (printed). "final" means
   non-partial with a finish_reason: text before tool calls in the same
   turn also arrives as response records, but without one. */
int llmrun_stage_line(const char *line, size_t len) {
    static const char marker[] = "[STEP_COMPLETE]";
    json_t *rec;
    const char *text;
    size_t m = sizeof marker - 1;
    (void)len;
    rec = json_parse(line);
    if (!rec) {
        fprintf(stderr, "flower: bad runner line\n");
        return -1;
    }
    if (json_str_of(rec, "type") &&
        !strcmp(json_str_of(rec, "type"), "response")) {
        json_t *part = json_get(rec, "partial");
        int partial = part && (part->type == JSON_BOOL
                                   ? (int)part->num
                                   : part->type != JSON_NULL);
        if (!partial && json_get(rec, "finish_reason")) {
            text = json_str_of(rec, "text");
            if (text) {
                size_t t = strlen(text);
                /* models may pad the marker line with whitespace */
                while (t > 0 &&
                       (text[t - 1] == '\n' || text[t - 1] == '\r' ||
                        text[t - 1] == ' ' || text[t - 1] == '\t'))
                    t--;
                int r = (t >= m &&
                         memcmp(text + t - m, marker, m) == 0)
                            ? 1
                            : 2;
                json_free(rec);
                return r;
            }
        }
    }
    json_free(rec);
    return 0;
}

/* one stdio server entry, borrowed fields copied. NULL oom */
json_t *llmrun_server_entry(const char *name, const char *cmd,
                            int required) {
    json_t *o = json_obj();
    if (!o) return NULL;
    if (obj_str(o, "type", "stdio") != 0 ||
        obj_str(o, "name", name) != 0 ||
        obj_str(o, "command_line", cmd) != 0) {
        json_free(o);
        return NULL;
    }
    if (!required) {
        json_t *r = json_bool(0);
        if (!r) {
            json_free(o);
            return NULL;
        }
        r->key = str_dup("required");
        if (!r->key) {
            json_free(r);
            json_free(o);
            return NULL;
        }
        json_add(o, r);
    }
    return o;
}

/* does hist (the transcript fed to the next turn) end with a final
   [STEP_COMPLETE] response? 1 yes, 0 no, -1 bad json (printed). */
int llmrun_hist_ends_complete(const buf_t *hist) {
    const char *p, *last_nl;
    char *copy;
    int r;
    if (hist->len == 0) return 0;
    /* find the last non-empty line */
    p = hist->data + hist->len;
    while (p > hist->data && (p[-1] == '\n' || p[-1] == '\r')) p--;
    last_nl = p;
    while (last_nl > hist->data && last_nl[-1] != '\n') last_nl--;
    if (last_nl == p) return 0;
    copy = strndup(last_nl, (size_t)(p - last_nl));
    if (!copy) return oom() - 1;
    r = llmrun_stage_line(copy, strlen(copy));
    free(copy);
    if (r < 0) return -1;
    return r == 1 ? 1 : 0;
}
