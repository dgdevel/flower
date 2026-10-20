/* setup.c - `flower setup`: interactive flower.json writer. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INPUT_LEN 512
#define BASE_LEN  1024
#define PATH_LEN  (BASE_LEN + 64)

struct settings {
    char protocol[INPUT_LEN];
    char api_base[INPUT_LEN];
    char model[INPUT_LEN];
    char api_key[INPUT_LEN];
    char user_name[INPUT_LEN];
    char user_role[INPUT_LEN];
};

/* read one line from stdin into buf (nul-terminated, newline stripped).
 * 1 on success, 0 on eof/error. */
static int read_line(char *buf, size_t size)
{
    if (!fgets(buf, (int)size, stdin))
        return 0;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 1;
}

/* copy def into line when the user entered nothing. */
static void keep_default(char *line, size_t size, const char *def)
{
    if (line[0] == '\0')
        snprintf(line, size, "%s", def);
}

static void ask(const char *prompt, const char *def, char *buf, size_t size)
{
    char line[INPUT_LEN];

    printf("%s [%s]: ", prompt, def);
    fflush(stdout);
    if (!read_line(line, sizeof(line)))
        exit(1);
    keep_default(line, sizeof(line), def);
    snprintf(buf, size, "%s", line);
}

/* ask and re-ask until the answer is a valid endpoint protocol. */
static void ask_protocol(const char *def, char *buf, size_t size)
{
    char line[INPUT_LEN];

    for (;;) {
        printf("endpoint protocol (openai/openai_responses/anthropic) "
               "[%s]: ", def);
        fflush(stdout);
        if (!read_line(line, sizeof(line)))
            exit(1);
        if (line[0] == '\0' || !strcmp(line, "openai")
            || !strcmp(line, "openai_responses") || !strcmp(line, "anthropic"))
            break;
        printf("invalid protocol, try again\n");
    }
    keep_default(line, sizeof(line), def);
    snprintf(buf, size, "%s", line);
}

/* ask and re-ask until the answer is a non-empty value. */
static void ask_required(const char *prompt, const char *def, char *buf,
                         size_t size)
{
    char line[INPUT_LEN];

    for (;;) {
        printf("%s [%s]: ", prompt, def);
        fflush(stdout);
        if (!read_line(line, sizeof(line)))
            exit(1);
        if (line[0] == '\0' && def[0] == '\0')
            continue; /* no default to keep, must answer */
        break;
    }
    keep_default(line, sizeof(line), def);
    snprintf(buf, size, "%s", line);
}

/* copy a json string field (if present) into out. */
static void get_str(cJSON *obj, const char *name, char *out, size_t size,
                    int require_nonempty)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);

    if (cJSON_IsString(item)
        && (item->valuestring[0] || !require_nonempty))
        snprintf(out, size, "%s", item->valuestring);
}

static void load_defaults(struct settings *s)
{
    char base[BASE_LEN];
    char path[PATH_LEN];
    char *text;
    long len;
    FILE *f;
    cJSON *root, *llm, *user;

    strcpy(s->protocol, "openai");
    s->api_base[0] = s->model[0] = s->api_key[0] = '\0';
    s->user_name[0] = s->user_role[0] = '\0';

    if (flower_base_dir(base, sizeof(base)) != 0)
        return;
    snprintf(path, sizeof(path), "%s/flower.json", base);
    f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return;
    }
    text = malloc((size_t)len + 1);
    if (!text) {
        fclose(f);
        return;
    }
    if (fread(text, 1, (size_t)len, f) != (size_t)len) {
        free(text);
        fclose(f);
        return;
    }
    text[len] = '\0';
    fclose(f);

    root = cJSON_Parse(text);
    free(text);
    if (!root)
        return;
    llm = cJSON_GetObjectItemCaseSensitive(root, "llm");
    if (cJSON_IsObject(llm)) {
        get_str(llm, "endpoint_protocol", s->protocol, sizeof(s->protocol), 1);
        get_str(llm, "api_base", s->api_base, sizeof(s->api_base), 1);
        get_str(llm, "model", s->model, sizeof(s->model), 0);
        get_str(llm, "api_key", s->api_key, sizeof(s->api_key), 0);
    }
    user = cJSON_GetObjectItemCaseSensitive(root, "user");
    if (cJSON_IsObject(user)) {
        get_str(user, "name", s->user_name, sizeof(s->user_name), 1);
        get_str(user, "role", s->user_role, sizeof(s->user_role), 1);
    }
    cJSON_Delete(root);
}

