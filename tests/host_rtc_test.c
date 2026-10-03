/*
 * Freya - the calendar RTC's register coding.
 *
 * The RTC is not here.  src/rtc.c talks to a stand-in register block and a
 * stand-in backup domain register on the host.  They do not count, and
 * nothing sets their flags but the test, so each case sets up the flags
 * the hardware would have: the crystal running, init mode granted, the
 * shadow registers in sync.  What is checked is what the driver writes and
 * how it reads it back: BCD, the weekday, the prescalers, the 24 hour
 * format, the year range, and the two ways the RTC can be unusable.
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
        printf("  FAIL  %s: expected 0x%lx, got 0x%lx\n", what, expected, got);
        fails++;
    }
}

/* The calls the driver makes into the rest of the kernel. */
static uint32_t s_ticks;
uint32_t sys_ticks(void) { return s_ticks += 7; }   /* time passes when asked */

static rtc_time_t s_set;
static int s_set_calls;
void rtc_set(const rtc_time_t *t) { s_set = *t; s_set_calls++; }

static char s_out[128];
int kprintf(const char *fmt, ...)
{
    strncat(s_out, fmt, sizeof s_out - strlen(s_out) - 1);
    return 0;
}

void kput_hms(uint32_t y, uint32_t mo, uint32_t d,
              uint32_t h, uint32_t mi, int sec)
{
    (void)y; (void)mo; (void)d; (void)h; (void)mi; (void)sec;
}

static uint32_t *TR, *DR, *ISR, *PRER, *CR, *BDCR;

#define LSE_RUNNING   ((1UL << 0) | (1UL << 1) | (1UL << 8) | (1UL << 15))
#define ISR_INITS     (1UL << 4)
#define ISR_RSF       (1UL << 5)
#define ISR_INITF     (1UL << 6)
#define ISR_INIT      (1UL << 7)

static rtc_time_t when(int y, int mo, int d, int h, int mi, int s)
{
    rtc_time_t t;

    t.year = (uint16_t)y; t.mon = (uint8_t)mo; t.day = (uint8_t)d;
    t.hour = (uint8_t)h;  t.min = (uint8_t)mi; t.sec = (uint8_t)s;
    return t;
}

int main(void)
{
    rtc_time_t t, back;

    rtcin_test_regs(&TR, &DR, &ISR, &PRER, &CR, &BDCR);
    printf("calendar RTC\n");

    /* --------------------------------------------------- writing */
    *BDCR = LSE_RUNNING;
    *ISR = ISR_INITF | ISR_RSF;          /* init mode is granted at once */
    *CR = 1UL << 6;                      /* left in 12 hour format       */
    t = when(2026, 10, 3, 21, 5, 9);
    check("a date in range is written", 0, rtcin_write(&t));
    check("the time is BCD, 24 hour", 0x210509, (long)*TR);
    /* 2026-10-03 is a Saturday: weekday 6, Monday being 1. */
    check("the date is BCD with the weekday", (0x26L << 16) | (6L << 13) | (0x10L << 8) | 0x03,
          (long)*DR);
    check("the prescalers make 1 Hz from 32768", (127L << 16) | 255, (long)*PRER);
    check("12 hour format is turned off", 0, (long)(*CR & (1UL << 6)));
    check("init mode is left", 0, (long)(*ISR & ISR_INIT));

    t = when(2000, 1, 1, 0, 0, 0);
    check("2000-01-01 is written", 0, rtcin_write(&t));
    check("and is a Saturday", 6, (long)((*DR >> 13) & 7));
    t = when(2024, 2, 29, 23, 59, 58);
    check("a leap day is written", 0, rtcin_write(&t));
    check("and is a Thursday", 4, (long)((*DR >> 13) & 7));
    t = when(2099, 12, 31, 12, 0, 0);
    check("2099-12-31 is written", 0, rtcin_write(&t));
    check("and is a Thursday", 4, (long)((*DR >> 13) & 7));

    t = when(1999, 12, 31, 0, 0, 0);
    check("1999 is refused: two digits of year", FREYA_ERR_ARG, rtcin_write(&t));
    t = when(2100, 1, 1, 0, 0, 0);
    check("2100 is refused", FREYA_ERR_ARG, rtcin_write(&t));

    *ISR = ISR_RSF;                      /* init mode never granted */
    t = when(2026, 1, 1, 0, 0, 0);
    check("an RTC that will not enter init mode is a timeout",
          FREYA_ERR_TIMEOUT, rtcin_write(&t));
    check("and is not left in init mode", 0, (long)(*ISR & ISR_INIT));

    *BDCR = 0;                           /* no crystal, and it never starts */
    *ISR = ISR_INITF | ISR_RSF;
    check("a crystal that does not start is a timeout",
          FREYA_ERR_TIMEOUT, rtcin_write(&t));
    check("and is turned off again", 0, (long)(*BDCR & 1));

    /* --------------------------------------------------- reading */
    *BDCR = LSE_RUNNING;
    *ISR = ISR_INITS | ISR_RSF;
    *TR = 0x235958;
    *DR = (0x26L << 16) | (6L << 13) | (0x12L << 8) | 0x31;
    memset(&back, 0, sizeof back);
    check("a set RTC is read", 0, rtcin_read(&back));
    check("the year", 2026, back.year);
    check("the month", 12, back.mon);
    check("the day", 31, back.day);
    check("the hour", 23, back.hour);
    check("the minute", 59, back.min);
    check("the second", 58, back.sec);

    t = when(2031, 7, 15, 8, 30, 0);
    *ISR = ISR_INITF | ISR_RSF | ISR_INITS;
    rtcin_write(&t);
    rtcin_read(&back);
    check("what is written reads back", 0, memcmp(&t, &back, sizeof t));

    *ISR = ISR_RSF;                      /* the year was never set */
    check("an RTC that was never set is not valid", FREYA_ERR_ARG, rtcin_read(&back));
    *ISR = ISR_INITS | ISR_RSF;
    *DR = (0x26L << 16) | (0x13L << 8) | 0x01;
    check("a month that does not decode is not valid", FREYA_ERR_ARG, rtcin_read(&back));
    *ISR = ISR_INITS;                    /* never in sync */
    check("shadow registers that never sync are an I/O error",
          FREYA_ERR_IO, rtcin_read(&back));
    *BDCR = 0;
    *ISR = ISR_INITS | ISR_RSF;
    check("an RTC that does not run is an I/O error", FREYA_ERR_IO, rtcin_read(&back));

    /* ------------------------------------------------------ boot */
    *BDCR = LSE_RUNNING;
    *ISR = ISR_INITS | ISR_RSF;
    *TR = 0x120000;
    *DR = (0x27L << 16) | (0x03L << 8) | 0x14;
    s_set_calls = 0;
    rtcin_boot();
    check("boot copies a set RTC into the software clock", 1, s_set_calls);
    check("as that date", 2027, s_set.year);

    *ISR = ISR_RSF;
    s_set_calls = 0;
    s_out[0] = '\0';
    rtcin_boot();
    check("boot leaves the clock alone when the RTC was never set", 0, s_set_calls);
    check("and says so", 1, strstr(s_out, "not set") != NULL);

    *BDCR = 0;
    s_out[0] = '\0';
    rtcin_boot();
    check("boot says when there is no crystal", 1,
          strstr(s_out, "no 32.768 kHz crystal") != NULL);

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
