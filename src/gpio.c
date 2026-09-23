/*
 * Freya - pins, and interrupts on them.
 *
 * A program names a pin as a port and a number in one integer
 * (FREYA_PB(0)), drives it through pin_mode / pin_read / pin_write, and
 * asks for an interrupt on an edge with pin_irq_attach().
 *
 * The hardware gives sixteen external interrupt lines, not one per pin:
 * line n is pin n of exactly one port, so PA0, PB0 and PC0 compete for
 * EXTI0 and the second attach is refused.  Lines 0..4 have an interrupt
 * each, 5..9 share one and 10..15 share another, which is why the two
 * group handlers walk their range.
 *
 * Everything chip specific - how a pin is configured, and which port a
 * line listens to - is a board hook; EXTI itself is the same register
 * block on every STM32 Freya runs on.
 */
#include "freya.h"

#define EXTI_LINES  16

typedef struct {
    freya_irq_fn fn;
    void    *arg;
    uint32_t count;
    uint32_t last_ms;        /* when the last edge was taken            */
    uint8_t  port;           /* which port owns the line                */
    uint8_t  edge;           /* FREYA_EDGE_*, zero when the line is free */
} exti_line_t;

static exti_line_t s_line[EXTI_LINES];

static const uint16_t s_reserved[] = BOARD_PIN_RESERVED;

_Static_assert(ARRAY_SIZE(s_reserved) == BOARD_PIN_PORTS,
               "BOARD_PIN_RESERVED needs one mask per port");

/* ------------------------------------------------------------- pins */
/*
 * A pin number becomes a port and a bit here, or it becomes an error.
 * This is the only place that decides whether a program may touch a pin
 * at all: the console and the card are refused, everything else is
 * handed over.
 */
static int pin_resolve(int pin, GPIO_TypeDef **port, int *bit)
{
    int p = FREYA_PIN_PORT(pin);
    int n = FREYA_PIN_NUM(pin);

    if (pin < 0 || pin > 0xFF || p >= BOARD_PIN_PORTS) return FREYA_ERR_PIN;
    if (s_reserved[p] & (1U << n)) return FREYA_ERR_PIN;

    *port = board_gpio_port(p);
    if (!*port) return FREYA_ERR_PIN;
    *bit = n;
    return 0;
}

int gpio_pin_mode(int pin, int mode)
{
    GPIO_TypeDef *port;
    int bit, rc;

    if (mode < FREYA_PIN_IN || mode > FREYA_PIN_ANALOG) return FREYA_ERR_ARG;
    rc = pin_resolve(pin, &port, &bit);
    if (rc != 0) return rc;

    board_pin_mode(port, bit, mode);
    return 0;
}

int gpio_pin_read(int pin)
{
    GPIO_TypeDef *port;
    int bit, rc;

    rc = pin_resolve(pin, &port, &bit);
    if (rc != 0) return rc;
    return (port->IDR >> bit) & 1U;
}

/* BSRR sets in the low half and clears in the high one, so a write - and
 * a toggle - is a single store that no interrupt can land inside. */
int gpio_pin_write(int pin, int value)
{
    GPIO_TypeDef *port;
    int bit, rc;

    rc = pin_resolve(pin, &port, &bit);
    if (rc != 0) return rc;

    port->BSRR = 1UL << (value ? bit : bit + 16);
    return 0;
}

int gpio_pin_toggle(int pin)
{
    GPIO_TypeDef *port;
    int bit, rc;

    rc = pin_resolve(pin, &port, &bit);
    if (rc != 0) return rc;

    port->BSRR = 1UL << ((port->ODR & (1UL << bit)) ? bit + 16 : bit);
    return 0;
}

/* ------------------------------------------------------ pin interrupts */
static int line_irq(int line)
{
    static const uint8_t first[] = {
        EXTI0_IRQn, EXTI1_IRQn, EXTI2_IRQn, EXTI3_IRQn, EXTI4_IRQn
    };

    if (line < (int)ARRAY_SIZE(first)) return first[line];
    return (line < 10) ? EXTI9_5_IRQn : EXTI15_10_IRQn;
}

/*
 * Arm a line for an edge, or - with no edge at all - silence it.  A line
 * masked in IMR raises nothing, so the two shared interrupts can stay
 * enabled with no source behind them.
 */
static void line_edges(int line, int edge)
{
    uint32_t bit = 1UL << line;

    if (edge & FREYA_EDGE_RISING)  EXTI->RTSR |= bit;
    else                           EXTI->RTSR &= ~bit;
    if (edge & FREYA_EDGE_FALLING) EXTI->FTSR |= bit;
    else                           EXTI->FTSR &= ~bit;
    EXTI->PR = bit;                     /* drop an edge seen while arming */
    if (edge) EXTI->IMR |=  bit;
    else      EXTI->IMR &= ~bit;
}

