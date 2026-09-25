/*
 * Freya - SPI NOR on the Black Pill's SOP-8 footprint.
 *
 * The chip shares SPI1 with the SD socket: CS, SCK and MOSI are PA4, PA5
 * and PA7, and MISO is PA6 or PB4 depending on the board revision.  Chip
 * select is the same pin as the card, so only one of the two is present.
 * A chip that answers JEDEC is a LittleFS volume at /spi1, the same calls
 * the card uses at /.  A blank chip is formatted on the first mount.
 */
#include "freya.h"
#include "lfsvol.h"

#if defined(FREYA_BOARD_BLACKPILL)

#define SPIFLASH_BUS    1
#define SECTOR          4096

static uint8_t  s_id[3];
static uint32_t s_bytes;
static uint32_t s_blocks;
static uint8_t  s_addr4;
static uint8_t  s_ready;
static uint8_t  s_mounted;

static uint8_t  s_cache[SECTOR];
static uint32_t s_cache_at = 0xFFFFFFFFUL;
static uint8_t  s_cache_dirty;

#ifdef FREYA_HOST
static uint8_t *s_mem;
#endif

static int cache_flush(void);
static int raw_read(uint32_t addr, uint8_t *dst, uint32_t len);
static int raw_prog(uint32_t addr, const uint8_t *src, uint32_t len);
static int raw_erase(uint32_t addr);

#ifndef FREYA_HOST

static int wait_ready(uint32_t ms)
{
    uint32_t start = sys_ticks();

    do {
        uint8_t st;

        sdspi_cs(1);
        (void)sdspi_xfer(0x05);
        st = sdspi_xfer(0xFF);
        sdspi_cs(0);
        if ((st & 0x01) == 0) return 0;
    } while ((uint32_t)(sys_ticks() - start) < ms);
    return -1;
}

static void cmd_addr(uint8_t cmd3, uint8_t cmd4, uint32_t addr)
{
    sdspi_cs(1);
    (void)sdspi_xfer(0x06);             /* write enable */
    sdspi_cs(0);
    sdspi_cs(1);
    if (s_addr4) {
        (void)sdspi_xfer(cmd4);
        (void)sdspi_xfer((uint8_t)(addr >> 24));
    } else {
        (void)sdspi_xfer(cmd3);
    }
    (void)sdspi_xfer((uint8_t)(addr >> 16));
    (void)sdspi_xfer((uint8_t)(addr >> 8));
    (void)sdspi_xfer((uint8_t)addr);
}

static int raw_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    if (wait_ready(2000) != 0) return -1;
    sdspi_cs(1);
    if (s_addr4) {
        (void)sdspi_xfer(0x13);
        (void)sdspi_xfer((uint8_t)(addr >> 24));
    } else {
        (void)sdspi_xfer(0x03);
    }
    (void)sdspi_xfer((uint8_t)(addr >> 16));
    (void)sdspi_xfer((uint8_t)(addr >> 8));
    (void)sdspi_xfer((uint8_t)addr);
    sdspi_read(dst, len);
    sdspi_cs(0);
    return 0;
}

static int raw_erase(uint32_t addr)
{
    if (wait_ready(2000) != 0) return -1;
    cmd_addr(0x20, 0x21, addr);         /* 4 KiB erase */
    sdspi_cs(0);
    return wait_ready(2000);
}

static int raw_prog(uint32_t addr, const uint8_t *src, uint32_t len)
{
    while (len) {
        uint32_t n = 256U - (addr & 255U);
        uint32_t i;
        int blank = 1;

        if (n > len) n = len;
        for (i = 0; i < n; i++)
            if (src[i] != 0xFF) { blank = 0; break; }
        if (!blank) {
            if (wait_ready(50) != 0) return -1;
            cmd_addr(0x02, 0x12, addr);
            sdspi_write(src, n);
            sdspi_cs(0);
            if (wait_ready(50) != 0) return -1;
        }
        addr += n;
        src += n;
        len -= n;
    }
    return 0;
}

