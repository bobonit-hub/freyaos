/*
 * Freya - internal flash programming for the STM32U585.
 *
 * Only the program flash region and the system settings area are writable
 * through here, and that is enforced in one function.  A mistake in a page
 * address is the difference between a failed install and a board that no
 * longer boots, so every erase and every program goes through in_region()
 * first, and nothing in freya_api_t reaches this file: a program cannot
 * rewrite the kernel that is running it.
 *
 * The U5 erases 8 KiB pages and programs a quad-word, 16 bytes, at once.
 * Each quad-word carries ECC and may be written only once after an erase,
 * so a write that ends part way into one keeps that tail back, padded
 * with 0xFF, until the next write continues it or flash_end() flushes it.
 * Every caller writes a range in ascending order after erasing it, which
 * is the one pattern this has to serve.
 *
 * The chip can read one bank while the other is busy, but the kernel and
 * the program region share bank 1, so the routines that wait on the
 * controller are placed in .ramfunc, linked for the base of the program
 * RAM region and copied there by flash_begin(), as on the other boards.
 * Calling them before that executes whatever happens to be in RAM, which
 * is why s_ready gates them.  Interrupts are masked per operation, so
 * the console handler drains its buffer between pages; some console
 * input is still lost while a page erases.
 */
#include "freya.h"

/* An 8 KiB page erase takes up to 3.4 ms; this is a generous bound on
 * the poll loop at 160 MHz, not a timing reference. */
#define FLASH_SPIN_LIMIT    50000000UL

#define BANK_SIZE           (FLASH_BANK2_BASE - 0x08000000UL)

extern char __ramfunc_start[], __ramfunc_end[], __ramfunc_load[];
extern char __app_flash_start[], __app_flash_end[];
extern char __settings_start[], __settings_end[];

static int s_ready;

/* The quad-word a write ended inside of: its address, and its bytes so
 * far, the rest still 0xFF.  s_pend_at is zero when nothing is held. */
static uint32_t s_pend_at;
static uint32_t s_pend[FLASH_QUADWORD / 4];

/* --------------------------------------------------- RAM resident core */
/*
 * These execute from SRAM.  They must not call anything that lives in
 * flash and must not touch a global in .data or .bss beyond what the
 * compiler keeps in registers; FLASH_R is a constant address and the
 * literal pool travels with the section.
 */
__attribute__((section(".ramfunc"), noinline, used))
static int ram_wait_idle(void)
{
    uint32_t spin = FLASH_SPIN_LIMIT;
    uint32_t sr;

    while (FLASH_R->NSSR & (FLASH_SR_BSY | FLASH_SR_WDW)) {
        if (--spin == 0) return FLASH_ERR_TIMEOUT;
    }
    sr = FLASH_R->NSSR;
    FLASH_R->NSSR = sr & (FLASH_SR_ERRORS | FLASH_SR_EOP);
    if (sr & FLASH_SR_WRPERR) return FLASH_ERR_PROTECTED;
    if (sr & FLASH_SR_ERRORS) return FLASH_ERR_PROG;
    return FLASH_OK;
}

__attribute__((section(".ramfunc"), noinline, used))
static int ram_erase_page(uint32_t addr)
{
    uint32_t off = addr - 0x08000000UL;
    uint32_t cr = FLASH_CR_PER | FLASH_CR_PNB((off % BANK_SIZE) / BOARD_FLASH_PAGE_SIZE);
    uint32_t pm;
    int rc;

    if (off >= BANK_SIZE) cr |= FLASH_CR_BKER;

    pm = irq_save();
    rc = ram_wait_idle();
    if (rc == FLASH_OK) {
        FLASH_R->NSCR = cr;
        FLASH_R->NSCR = cr | FLASH_CR_STRT;
        rc = ram_wait_idle();
        FLASH_R->NSCR = 0;
    }
    irq_restore(pm);
    return rc;
}

/* Four words into one aligned quad-word: the controller starts on the
 * fourth store. */
__attribute__((section(".ramfunc"), noinline, used))
static int ram_program_quad(uint32_t addr, const uint32_t *w)
{
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)addr;
    uint32_t pm;
    int rc;

    pm = irq_save();
    rc = ram_wait_idle();
    if (rc == FLASH_OK) {
        FLASH_R->NSCR = FLASH_CR_PG;
        dst[0] = w[0];
        dst[1] = w[1];
        dst[2] = w[2];
        dst[3] = w[3];
        rc = ram_wait_idle();
        FLASH_R->NSCR = 0;
        /* BSY is clear here, so reading the cells back is safe. */
        if (rc == FLASH_OK &&
            (dst[0] != w[0] || dst[1] != w[1] || dst[2] != w[2] || dst[3] != w[3]))
            rc = FLASH_ERR_VERIFY;
    }
    irq_restore(pm);
    return rc;
}

/* ------------------------------------------------------------- bounds */
/*
 * The single place that decides whether an address may be written.  Both
 * ends are checked against a reserved region, and the arithmetic cannot
 * wrap because len is bounded first.  The two writable regions are the
 * system settings and the program flash; a length that would span both
 * is refused, so an install cannot touch the settings.  Each starts a
 * page of its own, and the kernel extension lies between them, outside
 * both.
 */
static int in_slot(uint32_t addr, uint32_t len, uint32_t base, uint32_t size)
{
    if (len == 0 || len > size) return 0;
    if (addr < base) return 0;
    if (addr > base + size - len) return 0;
    return 1;
}

