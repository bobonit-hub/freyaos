/*
 * Freya - linker script for STM32F411CEU6 ("Black Pill")
 *
 * 512 KiB internal flash @ 0x08000000
 * 128 KiB SRAM          @ 0x20000000
 *
 * RAM map:
 *   0x20000000 .. 0x2000FFFF   kernel .data/.bss + system heap   (64 KiB)
 *   0x20010000 .. 0x2001DFFF   user program image region         (56 KiB)
 *   0x2001E000 .. 0x2001FFFF   main stack (kernel + user)        ( 8 KiB)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

__stack_size    = 8K;
__app_ram_start = 0x20010000;
__app_ram_end   = 0x2001E000;
__ram_start     = 0x20000000;
__ram_end       = 0x20020000;
__stack_top     = __ram_end;
__stack_limit   = __ram_end - __stack_size;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM       : {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    . = ALIGN(4);
    __etext = .;

    .data : AT (__etext)
    {
        . = ALIGN(4);
        __data_start = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    /* This board keeps no program in flash, so the kernel image ends with
     * the .data initialiser (see boards/bluepill/freya.ld for the other
     * arrangement). */
    __kernel_flash_end = __etext + SIZEOF(.data);

    /* Everything between here and the user program region is the system heap. */
    . = ALIGN(8);
    __heap_start = .;
    __heap_end   = __app_ram_start;

    ASSERT(__heap_start < __app_ram_start,
           "Freya: kernel .data/.bss overflow into the user program region")

    /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }
}
