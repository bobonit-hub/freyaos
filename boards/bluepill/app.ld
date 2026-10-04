/*
 * Freya - linker script for user programs on the Blue Pill.
 *
 * A program is linked as one contiguous RAM image at the address of the
 * kernel's program region.  There is no separate load address: 'load'
 * copies the file verbatim and zeroes .bss afterwards.
 */

ENTRY(app_main)

/* A program that starts no threads (the Makefile defines
 * __app_nothreads__ for it, and app_start.c sets FREYA_APP_F_NOTHREADS)
 * may run on into the two 1 KiB thread stacks that follow the window:
 * FREYA_APP_NOTHREADS_SIZE in include/freya_api.h. */
__app_ram_size__ = DEFINED(__app_nothreads__) ? 9K : 7K;

MEMORY
{
    APP (rwx) : ORIGIN = 0x20001C00, LENGTH = __app_ram_size__
}

SECTIONS
{
    .header 0x20001C00 :
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
        /* A program linked with __app_whole_window__ (WHOLE_WINDOW in
         * the Makefile) claims the rest of the window as well: the
         * header's .bss reaches its end, so the loader clears it and
         * counts it as the program's, and __bss_used__ is where the
         * program's own variables stop. */
        __bss_used__ = .;
        __bss_end__ = DEFINED(__app_whole_window__) ?
                      ORIGIN(APP) + LENGTH(APP) : .;
    } > APP

    /* The program stack is the kernel's main stack; keep the image small. */
    __image_size__ = __image_end__ - ORIGIN(APP);

    ASSERT(__bss_end__ <= ORIGIN(APP) + LENGTH(APP),
           "program does not fit the Freya program region (7 KiB, 9 KiB without threads)")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
