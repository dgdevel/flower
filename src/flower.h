/* flower.h - single public header: buffer, string utils, cli parser,
   update/crash-report decls. one include for the whole src/ tree. */
#ifndef FLOWER_H
#define FLOWER_H

#include <stddef.h>

/* release builds stamp this: tools/release.sh passes VERSION=x.y.z to make */
#ifndef FLOWER_VERSION
#define FLOWER_VERSION "0.0.1"
#endif

/* growable byte buffer. always NUL-terminates so buf.data doubles as a
   c string when the content has no embedded NULs. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
/* append n bytes; returns 0 ok, -1 oom (buffer stays valid) */
int buf_append(buf_t *b, const void *data, size_t n);
/* printf-style append: vsnprintf(NULL,0) for the size, grow, vsnprintf
   again (two-pass). returns 0 ok, -1 oom or format error */
int buf_appendf(buf_t *b, const char *fmt, ...);
void buf_reset(buf_t *b);

/* string utils (strutil.c), all byte-oriented and locale-unsafe by design */
/* trim leading+trailing isspace in place, returns s */
char *str_trim(char *s);
/* 1 if s starts with pfx */
int str_starts(const char *s, const char *pfx);
/* malloc'd copy of s, NULL on oom */
char *str_dup(const char *s);

/* markdown config chapters (md.c), format: docs/config_markdown_format.md */
typedef struct md_line {
    char *text; /* owned line without newline */
} md_line_t;

typedef struct md_chapter {
    char *title; /* owned, header text trimmed */
    int level;   /* 1..6 */
    char **keys; /* owned array of owned key strings (may be NULL) */
    char **vals; /* parallel array of owned values */
    int nkv;     /* number of key-value pairs */
    md_line_t *body; /* bare text lines when nkv==0, owned */
    int nbody;   /* number of body lines */
} md_chapter_t;

typedef struct md_doc {
    md_chapter_t *ch; /* flat list in file order */
    int nch;
} md_doc_t;

/* parse whole file content (NUL-terminated). returns 0 ok, -1 on malloc
   failure, -2 on duplicate chapter title at the same level, -3 malformed
   (header level jump > +1, or indented header). sets *out on success,
   zeroes it on failure. blank lines are not stored; once a chapter holds
   a key-value pair its bare lines are discarded. kv lines are "Key:
   value" at column 0, key [A-Za-z][A-Za-z0-9 _-]*, value = rest after
   the first ": " trimmed. anything else non-blank is a body line. */
int md_parse(const char *content, md_doc_t *out);

/* find chapter by exact title (case-sensitive). NULL if missing */
md_chapter_t *md_find(md_doc_t *doc, const char *title);

/* append/update key value in chapter. returns 0 ok, -1 oom */
int md_kv_set(md_chapter_t *c, const char *key, const char *val);

/* find value for key in chapter. NULL if missing */
const char *md_kv_get(const char **keys, const char **vals, int nkv,
                      const char *key);

/* free all chapters and zero the doc */
void md_free_doc(md_doc_t *doc);

/* serialize doc to buf: "# Title" header, "Key: value" lines (body lines
   when the chapter has none), chapters separated by one blank line,
   trailing newline. returns 0 ok, -1 oom. doc with 0 chapters -> empty
   buffer. */
int md_serialize(const md_doc_t *doc, buf_t *out);

/* load file at path, parse into doc. returns 0 ok, 1 not found (doc
   zeroed), -1/-2/-3 as md_parse. */
int md_load(const char *path, md_doc_t *doc);

/* serialize doc to buf then atomically write path (write tmp + rename).
   returns 0 ok, -1 io error (stderr message). */
int md_save(const char *path, const md_doc_t *doc);

/* json tree (json.c): a minimal rfc 8259 parser/serializer, no
   external deps. nodes own their strings and children. */
typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ
} json_type_t;

typedef struct json {
    json_type_t type;
    /* JSON_STR: owned string. JSON_OBJ children carry their member
       name in .key, everything else has key == NULL */
    char *str;           /* owned */
    char *key;           /* owned, object member name, NULL otherwise */
    double num;          /* JSON_NUM; JSON_BOOL uses (int)num */
    struct json *child;  /* first child (ARR/OBJ) */
    struct json *next;   /* next sibling */
} json_t;

/* parse whole document (NUL-terminated). returns tree or NULL on any
   syntax error (bad escapes, lone surrogates, trailing junk, nesting
   deeper than 64). empty input is an error. */
json_t *json_parse(const char *s);
/* fresh node of type t (JSON_STR nodes start with empty string) */
json_t *json_new(json_type_t t);
/* fresh string/number/bool node, NULL on oom */
json_t *json_str(const char *s);
json_t *json_num(double v);
json_t *json_bool(int v);
json_t *json_arr(void);
json_t *json_obj(void);
/* append child to the end of parent's ARR/OBJ. for OBJ the child's
   .key must be set before the call; caller keeps the child on oom. */
