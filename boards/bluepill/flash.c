/*
 * Freya - internal flash programming for the STM32F103C8T6.
 *
 * Only the program flash region and the auto-start flag slot are writable
 * through here, and that is enforced in one function.  A mistake in a page
 * address is the difference between a failed install and a board that no
 * longer boots, so every erase and every program goes through in_region()
 * first, and nothing in freya_api_t reaches this file: a program cannot
 * rewrite the kernel that is running it.
 *
 * The F103 has no read-while-write.  While FLASH_SR.BSY is set the flash
 * controller stalls bus reads, so the two routines that wait on it are
 * placed in .ramfunc, linked for the base of the program RAM region and
 * copied there by flash_begin().  Calling them before that executes
 * whatever happens to be in RAM, which is why s_ready gates them.
 *
 * What the RAM routines cannot fix is the console: the USART2 handler is
 * itself in flash, so it stalls with everything else.  Interrupts are
 * therefore masked per operation rather than across a whole install, which
 * lets the handler drain the receive buffer between pages; some console
 * input is still lost, and the software clock loses roughly the time the
 * install takes.
 */
#include "freya.h"

/* 40 ms is the worst case page erase; this is a generous bound on the
 * poll loop, not a timing reference. */
#define FLASH_SPIN_LIMIT    2000000UL

extern char __ramfunc_start[], __ramfunc_end[], __ramfunc_load[];
extern char __app_flash_start[], __app_flash_end[];
extern char __autostart_start[], __autostart_end[];

static int s_ready;

/* --------------------------------------------------- RAM resident core */
/*
 * These two execute from SRAM.  They must not call anything that lives in
 * flash and must not touch a global in .data or .bss beyond what the
 * compiler keeps in registers; FLASH_R is a constant address and the
 * literal pool travels with the section.
 */
__attribute__((section(".ramfunc"), noinline, used))
static int ram_wait_idle(void)
{
    uint32_t spin = FLASH_SPIN_LIMIT;

    while (FLASH_R->SR & FLASH_SR_BSY) {
        if (--spin == 0) return FLASH_ERR_TIMEOUT;
    }
    if (FLASH_R->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) {
        int rc = (FLASH_R->SR & FLASH_SR_WRPRTERR) ? FLASH_ERR_PROTECTED
                                                   : FLASH_ERR_PROG;
        FLASH_R->SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;
        return rc;
    }
    FLASH_R->SR = FLASH_SR_EOP;
    return FLASH_OK;
}

__attribute__((section(".ramfunc"), noinline, used))
static int ram_erase_page(uint32_t addr)
{
    uint32_t pm;
    int rc;

    pm = irq_save();
    rc = ram_wait_idle();
    if (rc == FLASH_OK) {
        FLASH_R->CR |= FLASH_CR_PER;
        FLASH_R->AR  = addr;
        FLASH_R->CR |= FLASH_CR_STRT;
        rc = ram_wait_idle();
        FLASH_R->CR &= ~(FLASH_CR_PER | FLASH_CR_STRT);
    }
    irq_restore(pm);
    return rc;
}

__attribute__((section(".ramfunc"), noinline, used))
static int ram_program_half(uint32_t addr, uint16_t val)
{
    uint32_t pm;
    int rc;

    pm = irq_save();
    rc = ram_wait_idle();
    if (rc == FLASH_OK) {
        FLASH_R->CR |= FLASH_CR_PG;
        *(volatile uint16_t *)(uintptr_t)addr = val;
        rc = ram_wait_idle();
        FLASH_R->CR &= ~FLASH_CR_PG;
        /* BSY is clear here, so reading the word back is safe. */
        if (rc == FLASH_OK && *(volatile uint16_t *)(uintptr_t)addr != val)
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
 * 128-byte auto-start slot and the program flash; a length that would
 * span both is refused, so an install cannot touch the flag.  They share
 * a 1 KiB erase page, and flash_erase() writes the other slot back.
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
    return in_slot(addr, len, FREYA_AUTOSTART_ADDR, FREYA_AUTOSTART_SIZE) ||
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
    if ((uint32_t)(uintptr_t)__autostart_start != FREYA_AUTOSTART_ADDR ||
        (uint32_t)(uintptr_t)__autostart_end !=
            FREYA_AUTOSTART_ADDR + FREYA_AUTOSTART_SIZE)
        return FLASH_ERR_RANGE;

    if (g_app.loaded || g_app.running) return FLASH_ERR_BUSY;

    memcpy(__ramfunc_start, __ramfunc_load,
           (size_t)(__ramfunc_end - __ramfunc_start));
    __dsb();
    __isb();

    FLASH_R->KEYR = FLASH_KEY1;
    FLASH_R->KEYR = FLASH_KEY2;
    if (FLASH_R->CR & FLASH_CR_LOCK) return FLASH_ERR_LOCKED;

    FLASH_R->SR = FLASH_SR_PGERR | FLASH_SR_WRPRTERR | FLASH_SR_EOP;
    s_ready = 1;
    return FLASH_OK;
}

void flash_end(void)
{
    FLASH_R->CR &= ~(FLASH_CR_PG | FLASH_CR_PER);
    FLASH_R->CR |= FLASH_CR_LOCK;
    s_ready = 0;

    /*
     * The region was just written as data and will later be executed, so
     * the write has to be visible to instruction fetch.  A barrier is all
     * this board needs: the F1 has a sequential prefetch buffer rather
     * than a cache, and it is dropped on the branch into the program.  A
     * part with an instruction cache - the F411 among them - would have to
     * invalidate it here as well.
     */
    __dsb();
    __isb();
}

/* --------------------------------------------------------- erase/write */
/*
 * Staging for a page that holds both the auto-start slot and the start of
 * the program image.  The program RAM region is free while flash is open.
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
    /* The auto-start slot begins on a page boundary, so rounding down
     * never lands in the kernel. */
    if (a < FREYA_AUTOSTART_ADDR) return FLASH_ERR_RANGE;

    scratch = page_scratch(page);

    for (; a < end; a += page) {
        uint32_t page_end = a + page;
        uint32_t wipe_s = (addr > a) ? addr : a;
        uint32_t wipe_e = (end < page_end) ? end : page_end;
        uint32_t keep_lo = wipe_s - a;
        uint32_t keep_hi = page_end - wipe_e;
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
         * already reads. */
        uint16_t v = (len > 1) ? (uint16_t)(p[0] | ((uint16_t)p[1] << 8))
                               : (uint16_t)(p[0] | 0xFF00U);
        int rc = ram_program_half(addr, v);
        if (rc != FLASH_OK) return rc;

        addr += 2;
        p    += 2;
        len   = (len > 2) ? len - 2 : 0;
    }
    return FLASH_OK;
}

const char *flash_err_str(int rc)
{
    switch (rc) {
    case FLASH_OK:            return "ok";
    case FLASH_ERR_RANGE:     return "address outside a writable flash region";
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
