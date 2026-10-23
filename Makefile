# flower - llm-assisted project tracker.
# targets:
#   all        release binary (./flower)
#   check      build + run the file io test suite (test/check.c)
#   asan       asan+ubsan build of the whole binary (build/asan/flower-asan)
#   fuzz       libFuzzer harnesses (clang only, see fuzz/)
#   check-vg   run the test suite under valgrind
#   clean      remove every build artifact
#   release    tag, build and publish binaries to GitHub Releases (gh)
#   packages   build .deb/.rpm/.pkg.tar.zst/.zip in docker (artifacts in dist/)
CC       := cc
CFLAGS   := -std=c11 -Wall -Wextra -O2
# json parser: system libcjson by default; distro packaging links a pinned
# static build instead (make CJSON=/tmp/cj/libcjson.a EXTRA_CFLAGS=-I/tmp/cj/include).
# when CJSON is overridden its pkg-config cflags must go too: EXTRA_CFLAGS
# carries the pinned include path then.
ifeq ($(origin CJSON)$(origin CJSON_CFLAGS),undefinedundefined)
  CFLAGS   += $(shell pkg-config --cflags libcjson)
endif
CJSON ?= $(shell pkg-config --libs libcjson)
CFLAGS   += $(EXTRA_CFLAGS)
LDFLAGS  :=
# libcurl hardcoded, not pkg-config: the distro build containers (see
# tools/pkg/*.sh) install the -dev packages without pkg-config.
LDLIBS   := $(CJSON) -lcurl

# release builds stamp the version into the binary (tools/release.sh)
ifneq ($(VERSION),)
CFLAGS += -DFLOWER_VERSION=\"$(VERSION)\"
endif

# prompts.gen.c is generated (rule below); pick it up whether or not it
# exists yet at parse time.
SRC := $(filter-out src/prompts.gen.c,$(wildcard src/*.c)) src/prompts.gen.c
OBJ := $(SRC:.c=.o)


BIN := flower

# sanitizer build lives in build/asan so the objects cannot collide with
# the plain ones in src/.
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

# fuzzing: libFuzzer harnesses over the project sources, clang only,
# built on explicit request (make fuzz). FUZZFLAGS defaults to asan.
FUZZ_HARNESS := path json
FUZZFLAGS    ?= -fsanitize=address

.PHONY: all clean check asan fuzz check-vg release packages

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

# prompt texts bundled into the binary: tools/gen-prompts.sh preprocesses
# src/prompts/**/*.txt into src/prompts.gen.{c,h} (a changed txt or script
# regenerates them before the next compilation). both targets share the
# one recipe run: gen-prompts.sh writes the pair together.
PROMPTS_TXT = $(shell find src/prompts -type f -name '*.txt' 2>/dev/null)

src/prompts.gen.c src/prompts.gen.h: $(PROMPTS_TXT)	tools/gen-prompts.sh
	tools/gen-prompts.sh

# objects need the generated prompts header: force the gen rule first
$(OBJ): src/prompts.gen.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# file io round trips, no network, no llm. Isolates $XDG_CONFIG_DIR and
# cwd in a temp dir, safe to run at any time.
check: test/check.c $(filter-out src/main.o,$(OBJ))
	$(CC) $(CFLAGS) -o test/check test/check.c \
	$(filter-out src/main.o,$(OBJ)) $(LDLIBS)
	./test/check

# ---- asan: sanitizer build of the whole binary, fresh objects in
# build/asan. usage: make asan && ./build/asan/flower-asan ...
build/asan/flower-asan: $(SRC)
	@mkdir -p build/asan
	$(CC) $(CFLAGS) $(ASANFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

asan: build/asan/flower-asan

# ---- fuzz: libFuzzer harnesses (clang, the default cc may not support
# -fsanitize=fuzzer). FUZZFLAGS=... swaps the sanitizer.
fuzz/fuzz_%: fuzz/fuzz_%.c $(filter-out src/main.c,$(SRC)) src/flower.h
	@mkdir -p $(@D)
	clang $(CFLAGS) -O1 -g -fno-omit-frame-pointer \
       -fsanitize=fuzzer-no-link $(FUZZFLAGS) -o $@ $< \
       $(filter-out src/main.c,$(SRC)) $(LDLIBS) \
       -fsanitize=fuzzer $(FUZZFLAGS)

fuzz: $(foreach h,$(FUZZ_HARNESS),fuzz/fuzz_$(h))

# ---- valgrind: test suite under valgrind (needs valgrind installed).
# usage: make check-vg   [VG=...] swaps the tool, [VGFLAGS=...] extra flags
VG      ?= valgrind
VGFLAGS ?= --leak-check=full --show-leak-kinds=definite \
           --errors-for-leak-kinds=definite --track-origins=yes \
           --error-exitcode=99 --log-file=test/valgrind.log

# -O1 instead of -O2: faster under valgrind, still debuggable
test/check-vg: test/check.c $(filter-out src/main.o,$(OBJ))
	$(CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -o $@ test/check.c \
	$(filter-out src/main.o,$(OBJ)) $(LDLIBS)

check-vg:
	$(MAKE) test/check-vg
	$(VG) $(VGFLAGS) ./test/check-vg
	@echo "valgrind clean (log: test/valgrind.log)"

clean:
	rm -f $(OBJ) $(BIN) test/check test/check-vg test/valgrind.log
	rm -rf build fuzz/fuzz_path fuzz/fuzz_json dist
	rm -f src/prompts.gen.c src/prompts.gen.h

# tag, build and publish binaries to GitHub Releases (needs gh)
# usage: make release TAG=v0.1.0
release:
	@tools/release.sh "$(TAG)"

# build distro packages (.deb/.rpm/.pkg.tar.zst/.zip) in docker; needs
# docker, artifacts land in dist/. usage: make packages TAG=v0.1.0 [TGT="deb rpm"]
packages:
	@tools/package.sh "$(TAG)" $(TGT)
