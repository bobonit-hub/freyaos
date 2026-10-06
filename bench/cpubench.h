/*
 * Freya - CPU performance tests: Dhrystone 2.1 and Whetstone.
 *
 * bench/cpubench.c is plain C with no library behind it but <math.h>.
 * samples/cpubench builds it into a program for the boards, and the
 * Linux shell builds it in as the cpubench() command.  Each gives it a
 * way to print, a millisecond clock and the Ctrl-C test.
 */
#ifndef FREYA_CPUBENCH_H
#define FREYA_CPUBENCH_H

#include <stdint.h>

typedef struct {
    int      (*printf)(const char *fmt, ...);  /* %s %c %d %u %x only   */
    uint32_t (*ticks_ms)(void);
    int      (*should_stop)(void);             /* non-zero after Ctrl-C */
    uint32_t cpu_hz;                           /* 0 when not known      */
} cpubench_io_t;

#define CPUBENCH_USAGE  "cpubench [-i|-f] [-t seconds]"

/* argv[0] is the name.  0 when every test ran and checked out, 1 when
 * one failed its check or Ctrl-C stopped it, 2 for a bad argument. */
int cpubench(const cpubench_io_t *io, int argc, char **argv);

#endif /* FREYA_CPUBENCH_H */
