/*
 * Freya - internal flash programming for the STM32F411CEU6.
 *
 * Only the program flash region (sector 4) and the auto-start slot (last
 * 128 bytes of sector 3) are writable through here.  Every erase and
 * program goes through in_region() first, and nothing in freya_api_t
 * reaches this file: a program cannot rewrite the kernel that is running
 * it.
 *
 * The F411 has no read-while-write.  Wait loops live in .ramfunc, linked
 * for the program RAM region and copied there by flash_begin().  The
 * instruction and data caches are dropped around an operation and
 * invalidated before fetch from the region that was just programmed.
 *
 * Sectors are unequal, so a 64 KiB program-region erase cannot keep a
 * tail in SRAM the way the F103 keeps a 1 KiB page.  Any write that
 * touches a sector erases that whole sector.  The auto-start slot is in
 * its own 16 KiB sector so that does not take the program with it.
 */
#include "freya.h"

/* 64 KiB sector erase is specified in milliseconds; this is a generous
 * bound on a 96 MHz poll loop, not a timing reference. */
#define FLASH_SPIN_LIMIT    200000000UL

extern char __ramfunc_start[], __ramfunc_end[], __ramfunc_load[];
extern char __app_flash_start[], __app_flash_end[];
extern char __autostart_start[], __autostart_end[];

static int s_ready;
static uint32_t s_acr;

typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t snb;
} flash_sector_t;

/* F411CE: 512 KiB, sectors 0..7. */
static const flash_sector_t s_sectors[] = {
    { 0x08000000UL, 16U * 1024U,  0 },
    { 0x08004000UL, 16U * 1024U,  1 },
    { 0x08008000UL, 16U * 1024U,  2 },
    { 0x0800C000UL, 16U * 1024U,  3 },
    { 0x08010000UL, 64U * 1024U,  4 },
    { 0x08020000UL, 128U * 1024U, 5 },
    { 0x08040000UL, 128U * 1024U,  6 },
    { 0x08060000UL, 128U * 1024U,  7 },
};

static const flash_sector_t *sector_of(uint32_t addr)
{
    for (unsigned i = 0; i < ARRAY_SIZE(s_sectors); i++) {
        uint32_t b = s_sectors[i].base;
        uint32_t e = b + s_sectors[i].size;

        if (addr >= b && addr < e) return &s_sectors[i];
    }
    return 0;
}

/* --------------------------------------------------- RAM resident core */
__attribute__((section(".ramfunc"), noinline, used))
static int ram_wait_idle(void)
{
    uint32_t spin = FLASH_SPIN_LIMIT;

    while (FLASH_R->SR & FLASH_SR_BSY) {
        if (--spin == 0) return FLASH_ERR_TIMEOUT;
    }
    if (FLASH_R->SR & FLASH_SR_ERRORS) {
        int rc = (FLASH_R->SR & FLASH_SR_WRPERR) ? FLASH_ERR_PROTECTED
                                                 : FLASH_ERR_PROG;
        FLASH_R->SR = FLASH_SR_ERRORS | FLASH_SR_EOP;
        return rc;
    }
    FLASH_R->SR = FLASH_SR_EOP;
    return FLASH_OK;
}

__attribute__((section(".ramfunc"), noinline, used))
static int ram_erase_sector(uint32_t snb)
{
    uint32_t pm;
    int rc;

    pm = irq_save();
    rc = ram_wait_idle();
    if (rc == FLASH_OK) {
        FLASH_R->CR = (FLASH_R->CR & ~FLASH_CR_SNB_MASK) |
                      FLASH_CR_SER | FLASH_CR_SNB(snb) | FLASH_CR_PSIZE_X32;
        FLASH_R->CR |= FLASH_CR_STRT;
        rc = ram_wait_idle();
        FLASH_R->CR &= ~(FLASH_CR_SER | FLASH_CR_STRT | FLASH_CR_SNB_MASK);
    }
    irq_restore(pm);
    return rc;
}

__attribute__((section(".ramfunc"), noinline, used))
static int ram_program_word(uint32_t addr, uint32_t val)
{
    uint32_t pm;
    int rc;

    pm = irq_save();
    rc = ram_wait_idle();
    if (rc == FLASH_OK) {
        FLASH_R->CR |= FLASH_CR_PG | FLASH_CR_PSIZE_X32;
        *(volatile uint32_t *)(uintptr_t)addr = val;
        rc = ram_wait_idle();
        FLASH_R->CR &= ~FLASH_CR_PG;
        if (rc == FLASH_OK && *(volatile uint32_t *)(uintptr_t)addr != val)
            rc = FLASH_ERR_VERIFY;
    }
    irq_restore(pm);
    return rc;
}

/* ------------------------------------------------------------- bounds */
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

/* A sector may be erased only when every byte of it is either unused
 * padding in sector 3 or a writable Freya region.  Sectors 0..2 are the
 * kernel; 5..7 are left alone. */
static int sector_erasable(const flash_sector_t *s)
{
    return s->snb == 3 || s->snb == 4;
}

/* --------------------------------------------------------------- setup */
uint32_t flash_page_size(void)
{
    return BOARD_FLASH_PAGE_SIZE;
}

int flash_begin(void)
{
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

    s_acr = FLASH_R->ACR;
    FLASH_R->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);

    FLASH_R->KEYR = FLASH_KEY1;
    FLASH_R->KEYR = FLASH_KEY2;
    if (FLASH_R->CR & FLASH_CR_LOCK) return FLASH_ERR_LOCKED;

    FLASH_R->CR = (FLASH_R->CR & ~FLASH_CR_SNB_MASK) | FLASH_CR_PSIZE_X32;
    FLASH_R->SR = FLASH_SR_ERRORS | FLASH_SR_EOP;
    s_ready = 1;
    return FLASH_OK;
}

void flash_end(void)
{
    FLASH_R->CR &= ~(FLASH_CR_PG | FLASH_CR_SER | FLASH_CR_STRT);
    FLASH_R->CR |= FLASH_CR_LOCK;
    s_ready = 0;

    FLASH_R->ACR |= FLASH_ACR_ICRST | FLASH_ACR_DCRST;
    FLASH_R->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH_R->ACR = s_acr;
    __dsb();
    __isb();
}

/* --------------------------------------------------------- erase/write */
int flash_erase(uint32_t addr, uint32_t len)
{
    uint32_t end, a;

    if (!s_ready) return FLASH_ERR_LOCKED;
    if (!in_region(addr, len)) return FLASH_ERR_RANGE;
    if (addr & 3U) return FLASH_ERR_ALIGN;

    end = addr + len;
    a = addr;
    while (a < end) {
        const flash_sector_t *s = sector_of(a);
        int rc;

        if (!s || !sector_erasable(s)) return FLASH_ERR_RANGE;
        rc = ram_erase_sector(s->snb);
        if (rc != FLASH_OK) return rc;
        a = s->base + s->size;
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
        uint32_t v = 0xFFFFFFFFUL;
        uint32_t n = (len >= 4U) ? 4U : len;
        int rc;

        memcpy(&v, p, n);
        if (!in_region(addr, 4) &&
            !in_slot(addr, 4, FREYA_APP_FLASH_ADDR, FREYA_APP_FLASH_SIZE) &&
            !in_slot(addr, 4, FREYA_AUTOSTART_ADDR, FREYA_AUTOSTART_SIZE))
            return FLASH_ERR_RANGE;
        rc = ram_program_word(addr, v);
        if (rc != FLASH_OK) return rc;

        addr += 4;
        p    += n;
        len  -= n;
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
