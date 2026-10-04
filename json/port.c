/*
 * Freya - the C library cJSON calls (see cjson_port.h).
 *
 * Memory comes from the kernel heap through the api freya_cjson_init()
 * stored, in chunks this file divides up (see below).  Each block
 * carries its size in front of it, so realloc() can copy, and cJSON's
 * default hooks, which use it to grow the print buffer, work as they are.
 *
 * Numbers are converted exactly, as a good C library does it: strtod()
 * returns the nearest double (ties to even) and %g the correctly rounded
 * digits.  Both work on integers of up to 1280 bits (big_t) on the
 * stack, about 200 bytes each; strtod() takes the fast path of one
 * floating point rounding when it is exact.  strtod() keeps the first 40
 * significant digits (DIGITS_MAX) and only notes whether any later one is
 * not zero, so a number of more digits that falls within 10^-40 of a
 * point halfway between two doubles may round the wrong way.
 *
 * Built with -fno-tree-loop-distribute-patterns, so that GCC does not
 * turn the loops below back into calls to memcpy and memset.
 */
#include <stdarg.h>
#include <stdint.h>
#include "freya_cjson.h"
#include "cjson_port.h"

/* The prototypes are what this file wants from cjson_port.h; its names
 * for malloc and free would also rename the api's members. */
#undef malloc
#undef free

static const freya_api_t *s_api;

void freya_cjson_init(const freya_api_t *api)
{
    s_api = api;
}

/* ------------------------------------------------------------- memory */
/*
 * The kernel lends a program at most 32 blocks at a time, and cJSON
 * takes one or two for every value, so the small ones come from chunks
 * divided up here: first fit, every block with an 8-byte header that
 * keeps its size, free neighbours joined as the search walks past them.
 * Each new chunk is twice the last, from FREYA_CJSON_CHUNK to
 * FREYA_CJSON_CHUNK_MAX, so a few kernel blocks hold a large document.
 * A chunk that is all free again goes back to the kernel.  A request of
 * more than a quarter of the largest chunk, such as a print buffer that
 * has grown, is a block of its own.
 */
#ifndef FREYA_CJSON_CHUNK
#define FREYA_CJSON_CHUNK 4096U
#endif
#ifndef FREYA_CJSON_CHUNK_MAX
#define FREYA_CJSON_CHUNK_MAX 65536U
#endif

#define TAG_FREE  0x45455246U           /* "FREE" */
#define TAG_USED  0x44455355U           /* "USED" */
#define TAG_OWN   0x214E574FU           /* "OWN!": a kernel block of its own */
#define HEADER    8U

typedef struct {
    uint32_t size;                      /* with the header, a multiple of 8 */
    uint32_t tag;
} block_t;

typedef struct chunk {
    struct chunk *next;
    uint32_t bytes;                     /* the blocks' room                */
    uint32_t free_bytes;                /* in free blocks, headers counted */
} chunk_t;

#define CHUNK_HEADER ((sizeof(chunk_t) + 7U) & ~7U)

static chunk_t *s_chunks;
static uint32_t s_chunk_next = FREYA_CJSON_CHUNK;

static block_t *first_block(chunk_t *c)
{
    return (block_t *)((uint8_t *)c + CHUNK_HEADER);
}

static block_t *next_block(block_t *b)
{
    return (block_t *)((uint8_t *)b + b->size);
}

