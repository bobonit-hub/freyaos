/*
 * Freya - a PDP-11 whose registers and words are 32 bits.
 *
 * The opcode in the low 16 bits of each instruction word is a PDP-11
 * opcode, and the condition codes sit in the same bits of the processor
 * status.  A word is 32 bits, so the program counter, the stack and an
 * index word all step by 4.  The high 16 bits of an instruction are
 * reserved and must be zero.
 *
 * The linker script puts this file in the kernel extension.  On the
 * Blue Pill that extension is 47 KiB, and the program region is 4 KiB
 * smaller so the machine fits.
 */
#include "freya.h"

int vm_reset(freya_vm_t *vm)
{
    int i;

    if (!vm) return FREYA_ERR_ARG;
    for (i = 0; i < FREYA_VM_NREGS; i++) vm->r[i] = 0;
    vm->psw = FREYA_VM_Z;
    return 0;
}

static int rd8(const uint8_t *m, uint32_t size, uint32_t a, uint32_t *o)
{
    if (a >= size) return -1;
    *o = m[a];
    return 0;
}

static int wr8(uint8_t *m, uint32_t size, uint32_t a, uint32_t v)
{
    if (a >= size) return -1;
    m[a] = (uint8_t)v;
    return 0;
}

static int rd32(const uint8_t *m, uint32_t size, uint32_t a, uint32_t *o)
{
    if (size < 4u || (a & 3u) || a > size - 4u) return -1;
    *o = (uint32_t)m[a] | ((uint32_t)m[a + 1] << 8) |
         ((uint32_t)m[a + 2] << 16) | ((uint32_t)m[a + 3] << 24);
    return 0;
}

static int wr32(uint8_t *m, uint32_t size, uint32_t a, uint32_t v)
{
    if (size < 4u || (a & 3u) || a > size - 4u) return -1;
    m[a] = (uint8_t)v;
    m[a + 1] = (uint8_t)(v >> 8);
    m[a + 2] = (uint8_t)(v >> 16);
    m[a + 3] = (uint8_t)(v >> 24);
    return 0;
}

static void nz(freya_vm_t *vm, uint32_t v, int byte)
{
    uint32_t sign = byte ? 0x80u : 0x80000000u;
    uint32_t mask = byte ? 0xffu : 0xffffffffu;

    vm->psw &= ~(FREYA_VM_N | FREYA_VM_Z);
    if ((v & mask) == 0) vm->psw |= FREYA_VM_Z;
    if (v & sign) vm->psw |= FREYA_VM_N;
}

/* A source or destination.  reg >= 0 means the operand is that register. */
typedef struct {
    int      reg;
    uint32_t addr;
} ea_t;

static int fetch_ea(freya_vm_t *vm, uint8_t *m, uint32_t size,
                    unsigned mode, unsigned reg, int byte, int is_dst, ea_t *ea)
{
    uint32_t x, step, addr;

    ea->reg = -1;
    if (mode == 0) {
        if (is_dst && byte == 2) return FREYA_VM_ILLEGAL; /* JMP/JSR */
        ea->reg = (int)reg;
        return 0;
    }
    step = (byte && reg < 6) ? 1u : 4u;
    switch (mode) {
    case 1:
        addr = vm->r[reg];
        break;
    case 2:
        addr = vm->r[reg];
        vm->r[reg] += step;
        break;
    case 3:
        if (rd32(m, size, vm->r[reg], &addr)) return FREYA_VM_FAULT;
        vm->r[reg] += 4u;
        break;
    case 4:
        vm->r[reg] -= step;
        addr = vm->r[reg];
        break;
    case 5:
        vm->r[reg] -= 4u;
        if (rd32(m, size, vm->r[reg], &addr)) return FREYA_VM_FAULT;
        break;
    case 6:
    case 7:
        if (rd32(m, size, vm->r[FREYA_VM_PC], &x)) return FREYA_VM_FAULT;
        vm->r[FREYA_VM_PC] += 4u;
        addr = vm->r[reg] + x;
        if (mode == 7 && rd32(m, size, addr, &addr)) return FREYA_VM_FAULT;
        break;
    default:
        return FREYA_VM_ILLEGAL;
    }
    ea->addr = addr;
    return 0;
}

