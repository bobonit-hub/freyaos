/*
 * Freya - PDP-11 opcodes on 32-bit registers.
 *
 * src/vm.c compiled unchanged.  Each program is a list of 32-bit words
 * whose low 16 bits are the PDP-11 opcode.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freya.h"

static int checks, fails;

static void check(const char *what, long expected, long got)
{
    checks++;
    if (expected == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %ld, got %ld\n", what, expected, got);
        fails++;
    }
}

/* Store one instruction word.  The high half stays zero. */
static void w(uint8_t *m, uint32_t addr, uint32_t op)
{
    m[addr] = (uint8_t)op;
    m[addr + 1] = (uint8_t)(op >> 8);
    m[addr + 2] = (uint8_t)(op >> 16);
    m[addr + 3] = (uint8_t)(op >> 24);
}

static void test_add(void)
{
    uint8_t mem[32];
    freya_vm_t vm;
    uint32_t ran = 0;
    int rc;

    memset(mem, 0, sizeof mem);
    /* MOV #2, R0; ADD #3, R0; HALT */
    w(mem, 0, 012700);
    w(mem, 4, 2);
    w(mem, 8, 062700);
    w(mem, 12, 3);
    w(mem, 16, 0);
    vm_reset(&vm);
    rc = vm_run(&vm, mem, sizeof mem, 0, &ran);
    check("add halts", FREYA_VM_HALT, rc);
    check("add ran the mov and the add", 2, (long)ran);
    check("2 + 3", 5, (long)vm.r[0]);
    check("Z clear after add", 0, (long)(vm.psw & FREYA_VM_Z));
}

static void test_sob(void)
{
    uint8_t mem[32];
    freya_vm_t vm;
    uint32_t ran = 0;

    memset(mem, 0, sizeof mem);
    /* MOV #4, R1; CLR R0; INC R0; SOB R1, INC; HALT */
    w(mem, 0, 012701);
    w(mem, 4, 4);
    w(mem, 8, 005000);
    w(mem, 12, 005200);
    w(mem, 16, 077102);         /* SOB R1, two words back to INC */
    w(mem, 20, 0);
    vm_reset(&vm);
    vm_run(&vm, mem, sizeof mem, 0, &ran);
    check("SOB counted to 4", 4, (long)vm.r[0]);
    check("SOB left R1 at 0", 0, (long)vm.r[1]);
}

static void test_flags(void)
{
    uint8_t mem[32];
    freya_vm_t vm;

    memset(mem, 0, sizeof mem);
    /* MOV #0xFFFFFFFF, R0; ADD #1, R0; HALT  -> 0, C and Z set */
    w(mem, 0, 012700);
    w(mem, 4, 0xffffffffu);
    w(mem, 8, 062700);
    w(mem, 12, 1);
    w(mem, 16, 0);
    vm_reset(&vm);
    vm_run(&vm, mem, sizeof mem, 8, NULL);
    check("carry wraps to 0", 0, (long)vm.r[0]);
    check("carry sets C", FREYA_VM_C, (long)(vm.psw & FREYA_VM_C));
    check("carry sets Z", FREYA_VM_Z, (long)(vm.psw & FREYA_VM_Z));
}

static void test_branch(void)
{
    uint8_t mem[48];
    freya_vm_t vm;

    memset(mem, 0, sizeof mem);
    w(mem, 0, 012701);
    w(mem, 4, 1);
    w(mem, 8, 012702);
    w(mem, 12, 2);
    w(mem, 16, 020102);         /* CMP R1, R2: 1 - 2, N set */
    w(mem, 20, 0x0502);         /* BLT +2, past MOV and its immediate */
    w(mem, 24, 012700);         /* MOV #9, R0 */
    w(mem, 28, 9);
    w(mem, 32, 012700);         /* MOV #7, R0 */
    w(mem, 36, 7);
    w(mem, 40, 0);
    vm_reset(&vm);
    vm_run(&vm, mem, sizeof mem, 16, NULL);
    check("BLT taken", 7, (long)vm.r[0]);
    check("MOV clears N", 0, (long)(vm.psw & FREYA_VM_N));
}

