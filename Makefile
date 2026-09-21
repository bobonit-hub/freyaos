# Freya - bare metal system for the STM32F411CEU6 "Black Pill"
#
#   make            build the kernel image and the example programs
#   make flash      flash build/freya.bin with st-flash (or make dfu / openocd)
#   make size       show the section sizes
#   make clean

TARGET    := freya
BUILD     := build
SRC_DIR   := src
APP_DIR   := apps
SMPL_DIR  := samples

CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
OBJCOPY   := $(CROSS)objcopy
OBJDUMP   := $(CROSS)objdump
SIZE      := $(CROSS)size

CPUFLAGS  := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CFLAGS    := $(CPUFLAGS) \
             -std=gnu11 -Os -g3 \
             -ffreestanding -fno-common -fno-builtin \
             -ffunction-sections -fdata-sections \
             -Wall -Wextra -Wshadow -Wundef \
             -Wno-unused-parameter \
             -Iinclude -I$(SRC_DIR)

ASFLAGS   := $(CPUFLAGS) -g3

LDSCRIPT  := ld/freya.ld
LDFLAGS   := $(CPUFLAGS) -nostdlib -T $(LDSCRIPT) \
             -Wl,--gc-sections -Wl,--build-id=none \
             -Wl,-Map=$(BUILD)/$(TARGET).map

CSRC      := $(wildcard $(SRC_DIR)/*.c)
ASRC      := $(wildcard $(SRC_DIR)/*.s)
OBJS      := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(CSRC)) \
             $(patsubst $(SRC_DIR)/%.s,$(BUILD)/%.o,$(ASRC))
DEPS      := $(OBJS:.o=.d)

# User programs, one directory per program under apps/
APPS      := hello spin
APP_BINS  := $(patsubst %,$(BUILD)/apps/%.bin,$(APPS))
APP_CFLAGS:= $(CPUFLAGS) -std=gnu11 -Os -g3 -ffreestanding -fno-common \
             -fno-builtin -Wall -Wextra -Wno-unused-parameter -Iinclude

# Sample programs, same ABI and linker script, one directory each under samples/
SAMPLES   := blink
SMPL_BINS := $(patsubst %,$(BUILD)/samples/%.bin,$(SAMPLES))

.PHONY: all apps samples size clean flash dfu openocd test
.SECONDARY:

all: $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex apps samples size

$(BUILD):
	@mkdir -p $(BUILD)/apps $(BUILD)/samples

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: $(SRC_DIR)/%.s | $(BUILD)
	@echo "  AS    $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	@echo "  LD    $@"
	@$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	@$(OBJCOPY) -O binary $< $@
	@echo "  BIN   $@"

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	@$(OBJCOPY) -O ihex $< $@
	@echo "  HEX   $@"

$(BUILD)/$(TARGET).lst: $(BUILD)/$(TARGET).elf
	@$(OBJDUMP) -h -S $< > $@

# ------------------------------------------------------------------ apps
apps: $(APP_BINS)

$(BUILD)/apps/%.elf: $(APP_DIR)/%/main.c $(APP_DIR)/common/app_start.c $(APP_DIR)/app.ld | $(BUILD)
	@echo "  APP   $@"
	@$(CC) $(APP_CFLAGS) -DAPP_NAME='"$*"' -nostdlib -T $(APP_DIR)/app.ld \
	       -Wl,-Map=$(@:.elf=.map) -Wl,--no-warn-rwx-segments \
	       $(APP_DIR)/common/app_start.c $< -lgcc -o $@

$(BUILD)/apps/%.bin: $(BUILD)/apps/%.elf
	@$(OBJCOPY) -O binary $< $@
	@echo "  BIN   $@"

# --------------------------------------------------------------- samples
samples: $(SMPL_BINS)

$(BUILD)/samples/%.elf: $(SMPL_DIR)/%/main.c $(APP_DIR)/common/app_start.c $(APP_DIR)/app.ld | $(BUILD)
	@mkdir -p $(@D)
	@echo "  SMPL  $@"
	@$(CC) $(APP_CFLAGS) -DAPP_NAME='"$*"' -nostdlib -T $(APP_DIR)/app.ld \
	       -Wl,-Map=$(@:.elf=.map) -Wl,--no-warn-rwx-segments \
	       $(APP_DIR)/common/app_start.c $< -lgcc -o $@

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
	@sh tests/run_tests.sh

flash: $(BUILD)/$(TARGET).bin
	st-flash --reset write $< 0x08000000

openocd: $(BUILD)/$(TARGET).elf
	openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
	        -c "program $< verify reset exit"

dfu: $(BUILD)/$(TARGET).bin
	dfu-util -a 0 -s 0x08000000:leave -D $<

clean:
	@rm -rf $(BUILD)

-include $(DEPS)
