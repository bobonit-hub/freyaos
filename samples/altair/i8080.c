/*
 * Freya - Intel 8080 interpreter for the Altair sample.
 *
 * All 256 opcodes, including the twelve the 8080 decodes as duplicates
 * of others: 08h..38h are NOP, CBh is JMP, D9h is RET and DDh, EDh, FDh
 * are CALL.  Flags follow the silicon, not the Z80: the auxiliary carry
 * of a subtraction is the carry out of bit 3 of A + ~operand + 1, ANA
 * sets it from bit 3 of either operand, and DCR sets it unless the low
 * nibble wrapped to F.  Timing is in clock states; a conditional call or
 * return that is taken costs six more than one that is not.
 */
#include "i8080.h"

static uint8_t s_szp[256];      /* S, Z and P of each result byte */

static const uint8_t k_cycles[256] = {
    4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7, 4,
    4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7, 4,
    4, 10, 16, 5,  5,  5,  7,  4,  4, 10, 16, 5,  5,  5,  7, 4,
    4, 10, 13, 5,  10, 10, 10, 4,  4, 10, 13, 5,  5,  5,  7, 4,
    5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7, 5,
    5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7, 5,
    5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7, 5,
    7, 7,  7,  7,  7,  7,  7,  7,  5, 5,  5,  5,  5,  5,  7, 5,
    4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7, 4,
    4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7, 4,
    4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7, 4,
    4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7, 4,
    5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7, 11,
    5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7, 11,
    5, 10, 10, 18, 11, 11, 7,  11, 5, 5,  10, 4,  11, 17, 7, 11,
    5, 10, 10, 4,  11, 11, 7,  11, 5, 5,  10, 4,  11, 17, 7, 11
};

void i8080_init(void)
{
    for (int i = 0; i < 256; i++) {
        int ones = 0;

        for (int b = i; b; b >>= 1)
            ones += b & 1;
        s_szp[i] = (uint8_t)((i & F_S) | (i == 0 ? F_Z : 0) |
                             ((ones & 1) ? 0 : F_P));
    }
}

/* Power-on leaves the registers as they were; only PC, the interrupt
 * enable and the halt latch are defined after a reset. */
void i8080_reset(i8080_t *c, uint16_t pc)
{
    c->pc = pc;
    c->inte = 0;
    c->halted = 0;
    c->r[R_F] = (uint8_t)((c->r[R_F] & 0xD7) | F_ONE);
}

static inline uint16_t fetch16(i8080_t *c)
{
    uint16_t v = mem_rd(c->pc);

    v |= (uint16_t)(mem_rd((uint16_t)(c->pc + 1)) << 8);
    c->pc = (uint16_t)(c->pc + 2);
    return v;
}

static inline uint16_t hl(const i8080_t *c)
{
    return (uint16_t)((c->r[R_H] << 8) | c->r[R_L]);
}

static inline uint8_t get8(i8080_t *c, int i)
{
    return i == 6 ? mem_rd(hl(c)) : c->r[i];
}

static inline void set8(i8080_t *c, int i, uint8_t v)
{
    if (i == 6)
        mem_wr(hl(c), v);
    else
        c->r[i] = v;
}

/* Pair 0..2 is BC, DE, HL; 3 is SP.  PUSH and POP use their own 3. */
static inline uint16_t getrp(const i8080_t *c, int p)
{
    if (p == 3)
        return c->sp;
    return (uint16_t)((c->r[2 * p] << 8) | c->r[2 * p + 1]);
}

static inline void setrp(i8080_t *c, int p, uint16_t v)
{
    if (p == 3) {
        c->sp = v;
    } else {
        c->r[2 * p] = (uint8_t)(v >> 8);
        c->r[2 * p + 1] = (uint8_t)v;
    }
}

static inline void push(i8080_t *c, uint16_t v)
{
    c->sp = (uint16_t)(c->sp - 1);
    mem_wr(c->sp, (uint8_t)(v >> 8));
    c->sp = (uint16_t)(c->sp - 1);
    mem_wr(c->sp, (uint8_t)v);
}

static inline uint16_t pop(i8080_t *c)
{
    uint16_t v = mem_rd(c->sp);

    v |= (uint16_t)(mem_rd((uint16_t)(c->sp + 1)) << 8);
    c->sp = (uint16_t)(c->sp + 2);
    return v;
}

/* NZ, Z, NC, C, PO, PE, P, M */
static inline int cond(const i8080_t *c, int n)
{
    static const uint8_t mask[4] = { F_Z, F_CY, F_P, F_S };
    int set = (c->r[R_F] & mask[n >> 1]) != 0;

    return (n & 1) ? set : !set;
}

