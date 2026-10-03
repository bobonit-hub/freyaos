# WeAct AT32F403ACGU7 "Black Pill 2"
#   Artery AT32F403A, Cortex-M4F at 240 MHz, 1 MiB flash, 96 KiB SRAM,
#   8 MHz crystal.  https://github.com/WeActStudio/WeActStudio.BlackPill

CPUFLAGS       := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
OPENOCD_TARGET := target/artery/at32f4x.cfg

# st-flash does not know Artery parts.  Over SWD, 'make openocd' programs
# the chip through OpenOCD's artery driver.
FLASH_UNSUPPORTED := st-flash does not support the AT32F403A - use 'make openocd' or 'make bootloader'

# The AT32F403A has a USB DFU loader in ROM: hold BOOT0, tap NRST, then run
# this.  The extension lies between the kernel and the program region, so
# a packed kernel + program image covers it with 0xFF: here the image goes
# first and the extension after it, the other way round from the F4
# boards, and :leave on the extension resets the chip.
BOOTLOADER_KEXT = dfu-util -a 0 -s 0x08000000 -D $(FLASH_IMAGE)
BOOTLOADER_CMD  = dfu-util -a 0 -s $$addr:leave -D $(BUILD)/$(TARGET)-kext.bin
BOOTLOADER_HINT := USB DFU (hold BOOT0, tap NRST)

# make dfu writes build/blackpill2/freya.dfu for that same ROM loader.
# 0xFFFF in bcdDevice matches any bootloader revision.
DFU_VID    := 0x2E3C
DFU_PID    := 0xDF11
DFU_DEVICE := 0xFFFF

# Rust programs: cargo builds them for this target (see rust/README.md).
RUST_TARGET    := thumbv7em-none-eabihf
RUST_CPU       := cortex-m4
