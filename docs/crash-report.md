# flower crash report

When flower dies on a fatal signal, the process prints one line to stderr before it goes down. This applies to every subcommand: `crash_report_init()` runs at the top of `main()` before any dispatch.

## What it prints

```
crash: signal 11 received
```

Signals caught: `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGABRT`. The process then `_exit(139)` - the shell reports it like a plain segfault, and `$?` is 139.

Honest scope: stderr only, no report file, no timestamps, no argv/pc record. The handler is async-signal-safe by construction - raw `write(2)` to fd 2, no allocator, no stdio.

## Backtrace

`FLOWER_CRASH_BACKTRACE=1` (checked once at init; set it to anything non-empty) makes the handler dump up to 32 frames with `backtrace_symbols_fd` after the signal line. Still stderr only. In a crashed process this is best-effort and may fail; mingw builds have no `execinfo` and skip it.

```sh
FLOWER_CRASH_BACKTRACE=1 ./flower version
```

The symbol list is most readable on the asan build:

```sh
make asan && FLOWER_CRASH_BACKTRACE=1 ./build/asan/flower-asan version
```

## Testing it

No subcommand crashes on purpose, so pick one of:

- `kill -SEGV <pid>` - run `flower version` (or any subcommand) under a debugger and send the signal from another shell. The handler catches signals raised externally too.
- add a deliberate null deref in a test branch of the code, e.g. `*(volatile int *)0 = 0;` in the command you are working on, build, run, remove it. With `SIGABRT` the same path is reachable through `abort()`.

Either way the expected output is the `crash:` line plus (with the env var) the backtrace, and exit status 139.
