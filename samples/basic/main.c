/*
 * basic - BASIC on the PDP-11 virtual machine.
 *
 *     run basic [-m KIB] [IMAGE] [PROGRAM.BAS]
 *
 * IMAGE is the interpreter compiled for the VM, /basic.bin by default,
 * built by basic/Makefile with cproc and QBE.  It is loaded at address 0
 * of a memory block of KIB kilobytes (256 by default) that the image, the
 * BASIC program, its variables and strings and the stack all share.  With
 * PROGRAM.BAS the program is loaded and run and the run ends when it
 * does; otherwise the interpreter takes commands from the console until
 * BYE.
 *
 * The interpreter asks for the outside world with TRAP n, the same
 * numbers basic/rt.s and basic/runbasic.c use, and this program serves
 * them with the kernel's console, clock and card.  Ctrl-C is taken raw
 * so that it stops the BASIC program and returns to READY rather than
 * ending this one.
 *
 * The image is about 100 KiB: every instruction of the 32-bit machine is
 * four bytes and most operands another four.  The boards Freya has today
 * keep 56 KiB for a program and its data, so on them this reports that
 * there is not enough memory.  A board with a larger heap runs it as is;
 * the PC does with basic/runbasic.c.
 */
#include "freya_api.h"

#define DEFAULT_IMAGE  "/basic.bin"
#define DEFAULT_KIB    256u
#define STACK_BYTES    16384u
#define SLICE          20000u        /* instructions between key polls */

static const freya_api_t *g;
static uint8_t *s_mem;
static uint32_t s_memsz;
static int s_break;                  /* Ctrl-C seen, not yet reported  */
static int s_done;                   /* the interpreter called exit    */
static int s_status;

static int check_addr(uint32_t a, uint32_t n)
{
    return a <= s_memsz && n <= s_memsz - a;
}

static const char *vm_string(uint32_t a)
{
    uint32_t i;

    for (i = a; i < s_memsz; i++)
        if (s_mem[i] == 0) return (const char *)s_mem + a;
    return 0;
}

static void put_word(uint32_t a, uint32_t v)
{
    s_mem[a]     = (uint8_t)v;
    s_mem[a + 1] = (uint8_t)(v >> 8);
    s_mem[a + 2] = (uint8_t)(v >> 16);
    s_mem[a + 3] = (uint8_t)(v >> 24);
}

/* Ctrl-C arrives as a key; anything else waiting stays for readline. */
static void poll_break(void)
{
    if (g->kbhit()) {
        int c = g->getc_timeout(0);

        if (c == 0x03) s_break = 1;
    }
}

/* A line with its own echo and erase; the newline is not stored. */
static int32_t do_readline(uint32_t buf, uint32_t max)
{
    uint32_t n = 0;

    if (!check_addr(buf, max) || max == 0) return -1;
    for (;;) {
        int c = g->getc();

        if (c < 0) return -1;
        if (c == '\r' || c == '\n') break;
        if (c == 0x03) {
            g->puts("^C\r\n");
            s_break = 1;
            n = 0;
            break;
        }
        if (c == 0x08 || c == 0x7f) {
            if (n) {
                n--;
                g->puts("\b \b");
            }
            continue;
        }
        if (c >= ' ' && n < max - 1) {
            s_mem[buf + n++] = (uint8_t)c;
            g->putc((char)c);
        }
    }
    g->puts("\r\n");
    s_mem[buf + n] = 0;
    return (int32_t)n;
}

