/*
 * Freya - the shell language on Linux.
 *
 * Not a board: src/freya.h includes a header with this name, and this
 * one says the shell is built as an ordinary Linux program.  There are
 * no pins, timers, buses, program flash or network link, so every
 * command and function that touches them is left out of the build.
 */
#ifndef FREYA_BOARD_H
#define FREYA_BOARD_H

#define FREYA_LINUX         1

#define BOARD_NAME          "Linux"
#define BOARD_MCU           "Linux"

/* src/freya.h refuses a board with less; nothing here reads it. */
#define BOARD_FLASH_KIB         128U
#define BOARD_FLASH_PAGE_SIZE   1024U

#define BOARD_SHELL_FLOAT    1
#define BOARD_NET_SUPPORTED  0
#define BOARD_ESP_LINK       0
#define BOARD_COMPRESS       0
#define BOARD_AEAD           0
#define BOARD_VM             0
#define BOARD_RTC_API        1

/* linux/platform.c.  linux_spawn() runs argv[0], found on PATH, with
 * the rest as its arguments.  With out set, what the program writes on
 * its standard output is collected into a kmalloc() buffer, *out and
 * *len, which the caller frees; otherwise it writes to the terminal.
 * Returns the exit status, 128 + the signal that ended it, or 127 when
 * it could not be started; -1, with a message, when nothing ran. */
int  linux_spawn(char *const argv[], char **out, uint32_t *len);
void linux_exit(int status) __attribute__((noreturn));

/* src/freya.h declares the timer lending calls with this type. */
typedef struct TIM_TypeDef TIM_TypeDef;

#endif /* FREYA_BOARD_H */
