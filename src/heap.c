/*
 * Freya - system heap.
 *
 * Boundary tagged, first fit, immediate coalescing.  The heap spans
 * everything between the end of .bss and the user program region, so it
 * grows automatically when the kernel's static footprint shrinks.
 */
#include "freya.h"

#define BLK_MAGIC   0x464B4C42UL    /* 'BLKF' */
#define ALIGN_UP(x) (((x) + 7U) & ~7U)

typedef struct blk {
    uint32_t magic;
    uint32_t size;      /* total block size including this header */
    uint32_t free;
    uint32_t pad;       /* keeps payloads 8 byte aligned          */
} blk_t;

static uint8_t *s_heap_base;
static uint32_t s_heap_size;
static uint32_t s_used;

void heap_init(void)
{
    blk_t *b;

    s_heap_base = (uint8_t *)__heap_start;
    s_heap_size = (uint32_t)((uint8_t *)__heap_end - (uint8_t *)__heap_start);
    s_heap_size &= ~7U;
    s_used = 0;

    b = (blk_t *)s_heap_base;
    b->magic = BLK_MAGIC;
    b->size  = s_heap_size;
    b->free  = 1;
    b->pad   = 0;
}

void *kmalloc(uint32_t size)
{
    uint32_t need = ALIGN_UP(size) + sizeof(blk_t);
    uint8_t *p = s_heap_base;
    uint32_t pm;

    if (size == 0 || need < size) return NULL;

    pm = irq_save();
    while (p < s_heap_base + s_heap_size) {
        blk_t *b = (blk_t *)p;

        if (b->magic != BLK_MAGIC) break;          /* heap corrupted */
        if (b->free && b->size >= need) {
            if (b->size >= need + sizeof(blk_t) + 16) {
                blk_t *n = (blk_t *)(p + need);
                n->magic = BLK_MAGIC;
                n->size  = b->size - need;
                n->free  = 1;
                n->pad   = 0;
                b->size  = need;
            }
            b->free = 0;
            s_used += b->size;
            irq_restore(pm);
            return p + sizeof(blk_t);
        }
        p += b->size;
    }
    irq_restore(pm);
    return NULL;
}

void kfree(void *ptr)
{
    blk_t *b;
    uint8_t *p;
    uint32_t pm;

    if (!ptr) return;
    b = (blk_t *)((uint8_t *)ptr - sizeof(blk_t));
    if (b->magic != BLK_MAGIC || b->free) return;

    pm = irq_save();
    b->free = 1;
    s_used -= b->size;

    /* Merge forward, then rescan from the base to merge backwards. */
    p = s_heap_base;
    while (p < s_heap_base + s_heap_size) {
        blk_t *cur = (blk_t *)p;
        blk_t *next;

        if (cur->magic != BLK_MAGIC) break;
        next = (blk_t *)(p + cur->size);
        if (cur->free && (uint8_t *)next < s_heap_base + s_heap_size &&
            next->magic == BLK_MAGIC && next->free) {
            cur->size += next->size;
            next->magic = 0;
            continue;                              /* try merging again */
        }
        p += cur->size;
    }
    irq_restore(pm);
}

void heap_stats(uint32_t *total, uint32_t *used, uint32_t *free_bytes,
                uint32_t *largest, uint32_t *blocks)
{
    uint8_t *p = s_heap_base;
    uint32_t f = 0, big = 0, n = 0;
    uint32_t pm = irq_save();

    while (p < s_heap_base + s_heap_size) {
        blk_t *b = (blk_t *)p;
        if (b->magic != BLK_MAGIC) break;
        n++;
        if (b->free) {
            uint32_t avail = b->size - sizeof(blk_t);
            f += avail;
            if (avail > big) big = avail;
        }
        p += b->size;
    }
    irq_restore(pm);

    if (total)      *total = s_heap_size;
    if (used)       *used = s_used;
    if (free_bytes) *free_bytes = f;
    if (largest)    *largest = big;
    if (blocks)     *blocks = n;
}

uint32_t stack_used(void)
{
    uint32_t sp;

    /* Thread mode runs on PSP.  The figure is the shell stack, which is
     * also the stack a program's main thread uses. */
    __asm volatile ("mrs %0, psp" : "=r"(sp));
    if (sp > (uint32_t)(uintptr_t)__thread_stack_top) return 0;
    return (uint32_t)(uintptr_t)__thread_stack_top - sp;
}

/* Walks the 0xDEADBEEF fill written by the reset handler. */
uint32_t stack_peak(void)
{
    const uint32_t *p = (const uint32_t *)__stack_limit;
    const uint32_t *top = (const uint32_t *)__stack_top;

    while (p < top && *p == 0xDEADBEEFUL) p++;
    return (uint32_t)((top - p) * 4);
}
