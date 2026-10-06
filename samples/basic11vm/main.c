/*
 * basic11vm - BASIC-11 on the Freya virtual machine.
 *
 *     run basic11vm [-m KIB] [-i IMAGE] [PROGRAM.BAS | -e TEXT]
 *
 * The interpreter of samples/basic11, the same basic/basic.c, compiled
 * for the PDP-11 of the kernel's vm_run() with cproc and QBE instead of
 * for the Cortex-M: basic/Makefile's "make vm" builds it as basic11.vm,
 * which this program loads from the card, /basic11.vm unless -i names
 * another file.  Its numbers are the same IEEE floats, computed with
 * integers (basic/fpsoft.c) to the bits the FPU gives, so a program
 * prints the same here as under basic11, only slower.  The arguments
 * are basic11's: -m the workspace in KiB, as much as there is room for
 * up to 32 by default; a program to load and run, or -e and its text.
 *
 * The machine's memory is the program window from the end of this
 * program's variables to the window's end, or to this program's own
 * code when the loader copied it to the top: the image at address 0,
 * then the workspace bas_main() is given, then the stack, then a HALT
 * for bas_main() to return to.  The image is 128 KiB, so only the
 * boards with a window of 168 KiB or more build this program.
 *
 * The image asks for the outside world with TRAP n, n from
 * basic/vmsys.h, and each is served by the function of
 * samples/basic11/sys.c that basic11 calls for the same thing.
 */
#include "freya_api.h"
#include "../../basic/bas.h"
#include "../../basic/vmsys.h"

#define DEFAULT_IMAGE "/basic11.vm"
#define DEFAULT_KIB   32u
#define MIN_KIB       12u        /* 8 KiB of workspace plus the scratch area */
/* The interpreter's deepest nesting takes 12 KiB of the machine's stack. */
#define STACK_BYTES   (20u * 1024u)
#define STACK_GUARD   0x5afe57acu
#define SLICE         20000u     /* instructions between looks at the stack */

#include "../basic11/sys.c"

extern char __bss_end__[];
extern const freya_app_header_t freya_header;

static uint8_t *mem;            /* the machine's address 0 */
static uint32_t memsz;

static void put_word(uint32_t a, uint32_t v)
{
    mem[a] = (uint8_t)v;
    mem[a + 1] = (uint8_t)(v >> 8);
    mem[a + 2] = (uint8_t)(v >> 16);
    mem[a + 3] = (uint8_t)(v >> 24);
}

/* n bytes at a in the machine, or NULL when they are not all there */
static void *span(uint32_t a, uint32_t n)
{
    if (a > memsz || n > memsz - a) return NULL;
    return mem + a;
}

