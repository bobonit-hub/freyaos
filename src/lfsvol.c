/*
 * Freya - LittleFS volume on the SPI NOR.
 *
 * A blank chip (erased to 0xFF) is formatted on the first mount.  A chip
 * that already holds LittleFS is mounted as it is.  Anything else is left
 * alone and the mount fails, so a foreign image is not wiped.
 */
#include "freya.h"

#if defined(FREYA_BOARD_BLACKPILL)

#include "lfsvol.h"
#include "lfs.h"

#define LFS_CACHE   256
#define LFS_BLOCK   4096
#define LFS_FILES   4
#define LFS_DIRS    2

static lfs_t s_lfs;
static struct lfs_config s_cfg;
static uint8_t s_read_buf[LFS_CACHE];
static uint8_t s_prog_buf[LFS_CACHE];
static uint8_t s_lookahead[32];
static uint8_t s_on;

static struct {
    uint8_t used;
    lfs_file_t file;
    struct lfs_file_config cfg;
    uint8_t buf[LFS_CACHE];
} s_file[LFS_FILES];

static struct {
    uint8_t used;
    lfs_dir_t dir;
} s_dir[LFS_DIRS];

static int map_err(int err)
{
    switch (err) {
    case LFS_ERR_OK:          return FAT_OK;
    case LFS_ERR_NOENT:       return FAT_ERR_NOENT;
    case LFS_ERR_EXIST:       return FAT_ERR_EXIST;
    case LFS_ERR_NOTDIR:      return FAT_ERR_NOTDIR;
    case LFS_ERR_ISDIR:       return FAT_ERR_ISDIR;
    case LFS_ERR_NOTEMPTY:    return FAT_ERR_NOTEMPTY;
    case LFS_ERR_NOSPC:       return FAT_ERR_NOSPC;
    case LFS_ERR_NOMEM:       return FAT_ERR_NOSPC;
    case LFS_ERR_INVAL:
    case LFS_ERR_NAMETOOLONG:
    case LFS_ERR_FBIG:
    case LFS_ERR_BADF:        return FAT_ERR_INVAL;
    default:                  return FAT_ERR_IO;
    }
}

static int bd_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buf, lfs_size_t size)
{
    uint32_t addr = block * c->block_size + off;
    (void)c;
    return spiflash_bd_read(addr, buf, size) == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buf, lfs_size_t size)
{
    uint32_t addr = block * c->block_size + off;
    (void)c;
    return spiflash_bd_prog(addr, buf, size) == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    uint32_t addr = block * c->block_size;
    (void)c;
    return spiflash_bd_erase(addr) == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int bd_sync(const struct lfs_config *c)
{
    (void)c;
    spiflash_bd_sync();
    return LFS_ERR_OK;
}

static void cfg_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.read = bd_read;
    s_cfg.prog = bd_prog;
    s_cfg.erase = bd_erase;
    s_cfg.sync = bd_sync;
    s_cfg.read_size = 256;
    s_cfg.prog_size = 256;
    s_cfg.block_size = LFS_BLOCK;
    s_cfg.block_count = spiflash_bytes() / LFS_BLOCK;
    s_cfg.cache_size = LFS_CACHE;
    s_cfg.lookahead_size = sizeof(s_lookahead);
    s_cfg.block_cycles = 500;
    s_cfg.read_buffer = s_read_buf;
    s_cfg.prog_buffer = s_prog_buf;
    s_cfg.lookahead_buffer = s_lookahead;
    s_cfg.name_max = 63;
}

static int to_local(const char *path, char *local, int size)
{
    char prefix[8];
    int n;

    n = ksnprintf(prefix, sizeof(prefix), "/spi%d", spiflash_bus());
    if (n < 0 || n >= (int)sizeof(prefix)) return FAT_ERR_INVAL;
    if (path[n] == '\0') {
        if (size < 2) return FAT_ERR_INVAL;
        local[0] = '/';
        local[1] = '\0';
        return FAT_OK;
    }
    if (path[n] != '/') return FAT_ERR_INVAL;
    strncpy(local, path + n, (size_t)size - 1);
    local[size - 1] = '\0';
    return FAT_OK;
}

