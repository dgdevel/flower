# flower CLI - configuration editing syntax

Full command line surface for editing every configuration file described in
docs/config_layout.md. Files on disk follow docs/config_markdown_format.md.

## Conventions

- Binary invoked as `./flower` from repo root.
- Identifiers are sequential and auto-assigned at creation: projects `P1, P2...`,
  tasks `T1, T2...`, actions `A1, A2...`. User never picks the numeric id.
- `<value>` arguments are quoted strings; spaces allowed inside quotes.
- Key names unique per chapter, chapter names unique per file. Duplicate name or
  key -> command fails, file unchanged, error message says what clashed.
- Chapters/keys not listed for a file are rejected (typo guard), except
  `resources.md` where chapter names are free-form.
- Every `set`/`add`/`remove` prints the resulting file path.
- Commands marked **[specified]** appear verbatim in config_layout.md. Others are
  proposals consistent with that grammar.

Grammar skeleton:

```
./flower setup
./flower project <P> <area> <verb> [args]
./flower project <P> task <T> <area> <verb> [args]
```

Areas: `(none)` = config.md, `details`, `context`, `resource`, `action`; `llm` is a top-level area of its own.

## User config - $FLOWER_BASE/flower.md

Chapter `# User`, keys `Name`, `Role`.

```
./flower setup
```

**[specified]** Interactive create/update of the whole file.

After the flower.md questions, setup offers a guided walk over llms.md: with no
profiles yet it offers to add a first one, otherwise it lists them and offers
to edit. It asks for the profile name, then `Api base`, `Protocol`
(`anthropic`, `openai`, `openai-responses`), `Api key`, `Model`, `Max tokens`,
`Reasoning effort`. Empty answer keeps the current value, eof (Ctrl-D) ends the
walk. Anthropic profiles want `Max tokens` set (llmkit rule). Repeatable keys
(`Header`, `Mcp proxy`, `Terminal tool`) stay with `flower llm <name> set`.

## LLM profiles - $FLOWER_BASE/llms.md

One chapter per named profile, keys inside each. Global, not per-project.

```
./flower llm add "claude" --base "https://api.anthropic.com" --protocol anthropic --key "sk-..." --model "claude-sonnet-4-5" --max-tokens 8192 --reasoning-effort high --header "X-Org=acme" --mcp-proxy "mcp.json" --terminal-tool "shell.exec"
./flower llm claude set Model "claude-opus-4-1"
./flower llm claude unset Header
./flower llm claude remove
./flower llm claude show
./flower llm list
```

- `add`: `--base` and `--protocol` required, every other flag optional. Repeatable
  flags `--header`, `--mcp-proxy`, `--terminal-tool` append in command order.
- Profile names: `[A-Za-z][A-Za-z0-9 _-]*`. Duplicate name -> command fails.
- Protocols: `anthropic`, `openai`, `openai-responses` (inherited from llmkit).
- `set` replaces the value for single keys, appends for `Header`, `Mcp proxy`
  and `Terminal tool`. Unknown key fails, missing profile fails.
- `set Protocol` validates the value. `unset Api base` / `unset Protocol` fail.
- `unset Header` removes every `Header` occurrence in the profile.
- `remove` deletes the whole chapter, `list` prints one profile name per line.

## Project config - projects/P1/config.md

Chapter `# Project`, keys `Name`, `Path`, `Description`.

```
./flower project P1 set Name "flower"        # [specified] creates P1 if missing
./flower project P1 set Path "/home/user/projects/flower"
./flower project P1 set Description "A project description"
./flower project P1 show
```

`set` on nonexistent project creates it. `set` on unknown key fails.

## Project details - projects/P1/details.md

Chapters = fields, value = bare chapter text. Fields: `Output`, `Objectives`,
`Scope`, `Constraints`, `Stakeholders`. All optional.

```
./flower project P1 details set Output "The project output are csv files."
./flower project P1 details set Stakeholders "Team A, Team B"
./flower project P1 details remove Output
./flower project P1 details show [Field]
```

`set` on existing field overwrites whole chapter text. `remove` deletes chapter.

## Project context - projects/P1/context.md

Chapters named `# <Prefix>: <Title>` plus optional bare body. Allowed prefixes:
`Fact`, `Pattern`, `Risk`, `Success metric`, `Failure sign`,
`Evaluation method`, `Rule`. Titles unique across the whole file, regardless of
prefix.

```
./flower project P1 context add Fact "configuration location" "Configuration always happens in directory X"
./flower project P1 context add Rule "temporary files" "Temp files are discarded automatically at process termination"
./flower project P1 context edit Fact "configuration location" "New body text"
./flower project P1 context rename Fact "configuration location" "config location"
./flower project P1 context remove Fact "configuration location"
./flower project P1 context list
```

- `add` fails on: unknown prefix, duplicate title, duplicate prefix+title.
- `edit` replaces body only.
- `rename` changes title, must stay unique.

## Project resources - projects/P1/resources.md

Free-form chapter name = resource name, bare value = pointer (url, path, or
`file#fragment`).

```
./flower project P1 resource add "staging webapp url" "https://www.example.org/"
./flower project P1 resource add "bulk csv generator" "src/product/feeds/generator.py#generate_csv"
./flower project P1 resource set "staging webapp url" "https://staging.example.org/"
./flower project P1 resource remove "bulk csv generator"
./flower project P1 resource list
```

`set` re-points existing resource, name unchanged.

## Task config - projects/P1/tasks/T1/config.md

