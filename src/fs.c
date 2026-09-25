/*
 * Freya - path handling and the descriptor table shared by the shell and
 * by user programs.  The FAT layer only understands absolute paths, so
 * this is where the working directory lives.
 */
#include "freya.h"
#include "fat.h"

#define MAX_FILES   4
#define MAX_DIRS    2

static fat_file_t s_files[MAX_FILES];
static fat_dir_t  s_dirs[MAX_DIRS];
static char       s_cwd[FAT_MAX_PATH] = "/";

/*
 * A program's pin or timer handler runs in interrupt context and can
 * preempt the thread anywhere, including halfway through a FAT update.
 * None of this is reentrant, so a call from a handler is refused rather
 * than allowed to leave the card inconsistent.
 */
static int from_handler(void)
{
#ifdef FREYA_HOST
    return 0;
#else
    return app_in_handler();
#endif
}

const char *fs_cwd(void)
{
    return s_cwd;
}

/*
 * Turns 'in' into a clean absolute path: applies the working directory,
 * collapses "." and "..", and strips duplicate and trailing slashes.
 */
int fs_abspath(const char *in, char *out, int size)
{
    char tmp[FAT_MAX_PATH * 2];
    int len = 0, out_len = 0;

    if (in[0] == '/') {
        tmp[len++] = '/';
        in++;
    } else {
        const char *c = s_cwd;
        while (*c && len < (int)sizeof(tmp) - 1) tmp[len++] = *c++;
        if (len == 0 || tmp[len - 1] != '/') tmp[len++] = '/';
    }
    while (*in && len < (int)sizeof(tmp) - 1) tmp[len++] = *in++;
    tmp[len] = '\0';

    out[out_len++] = '/';
    for (int i = 0; i < len; ) {
        char comp[FAT_MAX_NAME];
        int n = 0;

        while (i < len && tmp[i] == '/') i++;
        while (i < len && tmp[i] != '/') {
            if (n < (int)sizeof(comp) - 1) comp[n++] = tmp[i];
            i++;
        }
        comp[n] = '\0';
        if (n == 0) continue;

        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            while (out_len > 1 && out[out_len - 1] != '/') out_len--;
            if (out_len > 1) out_len--;          /* drop the slash too */
            if (out_len == 0) out_len = 1;
            continue;
        }
        if (out_len > 1) {
            if (out_len >= size - 1) return -1;
            out[out_len++] = '/';
        }
        for (int k = 0; k < n; k++) {
            if (out_len >= size - 1) return -1;
            out[out_len++] = comp[k];
        }
    }
    out[out_len] = '\0';
    return 0;
}

int fs_chdir(const char *path)
{
    char abs[FAT_MAX_PATH];
    fat_dirent_t e;
    int rc;

    if (fs_abspath(path, abs, sizeof(abs)) != 0) return FAT_ERR_INVAL;
    rc = fat_stat(abs, &e);
    if (rc != FAT_OK) return rc;
    if (!(e.attr & FAT_ATTR_DIR)) return FAT_ERR_NOTDIR;

    strncpy(s_cwd, abs, sizeof(s_cwd) - 1);
    s_cwd[sizeof(s_cwd) - 1] = '\0';
    return FAT_OK;
}

/* ----------------------------------------------------------- file table */
int fs_fd_open(const char *path, int flags)
{
    char abs[FAT_MAX_PATH];
    int rc;

    if (from_handler()) return FAT_ERR_INVAL;
    if (fs_abspath(path, abs, sizeof(abs)) != 0) return FAT_ERR_INVAL;

    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i].open) continue;
        rc = fat_open(&s_files[i], abs, flags);
        if (rc != FAT_OK) return rc;
        return i;
    }
    return FAT_ERR_NOSPC;
}

static fat_file_t *fd_get(int fd)
{
    if (from_handler()) return NULL;
    if (fd < 0 || fd >= MAX_FILES || !s_files[fd].open) return NULL;
    return &s_files[fd];
}

int fs_fd_close(int fd)
{
    fat_file_t *f = fd_get(fd);
    if (!f) return FAT_ERR_INVAL;
    return fat_close(f);
}

int fs_fd_read(int fd, void *buf, int len)
{
    fat_file_t *f = fd_get(fd);
    uint32_t got = 0;
    int rc;

    if (!f || len < 0) return FAT_ERR_INVAL;
    rc = fat_read(f, buf, (uint32_t)len, &got);
    return (rc == FAT_OK) ? (int)got : rc;
}

