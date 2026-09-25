/*
 * vm - run a few PDP-11 instructions on the 32-bit machine.
 *
 *     run vm.bin
 *
 * The program is MOV #20, R0 / ADD #22, R0 / HALT.  R0 comes back 42.
 */
#include "freya_api.h"

static void store(uint8_t *m, uint32_t addr, uint32_t op)
{
    m[addr]     = (uint8_t)op;
    m[addr + 1] = (uint8_t)(op >> 8);
    m[addr + 2] = (uint8_t)(op >> 16);
    m[addr + 3] = (uint8_t)(op >> 24);
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    uint8_t mem[32];
    freya_vm_t vm;
    uint32_t ran = 0;
    int rc, i;

    (void)argc;
    (void)argv;
    if (!FREYA_API_HAS(api, vm_run)) {
        api->puts("vm: this kernel has no virtual machine\r\n");
        return FREYA_EXIT_FAIL;
    }
    for (i = 0; i < (int)sizeof mem; i++) mem[i] = 0;
    store(mem, 0, 012700);          /* MOV #20, R0 */
    store(mem, 4, 20);
    store(mem, 8, 062700);          /* ADD #22, R0 */
    store(mem, 12, 22);
    store(mem, 16, 0);              /* HALT */

    rc = api->vm_reset(&vm);
    if (rc != 0) {
        api->printf("vm: reset %d\r\n", rc);
        return FREYA_EXIT_FAIL;
    }
    rc = api->vm_run(&vm, mem, sizeof mem, 0, &ran);
    api->printf("vm: R0=%u  steps=%u  status=%d\r\n",
                vm.r[0], ran, rc);
    if (rc != FREYA_VM_HALT || vm.r[0] != 42) return FREYA_EXIT_FAIL;
    return FREYA_EXIT_OK;
}