static int read_op(freya_vm_t *vm, uint8_t *m, uint32_t size,
                   const ea_t *ea, int byte, uint32_t *o)
{
    if (ea->reg >= 0) {
        *o = vm->r[ea->reg];
        if (byte) *o &= 0xffu;
        return 0;
    }
    return byte ? rd8(m, size, ea->addr, o) ? FREYA_VM_FAULT : 0
                : rd32(m, size, ea->addr, o) ? FREYA_VM_FAULT : 0;
}

static int write_op(freya_vm_t *vm, uint8_t *m, uint32_t size,
                    const ea_t *ea, int byte, uint32_t v)
{
    if (ea->reg >= 0) {
        if (byte) vm->r[ea->reg] = (vm->r[ea->reg] & ~0xffu) | (v & 0xffu);
        else vm->r[ea->reg] = v;
        return 0;
    }
    return byte ? wr8(m, size, ea->addr, v) ? FREYA_VM_FAULT : 0
                : wr32(m, size, ea->addr, v) ? FREYA_VM_FAULT : 0;
}

static int push(freya_vm_t *vm, uint8_t *m, uint32_t size, uint32_t v)
{
    vm->r[FREYA_VM_SP] -= 4u;
    if (wr32(m, size, vm->r[FREYA_VM_SP], v)) return FREYA_VM_FAULT;
    return 0;
}

static int pop(freya_vm_t *vm, uint8_t *m, uint32_t size, uint32_t *o)
{
    if (rd32(m, size, vm->r[FREYA_VM_SP], o)) return FREYA_VM_FAULT;
    vm->r[FREYA_VM_SP] += 4u;
    return 0;
}

static void logic_flags(freya_vm_t *vm, uint32_t v, int byte, int set_c, int c)
{
    vm->psw &= ~FREYA_VM_V;
    if (set_c) {
        vm->psw &= ~FREYA_VM_C;
        if (c) vm->psw |= FREYA_VM_C;
    }
    nz(vm, v, byte);
}

static void add_flags(freya_vm_t *vm, uint32_t a, uint32_t b, uint32_t r, int byte)
{
    uint32_t sign = byte ? 0x80u : 0x80000000u;
    uint32_t mask = byte ? 0xffu : 0xffffffffu;

    a &= mask;
    b &= mask;
    r &= mask;
    vm->psw &= ~(FREYA_VM_N | FREYA_VM_Z | FREYA_VM_V | FREYA_VM_C);
    if (r == 0) vm->psw |= FREYA_VM_Z;
    if (r & sign) vm->psw |= FREYA_VM_N;
    if (r < a) vm->psw |= FREYA_VM_C;
    if (~(a ^ b) & (a ^ r) & sign) vm->psw |= FREYA_VM_V;
}

/* r = a - b.  C is set when the subtraction borrows. */
static void sub_flags(freya_vm_t *vm, uint32_t a, uint32_t b, uint32_t r, int byte)
{
    uint32_t sign = byte ? 0x80u : 0x80000000u;
    uint32_t mask = byte ? 0xffu : 0xffffffffu;

    a &= mask;
    b &= mask;
    r &= mask;
    vm->psw &= ~(FREYA_VM_N | FREYA_VM_Z | FREYA_VM_V | FREYA_VM_C);
    if (r == 0) vm->psw |= FREYA_VM_Z;
    if (r & sign) vm->psw |= FREYA_VM_N;
    if (a < b) vm->psw |= FREYA_VM_C;
    if ((a ^ b) & (a ^ r) & sign) vm->psw |= FREYA_VM_V;
}

static int branch_taken(uint32_t psw, unsigned op)
{
    int n = (psw & FREYA_VM_N) != 0;
    int z = (psw & FREYA_VM_Z) != 0;
    int v = (psw & FREYA_VM_V) != 0;
    int c = (psw & FREYA_VM_C) != 0;

    switch (op >> 8) {
    case 0x01: return 1;
    case 0x02: return !z;
    case 0x03: return z;
    case 0x04: return n == v;
    case 0x05: return n != v;
    case 0x06: return !z && n == v;
    case 0x07: return z || n != v;
    case 0x80: return !n;
    case 0x81: return n;
    case 0x82: return !c && !z;
    case 0x83: return c || z;
    case 0x84: return !v;
    case 0x85: return v;
    case 0x86: return !c;
    case 0x87: return c;
    default:   return -1;
    }
}

