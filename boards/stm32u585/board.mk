# WeAct STM32U585CIU6 core board
#   Cortex-M33 with FPU at 160 MHz, 2 MiB flash, 768 KiB SRAM,
#   25 MHz crystal.  https://github.com/WeActStudio/WeActStudio.STM32U585Cx_CoreBoard

CPUFLAGS       := -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard
OPENOCD_TARGET := target/stm32u5x.cfg

# The SOP-8 footprint takes a SPI NOR chip: LittleFS at /spi1.
SPIFLASH       := 1

# st-flash does not know the U5 family.  Over SWD, 'make openocd' programs
# it through OpenOCD's stm32l4x driver, which covers the U5.
FLASH_UNSUPPORTED := st-flash does not support the STM32U585 - use 'make openocd' or 'make bootloader'

# The U585 has a USB DFU loader in ROM on the board's USB-C socket: hold
# BOOT0, tap NRST, then run this.  The extension lies between the kernel
# and the program region, so a packed kernel + program image covers it
# with 0xFF: the image goes first and the extension after it, and :leave
# on the extension resets the chip.
BOOTLOADER_KEXT = dfu-util -a 0 -s 0x08000000 -D $(FLASH_IMAGE)
BOOTLOADER_CMD  = dfu-util -a 0 -s $$addr:leave -D $(BUILD)/$(TARGET)-kext.bin
BOOTLOADER_HINT := USB DFU (hold BOOT0, tap NRST)

# make dfu writes build/stm32u585/freya.dfu for that same ROM loader.
# 0xFFFF in bcdDevice matches any bootloader revision.
DFU_VID    := 0x0483
DFU_PID    := 0xDF11
DFU_DEVICE := 0xFFFF

# Rust programs: cargo builds them for this target (see rust/README.md).
RUST_TARGET    := thumbv8m.main-none-eabihf
RUST_CPU       := cortex-m33
