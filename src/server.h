#ifndef FLOWER_SERVER_H
#define FLOWER_SERVER_H

#include <stdint.h>

/* Runs the HTTP server until SIGINT/SIGTERM. `llmkit` is the binary
 * the project scan agent runs (a path, or a name found via PATH;
 * NULL falls back to "llmkit"). Returns a process exit code. */
int server_run(const char *bind_addr /* NULL = all interfaces */,
               uint16_t port, const char *llmkit);

#endif
