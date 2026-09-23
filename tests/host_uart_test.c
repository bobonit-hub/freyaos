/*
 * Freya - the console's Ctrl-C rule.
 *
 * A running program is stopped by Ctrl-C, unless it asked for a raw
 * console with api->console_raw(1): then 0x03 is a key like any other
 * and reaches the program through getc().  That decision is made in the
 * USART receive interrupt, so src/uart.c is compiled here unchanged
 * against a USART that is a plain struct, and each case feeds the ISR
 * one byte.
 *
 * The receive loop reads SR at the top, SR again as its condition, DR
 * for the byte, and SR once more to find the FIFO empty.  The fake
 * register block is handed out through a function that counts those
 * accesses, so it can report one byte waiting for the first three and
 * none after that.
 */
#include <stdio.h>
#include <string.h>

#include "freya.h"

sys_clocks_t g_clocks;
app_state_t  g_app;

static int s_stops, s_kills;

uint32_t sys_ticks(void) { return 0; }
void thread_yield(void) { }
void board_uart_pins(void) { }
void app_request_stop(void) { s_stops++; }
int  app_handler_kill(uint32_t *frame) { (void)frame; s_kills++; return 0; }
int  app_should_stop(void) { return s_stops != 0; }

#define irq_save()      0u
#define irq_restore(pm) ((void)(pm))
#define __wfi()         ((void)0)

static USART_TypeDef s_fake;
static int           s_access;

static USART_TypeDef *fake_usart(void)
{
    s_fake.SR = s_access++ < 3 ? USART_SR_RXNE : 0;
    return &s_fake;
}

#undef  USART2
#define USART2 (fake_usart())

#include "../src/uart.c"

static int checks, fails;

static void check(const char *what, int expected, int got)
{
    checks++;
    if (expected == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %d, got %d\n", what, expected, got);
        fails++;
    }
}

static void receive(uint8_t c)
{
    s_access = 0;
    s_fake.DR = c;
    usart2_interrupt(NULL);
}

static void reset(int running, int raw)
{
    s_head = s_tail = 0;
    s_stops = s_kills = 0;
    g_app.running = running;
    uart_set_raw(raw);
}

int main(void)
{
    reset(1, 0);
    receive('A');
    check("a key reaches a running program", 'A', uart_getc_nb());
    check("and stops nothing", 0, s_stops);

    reset(1, 0);
    receive(0x03);
    check("Ctrl-C stops a running program", 1, s_stops);
    check("and looks for a looping handler to kill", 1, s_kills);
    check("and is not queued as a key", -1, uart_getc_nb());

    reset(1, 1);
    check("raw mode reads back as on", 1, uart_is_raw());
    receive(0x03);
    check("in raw mode Ctrl-C stops nothing", 0, s_stops);
    check("and kills no handler", 0, s_kills);
    check("and arrives as the key 0x03", 0x03, uart_getc_nb());
    receive('B');
    check("other keys still arrive in raw mode", 'B', uart_getc_nb());

    reset(0, 0);
    receive(0x03);
    check("with no program running Ctrl-C is the shell's key", 0x03,
          uart_getc_nb());
    check("and stops nothing", 0, s_stops);

    uart_set_raw(0);
    check("raw mode reads back as off", 0, uart_is_raw());

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
