/*
 * Freya - pin numbering, the timer period rule and the PWM one.
 *
 * Neither a pin nor a timer exists on the host, but the arithmetic
 * behind them does not need one: how a pin is packed into the integer a
 * program passes around, how a period in microseconds becomes the
 * prescaler and the reload a 16-bit timer can hold, and how a frequency
 * in hertz becomes the same pair.  Those two dividers are the part most
 * likely to be quietly wrong - a period off by a factor of two looks
 * like working code on the bench - so src/timer.c and src/pwm.c are
 * compiled here unchanged and driven over the whole range they accept,
 * at every clock the two boards can run at.
 *
 * The board's PWM map is checked beside them, because it is a table
 * written by hand: every pin in it has to be one a program may have, and
 * no two entries may name the same pin or the same timer channel.  The
 * I2C map is the same kind of table, and the half-period of its clock
 * is asked what frequency that delay would actually produce.  The SPI
 * divider is the same kind of arithmetic: of the eight power-of-two
 * taps, the one chosen has to be the fastest that does not exceed the
 * rate asked for.  The 1-Wire ROM search is the same kind of thing
 * that would be quietly wrong, so it is walked here against device
 * ids planted in place of a pin, and the CRC-8 those ids end with is
 * checked beside it.
 *
 * The kernel they expect around them is not here, so the few symbols
 * they refer to are defined below; nothing in the test calls the
 * register side of either driver.
 */
#include <stdio.h>
#include <string.h>

#include "freya.h"

/* The driver's own bookkeeping, which on a board belongs to the kernel. */
sys_clocks_t g_clocks;
app_state_t  g_app;
volatile uint32_t g_irq_events;

uint32_t sys_ticks(void) { return 0; }
void sys_delay_us(uint32_t us) { (void)us; }
int  app_in_handler(void) { return 0; }
int  app_should_stop(void) { return 0; }
int  app_handler_call(freya_irq_fn fn, int source, void *arg) { return 0; }

/* Masking interrupts is one instruction the host does not have; the
 * definitions in the board header go unused once these take their place. */
#define irq_save()      0u
#define irq_restore(pm) ((void)(pm))

/* Configuring a pin belongs to the board, and there is no board here. */
GPIO_TypeDef *board_gpio_port(int port) { return NULL; }
void board_pin_mode(GPIO_TypeDef *port, int pin, int mode) { }
void board_pin_af(GPIO_TypeDef *port, int pin, int af) { }
void board_spi_pins(void) { }
void board_spi_mux(SPI_TypeDef *spi, int sck, int miso, int mosi, int af)
{
    (void)spi; (void)sck; (void)miso; (void)mosi; (void)af;
}

#include "../src/timer.c"
#include "../src/pwm.c"
#include "../src/i2c.c"
#include "../src/w1.c"
#include "../src/spi.c"

static int checks, fails;

static void check(const char *what, long expected, long got)
{
    checks++;
    if (expected == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %ld, got %ld\n", what, expected, got);
        fails++;
    }
}

/*
 * What the hardware will actually do with a prescaler and a reload, in
 * microseconds: both count from zero, so the period is one more of each.
 */
static uint64_t period_of(uint32_t hz, uint16_t psc, uint16_t arr)
{
    uint64_t ticks = ((uint64_t)psc + 1) * ((uint64_t)arr + 1);

    return ticks * 1000000ULL / hz;
}

/* Every period the kernel accepts must come back as a period the timer
 * can be programmed with, and be the period that was asked for. */
static void sweep(uint32_t hz)
{
    uint32_t worst_us = 0;
    uint64_t worst_ppm = 0;
    uint32_t us;
    int bad_range = 0, bad_error = 0, bad_order = 0;
    uint64_t last = 0;

    for (us = FREYA_TIMER_MIN_US; us <= FREYA_TIMER_MAX_US;
         us += (us / 7) + 1) {
        uint16_t psc = 0xFFFF, arr = 0xFFFF;
        uint64_t got, err_ppm;

        if (timer_divide(hz, us, &psc, &arr) != 0) {
            bad_range++;
            continue;
        }
        got = period_of(hz, psc, arr);

        /* psc and arr are uint16_t, so the only way to leave the range
         * the hardware has is to have wrapped getting here. */
        if (got == 0) bad_range++;
        if (got < last) bad_order++;
        last = got;

        err_ppm = (got > us ? got - us : us - got) * 1000000ULL / us;
        if (err_ppm > worst_ppm) { worst_ppm = err_ppm; worst_us = us; }
        if (err_ppm > 2000) bad_error++;        /* 0.2 % */
    }

    printf("  --    %u MHz: worst error %u ppm at %u us\n",
           hz / 1000000U, (unsigned)worst_ppm, worst_us);
    check("every period in range is accepted", 0, bad_range);
    check("every period comes out within 0.2 %", 0, bad_error);
    check("a longer period never programs a shorter one", 0, bad_order);
}

