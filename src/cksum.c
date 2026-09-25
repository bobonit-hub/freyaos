/*
 * Freya - firmware control sum.
 *
 * The sum is the bytes of the kernel image and of the kernel extension,
 * added into a 32-bit accumulator.  It is stored in the fourth word of
 * the auto-start slot.  That word lies outside both images, so it is not
 * added; fw_sum_bytes() also skips an explicit range for the same rule.
 */
#include "freya.h"

#define FW_FLASH_BASE  0x08000000UL

uint32_t fw_sum_bytes(const uint8_t *p, uint32_t addr, uint32_t len,
                      uint32_t skip_addr, uint32_t skip_len)
{
    uint32_t sum = 0;
    uint32_t i;

    for (i = 0; i < len; i++) {
        uint32_t a = addr + i;

        if (skip_len && a >= skip_addr && (a - skip_addr) < skip_len)
            continue;
        sum += p[i];
    }
    return sum;
}

#ifndef FREYA_HOST
static uint32_t add_span(uint32_t sum, const uint8_t *p, const uint8_t *end)
{
    while (p < end)
        sum += *p++;
    return sum;
}

void fw_cksum_read(fw_cksum_t *out)
{
    uint32_t sum;

    sum = add_span(0, (const uint8_t *)FW_FLASH_BASE,
                   (const uint8_t *)__kernel_flash_end);
    sum = add_span(sum, (const uint8_t *)__kext_start,
                   (const uint8_t *)__kext_end);
    out->computed = sum;
    out->stored = *(const volatile uint32_t *)(uintptr_t)
                  (FREYA_AUTOSTART_ADDR + FREYA_CKSUM_OFF);
    if (out->stored == 0xFFFFFFFFUL)
        out->status = FW_CKSUM_BLANK;
    else if (out->stored == sum)
        out->status = FW_CKSUM_OK;
    else
        out->status = FW_CKSUM_MISMATCH;
}

int fw_cksum_show(void)
{
    fw_cksum_t ck;

    fw_cksum_read(&ck);
    if (ck.status == FW_CKSUM_OK)
        kprintf("0x%08x  ok\r\n", ck.stored);
    else if (ck.status == FW_CKSUM_BLANK)
        kprintf("blank %08x\r\n", ck.computed);
    else
        kprintf("0x%08x != 0x%08x\r\n", ck.stored, ck.computed);
    return ck.status;
}
#endif
