/*
 * Host side exercise for Freya's FAT layer.
 *
 * Compiles the very same src/fat.c that runs on the MCU against a disk
 * image file instead of an SD card, so the filesystem code can be checked
 * with mkfs.vfat / fsck.vfat / mtools before it ever touches hardware.
 *
 *   cc -Iinclude -Isrc tests/host_fat_test.c src/fat.c src/string.c \
 *      src/print.c -o build/hosttest
 *   ./build/hosttest image.img
 */
#include <stdio.h>
#include <stdlib.h>

#include "freya.h"
#include "fat.h"

/* ------------------------------------------------- board stubs */
sd_info_t    g_sd;
sys_clocks_t g_clocks;
app_state_t  g_app;

static FILE *s_img;
static int   s_fail;

void uart_putc(char c) { fputc(c, stdout); }
void uart_puts(const char *s) { fputs(s, stdout); }

uint32_t sys_ticks(void) { return 0; }
uint16_t rtc_fat_date(void) { return (uint16_t)(((2026 - 1980) << 9) | (9 << 5) | 21); }
uint16_t rtc_fat_time(void) { return (uint16_t)((21 << 11) | (30 << 5) | 15); }

int sd_init(void) { return 0; }

int sd_read_block(uint32_t lba, uint8_t *buf)
{
    if (fseek(s_img, (long)lba * 512, SEEK_SET) != 0) return -1;
    return (fread(buf, 1, 512, s_img) == 512) ? 0 : -1;
}

int sd_write_block(uint32_t lba, const uint8_t *buf)
{
    if (fseek(s_img, (long)lba * 512, SEEK_SET) != 0) return -1;
    return (fwrite(buf, 1, 512, s_img) == 512) ? 0 : -1;
}

int sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count)
{
    while (count--) {
        if (sd_read_block(lba++, buf) != 0) return -1;
        buf += 512;
    }
    return 0;
}

/* ------------------------------------------------- tiny test harness */
static int s_checks;

static void check(int cond, const char *what)
{
    s_checks++;
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        s_fail++;
    }
}

static void check_rc(int rc, const char *what)
{
    s_checks++;
    if (rc == FAT_OK) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s -> %s (%d)\n", what, fat_err_str(rc), rc);
        s_fail++;
    }
}

static int exists(const char *path)
{
    fat_dirent_t e;
    return fat_stat(path, &e) == FAT_OK;
}

static void list(const char *path)
{
    fat_dir_t d;
    fat_dirent_t e;

    if (fat_opendir(&d, path) != FAT_OK) { printf("    (cannot open %s)\n", path); return; }
    printf("    %s:\n", path);
    while (fat_readdir(&d, &e) == 0) {
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        printf("      %-34s %s%u\n", e.name, (e.attr & FAT_ATTR_DIR) ? "<dir> " : "", e.size);
    }
    fat_closedir(&d);
}

/* ------------------------------------------------------------- tests */
static void test_write_read(const char *path, uint32_t size)
{
    static uint8_t buf[8192];
    fat_file_t f;
    uint32_t done = 0, n;
    int rc, bad = 0;
    char label[96];

    ksnprintf(label, sizeof(label), "write %u bytes to %s", size, path);

    rc = fat_open(&f, path, FAT_WRITE | FAT_READ | FAT_CREATE | FAT_TRUNC);
    check_rc(rc, label);
    if (rc != FAT_OK) return;

    while (done < size) {
        uint32_t chunk = size - done < sizeof(buf) ? size - done : sizeof(buf);
        for (uint32_t i = 0; i < chunk; i++)
            buf[i] = (uint8_t)((done + i) * 31 + ((done + i) >> 8));
        rc = fat_write(&f, buf, chunk, &n);
        if (rc != FAT_OK || n != chunk) { bad = 1; break; }
        done += chunk;
    }
    check(!bad && done == size, "  all bytes written");
    check(f.size == size, "  file size tracked");
    check_rc(fat_close(&f), "  close");

    /* Read it back and compare. */
    rc = fat_open(&f, path, FAT_READ);
    check_rc(rc, "  reopen for reading");
    if (rc != FAT_OK) return;
    check(f.size == size, "  size on reopen");

    done = 0;
    bad = 0;
    while (done < size) {
        uint32_t chunk = size - done < sizeof(buf) ? size - done : sizeof(buf);
        rc = fat_read(&f, buf, chunk, &n);
        if (rc != FAT_OK || n != chunk) { bad = 1; break; }
        for (uint32_t i = 0; i < chunk; i++) {
            if (buf[i] != (uint8_t)((done + i) * 31 + ((done + i) >> 8))) {
                printf("    mismatch at offset %u\n", done + i);
                bad = 1;
                break;
            }
        }
        if (bad) break;
        done += chunk;
    }
    check(!bad && done == size, "  content matches what was written");

    /* Read past the end returns nothing. */
    rc = fat_read(&f, buf, 16, &n);
    check(rc == FAT_OK && n == 0, "  read at EOF returns 0 bytes");
    fat_close(&f);
}

