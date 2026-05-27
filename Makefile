CC := gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2

MAIN_BIN := main
LAUNCH_BIN := vm/launch

.PHONY: all clean

all: $(MAIN_BIN) $(LAUNCH_BIN)

$(MAIN_BIN): main.c
	$(CC) $(CFLAGS) $< -o $@

$(LAUNCH_BIN): vm/launch.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(MAIN_BIN) $(LAUNCH_BIN)