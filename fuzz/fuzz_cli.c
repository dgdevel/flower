/* fuzz/fuzz_cli.c - cli_parse grammar: the raw input is split at NUL
 * bytes into up to 64 argv words, "prog" is argv[0]. checks the return
 * stays in {0, -1, -2}; anything else is a parser bug -> abort. the
 * parsed values are never dereferenced beyond that. */
#include "flower.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WORDS 64

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* argv[0] = "prog", up to MAX_WORDS more -> 65 slots */
    char *argv[MAX_WORDS + 1];
    struct cli_opt opts[] = {{"verbose", 0}, {"name", 1}};
    struct cli_args out;
    char *copy;
    char *p;
    size_t nwords = 1;
    int rc;

    if (size == 0) return 0;
    copy = malloc(size + 1);
    if (!copy) return 0;
    memcpy(copy, data, size);
    copy[size] = '\0';

    argv[0] = (char *)"prog";
    p = copy;
    while (nwords <= MAX_WORDS) {
        /* walk to the next NUL: the word ends there (already 0) */
        char *nul = memchr(p, '\0', (size_t)(copy + size - p));
        argv[nwords++] = p;
        if (!nul) break;
        p = nul + 1;
        if (p == copy + size) break;
    }

    rc = cli_parse((int)nwords, argv, opts,
                   sizeof opts / sizeof opts[0], &out);
    if (rc != 0 && rc != -1 && rc != -2) abort();
    /* out.pos is malloc'd by cli_parse; the const is for its callers */
    if (rc == 0) free((void *)out.pos);
    free(copy);
    return 0;
}