static int in_region(uint32_t addr, uint32_t len)
{
    return in_slot(addr, len, FREYA_SETTINGS_ADDR, FREYA_SETTINGS_SIZE) ||
           in_slot(addr, len, FREYA_APP_FLASH_ADDR, FREYA_APP_FLASH_SIZE);
}

/* --------------------------------------------------------------- setup */
uint32_t flash_page_size(void)
{
    return BOARD_FLASH_PAGE_SIZE;
}

int flash_begin(void)
{
    /*
     * A linker script cannot include freya_api.h, so the region addresses
     * exist both there and in the header.  This is where the two are
     * compared: a mismatch means an erase would land somewhere nobody
     * intended, and refusing is the only safe answer.
     */
    if ((uint32_t)(uintptr_t)__app_flash_start != FREYA_APP_FLASH_ADDR ||
        (uint32_t)(uintptr_t)__app_flash_end !=
            FREYA_APP_FLASH_ADDR + FREYA_APP_FLASH_SIZE)
        return FLASH_ERR_RANGE;
    if ((uint32_t)(uintptr_t)__settings_start != FREYA_SETTINGS_ADDR ||
        (uint32_t)(uintptr_t)__settings_end !=
            FREYA_SETTINGS_ADDR + FREYA_SETTINGS_SIZE)
        return FLASH_ERR_RANGE;

    if (g_app.loaded || g_app.running) return FLASH_ERR_BUSY;

    memcpy(__ramfunc_start, __ramfunc_load,
           (size_t)(__ramfunc_end - __ramfunc_start));
    __dsb();
    __isb();

    FLASH_R->NSKEYR = FLASH_KEY1;
    FLASH_R->NSKEYR = FLASH_KEY2;
    if (FLASH_R->NSCR & FLASH_CR_LOCK) return FLASH_ERR_LOCKED;

    FLASH_R->NSSR = FLASH_SR_ERRORS | FLASH_SR_EOP;
    s_pend_at = 0;
    s_ready = 1;
    return FLASH_OK;
}

static int pend_flush(void)
{
    int rc;

    if (!s_pend_at) return FLASH_OK;
    rc = ram_program_quad(s_pend_at, s_pend);
    s_pend_at = 0;
    return rc;
}

void flash_end(void)
{
    if (s_ready) (void)pend_flush();
    FLASH_R->NSCR = FLASH_CR_LOCK;
    s_ready = 0;

    /*
     * The region was just written as data and will later be executed, so
     * the instruction cache has to forget what it held for it.
     */
    ICACHE->FCR = ICACHE_FCR_CBSYENDF;
    ICACHE->CR |= ICACHE_CR_CACHEINV;
    while (ICACHE->SR & ICACHE_SR_BUSYF) { }
    __dsb();
    __isb();
}

/* --------------------------------------------------------- erase/write */
/*
 * Any write that touches a page erases that whole page, the way the F4
 * boards erase whole sectors: both writable areas begin on a page of
 * their own, and the settings page holds nothing else.
 */
int flash_erase(uint32_t addr, uint32_t len)
{
    uint32_t page = BOARD_FLASH_PAGE_SIZE;
    uint32_t end, a;

    if (!s_ready) return FLASH_ERR_LOCKED;
    if (!in_region(addr, len)) return FLASH_ERR_RANGE;
    if (addr & 3U) return FLASH_ERR_ALIGN;

    end = addr + len;
    a = addr & ~(page - 1);
    if (a < FREYA_SETTINGS_ADDR ||
        (a > FREYA_SETTINGS_ADDR && a < FREYA_APP_FLASH_ADDR))
        return FLASH_ERR_RANGE;

    s_pend_at = 0;
    for (; a < end; a += page) {
        int rc = ram_erase_page(a);
        if (rc != FLASH_OK) return rc;
    }
    return FLASH_OK;
}

int flash_program(uint32_t addr, const void *src, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)src;

    if (!s_ready) return FLASH_ERR_LOCKED;
    if (!in_region(addr, len)) return FLASH_ERR_RANGE;
    if (addr & 3U) return FLASH_ERR_ALIGN;

    while (len > 0) {
        uint32_t quad = addr & ~(FLASH_QUADWORD - 1U);
        uint32_t off = addr - quad;
        uint32_t n = FLASH_QUADWORD - off;
        int rc;

        if (n > len) n = len;
        /* A held quad-word that this write does not continue goes first;
         * one it does continue gets the new bytes. */
        if (s_pend_at && s_pend_at != quad) {
            rc = pend_flush();
            if (rc != FLASH_OK) return rc;
        }
        if (!s_pend_at) {
            memset(s_pend, 0xFF, sizeof s_pend);
            s_pend_at = quad;
        }
        memcpy((uint8_t *)s_pend + off, p, n);
        if (off + n == FLASH_QUADWORD) {
            rc = pend_flush();
            if (rc != FLASH_OK) return rc;
        }

        addr += n;
        p    += n;
        len  -= n;
    }
    return FLASH_OK;
}

const char *flash_err_str(int rc)
{
    switch (rc) {
    case FLASH_OK:            return "ok";
    case FLASH_ERR_RANGE:     return "address out of range";
    case FLASH_ERR_ALIGN:     return "misaligned address";
    case FLASH_ERR_LOCKED:    return "flash is locked";
    case FLASH_ERR_BUSY:      return "a program is loaded";
    case FLASH_ERR_PROG:      return "programming error";
    case FLASH_ERR_PROTECTED: return "page is write protected";
    case FLASH_ERR_VERIFY:    return "read back does not match";
    case FLASH_ERR_TIMEOUT:   return "flash controller timeout";
    default:                  return "unknown error";
    }
}
