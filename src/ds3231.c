/*
 * Freya - DS3231 real time clock.
 *
 * Built only when make is given RTC=ds3231.  The chip is battery backed.
 * Freya still counts civil time from SysTick; this driver copies the chip
 * into that count at boot and writes the chip when the date command sets
 * the clock.  File timestamps keep reading the software count, which does
 * not wait on the bus.
 *
 * Only the I2C pins are wired.  SCL is PB6 and SDA is PB7 on every board,
 * the same pair I2C bus 1 already uses, and those two numbers are fixed
 * here rather than chosen at run time.  32 kHz, INT/SQW and RST stay
 * unconnected.  Writing the clock turns those outputs off: square wave
 * off, alarms off, and the 32 kHz pin high impedance.  The Blue Pill
 * image has no spare flash for a separate quiet at boot, so a chip that
 * has not been set yet may still drive 32 kHz until the date command.
 *
 * The lines are the bit-banged master in src/i2c.c.  A transfer opens bus
 * 1 when the shell does not already hold it, and closes it again after.
 * A program, or PWM, that already owns PB6 or PB7 is told the pins are
 * taken.  The address is the DS3231's fixed 7-bit address, 0x68.  The
 * clock is 100 kHz, which is the rate the datasheet requires.
 */
#include "freya.h"

#ifndef FREYA_RTC_DS3231
#error "src/ds3231.c is compiled only with RTC=ds3231"
#endif

/* Bus 1 is this pair on every board.  ds3231_pins_match() refuses to
 * talk if a board's map ever says otherwise. */
#define DS3231_BUS      1
#define DS3231_HZ       100000UL

#define DS3231_REG_CTRL 0x0E
#define DS3231_REG_STAT 0x0F
#define DS3231_RD_LEN   16          /* 0x00 .. status                    */
#define DS3231_WR_LEN   16          /* 0x00 .. status                    */

#define DS3231_OSF      0x80
#define DS3231_EN32KHZ  0x08
/* Oscillator on, square wave off, alarms off.  INT/SQW then stays idle. */
#define DS3231_CTRL_QUIET 0x04

typedef struct {
    uint8_t scl;
    uint8_t sda;
} ds3231_pin_t;

int ds3231_pins_match(void)
{
    static const ds3231_pin_t map[] = BOARD_I2C_MAP;

    return DS3231_BUS >= 1 &&
           DS3231_BUS <= (int)ARRAY_SIZE(map) &&
           map[DS3231_BUS - 1].scl == (uint8_t)DS3231_SCL &&
           map[DS3231_BUS - 1].sda == (uint8_t)DS3231_SDA;
}

