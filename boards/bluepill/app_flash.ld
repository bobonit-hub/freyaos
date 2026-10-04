/*
 * Freya - linker script for flash resident user programs on the Blue Pill.
 *
 * Unlike app.ld, this is linked with .text and .rodata addresses in the
 * program flash region and writable state in RAM.  The generated relocation
 * table lets the loader copy the image to RAM and rebase those flash-linked
 * program pointers before execution.
 *
 * The addresses here have to agree with FREYA_APP_FLASH_ADDR and
 * FREYA_APP_LOAD_ADDR in include/freya_api.h, and with boards/bluepill/
 * freya.ld.  ORIGIN is the first byte after the kernel image.  System
 * settings live at the end of flash, not in front of this region.  Nothing can check that at link time - a linker script
 * cannot include a C header - so the kernel compares the two at boot.
 */

ENTRY(app_main)

/* A program that starts no threads (the Makefile defines
 * __app_nothreads__ for it, and app_start.c sets FREYA_APP_F_NOTHREADS)
 * may run on into the two 1 KiB thread stacks that follow the window:
 * FREYA_APP_NOTHREADS_SIZE in include/freya_api.h. */
__app_ram_size__ = DEFINED(__app_nothreads__) ? 9K : 7K;

MEMORY
{
    APPFLASH (rx)  : ORIGIN = 0x0800C000, LENGTH = 24K
    APPRAM   (rwx) : ORIGIN = 0x20001C00, LENGTH = __app_ram_size__
}

SECTIONS
{
    .header :
    {
        . = ALIGN(128);
        KEEP(*(.app_header))
    } > APPFLASH

    .text :
    {
        . = ALIGN(4);
        *(.text .text.*)
        *(.glue_7) *(.glue_7t)
        . = ALIGN(4);
    } > APPFLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > APPFLASH

    . = ALIGN(4);
    __data_load__ = .;

    .data : AT (__data_load__)
    {
        . = ALIGN(4);
        __data_start__ = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > APPRAM

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
                      ORIGIN(APPRAM) + LENGTH(APPRAM) : .;
    } > APPRAM

    /* The file on the card is the header, the code, the constants and the
     * .data initialiser - everything with a load address in flash. */
    __image_size__ = __data_load__ + SIZEOF(.data) - ORIGIN(APPFLASH);

    /* The program stack is the kernel's main stack; keep .data and .bss
     * inside the RAM window and the image inside the flash region. */
    ASSERT(__data_load__ + SIZEOF(.data) <= ORIGIN(APPFLASH) + LENGTH(APPFLASH),
           "program does not fit the Freya program flash region")
    ASSERT(__bss_end__ <= ORIGIN(APPRAM) + LENGTH(APPRAM),
           "program .data + .bss do not fit the Freya program RAM region (7 KiB, 9 KiB without threads)")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
