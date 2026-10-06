/* runbasic - run the BASIC image on the PC with the Freya VM.
 *
 *   runbasic [-m kbytes] [-r program.bas] [-s] basic11.vm
 *
 * The image is basic.c compiled for the virtual machine (make vm).  It
 * is loaded at address 0, and _start, the first word, jumps to
 * bas_main(heap, size, flags).  The machine's memory is the image, then
 * the workspace bas_main() is given, -m kilobytes of it as basic-host
 * takes, then the stack, then a HALT for bas_main() to return to.  -r
 * loads and runs a program without the banner and the prompts and
 * exits when it ends; without it the interpreter talks to the terminal.
 *
 * The image asks for the outside world with TRAP n, n from vmsys.h, and
 * each is served by the function of hostrt.c that basic-host calls for
 * the same thing, so what the two print can be compared byte for byte.
 * -s steps one instruction at a time and reports, at the end, how many
 * ran and how deep the stack went.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freya.h"
#include "bas.h"
#include "vmsys.h"

#define STACK_BYTES (64u * 1024u)

uint32_t host_start(const char *program);

static uint8_t *mem;
static uint32_t memsz;
static int stepping;            /* -s */
static uint64_t total_steps;
static uint32_t stack_top, stack_low;

static void report(void)
{
    if (stepping)
        fprintf(stderr, "%llu instructions, %u bytes of stack\n",
                (unsigned long long)total_steps, stack_top - stack_low);
}

static void put_word(uint32_t a, uint32_t v)
{
    mem[a] = (uint8_t)v;
    mem[a + 1] = (uint8_t)(v >> 8);
    mem[a + 2] = (uint8_t)(v >> 16);
    mem[a + 3] = (uint8_t)(v >> 24);
}

/* The host's pointer to n bytes at a, or NULL when they are not all in
 * the machine's memory. */
static void *span(uint32_t a, uint32_t n)
{
    if (a > memsz || n > memsz - a) return NULL;
    return mem + a;
}

/* A string at a, or NULL when it runs off the end of memory. */
static const char *string(uint32_t a)
{
    uint32_t i;

    for (i = a; i < memsz; i++)
        if (mem[i] == 0) return (const char *)mem + a;
    return NULL;
}

/* Serve TRAP num; 0 for a number there is no such call for. */
static int syscall_(freya_vm_t *vm, unsigned num)
{
    uint32_t a0 = vm->r[0], a1 = vm->r[1], a2 = vm->r[2];
    int32_t r = -1;
    const char *s;
    void *p;
    int f[6], i;

    switch (num) {
    case VMSYS_EXIT:
        fflush(stdout);
        report();
        sys_exit((int32_t)a0);
        break;
    case VMSYS_PUTC:
        sys_putc((int32_t)a0);
        r = 0;
        break;
    case VMSYS_READLINE:
        if ((int32_t)a1 > 0 && (p = span(a0, a1)) != NULL)
            r = sys_readline(p, (int32_t)a1, (int32_t)a2);
        break;
    case VMSYS_BREAK:
        r = sys_break();
        break;
    case VMSYS_OPEN:
        if ((s = string(a0)) != NULL) r = sys_open(s, (int32_t)a1);
        break;
    case VMSYS_CLOSE:
        r = sys_close((int32_t)a0);
        break;
    case VMSYS_READ:
        if ((p = span(a1, a2)) != NULL) r = sys_read((int32_t)a0, p, (int32_t)a2);
        break;
    case VMSYS_WRITE:
        if ((p = span(a1, a2)) != NULL) r = sys_write((int32_t)a0, p, (int32_t)a2);
        break;
    case VMSYS_TICKS:
        r = (int32_t)sys_ticks();
        break;
    case VMSYS_UNLINK:
        if ((s = string(a0)) != NULL) r = sys_unlink(s);
        break;
    case VMSYS_CLOCK:
        if (span(a0, sizeof f) == NULL) break;
        r = sys_clock(f);
        if (r == 0)
            for (i = 0; i < 6; i++) put_word(a0 + 4u * (uint32_t)i, (uint32_t)f[i]);
        break;
    case VMSYS_SLEEP:
        sys_sleep(a0);
        r = 0;
        break;
    case VMSYS_INKEY:
        r = sys_inkey();
        break;
    case VMSYS_FLASH_SAVE:
        if ((p = span(a0, a1)) != NULL) r = sys_flash_save(p, (int32_t)a1);
        break;
    case VMSYS_AUTOSTART:
        r = sys_autostart((int32_t)a0);
        break;
    case VMSYS_PIN_MODE:
        r = sys_pin_mode((int32_t)a0, (int32_t)a1);
        break;
    case VMSYS_PIN_READ:
        r = sys_pin_read((int32_t)a0);
        break;
    case VMSYS_PIN_WRITE:
        r = sys_pin_write((int32_t)a0, (int32_t)a1);
        break;
    case VMSYS_PIN_TOGGLE:
        r = sys_pin_toggle((int32_t)a0);
        break;
    case VMSYS_PWM:
        r = sys_pwm((int32_t)a0, a1, a2);
        break;
    case VMSYS_ADC:
        r = sys_adc((int32_t)a0);
        break;
    case VMSYS_PIN_PULL:
        r = sys_pin_pull((int32_t)a0, (int32_t)a1);
        break;
    case VMSYS_PIN_PULL_GET:
        r = sys_pin_pull_get((int32_t)a0);
        break;
    default:
        return 0;
    }
    vm->r[0] = (uint32_t)r;
    return 1;
}