/* ---------------------------------------------------------- registers */
static int is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int month_days(int y, int m)
{
    static const uint8_t md[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (m == 2 && is_leap(y)) return 29;
    return md[m - 1];
}

/* The day register has to be 1..7 or the chip rejects the write in
 * spirit; the date does not depend on which day is which.  Freya
 * stores 1 and leaves the chip to count from there. */

static uint8_t __attribute__((noinline)) to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int from_bcd(uint8_t v, int *out)
{
    int hi = v >> 4;
    int lo = v & 0x0F;

    if (hi > 9 || lo > 9) return -1;
    *out = hi * 10 + lo;
    return 0;
}

/* 24-hour register.  Bit 6 set means a 12-hour value left by something
 * else; Freya does not keep that form, and the next date command
 * writes 24-hour time over it. */
static int hour_from_reg(uint8_t v, int *hour)
{
    int h;

    if ((v & 0xC0) || from_bcd(v, &h) != 0 || h > 23) return -1;
    *hour = h;
    return 0;
}

/* 0 the seven time registers are a civil time, -1 they are not.
 * The day-of-week register is not consulted: the date does not need it,
 * and a chip set by something else may number the days differently. */
static int decode_time(const uint8_t *r, rtc_time_t *t)
{
    int sec, min, hour, day, mon, year, century;

    if (from_bcd(r[0], &sec) != 0 || sec > 59) return -1;
    if (from_bcd(r[1], &min) != 0 || min > 59) return -1;
    if (hour_from_reg(r[2], &hour) != 0) return -1;
    if (from_bcd(r[4], &day) != 0 || day < 1) return -1;
    /* Bit 7 is the century.  Bits 6 and 5 are not part of the month. */
    if (r[5] & 0x60) return -1;
    century = (r[5] & 0x80) != 0;
    if (from_bcd((uint8_t)(r[5] & 0x1F), &mon) != 0 || mon < 1 || mon > 12)
        return -1;
    if (from_bcd(r[6], &year) != 0) return -1;
    year += century ? 2100 : 2000;
    if (day > month_days(year, mon)) return -1;

    t->year = (uint16_t)year;
    t->mon  = (uint8_t)mon;
    t->day  = (uint8_t)day;
    t->hour = (uint8_t)hour;
    t->min  = (uint8_t)min;
    t->sec  = (uint8_t)sec;
    return 0;
}

/* Sixteen bytes starting at register 0: the time, alarms left off, the
 * extra pins held idle, and the oscillator-stop flag cleared. */
static int encode_time(const rtc_time_t *t, uint8_t *r)
{
    int yy, century, i;

    if (!t || t->mon < 1 || t->mon > 12 || t->day < 1 ||
        t->hour > 23 || t->min > 59 || t->sec > 59 ||
        t->year < 2000 || t->year > 2199 ||
        t->day > month_days((int)t->year, (int)t->mon))
        return -1;

    yy = (int)t->year - 2000;
    century = yy >= 100;
    if (century) yy -= 100;

    r[0] = to_bcd(t->sec);
    r[1] = to_bcd(t->min);
    r[2] = to_bcd(t->hour);          /* 24-hour mode, bit 6 clear       */
    r[3] = 1;
    r[4] = to_bcd(t->day);
    r[5] = (uint8_t)(to_bcd(t->mon) | (century ? 0x80 : 0));
    r[6] = to_bcd(yy);
    for (i = 7; i < DS3231_WR_LEN; i++) r[i] = 0;
    r[DS3231_REG_CTRL] = DS3231_CTRL_QUIET;
    r[DS3231_REG_STAT] = 0;          /* OSF clear, 32 kHz pin off        */
    return 0;
}

/* --------------------------------------------------------------- bus */
#ifdef FREYA_HOST
static uint8_t s_mem[DS3231_RD_LEN];
static int s_fail;

void ds3231_test_load(const uint8_t *mem, int n)
{
    int i;

    for (i = 0; i < DS3231_RD_LEN; i++) s_mem[i] = 0;
    if (!mem || n < 0) n = 0;
    if (n > DS3231_RD_LEN) n = DS3231_RD_LEN;
    for (i = 0; i < n; i++) s_mem[i] = mem[i];
    s_fail = 0;
}

void ds3231_test_fail(int rc) { s_fail = rc; }

void ds3231_test_save(uint8_t *dst, int n)
{
    int i;

    if (!dst || n < 0) return;
    if (n > DS3231_RD_LEN) n = DS3231_RD_LEN;
    for (i = 0; i < n; i++) dst[i] = s_mem[i];
}

static int ds3231_xfer(const void *tx, int txlen, void *rx, int rxlen)
{
    const uint8_t *tb = tx;
    uint8_t *rb = rx;
    int reg, n, i;

    if (s_fail) return s_fail;
    if (!tb || txlen < 1) return FREYA_ERR_ARG;
    reg = tb[0];
    if (reg < 0 || reg >= DS3231_RD_LEN) return FREYA_ERR_ARG;
    n = txlen - 1;
    if (reg + n > DS3231_RD_LEN) return FREYA_ERR_ARG;
    for (i = 0; i < n; i++) s_mem[reg + i] = tb[1 + i];
    if (rxlen) {
        if (!rb || reg + rxlen > DS3231_RD_LEN) return FREYA_ERR_ARG;
        for (i = 0; i < rxlen; i++) rb[i] = s_mem[reg + i];
    }
    return 0;
}
#else
static int ds3231_xfer(const void *tx, int txlen, void *rx, int rxlen)
{
    i2c_info_t in;
    int opened = 0;
    int rc;

    if (i2c_info(DS3231_BUS - 1, &in) != 0) return FREYA_ERR_ARG;
    if (!in.open) {
        rc = i2c_open(DS3231_BUS, DS3231_HZ);
        if (rc) return rc;
        opened = 1;
    } else {
        /* Opening a bus this side already holds keeps it open and does
         * not change its pace.  Opening one a program holds is busy,
         * and this does not close that program's bus. */
        rc = i2c_open(DS3231_BUS, in.hz ? in.hz : DS3231_HZ);
        if (rc) return rc;
    }
    rc = i2c_transfer(DS3231_BUS, DS3231_ADDR, tx, txlen, rx, rxlen);
    if (opened) (void)i2c_close(DS3231_BUS);
    return rc;
}
#endif

static int regs_read(uint8_t *raw)
{
    uint8_t reg = 0;

    return ds3231_xfer(&reg, 1, raw, DS3231_RD_LEN);
}

/* --------------------------------------------------------------- calls */
int ds3231_read(rtc_time_t *t)
{
    uint8_t raw[DS3231_RD_LEN];
    rtc_time_t dec;
    int rc;

    if (!t) return FREYA_ERR_ARG;
    if (!ds3231_pins_match()) return FREYA_ERR_UNSUPPORTED;
    rc = regs_read(raw);
    if (rc) return rc;
    if (decode_time(raw, &dec) != 0) return FREYA_ERR_ARG;
    if (raw[DS3231_REG_STAT] & DS3231_OSF) return FREYA_ERR_ARG;
    *t = dec;
    return 0;
}

/* Its own section so the Blue Pill can park the write in the extension.
 * The read stays in the kernel image. */
int __attribute__((noinline, section(".text.ds3231_write")))
ds3231_write(const rtc_time_t *t)
{
    uint8_t buf[1 + DS3231_WR_LEN];
    int rc;

    if (!ds3231_pins_match()) return FREYA_ERR_UNSUPPORTED;
    buf[0] = 0;
    if (encode_time(t, buf + 1) != 0) return FREYA_ERR_ARG;
    rc = ds3231_xfer(buf, (int)sizeof buf, NULL, 0);
    return rc;
}

/* Its own section so the Blue Pill can park this call in the extension.
 * The F4 scripts already take every section of this file. */
#define BOOT_TEXT __attribute__((noinline, section(".text.ds3231_boot")))

void BOOT_TEXT ds3231_boot(void)
{
    rtc_time_t t;

    if (ds3231_read(&t) == 0) rtc_set(&t);
}
