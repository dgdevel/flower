# flower — single-binary webapp in C
# Requires GNU make >= 4.3 (grouped targets), a C11 compiler and cJSON
# (system package "libcjson"; build-time discovery via pkg-config).
# The web tools are llmkit's own (>= 1.2.0, `llmkit builtin-mcp`).

CC     ?= cc
CFLAGS ?= -O2
WARN    = -std=c11 -Wall -Wextra -Wpedantic

CJSON_CFLAGS := $(shell pkg-config --cflags libcjson 2>/dev/null)
CJSON_LIBS   := $(shell pkg-config --libs libcjson 2>/dev/null || echo -lcjson)

BIN     := flower
EMBED   := tools/embed
GEN_C   := src/assets_gen.c
GEN_H   := src/assets_gen.h
GEN_PC  := src/prompts_gen.c
GEN_PH  := src/prompts_gen.h
SELFTEST := tests/selftest

ASSET_SRCS   := $(shell find web -type f ! -name '.*' 2>/dev/null | LC_ALL=C sort)
PROMPT_SRCS  := $(shell find prompts -type f ! -name '.*' 2>/dev/null | LC_ALL=C sort)
SRCS := src/main.c src/server.c src/http.c src/theme.c src/projects.c src/agents.c \
        src/tasks.c src/context.c src/prompts.c src/fs.c \
        src/conv.c src/mcp.c src/researchers.c src/scan.c src/plan.c \
        src/util.c $(GEN_C) $(GEN_PC)
OBJS := $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(WARN) $(CFLAGS) -o $@ $(OBJS) $(CJSON_LIBS)

%.o: %.c
	$(CC) $(WARN) $(CFLAGS) -Isrc $(CJSON_CFLAGS) -MMD -MP -c -o $@ $<

# the generated tables are machine-written data (long string literals);
# skip -Wpedantic's overlength-strings noise for them
src/assets_gen.o: src/assets_gen.c $(GEN_H)
	$(CC) -std=c11 -Wall -Wextra $(CFLAGS) -Isrc -MMD -MP -c -o $@ $<
src/prompts_gen.o: src/prompts_gen.c $(GEN_PH)
	$(CC) -std=c11 -Wall -Wextra $(CFLAGS) -Isrc -MMD -MP -c -o $@ $<

$(OBJS): $(GEN_H) $(GEN_PH)

# regenerate the embedded asset/prompt tables whenever the sources change
$(GEN_C) $(GEN_H) &: $(EMBED) $(ASSET_SRCS)
	./$(EMBED) $(GEN_C) $(GEN_H) web asset
$(GEN_PC) $(GEN_PH) &: $(EMBED) $(PROMPT_SRCS)
	./$(EMBED) $(GEN_PC) $(GEN_PH) prompts prompt

$(EMBED): tools/embed.c
	$(CC) $(WARN) $(CFLAGS) -o $@ $<

run: $(BIN)
	./$(BIN)

check: $(BIN) $(SELFTEST)
	$(SELFTEST)
	tests/smoke.sh ./$(BIN)

# offline self-test: prompt templating and the fs tools, against
# tests/fixtures/
$(SELFTEST): tests/selftest.c src/fs.o src/prompts.o \
             src/context.o src/util.o src/prompts_gen.o
	$(CC) $(WARN) $(CFLAGS) -Isrc $(CJSON_CFLAGS) -o $@ $< \
	      src/fs.o src/prompts.o src/context.o \
	      src/util.o src/prompts_gen.o $(CJSON_LIBS)

clean:
	rm -f $(BIN) $(EMBED) $(GEN_C) $(GEN_H) $(GEN_PC) $(GEN_PH) \
	      $(SELFTEST) $(OBJS) $(OBJS:.o=.d)

.PHONY: all run check clean