/* Give a line back: silenced in the hardware and free in the table. */
static void line_free(int line)
{
    line_edges(line, 0);
    s_line[line].edge = 0;
    s_line[line].fn = NULL;
    s_line[line].count = 0;
}

int gpio_irq_attach(int pin, int edge, freya_irq_fn fn, void *arg)
{
    GPIO_TypeDef *port;
    exti_line_t *l;
    uint32_t pm;
    int line, rc;

    if (!(edge & FREYA_EDGE_BOTH) ||
        (edge & ~(FREYA_EDGE_BOTH | FREYA_EDGE_DEBOUNCE))) return FREYA_ERR_ARG;
    if (app_in_handler()) return FREYA_ERR_HANDLER;
    rc = pin_resolve(pin, &port, &line);
    if (rc != 0) return rc;

    l = &s_line[line];
    if (l->edge && l->port != (uint8_t)FREYA_PIN_PORT(pin))
        return FREYA_ERR_BUSY;          /* another port already has line n */

    pm = irq_save();

    l->fn      = fn;
    l->arg     = arg;
    l->count   = 0;
    l->last_ms = 0;
    l->port    = (uint8_t)FREYA_PIN_PORT(pin);
    l->edge    = (uint8_t)edge;

    board_exti_select(l->port, line);
    line_edges(line, edge);

    irq_restore(pm);

    nvic_set_priority(line_irq(line), IRQ_PRIO_HANDLER);
    nvic_enable(line_irq(line));
    return 0;
}

/*
 * The line this pin has an interrupt on, or -1 when it has none.  The
 * ownership matters: PA0 and PB0 share line 0, so asking about one of
 * them must not read - or take away - the interrupt the other attached.
 */
static int pin_line(int pin)
{
    GPIO_TypeDef *port;
    int line;

    if (pin_resolve(pin, &port, &line) != 0) return -1;
    if (!s_line[line].edge || s_line[line].port != (uint8_t)FREYA_PIN_PORT(pin))
        return -1;
    return line;
}

int gpio_irq_detach(int pin)
{
    uint32_t pm;
    int line;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    line = pin_line(pin);
    if (line < 0) return FREYA_ERR_ARG;

    pm = irq_save();
    line_free(line);
    irq_restore(pm);
    return 0;
}

uint32_t gpio_irq_count(int pin)
{
    int line = pin_line(pin);

    return (line < 0) ? 0 : s_line[line].count;
}

int gpio_irq_owns_pin(int pin)
{
    return pin_line(pin) >= 0;
}

void gpio_irq_release(void)
{
    uint32_t pm = irq_save();

    for (int line = 0; line < EXTI_LINES; line++) {
        if (s_line[line].edge) line_free(line);
        if (line < 5 || line == 9 || line == 15) nvic_disable(line_irq(line));
    }
    irq_restore(pm);
}

/*
 * One edge.  The pending bit is cleared before the handler runs, so an
 * edge that arrives while it is running is not lost; a handler that is
 * killed (a fault, or a Ctrl-C that found it looping) leaves the run
 * ending, and the line is silenced here rather than firing again into a
 * program that no longer exists.
 */
static void line_event(int line)
{
    exti_line_t *l = &s_line[line];
    uint32_t now;

    if (!l->edge || !g_app.running || app_should_stop()) {
        line_edges(line, 0);
        return;
    }

    now = sys_ticks();
    if ((l->edge & FREYA_EDGE_DEBOUNCE) && l->count &&
        (uint32_t)(now - l->last_ms) < FREYA_DEBOUNCE_MS)
        return;                         /* still the same press bouncing */

    l->last_ms = now;
    l->count++;
    g_irq_events++;

    if (l->fn) app_handler_call(l->fn, FREYA_PIN(l->port, line), l->arg);
}

static void exti_dispatch(int first, int last)
{
    for (int line = first; line <= last; line++) {
        uint32_t bit = 1UL << line;

        if (!(EXTI->PR & bit)) continue;
        EXTI->PR = bit;                 /* write 1 to clear */
        line_event(line);
    }
}

void EXTI0_IRQHandler(void)     { exti_dispatch(0, 0); }
void EXTI1_IRQHandler(void)     { exti_dispatch(1, 1); }
void EXTI2_IRQHandler(void)     { exti_dispatch(2, 2); }
void EXTI3_IRQHandler(void)     { exti_dispatch(3, 3); }
void EXTI4_IRQHandler(void)     { exti_dispatch(4, 4); }
void EXTI9_5_IRQHandler(void)   { exti_dispatch(5, 9); }
void EXTI15_10_IRQHandler(void) { exti_dispatch(10, 15); }
