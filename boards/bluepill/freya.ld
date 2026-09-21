/*
 * Freya - linker script for the STM32F103C8T6 ("Blue Pill")
 *
 * 64 KiB internal flash @ 0x08000000
 * 20 KiB SRAM           @ 0x20000000
 *
 * RAM map - the whole system has to fit in a fifth of what the Black Pill
 * offers, so the heap is small and the program region is sized for the
 * kind of program this board can run at all:
 *   0x20000000 .. 0x200017FF   kernel .data/.bss + system heap    ( 6 KiB)
 *   0x20001800 .. 0x200037FF   user program image region          ( 8 KiB)
 *   0x20003800 .. 0x20004FFF   main stack (kernel + user)         ( 6 KiB)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

__stack_size    = 6K;
__app_ram_start = 0x20001800;
__app_ram_end   = 0x20003800;
__ram_start     = 0x20000000;
__ram_end       = 0x20005000;
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

    /* Everything between here and the user program region is the system heap. */
    . = ALIGN(8);
    __heap_start = .;
    __heap_end   = __app_ram_start;

    ASSERT(__heap_start < __app_ram_start,
           "Freya: kernel .data/.bss overflow into the user program region")

    /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }
}
