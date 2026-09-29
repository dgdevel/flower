/*
 * web — outbound fetching + the two web tools behind /mcp.
 * See web.h; transport via libcurl, reading via src/html.c.
 */
#define _POSIX_C_SOURCE 200809L

#include "web.h"

#include "html.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FETCH_MAX   (8 * 1024 * 1024) /* response body cap */
#define CONNECT_TO  10L               /* seconds */
#define TOTAL_TO    25L               /* seconds */
#define RESULTS_MAX 10                /* records web_search returns */

/* a browser-ish UA: several sites (duckduckgo included) answer
 * curl's default with a bot wall */
#define UA "Mozilla/5.0 (X11; Linux x86_64) flower/1.0"

static const char *DDG_URL = "https://html.duckduckgo.com/html/?q=";

/* ---------- url helpers ---------- */

/* percent-encode everything but unreserved characters */
static char *url_encode(const char *s)
{
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1);
    if (!out) return NULL;
    char *w = out;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            *w++ = (char)c;
        } else {
            w += sprintf(w, "%%%02X", c);
        }
    }
    *w = '\0';
    return out;
}

/* percent-decode '+' as space too (query-string semantics) */
static char *url_decode(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out) return NULL;
    char *w = out;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+') {
            *w++ = ' ';
        } else if (s[i] == '%' && i + 2 < n &&
                   isxdigit((unsigned char)s[i + 1]) &&
                   isxdigit((unsigned char)s[i + 2])) {
            char hex[3] = { s[i + 1], s[i + 2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            *w++ = s[i];
        }
    }
    *w = '\0';
    return out;
}

/* the target of a duckduckgo redirect link:
 * "//duckduckgo.com/l/?uddg=<encoded>&rut=…" → the encoded url */
static char *ddg_target(const char *href)
{
    if (!href) return NULL;
    const char *uddg = strstr(href, "uddg=");
    if (uddg) {
        uddg += 5;
        const char *end = strchr(uddg, '&');
        size_t n = end ? (size_t)(end - uddg) : strlen(uddg);
        return url_decode(uddg, n);
    }
    /* a direct link; fix protocol-relative form */
    if (strncmp(href, "//", 2) == 0) {
        char *out = malloc(strlen(href) + 6);
        if (out) sprintf(out, "https:%s", href);
        return out;
    }
    return strdup(href);
}

/* ---------- fetch ---------- */

typedef struct {
    char *data;
    size_t len, cap;
    int overflow;
} body_t;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    body_t *b = ud;
    size_t n = size * nmemb;
    if (b->len + n > FETCH_MAX) {
        b->overflow = 1;
        return 0; /* short write: curl aborts the transfer */
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 8192;
        while (b->len + n + 1 > cap) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) return 0;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    if (b->data) b->data[b->len] = '\0';
    return n;
}

int web_get(const char *url, char **body, size_t *len,
            char *err, size_t err_n)
{
    *body = NULL;
    *len = 0;

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, err_n, "cannot init curl");
        return -1;
    }

    body_t b = { 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TO);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TOTAL_TO);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* gzip etc. */
    curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_IGNORED);
#if LIBCURL_VERSION_NUM >= 0x080400
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,
                     "http,https,file");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS |
                            CURLPROTO_FILE));
#endif

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (b.overflow) {
        snprintf(err, err_n, "response too large (max %d MB)",
                 FETCH_MAX / (1024 * 1024));
        free(b.data);
        return -1;
    }
    if (rc != CURLE_OK) {
        snprintf(err, err_n, "%s", curl_easy_strerror(rc));
        free(b.data);
        return -1;
    }
    /* file:// answers 0; http must be 2xx */
    if (status != 0 && (status < 200 || status >= 300)) {
        snprintf(err, err_n, "http status %ld", status);
        free(b.data);
        return -1;
    }
    if (!b.data) {
        b.data = malloc(1);
        if (!b.data) {
            snprintf(err, err_n, "out of memory");
            return -1;
        }
        b.data[0] = '\0';
    }
    *body = b.data;
    *len = b.overflow ? FETCH_MAX : b.len;
    return 0;
}

/* ---------- web_search ---------- */

typedef struct {
    char *url;    /* resolved link */
    char *title;
    char *snippet;
} ddg_hit_t;

typedef struct {
    ddg_hit_t hits[RESULTS_MAX];
    size_t count;
    /* state pairing titles with the snippet that follows them */
    int have_open;
} ddg_ctx_t;

