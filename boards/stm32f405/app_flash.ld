/*
 * Freya - linker script for flash resident user programs on the STM32F405xx.
 *
 * Unlike app.ld, this is linked with .text and .rodata addresses in the
 * program flash region and writable state in RAM.  The generated relocation
 * table lets the loader copy the image to RAM and rebase those flash-linked
 * program pointers before execution.
 *
 * The addresses here have to agree with FREYA_APP_FLASH_ADDR and
 * FREYA_APP_LOAD_ADDR in include/freya_api.h, and with boards/stm32f405/
 * freya.ld.  ORIGIN is 128-byte aligned so it sits immediately after the
 * auto-start slot.
 */

ENTRY(app_main)

MEMORY
{
    APPFLASH (rx)  : ORIGIN = 0x08010000, LENGTH = 64K
    APPRAM   (rwx) : ORIGIN = 0x20010000, LENGTH = 56K
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
        __bss_end__ = .;
    } > APPRAM

    /* The file on the card is the header, the code, the constants and the
     * .data initialiser - everything with a load address in flash. */
    __image_size__ = __data_load__ + SIZEOF(.data) - ORIGIN(APPFLASH);

    ASSERT(__data_load__ + SIZEOF(.data) <= ORIGIN(APPFLASH) + LENGTH(APPFLASH),
           "program does not fit the Freya program flash region")
    ASSERT(__bss_end__ <= ORIGIN(APPRAM) + LENGTH(APPRAM),
           "program .data + .bss do not fit the 56 KiB Freya program RAM region")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
