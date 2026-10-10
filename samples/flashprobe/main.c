/*
 * flashprobe - how much internal flash the chip really has.
 *
 * Every board Freya supports has at least 128 KiB.  An STM32F103C8 often
 * still says 64; that report is raised to 128 before anything is believed,
 * and a write is what settles the question of flash past that floor.
 * Reading extra pages proves nothing, because unimplemented flash reads
 * back as something.  So this program writes.
 *
 *     run("flashprobe.bin")        probe to twice the size used below
 *     run("flashprobe.bin", 256)    probe to 256 KiB
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
    !defined(FREYA_BOARD_STM32F401) && \
    !defined(FREYA_BOARD_STM32F405) && !defined(FREYA_BOARD_BLACKPILL2) && \
    !defined(FREYA_BOARD_STM32U585) && !defined(FREYA_BOARD_STM32H523) && \
    !defined(FREYA_BOARD_STM32H562) && !defined(FREYA_BOARD_STM32H723)
#error "flashprobe drives the flash controller itself and needs a board it knows"
#endif

/* The F411, the F401 and the F405 share one controller and one path. */
#if defined(FREYA_BOARD_BLACKPILL) || defined(FREYA_BOARD_STM32F401) || \
    defined(FREYA_BOARD_STM32F405)
#define FLASH_F4        1
#endif

/* The AT32F403A's controller is the F103's, so the two share a path. */
#if defined(FREYA_BOARD_BLUEPILL) || defined(FREYA_BOARD_BLACKPILL2)
#define FLASH_F1        1
#else
#define FLASH_F1        0
#endif

/* The U5 and the H5 program a quad-word at a time and erase 8 KiB units,
 * and the H7 a 32-byte flash word and 128 KiB sectors, each through its
 * own registers, so the three share a path as well. */
#if defined(FREYA_BOARD_STM32U585) || defined(FREYA_BOARD_STM32H523) || \
    defined(FREYA_BOARD_STM32H562) || defined(FREYA_BOARD_STM32H723)
#define FLASH_QUAD      1
#else
#define FLASH_QUAD      0
#endif

#define FLASH_ORIGIN        0x08000000UL
#define FLASH_MIN_KIB       128u            /* every supported board has this  */
#define BLOCK_BYTES         256u            /* the data block each step writes */
#if defined(FREYA_BOARD_STM32U585)
#define PROBE_MAX_KIB       4096u           /* as far up as this will look     */
#elif defined(FREYA_BOARD_STM32H723)
#define PROBE_MAX_KIB       2048u           /* as far up as this will look     */
#else
#define PROBE_MAX_KIB       1024u           /* as far up as this will look     */
#endif
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

/* ================================================ the AT32F403A == */
#if defined(FREYA_BOARD_BLACKPILL2)

#define MCU_NAME        "AT32F403A"
#define FLASHSIZE_REG   (*(const volatile uint16_t *)0x1FFFF7E0UL)
#define DECLARED_KIB    1024u
#define SPIN_LIMIT      50000000UL          /* 50 ms page erase at 240 MHz */

/* The F103's controller once per 512 KiB bank: the second set of
 * registers is 0x40 bytes above the first, and the address picks which
 * one an operation goes through. */
#define BANK2_BASE      0x08080000UL
static flash_regs_t *s_fl = (flash_regs_t *)0x40022000UL;
#define FL              s_fl
#define FL_BANK1        ((flash_regs_t *)0x40022000UL)
#define FL_BANK2        ((flash_regs_t *)0x40022040UL)

static void bank_select(uint32_t addr)
{
    s_fl = (addr >= BANK2_BASE) ? FL_BANK2 : FL_BANK1;
}

#define SR_BSY          (1UL << 0)
#define SR_PGERR        (1UL << 2)
#define SR_WRPRTERR     (1UL << 4)
#define SR_EOP          (1UL << 5)
#define SR_ERRORS       (SR_PGERR | SR_WRPRTERR)

#define CR_PG           (1UL << 0)
#define CR_PER          (1UL << 1)
#define CR_STRT         (1UL << 6)
#define CR_LOCK         (1UL << 7)

