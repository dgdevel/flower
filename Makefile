CC       := cc
CFLAGS   := -std=c11 -Wall -Wextra -O2
CFLAGS   += $(shell pkg-config --cflags libcjson)
LDFLAGS  :=
LDLIBS   := $(shell pkg-config --libs libcjson)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

BIN := flower

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# file io round trips, no network, no llm. Isolates $XDG_CONFIG_DIR and
# cwd in a temp dir, safe to run at any time.
check: test/check.c $(filter-out src/main.o,$(OBJ))
	$(CC) $(CFLAGS) -o test/check test/check.c \
	$(filter-out src/main.o,$(OBJ)) $(LDLIBS)
	./test/check

clean:
	rm -f $(OBJ) $(BIN) test/check
