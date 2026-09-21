/*
 * Freya - file log on the SD card.
 *
 * Lines go to /freya.log at the volume root.  Each line starts with the
 * software clock and a level name.  At FREYA_LOG_MAX_SIZE the current file
 * is renamed to /freya.log.old (after dropping any previous copy), so one
 * rotated log is kept and the rename is a directory-entry swap.
 *
 * The current level lives in RAM.  On a board with the auto-start slot it
 * is also the second word of that slot, so it survives a reset the same
 * way the auto-run and ram-dump flags do.
 *
 * With no card (or a volume that is not mounted) the file backend is not
 * used: a stub writes the same line to the console and never touches SPI
 * or FAT, so a flash-only program can still log.
 */
#include "freya.h"
#include "fat.h"

#define LINE_MAX  192

static int s_level = FREYA_LOG_INFO;
static int s_busy;

static const char *const s_names[] = {
    "OFF", "ERROR", "WARN", "INFO", "DEBUG"
};

const char *log_level_str(int level)
{
    if (level < 0 || level > FREYA_LOG_DEBUG) return "?";
    return s_names[level];
}

int log_get_level(void)
{
    return s_level;
}

void log_init(void)
{
    s_level = FREYA_LOG_INFO;
#if defined(FREYA_APP_FLASH_ADDR) && !defined(FREYA_HOST)
    {
        uint32_t stored = app_log_level_stored();

        if (stored <= (uint32_t)FREYA_LOG_DEBUG)
            s_level = (int)stored;
    }
#endif
}

int log_set_level(int level)
{
    if (level < FREYA_LOG_OFF || level > FREYA_LOG_DEBUG)
        return FAT_ERR_INVAL;
    s_level = level;
#if defined(FREYA_APP_FLASH_ADDR) && !defined(FREYA_HOST)
    {
        int rc = app_log_level_store((uint32_t)level);

        if (rc == FLASH_ERR_BUSY)
            return 0;           /* RAM took it; flash waits until idle */
        return rc;
    }
#else
    return 0;
#endif
}

typedef struct {
    char *buf;
    int   pos;
    int   size;
} line_t;

static void emit_line(void *arg, char c)
{
    line_t *s = arg;

    if (s->pos < s->size - 1)
        s->buf[s->pos] = c;
    s->pos++;
}

/*
 * Keep one previous log: drop /freya.log.old, then rename the current
 * file onto that name.  fat_rename writes the new directory entry before
 * deleting the old one, so a crash cannot orphan the cluster chain.
 */
static int rotate_log(void)
{
    int rc;

    rc = fat_unlink(FREYA_LOG_OLD_PATH);
    if (rc != FAT_OK && rc != FAT_ERR_NOENT)
        return rc;
    return fat_rename(FREYA_LOG_PATH, FREYA_LOG_OLD_PATH);
}

/* No SD / not mounted: keep the API live without talking to the card. */
static int append_stub(const char *line, int len)
{
    int i;

    for (i = 0; i < len; i++)
        uart_putc(line[i]);
    return 0;
}

static int append_line(const char *line, int len)
{
    fat_file_t f;
    uint32_t put = 0;
    int rc;

    if (len <= 0)
        return 0;
    if (!fat_mounted())
        return append_stub(line, len);

    rc = fat_open(&f, FREYA_LOG_PATH, FAT_WRITE | FAT_CREATE | FAT_APPEND);
    if (rc == FAT_OK && f.size >= FREYA_LOG_MAX_SIZE) {
        fat_close(&f);
        rc = rotate_log();
        if (rc != FAT_OK)
            return rc;
        rc = fat_open(&f, FREYA_LOG_PATH, FAT_WRITE | FAT_CREATE | FAT_APPEND);
    }
    if (rc != FAT_OK)
        return rc;

    rc = fat_write(&f, line, (uint32_t)len, &put);
    fat_close(&f);
    return rc;
}

void klog(int level, const char *fmt, ...)
{
    char buf[LINE_MAX];
    line_t line;
    rtc_time_t t;
    va_list ap;
    int n, cap;

    if (s_busy)
        return;
    if (s_level == FREYA_LOG_OFF || level < FREYA_LOG_ERROR || level > s_level)
        return;
    if (!fmt)
        return;

    s_busy = 1;
    rtc_get(&t);
    n = ksnprintf(buf, (int)sizeof(buf),
                  "%04u-%02u-%02u %02u:%02u:%02u %s ",
                  t.year, t.mon, t.day, t.hour, t.min, t.sec,
                  log_level_str(level));
    if (n < 0)
        n = 0;
    if (n >= (int)sizeof(buf))
        n = (int)sizeof(buf) - 1;

    cap = (int)sizeof(buf) - 3;         /* leave room for \r\n and NUL */
    if (n < cap) {
        line.buf  = buf;
        line.pos  = n;
        line.size = cap + 1;
        va_start(ap, fmt);
        kvfprintf(emit_line, &line, fmt, ap);
        va_end(ap);
        n = MIN(line.pos, cap);
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = '\0';

    append_line(buf, n);
    s_busy = 0;
}
