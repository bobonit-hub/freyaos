/*
 * Freya - cJSON for programs (build/<board>/json/libfreya_cjson.a).
 *
 * cJSON v1.7.19 (third_party/cjson), its parser, printer and object
 * calls, and cJSON_Utils: JSON Pointer (RFC 6901), JSON Patch (RFC 6902)
 * and JSON Merge Patch (RFC 7386).  Their own API is unchanged; see
 * cJSON.h and cJSON_Utils.h.  Built on the boards with the ESP32-C6
 * link and 192 KiB of SRAM or more (CJSON := 1 in board.mk); see
 * docs/json.md.
 *
 * Call freya_cjson_init(api) once before any cJSON call: cJSON's memory
 * then comes from api->malloc() and goes back with api->free(), or first
 * from a pool of the program's own with freya_cjson_init_pool().  Before
 * it, every allocation fails, so a parse returns NULL.  Memory the run
 * did not free is the kernel's to reclaim when it ends.
 *
 * Numbers are doubles, converted as a C library does: a number is read
 * to the nearest double, and printed correctly rounded to 15 significant
 * digits, or 17 when 15 do not read back as the same number.  Only the
 * first 40 significant digits of a number are kept exactly.
 */
#ifndef FREYA_CJSON_H
#define FREYA_CJSON_H

#include "freya_api.h"
#include "cJSON.h"
#include "cJSON_Utils.h"

void freya_cjson_init(const freya_api_t *api);

/* The same, and size bytes at pool for cJSON to use before the heap:
 * a static array, or the RAM a program installed in flash leaves free.
 * The pool is not handed back; the program must not use it otherwise
 * while cJSON holds values. */
void freya_cjson_init_pool(const freya_api_t *api, void *pool, uint32_t size);

#endif /* FREYA_CJSON_H */