static int jedec(uint8_t id[3])
{
    uint8_t again[3];

    sdspi_init();
    sdspi_set_speed(0);
    sdspi_cs(0);
    (void)sdspi_xfer(0xFF);

    sdspi_cs(1);
    (void)sdspi_xfer(0x9F);
    id[0] = sdspi_xfer(0xFF);
    id[1] = sdspi_xfer(0xFF);
    id[2] = sdspi_xfer(0xFF);
    sdspi_cs(0);
    (void)sdspi_xfer(0xFF);

    sdspi_cs(1);
    (void)sdspi_xfer(0x9F);
    again[0] = sdspi_xfer(0xFF);
    again[1] = sdspi_xfer(0xFF);
    again[2] = sdspi_xfer(0xFF);
    sdspi_cs(0);

    if (id[0] != again[0] || id[1] != again[1] || id[2] != again[2]) return -1;
    if (id[0] == 0x00 || id[0] == 0xFF) return -1;
    if (id[2] < 0x12 || id[2] > 0x1C) return -1;
    return 0;
}

/* v2.0 boards wire the chip's DO to PB4.  PA6 is put back to an input
 * so it does not fight the controller's MISO. */
static void miso_pb4(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN;
    (void)RCC->AHB1ENR;

    GPIOA->MODER &= ~(3UL << 12);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3UL << 12)) | (1UL << 12);

    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFUL << 16)) | (5UL << 16);
    GPIOB->OTYPER &= ~(1UL << 4);
    GPIOB->OSPEEDR |= (3UL << 8);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(3UL << 8)) | (1UL << 8);
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 8)) | (2UL << 8);
}

static void miso_pa6(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    GPIOB->MODER &= ~(3UL << 8);
    board_spi_pins();
}

#else /* FREYA_HOST */

static int raw_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    if (!s_mem || addr + len > s_bytes) return -1;
    memcpy(dst, s_mem + addr, len);
    return 0;
}

static int raw_erase(uint32_t addr)
{
    if (!s_mem || (addr & (SECTOR - 1U)) || addr + SECTOR > s_bytes) return -1;
    memset(s_mem + addr, 0xFF, SECTOR);
    return 0;
}

static int raw_prog(uint32_t addr, const uint8_t *src, uint32_t len)
{
    uint32_t i;

    if (!s_mem || addr + len > s_bytes) return -1;
    for (i = 0; i < len; i++) s_mem[addr + i] &= src[i];
    return 0;
}

int spiflash_test_bind(uint8_t *mem, uint32_t bytes)
{
    s_mem = mem;
    s_bytes = bytes;
    s_blocks = bytes / 512U;
    s_addr4 = bytes > (16U * 1024U * 1024U);
    s_id[0] = 0xEF;
    s_id[1] = 0x40;
    s_id[2] = 0;
    s_ready = 1;
    s_cache_at = 0xFFFFFFFFUL;
    s_cache_dirty = 0;
    s_mounted = 0;
    return 0;
}

#endif /* FREYA_HOST */

static int cache_flush(void)
{
    if (!s_cache_dirty || s_cache_at == 0xFFFFFFFFUL) return 0;
    if (raw_erase(s_cache_at) != 0) return -1;
    if (raw_prog(s_cache_at, s_cache, SECTOR) != 0) return -1;
    s_cache_dirty = 0;
    return 0;
}

static void bd_enter(void)
{
#ifndef FREYA_HOST
    app_guard_enter();
#endif
}

static void bd_leave(void)
{
#ifndef FREYA_HOST
    app_guard_leave();
#endif
}

int spiflash_bd_read(uint32_t addr, void *dst, uint32_t len)
{
    int rc;

    if (!s_ready || addr + len < addr || addr + len > s_bytes) return -1;
    bd_enter();
    if (cache_flush() != 0) { bd_leave(); return -1; }
    s_cache_at = 0xFFFFFFFFUL;
    rc = raw_read(addr, dst, len);
    bd_leave();
    return rc;
}

int spiflash_bd_prog(uint32_t addr, const void *src, uint32_t len)
{
    int rc;

    if (!s_ready || addr + len < addr || addr + len > s_bytes) return -1;
    bd_enter();
    if (cache_flush() != 0) { bd_leave(); return -1; }
    s_cache_at = 0xFFFFFFFFUL;
    rc = raw_prog(addr, src, len);
    bd_leave();
    return rc;
}

int spiflash_bd_erase(uint32_t addr)
{
    int rc;

    if (!s_ready || (addr & (SECTOR - 1U)) || addr + SECTOR > s_bytes) return -1;
    bd_enter();
    if (cache_flush() != 0) { bd_leave(); return -1; }
    s_cache_at = 0xFFFFFFFFUL;
    rc = raw_erase(addr);
    bd_leave();
    return rc;
}

