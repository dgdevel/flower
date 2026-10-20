/* fuzz_path.c - libFuzzer harness over the shared path helpers.
 * exercises flower_path_join / flower_snprintf truncation behavior and
 * the file io round trip under a fixed sandbox prefix. absolute or
 * ".." containing inputs stay inside the prefix (join puts it first).
 * build: make fuzz ; run: ./fuzz/fuzz_path */
#include "../src/flower.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char name[FLOWER_PATH_MAX + 1];
    char path[FLOWER_PATH_MAX];
    char *text = NULL;

    if (size == 0 || size > sizeof(name) - 1)
        return 0;
    memcpy(name, data, size);
    name[size] = '\0';

    /* the codebase refuses escaping targets (llm.c proposal_apply);
     * mirror that so the sandbox below is never left. */
    if (strstr(name, ".."))
        return 0;

    flower_path_join(path, sizeof(path), "/tmp/flower-fuzz", name);

    /* write/read round trip: truncation may send us to a shorter path,
     * that is fine - both calls must stay memory safe. */
    if (flower_write_file(path, name) == 0) {
        (void)flower_read_file(path, &text);
        if (text) {
            (void)flower_count_md_items(text);
            free(text);
        }
    }
    return 0;
}
