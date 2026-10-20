/* scan.c - `flower project scan`: llm-guided project data collection. */
#define _POSIX_C_SOURCE 200809L

#include "flower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* scan flow:
 * 1. resolve project for cwd. none yet -> project_create(cwd).
 * 2. build seed: prompt (identity, fenced-block contract, project block
 *    with current files) + user text that asks for config.json,
 *    details.md, context.md, resources.json proposals.
 * 3. conversation via llm_converse. proposals are applied on approval.
 * 4. transcript under projects/<id>/sessions/scan-<pid>.jsonl. */
int scan_cmd(int argc, char **argv)
{
    struct flower_settings s;
    struct flower_project p;
    char transcript[FLOWER_PATH_MAX];
    char sessions_dir[FLOWER_PATH_MAX];
    char prompt_text[512];
    char *seed, *prompt;
    int fresh;

    (void)argc;
    (void)argv;
    if (flower_settings_load(&s) != 0) {
        fprintf(stderr, "flower project scan: run 'flower setup' first\n");
        return 1;
    }
    if (project_find_current(&p) == 0) {
        fresh = 0;
        printf("project %s (%s) covers this directory\n", p.id, p.name);
    } else {
        char cwd[FLOWER_PATH_MAX];

        if (!getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "flower project scan: cannot resolve the current "
                            "directory\n");
            return 1;
        }
        if (project_create(cwd, &p) != 0) {
            fprintf(stderr, "flower project scan: cannot create the project\n");
            return 1;
        }
        fresh = 1;
        printf("created project %s (%s) for %s\n", p.id, p.name, p.path);
    }

    prompt = prompt_build(&s, &p, NULL, NULL);
    if (!prompt) {
        fprintf(stderr, "flower project scan: cannot build the prompt\n");
        return 1;
    }
    if (fresh)
        snprintf(prompt_text, sizeof(prompt_text),
                 "Scan the project directory (%s) with the tools. Then "
                 "propose the four data files of this new flower project: "
                 "projects/%s/config.json (name, path, description), "
                 "projects/%s/details.md (chapters: Output, Objectives, "
                 "Scope, Constraints, Resources, Stakeholders, only the "
                 "applicable ones), projects/%s/context.md (list items: "
                 "one per line, each of type Fact, Pattern, Risk, Success "
                 "metric, Failure sign, Evaluation method, or Rule), "
                 "projects/%s/resources.json (a json array of strings: "
                 "paths, urls, pointers). Use one flower block per file. "
                 "Ask me questions when the scan leaves doubt.",
                 p.path, p.id, p.id, p.id, p.id);
    else
        snprintf(prompt_text, sizeof(prompt_text),
                 "The project %s exists. Read its current data files "
                 "(config.json, details.md, context.md, resources.json "
                 "under projects/%s/), scan the project directory (%s) "
                 "for changes with the tools, then propose updated files "
                 "as flower blocks where the data is stale. Ask me "
                 "questions when something is unclear.",
                 p.id, p.id, p.path);
    {
        const char *parts[1];

        parts[0] = prompt;
        seed = llm_build_seed(&s, parts, 1, prompt_text);
    }
    free(prompt);
    if (!seed) {
        fprintf(stderr, "flower project scan: cannot build the seed\n");
        return 1;
    }

    snprintf(sessions_dir, sizeof(sessions_dir), "%s/sessions", p.dir);
    (void)flower_ensure_dir(sessions_dir);
    snprintf(transcript, sizeof(transcript),
             "%s/scan-%d.jsonl", sessions_dir, (int)getpid());

    return llm_converse(&s, seed, transcript);
}