void json_add(json_t *parent, json_t *child);
/* first member of obj matching key, NULL when missing. obj NULL-safe */
json_t *json_get(const json_t *obj, const char *key);
/* json_get + ->str of the hit, NULL-safe both ways */
const char *json_str_of(const json_t *obj, const char *key);
/* compact serialization of the subtree, malloc'd string, NULL oom.
   numbers print without ".0" when integral, strings escape " and \
   plus control chars (<0x20 as \uXXXX) */
char *json_print(const json_t *node);
/* deep free of the subtree, NULL-safe */
void json_free(json_t *node);

/* cli parser (cli.c). long options only: --name, --name=value.
   a lone "--" ends options; everything after is positional. */
struct cli_opt {
    const char *name;
    int has_arg; /* 0 flag, 1 value */
};

struct cli_args {
    /* malloc'd array of positional args, caller frees (the strings
       themselves are borrowed from argv) */
    const char *const *pos;
    int npos;
    /* value of the last option seen that took one (from --name <v> or
       --name=v); NULL when none was seen */
    const char *name_value;
    /* bit0: -h/--help seen */
    int flags;
};

/* returns 0 ok, -1 unknown option, -2 missing argument; errors go to
   stderr. on oom writes to stderr and returns -1. */
int cli_parse(int argc, char **argv, const struct cli_opt *opts,
              size_t nopts, struct cli_args *out);

/* config store (store.c), layout: docs/config_layout.md */

/* command layer (flower.c): setup and project/task configuration
   editing per docs/config_cli.md. argv[0]=prog, argv[1]=command. */
int flower_main(int argc, char **argv);
/* `flower mcp <P> <area>`: stdio json-rpc server over one project
   area (src/mcpserve.c) */
int cmd_mcp(int argc, char **argv);
/* scan.c: drive the llmkit runner session; argv0 for self-spawning */
int cmd_scan(const char *pid, const char *profile, int dry_run,
             const char *argv0);
/* $FLOWER_BASE: $FLOWER_BASE env if set, else $XDG_CONFIG_HOME/flower,
   else $HOME/.config/flower. NULL on missing HOME and no env. */
const char *store_base(void);
/* build path under base into malloc'd string. returns NULL oom.
   examples: store_path("projects/P1/config.md"), store_path(NULL) = base. */
char *store_path(const char *rel);
/* mkdir -p for path under base; returns 0 ok, -1 io (stderr msg) */
int store_mkdir_p(const char *rel_dir);

/* next free project/task id. scans the projects/P1..N dirs on disk.
   returns malloc'd "P1"/"T3" string or NULL oom. task: pass project id. */
char *store_next_project_id(void);
char *store_next_task_id(const char *pid);

/* next free action id in projects/P/tasks/T/actions.md: highest "A<n>"
   chapter number + 1, malloc'd "A4". NULL oom. doc may be empty. */
char *store_next_action_id(const md_doc_t *actions_doc);
/* validate id syntax: "P<n>" etc. 1 ok 0 bad */
int store_valid_id(const char *id, char kind); /* kind 'P','T','A' */
/* malloc'd project id detected from cwd via Path keys, NULL when none */
char *store_detect_project(void);

/* allowed keys for config.md chapters. NULL-terminated lists. */
extern const char *const store_user_keys[];     /* Name, Role */
extern const char *const store_project_keys[];  /* Name, Path, Description */
extern const char *const store_task_keys[];     /* Name, Description */
extern const char *const store_action_keys[];   /* Description, Status, Depends */
extern const char *const store_detail_fields[]; /* Output, Objectives, Scope,
                                                   Constraints, Stakeholders */
extern const char *const store_context_prefixes[]; /* Fact, Pattern, Risk,
                                                      Success metric, Failure
                                                      sign, Evaluation method,
                                                      Rule */
/* allowed keys for llms.md profile chapters (Api base, Api key,
   Protocol, Model, Max tokens, Reasoning effort, Header, Mcp proxy,
   Terminal tool; the last three may repeat) and the llmkit protocol
   values. */
extern const char *const store_llm_keys[];
extern const char *const store_llm_protocols[];
/* 1 if protocol value is one of store_llm_protocols */
int store_valid_llm_protocol(const char *protocol);
/* 1 if profile name is non-empty [A-Za-z][A-Za-z0-9 _-]* */
int store_valid_llm_name(const char *name);
/* context chapter title in file is "<Prefix>: <Title>"; helpers: */
/* 1 if title starts with one of allowed prefixes + ": " */
int store_is_context_title(const char *chapter_title);
/* split "<Prefix>: <Title>" -> malloc'd copies, 1 ok 0 not a valid split.
   prefix must be from store_context_prefixes. */
int store_split_context(const char *chapter_title, char **prefix, char **title);
/* build malloc'd "<Prefix>: <Title>" */
char *store_make_context_title(const char *prefix, const char *title);
/* 1 if status value valid (pending, complete) */
int store_valid_status(const char *status);
/* parse "A1, A2" comma list into malloc'd array of malloc'd ids (trim
   spaces), returns count, *out_ids set; NULL ids on 0. -1 oom. */
int store_parse_id_list(const char *s, char ***out_ids);
/* free one store_parse_id_list result */
void store_free_ids(char **ids, int n);

