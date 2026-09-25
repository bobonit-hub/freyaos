/*
 * Freya - print the program regions the ABI header declares.
 *
 * A linker script cannot include freya_api.h, so the addresses of the
 * program regions are written down twice: once in the linker scripts under
 * boards/<board> and once in the header.  Nothing in the build cross-checks
 * them.
 *
 * This prints what the header says, so that tests/run_tests.sh can compare
 * it against what the linker actually did.  The kernel makes the same
 * comparison at run time, in flash_begin(), but a build time failure is
 * the one worth having.
 */
#include <stdio.h>

#include "freya_api.h"

int main(void)
{
    printf("app_load_addr %lu\n",   (unsigned long)FREYA_APP_LOAD_ADDR);
    printf("app_region_size %lu\n", (unsigned long)FREYA_APP_REGION_SIZE);
    printf("abi_version %lu\n",     (unsigned long)FREYA_ABI_VERSION);
    printf("hdr_v1_size %lu\n",     (unsigned long)FREYA_APP_HDR_V1_SIZE);
    printf("hdr_v2_size %lu\n",     (unsigned long)FREYA_APP_HDR_V2_SIZE);
    printf("hdr_size %lu\n",        (unsigned long)sizeof(freya_app_header_t));
#ifdef FREYA_APP_FLASH_ADDR
    printf("app_flash_addr %lu\n",  (unsigned long)FREYA_APP_FLASH_ADDR);
    printf("app_flash_size %lu\n",  (unsigned long)FREYA_APP_FLASH_SIZE);
    printf("autostart_addr %lu\n",  (unsigned long)FREYA_AUTOSTART_ADDR);
    printf("autostart_size %lu\n",  (unsigned long)FREYA_AUTOSTART_SIZE);
    printf("ramdump_off %lu\n",     (unsigned long)FREYA_RAMDUMP_OFF);
    printf("cksum_off %lu\n",       (unsigned long)FREYA_CKSUM_OFF);
#endif
    return 0;
}