static int unary(freya_vm_t *vm, uint8_t *m, uint32_t size,
                 unsigned op, int byte)
{
    ea_t ea;
    uint32_t d, r, sign, mask, c;
    int rc;

    rc = fetch_ea(vm, m, size, op >> 3 & 7, op & 7, byte, 1, &ea);
    if (rc) return rc;
    rc = read_op(vm, m, size, &ea, byte, &d);
    if (rc) return rc;
    sign = byte ? 0x80u : 0x80000000u;
    mask = byte ? 0xffu : 0xffffffffu;
    d &= mask;
    c = vm->psw & FREYA_VM_C;
    r = d;

    switch ((op >> 6) & 077) {
    case 050: /* CLR */
        r = 0;
        logic_flags(vm, r, byte, 1, 0);
        break;
    case 051: /* COM */
        r = ~d;
        logic_flags(vm, r, byte, 1, 1);
        break;
    case 052: /* INC */
        r = d + 1u;
        vm->psw &= ~FREYA_VM_V;
        if (d == (sign - 1u)) vm->psw |= FREYA_VM_V;
        nz(vm, r, byte);
        break;
    case 053: /* DEC */
        r = d - 1u;
        vm->psw &= ~FREYA_VM_V;
        if (d == sign) vm->psw |= FREYA_VM_V;
        nz(vm, r, byte);
        break;
    case 054: /* NEG */
        r = (uint32_t)(0u - d);
        sub_flags(vm, 0, d, r, byte);
        break;
    case 055: /* ADC */
        r = d + c;
        add_flags(vm, d, c, r, byte);
        break;
    case 056: /* SBC */
        r = d - c;
        sub_flags(vm, d, c, r, byte);
        break;
    case 057: /* TST */
        logic_flags(vm, d, byte, 1, 0);
        return 0;
    case 060: /* ROR */
        r = (d >> 1) | (c ? sign : 0);
        logic_flags(vm, r, byte, 1, d & 1u);
        if (((vm->psw & FREYA_VM_N) != 0) != ((vm->psw & FREYA_VM_C) != 0))
            vm->psw |= FREYA_VM_V;
        break;
    case 061: /* ROL */
        r = (d << 1) | c;
        logic_flags(vm, r, byte, 1, (d & sign) != 0);
        if (((vm->psw & FREYA_VM_N) != 0) != ((vm->psw & FREYA_VM_C) != 0))
            vm->psw |= FREYA_VM_V;
        break;
    case 062: /* ASR */
        r = (d >> 1) | (d & sign);
        logic_flags(vm, r, byte, 1, d & 1u);
        if (((vm->psw & FREYA_VM_N) != 0) != ((vm->psw & FREYA_VM_C) != 0))
            vm->psw |= FREYA_VM_V;
        break;
    case 063: /* ASL */
        r = d << 1;
        logic_flags(vm, r, byte, 1, (d & sign) != 0);
        if (((vm->psw & FREYA_VM_N) != 0) != ((vm->psw & FREYA_VM_C) != 0))
            vm->psw |= FREYA_VM_V;
        break;
    default:
        return FREYA_VM_ILLEGAL;
    }
    return write_op(vm, m, size, &ea, byte, r & mask);
}

static int binary(freya_vm_t *vm, uint8_t *m, uint32_t size,
                  unsigned op, int byte, int kind)
{
    ea_t src, dst;
    uint32_t s, d, r;
    int rc;

    rc = fetch_ea(vm, m, size, (op >> 9) & 7, (op >> 6) & 7, byte, 0, &src);
    if (rc) return rc;
    rc = fetch_ea(vm, m, size, (op >> 3) & 7, op & 7, byte, 1, &dst);
    if (rc) return rc;
    rc = read_op(vm, m, size, &src, byte, &s);
    if (rc) return rc;
    if (kind == 0 || kind == 2) { /* MOV, BIT: destination is not read for MOV */
        if (kind == 2) {
            rc = read_op(vm, m, size, &dst, byte, &d);
            if (rc) return rc;
        } else {
            d = 0;
        }
    } else {
        rc = read_op(vm, m, size, &dst, byte, &d);
        if (rc) return rc;
    }

    switch (kind) {
    case 0: /* MOV */
        r = s;
        logic_flags(vm, r, byte, 0, 0);
        return write_op(vm, m, size, &dst, byte, r);
    case 1: /* CMP: flags from src - dst, nothing stored */
        r = s - d;
        sub_flags(vm, s, d, r, byte);
        return 0;
    case 2: /* BIT */
        r = s & d;
        logic_flags(vm, r, byte, 0, 0);
        return 0;
    case 3: /* BIC */
        r = d & ~s;
        logic_flags(vm, r, byte, 0, 0);
        return write_op(vm, m, size, &dst, byte, r);
    case 4: /* BIS */
        r = d | s;
        logic_flags(vm, r, byte, 0, 0);
        return write_op(vm, m, size, &dst, byte, r);
    case 5: /* ADD */
        r = d + s;
        add_flags(vm, d, s, r, 0);
        return write_op(vm, m, size, &dst, 0, r);
    case 6: /* SUB: dst - src */
        r = d - s;
        sub_flags(vm, d, s, r, 0);
        return write_op(vm, m, size, &dst, 0, r);
    default:
        return FREYA_VM_ILLEGAL;
    }
}

