/*
 * Freya - linker script for the STM32F103C8T6 ("Blue Pill")
 *
 * 64 KiB internal flash @ 0x08000000
 * 20 KiB SRAM           @ 0x20000000
 *
 * Flash map - the kernel keeps the bottom 44 KiB, the first unused
 * 128-byte aligned slot holds the auto-start flag and reserved padding,
 * and the rest of flash holds one program image, installed from the card
 * or packed in when the module is programmed, and executed in place:
 *   0x08000000 .. 0x0800AFFF   Freya kernel                      (44 KiB)
 *   0x0800B000 .. 0x0800B07F   auto-start flag + reserved          (128 B)
 *   0x0800B080 .. 0x0800FFFF   program flash region              (20352 B)
 *
 * RAM map - the whole system has to fit in a fifth of what the Black Pill
 * offers, so the heap is small and the program region is sized for the
 * kind of program this board can run at all:
 *   0x20000000 .. 0x200017FF   kernel .data/.bss + system heap    ( 6 KiB)
 *   0x20001800 .. 0x200037FF   user program image region          ( 8 KiB)
 *   0x20003800 .. 0x20004FFF   main stack (kernel + user)         ( 6 KiB)
 *
 * A flash resident program spends that 8 KiB window on its .data and .bss
 * alone; the image itself never occupies RAM.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 44K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

__stack_size         = 6K;
__app_ram_start      = 0x20001800;
__app_ram_end        = 0x20003800;
__autostart_start    = 0x0800B000;
__autostart_end      = 0x0800B080;
__app_flash_start    = 0x0800B080;
__app_flash_end      = 0x08010000;
__ram_start       = 0x20000000;
__ram_end         = 0x20005000;
__stack_top       = __ram_end;
__stack_limit     = __ram_end - __stack_size;

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
        . = ALIGN(4);
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
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

    /*
     * The flash erase and program routines have to execute from SRAM: the
     * F103 has no read-while-write, so the flash controller stalls bus
     * reads for as long as an operation is in flight.  They are linked for
     * the base of the program RAM region and copied there when an install
     * begins - a program cannot be loaded at that moment, so the region is
     * free scratch exactly when it is needed and the routines cost nothing
     * out of a 2 KiB heap.
     */
    .ramfunc __app_ram_start : AT (__etext + SIZEOF(.data))
    {
        . = ALIGN(4);
        __ramfunc_start = .;
        *(.ramfunc .ramfunc.*)
        . = ALIGN(4);
        __ramfunc_end = .;
    }
    __ramfunc_load     = LOADADDR(.ramfunc);
    __kernel_flash_end = LOADADDR(.ramfunc) + SIZEOF(.ramfunc);

    ASSERT((__autostart_start & 127) == 0,
           "Freya: auto-start slot is not 128-byte aligned")
    ASSERT((__app_flash_start & 127) == 0,
           "Freya: program flash is not 128-byte aligned")
    ASSERT((__autostart_end - __autostart_start) == 128,
           "Freya: auto-start slot is not 128 bytes")
    ASSERT(__kernel_flash_end <= __autostart_start,
           "Freya: kernel image overflows into the autostart slot")
    ASSERT(__ramfunc_end <= __app_ram_end,
           "Freya: the RAM resident flash routines do not fit the program region")

    /DISCARD/ : {
        *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) *(.comment)
        *(.eh_frame*) *(.ARM.extab*) *(.ARM.exidx*) *(.init) *(.fini)
    }
}
