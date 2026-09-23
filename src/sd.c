/*
 * Freya - SD / SDHC card driver (SPI mode).
 *
 * Implements the card identification sequence from the SD Physical Layer
 * spec, single and multiple block reads and single block writes.  Byte
 * addressed (standard capacity) and block addressed (high capacity) cards
 * are both handled; the rest of the system always talks in 512 byte LBAs.
 */
#include "freya.h"

sd_info_t g_sd;

#define CMD0    0       /* GO_IDLE_STATE           */
#define CMD1    1       /* SEND_OP_COND (MMC)      */
#define CMD8    8       /* SEND_IF_COND            */
#define CMD9    9       /* SEND_CSD                */
#define CMD10   10      /* SEND_CID                */
#define CMD12   12      /* STOP_TRANSMISSION       */
#define CMD16   16      /* SET_BLOCKLEN            */
#define CMD17   17      /* READ_SINGLE_BLOCK       */
#define CMD18   18      /* READ_MULTIPLE_BLOCK     */
#define CMD24   24      /* WRITE_BLOCK             */
#define CMD55   55      /* APP_CMD                 */
#define CMD58   58      /* READ_OCR                */
#define ACMD41  41      /* SD_SEND_OP_COND         */

#define R1_IDLE         0x01
#define R1_ILLEGAL_CMD  0x04

#define TOKEN_START     0xFE
#define TOKEN_MULTI     0xFC
#define TOKEN_STOP      0xFD

/* After VDD is applied the card needs the rail to finish rising before
 * the first clock.  The simplified spec allows 1 ms; 10 ms covers a
 * slow switch.  The 74 clocks are sd_init()'s. */
#define SD_POWER_UP_MS  10

/* The pull-down on the gate holds the card on out of reset, so the
 * socket is powered before this driver has run.  The two calls are in
 * their own section: the Black Pill linker keeps them out of the 48 KiB
 * image, and the Blue Pill image still has room for that section. */
static int s_powered = 1;

#define SD_PWR __attribute__((noinline, section(".text.sd_power")))

static uint8_t crc7(const uint8_t *data, int len)
{
    uint8_t crc = 0;

    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            crc <<= 1;
            if ((b & 0x80) ^ (crc & 0x80)) crc ^= 0x09;
            b <<= 1;
        }
    }
    return (uint8_t)((crc << 1) | 1);
}

/* Waits for the card to release the busy (all-zero) state on MISO. */
static int wait_ready(uint32_t ms)
{
    uint32_t start = sys_ticks();

    do {
        if (sdspi_xfer(0xFF) == 0xFF) return 0;
    } while ((uint32_t)(sys_ticks() - start) < ms);
    return -1;
}

static void deselect(void)
{
    sdspi_cs(0);
    (void)sdspi_xfer(0xFF);           /* release DO after CS rises */
}

static int select_card(void)
{
    sdspi_cs(1);
    (void)sdspi_xfer(0xFF);
    if (wait_ready(500) == 0) return 0;
    deselect();
    return -1;
}

static uint8_t send_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t frame[6];
    uint8_t r1 = 0xFF;

    if (cmd != CMD12 && wait_ready(500) != 0) return 0xFF;

    frame[0] = (uint8_t)(0x40 | cmd);
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >> 8);
    frame[4] = (uint8_t)arg;
    frame[5] = crc7(frame, 5);
    sdspi_write(frame, 6);

    if (cmd == CMD12) (void)sdspi_xfer(0xFF);     /* discard the stuff byte */

    for (int i = 0; i < 10; i++) {
        r1 = sdspi_xfer(0xFF);
        if (!(r1 & 0x80)) break;
    }
    return r1;
}

static uint8_t send_acmd(uint8_t cmd, uint32_t arg)
{
    uint8_t r1 = send_cmd(CMD55, 0);
    if (r1 > 1) return r1;
    return send_cmd(cmd, arg);
}

static int read_data(uint8_t *buf, uint32_t len)
{
    uint32_t start = sys_ticks();
    uint8_t token;

    do {
        token = sdspi_xfer(0xFF);
        if (token != 0xFF) break;
    } while ((uint32_t)(sys_ticks() - start) < 200);

    if (token != TOKEN_START) return -1;
    sdspi_read(buf, len);
    (void)sdspi_xfer(0xFF);           /* CRC16, ignored */
    (void)sdspi_xfer(0xFF);
    return 0;
}

static void decode_csd(void)
{
    const uint8_t *c = g_sd.csd;

    if ((c[0] >> 6) == 1) {                     /* CSD version 2.0 */
        uint32_t c_size = ((uint32_t)(c[7] & 0x3F) << 16) |
                          ((uint32_t)c[8] << 8) | c[9];
        g_sd.blocks = (c_size + 1) * 1024;
    } else {                                    /* CSD version 1.0 */
        uint32_t read_bl_len = c[5] & 0x0F;
        uint32_t c_size = ((uint32_t)(c[6] & 0x03) << 10) |
                          ((uint32_t)c[7] << 2) | ((uint32_t)c[8] >> 6);
        uint32_t mult = (uint32_t)(((c[9] & 0x03) << 1) | ((c[10] & 0x80) >> 7));
        uint32_t blk_nr = (c_size + 1) * (1UL << (mult + 2));
        g_sd.blocks = blk_nr * (1UL << read_bl_len) / 512UL;
    }
}

int SD_PWR sd_powered(void)
{
    return s_powered;
}

/* The rail only.  Callers that also own the filesystem go through
 * board_power(), which unmounts before this drops VDD. */
