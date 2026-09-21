/*
 * hello - example Freya program.
 *
 * Shows the whole service table: console output, arguments, timing, the
 * LED, the filesystem, and the cooperative stop check.  Build with the
 * top level Makefile, copy build/apps/hello.bin onto the card (or send it
 * with 'download hello.bin'), then 'run hello.bin'.
 */
#include "freya_api.h"

/*
 * In a RAM image these are part of the image the loader copies from the
 * card.  In a flash image the code and the initialiser stay in flash and
 * the loader copies .data into the RAM region and clears .bss there, so
 * printing them is a check that it did.  Volatile keeps the compiler from
 * folding the comparisons away and testing nothing.
 */
static char              s_greeting[] = "initialised data survived the load";
static volatile uint32_t s_magic      = 0xC0FFEE01UL;
static volatile uint32_t s_zero;

static void list_root(const freya_api_t *api)
{
    freya_stat_t st;
    int dd = api->opendir("/");

    if (dd < 0) {
        api->printf("  (cannot open /: %d)\r\n", dd);
        return;
    }
    while (api->readdir(dd, &st) == 0)
        api->printf("  %-24s %s%u\r\n", st.name, st.is_dir ? "<dir> " : "", st.size);
    api->closedir(dd);
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    uint32_t start = api->ticks_ms();
    int fd;

    api->printf("\r\nhello from a program running in Freya's program %s\r\n",
                ((uintptr_t)&app_main < 0x20000000UL) ? "flash region"
                                                      : "RAM region");
    api->printf("  api version %u, table size %u bytes\r\n", api->version, api->size);
    api->printf("  cpu %u Hz, %u ms since boot\r\n", api->cpu_hz(), start);
    api->printf("  code at 0x%08x, data at 0x%08x\r\n",
                (uint32_t)(uintptr_t)&app_main, (uint32_t)(uintptr_t)&s_magic);
    api->printf("  %s: .data %s, .bss %s\r\n", s_greeting,
                (s_magic == 0xC0FFEE01UL) ? "ok" : "WRONG",
                (s_zero == 0) ? "clear" : "DIRTY");

    api->printf("  argc = %d\r\n", argc);
    for (int i = 0; i < argc; i++)
        api->printf("    argv[%d] = \"%s\"\r\n", i, argv[i]);

    api->puts("\r\nroot directory:\r\n");
    list_root(api);

    api->puts("\r\nwriting /hello.log ...\r\n");
    fd = api->open("/hello.log", FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_APPEND);
    if (fd >= 0) {
        char line[64];
        int n = 0;
        const char *msg = "hello ran at tick ";
        uint32_t t = api->ticks_ms();
        char num[12];
        int d = 0;

        while (*msg) line[n++] = *msg++;
        do { num[d++] = (char)('0' + t % 10); t /= 10; } while (t);
        while (d--) line[n++] = num[d];
        line[n++] = '\r';
        line[n++] = '\n';
        api->write(fd, line, n);
        api->close(fd);
        api->puts("  done\r\n");
    } else {
        api->printf("  cannot write: %d\r\n", fd);
    }

    api->puts("\r\ncounting - press Ctrl-C to stop me\r\n");
    for (int i = 1; !api->should_stop(); i++) {
        api->printf("  tick %d\r\n", i);
        api->led(i & 1);
        api->delay_ms(500);
        api->yield();                 /* unwinds into the shell if stopped */
    }

    api->led(0);
    api->printf("\r\nfinished after %u ms\r\n", api->ticks_ms() - start);
    return 0;
}
