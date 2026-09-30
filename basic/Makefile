# BASIC for the host, where it is tested
#
#   make                 build basic-host, the interpreter compiled
#                        natively with the float arithmetic the board
#                        build uses
#   make test            run the BASIC programs under tests/
#   make clean
#
# The program for the board, samples/basic11, is built by the top-level
# Makefile with the other samples; it is the same basic.c.  Everything
# here is written to ../build/basic.

CC      ?= cc

TOP     := ..
OUT     := $(TOP)/build/basic

SRCS    := basic.c bas.h fpnat.c fpnat.h

all: $(OUT)/basic-host

# -fno-math-errno lets the builtin square root be the instruction, as
# VSQRT is on the board.
$(OUT)/basic-host: $(SRCS) hostrt.c | $(OUT)
	$(CC) -std=gnu99 -g -O2 -Wall -Wextra -Wno-unused-parameter -fno-math-errno \
	    -DBAS_HOST basic.c hostrt.c -lm -o $@

test: all
	@cd $(TOP) && sh tests/basic_tests.sh

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all test clean
