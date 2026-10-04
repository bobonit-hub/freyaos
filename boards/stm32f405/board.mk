# STM32F405xx
#   Cortex-M4F, 512 KiB or 1 MiB flash, 128 KiB SRAM (+ 64 KiB CCM),
#   8 MHz crystal.  SYSCLK is 168 MHz.

CPUFLAGS       := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
OPENOCD_TARGET := target/stm32f4x.cfg

# The F405 has a USB DFU loader in ROM: hold BOOT0, tap NRST, then run this.
# The extension is written first; :leave on the kernel image resets the chip.
BOOTLOADER_KEXT = dfu-util -a 0 -s $$addr -D $(BUILD)/$(TARGET)-kext.bin
BOOTLOADER_CMD  = dfu-util -a 0 -s 0x08000000:leave -D $(FLASH_IMAGE)
BOOTLOADER_HINT := USB DFU (hold BOOT0, tap NRST)

# make dfu writes build/stm32f405/freya.dfu for that same ROM loader.
# 0xFFFF in bcdDevice matches any bootloader revision.
DFU_VID    := 0x0483
DFU_PID    := 0xDF11
DFU_DEVICE := 0xFFFF

# The OTG core can be the USB host: make USB=1 mounts a stick at /usb.
USB_HOST       := 1
# More than 512 KiB of flash: room for USB audio, make USB=1 AUDIO=1,
# and the codecs a call needs (CODECS=1).
USB_AUDIO      := 1

# The chip's calendar RTC on the 32.768 kHz crystal: make RTC=internal.
RTC_INTERNAL   := 1

# Rust programs: cargo builds them for this target (see rust/README.md).
RUST_TARGET    := thumbv7em-none-eabihf
RUST_CPU       := cortex-m4
