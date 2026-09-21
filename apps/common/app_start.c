/*
 * Freya - program header shared by every user program.
 *
 * Link this file together with the program's main.c; the linker script
 * places the header at the very start of the image so that the kernel
 * loader finds it at offset 0 of the file.
 */
#include "freya_api.h"

#ifndef APP_NAME
#define APP_NAME "app"
#endif

int app_main(const freya_api_t *api, int argc, char **argv);

extern char __bss_start__[];
extern char __bss_end__[];
extern char __image_size__[];       /* absolute symbol: image byte count */

__attribute__((section(".app_header"), used))
const freya_app_header_t freya_header = {
    .magic       = FREYA_APP_MAGIC,
    .abi_version = FREYA_ABI_VERSION,
    .load_addr   = FREYA_APP_LOAD_ADDR,
    .entry       = (uint32_t)(uintptr_t)&app_main,
    .image_size  = (uint32_t)(uintptr_t)__image_size__,
    .bss_start   = (uint32_t)(uintptr_t)__bss_start__,
    .bss_end     = (uint32_t)(uintptr_t)__bss_end__,
    .stack_need  = 2048,
    .name        = APP_NAME,
};
