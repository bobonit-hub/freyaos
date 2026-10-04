/*
 * Freya - the file calls in a build without the SD card.
 *
 * Built in place of src/sd.c and src/fat.c when make is not given SD=1.
 * There is no FAT volume at /, so nothing is ever mounted there.  On a
 * board with SPI flash a path under /spi<n> still reaches its LittleFS
 * volume, through the same calls the card would use; every other path
 * has no filesystem.  lfsvol.h stands in for LittleFS on a board
 * without SPI flash.  A build with USB=1 uses src/fat.c instead, for the
 * stick at /usb.
 */
#include "freya.h"
#include "fat.h"
#include "lfsvol.h"

#if defined(FREYA_SD) || defined(FREYA_USB)
#error "src/nosd.c is compiled only without SD=1 and USB=1"
#endif

int  fat_mount(void)   { return FAT_ERR_NOFS; }
void fat_unmount(void) { }
int  fat_mounted(void) { return 0; }

const char *fat_type_str(void) { return "none"; }

int fat_free_clusters(uint32_t *free_clus)
{
    (void)free_clus;
    return FAT_ERR_NOFS;
}

int fat_sync(void)
{
    if (lfsvol_mounted()) return lfsvol_sync();
    return FAT_OK;
}

int fat_open(fat_file_t *f, const char *path, int flags)
{
    if (lfsvol_owns(path)) return lfsvol_open(f, path, flags);
    return FAT_ERR_NOFS;
}

int fat_read(fat_file_t *f, void *buf, uint32_t len, uint32_t *got)
{
    if (!f->open || !(f->flags & (FAT_READ | FAT_WRITE))) return FAT_ERR_INVAL;
    if (f->dev & 0x80) return lfsvol_read(f, buf, len, got);
    return FAT_ERR_INVAL;
}

int fat_write(fat_file_t *f, const void *buf, uint32_t len, uint32_t *put)
{
    if (!f->open || !(f->flags & FAT_WRITE)) return FAT_ERR_INVAL;
    if (f->dev & 0x80) return lfsvol_write(f, buf, len, put);
    return FAT_ERR_INVAL;
}

int fat_seek(fat_file_t *f, uint32_t pos)
{
    if (!f->open) return FAT_ERR_INVAL;
    if (f->dev & 0x80) return lfsvol_seek(f, pos);
    return FAT_ERR_INVAL;
}

int fat_close(fat_file_t *f)
{
    if (!f->open) return FAT_ERR_INVAL;
    if (f->dev & 0x80) return lfsvol_close(f);
    f->open = 0;
    return FAT_ERR_INVAL;
}

int fat_opendir(fat_dir_t *d, const char *path)
{
    if (lfsvol_owns(path)) return lfsvol_opendir(d, path);
    return FAT_ERR_NOFS;
}

int fat_readdir(fat_dir_t *d, fat_dirent_t *e)
{
    if (!d->open) return FAT_ERR_INVAL;
    if (d->dev & 0x80) return lfsvol_readdir(d, e);
    return FAT_ERR_INVAL;
}

int fat_closedir(fat_dir_t *d)
{
    if (d->dev & 0x80) return lfsvol_closedir(d);
    d->open = 0;
    return FAT_OK;
}

int fat_stat(const char *path, fat_dirent_t *e)
{
    if (lfsvol_owns(path)) return lfsvol_stat(path, e);
    return FAT_ERR_NOFS;
}

int fat_mkdir(const char *path)
{
    if (lfsvol_owns(path)) return lfsvol_mkdir(path);
    return FAT_ERR_NOFS;
}

int fat_unlink(const char *path)
{
    if (lfsvol_owns(path)) return lfsvol_unlink(path);
    return FAT_ERR_NOFS;
}

int fat_rename(const char *src, const char *dst)
{
    if (lfsvol_owns(src) || lfsvol_owns(dst)) return lfsvol_rename(src, dst);
    return FAT_ERR_NOFS;
}
