/*
 * Freya - the shell language on Linux.
 *
 * src/shell.c built as an ordinary program.  This file is what the
 * kernel gives the shell on a board: the console, the clock, the heap,
 * the file calls and the log.  The console is the terminal, put in raw
 * mode while the shell owns it, so line editing, the cursor-key history
 * and Ctrl-C are the shell's own, the way they are over the UART.  Files
 * are the host's, relative to the process's working directory.  run()
 * starts a program with fork() and execvp().
 *
 *   fsh                  an interactive prompt (a script, if stdin is not
 *                        a terminal)
 *   fsh <file>           run a script file
 *   fsh -c <text>        run the text
 */
#define _GNU_SOURCE
/* Before the C library's headers: glibc may define strchr() and its
 * kin as macros, which src/freya.h's own declarations of them cannot
 * follow. */
#include "freya.h"
#include "fat.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

/* The shell reads these; nothing on Linux sets them. */
app_state_t g_app;

/* ------------------------------------------------------------ console */
/*
 * Output is buffered and flushed before anything waits: a read, a sleep,
 * a program started by run(), the exit.  The shell ends its lines with
 * "\r\n" for the UART; a "\r" right before "\n" is dropped, so a pipe or
 * a file gets plain lines and the terminal's own output processing adds
 * the carriage return.
 */
static char s_out[4096];
static int  s_outn;
static int  s_cr;                   /* a '\r' not yet written           */

static void out_flush(void)
{
    int done = 0;

    while (done < s_outn) {
        ssize_t n = write(STDOUT_FILENO, s_out + done, (size_t)(s_outn - done));

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (int)n;
    }
    s_outn = 0;
}

static void out_byte(char c)
{
    if (s_outn == (int)sizeof s_out) out_flush();
    s_out[s_outn++] = c;
}

void uart_putc(char c)
{
    if (s_cr) {
        s_cr = 0;
        if (c != '\n') out_byte('\r');
    }
    if (c == '\r') {
        s_cr = 1;
        return;
    }
    out_byte(c);
}

void uart_puts(const char *s)
{
    while (*s) uart_putc(*s++);
}

void uart_write(const void *buf, int len)
{
    const char *p = buf;

    for (int i = 0; i < len; i++) uart_putc(p[i]);
}

static void console_flush(void)
{
    if (s_cr) {
        s_cr = 0;
        out_byte('\r');
    }
    out_flush();
}

/*
 * Input.  On a terminal the shell sees every key, Ctrl-C included, as a
 * byte: the terminal is raw (no echo, no line buffering, no signals)
 * while the shell runs and back in its own mode while a program started
 * by run() has it, or after the exit.  Keys are queued as they are read,
 * so a Ctrl-C typed while a script runs is found among them, the way
 * the console interrupt queues it on the board.
 */
#define CTRL_C  0x03
#define CTRL_D  0x04

static struct termios s_term_saved;
static int  s_term_raw;
static int  s_interactive;
static volatile sig_atomic_t s_sigint;

static unsigned char s_rx[256];
static int  s_rx_head, s_rx_tail;
static int  s_rx_eof;

static void term_raw(void)
{
    struct termios t;

    if (!s_interactive || s_term_raw) return;
    t = s_term_saved;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | IGNCR);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == 0) s_term_raw = 1;
}

static void term_restore(void)
{
    if (!s_term_raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_term_saved);
    s_term_raw = 0;
}

static void on_sigint(int sig)
{
    (void)sig;
    s_sigint = 1;
}

static void rx_push(unsigned char c)
{
    int next = (s_rx_head + 1) % (int)sizeof s_rx;

    if (next == s_rx_tail) return;      /* full: the key is lost */
    s_rx[s_rx_head] = c;
    s_rx_head = next;
}

/* Queue whatever the terminal has, waiting up to ms (-1 for ever) for
 * the first key.  0 when something arrived or input has ended. */
static int rx_fill(int ms)
{
    struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
    unsigned char buf[64];
    ssize_t n;
    int r;

    if (s_rx_eof) return 0;
    if (ms != 0) console_flush();
    r = poll(&p, 1, ms);
    if (r < 0) return errno == EINTR ? -1 : (s_rx_eof = 1, 0);
    if (r == 0) return -1;
    n = read(STDIN_FILENO, buf, sizeof buf);
    if (n < 0) return errno == EINTR || errno == EAGAIN ? -1 : (s_rx_eof = 1, 0);
    if (n == 0) {
        s_rx_eof = 1;
        return 0;
    }
    for (ssize_t i = 0; i < n; i++) rx_push(buf[i]);
    return 0;
}

