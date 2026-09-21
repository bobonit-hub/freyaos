/*
 * Freya - USART2 console driver, 115200 8N1.  The board wires the pins up
 * in board_uart_pins(); everything below is the same on every STM32.
 *
 * Receive is interrupt driven into a ring buffer so that characters are
 * never lost while the shell or a user program is busy.  The ISR also
 * implements the Ctrl-C abort: when a user program is running it rewrites
 * the stacked PC of the interrupted thread, so that returning from the
 * interrupt resumes execution in the abort trampoline instead of in the
 * program.  That lets Freya stop a program that never polls the console.
 */
#include "freya.h"

#define RX_BUF_SIZE     512             /* power of two */
#define RX_MASK         (RX_BUF_SIZE - 1)

#define CTRL_C          0x03

static volatile uint8_t  s_rx[RX_BUF_SIZE];
static volatile uint16_t s_head, s_tail;
static volatile uint32_t s_overruns;
static volatile uint8_t  s_raw_mode;     /* 1 = do not treat Ctrl-C specially */

void uart_init(uint32_t baud)
{
    uint32_t brr;

    board_uart_pins();

    USART2->CR1 = 0;
    /* 16x oversampling: BRR holds USARTDIV in 12.4 fixed point, fck / baud. */
    brr = (g_clocks.pclk1_hz + baud / 2) / baud;
    USART2->BRR = brr;
    USART2->CR2 = 0;                    /* 1 stop bit  */
    USART2->CR3 = 0;                    /* no flow ctl */
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    s_head = s_tail = 0;
    s_overruns = 0;
    s_raw_mode = 0;

    nvic_set_priority(USART2_IRQn, 2);
    nvic_enable(USART2_IRQn);
}

void USART2_IRQHandler(void)
{
    uint32_t sr = USART2->SR;

    if (sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) {
        (void)USART2->DR;               /* SR read + DR read clears them */
        s_overruns++;
    }

    while (USART2->SR & USART_SR_RXNE) {
        uint8_t c = (uint8_t)(USART2->DR & 0xFF);
        uint16_t next = (uint16_t)((s_head + 1) & RX_MASK);

        if (c == CTRL_C && !s_raw_mode && g_app.running) {
            /*
             * Hand the kill to PendSV instead of rewriting the frame here:
             * PendSV runs at the lowest priority, so it is guaranteed to
             * execute with the thread's exception frame on top of the
             * stack even if this interrupt preempted another handler.
             */
            app_request_stop();
            continue;
        }

        if (next != s_tail) {
            s_rx[s_head] = c;
            s_head = next;
        } else {
            s_overruns++;               /* drop - console is not keeping up */
        }
    }
}

void uart_set_raw(int raw)
{
    s_raw_mode = (uint8_t)(raw ? 1 : 0);
}

void uart_putc(char c)
{
    while (!(USART2->SR & USART_SR_TXE)) { }
    USART2->DR = (uint32_t)(uint8_t)c;
}

void uart_write(const void *buf, int len)
{
    const uint8_t *p = buf;
    while (len-- > 0) uart_putc((char)*p++);
}

void uart_puts(const char *s)
{
    while (*s) uart_putc(*s++);
}

void uart_drain_tx(void)
{
    while (!(USART2->SR & USART_SR_TC)) { }
}

int uart_rx_ready(void)
{
    return s_head != s_tail;
}

void uart_rx_flush(void)
{
    s_tail = s_head;
}

static int rx_pop(void)
{
    int c;
    uint32_t pm;

    if (s_head == s_tail) return -1;
    pm = irq_save();
    c = s_rx[s_tail];
    s_tail = (uint16_t)((s_tail + 1) & RX_MASK);
    irq_restore(pm);
    return c;
}

int uart_getc(void)
{
    for (;;) {
        int c = rx_pop();
        if (c >= 0) return c;
        if (g_app.running && app_should_stop()) return -1;
        __wfi();
    }
}

int uart_getc_timeout(uint32_t ms)
{
    uint32_t start = sys_ticks();

    for (;;) {
        int c = rx_pop();
        if (c >= 0) return c;
        if (g_app.running && app_should_stop()) return -1;
        if ((uint32_t)(sys_ticks() - start) >= ms) return -1;
    }
}

int uart_getc_raw_timeout(uint32_t ms)
{
    uint32_t start = sys_ticks();

    for (;;) {
        int c = rx_pop();
        if (c >= 0) return c;
        if ((uint32_t)(sys_ticks() - start) >= ms) return -1;
    }
}
