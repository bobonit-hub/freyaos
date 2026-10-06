# BASIC for the host, where it is tested, and for the virtual machine
#
#   make                 build basic-host, the interpreter compiled
#                        natively with the float arithmetic the board
#                        build uses
#   make vm              build basic11.vm, the same interpreter compiled
#                        for the Freya VM with cproc and QBE, and
#                        runbasic, which runs it on the PC
#   make arm             build basic-arm: basic.c compiled for the
#                        board, as samples/basic11 is, to run on the PC
#                        under qemu-arm
#   make test            run the BASIC programs under tests/ on
#                        basic-host and, when they have been built, on
#                        the image and on basic-arm too
#   make clean
#
# The program for the board, samples/basic11, is built by the top-level
# Makefile with the other samples; it is the same basic.c.  So is
# samples/basic11vm, which runs basic11.vm on a board.  The image needs
# the cproc and QBE builds described in ../qbe/README.md; point CPROC
# and QBE at them when they are not on the PATH:
#
#   make vm CPROC=/path/to/cproc-qbe QBE=/path/to/qbe
#
# Everything here is written to ../build/basic.

CC      ?= cc
ARMCC   ?= arm-none-eabi-gcc
CPP     ?= cpp
CPROC   ?= cproc-qbe
QBE     ?= qbe
PYTHON  ?= python3

TOP     := ..
OUT     := $(TOP)/build/basic

SRCS    := basic.c bas.h fpnat.c fpnat.h

all: $(OUT)/basic-host

# -fno-math-errno lets the builtin square root be the instruction, as
# VSQRT is on the board.  -ffp-contract=off keeps a multiply and an add
# two roundings, as on the board, on a PC whose compiler would fuse them.
$(OUT)/basic-host: $(SRCS) hostrt.c pretend.c | $(OUT)
	$(CC) -std=gnu99 -g -O2 -Wall -Wextra -Wno-unused-parameter -fno-math-errno \
	    -ffp-contract=off \
	    -DBAS_HOST basic.c hostrt.c -lm -o $@

vm: $(OUT)/basic11.vm $(OUT)/runbasic

# cproc-qbe has no include path option, so the preprocessor runs first.
$(OUT)/basic.i: $(SRCS) fpsoft.c | $(OUT)
	$(CPP) -nostdinc -undef -P -DBAS_VM -DBAS_SOFTFLOAT basic.c > $@

$(OUT)/basic.ssa: $(OUT)/basic.i
	$(CPROC) -t freya $< > $@

$(OUT)/basic.s: $(OUT)/basic.ssa
	$(QBE) -t freya $< > $@

# vmrt.s goes first, so that _start is address 0.
$(OUT)/basic11.vm: vmrt.s $(OUT)/basic.s $(TOP)/qbe/as.py
	cat vmrt.s $(OUT)/basic.s > $(OUT)/basic11.all.s
	$(PYTHON) $(TOP)/qbe/as.py -o $@ --map $(OUT)/basic11.map $(OUT)/basic11.all.s
	@ls -l $@

$(OUT)/runbasic: runbasic.c hostrt.c pretend.c bas.h vmsys.h $(TOP)/src/vm.c | $(OUT)
	$(CC) -std=gnu11 -g -O2 -Wall -Wextra -Wno-unused-parameter \
	    -I$(TOP)/include -I$(TOP)/src -I$(TOP)/boards/blackpill \
	    -DFREYA_BOARD_BLACKPILL -DFREYA_HOST -DBAS_HOST -DBAS_VMHOST \
	    runbasic.c hostrt.c $(TOP)/src/vm.c -o $@

arm: $(OUT)/basic-arm

# The flags of a Cortex-M4F program in the top-level Makefile, and
# samples/basic11's own.
$(OUT)/basic-arm: armrt.c armrt.h pretend.c $(SRCS) | $(OUT)
	$(ARMCC) -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
	    -std=gnu11 -Os -g -ffreestanding -fno-common -fno-builtin \
	    -ffunction-sections -fdata-sections -ffp-contract=off \
	    -Wall -Wextra -Wno-unused-parameter \
	    -nostdlib -static -Wl,--gc-sections -Wl,-e,_start armrt.c -lgcc -o $@

test: all
	@cd $(TOP) && sh tests/basic_tests.sh

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all vm arm test clean
