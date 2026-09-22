/*
 * spin - a deliberately impolite Freya program.
 *
 * It never checks should_stop() and never yields, so it can only be
 * stopped by Ctrl-C.  Use it to verify that the console interrupt really
 * can tear a runaway program out of a tight loop.
 *
 * 'spin fault' instead performs a bad memory access to show that a
 * crashing program is contained and reported rather than taking the
 * system down.
 */
#include "freya_api.h"

int app_main(const freya_api_t *api, int argc, char **argv)
{
    volatile uint32_t counter = 0;

    if (argc > 1 && argv[1][0] == 'f') {
        volatile uint32_t *bad = (volatile uint32_t *)0xF0000000UL;
        api->puts("spin: about to touch 0xF0000000 ...\r\n");
        *bad = 0xDEADBEEF;
        api->puts("spin: still alive?\r\n");
        return FREYA_EXIT_FAIL;
    }

    api->puts("spin: looping forever without checking anything.\r\n");
    api->puts("spin: press Ctrl-C.\r\n");

    for (;;) {
        counter++;
        if ((counter & 0x000FFFFF) == 0) api->led((int)(counter >> 20) & 1);
    }
}
