/*
 * flashprobe - how much internal flash the chip really has.
 *
 * Every board Freya supports has at least 128 KiB.  An STM32F103C8 often
 * still says 64; that report is raised to 128 before anything is believed,
 * and a write is what settles the question of flash past that floor.
 * Reading extra pages proves nothing, because unimplemented flash reads
 * back as something.  So this program writes.
 *
 *     run flashprobe.bin        probe to twice the size used below
 *     run flashprobe.bin 256    probe to 256 KiB
 *
 * One erase unit at a time, starting at the last unit inside the declared
 * flash and walking upwards, it programs a 256 byte block, reads it back,
 * compares, and erases the unit again.  The first unit that does not
 * compare is where the flash ends.
 *
 * The rule that makes this safe is that a unit is written only when it -
 * and the address it would mirror, if the decoder wraps - reads entirely
 * 0xFF first.  Whatever a probed address really turns out to be (new
 * flash, a mirror of a page nobody uses, or nothing at all), no byte that
 * mattered was there to lose, and the unit is erased again afterwards, so
 * the chip is left exactly as it was found.
 *
 * Freya's own flash driver cannot be used for this: it refuses every
 * address outside the program region on purpose, and nothing in the
 * service table reaches it.  A program is not isolated from the hardware,
 * though, so the controller is driven here directly - which is the whole
 * reason this is a sample and not a console command.
 */
#include "freya_api.h"

/* Every other sample is board independent; this one is the exception, and
 * a third board means a third section below rather than a default. */
#if !defined(FREYA_BOARD_BLUEPILL) && !defined(FREYA_BOARD_BLACKPILL) && \
    !defined(FREYA_BOARD_STM32F405)
#error "flashprobe drives the flash controller itself and needs a board it knows"
#endif

#define FLASH_ORIGIN        0x08000000UL
#define FLASH_MIN_KIB       128u            /* every supported board has this  */
#define BLOCK_BYTES         256u            /* the data block each step writes */
#define PROBE_MAX_KIB       1024u           /* as far up as this will look     */
#define ERR_TIMEOUT         0x80000000UL    /* not an SR bit: our own          */

#define FLASH_KEY1          0x45670123UL
#define FLASH_KEY2          0xCDEF89ABUL

/* The first five registers are at the same offsets on both families; the
 * sixth is the F1's address register and the F4's OPTCR, untouched there. */
typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t SR;
    volatile uint32_t CR;
    volatile uint32_t AR;
} flash_regs_t;

static inline uint32_t irq_off(void)
{
    uint32_t pm;
    __asm volatile ("mrs %0, primask" : "=r"(pm));
    __asm volatile ("cpsid i" ::: "memory");
    return pm;
}

static inline void irq_on(uint32_t pm)
{
    __asm volatile ("msr primask, %0" :: "r"(pm) : "memory");
}

static inline void dsb(void) { __asm volatile ("dsb 0xF" ::: "memory"); }
static inline void isb(void) { __asm volatile ("isb 0xF" ::: "memory"); }

/* ===================================================== the STM32F103 == */
#if defined(FREYA_BOARD_BLUEPILL)

#define MCU_NAME        "STM32F103"
#define FL              ((flash_regs_t *)0x40022000UL)
#define FLASHSIZE_REG   (*(const volatile uint16_t *)0x1FFFF7E0UL)
#define DECLARED_KIB    128u                /* if the chip will not say  */
#define SPIN_LIMIT      2000000UL           /* 40 ms page erase, loosely */

#define SR_BSY          (1UL << 0)
#define SR_PGERR        (1UL << 2)
#define SR_WRPRTERR     (1UL << 4)
#define SR_EOP          (1UL << 5)
#define SR_ERRORS       (SR_PGERR | SR_WRPRTERR)

#define CR_PG           (1UL << 0)
#define CR_PER          (1UL << 1)
#define CR_STRT         (1UL << 6)
#define CR_LOCK         (1UL << 7)

/* Flat 1 KiB pages all the way up, on every density of the family. */
static uint32_t unit_size(uint32_t addr)  { (void)addr; return 1024u; }
static uint32_t unit_base(uint32_t addr)  { return addr & ~1023UL; }
static int      unit_erasable(uint32_t addr) { (void)addr; return 1; }

static const char *err_str(uint32_t bits)
{
    if (bits & ERR_TIMEOUT)  return "controller timeout";
    if (bits & SR_WRPRTERR)  return "write protected";
    if (bits & SR_PGERR)     return "programming error";
    return "no error reported";
}

#endif /* FREYA_BOARD_BLUEPILL */

