# Virtual machine

A program can run a PDP-11 whose registers and words are 32 bits. There
are eight registers. R6 is the stack pointer and R7 is the program
counter. The condition codes N, Z, V and C sit in the same bits of the
processor status as they do on a PDP-11. The opcodes in the low 16 bits
of each instruction are the PDP-11 opcodes. A word is 32 bits, so the
stack, the program counter and an index word step by 4. A byte is still
8 bits. `samples/vm` adds two numbers on it.

## Calls

```c
int (*vm_reset)(freya_vm_t *vm);
int (*vm_step)(freya_vm_t *vm, void *mem, uint32_t size);
int (*vm_run)(freya_vm_t *vm, void *mem, uint32_t size,
              uint32_t steps, uint32_t *ran);
```

`freya_vm_t` holds `r[8]` and `psw`. `mem` is the whole address space,
little-endian, and an address is an offset into it. `vm_reset()` zeroes
the registers and sets Z. `vm_step()` executes the instruction at R7.
`vm_run()` executes up to `steps` instructions. A `steps` of 0 means
`FREYA_VM_MAX_STEPS`. When `ran` is not null it receives how many
instructions completed.

```c
freya_vm_t vm;
uint8_t mem[32];
uint32_t ran;

api->vm_reset(&vm);
api->vm_run(&vm, mem, sizeof mem, 0, &ran);
```

These calls were appended to the service table. A program built against
this header and handed an older kernel checks before it calls:

```c
if (!FREYA_API_HAS(api, vm_run)) {
    api->puts("this kernel has no virtual machine\r\n");
    return FREYA_EXIT_FAIL;
}
```

## What a call returns

| Value | Meaning |
|---|---|
| 0 | the instruction completed, or every requested step did |
| `FREYA_VM_HALT` | the instruction was HALT |
| `FREYA_VM_TRAP` | EMT, TRAP, BPT or IOT |
| `FREYA_VM_FAULT` | the address is outside `mem`, or a word is not on a 4-byte boundary |
| `FREYA_VM_ILLEGAL` | the opcode is not one this machine executes, or the high 16 bits of the instruction are not zero |
| `FREYA_VM_LIMIT` | a run with `steps` 0 used `FREYA_VM_MAX_STEPS` without halting |
| `FREYA_ERR_ARG` | `vm` is null, or `mem` is null while `size` is not |

HALT, a trap and an illegal opcode leave R7 on the next instruction,
except an illegal opcode whose high half is not zero: R7 stays on that
word. A fault leaves the registers as far as the instruction got,
including an autoincrement that already happened.

## Instructions

Double operand: MOV, MOVB, CMP, CMPB, BIT, BITB, BIC, BICB, BIS, BISB,
ADD, SUB. Single operand: CLR, CLRB, COM, COMB, INC, INCB, DEC, DECB,
NEG, NEGB, ADC, ADCB, SBC, SBCB, TST, TSTB, ROR, RORB, ROL, ROLB, ASR,
ASRB, ASL, ASLB, SWAB, SXT. Branches: BR, BNE, BEQ, BGE, BLT, BGT, BLE,
BPL, BMI, BHI, BLOS, BVC, BVS, BCC, BCS. Also JMP, JSR, RTS, SOB, MUL,
DIV, ASH, ASHC, XOR, the condition-code operators, RTI, RTT, WAIT and
RESET. WAIT and RESET do nothing. MARK, MFPI and MTPI are illegal.

SWAB exchanges the two 16-bit halves of a 32-bit word. MUL multiplies
two signed 32-bit values. An even register receives the high half and
the following register the low half. DIV divides the signed 64-bit
value in an even register pair. ASH and ASHC take the shift count from
the low 6 bits of the source, as a PDP-11 does. A branch offset is
still an 8-bit signed word count, and a word is 4 bytes. SOB's offset
is a 6-bit word count back.

Addressing is the PDP-11's eight modes. Mode 0 is the register. Modes
2 and 4 step R0–R5 by 1 for a byte and by 4 for a word. R6 and R7
always step by 4. An immediate is mode 2 on R7, and the immediate is
the following 32-bit word.

## The sample

```
freya: run vm.bin
vm: R0=42  steps=2  status=1
```

`status` 1 is `FREYA_VM_HALT`. The two completed instructions are the
MOV and the ADD; HALT is why the run stopped.
