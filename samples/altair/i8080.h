/*
 * Freya - Intel 8080 for the Altair sample.
 *
 * The registers are an array in the order the instruction set encodes
 * them - B, C, D, E, H, L, (M), A - with the flags kept in slot 6, where
 * M would be.  An instruction's register field is then an index, and a
 * register pair is two neighbouring bytes, high first.
 *
 * The CPU reaches memory through mem_rd() and mem_wr() and the I/O ports
 * through io_in() and io_out(); main.c supplies them before it includes
 * i8080.c.
 */
#ifndef ALTAIR_I8080_H
#define ALTAIR_I8080_H

#include <stdint.h>

#define R_B  0
#define R_C  1
#define R_D  2
#define R_E  3
#define R_H  4
#define R_L  5
#define R_F  6
#define R_A  7

/* The flag byte as PUSH PSW stores it: bit 1 is always set, bits 3 and 5
 * always clear. */
#define F_S   0x80
#define F_Z   0x40
#define F_AC  0x10
#define F_P   0x04
#define F_ONE 0x02
#define F_CY  0x01

typedef struct {
    uint8_t  r[8];
    uint16_t sp;
    uint16_t pc;
    uint8_t  inte;          /* EI / DI                                  */
    uint8_t  halted;        /* HLT executed, nothing to wake it         */
    uint32_t cycles;        /* clock states since reset, wraps          */
} i8080_t;

void     i8080_init(void);
void     i8080_reset(i8080_t *c, uint16_t pc);
uint32_t i8080_run(i8080_t *c, uint32_t budget);

#endif /* ALTAIR_I8080_H */
