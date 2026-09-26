#pragma once
#include <stdint.h>

/* One exchange with the STM32 console. Returns 0, or -3/-8 like the
 * other coprocessor operations. */
int term_server_handle(const uint8_t *data, uint16_t length,
                       uint8_t *reply, uint16_t *reply_length);
void term_server_start(void);
void term_server_down(void);