static void test_byte_and_stack(void)
{
    uint8_t mem[64];
    freya_vm_t vm;

    memset(mem, 0, sizeof mem);
    /* MOVB into a register sign-extends, as on a PDP-11, so 0xAB
     * arrives as 0xFFFFFFAB.  JSR R5, next; RTS R5; HALT. */
    w(mem, 0, 012706);          /* MOV #48, SP */
    w(mem, 4, 48);
    w(mem, 8, 0112700);         /* MOVB #0xAB, R0  (mode 2, reg 7, dst R0) */
    w(mem, 12, 0xab);
    w(mem, 16, 004537);         /* JSR R5, @(PC)+ */
    w(mem, 20, 28);             /* address of the RTS */
    w(mem, 24, 0);              /* HALT, where RTS returns */
    w(mem, 28, 000205);         /* RTS R5 */
    vm_reset(&vm);
    vm_run(&vm, mem, sizeof mem, 16, NULL);
    check("byte move sign-extends", (long)0xffffffabu, (long)vm.r[0]);
    check("JSR returned", 28, (long)vm.r[FREYA_VM_PC]);
    check("stack restored", 48, (long)vm.r[FREYA_VM_SP]);
}

/*
 * MOVB is the one byte instruction that touches the whole destination
 * register: the byte is sign-extended through it.  Every other byte
 * instruction leaves the rest of the register, and a byte written to
 * memory is one byte wherever it came from.
 */
static void test_movb_register(void)
{
    uint8_t mem[32];
    freya_vm_t vm;

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0112700);         /* MOVB #0x7F, R0 */
    w(mem, 4, 0x7f);
    vm_reset(&vm);
    vm.r[0] = 0x12345678u;
    vm_step(&vm, mem, sizeof mem);
    check("MOVB of a positive byte clears the rest", 0x7f, (long)vm.r[0]);

    w(mem, 4, 0x80);
    vm_reset(&vm);
    vm.r[0] = 0x12345678u;
    vm_step(&vm, mem, sizeof mem);
    check("MOVB of a negative byte fills the rest",
          (long)0xffffff80u, (long)vm.r[0]);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0110100);         /* MOVB R1, R0 */
    vm_reset(&vm);
    vm.r[0] = 0x11111111u;
    vm.r[1] = 0x000000ffu;
    vm_step(&vm, mem, sizeof mem);
    check("MOVB register to register sign-extends",
          (long)0xffffffffu, (long)vm.r[0]);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0105000);         /* CLRB R0 */
    vm_reset(&vm);
    vm.r[0] = 0x12345678u;
    vm_step(&vm, mem, sizeof mem);
    check("CLRB leaves the rest of the register", 0x12345600, (long)vm.r[0]);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0105200);         /* INCB R0 */
    vm_reset(&vm);
    vm.r[0] = 0x123456ffu;
    vm_step(&vm, mem, sizeof mem);
    check("INCB wraps inside the byte", 0x12345600, (long)vm.r[0]);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0112711);         /* MOVB #0xAB, (R1) */
    w(mem, 4, 0xab);
    mem[12] = 0x11;
    mem[13] = 0x22;
    vm_reset(&vm);
    vm.r[1] = 12;
    vm_step(&vm, mem, sizeof mem);
    check("MOVB to memory writes one byte", 0xab, (long)mem[12]);
    check("MOVB to memory leaves the next", 0x22, (long)mem[13]);
}

/*
 * The eight addressing modes, and what each leaves in the register it
 * stepped.  Modes 2 and 4 step R0-R5 by 1 for a byte and by 4 for a
 * word; R6 and R7 always step by 4.
 */