/* ADD ADC SUB SBB ANA XRA ORA CMP, in the order the opcode field has. */
static void alu(i8080_t *c, int op, uint8_t v)
{
    unsigned a = c->r[R_A];
    unsigned res;

    switch (op) {
    case 0:
    case 1:
        res = a + v + (op == 1 ? (c->r[R_F] & F_CY) : 0u);
        c->r[R_A] = (uint8_t)res;
        c->r[R_F] = (uint8_t)(s_szp[res & 0xFF] | ((a ^ v ^ res) & F_AC) |
                              (res >> 8) | F_ONE);
        break;
    case 2:
    case 3:
    case 7: {
        unsigned nv = (~v) & 0xFFu;
        unsigned cin = op == 3 ? !(c->r[R_F] & F_CY) : 1u;

        res = a + nv + cin;
        c->r[R_F] = (uint8_t)(s_szp[res & 0xFF] | ((a ^ nv ^ res) & F_AC) |
                              ((res >> 8) ^ 1u) | F_ONE);
        if (op != 7)
            c->r[R_A] = (uint8_t)res;
        break;
    }
    case 4:
        res = a & v;
        c->r[R_A] = (uint8_t)res;
        c->r[R_F] = (uint8_t)(s_szp[res] | (((a | v) & 0x08) << 1) | F_ONE);
        break;
    case 5:
        res = a ^ v;
        c->r[R_A] = (uint8_t)res;
        c->r[R_F] = (uint8_t)(s_szp[res] | F_ONE);
        break;
    default:
        res = a | v;
        c->r[R_A] = (uint8_t)res;
        c->r[R_F] = (uint8_t)(s_szp[res] | F_ONE);
        break;
    }
}

static void daa(i8080_t *c)
{
    unsigned a = c->r[R_A];
    unsigned lo = a & 0x0F, hi = a >> 4;
    unsigned fix = 0;
    uint8_t cy = c->r[R_F] & F_CY;

    if ((c->r[R_F] & F_AC) || lo > 9)
        fix = 0x06;
    if (cy || hi > 9 || (hi >= 9 && lo > 9)) {
        fix |= 0x60;
        cy = F_CY;
    }
    alu(c, 0, (uint8_t)fix);
    c->r[R_F] = (uint8_t)((c->r[R_F] & ~F_CY) | cy);
}

static void rotate(i8080_t *c, int op)
{
    uint8_t a = c->r[R_A];
    uint8_t f = c->r[R_F] & (uint8_t)~F_CY;
    uint8_t cy = c->r[R_F] & F_CY;

    switch (op) {
    case 0:                                         /* RLC */
        cy = a >> 7;
        a = (uint8_t)((a << 1) | cy);
        break;
    case 1:                                         /* RRC */
        cy = a & 1;
        a = (uint8_t)((a >> 1) | (cy << 7));
        break;
    case 2: {                                       /* RAL */
        uint8_t out = a >> 7;
        a = (uint8_t)((a << 1) | cy);
        cy = out;
        break;
    }
    default: {                                      /* RAR */
        uint8_t out = a & 1;
        a = (uint8_t)((a >> 1) | (cy << 7));
        cy = out;
        break;
    }
    }
    c->r[R_A] = a;
    c->r[R_F] = f | cy;
}

/* Instructions 00h..3Fh: the ones with a pair, a register or nothing. */
static void group0(i8080_t *c, uint8_t op)
{
    int r = (op >> 3) & 7;
    int p = (op >> 4) & 3;
    uint16_t a;

    switch (op & 7) {
    case 0:                                         /* NOP and its twins */
        break;
    case 1:
        if (op & 8) {                               /* DAD */
            uint32_t s = (uint32_t)hl(c) + getrp(c, p);
            setrp(c, 2, (uint16_t)s);
            c->r[R_F] = (uint8_t)((c->r[R_F] & ~F_CY) | (s >> 16));
        } else {                                    /* LXI */
            setrp(c, p, fetch16(c));
        }
        break;
    case 2:
        switch (r) {
        case 0: mem_wr(getrp(c, 0), c->r[R_A]); break;          /* STAX B */
        case 1: c->r[R_A] = mem_rd(getrp(c, 0)); break;         /* LDAX B */
        case 2: mem_wr(getrp(c, 1), c->r[R_A]); break;          /* STAX D */
        case 3: c->r[R_A] = mem_rd(getrp(c, 1)); break;         /* LDAX D */
        case 4:                                                 /* SHLD */
            a = fetch16(c);
            mem_wr(a, c->r[R_L]);
            mem_wr((uint16_t)(a + 1), c->r[R_H]);
            break;
        case 5:                                                 /* LHLD */
            a = fetch16(c);
            c->r[R_L] = mem_rd(a);
            c->r[R_H] = mem_rd((uint16_t)(a + 1));
            break;
        case 6: mem_wr(fetch16(c), c->r[R_A]); break;           /* STA */
        default: c->r[R_A] = mem_rd(fetch16(c)); break;         /* LDA */
        }
        break;
    case 3:                                         /* INX / DCX */
        setrp(c, p, (uint16_t)(getrp(c, p) + ((op & 8) ? -1 : 1)));
        break;
    case 4: {                                       /* INR */
        uint8_t v = (uint8_t)(get8(c, r) + 1);
        set8(c, r, v);
        c->r[R_F] = (uint8_t)((c->r[R_F] & F_CY) | s_szp[v] |
                              ((v & 0x0F) == 0 ? F_AC : 0) | F_ONE);
        break;
    }
    case 5: {                                       /* DCR */
        uint8_t v = (uint8_t)(get8(c, r) - 1);
        set8(c, r, v);
        c->r[R_F] = (uint8_t)((c->r[R_F] & F_CY) | s_szp[v] |
                              ((v & 0x0F) != 0x0F ? F_AC : 0) | F_ONE);
        break;
    }
    case 6:                                         /* MVI */
        set8(c, r, mem_rd(c->pc));
        c->pc = (uint16_t)(c->pc + 1);
        break;
    default:
        switch (r) {
        case 0: case 1: case 2: case 3:
            rotate(c, r);
            break;
        case 4:
            daa(c);
            break;
        case 5:                                     /* CMA */
            c->r[R_A] = (uint8_t)~c->r[R_A];
            break;
        case 6:                                     /* STC */
            c->r[R_F] |= F_CY;
            break;
        default:                                    /* CMC */
            c->r[R_F] ^= F_CY;
            break;
        }
        break;
    }
}

