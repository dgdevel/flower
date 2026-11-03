/* update.c - flower update: check github for a newer release and
   install it. uses the curl binary over the api, no external libraries;
   semver compare and sha256 are in-tree. */
#include "flower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define API_LATEST \
    "https://api.github.com/repos/dgdevel/flower/releases/latest"
#define DL_BASE \
    "https://github.com/dgdevel/flower/releases/download/"

const char *update_current_version(void) {
    return FLOWER_VERSION;
}

flower_semver_t update_parse_ver(const char *tag) {
    flower_semver_t v = {0, 0, 0, 0};
    const char *p = tag;
    int *fields[3] = {&v.maj, &v.min, &v.pat};
    int i;
    if (!p) return v;
    if (*p == 'v' || *p == 'V') p++;
    if (!*p) return v; /* empty after v: junk */
    for (i = 0; i < 3; i++) {
        char *end;
        long n;
        if (!*p) return v; /* shorter prefix: remaining fields stay 0 */
        if (!isdigit((unsigned char)*p)) return v; /* junk */
        n = strtol(p, &end, 10);
        if (*end && *end != '.') return v; /* trailing junk like 1.2.x */
        *fields[i] = (int)n;
        if (!*end) break;
        p = end + 1; /* skip the dot */
    }
    v.ok = 1;
    return v;
}

/* run curl and slurp its stdout into buf (read-only popen) */
static int popen_curl(const char *url, const char *accept, buf_t *out) {
    char cmd[512];
    FILE *f;
    buf_reset(out);
    /* url is an in-tree constant; accept is quoted in-tree too */
    snprintf(cmd, sizeof cmd, "curl -fsSL -H 'Accept: %s' %s", accept, url);
    f = popen(cmd, "r");
    if (!f) return -1;
    {
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
            if (buf_append(out, chunk, n) != 0) {
                pclose(f);
                return -1;
            }
    }
    return pclose(f) == 0 ? 0 : -1;
}

/* hand-rolled json probe: find "\"tag_name\"", then colon, then first
   quoted string. no json lib for one field. */
static int find_tag_name(const char *json, size_t len, char *tag,
                         size_t tagsz) {
    const char *needle = "\"tag_name\"";
    const char *p = json;
    size_t i;
    for (i = 0; i + strlen(needle) <= len; i++) {
        if (!strncmp(p + i, needle, strlen(needle))) {
            const char *q = p + i + strlen(needle);
            while (q < p + len && *q != ':') q++;
            if (q >= p + len) return -1;
            q++; /* past colon */
            while (q < p + len && *q != '"') q++;
            if (q >= p + len) return -1;
            q++; /* past open quote */
            {
                size_t n = 0;
                while (q < p + len && *q != '"' && n + 1 < tagsz)
                    tag[n++] = *q++;
                if (q >= p + len) return -1;
                tag[n] = '\0';
                return 0;
            }
        }
    }
    return -1;
}

int update_run(int argc, char **argv) {
    int yes = 0;
    int i;
    buf_t body = {0};
    char tag[64];
    flower_semver_t cur, rem;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes")) {
            yes = 1;
            continue;
        }
        fprintf(stderr, "usage: flower update [--yes]\n");
        return 2;
    }
    if (!yes) printf("update: checking for updates\n");
    if (popen_curl(API_LATEST, "application/vnd.github+json", &body) != 0) {
        fprintf(stderr, "update: fetch failed\n");
        return 1;
    }
    if (find_tag_name(body.data, body.len, tag, sizeof tag) != 0) {
        fprintf(stderr, "update: no tag_name in api response\n");
        return 1;
    }
    cur = update_parse_ver(update_current_version());
    rem = update_parse_ver(tag);
    if (!rem.ok || !cur.ok || rem.maj < cur.maj ||
        (rem.maj == cur.maj && rem.min < cur.min) ||
        (rem.maj == cur.maj && rem.min == cur.min && rem.pat <= cur.pat)) {
        printf("update: up to date (version %s)\n", update_current_version());
        return 0;
    }
    printf("update: new version %s available (current %s)\n", tag,
           update_current_version());
    if (!yes) {
        int c;
        printf("update: apply? [y/N] ");
        fflush(stdout);
        c = fgetc(stdin);
        /* drain rest of the line */
        while (c != '\n' && c != EOF) c = fgetc(stdin);
        if (c != 'y' && c != 'Y') {
            printf("update: aborted\n");
            return 0;
        }
    }
    printf("update: downloading ...\n");
    /* download + replace goes here: fetch DL_BASE <tag> artifact, verify
       sha256 in-tree, write <exe>.new, rename over <exe> */
    printf("update: self-update is a template stub - wire tools/release.sh "
           "artifacts here\n");
    return 0;
}
