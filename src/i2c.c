/*
 * Freya - I2C master.
 *
 * i2c_open() takes a bus number and a speed and owns those two pins
 * until i2c_close().  A transfer is polled, the way SPI is: the call
 * returns when the bytes are done, or when the bus has had long enough
 * and did not answer.  There is no interrupt and no slave mode.
 *
 * Addresses are 7-bit.  A write followed by a read is one call, because
 * that is how a device register is read and the repeated start between
 * the two belongs inside the call - a stop in the middle would let the
 * device drop the register it had just been pointed at.
 *
 * The lines are open-drain GPIO, not the on-chip I2C block.  The two
 * chips do not share that block, and the older one's master sequence
 * does not fit in what the Blue Pill has left of its kernel flash.  GPIO
 * is the same code on both.  A slave that holds the clock down is waited
 * on, so stretching still works.  The delay is a whole number of
 * microseconds, rounded up, so the bus is the speed that was asked for
 * or a little slower - at 400 kHz a microsecond is too coarse and the
 * clock lands at 250.
 *
 * Which pins a bus uses is the board's table.  Bus 1 is PB6 and PB7 on
 * every board.  The lines need pull-ups to 3.3 V.  The F4 enables its
 * weak internal ones on an open-drain output, which are enough to see a
 * single device up close and not enough to call a bus; the F1 cannot
 * enable them on an output.
 */
#include "freya.h"

typedef struct {
    uint8_t scl;
    uint8_t sda;
} i2c_bus_t;

static const i2c_bus_t s_i2c_bus[] = BOARD_I2C_MAP;
static const char *const s_i2c_name[] = BOARD_I2C_NAMES;

#define I2C_COUNT  ((int)ARRAY_SIZE(s_i2c_bus))

_Static_assert(I2C_COUNT > 0, "a board needs an I2C bus");
_Static_assert(ARRAY_SIZE(s_i2c_bus) == ARRAY_SIZE(s_i2c_name),
               "BOARD_I2C_NAMES must name every bus");

typedef struct {
    GPIO_TypeDef *sp;
    GPIO_TypeDef *dp;
    uint32_t      hz;
    uint32_t      half;          /* microseconds each half of SCL is held */
    uint32_t      sb;
    uint32_t      db;
    uint8_t       open;
    uint8_t       from_app;      /* a run's bus, dropped when it ends     */
} i2c_state_t;

static i2c_state_t s_i2c[I2C_COUNT];

/* -------------------------------------------------------------- time */
/*
 * Half of one SCL cycle, in microseconds, rounded up.  Two of them are
 * the period, so the result is never a faster bus than the one asked
 * for.  hz is a speed i2c_open() has already accepted, which keeps this
 * off zero.
 */
static uint32_t i2c_half_us(uint32_t hz)
{
    uint32_t us = (500000U + hz - 1U) / hz;
    return us ? us : 1U;
}

static int bus_index(int bus)
{
    if (bus < 1 || bus > I2C_COUNT) return -1;
    return bus - 1;
}

static int caller_owns(int idx)
{
    return s_i2c[idx].from_app == (uint8_t)(g_app.running ? 1 : 0);
}

/* How long a transfer may sit before the bus is called dead.  The
 * floor covers a slave that stretches the clock for a conversion; a
 * scan that gets NACKs returns at once and never spends it. */
static uint32_t xfer_ms(uint32_t hz, int bytes)
{
    uint32_t ms = 15U + (uint32_t)(bytes + 2) * 10000U / hz;
    return (ms > 1000U) ? 1000U : ms;
}

static int wait_until(uint32_t deadline)
{
    if (app_should_stop()) return FREYA_ERR_IO;
    if ((int32_t)(sys_ticks() - deadline) >= 0) return FREYA_ERR_TIMEOUT;
    return 0;
}

/* -------------------------------------------------------------- wires */
static void line_hi(GPIO_TypeDef *p, uint32_t bit) { p->BSRR = bit; }
static void line_lo(GPIO_TypeDef *p, uint32_t bit) { p->BSRR = bit << 16; }
static int  line_up(GPIO_TypeDef *p, uint32_t bit)
{
    return (p->IDR & bit) != 0;
}

/* Let the clock rise, and wait if a slave is holding it.  The high time
 * is the same half-period the low time gets, so the duty stays near half. */
static int scl_hi(i2c_state_t *s, uint32_t deadline)
{
    line_hi(s->sp, s->sb);
    while (!line_up(s->sp, s->sb)) {
        int rc = wait_until(deadline);
        if (rc) return rc;
    }
    sys_delay_us(s->half);
    return 0;
}

