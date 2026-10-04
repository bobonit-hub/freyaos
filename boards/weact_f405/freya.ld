/*
 * Freya - linker script for STM32F405xx
 *
 * 1 MiB internal flash          @ 0x08000000
 * 128 KiB SRAM                   @ 0x20000000
 * (64 KiB of CCM at 0x10000000 is left unused; DMA cannot reach it.)
 *
 * Flash map - the F4 erases in sectors (16, 16, 16, 16, 64, then 128 KiB
 * each; the 1 MiB part has twelve sectors).  The kernel stays in the first
 * three 16 KiB sectors.  System settings take sector 3, a 16 KiB sector of
 * their own, so erasing them never shares a sector with the kernel, the
 * program or the extension.  The program region is every sector between
 * the settings and the extension, which is the last sector.  This map
 * needs the 1 MiB part; every STM32F405 is one.
 *   0x08000000 .. 0x0800BFFF   Freya kernel                      (48 KiB)
 *   0x0800C000 .. 0x0800C3FF   system settings, two copies        (1 KiB)
 *   0x0800C400 .. 0x0800FFFF   rest of sector 3, unused
 *   0x08010000 .. 0x080DFFFF   program flash region, sectors 4..10 (832 KiB)
 *   0x080E0000 .. 0x080FFFFF   kernel extension (threads, SPI, Ascon,
 *                                  SPI flash, LittleFS, shell)    (128 KiB)
 *
 * The extension is a separate image.  It sits in sector 11 so that writing
 * the kernel or the program does not share an erase unit with it, and so
 * the 48 KiB kernel image stays inside sectors 0..2.
 *
 * RAM map:
 *   0x20000000 .. 0x2000EFFF   kernel .data/.bss + system heap   (60 KiB)
 *   0x2000F000 .. 0x2000FFFF   thread stacks, 4 x 1 KiB           ( 4 KiB)
 *   0x20010000 .. 0x2001DFFF   user program image region         (56 KiB)
 *   0x2001E000 .. 0x2001F7FF   shell stack                        ( 6 KiB)
 *   0x2001F800 .. 0x2001FFFF   interrupt stack                    ( 2 KiB)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 48K
    KEXT  (rx)  : ORIGIN = 0x080E0000, LENGTH = 128K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

__stack_size         = 8K;
__irq_stack_size     = 2K;
__worker_stack_size  = 1024;
__worker_count       = 4;
__app_ram_start      = 0x20010000;
__app_ram_end        = 0x2001E000;
__worker_stacks      = __app_ram_start - (__worker_count * __worker_stack_size);
__thread_stack_top   = __stack_top - __irq_stack_size;
__shell_stack_limit  = __stack_limit;
__settings_start     = 0x0800C000;
__settings_end       = 0x0800C400;
__autostart_start    = __settings_start;
__autostart_end      = __settings_end;
__app_flash_start    = 0x08010000;
__app_flash_end      = 0x080E0000;
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

    /*
     * Threads, the switch, the console commands that talk to them, and
     * the SPI master and the cipher.  Claimed before .text so the
     * wildcard below does not take them.  The 48 KiB image has no room
     * left for SPI or the cipher.
     * A call across the two images is an ordinary branch.
     */
    .kext :
    {
        . = ALIGN(4);
        __kext_start = .;
        *thread.o(.text .text.*)
        *thread.o(.rodata .rodata.*)
        *switch.o(.text .text.*)
        *esp_link.o(.text .text.* .rodata .rodata.*)
        *spi_dma.o(.text .text.* .rodata .rodata.*)
        *net.o(.text .text.* .rodata .rodata.*)
        *syslog.o(.text .text.* .rodata .rodata.*)
        *curl.o(.text .text.* .rodata .rodata.*)
        *http.o(.text .text.* .rodata .rodata.*)
        *shell.o(.rodata.s_cmds)
        *shell.o(.text.cmd_stop)
        *shell.o(.text.cmd_threads)
        *shell.o(.text.cmd_spi)
        *spi.o(.text.spi_*)
        *adc.o(.text .text.*)
        *board.o(.text.board_adc_read)
        *shell.o(.text.cmd_adc)
        *shell.o(.text.pin_fail*)
        /* Ascon-AEAD128.  The 48 KiB image has no room for it. */
        *shell.o(.text.cmd_aead)
        *aead.o(.text .text.* .rodata .rodata.*)
        *ascon.o(.text .text.* .rodata .rodata.*)
        /* heatshrink and the two file coders that drive it. */
        *shell.o(.text.cmd_compress)
        *shell.o(.text.cmd_decompress)
        *lz.o(.text .text.* .rodata .rodata.*)
        *heatshrink_encoder.o(.text .text.* .rodata .rodata.*)
        *heatshrink_decoder.o(.text .text.* .rodata .rodata.*)
        *vm.o(.text .text.*)
        /* Socket power.  The 48 KiB image has no room; this extension does.
         * The Blue Pill is the other way around, so its script leaves
         * these sections for .text. */
        *power.o(.text .text.*)
        *shell.o(.text.cmd_power)
        *sd.o(.text.sd_power*)
        *spi.o(.text.sdspi_quiesce)
        *board.o(.text.sdspi_quiesce)
        *spiflash.o(.text .text.*)
        *spiflash.o(.rodata .rodata.*)
        *lfsvol.o(.text .text.*)
        *lfsvol.o(.rodata .rodata.*)
        *lfs.o(.text .text.*)
        *lfs.o(.rodata .rodata.*)
        *lfs_util.o(.text .text.*)
        *lfs_util.o(.rodata .rodata.*)
        *string.o(.text.lfsstr)
        *fat.o(.text.fat_vol*)
        *fat.o(.rodata.fat_vol*)
        *board.o(.text.board_sd_power)
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
        *shell.o(.rodata.cmd_meminfo*)
        *shell.o(.text.cmd_uptime)
        *shell.o(.rodata.cmd_uptime*)
        *shell.o(.text.cmd_clear)
        *shell.o(.rodata.cmd_clear*)
        *shell.o(.text.cmd_pwd)
        *shell.o(.text.cmd_reboot)
        *shell.o(.rodata.cmd_reboot*)
        /* date grew when the DS3231 was added.  The 48 KiB image has no
         * room for that, and the command is not needed to boot. */
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
        *shell.o(.rodata.cmd_aead*)
        *shell.o(.rodata.cmd_compress*)
        *shell.o(.rodata.cmd_syslog*)
        *shell.o(.rodata.cmd_decompress*)
        *vm.o(.rodata .rodata.*)
        *power.o(.rodata .rodata.*)
        *shell.o(.rodata.cmd_power*)
        *shell.o(.rodata.shell_poll_runtime*)
        *shell.o(.rodata.kext_script*)
        *loader.o(.rodata.kext_script*)
        *loader.o(.rodata.kext_api)
        /* The clock calls of the program table, beside the date
         * command: needed at run time, not to boot. */
        *loader.o(.text.kext_rtc)
        *loader.o(.text.kext_rtc.*)
        /* Optional DS3231.  A build that did not pass RTC=ds3231 has no
         * such object, and this wildcard then matches nothing. */
        *ds3231.o(.text .text.*)
        *ds3231.o(.rodata .rodata.*)
        /* The chip's own RTC, RTC=internal; likewise absent otherwise. */
        *rtc.o(.text .text.*)
        *rtc.o(.rodata .rodata.*)
        /* The USB host and the stick at /usb, USB=1; likewise absent
         * otherwise.  Mounted after boot, not needed to get there. */
        *usbh.o(.text .text.* .rodata .rodata.*)
        *usbmsc.o(.text .text.* .rodata .rodata.*)
        *usbvol.o(.text .text.* .rodata .rodata.*)
        *usbdev.o(.text .text.* .rodata .rodata.*)
        *uac.o(.text .text.* .rodata .rodata.*)
        *audio.o(.text .text.* .rodata .rodata.*)
        *shell.o(.text.cmd_audio .text.tone_sample .text.audio_report .text.audio_fail)
        *shell.o(.rodata.cmd_audio* .rodata.audio_report* .rodata.audio_fail*)
        *board.o(.text.board_usb_*)
        *shell.o(.text.cmd_usb .text.usb_info .text.df_fat)
        *shell.o(.rodata.cmd_usb* .rodata.usb_info* .rodata.df_fat*)
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

    /* Everything between here and the thread stacks is the system heap.
     * The stacks sit in front of the program region so a thread does not
     * take its stack out of the shell's. */
    . = ALIGN(8);
    __heap_start = .;
    __heap_end   = __worker_stacks;

    ASSERT(__heap_start < __heap_end,
           "Freya: kernel .data/.bss overflow into the thread stacks")
    ASSERT(__worker_stacks + __worker_count * __worker_stack_size == __app_ram_start,
           "Freya: thread stacks must end where the program region begins")
    ASSERT(__shell_stack_limit < __thread_stack_top,
           "Freya: no room for the shell stack")
    ASSERT((__thread_stack_top & 7) == 0,
           "Freya: thread stack top is not 8-byte aligned")

    /*
     * The F405 has no read-while-write.  Erase and program wait loops live
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

    ASSERT((__settings_start & 127) == 0,
           "Freya: system settings are not 128-byte aligned")
    ASSERT((__app_flash_start & 127) == 0,
           "Freya: program flash is not 128-byte aligned")
    ASSERT((__settings_end - __settings_start) == 1024,
           "Freya: system settings are not 1 KiB")
    ASSERT(__settings_start >= 0x0800C000 && __settings_end <= 0x08010000,
           "Freya: system settings are not inside sector 3")
    ASSERT(__kernel_flash_end <= 0x0800C000,
           "Freya: kernel image does not fit in the first 48 KiB")
    ASSERT(__app_flash_end <= __kext_start,
           "Freya: program flash region overlaps the kernel extension")
    ASSERT(__kext_end <= 0x08100000,
           "Freya: kernel extension runs past the end of flash")
    ASSERT(__ramfunc_end <= __app_ram_end,
           "Freya: the RAM resident flash routines do not fit the program region")

    /DISCARD/ : {
        *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) *(.comment)
        *(.eh_frame*) *(.ARM.extab*) *(.ARM.exidx*) *(.init) *(.fini)
    }
}
