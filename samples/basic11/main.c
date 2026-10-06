/*
 * basic11 - BASIC-11 compiled for the board itself.
 *
 *     run basic11 [-m KIB] [PROGRAM.BAS | -e TEXT]
 *
 * The interpreter in basic/basic.c, the one samples/basic11vm runs on the
 * PDP-11 virtual machine, built with arm-none-eabi-gcc for the Cortex-M4F, the
 * Cortex-M33 and the Cortex-M3.  Its numbers are single-precision floats
 * (basic/fpnat.c): the FPU's on the Black Pill, the STM32F405, the Black
 * Pill 2, the STM32U585, the STM32H523, the STM32H562 and the STM32H723,
 * and on the Blue
 * Pill, whose Cortex-M3 has no floating point, the soft-float helpers of
 * src/softfp.c, which the Makefile links into every program there.
 *
 * The BASIC program, its variables and its strings live in KIB kilobytes
 * taken from the kernel heap: what -m asks for, or as much as the kernel
 * will give up to 32 KiB, with the interpreter's stack beside it.  The Blue Pill has no such heap.  There the
 * interpreter is built small (BAS_SMALL in basic.c) and its workspace is
 * a fixed part of this program's own RAM window, and -m is refused.
 *
 * With PROGRAM.BAS the program is loaded and run and the run ends when it
 * does, with status 1 after an error.  -e TEXT does the same with the
 * program's lines given as the text itself, one per line; it is how a
 * kernel built with BASIC=file starts the program it carries.  Otherwise
 * the interpreter takes commands from the console until BYE.
 *
 * basic.c wants its system calls and a setjmp: sys.c, written against
 * the Freya API and shared with basic11vm, and basic/armrt.h.
 */
#include "freya_api.h"

#define BAS_BANNER "BASIC-11 for Freya"
#ifndef __ARM_FP
#define BAS_SMALL
/*
 * The Blue Pill.  This program starts no threads and owns the whole of
 * its 9 KiB window (NOTHREADS and WHOLE_WINDOW in the Makefile).  Its
 * variables are at the bottom.  The program stack is the shell's, the
 * 2560 bytes directly above the window, and the top STACK_EXTRA bytes of
 * the window are left to it to grow into; the workspace is what lies
 * between.  The interpreter stops nesting STACK_MARGIN bytes short of
 * the bottom of that, which leaves room for the deepest calls below an
 * expression and the kernel's under them.
 */
#define WINDOW_END   (FREYA_APP_LOAD_ADDR + FREYA_APP_NOTHREADS_SIZE)
#define STACK_EXTRA  1024u
#define STACK_MARGIN 384u
#define BAS_STACK_FLOOR (WINDOW_END - STACK_EXTRA + STACK_MARGIN)
#else
/*
 * Every other board.  The shell's stack is 6 KiB, with the program's
 * window right under it, and a program run from flash has its code
 * copied to the top of that window: an expression nested NNEST deep
 * inside DEFs NFN deep needs more than the 6 KiB, and would write over
 * the interpreter itself.  So the interpreter runs on a stack of its
 * own, STACK_BYTES at the top of the block it takes from the heap.
 * The deepest a program can make it, NNEST levels inside DEFs NFN deep,
 * is 7.6 KiB with GCC -Os, as basic-arm -s measures it (basic/armrt.c),
 * which leaves room for the kernel's calls below that.  The floor is a
 * margin above the bottom that the nesting limit should never let the
 * stack reach; it stops the interpreter with ?Out of memory if it does,
 * rather than over the workspace.
 */
#define STACK_BYTES  12288u
#define STACK_MARGIN 1536u
static uintptr_t stack_floor;
#define BAS_STACK_FLOOR stack_floor
#endif
#include "../../basic/basic.c"

#ifdef BAS_SMALL
extern char __bss_used__[], __bss_end__[];
#else
#define DEFAULT_KIB  32u
#define MIN_KIB      12u        /* 8 KiB of workspace plus the scratch area */
#endif
#include "../../basic/armrt.h"

#include "sys.c"

/* ------------------------------------------------------------------ */

#ifndef BAS_SMALL
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
#endif