static void *chunk_alloc(chunk_t *c, uint32_t need)
{
    block_t *end = (block_t *)((uint8_t *)first_block(c) + c->bytes);

    for (block_t *b = first_block(c); b < end; b = next_block(b)) {
        if (b->tag != TAG_FREE) continue;
        while (next_block(b) < end && next_block(b)->tag == TAG_FREE)
            b->size += next_block(b)->size;
        if (b->size < need) continue;
        if (b->size - need >= HEADER + 8U) {
            block_t *rest = (block_t *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->tag = TAG_FREE;
            b->size = need;
        }
        b->tag = TAG_USED;
        c->free_bytes -= b->size;
        return (uint8_t *)b + HEADER;
    }
    return NULL;
}

void *freya_cjson_malloc(size_t size)
{
    uint32_t need, bytes;
    chunk_t *c;
    void *p;

    if (!s_api || size > 0x7fffffffU - 2 * HEADER) return NULL;
    need = ((uint32_t)size + HEADER + 7U) & ~7U;
    if (need > FREYA_CJSON_CHUNK_MAX / 4) {
        block_t *b = s_api->malloc(need);
        if (!b) return NULL;
        b->size = need;
        b->tag = TAG_OWN;
        return (uint8_t *)b + HEADER;
    }
    for (c = s_chunks; c; c = c->next) {
        if (c->free_bytes < need) continue;
        p = chunk_alloc(c, need);
        if (p) return p;
    }
    while (s_chunk_next < need) s_chunk_next *= 2;
    bytes = s_chunk_next;
    c = s_api->malloc(CHUNK_HEADER + bytes);
    if (!c) {                   /* a heap short of the doubled size */
        bytes = need > FREYA_CJSON_CHUNK ? need : FREYA_CJSON_CHUNK;
        c = s_api->malloc(CHUNK_HEADER + bytes);
        if (!c) return NULL;
    } else if (s_chunk_next < FREYA_CJSON_CHUNK_MAX) {
        s_chunk_next *= 2;
    }
    c->bytes = bytes;
    c->free_bytes = bytes;
    first_block(c)->size = bytes;
    first_block(c)->tag = TAG_FREE;
    c->next = s_chunks;
    s_chunks = c;
    return chunk_alloc(c, need);
}

void freya_cjson_free(void *p)
{
    block_t *b;
    chunk_t **link;

    if (!p || !s_api) return;
    b = (block_t *)((uint8_t *)p - HEADER);
    if (b->tag == TAG_OWN) {
        b->tag = 0;
        s_api->free(b);
        return;
    }
    if (b->tag != TAG_USED) return;             /* not ours, or freed twice */
    for (link = &s_chunks; *link; link = &(*link)->next) {
        chunk_t *c = *link;
        if ((uint8_t *)b < (uint8_t *)first_block(c) ||
            (uint8_t *)b >= (uint8_t *)first_block(c) + c->bytes)
            continue;
        b->tag = TAG_FREE;
        c->free_bytes += b->size;
        if (c->free_bytes == c->bytes) {
            *link = c->next;
            s_api->free(c);
        }
        return;
    }
}

void *freya_cjson_realloc(void *p, size_t size)
{
    uint8_t *q;
    uint32_t old;

    if (!p) return freya_cjson_malloc(size);
    old = ((block_t *)((uint8_t *)p - HEADER))->size - HEADER;
    if (size <= old) return p;
    q = freya_cjson_malloc(size);
    if (!q) return NULL;
    memcpy(q, p, old);
    freya_cjson_free(p);
    return q;
}

/* ------------------------------------------------------------ strings */
#ifndef FREYA_HOST
__attribute__((weak)) void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((weak)) void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--) *d++ = (uint8_t)c;
    return dst;
}
#endif

size_t freya_cjson_strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) n++;
    return n;
}

int freya_cjson_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int freya_cjson_strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) break;
    }
    return 0;
}

char *freya_cjson_strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *freya_cjson_strcat(char *dst, const char *src)
{
    freya_cjson_strcpy(dst + freya_cjson_strlen(dst), src);
    return dst;
}