/* 2 KiB pages in both banks. */
static uint32_t unit_size(uint32_t addr)  { (void)addr; return 2048u; }
static uint32_t unit_base(uint32_t addr)  { return addr & ~2047UL; }
static int      unit_erasable(uint32_t addr) { (void)addr; return 1; }

static const char *err_str(uint32_t bits)
{
    if (bits & ERR_TIMEOUT)  return "controller timeout";
    if (bits & SR_WRPRTERR)  return "write protected";
    if (bits & SR_PGERR)     return "programming error";
    return "no error reported";
}

#else
#define bank_select(addr) ((void)(addr))
#endif /* FREYA_BOARD_BLACKPILL2 */

/* ================================================ the STM32U585 === */
#if defined(FREYA_BOARD_STM32U585)

#define MCU_NAME        "STM32U585"
#define FLASHSIZE_REG   (*(const volatile uint16_t *)0x0BFA07A0UL)
#define DECLARED_KIB    2048u
#define SPIN_LIMIT      50000000UL          /* a page erase is milliseconds */

/* The U5's controller keeps its non-secure key, status and control
 * registers further up than the F1 and F4 do; under these names the
 * shared code below drives them unchanged. */
typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t RES0;
    volatile uint32_t KEYR;                 /* NSKEYR */
    volatile uint32_t SECKEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t RES1;
    volatile uint32_t PDKEY1R;
    volatile uint32_t PDKEY2R;
    volatile uint32_t SR;                   /* NSSR   */
    volatile uint32_t SECSR;
    volatile uint32_t CR;                   /* NSCR   */
} u5_flash_regs_t;

#define FL              ((u5_flash_regs_t *)0x40022000UL)
#define BANK_BYTES      0x100000UL          /* two banks of 1 MiB          */

#define SR_EOP          (1UL << 0)
#define SR_OPERR        (1UL << 1)
#define SR_PROGERR      (1UL << 3)
#define SR_WRPERR       (1UL << 4)
#define SR_PGAERR       (1UL << 5)
#define SR_SIZERR       (1UL << 6)
#define SR_PGSERR       (1UL << 7)
#define SR_BSY          (1UL << 16)
#define SR_ERRORS       (SR_OPERR | SR_PROGERR | SR_WRPERR | SR_PGAERR | \
                         SR_SIZERR | SR_PGSERR)

#define CR_PG           (1UL << 0)
#define CR_PER          (1UL << 1)
#define CR_PNB(n)       (((uint32_t)(n) & 0x7FUL) << 3)
#define CR_BKER         (1UL << 11)
#define CR_STRT         (1UL << 16)
#define CR_LOCK         (1UL << 31)

/* Erase the 8 KiB page at offset 'off': PNB names it inside its bank. */
#define CR_ERASE(off)   (CR_PER | CR_PNB(((off) % BANK_BYTES) / 8192u) | \
                         (((off) >= BANK_BYTES) ? CR_BKER : 0))
#define CR_GO           CR_STRT
#define CR_PROG         CR_PG
#define PROG_WORDS      4u                  /* a quad-word                 */

static const char *err_str(uint32_t bits)
{
    if (bits & ERR_TIMEOUT) return "controller timeout";
    if (bits & SR_WRPERR)   return "write protected";
    if (bits & SR_PGSERR)   return "programming sequence error";
    if (bits & SR_SIZERR)   return "programming size error";
    if (bits & SR_PGAERR)   return "programming alignment error";
    if (bits & SR_PROGERR)  return "programming error";
    if (bits & SR_OPERR)    return "operation error";
    return "no error reported";
}

#endif /* FREYA_BOARD_STM32U585 */

/* ======================================= the STM32H523 and H562 === */
#if defined(FREYA_BOARD_STM32H523) || defined(FREYA_BOARD_STM32H562)

