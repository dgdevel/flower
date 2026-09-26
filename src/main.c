/* flower — single-binary webapp server. See README.md. */
#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
        "flower — single-binary webapp server\n"
        "\n"
        "usage: %s [-b ADDR] [-p PORT] [-h]\n"
        "\n"
        "options:\n"
        "  -b ADDR   address to bind (default 0.0.0.0 = all interfaces)\n"
        "  -p PORT   port to listen on (default 8080)\n"
        "  -h        show this help\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *addr = NULL; /* NULL = bind all interfaces (0.0.0.0) */
    long port = 8080;

    int opt;
    while ((opt = getopt(argc, argv, "b:hp:")) != -1) {
        switch (opt) {
        case 'b':
            addr = strcmp(optarg, "*") == 0 ? NULL : optarg;
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
    return server_run(addr, (uint16_t)port);
}
