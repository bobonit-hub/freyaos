# Geehy APM32F407ZGT6 board
#   Cortex-M4F, 1 MiB flash, 128 KiB SRAM (+ 64 KiB CCM), LQFP144,
#   8 MHz crystal, microSD on the SDIO pins, LED on PF9 (active low).
#   SYSCLK is 168 MHz.  The STM32F407's register map: see board.h.

CPUFLAGS       := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
OPENOCD_TARGET := target/stm32f4x.cfg

# Geehy's ROM loader speaks the ST protocol on USART1 (PA9/PA10).  Set
# BOOT0 high, tap NRST, then run this.  No .dfu file is made: its USB
# vendor and product ids would be ST's, and the APM32's loader need not
# answer to them.
BOOTLOADER_KEXT = stm32flash -w $(BUILD)/$(TARGET)-kext.bin -v -S $$addr \
                             $(if $(PORT),$(PORT),/dev/ttyUSB0)
BOOTLOADER_CMD  = stm32flash -w $(FLASH_IMAGE) -v -g 0x08000000 \
                             $(if $(PORT),$(PORT),/dev/ttyUSB0)
BOOTLOADER_HINT := USART1 ROM loader (set BOOT0 high, tap NRST)

# The OTG core can be the USB host: make USB=1 mounts a stick at /usb.
USB_HOST       := 1
# More than 512 KiB of flash: room for USB audio, make USB=1 AUDIO=1,
# and the codecs a call needs (CODECS=1).
USB_AUDIO      := 1

# The chip's calendar RTC, for a board with the 32.768 kHz crystal fitted:
# make RTC=internal.
RTC_INTERNAL   := 1

# Rust programs: cargo builds them for this target (see rust/README.md).
RUST_TARGET    := thumbv7em-none-eabihf
RUST_CPU       := cortex-m4
