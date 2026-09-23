/*
 * Freya - SPI.
 *
 * The SD card has SPI1 to itself.  sdspi_* is that bus: the board wires
 * the pins and names the two dividers, because card identification has
 * to stay inside the 100-400 kHz window and the data phase then moves
 * up to BOARD_SPI_BR_FAST.
 *
 * A program gets a different controller.  spi_open() takes a bus number,
 * a speed and a mode and owns SCK, MISO and MOSI until spi_close().  The
 * transfer is polled, the way the card's is: the call returns when the
 * bytes are done.  There is no slave mode and no interrupt.  Chip select
 * is not one of these pins.  A device is selected by an ordinary pin the
 * program drives, so two devices can share the bus.
 *
 * The clock is a power-of-two division of the bus clock.  Of the eight
 * the hardware has, the one used is the fastest that does not exceed the
 * rate asked for.  SPI2 sits on APB1 on both boards, so that clock is
 * PCLK1.  Asking for 1 MHz lands on 750 kHz on the Black Pill and on
 * 562.5 kHz on the Blue Pill.
 */
#include "freya.h"

/* ------------------------------------------------------- the SD card */
#define CS_PORT     BOARD_SD_CS_PORT
#define CS_PIN      BOARD_SD_CS_PIN

void sdspi_init(void)
{
    board_spi_pins();

    SPI1->CR1 = 0;
#if BOARD_SPI_HAS_I2S
    SPI1->I2SCFGR = 0;                  /* SPI mode, not I2S */
#endif
    SPI1->CR2 = 0;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                (BOARD_SPI_BR_SLOW << SPI_CR1_BR_SHIFT);   /* mode 0 */
    SPI1->CR1 |= SPI_CR1_SPE;
}

void sdspi_set_speed(int fast)
{
    uint32_t br = fast ? (uint32_t)BOARD_SPI_BR_FAST : (uint32_t)BOARD_SPI_BR_SLOW;

    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 = (SPI1->CR1 & ~SPI_CR1_BR_MASK) | (br << SPI_CR1_BR_SHIFT);
    SPI1->CR1 |= SPI_CR1_SPE;
}

void sdspi_cs(int low)
{
    if (low) CS_PORT->BSRR = (1UL << (CS_PIN + 16));
    else     CS_PORT->BSRR = (1UL << CS_PIN);
}

uint8_t sdspi_xfer(uint8_t v)
{
    while (!(SPI1->SR & SPI_SR_TXE)) { }
    SPI1->DR = v;
    while (!(SPI1->SR & SPI_SR_RXNE)) { }
    return (uint8_t)(SPI1->DR & 0xFF);
}

void sdspi_write(const uint8_t *buf, uint32_t len)
{
    while (len--) (void)sdspi_xfer(*buf++);
}

void sdspi_read(uint8_t *buf, uint32_t len)
{
    while (len--) *buf++ = sdspi_xfer(0xFF);
}

/* --------------------------------------------------------- a program */
typedef struct {
    SPI_TypeDef *regs;
    uint8_t      apb;            /* 1 or 2: which PCLK the divider uses */
    uint8_t      sck;
    uint8_t      miso;
    uint8_t      mosi;
    uint8_t      af;
} spi_bus_t;

static const spi_bus_t s_spi_bus[] = BOARD_SPI_MAP;
static const char *const s_spi_name[] = BOARD_SPI_NAMES;

#define SPI_COUNT  ((int)ARRAY_SIZE(s_spi_bus))

_Static_assert(SPI_COUNT > 0, "a board needs an SPI bus");
_Static_assert(ARRAY_SIZE(s_spi_bus) == ARRAY_SIZE(s_spi_name),
               "BOARD_SPI_NAMES must name every bus");

typedef struct {
    uint32_t hz;                 /* the divider's rate, 0 when shut    */
    uint8_t  open;
    uint8_t  from_app;           /* a run's bus, dropped when it ends  */
    uint8_t  br;
    uint8_t  mode;
} spi_state_t;

static spi_state_t s_spi[SPI_COUNT];

/* The rate one BR field produces.  br is 0..7, so the divisor is
 * 2, 4, 8 ... 256 and the result is never a faster clock than pclk/2. */
static uint32_t spi_hz(uint32_t pclk, int br)
{
    return pclk / (2U << br);
}

/* The fastest of those eight that does not exceed hz, or -1 when even
 * the slowest one would.  hz is zero only when a caller skipped the
 * range check; pclk is zero before the clocks exist. */
