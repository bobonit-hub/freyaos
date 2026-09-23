/*
 * Freya - the Altair's 64 KiB, for the Altair sample.
 *
 * The address space is 64 pages of 1 KiB.  Every page has a pointer to
 * read through and a pointer to write through, so an access is one
 * table lookup whatever sits behind it:
 *
 *   from 0000      MEM_BASE_KB of RAM in the program's .bss: 48 KiB,
 *                  through BFFF, or 16 KiB through 3FFF for altair16
 *   above that     more RAM from the heap, one page at a time, when
 *   up to F7FF     --ram asks for more than the .bss holds
 *   F800 .. FBFF   the Turnkey Module's 1 KiB SRAM
 *   FC00 .. FFFF   the Turnkey Module's four 256-byte PROM sockets
 *
 * A page with nothing behind it reads FFh, the way an empty slot on the
 * S-100 bus does, and swallows writes.  That matters: BASIC finds the
 * top of memory by writing a byte and reading it back, so an absent page
 * must not remember what was written to it.  A PROM page reads its
 * contents and swallows writes the same way.
 *
 * The Blue Pill is the exception for altair16.  Its SRAM is 20 KiB, and
 * the kernel, the stacks and the 8 KiB program window already fill it, so
 * 16 KiB of 8080 RAM cannot be an array in .bss.  That RAM is the top
 * 16 KiB of the program flash instead.  Reads are ordinary loads.  Writes
 * stay in a small cache in the window and a dirty page is erased and
 * programmed back when the cache needs the slot.  The erase routine is
 * copied into SRAM first, because the F103 stalls an instruction fetch
 * from flash for as long as the controller is busy.  The Turnkey SRAM
 * and the PROM sockets stay real RAM in the window.
 */
#define MEM_PAGES       64
#define MEM_PAGE_SHIFT  10
#define MEM_PAGE        (1u << MEM_PAGE_SHIFT)
#define MEM_PAGE_MASK   (MEM_PAGE - 1)

/* samples/altair16 sets both before this file is included. */
#ifndef MEM_BASE_KB
#define MEM_BASE_KB     48u                 /* the .bss part            */
#endif
#ifndef MEM_MAX_KB
#define MEM_MAX_KB      62u                 /* up to the Turnkey SRAM   */
#endif
#define MEM_TK_RAM      0xF800u
#define MEM_PROM        0xFC00u
#define MEM_PROM_SOCKET 256u

/*
 * 16 KiB is the whole of what altair16 asks for, and it is also exactly
 * the flash this path reserves.  A larger machine has no such region.
 */
#if defined(FREYA_BOARD_BLUEPILL) && !defined(FREYA_HOST) && (MEM_MAX_KB <= 16u)
#define MEM_MAIN_IN_FLASH 1
#define FLASH_RAM_BYTES   (16u * 1024u)
#define FLASH_RAM_BASE    (FREYA_APP_FLASH_ADDR + FREYA_APP_FLASH_SIZE - FLASH_RAM_BYTES)
#define CACHE_SLOTS       3
#define FLASH_OP_MAX      768u
#endif

static uint8_t s_tk_ram[MEM_PAGE];
static uint8_t s_prom[MEM_PAGE];
static unsigned s_ram_kb;

#ifdef MEM_MAIN_IN_FLASH

/* Offsets match the F103 flash controller.  A program has no board
 * header of its own; these are the registers this file touches. */
typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t SR;
    volatile uint32_t CR;
    volatile uint32_t AR;
} mem_flash_t;

#define MEM_FLASH           ((mem_flash_t *)0x40022000UL)
#define MEM_FLASH_KEY1      0x45670123UL
#define MEM_FLASH_KEY2      0xCDEF89ABUL
#define MEM_FLASH_PRFTBE    (1UL << 4)
#define MEM_FLASH_BSY       (1UL << 0)
#define MEM_FLASH_PGERR     (1UL << 2)
#define MEM_FLASH_WRPRTERR  (1UL << 4)
#define MEM_FLASH_EOP       (1UL << 5)
#define MEM_FLASH_PG        (1UL << 0)
#define MEM_FLASH_PER       (1UL << 1)
#define MEM_FLASH_STRT      (1UL << 6)
#define MEM_FLASH_LOCK      (1UL << 7)
#define MEM_FLASH_SPIN      2000000UL

static uint8_t   s_cache[CACHE_SLOTS][MEM_PAGE] __attribute__((aligned(4)));
static uint8_t   s_cache_valid[CACHE_SLOTS];
static uint8_t   s_cache_dirty[CACHE_SLOTS];
static uint8_t   s_cache_page[CACHE_SLOTS];
static uint32_t  s_cache_age[CACHE_SLOTS];
static uint32_t  s_cache_clock;
static uint8_t   s_op[FLASH_OP_MAX] __attribute__((aligned(4)));
static int     (*s_op_fn)(uint32_t dest, const uint8_t *src);
static uint8_t   s_flash_open;