/* ========================================== the STM32F411 and F405 == */
#if defined(FREYA_BOARD_BLACKPILL) || defined(FREYA_BOARD_STM32F405)

#if defined(FREYA_BOARD_STM32F405)
#define MCU_NAME        "STM32F405"
#else
#define MCU_NAME        "STM32F411"
#endif
#define FL              ((flash_regs_t *)0x40023C00UL)
#define FLASHSIZE_REG   (*(const volatile uint16_t *)0x1FFF7A22UL)
#define DECLARED_KIB    512u
#define SPIN_LIMIT      200000000UL         /* a sector erase is seconds */

#define SR_EOP          (1UL << 0)
#define SR_OPERR        (1UL << 1)
#define SR_WRPERR       (1UL << 4)
#define SR_PGAERR       (1UL << 5)
#define SR_PGPERR       (1UL << 6)
#define SR_PGSERR       (1UL << 7)
#define SR_BSY          (1UL << 16)
#define SR_ERRORS       (SR_OPERR | SR_WRPERR | SR_PGAERR | SR_PGPERR | SR_PGSERR)

#define CR_PG           (1UL << 0)
#define CR_SER          (1UL << 1)
#define CR_SNB_MASK     (0xFUL << 3)
#define CR_SNB(n)       (((uint32_t)(n) & 0xFUL) << 3)
#define CR_PSIZE_X32    (2UL << 8)
#define CR_STRT         (1UL << 16)
#define CR_LOCK         (1UL << 31)

#define ACR_ICEN        (1UL << 9)
#define ACR_DCEN        (1UL << 10)
#define ACR_ICRST       (1UL << 11)
#define ACR_DCRST       (1UL << 12)

/* Sectors, and unequal ones: four of 16 KiB, one of 64, then 128 KiB to
 * the top.  Above 512 KiB that pattern is simply continued, which is what
 * the 1 MiB parts of the same line do. */
static uint32_t unit_size(uint32_t addr)
{
    uint32_t off = addr - FLASH_ORIGIN;

    if (off < 0x10000UL) return 0x4000UL;
    if (off < 0x20000UL) return 0x10000UL;
    return 0x20000UL;
}

static uint32_t unit_base(uint32_t addr)
{
    return addr & ~(unit_size(addr) - 1UL);
}

static uint32_t sector_of(uint32_t addr)
{
    uint32_t off = addr - FLASH_ORIGIN;

    if (off < 0x10000UL) return off / 0x4000UL;
    if (off < 0x20000UL) return 4;
    return 4UL + off / 0x20000UL;
}

/* SNB is four bits wide, so the controller cannot name a sector past 15. */
static int unit_erasable(uint32_t addr) { return sector_of(addr) <= 15UL; }

static const char *err_str(uint32_t bits)
{
    if (bits & ERR_TIMEOUT) return "controller timeout";
    if (bits & SR_WRPERR)   return "write protected";
    if (bits & SR_PGSERR)   return "programming sequence error";
    if (bits & SR_PGPERR)   return "programming parallelism error";
    if (bits & SR_PGAERR)   return "programming alignment error";
    if (bits & SR_OPERR)    return "operation error";
    return "no error reported";
}

#endif /* FREYA_BOARD_BLACKPILL || FREYA_BOARD_STM32F405 */

/* ================================================ the controller, both = */
/*
 * Everything below runs with interrupts masked for the length of one
 * operation, because neither part has read-while-write: while SR.BSY is
 * set the controller stalls bus reads, and every interrupt handler Freya
 * has is in flash.  The program itself must therefore have been loaded or
 * copied into RAM before a controller operation starts.
 */
static uint32_t wait_idle(void)
{
    uint32_t spin = SPIN_LIMIT;
    uint32_t sr;

    while (FL->SR & SR_BSY) {
        if (--spin == 0) return ERR_TIMEOUT;
    }
    sr = FL->SR & SR_ERRORS;
    FL->SR = SR_ERRORS | SR_EOP;        /* both families clear by writing 1 */
    return sr;
}

static uint32_t unit_erase(uint32_t addr)
{
    uint32_t pm = irq_off();
    uint32_t rc = wait_idle();

    if (rc == 0) {
#if defined(FREYA_BOARD_BLUEPILL)
        FL->CR |= CR_PER;
        FL->AR  = addr;
        FL->CR |= CR_STRT;
        rc = wait_idle();
        FL->CR &= ~(CR_PER | CR_STRT);
#else
        FL->CR = (FL->CR & ~CR_SNB_MASK) | CR_SER | CR_SNB(sector_of(addr)) |
                 CR_PSIZE_X32;
        FL->CR |= CR_STRT;
        rc = wait_idle();
        FL->CR &= ~(CR_SER | CR_STRT | CR_SNB_MASK);
#endif
    }
    irq_on(pm);
    return rc;
}