static void test_modes(void)
{
    static const struct {
        const char *name;
        uint32_t    op;
        uint32_t    index;
        int         words;
        uint32_t    r1;
        uint32_t    want_r0;
        uint32_t    want_r1;
    } t[] = {
        { "mode 0  MOV R1, R0",       0010100, 0, 1, 0x1234, 0x1234,     0x1234 },
        { "mode 1  MOV (R1), R0",     0011100, 0, 1, 64,     0x11111111, 64 },
        { "mode 2  MOV (R1)+, R0",    0012100, 0, 1, 64,     0x11111111, 68 },
        { "mode 2  MOVB (R1)+, R0",   0112100, 0, 1, 64,     0x11,       65 },
        { "mode 2  MOVB a high byte", 0112100, 0, 1, 80,     0xffffff80, 81 },
        { "mode 3  MOV @(R1)+, R0",   0013100, 0, 1, 72,     0x11111111, 76 },
        { "mode 4  MOV -(R1), R0",    0014100, 0, 1, 68,     0x11111111, 64 },
        { "mode 4  MOVB -(R1), R0",   0114100, 0, 1, 69,     0x22,       68 },
        { "mode 5  MOV @-(R1), R0",   0015100, 0, 1, 76,     0x11111111, 72 },
        { "mode 6  MOV 8(R1), R0",    0016100, 8, 2, 56,     0x11111111, 56 },
        { "mode 7  MOV @8(R1), R0",   0017100, 8, 2, 64,     0x11111111, 64 },
    };
    uint8_t mem[128];
    freya_vm_t vm;
    char label[80];
    size_t i;

    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        memset(mem, 0, sizeof mem);
        w(mem, 0, t[i].op);
        if (t[i].words == 2) w(mem, 4, t[i].index);
        w(mem, 64, 0x11111111);
        w(mem, 68, 0x22222222);
        w(mem, 72, 64);
        w(mem, 76, 68);
        mem[80] = 0x80;
        vm_reset(&vm);
        vm.r[1] = t[i].r1;
        check(t[i].name, 0, vm_step(&vm, mem, sizeof mem));
        snprintf(label, sizeof label, "%s -> R0", t[i].name);
        check(label, (long)t[i].want_r0, (long)vm.r[0]);
        snprintf(label, sizeof label, "%s -> R1", t[i].name);
        check(label, (long)t[i].want_r1, (long)vm.r[1]);
        snprintf(label, sizeof label, "%s -> PC", t[i].name);
        check(label, (long)(4 * t[i].words), (long)vm.r[FREYA_VM_PC]);
    }

    /* A byte operand does not shorten the stack pointer's step. */
    memset(mem, 0, sizeof mem);
    w(mem, 0, 0112600);         /* MOVB (SP)+, R0 */
    w(mem, 64, 0x11111111);
    vm_reset(&vm);
    vm.r[FREYA_VM_SP] = 64;
    vm_step(&vm, mem, sizeof mem);
    check("MOVB (SP)+ steps the stack by a word", 68,
          (long)vm.r[FREYA_VM_SP]);
}

/*
 * JMP and JSR take an address, not a word or a byte, so an
 * autoincrement of theirs steps by 4 whichever register it is on.  A
 * register has no address, so mode 0 is illegal for both.
 */
static void test_jump_modes(void)
{
    uint8_t mem[128];
    freya_vm_t vm;
    int rc;

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0000121);         /* JMP (R1)+ */
    vm_reset(&vm);
    vm.r[1] = 64;
    check("JMP (R1)+", 0, vm_step(&vm, mem, sizeof mem));
    check("JMP (R1)+ jumped", 64, (long)vm.r[FREYA_VM_PC]);
    check("JMP (R1)+ stepped by a word", 68, (long)vm.r[1]);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0004541);         /* JSR R5, -(R1) */
    vm_reset(&vm);
    vm.r[1] = 68;
    vm.r[FREYA_VM_SP] = 128;
    check("JSR R5, -(R1)", 0, vm_step(&vm, mem, sizeof mem));
    check("JSR -(R1) jumped", 64, (long)vm.r[FREYA_VM_PC]);
    check("JSR -(R1) stepped by a word", 64, (long)vm.r[1]);
    check("JSR saved the return address", 4, (long)vm.r[5]);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0000101);         /* JMP R1: a register has no address */
    vm_reset(&vm);
    rc = vm_step(&vm, mem, sizeof mem);
    check("JMP on a register is illegal", FREYA_VM_ILLEGAL, rc);
    check("JMP on a register leaves the PC", 0, (long)vm.r[FREYA_VM_PC]);
}

