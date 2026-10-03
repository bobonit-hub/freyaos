/*
 * Freya - internal flash programming for the AT32F403ACGU7.
 *
 * Only the program flash region and the system settings area are writable
 * through here, and that is enforced in one function.  A mistake in a page
 * address is the difference between a failed install and a board that no
 * longer boots, so every erase and every program goes through in_region()
 * first, and nothing in freya_api_t reaches this file: a program cannot
 * rewrite the kernel that is running it.
 *
 * The controller is the F103's, twice: one set of registers for each
 * 512 KiB bank, chosen by the address.  A read of either bank while one
 * is busy stalls the bus, so the routines that wait on it are placed in
 * .ramfunc, linked for the base of the program RAM region and copied
 * there by flash_begin().  Calling them before that executes whatever
 * happens to be in RAM, which is why s_ready gates them.
 *
 * What the RAM routines cannot fix is the console: the USART2 handler is
 * itself in flash, so it stalls with everything else.  Interrupts are
 * therefore masked per operation rather than across a whole install, which
 * lets the handler drain the receive buffer between pages; some console
 * input is still lost, and the software clock loses roughly the time the
 * install takes.
 */
#include "freya.h"

/* A 2 KiB page erase takes 50 ms; this is a generous bound on the poll
 * loop at 240 MHz, not a timing reference. */
#define FLASH_SPIN_LIMIT    50000000UL

extern char __ramfunc_start[], __ramfunc_end[], __ramfunc_load[];
extern char __app_flash_start[], __app_flash_end[];
extern char __settings_start[], __settings_end[];

static int s_ready;

/* --------------------------------------------------- RAM resident core */
/*
 * These execute from SRAM.  They must not call anything that lives in
 * flash and must not touch a global in .data or .bss beyond what the
 * compiler keeps in registers; FLASH_R and FLASH_R2 are constant
 * addresses and the literal pool travels with the section.
 */
#define BANK_OF(addr) ((addr) >= FLASH_BANK2_BASE ? FLASH_R2 : FLASH_R)

__attribute__((section(".ramfunc"), noinline, used))
static int ram_wait_idle(FLASH_TypeDef *fl)
{
    uint32_t spin = FLASH_SPIN_LIMIT;

    while (fl->SR & FLASH_SR_BSY) {
        if (--spin == 0) return FLASH_ERR_TIMEOUT;
    }
    if (fl->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) {
        int rc = (fl->SR & FLASH_SR_WRPRTERR) ? FLASH_ERR_PROTECTED
                                              : FLASH_ERR_PROG;
        fl->SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;
        return rc;
    }
    fl->SR = FLASH_SR_EOP;
    return FLASH_OK;
}

__attribute__((section(".ramfunc"), noinline, used))
static int ram_erase_page(uint32_t addr)
{
    FLASH_TypeDef *fl = BANK_OF(addr);
    uint32_t pm;
    int rc;

    pm = irq_save();
    rc = ram_wait_idle(fl);
    if (rc == FLASH_OK) {
        fl->CR |= FLASH_CR_PER;
        fl->AR  = addr;
        fl->CR |= FLASH_CR_STRT;
        rc = ram_wait_idle(fl);
        fl->CR &= ~(FLASH_CR_PER | FLASH_CR_STRT);
    }
    irq_restore(pm);
    return rc;
}

/* A word or a halfword: the controller takes the width of the store.
 * A word is one 50 us programming cycle where two halfwords are two. */