static int bit_wr(i2c_state_t *s, int one, uint32_t deadline)
{
    int rc;

    if (one) line_hi(s->dp, s->db);
    else     line_lo(s->dp, s->db);
    sys_delay_us(s->half);
    rc = scl_hi(s, deadline);
    line_lo(s->sp, s->sb);
    return rc;
}

static int bit_rd(i2c_state_t *s, uint32_t deadline)
{
    int rc, b;

    line_hi(s->dp, s->db);
    sys_delay_us(s->half);
    rc = scl_hi(s, deadline);
    b  = line_up(s->dp, s->db);
    line_lo(s->sp, s->sb);
    return rc ? rc : b;
}

static int byte_wr(i2c_state_t *s, int byte, uint32_t deadline)
{
    int ack;

    for (int i = 0; i < 8; i++) {
        int rc = bit_wr(s, (byte & 0x80) != 0, deadline);
        if (rc) return rc;
        byte <<= 1;
    }
    ack = bit_rd(s, deadline);
    if (ack < 0) return ack;
    return ack ? FREYA_ERR_NACK : 0;
}

static int byte_rd(i2c_state_t *s, int ack, uint8_t *out, uint32_t deadline)
{
    int v = 0, rc;

    for (int i = 0; i < 8; i++) {
        int b = bit_rd(s, deadline);
        if (b < 0) return b;
        v = (v << 1) | b;
    }
    rc = bit_wr(s, ack ? 0 : 1, deadline);
    if (rc) return rc;
    *out = (uint8_t)v;
    return 0;
}

static int bus_start(i2c_state_t *s, uint32_t deadline)
{
    int rc;

    line_hi(s->dp, s->db);
    sys_delay_us(s->half);
    rc = scl_hi(s, deadline);
    if (rc) return rc;
    line_lo(s->dp, s->db);
    sys_delay_us(s->half);
    line_lo(s->sp, s->sb);
    return 0;
}

static int bus_stop(i2c_state_t *s, uint32_t deadline)
{
    int rc;

    line_lo(s->dp, s->db);
    sys_delay_us(s->half);
    rc = scl_hi(s, deadline);
    line_hi(s->dp, s->db);
    sys_delay_us(s->half);
    return rc;
}

/* Nine clocks finish a byte a slave was in the middle of, and the edges
 * after them are a stop.  A stuck clock is left alone: pulsing data at
 * it does not help, and the next transfer will time out the same way. */
static void bus_kick(i2c_state_t *s)
{
    line_hi(s->dp, s->db);
    for (int i = 0; i < 9; i++) {
        line_lo(s->sp, s->sb);
        sys_delay_us(5);
        line_hi(s->sp, s->sb);
        sys_delay_us(5);
        if (line_up(s->dp, s->db)) break;
    }
    line_lo(s->dp, s->db);
    sys_delay_us(5);
    line_hi(s->sp, s->sb);
    sys_delay_us(5);
    line_hi(s->dp, s->db);
}

static void pins_od(i2c_state_t *s, const i2c_bus_t *b)
{
    int sn = FREYA_PIN_NUM(b->scl);
    int dn = FREYA_PIN_NUM(b->sda);

    s->sb = 1UL << sn;
    s->db = 1UL << dn;
    board_pin_mode(s->sp, sn, FREYA_PIN_OUT_OD);
    board_pin_mode(s->dp, dn, FREYA_PIN_OUT_OD);
    line_hi(s->sp, s->sb);
    line_hi(s->dp, s->db);
}

static void pins_in(const i2c_bus_t *b, i2c_state_t *s)
{
    board_pin_mode(s->sp, FREYA_PIN_NUM(b->scl), FREYA_PIN_IN);
    board_pin_mode(s->dp, FREYA_PIN_NUM(b->sda), FREYA_PIN_IN);
}

static int xfer(int idx, int addr, const void *tx, int txlen,
                void *rx, int rxlen, uint32_t deadline)
{
    const uint8_t *tb = tx;
    uint8_t *rb = rx;
    i2c_state_t *s = &s_i2c[idx];
    int rc;

    if (txlen || !rxlen) {
        rc = bus_start(s, deadline);
        if (rc) return rc;
        rc = byte_wr(s, (addr << 1) & 0xFE, deadline);
        if (rc) goto nack;
        for (int i = 0; i < txlen; i++) {
            rc = byte_wr(s, tb[i], deadline);
            if (rc) goto nack;
        }
        if (!rxlen) return bus_stop(s, deadline);
    }

    rc = bus_start(s, deadline);
    if (rc) return rc;
    rc = byte_wr(s, ((addr << 1) & 0xFE) | 1, deadline);
    if (rc) goto nack;
    for (int i = 0; i < rxlen; i++) {
        rc = byte_rd(s, i + 1 < rxlen, &rb[i], deadline);
        if (rc) return rc;
    }
    return bus_stop(s, deadline);

nack:
    if (rc == FREYA_ERR_NACK) {
        int stop = bus_stop(s, deadline);
        if (stop) return stop;
    }
    return rc;
}