int SD_PWR sd_power(int on)
{
    if (on) {
        int cold = !s_powered;

        board_sd_power(1);
        s_powered = 1;
        if (cold) sys_delay_ms(SD_POWER_UP_MS);
        return 0;
    }

    memset(&g_sd, 0, sizeof(g_sd));
    sdspi_quiesce();
    board_sd_power(0);
    s_powered = 0;
    return 0;
}

int sd_init(void)
{
    uint8_t r1, ocr[4];
    uint32_t start;
    int i;

    if (!s_powered) sd_power(1);

    memset(&g_sd, 0, sizeof(g_sd));
    sdspi_init();
    sdspi_set_speed(0);

    /* At least 74 clocks with CS and DI high to wake the card up. */
    sdspi_cs(0);
    for (i = 0; i < 10; i++) (void)sdspi_xfer(0xFF);

    sdspi_cs(1);
    for (i = 0; i < 64; i++) {
        r1 = send_cmd(CMD0, 0);
        if (r1 == R1_IDLE) break;
        sys_delay_ms(2);
    }
    if (r1 != R1_IDLE) { deselect(); return -1; }

    r1 = send_cmd(CMD8, 0x000001AA);
    if (r1 == R1_IDLE) {
        sdspi_read(ocr, 4);                       /* rest of R7 */
        if (ocr[2] != 0x01 || ocr[3] != 0xAA) { deselect(); return -2; }

        start = sys_ticks();
        do {
            r1 = send_acmd(ACMD41, 0x40000000); /* HCS = 1 */
            if (r1 == 0) break;
        } while ((uint32_t)(sys_ticks() - start) < 2000);
        if (r1 != 0) { deselect(); return -3; }

        if (send_cmd(CMD58, 0) != 0) { deselect(); return -4; }
        sdspi_read(ocr, 4);
        g_sd.type = (ocr[0] & 0x40) ? SD_TYPE_SDHC : SD_TYPE_SD2;
    } else {
        /* Version 1 SD or MMC. */
        r1 = send_acmd(ACMD41, 0);
        if (r1 <= 1) {
            g_sd.type = SD_TYPE_SD1;
            start = sys_ticks();
            do {
                r1 = send_acmd(ACMD41, 0);
                if (r1 == 0) break;
            } while ((uint32_t)(sys_ticks() - start) < 2000);
        } else {
            g_sd.type = SD_TYPE_MMC;
            start = sys_ticks();
            do {
                r1 = send_cmd(CMD1, 0);
                if (r1 == 0) break;
            } while ((uint32_t)(sys_ticks() - start) < 2000);
        }
        if (r1 != 0) { deselect(); return -5; }
    }

    if (g_sd.type != SD_TYPE_SDHC) {
        if (send_cmd(CMD16, 512) != 0) { deselect(); return -6; }
    }

    if (send_cmd(CMD9, 0) == 0) read_data(g_sd.csd, 16);
    if (send_cmd(CMD10, 0) == 0) read_data(g_sd.cid, 16);
    decode_csd();

    deselect();
    sdspi_set_speed(1);
    g_sd.initialised = 1;
    return 0;
}

static uint32_t lba_to_arg(uint32_t lba)
{
    return (g_sd.type == SD_TYPE_SDHC) ? lba : lba * 512UL;
}

/*
 * The public entry points hold the abort guard so that a Ctrl-C can never
 * interrupt a transfer that is already in flight on the bus.
 */
static int read_block(uint32_t lba, uint8_t *buf)
{
    int rc = -1;

    if (select_card() != 0) return -1;
    if (send_cmd(CMD17, lba_to_arg(lba)) == 0)
        rc = read_data(buf, 512);

    deselect();
    return rc;
}

int sd_read_block(uint32_t lba, uint8_t *buf)
{
    int rc;

    if (!g_sd.initialised) return -1;
    app_guard_enter();
    rc = read_block(lba, buf);
    app_guard_leave();
    return rc;
}

int sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count)
{
    int rc = 0;

    if (!g_sd.initialised) return -1;
    if (count == 0) return 0;
    if (count == 1) return sd_read_block(lba, buf);

    app_guard_enter();
    if (select_card() != 0) {
        app_guard_leave();
        return -1;
    }

    if (send_cmd(CMD18, lba_to_arg(lba)) == 0) {
        while (count--) {
            if (read_data(buf, 512) != 0) { rc = -1; break; }
            buf += 512;
        }
        send_cmd(CMD12, 0);
    } else {
        rc = -1;
    }

    deselect();
    app_guard_leave();
    return rc;
}

int sd_write_block(uint32_t lba, const uint8_t *buf)
{
    int rc = -1;

    if (!g_sd.initialised) return -1;

    app_guard_enter();
    if (select_card() != 0) {
        app_guard_leave();
        return -1;
    }

    if (send_cmd(CMD24, lba_to_arg(lba)) == 0) {
        (void)sdspi_xfer(0xFF);
        sdspi_xfer(TOKEN_START);
        sdspi_write(buf, 512);
        (void)sdspi_xfer(0xFF);           /* dummy CRC16 */
        (void)sdspi_xfer(0xFF);

        if ((sdspi_xfer(0xFF) & 0x1F) == 0x05)
            rc = wait_ready(1000);
    }

    deselect();
    app_guard_leave();
    return rc;
}

const char *sd_type_str(void)
{
    switch (g_sd.type) {
    case SD_TYPE_MMC:  return "MMC";
    case SD_TYPE_SD1:  return "SD v1";
    case SD_TYPE_SD2:  return "SD v2 (SDSC)";
    case SD_TYPE_SDHC: return "SD v2 (SDHC/SDXC)";
    default:           return "none";
    }
}
