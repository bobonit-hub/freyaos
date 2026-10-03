/*
 * Freya - the chip's own calendar RTC.
 *
 * Built only when make is given RTC=internal, and only for a board whose
 * chip has the calendar RTC of the F4, U5, H5 and H7 (BOARD_RTC_INTERNAL
 * in its board.h).  It is the DS3231's job done by the chip: the RTC runs
 * from the 32.768 kHz crystal (LSE) in the backup domain, so it keeps time
 * through a reset, and through a power cut when VBAT has a battery.
 * Freya still counts civil time from SysTick; this driver copies the RTC
 * into that count at boot and writes the RTC when the date command, or a
 * program's rtc_set(), sets the clock.
 *
 * The register block is the same on all four families as far as it is
 * used here: TR and DR hold the time and the date in BCD, ISR (ICSR on
 * the U5 and H5) the init and sync flags, PRER the two prescalers and WPR
 * the write protection key.  Only CR moves, and the board names its
 * offset.  What a board supplies is the base address and
 * board_rtc_access(), which turns on the clocks the RTC registers need and
 * opens the backup domain for writing.  The two digit year is 2000..2099.
 */
#include "freya.h"

#ifndef FREYA_RTC_INTERNAL
#error "src/rtc.c is compiled only with RTC=internal"
#endif
#ifndef BOARD_RTC_INTERNAL
#error "RTC=internal: this board has no calendar RTC driver"
#endif

typedef struct {
    __IO uint32_t TR;          /* 0x00 */
    __IO uint32_t DR;          /* 0x04 */
    __IO uint32_t R08;         /* 0x08: CR on the F4 and H7, SSR on the U5/H5 */
    __IO uint32_t ISR;         /* 0x0C: ISR, or ICSR */
    __IO uint32_t PRER;        /* 0x10 */
    __IO uint32_t R14[4];      /* 0x14 .. 0x20 */
    __IO uint32_t WPR;         /* 0x24 */
} rtc_regs_t;

#define RTC_ISR_INITS       (1UL << 4)      /* the year is not 0: it was set */
#define RTC_ISR_RSF         (1UL << 5)      /* the shadow registers are current */
#define RTC_ISR_INITF       (1UL << 6)      /* in init mode: writable */
#define RTC_ISR_INIT        (1UL << 7)
#define RTC_ISR_BIN_MASK    (3UL << 8)      /* U5/H5: 0 is BCD; F4/H7: alarm flags */
#define RTC_CR_FMT          (1UL << 6)      /* 12 hour format */

/* The backup domain control register, the same bits on every family. */
#define BDCR_LSEON          (1UL << 0)
#define BDCR_LSERDY         (1UL << 1)
#define BDCR_RTCSEL_MASK    (3UL << 8)
#define BDCR_RTCSEL_LSE     (1UL << 8)
#define BDCR_RTCEN          (1UL << 15)
#define BDCR_BDRST          (1UL << 16)

#define LSE_START_MS        2000U           /* a crystal can take seconds */
#define RTC_WAIT_MS         20U

#ifdef FREYA_HOST
static rtc_regs_t s_fake_rtc;
static uint32_t   s_fake_bdcr;
#define RTCR                (&s_fake_rtc)
#define BDCR                s_fake_bdcr
#define RTC_CR              (s_fake_rtc.R08)
#define board_rtc_access()  ((void)0)
#else
#define RTCR                ((rtc_regs_t *)BOARD_RTC_BASE)
#define BDCR                (RCC->BDCR)
#define RTC_CR              (*(__IO uint32_t *)(BOARD_RTC_BASE + BOARD_RTC_CR_OFF))
#endif

#define RTC_TEXT __attribute__((noinline, section(".text.rtcin")))

/* ------------------------------------------------------------ helpers */
static uint32_t RTC_TEXT bcd(uint32_t v)   { return ((v / 10U) << 4) | (v % 10U); }
static uint32_t RTC_TEXT unbcd(uint32_t v) { return (v >> 4) * 10U + (v & 0xFU); }

/* Wait for bits of ISR to be set (on) or clear (!on), for up to ms. */
static int RTC_TEXT wait_isr(uint32_t bits, int on, uint32_t ms)
{
    uint32_t start = sys_ticks();

    for (;;) {
        int now = (RTCR->ISR & bits) == bits;
        if (now == on) return 0;
        if ((uint32_t)(sys_ticks() - start) >= ms) return FREYA_ERR_TIMEOUT;
    }
}

/* Monday is 1, as the RTC counts; 1970-01-01 was a Thursday. */
static uint32_t RTC_TEXT weekday(const rtc_time_t *t)
{
    static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212,
                                         243, 273, 304, 334 };
    uint32_t y = t->year;
    uint32_t days = (y - 1970U) * 365U + (y - 1969U) / 4U;   /* to 2099 */

    days += before[t->mon - 1] + t->day - 1U;
    if (t->mon > 2 && (y % 4U) == 0) days++;
    return (days + 3U) % 7U + 1U;
}

static int RTC_TEXT running(void)
{
    uint32_t b = BDCR;

    return (b & (BDCR_LSERDY | BDCR_RTCEN)) == (BDCR_LSERDY | BDCR_RTCEN) &&
           (b & BDCR_RTCSEL_MASK) == BDCR_RTCSEL_LSE;
}

/*
 * Start the crystal and give it to the RTC, unless that is already so.
 * The source can only be changed by resetting the backup domain, which is
 * done when something else (LSI, HSE) had been chosen.
 */