#if defined(FREYA_BOARD_STM32H562)
#define MCU_NAME        "STM32H562"
#define DECLARED_KIB    1024u
#define BANK_BYTES      0x80000UL           /* two banks of 512 KiB        */
#else
#define MCU_NAME        "STM32H523"
#define DECLARED_KIB    512u
#define BANK_BYTES      0x40000UL           /* two banks of 256 KiB        */
#endif
#define FLASHSIZE_REG   (*(const volatile uint16_t *)0x08FFF80CUL)
#define SPIN_LIMIT      50000000UL          /* a sector erase is milliseconds */

/* The H5's controller: the non-secure key, status and control registers
 * under the names the shared code uses, and a separate register to clear
 * the status flags. */
typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR;                 /* NSKEYR */
    volatile uint32_t SECKEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t NSOBKKEYR;
    volatile uint32_t SECOBKKEYR;
    volatile uint32_t OPSR;
    volatile uint32_t OPTCR;
    volatile uint32_t SR;                   /* NSSR   */
    volatile uint32_t SECSR;
    volatile uint32_t CR;                   /* NSCR   */
    volatile uint32_t SECCR;
    volatile uint32_t CCR;                  /* NSCCR  */
} h5_flash_regs_t;

#define FL              ((h5_flash_regs_t *)0x40022000UL)
#define FL_CLEAR(bits)  (FL->CCR = (bits))

/* Busy while the controller works or still holds data to write. */
#define SR_BSY          ((1UL << 0) | (1UL << 1) | (1UL << 3))
#define SR_EOP          (1UL << 16)
#define SR_WRPERR       (1UL << 17)
#define SR_PGSERR       (1UL << 18)
#define SR_STRBERR      (1UL << 19)
#define SR_INCERR       (1UL << 20)
#define SR_ERRORS       (SR_WRPERR | SR_PGSERR | SR_STRBERR | SR_INCERR)

#define CR_LOCK         (1UL << 0)
#define CR_PG           (1UL << 1)
#define CR_SER          (1UL << 2)
#define CR_START        (1UL << 5)
#define CR_SNB(n)       (((uint32_t)(n) & 0x7FUL) << 6)
#define CR_BKSEL        (1UL << 31)

/* Erase the 8 KiB sector at offset 'off': SNB names it inside its bank. */
#define CR_ERASE(off)   (CR_SER | CR_SNB(((off) % BANK_BYTES) / 8192u) | \
                         (((off) >= BANK_BYTES) ? CR_BKSEL : 0))
#define CR_GO           CR_START
#define CR_PROG         CR_PG
#define PROG_WORDS      4u                  /* a quad-word                 */

static const char *err_str(uint32_t bits)
{
    if (bits & ERR_TIMEOUT) return "controller timeout";
    if (bits & SR_WRPERR)   return "write protected";
    if (bits & SR_PGSERR)   return "programming sequence error";
    if (bits & SR_STRBERR)  return "strobe error";
    if (bits & SR_INCERR)   return "inconsistency error";
    return "no error reported";
}

#endif /* FREYA_BOARD_STM32H523, FREYA_BOARD_STM32H562 */

/* ================================================ the STM32H723 === */
#if defined(FREYA_BOARD_STM32H723)

#define MCU_NAME        "STM32H723"
#define FLASHSIZE_REG   (*(const volatile uint16_t *)0x1FF1E880UL)
#define DECLARED_KIB    1024u
#define SPIN_LIMIT      2000000000UL        /* a sector erase is seconds   */

/* The H7's bank 1 registers under the names the shared code uses, and a
 * separate register to clear the status flags. */
typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR;                 /* KEYR1 */
    volatile uint32_t OPTKEYR;
    volatile uint32_t CR;                   /* CR1   */
    volatile uint32_t SR;                   /* SR1   */
    volatile uint32_t CCR;                  /* CCR1  */
} h7_flash_regs_t;

#define FL              ((h7_flash_regs_t *)0x52002000UL)
#define FL_CLEAR(bits)  (FL->CCR = (bits))

