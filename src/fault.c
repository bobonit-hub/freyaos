/*
 * Freya - fault handling.
 *
 * A fault raised by a user program is contained: the faulting thread is
 * redirected into a trampoline that unwinds back into the shell, and the
 * program is reported as crashed.  A fault raised by the kernel itself is
 * fatal and dumps the machine state to the console.
 */
#include "freya.h"

void app_fault_trampoline(void);        /* loader.c */
extern volatile int g_app_stop_reason;  /* loader.c */

static const char *fault_name(uint32_t kind)
{
    switch (kind) {
    case APP_STOP_HARDFAULT:  return "HardFault";
    case APP_STOP_MEMFAULT:   return "MemManage fault";
    case APP_STOP_BUSFAULT:   return "BusFault";
    case APP_STOP_USAGEFAULT: return "UsageFault";
    default:                  return "Fault";
    }
}

static void describe(uint32_t kind)
{
    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;

    kprintf("  cause : %s\r\n", fault_name(kind));
    kprintf("  CFSR  : 0x%08x   HFSR: 0x%08x\r\n", cfsr, hfsr);
    if (cfsr & (1UL << 7))  kprintf("  MMFAR : 0x%08x (data access)\r\n", SCB->MMFAR);
    if (cfsr & (1UL << 15)) kprintf("  BFAR  : 0x%08x (bus address)\r\n", SCB->BFAR);
    if (cfsr & (1UL << 0))  kprintf("  detail: instruction access violation\r\n");
    if (cfsr & (1UL << 1))  kprintf("  detail: data access violation\r\n");
    if (cfsr & (1UL << 8))  kprintf("  detail: instruction bus error\r\n");
    if (cfsr & (1UL << 9))  kprintf("  detail: precise data bus error\r\n");
    if (cfsr & (1UL << 10)) kprintf("  detail: imprecise data bus error\r\n");
    if (cfsr & (1UL << 16)) kprintf("  detail: undefined instruction\r\n");
    if (cfsr & (1UL << 17)) kprintf("  detail: invalid state (bad Thumb/EPSR)\r\n");
    if (cfsr & (1UL << 18)) kprintf("  detail: invalid PC load on return\r\n");
    if (cfsr & (1UL << 19)) kprintf("  detail: no coprocessor\r\n");
    if (cfsr & (1UL << 24)) kprintf("  detail: unaligned access\r\n");
    if (cfsr & (1UL << 25)) kprintf("  detail: divide by zero\r\n");
    if (hfsr & (1UL << 30)) kprintf("  detail: escalated configurable fault\r\n");
}

static void clear_fault_status(void)
{
    SCB->CFSR = SCB->CFSR;              /* write 1 to clear */
    SCB->HFSR = SCB->HFSR;
}

/*
 * Entered from the assembly shim with r0 = exception frame of the faulting
 * context and r1 = APP_STOP_* code.
 */
void freya_fault_handler(uint32_t *frame, uint32_t kind)
{
    int from_thread = ((frame[7] & 0x1FFUL) == 0);
    int in_app = g_app.running && from_thread;

    if (in_app) {
        kprintf("\r\n[freya] program fault at pc=0x%08x lr=0x%08x\r\n",
                frame[6], frame[5]);
        describe(kind);
        clear_fault_status();
        g_app_stop_reason = (int)kind;
        /* Resume the thread in the trampoline; it unwinds into the shell. */
        frame[6] = (uint32_t)(uintptr_t)app_fault_trampoline;
        frame[7] = (frame[7] & ~0x0600FC00UL) | (1UL << 24);
        return;
    }

    uart_set_raw(1);
    kprintf("\r\n\r\n*** FREYA KERNEL PANIC ***\r\n");
    kprintf("  r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\r\n",
            frame[0], frame[1], frame[2], frame[3]);
    kprintf("  r12=0x%08x lr=0x%08x pc=0x%08x xpsr=0x%08x\r\n",
            frame[4], frame[5], frame[6], frame[7]);
    describe(kind);
    kprintf("  uptime: %u ms\r\n", sys_uptime_ms());

#ifdef FREYA_APP_FLASH_ADDR
    /* Dump from thread mode so SD timeouts still see SysTick. */
    if (kind == APP_STOP_BUSFAULT && from_thread) {
        clear_fault_status();
        frame[6] = (uint32_t)(uintptr_t)ramdump_then_halt;
        frame[7] = (frame[7] & ~0x0600FC00UL) | (1UL << 24);
        uart_drain_tx();
        return;
    }
#endif

    kprintf("\r\nSystem halted - press any key to reboot.\r\n");
    uart_drain_tx();

    for (;;) {
        if (uart_rx_ready()) sys_reboot();
        led_toggle();
        for (volatile uint32_t i = 0; i < 1500000; i++) { }
    }
}

void NMI_Handler(void)
{
    /* The clock security system raises NMI when HSE dies. */
    RCC->CIR |= (1UL << 23);            /* CSSC: clear the flag */
    kprintf("\r\n[freya] HSE failure, running from HSI backup clock\r\n");
}
