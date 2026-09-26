#ifndef FREYA_ESP_LINK_H
#define FREYA_ESP_LINK_H

#include <stdint.h>

#define ESP_FRAME_SIZE       512U
#define ESP_FRAME_MAGIC      0x31505345UL  /* "ESP1" */
#define ESP_FRAME_VERSION    1U
#define ESP_FRAME_HEADER     20U
#define ESP_FRAME_PAYLOAD    (ESP_FRAME_SIZE - ESP_FRAME_HEADER)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t sequence;
    uint16_t length;
    int16_t  status;
    uint32_t crc32;
    uint8_t  payload[ESP_FRAME_PAYLOAD];
} esp_frame_t;

enum {
    ESP_OP_FETCH = 0,
    ESP_OP_WIFI_ON,
    ESP_OP_WIFI_OFF,
    ESP_OP_WIFI_CREDENTIALS,
    ESP_OP_WIFI_CONNECT,
    ESP_OP_WIFI_DISCONNECT,
    ESP_OP_WIFI_STATUS,
    ESP_OP_WIFI_SCAN_START,
    ESP_OP_WIFI_SCAN_NEXT,
    ESP_OP_PING_START,
    ESP_OP_PING_RESULT,
    ESP_OP_SOCKET,
    ESP_OP_CLOSE,
    ESP_OP_CONNECT,
    ESP_OP_BIND,
    ESP_OP_LISTEN,
    ESP_OP_ACCEPT,
    ESP_OP_SEND,
    ESP_OP_RECV,
    ESP_OP_SENDTO,
    ESP_OP_RECVFROM,
    ESP_OP_TLS_CONNECT,
    ESP_OP_HTTP_START,
    ESP_OP_HTTP_INFO,
    ESP_OP_HTTP_READ,
    ESP_OP_HTTP_CLOSE,
    ESP_OP_TERM,
    ESP_OP_WEB,
    ESP_OP_EVENT = 0x8000
};

uint32_t esp_frame_crc(const esp_frame_t *frame);
int      esp_frame_encode(esp_frame_t *frame, uint16_t opcode,
                          uint32_t sequence, int status,
                          const void *payload, uint16_t length);
int      esp_frame_valid(const esp_frame_t *frame);

int      esp_link_open(void);
void     esp_link_close(void);
int      esp_link_is_open(void);
int      esp_link_owns_pin(int pin);
int      esp_link_submit(uint16_t opcode, const void *payload, uint16_t length);
int      esp_link_poll(void);
int      esp_link_response_ready(uint16_t opcode);
int      esp_link_response(uint16_t opcode, void *payload, uint16_t *length);

int      spi_dma_start(const void *tx, void *rx, uint16_t length);
int      spi_dma_done(void);
void     spi_dma_cancel(void);

#endif
