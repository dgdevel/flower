# flower CLI — configuration editing syntax

Full command line surface for editing every configuration file described in
docs/config_layout.md. Files on disk follow docs/config_markdown_format.md.

## Conventions

- Binary invoked as `./flower` from repo root.
- Identifiers are sequential and auto-assigned at creation: projects `P1, P2...`,
  tasks `T1, T2...`, actions `A1, A2...`. User never picks the numeric id.
- `<value>` arguments are quoted strings; spaces allowed inside quotes.
- Key names unique per chapter, chapter names unique per file. Duplicate name or
  key → command fails, file unchanged, error message says what clashed.
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

Areas: `(none)` = config.md, `details`, `context`, `resource`, `action`.

## User config — $FLOWER_BASE/flower.md

Chapter `# User`, keys `Name`, `Role`.

```
./flower setup
```

**[specified]** Interactive create/update of the whole file.

## Project config — projects/P1/config.md

Chapter `# Project`, keys `Name`, `Path`, `Description`.

```
./flower project P1 set Name "flower"        # [specified] creates P1 if missing
./flower project P1 set Path "/home/user/projects/flower"
./flower project P1 set Description "A project description"
./flower project P1 show
```

`set` on nonexistent project creates it. `set` on unknown key fails.

## Project details — projects/P1/details.md

Chapters = fields, value = bare chapter text. Fields: `Output`, `Objectives`,
`Scope`, `Constraints`, `Stakeholders`. All optional.

```
./flower project P1 details set Output "The project output are csv files."
./flower project P1 details set Stakeholders "Team A, Team B"
./flower project P1 details remove Output
./flower project P1 details show [Field]
```

`set` on existing field overwrites whole chapter text. `remove` deletes chapter.

## Project context — projects/P1/context.md

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

## Project resources — projects/P1/resources.md

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

## Task config — projects/P1/tasks/T1/config.md

Chapter `# Task`, keys `Name`, `Description`. Same shape as project config.

```
./flower project P1 task T1 set Name "mix ingredients"      # creates T1 if missing
./flower project P1 task T1 set Description "Combine water and flour"
./flower project P1 task T1 show
```

## Task context — projects/P1/tasks/T1/context.md

Identical format and verbs to project context:

```
./flower project P1 task T1 context add Fact "..." "..."
./flower project P1 task T1 context edit|rename|remove <Prefix> "<Title>" [...]
./flower project P1 task T1 context list
```

## Actions — projects/P1/tasks/T1/actions.md

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

## Escape hatch

```
./flower edit projects/P1/context.md
```

Opens given path relative to $FLOWER_BASE in $EDITOR, then validates file
against format rules (uniqueness, allowed prefixes/keys) before saving.

## Deletion

```
./flower project P1 remove
./flower project P1 task T1 remove
```

Destructive: ask confirmation, delete whole directory tree.