/* An opcode this machine does not have leaves R7 on it, every time. */
static void test_illegal_pc(void)
{
    static const struct {
        const char *name;
        uint32_t    op;
    } t[] = {
        { "MARK",                  0006400 },
        { "MFPI",                  0006500 },
        { "MTPI",                  0006600 },
        { "SPL",                   0000230 },
        { "DIV on an odd register", 0071102 },
        { "ASHC on an odd register", 0073102 },
        { "the high half set",     0x00010000u },
    };
    uint8_t mem[32];
    freya_vm_t vm;
    char label[80];
    size_t i;

    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        memset(mem, 0, sizeof mem);
        w(mem, 0, t[i].op);
        vm_reset(&vm);
        snprintf(label, sizeof label, "%s is illegal", t[i].name);
        check(label, FREYA_VM_ILLEGAL, vm_step(&vm, mem, sizeof mem));
        snprintf(label, sizeof label, "%s leaves the PC on it", t[i].name);
        check(label, 0, (long)vm.r[FREYA_VM_PC]);
    }
}

/* A dividend whose high word has its top bit set, and the two ways the
 * quotient can fail to exist. */
static void test_div(void)
{
    uint8_t mem[32];
    freya_vm_t vm;

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0071002);         /* DIV R2, R0 */
    vm_reset(&vm);
    vm.r[0] = 0xffffffffu;      /* -42 as a 64-bit dividend */
    vm.r[1] = 0xffffffd6u;
    vm.r[2] = 7;
    check("DIV of a negative dividend", 0, vm_step(&vm, mem, sizeof mem));
    check("negative quotient", (long)0xfffffffau, (long)vm.r[0]);
    check("negative remainder", 0, (long)vm.r[1]);
    check("negative quotient sets N", FREYA_VM_N, (long)(vm.psw & FREYA_VM_N));

    vm_reset(&vm);
    vm.r[0] = 0;
    vm.r[1] = 42;
    vm.r[2] = 0;
    vm_step(&vm, mem, sizeof mem);
    check("DIV by zero sets V", FREYA_VM_V, (long)(vm.psw & FREYA_VM_V));
    check("DIV by zero sets C", FREYA_VM_C, (long)(vm.psw & FREYA_VM_C));

    vm_reset(&vm);
    vm.r[0] = 1;                /* 2^32 / 1 does not fit in 32 bits */
    vm.r[1] = 0;
    vm.r[2] = 1;
    vm_step(&vm, mem, sizeof mem);
    check("a quotient too large sets V", FREYA_VM_V, (long)(vm.psw & FREYA_VM_V));
    check("a quotient too large keeps the dividend", 1, (long)vm.r[0]);
}