/* What the hardware will make of a prescaler and a reload, in millihertz,
 * which is fine enough to tell 1 Hz apart from what misses it. */
static uint64_t freq_of(uint32_t hz, uint16_t psc, uint16_t arr)
{
    uint64_t ticks = ((uint64_t)psc + 1) * ((uint64_t)arr + 1);

    return (uint64_t)hz * 1000ULL / ticks;
}

/*
 * Every frequency the kernel accepts has to come back as one the timer
 * can be programmed with, and be near the one that was asked for.  How
 * near is set by the counts left in a period: a megahertz at 96 MHz is
 * 96 ticks, so the next reload up is a percent away and nothing can do
 * better.  Ten times that is the bound here, and the sweep reports what
 * it actually found.
 */
static void pwm_sweep(uint32_t hz)
{
    uint32_t worst_hz = 0, freq;
    uint64_t worst_ppm = 0;
    int bad_range = 0, bad_error = 0, bad_steps = 0;

    for (freq = FREYA_PWM_MIN_HZ; freq <= FREYA_PWM_MAX_HZ;
         freq += (freq / 7) + 1) {
        uint16_t psc = 0xFFFF, arr = 0xFFFF;
        uint64_t got, err_ppm;

        if (pwm_divide(hz, freq, &psc, &arr) != 0) {
            bad_range++;
            continue;
        }
        /* A reload of zero would be a pin with no duty cycle to set. */
        if (arr < 1) bad_steps++;

        got = freq_of(hz, psc, arr);
        err_ppm = (got > freq * 1000ULL ? got - freq * 1000ULL
                                        : freq * 1000ULL - got)
                  * 1000000ULL / (freq * 1000ULL);
        if (err_ppm > worst_ppm) { worst_ppm = err_ppm; worst_hz = freq; }
        if (err_ppm > 20000) bad_error++;       /* 2 % */
    }

    printf("  --    %u MHz: worst error %u ppm at %u Hz\n",
           hz / 1000000U, (unsigned)worst_ppm, worst_hz);
    check("every frequency in range is accepted", 0, bad_range);
    check("every one of them comes out within 2 %", 0, bad_error);
    check("and with at least two counts to divide a duty cycle into",
          0, bad_steps);
}

/* A frequency, like a period, is exact when the timer clock divides it
 * into a prescaler and a reload that both fit. */
static void pwm_within(uint32_t hz, uint32_t freq, unsigned ppm)
{
    char what[80];
    uint16_t psc = 0, arr = 0;
    uint64_t got, err;

    if (pwm_divide(hz, freq, &psc, &arr) != 0) {
        snprintf(what, sizeof(what), "%u Hz at %u MHz is accepted",
                 freq, hz / 1000000U);
        check(what, 1, 0);
        return;
    }
    got = freq_of(hz, psc, arr);
    err = (got > freq * 1000ULL ? got - freq * 1000ULL : freq * 1000ULL - got)
          * 1000000ULL / (freq * 1000ULL);

    snprintf(what, sizeof(what), "%u Hz at %u MHz is %s", freq,
             hz / 1000000U, ppm ? "close enough" : "exact");
    check(what, 1, err <= ppm);
}

/*
 * The board's PWM map is a hand written table, and the two mistakes it
 * invites are naming a pin the kernel keeps for the console or the card,
 * and naming one timer channel twice - which would be two pins fighting
 * over one compare register.
 */
