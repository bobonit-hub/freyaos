/*
 * Freya - USART2 console driver, 921600 8N1.  The board wires the pins up
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
static volatile int      s_waiters;      /* blocked in uart_getc          */

/* A copy of what the console transmitted, for the C6 terminal session.
 * The shell drains it; bytes are dropped only when that copy is full.
 * Boards without the coprocessor do not keep the copy: it is 1 KiB,
 * which the Blue Pill's kernel RAM does not have. */
#if BOARD_ESP_LINK
#define TERM_TX_SIZE    1024
#define TERM_TX_MASK    (TERM_TX_SIZE - 1)
static uint8_t  s_term_tx[TERM_TX_SIZE];
static volatile uint16_t s_term_th, s_term_tt;
#endif

__attribute__((weak)) void term_pump(void) { }

/* A page script prints into this buffer instead of the terminal copy.
 * The local console still shows the text. */
static char *s_cap_buf;
static int   s_cap_max;
static int   s_cap_len;
static int   s_cap_on;
static int   s_cap_drop;

void uart_capture_begin(char *buf, int max)
{
    s_cap_buf = buf;
    s_cap_max = max > 0 ? max : 0;
    s_cap_len = 0;
    s_cap_drop = 0;
    s_cap_on = 1;
}

int uart_capture_end(void)
{
    int n = s_cap_len;

    s_cap_on = 0;
    s_cap_buf = NULL;
    return n;
}

int uart_capture_dropped(void)
{
    return s_cap_drop;
}

#if BOARD_ESP_LINK
static void term_tx_push(uint8_t c)
{
    uint32_t pm = irq_save();
    uint16_t next = (uint16_t)((s_term_th + 1) & TERM_TX_MASK);

    if (next != s_term_tt) {
        s_term_tx[s_term_th] = c;
        s_term_th = next;
    }
    irq_restore(pm);
}

int uart_term_pending(void)
{
    uint32_t pm = irq_save();
    int n = (int)((s_term_th - s_term_tt) & TERM_TX_MASK);
    irq_restore(pm);
    return n;
}

int uart_term_peek(uint8_t *dst, int max)
{
    uint32_t pm = irq_save();
    uint16_t t = s_term_tt;
    int n = 0;

    while (n < max && t != s_term_th) {
        dst[n++] = s_term_tx[t];
        t = (uint16_t)((t + 1) & TERM_TX_MASK);
    }
    irq_restore(pm);
    return n;
}

void uart_term_drop(int n)
{
    uint32_t pm = irq_save();

    while (n > 0 && s_term_tt != s_term_th) {
        s_term_tt = (uint16_t)((s_term_tt + 1) & TERM_TX_MASK);
        n--;
    }
    irq_restore(pm);
}
#else
static void term_tx_push(uint8_t c) { (void)c; }

int uart_term_pending(void) { return 0; }

int uart_term_peek(uint8_t *dst, int max)
{
    (void)dst;
    (void)max;
    return 0;
}

void uart_term_drop(int n) { (void)n; }
#endif

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

    nvic_set_priority(USART2_IRQn, IRQ_PRIO_CONSOLE);
    nvic_enable(USART2_IRQn);
}

void usart2_interrupt(uint32_t *frame);

/*
 * The console runs above every other interrupt, which makes it the only
 * place from which a program's own pin or timer handler can be stopped
 * once it stops finishing.  Doing that needs the exception frame of
 * whatever was interrupted, so the entry point is a shim that captures
 * MSP before the compiler's prologue has moved it.
 */
#ifndef FREYA_HOST
__attribute__((naked)) void USART2_IRQHandler(void)
{
    __asm volatile ("mrs r0, msp\n\t"
                    "b   usart2_interrupt");
}
#endif

void usart2_interrupt(uint32_t *frame)
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
             *
             * PendSV cannot preempt a pin or timer handler, though - it
             * is below them - so a handler that is looping is redirected
             * here, where its frame is the one that was interrupted.  It
             * unwinds into its own interrupt, that interrupt returns, and
             * PendSV then ends the run as usual.
             */
            app_request_stop();
            app_handler_kill(frame);
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

void uart_rx_push(uint8_t c)
{
    uint16_t next;
    uint32_t pm = irq_save();

    if (c == CTRL_C && !s_raw_mode && g_app.running)
        app_request_stop();
    next = (uint16_t)((s_head + 1) & RX_MASK);
    if (next != s_tail) {
        s_rx[s_head] = c;
        s_head = next;
    } else {
        s_overruns++;
    }
    irq_restore(pm);
}

void uart_set_raw(int raw)
{
    s_raw_mode = (uint8_t)(raw ? 1 : 0);
}

void uart_putc(char c)
{
    while (!(USART2->SR & USART_SR_TXE)) { }
    USART2->DR = (uint32_t)(uint8_t)c;
    if (s_cap_on) {
        if (s_cap_buf && s_cap_len < s_cap_max)
            s_cap_buf[s_cap_len++] = c;
        else
            s_cap_drop = 1;
        return;
    }
    term_tx_push((uint8_t)c);
    if (c == '\n' || uart_term_pending() >= 48)
        term_pump();
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

/* 1 if Ctrl-C is the next key.  That byte is taken, and so is anything
 * queued behind it, so a sleep or a loop that was cancelled does not
 * then run whatever was typed after the Ctrl-C.  Any other key stays
 * in the ring for the prompt. */
int uart_take_ctrlc(void)
{
    uint32_t pm = irq_save();

    if (s_head == s_tail || s_rx[s_tail] != CTRL_C) {
        irq_restore(pm);
        return 0;
    }
    s_tail = s_head;
    irq_restore(pm);
    return 1;
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

int uart_getc_nb(void)
{
    return rx_pop();
}

int uart_is_raw(void)
{
    return s_raw_mode;
}

int uart_waiters(void)
{
    return s_waiters;
}

static int uart_wait(uint32_t ms, int timed, int honor_stop)
{
    uint32_t start = sys_ticks();
    int c;

    {
        uint32_t pm = irq_save();
        s_waiters++;
        irq_restore(pm);
    }
    for (;;) {
        c = rx_pop();
        if (c >= 0) break;
        term_pump();
        c = rx_pop();
        if (c >= 0) break;
        if (honor_stop && g_app.running && app_should_stop()) break;
        if (timed && (uint32_t)(sys_ticks() - start) >= ms) break;
        thread_yield();
        if (!timed) __wfi();
    }
    {
        uint32_t pm = irq_save();
        if (s_waiters) s_waiters--;
        irq_restore(pm);
    }
    return c;
}

int uart_getc(void)                     { return uart_wait(0, 0, 1); }
int uart_getc_timeout(uint32_t ms)      { return uart_wait(ms, 1, 1); }
int uart_getc_raw_timeout(uint32_t ms)  { return uart_wait(ms, 1, 0); }
