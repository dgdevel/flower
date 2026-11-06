# flower

`flower` is a markdown configuration manager: projects, tasks, context
notes and actions live as plain markdown files under a single config
directory (`$FLOWER_BASE`), so everything stays readable and greppable.

Single binary, C11, zero dependencies (only pthread, part of glibc). The
config layout and the command surface follow the specs in
[docs/](docs/).

## Install

Grab a tarball from GitHub Releases (`flower-v1.2.3-<os>-<arch>.tar.gz`,
binary plus `README.md` and `LICENSE.md`) or a distro package:

- `deb` - ubuntu 22.04+, debian 12+ (`apt install ./flower_..._.deb`)
- `rpm` - fedora 41+ and rhel/alma/rocky 9+ (`dnf install ./flower-..._.rpm`)
- `arch` - `pacman -U flower-...pkg.tar.zst`
- `win` / `mac` - zips of the cross-compiled binary

## Build

Requirements: a C11 compiler and `pthreads` - nothing else. All code lives
in `src/`, no external libraries to hunt down.

```sh
make              # build the flower binary
make check        # run the self-check test suite
make check-vg     # run the checks through valgrind (see test/valgrind.log)
make asan         # sanitizer build for deeper debugging
make fuzz         # set up libFuzzer harnesses (see fuzz/README.md)
```

## Commands

```
./flower setup
./flower project [<P>] <area> <verb> [args]
./flower project [<P>] task <T> <area> <verb> [args]
```

- `setup` - create/update `$FLOWER_BASE/flower.md`, then guided add/edit of llm
  profiles (`llms.md`)
- `project [<P>] set|show|remove` - project config (`Name`, `Path`,
  `Description`)
- omitted `<P>` is auto-detected from the working directory via each project's
  `Path`
- `project <P> details set|remove|show` - detail chapters (`Output`,
  `Objectives`, `Scope`, ...)
- `project <P> context add|edit|rename|remove|list` - context entries
  (`Fact`, `Rule`, ... prefixes)
- `project <P> resource add|set|remove|list` - resource pointers
- `project <P> task <T> set|show|remove` - task config (`Name`,
  `Description`)
- `project <P> task <T> context <verb>` - task context, same verbs as
  project context
- `project <P> task <T> action add|set|remove|list` - actions, `Status`,
  `Depends`
- `edit <path>` - escape hatch: `$EDITOR` on a file under `$FLOWER_BASE`,
  validated before saving

Representative examples:

```sh
./flower setup
./flower project P1 set Name "flower"
./flower project P1 details set Output "The project output are csv files."
./flower project P1 context add Fact "configuration location" "Configuration always happens in directory X"
./flower project P1 resource add "staging webapp url" "https://www.example.org/"
./flower project P1 task T1 set Name "mix ingredients"
./flower project P1 task T1 action add "take water" --description "Water can be taken using a glass."
./flower project P1 task T1 action A3 set Depends "A1, A2"
./flower edit projects/P1/context.md
```

Every `set`/`add`/`remove` prints the resulting file path. Duplicate
chapter or key names and unknown keys fail, file unchanged, with the clash
named in the error.

## Config layout

```
$FLOWER_BASE/
  flower.md                     # User: Name, Role
  projects/
    P1/
      config.md                 # Project: Name, Path, Description
      details.md                # Output, Objectives, Scope, ...
      context.md                # "<Prefix>: <Title>" chapters
      resources.md              # free-form names, pointer values
      tasks/
        T1/
          config.md             # Task: Name, Description
          context.md
          actions.md            # "A1: <title>", Status, Depends
```

Full specs: [docs/config_layout.md](docs/config_layout.md),
[docs/config_cli.md](docs/config_cli.md),
[docs/config_markdown_format.md](docs/config_markdown_format.md).

## Develop

- `src/` holds all sources, `flower.h` is the single header.
- `test/selfcheck.c` is the unit suite (`make check`); `test/smoke-part1.sh`,
  `test/smoke-part2.sh` and `test/smoke-part3.sh` run the docs/config_cli.md
  examples end to end against a throwaway `$FLOWER_BASE`.
- `fuzz/` has the libFuzzer harnesses, `tools/` the packaging scripts.
- Sources are ASCII-only, enforced by the `check-ascii` gate that runs
  before every compilation.

Module map and working rules: [AGENTS.md](AGENTS.md).

## Release

One command tags, builds, packages and publishes (needs `gh`, docker
optional):

```sh
make release TAG=v1.2.3          # or: tools/release.sh v1.2.3
make packages TAG=v1.2.3         # distro packages only
```

Details: [docs/release.md](docs/release.md).

## License

This project is proudly distributed under the European Union Public
Licence v1.2 (EUPL-1.2).
