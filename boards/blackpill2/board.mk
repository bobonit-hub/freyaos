# WeAct AT32F403ACGU7 "Black Pill 2"
#   Artery AT32F403A, Cortex-M4F at 240 MHz, 1 MiB flash, 96 KiB SRAM,
#   8 MHz crystal.  https://github.com/WeActStudio/WeActStudio.BlackPill

CPUFLAGS       := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
OPENOCD_TARGET := target/artery/at32f4x.cfg

# st-flash does not know Artery parts, and OpenOCD's artery driver has no
# entry for the 1 MiB AT32F403ACG ("Cannot identify target as an Artery
# device"): it debugs over SWD, but 'make openocd' cannot program it.
FLASH_UNSUPPORTED := st-flash does not support the AT32F403A - use 'make bootloader'

# The AT32F403A has a USB DFU loader in ROM: hold BOOT0, tap NRST, then run
# 'make bootloader'.  The loader stalls a request now and then, and plain
# dfu-util stops at the first stall, so tools/dfu_flash.py writes the DfuSe
# file a page per dfu-util run and retries each page.  With a program the
# file covers the whole flash: the chip is mass-erased and the settings
# are written too.
BOOTLOADER_DFU  = python3 tools/dfu_flash.py --device 2e3c:df11
BOOTLOADER_HINT := USB DFU (hold BOOT0, tap NRST)

# make dfu writes build/blackpill2/freya.dfu, or freya+<program>.dfu, for
# that same ROM loader.  0xFFFF in bcdDevice matches any bootloader revision.
DFU_VID    := 0x2E3C
DFU_PID    := 0xDF11
DFU_DEVICE := 0xFFFF

# Rust programs: cargo builds them for this target (see rust/README.md).
RUST_TARGET    := thumbv7em-none-eabihf
RUST_CPU       := cortex-m4
