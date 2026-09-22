/*
 * Freya - linker script for STM32F411CEU6 ("Black Pill")
 *
 * 512 KiB internal flash @ 0x08000000
 * 128 KiB SRAM          @ 0x20000000
 *
 * Flash map - the F4 erases in sectors (16, 16, 16, 16, 64, 128, 128,
 * 128 KiB).  The kernel stays in the first three 16 KiB sectors so that
 * the auto-start slot can live at the end of sector 3 without sharing an
 * erase unit with the kernel or with the program:
 *   0x08000000 .. 0x0800BFFF   Freya kernel                      (48 KiB)
 *   0x0800C000 .. 0x0800FF7F   unused (rest of sector 3)
 *   0x0800FF80 .. 0x0800FFFF   auto-start flag + reserved          (128 B)
 *   0x08010000 .. 0x0801FFFF   program flash region              (64 KiB)
 *   0x08020000 .. 0x0807FFFF   unused
 *
 * RAM map:
 *   0x20000000 .. 0x2000FFFF   kernel .data/.bss + system heap   (64 KiB)
 *   0x20010000 .. 0x2001DFFF   user program image region         (56 KiB)
 *   0x2001E000 .. 0x2001FFFF   main stack (kernel + user)        ( 8 KiB)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 48K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

__stack_size         = 8K;
__app_ram_start      = 0x20010000;
__app_ram_end        = 0x2001E000;
__autostart_start    = 0x0800FF80;
__autostart_end      = 0x08010000;
__app_flash_start    = 0x08010000;
__app_flash_end      = 0x08020000;
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
     * The F411 has no read-while-write.  Erase and program wait loops live
     * in the program RAM region and are copied there by flash_begin().
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
