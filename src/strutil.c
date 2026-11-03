/* strutil.c - small byte-oriented string helpers, no locale deps. */
#include "flower.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *str_trim(char *s) {
    char *start = s;
    /* find first non-space */
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    /* strip trailing space */
    {
        size_t len = strlen(s);
        while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    }
    return s;
}

int str_starts(const char *s, const char *pfx) {
    size_t n = strlen(pfx);
    return strncmp(s, pfx, n) == 0;
}

/* malloc'd copy, NULL on oom */
char *str_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