/* The F1 programs a halfword at a time and the F4 a word, so the block is
 * kept as words and taken apart here. */
static uint32_t program_block(uint32_t addr, const uint32_t *w, uint32_t words)
{
    uint32_t rc = 0;
    uint32_t i;

    for (i = 0; i < words && rc == 0; i++) {
        uint32_t pm = irq_off();

        rc = wait_idle();
        if (rc == 0) {
#if defined(FREYA_BOARD_BLUEPILL)
            FL->CR |= CR_PG;
            *(volatile uint16_t *)(uintptr_t)(addr + i * 4U) = (uint16_t)w[i];
            rc = wait_idle();
            if (rc == 0) {
                *(volatile uint16_t *)(uintptr_t)(addr + i * 4U + 2U) =
                    (uint16_t)(w[i] >> 16);
                rc = wait_idle();
            }
            FL->CR &= ~CR_PG;
#else
            FL->CR |= CR_PG | CR_PSIZE_X32;
            *(volatile uint32_t *)(uintptr_t)(addr + i * 4U) = w[i];
            rc = wait_idle();
            FL->CR &= ~CR_PG;
#endif
        }
        irq_on(pm);
    }
    return rc;
}

static int flash_unlock(void)
{
    FL->KEYR = FLASH_KEY1;
    FL->KEYR = FLASH_KEY2;
    return (FL->CR & CR_LOCK) ? -1 : 0;
}

#if defined(FREYA_BOARD_BLACKPILL) || defined(FREYA_BOARD_STM32F405)
/* On the F411 the caches have to go: a read-back that came out of the data
 * cache would compare equal to whatever was there before the erase. */
static uint32_t s_acr;
#endif

static int probe_begin(void)
{
#if defined(FREYA_BOARD_BLACKPILL) || defined(FREYA_BOARD_STM32F405)
    s_acr = FL->ACR;
    FL->ACR &= ~(ACR_ICEN | ACR_DCEN);
#endif
    if (flash_unlock() != 0) return -1;
    FL->SR = SR_ERRORS | SR_EOP;
#if defined(FREYA_BOARD_BLACKPILL) || defined(FREYA_BOARD_STM32F405)
    FL->CR = (FL->CR & ~CR_SNB_MASK) | CR_PSIZE_X32;
#endif
    return 0;
}

static void probe_end(void)
{
#if defined(FREYA_BOARD_BLUEPILL)
    FL->CR &= ~(CR_PG | CR_PER);
#else
    FL->CR &= ~(CR_PG | CR_SER | CR_STRT);
#endif
    FL->CR |= CR_LOCK;
#if defined(FREYA_BOARD_BLACKPILL) || defined(FREYA_BOARD_STM32F405)
    FL->ACR |= ACR_ICRST | ACR_DCRST;
    FL->ACR &= ~(ACR_ICRST | ACR_DCRST);
    FL->ACR = s_acr;
#endif
    dsb();
    isb();
}

/* ======================================================== the probe === */
enum {
    PR_OK = 0,
    PR_MIRROR,      /* reads as the image of another address, detail  */
    PR_ZEROS,       /* reads as all zeros, which is what nothing does */
    PR_NOTBLANK,    /* something is stored there                      */
    PR_KERNEL,      /* below the program region: never write there    */
    PR_NOERASE,     /* the controller cannot name that erase unit     */
    PR_PROGERR,     /* the write was refused, SR bits in detail       */
    PR_MISMATCH,    /* wrote, read back something else at detail      */
    PR_ALIAS,       /* the write landed at detail instead             */
    PR_ERASEERR,    /* programmed, and would not erase again          */
    PR_NOTERASED    /* the erase said ok and the cells did not clear  */
};

/*
 * The pattern each word is programmed with.  A word carries its own
 * address, so a decoder that drops high bits inside the block, and a bus
 * that answers every read with the same thing, both fail the compare.
 */
static uint32_t pattern(uint32_t addr)
{
    return addr ^ 0xA5A55A5AUL;
}

/*
 * One step.  'mirror' is the address this one would fold onto if the chip
 * ignores the address bits above the declared size, or 0 for a unit that
 * is inside the declared flash and so has nothing to fold onto.
 */
