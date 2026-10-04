# WeAct MiniSTM32H723 (STM32H723VGT6)
#   Cortex-M7 with FPU at 520 MHz, 1 MiB flash, 564 KiB SRAM (320 KiB of
#   AXI SRAM used), 25 MHz crystal.
#   https://github.com/WeActStudio/WeActStudio.MiniSTM32H723

CPUFLAGS       := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
OPENOCD_TARGET := target/stm32h7x.cfg

# The 8 MiB SPI NOR on SPI1: LittleFS at /spi1.
SPIFLASH       := 1

# st-flash writes the kernel and the extension, and fwsum.py then updates
# the settings in sector 1, which it reads back whole.
CKSUM_PAGE_BASE := 0x08020000
CKSUM_PAGE_SIZE := 131072

# The H723 has a USB DFU loader in ROM on the board's USB-C socket: hold
# BOOT0, tap NRST, then run this.  The extension is written first; :leave
# on the kernel image resets the chip.
BOOTLOADER_KEXT = dfu-util -a 0 -s $$addr -D $(BUILD)/$(TARGET)-kext.bin
BOOTLOADER_CMD  = dfu-util -a 0 -s 0x08000000:leave -D $(FLASH_IMAGE)
BOOTLOADER_HINT := USB DFU (hold BOOT0, tap NRST)

# make dfu writes build/stm32h723/freya.dfu for that same ROM loader.
# 0xFFFF in bcdDevice matches any bootloader revision.
DFU_VID    := 0x0483
DFU_PID    := 0xDF11
DFU_DEVICE := 0xFFFF

# The OTG core can be the USB host: make USB=1 mounts a stick at /usb.
USB_HOST       := 1

# The chip's calendar RTC on the 32.768 kHz crystal: make RTC=internal.
RTC_INTERNAL   := 1

# Rust programs: cargo builds them for this target (see rust/README.md).
RUST_TARGET    := thumbv7em-none-eabihf
RUST_CPU       := cortex-m7
