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

static uint8_t *s_rd[MEM_PAGES];
static uint8_t *s_wr[MEM_PAGES];
static uint8_t *s_heap_page[MEM_PAGES];     /* pages that came from malloc */

static uint8_t s_ram[MEM_BASE_KB * 1024u];
static uint8_t s_tk_ram[MEM_PAGE];
static uint8_t s_prom[MEM_PAGE];
static uint8_t s_open_bus[MEM_PAGE];        /* all FFh                  */
static uint8_t s_sink[MEM_PAGE];            /* where ignored writes go  */
static unsigned s_ram_kb;

static inline uint8_t mem_rd(uint16_t a)
{
    return s_rd[a >> MEM_PAGE_SHIFT][a & MEM_PAGE_MASK];
}

static inline void mem_wr(uint16_t a, uint8_t v)
{
    s_wr[a >> MEM_PAGE_SHIFT][a & MEM_PAGE_MASK] = v;
}

/* Writes to a PROM too: this is how an image is put into one. */
static void mem_poke(uint16_t a, uint8_t v)
{
    if (a >= MEM_PROM)
        s_prom[a - MEM_PROM] = v;
    else if (s_wr[a >> MEM_PAGE_SHIFT] != s_sink)
        mem_wr(a, v);
}

static void fill(uint8_t *p, uint8_t v, unsigned n)
{
    while (n--)
        *p++ = v;
}

static void mem_release(void)
{
    for (int i = 0; i < MEM_PAGES; i++) {
        if (s_heap_page[i]) {
            g->free(s_heap_page[i]);
            s_heap_page[i] = NULL;
        }
    }
}

/*
 * Maps the address space for ram_kb of main RAM and returns how much it
 * got: past MEM_BASE_KB the rest comes from the heap and stops at the
 * first page the heap cannot give.  The Turnkey SRAM is always there;
 * the PROM page starts out as four empty sockets.
 */
static unsigned mem_init(unsigned ram_kb)
{
    unsigned i;

    mem_release();
    if (ram_kb > MEM_MAX_KB)
        ram_kb = MEM_MAX_KB;
    fill(s_open_bus, 0xFF, MEM_PAGE);
    fill(s_prom, 0xFF, MEM_PAGE);

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
