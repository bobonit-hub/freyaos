# STM32F103C8T6 "Blue Pill"
#   Cortex-M3, 128 KiB flash, 20 KiB SRAM, 8 MHz crystal.
#   The size register often still reads 64 KiB.  st-flash and OpenOCD are
#   told 128 KiB or they refuse an image that uses the top half.

CPUFLAGS       := -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
OPENOCD_TARGET := target/stm32f1x.cfg
STFLASH_OPTS   := --flash=128k
OPENOCD_PRE    := -c "set FLASH_SIZE 0x20000"

# The F103 has no USB bootloader; its ROM loader speaks the ST protocol on
# USART1 (PA9/PA10).  Set BOOT0 high, tap NRST, then run this.
BOOTLOADER_CMD  = stm32flash -w $(FLASH_IMAGE) -v -g 0x08000000 \
                             $(if $(PORT),$(PORT),/dev/ttyUSB0)
BOOTLOADER_HINT := USART1 ROM loader (set BOOT0 high, tap NRST)