int setup_run(void)
{
    char base[BASE_LEN];
    char path[PATH_LEN];
    struct settings s;
    cJSON *root, *llm, *user, *item;
    char *text;
    FILE *f;
    int fd;
    int rc = 1;

    if (flower_base_dir(base, sizeof(base)) != 0) {
        fprintf(stderr, "flower setup: cannot resolve config directory "
                        "(set XDG_CONFIG_DIR or HOME)\n");
        return 1;
    }
    if (flower_ensure_dir(base) != 0) {
        fprintf(stderr, "flower setup: cannot create %s: %s\n", base,
                strerror(errno));
        return 1;
    }

    load_defaults(&s);

    printf("flower setup - press enter to keep the value in brackets\n\n");
    ask_protocol(s.protocol, s.protocol, sizeof(s.protocol));
    ask_required("api base url", s.api_base, s.api_base, sizeof(s.api_base));
    ask("model name (optional, empty for none)", s.model, s.model,
        sizeof(s.model));
    ask("api key (optional, empty for none)", s.api_key, s.api_key,
        sizeof(s.api_key));
    ask_required("user name", s.user_name, s.user_name, sizeof(s.user_name));
    ask_required("user role", s.user_role, s.user_role, sizeof(s.user_role));

    root = cJSON_CreateObject();
    llm = cJSON_CreateObject();
    user = cJSON_CreateObject();
    if (!root || !llm || !user)
        goto out;

    item = cJSON_CreateString(s.protocol);
    if (!item || !cJSON_AddItemToObject(llm, "endpoint_protocol", item))
        goto out;
    item = cJSON_CreateString(s.api_base);
    if (!item || !cJSON_AddItemToObject(llm, "api_base", item))
        goto out;
    if (s.model[0]) {
        item = cJSON_CreateString(s.model);
        if (!item || !cJSON_AddItemToObject(llm, "model", item))
            goto out;
    }
    if (s.api_key[0]) {
        item = cJSON_CreateString(s.api_key);
        if (!item || !cJSON_AddItemToObject(llm, "api_key", item))
            goto out;
    }
    item = cJSON_CreateString(s.user_name);
    if (!item || !cJSON_AddItemToObject(user, "name", item))
        goto out;
    item = cJSON_CreateString(s.user_role);
    if (!item || !cJSON_AddItemToObject(user, "role", item))
        goto out;
    if (!cJSON_AddItemToObject(root, "llm", llm)
        || !cJSON_AddItemToObject(root, "user", user)) {
        goto out;
    }
    llm = NULL;
    user = NULL;

    text = cJSON_Print(root);
    if (!text)
        goto out;

    snprintf(path, sizeof(path), "%s/flower.json", base);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "flower setup: cannot open %s: %s\n", path, strerror(errno));
        free(text);
        goto out;
    }
    f = fdopen(fd, "wb");
    if (!f) {
        fprintf(stderr, "flower setup: cannot open %s: %s\n", path, strerror(errno));
        close(fd);
        free(text);
        goto out;
    }
    fputs(text, f);
    fputc('\n', f);
    if (fclose(f) != 0) {
        fprintf(stderr, "flower setup: cannot write %s: %s\n", path,
                strerror(errno));
        free(text);
        goto out;
    }
    free(text);

    printf("\nwritten %s\n", path);
    printf("  endpoint_protocol: %s\n", s.protocol);
    printf("  api_base: %s\n", s.api_base);
    printf("  model: %s\n", s.model[0] ? s.model : "(none)");
    printf("  api_key: %s\n", s.api_key[0] ? "(set)" : "(none)");
    printf("  user name: %s\n", s.user_name);
    printf("  user role: %s\n", s.user_role);
    rc = 0;

out:
    if (rc != 0)
        fprintf(stderr, "flower setup: out of memory\n");
    cJSON_Delete(root);
    return rc;
}
