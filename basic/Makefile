# BASIC for the Freya virtual machine, and for the board itself
#
#   make                 build basic.bin, the VM image, with cproc and QBE
#   make host            build basic-host, the interpreter with the FP11
#                        arithmetic compiled natively, basic-float, the
#                        same interpreter with the float arithmetic the
#                        board build uses, and runbasic, which runs the
#                        image on the PC with the VM from src/vm.c
#   make test            run the BASIC programs under tests/ on both host
#                        builds and, when the image was built, on the VM
#   make clean
#
# The image needs the cproc and QBE builds described in ../qbe/README.md.
# Point CPROC and QBE at them when they are not on the PATH:
#
#   make CPROC=/tmp/cproc/cproc-qbe QBE=/tmp/qbe/qbe
#
# The program for the board, samples/basic11, is built by the top-level
# Makefile with the other samples.  Everything here is written to
# ../build/basic.

CPROC   ?= cproc-qbe
QBE     ?= qbe
CPP     ?= cpp
CC      ?= cc
PYTHON  ?= python3

TOP     := ..
OUT     := $(TOP)/build/basic
AS      := $(PYTHON) $(TOP)/qbe/as.py
BOARD   ?= blackpill
BOARD_DEF := -DFREYA_BOARD_$(shell echo $(BOARD) | tr a-z A-Z)

HOSTFLAGS := -std=gnu11 -g -O2 -Wall -Wextra -Wno-unused-parameter \
             -I$(TOP)/include -I$(TOP)/src -I$(TOP)/boards/$(BOARD) \
             $(BOARD_DEF) -DFREYA_HOST

IMAGE   := $(OUT)/basic.bin
SRCS    := basic.c bas.h fp11.c fp11.h fpnat.c fpnat.h

all: $(IMAGE)

# cproc-qbe has no include path option, so the preprocessor runs first.
$(OUT)/basic.i: $(SRCS) | $(OUT)
	$(CPP) -nostdinc -undef -P -I. -DBAS_VM -DBAS_FP11 basic.c > $@

$(OUT)/basic.ssa: $(OUT)/basic.i
	$(CPROC) -t freya $< > $@

$(OUT)/basic.s: $(OUT)/basic.ssa
	$(QBE) -t freya $< > $@

$(IMAGE): rt.s $(OUT)/basic.s $(TOP)/qbe/as.py
	cat rt.s $(OUT)/basic.s > $(OUT)/all.s
	$(AS) -o $@ --map $(OUT)/basic.map $(OUT)/all.s
	@ls -l $@

host: $(OUT)/basic-host $(OUT)/basic-float $(OUT)/runbasic

$(OUT)/basic-host: $(SRCS) hostrt.c | $(OUT)
	$(CC) -std=c99 -g -O2 -Wall -Wextra -Wno-unused-parameter -DBAS_HOST -DBAS_FP11 \
	    basic.c hostrt.c -o $@

# -fno-math-errno lets the builtin square root be the instruction, as
# VSQRT is on the board; the banner is the board build's.
$(OUT)/basic-float: $(SRCS) hostrt.c | $(OUT)
	$(CC) -std=gnu99 -g -O2 -Wall -Wextra -Wno-unused-parameter -fno-math-errno \
	    -DBAS_HOST -DBAS_BANNER='"BASIC-11 for Freya"' basic.c hostrt.c -lm -o $@

$(OUT)/runbasic: runbasic.c $(TOP)/src/vm.c | $(OUT)
	$(CC) $(HOSTFLAGS) runbasic.c $(TOP)/src/vm.c -o $@

test: host
	@cd $(TOP) && sh tests/basic_tests.sh

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all host test clean
