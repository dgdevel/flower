#ifndef FLOWER_PROMPTS_H
#define FLOWER_PROMPTS_H

/*
 * prompts — the compiled-in prompt texts flower ships: the system
 * prompts of the builtin agents and the descriptions of its own mcp
 * tools. The files live as plain .txt under prompts/ in the repo
 * (agents/<name>/system_prompt.txt, mcp/<tool>/description.txt,
 * mcp/<tool>/arguments/<arg>.txt), are compiled in at build time by
 * tools/embed (like web/ assets, but never served over http) and are
 * meant to be tweaked freely — rebuild and they are live.
 *
 * prompt_render substitutes the small {{variable}} template language
 * used to inject task/project context into a prompt.
 */

#include "projects.h"
#include <stddef.h>

/* Look a prompt up by path relative to prompts/ (no leading slash,
 * e.g. "agents/online_researcher/system_prompt.txt"). Returns a
 * malloc'd, NUL-terminated copy with trailing whitespace trimmed, or
 * NULL when no such prompt is compiled in. */
char *prompt_text(const char *name);

/*
 * Render {{variable}} templates. Known variables (a NULL project
 * renders them as empty strings):
 *   {{project_path}}        the project's working directory —
 *                           available, but unused by the shipped
 *                           prompts: the fs tools are project-
 *                           relative, so disclosing where the
 *                           project lives is unnecessary
 *   {{project_name}}        the project's title
 *   {{project_attributes}}  its detail fields, "Field: text" per line
 *                           (empty fields omitted; multi-line fields
 *                           indented), empty when all are empty
 *   {{project_context}}     its typed context items, "- [type] text"
 *                           per line, empty when there are none
 * Unknown {{...}} tokens are left verbatim so typos stay visible.
 * Returns a malloc'd string; never NULL (falls back to a copy).
 */
char *prompt_render(const char *tpl, const project_t *proj);

#endif
