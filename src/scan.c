/* scan.c - flower project <P> scan <profile>: drive a staged llmkit
   runner session that scans the project and fills details.md,
   context.md and resources.md through the flower mcp servers. three
   stages (details, context, resources), one system prompt and one user
   kickoff each: the stage records are written to the runner's stdin as
   it runs, a flush record starts each stage, and the stage ends when
   [STEP_COMPLETE] closes its final response. the shared runner session
   plumbing lives in llmrun.c. design: plan.md, llmkit records:
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

int cmd_scan(const char *pid, const char *profile, int dry_run,
             const char *argv0);

/* ---------- scan stages ---------- */

/* project tree under study: set by cmd_scan from config.md, embedded
   in every stage system prompt */
static const char *scan_proj_path = "";

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
        buf_appendf(&b, "\nthe project under study is at %s; explore "
                        "that tree through scout, not the runner cwd\n",
                    scan_proj_path[0] ? scan_proj_path : "<unset>") != 0 ||
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

/* append one stage's records to out: system prompt, kickoff user
   message, then the flush that starts the stage (a flush is only legal
   with new records pending - llmkit rule). 0 ok, -1 oom */
static int stage_records(buf_t *out, const scan_stage_t *st,
                         const char *text) {
    json_t *rec;
    rec = llmrun_system_record(text);
    if (!rec) return -1;
    if (emit_record(out, rec) != 0) return -1;
    rec = llmrun_user_record(st->kickoff);
    if (!rec) return -1;
    if (emit_record(out, rec) != 0) return -1;
    if (emit_record(out, json_parse("{\"type\":\"flush\"}")) != 0)
        return -1;
    return 0;
}


/* scan creates a missing project on the fly: config.md gets Name from
   the current directory name and Path from getcwd(3), so scanning a
   repo is one command with no project setup step. 0 ok, 1 error
   printed (io or oom). */
static int scan_ensure_project(const char *pid) {
    char cwd[8192];
    const char *base;
    md_doc_t doc;
    md_chapter_t *c;
    char *rel_dir, *rel_file, *path;
    int rc;

    rel_dir = xasprintf("projects/%s", pid);
    if (!rel_dir) return oom();
    if (rel_dir_exists(rel_dir)) {
        free(rel_dir);
        return 0;
    }

    if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, "flower: cannot get current directory: %s\n",
                strerror(errno));
        free(rel_dir);
        return 1;
    }
    base = strrchr(cwd, '/');
    base = base ? base + 1 : cwd;
    if (!*base) base = "root"; /* cwd == "/" */

    memset(&doc, 0, sizeof doc); /* fresh doc: no file read needed */
    c = doc_add_chapter(&doc, "Project", 1);
    if (!c || doc_append_kv(c, "Name", base) != 0 ||
        doc_append_kv(c, "Path", cwd) != 0) {
        md_free_doc(&doc);
        free(rel_dir);
        return oom();
    }
    rel_file = xasprintf("projects/%s/config.md", pid);
    if (!rel_file) {
        md_free_doc(&doc);
        free(rel_dir);
        return oom();
    }
    if (store_mkdir_p(rel_dir) != 0) {
        rc = 1;
        goto out;
    }
    path = store_path(rel_file);
    if (!path) {
        rc = oom();
        goto out;
    }
    rc = md_save(path, &doc) != 0 ? 1 : 0;
    free(path);
    if (rc == 0)
        fprintf(stderr, "flower: created project %s (%s)\n", pid, cwd);
out:
    free(rel_file);
    free(rel_dir);
    md_free_doc(&doc);
    return rc;
}

