# flower — single-binary webapp in C
# Requires GNU make >= 4.3 (grouped targets), a C11 compiler, and cJSON
# (system package "libcjson"; build-time discovery via pkg-config).

CC     ?= cc
CFLAGS ?= -O2
WARN    = -std=c11 -Wall -Wextra -Wpedantic

CJSON_CFLAGS := $(shell pkg-config --cflags libcjson 2>/dev/null)
CJSON_LIBS   := $(shell pkg-config --libs libcjson 2>/dev/null || echo -lcjson)

BIN     := flower
EMBED   := tools/embed
GEN_C   := src/assets_gen.c
GEN_H   := src/assets_gen.h

ASSET_SRCS := $(shell find web -type f ! -name '.*' 2>/dev/null | LC_ALL=C sort)
SRCS := src/main.c src/server.c src/http.c src/theme.c src/projects.c src/agents.c src/tasks.c src/context.c $(GEN_C)
OBJS := $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(WARN) $(CFLAGS) -o $@ $(OBJS) $(CJSON_LIBS)

%.o: %.c
	$(CC) $(WARN) $(CFLAGS) -Isrc $(CJSON_CFLAGS) -MMD -MP -c -o $@ $<

# the generated asset table is machine-written data (long string literals);
# skip -Wpedantic's overlength-strings noise for it
src/assets_gen.o: src/assets_gen.c $(GEN_H)
	$(CC) -std=c11 -Wall -Wextra $(CFLAGS) -Isrc -MMD -MP -c -o $@ $<

$(OBJS): $(GEN_H)

# regenerate the embedded asset table whenever the web/ sources change
$(GEN_C) $(GEN_H) &: $(EMBED) $(ASSET_SRCS)
	./$(EMBED) $(GEN_C) $(GEN_H) web

$(EMBED): tools/embed.c
	$(CC) $(WARN) $(CFLAGS) -o $@ $<

run: $(BIN)
	./$(BIN)

check: $(BIN)
	tests/smoke.sh ./$(BIN)

clean:
	rm -f $(BIN) $(EMBED) $(GEN_C) $(GEN_H) $(OBJS) $(OBJS:.o=.d)

.PHONY: all run check clean

-include $(OBJS:.o=.d)
