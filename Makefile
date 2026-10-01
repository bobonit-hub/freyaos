# Freya - a bare metal system for small STM32 boards
#
#   make                    build the kernel image and the example programs
#   make BOARD=bluepill     build for the STM32F103C8T6 "Blue Pill"
#   make BOARD=stm32f405    build for the STM32F405xx (8 MHz crystal)
#   make rust               build the Rust samples (needs cargo; see rust/README.md)
#   make RTC=ds3231         also build the DS3231 driver (PB6 SCL, PB7 SDA)
#   make FIRMWARE_VERSION=3.1.1
#                           override the firmware version
#   make flash              flash the image with st-flash
#   make flash PROGRAM=hello
#                           flash the kernel and one program into the module
#   make flash SCRIPT=boot.sh
#                           flash the kernel and one shell script into the
#                           program flash region
#   make BOARD=bluepill flash PROGRAM=hello AUTOSTART=1
#   make BOARD=bluepill flash SCRIPT=boot.sh AUTOSTART=1
#                           same, with the auto-start flag already on
#   make BOARD=stm32f405 dfu
#                           pack the kernel and the extension into a DfuSe file
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

LFS_DIR   := third_party/littlefs
LFS_FLAGS := -I$(LFS_DIR) -DLFS_NO_MALLOC -DLFS_NO_ASSERT \
             -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NAME_MAX=63

# heatshrink finds its configuration in src/heatshrink_config.h.  Both
# objects are compiled for every board; on one whose board.h leaves
# BOARD_COMPRESS at 0 nothing refers to them and the linker drops them.
HS_DIR    := third_party/heatshrink
HS_FLAGS  := -I$(HS_DIR)

# Ascon-AEAD128, the NIST SP 800-232 reference.  The object is compiled
# for every board; on one whose board.h leaves BOARD_AEAD at 0 nothing
# refers to it and the linker drops it.
ASCON_DIR := third_party/ascon
ASCON_FLAGS := -I$(ASCON_DIR)

CFLAGS    := $(CPUFLAGS) $(BOARD_DEF) $(LFS_FLAGS) $(HS_FLAGS) $(ASCON_FLAGS) \
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

# With no override, src/freya.h supplies the hardcoded firmware version.
# An override must remain the same three-component numeric form.
# The OS version printed by the banner is not this value.
FIRMWARE_VERSION ?=
ifneq ($(strip $(FIRMWARE_VERSION)),)
ifeq ($(shell printf '%s\n' '$(FIRMWARE_VERSION)' | grep -Ec '^[0-9]+\.[0-9]+\.[0-9]+$$'),0)
$(error FIRMWARE_VERSION='$(FIRMWARE_VERSION)' is invalid - use major.minor.patch, for example 3.1.1)
endif
CFLAGS    += -DFREYA_FIRMWARE_VERSION='"$(FIRMWARE_VERSION)"'
endif

ASFLAGS   := $(CPUFLAGS) -g3

LDSCRIPT  := $(BOARD_DIR)/freya.ld
LDFLAGS   := $(CPUFLAGS) -nostdlib -T $(LDSCRIPT) \
             -Wl,--gc-sections -Wl,--build-id=none \
             -Wl,-Map=$(BUILD)/$(TARGET).map