static void pwm_map(void)
{
    static const uint16_t reserved[] = BOARD_PIN_RESERVED;
    int bad_pin = 0, bad_timer = 0, bad_ch = 0, dup = 0;

    for (int i = 0; i < PWM_COUNT; i++) {
        int port = FREYA_PIN_PORT(s_map[i].pin);
        int num  = FREYA_PIN_NUM(s_map[i].pin);

        if (port >= BOARD_PIN_PORTS || (reserved[port] & (1U << num)))
            bad_pin++;
        if (s_map[i].timer >= BOARD_TIMER_COUNT) bad_timer++;
        if (s_map[i].ch < 1 || s_map[i].ch > 4) bad_ch++;

        for (int j = 0; j < i; j++) {
            if (s_map[j].pin == s_map[i].pin) dup++;
            if (s_map[j].timer == s_map[i].timer &&
                s_map[j].ch == s_map[i].ch) dup++;
        }
    }

    printf("  --    %d PWM channels on this board\n", PWM_COUNT);
    check("no PWM pin is one the kernel keeps for itself", 0, bad_pin);
    check("every channel is on a timer the board lists", 0, bad_timer);
    check("and is one of that timer's four", 0, bad_ch);
    check("no pin and no timer channel appears twice", 0, dup);
    check("there is at least one channel to open", 1, PWM_COUNT > 0);
}

/* What the microsecond delay will actually clock the bus at.  Two half
 * periods are the cycle, and the half is rounded up, so this is the
 * asked-for rate or slower. */
static uint32_t i2c_got(uint32_t hz)
{
    return 1000000U / (2U * i2c_half_us(hz));
}

static void i2c_sweep(void)
{
    int bad = 0;

    for (uint32_t hz = FREYA_I2C_MIN_HZ; hz <= FREYA_I2C_MAX_HZ;
         hz += (hz / 7) + 1) {
        uint32_t half = (500000U + hz - 1U) / hz;
        uint32_t out;

        if (half == 0) half = 1;
        out = i2c_got(hz);
        if (i2c_half_us(hz) != half || out == 0 || out > hz) bad++;
    }
    check("every I2C speed in range is accepted and not run fast", 0, bad);
}

static void i2c_map(void)
{
    static const uint16_t reserved[] = BOARD_PIN_RESERVED;
    i2c_info_t a, b;
    int n = 0, bad = 0, dup = 0;

    while (i2c_info(n, &a) == 0) {
        int pins[2] = { a.scl, a.sda };

        if (a.scl == a.sda || !a.name) bad++;
        for (int k = 0; k < 2; k++) {
            int port = FREYA_PIN_PORT(pins[k]);
            int num  = FREYA_PIN_NUM(pins[k]);
            if (port >= BOARD_PIN_PORTS || (reserved[port] & (1U << num)))
                bad++;
        }
        for (int j = 0; j < n; j++) {
            i2c_info(j, &b);
            if (b.scl == a.scl || b.scl == a.sda ||
                b.sda == a.scl || b.sda == a.sda) dup++;
        }
        n++;
    }

    printf("  --    %d I2C buses on this board\n", n);
    check("this board has an I2C bus", 1, n > 0);
    check("bus 1 is SCL PB6 and SDA PB7", 1,
          i2c_info(0, &a) == 0 && a.scl == FREYA_PB(6) && a.sda == FREYA_PB(7));
    check("no I2C pin is one the kernel keeps", 0, bad);
    check("no I2C pin is shared between buses", 0, dup);
}

/* The tap is acceptable when it is the fastest one that does not exceed
 * the rate asked for.  A slower tap that also fits means a faster one
 * was left on the table. */
static int spi_tap_ok(uint32_t pclk, uint32_t hz)
{
    int br = spi_br(pclk, hz);
    uint32_t rate;

    if (br < 0 || br > 7) return 0;
    rate = spi_hz(pclk, br);
    if (rate == 0 || rate > hz) return 0;
    if (br > 0 && spi_hz(pclk, br - 1) <= hz) return 0;
    return 1;
}

static void spi_sweep(uint32_t pclk)
{
    char what[80];
    int bad = 0;

    for (uint32_t hz = FREYA_SPI_MIN_HZ; hz <= FREYA_SPI_MAX_HZ;
         hz += (hz / 7) + 1)
        if (!spi_tap_ok(pclk, hz)) bad++;
    snprintf(what, sizeof what,
             "every SPI speed at %u MHz is a tap that is not too fast",
             pclk / 1000000U);
    check(what, 0, bad);
}

