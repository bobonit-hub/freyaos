/*
 * Freya - LittleFS on the Black Pill SPI NOR, mounted at /spiN.
 * The calls match the FAT ones so the shell and the descriptor table
 * do not grow a second path.
 */
#ifndef FREYA_LFSVOL_H
#define FREYA_LFSVOL_H

#include "fat.h"

int  lfsvol_mount(int *formatted);
void lfsvol_unmount(void);
int  lfsvol_mounted(void);
int  lfsvol_sync(void);
int  lfsvol_owns(const char *path);
int  lfsvol_used_blocks(uint32_t *used);

int  lfsvol_stat(const char *path, fat_dirent_t *e);
int  lfsvol_open(fat_file_t *f, const char *path, int flags);
int  lfsvol_read(fat_file_t *f, void *buf, uint32_t len, uint32_t *got);
int  lfsvol_write(fat_file_t *f, const void *buf, uint32_t len, uint32_t *put);
int  lfsvol_seek(fat_file_t *f, uint32_t pos);
int  lfsvol_close(fat_file_t *f);

int  lfsvol_opendir(fat_dir_t *d, const char *path);
int  lfsvol_readdir(fat_dir_t *d, fat_dirent_t *e);   /* 1 = end of dir */
int  lfsvol_closedir(fat_dir_t *d);

int  lfsvol_mkdir(const char *path);
int  lfsvol_unlink(const char *path);
int  lfsvol_rename(const char *src, const char *dst);

#endif
