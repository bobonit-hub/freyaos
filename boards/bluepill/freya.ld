/*
 * Freya - linker script for the STM32F103C8T6 ("Blue Pill")
 *
 * 128 KiB internal flash @ 0x08000000
 * 20 KiB SRAM            @ 0x20000000
 *
 * Flash map - the kernel keeps the bottom 48 KiB, then one program image,
 * installed from the card or packed in when the module is programmed,
 * then copied to RAM when it fits.  The kernel extension follows the
 * program and stops at the last page, which holds two copies of the
 * system settings.  The size register on these parts often still reads
 * 64 KiB.
 *   0x08000000 .. 0x0800BFFF   Freya kernel                      (48 KiB)
 *   0x0800C000 .. 0x08011FFF   program flash region              (24 KiB)
 *   0x08012000 .. 0x0801FBFF   kernel extension                  (55 KiB)
 *   0x0801FC00 .. 0x0801FFFF   system settings, two copies        (1 KiB)
 *
 * The extension is its own image.  The program region stops short of it,
 * so install and uninstall never erase it, and a settings erase is the
 * last page only.
 *
 * RAM map - the whole system has to fit in a fifth of what the Black Pill
 * offers, so the heap is small and the program region is sized for the
 * kind of program this board can run at all:
 *   0x20000000 .. 0x20001BFF   kernel .data/.bss + system heap    ( 7 KiB)
 *   0x20001C00 .. 0x200037FF   user program image region          ( 7 KiB)
 *   0x20003800 .. 0x20003FFF   thread stacks, 2 x 1 KiB           ( 2 KiB)
 *   0x20004000 .. 0x200049FF   shell stack                      (2560 B)
 *   0x20004A00 .. 0x20004FFF   interrupt stack                  (1536 B)
 *
 * A copied installed program shares that 7 KiB window between its image
 * (at the top) and its .data and .bss (at the bottom).  Larger images
 * execute in place and use the window only for writable state.
 *
 * The kernel slice is a kilobyte wider than the program window is, the
 * other way round from the Black Pill: the static buffers of the shell,
 * the console and the file system come to just over 6 KiB on their own,
 * and what is left of the seventh kilobyte is the whole system heap.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 48K
    KEXT  (rx)  : ORIGIN = 0x08012000, LENGTH = 55K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

__stack_size         = 6K;
__irq_stack_size     = 1536;
__worker_stack_size  = 1024;
__worker_count       = 2;
__app_ram_start      = 0x20001C00;
__app_ram_end        = 0x20003800;
__worker_stacks      = __stack_limit;
__thread_stack_top   = __stack_top - __irq_stack_size;
__shell_stack_limit  = __worker_stacks + __worker_count * __worker_stack_size;
__settings_start     = 0x0801FC00;
__settings_end       = 0x08020000;
__autostart_start    = __settings_start;
__autostart_end      = __settings_end;
__app_flash_start    = 0x0800C000;
__app_flash_end      = 0x08012000;
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

    /*
     * Threads, the switch, the console commands that talk to them, and
     * the SPI master and the cipher.  Claimed before .text so the
     * wildcard below does not take them.  The extension is 55 KiB so
     * SPI, XTEA, the virtual machine and the shell variables fit beside
     * the scheduler.  The last page of flash is system settings.
     * Float helpers for the variables live here too: the 48 KiB image
     * has no room for them.
     * A call across the two images is an ordinary branch.
     */
    .kext :
    {
        . = ALIGN(4);
        __kext_start = .;
        *thread.o(.text .text.*)
        *thread.o(.rodata .rodata.*)
        *switch.o(.text .text.*)
        *net.o(.text .text.* .rodata .rodata.*)
        *curl.o(.text .text.* .rodata .rodata.*)
        *shell.o(.text.cmd_spi)
        *spi.o(.text.spi_*)
        *adc.o(.text .text.*)
        *board.o(.text.board_adc_read)
        *shell.o(.text.cmd_adc)
        *shell.o(.text.pin_fail*)
        /* XTEA is not built here either.  crypt() is a stub of a
         * dozen bytes in the kernel image, and the command that says
         * so is .text.kext_script, already claimed above. */
        /* Ascon is not built here.  The two service calls are a dozen
         * bytes and stay in the kernel image; the command that says so
         * is .text.kext_script, already claimed above. */
        /* heatshrink and the two file coders that drive it. */
        *shell.o(.text.cmd_compress)
        *shell.o(.text.cmd_decompress)
        *lz.o(.text .text.* .rodata .rodata.*)
        *heatshrink_encoder.o(.text .text.* .rodata .rodata.*)
        *heatshrink_decoder.o(.text .text.* .rodata .rodata.*)
        /* The machine is not built here; see BOARD_VM.  Its three
         * service calls are stubs of a few bytes in the kernel image. */
        /* Socket power is left for .text; the 48 KiB image still has
         * room for it.  The Black Pill script claims those sections
         * up here instead. */
        *shell.o(.text.shell_poll_runtime)
        /* Terminal login, the password and the settings area are
         * runtime-only.  Keep them in the extension so the fixed 48 KiB
         * boot image stays inside its flash. */
        *settings.o(.text .text.* .rodata .rodata.*)
        *loader.o(.text.app_*password*)
        *shell.o(.text.cmd_password)
        *shell.o(.text.line_holds_secret)
        *shell.o(.text.secret_wipe*)
        *shell.o(.text.readline*)
        *shell.o(.text.shell_run)
        *shell.o(.rodata.cmd_password*)
        *shell.o(.rodata.line_holds_secret*)
        *shell.o(.rodata.readline*)
        *shell.o(.rodata.shell_run*)
        *shell.o(.text.cmd_loglevel)
        *shell.o(.rodata.cmd_loglevel*)
        *shell.o(.text.cmd_meminfo)
        *xmodem.o(.text .text.*)
        *xmodem.o(.rodata .rodata.*)
        *softfp.o(.text .text.*)
        /* The date command and the clock setter beside it.  They
         * were in the 48 KiB image while it had room; the cipher this
         * board no longer builds left enough space here for them. */
        *shell.o(.text.cmd_date)
        *shell.o(.text.cmd_date.*)
        *shell.o(.rodata.cmd_date*)
        *shell.o(.text.kext_script)
        *shell.o(.text.kext_script.*)
        *loader.o(.text.kext_script)
        *loader.o(.text.kext_script.*)
        *uart.o(.text.uart_take_ctrlc)
        *shell.o(.rodata.cmd_stop*)
        *shell.o(.rodata.cmd_threads*)
        *shell.o(.rodata.cmd_spi*)
        *spi.o(.rodata.spi_*)
        *adc.o(.rodata .rodata.*)
        *shell.o(.rodata.cmd_adc*)
        *shell.o(.rodata.pin_fail*)
        *shell.o(.rodata.cmd_compress*)
        *shell.o(.rodata.cmd_decompress*)
        *shell.o(.rodata.shell_poll_runtime*)
        *softfp.o(.rodata .rodata.*)
        *shell.o(.rodata.kext_script*)
        *loader.o(.rodata.kext_script*)
        *loader.o(.rodata.kext_api)
        /* The clock calls of the program table, beside the date
         * command: needed at run time, not to boot. */
        *loader.o(.text.kext_rtc)
        *loader.o(.text.kext_rtc.*)
        *shell.o(.rodata.s_cmds)
        /* RTC decoding and bus helpers live here so enabling the driver
         * does not overflow the fixed 48 KiB boot image.  The write entry
         * itself remains in that image to balance the two regions. */
        *ds3231.o(.text.month_days)
        *ds3231.o(.text.to_bcd)
        *ds3231.o(.text.from_bcd)
        *ds3231.o(.text.ds3231_xfer)
        *ds3231.o(.text.ds3231_pins_match)
        *ds3231.o(.text.ds3231_read)
        *ds3231.o(.text.ds3231_boot)
        *ds3231.o(.rodata .rodata.*)
        . = ALIGN(4);
        __kext_end = .;
    } > KEXT

    ASSERT(__kext_start == ORIGIN(KEXT),
           "Freya: kernel extension does not start where it is flashed")
    ASSERT(__kext_end <= ORIGIN(KEXT) + LENGTH(KEXT),
           "Freya: kernel extension overflows its flash region")

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
    ASSERT(__shell_stack_limit < __thread_stack_top,
           "Freya: no room for the shell stack")
    ASSERT((__thread_stack_top & 7) == 0,
           "Freya: thread stack top is not 8-byte aligned")

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

    ASSERT((__settings_start & 127) == 0,
           "Freya: system settings are not 128-byte aligned")
    ASSERT((__app_flash_start & 127) == 0,
           "Freya: program flash is not 128-byte aligned")
    ASSERT((__settings_end - __settings_start) == 1024,
           "Freya: system settings are not 1 KiB")
    ASSERT(__settings_end == 0x08020000,
           "Freya: system settings are not the last bytes of flash")
    ASSERT(__kernel_flash_end <= __app_flash_start,
           "Freya: kernel image overflows into the program region")
    ASSERT(__kext_end <= __settings_start,
           "Freya: kernel extension overflows into system settings")
    ASSERT(__ramfunc_end <= __app_ram_end,
           "Freya: the RAM resident flash routines do not fit the program region")

    /DISCARD/ : {
        *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) *(.comment)
        *(.eh_frame*) *(.ARM.extab*) *(.ARM.exidx*) *(.init) *(.fini)
    }
}