static int probe_unit(uint32_t addr, uint32_t size, uint32_t mirror,
                      uint32_t *detail)
{
    const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)addr;
    const volatile uint32_t *m = (const volatile uint32_t *)(uintptr_t)mirror;
    const uint32_t words = size / 4u;
    const uint32_t block = BLOCK_BYTES / 4u;
    uint32_t want[BLOCK_BYTES / 4u];
    uint32_t i, rc;
    int blank = 1, zeros = 1, mirrored = 1, mirror_blank = 1, aliased;

    /* Freya's own driver bounds every write to the program region; this
     * one is outside it by definition, so the floor is checked here. */
    if (addr < FREYA_APP_FLASH_ADDR) return PR_KERNEL;
    if (!unit_erasable(addr)) return PR_NOERASE;

    for (i = 0; i < words; i++) {
        uint32_t v = p[i];

        if (v != 0xFFFFFFFFUL) blank = 0;
        if (v != 0UL) zeros = 0;
        if (mirror) {
            uint32_t mv = m[i];

            if (mv != v) mirrored = 0;
            if (mv != 0xFFFFFFFFUL) mirror_blank = 0;
        }
    }

    /* Reading back a page that holds something else, byte for byte, is a
     * wrapped address decoder rather than new flash. */
    if (mirror && mirrored && !mirror_blank) {
        *detail = mirror;
        return PR_MIRROR;
    }
    /* Erased flash is 0xFF, never 0x00; a whole unit of zeros is what an
     * address with no memory behind it reads as on this family. */
    if (zeros) return PR_ZEROS;
    if (!blank) return PR_NOTBLANK;

    for (i = 0; i < block; i++) want[i] = pattern(addr + i * 4u);

    rc = program_block(addr, want, block);
    if (rc != 0) {
        *detail = rc;
        return PR_PROGERR;
    }
    for (i = 0; i < block; i++) {
        if (p[i] != want[i]) {
            *detail = addr + i * 4u;
            return PR_MISMATCH;
        }
    }

    /* A mirror of a blank page compares equal either way, so the write
     * itself is the test: if the pattern also turned up down there, the
     * two addresses are one set of cells. */
    aliased = 0;
    if (mirror && mirror_blank) {
        aliased = 1;
        for (i = 0; i < block; i++) {
            if (m[i] != want[i]) { aliased = 0; break; }
        }
    }

    rc = unit_erase(addr);
    if (rc != 0) {
        *detail = rc;
        return PR_ERASEERR;
    }
    for (i = 0; i < block; i++) {
        if (p[i] != 0xFFFFFFFFUL) {
            *detail = addr + i * 4u;
            return PR_NOTERASED;
        }
    }

    if (aliased) {
        *detail = mirror;
        return PR_ALIAS;
    }
    return PR_OK;
}

static void print_result(const freya_api_t *api, int rc, uint32_t detail)
{
    switch (rc) {
    case PR_OK:
        api->puts("ok\r\n");
        break;
    case PR_MIRROR:
        api->printf("reads back as 0x%08x - a mirror, not new flash\r\n",
                    detail);
        break;
    case PR_ZEROS:
        api->puts("reads as all zeros - nothing answers here\r\n");
        break;
    case PR_NOTBLANK:
        api->puts("not blank - something is stored there, leaving it alone\r\n");
        break;
    case PR_KERNEL:
        api->puts("below the program region - not writing near the kernel\r\n");
        break;
    case PR_NOERASE:
        api->puts("past the last erase unit the controller can name\r\n");
        break;
    case PR_PROGERR:
        api->printf("the write was refused (%s)\r\n", err_str(detail));
        break;
    case PR_MISMATCH:
        api->printf("wrote, read back something else at 0x%08x\r\n", detail);
        break;
    case PR_ALIAS:
        api->printf("the write landed at 0x%08x too - one set of cells\r\n",
                    detail);
        break;
    case PR_ERASEERR:
        api->printf("programmed, then would not erase (%s) - the block is "
                    "left behind\r\n", err_str(detail));
        break;
    case PR_NOTERASED:
        api->printf("the erase reported no error and 0x%08x is still "
                    "programmed\r\n", detail);
        break;
    default:
        api->puts("unknown result\r\n");
        break;
    }
}

/* ========================================================== the shell = */
/* The program region has no libc, so digits are converted by hand. */
static uint32_t parse_uint(const char *s, uint32_t fallback)
{
    uint32_t v = 0;
    int digits = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s++ - '0');
        digits++;
    }
    return (digits && *s == '\0') ? v : fallback;
}

