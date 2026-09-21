/*
 * Freya - small formatted output engine.
 *
 * Supports: %c %s %d %i %u %x %X %p %% with the 'l' length modifier,
 * zero/space padding, left alignment, '+' sign and a field precision for
 * strings (%.8s).  No floating point - the kernel has no use for it.
 */
#include "freya.h"

typedef struct {
    void (*emit)(void *, char);
    void *arg;
    int   count;
} out_t;

static void out_char(out_t *o, char c)
{
    o->emit(o->arg, c);
    o->count++;
}

static void out_pad(out_t *o, char pad, int n)
{
    while (n-- > 0) out_char(o, pad);
}

static int u32_to_str(char *buf, uint32_t v, uint32_t base, int upper)
{
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;
    char tmp[32];
    int n = 0, i;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);

    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

int kvfprintf(void (*emit)(void *, char), void *arg, const char *fmt, va_list ap)
{
    out_t o = { emit, arg, 0 };
    char buf[36];

    while (*fmt) {
        int left = 0, zero = 0, plus = 0, width = 0, prec = -1, lng = 0;
        char spec;

        if (*fmt != '%') { out_char(&o, *fmt++); continue; }
        fmt++;

        for (;;) {
            if (*fmt == '-')      { left = 1; fmt++; }
            else if (*fmt == '0') { zero = 1; fmt++; }
            else if (*fmt == '+') { plus = 1; fmt++; }
            else if (*fmt == ' ') { fmt++; }
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = 1; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') lng = 1; fmt++; }

        spec = *fmt++;
        switch (spec) {
        case '\0':
            return o.count;
        case '%':
            out_char(&o, '%');
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left) out_pad(&o, ' ', width - 1);
            out_char(&o, c);
            if (left) out_pad(&o, ' ', width - 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = 0;
            if (!s) s = "(null)";
            while (s[len] && (prec < 0 || len < prec)) len++;
            if (!left) out_pad(&o, ' ', width - len);
            for (int i = 0; i < len; i++) out_char(&o, s[i]);
            if (left) out_pad(&o, ' ', width - len);
            break;
        }
        case 'd':
        case 'i': {
            int32_t v = lng ? (int32_t)va_arg(ap, long) : va_arg(ap, int);
            uint32_t mag = (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
            int n = u32_to_str(buf, mag, 10, 0);
            int sign = (v < 0) ? 1 : (plus ? 1 : 0);
            if (!left && !zero) out_pad(&o, ' ', width - n - sign);
            if (v < 0) out_char(&o, '-');
            else if (plus) out_char(&o, '+');
            if (!left && zero) out_pad(&o, '0', width - n - sign);
            for (int i = 0; i < n; i++) out_char(&o, buf[i]);
            if (left) out_pad(&o, ' ', width - n - sign);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'p': {
            uint32_t v;
            uint32_t base = (spec == 'u') ? 10 : 16;
            int n;
            if (spec == 'p') { v = (uint32_t)(uintptr_t)va_arg(ap, void *); width = 8; zero = 1; }
            else v = lng ? (uint32_t)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            n = u32_to_str(buf, v, base, spec == 'X');
            if (!left) out_pad(&o, zero ? '0' : ' ', width - n);
            for (int i = 0; i < n; i++) out_char(&o, buf[i]);
            if (left) out_pad(&o, ' ', width - n);
            break;
        }
        default:
            out_char(&o, '%');
            out_char(&o, spec);
            break;
        }
    }
    return o.count;
}

static void emit_uart(void *arg, char c)
{
    (void)arg;
    uart_putc(c);
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = kvfprintf(emit_uart, NULL, fmt, ap);
    va_end(ap);
    return n;
}

typedef struct {
    char *buf;
    int   pos;
    int   size;
} sbuf_t;

static void emit_sbuf(void *arg, char c)
{
    sbuf_t *s = arg;
    if (s->pos < s->size - 1) s->buf[s->pos] = c;
    s->pos++;
}

int ksnprintf(char *out, int size, const char *fmt, ...)
{
    sbuf_t s = { out, 0, size };
    va_list ap;

    if (size <= 0) return 0;
    va_start(ap, fmt);
    kvfprintf(emit_sbuf, &s, fmt, ap);
    va_end(ap);
    out[MIN(s.pos, size - 1)] = '\0';
    return s.pos;
}

/* Prints a byte count as "123 B", "45.6 KiB", "1.2 MiB" ... */
void kput_size(uint64_t bytes)
{
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    uint64_t v = bytes;
    uint32_t frac = 0;
    int u = 0;

    while (v >= 1024 && u < 4) {
        frac = (uint32_t)(((v % 1024) * 10) / 1024);
        v /= 1024;
        u++;
    }
    if (u == 0) kprintf("%u B", (uint32_t)v);
    else        kprintf("%u.%u %s", (uint32_t)v, frac, unit[u]);
}
