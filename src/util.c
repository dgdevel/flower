/* util — see util.h */
#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

int valid_utf8_text(const char *s, size_t n, int multiline)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp;
        int need;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) {
                if (multiline && (c == '\n' || c == '\t')) { i++; continue; }
                return 0;
            }
            i++;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f; need = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f; need = 2;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07; need = 3;
        } else {
            return 0; /* stray continuation / invalid lead */
        }
        if (i + (size_t)need >= n) return 0; /* truncated sequence */
        for (int k = 1; k <= need; k++) {
            unsigned char cc = (unsigned char)s[i + (size_t)k];
            if ((cc & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (need == 1 && cp < 0x80) return 0;            /* overlong */
        if (need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)))
            return 0;                                     /* overlong/surrogate */
        if (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
        i += (size_t)need + 1;
    }
    return 1;
}

void gen_hex_id(char *buf, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    size_t nb = (n + 1) / 2;
    int ok = nb <= sizeof raw;
    if (ok) {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            ok = fread(raw, 1, nb, f) == nb;
            fclose(f);
        }
    }
    if (!ok) {
        unsigned seed = (unsigned)getpid() ^ (unsigned)time(NULL);
        for (size_t i = 0; i < nb && i < sizeof raw; i++) {
            seed = seed * 1103515245u + 12345u;
            raw[i] = (unsigned char)(seed >> 16);
        }
    }
    for (size_t i = 0; i < n; i++)
        buf[i] = hex[(raw[i / 2] >> (i % 2 ? 0 : 4)) & 0xf];
    buf[n] = '\0';
}

char *read_whole_file(const char *path, long cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0 && sz < cap) {
            rewind(f);
            buf = malloc((size_t)sz + 1);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
                buf[sz] = '\0';
            else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}

int save_json_atomic(const char *path, const cJSON *j)
{
    /* the biggest caller builds "<4k config dir>/agents/<96-char
     * name>.json"; tmp holds that plus ".tmp.<pid>" */
    char tmp[8704 + 32];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid())
        >= (int)sizeof tmp)
        return -1;
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    char *out = j ? cJSON_Print(j) : NULL; /* pretty-printed */
    int ok = out && fputs(out, f) != EOF && fputc('\n', f) != EOF &&
             fclose(f) == 0;
    free(out);
    if (!ok) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

void path_set(char *dst, size_t n, const char *src)
{
    size_t l = strnlen(src, n - 1);
    memcpy(dst, src, l);
    dst[l] = '\0';
}

void path_add(char *dst, size_t n, const char *suffix)
{
    size_t l = strnlen(dst, n - 1);
    size_t m = strnlen(suffix, n - 1 - l);
    memcpy(dst + l, suffix, m);
    dst[l + m] = '\0';
}

void path_add_index(char *dst, size_t n, size_t i)
{
    char idx[24];
    snprintf(idx, sizeof idx, "[%zu]", i);
    path_add(dst, n, idx);
}
