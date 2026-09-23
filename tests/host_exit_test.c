/*
 * Freya - exit status rule.
 *
 * The kernel decides a run's status in one place, freya_exit_status() in
 * include/freya_api.h, so that the shell, the log and a program that asks
 * for the previous run all report the same number.  That rule is a pure
 * function of the reason and the code, which makes it the one part of the
 * loader that can be checked off the board.
 *
 * Checked here: the truncation to a byte, the 128 + reason statuses the
 * kernel synthesises, the constants programs are given to return, and the
 * feature test a program uses on a service table older than itself.
 */
#include <stdio.h>
#include <string.h>

#include "freya_api.h"

static int checks, fails;

static void check(const char *what, int expected, int got)
{
    checks++;
    if (expected == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %d, got %d\n", what, expected, got);
        fails++;
    }
}

int main(void)
{
    freya_api_t api;

    /* A program that ended the run itself: its code, as a byte. */
    check("app_main() returning 0 is a success",
          FREYA_EXIT_OK, freya_exit_status(FREYA_STOP_NONE, 0));
    check("a return value is kept",
          7, freya_exit_status(FREYA_STOP_NONE, 7));
    check("exit(2) is a usage status",
          FREYA_EXIT_USAGE, freya_exit_status(FREYA_STOP_EXIT, FREYA_EXIT_USAGE));
    check("exit(255) is the largest status",
          FREYA_EXIT_MAX, freya_exit_status(FREYA_STOP_EXIT, 255));
    check("exit(-1) reads back as 255, the way a shell reports it",
          255, freya_exit_status(FREYA_STOP_EXIT, -1));
    check("exit(256) is 0, because a status is a byte",
          0, freya_exit_status(FREYA_STOP_EXIT, 256));
    check("exit(1000) keeps the low byte",
          1000 & 0xFF, freya_exit_status(FREYA_STOP_EXIT, 1000));

    /* A run the kernel ended: 128 + the reason, code ignored. */
    check("Ctrl-C is 130, as it is in a POSIX shell",
          130, freya_exit_status(FREYA_STOP_CTRLC, 0));
    check("FREYA_EXIT_STOPPED is the status a stopped run reports",
          FREYA_EXIT_STOPPED, freya_exit_status(FREYA_STOP_CTRLC, 0));
    check("a stopped run ignores whatever the program had returned",
          FREYA_EXIT_STOPPED, freya_exit_status(FREYA_STOP_CTRLC, 5));
    check("a hard fault is 128 + its reason",
          FREYA_EXIT_KILLED + FREYA_STOP_HARDFAULT,
          freya_exit_status(FREYA_STOP_HARDFAULT, 0));
    check("a memory fault is 128 + its reason",
          FREYA_EXIT_KILLED + FREYA_STOP_MEMFAULT,
          freya_exit_status(FREYA_STOP_MEMFAULT, 0));
    check("a bus fault is 133",
          133, freya_exit_status(FREYA_STOP_BUSFAULT, 0));
    check("a usage fault is 134",
          134, freya_exit_status(FREYA_STOP_USAGEFAULT, 0));
    check("no killed run ever reports success",
          1, freya_exit_status(FREYA_STOP_CTRLC, 0) != FREYA_EXIT_OK);
    check("a killed status still fits a byte",
          1, freya_exit_status(FREYA_STOP_USAGEFAULT, 0) <= FREYA_EXIT_MAX);
    check("a reason the kernel never sets falls back to the code",
          4, freya_exit_status(99, 4));

    /* The statuses the shell hands out for its own failures. */
    check("'command not found' is 127, out of a program's way",
          127, FREYA_EXIT_NOTFOUND);
    check("an image the loader refused is 126, as a shell reports it",
          126, FREYA_EXIT_NOEXEC);
    check("a program can still return 127 itself",
          FREYA_EXIT_NOTFOUND,
          freya_exit_status(FREYA_STOP_EXIT, FREYA_EXIT_NOTFOUND));

    /* The ABI shape the kernel fills in. */
    check("freya_exit_t is 32 bytes", 32, (int)sizeof(freya_exit_t));
    check("a name is 20 bytes, the same as the kernel keeps",
          20, (int)sizeof(((freya_exit_t *)0)->name));

    /* How a program tests for a call appended after it was written. */
    memset(&api, 0, sizeof(api));
    api.size = sizeof(freya_api_t);
    check("a full table has last_exit", 1, FREYA_API_HAS(&api, last_exit) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, last_exit) + sizeof(api.last_exit);
    check("a table ending exactly at last_exit has it",
          1, FREYA_API_HAS(&api, last_exit) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, last_exit);
    check("a table that stops short of it does not",
          0, FREYA_API_HAS(&api, last_exit) ? 1 : 0);
    check("and has no exit_reason_str either",
          0, FREYA_API_HAS(&api, exit_reason_str) ? 1 : 0);
    api.size = sizeof(freya_api_t);
    check("power is the last call in the table",
          (int)sizeof(freya_api_t),
          (int)(__builtin_offsetof(freya_api_t, power) +
                sizeof(api.power)));
    api.size = __builtin_offsetof(freya_api_t, power);
    check("a kernel from before power does not offer it",
          0, FREYA_API_HAS(&api, power) ? 1 : 0);
    check("but still offers console_raw", 1, FREYA_API_HAS(&api, console_raw) ? 1 : 0);
    api.size = __builtin_offsetof(freya_api_t, console_raw);
    check("a kernel from before console_raw does not offer it",
          0, FREYA_API_HAS(&api, console_raw) ? 1 : 0);
    check("but still offers crypt", 1, FREYA_API_HAS(&api, crypt) ? 1 : 0);

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