void spiflash_bd_sync(void)
{
    bd_enter();
    (void)cache_flush();
    bd_leave();
}

int spiflash_bd_blank(void)
{
    uint8_t buf[256];
    uint32_t off;

    for (off = 0; off < SECTOR; off += sizeof(buf)) {
        uint32_t i;

        if (spiflash_bd_read(off, buf, sizeof(buf)) != 0) return 0;
        for (i = 0; i < sizeof(buf); i++)
            if (buf[i] != 0xFF) return 0;
    }
    return 1;
}

static int cache_load(uint32_t addr)
{
    addr &= ~(SECTOR - 1U);
    if (s_cache_at == addr) return 0;
    if (cache_flush() != 0) return -1;
    if (raw_read(addr, s_cache, SECTOR) != 0) return -1;
    s_cache_at = addr;
    s_cache_dirty = 0;
    return 0;
}

int spiflash_read_block(uint32_t lba, uint8_t *buf)
{
    uint32_t off;
    int rc;

    if (!s_ready || lba >= s_blocks) return -1;
    off = lba * 512U;
#ifndef FREYA_HOST
    app_guard_enter();
#endif
    rc = cache_load(off);
    if (rc == 0) memcpy(buf, s_cache + (off & (SECTOR - 1U)), 512);
#ifndef FREYA_HOST
    app_guard_leave();
#endif
    return rc;
}

int spiflash_write_block(uint32_t lba, const uint8_t *buf)
{
    uint32_t off;
    int rc;

    if (!s_ready || lba >= s_blocks) return -1;
    off = lba * 512U;
#ifndef FREYA_HOST
    app_guard_enter();
#endif
    rc = cache_load(off);
    if (rc == 0) {
        memcpy(s_cache + (off & (SECTOR - 1U)), buf, 512);
        s_cache_dirty = 1;
    }
#ifndef FREYA_HOST
    app_guard_leave();
#endif
    return rc;
}

void spiflash_sync(void)
{
#ifndef FREYA_HOST
    app_guard_enter();
#endif
    (void)cache_flush();
#ifndef FREYA_HOST
    app_guard_leave();
#endif
}

static const char *mfr_name(uint8_t id)
{
    switch (id) {
    case 0xEF: return "Winbond";
    case 0xC8: return "GigaDevice";
    case 0xC2: return "Macronix";
    case 0x1F: return "Adesto";
    case 0x20: return "Micron";
    case 0x9D: return "ISSI";
    case 0xA1: return "Fudan";
    case 0x68: return "Boya";
    case 0x5E: return "Zbit";
    default:   return "SPI NOR";
    }
}

const char *spiflash_name(void)
{
    static char buf[32];

    if (!s_ready) return "none";
    ksnprintf(buf, sizeof(buf), "%s %u Mbit", mfr_name(s_id[0]),
              (unsigned)(s_bytes / (1024U * 128U)));
    return buf;
}

uint32_t spiflash_bytes(void) { return s_bytes; }
int spiflash_bus(void) { return SPIFLASH_BUS; }
int spiflash_mounted(void) { return s_mounted; }

#ifndef FREYA_HOST
static void accept_id(const uint8_t id[3])
{
    s_id[0] = id[0];
    s_id[1] = id[1];
    s_id[2] = id[2];
    s_bytes = 1UL << id[2];
    s_blocks = s_bytes / 512U;
    s_addr4 = id[2] >= 0x19;
    s_ready = 1;
    s_cache_at = 0xFFFFFFFFUL;
    s_cache_dirty = 0;
}
#endif

int spiflash_probe(void)
{
#ifndef FREYA_HOST
    uint8_t id[3];

    s_ready = 0;
    if (jedec(id) == 0) {
        accept_id(id);
        sdspi_set_speed(1);
        return 0;
    }
    miso_pb4();
    if (jedec(id) == 0) {
        accept_id(id);
        sdspi_set_speed(1);
        return 0;
    }
    miso_pa6();
    s_ready = 0;
    return -1;
#else
    return s_ready ? 0 : -1;
#endif
}

