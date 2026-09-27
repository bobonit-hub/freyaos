/*
 * Freya - system settings at the end of internal flash.
 *
 * Two copies.  Each begins with FREYA_SETTINGS_MAGIC.  The last named
 * field is followed by a checksum of every other byte in the copy.  The
 * name-to-offset map is fixed: autostart, loglevel, ramdump, cksum,
 * password.
 *
 * Every get or set reads both copies.  A copy is blank when it is still
 * erased, valid when the marker and the checksum agree, and corrupt
 * otherwise.  One valid copy is written over the other without a console
 * message.  When both are corrupt the checksum words are left alone,
 * unless a set changes a field: that rewrites both copies and stores a
 * new checksum in each.  The firmware sum word is kept as it was unless
 * the field being written is "cksum" itself.
 */
#include "freya.h"

#include <string.h>

#define SET_ERASED  0xFFFFFFFFUL

enum {
    COPY_BLANK = 0,
    COPY_VALID,
    COPY_BAD
};

/* Hardcoded.  Offsets are within one copy, after the marker. */
static const struct {
    const char *name;
    uint16_t    off;
    uint16_t    len;
} s_map[] = {
    { "autostart", FREYA_SET_AUTOSTART_OFF, 4 },
    { "loglevel",  FREYA_SET_LOGLEVEL_OFF,  4 },
    { "ramdump",   FREYA_SET_RAMDUMP_OFF,   4 },
    { "cksum",     FREYA_SET_CKSUM_OFF,     4 },
    { "password",  FREYA_SET_PASSWORD_OFF,  FREYA_PASSWORD_LEN },
};

#ifdef FREYA_HOST
static uint8_t s_flash[FREYA_SETTINGS_SIZE];
static int     s_writes;

void settings_test_reset(void)
{
    memset(s_flash, 0xFF, sizeof s_flash);
    s_writes = 0;
}

uint8_t *settings_test_flash(void)
{
    return s_flash;
}

int settings_test_writes(void)
{
    return s_writes;
}
#endif

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    uint32_t v;

    memcpy(&v, p + off, 4);
    return v;
}

static void wr32(uint8_t *p, uint32_t off, uint32_t v)
{
    memcpy(p + off, &v, 4);
}

static const void *field_by_name(const char *name, uint16_t *off, uint16_t *len)
{
    unsigned i;

    if (!name) return NULL;
    for (i = 0; i < ARRAY_SIZE(s_map); i++) {
        if (strcmp(s_map[i].name, name) != 0) continue;
        *off = s_map[i].off;
        *len = s_map[i].len;
        return s_map[i].name;
    }
    return NULL;
}

static int copy_blank(const uint8_t *b)
{
    unsigned i;

    for (i = 0; i < FREYA_SETTINGS_BLOCK; i++)
        if (b[i] != 0xFF) return 0;
    return 1;
}

static uint32_t copy_sum(const uint8_t *b)
{
    return fw_sum_bytes(b, 0, FREYA_SETTINGS_BLOCK, FREYA_SET_SUM_OFF, 4);
}

static void copy_seal(uint8_t *b)
{
    wr32(b, FREYA_SET_MARKER_OFF, FREYA_SETTINGS_MAGIC);
    wr32(b, FREYA_SET_SUM_OFF, copy_sum(b));
}

static int copy_kind(const uint8_t *b)
{
    if (copy_blank(b)) return COPY_BLANK;
    if (rd32(b, FREYA_SET_MARKER_OFF) == FREYA_SETTINGS_MAGIC &&
        rd32(b, FREYA_SET_SUM_OFF) == copy_sum(b))
        return COPY_VALID;
    return COPY_BAD;
}

static void settings_load(uint8_t *c0, uint8_t *c1)
{
#ifdef FREYA_HOST
    memcpy(c0, s_flash, FREYA_SETTINGS_BLOCK);
    memcpy(c1, s_flash + FREYA_SETTINGS_BLOCK, FREYA_SETTINGS_BLOCK);
#else
    const uint8_t *p = (const uint8_t *)(uintptr_t)FREYA_SETTINGS_ADDR;

    memcpy(c0, p, FREYA_SETTINGS_BLOCK);
    memcpy(c1, p + FREYA_SETTINGS_BLOCK, FREYA_SETTINGS_BLOCK);
#endif
}

/*
 * Erase the settings area and program both copies from one sealed block.
 * 'unload' drops a program that is loaded but not running, which is what
 * a settings change from the shell has always done.  A silent repair
 * leaves that program in place and skips the write when flash is busy.
 */