/*
 * Erase one page, and if src is not null program it afterwards.  This
 * stays in its own section so flash_op_install() can copy the whole
 * thing, literal pool included, into SRAM.  It must not call anything:
 * a call would fetch from flash while the controller is busy.
 */
__attribute__((section(".text.flashop"), noinline, noclone, noipa, used, aligned(4)))
static int flash_ram_op(uint32_t dest, const uint8_t *src)
{
    mem_flash_t *fl = MEM_FLASH;
    uint32_t pm, spin, acr;
    int rc = 0;

    __asm volatile ("mrs %0, primask" : "=r"(pm) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");

    spin = MEM_FLASH_SPIN;
    while (fl->SR & MEM_FLASH_BSY) {
        if (--spin == 0) {
            rc = -1;
            goto out;
        }
    }
    if (fl->SR & (MEM_FLASH_PGERR | MEM_FLASH_WRPRTERR)) {
        fl->SR = MEM_FLASH_PGERR | MEM_FLASH_WRPRTERR | MEM_FLASH_EOP;
        rc = -1;
        goto out;
    }
    fl->SR = MEM_FLASH_EOP;

    fl->CR |= MEM_FLASH_PER;
    fl->AR  = dest;
    fl->CR |= MEM_FLASH_STRT;
    spin = MEM_FLASH_SPIN;
    while (fl->SR & MEM_FLASH_BSY) {
        if (--spin == 0) {
            rc = -1;
            break;
        }
    }
    fl->CR &= ~(MEM_FLASH_PER | MEM_FLASH_STRT);
    if (rc)
        goto out;
    if (fl->SR & (MEM_FLASH_PGERR | MEM_FLASH_WRPRTERR)) {
        fl->SR = MEM_FLASH_PGERR | MEM_FLASH_WRPRTERR | MEM_FLASH_EOP;
        rc = -1;
        goto out;
    }

    if (src) {
        const uint16_t *hw = (const uint16_t *)src;
        unsigned i;

        for (i = 0; i < MEM_PAGE / 2u; i++) {
            uint16_t v = hw[i];

            /* An erased halfword already reads FFFFh. */
            if (v == 0xFFFFu)
                continue;
            fl->CR |= MEM_FLASH_PG;
            *(volatile uint16_t *)(uintptr_t)(dest + i * 2u) = v;
            spin = MEM_FLASH_SPIN;
            while (fl->SR & MEM_FLASH_BSY) {
                if (--spin == 0) {
                    rc = -1;
                    break;
                }
            }
            fl->CR &= ~MEM_FLASH_PG;
            if (rc)
                break;
            if (fl->SR & (MEM_FLASH_PGERR | MEM_FLASH_WRPRTERR)) {
                fl->SR = MEM_FLASH_PGERR | MEM_FLASH_WRPRTERR | MEM_FLASH_EOP;
                rc = -1;
                break;
            }
            if (*(volatile uint16_t *)(uintptr_t)(dest + i * 2u) != v) {
                rc = -1;
                break;
            }
        }
    }

out:
    acr = fl->ACR;
    fl->ACR = acr & ~MEM_FLASH_PRFTBE;
    fl->ACR = acr;
    __asm volatile ("dsb 0xF" ::: "memory");
    __asm volatile ("isb 0xF" ::: "memory");
    __asm volatile ("msr primask, %0" :: "r"(pm) : "memory");
    return rc;
}

/* Next section in this file, so the bytes between the two symbols are
 * the routine and nothing else. */
__attribute__((section(".text.flashop_end"), noinline, used, naked, aligned(4)))
static void flash_ram_op_end(void)
{
    __asm volatile ("nop");
}

static int flash_op_install(void)
{
    uintptr_t from = (uintptr_t)flash_ram_op & ~1u;
    uintptr_t to   = (uintptr_t)flash_ram_op_end & ~1u;
    uintptr_t n    = to - from;
    uintptr_t i;

    if (to <= from || n > FLASH_OP_MAX)
        return 0;
    for (i = 0; i < n; i++)
        s_op[i] = *(const uint8_t *)(from + i);
    __asm volatile ("dsb 0xF" ::: "memory");
    __asm volatile ("isb 0xF" ::: "memory");
    s_op_fn = (int (*)(uint32_t, const uint8_t *))((uintptr_t)s_op | 1u);
    return 1;
}

static void cache_drop(void)
{
    for (int i = 0; i < CACHE_SLOTS; i++)
        s_cache_valid[i] = 0;
}

static void flash_ram_end(void)
{
    cache_drop();
    s_ram_kb = 0;
    if (!s_flash_open)
        return;
    MEM_FLASH->CR &= ~(MEM_FLASH_PG | MEM_FLASH_PER);
    MEM_FLASH->CR |= MEM_FLASH_LOCK;
    s_flash_open = 0;
}

/* The image occupies the bottom of the program flash.  Main RAM is the
 * top 16 KiB of that same region, so a program that grew into it would
 * be erased by its own memory. */
static int flash_ram_begin(unsigned ram_kb)
{
    const freya_app_header_t *hdr =
        (const freya_app_header_t *)(uintptr_t)FREYA_APP_FLASH_ADDR;
    unsigned i;

    if (hdr->magic != FREYA_APP_MAGIC ||
        hdr->image_size > FREYA_APP_FLASH_SIZE ||
        FREYA_APP_FLASH_ADDR + hdr->image_size > FLASH_RAM_BASE ||
        ram_kb * MEM_PAGE > FLASH_RAM_BYTES) {
        g->printf("altair: main RAM does not fit above this program\r\n");
        return 0;
    }

    if (!flash_op_install()) {
        g->printf("altair: the flash routine does not fit in RAM\r\n");
        return 0;
    }
    if (MEM_FLASH->CR & MEM_FLASH_LOCK) {
        MEM_FLASH->KEYR = MEM_FLASH_KEY1;
        MEM_FLASH->KEYR = MEM_FLASH_KEY2;
    }
    if (MEM_FLASH->CR & MEM_FLASH_LOCK) {
        g->printf("altair: flash is locked\r\n");
        return 0;
    }
    s_flash_open = 1;
    cache_drop();

    g->printf("altair: clearing %u KiB of main RAM\r\n", ram_kb);
    for (i = 0; i < ram_kb; i++) {
        if (s_op_fn(FLASH_RAM_BASE + i * MEM_PAGE, NULL)) {
            g->printf("altair: could not clear main RAM\r\n");
            flash_ram_end();
            return 0;
        }
    }
    return 1;
}

static int cache_lookup(unsigned page)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache_valid[i] && s_cache_page[i] == page) {
            s_cache_age[i] = ++s_cache_clock;
            return i;
        }
    }
    return -1;
}

