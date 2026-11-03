/* buf.c - growable byte buffer. realloc doubling, always NUL-terminates. */
#include "flower.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

void buf_init(buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf_t *b) {
    free(b->data);
    buf_init(b);
}

int buf_append(buf_t *b, const void *data, size_t n) {
    /* +1: room for the NUL terminator, always kept past len */
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        char *p;
        while (cap < b->len + n + 1) cap *= 2;
        p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p;
        b->cap = cap;
    }
    if (n) memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int buf_appendf(buf_t *b, const char *fmt, ...) {
    va_list ap;
    int need;
    char *p;
    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return -1;
    /* grow for the formatted body plus terminator */
    if (b->len + (size_t)need + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + (size_t)need + 1) cap *= 2;
        p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p;
        b->cap = cap;
    }
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
    return 0;
}

void buf_reset(buf_t *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}
