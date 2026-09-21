# STM32F103C8T6 "Blue Pill"
#   Cortex-M3, 64 KiB flash, 20 KiB SRAM, 8 MHz crystal.

CPUFLAGS       := -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
OPENOCD_TARGET := target/stm32f1x.cfg

# The F103 has no USB bootloader; its ROM loader speaks the ST protocol on
# USART1 (PA9/PA10).  Set BOOT0 high, tap NRST, then run this.
BOOTLOADER_CMD  = stm32flash -w $(BUILD)/$(TARGET).bin -v -g 0x08000000 \
                             $(if $(PORT),$(PORT),/dev/ttyUSB0)
BOOTLOADER_HINT := USART1 ROM loader (set BOOT0 high, tap NRST)