/* The instructions with nowhere else to be checked. */
static void test_odds_and_ends(void)
{
    uint8_t mem[64];
    freya_vm_t vm;

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0074001);         /* XOR R0, R1 */
    vm_reset(&vm);
    vm.r[0] = 0x00000f0fu;
    vm.r[1] = 0x000000ffu;
    vm_step(&vm, mem, sizeof mem);
    check("XOR", 0x0ff0, (long)vm.r[1]);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0000300);         /* SWAB R0 */
    vm_reset(&vm);
    vm.r[0] = 0x12345678u;
    vm_step(&vm, mem, sizeof mem);
    check("SWAB swaps the halves", 0x56781234, (long)vm.r[0]);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0006700);         /* SXT R0 */
    vm_reset(&vm);
    vm.psw |= FREYA_VM_N;
    vm_step(&vm, mem, sizeof mem);
    check("SXT with N set", (long)0xffffffffu, (long)vm.r[0]);
    vm_reset(&vm);
    vm.r[0] = 0x1234u;
    vm_step(&vm, mem, sizeof mem);
    check("SXT with N clear", 0, (long)vm.r[0]);
    check("SXT with N clear sets Z", FREYA_VM_Z, (long)(vm.psw & FREYA_VM_Z));

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0000261);         /* SEC */
    w(mem, 4, 0000257);         /* CCC */
    vm_reset(&vm);
    vm_step(&vm, mem, sizeof mem);
    check("SEC sets C", FREYA_VM_C, (long)(vm.psw & FREYA_VM_C));
    vm_step(&vm, mem, sizeof mem);
    check("CCC clears them all", 0, (long)vm.psw);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0000002);         /* RTI */
    w(mem, 32, 20);             /* the stacked PC */
    w(mem, 36, FREYA_VM_C | ~0xfu); /* the stacked PSW, with rubbish above */
    vm_reset(&vm);
    vm.r[FREYA_VM_SP] = 32;
    check("RTI", 0, vm_step(&vm, mem, sizeof mem));
    check("RTI took the PC off the stack", 20, (long)vm.r[FREYA_VM_PC]);
    check("RTI took the status off the stack", FREYA_VM_C, (long)vm.psw);
    check("RTI popped both words", 40, (long)vm.r[FREYA_VM_SP]);
}

/* V and C are the flags a shift or a subtraction gets quietly wrong. */
static void test_shift_and_sub_flags(void)
{
    uint8_t mem[32];
    freya_vm_t vm;

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0006300);         /* ASL R0 */
    vm_reset(&vm);
    vm.r[0] = 0x40000000u;
    vm_step(&vm, mem, sizeof mem);
    check("ASL into the sign bit", (long)0x80000000u, (long)vm.r[0]);
    check("ASL sets N", FREYA_VM_N, (long)(vm.psw & FREYA_VM_N));
    check("ASL clears C", 0, (long)(vm.psw & FREYA_VM_C));
    check("ASL sets V when N and C differ", FREYA_VM_V,
          (long)(vm.psw & FREYA_VM_V));

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0006200);         /* ASR R0 */
    vm_reset(&vm);
    vm.r[0] = 1;
    vm_step(&vm, mem, sizeof mem);
    check("ASR to zero", 0, (long)vm.r[0]);
    check("ASR sets C from the bit shifted out", FREYA_VM_C,
          (long)(vm.psw & FREYA_VM_C));
    check("ASR sets Z", FREYA_VM_Z, (long)(vm.psw & FREYA_VM_Z));
    check("ASR sets V when N and C differ", FREYA_VM_V,
          (long)(vm.psw & FREYA_VM_V));

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0020001);         /* CMP R0, R1: R0 - R1 */
    vm_reset(&vm);
    vm.r[0] = 1;
    vm.r[1] = 2;
    vm_step(&vm, mem, sizeof mem);
    check("CMP leaves the destination", 2, (long)vm.r[1]);
    check("CMP sets N", FREYA_VM_N, (long)(vm.psw & FREYA_VM_N));
    check("CMP borrows into C", FREYA_VM_C, (long)(vm.psw & FREYA_VM_C));
    check("CMP clears V", 0, (long)(vm.psw & FREYA_VM_V));

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0160100);         /* SUB R1, R0: R0 - R1 */
    vm_reset(&vm);
    vm.r[0] = 0x80000000u;
    vm.r[1] = 1;
    vm_step(&vm, mem, sizeof mem);
    check("SUB", 0x7fffffff, (long)vm.r[0]);
    check("SUB sets V on signed overflow", FREYA_VM_V,
          (long)(vm.psw & FREYA_VM_V));
    check("SUB clears C without a borrow", 0, (long)(vm.psw & FREYA_VM_C));
}

