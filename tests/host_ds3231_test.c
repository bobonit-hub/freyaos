/*
 * Freya - DS3231 register coding.
 *
 * The chip is not here.  src/ds3231.c talks to a stand-in register file
 * on the host, and the same file is what a transfer would have written.
 * The pins are checked too: PB6 and PB7 have to be what this board calls
 * I2C bus 1, on every board the test is compiled for.
 */
#include <stdio.h>
#include <string.h>

#include "freya.h"

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

static void check_mem(const char *what, const uint8_t *got, const uint8_t *want, int n)
{
    checks++;
    if (memcmp(got, want, (size_t)n) == 0) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        fails++;
    }
}

/* Boot copies a valid chip into the software clock and prints nothing. */
int kprintf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

void kput_hms(uint32_t y, uint32_t mo, uint32_t d,
              uint32_t h, uint32_t mi, int sec)
{
    (void)y; (void)mo; (void)d; (void)h; (void)mi; (void)sec;
}

void rtc_set(const rtc_time_t *t) { (void)t; }

static void load(const uint8_t *mem, int n)
{
    ds3231_test_load(mem, n);
}

static int read_time(rtc_time_t *t)
{
    return ds3231_read(t);
}

static void test_pins(void)
{
    printf("-- %s\n", BOARD_MCU);
    check("SCL is PB6", FREYA_PB(6), DS3231_SCL);
    check("SDA is PB7", FREYA_PB(7), DS3231_SDA);
    check("address is 0x68", 0x68, DS3231_ADDR);
    check("bus 1 uses those pins", 1, ds3231_pins_match());
}

static void test_round_trip(void)
{
    rtc_time_t t, back;
    uint8_t mem[19];
    uint8_t want[16] = {
        0x00, 0x40, 0x20, 0x01, 0x27, 0x09, 0x26,
        0, 0, 0, 0, 0, 0, 0,
        0x04, 0x00
    };
    int rc;

    memset(mem, 0, sizeof mem);
    t.year = 2026; t.mon = 9; t.day = 27;
    t.hour = 20;  t.min = 40; t.sec = 0;
    load(NULL, 0);
    rc = ds3231_write(&t);
    check("a Sunday evening is stored", 0, rc);
    ds3231_test_save(mem, 16);
    check_mem("2026-09-27 20:40:00 is those registers", mem, want, 16);

    load(mem, 16);
    rc = read_time(&back);
    check("the stored time reads back", 0, rc);
    check("year", 2026, back.year);
    check("month", 9, back.mon);
    check("day", 27, back.day);
    check("hour", 20, back.hour);
    check("minute", 40, back.min);
    check("second", 0, back.sec);
}

static void test_12hour(void)
{
    rtc_time_t t;
    uint8_t raw[19];
    int rc;

    memset(raw, 0, sizeof raw);
    raw[2] = 0x72;             /* 12-hour mode, 12 PM */
    raw[4] = 0x01;
    raw[5] = 0x01;
    raw[6] = 0x00;
    load(raw, 16);
    rc = read_time(&t);
    check("12-hour mode is not a time Freya keeps", FREYA_ERR_ARG, rc);
}

static void test_century_and_leap(void)
{
    rtc_time_t t;
    uint8_t mem[19];
    int rc;

    t.year = 2100; t.mon = 1; t.day = 1;
    t.hour = 0; t.min = 0; t.sec = 0;
    load(NULL, 0);
    rc = ds3231_write(&t);
    check("2100 is stored", 0, rc);
    ds3231_test_save(mem, 16);
    check("century bit set", 0x81, mem[5]);
    check("year byte is 00", 0x00, mem[6]);
    check("the day register is stored as 1", 1, mem[3]);

    t.day = 29; t.mon = 2;
    rc = ds3231_write(&t);
    check("2100-02-29 is refused", FREYA_ERR_ARG, rc);
    ds3231_test_save(mem, 7);
    check("a refused date does not touch the month", 0x81, mem[5]);

    t.year = 2000; t.mon = 2; t.day = 29;
    rc = ds3231_write(&t);
    check("2000-02-29 is a leap day", 0, rc);

    t.year = 1999;
    rc = ds3231_write(&t);
    check("1999 does not fit", FREYA_ERR_ARG, rc);

    t.year = 2200; t.mon = 1; t.day = 1;
    rc = ds3231_write(&t);
    check("2200 does not fit", FREYA_ERR_ARG, rc);

    t.year = 2199; t.mon = 12; t.day = 31;
    t.hour = 23; t.min = 59; t.sec = 59;
    rc = ds3231_write(&t);
    check("2199-12-31 23:59:59 is stored", 0, rc);
}

static void test_osf_and_quiet(void)
{
    rtc_time_t t;
    uint8_t raw[19];
    uint8_t saved[19];
    int rc;

    memset(raw, 0, sizeof raw);
    raw[0] = 0x30;
    raw[1] = 0x15;
    raw[2] = 0x08;
    raw[3] = 0x01;
    raw[4] = 0x02;
    raw[5] = 0x03;
    raw[6] = 0x24;
    raw[0x0E] = 0x1C;
    raw[0x0F] = 0x88;          /* oscillator stopped, 32 kHz on */
    load(raw, 16);
    rc = read_time(&t);
    check("a stopped oscillator is not a valid time", FREYA_ERR_ARG, rc);

    ds3231_test_fail(FREYA_ERR_NACK);
    rc = read_time(&t);
    check("no answer", FREYA_ERR_NACK, rc);

    t.year = 2026; t.mon = 9; t.day = 27;
    t.hour = 20; t.min = 40; t.sec = 0;
    ds3231_test_fail(FREYA_ERR_BUSY);
    rc = ds3231_write(&t);
    check("taken pins are not written", FREYA_ERR_BUSY, rc);
    ds3231_test_save(saved, 19);
    check("a refused write leaves the status byte", 0x88, saved[0x0F]);
}

static void test_garbage(void)
{
    rtc_time_t t;
    uint8_t raw[19];
    int rc;

    memset(raw, 0, sizeof raw);
    raw[4] = 0x01;
    raw[5] = 0x01;
    raw[6] = 0x26;
    raw[0] = 0x0A;
    load(raw, 16);
    rc = read_time(&t);
    check("a bad BCD second is refused", FREYA_ERR_ARG, rc);

    raw[0] = 0x00;
    raw[5] = 0x21;             /* bit 5 set, which is not the century */
    load(raw, 16);
    rc = read_time(&t);
    check("a stray month bit is refused", FREYA_ERR_ARG, rc);

    raw[5] = 0x02;
    raw[4] = 0x30;             /* 30 February */
    load(raw, 16);
    rc = read_time(&t);
    check("30 February is refused", FREYA_ERR_ARG, rc);
}

int main(void)
{
    test_pins();
    test_round_trip();
    test_12hour();
    test_century_and_leap();
    test_osf_and_quiet();
    test_garbage();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