static int RTC_TEXT lse_start(void)
{
    uint32_t start;

    board_rtc_access();
    if (running()) return 0;
    if ((BDCR & BDCR_RTCSEL_MASK) && (BDCR & BDCR_RTCSEL_MASK) != BDCR_RTCSEL_LSE) {
        BDCR |= BDCR_BDRST;
        BDCR &= ~BDCR_BDRST;
    }
    BDCR |= BDCR_LSEON;
    start = sys_ticks();
    while (!(BDCR & BDCR_LSERDY)) {
        if ((uint32_t)(sys_ticks() - start) >= LSE_START_MS) {
            BDCR &= ~BDCR_LSEON;
            return FREYA_ERR_TIMEOUT;
        }
    }
    BDCR = (BDCR & ~BDCR_RTCSEL_MASK) | BDCR_RTCSEL_LSE;
    BDCR |= BDCR_RTCEN;
    return 0;
}

/* -------------------------------------------------------------- calls */
/*
 * 0 the time is valid and *t was filled.  FREYA_ERR_ARG the RTC runs but
 * was never set, or does not decode.  FREYA_ERR_IO it does not run: no
 * crystal started, or the backup domain lost its power.
 */
int RTC_TEXT rtcin_read(rtc_time_t *t)
{
    uint32_t tr, dr;

    board_rtc_access();
    if (!running()) return FREYA_ERR_IO;
    if (!(RTCR->ISR & RTC_ISR_INITS)) return FREYA_ERR_ARG;
    /* After a reset the shadow registers are stale until RSF is set. */
    if (wait_isr(RTC_ISR_RSF, 1, RTC_WAIT_MS) != 0) return FREYA_ERR_IO;

    tr = RTCR->TR;                      /* locks DR until it is read */
    dr = RTCR->DR;
    t->hour = (uint8_t)unbcd((tr >> 16) & 0x3FU);
    t->min  = (uint8_t)unbcd((tr >> 8) & 0x7FU);
    t->sec  = (uint8_t)unbcd(tr & 0x7FU);
    t->year = (uint16_t)(2000U + unbcd((dr >> 16) & 0xFFU));
    t->mon  = (uint8_t)unbcd((dr >> 8) & 0x1FU);
    t->day  = (uint8_t)unbcd(dr & 0x3FU);
    if (t->mon < 1 || t->mon > 12 || t->day < 1 || t->day > 31 ||
        t->hour > 23 || t->min > 59 || t->sec > 59)
        return FREYA_ERR_ARG;
    return 0;
}

/* 0, FREYA_ERR_ARG for a date the RTC cannot hold (outside 2000..2099),
 * or FREYA_ERR_TIMEOUT when the crystal or the RTC does not respond. */
int RTC_TEXT rtcin_write(const rtc_time_t *t)
{
    int rc;

    if (!t || t->year < 2000 || t->year > 2099) return FREYA_ERR_ARG;
    rc = lse_start();
    if (rc) return rc;

    RTCR->WPR = 0xCA;                   /* unlock */
    RTCR->WPR = 0x53;
    RTCR->ISR |= RTC_ISR_INIT;
    rc = wait_isr(RTC_ISR_INITF, 1, RTC_WAIT_MS);
    if (rc == 0) {
        /* 32768 / (127 + 1) / (255 + 1) = 1 Hz, one write at a time. */
        RTCR->PRER = 255U;
        RTCR->PRER |= 127UL << 16;
        RTC_CR &= ~RTC_CR_FMT;          /* 24 hours */
        RTCR->ISR &= ~RTC_ISR_BIN_MASK; /* BCD, not binary */
        RTCR->TR = (bcd(t->hour) << 16) | (bcd(t->min) << 8) | bcd(t->sec);
        RTCR->DR = (bcd(t->year - 2000U) << 16) | (weekday(t) << 13) |
                   (bcd(t->mon) << 8) | bcd(t->day);
    }
    RTCR->ISR &= ~RTC_ISR_INIT;         /* counting again */
    RTCR->WPR = 0xFF;                   /* lock */
    return rc;
}

/* The RTC into the software clock, and a line saying how that went.
 * A board whose RTC was never started starts its crystal here, so the
 * clock keeps counting from the first date that is set. */
void RTC_TEXT rtcin_boot(void)
{
    rtc_time_t t;
    int rc = rtcin_read(&t);

    kprintf("[boot] RTC        : ");
    if (rc == FREYA_ERR_IO) {
        rc = lse_start();
        if (rc) {
            kprintf("no 32.768 kHz crystal\r\n");
            return;
        }
        rc = FREYA_ERR_ARG;
    }
    if (rc) {
        kprintf("not set; set it with date()\r\n");
        return;
    }
    rtc_set(&t);
    kput_hms(t.year, t.mon, t.day, t.hour, t.min, t.sec);
    kprintf("\r\n");
}

#ifdef FREYA_HOST
void rtcin_test_regs(uint32_t **tr, uint32_t **dr, uint32_t **isr,
                     uint32_t **prer, uint32_t **cr, uint32_t **bdcr)
{
    *tr = (uint32_t *)&s_fake_rtc.TR;
    *dr = (uint32_t *)&s_fake_rtc.DR;
    *isr = (uint32_t *)&s_fake_rtc.ISR;
    *prer = (uint32_t *)&s_fake_rtc.PRER;
    *cr = (uint32_t *)&s_fake_rtc.R08;
    *bdcr = &s_fake_bdcr;
}
#endif