static int pwm_has_pin(int pin)
{
    for (int i = 0; i < PWM_COUNT; i++)
        if (s_map[i].pin == pin) return 1;
    return 0;
}

static void spi_map(void)
{
    static const uint16_t reserved[] = BOARD_PIN_RESERVED;
    spi_info_t a, b;
    i2c_info_t bus;
    int n = 0, bad = 0, dup = 0, clash = 0;

    while (spi_info(n, &a) == 0) {
        int pins[3] = { a.sck, a.miso, a.mosi };

        if (a.sck == a.miso || a.sck == a.mosi || a.miso == a.mosi || !a.name)
            bad++;
        for (int k = 0; k < 3; k++) {
            int port = FREYA_PIN_PORT(pins[k]);
            int num  = FREYA_PIN_NUM(pins[k]);
            if (port >= BOARD_PIN_PORTS || (reserved[port] & (1U << num)))
                bad++;
            if (pwm_has_pin(pins[k])) clash++;
        }
        for (int j = 0; i2c_info(j, &bus) == 0; j++) {
            if (bus.scl == a.sck || bus.scl == a.miso || bus.scl == a.mosi ||
                bus.sda == a.sck || bus.sda == a.miso || bus.sda == a.mosi)
                clash++;
        }
        for (int j = 0; j < n; j++) {
            spi_info(j, &b);
            if (b.sck == a.sck || b.sck == a.miso || b.sck == a.mosi ||
                b.miso == a.sck || b.miso == a.miso || b.miso == a.mosi ||
                b.mosi == a.sck || b.mosi == a.miso || b.mosi == a.mosi)
                dup++;
        }
        n++;
    }

    printf("  --    %d SPI buses on this board\n", n);
    check("this board has an SPI bus", 1, n > 0);
    check("bus 1 is SCK PB13, MISO PB14, MOSI PB15", 1,
          spi_info(0, &a) == 0 && a.sck == FREYA_PB(13) &&
          a.miso == FREYA_PB(14) && a.mosi == FREYA_PB(15));
    check("no SPI pin is one the kernel keeps", 0, bad);
    check("no SPI pin is shared between buses", 0, dup);
    check("no SPI pin is also PWM or I2C", 0, clash);
}

/*
 * A period is exact when the timer clock divides it into a prescaler and
 * a reload that both fit; when it does not, what a program gets is the
 * closest the pair can come, and 'ppm' is how close that has to be.
 */
static void within(uint32_t hz, uint32_t us, unsigned ppm)
{
    char what[80];
    uint16_t psc = 0, arr = 0;
    uint64_t got, err;

    if (timer_divide(hz, us, &psc, &arr) != 0) {
        snprintf(what, sizeof(what), "%u us at %u MHz is accepted",
                 us, hz / 1000000U);
        check(what, 1, 0);
        return;
    }
    got = period_of(hz, psc, arr);
    err = (got > us ? got - us : us - got) * 1000000ULL / us;

    if (ppm == 0)
        snprintf(what, sizeof(what), "%u us at %u MHz is exact",
                 us, hz / 1000000U);
    else
        snprintf(what, sizeof(what), "%u us at %u MHz is within %u ppm "
                 "(it is %u)", us, hz / 1000000U, ppm, (unsigned)err);
    check(what, 1, err <= ppm);
}

static int rom_is(const uint8_t *got, const uint8_t *want)
{
    for (int i = 0; i < FREYA_W1_ROM_LEN; i++)
        if (got[i] != want[i]) return 0;
    return 1;
}

static void expect_rom(const char *what, const uint8_t *got, const uint8_t *want)
{
    if (!rom_is(got, want)) {
        printf("        got");
        for (int i = 0; i < FREYA_W1_ROM_LEN; i++) printf(" %02x", got[i]);
        printf("\n");
    }
    check(what, 1, rom_is(got, want));
}

static void close_pin(int pin)
{
    g_app.running = 0;
    w1_close(pin);
}

/* The walk, the CRC and the slot table.  A pin here is the host model,
 * which answers the bits a set of ROMs would. */
