#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* case-insensitive substring search (ASCII) */
static bool ci_contains(const char *h, const char *n)
{
    for (; *h; h++) {
        size_t i = 0;
        while (n[i] && h[i] && lower(h[i]) == lower(n[i])) i++;
        if (!n[i]) return true;
    }
    return false;
}

/* case-insensitive equality of the first n chars (t must be lowercase) */
static bool ci_eq_n(const char *s, const char *t, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (lower(s[i]) != t[i]) return false;
    return true;
}

static bool is_tchar(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
           c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' ||
           c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

int http_parse_request(const char *buf, size_t len,
                       http_request_t *req, size_t *consumed)
{
    /* locate end of the request head */
    size_t head_end = 0;
    bool found = false;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\0') return -1;
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            head_end = i + 4;
            found = true;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < len; i++)
            if (buf[i] == '\0') return -1;
        return 0; /* need more input */
    }

    /* --- request line --- */
    const char *eol = memchr(buf, '\n', head_end);
    if (!eol) return -1;
    size_t line_len = (size_t)(eol - buf); /* ends with '\r' */
    if (line_len == 0 || buf[line_len - 1] != '\r') return -1;
    line_len--; /* drop the '\r' */

    size_t i = 0, m = 0;
    while (i < line_len && buf[i] != ' ') {
        if (!is_tchar((unsigned char)buf[i])) return -1;
        if (m + 1 >= sizeof req->method) return -1;
        req->method[m++] = buf[i++];
    }
    req->method[m] = '\0';
    if (m == 0 || i >= line_len) return -1;
    i++; /* single SP */

    size_t t = 0;
    while (i < line_len && buf[i] != ' ') {
        unsigned char c = (unsigned char)buf[i];
        if (c <= 0x20 || c == 0x7f) return -1; /* no space/control in target */
        if (t + 1 >= sizeof req->target) return -1;
        req->target[t++] = buf[i++];
    }
    req->target[t] = '\0';
    if (t == 0 || req->target[0] != '/' || i >= line_len) return -1;
    i++; /* single SP */

    /* version: exactly "HTTP/1.0" or "HTTP/1.1" */
    size_t rem = line_len - i;
    if (rem != 8 || memcmp(buf + i, "HTTP/1.", 7) != 0 ||
        (buf[i + 7] != '0' && buf[i + 7] != '1'))
        return -1;
    req->minor = buf[i + 7] - '0';

    /* --- headers: we only care about Connection --- */
    bool hdr_close = false, hdr_keepalive = false;
    size_t pos = line_len + 2; /* skip the request line's CRLF
                                * (line_len excludes it, so +2, not +1) */
    while (pos < head_end) {
        const char *le = memchr(buf + pos, '\n', head_end - pos);
        size_t ll = le ? (size_t)(le - (buf + pos)) : head_end - pos;
        size_t next = le ? (size_t)(le - buf) + 1 : head_end;
        if (ll > 0 && buf[pos + ll - 1] == '\r') ll--;
        if (ll == 0) break; /* blank line = end of headers */

        const char *colon = memchr(buf + pos, ':', ll);
        if (colon) {
            size_t name_len = (size_t)(colon - (buf + pos));
            if (name_len == 10 && ci_eq_n(buf + pos, "connection", 10)) {
                const char *v = colon + 1;
                while (v < buf + pos + ll && (*v == ' ' || *v == '\t')) v++;
                size_t vl = (size_t)(buf + pos + ll - v);
                char val[64];
                if (vl < sizeof val) {
                    memcpy(val, v, vl);
                    val[vl] = '\0';
                    if (ci_contains(val, "close")) hdr_close = true;
                    if (ci_contains(val, "keep-alive")) hdr_keepalive = true;
                }
            }
        }
        pos = next;
    }

    req->keep_alive = (req->minor == 1) ? !hdr_close : hdr_keepalive;
    *consumed = head_end;
    return 1;
}

const char *http_status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 431: return "Request Header Fields Too Large";
    default:  return "Unknown";
    }
}

int http_build_response(char **out, size_t *out_len, int code,
                        const char *content_type, const char *extra,
                        const void *body, size_t body_len,
                        bool keep_alive, bool head_only)
{
    const char *fmt =
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: %s\r\n"
        "%s"
        "\r\n";

    size_t head_size = (size_t)snprintf(NULL, 0, fmt, code,
                                        http_status_text(code), content_type,
                                        body_len,
                                        keep_alive ? "keep-alive" : "close",
                                        extra ? extra : "");
    /* Wire layout must be exactly [head][body]:
     * snprintf writes its NUL terminator at buf[head_size]; the body then
     * OVERWRITES that byte, so no stray NUL rides between head and body
     * (a NUL there breaks JS with "illegal character U+0000"). */
    char *buf = malloc(head_size + body_len + 1);
    if (!buf) return -1;
    int n = snprintf(buf, head_size + 1, fmt, code, http_status_text(code),
                     content_type, body_len,
                     keep_alive ? "keep-alive" : "close",
                     extra ? extra : "");
    if (n < 0 || (size_t)n != head_size) {
        free(buf);
        return -1;
    }
    if (!head_only && body_len > 0)
        memcpy(buf + head_size, body, body_len);
    *out = buf;
    *out_len = head_size + (head_only ? 0 : body_len);
    return 0;
}

int http_build_sse_head(char **out, size_t *out_len)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "X-Accel-Buffering: no\r\n" /* nginx-style proxies: stream, don't buffer */
        "Connection: keep-alive\r\n"
        "\r\n";
    size_t n = sizeof head - 1;
    char *buf = malloc(n);
    if (!buf) return -1;
    memcpy(buf, head, n);
    *out = buf;
    *out_len = n;
    return 0;
}