/* config plumbing (config.c): doc schema validation, load/save under
   $FLOWER_BASE. split out of flower.c, behavior unchanged. */

/* out-of-memory reporter: prints "flower: out of memory" to stderr and
   returns 1 so callers can `return oom();` */
int oom(void);
/* malloc'd sprintf, NULL on oom or format error */
char *xasprintf(const char *fmt, ...);

/* file kinds for load-time validation */
enum {
    FV_USER,     /* flower.md: one "# User" kv chapter */
    FV_PROJECT,  /* config.md: one "# Project" kv chapter */
    FV_TASK,     /* config.md: one "# Task" kv chapter */
    FV_FIELDS,   /* details.md: chapters from store_detail_fields */
    FV_CONTEXT,  /* context.md: "<Prefix>: <Title>" bare chapters */
    FV_RESOURCE, /* resources.md: any name, bare value */
    FV_ACTIONS,  /* actions.md: "# A<n>: <Title>" kv chapters */
    FV_LLM       /* llms.md: one kv chapter per profile */
};

/* chapter title and allowed keys for the kv kinds */
void kind_schema(int kind, const char **chapter,
                 const char *const **keys);
/* 1 if k is in the NULL-terminated keys list */
int key_allowed(const char *const *keys, const char *k);
/* entry from store_context_prefixes matching p, NULL when unknown */
const char *prefix_known(const char *p);
/* 1 when s is one or more digits (a positive whole number) */
int llm_digits(const char *s);

/* doc mutation helpers (md.c keeps parsing only) */
/* append a fresh chapter at the doc tail. returns it, NULL oom (doc
   stays valid, nch unchanged) */
md_chapter_t *doc_add_chapter(md_doc_t *doc, const char *title,
                              int level);
/* drop chapter idx, freeing it */
void doc_remove_chapter(md_doc_t *doc, int idx);
/* append one kv pair at the chapter tail, no duplicate check (md.c is
   parsing-only, so repeatable keys grow here). 0 ok, -1 oom */
int doc_append_kv(md_chapter_t *c, const char *key, const char *val);
/* replace the chapter value: kv pairs go away, body becomes text split
   on newlines (blank lines dropped, md_parse drops them anyway).
   0 ok, -1 oom */
int chapter_set_body(md_chapter_t *c, const char *text);

/* context/action chapter title helpers */
/* title part of a context chapter, NULL when the title is not a valid
   "<Prefix>: <Title>" */
char *context_title_part(const char *chapter_title);
/* "A<n>: <Title>" split. 1 ok (malloc'd id + title), 0 bad shape.
   title may come back empty ("A3: ") so callers can reject it. */
int action_split(const char *chapter_title, char **id, char **title);
/* 1 when the chapter title is a valid "# A<n>: <Title>" */
int action_valid_title(const char *t);
/* 1 when some chapter is action id ("A1" matching "A1: title") */
int action_exists(const md_doc_t *doc, const char *id);
/* malloc'd title text of an action chapter, "" (not NULL) on oom-free
   bad shape */
char *action_title_of(const md_chapter_t *c);
/* 1 when some chapter (other than skip) already holds this title */
int context_title_taken(md_doc_t *doc, const char *title,
                        const md_chapter_t *skip);

/* schema check of an already parsed file; rejects stray chapters, keys
   and key-value pairs where only bare text may live. on failure writes
   the reason into out (snprintf-style, always NUL-terminated) and
   returns 1. */
int doc_check(int kind, const md_doc_t *doc, char *out, size_t outn);
/* doc_check + stderr line prefixed with path */
int validate_doc(const char *path, int kind, md_doc_t *doc);
/* load store_path(rel) when present and validate it. 0 ok (doc may be
   empty), 1 after printing why (doc freed). */
int load_rel(const char *rel, int kind, md_doc_t *doc);
/* print path as absolute: prefix it with the base directory when the
   caller passed a relative one. realpath is not used: it would fail on
   the freshly removed files too, not only on the just-written ones */
void print_abs(const char *path);
/* save doc to store_path(rel), creating mkdir_rel first when given,
   then print the absolute path */
int save_rel(const char *mkdir_rel, const char *rel,
             const md_doc_t *doc);
/* dump the raw file to stdout; missing file prints nothing */
int print_rel_file(const char *rel);
/* 1 when store_path(rel) is an existing directory */
int rel_dir_exists(const char *rel);
/* 1 after printing an error when projects/<pid> is missing */
int require_project(const char *pid);
/* same for projects/<pid>/tasks/<tid> (checks the project too) */
int require_task(const char *pid, const char *tid);

/* crash report (crash_report.c) */
void crash_report_init(const char *argv0);

/* update (update.c) */
int update_run(int argc, char **argv);
const char *update_current_version(void);

/* semver parser exposed for tests: "v1.2.3", "1.2", "v3" parse ok;
   shorter prefixes get zeros; junk fails with ok=0 */
typedef struct {
    int maj, min, pat;
    int ok;
} flower_semver_t;
flower_semver_t update_parse_ver(const char *tag);

#endif
