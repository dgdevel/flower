# flower

flower is a command line tool for LLM-assisted project management. It keeps project data in files on your computer and uses a language model (LLM) to scan code, collect facts, and work tasks and actions.

## Tech stack

- C language, C11.
- Make build with a plain Makefile.
- cJSON library for JSON. Install it with your package manager (`libcjson-dev` on Debian, `cjson` on Homebrew).
- libcurl for the update command (`libcurl4-openssl-dev` on Debian, `curl` on Homebrew).
- `llmkit` does the LLM and MCP calls

## Build and run

You need a C compiler, `make`, `pkg-config`, cJSON, and the `llmkit` binary in `$PATH`.

```
make
./flower setup
```

`make` writes the binary `flower` into the project root. `make check` runs the file io round trip tests (no network, no LLM). `make clean` removes the build files.

More targets: `make asan` builds the whole binary with address+undefined sanitizer into `build/asan/flower-asan`; `make fuzz` builds the libFuzzer harnesses in `fuzz/` (clang required, run e.g. `./fuzz/fuzz_json`); `make check-vg` runs the test suite under valgrind (log in `test/valgrind.log`).

Release targets: `make release TAG=v0.1.0` tags the release, builds and publishes binaries to GitHub Releases (needs `gh`); `make packages TAG=v0.1.0` builds .deb/.rpm/.pkg.tar.zst/.zip packages in docker, artifacts land in `dist/`.

## Commands

Run `./flower` with no command to see the usage text. Exit codes: 0 ok, 1 runtime error, 2 usage, 3 no project found, 4 no current task.

- `flower setup`: interactive configuration of the LLM endpoint and your name and role. Writes `flower.json` with mode `0600`.
- `flower project [--json]`: show the project that covers the current directory: id, name, path, task count, current task, then the full data set: details.md, context items (parsed from context.md), resources (parsed from resources.json). `--json` prints the same as one json object (`details` string, `context_items` and `resources` arrays of `{type, content}`).
- `flower project scan`: LLM conversation that fills the project data. No project covers the cwd yet: a new project is created first. The model gets one project tool, `update_project(details, contextItems, resources)`: a single call writes `details.md`, `context.md`, `resources.json` and ends the scan. For inspection it gets readonly tools (`files_list`, `files_search`, `file_read`, `file_analyze`) rooted at the project directory - the raw filesystem root is never exposed.
- `flower task`: show the current task. Same output as `task show`.
- `flower task list`: list the tasks of the project as `P1-T1  name` lines. The current task is marked.
- `flower task show [ID]`: show task details. `ID` is `T1` for the current project or `P2-T1` for another project. Without `ID`, the current task is shown.
- `flower task set-current <ID>`: set the current task pointer.
- `flower task create <prompt>`: create a task from the prompt (argv words joined, or stdin until end of file) and fill `config.json`, `context.md`, `resources.json`, `actions.json` in an LLM conversation. The new task becomes current.
- `flower task update [--id=ID] <prompt>`: same flow against an existing task, the current task by default.
- `flower action list`: list the actions of the current task: id, state, type, outcome, name, blocked-by.
- `flower action resume [ID]`: LLM conversation on the action record with task and project context. The model works the action with tools. After the conversation, flower asks for the new state (Pending, In Progress, Paused, Completed) and outcome (Success, Partial, Failure) and writes `actions.json`. Without an ID, the first action that is not Completed is picked.
- `flower update [--yes]`: check GitHub for a newer release and install it.
- `flower version`: show the version.

## LLM conversations

Every LLM command is a conversation: thinking, tool calls, and the response render on the terminal as they arrive. A `>` prompt after each turn accepts a follow-up; an empty line or `q` ends the session. Each session keeps its transcript under `$FLOWER_BASE/projects/<id>/sessions/` (project scans) or `.../tasks/<id>/sessions/` (task and action commands), in the llmkit records format.

The model never writes files with tools, except in a scan: there the `update_project` project tool writes the three data files itself (details.md, context.md, resources.json) in one validated call, and the scan ends. Everywhere else the model proposes files as fenced `flower` blocks that name the target path (relative to `$FLOWER_BASE`) and hold the full content. flower shows each proposal and writes it only after you answer `y`. This keeps every write under your control and reviewable.

## Data layout

Projects get ids `P1`, `P2`, and so on. Tasks get ids `T1`, `T2`, and so on within their project. Actions get ids `A1`, `A2`, and so on within their task. `P2-T1` names a task of another project.

The configuration base directory is `$FLOWER_BASE`. The tool looks for it in this order:

1. `$XDG_CONFIG_DIR/flower`
2. `$HOME/.config/flower`

```
$FLOWER_BASE/flower.json                      llm settings, user name and role
$FLOWER_BASE/projects/P1/config.json          name, path, description
$FLOWER_BASE/projects/P1/details.md           chapters: Output, Objectives, Scope, ...
$FLOWER_BASE/projects/P1/context.md           items: Fact, Pattern, Risk, ...
$FLOWER_BASE/projects/P1/resources.json       array of paths, urls, pointers
$FLOWER_BASE/projects/P1/current-task         task id of the current task
$FLOWER_BASE/projects/P1/sessions/            scan transcripts (jsonl)
$FLOWER_BASE/projects/P1/tasks/T1/config.json name, description
$FLOWER_BASE/projects/P1/tasks/T1/context.md  task items
$FLOWER_BASE/projects/P1/tasks/T1/resources.json
$FLOWER_BASE/projects/P1/tasks/T1/actions.json
$FLOWER_BASE/projects/P1/tasks/T1/sessions/   task and action transcripts
```

## Code layout

- `src/main.c` dispatches commands and prints the usage text.
- `src/setup.c` holds the setup command.
- `src/config.c` resolves the configuration directory, loads settings, and holds generic file helpers.
- `src/project.c` holds the project registry and project file io.
- `src/task.c` holds task io, the current-task pointer, and actions io.
- `src/project_cmds.c`, `src/task_cmds.c`, and `src/scan.c` hold the command surfaces.
- `src/llm.c` bridges to the `llmkit runner`: seed building, turn running, rendering, proposal parsing and application.
- `src/prompt.c` assembles the shared system prompt.
- `src/mcp_scan.c` holds the scan project server (`flower mcp-scan <P-id>`): the `update_project` tool.
- `test/check.c` holds the file io round trip tests, run with `make check`.
- `src/flower.h` declares the shared functions.
- `llmkit/` holds the llmkit library, a separate project.

## Debug aid

`flower llm-ping` is a hidden command that runs one turn (`flower llm-ping converse` for the conversation loop). It needs settings but no project.

## Roadmap

A web app with the same features is planned but out of scope for now.