int lfsvol_owns(const char *path)
{
    char prefix[8];
    int n;

    if (!s_on || !path) return 0;
    n = ksnprintf(prefix, sizeof(prefix), "/spi%d", spiflash_bus());
    if (n < 0 || strncmp(path, prefix, (size_t)n) != 0) return 0;
    return path[n] == '\0' || path[n] == '/';
}

int lfsvol_mounted(void) { return s_on; }

int lfsvol_mount(int *formatted)
{
    int err;

    if (formatted) *formatted = 0;
    if (s_on) return FAT_OK;
    if (spiflash_bytes() < LFS_BLOCK * 2) return FAT_ERR_IO;
    cfg_init();
    err = lfs_mount(&s_lfs, &s_cfg);
    if (err) {
        if (!spiflash_bd_blank()) return map_err(err);
        err = lfs_format(&s_lfs, &s_cfg);
        if (err) return map_err(err);
        err = lfs_mount(&s_lfs, &s_cfg);
        if (err) return map_err(err);
        if (formatted) *formatted = 1;
    }
    s_on = 1;
    return FAT_OK;
}

void lfsvol_unmount(void)
{
    int i;

    if (!s_on) return;
    for (i = 0; i < LFS_FILES; i++) {
        if (!s_file[i].used) continue;
        (void)lfs_file_close(&s_lfs, &s_file[i].file);
        s_file[i].used = 0;
    }
    for (i = 0; i < LFS_DIRS; i++) {
        if (!s_dir[i].used) continue;
        (void)lfs_dir_close(&s_lfs, &s_dir[i].dir);
        s_dir[i].used = 0;
    }
    (void)lfs_unmount(&s_lfs);
    s_on = 0;
}

int lfsvol_sync(void)
{
    int i, err = LFS_ERR_OK;

    if (!s_on) return FAT_OK;
    for (i = 0; i < LFS_FILES; i++) {
        if (!s_file[i].used) continue;
        err = lfs_file_sync(&s_lfs, &s_file[i].file);
        if (err) return map_err(err);
    }
    spiflash_bd_sync();
    return FAT_OK;
}

int lfsvol_used_blocks(uint32_t *used)
{
    lfs_ssize_t n;

    if (!s_on) return FAT_ERR_NOFS;
    n = lfs_fs_size(&s_lfs);
    if (n < 0) return map_err((int)n);
    *used = (uint32_t)n;
    return FAT_OK;
}

static void fill_ent(fat_dirent_t *e, const struct lfs_info *info)
{
    if (!e) return;
    memset(e, 0, sizeof(*e));
    strncpy(e->name, info->name, FAT_MAX_NAME - 1);
    e->size = info->size;
    if (info->type == LFS_TYPE_DIR) e->attr = FAT_ATTR_DIR;
    else e->attr = FAT_ATTR_ARCHIVE;
}

int lfsvol_stat(const char *path, fat_dirent_t *e)
{
    char local[FAT_MAX_PATH];
    struct lfs_info info;
    int rc, err;

    if (!s_on) return FAT_ERR_NOFS;
    rc = to_local(path, local, sizeof(local));
    if (rc != FAT_OK) return rc;
    err = lfs_stat(&s_lfs, local, &info);
    if (err) return map_err(err);
    if (local[0] == '/' && local[1] == '\0') {
        strncpy(info.name, "/", FAT_MAX_NAME - 1);
        info.type = LFS_TYPE_DIR;
        info.size = 0;
    }
    fill_ent(e, &info);
    return FAT_OK;
}

static int open_flags(int flags)
{
    int out;

    if ((flags & FAT_READ) && (flags & FAT_WRITE)) out = LFS_O_RDWR;
    else if (flags & FAT_WRITE) out = LFS_O_WRONLY;
    else out = LFS_O_RDONLY;
    if (flags & FAT_CREATE) out |= LFS_O_CREAT;
    if (flags & FAT_TRUNC)  out |= LFS_O_TRUNC;
    if (flags & FAT_APPEND) out |= LFS_O_APPEND;
    return out;
}

