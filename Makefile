CC := arm-linux-gnueabihf-gcc
STRIP := arm-linux-gnueabihf-strip

CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=

SRC := files/waybeam-pwm.c
BIN := waybeam-pwm

.PHONY: all clean strip help

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

strip: $(BIN)
	$(STRIP) $(BIN)

clean:
	$(RM) $(BIN)

help:
	@echo "Targets:"
	@echo "  make            Build $(BIN) with $(CC)"
	@echo "  make strip      Strip $(BIN) with $(STRIP)"
	@echo "  make clean      Remove build output"
	@echo ""
	@echo "Examples:"
	@echo "  make"
	@echo "  make clean"
	@echo "  make CC=gcc"
