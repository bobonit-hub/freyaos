# WeAct STM32H523CET6 core board
#   Cortex-M33 with FPU at 250 MHz, 512 KiB flash, 272 KiB SRAM,
#   8 MHz crystal.  https://github.com/WeActStudio/WeActStudio.STM32H523CoreBoard

CPUFLAGS       := -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard

# OpenOCD 0.12 has no H5 target script; 'make openocd' needs a build that
# ships target/stm32h5x.cfg.  'make bootloader' needs nothing but dfu-util.
OPENOCD_TARGET := target/stm32h5x.cfg

# The SOP-8 footprint takes a SPI NOR chip: LittleFS at /spi1.
SPIFLASH       := 1

# st-flash writes the kernel and the extension, and fwsum.py then updates
# the settings in their own 8 KiB page at 0x0800C000.
CKSUM_PAGE_BASE := 0x0800C000
CKSUM_PAGE_SIZE := 8192

# The H523 has a USB DFU loader in ROM on the board's USB-C socket: hold
# BOOT0, tap NRST, then run this.  The extension lies between the kernel
# and the program region, so a packed kernel + program image covers it
# with 0xFF: the image goes first and the extension after it, and :leave
# on the extension resets the chip.
BOOTLOADER_KEXT = dfu-util -a 0 -s 0x08000000 -D $(FLASH_IMAGE)
BOOTLOADER_CMD  = dfu-util -a 0 -s $$addr:leave -D $(BUILD)/$(TARGET)-kext.bin
BOOTLOADER_HINT := USB DFU (hold BOOT0, tap NRST)

# make dfu writes build/stm32h523/freya.dfu for that same ROM loader.
# 0xFFFF in bcdDevice matches any bootloader revision.
DFU_VID    := 0x0483
DFU_PID    := 0xDF11
DFU_DEVICE := 0xFFFF

# Rust programs: cargo builds them for this target (see rust/README.md).
RUST_TARGET    := thumbv8m.main-none-eabihf
RUST_CPU       := cortex-m33
