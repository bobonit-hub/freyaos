/* vmsys.h - the system calls of the BASIC image, by TRAP number.
 *
 * vmrt.s makes each sys_* call of bas.h a TRAP with this number, its
 * arguments in R0-R2 and its result in R0; the programs that run the
 * image, basic/runbasic.c on the PC and samples/basic11vm on a board,
 * serve them.  Keep the three in the same order.
 */
#ifndef VMSYS_H
#define VMSYS_H

enum {
    VMSYS_EXIT,        /* 0 */
    VMSYS_PUTC,        /* 1 */
    VMSYS_READLINE,    /* 2 */
    VMSYS_BREAK,       /* 3 */
    VMSYS_OPEN,        /* 4 */
    VMSYS_CLOSE,       /* 5 */
    VMSYS_READ,        /* 6 */
    VMSYS_WRITE,       /* 7 */
    VMSYS_TICKS,       /* 8 */
    VMSYS_UNLINK,      /* 9 */
    VMSYS_CLOCK,       /* 10 */
    VMSYS_SLEEP,       /* 11 */
    VMSYS_INKEY,       /* 12 */
    VMSYS_FLASH_SAVE,  /* 13 */
    VMSYS_AUTOSTART,   /* 14 */
    VMSYS_PIN_MODE,    /* 15 */
    VMSYS_PIN_READ,    /* 16 */
    VMSYS_PIN_WRITE,   /* 17 */
    VMSYS_PIN_TOGGLE,  /* 18 */
    VMSYS_PWM,         /* 19 */
    VMSYS_ADC,         /* 20 */
    VMSYS_COUNT
};

#endif
