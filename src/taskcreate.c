/* taskcreate.c - flower project [<P>] task create [<profile>]
   [--dry-run]: ask the user for a task name + description, create the
   task directory and its config.md, then run one llmkit runner session
   in two stages: stage 1 fills the new task's context.md (through the
   task-scoped context tools and the readonly scout subagent), stage 2
   plans the action hierarchy in actions.md. same session shape as
   scan.c; the shared plumbing lives in llmrun.c. design: plan.md,
   llmkit records: llmkit/docs/records.md. */
#define _POSIX_C_SOURCE 200809L
#include "flower.h"
#include "prompts.gen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int cmd_task_create(const char *pid, const char *profile, int dry_run,
                    const char *argv0);

/* ---------- interactive prompts ---------- */

/* one required text prompt on stdin: retried on an empty line, eof
   aborts. returns 1 with the trimmed answer in out, 0 on eof (a
   message has been printed). */
static int ask_required(const char *prompt, char *out, size_t outn) {
    for (;;) {
        int eof = 0;
        /* stderr: the dry-run dumps its jsonl to stdout, a prompt
           there would corrupt it */
        fprintf(stderr, "%s: ", prompt);
        fflush(stderr);
        if (!fgets(out, (int)outn, stdin)) {
            eof = 1;
            out[0] = '\0';
        } else {
            str_trim(out);
        }
        if (eof || !out[0]) {
            if (eof || feof(stdin)) {
                fprintf(stderr, "flower: aborted\n");
                return 0;
            }
            /* empty line: ask again */
            continue;
        }
        return 1;
    }
}

/* ---------- profile default ---------- */

/* pick the profile when none was passed: the only chapter title in
   llms.md. returns a malloc'd name or NULL after printing why. */
static char *default_profile(void) {
    md_doc_t doc;
    char *name = NULL;
    if (load_rel("llms.md", FV_LLM, &doc) != 0) return NULL;
    if (doc.nch == 0)
        fprintf(stderr, "flower: no llm profiles configured (add one "
                        "with 'flower llm add')\n");
    else if (doc.nch == 1) {
        name = str_dup(doc.ch[0].title);
        if (!name) oom();
    } else {
        buf_t b;
        int i;
        buf_init(&b);
        for (i = 0; i < doc.nch; i++) {
            if (buf_appendf(&b, "%s%s", i ? ", " : "",
                            doc.ch[i].title) != 0) {
                buf_free(&b);
                md_free_doc(&doc);
                oom();
                return NULL;
            }
        }
        fprintf(stderr, "flower: multiple llm profiles, pass one of: "
                        "%s\n",
                b.data ? b.data : "");
        buf_free(&b);
    }
    md_free_doc(&doc);
    return name;
}

/* ---------- task scaffolding ---------- */

/* create projects/<pid>/tasks/<tid> and its config.md (# Task with
   Name + Description). 0 ok, 1 error printed. tid_out receives a
   malloc'd id. */
