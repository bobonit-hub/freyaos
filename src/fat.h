/*
 * Freya - FAT16 / FAT32 filesystem.
 *
 * Reads and writes files, creates and removes directories, understands
 * MBR partitioned and "superfloppy" cards, reads VFAT long file names and
 * creates them when a name does not fit the classic 8.3 form.
 */
#ifndef FREYA_FAT_H
#define FREYA_FAT_H

#include <stdint.h>

#define FAT_MAX_NAME        64
#define FAT_MAX_PATH        128

enum {
    FAT_OK          =  0,
    FAT_ERR_IO      = -1,
    FAT_ERR_NOFS    = -2,
    FAT_ERR_NOENT   = -3,
    FAT_ERR_EXIST   = -4,
    FAT_ERR_NOSPC   = -5,
    FAT_ERR_INVAL   = -6,
    FAT_ERR_NOTDIR  = -7,
    FAT_ERR_ISDIR   = -8,
    FAT_ERR_NOTEMPTY= -9,
    FAT_ERR_NOFILE  = -10,
    FAT_ERR_RDONLY  = -11
};

#define FAT_ATTR_RDONLY     0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME     0x08
#define FAT_ATTR_DIR        0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

/* fat_open() modes - mirror the values in freya_api.h */
#define FAT_READ            0x01
#define FAT_WRITE           0x02
#define FAT_CREATE          0x04
#define FAT_TRUNC           0x08
#define FAT_APPEND          0x10

typedef struct {
    uint8_t  mounted;
    uint8_t  type;              /* 16 or 32                            */
    uint8_t  sec_per_clus;
    uint8_t  num_fats;
    uint16_t root_ent_cnt;      /* FAT16 only                          */
    uint16_t rsvd_sec;
    uint32_t fat_size;          /* sectors per FAT                     */
    uint32_t part_lba;          /* first sector of the volume          */
    uint32_t tot_sec;
    uint32_t fat_start;         /* LBA of FAT #0                       */
    uint32_t root_start;        /* LBA of the FAT16 root directory     */
    uint32_t root_sectors;
    uint32_t data_start;        /* LBA of cluster 2                    */
    uint32_t clus_count;        /* number of data clusters             */
    uint32_t root_clus;         /* FAT32 root directory cluster        */
    uint32_t next_free;         /* allocation hint                     */
    uint32_t bytes_per_clus;
    uint32_t fsinfo_lba;        /* FAT32 FSInfo sector, 0 if absent    */
    uint32_t free_count;        /* free clusters, when free_valid      */
    uint8_t  free_valid;
    uint8_t  fsinfo_dirty;
    char     label[12];
} fat_fs_t;

extern fat_fs_t g_fs;

/* Directory scanner - also used internally. */
typedef struct {
    uint32_t clus;
    uint32_t lba;
    uint32_t sec_in_clus;
    uint32_t root_sec_left;
    uint32_t idx;               /* 0..15, entry inside the sector      */
    uint8_t  fixed_root;
    uint8_t  ended;
} fat_scan_t;

typedef struct {
    char     name[FAT_MAX_NAME];
    uint8_t  attr;
    uint32_t size;
    uint32_t clus;
    uint16_t wdate;
    uint16_t wtime;
} fat_dirent_t;

typedef struct {
    fat_scan_t scan;
    uint8_t    open;
} fat_dir_t;

typedef struct {
    uint8_t  open;
    uint8_t  flags;
    uint8_t  dirty;
    uint32_t first_clus;
    uint32_t size;
    uint32_t pos;
    uint32_t cur_clus;
    uint32_t cur_clus_idx;      /* index of cur_clus in the chain      */
    uint32_t dir_lba;           /* location of the 8.3 directory entry */
    uint16_t dir_off;
} fat_file_t;

int  fat_mount(void);
void fat_unmount(void);
int  fat_mounted(void);
const char *fat_err_str(int err);
const char *fat_type_str(void);
int  fat_free_clusters(uint32_t *free_clus);
int  fat_sync(void);

int  fat_open(fat_file_t *f, const char *path, int flags);
int  fat_read(fat_file_t *f, void *buf, uint32_t len, uint32_t *got);
int  fat_write(fat_file_t *f, const void *buf, uint32_t len, uint32_t *put);
int  fat_seek(fat_file_t *f, uint32_t pos);
int  fat_close(fat_file_t *f);

int  fat_opendir(fat_dir_t *d, const char *path);
int  fat_readdir(fat_dir_t *d, fat_dirent_t *e);   /* 1 = end of dir */
int  fat_closedir(fat_dir_t *d);

int  fat_stat(const char *path, fat_dirent_t *e);
int  fat_mkdir(const char *path);
int  fat_unlink(const char *path);                 /* file or empty dir */

#endif /* FREYA_FAT_H */
