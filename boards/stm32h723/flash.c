/*
 * Freya - internal flash programming for the STM32H723.
 *
 * Only the program flash region and the system settings area are writable
 * through here, and that is enforced in one function.  A mistake in a page
 * address is the difference between a failed install and a board that no
 * longer boots, so every erase and every program goes through in_region()
 * first, and nothing in freya_api_t reaches this file: a program cannot
 * rewrite the kernel that is running it.
 *
 * The H7 erases 128 KiB sectors and programs a flash word, 32 bytes, at
 * once.  Each flash word carries ECC and may be written only once after an
 * erase, so a write that ends part way into one keeps that tail back,
 * padded with 0xFF, until the next write continues it or flash_end()
 * flushes it.  This is boards/stm32u585/flash.c with the H7's registers
 * and the wider word.
 * Every caller writes a range in ascending order after erasing it, which
 * is the one pattern this has to serve.
 *
 * The chip has one bank, so a busy controller stalls every flash read,
 * and the routines that wait on it are placed in .ramfunc, linked for the
 * base of the program RAM region and copied there by flash_begin(), as
 * on the other boards.
 * Calling them before that executes whatever happens to be in RAM, which
 * is why s_ready gates them.  Interrupts are masked per operation, so
 * the console handler drains its buffer between flash words; console
 * input is lost while a sector erases.
 */
#include "freya.h"

/* A 128 KiB sector erase is specified at up to about two seconds; this
 * is a generous bound on the poll loop at 520 MHz, not a timing
 * reference. */
#define FLASH_SPIN_LIMIT    2000000000UL

#define FLASH_QUADWORD      FLASH_WORD      /* the unit held back below */

extern char __ramfunc_start[], __ramfunc_end[], __ramfunc_load[];
extern char __app_flash_start[], __app_flash_end[];
extern char __settings_start[], __settings_end[];

static int s_ready;

/* The flash word a write ended inside of: its address, and its bytes so
 * far, the rest still 0xFF.  s_pend_at is zero when nothing is held. */
static uint32_t s_pend_at;
static uint32_t s_pend[FLASH_QUADWORD / 4];

/* --------------------------------------------------- RAM resident core */
/*
 * These execute from SRAM.  They must not call anything that lives in
 * flash and must not touch a global in .data or .bss beyond what the
 * compiler keeps in registers; FLASH_R is a constant address and the
 * literal pool travels with the section.  The instruction cache is
 * invalidated after they are copied, so it cannot hold an older copy.
 */
__attribute__((section(".ramfunc"), noinline, used))
static int ram_wait_idle(void)
{
    uint32_t spin = FLASH_SPIN_LIMIT;
    uint32_t sr;

    while (FLASH_R->SR1 & (FLASH_SR_BSY | FLASH_SR_QW | FLASH_SR_WBNE)) {
        if (--spin == 0) return FLASH_ERR_TIMEOUT;
    }
    sr = FLASH_R->SR1;
    FLASH_R->CCR1 = sr & (FLASH_SR_ERRORS | FLASH_SR_EOP);
    if (sr & FLASH_SR_WRPERR) return FLASH_ERR_PROTECTED;
    if (sr & FLASH_SR_ERRORS) return FLASH_ERR_PROG;
    return FLASH_OK;
}

__attribute__((section(".ramfunc"), noinline, used))
static int ram_erase_sector(uint32_t addr)
{
    uint32_t cr = FLASH_CR_SER | FLASH_CR_PSIZE_X32 |
                  FLASH_CR_SNB((addr - 0x08000000UL) / BOARD_FLASH_PAGE_SIZE);
    uint32_t pm;
    int rc;

    pm = irq_save();
    rc = ram_wait_idle();
    if (rc == FLASH_OK) {
        FLASH_R->CR1 = cr;
        FLASH_R->CR1 = cr | FLASH_CR_START;
        rc = ram_wait_idle();
        FLASH_R->CR1 = 0;
    }
    irq_restore(pm);
    return rc;
}

/* Eight words into one aligned flash word: the controller starts on the
 * eighth store.  The stores are fenced so the core does not merge or
 * reorder them on the way. */
__attribute__((section(".ramfunc"), noinline, used))
static int ram_program_quad(uint32_t addr, const uint32_t *w)
{
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)addr;
    uint32_t pm;
    int rc;

    pm = irq_save();
    rc = ram_wait_idle();
    if (rc == FLASH_OK) {
        FLASH_R->CR1 = FLASH_CR_PG | FLASH_CR_PSIZE_X32;
        __isb();
        __dsb();
        for (int i = 0; i < 8; i++) dst[i] = w[i];
        __isb();
        __dsb();
        rc = ram_wait_idle();
        FLASH_R->CR1 = 0;
        /* BSY is clear here, so reading the cells back is safe. */
        for (int i = 0; rc == FLASH_OK && i < 8; i++)
            if (dst[i] != w[i]) rc = FLASH_ERR_VERIFY;
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
 * sector of its own, and the kernel extension is the last sector, outside
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
    icache_invalidate();

    FLASH_R->KEYR1 = FLASH_KEY1;
    FLASH_R->KEYR1 = FLASH_KEY2;
    if (FLASH_R->CR1 & FLASH_CR_LOCK) return FLASH_ERR_LOCKED;

    FLASH_R->CCR1 = FLASH_SR_ERRORS | FLASH_SR_EOP;
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
    FLASH_R->CR1 = FLASH_CR_LOCK;
    s_ready = 0;

    /*
     * The region was just written as data and will later be executed, so
     * the instruction cache has to forget what it held for it.
     */
    icache_invalidate();
}

/* --------------------------------------------------------- erase/write */
/*
 * Any write that touches a sector erases that whole sector, the way the
 * F4 boards do: both writable areas begin on a sector of their own, and
 * the settings sector holds nothing else.
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
        int rc = ram_erase_sector(a);
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
        /* A held flash word that this write does not continue goes first;
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
