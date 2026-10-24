/* main.c - subcommand dispatch. */
#include "flower.h"

#include <stdio.h>
#include <string.h>

static int cmd_version(void)
{
    printf("flower %s\n", FLOWER_VERSION);
    return 0;
}

static int usage(void)
{
    fputs("usage: flower <command> [args]\n"
          "\n"
          "commands:\n"
          "  setup                      configure llm and user settings\n"
          "  project [--json]           show the project covering the cwd\n"
          "  project scan               llm conversation that creates or\n"
          "                             updates the project data files\n"
          "  task                       show the current task\n"
          "  task list                  list the tasks of the project\n"
          "  task show [ID]             show a task (ID: T1 or P2-T1)\n"
          "  task set-current <ID>      set the current task pointer\n"
          "  task create <prompt>       create a task and fill it with the\n"
          "                             llm (prompt: argv or stdin)\n"
          "  task update [--id=ID] <prompt>\n"
          "                             update a task with the llm\n"
          "  action list                list the actions of the current task\n"
          "  action resume [ID]         work an action with the llm, then\n"
          "                             record its state and outcome\n"
          "  update [--yes]           check github for a newer release and\n"
          "                             install it\n"
          "  version                  show the version\n"
          "\n"
          "llm commands need 'flower setup' and the llmkit binary in PATH.\n"
          "they write proposed files only after you approve each one.\n",
          stderr);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    if (!strcmp(argv[1], "setup"))
        return setup_run();
    if (!strcmp(argv[1], "project")) {
        if (argc >= 3 && !strcmp(argv[2], "scan"))
            return scan_cmd(argc - 2, argv + 2);
        return project_cmd(argc - 1, argv + 1);
    }
    if (!strcmp(argv[1], "task"))
        return task_cmd(argc - 1, argv + 1);
    if (!strcmp(argv[1], "action"))
        return action_cmd(argc - 1, argv + 1);
    if (!strcmp(argv[1], "llm-ping"))
        return llm_ping_cmd(argc - 1, argv + 1);
    if (!strcmp(argv[1], "mcp-scan"))
        return mcp_scan_cmd(argc - 2, argv + 2);
    if (!strcmp(argv[1], "version"))
        return cmd_version();
    if (!strcmp(argv[1], "update"))
        return update_cmd(argc - 1, argv + 1);
    return usage();
}
