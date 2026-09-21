/*
 * Freya - program header shared by every user program.
 *
 * Link this file together with the program's main.c; the linker script
 * places the header at the very start of the image so that the kernel
 * loader finds it at offset 0 of the file.
 *
 * The same source builds either kind of program.  Linked with app.ld the
 * result is a RAM image, loaded into the program region and run there.
 * Linked with app_flash.ld and compiled with -DFREYA_APP_XIP the result
 * executes in place from the program flash region, and the three .data
 * addresses tell the loader where to copy its initialised variables.
 */
#include "freya_api.h"

#ifndef APP_NAME
#define APP_NAME "app"
#endif

int app_main(const freya_api_t *api, int argc, char **argv);

extern char __bss_start__[];
extern char __bss_end__[];
extern char __image_size__[];       /* absolute symbol: image byte count */

#ifdef FREYA_APP_XIP
extern char __data_load__[];
extern char __data_start__[];
extern char __data_end__[];
#define HDR_LOAD_ADDR   FREYA_APP_FLASH_ADDR
#define HDR_FLAGS       FREYA_APP_F_XIP
#define HDR_DATA_SRC    ((uint32_t)(uintptr_t)__data_load__)
#define HDR_DATA_START  ((uint32_t)(uintptr_t)__data_start__)
#define HDR_DATA_END    ((uint32_t)(uintptr_t)__data_end__)
#else
#define HDR_LOAD_ADDR   FREYA_APP_LOAD_ADDR
#define HDR_FLAGS       0UL
#define HDR_DATA_SRC    0UL
#define HDR_DATA_START  0UL
#define HDR_DATA_END    0UL
#endif

__attribute__((section(".app_header"), used))
const freya_app_header_t freya_header = {
    .magic       = FREYA_APP_MAGIC,
    .abi_version = FREYA_ABI_VERSION,
    .load_addr   = HDR_LOAD_ADDR,
    .entry       = (uint32_t)(uintptr_t)&app_main,
    .image_size  = (uint32_t)(uintptr_t)__image_size__,
    .bss_start   = (uint32_t)(uintptr_t)__bss_start__,
    .bss_end     = (uint32_t)(uintptr_t)__bss_end__,
    .stack_need  = 2048,
    .name        = APP_NAME,
    .flags       = HDR_FLAGS,
    .data_src    = HDR_DATA_SRC,
    .data_start  = HDR_DATA_START,
    .data_end    = HDR_DATA_END,
};
