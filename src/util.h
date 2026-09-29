/* util — the small pieces every config store and parser shares:
 * text validation, id generation, whole-file reads, atomic json
 * writes, json error paths and the growable byte buffer. */
#ifndef FLOWER_UTIL_H
#define FLOWER_UTIL_H

#include <cJSON.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* valid UTF-8 with no control characters (C0, DEL); multiline also
 * allows \n and \t. n is the byte length. */
int valid_utf8_text(const char *s, size_t n, int multiline);

/* random lowercase-hex id, n chars (buf holds n+1); uniqueness, not
 * secrecy, is the goal. /dev/urandom with a time+pid fallback. */
void gen_hex_id(char *buf, size_t n);

/* whole file as a NUL-terminated string, or NULL when missing,
 * unreadable or larger than cap bytes */
char *read_whole_file(const char *path, long cap);

/* pretty-printed json + '\n', written atomically (tmp + rename);
 * 0 on success. j == NULL fails without touching the target. */
int save_json_atomic(const char *path, const cJSON *j);

/* json error paths like "tasks[0].actions[1]": memcpy cannot trip
 * -Wformat-truncation, deep nesting clamps the reported path */
void path_set(char *dst, size_t n, const char *src);
void path_add(char *dst, size_t n, const char *suffix);
void path_add_index(char *dst, size_t n, size_t i);

/* growable, always NUL-terminated byte buffer */
typedef struct {
    char *data;
    size_t len, cap;
} sbuf_t;

static inline int sb_putn(sbuf_t *b, const char *s, size_t n)
{
    if (!n) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) return -1;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static inline int sb_puts(sbuf_t *b, const char *s)
{
    return sb_putn(b, s ? s : "", s ? strlen(s) : 0);
}

static inline int sb_putc(sbuf_t *b, char c)
{
    return sb_putn(b, &c, 1);
}

#endif
