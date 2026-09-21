# WeAct STM32F411CEU6 "Black Pill"
#   Cortex-M4F, 512 KiB flash, 128 KiB SRAM, 25 MHz crystal.

CPUFLAGS       := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
OPENOCD_TARGET := target/stm32f4x.cfg

# The F411 has a USB DFU loader in ROM: hold BOOT0, tap NRST, then run this.
BOOTLOADER_CMD  = dfu-util -a 0 -s 0x08000000:leave -D $(FLASH_IMAGE)
BOOTLOADER_HINT := USB DFU (hold BOOT0, tap NRST)
