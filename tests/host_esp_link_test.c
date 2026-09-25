#include <stdio.h>
#include <string.h>
#include "freya.h"

#undef BOARD_ESP_LINK
#define BOARD_ESP_LINK 0
#include "../src/esp_link.c"

static int checks, fails;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL  %s\n", what);
    } else printf("  ok    %s\n", what);
}

int main(void)
{
    esp_frame_t f, copy;
    uint8_t payload[ESP_FRAME_PAYLOAD];

    for (unsigned i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)i;
    check(esp_frame_encode(&f, ESP_OP_SEND, 42, 0, payload,
                           sizeof payload) == 0,
          "a maximum-size frame encodes");
    check(sizeof f == 512 && esp_frame_valid(&f),
          "the encoded frame is exactly 512 bytes with a valid CRC");
    check(f.sequence == 42 && f.length == ESP_FRAME_PAYLOAD &&
          memcmp(f.payload, payload, sizeof payload) == 0,
          "sequence and fragmented socket payload survive the frame");
    copy = f;
    copy.payload[117] ^= 0x40;
    check(!esp_frame_valid(&copy), "payload corruption is detected");
    copy = f;
    copy.sequence++;
    check(!esp_frame_valid(&copy), "header corruption is detected");
    check(esp_frame_encode(&copy, ESP_OP_SEND, 1, 0, payload,
                           ESP_FRAME_PAYLOAD + 1) == FREYA_ERR_ARG,
          "an oversized frame is refused");
    copy = f;
    copy.sequence = f.sequence;
    copy.crc32 = esp_frame_crc(&copy);
    check(copy.sequence == f.sequence && esp_frame_valid(&copy),
          "a duplicate sequence remains identifiable and valid");

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
