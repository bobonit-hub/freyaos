/*
 * Freya - freestanding string and memory helpers.
 * Freya links with -nostdlib, so the compiler's implicit calls to
 * memcpy/memset land here.
 */
#include "freya.h"

/* Keep GCC from turning these loops into calls to themselves. */
#define NO_LOOP_IDIOM __attribute__((optimize("no-tree-loop-distribute-patterns")))

NO_LOOP_IDIOM void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (((uintptr_t)d | (uintptr_t)s) % 4 == 0) {
        uint32_t *d4 = (uint32_t *)d;
        const uint32_t *s4 = (const uint32_t *)s;
        while (n >= 4) { *d4++ = *s4++; n -= 4; }
        d = (uint8_t *)d4;
        s = (const uint8_t *)s4;
    }
    while (n--) *d++ = *s++;
    return dst;
}

NO_LOOP_IDIOM void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);
    d += n;
    s += n;
    while (n--) *--d = *--s;
    return dst;
}

NO_LOOP_IDIOM void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint8_t v = (uint8_t)c;

    if ((uintptr_t)d % 4 == 0 && n >= 4) {
        uint32_t w = (uint32_t)v * 0x01010101UL;
        uint32_t *d4 = (uint32_t *)d;
        while (n >= 4) { *d4++ = w; n -= 4; }
        d = (uint8_t *)d4;
    }
    while (n--) *d++ = v;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

char to_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int strcasecmp(const char *a, const char *b)
{
    while (*a && to_upper(*a) == to_upper(*b)) { a++; b++; }
    return (int)(uint8_t)to_upper(*a) - (int)(uint8_t)to_upper(*b);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++)
        if (*s == (char)c) last = s;
    return (char *)last;
}

/*
 * Parses decimal, 0x hex and 0b binary.  Returns 0 on success.
 */
int str_to_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0, base = 10;
    int digits = 0;

    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }

    for (; *s; s++) {
        uint32_t d;
        char c = to_lower(*s);
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
