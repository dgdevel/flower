#ifndef FLOWER_ANALYZE_H
#define FLOWER_ANALYZE_H

/*
 * analyze — the file-structure analyzers behind the analyze tool:
 * given a text file of a known type, its shape comes back as one
 * line per element (line number, an indent per nesting level, the
 * element — a full signature for a function) instead of the
 * file's whole content. See src/fs.c for the tool wiring and
 * path resolution.
 *
 * One analyzer per file type, registered in the table at the bottom
 * of analyze.c: adding a type is an analyze_<name>() function next
 * to its siblings there plus one { name, extensions, fn } entry.
 */

#include "util.h"

#include <stddef.h>

/* what an analyzer writes into: node_add() (in analyze.c) appends
 * one rendered node line and enforces the shared node cap */
typedef struct {
    sbuf_t out;
    int count;
    int truncated;
} nodes_t;

/* one analyzer: return 0, or -1 when out of memory. Nodes go
 * through node_add(); an empty result renders as "(nothing
 * found)" — refusing a file (binary, wrong type) is the caller's
 * job, not the analyzer's. */
typedef int (*analyze_fn)(const char *text, nodes_t *ns);

typedef struct {
    const char *name;           /* labels the reply: "[markdown]" */
    const char *const *exts;    /* lowercase, dot-prefixed, NULL-ended */
    analyze_fn fn;
} analyzer_t;

/* the registry, in a stable order */
const analyzer_t *analyzers(void);
size_t analyzers_count(void);

/* the analyzer owning path's extension (case-insensitive), NULL
 * when the file type is unknown */
const analyzer_t *analyzer_for_ext(const char *path);

/* the tool body: read `path`, refuse directories, oversized and
 * binary files and unknown types, then render —
 * "path  [kind]  N lines" and one node per line. Returns a
 * malloc'd string, or NULL with a short message in err. */
char *analyze_file(const char *path, char *err, size_t err_n);

#endif