int cmd_scan(const char *pid, const char *profile, int dry_run,
             const char *argv0) {
    profile_t p;
    md_doc_t llms_doc; /* holds the borrowed profile strings */
    char exe[4096];    /* absolute self path for mcp server spawns */
    char *seed_rel = NULL, *seed_path = NULL, *scout_path = NULL;
    char *scout_name = NULL; /* scout subagent profile override */
    profile_t sp;
    md_doc_t scout_doc;
    int have_scout = 0;
    buf_t seed, input;
    json_t *llm = NULL, *tools = NULL, *sysrec = NULL;
    const char *llmkit;
    int rc = 1;
    int i;

    if (scan_ensure_project(pid) != 0) return 1;
    if (profile_load(profile, &p, &llms_doc) != 0) return 1;

    /* the scout subagent may run a different profile: resolve it
       here; NULL means the session profile for both */
    scout_name = store_scout_profile();
    if (scout_name && strcmp(scout_name, profile) != 0) {
        if (profile_load(scout_name, &sp, &scout_doc) != 0) {
            free(scout_name);
            return 1;
        }
        have_scout = 1;
    }

    /* the project tree the agent and its scout subagent must explore:
       read from projects/<pid>/config.md so the runner cwd (the flower
       repo) is never mistaken for the target project */
    char proj_path[4096] = "";
    {
        md_doc_t cfg;
        md_chapter_t *c;
        char *cfg_rel;
        int j;
        cfg_rel = xasprintf("projects/%s/config.md", pid);
        if (!cfg_rel) return oom();
        if (load_rel(cfg_rel, FV_PROJECT, &cfg) != 0) cfg.nch = 0;
        free(cfg_rel);
        c = md_find(&cfg, "Project");
        if (c)
            for (j = 0; j < c->nkv; j++)
                if (!strcmp(c->keys[j], "Path")) {
                    snprintf(proj_path, sizeof proj_path, "%s",
                             c->vals[j]);
                    break;
                }
        md_free_doc(&cfg);
    }
    scan_proj_path = proj_path;

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
    llm = llm_record(have_scout ? &sp : &p);
    if (!llm || emit_record(&seed, llm) != 0) goto oom;
    llm = NULL;
    sysrec = json_obj();
    if (!sysrec) goto oom;
    {
        char *scout_text = xasprintf("%s\nthe project under study is at "
                                     "%s: use paths under it, never at "
                                     "the runner cwd",
                                     prompt_system_prompts_scout,
                                     proj_path[0] ? proj_path : "the "
                                     "configured project path");
        json_t *content = json_arr();
        json_t *blk = json_obj();
        json_t *txt = scout_text ? json_str(scout_text)
                                 : json_str(prompt_system_prompts_scout);
        free(scout_text);
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
            json_t *srv = llmrun_server_entry("scout-tools", cl.data, 0);
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
    /* one growing scout conversation: invoke keeps the context it
       gained in earlier stages (plus a context_reset tool). the live
       scan also names a transcript file: the subagent's llmkit tees
       its whole conversation there (seed records first), same rules
       as the main session. dry-run leaves it unset - nothing runs */
    {
        json_t *opts = json_parse("{\"type\":\"options\","
                                  "\"retain_context\":true}");
        if (!opts) goto oom;
        if (!dry_run) {
            scout_path = session_open("scan", pid, "-scout");
            if (!scout_path) goto out;
            /* note the path for the operator: the helper does not tee
               its conversation (llmkit agent-as-tool has no transcript
               option - an unknown option kills the server at spawn) */
            fprintf(stderr, "flower: scout transcript is not supported "
                            "by llmkit agent-as-tool; sessions at %s\n",
                    scout_path);
            free(scout_path);
            scout_path = NULL;
        }
        if (emit_record(&seed, opts) != 0) {
            json_free(opts);
            goto oom;
        }
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
    /* scout calls can take minutes (a small model behind a slow
       endpoint): a generous tool budget keeps one call alive without
       making the stage fatal - the scan-level time budget is the
       operator's watchdog, not a runner kill switch */
    if (emit_record(&input,
                    json_parse("{\"type\":\"options\","
                               "\"tool_call_timeout\":240}")) != 0)
        goto oom;
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
                srv = llmrun_server_entry(areas[a], cl.data, 1);
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
            srv = llmrun_server_entry("scout", cl.data, 1);
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
            srv = llmrun_server_entry(nm, cl.data, 0);
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

    /* the live scan: one <llmkit> runner per turn. the runner exits
       after a final response instead of waiting for steering, so each
       turn is a fresh runner fed the whole transcript so far plus this
       stage's records and a flush, then eof. every output line is
       teed to stdout, the session transcript and the transcript fed
       to the next turn, keeping the session a valid replayable jsonl:
       input records in send order, then output records. a turn that
       ends without [STEP_COMPLETE] gets one nudge retry. */
    {
        buf_t hist;
        FILE *sess = NULL;
        char *sess_path = NULL;
        int s;
        int failed = 0;

        buf_init(&hist);
        /* input currently holds header, llm, tools: the turn 1 prefix.
           the session file mirrors every record in hist plus runner
           output, 0600: it carries the api key */
        sess_path = session_open("scan", pid, "");
        if (!sess_path) failed = 1;
        else {
            sess = fopen(sess_path, "wb");
            if (!sess) {
                fprintf(stderr, "flower: cannot open %s: %s\n", sess_path,
                        strerror(errno));
                free(sess_path);
                sess_path = NULL;
                failed = 1;
            }
        }
        /* write the prefix records into hist and the session */
        if (!failed && fwrite(input.data, 1, input.len, sess) != input.len) {
            fprintf(stderr, "flower: session write error\n");
            failed = 1;
        }
        if (!failed && buf_append(&hist, input.data, input.len) != 0)
            failed = oom();

        for (s = 0; s < 3 && !failed; s++) {
            char *text = stage_prompt_text(pid, &scan_stages[s]);
            buf_t turn;      /* hist + this stage's records */
            int nudges = 0;
            int done = 0;
            if (!text) {
                failed = oom();
                break;
            }
            buf_init(&turn);
            if (buf_append(&turn, hist.data, hist.len) != 0) {
                buf_free(&turn);
                free(text);
                failed = oom();
                break;
            }
            if (stage_records(&turn, &scan_stages[s], text) != 0 ||
                fwrite(turn.data + hist.len, 1, turn.len - hist.len,
                       sess) != turn.len - hist.len) {
                buf_free(&turn);
                free(text);
                if (!failed) failed = oom();
                break;
            }
            free(text);
            for (;;) {
                int w = run_turn(llmkit, &turn, sess, &hist);
                int code;
                if (w == -2) {
                    fprintf(stderr, "flower: cannot run '%s runner' - set "
                                    "FLOWER_LLMKIT to the llmkit binary\n",
                            llmkit);
                    failed = 1;
                    break;
                }
                if (w < 0) {
                    failed = 1;
                    break;
                }
                code = WEXITSTATUS(w);
                if (code != 0) {
                    /* the runner's error records are already on stdout
                       and in the session; its exit code wins */
                    rc = code;
                    failed = 1;
                    break;
                }
                {
                    int fin = llmrun_hist_ends_complete(&hist);
                    if (fin < 0) {
                        failed = 1;
                        break;
                    }
                    if (fin) {
                        done = 1;
                        break;
                    }
                }
                /* turn ended without the marker: nudge once */
                if (nudges++ >= 1) {
                    fprintf(stderr, "flower: stage %d ended without "
                                    "[STEP_COMPLETE]\n", s + 1);
                    failed = 1;
                    break;
                }
                buf_reset(&turn);
                if (buf_append(&turn, hist.data, hist.len) != 0 ||
                    llmrun_nudge_records(&turn) != 0) {
                    failed = oom();
                    break;
                }
                if (fwrite(turn.data + hist.len, 1, turn.len - hist.len,
                           sess) != turn.len - hist.len) {
                    fprintf(stderr, "flower: session write error\n");
                    failed = 1;
                    break;
                }
            }
            buf_free(&turn);
            (void)done;
        }
        if (sess) {
            if (fclose(sess) != 0)
                fprintf(stderr, "flower: session write error\n");
            else
                fprintf(stderr, "flower: session: %s\n", sess_path);
        }
        /* the scout tee is written by the subagent's own llmkit
           process; only report the path here, the file may hold just
           the seed when the scan never invoked scout */
        if (scout_path)
            fprintf(stderr, "flower: scout transcript: %s\n", scout_path);
        free(sess_path);
        buf_free(&hist);
        if (failed && rc == 0) rc = 1;
        if (!failed) rc = 0;
    }

out:
    free(seed_rel);
    free(seed_path);
    free(scout_path);
    buf_free(&seed);
    buf_free(&input);
    json_free(llm);
    json_free(tools);
    json_free(sysrec);
    md_free_doc(&llms_doc);
    profile_free(&p);
    if (have_scout) {
        profile_free(&sp);
        md_free_doc(&scout_doc);
    }
    free(scout_name);
    return rc;

oom:
    fprintf(stderr, "flower: out of memory\n");
    rc = 1;
    goto out;
}