static int spi_br(uint32_t pclk, uint32_t hz)
{
    if (hz == 0 || pclk < 2U) return -1;
    for (int br = 0; br <= 7; br++)
        if (spi_hz(pclk, br) <= hz) return br;
    return -1;
}

static uint32_t spi_pclk(const spi_bus_t *b)
{
    return (b->apb == 2) ? g_clocks.pclk2_hz : g_clocks.pclk1_hz;
}

static int spi_bus_index(int bus)
{
    if (bus < 1 || bus > SPI_COUNT) return -1;
    return bus - 1;
}

static int spi_caller_owns(int idx)
{
    return s_spi[idx].from_app == (uint8_t)(g_app.running ? 1 : 0);
}

/* How long a transfer may sit before the shifter is called dead.  The
 * budget is a little more than ten bit times at the rate the divider
 * actually runs, so a slow tap still finishes and a stopped peripheral
 * does not spin forever. */
static uint32_t spi_xfer_ms(uint32_t hz, int bytes)
{
    uint32_t ms = 10U + (uint32_t)bytes * 12000U / hz;
    return (ms > 1000U) ? 1000U : ms;
}

static int spi_wait_until(uint32_t deadline)
{
    if (app_should_stop()) return FREYA_ERR_IO;
    if ((int32_t)(sys_ticks() - deadline) >= 0) return FREYA_ERR_TIMEOUT;
    return 0;
}

static int spi_wait_set(SPI_TypeDef *spi, uint32_t flag, uint32_t deadline)
{
    while (!(spi->SR & flag)) {
        int rc = spi_wait_until(deadline);
        if (rc) return rc;
    }
    return 0;
}

static int spi_wait_clear(SPI_TypeDef *spi, uint32_t flag, uint32_t deadline)
{
    while (spi->SR & flag) {
        int rc = spi_wait_until(deadline);
        if (rc) return rc;
    }
    return 0;
}

static void spi_hw(SPI_TypeDef *regs, int br, int mode)
{
    uint32_t cr = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                  ((uint32_t)br << SPI_CR1_BR_SHIFT);

    /* Mode n is CPOL in bit 1 and CPHA in bit 0, which is also where
     * those two bits sit in CR1. */
    if (mode & 1) cr |= SPI_CR1_CPHA;
    if (mode & 2) cr |= SPI_CR1_CPOL;

    regs->CR1 = 0;
#if BOARD_SPI_HAS_I2S
    regs->I2SCFGR = 0;                  /* SPI mode, not I2S */
#endif
    regs->CR2 = 0;
    regs->CR1 = cr;
    regs->CR1 = cr | SPI_CR1_SPE;
    if (regs->SR & SPI_SR_RXNE) (void)regs->DR;
}

static void spi_pin_in(int pin)
{
    GPIO_TypeDef *port = board_gpio_port(FREYA_PIN_PORT(pin));

    if (port) board_pin_mode(port, FREYA_PIN_NUM(pin), FREYA_PIN_IN);
}

static int spi_pins_present(const spi_bus_t *b)
{
    if (!board_gpio_port(FREYA_PIN_PORT(b->sck))) return 0;
    if (!board_gpio_port(FREYA_PIN_PORT(b->miso))) return 0;
    if (!board_gpio_port(FREYA_PIN_PORT(b->mosi))) return 0;
    return 1;
}

static int spi_pins_taken(const spi_bus_t *b)
{
    int pins[3] = { b->sck, b->miso, b->mosi };

    for (int i = 0; i < 3; i++) {
        if (pwm_pin_busy(pins[i]) || i2c_owns_pin(pins[i]) ||
            w1_owns_pin(pins[i])) return 1;
    }
    return 0;
}

static void spi_bus_close(int idx)
{
    const spi_bus_t *b = &s_spi_bus[idx];

    if (!s_spi[idx].open) return;
    b->regs->CR1 &= ~SPI_CR1_SPE;
    spi_pin_in(b->sck);
    spi_pin_in(b->miso);
    spi_pin_in(b->mosi);
    s_spi[idx].open = 0;
    s_spi[idx].from_app = 0;
    s_spi[idx].hz = 0;
}