static int rx_pop(void)
{
    int c;

    if (s_rx_tail == s_rx_head) return s_rx_eof ? CTRL_D : -1;
    c = s_rx[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) % (int)sizeof s_rx;
    return c;
}

/* A SIGINT that arrived without a terminal key, from kill or from a
 * program's Ctrl-C, reads as the key. */
static void sigint_to_key(void)
{
    if (!s_sigint) return;
    s_sigint = 0;
    rx_push(CTRL_C);
}

int uart_getc(void)
{
    for (;;) {
        int c;

        sigint_to_key();
        c = rx_pop();
        if (c >= 0) return c;
        rx_fill(-1);
    }
}

int uart_getc_timeout(uint32_t ms)
{
    struct timespec t0, t;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        long spent;
        int c;

        sigint_to_key();
        c = rx_pop();
        if (c >= 0) return c;
        clock_gettime(CLOCK_MONOTONIC, &t);
        spent = (t.tv_sec - t0.tv_sec) * 1000L + (t.tv_nsec - t0.tv_nsec) / 1000000L;
        if (spent >= (long)ms) return -1;
        rx_fill((int)((long)ms - spent));
    }
}

int uart_getc_nb(void)
{
    sigint_to_key();
    if (s_rx_tail == s_rx_head && s_interactive) rx_fill(0);
    if (s_rx_tail == s_rx_head) return -1;
    return rx_pop();
}

/* 1 when a Ctrl-C is waiting.  Everything typed before it goes too. */
int uart_take_ctrlc(void)
{
    sigint_to_key();
    if (s_interactive) rx_fill(0);
    for (int i = s_rx_tail; i != s_rx_head; i = (i + 1) % (int)sizeof s_rx) {
        if (s_rx[i] == CTRL_C) {
            s_rx_tail = s_rx_head;
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- time */
static struct timespec s_start;
static time_t s_rtc_offset;         /* date("...") minus the host clock */

uint32_t sys_uptime_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - s_start.tv_sec) * 1000L +
                      (t.tv_nsec - s_start.tv_nsec) / 1000000L);
}

void sys_delay_ms(uint32_t ms)
{
    struct timespec t = { .tv_sec = ms / 1000U,
                          .tv_nsec = (long)(ms % 1000U) * 1000000L };

    console_flush();
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
        if (s_sigint) break;
    }
}

/* The local time.  Setting it with date() moves this process's clock
 * only, the way the board's clock is not battery backed. */
void rtc_get(rtc_time_t *t)
{
    time_t now = time(NULL) + s_rtc_offset;
    struct tm tm;

    localtime_r(&now, &tm);
    t->year = (uint16_t)(tm.tm_year + 1900);
    t->mon = (uint8_t)(tm.tm_mon + 1);
    t->day = (uint8_t)tm.tm_mday;
    t->hour = (uint8_t)tm.tm_hour;
    t->min = (uint8_t)tm.tm_min;
    t->sec = (uint8_t)tm.tm_sec;
}

void rtc_set(const rtc_time_t *t)
{
    struct tm tm;

    memset(&tm, 0, sizeof tm);
    tm.tm_year = t->year - 1900;
    tm.tm_mon = t->mon - 1;
    tm.tm_mday = t->day;
    tm.tm_hour = t->hour;
    tm.tm_min = t->min;
    tm.tm_sec = t->sec;
    tm.tm_isdst = -1;
    s_rtc_offset = mktime(&tm) - time(NULL);
}

/* --------------------------------------------------------------- heap */
void *kmalloc(uint32_t size)
{
    return malloc(size ? size : 1);
}

void kfree(void *ptr)
{
    free(ptr);
}

/* ------------------------------------------------------------- string */
/* Decimal, 0x hex and 0b binary, as src/string.c reads them. */
int str_to_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0, base = 10;
    int digits = 0;

    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }

    for (; *s; s++) {
        uint32_t d;
        char c = *s;

        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else return -1;
        if (d >= base) return -1;
        v = v * base + d;
        digits++;
    }
    if (!digits) return -1;
    *out = v;
    return 0;
}

