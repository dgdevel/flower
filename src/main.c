/* main.c - subcommand dispatch, help, version. command implementations
   for the flower config commands live in flower.c. */
#include "flower.h"
#include "crash_report.h"

#include <stdio.h>
#include <string.h>

static int usage(FILE *out) {
    fputs("usage: flower <command> [args]\n"
          "\n"
          "commands:\n"
          "  setup                          create/update $FLOWER_BASE/flower.md\n"
          "  project <P> set <Key> \"<Value>\"\n"
          "                                edit project config.md (Name, Path,\n"
          "                                Description)\n"
          "  project <P> show               print project config.md\n"
          "  project <P> details set|remove|show [Field]\n"
          "                                edit projects/<P>/details.md\n"
          "  project <P> context add|edit|rename|remove|list\n"
          "                                edit projects/<P>/context.md\n"
          "  project <P> resource add|set|remove|list\n"
          "                                edit projects/<P>/resources.md\n"
          "  project <P> task <T> set <Key> \"<Value>\"\n"
          "                                edit task config.md (Name,\n"
          "                                Description)\n"
          "  project <P> task <T> show      print task config.md\n"
          "  project <P> task <T> context <verb> [args]\n"
          "                                edit task context.md\n"
          "  project <P> task <T> action add \"<Title>\" [--description \"<text>\"]\n"
          "                                add action to actions.md\n"
          "  project <P> task <T> action <A> set <Key> \"<Value>\"\n"
          "                                edit action (Description, Status,\n"
          "                                Depends)\n"
          "  project <P> task <T> action <A> remove\n"
          "                                drop action chapter\n"
          "  project <P> task <T> action list [--pending|--complete]\n"
          "                                list \"<id>: <title> [<status>]\"\n"
          "  project <P> remove               delete project directory\n"
          "  project <P> task <T> remove      delete task directory\n"
          "  edit <path>                    $EDITOR on file under FLOWER_BASE,\n"
          "                                validated before saving\n"
          "  update [--yes]                 check github for a newer release\n"
          "                                and install it\n"
          "  help                           show this help\n"
          "  version                        show the version\n",
          out);
    return 1;
}

static int cmd_help(void) {
    usage(stdout);
    return 0;
}

static int cmd_version(void) {
    printf("flower %s\n", FLOWER_VERSION);
    return 0;
}

static int cmd_update(int argc, char **argv) {
    return update_run(argc, argv);
}

int main(int argc, char **argv) {
    const char *cmd;
    crash_report_init(argc > 0 && argv[0] ? argv[0] : "flower");
    if (argc < 2) return usage(stderr);
    cmd = argv[1];
    if (!strcmp(cmd, "setup")) return flower_main(argc, argv);
    if (!strcmp(cmd, "project")) return flower_main(argc, argv);
    if (!strcmp(cmd, "edit")) return flower_main(argc, argv);
    if (!strcmp(cmd, "update")) return cmd_update(argc, argv);
    if (!strcmp(cmd, "help")) return cmd_help();
    if (!strcmp(cmd, "version")) return cmd_version();
    fprintf(stderr, "flower: unknown command '%s'\n", cmd);
    return usage(stderr);
}
