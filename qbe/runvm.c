/* Run a Freya VM image. The image is loaded at address 0. A halt
 * instruction sits above the stack, and its address is the return
 * address of the entry function. The value left in R0 is printed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freya.h"

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    uint8_t *mem;
    uint32_t size, memsz, halt, sp, entry;
    freya_vm_t vm;
    uint32_t ran = 0;
    int rc;

    if (argc != 3) {
        fprintf(stderr, "usage: runvm image.bin entry\n");
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    entry = (uint32_t)strtoul(argv[2], 0, 0);
    size = (uint32_t)n;
    memsz = size + 65536;
    mem = calloc(1, memsz);
    if (fread(mem, 1, size, f) != size) {
        perror("read");
        return 1;
    }
    fclose(f);
    halt = memsz - 4;
    mem[halt] = 0;
    sp = halt - 4;
    mem[sp] = (uint8_t)halt;
    mem[sp + 1] = (uint8_t)(halt >> 8);
    mem[sp + 2] = (uint8_t)(halt >> 16);
    mem[sp + 3] = (uint8_t)(halt >> 24);
    vm_reset(&vm);
    vm.r[FREYA_VM_SP] = sp;
    vm.r[FREYA_VM_PC] = entry;
    rc = vm_run(&vm, mem, memsz, 0, &ran);
    printf("r0=%u r1=%u steps=%u status=%d\n", vm.r[0], vm.r[1], ran, rc);
    free(mem);
    return rc == FREYA_VM_HALT ? 0 : 1;
}
