/* fuzz/fuzz_buf.c - buf_t growth: init, append in two data-split
 * chunks, one formatted append, reset, full re-append, free.
 * exercises realloc growth across arbitrary chunk boundaries. */
#include "flower.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    buf_t b;
    const char *s = (const char *)data;
    size_t cut = 0;

    if (size > 1) {
        /* consume the first byte and split the rest at a data-derived
           offset, so the fuzzer steers the chunk sizes itself */
        cut = data[0] % size;
        s = (const char *)(data + 1);
        size--;
    }

    buf_init(&b);
    buf_append(&b, s, cut);
    buf_append(&b, s + cut, size - cut);
    buf_appendf(&b, "%d|%.*s", (int)size, (int)(size - cut), s + cut);
    buf_reset(&b);
    buf_append(&b, s, size);
    buf_free(&b);
    return 0;
}
