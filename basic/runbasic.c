/* runbasic - run the BASIC image on a PC with the Freya VM.
 *
 *   runbasic [-m kbytes] [-r program.bas] basic.bin
 *
 * The image is loaded at address 0 and _start, the first word of the
 * image, jumps to main(heap_lo, heap_hi, flags).  Memory above the
 * image up to the stack is the heap the interpreter carves up itself.
 * -r loads and runs a program without the banner and the prompts, and
 * exits when it ends; without it the interpreter talks to the terminal.
 *
 * System calls are TRAP instructions; see rt.s for the numbers.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "freya.h"

#define STACK_BYTES 16384

static volatile sig_atomic_t got_int;
static int count_steps;          /* -s: report the instruction count */
static uint64_t total_steps;
static const char *queued[4];
static int nqueued;

static void on_int(int sig)
{
    (void)sig;
    got_int = 1;
}

static int check_addr(uint32_t memsz, uint32_t a, uint32_t n)
{
    return a <= memsz && n <= memsz - a;
}

/* Read one line into the VM buffer; the newline is dropped.  Returns
 * the count, or -1 at end of input. */
static int32_t do_readline(uint8_t *mem, uint32_t memsz, uint32_t buf, uint32_t max)
{
    uint32_t n = 0;
    int c;

    if (!check_addr(memsz, buf, max) || max == 0) return -1;
    if (nqueued) {
        const char *s = queued[0];
        memmove(queued, queued + 1, (size_t)(--nqueued) * sizeof queued[0]);
        while (*s && n < max - 1) mem[buf + n++] = (uint8_t)*s++;
        mem[buf + n] = 0;
        return (int32_t)n;
    }
    fflush(stdout);
    for (;;) {
        c = getchar();
        if (c == EOF) {
            if (errno == EINTR) {
                clearerr(stdin);
                got_int = 1;
                mem[buf] = 0;
                return 0;
            }
            if (n == 0) return -1;
            break;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < max - 1) mem[buf + n++] = (uint8_t)c;
    }
    mem[buf + n] = 0;
    return (int32_t)n;
}

static const char *vm_string(uint8_t *mem, uint32_t memsz, uint32_t a)
{
    uint32_t i;

    for (i = a; i < memsz; i++)
        if (mem[i] == 0) return (const char *)mem + a;
    return NULL;
}

static uint32_t ticks_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static int syscall_(freya_vm_t *vm, uint8_t *mem, uint32_t memsz, unsigned num)
{
    uint32_t a0 = vm->r[0], a1 = vm->r[1], a2 = vm->r[2];
    const char *s;
    int32_t r = -1;

    switch (num) {
    case 0: /* exit */
        fflush(stdout);
        if (count_steps) fprintf(stderr, "%llu instructions\n", (unsigned long long)total_steps);
        exit((int)a0);
    case 1: /* putc */
        putchar((int)(a0 & 0xff));
        if ((a0 & 0xff) == '\n') fflush(stdout);
        r = 0;
        break;
    case 2: /* readline(buf, max) */
        r = do_readline(mem, memsz, a0, a1);
        break;
    case 3: /* break: 1 if ^C was seen since the last call */
        r = got_int;
        got_int = 0;
        break;
    case 4: /* open(path, mode) */
        s = vm_string(mem, memsz, a0);
        if (s) {
            int fd = open(s, a1 ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, 0666);
            r = fd;
        }
        break;
    case 5: /* close(fd) */
        r = close((int)a0);
        break;
    case 6: /* read(fd, buf, n) */
        if (check_addr(memsz, a1, a2)) r = (int32_t)read((int)a0, mem + a1, a2);
        break;
    case 7: /* write(fd, buf, n) */
        if (check_addr(memsz, a1, a2)) r = (int32_t)write((int)a0, mem + a1, a2);
        break;
    case 8: /* ticks in milliseconds */
        r = (int32_t)ticks_ms();
        break;
    case 9: /* unlink(path) */
        s = vm_string(mem, memsz, a0);
        if (s) r = unlink(s);
        break;
    default:
        fprintf(stderr, "runbasic: unknown trap %u\n", num);
        return 0;
    }
    vm->r[0] = (uint32_t)r;
    return 1;
}

