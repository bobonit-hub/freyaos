/*
 * threads - two threads, a name and a priority each.
 *
 * The program's main thread prints slowly.  "blink" is started above it,
 * so it runs first and only gives the main thread the CPU when it sleeps.
 * Ctrl-C, or `stop` with no name, ends the run and both threads.  `stop
 * blink` ends just that one; `threads` lists whatever is still around.
 */
#include "freya_api.h"

static void blink(void *arg)
{
    const freya_api_t *api = arg;
    uint32_t n = 0;

    while (!api->should_stop()) {
        api->led((int)(n & 1));
        api->printf("blink %u\r\n", n);
        n++;
        if (api->thread_sleep(300) != 0) return;
        if (n >= 8) return;
    }
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    int id;

    (void)argc;
    (void)argv;

    if (!FREYA_API_HAS(api, thread_self)) {
        api->puts("threads: this kernel has no thread calls\r\n");
        return FREYA_EXIT_FAIL;
    }

    api->printf("main is thread %d\r\n", api->thread_self());
    id = api->thread_create("blink", 2, blink, (void *)api);
    if (id < 0) {
        api->printf("thread_create: %d\r\n", id);
        return FREYA_EXIT_FAIL;
    }
    api->printf("blink is thread %d\r\n", id);

    for (int i = 0; i < 5 && !api->should_stop(); i++) {
        api->printf("main %d\r\n", i);
        if (api->thread_sleep(500) != 0) break;
    }
    return FREYA_EXIT_OK;
}