int lfsvol_open(fat_file_t *f, const char *path, int flags)
{
    char local[FAT_MAX_PATH];
    int rc, err, slot, mode;
    lfs_soff_t sz;

    if (!s_on) return FAT_ERR_NOFS;
    rc = to_local(path, local, sizeof(local));
    if (rc != FAT_OK) return rc;
    if (local[0] == '/' && local[1] == '\0') return FAT_ERR_ISDIR;

    slot = -1;
    for (int i = 0; i < LFS_FILES; i++) {
        if (!s_file[i].used) { slot = i; break; }
    }
    if (slot < 0) return FAT_ERR_NOSPC;

    memset(&s_file[slot].cfg, 0, sizeof(s_file[slot].cfg));
    s_file[slot].cfg.buffer = s_file[slot].buf;
    mode = open_flags(flags);
    err = lfs_file_opencfg(&s_lfs, &s_file[slot].file, local, mode,
                           &s_file[slot].cfg);
    if (err) return map_err(err);

    sz = lfs_file_size(&s_lfs, &s_file[slot].file);
    if (sz < 0) {
        (void)lfs_file_close(&s_lfs, &s_file[slot].file);
        return map_err((int)sz);
    }
    memset(f, 0, sizeof(*f));
    f->open = 1;
    f->flags = (uint8_t)flags;
    f->dev = (uint8_t)(0x80 | slot);
    f->size = (uint32_t)sz;
    f->pos = (uint32_t)lfs_file_tell(&s_lfs, &s_file[slot].file);
    s_file[slot].used = 1;
    return FAT_OK;
}

int lfsvol_read(fat_file_t *f, void *buf, uint32_t len, uint32_t *got)
{
    lfs_ssize_t n;

    if (!f->open || (f->dev & 0x80) == 0 || !s_file[f->dev & 0x7f].used) return FAT_ERR_INVAL;
    if (!(f->flags & (FAT_READ | FAT_WRITE))) return FAT_ERR_INVAL;
    n = lfs_file_read(&s_lfs, &s_file[f->dev & 0x7f].file, buf, len);
    if (n < 0) return map_err((int)n);
    f->pos = (uint32_t)lfs_file_tell(&s_lfs, &s_file[f->dev & 0x7f].file);
    if (got) *got = (uint32_t)n;
    return FAT_OK;
}

int lfsvol_write(fat_file_t *f, const void *buf, uint32_t len, uint32_t *put)
{
    lfs_ssize_t n;
    lfs_soff_t sz;

    if (!f->open || (f->dev & 0x80) == 0 || !s_file[f->dev & 0x7f].used) return FAT_ERR_INVAL;
    if (!(f->flags & FAT_WRITE)) return FAT_ERR_INVAL;
    n = lfs_file_write(&s_lfs, &s_file[f->dev & 0x7f].file, buf, len);
    if (n < 0) return map_err((int)n);
    sz = lfs_file_size(&s_lfs, &s_file[f->dev & 0x7f].file);
    f->pos = (uint32_t)lfs_file_tell(&s_lfs, &s_file[f->dev & 0x7f].file);
    if (sz >= 0) f->size = (uint32_t)sz;
    if (put) *put = (uint32_t)n;
    return ((uint32_t)n == len) ? FAT_OK : FAT_ERR_NOSPC;
}

int lfsvol_seek(fat_file_t *f, uint32_t pos)
{
    lfs_soff_t n;

    if (!f->open || (f->dev & 0x80) == 0 || !s_file[f->dev & 0x7f].used) return FAT_ERR_INVAL;
    if (pos > f->size) pos = f->size;
    n = lfs_file_seek(&s_lfs, &s_file[f->dev & 0x7f].file, (lfs_soff_t)pos, LFS_SEEK_SET);
    if (n < 0) return map_err((int)n);
    f->pos = (uint32_t)n;
    return FAT_OK;
}

