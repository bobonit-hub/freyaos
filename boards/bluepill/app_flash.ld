/*
 * Freya - linker script for flash resident user programs on the Blue Pill.
 *
 * Unlike app.ld, which lays a program out as one contiguous RAM image,
 * this is the ordinary split: .text and .rodata execute in place from the
 * program flash region, .data is copied from an initialiser in flash into
 * the program RAM region, and .bss is zeroed there.  The loader does both,
 * the same way it already zeroes .bss for a RAM image.
 *
 * The addresses here have to agree with FREYA_APP_FLASH_ADDR and
 * FREYA_APP_LOAD_ADDR in include/freya_api.h, and with boards/bluepill/
 * freya.ld.  ORIGIN is 128-byte aligned so it sits immediately after the
 * auto-start slot.  Nothing can check that at link time - a linker script
 * cannot include a C header - so the kernel compares the two at boot.
 */

ENTRY(app_main)

MEMORY
{
    APPFLASH (rx)  : ORIGIN = 0x08009C80, LENGTH = 25472
    APPRAM   (rwx) : ORIGIN = 0x20001800, LENGTH = 8K
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

    /* The program stack is the kernel's main stack; keep .data and .bss
     * inside the RAM window and the image inside the flash region. */
    ASSERT(__data_load__ + SIZEOF(.data) <= ORIGIN(APPFLASH) + LENGTH(APPFLASH),
           "program does not fit the Freya program flash region")
    ASSERT(__bss_end__ <= ORIGIN(APPRAM) + LENGTH(APPRAM),
           "program .data + .bss do not fit the 8 KiB Freya program RAM region")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
