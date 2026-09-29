#ifndef FLOWER_WEB_H
#define FLOWER_WEB_H

/*
 * web — flower's outbound reader: fetch a url and turn it into text.
 * libcurl does the transport (http/https, and file:// so tests can
 * feed fixtures); src/html.c does the reading. Two operations back
 * the /mcp tools:
 *
 *   web_search  query → "Url: …\nDescription: …" records parsed out
 *               of DuckDuckGo's html endpoint
 *   web_read    url → the page's main content as markdown
 */

#include <stddef.h>

/* GET `url` into a malloc'd, NUL-terminated buffer. Redirects are
 * followed; the body is capped (~8 MB) and timed out (~25 s).
 * Returns 0 on a 2xx answer; on failure -1 with a short message in
 * err ("search failed: …"-style prefixes are the caller's). */
int web_get(const char *url, char **body, size_t *len,
            char *err, size_t err_n);

/* Search the web and return the top results as plain text records,
 * one blank line between them:
 *
 *   Url: https://example.com/page
 *   Description: Title — snippet text
 *
 * Returns a malloc'd string ("" when nothing parsed — treat as "no
 * results"), or NULL with err filled when the request itself failed. */
char *web_search(const char *query, char *err, size_t err_n);

/* Fetch one page and return its readable content as markdown.
 * Returns a malloc'd string ("" when the page had no readable
 * body), or NULL with err filled when the fetch failed. */
char *web_read(const char *url, char *err, size_t err_n);

/* The DuckDuckGo html parsing, split out so tests can feed fixtures:
 * renders the record list shown above from a result page body. */
char *web_ddg_records(const char *html, size_t len);

#endif
