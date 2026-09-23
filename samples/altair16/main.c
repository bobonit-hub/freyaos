/*
 * Freya - an Altair 8800b Turnkey with 16 KiB of RAM.
 *
 * The same 8080, Turnkey Module and card loader as samples/altair.  The
 * main RAM is 16 KiB and no more.  On the Black Pill the interpreter and
 * that memory share the program region, so this one is loaded with 'run'
 * as well as installed in flash.  On the Blue Pill the SRAM is 20 KiB
 * and the program window is 8 KiB, so the image is flash only and the
 * 16 KiB itself lives in program flash.  4K and 8K BASIC fit.  Extended
 * BASIC's loader sits at 3F00h, inside this RAM, and leaves a little
 * workspace.  Disk BASIC's loader is at 7E00h and does not fit.
 *
 * The sources live in samples/altair.  MEM_BASE_KB and MEM_MAX_KB have
 * to be set before that main.c is included, which is what this file is.
 */
#define MEM_BASE_KB  16u
#define MEM_MAX_KB   16u
#include "../altair/main.c"
