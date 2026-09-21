/*
 * Freya - linker script for user programs.
 *
 * A program is linked as one contiguous RAM image at the address of the
 * kernel's program region.  There is no separate load address: 'load'
 * copies the file verbatim and zeroes .bss afterwards.
 */

ENTRY(app_main)

MEMORY
{
    APP (rwx) : ORIGIN = 0x20010000, LENGTH = 56K
}

SECTIONS
{
    .header 0x20010000 :
    {
        KEEP(*(.app_header))
    } > APP

    .text :
    {
        . = ALIGN(4);
        *(.text .text.*)
        *(.glue_7) *(.glue_7t)
        . = ALIGN(4);
    } > APP

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > APP

    .data :
    {
        . = ALIGN(4);
        *(.data .data.*)
        . = ALIGN(4);
    } > APP

    . = ALIGN(4);
    __image_end__ = .;

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > APP

    /* The program stack is the kernel's main stack; keep the image small. */
    __image_size__ = __image_end__ - ORIGIN(APP);

    ASSERT(__bss_end__ <= ORIGIN(APP) + LENGTH(APP),
           "program does not fit the 56 KiB Freya program region")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