static int eis(freya_vm_t *vm, uint8_t *m, uint32_t size, unsigned op)
{
    unsigned reg = (op >> 6) & 7;
    ea_t src;
    uint32_t s;
    int rc, count, i, paired, vbit;
    int64_t prod, acc;
    uint64_t wide;
    uint32_t cout;

    if ((op & 0177000) == 0077000) { /* SOB */
        vm->r[reg] -= 1u;
        if (vm->r[reg] != 0)
            vm->r[FREYA_VM_PC] -= (op & 077) * 4u;
        return 0;
    }
    rc = fetch_ea(vm, m, size, (op >> 3) & 7, op & 7, 0, 0, &src);
    if (rc) return rc;
    rc = read_op(vm, m, size, &src, 0, &s);
    if (rc) return rc;

    if ((op & 0177000) == 0070000) { /* MUL, signed */
        prod = (int64_t)(int32_t)vm->r[reg] * (int64_t)(int32_t)s;
        vm->psw &= ~(FREYA_VM_N | FREYA_VM_Z | FREYA_VM_V | FREYA_VM_C);
        if (reg & 1u) {
            vm->r[reg] = (uint32_t)prod;
            nz(vm, vm->r[reg], 0);
        } else {
            vm->r[reg] = (uint32_t)((uint64_t)prod >> 32);
            vm->r[reg | 1u] = (uint32_t)prod;
            if (prod < 0) vm->psw |= FREYA_VM_N;
            if (prod == 0) vm->psw |= FREYA_VM_Z;
        }
        if (prod != (int64_t)(int32_t)prod) vm->psw |= FREYA_VM_V;
        return 0;
    }
    if ((op & 0177000) == 0071000) { /* DIV, signed, even register */
        int32_t dv = (int32_t)s;
        int neg = 0;
        uint32_t mag, bit;
        uint64_t ud, uq, remv;
        if (reg & 1u) return FREYA_VM_ILLEGAL;
        acc = ((int64_t)(uint64_t)vm->r[reg] << 32) | vm->r[reg | 1u];
        vm->psw &= ~(FREYA_VM_N | FREYA_VM_Z | FREYA_VM_V | FREYA_VM_C);
        if (dv == 0 || (dv == -1 &&
                        acc == (int64_t)((uint64_t)1 << 63))) {
            vm->psw |= FREYA_VM_V;
            if (dv == 0) vm->psw |= FREYA_VM_C;
            return 0;
        }
        /* The quotient has to fit in 32 bits.  Dividing by shifts keeps
         * this file off the 64-bit helper in libgcc, which the 48 KiB
         * image has no room for. */
        if (acc < 0) {
            ud = (uint64_t)(-acc);
            neg ^= 1;
        } else {
            ud = (uint64_t)acc;
        }
        mag = dv < 0 ? (uint32_t)(-dv) : (uint32_t)dv;
        if (dv < 0) neg ^= 1;
        uq = 0;
        remv = 0;
        for (bit = 0; bit < 64; bit++) {
            remv = (remv << 1) | ((ud >> (63 - bit)) & 1u);
            uq <<= 1;
            if (remv >= mag) {
                remv -= mag;
                uq |= 1;
            }
        }
        if (uq > 0x80000000ull || (uq == 0x80000000ull && !neg)) {
            vm->psw |= FREYA_VM_V;
            return 0;
        }
        if (neg) uq = (uint64_t)(-(int64_t)uq);
        vm->r[reg] = (uint32_t)uq;
        if (acc < 0) remv = (uint64_t)(-(int64_t)remv);
        vm->r[reg | 1u] = (uint32_t)remv;
        nz(vm, vm->r[reg], 0);
        return 0;
    }
    if ((op & 0177000) == 0074000) { /* XOR */
        uint32_t r = vm->r[reg] ^ s;
        /* XOR's operand is the destination, already read as s.  Write it back. */
        logic_flags(vm, r, 0, 0, 0);
        return write_op(vm, m, size, &src, 0, r);
    }
    if ((op & 0177000) != 0072000 && (op & 0177000) != 0073000)
        return FREYA_VM_ILLEGAL;

    /* ASH and ASHC.  The count is the low 6 bits of the source, signed. */
    count = (int)(s << 26) >> 26;
    paired = (op & 0177000) == 0073000;
    if (paired) {
        if (reg & 1u) return FREYA_VM_ILLEGAL;
        wide = ((uint64_t)vm->r[reg] << 32) | vm->r[reg | 1u];
    } else {
        wide = (uint64_t)(int64_t)(int32_t)vm->r[reg];
    }
    cout = 0;
    vbit = 0;
    if (count > 0) {
        for (i = 0; i < count; i++) {
            uint64_t next = wide << 1;
            uint32_t sign = paired ? 63u : 31u;
            cout = (uint32_t)(wide >> sign) & 1u;
            if (((wide ^ next) >> sign) & 1u) vbit = 1;
            wide = next;
        }
    } else if (count < 0) {
        for (i = 0; i < -count; i++) {
            uint64_t next = paired ? (uint64_t)((int64_t)wide >> 1)
                                   : (uint64_t)((int64_t)(int32_t)wide >> 1);
            cout = (uint32_t)wide & 1u;
            wide = next;
        }
    }
    vm->psw &= ~(FREYA_VM_N | FREYA_VM_Z | FREYA_VM_V | FREYA_VM_C);
    if (cout) vm->psw |= FREYA_VM_C;
    if (vbit) vm->psw |= FREYA_VM_V;
    if (paired) {
        vm->r[reg] = (uint32_t)(wide >> 32);
        vm->r[reg | 1u] = (uint32_t)wide;
        nz(vm, vm->r[reg], 0);
        if (vm->r[reg] == 0 && vm->r[reg | 1u] == 0) vm->psw |= FREYA_VM_Z;
        else vm->psw &= ~FREYA_VM_Z;
    } else {
        vm->r[reg] = (uint32_t)wide;
        nz(vm, vm->r[reg], 0);
    }
    return 0;
}

