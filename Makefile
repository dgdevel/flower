CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -pthread -Isrc
CFLAGS  += $(EXTRA_CFLAGS)
# no external deps: everything the binary needs is in src/, so LDLIBS is
# just -pthread
LDLIBS   = -pthread

# release builds stamp the version into the binary (tools/release.sh)
ifneq ($(VERSION),)
CFLAGS  += -DFLOWER_VERSION=\"$(VERSION)\"
endif

SRC  = src/buf.c src/strutil.c src/md.c src/store.c src/cli.c src/crash_report.c src/update.c src/flower.c
MAIN = src/main.c
TEST = test/selfcheck.c

all: flower

# fuzzing: libFuzzer harnesses over the project sources (fuzz/README.md).
# clang only, built on explicit request. FUZZFLAGS defaults to asan;
FUZZFLAGS ?= -fsanitize=address
#   make fuzz FUZZFLAGS=-fsanitize=address,undefined
# run one: ./fuzz/fuzz_buf fuzz/seeds/buf
FUZZ_HARNESS = buf cli

fuzz: $(foreach h,$(FUZZ_HARNESS),fuzz/fuzz_$(h))

fuzz/fuzz_%: fuzz/fuzz_%.c $(SRC) src/flower.h src/crash_report.h | check-ascii
	@mkdir -p $(@D)
	clang $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer-no-link \
	      $(FUZZFLAGS) -o $@ $< $(SRC) $(LDLIBS) -fsanitize=fuzzer $(FUZZFLAGS)

# runs before every compilation (order-only: gates the build without
# forcing a relink when nothing changed)
check-ascii:
	@tools/check-ascii.sh

flower: $(MAIN) $(SRC) src/flower.h src/crash_report.h | check-ascii
	$(CC) $(CFLAGS) -o $@ $(MAIN) $(SRC) $(LDLIBS)

test/selfcheck: $(TEST) $(SRC) src/flower.h src/crash_report.h | check-ascii
	@mkdir -p test
	$(CC) $(CFLAGS) -o $@ $(TEST) $(SRC) $(LDLIBS)

check: test/selfcheck
	./test/selfcheck

# valgrind pass over the self-check suite (needs valgrind installed)
# usage: make check-vg   [VG=...] swaps the tool, [VGFLAGS=...] extra flags
VG      ?= valgrind
VGFLAGS ?= --leak-check=full --show-leak-kinds=definite \
           --errors-for-leak-kinds=definite --track-origins=yes \
           --child-silent-after-fork=yes --fair-sched=yes \
           --error-exitcode=99 --log-file=test/valgrind.log

# -O1 instead of -O2: faster under valgrind, still debuggable
test/selfcheck-vg: $(TEST) $(SRC) src/flower.h src/crash_report.h | check-ascii
	@mkdir -p test
	$(CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -o $@ $(TEST) $(SRC) $(LDLIBS)

check-vg: test/selfcheck-vg
	$(VG) $(VGFLAGS) ./test/selfcheck-vg
	@echo "valgrind clean (log: test/valgrind.log)"

# sanitizer build of the same binary (plan.md): the default `make` target
# stays untouched; the sanitized objects are compiled fresh into
# build/asan so they never collide with the plain ones.
# usage: make asan && FLOWER_CRASH_BACKTRACE=1 ./build/asan/flower-asan version
ASANFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g

build/asan/flower-asan: $(MAIN) $(SRC) src/flower.h src/crash_report.h | check-ascii
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(ASANFLAGS) -o $@ $(MAIN) $(SRC) $(LDLIBS)

asan: build/asan/flower-asan

clean:
	rm -rf dist
	rm -rf build
	rm -f flower flower.exe test/selfcheck \
	      test/selfcheck-vg test/valgrind.log \
	      fuzz/fuzz_buf fuzz/fuzz_cli

# tag, build and publish binaries to GitHub Releases (needs gh)
# usage: make release TAG=v1.2.3
release:
	@tools/release.sh "$(TAG)"

# build distro packages (.deb/.rpm/.pkg.tar.zst/.zip) in docker; needs
# docker, artifacts land in dist/. usage: make packages TAG=v1.2.3 [TGT="deb arch"]
packages:
	@tools/package.sh "$(TAG)" $(TGT)

.PHONY: all check check-ascii check-vg clean release packages fuzz asan