static void copy_page(uint8_t *dst, const uint8_t *src)
{
    for (unsigned i = 0; i < MEM_PAGE; i++)
        dst[i] = src[i];
}

/* Reads of a page that is not dirty go straight to flash, so the
 * interpreter does not push the written pages out of the cache. */
static int cache_fill(unsigned page)
{
    int slot = 0;

    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (!s_cache_valid[i]) {
            slot = i;
            goto take;
        }
        if (s_cache_age[i] < s_cache_age[slot])
            slot = i;
    }
    if (s_cache_dirty[slot]) {
        uint32_t dest = FLASH_RAM_BASE + (uint32_t)s_cache_page[slot] * MEM_PAGE;

        if (s_op_fn(dest, s_cache[slot]))
            return -1;
    }
take:
    copy_page(s_cache[slot],
              (const uint8_t *)(uintptr_t)(FLASH_RAM_BASE + page * MEM_PAGE));
    s_cache_page[slot]  = (uint8_t)page;
    s_cache_valid[slot] = 1;
    s_cache_dirty[slot] = 0;
    s_cache_age[slot]   = ++s_cache_clock;
    return slot;
}

static uint8_t flash_ram_rd(uint16_t a)
{
    unsigned page = a >> MEM_PAGE_SHIFT;
    unsigned off  = a & MEM_PAGE_MASK;
    int slot = cache_lookup(page);

    if (slot >= 0)
        return s_cache[slot][off];
    return *(volatile uint8_t *)(uintptr_t)(FLASH_RAM_BASE + a);
}

static void flash_ram_wr(uint16_t a, uint8_t v)
{
    unsigned page = a >> MEM_PAGE_SHIFT;
    int slot = cache_lookup(page);

    if (slot < 0) {
        slot = cache_fill(page);
        if (slot < 0)
            return;
    }
    s_cache[slot][a & MEM_PAGE_MASK] = v;
    s_cache_dirty[slot] = 1;
}

#else /* SRAM main RAM, page tables */

static uint8_t *s_rd[MEM_PAGES];
static uint8_t *s_wr[MEM_PAGES];
static uint8_t *s_heap_page[MEM_PAGES];     /* pages that came from malloc */
static uint8_t s_ram[MEM_BASE_KB * 1024u];
static uint8_t s_open_bus[MEM_PAGE];        /* all FFh                  */
static uint8_t s_sink[MEM_PAGE];            /* where ignored writes go  */