static void put_word(uint8_t *mem, uint32_t a, uint32_t v)
{
    mem[a] = (uint8_t)v;
    mem[a + 1] = (uint8_t)(v >> 8);
    mem[a + 2] = (uint8_t)(v >> 16);
    mem[a + 3] = (uint8_t)(v >> 24);
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    uint8_t *mem;
    uint32_t size, memsz = 256 * 1024, halt, sp, heap_lo, heap_hi, flags = 0;
    freya_vm_t vm;
    int rc, i;
    const char *image = NULL, *program = NULL;
    static char oldcmd[600];
    long trace = 0;
    uint32_t ran;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc)
            memsz = (uint32_t)strtoul(argv[++i], 0, 0) * 1024;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            program = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            trace = strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-s"))
            count_steps = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: runbasic [-m kbytes] [-r program.bas] [-t steps] [-s] basic.bin\n");
            return 1;
        } else
            image = argv[i];
    }
    if (!image) {
        fprintf(stderr, "usage: runbasic [-m kbytes] [-r program.bas] [-t steps] [-s] basic.bin\n");
        return 1;
    }
    f = fopen(image, "rb");
    if (!f) {
        perror(image);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    size = (uint32_t)n;
    if (size + STACK_BYTES + 4096 > memsz) {
        fprintf(stderr, "runbasic: %u bytes of memory is too little for a %u byte image\n", memsz, size);
        return 1;
    }
    mem = calloc(1, memsz);
    if (fread(mem, 1, size, f) != size) {
        perror("read");
        return 1;
    }
    fclose(f);

    if (program) {
        snprintf(oldcmd, sizeof oldcmd, "OLD \"%s\"", program);
        queued[nqueued++] = oldcmd;
        queued[nqueued++] = "RUN";
        flags = 1;
    }

    /* HALT at the top, then the frame of main: return address, args. */
    halt = memsz - 4;
    put_word(mem, halt, 0);
    sp = halt - 16;
    put_word(mem, sp, halt);
    heap_lo = (size + 3) & ~3u;
    heap_hi = memsz - STACK_BYTES;
    put_word(mem, sp + 4, heap_lo);
    put_word(mem, sp + 8, heap_hi);
    put_word(mem, sp + 12, flags);

    {
        /* sigaction: signal() may reset to the default after one ^C */
        struct sigaction sa;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_int;
        sigaction(SIGINT, &sa, NULL);
    }
    vm_reset(&vm);
    vm.r[FREYA_VM_SP] = sp;
    vm.r[FREYA_VM_PC] = 0;
    for (;;) {
        if (trace > 0) {
            /* -t n: print the registers before each of the first n steps */
            trace--;
            fprintf(stderr, "pc=%05x sp=%05x r5=%05x r0=%08x r1=%08x r2=%08x r3=%08x\n",
                    vm.r[7], vm.r[6], vm.r[5], vm.r[0], vm.r[1], vm.r[2], vm.r[3]);
            rc = vm_run(&vm, mem, memsz, 1, &ran);
        } else
            rc = vm_run(&vm, mem, memsz, 0, &ran);
        total_steps += ran;
        if (rc == 0 || rc == FREYA_VM_LIMIT) continue;
        if (rc == FREYA_VM_TRAP) {
            uint32_t pc = vm.r[FREYA_VM_PC];
            unsigned num = mem[pc - 4];
            if (!syscall_(&vm, mem, memsz, num)) break;
            continue;
        }
        break;
    }
    fflush(stdout);
    if (count_steps) fprintf(stderr, "%llu instructions\n", (unsigned long long)total_steps);
    if (rc == FREYA_VM_HALT) return (int)vm.r[0];
    fprintf(stderr, "runbasic: vm stopped with status %d at pc=%u\n", rc, vm.r[FREYA_VM_PC]);
    for (i = 0; i < FREYA_VM_NREGS; i++)
        fprintf(stderr, "  r%d=%u (0x%x)\n", i, vm.r[i], vm.r[i]);
    return 1;
}
