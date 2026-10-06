/*
 * cpubench - CPU performance tests: Dhrystone 2.1 and Whetstone.
 *
 *     run cpubench                  both tests, 2 s each
 *     run cpubench -i               Dhrystone alone (integer)
 *     run cpubench -f -t 5          Whetstone alone (float), 5 s
 *
 * The tests are bench/cpubench.c, which the Linux shell builds in as
 * its cpubench() command; this file gives them the console, the clock,
 * Ctrl-C and the CPU's clock from the service table.  The Makefile
 * builds this program with -O2, not the -Os of the other samples, and
 * links the toolchain's libm for Whetstone's sinf(), expf() and the
 * rest.  On the Blue Pill every float is the soft-float helpers'.
 */
#include "freya_api.h"

#include "../../bench/cpubench.c"

/* A program has no C library, and a record assignment in Dhrystone may
 * be compiled into a call to memcpy(). */
void *memcpy(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, unsigned int n)
{
    unsigned char *d = dst;

    while (n--) *d++ = (unsigned char)c;
    return dst;
}

/* libm sets errno on a domain or range error; Whetstone never makes
 * one, but the functions refer to it. */
int *__errno(void)
{
    static int e;

    return &e;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    cpubench_io_t io;
    int rc;

    io.printf      = api->printf;
    io.ticks_ms    = api->ticks_ms;
    io.should_stop = api->should_stop;
    io.cpu_hz      = api->cpu_hz();

    rc = cpubench(&io, argc, argv);
    if (rc == 2) return FREYA_EXIT_USAGE;
    if (rc != 0) return api->should_stop() ? FREYA_EXIT_STOPPED : FREYA_EXIT_FAIL;
    return FREYA_EXIT_OK;
}