static int syscall_(freya_vm_t *vm, unsigned num)
{
    uint32_t a0 = vm->r[0], a1 = vm->r[1], a2 = vm->r[2];
    const char *s;
    int32_t r = -1;

    switch (num) {
    case 0: /* exit(code) */
        s_done = 1;
        s_status = (int)a0;
        return 1;
    case 1: /* putc(c) */
        if ((a0 & 0xff) == '\n') g->putc('\r');
        g->putc((char)a0);
        r = 0;
        break;
    case 2: /* readline(buf, max) -> length or -1 */
        r = do_readline(a0, a1);
        break;
    case 3: /* break() -> 1 once Ctrl-C was seen */
        poll_break();
        r = s_break;
        s_break = 0;
        break;
    case 4: /* open(path, mode 0 read / 1 write) -> fd or -1 */
        s = vm_string(a0);
        if (s)
            r = g->open(s, a1 ? FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC
                              : FREYA_O_RDONLY);
        break;
    case 5: /* close(fd) */
        r = g->close((int)a0);
        break;
    case 6: /* read(fd, buf, n) */
        if (check_addr(a1, a2)) r = g->read((int)a0, s_mem + a1, (int)a2);
        break;
    case 7: /* write(fd, buf, n) */
        if (check_addr(a1, a2)) r = g->write((int)a0, s_mem + a1, (int)a2);
        break;
    case 8: /* ticks in milliseconds */
        r = (int32_t)g->ticks_ms();
        break;
    case 9: /* unlink(path) */
        s = vm_string(a0);
        if (s) r = g->unlink(s);
        break;
    default:
        g->printf("basic: unknown trap %u at %u\r\n", num, vm->r[FREYA_VM_PC]);
        return 0;
    }
    vm->r[0] = (uint32_t)r;
    return 1;
}

static int parse_kib(const char *s, uint32_t *out)
{
    uint32_t v = 0;

    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 1000000u) return -1;
        v = v * 10u + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}

