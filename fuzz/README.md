# Fuzzing

libFuzzer harnesses over the project's offline parsing surfaces: the
buffer growth layer and the long-option parser. Each
harness links every `src/*.c` translation unit (same set as the
selfcheck, minus `main.c`) so coverage instrumentation sees the real
code paths.

## Prerequisites

clang with libFuzzer (`-fsanitize=fuzzer`). No other install: the
project has zero external libraries, so clang is the only dependency.
libFuzzer ships with clang (`apt install clang` / `pacman -S clang` /
Xcode clang). Vendoring is neither needed nor useful - libFuzzer is a
compiler-rt runtime, not a library you drop into a repo. (AFL++ would
be the tool to vendor/download separately, `afl-clang-fast` replacing
`cc`.)

## Build

```sh
make fuzz                        # both, asan + fuzzer
make fuzz/fuzz_cli               # one
make fuzz FUZZFLAGS=-fsanitize=address,undefined   # ubsan too
```

Every build runs the ascii gate first (order-only); the harnesses are
built into `fuzz/` as `fuzz_buf`, `fuzz_cli`.

## Run

```sh
./fuzz/fuzz_buf    fuzz/seeds/buf                  # one-off short run
./fuzz/fuzz_cli  -max_total_time=600 fuzz/out/cli fuzz/seeds/cli
```

Write corpus growth to `fuzz/out/<name>/` (gitignored) and keep
`fuzz/seeds/<name>/` as the curated, checked-in seed set.

## Harnesses

| target | surface | notes |
|---|---|---|
| `fuzz_buf` | `buf_append`/`buf_appendf`/`buf_reset` (src/buf.c) | fed in two data-split chunks; exercises realloc growth across arbitrary chunk boundaries |
| `fuzz_cli` | `cli_parse` (src/cli.c) | input split at NULs into up to 64 argv words; checks the return stays in {0, -1, -2}; covers `--name`, `--name=v`, lone `--`, unknown options, missing args |

Both are pure: no network, no spawning, no filesystem writes.

## Findings

Findings land here once the first soak runs.

## Adding a harness

1. `fuzz/fuzz_<name>.c` with `int LLVMFuzzerTestOneInput(const uint8_t *, size_t)`.
2. add the name to `FUZZ_HARNESS` in the Makefile.
3. `fuzz/seeds/<name>/` with 1-3 minimal inputs.
4. keep the harness offline: no spawning, no network, no fs writes
   outside /tmp.
