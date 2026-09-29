# BASIC for the Freya virtual machine
#
#   make                 build basic.bin, the VM image, with cproc and QBE
#   make host            build basic-host, the same interpreter compiled
#                        natively, and runbasic, which runs the image on
#                        the PC with the VM from src/vm.c
#   make test            run the BASIC programs under tests/ natively and,
#                        when the image was built, on the VM too
#   make clean
#
# The image needs the cproc and QBE builds described in ../qbe/README.md.
# Point CPROC and QBE at them when they are not on the PATH:
#
#   make CPROC=/tmp/cproc/cproc-qbe QBE=/tmp/qbe/qbe
#
# Everything is written to ../build/basic.

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

all: $(IMAGE)

# cproc-qbe has no include path option, so the preprocessor runs first.
$(OUT)/basic.i: basic.c fp11.c fp11.h bas.h | $(OUT)
	$(CPP) -nostdinc -undef -P -I. basic.c > $@

$(OUT)/basic.ssa: $(OUT)/basic.i
	$(CPROC) -t freya $< > $@

$(OUT)/basic.s: $(OUT)/basic.ssa
	$(QBE) -t freya $< > $@

$(IMAGE): rt.s $(OUT)/basic.s $(TOP)/qbe/as.py
	cat rt.s $(OUT)/basic.s > $(OUT)/all.s
	$(AS) -o $@ --map $(OUT)/basic.map $(OUT)/all.s
	@ls -l $@

host: $(OUT)/basic-host $(OUT)/runbasic

$(OUT)/basic-host: basic.c fp11.c fp11.h bas.h hostrt.c | $(OUT)
	$(CC) -std=c99 -g -O2 -Wall -Wextra -Wno-unused-parameter -DBAS_HOST \
	    basic.c hostrt.c -o $@

$(OUT)/runbasic: runbasic.c $(TOP)/src/vm.c | $(OUT)
	$(CC) $(HOSTFLAGS) runbasic.c $(TOP)/src/vm.c -o $@

test: host
	@cd $(TOP) && sh tests/basic_tests.sh

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: all host test clean
