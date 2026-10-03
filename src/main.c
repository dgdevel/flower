/* flower — single-binary webapp server. See README.md. */
#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "scan.h"
#include "theme.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
        "flower — single-binary webapp server\n"
        "\n"
        "usage: %s [-b ADDR] [-p PORT] [-c DIR] [-l BIN] [-h]\n"
        "       %s researcher <agent> --project <id> [--llm <name>]\n"
        "                    [--base-url <url>] [-c DIR] [-l BIN]\n"
        "                    (a scan's researcher agent as a stdio mcp\n"
        "                    server — 'flower researcher -h' says more)\n"
        "\n"
        "options:\n"
        "  -b ADDR   address to bind (default 0.0.0.0 = all interfaces)\n"
        "  -p PORT   port to listen on (default 8080)\n"
        "  -c DIR    config directory (default: $XDG_CONFIG_HOME/flower,\n"
        "            or ~/.config/flower)\n"
        "  -l BIN    the llmkit binary the project scan agent runs\n"
        "            (default: $FLOWER_LLMKIT, or llmkit from PATH)\n"
        "  -h        show this help\n",
        prog, prog);
}

int main(int argc, char **argv)
{
    /* the debug subcommand: one researcher agent as a stdio mcp
     * server (src/scan.c); it never serves http */
    if (argc > 1 && strcmp(argv[1], "researcher") == 0)
        return researcher_main(argc - 1, argv + 1);

    const char *addr = NULL; /* NULL = bind all interfaces (0.0.0.0) */
    const char *cfg_dir = NULL;
    const char *llmkit = NULL;
    long port = 8080;

    int opt;
    while ((opt = getopt(argc, argv, "b:c:hl:p:")) != -1) {
        switch (opt) {
        case 'b':
            addr = strcmp(optarg, "*") == 0 ? NULL : optarg;
            break;
        case 'c':
            cfg_dir = optarg;
            break;
        case 'l':
            llmkit = optarg;
            break;
        case 'p': {
            char *end = NULL;
            port = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || port < 1 || port > 65535) {
                fprintf(stderr, "flower: invalid port '%s'\n", optarg);
                return 2;
            }
            break;
        }
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        default:
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (theme_init(cfg_dir) != 0) {
        fprintf(stderr,
                "flower: cannot resolve or create config directory "
                "(set HOME, XDG_CONFIG_HOME, or use -c DIR)\n");
        return 1;
    }
    if (!llmkit)
        llmkit = getenv("FLOWER_LLMKIT"); /* may stay NULL: "llmkit" */
    return server_run(addr, (uint16_t)port, llmkit);
}
