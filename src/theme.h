#ifndef FLOWER_THEME_H
#define FLOWER_THEME_H

/*
 * theme — flower's filesystem-backed configuration.
 *
 * Config lives in $XDG_CONFIG_HOME/flower/ (or ~/.config/flower/), starting
 * with theme.json. Loading is lenient (bad values fall back to defaults);
 * saving goes through an atomic tmp+rename write; PUT parsing is strict
 * (unknown keys and invalid values are rejected with a precise error).
 */

#include <stddef.h>

#define THEME_MAX 96 /* max length of any theme string incl. NUL */

typedef struct {
    char background_primary[16];
    char background_secondary[16];
    char background_tertiary[16];
    char text_primary_color[16];
    char text_primary_font[THEME_MAX];
    char text_primary_size[16];
    char text_secondary_color[16];
    char text_secondary_font[THEME_MAX];
    char text_secondary_size[16];
    char accent[16];
    char success[16];
    char warning[16];
} theme_t;

typedef enum {
    THEME_OK = 0,     /* parsed and valid */
    THEME_E_JSON = 1, /* not JSON / not an object */
    THEME_E_FIELD = 2 /* valid JSON, but a value/key is rejected */
} theme_parse_result_t;

/* Resolve and create the config dir. `override` (may be NULL) wins over
 * the XDG default. Returns 0 on success. Call before anything else. */
int theme_init(const char *override);

/* Path of the resolved config directory (valid after theme_init). */
const char *theme_dir(void);

void theme_defaults(theme_t *t);

/* Load theme.json into *t. Always fills *t (defaults on any problem).
 * Returns 0 if the file was loaded, 1 if defaults were used. */
int theme_load(theme_t *t);

/* Persist as theme.json (atomic write). 0 on success. */
int theme_save(const theme_t *t);

/* Strict parse+validate a full theme document. On failure fills
 * err_field ("" for whole-document errors) and err_msg. */
theme_parse_result_t theme_from_json(const char *buf, size_t len, theme_t *out,
                                     char *err_field, size_t err_field_n,
                                     char *err_msg, size_t err_msg_n);

/* Serialize as compact JSON. malloc'd, caller frees; NULL on OOM. */
char *theme_to_json(const theme_t *t);

/* {"error": msg[, "field": field]} as compact JSON. malloc'd, may be NULL. */
char *theme_error_json(const char *msg, const char *field);

#endif