/* Instructions C0h..FFh: jumps, calls, the stack, I/O and immediates.
 * Returns the extra clock states a taken conditional costs. */
static uint32_t group3(i8080_t *c, uint8_t op)
{
    int n = (op >> 3) & 7;
    int p = (op >> 4) & 3;
    uint16_t a, t;

    switch (op & 7) {
    case 0:                                         /* Rcc */
        if (cond(c, n)) {
            c->pc = pop(c);
            return 6;
        }
        break;
    case 1:
        if (!(op & 8)) {                            /* POP */
            t = pop(c);
            if (p == 3) {
                c->r[R_A] = (uint8_t)(t >> 8);
                c->r[R_F] = (uint8_t)((t & 0xD7) | F_ONE);
            } else {
                setrp(c, p, t);
            }
        } else if (p <= 1) {                        /* RET, and D9h */
            c->pc = pop(c);
        } else if (p == 2) {                        /* PCHL */
            c->pc = hl(c);
        } else {                                    /* SPHL */
            c->sp = hl(c);
        }
        break;
    case 2:                                         /* Jcc */
        a = fetch16(c);
        if (cond(c, n))
            c->pc = a;
        break;
    case 3:
        switch (n) {
        case 0: case 1:                             /* JMP, and CBh */
            c->pc = fetch16(c);
            break;
        case 2:                                     /* OUT */
            io_out(mem_rd(c->pc), c->r[R_A]);
            c->pc = (uint16_t)(c->pc + 1);
            break;
        case 3:                                     /* IN */
            c->r[R_A] = io_in(mem_rd(c->pc));
            c->pc = (uint16_t)(c->pc + 1);
            break;
        case 4:                                     /* XTHL */
            t = pop(c);
            push(c, hl(c));
            setrp(c, 2, t);
            break;
        case 5:                                     /* XCHG */
            t = getrp(c, 1);
            setrp(c, 1, hl(c));
            setrp(c, 2, t);
            break;
        case 6:
            c->inte = 0;
            break;
        default:
            c->inte = 1;
            break;
        }
        break;
    case 4:                                         /* Ccc */
        a = fetch16(c);
        if (cond(c, n)) {
            push(c, c->pc);
            c->pc = a;
            return 6;
        }
        break;
    case 5:
        if (!(op & 8)) {                            /* PUSH */
            if (p == 3)
                push(c, (uint16_t)((c->r[R_A] << 8) | c->r[R_F]));
            else
                push(c, getrp(c, p));
        } else {                                    /* CALL, and its twins */
            a = fetch16(c);
            push(c, c->pc);
            c->pc = a;
        }
        break;
    case 6:                                         /* ADI .. CPI */
        alu(c, n, mem_rd(c->pc));
        c->pc = (uint16_t)(c->pc + 1);
        break;
    default:                                        /* RST */
        push(c, c->pc);
        c->pc = (uint16_t)(op & 0x38);
        break;
    }
    return 0;
}

/* Runs until at least 'budget' clock states have gone by or the CPU
 * halts, and returns how many went by. */
uint32_t i8080_run(i8080_t *c, uint32_t budget)
{
    uint32_t used = 0;

    while (used < budget && !c->halted) {
        uint8_t op = mem_rd(c->pc);

        c->pc = (uint16_t)(c->pc + 1);
        used += k_cycles[op];

        switch (op >> 6) {
        case 0:
            group0(c, op);
            break;
        case 1:
            if (op == 0x76)
                c->halted = 1;
            else
                set8(c, (op >> 3) & 7, get8(c, op & 7));
            break;
        case 2:
            alu(c, (op >> 3) & 7, get8(c, op & 7));
            break;
        default:
            used += group3(c, op);
            break;
        }
    }
    c->cycles += used;
    return used;
}