Chapter `# Task`, keys `Name`, `Description`. Same shape as project config.

```
./flower project P1 task T1 set Name "mix ingredients"      # creates T1 if missing
./flower project P1 task T1 set Description "Combine water and flour"
./flower project P1 task T1 show
```

## Task context - projects/P1/tasks/T1/context.md

Identical format and verbs to project context:

```
./flower project P1 task T1 context add Fact "..." "..."
./flower project P1 task T1 context edit|rename|remove <Prefix> "<Title>" [...]
./flower project P1 task T1 context list
```

## Actions - projects/P1/tasks/T1/actions.md

Chapters named `# <A>: <Title>`, keys `Description`, `Status`, `Depends`.
`Status` values: `pending`, `complete`. `Depends` value: comma-separated action
ids, e.g. `"A1, A2"`.

```
./flower project P1 task T1 action add "take water"                     # -> A1
./flower project P1 task T1 action add "take flour" --description "Flour is in the cupboard."
./flower project P1 task T1 action A3 set Description "put water and flour in a bowl."
./flower project P1 task T1 action A3 set Status "complete"
./flower project P1 task T1 action A3 set Depends "A1, A2"
./flower project P1 task T1 action A3 remove
./flower project P1 task T1 action list [--pending|--complete]
```

- `add` assigns next free id; optional `--description <text>` avoids second call.
- `set Depends` fails on unknown action id in same task or self-reference.
- `remove` fails if another action still depends on it; remove dependents first.

## Project scan - staged llm session over the config

Runs one llm session (via the `llmkit runner`) in three stages that
read the project and write details.md, context.md and resources.md
themselves, through flower's own mcp servers. The session's tools:

- `details`, `context`, `resource`: `flower mcp <P> <area>` stdio json-rpc
  servers exposing the `details_*`, `context_*`, `resource_*` tools -
  same validation as the cli verbs above.
- `scout`: a readonly search subagent (`llmkit agent-as-tool` wrapping
  `llmkit builtin-mcp --read-only`) for listing, reading and grepping
  the project tree. it cannot write config. it retains its context
  across the scan's stages (one growing conversation, resettable by
  the main agent through its `context_reset` tool).

The stages run in one runner process: flower feeds header, llm and
tools records to its stdin, then per stage a system prompt, a kickoff
user message and a `flush` record (llmkit steering). The stage ends
when the final response of its turn ends with a line that is exactly
`[STEP_COMPLETE]`; if it does not, flower sends one nudge user record
and flush before giving up on the stage. Each stage's system prompt
embeds only the current content of the file that stage writes, with
every header shifted one `#` deeper, so the agent updates existing
entries instead of duplicating them:

- stage 1 fills details.md (Output, Objectives, Scope, Constraints,
  Stakeholders),
- stage 2 fills context.md (one chapter per finding, allowed prefixes),
- stage 3 fills resources.md (one named pointer per entry).

```
./flower project P1 scan claude            # run the session
./flower project P1 scan claude --dry-run  # print the staged runner jsonl only
```

- `<profile>` names a chapter in llms.md (see the llm section).
- nonexistent project is created first: config.md gets `Name` from the
  current directory name and `Path` from the current working directory
  (a note goes to stderr), so scanning a repo is one command.
- anthropic profiles need `Max tokens` (llmkit rule).
- profile `Mcp proxy` entries join the session as extra tool servers,
  `Terminal tool` entries mark tools as terminal (llmkit rule).
- artifacts under `projects/P1/scan/`: `scout-seed.jsonl` only (subagent
  seed, mode 0600). the runner records are written to the runner's
  stdin live; there is no input file.
- the live scan runs one `llmkit runner` process per turn: the runner
  exits after a final response instead of waiting for steering, so each
  stage (and each nudge) is a fresh runner fed the whole transcript so
  far plus the stage's records, ending in a flush and eof. a turn whose
  response lacks `[STEP_COMPLETE]` gets one nudge retry.
- the whole session is saved as a jsonl transcript under
  `projects/P1/llm-sessions/scan-<timestamp>.jsonl` (mode 0600 - it
  carries the api key): every record sent to the runner's stdin, in
  send order, followed by every runner output record (the runner's
  duplicate header line is dropped). the file is a valid continuation
  transcript, replayable with `llmkit runner < <file>`.
- the scout subagent's conversation is teed to
  `projects/P1/llm-sessions/scan-<timestamp>-scout.jsonl` (mode 0600,
  same guarantee): the seed records first, then every invoke's records
  as they happen - the subagent's llmkit writes it (the seed names the
  file in its `options.transcript`). context resets and rolled-back
  invokes are truncated out, so the file always mirrors the subagent's
  live context. it is a valid continuation transcript too. the file is
  created up front and may hold only the seed when a scan completed
  without ever invoking scout.
- exit code: the runner's own exit code when it exits nonzero (127
  means the llmkit binary was not found), 1 when the stages did not
  complete despite a clean runner exit, 0 when all three stages ended
  with `[STEP_COMPLETE]`.
- `FLOWER_LLMKIT` overrides the llmkit binary path (default `llmkit`
  from PATH). The runner's jsonl output goes to stdout unchanged.

## Escape hatch

```
./flower edit projects/P1/context.md
./flower edit llms.md
```

Opens given path relative to $FLOWER_BASE in $EDITOR, then validates file
against format rules (uniqueness, allowed prefixes/keys) before saving.

## Deletion

```
./flower project P1 remove
./flower project P1 task T1 remove
```

Destructive: ask confirmation, delete whole directory tree.
