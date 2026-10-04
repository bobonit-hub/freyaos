/*
 * Freya - what cJSON calls from a C library, renamed to json/port.c.
 *
 * The Makefile includes this ahead of third_party/cjson/cJSON.c and
 * cJSON_Utils.c (-include), so the upstream files stay unchanged.  The
 * system headers come first, so that their own declarations and macros
 * are in place before the names below take over; cJSON's includes of
 * them later find their guards set.
 *
 * Everything is renamed but memcpy and memset, which GCC also calls on
 * its own for copies and clears.  port.c defines those two weak, so a
 * program's own copies win.  A program that links the library gets no
 * sprintf, strtod or malloc of the C library's names from it.
 */
#ifndef FREYA_CJSON_PORT_H
#define FREYA_CJSON_PORT_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#undef tolower
#undef sprintf
#undef sscanf
#undef strtod
#undef malloc
#undef free
#undef realloc
#undef strlen
#undef strcmp
#undef strncmp
#undef strcpy
#undef strcat
#undef strrchr
#undef fabs

#define tolower  freya_cjson_tolower
#define sprintf  freya_cjson_sprintf
#define sscanf   freya_cjson_sscanf
#define strtod   freya_cjson_strtod
#define malloc   freya_cjson_malloc
#define free     freya_cjson_free
#define realloc  freya_cjson_realloc
#define strlen   freya_cjson_strlen
#define strcmp   freya_cjson_strcmp
#define strncmp  freya_cjson_strncmp
#define strcpy   freya_cjson_strcpy
#define strcat   freya_cjson_strcat
#define strrchr  freya_cjson_strrchr
#define fabs     freya_cjson_fabs

int     freya_cjson_tolower(int c);
int     freya_cjson_sprintf(char *out, const char *fmt, ...);
int     freya_cjson_sscanf(const char *s, const char *fmt, ...);
double  freya_cjson_strtod(const char *s, char **end);
void   *freya_cjson_malloc(size_t size);
void    freya_cjson_free(void *p);
void   *freya_cjson_realloc(void *p, size_t size);
size_t  freya_cjson_strlen(const char *s);
int     freya_cjson_strcmp(const char *a, const char *b);
int     freya_cjson_strncmp(const char *a, const char *b, size_t n);
char   *freya_cjson_strcpy(char *dst, const char *src);
char   *freya_cjson_strcat(char *dst, const char *src);
char   *freya_cjson_strrchr(const char *s, int c);
double  freya_cjson_fabs(double v);

#endif /* FREYA_CJSON_PORT_H */