static int settings_store(const uint8_t *block, int unload)
{
    uint8_t data[FREYA_SETTINGS_DATA];

    memcpy(data, block, FREYA_SETTINGS_BLOCK);
    memcpy(data + FREYA_SETTINGS_BLOCK, block, FREYA_SETTINGS_BLOCK);
#ifdef FREYA_HOST
    memset(s_flash, 0xFF, sizeof s_flash);
    memcpy(s_flash, data, sizeof data);
    s_writes++;
    (void)unload;
    return 0;
#else
    int rc;

    if (g_app.running) return FLASH_ERR_BUSY;
    if (g_app.loaded) {
        if (!unload) return FLASH_ERR_BUSY;
        app_unload();
    }
    rc = flash_begin();
    if (rc != FLASH_OK) return rc;
    rc = flash_erase(FREYA_SETTINGS_ADDR, FREYA_SETTINGS_SIZE);
    if (rc == FLASH_OK)
        rc = flash_program(FREYA_SETTINGS_ADDR, data, FREYA_SETTINGS_DATA);
    flash_end();
    return rc;
#endif
}

/*
 * 0 when the area can be trusted (a valid copy, or both still erased).
 * -1 when both copies are corrupt: nothing is written.
 * A repair stores the good copy over the bad one.  If the store cannot
 * run, the RAM copies are still the repaired pair for this call.
 */
static int reconcile(uint8_t *c0, uint8_t *c1)
{
    int k0 = copy_kind(c0);
    int k1 = copy_kind(c1);
    const uint8_t *good = NULL;

    if (k0 == COPY_VALID && k1 == COPY_VALID) return 0;
    if (k0 == COPY_BLANK && k1 == COPY_BLANK) return 0;
    if (k0 == COPY_VALID) good = c0;
    else if (k1 == COPY_VALID) good = c1;
    if (!good) return -1;

    if (k0 != COPY_VALID) memcpy(c0, good, FREYA_SETTINGS_BLOCK);
    if (k1 != COPY_VALID) memcpy(c1, good, FREYA_SETTINGS_BLOCK);
    /* The bad copy is replaced from the good one.  No console line. */
    (void)settings_store(good, 0);
    return 0;
}

int settings_get(const char *name, void *buf, uint32_t len)
{
    uint8_t c0[FREYA_SETTINGS_BLOCK];
    uint8_t c1[FREYA_SETTINGS_BLOCK];
    uint16_t off, flen;

    if (!buf || !field_by_name(name, &off, &flen) || len != flen) return -1;
    settings_load(c0, c1);
    if (reconcile(c0, c1) != 0) return -1;
    if (copy_kind(c0) == COPY_BLANK) memset(buf, 0xFF, len);
    else memcpy(buf, c0 + off, len);
    return 0;
}

int settings_set(const char *name, const void *buf, uint32_t len)
{
    uint8_t c0[FREYA_SETTINGS_BLOCK];
    uint8_t c1[FREYA_SETTINGS_BLOCK];
    uint8_t block[FREYA_SETTINGS_BLOCK];
    uint16_t off, flen;
    int both_bad;

    if (!buf || !field_by_name(name, &off, &flen) || len != flen) return -1;
    settings_load(c0, c1);
    both_bad = reconcile(c0, c1) != 0;
    if (!both_bad && copy_kind(c0) == COPY_VALID)
        memcpy(block, c0, sizeof block);
    else {
        memset(block, 0xFF, sizeof block);
        /*
         * Both copies are corrupt, or the area is still erased.  An erased
         * area has no firmware sum to keep.  A corrupt pair keeps the sum
         * word already in the first copy, unless this set is that word.
         */
        if (both_bad && strcmp(name, "cksum") != 0)
            memcpy(block + FREYA_SET_CKSUM_OFF, c0 + FREYA_SET_CKSUM_OFF, 4);
    }
    if (!both_bad && memcmp(block + off, buf, len) == 0 &&
        copy_kind(c0) == COPY_VALID &&
        memcmp(c0, c1, FREYA_SETTINGS_BLOCK) == 0)
        return 0;
    memcpy(block + off, buf, len);
    copy_seal(block);
    return settings_store(block, 1);
}

int settings_ok(void)
{
    uint8_t c0[FREYA_SETTINGS_BLOCK];
    uint8_t c1[FREYA_SETTINGS_BLOCK];

    settings_load(c0, c1);
    return reconcile(c0, c1) == 0;
}

int settings_block(void *buf, int len)
{
    if (!buf || len < (int)FREYA_SETTINGS_BLOCK) return FREYA_ERR_ARG;
    memset(buf, 0xFF, FREYA_SETTINGS_BLOCK);
    copy_seal(buf);
    return (int)FREYA_SETTINGS_BLOCK;
}

uint32_t settings_area_size(void)
{
    return FREYA_SETTINGS_SIZE;
}

uint32_t settings_fw_stored(void)
{
    uint8_t c0[FREYA_SETTINGS_BLOCK];
    uint8_t c1[FREYA_SETTINGS_BLOCK];

    settings_load(c0, c1);
    if (reconcile(c0, c1) != 0)
        return rd32(c0, FREYA_SET_CKSUM_OFF);
    if (copy_kind(c0) == COPY_BLANK) return SET_ERASED;
    return rd32(c0, FREYA_SET_CKSUM_OFF);
}
