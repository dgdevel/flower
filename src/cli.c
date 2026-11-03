/* cli.c - long-option parser: --name, --name value, --name=value,
   a lone "--" ends options, everything else is positional. */
#include "flower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct cli_opt *opt_lookup(const struct cli_opt *opts,
                                        size_t nopts, const char *name) {
    size_t i;
    for (i = 0; i < nopts; i++)
        if (!strcmp(opts[i].name, name)) return &opts[i];
    return NULL;
}

int cli_parse(int argc, char **argv, const struct cli_opt *opts,
              size_t nopts, struct cli_args *out) {
    int endopts = 0;
    const char **pos = NULL;
    int npos = 0;
    int i;
    out->pos = NULL;
    out->npos = 0;
    out->name_value = NULL;
    out->flags = 0;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (endopts || arg[0] != '-' || arg[1] != '-') {
            /* positional */
            const char **p = realloc(pos, (size_t)(npos + 1) * sizeof *p);
            if (!p) {
                free(pos);
                fprintf(stderr, "flower: out of memory\n");
                return -1;
            }
            pos = p;
            pos[npos++] = arg;
            continue;
        }
        if (!arg[2]) { /* lone "--": everything after is positional */
            endopts = 1;
            continue;
        }
        {
            /* split --name[=value] */
            const char *eq = strchr(arg + 2, '=');
            size_t namelen = eq ? (size_t)(eq - (arg + 2)) : strlen(arg + 2);
            char name[64];
            const struct cli_opt *opt;
            const char *value = NULL;
            if (namelen >= sizeof name) namelen = sizeof name - 1;
            memcpy(name, arg + 2, namelen);
            name[namelen] = '\0';
            opt = opt_lookup(opts, nopts, name);
            if (!opt && !strcmp(name, "help")) {
                /* -h/--help always recognized, no table entry needed */
                out->flags |= 1;
                continue;
            }
            if (!opt) {
                fprintf(stderr, "flower: unknown option '--%s'\n",
                        name);
                free(pos);
                return -1;
            }
            if (opt->has_arg) {
                if (eq) {
                    value = eq + 1;
                } else if (i + 1 < argc) {
                    value = argv[++i];
                } else {
                    fprintf(stderr,
                            "flower: option '--%s' requires an "
                            "argument\n",
                            name);
                    free(pos);
                    return -2;
                }
                out->name_value = value;
            } else {
                if (!strcmp(name, "help")) out->flags |= 1;
            }
        }
    }
    out->pos = pos;
    out->npos = npos;
    return 0;
}