/* ---------------------------------------------------------------- log */
/* klog() writes to standard error: the clock, the level, the message. */
static int s_log_level = FREYA_LOG_INFO;

static const char *const s_log_names[] = {
    "OFF", "ERROR", "WARN", "INFO", "DEBUG"
};

const char *log_level_str(int level)
{
    if (level < 0 || level > FREYA_LOG_DEBUG) return "?";
    return s_log_names[level];
}

int log_get_level(void)
{
    return s_log_level;
}

int log_set_level(int level)
{
    if (level < FREYA_LOG_OFF || level > FREYA_LOG_DEBUG) return FAT_ERR_INVAL;
    s_log_level = level;
    return 0;
}

typedef struct {
    char *buf;
    int pos, size;
} log_line_t;

static void log_emit(void *arg, char c)
{
    log_line_t *l = arg;

    if (l->pos < l->size - 1) l->buf[l->pos++] = c;
}

void klog(int level, const char *fmt, ...)
{
    char buf[512];
    log_line_t line = { buf, 0, (int)sizeof buf };
    rtc_time_t t;
    va_list ap;

    if (s_log_level == FREYA_LOG_OFF || level < FREYA_LOG_ERROR ||
        level > s_log_level || !fmt)
        return;
    rtc_get(&t);
    line.pos = ksnprintf(buf, (int)sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u %s ",
                         t.year, t.mon, t.day, t.hour, t.min, t.sec,
                         log_level_str(level));
    if (line.pos < 0) line.pos = 0;
    if (line.pos > line.size - 1) line.pos = line.size - 1;
    va_start(ap, fmt);
    kvfprintf(log_emit, &line, fmt, ap);
    va_end(ap);
    buf[line.pos] = '\0';
    console_flush();
    fprintf(stderr, "%s\n", buf);
}

/* -------------------------------------------------------------- files */
/*
 * The FAT calls the shell makes, on the host's files.  A failure the FAT
 * codes name is reported as that code; any other carries its errno, so
 * the message is the host's own.
 */
#define ERR_ERRNO_BASE  (-100)

static int fs_err(int e)
{
    switch (e) {
    case ENOENT:    return FAT_ERR_NOENT;
    case EEXIST:    return FAT_ERR_EXIST;
    case ENOSPC:    return FAT_ERR_NOSPC;
    case EINVAL:    return FAT_ERR_INVAL;
    case ENOTDIR:   return FAT_ERR_NOTDIR;
    case EISDIR:    return FAT_ERR_ISDIR;
    case ENOTEMPTY: return FAT_ERR_NOTEMPTY;
    case EROFS:     return FAT_ERR_RDONLY;
    default:        return ERR_ERRNO_BASE - e;
    }
}

const char *fat_err_str(int err)
{
    if (err <= ERR_ERRNO_BASE) return strerror(ERR_ERRNO_BASE - err);
    switch (err) {
    case FAT_OK:           return "ok";
    case FAT_ERR_IO:       return "I/O error";
    case FAT_ERR_NOFS:     return "no filesystem";
    case FAT_ERR_NOENT:    return "no such file or directory";
    case FAT_ERR_EXIST:    return "already exists";
    case FAT_ERR_NOSPC:    return "no space left";
    case FAT_ERR_INVAL:    return "invalid argument";
    case FAT_ERR_NOTDIR:   return "not a directory";
    case FAT_ERR_ISDIR:    return "is a directory";
    case FAT_ERR_NOTEMPTY: return "directory not empty";
    case FAT_ERR_NOFILE:   return "not a regular file";
    case FAT_ERR_RDONLY:   return "read-only";
    default:               return "unknown error";
    }
}

int fat_mounted(void) { return 1; }
int fat_sync(void)    { return FAT_OK; }

static char s_cwd[FAT_MAX_PATH];

const char *fs_cwd(void)
{
    if (!getcwd(s_cwd, sizeof s_cwd)) strcpy(s_cwd, "?");
    return s_cwd;
}

