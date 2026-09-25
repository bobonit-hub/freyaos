/*
 * Host exercise for the Black Pill SPI NOR volume.
 *
 * The same src/spiflash.c that runs on the board, pointed at a memory
 * image instead of the chip.  A 512-byte write must not disturb the
 * rest of its 4 KiB erase unit, and a LittleFS file under /spi1 must come
 * back intact after a remount and stay off the SD card.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freya.h"
#include "fat.h"

sd_info_t    g_sd;
sys_clocks_t g_clocks;
app_state_t  g_app;

static FILE *s_sd;
static int s_fail;
static int s_checks;

void uart_putc(char c) { fputc(c, stdout); }
void uart_puts(const char *s) { fputs(s, stdout); }
uint32_t sys_ticks(void) { return 0; }
uint16_t rtc_fat_date(void) { return (uint16_t)(((2026 - 1980) << 9) | (9 << 5) | 25); }
uint16_t rtc_fat_time(void) { return (uint16_t)((12 << 11) | (0 << 5)); }
void rtc_get(rtc_time_t *t)
{
    t->year = 2026; t->mon = 9; t->day = 25;
    t->hour = 12; t->min = 0; t->sec = 0;
}
int sd_init(void) { return s_sd ? 0 : -1; }
int sd_read_block(uint32_t lba, uint8_t *buf)
{
    if (!s_sd) return -1;
    if (fseek(s_sd, (long)lba * 512, SEEK_SET) != 0) return -1;
    return fread(buf, 1, 512, s_sd) == 512 ? 0 : -1;
}
int sd_write_block(uint32_t lba, const uint8_t *buf)
{
    if (!s_sd) return -1;
    if (fseek(s_sd, (long)lba * 512, SEEK_SET) != 0) return -1;
    return fwrite(buf, 1, 512, s_sd) == 512 ? 0 : -1;
}
int sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count)
{
    while (count--) {
        if (sd_read_block(lba++, buf) != 0) return -1;
        buf += 512;
    }
    return 0;
}

static void check(int cond, const char *what)
{
    s_checks++;
    if (cond) printf("  ok    %s\n", what);
    else { printf("  FAIL  %s\n", what); s_fail++; }
}

static void check_rc(int rc, const char *what)
{
    s_checks++;
    if (rc == FAT_OK) printf("  ok    %s\n", what);
    else { printf("  FAIL  %s -> %s (%d)\n", what, fat_err_str(rc), rc); s_fail++; }
}

static int test_rmw(void)
{
    uint8_t *mem = malloc(65536);
    uint8_t blk[512];
    uint32_t i;

    if (!mem) return 1;
    memset(mem, 0xA5, 65536);
    check(spiflash_test_bind(mem, 65536) == 0, "bind a 64 KiB chip");
    for (i = 0; i < 512; i++) blk[i] = (uint8_t)(0x22 + (i & 7));
    check(spiflash_write_block(1, blk) == 0, "write logical block 1");
    spiflash_sync();
    check(mem[0] == 0xA5 && mem[511] == 0xA5, "erase unit head unchanged");
    check(memcmp(mem + 512, blk, 512) == 0, "block 1 holds the write");
    check(mem[1024] == 0xA5 && mem[4095] == 0xA5, "rest of the 4 KiB unit unchanged");
    check(mem[4096] == 0xA5, "the next erase unit untouched");
    free(mem);
    return 0;
}

static int test_lfs(void)
{
    enum { CHIP = 128 * 1024 };
    uint8_t *mem = malloc(CHIP);
    fat_file_t f;
    fat_dir_t dir;
    fat_dirent_t e;
    uint8_t body[1000];
    uint8_t buf[1000];
    uint32_t n, i;
    int rc, saw = 0;

    if (!mem) return 1;
    memset(mem, 0xA5, CHIP);
    check(spiflash_test_bind(mem, CHIP) == 0, "bind a chip that is not blank");
    rc = spiflash_mount_fs();
    check(rc != FAT_OK && rc != 1, "a foreign image is not formatted");
    check(mem[0] == 0xA5 && mem[CHIP - 1] == 0xA5, "and its bytes are left alone");

    memset(mem, 0xFF, CHIP);
    check(spiflash_test_bind(mem, CHIP) == 0, "bind an erased 128 KiB chip");
    rc = spiflash_mount_fs();
    check(rc == 1, "a blank chip is formatted as LittleFS");

    check(fat_stat("/spi1", &e) == FAT_OK && (e.attr & FAT_ATTR_DIR),
          "/spi1 is the flash root");
    check(fat_stat("/only-on-card", &e) == FAT_ERR_NOFS,
          "the card is not that volume");

    for (i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i * 3 + 1);
    rc = fat_open(&f, "/spi1/SPI.TXT", FAT_WRITE | FAT_CREATE | FAT_TRUNC);
    check_rc(rc, "create /spi1/SPI.TXT");
    if (rc == FAT_OK) {
        check(fat_write(&f, body, sizeof body, &n) == FAT_OK && n == sizeof body,
              "write a file larger than the cache");
        check_rc(fat_seek(&f, 10), "seek inside it");
        check(fat_write(&f, "XYZ", 3, &n) == FAT_OK && n == 3, "overwrite three bytes");
        check_rc(fat_close(&f), "close it");
    }
    check_rc(fat_mkdir("/spi1/dir"), "mkdir /spi1/dir");
    check(fat_mkdir("/spi1/dir") == FAT_ERR_EXIST, "mkdir of an existing name fails");
    rc = fat_open(&f, "/spi1/dir/n.txt", FAT_WRITE | FAT_CREATE | FAT_TRUNC);
    check_rc(rc, "create a file in the directory");
    if (rc == FAT_OK) {
        fat_write(&f, "n", 1, &n);
        fat_close(&f);
    }
    check(fat_unlink("/spi1/dir") == FAT_ERR_NOTEMPTY,
          "refuses to delete a non-empty directory");
    check_rc(fat_rename("/spi1/SPI.TXT", "/spi1/dir/moved.txt"),
             "rename within the flash volume");
    check(fat_rename("/spi1/dir/moved.txt", "/SPI.TXT") == FAT_ERR_INVAL,
          "a rename onto the card is refused");

    spiflash_unmount();
    rc = spiflash_mount_fs();
    check(rc == FAT_OK, "remount keeps the filesystem");
    rc = fat_open(&f, "/spi1/dir/moved.txt", FAT_READ);
    check_rc(rc, "reopen the file after remount");
    if (rc == FAT_OK) {
        memset(buf, 0, sizeof buf);
        check(fat_read(&f, buf, sizeof buf, &n) == FAT_OK && n == sizeof body,
              "the size survived");
        body[10] = 'X'; body[11] = 'Y'; body[12] = 'Z';
        check(memcmp(buf, body, sizeof body) == 0, "the bytes survived");
        fat_close(&f);
    }
    check_rc(fat_opendir(&dir, "/spi1/dir"), "open the directory");
    while (fat_readdir(&dir, &e) == 0) {
        if (strcmp(e.name, "moved.txt") == 0) saw = 1;
    }
    fat_closedir(&dir);
    check(saw, "the directory lists the renamed file");
    check_rc(fat_unlink("/spi1/dir/moved.txt"), "delete the file");
    check_rc(fat_unlink("/spi1/dir/n.txt"), "delete the other file");
    check_rc(fat_unlink("/spi1/dir"), "delete the empty directory");
    check_rc(fat_sync(), "sync the volume");
    free(mem);
    return 0;
}

int main(void)
{
    printf("SPI flash volume\n");
    test_rmw();
    if (test_lfs() != 0) return 1;
    printf("%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