/* a string at a, or NULL when it runs off the end of memory */
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

    switch (num) {
    case VMSYS_EXIT:
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
        if ((p = span(a0, 6 * 4)) != NULL) r = sys_clock(p);
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
    default:
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

#define USAGE "usage: basic11vm [-m KIB] [-i IMAGE] [PROGRAM.BAS | -e TEXT]\r\n"

int app_main(const freya_api_t *api, int argc, char **argv)
{
    const char *program = 0, *image = DEFAULT_IMAGE;
    uintptr_t lo, hi, code;
    uint32_t kib = 0, room, size, heap, stack_lo, halt, sp, flags = 0, ran;
    static char oldcmd[80];
    freya_vm_t vm;
    int fd, n, i, rc;

    g = api;
    if (!FREYA_API_HAS(api, console_raw) || !FREYA_API_HAS(api, vm_run)) {
        api->puts("basic11vm: this kernel has no raw console or no virtual machine\r\n");
        return FREYA_EXIT_FAIL;
    }
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'm' && argv[i][2] == 0 && i + 1 < argc) {
            if (parse_kib(argv[++i], &kib) || kib < MIN_KIB) {
                api->printf("basic11vm: -m needs a size in KiB, %u or more\r\n", MIN_KIB);
                return FREYA_EXIT_USAGE;
            }
        } else if (argv[i][0] == '-' && argv[i][1] == 'i' && argv[i][2] == 0 && i + 1 < argc) {
            image = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] == 'e' && argv[i][2] == 0 &&
                   i + 1 < argc && !program && !text) {
            text = argv[++i];
        } else if (argv[i][0] == '-' || program || text) {
            api->puts(USAGE);
            return FREYA_EXIT_USAGE;
        } else {
            program = argv[i];
        }
    }

    /* The machine's memory: the window past the variables, up to this
     * program's code when the loader put that at the top. */
    lo = ((uintptr_t)__bss_end__ + 7u) & ~(uintptr_t)7u;
    hi = FREYA_APP_LOAD_ADDR + FREYA_APP_REGION_SIZE;
    code = (uintptr_t)&freya_header;
    if (code >= lo && code < hi) hi = code & ~(uintptr_t)7u;
    mem = (uint8_t *)lo;
    room = (uint32_t)(hi - lo);

    fd = api->open(image, FREYA_O_RDONLY);
    if (fd < 0) {
        api->printf("basic11vm: cannot open %s - build it with make -C basic vm "
                    "and copy it to the card\r\n", image);
        return FREYA_EXIT_FAIL;
    }
    size = 0;
    while (size < room && (n = api->read(fd, mem + size, (int)(room - size))) > 0)
        size += (uint32_t)n;
    api->close(fd);
    if (size == 0 || size >= room) {
        api->printf("basic11vm: %s is not an image that fits %u KiB\r\n",
                    image, (unsigned)(room / 1024u));
        return FREYA_EXIT_FAIL;
    }

    /* the workspace: what was asked for, or the most there is room for */
    heap = (size + 7u) & ~7u;
    if (!kib) {
        for (kib = DEFAULT_KIB; kib > MIN_KIB; kib -= 4u)
            if (heap + kib * 1024u + STACK_BYTES + 4u <= room) break;
    }
    if (heap + kib * 1024u + STACK_BYTES + 4u > room) {
        api->printf("basic11vm: no room for %u KiB of workspace beside the image\r\n",
                    (unsigned)kib);
        return FREYA_EXIT_FAIL;
    }
    memsz = heap + kib * 1024u + STACK_BYTES + 4u;
    stack_lo = heap + kib * 1024u;
    put_word(stack_lo, STACK_GUARD);

    if (text) {
        queued[nqueued++] = "RUN";
        flags |= 1;
        scripted = 1;
    } else if (program) {
        const char *p = program;

        n = 0;
        oldcmd[n++] = 'O'; oldcmd[n++] = 'L'; oldcmd[n++] = 'D';
        oldcmd[n++] = ' '; oldcmd[n++] = '"';
        while (*p && n < (int)sizeof oldcmd - 3) oldcmd[n++] = *p++;
        oldcmd[n++] = '"';
        oldcmd[n] = 0;
        queued[nqueued++] = oldcmd;
        queued[nqueued++] = "RUN";
        flags |= 1;
        scripted = 1;
    }

    /* a HALT at the top, then the frame of bas_main(heap, size, flags) */
    halt = memsz - 4u;
    put_word(halt, 0);
    sp = halt - 16u;
    put_word(sp, halt);
    put_word(sp + 4u, heap);
    put_word(sp + 8u, kib * 1024u);
    put_word(sp + 12u, flags);

    /* The interpreter leaves through sys_exit(), and the kernel turns
     * raw mode off when the run ends. */
    api->console_raw(1);
    api->vm_reset(&vm);
    vm.r[FREYA_VM_SP] = sp;
    vm.r[FREYA_VM_PC] = 0;
    for (;;) {
        rc = api->vm_run(&vm, mem, memsz, SLICE, &ran);
        if (vm.r[FREYA_VM_SP] < stack_lo + 4u ||
            mem[stack_lo] != (uint8_t)STACK_GUARD ||
            mem[stack_lo + 3u] != (uint8_t)(STACK_GUARD >> 24)) {
            api->puts("\r\nbasic11vm: the interpreter ran out of stack\r\n");
            return FREYA_EXIT_FAIL;
        }
        if (rc == 0) continue;
        if (rc == FREYA_VM_TRAP && syscall_(&vm, mem[vm.r[FREYA_VM_PC] - 4u])) continue;
        break;
    }
    if (rc == FREYA_VM_HALT) return (int)vm.r[0];
    api->printf("\r\nbasic11vm: the machine stopped with %d at pc=0x%x\r\n",
                rc, (unsigned)vm.r[FREYA_VM_PC]);
    return FREYA_EXIT_FAIL;
}
