# examples

small runnable scripts showing the subcommands of `flower`.
run any of them from the repo root after `make`:

    ./examples/scripts/<name>.sh

each script runs in a throwaway `$FLOWER_BASE` (`mktemp -d`), so it
never touches your real config under `$XDG_CONFIG_HOME/flower`.

## scripts

- `setup.sh` - create the base `flower.md` from a piped Name and Role on
  stdin, then print the resulting file
- `project.sh` - create project P1 (Name/Path/Description), set details,
  add Fact and Rule context entries and two resources, then `project P1
  show`, `cat` the `context.md` and list resources
- `task-actions.sh` - task T1 with three actions (one via
  `--description`), set `Depends` on A3 and `Status` on A1, then
  `action list` and `action list --pending`
- `context-edit.sh` - context add, edit body, rename title, the
  duplicate-title rejection (exit 1), and remove

the full command walkthrough lives in the smoke scripts
`test/smoke-part1.sh` and `test/smoke-part2.sh`, which run the
docs/config_cli.md examples end to end in a throwaway `$FLOWER_BASE`.