static int usage(void)
{
    fprintf(stderr, "usage: runbasic [-m kbytes] [-r program.bas] [-s] basic11.vm\n");
    return 1;
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    uint32_t size, work = 256 * 1024, heap, halt, sp, ran = 0;
    const char *image = NULL, *program = NULL;
    freya_vm_t vm;
    int rc, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc)
            work = (uint32_t)strtoul(argv[++i], 0, 0) * 1024;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            program = argv[++i];
        else if (!strcmp(argv[i], "-s"))
            stepping = 1;
        else if (argv[i][0] == '-' || image)
            return usage();
        else
            image = argv[i];
    }
    if (!image) return usage();
    f = fopen(image, "rb");
    if (!f) {
        perror(image);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    size = (uint32_t)n;
    heap = (size + 3u) & ~3u;
    memsz = heap + work + STACK_BYTES + 4u;
    mem = calloc(1, memsz);
    if (!mem || fread(mem, 1, size, f) != size) {
        perror(image);
        return 1;
    }
    fclose(f);

    /* a HALT at the top, then the frame of bas_main(heap, work, flags) */
    halt = memsz - 4u;
    put_word(halt, 0);
    sp = halt - 16u;
    put_word(sp, halt);
    put_word(sp + 4u, heap);
    put_word(sp + 8u, work);
    put_word(sp + 12u, host_start(program));
    stack_top = stack_low = sp;

    vm_reset(&vm);
    vm.r[FREYA_VM_SP] = sp;
    vm.r[FREYA_VM_PC] = 0;
    for (;;) {
        rc = vm_run(&vm, mem, memsz, stepping ? 1u : 100000u, &ran);
        total_steps += ran;
        if (vm.r[FREYA_VM_SP] < stack_low) stack_low = vm.r[FREYA_VM_SP];
        if (stepping && stack_low < heap + work) break;
        if (rc == 0 || rc == FREYA_VM_LIMIT) continue;
        if (rc == FREYA_VM_TRAP && syscall_(&vm, mem[vm.r[FREYA_VM_PC] - 4u]))
            continue;
        break;
    }
    fflush(stdout);
    report();
    if (rc == FREYA_VM_HALT) return (int)vm.r[0];
    if (stack_low < heap + work)
        fprintf(stderr, "runbasic: the stack ran into the workspace\n");
    fprintf(stderr, "runbasic: the machine stopped with %d at pc=%#x\n", rc, vm.r[FREYA_VM_PC]);
    for (i = 0; i < FREYA_VM_NREGS; i++)
        fprintf(stderr, "  r%d=%#x\n", i, vm.r[i]);
    return 1;
}