int fs_fd_write(int fd, const void *buf, int len)
{
    fat_file_t *f = fd_get(fd);
    uint32_t put = 0;
    int rc;

    if (!f || len < 0) return FAT_ERR_INVAL;
    rc = fat_write(f, buf, (uint32_t)len, &put);
    return (rc == FAT_OK) ? (int)put : rc;
}

int fs_fd_seek(int fd, int32_t off, int whence)
{
    fat_file_t *f = fd_get(fd);
    int32_t base;

    if (!f) return FAT_ERR_INVAL;
    switch (whence) {
    case FREYA_SEEK_SET: base = 0; break;
    case FREYA_SEEK_CUR: base = (int32_t)f->pos; break;
    case FREYA_SEEK_END: base = (int32_t)f->size; break;
    default: return FAT_ERR_INVAL;
    }
    if (base + off < 0) return FAT_ERR_INVAL;
    return fat_seek(f, (uint32_t)(base + off));
}

int32_t fs_fd_tell(int fd)
{
    fat_file_t *f = fd_get(fd);
    return f ? (int32_t)f->pos : FAT_ERR_INVAL;
}

int32_t fs_fd_size(int fd)
{
    fat_file_t *f = fd_get(fd);
    return f ? (int32_t)f->size : FAT_ERR_INVAL;
}

void fs_close_all(void)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (s_files[i].open) fat_close(&s_files[i]);
    for (int i = 0; i < MAX_DIRS; i++) {
#ifdef FREYA_BOARD_BLACKPILL
        if (s_dirs[i].open) fat_closedir(&s_dirs[i]);
#else
        s_dirs[i].open = 0;
#endif
    }
}

/* ------------------------------------------------------ directory table */
int fs_dd_open(const char *path)
{
    char abs[FAT_MAX_PATH];
    int rc;

    if (from_handler()) return FAT_ERR_INVAL;
    if (fs_abspath(path, abs, sizeof(abs)) != 0) return FAT_ERR_INVAL;

    for (int i = 0; i < MAX_DIRS; i++) {
        if (s_dirs[i].open) continue;
        rc = fat_opendir(&s_dirs[i], abs);
        if (rc != FAT_OK) return rc;
        return i;
    }
    return FAT_ERR_NOSPC;
}

int fs_dd_read(int dd, freya_stat_t *st)
{
    fat_dirent_t e;
    int rc;

    if (from_handler()) return FAT_ERR_INVAL;
    if (dd < 0 || dd >= MAX_DIRS || !s_dirs[dd].open) return FAT_ERR_INVAL;

    rc = fat_readdir(&s_dirs[dd], &e);
    if (rc != 0) return rc;                 /* 1 = end of directory */

    strncpy(st->name, e.name, sizeof(st->name) - 1);
    st->name[sizeof(st->name) - 1] = '\0';
    st->size   = e.size;
    st->is_dir = (e.attr & FAT_ATTR_DIR) ? 1 : 0;
    return 0;
}

int fs_dd_close(int dd)
{
    if (dd < 0 || dd >= MAX_DIRS) return FAT_ERR_INVAL;
    s_dirs[dd].open = 0;
    return FAT_OK;
}

int fs_rename(const char *old_path, const char *new_path)
{
    char src[FAT_MAX_PATH], dst[FAT_MAX_PATH];
    int rc, n;

    if (from_handler()) return FAT_ERR_INVAL;
    if (fs_abspath(old_path, src, sizeof(src)) != 0) return FAT_ERR_INVAL;
    if (fs_abspath(new_path, dst, sizeof(dst)) != 0) return FAT_ERR_INVAL;

    rc = fat_rename(src, dst);
    if (rc != FAT_OK) return rc;

    n = (int)strlen(src);
    if (strncmp(s_cwd, src, (size_t)n) == 0 &&
        (s_cwd[n] == '\0' || s_cwd[n] == '/')) {
        char rebuilt[FAT_MAX_PATH];
        if (ksnprintf(rebuilt, sizeof(rebuilt), "%s%s", dst, s_cwd + n)
            < (int)sizeof(rebuilt)) {
            strncpy(s_cwd, rebuilt, sizeof(s_cwd) - 1);
            s_cwd[sizeof(s_cwd) - 1] = '\0';
        }
    }
    return FAT_OK;
}