/* The image goes to address 0; -1 with the reason printed. */
static int load_image(const char *path, uint32_t *size)
{
    int fd, n;
    int32_t sz;

    fd = g->open(path, FREYA_O_RDONLY);
    if (fd < 0) {
        g->printf("basic: cannot open %s\r\n", path);
        return -1;
    }
    sz = g->fsize(fd);
    if (sz <= 0 || (uint32_t)sz + STACK_BYTES + 8192u > s_memsz) {
        g->printf("basic: %s is %ld bytes; %u KiB of memory is not enough\r\n",
                  path, (long)sz, s_memsz / 1024u);
        g->close(fd);
        return -1;
    }
    n = g->read(fd, s_mem, (int)sz);
    g->close(fd);
    if (n != sz) {
        g->printf("basic: short read of %s\r\n", path);
        return -1;
    }
    *size = (uint32_t)sz;
    return 0;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    const char *image = DEFAULT_IMAGE, *program = 0;
    uint32_t kib = DEFAULT_KIB, size, halt, sp, heap_lo, heap_hi, flags = 0, ran;
    freya_vm_t vm;
    int i, rc, raw, npos = 0;

    g = api;
    if (!FREYA_API_HAS(api, vm_run) || !FREYA_API_HAS(api, console_raw)) {
        api->puts("basic: this kernel has no virtual machine\r\n");
        return FREYA_EXIT_FAIL;
    }
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'm' && argv[i][2] == 0 && i + 1 < argc) {
            if (parse_kib(argv[++i], &kib) || kib < 64u) {
                api->puts("basic: -m needs a size in KiB, 64 or more\r\n");
                return FREYA_EXIT_USAGE;
            }
        } else if (argv[i][0] == '-') {
            api->puts("usage: basic [-m KIB] [IMAGE] [PROGRAM.BAS]\r\n");
            return FREYA_EXIT_USAGE;
        } else if (npos == 0) {
            image = argv[i];
            npos++;
        } else if (npos == 1) {
            program = argv[i];
            npos++;
        } else {
            api->puts("usage: basic [-m KIB] [IMAGE] [PROGRAM.BAS]\r\n");
            return FREYA_EXIT_USAGE;
        }
    }
    /* one argument that ends in .BAS is the program, the image default */
    if (npos == 1) {
        const char *e = image;
        int len = 0;

        while (e[len]) len++;
        if (len > 4 && (e[len - 4] == '.')
        && (e[len - 3] == 'b' || e[len - 3] == 'B')
        && (e[len - 2] == 'a' || e[len - 2] == 'A')
        && (e[len - 1] == 's' || e[len - 1] == 'S')) {
            program = image;
            image = DEFAULT_IMAGE;
        }
    }

    s_memsz = kib * 1024u;
    s_mem = api->malloc(s_memsz);
    if (!s_mem) {
        api->printf("basic: no %u KiB of memory for the machine\r\n", kib);
        return FREYA_EXIT_FAIL;
    }
    for (i = 0; (uint32_t)i < s_memsz; i++) s_mem[i] = 0;
    if (load_image(image, &size)) {
        api->free(s_mem);
        return FREYA_EXIT_FAIL;
    }

    /* the frame of main(heap_lo, heap_hi, flags) under a HALT */
    halt = s_memsz - 4;
    put_word(halt, 0);
    sp = halt - 16;
    heap_lo = (size + 3) & ~3u;
    heap_hi = s_memsz - STACK_BYTES;
    if (program) flags |= 1;                /* batch: no banner, exit at the end */
    put_word(sp, halt);
    put_word(sp + 4, heap_lo);
    put_word(sp + 8, heap_hi);
    put_word(sp + 12, flags);

    api->vm_reset(&vm);
    vm.r[FREYA_VM_SP] = sp;
    vm.r[FREYA_VM_PC] = 0;

    raw = api->console_raw(1);
    {
        /* With a program the interpreter's first two lines of input are
         * OLD "name" and RUN, as if typed. */
        static const char *queued[2];
        static char oldcmd[80];
        int nq = 0;

        if (program) {
            const char *p = program;
            int n = 0;

            oldcmd[n++] = 'O'; oldcmd[n++] = 'L'; oldcmd[n++] = 'D';
            oldcmd[n++] = ' '; oldcmd[n++] = '"';
            while (*p && n < (int)sizeof oldcmd - 3) oldcmd[n++] = *p++;
            oldcmd[n++] = '"';
            oldcmd[n] = 0;
            queued[nq++] = oldcmd;
            queued[nq++] = "RUN";
        }
        for (;;) {
            rc = api->vm_run(&vm, s_mem, s_memsz, SLICE, &ran);
            if (rc == 0 || rc == FREYA_VM_LIMIT) {
                poll_break();
                continue;
            }
            if (rc == FREYA_VM_TRAP) {
                uint32_t pc = vm.r[FREYA_VM_PC];
                unsigned num = s_mem[pc - 4];

                if (num == 2 && nq) {
                    /* a queued command instead of the console */
                    const char *s = queued[0];
                    uint32_t n = 0, buf = vm.r[0], max = vm.r[1];

                    queued[0] = queued[1];
                    nq--;
                    if (check_addr(buf, max) && max) {
                        while (*s && n < max - 1) s_mem[buf + n++] = (uint8_t)*s++;
                        s_mem[buf + n] = 0;
                        vm.r[0] = n;
                    } else
                        vm.r[0] = (uint32_t)-1;
                    continue;
                }
                if (!syscall_(&vm, num) || s_done) break;
                continue;
            }
            break;
        }
    }
    api->console_raw(raw);

    if (s_done) {
        api->free(s_mem);
        return s_status ? FREYA_EXIT_FAIL : FREYA_EXIT_OK;
    }
    if (rc != FREYA_VM_HALT) {
        api->printf("basic: machine stopped, status %d at pc=%u\r\n", rc, vm.r[FREYA_VM_PC]);
        for (i = 0; i < FREYA_VM_NREGS; i++)
            api->printf("  r%d=%u\r\n", i, vm.r[i]);
        api->free(s_mem);
        return FREYA_EXIT_FAIL;
    }
    api->free(s_mem);
    return vm.r[0] ? FREYA_EXIT_FAIL : FREYA_EXIT_OK;
}
