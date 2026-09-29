#ifndef FLOWER_HTML_H
#define FLOWER_HTML_H

/*
 * html — a tolerant reader for the two shapes flower actually meets
 * on the web: search-engine result pages and articles. It builds a
 * small DOM from arbitrary (often broken) markup, then either walks
 * it (DuckDuckGo result extraction) or picks the main content and
 * serializes it as markdown (a readability-lite: strip obvious
 * non-content, score paragraph containers by text, keep the winner).
 *
 * Deliberately not a browser: no css, no scripting, no tree fixing
 * beyond tag matching. Everything is best-effort — a weird page
 * yields a rough extraction, never a crash.
 */

#include <stddef.h>

typedef struct html_node {
    struct html_node *parent, *child, *last_child, *next;
    char tag[12];      /* lowercased element name; "" = text node */
    char *class_;      /* class attribute (decoded), NULL if absent */
    char *id;          /* id attribute (decoded), NULL if absent */
    char *href;        /* href attribute (decoded), NULL if absent */
    char *text;        /* text node content (entities decoded) */
} html_node_t;

typedef struct {
    html_node_t *root; /* synthetic root holding the parsed top level */
    size_t nodes;      /* nodes kept (caps apply; overflow is dropped) */
} html_doc_t;

/* Parse (best effort; caps protect against pathological input).
 * Returns NULL only on out-of-memory at the very start. */
html_doc_t *html_parse(const char *buf, size_t len);
void html_doc_free(html_doc_t *doc);

/* Depth-first pre-order walk from `n` (inclusive). A nonzero cb
 * return stops the walk and becomes html_walk's return (else 0). */
int html_walk(html_node_t *n, int (*cb)(html_node_t *, void *), void *ctx);

/* Case-insensitive tag compare ("" matches nothing). */
int html_is(const html_node_t *n, const char *tag);
/* Whole-token match against the class attribute. */
int html_has_class(const html_node_t *n, const char *cls);

/* Malloc'd text of the subtree, whitespace runs collapsed to single
 * spaces and trimmed (titles, snippets, link labels). NULL on OOM. */
char *html_text(const html_node_t *n);

/* Readability-lite: pick the main content subtree (an <article>/<main>
 * with substance, else the best-scored container) and serialize it as
 * markdown — headings, paragraphs, lists, links, emphasis, code.
 * Malloc'd; never NULL ("" when nothing readable is found). */
char *html_to_markdown(html_doc_t *doc);

#endif