/* in against the working directory, with "." and ".." taken out. */
int fs_abspath(const char *in, char *out, int size)
{
    char tmp[FAT_MAX_PATH * 2];
    int len = 0, o = 0;

    if (in[0] != '/') {
        const char *c = fs_cwd();

        len = (int)strlen(c);
        if (len >= (int)sizeof tmp - 1) return -1;
        memcpy(tmp, c, (size_t)len);
        tmp[len++] = '/';
    }
    while (*in && len < (int)sizeof tmp - 1) tmp[len++] = *in++;
    if (*in) return -1;

    out[o++] = '/';
    for (int i = 0; i < len; ) {
        int start, n;

        while (i < len && tmp[i] == '/') i++;
        start = i;
        while (i < len && tmp[i] != '/') i++;
        n = i - start;
        if (n == 0 || (n == 1 && tmp[start] == '.')) continue;
        if (n == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            while (o > 1 && out[o - 1] != '/') o--;
            if (o > 1) o--;
            continue;
        }
        if (o > 1) {
            if (o >= size - 1) return -1;
            out[o++] = '/';
        }
        if (o + n >= size) return -1;
        memcpy(out + o, tmp + start, (size_t)n);
        o += n;
    }
    out[o] = '\0';
    return 0;
}

int fs_chdir(const char *path)
{
    char abs[FAT_MAX_PATH];

    if (fs_abspath(path, abs, sizeof abs) != 0) return FAT_ERR_INVAL;
    if (chdir(abs) != 0) return fs_err(errno);
    return FAT_OK;
}

int fs_rename(const char *old_path, const char *new_path)
{
    char a[FAT_MAX_PATH], b[FAT_MAX_PATH];

    if (fs_abspath(old_path, a, sizeof a) != 0 ||
        fs_abspath(new_path, b, sizeof b) != 0)
        return FAT_ERR_INVAL;
    if (rename(a, b) != 0) return fs_err(errno);
    return FAT_OK;
}

