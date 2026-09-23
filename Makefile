# Freya - a bare metal system for small STM32 boards
#
#   make                    build the kernel image and the example programs
#   make BOARD=bluepill     build for the STM32F103C8T6 "Blue Pill"
#   make flash              flash the image with st-flash
#   make flash PROGRAM=hello
#                           flash the kernel and one program into the module
#   make BOARD=bluepill flash PROGRAM=hello AUTOSTART=1
#                           same, with the auto-start flag already on
#   make size               show the section sizes
#   make clean
#
# Everything a board needs lives in boards/<board>: its register header,
# its startup code, its bring-up (boards/<board>/board.c), its linker
# scripts and the compiler flags in boards/<board>/board.mk.

TARGET    := freya
BOARD     ?= blackpill

SRC_DIR   := src
APP_DIR   := apps
SMPL_DIR  := samples
BOARD_DIR := boards/$(BOARD)
BUILD     := build/$(BOARD)

ifeq ($(wildcard $(BOARD_DIR)/board.mk),)
$(error unknown BOARD '$(BOARD)' - available: $(patsubst boards/%/,%,$(dir $(wildcard boards/*/board.mk))))
endif
include $(BOARD_DIR)/board.mk

# Programs are linked against the same board as the kernel, so the two
# agree on where the program region is (see include/freya_api.h).
BOARD_DEF := -DFREYA_BOARD_$(shell echo $(BOARD) | tr a-z A-Z)

CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
OBJCOPY   := $(CROSS)objcopy
OBJDUMP   := $(CROSS)objdump
NM        := $(CROSS)nm
SIZE      := $(CROSS)size

CFLAGS    := $(CPUFLAGS) $(BOARD_DEF) \
             -std=gnu11 -Os -g3 \
             -ffreestanding -fno-common -fno-builtin \
             -ffunction-sections -fdata-sections \
             -fomit-frame-pointer \
             -fno-asynchronous-unwind-tables -fno-unwind-tables \
             -fmerge-all-constants \
             -falign-functions=2 -falign-jumps=2 -falign-loops=2 \
             -Wall -Wextra -Wshadow -Wundef \
             -Wno-unused-parameter \
             -Iinclude -I$(SRC_DIR) -I$(BOARD_DIR)

ASFLAGS   := $(CPUFLAGS) -g3

LDSCRIPT  := $(BOARD_DIR)/freya.ld
LDFLAGS   := $(CPUFLAGS) -nostdlib -T $(LDSCRIPT) \
             -Wl,--gc-sections -Wl,--build-id=none \
             -Wl,-Map=$(BUILD)/$(TARGET).map

CSRC      := $(wildcard $(SRC_DIR)/*.c)
ASRC      := $(wildcard $(SRC_DIR)/*.s) $(wildcard $(SRC_DIR)/*.S)
BCSRC     := $(wildcard $(BOARD_DIR)/*.c)
BASRC     := $(wildcard $(BOARD_DIR)/*.s)
OBJS      := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(filter %.c,$(CSRC))) \
             $(patsubst $(SRC_DIR)/%.s,$(BUILD)/%.o,$(filter %.s,$(ASRC))) \
             $(patsubst $(SRC_DIR)/%.S,$(BUILD)/%.o,$(filter %.S,$(ASRC))) \
             $(patsubst $(BOARD_DIR)/%.c,$(BUILD)/board/%.o,$(BCSRC)) \
             $(patsubst $(BOARD_DIR)/%.s,$(BUILD)/board/%.o,$(BASRC))
DEPS      := $(OBJS:.o=.d)

# User programs, one directory per program under apps/
APPS      := hello spin
APP_BINS  := $(patsubst %,$(BUILD)/apps/%.bin,$(APPS))
APP_LD    := $(BOARD_DIR)/app.ld
APP_CFLAGS:= $(CPUFLAGS) $(BOARD_DEF) -std=gnu11 -Os -g3 -ffreestanding \
             -fno-common -fno-builtin -ffunction-sections -fdata-sections \
             -Wall -Wextra -Wno-unused-parameter -Iinclude

# Cortex-M3 has no FPU.  A program that uses float calls the helpers in
# src/softfp.c; --gc-sections leaves them out of a program that does not.
# The header section is KEEP'd, so it survives that collection.
ifneq ($(filter -mfloat-abi=soft,$(CPUFLAGS)),)
APP_SOFTFP := $(SRC_DIR)/softfp.c
APP_GC     := -Wl,--gc-sections
else
APP_SOFTFP :=
APP_GC     :=
endif

# Sample programs, same ABI and linker script, one directory each under samples/
SAMPLES   := blink tetris log forth irq pwm i2c w1 flashprobe threads
# A sample whose code is larger than a board's program RAM region is built
# there as a flash image only: forth is 8 KiB of interpreter, which is the
# whole of the Blue Pill's RAM window before its dictionary is counted.
XIP_ONLY_bluepill := forth
XIP_ONLY  := $(XIP_ONLY_$(BOARD))
SMPL_BINS := $(patsubst %,$(BUILD)/samples/%.bin,$(filter-out $(XIP_ONLY),$(SAMPLES)))

# A board that reserves part of its flash for a program image supplies a
# second program linker script.  Every app and sample is then built both
# ways from the same objects: a RAM image for 'load', and a '.xip.bin'
# that executes from flash for 'install'.
APP_XIP_LD := $(wildcard $(BOARD_DIR)/app_flash.ld)
ifneq ($(APP_XIP_LD),)
APP_BINS   += $(patsubst %,$(BUILD)/apps/%.xip.bin,$(APPS))
SMPL_BINS  += $(patsubst %,$(BUILD)/samples/%.xip.bin,$(SAMPLES))
else ifneq ($(XIP_ONLY),)
$(error $(XIP_ONLY): needs a flash image, but '$(BOARD)' keeps no program in flash)
endif

# One user program to store in the board's program flash region when the
# module is programmed.  An app name (hello), a sample name (blink), or the
# path of a .xip.bin linked for that region.  Unset, the image is the kernel
# alone and whatever already occupies the region is left untouched.
PROGRAM ?=
# Set the auto-start flag in the packed image.  Off unless asked, so a
# module programmed with PROGRAM= still boots to the shell on every reset.
AUTOSTART ?=

ifeq ($(AUTOSTART),1)
ifeq ($(PROGRAM),)
$(error AUTOSTART=1 needs PROGRAM= so there is a flash image to set the flag in)
endif
endif

ifneq ($(PROGRAM),)
ifeq ($(APP_XIP_LD),)
$(error PROGRAM=$(PROGRAM): '$(BOARD)' keeps no program in flash)
endif
ifneq ($(filter $(PROGRAM),$(APPS)),)
PROGRAM_BIN := $(BUILD)/apps/$(PROGRAM).xip.bin
else ifneq ($(filter $(PROGRAM),$(SAMPLES)),)
PROGRAM_BIN := $(BUILD)/samples/$(PROGRAM).xip.bin
else
PROGRAM_BIN := $(PROGRAM)
endif
PROGRAM_TAG := $(patsubst %.xip.bin,%,$(notdir $(PROGRAM_BIN)))
ifeq ($(AUTOSTART),1)
FLASH_IMAGE := $(BUILD)/$(TARGET)+$(PROGRAM_TAG)+autostart.bin
PACK_AUTOSTART := --autostart
else
FLASH_IMAGE := $(BUILD)/$(TARGET)+$(PROGRAM_TAG).bin
PACK_AUTOSTART :=
endif
else
FLASH_IMAGE := $(BUILD)/$(TARGET).bin
endif

.PHONY: all apps samples size clean flash bootloader openocd image test
.SECONDARY:

all: $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex apps samples size

$(BUILD):
	@mkdir -p $(BUILD)/board $(BUILD)/apps $(BUILD)/samples

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: $(SRC_DIR)/%.s | $(BUILD)
	@echo "  AS    $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SRC_DIR)/%.S | $(BUILD)
	@echo "  AS    $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/board/%.o: $(BOARD_DIR)/%.c | $(BUILD)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/board/%.o: $(BOARD_DIR)/%.s | $(BUILD)
	@echo "  AS    $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	@echo "  LD    $@"
	@$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	@$(OBJCOPY) -O binary -R .kext $< $@
	@$(OBJCOPY) -O binary -j .kext $< $(BUILD)/$(TARGET)-kext.bin
	@echo "  BIN   $@"

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	@$(OBJCOPY) -O ihex $< $@
	@echo "  HEX   $@"

$(BUILD)/$(TARGET).lst: $(BUILD)/$(TARGET).elf
	@$(OBJDUMP) -h -S $< > $@

# ------------------------------------------------------------------ apps
apps: $(APP_BINS)

$(BUILD)/apps/%.elf: $(APP_DIR)/%/main.c $(APP_DIR)/common/app_start.c $(APP_LD) | $(BUILD)
	@echo "  APP   $@"
	@$(CC) $(APP_CFLAGS) -DAPP_NAME='"$*"' -nostdlib -T $(APP_LD) \
	       -Wl,-Map=$(@:.elf=.map) -Wl,--no-warn-rwx-segments $(APP_GC) \
	       $(APP_DIR)/common/app_start.c $< $(APP_SOFTFP) -lgcc -o $@

$(BUILD)/apps/%.xip.elf: $(APP_DIR)/%/main.c $(APP_DIR)/common/app_start.c $(APP_XIP_LD) | $(BUILD)
	@echo "  APP   $@"
	@$(CC) $(APP_CFLAGS) -DAPP_NAME='"$*"' -DFREYA_APP_XIP -nostdlib -T $(APP_XIP_LD) \
	       -Wl,--emit-relocs -Wl,-Map=$(@:.elf=.map) -Wl,--no-warn-rwx-segments $(APP_GC) \
	       $(APP_DIR)/common/app_start.c $< $(APP_SOFTFP) -lgcc -o $@

$(BUILD)/apps/%.xip.bin: $(BUILD)/apps/%.xip.elf tools/xip_image.py
	@python3 tools/xip_image.py --objcopy $(OBJCOPY) $< $@
	@echo "  BIN   $@"

$(BUILD)/apps/%.bin: $(BUILD)/apps/%.elf
	@$(OBJCOPY) -O binary $< $@
	@echo "  BIN   $@"

# --------------------------------------------------------------- samples
samples: $(SMPL_BINS)

$(BUILD)/samples/%.elf: $(SMPL_DIR)/%/main.c $(APP_DIR)/common/app_start.c $(APP_LD) | $(BUILD)
	@mkdir -p $(@D)
	@echo "  SMPL  $@"
	@$(CC) $(APP_CFLAGS) -DAPP_NAME='"$*"' -nostdlib -T $(APP_LD) \
	       -Wl,-Map=$(@:.elf=.map) -Wl,--no-warn-rwx-segments $(APP_GC) \
	       $(APP_DIR)/common/app_start.c $< $(APP_SOFTFP) -lgcc -o $@

$(BUILD)/samples/%.xip.elf: $(SMPL_DIR)/%/main.c $(APP_DIR)/common/app_start.c $(APP_XIP_LD) | $(BUILD)
	@mkdir -p $(@D)
	@echo "  SMPL  $@"
	@$(CC) $(APP_CFLAGS) -DAPP_NAME='"$*"' -DFREYA_APP_XIP -nostdlib -T $(APP_XIP_LD) \
	       -Wl,--emit-relocs -Wl,-Map=$(@:.elf=.map) -Wl,--no-warn-rwx-segments $(APP_GC) \
	       $(APP_DIR)/common/app_start.c $< $(APP_SOFTFP) -lgcc -o $@

$(BUILD)/samples/%.xip.bin: $(BUILD)/samples/%.xip.elf tools/xip_image.py
	@python3 tools/xip_image.py --objcopy $(OBJCOPY) $< $@
	@echo "  BIN   $@"

$(BUILD)/samples/%.bin: $(BUILD)/samples/%.elf
	@$(OBJCOPY) -O binary $< $@
	@echo "  BIN   $@"

# ----------------------------------------------------------------- misc
size: $(BUILD)/$(TARGET).elf
	@echo
	@$(SIZE) $(BUILD)/$(TARGET).elf
	@echo

disasm: $(BUILD)/$(TARGET).lst

# Runs the FAT and XMODEM code on the host against real FAT images.
test:
	@BOARD=$(BOARD) sh tests/run_tests.sh

# Kernel plus, when PROGRAM is set, the one program image at the address the
# linker reserved.  The region bounds and the auto-start slot are read from
# the kernel ELF so they cannot drift away from boards/<board>/freya.ld.
ifneq ($(PROGRAM),)
$(FLASH_IMAGE): $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin $(PROGRAM_BIN) tools/pack_image.py
	@echo "  PACK  $@"
	@set -eu; \
	 start=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__app_flash_start" { print "0x" $$1 }'); \
	 end=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__app_flash_end" { print "0x" $$1 }'); \
	 slot=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__autostart_start" { print "0x" $$1 }'); \
	 slot_end=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__autostart_end" { print "0x" $$1 }'); \
	 test -n "$$start" && test -n "$$end" && test -n "$$slot" && test -n "$$slot_end"; \
	 python3 tools/pack_image.py \
	     --kernel $(BUILD)/$(TARGET).bin \
	     --app $(PROGRAM_BIN) \
	     --load-addr $$start \
	     --region-end $$end \
	     --slot-addr $$slot \
	     --slot-end $$slot_end \
	     $(PACK_AUTOSTART) \
	     --out $@

all: $(FLASH_IMAGE)
endif

image: $(FLASH_IMAGE)

# The thread scheduler is a second image (__kext_start).  It is written on
# its own so the gap between the kernel and that address is not erased.
flash: $(FLASH_IMAGE) $(BUILD)/$(TARGET)-kext.bin
	@set -eu; \
	addr=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__kext_start" { print "0x" $$1 }'); \
	test -n "$$addr"; \
	st-flash $(STFLASH_OPTS) write $(FLASH_IMAGE) 0x08000000; \
	st-flash $(STFLASH_OPTS) --reset write $(BUILD)/$(TARGET)-kext.bin $$addr

ifeq ($(PROGRAM),)
openocd: $(BUILD)/$(TARGET).elf
	openocd $(OPENOCD_PRE) -f interface/stlink.cfg -f $(OPENOCD_TARGET) \
	        -c "program $< verify reset exit"
else
openocd: $(FLASH_IMAGE) $(BUILD)/$(TARGET)-kext.bin
	@set -eu; \
	addr=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__kext_start" { print "0x" $$1 }'); \
	test -n "$$addr"; \
	openocd $(OPENOCD_PRE) -f interface/stlink.cfg -f $(OPENOCD_TARGET) \
	        -c "program $(FLASH_IMAGE) verify 0x08000000" \
	        -c "program $(BUILD)/$(TARGET)-kext.bin verify reset exit $$addr"
endif

# The chip's own ROM loader: $(BOOTLOADER_HINT)
bootloader: $(FLASH_IMAGE) $(BUILD)/$(TARGET)-kext.bin
	@set -eu; \
	addr=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__kext_start" { print "0x" $$1 }'); \
	test -n "$$addr"; \
	$(BOOTLOADER_KEXT); \
	$(BOOTLOADER_CMD)

clean:
	@rm -rf build

-include $(DEPS)
