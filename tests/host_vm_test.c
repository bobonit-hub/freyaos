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
    /* MOVB #0xAB, R0 leaves the low byte and clears nothing above it
     * when the register was 0.  JSR R5, next; RTS R5; HALT. */
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
    check("byte move", 0xab, (long)vm.r[0]);
    check("JSR returned", 28, (long)vm.r[FREYA_VM_PC]);
    check("stack restored", 48, (long)vm.r[FREYA_VM_SP]);
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
    test_mul_ash();
    test_faults();
    test_step_limit();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
