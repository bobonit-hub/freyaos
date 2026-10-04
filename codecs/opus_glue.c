/*
 * Freya codec pack - what libopus needs from a C library.
 *
 * A Freya program links without one.  libopus wants memcpy, memmove,
 * memset and abs, and allocation, which here is one static scratch
 * buffer for its pseudostack (codecs/opus/config.h) and nothing else:
 * opus_alloc() fails, so the _create() calls return NULL and a program
 * uses the _init() calls on memory of its own.  A program that has its
 * own memcpy and friends keeps them; the linker only takes these when
 * nothing else defines them.
 *
 * Built with -fno-tree-loop-distribute-patterns, so that GCC does not
 * turn the loops below back into calls to themselves.
 */
#include <stddef.h>
#include <stdint.h>
#include "freya_codecs.h"

static char s_scratch[FREYA_OPUS_SCRATCH] __attribute__((aligned(8)));

void *opus_alloc_scratch(size_t size)
{
    return size <= sizeof(s_scratch) ? s_scratch : NULL;
}

void *opus_alloc(size_t size) { (void)size; return NULL; }
void *opus_realloc(void *ptr, size_t size) { (void)ptr; (void)size; return NULL; }
void  opus_free(void *ptr) { (void)ptr; }

/* How much of the scratch has ever been used: the host test watches it. */
size_t freya_opus_scratch_used(void)
{
    size_t n = sizeof(s_scratch);

    while (n > 0 && s_scratch[n - 1] == (char)0xA5) n--;
    return n;
}

void freya_opus_scratch_paint(void)
{
    for (size_t i = 0; i < sizeof(s_scratch); i++) s_scratch[i] = (char)0xA5;
}

#ifndef FREYA_HOST
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int abs(int v)
{
    return v < 0 ? -v : v;
}
#endif
