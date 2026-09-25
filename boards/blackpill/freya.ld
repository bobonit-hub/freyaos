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
 *   0x08020000 .. 0x08033FFF   kernel extension (threads, SPI, XTEA,
 *                                  SPI flash, LittleFS, shell)     (80 KiB)
 *   0x08034000 .. 0x0807FFFF   unused
 *
 * The extension is a separate image.  It sits in sector 5 so that writing
 * the kernel does not share an erase unit with the auto-start slot or with
 * the program, and so the 48 KiB image stays inside sectors 0..2.
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
    KEXT  (rx)  : ORIGIN = 0x08020000, LENGTH = 80K
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

    /*
     * Threads, the switch, the console commands that talk to them, and
     * the SPI master and the cipher.  Claimed before .text so the
     * wildcard below does not take them.  The 48 KiB image has no room
     * left for SPI or XTEA.
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
        *curl.o(.text .text.* .rodata .rodata.*)
        *shell.o(.rodata.s_cmds)
        *shell.o(.text.cmd_stop)
        *shell.o(.text.cmd_threads)
        *shell.o(.text.cmd_spi)
        *spi.o(.text.spi_*)
        *adc.o(.text .text.*)
        *board.o(.text.board_adc_read)
        *shell.o(.text.cmd_adc)
        *shell.o(.text.pin_fail*)
        *shell.o(.text.cmd_crypt)
        *crypt.o(.text .text.*)
        *vm.o(.text .text.*)
        /* Socket power.  The 48 KiB image has no room; this extension does.
         * The Blue Pill is the other way around, so its script leaves
         * these sections for .text. */
        *power.o(.text .text.*)
        *shell.o(.text.cmd_power)
        *sd.o(.text.sd_power*)
        *spi.o(.text.sdspi_quiesce)
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
        *shell.o(.rodata.cmd_crypt*)
        *crypt.o(.rodata .rodata.*)
        *vm.o(.rodata .rodata.*)
        *power.o(.rodata .rodata.*)
        *shell.o(.rodata.cmd_power*)
        *shell.o(.rodata.shell_poll_runtime*)
        *shell.o(.rodata.kext_script*)
        *loader.o(.rodata.kext_script*)
        *loader.o(.rodata.kext_api)
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
    ASSERT(__kernel_flash_end <= 0x0800C000,
           "Freya: kernel image does not fit in the first 48 KiB")
    ASSERT(__kernel_flash_end <= __autostart_start,
           "Freya: kernel image overflows into the autostart slot")
    ASSERT(__ramfunc_end <= __app_ram_end,
           "Freya: the RAM resident flash routines do not fit the program region")

    /DISCARD/ : {
        *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) *(.comment)
        *(.eh_frame*) *(.ARM.extab*) *(.ARM.exidx*) *(.init) *(.fini)
    }
}
