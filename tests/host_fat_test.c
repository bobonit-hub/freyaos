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
void rtc_get(rtc_time_t *t)
{
    t->year = 2026;
    t->mon = 9;
    t->day = 21;
    t->hour = 20;
    t->min = 30;
    t->sec = 0;
}

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

    printf("\nrename\n");
    {
        fat_file_t f;
        uint8_t buf[32];
        uint32_t n, free_a = 0, free_b = 0;

        check_rc(fat_open(&f, "/docs/to_move.txt", FAT_WRITE | FAT_CREATE | FAT_TRUNC),
                 "create a file to rename");
        check_rc(fat_write(&f, "rename-payload", 14, &n), "  write");
        check_rc(fat_close(&f), "  close");

        check_rc(fat_free_clusters(&free_a), "free clusters before rename");
        check_rc(fat_rename("/docs/to_move.txt", "/docs/moved.txt"),
                 "rename in the same directory");
        check(!exists("/docs/to_move.txt") && exists("/docs/moved.txt"),
              "  old name gone, new name present");
        check_rc(fat_open(&f, "/docs/moved.txt", FAT_READ), "  reopen under the new name");
        fat_read(&f, buf, sizeof(buf), &n);
        check(n == 14 && !memcmp(buf, "rename-payload", 14), "  content survived");
        fat_close(&f);

        check_rc(fat_rename("/docs/moved.txt", "/docs/sub/moved.txt"),
                 "move into a subdirectory");
        check(!exists("/docs/moved.txt") && exists("/docs/sub/moved.txt"),
              "  it lives in the subdirectory");

        check(fat_rename("/docs/sub/moved.txt", "/docs/small.txt") == FAT_ERR_EXIST,
              "refuses to overwrite an existing name");
        check(fat_rename("/docs/nope.txt", "/docs/x.txt") == FAT_ERR_NOENT,
              "renaming a missing file fails");
        check(fat_rename("/docs", "/docs/sub/trap") == FAT_ERR_INVAL,
              "refuses to move a directory into itself");
        check(fat_rename("/", "/elsewhere") == FAT_ERR_INVAL, "refuses to rename the root");

        check_rc(fat_rename("/docs/sub", "/docs/folder"), "rename a directory with children");
        check(exists("/docs/folder/deep.dat") && exists("/docs/folder/moved.txt"),
              "  children follow the directory");
        check(!exists("/docs/sub"), "  the old directory name is gone");
        check_rc(fat_rename("/docs/folder", "/docs/sub"), "  rename it back");
        check(exists("/docs/sub/deep.dat"), "  children still resolve");

        check_rc(fat_mkdir("/bin/place"), "mkdir a destination for a directory move");
        check_rc(fat_rename("/docs/sub", "/bin/place/sub"),
                 "move a directory to another parent");
        check(exists("/bin/place/sub/deep.dat") && !exists("/docs/sub"),
              "  the tree is reachable from the new parent");
        check_rc(fat_rename("/bin/place/sub", "/docs/sub"), "  move the tree back");
        check_rc(fat_unlink("/bin/place"), "  remove the empty destination directory");

        {
            const char *lfn_src = "/docs/A Rather Long File Name.text";
            const char *lfn_dst = "/docs/Renamed Long File.text";
            check_rc(fat_rename(lfn_src, lfn_dst), "rename a long name");
            check(exists(lfn_dst) && !exists(lfn_src), "  long name moved");
            check_rc(fat_rename(lfn_dst, lfn_src), "  restore the long name");
        }

        check_rc(fat_rename("/docs/sub/moved.txt", "/bin/moved.txt"),
                 "move a file across directories");
        check(exists("/bin/moved.txt") && !exists("/docs/sub/moved.txt"),
              "  it left the old directory");

        check_rc(fat_free_clusters(&free_b), "free clusters after rename");
        check(free_a == free_b, "rename did not allocate or free data clusters");
        check_rc(fat_rename("/docs/small.txt", "/docs/small.txt"),
                 "renaming to the same path is a no-op");
        check_rc(fat_unlink("/bin/moved.txt"), "remove the extra renamed file");
    }

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

    printf("\nfile log\n");
    {
        fat_file_t f;
        fat_dirent_t e;
        uint8_t head[40];
        uint32_t n, log_sz;
        uint32_t before;

        log_init();
        check(log_get_level() == FREYA_LOG_INFO, "default log level is info");
        check(log_set_level(FREYA_LOG_DEBUG) == 0, "set log level to debug");
        check(log_get_level() == FREYA_LOG_DEBUG, "  it stuck");
        check(log_set_level(FREYA_LOG_INFO) == 0, "set log level back to info");

        log_sz = FREYA_LOG_MAX_SIZE;
        test_write_read(FREYA_LOG_PATH, log_sz);
        check(exists(FREYA_LOG_PATH) && !exists(FREYA_LOG_OLD_PATH),
              "log file is at the volume root, no old copy yet");

        klog(FREYA_LOG_INFO, "after rotate %u", 1);
        check(exists(FREYA_LOG_OLD_PATH), "over-size log was renamed to the old name");
        check(fat_stat(FREYA_LOG_OLD_PATH, &e) == FAT_OK && e.size == log_sz,
              "  the old file kept its bytes");
        check(fat_stat(FREYA_LOG_PATH, &e) == FAT_OK && e.size > 0 && e.size < log_sz,
              "  a new log was created");

        check_rc(fat_open(&f, FREYA_LOG_PATH, FAT_READ), "read the new log");
        check_rc(fat_read(&f, head, sizeof(head), &n), "  first bytes");
        fat_close(&f);
        check(n >= 27 && !memcmp(head, "2026-09-21 20:30:00 INFO ", 25),
              "  line starts with datetime and level");

        before = e.size;
        klog(FREYA_LOG_DEBUG, "too quiet");
        check(fat_stat(FREYA_LOG_PATH, &e) == FAT_OK && e.size == before,
              "debug is filtered at info");

        check(log_set_level(FREYA_LOG_OFF) == 0, "set log level to off");
        klog(FREYA_LOG_ERROR, "silent");
        check(fat_stat(FREYA_LOG_PATH, &e) == FAT_OK && e.size == before,
              "nothing is written when the level is off");

        check_rc(fat_unlink(FREYA_LOG_PATH), "remove the new log");
        check_rc(fat_unlink(FREYA_LOG_OLD_PATH), "remove the old log");
        check_rc(fat_free_clusters(&free_after), "count free clusters after log rotate");
        check(free_before == free_after, "log rotate returned every cluster");

        fat_unmount();
        check(log_set_level(FREYA_LOG_INFO) == 0, "level can still be set with no volume");
        klog(FREYA_LOG_INFO, "no card");
        check_rc(fat_mount(), "remount after the no-card stub");
        check(!exists(FREYA_LOG_PATH) && !exists(FREYA_LOG_OLD_PATH),
              "stub log did not create a file on the card");
        check_rc(fat_free_clusters(&free_after), "count free clusters after stub log");
        check(free_before == free_after, "stub log allocated no clusters");
    }

    fat_unmount();
    fclose(s_img);

    printf("\n%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