int spiflash_mount_fs(void)
{
    int formatted = 0;
    int rc;

    if (!s_ready && spiflash_probe() != 0) return -1;
    lfsvol_unmount();
    s_mounted = 0;
    rc = lfsvol_mount(&formatted);
    if (rc != FAT_OK) return rc;
    s_mounted = 1;
    return formatted ? 1 : FAT_OK;
}

void spiflash_unmount(void)
{
    lfsvol_unmount();
    s_mounted = 0;
}

int spiflash_attach(void)
{
    int rc = spiflash_mount_fs();

    return (rc == FAT_OK || rc == 1) ? 0 : rc;
}

static void print_mounted(int formatted)
{
    kprintf("LittleFS");
    if (formatted) kprintf(" (formatted)");
    kprintf(", block ");
    kput_size(SECTOR);
}

/* Printed from here so the text lives in the kernel extension. */
void spiflash_boot(void)
{
    int rc;

    if (spiflash_probe() != 0) return;
    kprintf("[boot] SPI flash  : %s, ", spiflash_name());
    kput_size(spiflash_bytes());
    kprintf("\r\n[boot] filesystem : ");
    rc = spiflash_mount_fs();
    if (rc != FAT_OK && rc != 1) {
        kprintf("%s\r\n", fat_err_str(rc));
        return;
    }
    print_mounted(rc == 1);
    kprintf(", mounted on /spi%d\r\n", spiflash_bus());
#ifndef FREYA_HOST
    (void)fs_chdir("/spi1");
#endif
}

int spiflash_mount_cmd(void)
{
    int rc;

    if (spiflash_probe() != 0) return -1;
    kprintf("SPI flash %s, ", spiflash_name());
    kput_size(spiflash_bytes());
    kprintf("\r\n");
    rc = spiflash_mount_fs();
    if (rc != FAT_OK && rc != 1) {
        kprintf("mount: %s\r\n", fat_err_str(rc));
        return -1;
    }
    kprintf("mounted ");
    print_mounted(rc == 1);
    kprintf(" on /spi%d\r\n", spiflash_bus());
#ifndef FREYA_HOST
    (void)fs_chdir("/spi1");
#endif
    return 0;
}

void spiflash_info(void)
{
    if (!s_mounted) return;
    kprintf("  %-11s: %s, ", "spi flash", spiflash_name());
    kput_size(spiflash_bytes());
    kprintf(", LittleFS on /spi%d\r\n", spiflash_bus());
}

void spiflash_df(void)
{
    uint32_t used = 0;
    uint32_t blocks;

    if (!s_mounted) return;
    blocks = spiflash_bytes() / SECTOR;
    kprintf("scanning /spi%d ...\r\n", spiflash_bus());
    if (lfsvol_used_blocks(&used) != FAT_OK) {
        kprintf("df: I/O error\r\n");
        return;
    }
    kprintf("  /spi%d : LittleFS\r\n  capacity   : ", spiflash_bus());
    kput_size((uint64_t)blocks * SECTOR);
    kprintf("\r\n  free       : ");
    kput_size((uint64_t)(blocks - used) * SECTOR);
    kprintf("\r\n  used       : ");
    kput_size((uint64_t)used * SECTOR);
    kprintf("\r\n  block      : ");
    kput_size(SECTOR);
    kprintf("\r\n");
}

#else /* not the Black Pill */

int spiflash_probe(void) { return -1; }
int spiflash_mount_fs(void) { return -1; }
int spiflash_attach(void) { return -1; }
int spiflash_mounted(void) { return 0; }
const char *spiflash_name(void) { return "none"; }
uint32_t spiflash_bytes(void) { return 0; }
int spiflash_bus(void) { return 0; }
void spiflash_boot(void) { }
int spiflash_mount_cmd(void) { return -1; }
void spiflash_unmount(void) { }
void spiflash_info(void) { }
void spiflash_df(void) { }
int spiflash_bd_read(uint32_t addr, void *dst, uint32_t len)
{ (void)addr; (void)dst; (void)len; return -1; }
int spiflash_bd_prog(uint32_t addr, const void *src, uint32_t len)
{ (void)addr; (void)src; (void)len; return -1; }
int spiflash_bd_erase(uint32_t addr) { (void)addr; return -1; }
void spiflash_bd_sync(void) { }
int spiflash_bd_blank(void) { return 0; }

#endif