/* Busy while the controller works or still holds data to write. */
#define SR_BSY          ((1UL << 0) | (1UL << 1) | (1UL << 2))
#define SR_EOP          (1UL << 16)
#define SR_WRPERR       (1UL << 17)
#define SR_PGSERR       (1UL << 18)
#define SR_STRBERR      (1UL << 19)
#define SR_INCERR       (1UL << 21)
#define SR_OPERR        (1UL << 22)
#define SR_ERRORS       (SR_WRPERR | SR_PGSERR | SR_STRBERR | SR_INCERR | \
                         SR_OPERR)

#define CR_LOCK         (1UL << 0)
#define CR_PG           (1UL << 1)
#define CR_SER          (1UL << 2)
#define CR_PSIZE_X32    (2UL << 4)
#define CR_START        (1UL << 7)
#define CR_SNB(n)       (((uint32_t)(n) & 7UL) << 8)

/* Erase the 128 KiB sector at offset 'off'.  One bank, eight sectors. */
#define CR_ERASE(off)   (CR_SER | CR_PSIZE_X32 | CR_SNB((off) / 0x20000UL))
#define CR_GO           CR_START
#define CR_PROG         (CR_PG | CR_PSIZE_X32)
#define PROG_WORDS      8u                  /* a 32-byte flash word        */

static uint32_t unit_size(uint32_t addr)  { (void)addr; return 0x20000u; }
static uint32_t unit_base(uint32_t addr)  { return addr & ~0x1FFFFUL; }
static int      unit_erasable(uint32_t addr)
{
    return addr - FLASH_ORIGIN < 8u * 0x20000UL;
}

static const char *err_str(uint32_t bits)
{
    if (bits & ERR_TIMEOUT) return "controller timeout";
    if (bits & SR_WRPERR)   return "write protected";
    if (bits & SR_PGSERR)   return "programming sequence error";
    if (bits & SR_STRBERR)  return "strobe error";
    if (bits & SR_INCERR)   return "inconsistency error";
    if (bits & SR_OPERR)    return "operation error";
    return "no error reported";
}

#endif /* FREYA_BOARD_STM32H723 */

#if defined(FREYA_BOARD_STM32U585) || defined(FREYA_BOARD_STM32H523) || \
    defined(FREYA_BOARD_STM32H562)
/* 8 KiB units, in two banks. */
static uint32_t unit_size(uint32_t addr)  { (void)addr; return 8192u; }
static uint32_t unit_base(uint32_t addr)  { return addr & ~8191UL; }
static int      unit_erasable(uint32_t addr)
{
    return addr - FLASH_ORIGIN < 2u * BANK_BYTES;
}

/* The instruction cache serves data reads of flash as well, so it is off
 * while the probe runs and invalidated afterwards.  It is at the same
 * address on both. */
#define ICACHE_CR       (*(volatile uint32_t *)0x40030400UL)
#define ICACHE_SR       (*(volatile uint32_t *)0x40030404UL)
#define ICACHE_EN       (1UL << 0)
#define ICACHE_INV      (1UL << 1)
#define ICACHE_BUSY     (1UL << 0)
#endif

/* Every other controller clears a flag by writing it to SR. */
#ifndef FL_CLEAR
#define FL_CLEAR(bits)  (FL->SR = (bits))
#endif

/* ================================== the STM32F411, F401 and F405 == */
#ifdef FLASH_F4

#if defined(FREYA_BOARD_STM32F405)
#define MCU_NAME        "STM32F405"
#elif defined(FREYA_BOARD_STM32F401)
#define MCU_NAME        "STM32F401"
#else
#define MCU_NAME        "STM32F411"
#endif
#define FL              ((flash_regs_t *)0x40023C00UL)
#define FLASHSIZE_REG   (*(const volatile uint16_t *)0x1FFF7A22UL)
#ifdef FREYA_BOARD_STM32F401
#define DECLARED_KIB    256u
#else
#define DECLARED_KIB    512u
#endif
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

#endif /* FLASH_F4 */

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
    FL_CLEAR(SR_ERRORS | SR_EOP);       /* every family clears by writing 1 */
    return sr;
}

