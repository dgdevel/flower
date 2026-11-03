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
