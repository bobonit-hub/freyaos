/*
 * Freya - linker script for the STM32F103C8T6 ("Blue Pill")
 *
 * 128 KiB internal flash @ 0x08000000
 * 20 KiB SRAM            @ 0x20000000
 *
 * Flash map - the kernel keeps the bottom 48 KiB, the first unused
 * 128-byte aligned slot holds the auto-start flag and reserved padding,
 * and the rest of the 128 KiB holds one program image, installed from the
 * card or packed in when the module is programmed, then copied to RAM when
 * it fits.  The size register on these parts often still reads 64 KiB.
 *   0x08000000 .. 0x0800BFFF   Freya kernel                      (48 KiB)
 *   0x0800C000 .. 0x0800C07F   auto-start flag + reserved          (128 B)
 *   0x0800C080 .. 0x0801D7FF   program flash region             (71552 B)
 *   0x0801D800 .. 0x0801FFFF   kernel extension (threads, SPI)    (10 KiB)
 *
 * The extension is its own image at the top of the 128 KiB.  The program
 * region stops short of it, so install and uninstall never erase it,
 * and the 48 KiB image still ends on the page before the auto-start slot.
 *
 * RAM map - the whole system has to fit in a fifth of what the Black Pill
 * offers, so the heap is small and the program region is sized for the
 * kind of program this board can run at all:
 *   0x20000000 .. 0x200017FF   kernel .data/.bss + system heap    ( 6 KiB)
 *   0x20001800 .. 0x200037FF   user program image region          ( 8 KiB)
 *   0x20003800 .. 0x20003FFF   thread stacks, 2 x 1 KiB           ( 2 KiB)
 *   0x20004000 .. 0x200049FF   shell stack                      (2560 B)
 *   0x20004A00 .. 0x20004FFF   interrupt stack                  (1536 B)
 *
 * A copied installed program shares that 8 KiB window between its image
 * (at the top) and its .data and .bss (at the bottom).  Larger images
 * execute in place and use the window only for writable state.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 48K
    KEXT  (rx)  : ORIGIN = 0x0801D800, LENGTH = 10K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

__stack_size         = 6K;
__irq_stack_size     = 1536;
__worker_stack_size  = 1024;
__worker_count       = 2;
__app_ram_start      = 0x20001800;
__app_ram_end        = 0x20003800;
__worker_stacks      = __stack_limit;
__thread_stack_top   = __stack_top - __irq_stack_size;
__shell_stack_limit  = __worker_stacks + __worker_count * __worker_stack_size;
__autostart_start    = 0x0800C000;
__autostart_end      = 0x0800C080;
__app_flash_start    = 0x0800C080;
__app_flash_end      = 0x0801D800;
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
     * the SPI master.  Claimed before .text so the wildcard below does
     * not take them.  The extension is 10 KiB so SPI fits beside them.
     * A call across the two images is an ordinary branch.
     */
    .kext :
    {
        . = ALIGN(4);
        __kext_start = .;
        *thread.o(.text .text.*)
        *thread.o(.rodata .rodata.*)
        *switch.o(.text .text.*)
        *shell.o(.text.cmd_stop)
        *shell.o(.text.cmd_threads)
        *shell.o(.text.cmd_spi)
        *spi.o(.text.spi_*)
        *shell.o(.text.shell_poll_runtime)
        *shell.o(.text.kext_script)
        *shell.o(.text.kext_script.*)
        *loader.o(.text.kext_script)
        *loader.o(.text.kext_script.*)
        *uart.o(.text.uart_take_ctrlc)
        *shell.o(.rodata.cmd_stop*)
        *shell.o(.rodata.cmd_threads*)
        *shell.o(.rodata.cmd_spi*)
        *spi.o(.rodata.spi_*)
        *shell.o(.rodata.shell_poll_runtime*)
        *shell.o(.rodata.kext_script*)
        *loader.o(.rodata.kext_script*)
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