static uint32_t unit_erase(uint32_t addr)
{
    uint32_t pm;
    uint32_t rc;

    bank_select(addr);
    pm = irq_off();
    rc = wait_idle();
    if (rc == 0) {
#if FLASH_F1
        FL->CR |= CR_PER;
        FL->AR  = addr;
        FL->CR |= CR_STRT;
        rc = wait_idle();
        FL->CR &= ~(CR_PER | CR_STRT);
#elif FLASH_QUAD
        uint32_t off = addr - FLASH_ORIGIN;

        FL->CR = CR_ERASE(off);
        FL->CR |= CR_GO;
        rc = wait_idle();
        FL->CR = 0;
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

/* The F1 programs a halfword at a time, the F4 a word and the U5 and H5
 * four words at once, so the block is kept as words and taken apart here. */
#if FLASH_QUAD
static uint32_t program_block(uint32_t addr, const uint32_t *w, uint32_t words)
{
    uint32_t rc = 0;
    uint32_t i;

    for (i = 0; i + PROG_WORDS <= words && rc == 0; i += PROG_WORDS) {
        volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)(addr + i * 4U);
        uint32_t pm = irq_off();
        uint32_t k;

        rc = wait_idle();
        if (rc == 0) {
            FL->CR = CR_PROG;
            dsb();
            for (k = 0; k < PROG_WORDS; k++) dst[k] = w[i + k];
            dsb();
            rc = wait_idle();
            FL->CR = 0;
        }
        irq_on(pm);
    }
    return rc;
}
#else
static uint32_t program_block(uint32_t addr, const uint32_t *w, uint32_t words)
{
    uint32_t rc = 0;
    uint32_t i;

    bank_select(addr);
    for (i = 0; i < words && rc == 0; i++) {
        uint32_t pm = irq_off();

        rc = wait_idle();
        if (rc == 0) {
#if FLASH_F1
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
#endif

static int flash_unlock(void)
{
#if defined(FREYA_BOARD_BLACKPILL2)
    FL_BANK2->KEYR = FLASH_KEY1;
    FL_BANK2->KEYR = FLASH_KEY2;
    FL_BANK2->SR = SR_ERRORS | SR_EOP;
    if (FL_BANK2->CR & CR_LOCK) return -1;
    bank_select(FLASH_ORIGIN);
#endif
    FL->KEYR = FLASH_KEY1;
    FL->KEYR = FLASH_KEY2;
    return (FL->CR & CR_LOCK) ? -1 : 0;
}

#ifdef FLASH_F4
/* On the F411 the caches have to go: a read-back that came out of the data
 * cache would compare equal to whatever was there before the erase. */
static uint32_t s_acr;
#endif

static int probe_begin(void)
{
#ifdef FLASH_F4
    s_acr = FL->ACR;
    FL->ACR &= ~(ACR_ICEN | ACR_DCEN);
#endif
#ifdef ICACHE_CR
    ICACHE_CR &= ~ICACHE_EN;
    while (ICACHE_SR & ICACHE_BUSY) { }
#endif
    if (flash_unlock() != 0) return -1;
    FL_CLEAR(SR_ERRORS | SR_EOP);
#ifdef FLASH_F4
    FL->CR = (FL->CR & ~CR_SNB_MASK) | CR_PSIZE_X32;
#endif
    return 0;
}

static void probe_end(void)
{
#if defined(FREYA_BOARD_BLACKPILL2)
    FL_BANK2->CR &= ~(CR_PG | CR_PER);
    FL_BANK2->CR |= CR_LOCK;
    bank_select(FLASH_ORIGIN);
#endif
#if FLASH_F1
    FL->CR &= ~(CR_PG | CR_PER);
#elif FLASH_QUAD
    FL->CR = 0;
#else
    FL->CR &= ~(CR_PG | CR_SER | CR_STRT);
#endif
    FL->CR |= CR_LOCK;
#ifdef ICACHE_CR
    ICACHE_CR |= ICACHE_INV;
    while (ICACHE_SR & ICACHE_BUSY) { }
    ICACHE_CR |= ICACHE_EN;
#endif
#ifdef FLASH_F4
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
                  "            instruction fetch from flash.  Use 'run(\""
                  "flashprobe.bin\")'.\r\n");
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
