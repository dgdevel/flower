/* fuzz_json.c - libFuzzer harness over the cJSON code paths used by the
 * project: parse -> inspect -> print -> reparse, plus the markdown item
 * counter (config.json / context.md content comes from disk or the llm,
 * so it is untrusted input).
 * build: make fuzz ; run: ./fuzz/fuzz_json */
#include "../src/flower.h"

#include <cjson/cJSON.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *text;
    cJSON *root, *reparsed;
    char *printed;

    if (size == 0 || size > 1 << 16)
        return 0;
    text = malloc(size + 1);
    if (!text)
        return 0;
    memcpy(text, data, size);
    text[size] = '\0';

    (void)flower_count_md_items(text);

    root = cJSON_Parse(text);
    if (root) {
        printed = cJSON_Print(root);
        if (printed) {
            reparsed = cJSON_Parse(printed);
            if (reparsed)
                cJSON_Delete(reparsed);
            free(printed);
        }
        cJSON_Delete(root);
    }
    free(text);
    return 0;
}