static int running_from_flash(void)
{
    uintptr_t a = (uintptr_t)(void *)&pattern;

    return a >= FLASH_ORIGIN && a < 0x20000000UL;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    uint32_t declared_kib = FLASHSIZE_REG;
    uint32_t declared, ceiling, addr, end, proven;
    uint32_t reported;
    int i, floor_only, stopped = 0;

    /*
     * An erase stalls every read of flash, instruction fetch included, so
     * the code doing it has to be somewhere else.  Both a card-loaded image
     * and an installed image copied and relocated by the loader are in RAM.
     * Keep the check so an old loader fails safely instead of hanging.
     */
    if (running_from_flash()) {
        api->puts("flashprobe: this one has to run from RAM - an erase stalls\r\n"
                  "            instruction fetch from flash.  Use 'run "
                  "flashprobe.bin'.\r\n");
        return FREYA_EXIT_FAIL;
    }

    reported = declared_kib;
    if (declared_kib == 0 || declared_kib == 0xFFFFu) declared_kib = DECLARED_KIB;
    if (declared_kib < FLASH_MIN_KIB) declared_kib = FLASH_MIN_KIB;
    declared = declared_kib * 1024u;

    proven  = FLASH_ORIGIN + declared;
    ceiling = 2u * declared_kib;
    for (i = 1; i < argc; i++) ceiling = parse_uint(argv[i], ceiling);
    if (ceiling > PROBE_MAX_KIB) ceiling = PROBE_MAX_KIB;
    if (ceiling < declared_kib)  ceiling = declared_kib;
    ceiling *= 1024u;
    end = FLASH_ORIGIN + ceiling;

    api->printf("flashprobe: %s reports %u KiB", MCU_NAME, reported);
    if (reported == 0 || reported == 0xFFFFu)
        api->printf(" (unreadable - assuming %u)", declared_kib);
    else if (reported != declared_kib)
        api->printf(" (using %u)", declared_kib);
    api->printf(", probing to %u KiB\r\n", ceiling / 1024u);
    api->puts("flashprobe: each step writes 256 B into an erase unit that "
              "reads blank,\r\n"
              "            compares it, and erases the unit again\r\n");

    /* The last unit inside the declared flash goes first: it is flash that
     * certainly exists, so it says whether the procedure works before any
     * of its answers higher up are believed. */
    for (addr = unit_base(FLASH_ORIGIN + declared - 1u); addr < end; ) {
        uint32_t size = unit_size(addr);
        uint32_t detail = 0;
        uint32_t mirror = 0;
        int reference = addr < FLASH_ORIGIN + declared;
        int rc;

        if (api->should_stop()) { stopped = 1; break; }

        if (!reference)
            mirror = FLASH_ORIGIN + (addr - FLASH_ORIGIN) % declared;

        /* The address goes out before the step rather than after it: a
         * read of flash that is not there is a BusFault on some parts,
         * and then this line is the answer. */
        if (reference) api->printf("  0x%08x  %10s  ", addr, "reference");
        else api->printf("  0x%08x  %6u KiB  ", addr,
                         (addr + size - FLASH_ORIGIN) / 1024u);

        /* Unlocked for one step at a time, so that a Ctrl-C between two of
         * them cannot leave the controller open behind us. */
        if (probe_begin() != 0) {
            probe_end();
            api->puts("the flash controller would not unlock\r\n");
            break;
        }
        rc = probe_unit(addr, size, mirror, &detail);
        probe_end();
        print_result(api, rc, detail);

        /* Every error either family reports is write-1-to-clear and the
         * controller re-arms with the next unlock, so there is nothing to
         * recover from here.  A poll that never saw BSY drop is the one
         * exception, and it is the shell's problem rather than ours. */
        if ((rc == PR_PROGERR || rc == PR_ERASEERR) && (detail & ERR_TIMEOUT))
            api->puts("flashprobe: the controller never went idle - 'reboot' "
                      "before anything else writes flash\r\n");

        addr += size;
        if (!reference) {
            if (rc != PR_OK) break;
            proven = addr;
        }
    }

    /* Only a step that failed says where the flash ends.  Running out of
     * ceiling, or out of patience, gives a floor and not a size. */
    floor_only = stopped || proven >= end;
    if (stopped) api->printf("flashprobe: stopped at 0x%08x\r\n", addr);

    api->printf("flashprobe: %s%u KiB of flash", floor_only ? "at least " : "",
                (proven - FLASH_ORIGIN) / 1024u);
    if (proven > FLASH_ORIGIN + declared)
        api->printf(" - %u KiB more than the chip declares\r\n",
                    (proven - FLASH_ORIGIN) / 1024u - declared_kib);
    else
        api->puts(" - the declared size is all of it\r\n");
    if (floor_only && !stopped)
        api->puts("flashprobe: that is where the probe was told to stop, not "
                  "where the flash is\r\n");

    return FREYA_EXIT_OK;
}