#ifdef BAS_SMALL
#define USAGE "usage: basic11 [PROGRAM.BAS | -e TEXT]\r\n"
#else
#define USAGE "usage: basic11 [-m KIB] [PROGRAM.BAS | -e TEXT]\r\n"
#endif

int app_main(const freya_api_t *api, int argc, char **argv)
{
    const char *program = 0;
    uint32_t flags = 0;
#ifdef BAS_SMALL
    uint32_t work_bytes;
#else
    uint32_t kib = 0;
#endif
    uint8_t *heap = 0;
    static char oldcmd[80];
    int i;

    g = api;
    if (!FREYA_API_HAS(api, console_raw)) {
        api->puts("basic11: this kernel has no raw console\r\n");
        return FREYA_EXIT_FAIL;
    }
    for (i = 1; i < argc; i++) {
#ifndef BAS_SMALL
        if (argv[i][0] == '-' && argv[i][1] == 'm' && argv[i][2] == 0 && i + 1 < argc) {
            if (parse_kib(argv[++i], &kib) || kib < MIN_KIB) {
                api->printf("basic11: -m needs a size in KiB, %u or more\r\n", MIN_KIB);
                return FREYA_EXIT_USAGE;
            }
            continue;
        }
#endif
        if (argv[i][0] == '-' && argv[i][1] == 'e' && argv[i][2] == 0 &&
            i + 1 < argc && !program && !text) {
            text = argv[++i];
        } else if (argv[i][0] == '-' || program || text) {
            api->puts(USAGE);
            return FREYA_EXIT_USAGE;
        } else {
            program = argv[i];
        }
    }

#ifdef BAS_SMALL
    /* Everything from the end of the variables to the stack's room.  A
     * program image copied to RAM would sit at the top of the window,
     * so check that the linker really gave this program all of it. */
    if ((uintptr_t)__bss_end__ != WINDOW_END) {
        api->puts("basic11: not linked to own the program window\r\n");
        return FREYA_EXIT_FAIL;
    }
    heap = (uint8_t *)(((uintptr_t)__bss_used__ + 7u) & ~(uintptr_t)7u);
    work_bytes = (uint32_t)(WINDOW_END - STACK_EXTRA - (uintptr_t)heap);
#else
    /* the workspace: what was asked for, or the most the kernel has,
     * and the interpreter's stack above it in the same block */
    if (kib) {
        heap = api->malloc(kib * 1024u + STACK_BYTES);
    } else {
        for (kib = DEFAULT_KIB; kib >= MIN_KIB && !heap; kib -= 4u)
            heap = api->malloc(kib * 1024u + STACK_BYTES);
        if (heap) kib += 4u;
    }
    if (!heap) {
        api->printf("basic11: no %u KiB of memory for the workspace\r\n",
                    kib < MIN_KIB ? MIN_KIB : kib);
        return FREYA_EXIT_FAIL;
    }
    stack_floor = (uintptr_t)heap + kib * 1024u + STACK_MARGIN;
#endif

    if (text) {
        /* the lines of the text, then RUN */
        queued[nqueued++] = "RUN";
        flags |= 1;                     /* batch: no banner, exit at the end */
        scripted = 1;
    } else if (program) {
        /* the first two lines of input are OLD "name" and RUN */
        const char *p = program;
        int n = 0;

        oldcmd[n++] = 'O'; oldcmd[n++] = 'L'; oldcmd[n++] = 'D';
        oldcmd[n++] = ' '; oldcmd[n++] = '"';
        while (*p && n < (int)sizeof oldcmd - 3) oldcmd[n++] = *p++;
        oldcmd[n++] = '"';
        oldcmd[n] = 0;
        queued[nqueued++] = oldcmd;
        queued[nqueued++] = "RUN";
        flags |= 1;                     /* batch: no banner, exit at the end */
        scripted = 1;
    }

    /* The interpreter leaves through sys_exit(), and the kernel frees
     * the workspace and turns raw mode off when the run ends. */
    api->console_raw(1);
#ifdef BAS_SMALL
    return bas_main(heap, work_bytes, flags);
#else
    return bas_main_on(heap, kib * 1024u, flags,
                       heap + kib * 1024u + STACK_BYTES);
#endif
}