# The DS3231 driver is left out unless the build asks for it.  The pins
# are PB6 (SCL) and PB7 (SDA) on every board; INT/SQW, 32 kHz and RST
# are not connected.  The software clock is built either way.
RTC ?=
CSRC      := $(filter-out $(SRC_DIR)/ds3231.c,$(wildcard $(SRC_DIR)/*.c))
ifeq ($(RTC),ds3231)
CFLAGS    += -DFREYA_RTC_DS3231
CSRC      += $(SRC_DIR)/ds3231.c
else ifneq ($(RTC),)
$(error RTC='$(RTC)' is not a supported clock - use RTC=ds3231, or leave RTC unset)
endif

# main.c and shell.c compile different code when RTC changes, and the
# driver appears or disappears.  The stamp is rewritten only when the
# value changes, so an ordinary rebuild does not redo those files.
$(BUILD)/rtc.stamp: FORCE | $(BUILD)
	@echo '$(RTC)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(BUILD)/main.o $(BUILD)/shell.o $(BUILD)/ds3231.o: $(BUILD)/rtc.stamp

# Command-line flag changes are not visible to make's dependency scanner.
# Keep the shell object, which displays the version, tied to the value.
$(BUILD)/firmware-version.stamp: FORCE | $(BUILD)
	@echo '$(FIRMWARE_VERSION)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(BUILD)/shell.o: $(BUILD)/firmware-version.stamp

.PHONY: FORCE
FORCE:
ASRC      := $(wildcard $(SRC_DIR)/*.s) $(wildcard $(SRC_DIR)/*.S)
BCSRC     := $(wildcard $(BOARD_DIR)/*.c)
BASRC     := $(wildcard $(BOARD_DIR)/*.s)
OBJS      := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(filter %.c,$(CSRC))) \
             $(patsubst $(SRC_DIR)/%.s,$(BUILD)/%.o,$(filter %.s,$(ASRC))) \
             $(patsubst $(SRC_DIR)/%.S,$(BUILD)/%.o,$(filter %.S,$(ASRC))) \
             $(patsubst $(BOARD_DIR)/%.c,$(BUILD)/board/%.o,$(BCSRC)) \
             $(patsubst $(BOARD_DIR)/%.s,$(BUILD)/board/%.o,$(BASRC)) \
             $(if $(filter blackpill,$(BOARD)),$(BUILD)/lfs.o $(BUILD)/lfs_util.o) \
             $(BUILD)/heatshrink_encoder.o $(BUILD)/heatshrink_decoder.o \
             $(BUILD)/ascon.o
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
SAMPLES   := blink tetris edit log forth irq pwm adc i2c spi w1 crypt aead compress flashprobe threads vm \
             basic11 altair altair16 httpd
# A sample a board has no room for at all is not built there.  The 48 KiB
# Altair keeps the 8080's RAM in the program region.  The Blue Pill's
# window is 8 KiB of a 20 KiB SRAM, which cannot hold that.  basic11 is
# the BASIC interpreter compiled for the board with the FPU's floats for
# its numbers; the Blue Pill's Cortex-M3 has no FPU.  httpd keeps a 4 KiB
# page beside its upload buffers and needs the ESP32-C6, which the Blue
# Pill has no link for.
SKIP_bluepill := altair basic11 httpd
SAMPLES   := $(filter-out $(SKIP_$(BOARD)),$(SAMPLES))
# A sample whose code is larger than a board's program RAM region is built
# there as a flash image only: forth is 8 KiB of interpreter, which is the
# whole of the Blue Pill's RAM window before its dictionary is counted, and
# the Altair's 8080 memory fills the Black Pill's RAM window by itself.
# altair16 on the Blue Pill keeps its 16 KiB in the top of program flash,
# so the interpreter runs from flash too.  rustdemo carries core::fmt and
# the heap behind alloc, 12 KiB, which is past that window too.
XIP_ONLY_bluepill  := forth altair16 rustdemo
XIP_ONLY_blackpill := altair
XIP_ONLY_stm32f405 := altair
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

# Rust samples, one Cargo package each under samples/, built against the
# bindings in rust/freya (see rust/README.md).  Cargo builds a static
# library for the board's RUST_TARGET; it is linked with app_start.c and
# the same linker scripts as a C program, so the header, the .bin and the
# .xip.bin come out the same way.  Without cargo they are left out and
# everything else still builds.
#
# LLVM builds some addresses into movw/movt pairs where gcc would use a
# literal pool word.  An installed image's table moves only words, so
# when the loader copies the image to RAM those pairs keep pointing at
# the flash copy.  That copy is the same bytes and stays in place for
# the whole run, so the code they reach and the constants they read are
# still right; they are just reached in flash.
RUST_SAMPLES := rustdemo
CARGO     ?= $(or $(shell command -v cargo 2>/dev/null),$(wildcard $(HOME)/.cargo/bin/cargo))
RUST_LIBDIR := $(BUILD)/rust/$(RUST_TARGET)/release
RUST_FLAGS  := -C target-cpu=$(RUST_CPU)
ifneq ($(CARGO),)
SMPL_BINS += $(patsubst %,$(BUILD)/samples/%.bin,$(filter-out $(XIP_ONLY),$(RUST_SAMPLES)))
ifneq ($(APP_XIP_LD),)
SMPL_BINS += $(patsubst %,$(BUILD)/samples/%.xip.bin,$(RUST_SAMPLES))
endif
else ifneq ($(filter all samples rust,$(or $(MAKECMDGOALS),all)),)
$(info Rust samples skipped: cargo not found (see rust/README.md))
endif

# One user program to store in the board's program flash region when the
# module is programmed.  An app name (hello), a sample name (blink), or the
# path of a .xip.bin linked for that region.  Unset, the image is the kernel
# alone and whatever already occupies the region is left untouched.
PROGRAM ?=
# A shell script to store in that same region, in place of a program.
# The next boot runs it when AUTOSTART=1 and /autorun.bin is absent.
SCRIPT ?=
# Set the auto-start flag in the packed image.  Off unless asked, so a
# module programmed with PROGRAM= or SCRIPT= still boots to the shell
# on every reset.
AUTOSTART ?=

ifneq ($(PROGRAM),)
ifneq ($(SCRIPT),)
$(error PROGRAM and SCRIPT cannot both be set)
endif
endif

ifeq ($(AUTOSTART),1)
ifeq ($(PROGRAM)$(SCRIPT),)
$(error AUTOSTART=1 needs PROGRAM= or SCRIPT= so there is a flash image to set the flag in)
endif
endif

ifneq ($(PROGRAM)$(SCRIPT),)
ifeq ($(APP_XIP_LD),)
$(error '$(BOARD)' keeps no program in flash)
endif
endif

ifneq ($(PROGRAM),)
ifneq ($(filter $(PROGRAM),$(SKIP_$(BOARD))),)
$(error PROGRAM=$(PROGRAM): that sample does not fit '$(BOARD)')
endif
ifneq ($(filter $(PROGRAM),$(APPS)),)
PROGRAM_BIN := $(BUILD)/apps/$(PROGRAM).xip.bin
else ifneq ($(filter $(PROGRAM),$(SAMPLES) $(RUST_SAMPLES)),)
PROGRAM_BIN := $(BUILD)/samples/$(PROGRAM).xip.bin
else
PROGRAM_BIN := $(PROGRAM)
endif
PACK_INPUT := $(PROGRAM_BIN)
PACK_KIND := --app $(PROGRAM_BIN)
PAYLOAD_TAG := $(patsubst %.xip.bin,%,$(notdir $(PROGRAM_BIN)))
else ifneq ($(SCRIPT),)
PACK_INPUT := $(SCRIPT)
PACK_KIND := --script $(SCRIPT)
PAYLOAD_TAG := $(basename $(notdir $(SCRIPT)))
endif

ifneq ($(PROGRAM)$(SCRIPT),)
ifeq ($(AUTOSTART),1)
FLASH_IMAGE := $(BUILD)/$(TARGET)+$(PAYLOAD_TAG)+autostart.bin
PACK_AUTOSTART := --autostart
else
FLASH_IMAGE := $(BUILD)/$(TARGET)+$(PAYLOAD_TAG).bin
PACK_AUTOSTART :=
endif
else
FLASH_IMAGE := $(BUILD)/$(TARGET).bin
endif

.PHONY: all apps samples rust size clean flash bootloader openocd image test dfu
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

$(BUILD)/lfs.o: $(LFS_DIR)/lfs.c | $(BUILD)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -Wno-shadow -MMD -MP -c $< -o $@

$(BUILD)/lfs_util.o: $(LFS_DIR)/lfs_util.c | $(BUILD)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -Wno-shadow -MMD -MP -c $< -o $@

$(BUILD)/heatshrink_%.o: $(HS_DIR)/heatshrink_%.c | $(BUILD)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -Wno-implicit-fallthrough -MMD -MP -c $< -o $@

$(BUILD)/ascon.o: $(ASCON_DIR)/aead.c | $(BUILD)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

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

# ------------------------------------------------------------ Rust samples
rust: $(foreach s,$(RUST_SAMPLES),$(filter $(BUILD)/samples/$(s).bin $(BUILD)/samples/$(s).xip.bin,$(SMPL_BINS)))
ifeq ($(CARGO),)
	@echo "cargo not found: install Rust with rustup and the $(RUST_TARGET) target" >&2; exit 1
endif

# Cargo decides whether the library is stale, so it is always asked.
$(RUST_LIBDIR)/lib%.a: FORCE | $(BUILD)
	@echo "  CARGO $*"
	@FREYA_BOARD=$(BOARD) FREYA_CC=$(CC) RUSTFLAGS="$(RUST_FLAGS)" \
	 $(CARGO) build --quiet --release --target $(RUST_TARGET) \
	   --manifest-path $(SMPL_DIR)/$*/Cargo.toml --target-dir $(abspath $(BUILD)/rust)

RUST_ELFS     := $(patsubst %,$(BUILD)/samples/%.elf,$(RUST_SAMPLES))
RUST_XIP_ELFS := $(patsubst %,$(BUILD)/samples/%.xip.elf,$(RUST_SAMPLES))

$(RUST_ELFS): $(BUILD)/samples/%.elf: $(RUST_LIBDIR)/lib%.a $(APP_DIR)/common/app_start.c $(APP_LD)
	@mkdir -p $(@D)
	@echo "  RUST  $@"
	@$(CC) $(APP_CFLAGS) -DAPP_NAME='"$*"' -nostdlib -T $(APP_LD) \
	       -Wl,-Map=$(@:.elf=.map) -Wl,--no-warn-rwx-segments -Wl,--gc-sections -Wl,-z,noexecstack \
	       $(APP_DIR)/common/app_start.c $< -lgcc -o $@

$(RUST_XIP_ELFS): $(BUILD)/samples/%.xip.elf: $(RUST_LIBDIR)/lib%.a $(APP_DIR)/common/app_start.c $(APP_XIP_LD)
	@mkdir -p $(@D)
	@echo "  RUST  $@"
	@$(CC) $(APP_CFLAGS) -DAPP_NAME='"$*"' -DFREYA_APP_XIP -nostdlib -T $(APP_XIP_LD) \
	       -Wl,--emit-relocs -Wl,-Map=$(@:.elf=.map) -Wl,--no-warn-rwx-segments -Wl,--gc-sections -Wl,-z,noexecstack \
	       $(APP_DIR)/common/app_start.c $< -lgcc -o $@

# A sample in several files keeps main.c as the one the rule compiles,
# and main.c includes the rest; this makes a change to any of them count.
$(BUILD)/samples/altair.elf $(BUILD)/samples/altair.xip.elf: \
	$(wildcard $(SMPL_DIR)/altair/*.c $(SMPL_DIR)/altair/*.h)

# altair16 is the same sources with 16 KiB of RAM; its main.c includes them.
$(BUILD)/samples/altair16.elf $(BUILD)/samples/altair16.xip.elf: \
	$(SMPL_DIR)/altair16/main.c \
	$(wildcard $(SMPL_DIR)/altair/*.c $(SMPL_DIR)/altair/*.h)

# basic11 is the interpreter under basic/ with the float arithmetic;
# its main.c includes basic.c, which includes the rest.
$(BUILD)/samples/basic11.elf $(BUILD)/samples/basic11.xip.elf: \
	basic/basic.c basic/bas.h basic/fpnat.c basic/fpnat.h

# ----------------------------------------------------------------- misc
size: $(BUILD)/$(TARGET).elf
	@echo
	@$(SIZE) $(BUILD)/$(TARGET).elf
	@echo

disasm: $(BUILD)/$(TARGET).lst

# Runs the FAT and XMODEM code on the host against real FAT images.
test:
	@BOARD=$(BOARD) sh tests/run_tests.sh

# Kernel plus, when PROGRAM or SCRIPT is set, that image at the address the
# linker reserved.  The region bounds are read from the kernel ELF so they
# cannot drift away from boards/<board>/freya.ld.  System settings are a
# separate image in their own erase unit: two copies, the firmware sum,
# and the auto-start flag when AUTOSTART=1.
ifneq ($(PROGRAM)$(SCRIPT),)
$(FLASH_IMAGE): $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin $(PACK_INPUT) tools/pack_image.py
	@echo "  PACK  $@"
	@set -eu; \
	 start=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__app_flash_start" { print "0x" $$1 }'); \
	 end=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__app_flash_end" { print "0x" $$1 }'); \
	 test -n "$$start" && test -n "$$end"; \
	 python3 tools/pack_image.py \
	     --kernel $(BUILD)/$(TARGET).bin \
	     $(PACK_KIND) \
	     --load-addr $$start \
	     --region-end $$end \
	     --out $@

ifeq ($(AUTOSTART),1)
SETTINGS_BIN := $(BUILD)/settings-on.bin
else
SETTINGS_BIN := $(BUILD)/settings.bin
endif

$(SETTINGS_BIN): $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin tools/fwsum.py tools/settings.py
	@set -eu; \
	 kext=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__kext_start" { print "0x" $$1 }'); \
	 test -n "$$kext"; \
	 python3 tools/fwsum.py \
	     --span 0x08000000:$(BUILD)/$(TARGET).bin \
	     --span $$kext:$(BUILD)/$(TARGET)-kext.bin \
	     $(PACK_AUTOSTART) \
	     --settings-out $@

all: $(FLASH_IMAGE) $(SETTINGS_BIN)
endif

image: $(FLASH_IMAGE)

# The thread scheduler is a second image (__kext_start).  It is written on
# its own so nothing between the kernel and that address is erased.
# System settings are an erase unit of their own: the last 1 KiB page of
# the Blue Pill, sector 3 (16 KiB) of the F4 boards, where a packed
# program image passes over them as 0xFF and the settings are written
# again after it (auto-start off unless AUTOSTART=1).  A kernel-only flash
# reads the unit back and updates the firmware sum in both copies, and
# leaves the sum alone when both copies are corrupt.
ifeq ($(BOARD),bluepill)
CKSUM_PAGE_BASE := 0x0801FC00
CKSUM_PAGE_SIZE := 1024
else
CKSUM_PAGE_BASE := 0x0800C000
CKSUM_PAGE_SIZE := 16384
endif

ifneq ($(PROGRAM)$(SCRIPT),)
flash: $(FLASH_IMAGE) $(SETTINGS_BIN) $(BUILD)/$(TARGET)-kext.bin tools/fwsum.py
	@set -eu; \
	addr=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__kext_start" { print "0x" $$1 }'); \
	slot=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__settings_start" { print "0x" $$1 }'); \
	test -n "$$addr" && test -n "$$slot"; \
	st-flash $(STFLASH_OPTS) write $(FLASH_IMAGE) 0x08000000; \
	st-flash $(STFLASH_OPTS) write $(BUILD)/$(TARGET)-kext.bin $$addr; \
	st-flash $(STFLASH_OPTS) write $(SETTINGS_BIN) $$slot; \
	st-flash $(STFLASH_OPTS) reset
else
flash: $(FLASH_IMAGE) $(BUILD)/$(TARGET)-kext.bin tools/fwsum.py
	@set -eu; \
	addr=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__kext_start" { print "0x" $$1 }'); \
	slot=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__settings_start" { print "0x" $$1 }'); \
	test -n "$$addr" && test -n "$$slot"; \
	st-flash $(STFLASH_OPTS) write $(FLASH_IMAGE) 0x08000000; \
	st-flash $(STFLASH_OPTS) write $(BUILD)/$(TARGET)-kext.bin $$addr; \
	python3 tools/fwsum.py --device \
	    $(patsubst %,--st-opt %,$(STFLASH_OPTS)) \
	    --span 0x08000000:$(BUILD)/$(TARGET).bin \
	    --span $$addr:$(BUILD)/$(TARGET)-kext.bin \
	    --settings-addr $$slot \
	    --page-base $(CKSUM_PAGE_BASE) --page-size $(CKSUM_PAGE_SIZE); \
	st-flash $(STFLASH_OPTS) reset
endif

ifeq ($(PROGRAM)$(SCRIPT),)
openocd: $(BUILD)/$(TARGET).elf
	openocd $(OPENOCD_PRE) -f interface/stlink.cfg -f $(OPENOCD_TARGET) \
	        -c "program $< verify reset exit"
else
openocd: $(FLASH_IMAGE) $(SETTINGS_BIN) $(BUILD)/$(TARGET)-kext.bin
	@set -eu; \
	addr=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__kext_start" { print "0x" $$1 }'); \
	slot=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__settings_start" { print "0x" $$1 }'); \
	test -n "$$addr" && test -n "$$slot"; \
	openocd $(OPENOCD_PRE) -f interface/stlink.cfg -f $(OPENOCD_TARGET) \
	        -c "program $(FLASH_IMAGE) verify 0x08000000" \
	        -c "program $(BUILD)/$(TARGET)-kext.bin verify $$addr" \
	        -c "program $(SETTINGS_BIN) verify reset exit $$slot"
endif

# DfuSe file for a board whose ROM loader speaks USB DFU.  The kernel and
# the extension are separate images, so the gap between them is left alone.
ifdef DFU_VID
dfu: $(BUILD)/$(TARGET).dfu

ifneq ($(PROGRAM)$(SCRIPT),)
$(BUILD)/$(TARGET).dfu: $(FLASH_IMAGE) $(SETTINGS_BIN) $(BUILD)/$(TARGET)-kext.bin tools/dfu_image.py
	@set -eu; \
	addr=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__kext_start" { print "0x" $$1 }'); \
	slot=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__settings_start" { print "0x" $$1 }'); \
	test -n "$$addr" && test -n "$$slot"; \
	python3 tools/dfu_image.py \
	    --vid $(DFU_VID) --pid $(DFU_PID) --device $(DFU_DEVICE) \
	    --image 0x08000000:$(FLASH_IMAGE) \
	    --image $$addr:$(BUILD)/$(TARGET)-kext.bin \
	    --image $$slot:$(SETTINGS_BIN) \
	    --out $@
else
$(BUILD)/$(TARGET).dfu: $(FLASH_IMAGE) $(BUILD)/$(TARGET)-kext.bin tools/dfu_image.py
	@set -eu; \
	addr=$$($(NM) $(BUILD)/$(TARGET).elf | awk '$$3 == "__kext_start" { print "0x" $$1 }'); \
	test -n "$$addr"; \
	python3 tools/dfu_image.py \
	    --vid $(DFU_VID) --pid $(DFU_PID) --device $(DFU_DEVICE) \
	    --image 0x08000000:$(FLASH_IMAGE) \
	    --image $$addr:$(BUILD)/$(TARGET)-kext.bin \
	    --out $@
endif
else
dfu:
	$(error '$(BOARD)' has no DFU firmware target)
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
