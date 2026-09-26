#ifndef FLOWER_HTTP_H
#define FLOWER_HTTP_H

#include <stdbool.h>
#include <stddef.h>

/* Minimal HTTP/1.1 request-head parser + response builders.
 * Enough for GET/HEAD + Server-Sent Events; see README for limits. */

typedef struct {
    char method[8];
    char target[2048];
    int  minor;       /* HTTP version is 1.minor (0 or 1) */
    bool keep_alive;
} http_request_t;

/* Parse one request head from buf.
 * Returns  1: complete, *consumed = head size (incl. final CRLFCRLF)
 *          0: incomplete, need more input
 *         -1: malformed */
int http_parse_request(const char *buf, size_t len,
                       http_request_t *req, size_t *consumed);

const char *http_status_text(int code);

/* Build a full response (status line + headers + body) into a fresh
 * malloc'd buffer. `extra` is inserted as raw header lines and must be
 * empty or end with CRLF. head_only=true omits the body but keeps
 * Content-Length (HEAD requests). Returns 0 on success. */
int http_build_response(char **out, size_t *out_len, int code,
                        const char *content_type, const char *extra,
                        const void *body, size_t body_len,
                        bool keep_alive, bool head_only);

/* Response head that switches the connection to an SSE stream
 * (no Content-Length; the connection stays open). */
int http_build_sse_head(char **out, size_t *out_len);

#endif