static void w1_cases(void)
{
    static const uint8_t crc_vec[] = {
        0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00
    };
    static const uint8_t devs[][FREYA_W1_ROM_LEN] = {
        { 0x28, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29 },
        { 0x28, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70 },
        { 0x28, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47 }
    };
    static const int pins[] = {
        FREYA_PB(12), FREYA_PB(13), FREYA_PB(14), FREYA_PB(15), FREYA_PA(0)
    };
    uint8_t got[FREYA_W1_ROM_LEN], with_crc[8];
    uint8_t bad[FREYA_W1_ROM_LEN];
    uint8_t cmd;
    w1_info_t info;
    int i;

    check("CRC-8 of the published vector is 0xA2",
          0xA2, w1_crc(crc_vec, 7));
    for (i = 0; i < 7; i++) with_crc[i] = crc_vec[i];
    with_crc[7] = 0xA2;
    check("and the vector plus that byte comes out 0", 0, w1_crc(with_crc, 8));
    check("an empty buffer's CRC is 0", 0, w1_crc(NULL, 0));
    check("a CRC with no bytes is refused", FREYA_ERR_ARG, w1_crc(NULL, 1));
    for (i = 0; i < 3; i++)
        check("each planted ROM includes its own CRC",
              0, w1_crc(devs[i], FREYA_W1_ROM_LEN));

    check("the console pins are not a 1-Wire pin",
          FREYA_ERR_PIN, w1_open(FREYA_PA(2)));
    check("nor the rest of the card", FREYA_ERR_PIN, w1_open(FREYA_PA(7)));
    check("nor a pin past the ports", FREYA_ERR_PIN, w1_open(FREYA_PIN(5, 0)));
    check("nor a negative pin", FREYA_ERR_PIN, w1_open(-1));

    w1_host_load(NULL, 0);
    check("an empty pin opens", 0, w1_open(FREYA_PB(12)));
    check("and a reset finds nobody", FREYA_ERR_NACK, w1_reset(FREYA_PB(12)));
    check("and a search finds nobody",
          FREYA_ERR_NACK, w1_search(FREYA_PB(12), got));
    check("a missing buffer is refused",
          FREYA_ERR_ARG, w1_write(FREYA_PB(12), NULL, 1));
    check("and so is a transfer past the maximum",
          FREYA_ERR_ARG, w1_read(FREYA_PB(12), got, FREYA_W1_MAX_LEN + 1));
    check("and so is a search with nowhere to put the ROM",
          FREYA_ERR_ARG, w1_search(FREYA_PB(12), NULL));
    check("a zero-length write is nothing", 0, w1_write(FREYA_PB(12), NULL, 0));
    close_pin(FREYA_PB(12));
    check("a pin that is not open cannot be reset",
          FREYA_ERR_ARG, w1_reset(FREYA_PB(12)));
    check("nor closed", FREYA_ERR_ARG, w1_close(FREYA_PB(12)));

    w1_host_load(devs, 3);
    check("the bus opens", 0, w1_open(FREYA_PB(12)));
    check("a reset finds the devices", 0, w1_reset(FREYA_PB(12)));
    check("the first ROM is the zero branch",
          0, w1_search(FREYA_PB(12), got));
    expect_rom("which is the id whose next bit is 0", got, devs[1]);
    check("the second ROM is the other branch of that bit",
          0, w1_search(FREYA_PB(12), got));
    expect_rom("which is the lower of the two that remain", got, devs[0]);
    check("the third ROM is the last device",
          0, w1_search(FREYA_PB(12), got));
    expect_rom("which is the id that took every one", got, devs[2]);
    check("the walk then ends", FREYA_ERR_NACK, w1_search(FREYA_PB(12), got));
    check("and the next call starts over", 0, w1_search(FREYA_PB(12), got));
    expect_rom("back at the zero branch", got, devs[1]);
    close_pin(FREYA_PB(12));

    for (i = 0; i < FREYA_W1_ROM_LEN; i++) bad[i] = devs[0][i];
    bad[7] = 0;
    w1_host_load(&bad, 1);
    check("a corrupt id still opens", 0, w1_open(FREYA_PB(12)));
    check("and the search refuses it", FREYA_ERR_IO, w1_search(FREYA_PB(12), got));
    check("and the next walk starts clean, and refuses it again",
          FREYA_ERR_IO, w1_search(FREYA_PB(12), got));
    close_pin(FREYA_PB(12));

    w1_host_load(&devs[0], 1);
    check("one device opens", 0, w1_open(FREYA_PB(12)));
    check("Read ROM starts with a reset", 0, w1_reset(FREYA_PB(12)));
    cmd = 0x33;
    check("and the command byte", 0, w1_write(FREYA_PB(12), &cmd, 1));
    check("and the eight bytes come back", 0, w1_read(FREYA_PB(12), got, 8));
    expect_rom("which are that device", got, devs[0]);
    check("the strong pull-up takes", 0, w1_pullup(FREYA_PB(12), 1));
    check("and the pin reports it",
          1, w1_info(0, &info) == 0 && info.pin == FREYA_PB(12) && info.pullup);
    check("a reset releases it", 0, w1_reset(FREYA_PB(12)));
    check("so the pin is open drain again",
          1, w1_info(0, &info) == 0 && !info.pullup);

    g_app.running = 1;
    check("a program cannot close the console's pin",
          FREYA_ERR_BUSY, w1_close(FREYA_PB(12)));
    check("nor reset it", FREYA_ERR_BUSY, w1_reset(FREYA_PB(12)));
    g_app.running = 0;
    close_pin(FREYA_PB(12));

    w1_host_load(devs, 1);
    for (i = 0; i < 4; i++)
        check("each of the four buses opens", 0, w1_open(pins[i]));
    check("the fifth is told they are full", FREYA_ERR_BUSY, w1_open(pins[4]));
    check("opening one that is already open does not take another",
          0, w1_open(pins[0]));
    check("so there are still four", -1, w1_info(4, &info));
    check("and the first is the first pin",
          pins[0], w1_info(0, &info) == 0 ? info.pin : -1);
    check("that pin is owned", 1, w1_owns_pin(pins[0]));
    check("a pin that was not opened is not", 0, w1_owns_pin(FREYA_PB(1)));
    g_app.running = 1;
    w1_release();
    check("a console bus survives the end of a run", 1, w1_owns_pin(pins[0]));
    g_app.running = 0;
    for (i = 0; i < 4; i++) close_pin(pins[i]);
    check("and once closed it is free", 0, w1_owns_pin(pins[0]));

    g_app.running = 1;
    check("a program can open a pin", 0, w1_open(FREYA_PB(12)));
    w1_release();
    check("and the end of the run closes it", 0, w1_owns_pin(FREYA_PB(12)));
    g_app.running = 0;
}

