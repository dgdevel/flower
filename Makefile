# flower — single-binary webapp in C
# Requires GNU make >= 4.3 (grouped targets) and a C11 compiler.

CC     ?= cc
CFLAGS ?= -O2
WARN    = -std=c11 -Wall -Wextra -Wpedantic

BIN     := flower
EMBED   := tools/embed
GEN_C   := src/assets_gen.c
GEN_H   := src/assets_gen.h

ASSET_SRCS := $(shell find web -type f ! -name '.*' 2>/dev/null | LC_ALL=C sort)
SRCS := src/main.c src/server.c src/http.c $(GEN_C)
OBJS := $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(WARN) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(WARN) $(CFLAGS) -Isrc -MMD -MP -c -o $@ $<

$(OBJS): $(GEN_H)

# regenerate the embedded asset table whenever the web/ sources change
$(GEN_C) $(GEN_H) &: $(EMBED) $(ASSET_SRCS)
	./$(EMBED) $(GEN_C) $(GEN_H) web

$(EMBED): tools/embed.c
	$(CC) $(WARN) $(CFLAGS) -o $@ $<

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) $(EMBED) $(GEN_C) $(GEN_H) $(OBJS) $(OBJS:.o=.d)

.PHONY: all run clean

-include $(OBJS:.o=.d)