static void stat_to_dirent(const char *name, const struct stat *st, fat_dirent_t *e)
{
    struct tm tm;
    int year;

    memset(e, 0, sizeof *e);
    strncpy(e->name, name, sizeof e->name - 1);
    if (S_ISDIR(st->st_mode)) e->attr |= FAT_ATTR_DIR;
    if (!(st->st_mode & (S_IWUSR | S_IWGRP | S_IWOTH))) e->attr |= FAT_ATTR_RDONLY;
    if (name[0] == '.') e->attr |= FAT_ATTR_HIDDEN;
    e->size = st->st_size > 0xFFFFFFFF ? 0xFFFFFFFFu : (uint32_t)st->st_size;
    localtime_r(&st->st_mtime, &tm);
    year = tm.tm_year + 1900;
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;
    e->wdate = (uint16_t)(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    e->wtime = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

int fat_stat(const char *path, fat_dirent_t *e)
{
    struct stat st;
    const char *base = strrchr(path, '/');

    if (stat(path, &st) != 0) return fs_err(errno);
    stat_to_dirent(base && base[1] ? base + 1 : path, &st, e);
    return FAT_OK;
}

int fat_mkdir(const char *path)
{
    if (mkdir(path, 0777) != 0) return fs_err(errno);
    return FAT_OK;
}

int fat_unlink(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0) return fs_err(errno);
    if (S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) {
        /* rmdir reports a full directory as either. */
        if (errno == EEXIST) return FAT_ERR_NOTEMPTY;
        return fs_err(errno);
    }
    return FAT_OK;
}

/* An open directory is a slot here; fat_dir_t only carries its number. */
#define DIR_SLOTS  8

static struct {
    DIR *d;
    char path[FAT_MAX_PATH];
} s_dirs[DIR_SLOTS];

int fat_opendir(fat_dir_t *dir, const char *path)
{
    int i;

    for (i = 0; i < DIR_SLOTS && s_dirs[i].d; i++) { }
    if (i == DIR_SLOTS) return FAT_ERR_IO;
    s_dirs[i].d = opendir(path);
    if (!s_dirs[i].d) return fs_err(errno);
    strncpy(s_dirs[i].path, path, sizeof s_dirs[i].path - 1);
    memset(dir, 0, sizeof *dir);
    dir->open = 1;
    dir->dev = (uint8_t)i;
    return FAT_OK;
}

int fat_readdir(fat_dir_t *dir, fat_dirent_t *e)
{
    DIR *d;

    if (!dir->open || dir->dev >= DIR_SLOTS || !(d = s_dirs[dir->dev].d))
        return FAT_ERR_INVAL;
    for (;;) {
        char full[FAT_MAX_PATH + FAT_MAX_NAME + 1];
        struct dirent *de;
        struct stat st;

        errno = 0;
        de = readdir(d);
        if (!de) return errno ? fs_err(errno) : 1;
        snprintf(full, sizeof full, "%s/%s", s_dirs[dir->dev].path, de->d_name);
        /* A name that went away between the two calls is skipped. */
        if (stat(full, &st) != 0 && lstat(full, &st) != 0) continue;
        stat_to_dirent(de->d_name, &st, e);
        return FAT_OK;
    }
}

int fat_closedir(fat_dir_t *dir)
{
    if (!dir->open || dir->dev >= DIR_SLOTS || !s_dirs[dir->dev].d)
        return FAT_ERR_INVAL;
    closedir(s_dirs[dir->dev].d);
    s_dirs[dir->dev].d = NULL;
    dir->open = 0;
    return FAT_OK;
}

/* Descriptors the scripts hold: small numbers, each one a host file this
 * process opened, so a script cannot close the terminal by number. */
#define FD_SLOTS  16

static int s_fds[FD_SLOTS];

static int host_fd(int fd)
{
    if (fd < 0 || fd >= FD_SLOTS || s_fds[fd] <= 0) return -1;
    return s_fds[fd];
}

int fs_fd_open(const char *path, int flags)
{
    char abs[FAT_MAX_PATH];
    int i, oflags, h;

    if (fs_abspath(path, abs, sizeof abs) != 0) return FAT_ERR_INVAL;
    for (i = 0; i < FD_SLOTS && s_fds[i] > 0; i++) { }
    if (i == FD_SLOTS) return FAT_ERR_NOSPC;
    switch (flags & FREYA_O_RDWR) {
    case FREYA_O_WRONLY: oflags = O_WRONLY; break;
    case FREYA_O_RDWR:   oflags = O_RDWR;   break;
    default:             oflags = O_RDONLY; break;
    }
    if (flags & FREYA_O_CREATE) oflags |= O_CREAT;
    if (flags & FREYA_O_TRUNC)  oflags |= O_TRUNC;
    if (flags & FREYA_O_APPEND) oflags |= O_APPEND;
    h = open(abs, oflags | O_CLOEXEC, 0666);
    if (h < 0) return fs_err(errno);
    s_fds[i] = h;
    return i;
}

int fs_fd_close(int fd)
{
    int h = host_fd(fd);

    if (h < 0) return FAT_ERR_INVAL;
    s_fds[fd] = 0;
    if (close(h) != 0) return fs_err(errno);
    return FAT_OK;
}

int fs_fd_read(int fd, void *buf, int len)
{
    int h = host_fd(fd);
    ssize_t n;

    if (h < 0) return FAT_ERR_INVAL;
    do n = read(h, buf, (size_t)len); while (n < 0 && errno == EINTR);
    return n < 0 ? fs_err(errno) : (int)n;
}

int fs_fd_write(int fd, const void *buf, int len)
{
    int h = host_fd(fd), done = 0;

    if (h < 0) return FAT_ERR_INVAL;
    while (done < len) {
        ssize_t n = write(h, (const char *)buf + done, (size_t)(len - done));

        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return fs_err(errno);
        done += (int)n;
    }
    return done;
}

int fs_fd_seek(int fd, int32_t off, int whence)
{
    int h = host_fd(fd);
    int w = whence == FREYA_SEEK_END ? SEEK_END :
            whence == FREYA_SEEK_CUR ? SEEK_CUR : SEEK_SET;

    if (h < 0) return FAT_ERR_INVAL;
    if (lseek(h, off, w) < 0) return fs_err(errno);
    return FAT_OK;
}

int32_t fs_fd_tell(int fd)
{
    int h = host_fd(fd);
    off_t at;

    if (h < 0) return FAT_ERR_INVAL;
    at = lseek(h, 0, SEEK_CUR);
    if (at < 0) return fs_err(errno);
    return at > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)at;
}

int32_t fs_fd_size(int fd)
{
    int h = host_fd(fd);
    struct stat st;

    if (h < 0) return FAT_ERR_INVAL;
    if (fstat(h, &st) != 0) return fs_err(errno);
    return st.st_size > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)st.st_size;
}