int main(void)
{
    const uint32_t f4 = 96000000U;      /* Black Pill, and its HSI fallback */
    const uint32_t f1 = 72000000U;      /* Blue Pill                        */
    const uint32_t f1_hsi = 64000000U;  /* Blue Pill without its crystal    */
    freya_api_t api;
    uint16_t psc, arr;

    /* ------------------------------------------------- pin numbering */
    check("a pin is its port and its number in one integer",
          FREYA_PB(7), FREYA_PIN(1, 7));
    check("the port comes back out", 2, FREYA_PIN_PORT(FREYA_PC(13)));
    check("the number comes back out", 13, FREYA_PIN_NUM(FREYA_PC(13)));
    check("PA0 is not a negative number, so an error is never a pin",
          0, FREYA_PA(0));
    check("the highest pin still fits a byte", 1, FREYA_PC(15) <= 0xFF ? 1 : 0);
    check("ports do not overlap", 1, FREYA_PB(0) > FREYA_PA(15) ? 1 : 0);
    check("the debounce flag is not an edge",
          0, FREYA_EDGE_DEBOUNCE & FREYA_EDGE_BOTH);
    check("both edges is rising and falling",
          FREYA_EDGE_BOTH, FREYA_EDGE_RISING | FREYA_EDGE_FALLING);

    /* --------------------------------------------- the timer divider */
    check("the timer clock is PCLK1 doubled when APB1 is divided",
          (long)f4, (long)(g_clocks.pclk1_hz = f4 / 2,
                           g_clocks.hclk_hz = f4, timer_clock_hz()));
    check("and PCLK1 itself when it is not",
          (long)f4, (long)(g_clocks.pclk1_hz = f4,
                           g_clocks.hclk_hz = f4, timer_clock_hz()));

    check("a period below the floor is refused",
          FREYA_ERR_ARG, timer_divide(f4, FREYA_TIMER_MIN_US - 1, &psc, &arr));
    check("a period of zero is refused",
          FREYA_ERR_ARG, timer_divide(f4, 0, &psc, &arr));
    check("a period past the ceiling is refused",
          FREYA_ERR_ARG, timer_divide(f4, FREYA_TIMER_MAX_US + 1, &psc, &arr));
    check("the floor itself is accepted", 0,
          timer_divide(f4, FREYA_TIMER_MIN_US, &psc, &arr));
    check("the ceiling itself is accepted", 0,
          timer_divide(f4, FREYA_TIMER_MAX_US, &psc, &arr));

    /* A period short enough to need no prescaler must not get one: that
     * is where the resolution a program asked for is either kept or lost. */
    timer_divide(f4, 100, &psc, &arr);
    check("100 us at 96 MHz needs no prescaler", 0, psc);
    check("and counts 9600 ticks", 9599, arr);

    /* The periods a program is most likely to ask for, to the
     * microsecond; a second is the one place where sixteen bits of each
     * cannot land on the number exactly, and it misses by a millionth. */
    within(f4, 1000, 0);
    within(f4, 20000, 0);
    within(f1, 1000, 0);
    within(f1_hsi, 1000, 0);
    within(f4, 1000000, 1);
    within(f1, 500000, 10);

    sweep(f4);
    sweep(f1);
    sweep(f1_hsi);

    /* ----------------------------------------------- the PWM divider */
    check("a frequency below the floor is refused",
          FREYA_ERR_ARG, pwm_divide(f4, FREYA_PWM_MIN_HZ - 1, &psc, &arr));
    check("a frequency past the ceiling is refused",
          FREYA_ERR_ARG, pwm_divide(f4, FREYA_PWM_MAX_HZ + 1, &psc, &arr));
    check("the floor itself is accepted", 0,
          pwm_divide(f4, FREYA_PWM_MIN_HZ, &psc, &arr));
    check("the ceiling itself is accepted", 0,
          pwm_divide(f4, FREYA_PWM_MAX_HZ, &psc, &arr));

    /* A frequency the counter can reach without a prescaler must not get
     * one: what the prescaler divides away is duty cycle resolution. */
    pwm_divide(f4, 2000, &psc, &arr);
    check("2 kHz at 96 MHz needs no prescaler", 0, psc);
    check("and counts 48000 ticks", 47999, arr);

    /* The three a program is most likely to ask for: a servo's frame, an
     * LED that cannot be seen to flicker, and a quiet motor. */
    pwm_within(f4, 50, 0);
    pwm_within(f1, 50, 10);
    pwm_within(f1_hsi, 50, 10);
    pwm_within(f4, 1000, 0);
    pwm_within(f1, 20000, 0);

    /* A servo is set by pulse width, which is worked out from the same
     * prescaler: at 50 Hz it has to land inside a microsecond of what
     * was asked for, or the arm sits visibly off. */
    pwm_divide(f4, 50, &psc, &arr);
    check("50 Hz at 96 MHz steps a pulse by 312 ns",
          312, (long)((uint64_t)((uint32_t)psc + 1) * 1000000000ULL / f4));
    check("and counts the 20 ms frame in 64000 steps", 63999, arr);

    pwm_sweep(f4);
    pwm_sweep(f1);
    pwm_sweep(f1_hsi);

    pwm_map();

    /* -------------------------------------------------- the I2C clock */
    check("a speed below the floor is refused",
          FREYA_ERR_ARG, i2c_open(1, FREYA_I2C_MIN_HZ - 1));
    check("and a speed above fast mode",
          FREYA_ERR_ARG, i2c_open(1, FREYA_I2C_MAX_HZ + 1));
    check("and a bus the board does not have",
          FREYA_ERR_ARG, i2c_open(0, 100000));
    check("100 kHz is one half-period of 5 us", 5, (long)i2c_half_us(100000));
    check("and that is 100 kHz", 100000, (long)i2c_got(100000));
    check("10 kHz is one half-period of 50 us", 50, (long)i2c_half_us(10000));
    check("400 kHz cannot be split into microseconds",
          2, (long)i2c_half_us(400000));
    check("so it is clocked at 250 kHz", 250000, (long)i2c_got(400000));
    i2c_sweep();
    i2c_map();

    /* -------------------------------------------------------- 1-Wire */
    w1_cases();

    /* ----------------------------------------------------------- SPI */
    check("a speed below the floor is refused",
          FREYA_ERR_ARG, spi_open(1, FREYA_SPI_MIN_HZ - 1, FREYA_SPI_MODE0));
    check("and a speed above the ceiling",
          FREYA_ERR_ARG, spi_open(1, FREYA_SPI_MAX_HZ + 1, FREYA_SPI_MODE0));
    check("and a mode the hardware does not have",
          FREYA_ERR_ARG, spi_open(1, 1000000, FREYA_SPI_MODE3 + 1));
    check("and a bus the board does not have",
          FREYA_ERR_ARG, spi_open(0, 1000000, FREYA_SPI_MODE0));
    check("24 MHz on a 48 MHz bus is the /2 tap",
          0, spi_br(48000000, 24000000));
    check("and that tap is 24 MHz", 24000000, (long)spi_hz(48000000, 0));
    check("just under 24 MHz steps down to /4",
          1, spi_br(48000000, 23999999));
    check("1 MHz on 48 MHz lands on 750 kHz",
          750000, (long)spi_hz(48000000, spi_br(48000000, 1000000)));
    check("187.5 kHz is the slowest tap of a 48 MHz bus",
          7, spi_br(48000000, FREYA_SPI_MIN_HZ));
    check("slower than that tap is refused",
          -1, spi_br(48000000, FREYA_SPI_MIN_HZ - 1));
    check("1 MHz on 36 MHz lands on 562.5 kHz",
          562500, (long)spi_hz(36000000, spi_br(36000000, 1000000)));
    check("24 MHz asked of a 36 MHz bus is its /2 tap",
          0, spi_br(36000000, FREYA_SPI_MAX_HZ));
    check("and that tap is 18 MHz", 18000000, (long)spi_hz(36000000, 0));
    check("the floor still has a tap on a 32 MHz bus",
          7, spi_br(32000000, FREYA_SPI_MIN_HZ));
    check("and that tap is 125 kHz", 125000, (long)spi_hz(32000000, 7));
    spi_sweep(48000000);
    spi_sweep(36000000);
    spi_sweep(32000000);
    spi_map();

    /* ------------------------------------------- the table a program sees */
    memset(&api, 0, sizeof(api));
    api.size = sizeof(freya_api_t);
    check("a full table has the pin calls", 1, FREYA_API_HAS(&api, pin_mode) ? 1 : 0);
    check("and the timer calls", 1, FREYA_API_HAS(&api, timer_open) ? 1 : 0);
    check("and the PWM calls", 1, FREYA_API_HAS(&api, pwm_freq) ? 1 : 0);
    check("and the I2C calls", 1, FREYA_API_HAS(&api, i2c_transfer) ? 1 : 0);
    check("and the 1-Wire calls", 1, FREYA_API_HAS(&api, w1_crc) ? 1 : 0);
    check("and the SPI calls", 1, FREYA_API_HAS(&api, spi_transfer) ? 1 : 0);
    check("and the crypt calls", 1, FREYA_API_HAS(&api, crypt) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, exit_reason_str) +
               sizeof(api.exit_reason_str);
    check("a kernel from before them says so", 0,
          FREYA_API_HAS(&api, pin_mode) ? 1 : 0);
    check("including for irq_wait, the last of them", 0,
          FREYA_API_HAS(&api, irq_wait) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, irq_wait) + sizeof(api.irq_wait);
    check("a kernel with the interrupts but not PWM says that too", 0,
          FREYA_API_HAS(&api, pwm_open) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, pwm_freq) + sizeof(api.pwm_freq);
    check("a kernel with PWM but not I2C says that too", 0,
          FREYA_API_HAS(&api, i2c_open) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, i2c_transfer) +
               sizeof(api.i2c_transfer);
    check("a kernel with I2C but not 1-Wire says that too", 0,
          FREYA_API_HAS(&api, w1_open) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, thread_self) +
               sizeof(api.thread_self);
    check("a kernel with threads but not SPI says that too", 0,
          FREYA_API_HAS(&api, spi_open) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, spi_read) + sizeof(api.spi_read);
    check("a kernel with SPI but not crypt says that too", 0,
          FREYA_API_HAS(&api, crypt) ? 1 : 0);

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
