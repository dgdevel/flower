# flower Agents Guide

This document gives context and instructions for agents that work on the `flower` project.

## Project Overview
`flower` is a markdown configuration manager: a single C binary that edits
the markdown files under `$FLOWER_BASE` described in `docs/config_layout.md`.
It grew out of the hello-world-c template; the update/packaging/fuzz
machinery comes from it. Every feature stays small and
dependency-free.

## Technical Stack
- Language: C (C11)
- Dependencies: none (pthread only)
- Build system: GNU Make
- Platforms: linux (native plus docker-built deb/rpm/arch packages), windows and macos via the cross-builds in `tools/`.

## Project Structure
- `src/`: the source code.
    - `main.c`: entry point and subcommand dispatch (setup, project, edit, update, help, version).
    - `flower.h`: the public header - version macro and the APIs of the modules below.
    - `buf.c`: the `buf_t` dynamic buffer (`buf_init`/`buf_free`/`buf_append`/`buf_appendf`/`buf_reset`).
    - `strutil.c`: byte-oriented string helpers (`str_trim`, `str_starts`).
    - `md.c`: markdown chapter parser/serializer (`md_parse`/`md_find`/`md_kv_set`/`md_serialize`/`md_load`/`md_save`).
    - `store.c`: `$FLOWER_BASE` resolution, path builders, id allocators (`P1`/`T1`/`A1`), schema tables (allowed keys and context prefixes).
    - `cli.c`: the long-option parser (`--name`, `--name value`, `--name=value`, lone `--`).
    - `crash_report.c`/`.h`: fatal-signal handler (SIGSEGV/SIGBUS/SIGFPE/SIGABRT), optional backtrace.
    - `update.c`: `flower update` - latest-release check via the curl binary; download step is a documented stub.
    - `flower.c`: the command layer (`flower_main`) - setup, project/task configuration editing per docs/config_cli.md.
- `test/selfcheck.c`: the self-check unit suite (`make check`).
- `test/smoke-part1.sh`, `test/smoke-part2.sh`: end-to-end runs of the docs/config_cli.md examples in a throwaway `$FLOWER_BASE`.
- `fuzz/`: libFuzzer harnesses and seed corpora (see `fuzz/README.md`).
- `tools/`: build, release, and packaging utilities (`release.sh`, `package.sh`, `check-ascii.sh`, `pkg/`).
- `docs/`: release and crash-report documentation.
- `examples/`: sample usage and scripts.

## Build and Test
### Building
Run `make` to build the `flower` binary.
```bash
make
```

### Testing
Run the self-check test suite.
```bash
make check
```

Optional passes: `make check-vg` (valgrind), `make asan` (sanitizer build), `make fuzz` (libFuzzer, needs clang).

## Development Guidelines
- ASCII-only sources, enforced by the `check-ascii` gate that runs before every compilation.
- Buffer work goes through `src/buf.c` - no ad-hoc realloc loops elsewhere.
- New command: a handler in `src/flower.c`, one dispatch line in `src/main.c`, one usage entry. Reusable logic goes in a module under `src/` with its API in `flower.h`.
- Schema additions (new keys, detail fields, context prefixes) go in the `store.c` tables - nowhere else.
- Markdown format changes (chapter/kv parsing, serialization) go in `md.c` only.
- Code style: follow the existing C11 patterns, lowercase comments.
- The `update` download step is a stub on purpose; keep it honest in docs until wired.