int vm_step(freya_vm_t *vm, void *mem, uint32_t size)
{
    uint8_t *m = mem;
    uint32_t word, pc;
    unsigned op;
    int taken, rc;
    ea_t ea;

    if (!vm || (!mem && size) || size < 4u) return FREYA_ERR_ARG;
    pc = vm->r[FREYA_VM_PC];
    if (rd32(m, size, pc, &word)) return FREYA_VM_FAULT;
    if (word >> 16) return FREYA_VM_ILLEGAL;
    op = word & 0xffffu;
    vm->r[FREYA_VM_PC] = pc + 4u;

    if (op == 0000000) return FREYA_VM_HALT;
    if (op == 0000001 || op == 0000005) return 0;          /* WAIT, RESET */
    if (op == 0000002 || op == 0000006) {                  /* RTI, RTT */
        rc = pop(vm, m, size, &vm->r[FREYA_VM_PC]);
        if (rc) return rc;
        rc = pop(vm, m, size, &vm->psw);
        if (rc) return rc;
        vm->psw &= FREYA_VM_N | FREYA_VM_Z | FREYA_VM_V | FREYA_VM_C;
        return 0;
    }
    if (op == 0000003 || op == 0000004) return FREYA_VM_TRAP; /* BPT, IOT */
    if ((op & 0177770) == 0000200) {                       /* RTS */
        unsigned reg = op & 7;
        vm->r[FREYA_VM_PC] = vm->r[reg];
        return pop(vm, m, size, &vm->r[reg]);
    }
    if ((op & 0177740) == 0000240) {                       /* condition codes */
        unsigned bits = op & 017;
        if (op & 020) vm->psw |= bits;
        else vm->psw &= ~bits;
        vm->psw &= FREYA_VM_N | FREYA_VM_Z | FREYA_VM_V | FREYA_VM_C;
        return 0;
    }
    taken = branch_taken(vm->psw, op);
    if (taken >= 0) {
        if (taken) {
            int8_t disp = (int8_t)(op & 0xff);
            vm->r[FREYA_VM_PC] += (int32_t)disp * 4;
        }
        return 0;
    }
    if ((op & 0177700) == 0000100) {                       /* JMP */
        rc = fetch_ea(vm, m, size, (op >> 3) & 7, op & 7, 2, 1, &ea);
        if (rc) return rc;
        vm->r[FREYA_VM_PC] = ea.addr;
        return 0;
    }
    if ((op & 0177700) == 0000300) {                       /* SWAB: swap halves */
        uint32_t d, r;
        rc = fetch_ea(vm, m, size, (op >> 3) & 7, op & 7, 0, 1, &ea);
        if (rc) return rc;
        rc = read_op(vm, m, size, &ea, 0, &d);
        if (rc) return rc;
        r = (d << 16) | (d >> 16);
        logic_flags(vm, r, 0, 1, 0);
        return write_op(vm, m, size, &ea, 0, r);
    }
    if ((op & 0177000) == 0004000) {                       /* JSR */
        unsigned reg = (op >> 6) & 7;
        rc = fetch_ea(vm, m, size, (op >> 3) & 7, op & 7, 2, 1, &ea);
        if (rc) return rc;
        rc = push(vm, m, size, vm->r[reg]);
        if (rc) return rc;
        vm->r[reg] = vm->r[FREYA_VM_PC];
        vm->r[FREYA_VM_PC] = ea.addr;
        return 0;
    }
    if ((op & 0177700) == 0005000 || ((op & 0177700) >= 0005100 &&
                                      (op & 0177700) <= 0006300) ||
        (op & 0177700) == 0006700) {
        if ((op & 0177700) == 0006700) {                   /* SXT */
            uint32_t r = (vm->psw & FREYA_VM_N) ? 0xffffffffu : 0;
            rc = fetch_ea(vm, m, size, (op >> 3) & 7, op & 7, 0, 1, &ea);
            if (rc) return rc;
            vm->psw &= ~FREYA_VM_V;
            nz(vm, r, 0);
            if (r) vm->psw |= FREYA_VM_N;
            return write_op(vm, m, size, &ea, 0, r);
        }
        return unary(vm, m, size, op, 0);
    }
    if ((op & 0177700) == 0105000 || ((op & 0177700) >= 0105100 &&
                                      (op & 0177700) <= 0106300))
        return unary(vm, m, size, op, 1);
    if ((op & 0170000) == 0010000) return binary(vm, m, size, op, 0, 0);
    if ((op & 0170000) == 0110000) return binary(vm, m, size, op, 1, 0);
    if ((op & 0170000) == 0020000) return binary(vm, m, size, op, 0, 1);
    if ((op & 0170000) == 0120000) return binary(vm, m, size, op, 1, 1);
    if ((op & 0170000) == 0030000) return binary(vm, m, size, op, 0, 2);
    if ((op & 0170000) == 0130000) return binary(vm, m, size, op, 1, 2);
    if ((op & 0170000) == 0040000) return binary(vm, m, size, op, 0, 3);
    if ((op & 0170000) == 0140000) return binary(vm, m, size, op, 1, 3);
    if ((op & 0170000) == 0050000) return binary(vm, m, size, op, 0, 4);
    if ((op & 0170000) == 0150000) return binary(vm, m, size, op, 1, 4);
    if ((op & 0170000) == 0060000) return binary(vm, m, size, op, 0, 5);
    if ((op & 0170000) == 0160000) return binary(vm, m, size, op, 0, 6);
    if ((op & 0170000) == 0070000) return eis(vm, m, size, op);
    if ((op & 0177400) == 0104000 || (op & 0177400) == 0104400)
        return FREYA_VM_TRAP;
    vm->r[FREYA_VM_PC] = pc;
    return FREYA_VM_ILLEGAL;
}

int vm_run(freya_vm_t *vm, void *mem, uint32_t size, uint32_t steps, uint32_t *ran)
{
    uint32_t n = 0;
    int rc = 0;
    int capped = 0;

    if (!vm || (!mem && size)) return FREYA_ERR_ARG;
    if (steps == 0) {
        steps = FREYA_VM_MAX_STEPS;
        capped = 1;
    }
    while (n < steps) {
        rc = vm_step(vm, mem, size);
        if (rc) break;
        n++;
    }
    if (ran) *ran = n;
    if (rc) return rc;
    if (capped && n == steps) return FREYA_VM_LIMIT;
    return 0;
}