static void test_seek(const char *path)
{
    fat_file_t f;
    uint8_t buf[16];
    uint32_t n;

    if (fat_open(&f, path, FAT_READ) != FAT_OK) { check(0, "seek: open"); return; }
    check_rc(fat_seek(&f, 5000), "seek to 5000");
    check(fat_read(&f, buf, 4, &n) == FAT_OK && n == 4, "  read after seek");
    check(buf[0] == (uint8_t)(5000 * 31 + (5000 >> 8)), "  data at the seek position");
    check_rc(fat_seek(&f, 0), "  rewind");
    check(fat_read(&f, buf, 1, &n) == FAT_OK && buf[0] == 0, "  first byte after rewind");
    fat_close(&f);
}

/*
 * Interop pass: copy a file that Linux put on the card, using only Freya
 * calls, and create a long named file for Linux to read back.
 */
static int run_interop(void)
{
    static uint8_t buf[4096];
    fat_file_t in, out;
    uint32_t n, total = 0;
    fat_dirent_t e;

    check_rc(fat_mount(), "mount the image Linux formatted");

    check(fat_stat("/from_host.bin", &e) == FAT_OK, "sees the file mtools wrote");
    check(fat_stat("/hostdir", &e) == FAT_OK && (e.attr & FAT_ATTR_DIR),
          "sees the directory mtools made");

    check_rc(fat_open(&in, "/from_host.bin", FAT_READ), "open the host file");
    check_rc(fat_open(&out, "/by_freya.bin", FAT_WRITE | FAT_CREATE | FAT_TRUNC),
             "create the copy");
    for (;;) {
        if (fat_read(&in, buf, sizeof(buf), &n) != FAT_OK || n == 0) break;
        if (fat_write(&out, buf, n, &n) != FAT_OK) { check(0, "copy write"); break; }
        total += n;
    }
    check(total == in.size, "copied every byte");
    check_rc(fat_close(&in), "close the source");
    check_rc(fat_close(&out), "close the copy");

    check_rc(fat_open(&out, "/hostdir/Written By Freya.txt",
                      FAT_WRITE | FAT_CREATE | FAT_TRUNC), "create a long name for Linux");
    check_rc(fat_write(&out, "freya was here\n", 15, &n), "  write");
    check_rc(fat_close(&out), "  close");

    fat_unmount();
    printf("\n%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    uint32_t free_before = 0, free_after = 0;

    if (argc < 2) { fprintf(stderr, "usage: hosttest <image> [interop]\n"); return 2; }
    s_img = fopen(argv[1], "r+b");
    if (!s_img) { perror("open image"); return 2; }

    if (argc > 2 && !strcmp(argv[2], "interop")) {
        int rc = run_interop();
        fclose(s_img);
        return rc;
    }

    printf("mount\n");
    check_rc(fat_mount(), "fat_mount");
    printf("    type %s, label \"%s\", cluster %u B, %u clusters, %u FATs\n",
           fat_type_str(), g_fs.label, g_fs.bytes_per_clus, g_fs.clus_count, g_fs.num_fats);
    if (!fat_mounted()) return 1;

    check_rc(fat_free_clusters(&free_before), "count free clusters");

    printf("\ndirectories\n");
    check_rc(fat_mkdir("/docs"), "mkdir /docs");
    check_rc(fat_mkdir("/docs/sub"), "mkdir /docs/sub");
    check_rc(fat_mkdir("/bin"), "mkdir /bin");
    check(fat_mkdir("/docs") == FAT_ERR_EXIST, "mkdir of an existing name fails");
    check(exists("/docs") && exists("/docs/sub"), "both directories resolve");
    check(fat_stat("/nope/deep", NULL) != FAT_OK, "stat of a missing path fails");

    printf("\nsmall file\n");
    test_write_read("/docs/small.txt", 11);

    printf("\nmulti cluster file\n");
    test_write_read("/docs/big.bin", 100000);

    printf("\nsub directory file\n");
    test_write_read("/docs/sub/deep.dat", 4096);

    printf("\nseek\n");
    test_seek("/docs/big.bin");

    printf("\nlong file names\n");
    {
        fat_file_t f;
        uint32_t n;
        const char *lfn = "/docs/A Rather Long File Name.text";
        check_rc(fat_open(&f, lfn, FAT_WRITE | FAT_CREATE | FAT_TRUNC), "create a long name");
        check_rc(fat_write(&f, "long name payload", 17, &n), "  write");
        check_rc(fat_close(&f), "  close");
        check(exists(lfn), "  resolves by its long name");
        check(exists("/docs/ARATHE~1.TEX"), "  resolves by its generated 8.3 name");

        check_rc(fat_open(&f, "/docs/MixedCase.Txt", FAT_WRITE | FAT_CREATE), "create a mixed case name");
        fat_close(&f);
        check(exists("/docs/mixedcase.txt"), "  lookup is case insensitive");
    }

    printf("\nappend\n");
    {
        fat_file_t f;
        uint8_t buf[64];
        uint32_t n;
        check_rc(fat_open(&f, "/docs/small.txt", FAT_WRITE | FAT_APPEND), "open for append");
        check(f.pos == 11, "  position starts at EOF");
        check_rc(fat_write(&f, "APPENDED", 8, &n), "  write");
        check_rc(fat_close(&f), "  close");
        check_rc(fat_open(&f, "/docs/small.txt", FAT_READ), "  reopen");
        check(f.size == 19, "  size grew to 19");
        fat_read(&f, buf, sizeof(buf), &n);
        check(n == 19 && !memcmp(buf + 11, "APPENDED", 8), "  appended bytes are there");
        fat_close(&f);
    }

    printf("\nmany entries in one directory\n");
    {
        int made = 0;
        for (int i = 0; i < 40; i++) {
            char name[64];
            fat_file_t f;
            uint32_t n;
            ksnprintf(name, sizeof(name), "/bin/file%02d.bin", i);
            if (fat_open(&f, name, FAT_WRITE | FAT_CREATE | FAT_TRUNC) != FAT_OK) break;
            fat_write(&f, name, (uint32_t)strlen(name), &n);
            if (fat_close(&f) != FAT_OK) break;
            made++;
        }
        check(made == 40, "created 40 files in one directory");

        int counted = 0;
        fat_dir_t d;
        fat_dirent_t e;
        if (fat_opendir(&d, "/bin") == FAT_OK) {
            while (fat_readdir(&d, &e) == 0)
                if (strcmp(e.name, ".") && strcmp(e.name, "..")) counted++;
            fat_closedir(&d);
        }
        check(counted == 40, "all 40 are listed back");
    }

    printf("\nlistings\n");
    list("/");
    list("/docs");

    printf("\ndeletion\n");
    check(fat_unlink("/docs") == FAT_ERR_NOTEMPTY, "refuses to delete a non-empty directory");
    check_rc(fat_unlink("/docs/sub/deep.dat"), "delete a file");
    check(!exists("/docs/sub/deep.dat"), "  it is gone");
    check_rc(fat_unlink("/docs/sub"), "delete the now empty directory");
    check_rc(fat_unlink("/docs/A Rather Long File Name.text"), "delete a long name file");
    check(!exists("/docs/ARATHE~1.TEX"), "  its 8.3 alias is gone too");
    check(fat_unlink("/docs/nothing") == FAT_ERR_NOENT, "deleting a missing file fails");

    for (int i = 0; i < 40; i++) {
        char name[64];
        ksnprintf(name, sizeof(name), "/bin/file%02d.bin", i);
        if (fat_unlink(name) != FAT_OK) { check(0, "bulk delete"); break; }
    }
    check(!exists("/bin/file00.bin") && !exists("/bin/file39.bin"), "bulk delete emptied /bin");
    check_rc(fat_unlink("/bin"), "remove /bin");

    printf("\nspace accounting\n");
    check_rc(fat_unlink("/docs/big.bin"), "delete the 100 kB file");
    check_rc(fat_unlink("/docs/small.txt"), "delete the small file");
    check_rc(fat_unlink("/docs/MixedCase.Txt"), "delete the mixed case file");
    check_rc(fat_unlink("/docs"), "remove /docs");
    check_rc(fat_free_clusters(&free_after), "count free clusters again");
    printf("    free before %u, after %u\n", free_before, free_after);
    check(free_before == free_after, "every allocated cluster came back");

    fat_unmount();
    fclose(s_img);

    printf("\n%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
