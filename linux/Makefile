# Freya - the shell language as a Linux program.
#
#   make -C linux           build build/linux/fsh
#   make -C linux test      build it and run linux/test.sh
#   make -C linux clean
#
# src/shell.c is the interpreter the boards run; linux/board.h leaves out
# every command and function that needs their hardware, and
# linux/platform.c is the console, the clock, the files and run().
# bench/cpubench.c is the cpubench() command, Whetstone's functions
# from libm.

ROOT    := ..
BUILD   := $(ROOT)/build/linux
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -MMD -MP \
           -I$(ROOT)/linux -I$(ROOT)/include -I$(ROOT)/src -I$(ROOT)/bench
# GCC's -O2 sees a truncation in the history copy that cannot happen.
CFLAGS  += $(shell $(CC) -Werror -Wno-stringop-truncation -E -x c /dev/null \
             >/dev/null 2>&1 && echo -Wno-stringop-truncation)

SRCS    := $(ROOT)/linux/platform.c $(ROOT)/src/shell.c $(ROOT)/src/print.c \
           $(ROOT)/src/cksum.c $(ROOT)/bench/cpubench.c
OBJS    := $(patsubst $(ROOT)/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all test clean

all: $(BUILD)/fsh

$(BUILD)/fsh: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(BUILD)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BUILD)/fsh
	sh $(ROOT)/linux/test.sh $(BUILD)/fsh

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
