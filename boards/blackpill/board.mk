# WeAct STM32F411CEU6 "Black Pill"
#   Cortex-M4F, 512 KiB flash, 128 KiB SRAM, 25 MHz crystal.

CPUFLAGS       := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
OPENOCD_TARGET := target/stm32f4x.cfg

# The SOP-8 footprint takes a SPI NOR chip: LittleFS at /spi1.
SPIFLASH       := 1

# The F411 has a USB DFU loader in ROM: hold BOOT0, tap NRST, then run this.
# The extension is written first; :leave on the kernel image resets the chip.
BOOTLOADER_KEXT = dfu-util -a 0 -s $$addr -D $(BUILD)/$(TARGET)-kext.bin
BOOTLOADER_CMD  = dfu-util -a 0 -s 0x08000000:leave -D $(FLASH_IMAGE)
BOOTLOADER_HINT := USB DFU (hold BOOT0, tap NRST)

# The chip's calendar RTC on the 32.768 kHz crystal: make RTC=internal.
RTC_INTERNAL   := 1

# Rust programs: cargo builds them for this target (see rust/README.md).
RUST_TARGET    := thumbv7em-none-eabihf
RUST_CPU       := cortex-m4