/* -------------------------------------------------------------- calls */
int spi_owns_pin(int pin)
{
    if (pin < 0 || pin > 0xFF) return 0;
    for (int i = 0; i < SPI_COUNT; i++) {
        if (!s_spi[i].open) continue;
        if (s_spi_bus[i].sck == (uint8_t)pin ||
            s_spi_bus[i].miso == (uint8_t)pin ||
            s_spi_bus[i].mosi == (uint8_t)pin) return 1;
    }
    return 0;
}

int spi_open(int bus, uint32_t hz, int mode)
{
    int idx = spi_bus_index(bus);
    int br;
    const spi_bus_t *b;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (idx < 0 || mode < FREYA_SPI_MODE0 || mode > FREYA_SPI_MODE3 ||
        hz < FREYA_SPI_MIN_HZ || hz > FREYA_SPI_MAX_HZ)
        return FREYA_ERR_ARG;
    if (s_spi[idx].open && !spi_caller_owns(idx)) return FREYA_ERR_BUSY;

    b = &s_spi_bus[idx];
    /* A pin already driving PWM, or held as I2C or 1-Wire, is not also
     * a clock or a data line. */
    if (spi_pins_taken(b)) return FREYA_ERR_BUSY;

    br = spi_br(spi_pclk(b), hz);
    if (br < 0) return FREYA_ERR_ARG;
    if (!spi_pins_present(b)) return FREYA_ERR_ARG;

    board_spi_mux(b->regs, b->sck, b->miso, b->mosi, b->af);
    spi_hw(b->regs, br, mode);
    s_spi[idx].br       = (uint8_t)br;
    s_spi[idx].mode     = (uint8_t)mode;
    s_spi[idx].hz       = spi_hz(spi_pclk(b), br);
    s_spi[idx].open     = 1;
    s_spi[idx].from_app = (uint8_t)(g_app.running ? 1 : 0);
    return 0;
}

int spi_close(int bus)
{
    int idx = spi_bus_index(bus);

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (idx < 0 || !s_spi[idx].open) return FREYA_ERR_ARG;
    if (!spi_caller_owns(idx)) return FREYA_ERR_BUSY;

    spi_bus_close(idx);
    return 0;
}

int spi_transfer(int bus, const void *tx, void *rx, int len)
{
    int idx = spi_bus_index(bus);
    const uint8_t *tb = tx;
    uint8_t *rb = rx;
    SPI_TypeDef *regs;
    uint32_t deadline;
    int rc = 0;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (idx < 0 || !s_spi[idx].open) return FREYA_ERR_ARG;
    if (len < 0 || len > FREYA_SPI_MAX_LEN) return FREYA_ERR_ARG;
    if (len && !tx && !rx) return FREYA_ERR_ARG;
    if (len == 0) return 0;

    regs = s_spi_bus[idx].regs;
    deadline = sys_ticks() + spi_xfer_ms(s_spi[idx].hz, len);
    for (int i = 0; i < len; i++) {
        uint8_t out = tb ? tb[i] : 0xFF;
        uint8_t in;

        rc = spi_wait_set(regs, SPI_SR_TXE, deadline);
        if (rc) break;
        regs->DR = out;
        rc = spi_wait_set(regs, SPI_SR_RXNE, deadline);
        if (rc) break;
        in = (uint8_t)(regs->DR & 0xFF);
        if (rb) rb[i] = in;
    }
    if (rc == 0) rc = spi_wait_clear(regs, SPI_SR_BSY, deadline);
    if (rc) spi_hw(regs, s_spi[idx].br, s_spi[idx].mode);
    return rc;
}

int spi_write(int bus, const void *buf, int len)
{
    return spi_transfer(bus, buf, NULL, len);
}

int spi_read(int bus, void *buf, int len)
{
    return spi_transfer(bus, NULL, buf, len);
}

void spi_release(void)
{
    for (int i = 0; i < SPI_COUNT; i++)
        if (s_spi[i].open && s_spi[i].from_app) spi_bus_close(i);
}

int spi_info(int idx, spi_info_t *info)
{
    if (idx < 0 || idx >= SPI_COUNT || !info) return -1;

    info->name = s_spi_name[idx];
    info->sck  = s_spi_bus[idx].sck;
    info->miso = s_spi_bus[idx].miso;
    info->mosi = s_spi_bus[idx].mosi;
    info->open = s_spi[idx].open;
    info->mode = s_spi[idx].open ? s_spi[idx].mode : 0;
    info->hz   = s_spi[idx].open ? s_spi[idx].hz : 0;
    return 0;
}