/* ----------------------------------------------------------- programs */
int linux_spawn(char *const argv[], char **out, uint32_t *len)
{
    int pipefd[2] = { -1, -1 };
    char *buf = NULL;
    uint32_t n = 0, cap = 0;
    pid_t pid;
    int st;

    console_flush();
    if (out && pipe2(pipefd, O_CLOEXEC) != 0) {
        kprintf("run: %s\r\n", strerror(errno));
        return -1;
    }
    term_restore();
    pid = fork();
    if (pid < 0) {
        int e = errno;

        term_raw();
        if (out) {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        kprintf("run: %s\r\n", strerror(e));
        return -1;
    }
    if (pid == 0) {
        if (out) dup2(pipefd[1], STDOUT_FILENO);
        execvp(argv[0], argv);
        fprintf(stderr, "run: %s: %s\n", argv[0], strerror(errno));
        _exit(FREYA_EXIT_NOTFOUND);
    }
    if (out) {
        close(pipefd[1]);
        for (;;) {
            ssize_t got;

            if (n == cap) {
                uint32_t ncap = cap ? cap * 2U : 4096U;
                char *nb = ncap > cap ? realloc(buf, ncap) : NULL;

                if (!nb) {
                    kprintf("run: out of memory\r\n");
                    free(buf);
                    buf = NULL;
                    n = 0;
                    kill(pid, SIGTERM);
                    break;
                }
                buf = nb;
                cap = ncap;
            }
            got = read(pipefd[0], buf + n, cap - n);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            n += (uint32_t)got;
        }
        close(pipefd[0]);
    }
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            st = FREYA_EXIT_FAIL << 8;
            break;
        }
    }
    term_raw();
    if (out) {
        *out = buf;
        *len = n;
    }
    if (WIFSIGNALED(st)) {
        /* Ctrl-C at the program stops the script too. */
        if (WTERMSIG(st) == SIGINT) s_sigint = 1;
        return 128 + WTERMSIG(st);
    }
    return WEXITSTATUS(st);
}

void linux_exit(int status)
{
    console_flush();
    term_restore();
    exit(status & FREYA_EXIT_MAX);
}

/* --------------------------------------------------------------- main */
static int usage(void)
{
    fprintf(stderr,
            "usage: fsh                 interactive shell\n"
            "       fsh <file>          run a script\n"
            "       fsh -c <text>       run the text\n"
            "       fsh < <file>        run standard input as a script\n");
    return 2;
}

/* Standard input that is not a terminal is one script. */
static int run_stdin(void)
{
    char *buf = NULL;
    size_t n = 0, cap = 0;
    int rc;

    for (;;) {
        ssize_t got;

        if (n + 1 >= cap) {
            char *nb;

            cap = cap ? cap * 2 : 4096;
            nb = realloc(buf, cap);
            if (!nb) {
                fprintf(stderr, "fsh: out of memory\n");
                free(buf);
                return FREYA_EXIT_FAIL;
            }
            buf = nb;
        }
        got = read(STDIN_FILENO, buf + n, cap - n - 1);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            fprintf(stderr, "fsh: %s\n", strerror(errno));
            free(buf);
            return FREYA_EXIT_FAIL;
        }
        if (got == 0) break;
        n += (size_t)got;
    }
    buf[n] = '\0';
    if (!script_text_ok(buf, (uint32_t)n)) {
        fprintf(stderr, "fsh: standard input is not a shell script\n");
        free(buf);
        return FREYA_EXIT_FAIL;
    }
    rc = shell_exec(buf);
    free(buf);
    return rc;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    int rc;

    clock_gettime(CLOCK_MONOTONIC, &s_start);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    if (argc == 3 && strcmp(argv[1], "-c") == 0) {
        rc = shell_exec(argv[2]);
    } else if (argc == 2 && argv[1][0] == '-') {
        return usage();
    } else if (argc == 2) {
        rc = shell_source_file(argv[1]);
    } else if (argc != 1) {
        return usage();
    } else if (!isatty(STDIN_FILENO)) {
        rc = run_stdin();
    } else {
        if (tcgetattr(STDIN_FILENO, &s_term_saved) == 0) s_interactive = 1;
        term_raw();
        console_banner();
        rc = shell_run();
    }
    linux_exit(rc);
}
