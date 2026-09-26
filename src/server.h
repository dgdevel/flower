#ifndef FLOWER_SERVER_H
#define FLOWER_SERVER_H

#include <stdint.h>

/* Runs the HTTP server until SIGINT/SIGTERM. Returns a process exit code. */
int server_run(const char *bind_addr /* NULL = all interfaces */, uint16_t port);

#endif
