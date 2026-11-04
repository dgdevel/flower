/* scan.c - flower project <P> scan <profile>: drive a staged llmkit
   runner session that scans the project and fills details.md,
   context.md and resources.md through the flower mcp servers. three
   stages (details, context, resources), one system prompt and one user
   kickoff each: the stage records are written to the runner's stdin as
   it runs, a flush record starts each stage, and the stage ends when
   [STEP_COMPLETE] closes its final response. design: plan.md,
   llmkit records: llmkit/docs/records.md. */
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int cmd_scan(const char *pid, const char *profile, int dry_run,
             const char *argv0);

/* ---------- small helpers ---------- */

/* shell-quote s into b: single quotes, embedded quote closed+reopened */
static int sh_quote(buf_t *b, const char *s) {
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
static int emit_record(buf_t *b, json_t *rec) {
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
static int obj_str(json_t *o, const char *key, const char *v) {
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

/* profile values: up to one each, key/value arrays for repeats */
typedef struct {
    const char *base;      /* Api base */
    const char *protocol;  /* Protocol */
    const char *key;       /* Api key */
    const char *model;     /* Model */
    const char *max_tokens;/* Max tokens */
    const char *effort;    /* Reasoning effort */
    char **hdr_names;      /* Header pairs, split */
    char **hdr_vals;
    int nhdr;
    char **proxies;        /* Mcp proxy paths */
    int nproxy;
    char **terminals;      /* Terminal tool server.tool entries */
    int nterminal;
} profile_t;

static void profile_free(profile_t *p) {
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
static int profile_load(const char *name, profile_t *p, md_doc_t *doc_out) {
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
static json_t *llm_record(const profile_t *p) {
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
static int write_private(const char *path, const buf_t *b) {
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

/* open projects/<pid>/llm-sessions/scan-<stamp>.jsonl, unique name.
   malloc'd path, NULL on io error (printed). mode 0600 from birth. */
static char *session_open(const char *pid) {
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
        if (attempt == 0) snprintf(name, sizeof name, "scan-%s.jsonl", stamp);
        else snprintf(name, sizeof name, "scan-%s-%d.jsonl", stamp, attempt);
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
static int session_line(const char *line, size_t len, FILE *sess,
                        int *skipped_header) {
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
    return 0;
}

/* shift every markdown header one level deeper in a serialized doc:
   "# x" -> "## x" (md.c header_scan rule: run of 1..6 '#', then ' '
   or tab or end of line). returns malloc'd copy, NULL oom */
static char *shift_headers(const char *s) {
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
static int append_file(buf_t *b, const char *rel) {
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

/* one scan stage: bundled instructions + the config file it writes +
   its kickoff user message */
typedef struct {
    const char *prompt;  /* stage instructions (system prompt body) */
    const char *file;    /* projects/<pid>/<file>, embedded as current */
    const char *kickoff; /* stage start user message */
} scan_stage_t;

static const scan_stage_t scan_stages[3] = {
    {prompt_system_prompts_scan_stage1_details, "details.md",
     prompt_system_prompts_scan_stage1_start},
    {prompt_system_prompts_scan_stage2_context, "context.md",
     prompt_system_prompts_scan_stage2_start},
    {prompt_system_prompts_scan_stage3_resources, "resources.md",
     prompt_system_prompts_scan_stage3_start},
};

/* the system prompt for one stage: shared preamble + stage
   instructions + the current content of the file this stage writes,
   headers shifted one level deeper. NULL oom */
static char *stage_prompt_text(const char *pid, const scan_stage_t *st) {
    buf_t b;
    char *rel;
    buf_init(&b);
    if (buf_appendf(&b, "%s\n%s", prompt_system_prompts_scan_preamble,
                    st->prompt) != 0 ||
        buf_appendf(&b, "\n# Current %s\n", st->file) != 0)
        goto fail;
    rel = xasprintf("projects/%s/%s", pid, st->file);
    if (!rel) goto fail;
    if (append_file(&b, rel) != 0) {
        free(rel);
        goto fail;
    }
    free(rel);
    return b.data;
fail:
    buf_free(&b);
    return NULL;
}

/* one stage's system prompt record. NULL oom */
static json_t *stage_system_record(const char *text) {
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
static json_t *stage_user_record(const char *text) {
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

/* append one stage's records to out: system prompt, kickoff user
   message, then the flush that starts the stage (a flush is only legal
   with new records pending - llmkit rule). 0 ok, -1 oom */
static int stage_records(buf_t *out, const scan_stage_t *st,
                         const char *text) {
    json_t *rec;
    rec = stage_system_record(text);
    if (!rec) return -1;
    if (emit_record(out, rec) != 0) return -1;
    rec = stage_user_record(st->kickoff);
    if (!rec) return -1;
    if (emit_record(out, rec) != 0) return -1;
    if (emit_record(out, json_parse("{\"type\":\"flush\"}")) != 0)
        return -1;
    return 0;
}

/* append the nudge user record + flush: the turn ended without the
   stage marker. 0 ok, -1 oom */
static int nudge_records(buf_t *out) {
    json_t *rec = stage_user_record(prompt_system_prompts_scan_nudge);
    if (!rec) return -1;
    if (emit_record(out, rec) != 0) return -1;
    if (emit_record(out, json_parse("{\"type\":\"flush\"}")) != 0)
        return -1;
    return 0;
}

/* write out to the runner's stdin in full and mirror it to the session
   transcript. 0 ok, -1 io error */
static int send_records(int fd, FILE *sess, buf_t *out) {
    size_t off = 0;
    while (off < out->len) {
        ssize_t n = write(fd, out->data + off, out->len - off);
        if (n <= 0) {
            fprintf(stderr, "flower: runner stdin write error\n");
            return -1;
        }
        off += (size_t)n;
    }
    if (sess && out->len > 0 &&
        fwrite(out->data, 1, out->len, sess) != out->len) {
        fprintf(stderr, "flower: session write error\n");
        return -1;
    }
    buf_reset(out);
    return 0;
}

/* classify one runner output line for stage tracking: 1 a final
   response ending in [STEP_COMPLETE], 2 a final response without the
   marker, 0 anything else, -1 unparseable (printed). "final" means
   non-partial with a finish_reason: text before tool calls in the same
   turn also arrives as response records, but without one. */
static int stage_line(const char *line, size_t len) {
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
        !strcmp(json_str_of(rec, "type"), "response") &&
        !json_get(rec, "partial") && json_get(rec, "finish_reason")) {
        text = json_str_of(rec, "text");
        if (text) {
            size_t t = strlen(text);
            /* models may pad the marker line with whitespace */
            while (t > 0 && (text[t - 1] == '\n' || text[t - 1] == '\r' ||
                             text[t - 1] == ' ' || text[t - 1] == '\t'))
                t--;
            int r = (t >= m && memcmp(text + t - m, marker, m) == 0) ? 1 : 2;
            json_free(rec);
            return r;
        }
    }
    json_free(rec);
    return 0;
}

/* read runner output through session_line until the stage's turn ends.
   1 done ([STEP_COMPLETE]), 0 turn ended without the marker (caller
   may nudge), -1 io error (printed), 2 the runner's output closed
   before a final response */
static int wait_stage_end(FILE *rp, char **line, size_t *cap, FILE *sess,
                          int *skipped_header) {
    for (;;) {
        ssize_t got;
        if ((got = getline(line, cap, rp)) < 0) {
            if (ferror(rp)) {
                fprintf(stderr, "flower: session read error\n");
                return -1;
            }
            return 2; /* eof without a final response */
        }
        if (!*line) continue; /* getline may set NULL on empty input */
        while (got > 0 && ((*line)[got - 1] == '\n' ||
                           (*line)[got - 1] == '\r'))
            got--;
        if (got <= 0) continue;
        if (session_line(*line, (size_t)got, sess, skipped_header) != 0)
            return -1;
        switch (stage_line(*line, (size_t)got)) {
        case 1:
            return 1;
        case 2:
            return 0;
        case -1:
            return -1;
        default:
            break;
        }
    }
}

/* one stdio server entry, borrowed fields copied. NULL oom */
static json_t *server_entry(const char *name, const char *cmd,
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

/* ---------- cmd_scan ---------- */

int cmd_scan(const char *pid, const char *profile, int dry_run,
             const char *argv0) {
    profile_t p;
    md_doc_t llms_doc; /* holds the borrowed profile strings */
    char exe[4096];    /* absolute self path for mcp server spawns */
    char *seed_rel = NULL, *seed_path = NULL;
    buf_t seed, input;
    json_t *llm = NULL, *tools = NULL, *sysrec = NULL;
    const char *llmkit;
    int rc = 1;
    int i;

    if (!require_project(pid)) return 1;
    if (profile_load(profile, &p, &llms_doc) != 0) return 1;

    /* resolve the flower binary: the runner spawns mcp servers from
       its own cwd, so a relative argv[0] would break */
    {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) exe[n] = '\0';
        else snprintf(exe, sizeof exe, "%s", argv0);
    }

    llmkit = getenv("FLOWER_LLMKIT");
    if (!llmkit || !llmkit[0]) llmkit = "llmkit";

    buf_init(&seed);
    buf_init(&input);

    /* --- seed: header, llm, system, tools, agent-as-tool --- */
    if (emit_record(&seed, json_parse("{\"type\":\"header\","
                                      "\"version\":1}")) != 0)
        goto oom;
    llm = llm_record(&p);
    if (!llm || emit_record(&seed, llm) != 0) goto oom;
    llm = NULL;
    sysrec = json_obj();
    if (!sysrec) goto oom;
    {
        json_t *content = json_arr();
        json_t *blk = json_obj();
        json_t *txt = json_str(prompt_system_prompts_scout);
        if (!content || !blk || !txt) {
            json_free(content);
            json_free(blk);
            json_free(txt);
            goto oom;
        }
        txt->key = str_dup("text");
        if (!txt->key) {
            json_free(txt);
            json_free(content);
            json_free(blk);
            goto oom;
        }
        json_add(blk, txt);
        if (obj_str(blk, "type", "text") != 0) {
            json_free(content);
            json_free(blk);
            goto oom;
        }
        json_add(content, blk);
        if (obj_str(sysrec, "type", "system") != 0) {
            json_free(content);
            goto oom;
        }
        content->key = str_dup("content");
        if (!content->key) {
            json_free(content);
            goto oom;
        }
        json_add(sysrec, content);
    }
    if (emit_record(&seed, sysrec) != 0) goto oom;
    sysrec = NULL;
    {
        buf_t cl;
        buf_init(&cl);
        if (sh_quote(&cl, llmkit) != 0 ||
            buf_appendf(&cl, " builtin-mcp --read-only") != 0) {
            buf_free(&cl);
            goto oom;
        }
        tools = json_obj();
        if (!tools) {
            buf_free(&cl);
            goto oom;
        }
        {
            json_t *arr = json_arr();
            json_t *srv = server_entry("scout-tools", cl.data, 0);
            if (!arr || !srv) {
                json_free(arr);
                json_free(srv);
                buf_free(&cl);
                goto oom;
            }
            json_add(arr, srv);
            if (obj_str(tools, "type", "tools") != 0) {
                json_free(arr);
                buf_free(&cl);
                goto oom;
            }
            arr->key = str_dup("tools");
            if (!arr->key) {
                json_free(arr);
                buf_free(&cl);
                goto oom;
            }
            json_add(tools, arr);
        }
        buf_free(&cl);
    }
    if (emit_record(&seed, tools) != 0) goto oom;
    tools = NULL;
    {
        json_t *aat = json_obj();
        if (!aat) goto oom;
        if (obj_str(aat, "type", "agent-as-tool") != 0 ||
            obj_str(aat, "tool_description",
                    prompt_mcp_scout_description) != 0 ||
            obj_str(aat, "input_description",
                    prompt_mcp_scout_arguments_input) != 0) {
            json_free(aat);
            goto oom;
        }
        if (emit_record(&seed, aat) != 0) goto oom;
    }

    /* write the seed under projects/<pid>/scan/ */
    seed_rel = xasprintf("projects/%s/scan/scout-seed.jsonl", pid);
    if (!seed_rel) goto oom;
    if (store_mkdir_p("projects") != 0) goto out; /* base may be fresh */
    {
        char *dir = xasprintf("projects/%s/scan", pid);
        if (!dir) goto oom;
        if (store_mkdir_p(dir) != 0) {
            free(dir);
            goto out;
        }
        free(dir);
    }
    seed_path = store_path(seed_rel);
    if (!seed_path) goto oom;
    if (write_private(seed_path, &seed) != 0) goto out;

    /* --- runner stdin: header, llm, tools; the stage records are
       appended as the scan progresses --- */
    if (emit_record(&input, json_parse("{\"type\":\"header\","
                                       "\"version\":1}")) != 0)
        goto oom;
    llm = llm_record(&p);
    if (!llm || emit_record(&input, llm) != 0) goto oom;
    llm = NULL;
    {
        json_t *tools_rec = json_obj();
        json_t *arr;
        if (!tools_rec) goto oom;
        arr = json_arr();
        if (!arr) {
            json_free(tools_rec);
            goto oom;
        }
        /* flower mcp servers */
        {
            static const char *areas[] = {"details", "context",
                                          "resource"};
            size_t a;
            for (a = 0; a < sizeof areas / sizeof areas[0]; a++) {
                buf_t cl;
                json_t *srv;
                buf_init(&cl);
                if (sh_quote(&cl, exe) != 0 ||
                    buf_appendf(&cl, " mcp %s %s", pid, areas[a]) != 0) {
                    buf_free(&cl);
                    json_free(arr);
                    json_free(tools_rec);
                    goto oom;
                }
                srv = server_entry(areas[a], cl.data, 1);
                buf_free(&cl);
                if (!srv) {
                    json_free(arr);
                    json_free(tools_rec);
                    goto oom;
                }
                json_add(arr, srv);
            }
        }
        /* the scout subagent */
        {
            buf_t cl;
            json_t *srv;
            buf_init(&cl);
            if (sh_quote(&cl, llmkit) != 0 ||
                buf_appendf(&cl, " agent-as-tool ") != 0 ||
                sh_quote(&cl, seed_path) != 0) {
                buf_free(&cl);
                json_free(arr);
                json_free(tools_rec);
                goto oom;
            }
            srv = server_entry("scout", cl.data, 1);
            buf_free(&cl);
            if (!srv) {
                json_free(arr);
                json_free(tools_rec);
                goto oom;
            }
            json_add(arr, srv);
        }
        /* profile mcp proxies */
        for (i = 0; i < p.nproxy; i++) {
            const char *cfg = p.proxies[i];
            char *abs;
            const char *slash = strrchr(cfg, '/');
            const char *base = slash ? slash + 1 : cfg;
            const char *dot = strrchr(base, '.');
            size_t nlen = dot ? (size_t)(dot - base) : strlen(base);
            char *nm;
            buf_t cl;
            json_t *srv;
            if (nlen == 0) {
                fprintf(stderr, "flower: profile '%s': Mcp proxy '%s' "
                                "yields an empty server name\n",
                        profile, cfg);
                json_free(arr);
                json_free(tools_rec);
                goto out;
            }
            nm = strndup(base, nlen);
            if (!nm) {
                json_free(arr);
                json_free(tools_rec);
                goto oom;
            }
            abs = cfg[0] == '/' ? str_dup(cfg)
                                : xasprintf("%s/%s", store_base(), cfg);
            if (!abs) {
                free(nm);
                json_free(arr);
                json_free(tools_rec);
                goto oom;
            }
            buf_init(&cl);
            if (sh_quote(&cl, llmkit) != 0 ||
                buf_appendf(&cl, " mcp-proxy ") != 0 ||
                sh_quote(&cl, abs) != 0) {
                free(nm);
                free(abs);
                buf_free(&cl);
                json_free(arr);
                json_free(tools_rec);
                goto oom;
            }
            srv = server_entry(nm, cl.data, 0);
            free(nm);
            free(abs);
            buf_free(&cl);
            if (!srv) {
                json_free(arr);
                json_free(tools_rec);
                goto oom;
            }
            json_add(arr, srv);
        }
        /* terminal tools: attach to the matching server entries */
        for (i = 0; i < p.nterminal; i++) {
            const char *tt = p.terminals[i];
            const char *dot = strchr(tt, '.');
            json_t *found = NULL;
            json_t *tarr;
            json_t *s;
            if (!dot || dot == tt || !dot[1]) {
                fprintf(stderr, "flower: profile '%s': Terminal tool "
                                "'%s' is not <server>.<tool>\n",
                        profile, tt);
                json_free(arr);
                json_free(tools_rec);
                goto out;
            }
            for (s = arr->child; s; s = s->next) {
                const char *nm = json_str_of(s, "name");
                size_t sl = (size_t)(dot - tt);
                if (nm && strlen(nm) == sl && strncmp(nm, tt, sl) == 0) {
                    found = s;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "flower: profile '%s': Terminal tool "
                                "'%s' names no server in this scan\n",
                        profile, tt);
                json_free(arr);
                json_free(tools_rec);
                goto out;
            }
            tarr = (json_t *)json_get(found, "terminal_tools");
            if (!tarr) {
                tarr = json_arr();
                if (!tarr) {
                    json_free(arr);
                    json_free(tools_rec);
                    goto oom;
                }
                tarr->key = str_dup("terminal_tools");
                if (!tarr->key) {
                    json_free(tarr);
                    json_free(arr);
                    json_free(tools_rec);
                    goto oom;
                }
                json_add(found, tarr);
            }
            {
                json_t *one = json_str(dot + 1);
                if (!one) {
                    json_free(arr);
                    json_free(tools_rec);
                    goto oom;
                }
                json_add(tarr, one);
            }
        }
        if (obj_str(tools_rec, "type", "tools") != 0) {
            json_free(arr);
            json_free(tools_rec);
            goto oom;
        }
        arr->key = str_dup("tools");
        if (!arr->key) {
            json_free(arr);
            json_free(tools_rec);
            goto oom;
        }
        json_add(tools_rec, arr);
        if (emit_record(&input, tools_rec) != 0) goto oom;
    }
    /* stage 1 records are appended as the scan progresses */
    if (dry_run) {
        int s;
        for (s = 0; s < 3; s++) {
            char *text = stage_prompt_text(pid, &scan_stages[s]);
            if (!text) goto oom;
            if (stage_records(&input, &scan_stages[s], text) != 0) {
                free(text);
                goto oom;
            }
            free(text);
        }
        fprintf(stderr, "flower: seed: %s\n", seed_path);
        fwrite(input.data, 1, input.len, stdout);
        rc = ferror(stdout) ? 1 : 0;
        goto out;
    }

    /* spawn <llmkit> runner with a pipe as stdin; the parent keeps the
       write end and feeds the records stage by stage: header/llm/tools
       first, then per stage system + user + flush, waiting for the
       [STEP_COMPLETE] marker before the next one. the runner's stdout
       is teed to flower's stdout and the session transcript. */
    {
        pid_t kid;
        int st;
        int inpipe[2];
        int outp[2];
        FILE *sess = NULL;
        char *sess_path = NULL;
        int skipped_header = 0;
        FILE *rp = NULL;
        char *line = NULL;
        size_t cap = 0;
        int s, rc2 = 0;

        if (pipe(inpipe) != 0 || pipe(outp) != 0) {
            fprintf(stderr, "flower: pipe: %s\n", strerror(errno));
            if (inpipe[0] >= 0) {
                close(inpipe[0]);
                close(inpipe[1]);
            }
            goto out;
        }
        fflush(stdout);
        kid = fork();
        if (kid < 0) {
            fprintf(stderr, "flower: fork: %s\n", strerror(errno));
            close(inpipe[0]);
            close(inpipe[1]);
            close(outp[0]);
            close(outp[1]);
            goto out;
        }
        if (kid == 0) {
            if (dup2(inpipe[0], STDIN_FILENO) < 0 ||
                dup2(outp[1], STDOUT_FILENO) < 0)
                _exit(127);
            close(inpipe[0]);
            close(inpipe[1]);
            close(outp[0]);
            close(outp[1]);
            execl(llmkit, "llmkit", "runner", (char *)NULL);
            _exit(127);
        }
        close(inpipe[0]);
        close(outp[1]);
        /* the session transcript: records appended as they are sent to
           the runner's stdin, then the runner output line by line
           (minus its duplicate header). 0600: the llm record carries
           the api key. */
        sess_path = session_open(pid);
        if (sess_path) {
            sess = fopen(sess_path, "wb");
            if (!sess) {
                fprintf(stderr, "flower: cannot open %s: %s\n", sess_path,
                        strerror(errno));
                free(sess_path);
                sess_path = NULL;
            }
        }
        rp = fdopen(outp[0], "r");
        if (!rp) {
            fprintf(stderr, "flower: cannot open runner output: %s\n",
                    strerror(errno));
            close(outp[0]);
        }

        /* feed the initial records, then run the three stages */
        if (send_records(inpipe[1], sess, &input) != 0) rc2 = -1;
        if (!rp) rc2 = -1; /* outp[0] is closed in the cleanup below */
        for (s = 0; s < 3 && rc2 == 0; s++) {
            char *text = stage_prompt_text(pid, &scan_stages[s]);
            int nudges = 0;
            if (!text) {
                rc2 = -1;
                break;
            }
            if (stage_records(&input, &scan_stages[s], text) != 0 ||
                send_records(inpipe[1], sess, &input) != 0) {
                free(text);
                rc2 = -1;
                break;
            }
            free(text);
            for (;;) {
                int w = wait_stage_end(rp, &line, &cap, sess,
                                       &skipped_header);
                if (w == 1) break; /* marker: stage done */
                if (w < 0 || w == 2) {
                    rc2 = -1;
                    break;
                }
                /* the turn ended without the marker: nudge once */
                if (nudges++ >= 1) {
                    rc2 = -1;
                    break;
                }
                if (nudge_records(&input) != 0 ||
                    send_records(inpipe[1], sess, &input) != 0)
                    rc2 = -1;
            }
        }
        close(inpipe[1]); /* EOF: the runner's stdin is done */
        free(line);
        if (rp) fclose(rp);
        else close(outp[0]);
        if (sess) {
            if (fclose(sess) != 0)
                fprintf(stderr, "flower: session write error\n");
            else
                fprintf(stderr, "flower: session: %s\n", sess_path);
        }
        free(sess_path);
        if (waitpid(kid, &st, 0) < 0) {
            fprintf(stderr, "flower: waitpid: %s\n", strerror(errno));
            rc = 1;
        } else if (WIFEXITED(st)) {
            int code = WEXITSTATUS(st);
            if (code == 127)
                fprintf(stderr, "flower: cannot run '%s runner' - set "
                                "FLOWER_LLMKIT to the llmkit binary\n",
                        llmkit);
            /* a nonzero runner exit wins; a clean exit with unfinished
               stages is still a failure */
            rc = code == 127 ? 1 : (code != 0 ? code
                                              : (rc2 < 0 ? 1 : 0));
        } else {
            rc = 1;
        }
    }

out:
    free(seed_rel);
    free(seed_path);
    buf_free(&seed);
    buf_free(&input);
    json_free(llm);
    json_free(tools);
    json_free(sysrec);
    md_free_doc(&llms_doc);
    profile_free(&p);
    return rc;

oom:
    fprintf(stderr, "flower: out of memory\n");
    rc = 1;
    goto out;
}
