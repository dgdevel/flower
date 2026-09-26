#ifndef FLOWER_PROJECTS_H
#define FLOWER_PROJECTS_H

/*
 * projects — the user's project list, persisted as projects.json in the
 * config dir (see theme.h for directory resolution). Same contract as
 * theme.c: lenient load (broken entries are repaired or dropped, the
 * rest still boots), strict validated save via atomic tmp+rename.
 *
 * A project's identity is its working directory (unique in the list).
 * Title defaults to the directory's basename, color/emoji to a palette.
 */

#include <stddef.h>

#define PROJECTS_MAX      64  /* list length cap (keeps PUTs under 64 KB) */
#define PROJECT_DIR_MAX   512
#define PROJECT_TITLE_MAX 96
#define PROJECT_EMOJI_MAX 32

typedef struct {
    char dir[PROJECT_DIR_MAX];
    char title[PROJECT_TITLE_MAX];
    char color[16];
    char emoji[PROJECT_EMOJI_MAX];
} project_t;

typedef struct {
    project_t items[PROJECTS_MAX];
    size_t count;
} projects_t;

typedef enum {
    PROJECTS_OK = 0,     /* parsed and valid */
    PROJECTS_E_JSON = 1, /* not JSON / not an array */
    PROJECTS_E_FIELD = 2 /* valid JSON, but an entry/value is rejected */
} projects_parse_result_t;

/* Load projects.json into *p (empty list on any problem). Returns 0 if
 * the file was read and parsed, 1 if the list started empty. */
int projects_load(projects_t *p);

/* Persist as projects.json (atomic write). 0 on success. */
int projects_save(const projects_t *p);

/* Strict parse+validate a full project array (PUT body). On failure
 * fills err_field (e.g. "projects[1].color", "" for whole-document
 * errors) and err_msg. */
projects_parse_result_t projects_from_json(const char *buf, size_t len,
                                           projects_t *out,
                                           char *err_field, size_t err_field_n,
                                           char *err_msg, size_t err_msg_n);

/* Serialize as a compact JSON array. malloc'd, caller frees; NULL on OOM.
 * with_exists adds a live "exists":true/false per project (stat() at
 * call time) so clients can flag vanished working directories. */
char *projects_to_json(const projects_t *p, int with_exists);

#endif
