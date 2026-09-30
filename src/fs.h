#ifndef FLOWER_FS_H
#define FLOWER_FS_H

/*
 * fs — the local-filesystem tools behind the mcp surfaces: read a
 * slice of a file, list a directory through a small glob language
 * (`*`, `*.*`, `*.ext` — each also with the recursive `**` prefix
 * to walk subdirectories), and grep the contents of the files a
 * filepath glob selects.
 *
 * Two surfaces share these tools. The bare /mcp is server-wide:
 * paths resolve like the flower process sees them — absolute paths
 * are the reliable form. A project surface (POST
 * /projects/{seq}/mcp) is grounded in the project's working
 * directory by fs_set_root(): paths are relative to it ("." is the
 * project itself, ".." may not climb out — the jail is checked
 * after lexical normalization) and replies and errors speak
 * project-relative paths, so the surface does not disclose where
 * the project lives. Access is whatever the server's own user may
 * do.
 */

#include <cJSON.h>
#include <stddef.h>

/* Read `length` bytes from `offset` (both clamped sensibly; see the
 * tool description) and return them as a malloc'd string, or NULL
 * with a short message in err (missing file, directory, binary
 * content, offset past end, …). */
char *fs_read_path(const char *path, long long offset, long long length,
                   char *err, size_t err_n);

/* List `dir` through the glob pattern; one path per line (directories
 * carry a trailing "/"), sorted, capped with a final "[truncated]"
 * line. Patterns with the recursive `**` prefix walk subdirectories
 * (symlinked
 * directories are listed but not descended). Returns a malloc'd
 * string, or NULL with err filled. */
char *fs_glob(const char *dir, const char *pattern,
              char *err, size_t err_n);

/* Search the contents of the files a filepath glob selects for
 * lines matching a POSIX extended regular expression (the grep -E
 * flavor, case-sensitive). The glob is the directory to search
 * plus the list_files leaf pattern: "DIR" with "*.c" searches the
 * .c files directly in it, the recursive ** leaf every
 * subdirectory too, a full file path that one file, and a
 * directory alone every file under it. Matches come back as
 * "path:line:text", sorted, with a final "[N matches in K of M
 * files]" tally; binary and over-8MB files are skipped, long lines
 * cut, the reply capped at 200 matches. "no files match <glob>"
 * when the glob selects nothing. Returns a malloc'd string, or
 * NULL with err filled (bad regex, bad directory, malformed
 * glob). */
char *fs_grep(const char *glob, const char *pattern,
              char *err, size_t err_n);

/* mcp tools/call dispatchers (the argument names match the schema):
 * read_file {path, offset?, length?}, list_files {path, glob},
 * grep {glob, pattern}. Same return contract as the web tools.
 * Path arguments are resolved against the surface root set by
 * fs_set_root() — see above. */
char *fs_tool_read_file(const cJSON *args, char *err, size_t err_n);
char *fs_tool_list_files(const cJSON *args, char *err, size_t err_n);
char *fs_tool_grep(const cJSON *args, char *err, size_t err_n);

/* Ground the tool dispatchers in `dir` (a project's working
 * directory): relative paths resolve under it and may not escape,
 * replies and errors come back project-relative. NULL (or an empty
 * string) restores the server-wide behavior — absolute paths only.
 * The server sets this around one tools/call dispatch; being
 * single-threaded is the locking. */
void fs_set_root(const char *dir);

/* is a root currently set? */
int fs_rooted(void);

#endif