__attribute__((section(".ramfunc"), noinline, used))
static int ram_program(uint32_t addr, uint32_t val, int word)
{
    FLASH_TypeDef *fl = BANK_OF(addr);
    uint32_t pm;
    int rc;

    pm = irq_save();
    rc = ram_wait_idle(fl);
    if (rc == FLASH_OK) {
        fl->CR |= FLASH_CR_PG;
        if (word) *(volatile uint32_t *)(uintptr_t)addr = val;
        else      *(volatile uint16_t *)(uintptr_t)addr = (uint16_t)val;
        rc = ram_wait_idle(fl);
        fl->CR &= ~FLASH_CR_PG;
        /* BSY is clear here, so reading the cells back is safe. */
        if (rc == FLASH_OK &&
            (word ? *(volatile uint32_t *)(uintptr_t)addr != val
                  : *(volatile uint16_t *)(uintptr_t)addr != (uint16_t)val))
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
 * system settings page and the program flash; a length that would span
 * both is refused, so an install cannot touch the settings.  Each starts
 * a 2 KiB page of its own, and the kernel extension lies between them,
 * outside both.
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

    FLASH_R->KEYR = FLASH_KEY1;
    FLASH_R->KEYR = FLASH_KEY2;
    FLASH_R2->KEYR = FLASH_KEY1;
    FLASH_R2->KEYR = FLASH_KEY2;
    if ((FLASH_R->CR | FLASH_R2->CR) & FLASH_CR_LOCK) return FLASH_ERR_LOCKED;

    FLASH_R->SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;
    FLASH_R2->SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;
    s_ready = 1;
    return FLASH_OK;
}

void flash_end(void)
{
    FLASH_R->CR &= ~(FLASH_CR_PG | FLASH_CR_PER);
    FLASH_R->CR |= FLASH_CR_LOCK;
    FLASH_R2->CR &= ~(FLASH_CR_PG | FLASH_CR_PER);
    FLASH_R2->CR |= FLASH_CR_LOCK;
    s_ready = 0;

    /*
     * The region was just written as data and will later be executed, so
     * the write has to be visible to instruction fetch.  A barrier is all
     * this chip needs: it has no instruction cache to invalidate.
     */
    __dsb();
    __isb();
}

/* --------------------------------------------------------- erase/write */
/*
 * Staging for a page erase that must keep bytes outside the range.  The
 * program RAM region is free while flash is open.
 */
static uint8_t *page_scratch(uint32_t page)
{
    uintptr_t a = ((uintptr_t)__ramfunc_end + 3U) & ~(uintptr_t)3U;

    if (a + page > (uintptr_t)__app_ram_end) return 0;
    return (uint8_t *)a;
}

int flash_erase(uint32_t addr, uint32_t len)
{
    uint32_t page = BOARD_FLASH_PAGE_SIZE;
    uint32_t end, a;
    uint8_t *scratch;

    if (!s_ready) return FLASH_ERR_LOCKED;
    if (!in_region(addr, len)) return FLASH_ERR_RANGE;
    if (addr & 1U) return FLASH_ERR_ALIGN;

    end = addr + len;
    a = addr & ~(page - 1);
    /* Both writable areas begin on a page boundary, so rounding down
     * never lands in the kernel or in the kernel extension. */
    if (in_slot(addr, len, FREYA_SETTINGS_ADDR, FREYA_SETTINGS_SIZE)) {
        if (a < FREYA_SETTINGS_ADDR) return FLASH_ERR_RANGE;
    } else if (a < FREYA_APP_FLASH_ADDR) {
        return FLASH_ERR_RANGE;
    }

    scratch = page_scratch(page);

    for (; a < end; a += page) {
        uint32_t page_end = a + page;
        /* The settings are 1 KiB at the start of a 2 KiB page that holds
         * nothing else, so its upper half has nothing to keep. */
        uint32_t keep_end = (a == FREYA_SETTINGS_ADDR)
                          ? FREYA_SETTINGS_ADDR + FREYA_SETTINGS_SIZE
                          : page_end;
        uint32_t wipe_s = (addr > a) ? addr : a;
        uint32_t wipe_e = (end < page_end) ? end : page_end;
        uint32_t keep_lo = wipe_s - a;
        uint32_t keep_hi = (keep_end > wipe_e) ? keep_end - wipe_e : 0;
        int rc;

        if (keep_lo == 0 && keep_hi == 0) {
            rc = ram_erase_page(a);
            if (rc != FLASH_OK) return rc;
            continue;
        }

        if (!scratch) return FLASH_ERR_RANGE;
        if ((keep_lo && !in_region(a, keep_lo)) ||
            (keep_hi && !in_region(wipe_e, keep_hi)))
            return FLASH_ERR_RANGE;

        memcpy(scratch, (const void *)(uintptr_t)a, page);
        rc = ram_erase_page(a);
        if (rc != FLASH_OK) return rc;
        if (keep_lo) {
            rc = flash_program(a, scratch, keep_lo);
            if (rc != FLASH_OK) return rc;
        }
        if (keep_hi) {
            rc = flash_program(wipe_e, scratch + (wipe_e - a), keep_hi);
            if (rc != FLASH_OK) return rc;
        }
    }
    return FLASH_OK;
}

int flash_program(uint32_t addr, const void *src, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)src;

    if (!s_ready) return FLASH_ERR_LOCKED;
    if (!in_region(addr, len)) return FLASH_ERR_RANGE;
    if (addr & 1U) return FLASH_ERR_ALIGN;

    while (len > 0) {
        /* Built byte by byte: the source may be an odd offset into a card
         * buffer, and a trailing odd byte is padded the way erased flash
         * already reads.  Whole aligned words go in one cycle. */
        int word = !(addr & 3U) && len >= 4;
        uint32_t v;
        int rc;

        if (word)
            v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        else
            v = (len > 1) ? (uint32_t)(p[0] | ((uint32_t)p[1] << 8))
                          : (uint32_t)(p[0] | 0xFF00U);
        rc = ram_program(addr, v, word);
        if (rc != FLASH_OK) return rc;

        addr += word ? 4U : 2U;
        p    += word ? 4U : 2U;
        len   = (len > (word ? 4U : 2U)) ? len - (word ? 4U : 2U) : 0;
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
