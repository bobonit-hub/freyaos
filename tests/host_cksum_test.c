/*
 * Freya - the firmware sum skips the stored word.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freya.h"

static int fails;

static void check(const char *what, uint32_t want, uint32_t got)
{
    if (want == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %u, got %u\n", what, want, got);
        fails++;
    }
}

int main(void)
{
    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t image[9] = { 0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0, 0x55 };
    uint32_t sum;

    check("every byte is added", 36,
          fw_sum_bytes(data, 0x100, 8, 0, 0));
    /* 0x102..0x105 are the stored word: 3+4+5+6.  The rest still counts. */
    check("the stored word is left out", 18,
          fw_sum_bytes(data, 0x100, 8, 0x102, 4));

    sum = fw_sum_bytes(image, 0x200, 9, 0x204, 4);
    memcpy(image + 4, &sum, 4);
    check("writing the sum does not change it", sum,
          fw_sum_bytes(image, 0x200, 9, 0x204, 4));
    check("a byte outside the hole still counts",
          (uint32_t)(0x11 + 0x22 + 0x33 + 0x44 + 0x55), sum);

    return fails ? 1 : 0;
}
