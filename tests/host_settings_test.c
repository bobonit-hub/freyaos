/*
 * Freya - two settings copies, marker, checksum, silent repair.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freya.h"

void settings_test_reset(void);
uint8_t *settings_test_flash(void);
int settings_test_writes(void);

static int fails;

static void check(const char *what, int cond)
{
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        fails++;
    }
}

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    uint32_t v;

    memcpy(&v, p + off, 4);
    return v;
}

int main(void)
{
    uint8_t *flash;
    uint32_t v, saved_cksum, saved_sum;
    uint8_t before[FREYA_SETTINGS_DATA];
    uint8_t pass[FREYA_PASSWORD_LEN];
    int writes;

    settings_test_reset();
    flash = settings_test_flash();
    check("erased settings are ok", settings_ok() == 1);
    check("erased settings are not rewritten", settings_test_writes() == 0);
    check("erased auto-start reads as off",
          settings_get("autostart", &v, 4) == 0 && v == 0xFFFFFFFFUL);

    v = FREYA_AUTOSTART_MAGIC;
    check("setting auto-start succeeds", settings_set("autostart", &v, 4) == 0);
    check("a change writes flash once", settings_test_writes() == 1);
    check("both copies carry the marker",
          rd32(flash, 0) == FREYA_SETTINGS_MAGIC &&
          rd32(flash, FREYA_SETTINGS_BLOCK) == FREYA_SETTINGS_MAGIC);
    check("both copies match",
          memcmp(flash, flash + FREYA_SETTINGS_BLOCK, FREYA_SETTINGS_BLOCK) == 0);
    check("auto-start reads back",
          settings_get("autostart", &v, 4) == 0 && v == FREYA_AUTOSTART_MAGIC);
    check("a read of valid copies does not write", settings_test_writes() == 1);

    v = 0x11223344UL;
    check("the firmware sum can be stored by name",
          settings_set("cksum", &v, 4) == 0);
    check("the firmware sum reads back",
          settings_get("cksum", &v, 4) == 0 && v == 0x11223344UL);
    saved_cksum = rd32(flash, FREYA_SET_CKSUM_OFF);
    saved_sum = rd32(flash, FREYA_SET_SUM_OFF);

    flash[FREYA_SETTINGS_BLOCK] ^= 0x5A;
    writes = settings_test_writes();
    check("one corrupt copy still reads",
          settings_get("cksum", &v, 4) == 0 && v == 0x11223344UL);
    check("one corrupt copy is repaired silently",
          settings_test_writes() == writes + 1);
    check("the repair rewrote both copies from the good one",
          memcmp(flash, flash + FREYA_SETTINGS_BLOCK, FREYA_SETTINGS_BLOCK) == 0 &&
          rd32(flash, FREYA_SET_SUM_OFF) == saved_sum);

    flash[0] ^= 0xFF;
    flash[FREYA_SETTINGS_BLOCK] ^= 0xFF;
    memcpy(before, flash, FREYA_SETTINGS_DATA);
    writes = settings_test_writes();
    check("both corrupt copies fail a read",
          settings_get("autostart", &v, 4) != 0);
    check("both corrupt copies are not rewritten",
          settings_test_writes() == writes &&
          memcmp(flash, before, FREYA_SETTINGS_DATA) == 0);
    check("sysinfo would report fail", settings_ok() == 0);
    check("reporting fail still does not touch the checksum",
          settings_test_writes() == writes &&
          rd32(flash, FREYA_SET_SUM_OFF) == rd32(before, FREYA_SET_SUM_OFF) &&
          rd32(flash, FREYA_SETTINGS_BLOCK + FREYA_SET_SUM_OFF) ==
              rd32(before, FREYA_SETTINGS_BLOCK + FREYA_SET_SUM_OFF));

    v = 3;
    check("a change rewrites both copies", settings_set("loglevel", &v, 4) == 0);
    check("the new log level is stored",
          settings_get("loglevel", &v, 4) == 0 && v == 3);
    check("a change keeps the firmware sum when both copies were corrupt",
          rd32(flash, FREYA_SET_CKSUM_OFF) == saved_cksum &&
          rd32(flash, FREYA_SETTINGS_BLOCK + FREYA_SET_CKSUM_OFF) == saved_cksum);
    check("a change updates both checksums",
          rd32(flash, FREYA_SET_SUM_OFF) != saved_sum &&
          rd32(flash, FREYA_SET_SUM_OFF) ==
              rd32(flash, FREYA_SETTINGS_BLOCK + FREYA_SET_SUM_OFF) &&
          settings_ok() == 1);

    memcpy(pass, "12345678", FREYA_PASSWORD_LEN);
    saved_sum = rd32(flash, FREYA_SET_SUM_OFF);
    writes = settings_test_writes();
    check("the password is stored by name",
          settings_set("password", pass, FREYA_PASSWORD_LEN) == 0);
    check("the password write updates the checksum",
          settings_test_writes() == writes + 1 &&
          rd32(flash, FREYA_SET_SUM_OFF) != saved_sum);
    memset(pass, 0, sizeof pass);
    check("the password reads back",
          settings_get("password", pass, FREYA_PASSWORD_LEN) == 0 &&
          memcmp(pass, "12345678", FREYA_PASSWORD_LEN) == 0);

    writes = settings_test_writes();
    {
        uint8_t block[FREYA_SETTINGS_BLOCK];
        uint32_t sum;

        check("the default block is one settings copy",
              settings_block(block, (int)sizeof block) == (int)FREYA_SETTINGS_BLOCK);
        check("the default block starts with the marker",
              rd32(block, 0) == FREYA_SETTINGS_MAGIC);
        check("the default block leaves the named fields erased",
              rd32(block, FREYA_SET_AUTOSTART_OFF) == 0xFFFFFFFFUL &&
              rd32(block, FREYA_SET_CKSUM_OFF) == 0xFFFFFFFFUL &&
              block[FREYA_SET_PASSWORD_OFF] == 0xFF);
        sum = 0;
        for (unsigned i = 0; i < FREYA_SETTINGS_BLOCK; i++) {
            if (i >= FREYA_SET_SUM_OFF && i < FREYA_SET_SUM_OFF + 4) continue;
            sum += block[i];
        }
        check("the default block carries its checksum",
              rd32(block, FREYA_SET_SUM_OFF) == sum);
        check("a short buffer is refused",
              settings_block(block, 4) == FREYA_ERR_ARG);
        check("the system area is the reserved flash size",
              settings_area_size() == FREYA_SETTINGS_SIZE);
    }

    check("an unknown name is refused", settings_get("nope", &v, 4) != 0);
    check("a wrong length is refused", settings_set("loglevel", &v, 1) != 0);
    check("a refused call does not write", settings_test_writes() == writes);

    return fails ? 1 : 0;
}
