#pragma once
#include <stdint.h>

/* One exchange with the STM32 file service. Returns 0, or -3/-7/-8
 * like the other coprocessor operations. */
int web_server_handle(const uint8_t *data, uint16_t length,
                      uint8_t *reply, uint16_t *reply_length);
void web_server_start(void);
void web_server_down(void);