static int ddg_anchor(html_node_t *n, void *ud)
{
    ddg_ctx_t *ctx = ud;
    if (!html_is(n, "a")) return 0;
    if (!html_has_class(n, "result__a") && !html_has_class(n, "result__snippet"))
        return 0;
    if (ctx->count >= RESULTS_MAX) return 1; /* enough */

    char *txt = html_text(n);
    if (!txt) return 0;

    if (html_has_class(n, "result__a")) {
        char *url = ddg_target(n->href);
        if (url && url[0]) {
            ddg_hit_t *h = &ctx->hits[ctx->count++];
            h->url = url;
            h->title = txt;
            h->snippet = NULL;
            ctx->have_open = 1;
            return 0;
        }
        free(url);
    } else if (ctx->have_open) {
        ctx->hits[ctx->count - 1].snippet = txt;
        ctx->have_open = 0;
        return 0;
    }
    free(txt);
    return 0;
}

char *web_ddg_records(const char *html, size_t len)
{
    html_doc_t *doc = html_parse(html, len);
    if (!doc) return NULL;

    ddg_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    html_walk(doc->root, ddg_anchor, &ctx);

    /* assemble the record list */
    char *out = malloc(1);
    size_t out_len = 0, out_cap = 1;
    if (!out) {
        html_doc_free(doc);
        return NULL;
    }
    out[0] = '\0';
    for (size_t i = 0; i < ctx.count; i++) {
        ddg_hit_t *h = &ctx.hits[i];
        const char *desc = h->snippet && h->snippet[0] ? h->snippet
                                                       : (h->title ? h->title : "");
        /* "Description: title — snippet" when both exist */
        size_t need = strlen("Url: \nDescription: \n\n") + strlen(h->url) +
                      strlen(desc) + 2;
        if (h->title && h->snippet && h->snippet[0])
            need += strlen(h->title) + 3;
        if (out_len + need + 1 > out_cap) {
            out_cap = out_len + need + 1;
            char *d = realloc(out, out_cap);
            if (!d) break;
            out = d;
        }
        out_len += sprintf(out + out_len, "Url: %s\n", h->url);
        if (h->title && h->snippet && h->snippet[0])
            out_len += sprintf(out + out_len, "Description: %s — %s\n",
                               h->title, h->snippet);
        else
            out_len += sprintf(out + out_len, "Description: %s\n", desc);
        out_len += sprintf(out + out_len, "\n");
    }

    for (size_t i = 0; i < ctx.count; i++) {
        free(ctx.hits[i].url);
        free(ctx.hits[i].title);
        free(ctx.hits[i].snippet);
    }
    html_doc_free(doc);
    return out;
}

char *web_search(const char *query, char *err, size_t err_n)
{
    if (!query || !*query) {
        snprintf(err, err_n, "empty query");
        return NULL;
    }
    char *q = url_encode(query);
    if (!q) {
        snprintf(err, err_n, "out of memory");
        return NULL;
    }
    char url[2048];
    snprintf(url, sizeof url, "%s%s", DDG_URL, q);
    free(q);

    char *body = NULL;
    size_t len = 0;
    if (web_get(url, &body, &len, err, err_n) != 0) {
        return NULL;
    }
    char *records = web_ddg_records(body, len);
    free(body);
    if (!records) {
        snprintf(err, err_n, "could not parse the result page");
        return NULL;
    }
    return records;
}

/* ---------- web_fetch ---------- */

char *web_read(const char *url, char *err, size_t err_n)
{
    if (!url || !*url) {
        snprintf(err, err_n, "empty url");
        return NULL;
    }
    if (strncasecmp(url, "http://", 7) != 0 &&
        strncasecmp(url, "https://", 8) != 0 &&
        strncasecmp(url, "file://", 7) != 0) {
        snprintf(err, err_n, "url must be http(s):// or file://");
        return NULL;
    }

    char *body = NULL;
    size_t len = 0;
    if (web_get(url, &body, &len, err, err_n) != 0)
        return NULL;

    html_doc_t *doc = html_parse(body, len);
    free(body);
    if (!doc) {
        snprintf(err, err_n, "could not parse the page");
        return NULL;
    }
    char *md = html_to_markdown(doc);
    html_doc_free(doc);
    if (!md) {
        snprintf(err, err_n, "could not extract the page content");
        return NULL;
    }
    return md;
}