int lfsvol_close(fat_file_t *f)
{
    int err;

    if (!f->open || (f->dev & 0x80) == 0) return FAT_ERR_INVAL;
    if ((f->dev & 0x7f) >= LFS_FILES || !s_file[f->dev & 0x7f].used) return FAT_ERR_INVAL;
    err = lfs_file_close(&s_lfs, &s_file[f->dev & 0x7f].file);
    s_file[f->dev & 0x7f].used = 0;
    f->open = 0;
    return err ? map_err(err) : FAT_OK;
}

int lfsvol_opendir(fat_dir_t *d, const char *path)
{
    char local[FAT_MAX_PATH];
    int rc, err, slot;

    if (!s_on) return FAT_ERR_NOFS;
    rc = to_local(path, local, sizeof(local));
    if (rc != FAT_OK) return rc;
    slot = -1;
    for (int i = 0; i < LFS_DIRS; i++) {
        if (!s_dir[i].used) { slot = i; break; }
    }
    if (slot < 0) return FAT_ERR_NOSPC;
    err = lfs_dir_open(&s_lfs, &s_dir[slot].dir, local);
    if (err) return map_err(err);
    memset(d, 0, sizeof(*d));
    d->open = 1;
    d->dev = (uint8_t)(0x80 | slot);
    s_dir[slot].used = 1;
    return FAT_OK;
}

int lfsvol_readdir(fat_dir_t *d, fat_dirent_t *e)
{
    struct lfs_info info;
    int err;

    if (!d->open || (d->dev & 0x80) == 0 || (d->dev & 0x7f) >= LFS_DIRS ||
        !s_dir[d->dev & 0x7f].used)
        return FAT_ERR_INVAL;
    err = lfs_dir_read(&s_lfs, &s_dir[d->dev & 0x7f].dir, &info);
    if (err < 0) return map_err(err);
    if (err == 0) return 1;
    fill_ent(e, &info);
    return 0;
}

int lfsvol_closedir(fat_dir_t *d)
{
    int err = 0;

    if (!d->open || (d->dev & 0x80) == 0) {
        d->open = 0;
        return FAT_OK;
    }
    if ((d->dev & 0x7f) < LFS_DIRS && s_dir[d->dev & 0x7f].used) {
        err = lfs_dir_close(&s_lfs, &s_dir[d->dev & 0x7f].dir);
        s_dir[d->dev & 0x7f].used = 0;
    }
    d->open = 0;
    return err ? map_err(err) : FAT_OK;
}

int lfsvol_mkdir(const char *path)
{
    char local[FAT_MAX_PATH];
    int rc, err;

    if (!s_on) return FAT_ERR_NOFS;
    rc = to_local(path, local, sizeof(local));
    if (rc != FAT_OK) return rc;
    if (local[0] == '/' && local[1] == '\0') return FAT_ERR_EXIST;
    err = lfs_mkdir(&s_lfs, local);
    return err ? map_err(err) : FAT_OK;
}

int lfsvol_unlink(const char *path)
{
    char local[FAT_MAX_PATH];
    int rc, err;

    if (!s_on) return FAT_ERR_NOFS;
    rc = to_local(path, local, sizeof(local));
    if (rc != FAT_OK) return rc;
    if (local[0] == '/' && local[1] == '\0') return FAT_ERR_INVAL;
    err = lfs_remove(&s_lfs, local);
    return err ? map_err(err) : FAT_OK;
}

int lfsvol_rename(const char *src, const char *dst)
{
    char a[FAT_MAX_PATH], b[FAT_MAX_PATH];
    int rc, err;

    if (!s_on) return FAT_ERR_NOFS;
    if (!lfsvol_owns(src) || !lfsvol_owns(dst)) return FAT_ERR_INVAL;
    rc = to_local(src, a, sizeof(a));
    if (rc != FAT_OK) return rc;
    rc = to_local(dst, b, sizeof(b));
    if (rc != FAT_OK) return rc;
    if (strcmp(a, b) == 0) return FAT_OK;
    if ((a[0] == '/' && a[1] == '\0') || (b[0] == '/' && b[1] == '\0'))
        return FAT_ERR_INVAL;
    err = lfs_rename(&s_lfs, a, b);
    return err ? map_err(err) : FAT_OK;
}

#endif /* FREYA_BOARD_BLACKPILL */