static int create_task_dir(const char *pid, const char *name,
                           const char *description, char **tid_out) {
    md_doc_t doc;
    md_chapter_t *c;
    char *tid, *rel_dir, *rel_file, *path;
    int rc;

    tid = store_next_task_id(pid);
    if (!tid) return oom();
    rel_dir = xasprintf("projects/%s/tasks/%s", pid, tid);
    rel_file = xasprintf("projects/%s/tasks/%s/config.md", pid, tid);
    if (!rel_dir || !rel_file) {
        free(rel_dir);
        free(rel_file);
        free(tid);
        return oom();
    }
    memset(&doc, 0, sizeof doc); /* fresh doc: nothing to read */
    c = doc_add_chapter(&doc, "Task", 1);
    if (!c || doc_append_kv(c, "Name", name) != 0 ||
        doc_append_kv(c, "Description", description) != 0) {
        md_free_doc(&doc);
        free(rel_dir);
        free(rel_file);
        free(tid);
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
    if (rc == 0) {
        fprintf(stderr, "flower: created task %s\n", tid);
        *tid_out = tid;
        tid = NULL;
    }
out:
    free(tid);
    free(rel_dir);
    free(rel_file);
    md_free_doc(&doc);
    return rc;
}

/* ---------- stage prompts ---------- */

/* project tree and task identity: set by cmd_task_create, embedded in
   every stage system prompt */
static const char *g_proj_path = "";
static const char *g_task_name = "";
static const char *g_task_desc = "";

/* the identity of the task under study, embedded in every stage
   system prompt. 0 ok, -1 oom */
static int append_task_identity(buf_t *b, const char *tid,
                                const char *name,
                                const char *description) {
    return buf_appendf(b, "\n## The task\n"
                          "- id: %s\n"
                          "- name: %s\n"
                          "- description: %s\n",
                       tid, name, description) != 0
               ? -1
               : 0;
}

/* one stage's system prompt text: shared preamble + stage
   instructions + the project path line + the task identity + the
   files embedded as sections, headers shifted one level deeper.
   NULL oom */
static char *stage_prompt_text(const char *pid, const char *tid,
                               int stage) {
    buf_t b;
    char *rel;
    buf_init(&b);
    if (buf_appendf(&b, "%s\n%s\nthe project under study is at %s; "
                        "explore that tree through scout, not the "
                        "runner cwd\n",
                    prompt_system_prompts_taskcreate_preamble,
                    stage == 0
                        ? prompt_system_prompts_taskcreate_stage1_context
                        : prompt_system_prompts_taskcreate_stage2_actions,
                    g_proj_path[0] ? g_proj_path : "<unset>") != 0)
        goto fail;
    if (append_task_identity(&b, tid, g_task_name,
                             g_task_desc) != 0)
        goto fail;
    if (stage == 0) {
        static const char *files[] = {
            "# Project config.md", "config.md",
            "# Project details.md", "details.md",
            "# Project context.md", "context.md",
            "# Project resources.md", "resources.md",
        };
        size_t f;
        for (f = 0; f < sizeof files / sizeof files[0]; f += 2) {
            if (buf_appendf(&b, "\n%s\n", files[f]) != 0) goto fail;
            rel = xasprintf("projects/%s/%s", pid, files[f + 1]);
            if (!rel) goto fail;
            if (append_file(&b, rel) != 0) {
                free(rel);
                goto fail;
            }
            free(rel);
        }
        if (buf_appendf(&b, "\n# Current tasks/%s/config.md\n",
                        tid) != 0)
            goto fail;
        rel = xasprintf("projects/%s/tasks/%s/config.md", pid, tid);
        if (!rel) goto fail;
        if (append_file(&b, rel) != 0) {
            free(rel);
            goto fail;
        }
        free(rel);
        if (buf_appendf(&b, "\n# Current tasks/%s/context.md\n",
                        tid) != 0)
            goto fail;
        rel = xasprintf("projects/%s/tasks/%s/context.md", pid, tid);
        if (!rel) goto fail;
        if (append_file(&b, rel) != 0) {
            free(rel);
            goto fail;
        }
        free(rel);
    } else {
        if (buf_appendf(&b, "\n# Current tasks/%s/context.md\n",
                        tid) != 0)
            goto fail;
        rel = xasprintf("projects/%s/tasks/%s/context.md", pid, tid);
        if (!rel) goto fail;
        if (append_file(&b, rel) != 0) {
            free(rel);
            goto fail;
        }
        free(rel);
        if (buf_appendf(&b, "\n# Current actions.md\n") != 0)
            goto fail;
        rel = xasprintf("projects/%s/tasks/%s/actions.md", pid, tid);
        if (!rel) goto fail;
        if (append_file(&b, rel) != 0) {
            free(rel);
            goto fail;
        }
        free(rel);
    }
    return b.data;
fail:
    buf_free(&b);
    return NULL;
}

/* append one stage's records to out: system prompt, kickoff user
   message, then the flush that starts the stage. 0 ok, -1 oom */
static int stage_records(buf_t *out, int stage, const char *text) {
    json_t *rec;
    rec = llmrun_system_record(text);
    if (!rec) return -1;
    if (emit_record(out, rec) != 0) return -1;
    rec = llmrun_user_record(stage == 0
                                 ? prompt_system_prompts_taskcreate_stage1_start
                                 : prompt_system_prompts_taskcreate_stage2_start);
    if (!rec) return -1;
    if (emit_record(out, rec) != 0) return -1;
    if (emit_record(out, json_parse("{\"type\":\"flush\"}")) != 0)
        return -1;
    return 0;
}

/* the taskcreate nudge + flush: llmrun_nudge_records still carries
   the scan text. 0 ok, -1 oom */
static int nudge_records(buf_t *out) {
    json_t *rec = llmrun_user_record(
        prompt_system_prompts_taskcreate_nudge);
    if (!rec) return -1;
    if (emit_record(out, rec) != 0) return -1;
    if (emit_record(out, json_parse("{\"type\":\"flush\"}")) != 0)
        return -1;
    return 0;
}

/* ---------- main ---------- */

int cmd_task_create(const char *pid, const char *profile, int dry_run,
                    const char *argv0) {
    profile_t p;
    md_doc_t llms_doc; /* holds the borrowed profile strings */
    char name[512], desc[4096];
    char proj_path[4096] = "";
    char exe[4096]; /* absolute self path for mcp server spawns */
    char *profile_owned = NULL, *tid = NULL;
    char *seed_rel = NULL, *seed_path = NULL;
    char *scout_name = NULL; /* scout subagent profile override */
    profile_t sp;
    md_doc_t scout_doc;
    int have_scout = 0;
    buf_t seed, input;
    json_t *llm = NULL, *tools = NULL, *sysrec = NULL;
    const char *llmkit;
    int rc = 1;
    int i;

    /* init up front: the oom/out paths below run before the buffers
       hold anything, and buf_free on a zeroed buf is a no-op */
    buf_init(&seed);
    buf_init(&input);

    if (!require_project(pid)) return 1;

    /* profile default: the only chapter in llms.md */
    if (!profile) {
        profile_owned = default_profile();
        if (!profile_owned) return 1;
        profile = profile_owned;
    }

    /* resolve and validate the profile before asking anything: a bad
       profile should fail fast, not after the user typed a task */
    if (profile_load(profile, &p, &llms_doc) != 0) {
        free(profile_owned);
        return 1;
    }

    /* the scout subagent may run a different profile: resolve it
       here; NULL means the session profile for both */
    scout_name = store_scout_profile();
    if (scout_name && strcmp(scout_name, profile) != 0) {
        if (profile_load(scout_name, &sp, &scout_doc) != 0) {
            free(scout_name);
            free(profile_owned);
            return 1;
        }
        have_scout = 1;
    }

    if (!ask_required("Task name", name, sizeof name)) {
        free(profile_owned);
        return 1;
    }
    if (!ask_required("Task description", desc, sizeof desc)) {
        free(profile_owned);
        return 1;
    }

    /* the project tree the agent and its scout subagent must explore:
       read from projects/<pid>/config.md so the runner cwd (the flower
       repo) is never mistaken for the target project */
    {
        md_doc_t cfg;
        md_chapter_t *c;
        char *cfg_rel;
        int j;
        cfg_rel = xasprintf("projects/%s/config.md", pid);
        if (!cfg_rel) {
            free(profile_owned);
            goto oom;
        }
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
    g_proj_path = proj_path;
    g_task_name = name;
    g_task_desc = desc;

    /* create the task before the session: the mcp task servers need
       the directory on disk */
    if (create_task_dir(pid, name, desc, &tid) != 0) goto out;

    /* resolve the flower binary: the runner spawns mcp servers from
       its own cwd, so a relative argv[0] would break */
    {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) exe[n] = '\0';
        else snprintf(exe, sizeof exe, "%s", argv0);
    }

    llmkit = getenv("FLOWER_LLMKIT");
    if (!llmkit || !llmkit[0]) llmkit = "llmkit";

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
       run also names a transcript file: the subagent's llmkit tees
       its whole conversation there (seed records first), same rules
       as the main session. dry-run leaves it unset - nothing runs */
    {
        json_t *opts = json_parse("{\"type\":\"options\","
                                  "\"retain_context\":true}");
        if (!opts) goto oom;
        if (!dry_run) {
            char *scout_path = session_open("taskcreate", pid, "-scout");
            if (!scout_path) goto out;
            /* note the path for the operator: the helper does not tee
               its conversation (llmkit agent-as-tool has no transcript
               option - an unknown option kills the server at spawn) */
            fprintf(stderr, "flower: scout transcript is not supported "
                            "by llmkit agent-as-tool; sessions at %s\n",
                    scout_path);
            free(scout_path);
        }
        if (emit_record(&seed, opts) != 0) {
            json_free(opts);
            goto oom;
        }
    }

    /* write the seed under projects/<pid>/tasks/<tid>/ */
    seed_rel = xasprintf("projects/%s/tasks/%s/scout-seed.jsonl",
                         pid, tid);
    if (!seed_rel) goto oom;
    {
        char *dir = xasprintf("projects/%s/tasks/%s", pid, tid);
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
       appended as the session progresses --- */
    if (emit_record(&input, json_parse("{\"type\":\"header\","
                                       "\"version\":1}")) != 0)
        goto oom;
    llm = llm_record(&p);
    if (!llm || emit_record(&input, llm) != 0) goto oom;
    llm = NULL;
    /* scout calls can take minutes (a small model behind a slow
       endpoint): a generous tool budget keeps one call alive without
       making the stage fatal - the session-level time budget is the
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
        /* flower mcp servers: project visibility (read-only per the
           preamble) and the task files this session writes */
        {
            static const char *proj_areas[] = {"details", "context",
                                               "resource"};
            static const char *proj_names[] = {"project-details",
                                               "project-context",
                                               "project-resource"};
            static const char *task_areas[] = {"config", "context",
                                               "action"};
            static const char *task_names[] = {"task-config",
                                               "task-context",
                                               "task-action"};
            size_t a;
            for (a = 0; a < 3; a++) {
                buf_t cl;
                json_t *srv;
                buf_init(&cl);
                if (sh_quote(&cl, exe) != 0 ||
                    buf_appendf(&cl, " mcp %s %s", pid,
                                proj_areas[a]) != 0) {
                    buf_free(&cl);
                    json_free(arr);
                    json_free(tools_rec);
                    goto oom;
                }
                srv = llmrun_server_entry(proj_names[a], cl.data, 1);
                buf_free(&cl);
                if (!srv) {
                    json_free(arr);
                    json_free(tools_rec);
                    goto oom;
                }
                json_add(arr, srv);
            }
            for (a = 0; a < 3; a++) {
                buf_t cl;
                json_t *srv;
                buf_init(&cl);
                if (sh_quote(&cl, exe) != 0 ||
                    buf_appendf(&cl, " mcp %s task %s %s", pid, tid,
                                task_areas[a]) != 0) {
                    buf_free(&cl);
                    json_free(arr);
                    json_free(tools_rec);
                    goto oom;
                }
                srv = llmrun_server_entry(task_names[a], cl.data, 1);
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
                                "'%s' names no server in this session\n",
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

    /* stage records are appended as the session progresses */
    if (dry_run) {
        int s;
        for (s = 0; s < 2; s++) {
            char *text = stage_prompt_text(pid, tid, s);
            if (!text) goto oom;
            if (stage_records(&input, s, text) != 0) {
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

    /* the live session: one <llmkit> runner per turn. the runner exits
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
        sess_path = session_open("taskcreate", pid, "");
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

        for (s = 0; s < 2 && !failed; s++) {
            char *text = stage_prompt_text(pid, tid, s);
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
            if (stage_records(&turn, s, text) != 0 ||
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
                    nudge_records(&turn) != 0) {
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
        free(sess_path);
        buf_free(&hist);
        if (failed && rc == 0) rc = 1;
        if (!failed) rc = 0;
    }

out:
    free(profile_owned);
    free(tid);
    free(seed_rel);
    free(seed_path);
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