char *freya_cjson_strrchr(const char *s, int c)
{
    const char *last = NULL;

    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

int freya_cjson_tolower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

double freya_cjson_fabs(double v)
{
    return v < 0 ? -v : v;
}

/* ------------------------------------------------------------ numbers */
/*
 * Unsigned integers of up to 1408 bits, enough for every double times
 * the powers of ten and two that the conversions below scale it by.
 */
#define BIG_WORDS 44

typedef struct {
    int      n;                          /* words in use, no leading zero */
    uint32_t w[BIG_WORDS];
} big_t;

static void big_set(big_t *a, uint64_t v)
{
    a->n = 0;
    while (v) {
        a->w[a->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static void big_mul_small(big_t *a, uint32_t m)
{
    uint64_t carry = 0;

    for (int i = 0; i < a->n; i++) {
        carry += (uint64_t)a->w[i] * m;
        a->w[i] = (uint32_t)carry;
        carry >>= 32;
    }
    if (carry && a->n < BIG_WORDS) a->w[a->n++] = (uint32_t)carry;
}

static void big_add_small(big_t *a, uint32_t v)
{
    for (int i = 0; v && i < a->n; i++) {
        uint64_t sum = (uint64_t)a->w[i] + v;
        a->w[i] = (uint32_t)sum;
        v = (uint32_t)(sum >> 32);
    }
    if (v && a->n < BIG_WORDS) a->w[a->n++] = v;
}

static void big_mul_pow10(big_t *a, int e)
{
    for (; e >= 9; e -= 9) big_mul_small(a, 1000000000U);
    if (e > 0) {
        uint32_t m = 1;
        while (e--) m *= 10;
        big_mul_small(a, m);
    }
}

static void big_shl(big_t *a, int bits)
{
    int words = bits / 32, shift = bits % 32;

    if (!a->n) return;
    if (shift) {
        uint32_t carry = 0;
        for (int i = 0; i < a->n; i++) {
            uint32_t w = a->w[i];
            a->w[i] = w << shift | carry;
            carry = w >> (32 - shift);
        }
        if (carry && a->n < BIG_WORDS) a->w[a->n++] = carry;
    }
    if (words) {
        if (a->n + words > BIG_WORDS) words = BIG_WORDS - a->n;
        for (int i = a->n - 1; i >= 0; i--) a->w[i + words] = a->w[i];
        for (int i = 0; i < words; i++) a->w[i] = 0;
        a->n += words;
    }
}

static int big_cmp(const big_t *a, const big_t *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* a -= b, for a >= b. */
static void big_sub(big_t *a, const big_t *b)
{
    int64_t borrow = 0;

    for (int i = 0; i < a->n; i++) {
        int64_t v = (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0) - borrow;
        borrow = v < 0;
        a->w[i] = (uint32_t)v;
    }
    while (a->n && !a->w[a->n - 1]) a->n--;
}

typedef union { double d; uint64_t u; } bits_t;

/* A finite positive double as mantissa * 2^exponent. */
static void split(double d, uint64_t *m, int *e)
{
    bits_t b;
    int biased;

    b.d = d;
    biased = (int)(b.u >> 52 & 0x7ff);
    *m = b.u & ((1ULL << 52) - 1);
    if (biased) *m |= 1ULL << 52;
    *e = (biased ? biased : 1) - 1075;
}

/* Compares m * 10^e10 with k * 2^e2. */
static int cmp_exact(const big_t *m, int e10, uint64_t k, int e2)
{
    big_t lhs = *m, rhs;

    big_set(&rhs, k);
    if (e10 > 0) big_mul_pow10(&lhs, e10);
    else big_mul_pow10(&rhs, -e10);
    if (e2 > 0) big_shl(&rhs, e2);
    else big_shl(&lhs, -e2);
    return big_cmp(&lhs, &rhs);
}

static const double s_pow10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* v * 10^e, close: one rounding for |e| <= 22, one more per further 22. */
static double scale10(double v, int e)
{
    while (e > 22) { v *= 1e22; e -= 22; }
    while (e < -22) { v /= 1e22; e += 22; }
    return e >= 0 ? v * s_pow10[e] : v / s_pow10[-e];
}

/* Significant digits strtod() keeps; later ones only count as sticky. */
#define DIGITS_MAX 40

/*
 * The double nearest m * 10^e10, ties to even, where m is the digits
 * kept and guess a close double to start from.  sticky says digits after
 * them were dropped, so the value is a little above m * 10^e10.  The
 * guess is corrected one step at a time against the halfway points on
 * either side of it, compared exactly.
 */
static double nearest(const big_t *m, int e10, int sticky, double guess)
{
    bits_t b;

    b.d = guess;
    if (b.u >= 0x7ff0000000000000ULL) b.u = 0x7fefffffffffffffULL;
    if (b.u == 0) b.u = 1;
    for (int step = 0; step < 64; step++) {
        uint64_t k;
        int e2, c;

        split(b.d, &k, &e2);
        /* above: halfway to the next double up */
        c = cmp_exact(m, e10, 2 * k + 1, e2 - 1);
        if (c == 0 && sticky) c = 1;
        if (c > 0 || (c == 0 && (k & 1))) {
            if (b.u == 0x7fefffffffffffffULL) return HUGE_VAL;
            b.u++;
            if (c == 0) break;
            continue;
        }
        if (c == 0) break;
        /* below: halfway to the next double down, which is nearer at the
         * bottom of a binade */
        if (b.u == 1) {
            c = cmp_exact(m, e10, 1, -1075);
            if (c < 0 || (c == 0 && !sticky)) return 0.0;
            break;
        }
        if (k == (1ULL << 52) && e2 > -1074)
            c = cmp_exact(m, e10, 4 * k - 1, e2 - 2);
        else
            c = cmp_exact(m, e10, 2 * k - 1, e2 - 1);
        if (c < 0 || (c == 0 && !sticky && (k & 1))) {
            b.u--;
            if (c == 0) break;
            continue;
        }
        break;
    }
    return b.d;
}

double freya_cjson_strtod(const char *s, char **end)
{
    const char *p = s;
    char digit[DIGITS_MAX];
    uint64_t m = 0;
    int count = 0, exp10 = 0, neg = 0, any = 0, sticky = 0, point = 0;
    double v;

    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    for (;; p++) {
        if (*p == '.' && !point) {
            point = 1;
            continue;
        }
        if (*p < '0' || *p > '9') break;
        any = 1;
        if (count == 0 && *p == '0') {          /* a leading zero */
            if (point) exp10--;
        } else if (count < DIGITS_MAX) {
            digit[count++] = *p;
            if (point) exp10--;
        } else {
            if (!point) exp10++;
            if (*p != '0') sticky = 1;
        }
    }
    if (!any) {
        if (end) *end = (char *)s;
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, e = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (*q >= '0' && *q <= '9') {
            for (; *q >= '0' && *q <= '9'; q++)
                if (e < 100000) e = e * 10 + (*q - '0');
            exp10 += eneg ? -e : e;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    while (count && digit[count - 1] == '0') {   /* trailing zeros */
        count--;
        exp10++;
    }

    if (count == 0) v = 0.0;
    else if (exp10 + count > 310) v = HUGE_VAL;  /* past 1e309 */
    else if (exp10 + count < -324) v = 0.0;      /* under 1e-324 */
    else {
        int first = count < 19 ? count : 19;
        for (int i = 0; i < first; i++) m = m * 10 + (uint64_t)(digit[i] - '0');
        if (count <= 19 && !sticky && m <= (1ULL << 53) &&
            exp10 >= -22 && exp10 <= 22) {
            v = scale10((double)m, exp10);       /* exact: one rounding */
        } else {
            int e = exp10 + count - first;       /* the guess's exponent */
            big_t big;
            big_set(&big, 0);
            for (int i = 0; i < count; i++) {
                big_mul_small(&big, 10);
                big_add_small(&big, (uint32_t)(digit[i] - '0'));
            }
            v = e < -300 ? scale10(scale10((double)m, e + 300), -300)
                         : scale10((double)m, e);
            v = nearest(&big, exp10, sticky, v);
        }
    }
    return neg ? -v : v;
}

static char *put_unsigned(char *out, uint64_t v, int base, int width,
                          char pad, int upper)
{
    char text[24];
    int n = 0;

    do {
        int d = (int)(v % (unsigned)base);
        text[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= (unsigned)base;
    } while (v);
    while (width-- > n) *out++ = pad;
    while (n) *out++ = text[--n];
    return out;
}

/* %.<prec>g, as C prints it without the # flag, correctly rounded. */
static char *put_general(char *out, double d, int prec)
{
    bits_t bits;
    big_t num, den, half;
    char digit[17];
    uint64_t m;
    int e2, e10, count, top, c;

    if (prec < 1) prec = 1;
    if (prec > 17) prec = 17;
    bits.d = d;
    if (bits.u >> 63) *out++ = '-';
    bits.u &= ~(1ULL << 63);
    if (bits.u >= 0x7ff0000000000000ULL) {
        const char *s = bits.u > 0x7ff0000000000000ULL ? "nan" : "inf";
        while (*s) *out++ = *s++;
        return out;
    }
    if (bits.u == 0) {
        *out++ = '0';
        return out;
    }

    /* num / den is the value, scaled into [1, 10) by 10^-e10. */
    split(bits.d, &m, &e2);
    big_set(&num, m);
    big_set(&den, 1);
    if (e2 > 0) big_shl(&num, e2);
    else big_shl(&den, -e2);
    for (top = 63; !(m >> top); top--)
        ;
    e10 = ((top + e2) * 78913) >> 18;           /* floor(log10), or one less */
    if (e10 > 0) big_mul_pow10(&den, e10);
    else big_mul_pow10(&num, -e10);
    half = den;
    big_mul_small(&half, 10);
    if (big_cmp(&num, &half) >= 0) {
        big_mul_small(&den, 10);
        e10++;
    }
    if (big_cmp(&num, &den) < 0) {
        big_mul_small(&num, 10);
        e10--;
    }

    for (int i = 0; i < prec; i++) {
        int dgt = 0;
        while (big_cmp(&num, &den) >= 0) {
            big_sub(&num, &den);
            dgt++;
        }
        digit[i] = (char)('0' + dgt);
        big_mul_small(&num, 10);
    }
    /* num is now ten times the rest: round on it against den * 5. */
    half = den;
    big_mul_small(&half, 5);
    c = big_cmp(&num, &half);
    if (c > 0 || (c == 0 && ((digit[prec - 1] - '0') & 1))) {
        int i = prec - 1;
        while (i >= 0 && digit[i] == '9') digit[i--] = '0';
        if (i >= 0) {
            digit[i]++;
        } else {
            digit[0] = '1';
            e10++;
        }
    }
    count = prec;
    while (count > 1 && digit[count - 1] == '0') count--;

    if (e10 < -4 || e10 >= prec) {
        *out++ = digit[0];
        if (count > 1) {
            *out++ = '.';
            for (int i = 1; i < count; i++) *out++ = digit[i];
        }
        *out++ = 'e';
        *out++ = e10 < 0 ? '-' : '+';
        out = put_unsigned(out, (uint64_t)(e10 < 0 ? -e10 : e10), 10, 2, '0', 0);
    } else if (e10 >= 0) {
        for (int i = 0; i <= e10; i++) *out++ = digit[i];
        if (count > e10 + 1) {
            *out++ = '.';
            for (int i = e10 + 1; i < count; i++) *out++ = digit[i];
        }
    } else {
        *out++ = '0';
        *out++ = '.';
        for (int i = -1; i > e10; i--) *out++ = '0';
        for (int i = 0; i < count; i++) *out++ = digit[i];
    }
    return out;
}

/*
 * The conversions cJSON and cJSON_Utils use, and a little more: %d %i
 * %u %x %X with an l, a width and a 0 flag, %s, %c, %%, and %g with a
 * precision.
 */
int freya_cjson_sprintf(char *out, const char *fmt, ...)
{
    char *start = out;
    va_list ap;

    va_start(ap, fmt);
    while (*fmt) {
        int width = 0, prec = 6, lng = 0;
        char pad = ' ';

        if (*fmt != '%') {
            *out++ = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            prec = 0;
            for (fmt++; *fmt >= '0' && *fmt <= '9'; fmt++)
                prec = prec * 10 + (*fmt - '0');
        }
        while (*fmt == 'l') { lng = 1; fmt++; }
        switch (*fmt) {
        case 'd':
        case 'i': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            uint64_t u = (uint64_t)(v < 0 ? -(int64_t)v : v);
            if (v < 0) { *out++ = '-'; if (width) width--; }
            out = put_unsigned(out, u, 10, width, pad, 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            out = put_unsigned(out, v, *fmt == 'u' ? 10 : 16, width, pad,
                               *fmt == 'X');
            break;
        }
        case 'g':
            out = put_general(out, va_arg(ap, double), prec);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            while (*s) *out++ = *s++;
            break;
        }
        case 'c':
            *out++ = (char)va_arg(ap, int);
            break;
        case '%':
            *out++ = '%';
            break;
        default:
            va_end(ap);
            *out = '\0';
            return -1;
        }
        if (*fmt) fmt++;
    }
    va_end(ap);
    *out = '\0';
    return (int)(out - start);
}

/* Only "%lg", the one form cJSON reads a number back with. */
int freya_cjson_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    char *end;
    double v;

    if (freya_cjson_strcmp(fmt, "%lg") != 0) return -1;
    v = freya_cjson_strtod(s, &end);
    if (end == s) return 0;
    va_start(ap, fmt);
    *va_arg(ap, double *) = v;
    va_end(ap);
    return 1;
}