#endif /* MEM_MAIN_IN_FLASH */

#ifdef MEM_MAIN_IN_FLASH
static uint8_t mem_rd(uint16_t a)
#else
static inline uint8_t mem_rd(uint16_t a)
#endif
{
#ifdef MEM_MAIN_IN_FLASH
    if ((unsigned)a < s_ram_kb * MEM_PAGE)
        return flash_ram_rd(a);
    if (a >= MEM_PROM)
        return s_prom[a - MEM_PROM];
    if (a >= MEM_TK_RAM)
        return s_tk_ram[a - MEM_TK_RAM];
    return 0xFF;
#else
    return s_rd[a >> MEM_PAGE_SHIFT][a & MEM_PAGE_MASK];
#endif
}

#ifdef MEM_MAIN_IN_FLASH
static void mem_wr(uint16_t a, uint8_t v)
#else
static inline void mem_wr(uint16_t a, uint8_t v)
#endif
{
#ifdef MEM_MAIN_IN_FLASH
    if ((unsigned)a < s_ram_kb * MEM_PAGE) {
        flash_ram_wr(a, v);
        return;
    }
    if (a >= MEM_PROM)
        return;
    if (a >= MEM_TK_RAM)
        s_tk_ram[a - MEM_TK_RAM] = v;
#else
    s_wr[a >> MEM_PAGE_SHIFT][a & MEM_PAGE_MASK] = v;
#endif
}

/* Writes to a PROM too: this is how an image is put into one. */
static void mem_poke(uint16_t a, uint8_t v)
{
    if (a >= MEM_PROM)
        s_prom[a - MEM_PROM] = v;
#ifdef MEM_MAIN_IN_FLASH
    else
        mem_wr(a, v);
#else
    else if (s_wr[a >> MEM_PAGE_SHIFT] != s_sink)
        mem_wr(a, v);
#endif
}

static void fill(uint8_t *p, uint8_t v, unsigned n)
{
    while (n--)
        *p++ = v;
}

static void mem_release(void)
{
#ifdef MEM_MAIN_IN_FLASH
    flash_ram_end();
#else
    for (int i = 0; i < MEM_PAGES; i++) {
        if (s_heap_page[i]) {
            g->free(s_heap_page[i]);
            s_heap_page[i] = NULL;
        }
    }
#endif
}

/*
 * Maps the address space for ram_kb of main RAM and returns how much it
 * got: past MEM_BASE_KB the rest comes from the heap and stops at the
 * first page the heap cannot give.  The Turnkey SRAM is always there;
 * the PROM page starts out as four empty sockets.
 */
static unsigned mem_init(unsigned ram_kb)
{
    mem_release();
    if (ram_kb > MEM_MAX_KB)
        ram_kb = MEM_MAX_KB;
    fill(s_prom, 0xFF, MEM_PAGE);

#ifdef MEM_MAIN_IN_FLASH
    if (!flash_ram_begin(ram_kb))
        return 0;
    s_ram_kb = ram_kb;
    return s_ram_kb;
#else
    unsigned i;

    fill(s_open_bus, 0xFF, MEM_PAGE);

    for (i = 0; i < MEM_PAGES; i++) {
        s_rd[i] = s_open_bus;
        s_wr[i] = s_sink;
    }
    for (i = 0; i < ram_kb; i++) {
        uint8_t *p;

        if (i < MEM_BASE_KB) {
            p = s_ram + i * MEM_PAGE;
        } else {
            p = g->malloc(MEM_PAGE);
            if (!p)
                break;
            s_heap_page[i] = p;
        }
        s_rd[i] = s_wr[i] = p;
    }
    s_ram_kb = i;

    s_rd[MEM_TK_RAM >> MEM_PAGE_SHIFT] = s_tk_ram;
    s_wr[MEM_TK_RAM >> MEM_PAGE_SHIFT] = s_tk_ram;
    s_rd[MEM_PROM >> MEM_PAGE_SHIFT] = s_prom;
    return s_ram_kb;
#endif
}

/* Whether a PROM socket holds anything: an erased 1702A reads all FFh
 * here, the same as an empty socket. */
static int mem_prom_loaded(uint16_t addr)
{
    const uint8_t *p;

    if (addr < MEM_PROM)
        return 0;
    p = s_prom + ((addr - MEM_PROM) & ~(MEM_PROM_SOCKET - 1));
    for (unsigned i = 0; i < MEM_PROM_SOCKET; i++)
        if (p[i] != 0xFF)
            return 1;
    return 0;
}