static void test_mul_ash(void)
{
    uint8_t mem[32];
    freya_vm_t vm;

    memset(mem, 0, sizeof mem);
    w(mem, 0, 012700);          /* MOV #6, R0 */
    w(mem, 4, 6);
    w(mem, 8, 012701);          /* MOV #7, R1 */
    w(mem, 12, 7);
    w(mem, 16, 0070001);        /* MUL R1, R0  -> R0:R1 = 42 */
    w(mem, 20, 0);
    vm_reset(&vm);
    vm_run(&vm, mem, sizeof mem, 8, NULL);
    check("MUL high", 0, (long)vm.r[0]);
    check("MUL low", 42, (long)vm.r[1]);
    check("MUL fits, V clear", 0, (long)(vm.psw & FREYA_VM_V));

    memset(mem, 0, sizeof mem);
    w(mem, 0, 012700);
    w(mem, 4, 1);
    w(mem, 8, 012701);
    w(mem, 12, 4);              /* shift count */
    w(mem, 16, 0072001);        /* ASH R1, R0 */
    w(mem, 20, 0);
    vm_reset(&vm);
    vm_run(&vm, mem, sizeof mem, 8, NULL);
    check("ASH left 4", 16, (long)vm.r[0]);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 012701);          /* MOV #42, R1  (low half of the dividend) */
    w(mem, 4, 42);
    w(mem, 8, 005000);          /* CLR R0       (high half) */
    w(mem, 12, 012702);         /* MOV #7, R2 */
    w(mem, 16, 7);
    w(mem, 20, 0071002);        /* DIV R2, R0 */
    w(mem, 24, 0);
    vm_reset(&vm);
    vm_run(&vm, mem, sizeof mem, 8, NULL);
    check("DIV quotient", 6, (long)vm.r[0]);
    check("DIV remainder", 0, (long)vm.r[1]);
}

static void test_faults(void)
{
    uint8_t mem[16];
    freya_vm_t vm;
    int rc;

    memset(mem, 0, sizeof mem);
    w(mem, 0, 0x00010000u);     /* high half set: not a PDP-11 opcode */
    vm_reset(&vm);
    rc = vm_step(&vm, mem, sizeof mem);
    check("reserved opcode", FREYA_VM_ILLEGAL, rc);
    check("PC stays on it", 0, (long)vm.r[FREYA_VM_PC]);

    vm_reset(&vm);
    vm.r[FREYA_VM_PC] = 2;
    rc = vm_step(&vm, mem, sizeof mem);
    check("odd PC faults", FREYA_VM_FAULT, rc);

    rc = vm_step(NULL, mem, sizeof mem);
    check("null machine", FREYA_ERR_ARG, rc);

    memset(mem, 0, sizeof mem);
    w(mem, 0, 011011);          /* MOV (R1), R0 with R1 = 0: reads address 0, ok */
    vm_reset(&vm);
    rc = vm_step(&vm, mem, sizeof mem);
    check("MOV from address 0", 0, rc);
}

static void test_step_limit(void)
{
    uint8_t mem[8];
    freya_vm_t vm;
    uint32_t ran = 99;
    int rc;

    memset(mem, 0, sizeof mem);
    w(mem, 0, 000400 | 0377);   /* BR -1, back to itself */
    vm_reset(&vm);
    rc = vm_run(&vm, mem, sizeof mem, 5, &ran);
    check("bounded run returns 0", 0, rc);
    check("bounded run counted 5", 5, (long)ran);
}

int main(void)
{
    test_add();
    test_sob();
    test_flags();
    test_branch();
    test_byte_and_stack();
    test_movb_register();
    test_modes();
    test_jump_modes();
    test_mul_ash();
    test_div();
    test_odds_and_ends();
    test_shift_and_sub_flags();
    test_faults();
    test_illegal_pc();
    test_step_limit();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