static void bus_close(int idx)
{
    if (!s_i2c[idx].open) return;
    pins_in(&s_i2c_bus[idx], &s_i2c[idx]);
    s_i2c[idx].open = 0;
    s_i2c[idx].from_app = 0;
    s_i2c[idx].hz = 0;
}

/* -------------------------------------------------------------- calls */
int i2c_owns_pin(int pin)
{
    if (pin < 0 || pin > 0xFF) return 0;
    for (int i = 0; i < I2C_COUNT; i++) {
        if (!s_i2c[i].open) continue;
        if (s_i2c_bus[i].scl == (uint8_t)pin ||
            s_i2c_bus[i].sda == (uint8_t)pin) return 1;
    }
    return 0;
}

int i2c_open(int bus, uint32_t hz)
{
    int idx = bus_index(bus);
    GPIO_TypeDef *sp, *dp;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (idx < 0 || hz < FREYA_I2C_MIN_HZ || hz > FREYA_I2C_MAX_HZ)
        return FREYA_ERR_ARG;
    if (s_i2c[idx].open && !caller_owns(idx)) return FREYA_ERR_BUSY;
    /* A pin already driving PWM, or held as SPI or 1-Wire, is not also
     * a clock or a data line. */
    if (pwm_pin_busy(s_i2c_bus[idx].scl) || pwm_pin_busy(s_i2c_bus[idx].sda) ||
        spi_owns_pin(s_i2c_bus[idx].scl) || spi_owns_pin(s_i2c_bus[idx].sda) ||
        w1_owns_pin(s_i2c_bus[idx].scl) || w1_owns_pin(s_i2c_bus[idx].sda))
        return FREYA_ERR_BUSY;

    sp = board_gpio_port(FREYA_PIN_PORT(s_i2c_bus[idx].scl));
    dp = board_gpio_port(FREYA_PIN_PORT(s_i2c_bus[idx].sda));
    if (!sp || !dp) return FREYA_ERR_ARG;

    s_i2c[idx].sp   = sp;
    s_i2c[idx].dp   = dp;
    s_i2c[idx].hz   = hz;
    s_i2c[idx].half = i2c_half_us(hz);
    pins_od(&s_i2c[idx], &s_i2c_bus[idx]);
    bus_kick(&s_i2c[idx]);
    s_i2c[idx].open     = 1;
    s_i2c[idx].from_app = (uint8_t)(g_app.running ? 1 : 0);
    return 0;
}

int i2c_close(int bus)
{
    int idx = bus_index(bus);

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (idx < 0 || !s_i2c[idx].open) return FREYA_ERR_ARG;
    if (!caller_owns(idx)) return FREYA_ERR_BUSY;

    bus_close(idx);
    return 0;
}

int i2c_transfer(int bus, int addr, const void *tx, int txlen,
                 void *rx, int rxlen)
{
    int idx = bus_index(bus);
    int rc;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (idx < 0 || !s_i2c[idx].open) return FREYA_ERR_ARG;
    if (addr < 0 || addr > 0x7F) return FREYA_ERR_ARG;
    if (txlen < 0 || rxlen < 0 ||
        txlen > FREYA_I2C_MAX_LEN || rxlen > FREYA_I2C_MAX_LEN)
        return FREYA_ERR_ARG;
    if ((txlen && !tx) || (rxlen && !rx)) return FREYA_ERR_ARG;

    rc = xfer(idx, addr, tx, txlen, rx, rxlen,
              sys_ticks() + xfer_ms(s_i2c[idx].hz, txlen + rxlen));
    if (rc == FREYA_ERR_TIMEOUT || rc == FREYA_ERR_IO) bus_kick(&s_i2c[idx]);
    return rc;
}

int i2c_write(int bus, int addr, const void *buf, int len)
{
    return i2c_transfer(bus, addr, buf, len, NULL, 0);
}

int i2c_read(int bus, int addr, void *buf, int len)
{
    return i2c_transfer(bus, addr, NULL, 0, buf, len);
}

void i2c_release(void)
{
    for (int i = 0; i < I2C_COUNT; i++)
        if (s_i2c[i].open && s_i2c[i].from_app) bus_close(i);
}

int i2c_info(int idx, i2c_info_t *info)
{
    if (idx < 0 || idx >= I2C_COUNT || !info) return -1;

    info->name = s_i2c_name[idx];
    info->scl  = s_i2c_bus[idx].scl;
    info->sda  = s_i2c_bus[idx].sda;
    info->open = s_i2c[idx].open;
    info->hz   = s_i2c[idx].open ? s_i2c[idx].hz : 0;
    return 0;
}
