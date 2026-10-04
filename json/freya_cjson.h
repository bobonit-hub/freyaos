/*
 * Freya - cJSON for programs (build/<board>/json/libfreya_cjson.a).
 *
 * cJSON v1.7.19 (third_party/cjson), its parser, printer and object
 * calls, and cJSON_Utils: JSON Pointer (RFC 6901), JSON Patch (RFC 6902)
 * and JSON Merge Patch (RFC 7386).  Their own API is unchanged; see
 * cJSON.h and cJSON_Utils.h.  Built on every board with the ESP32-C6
 * link; see docs/json.md.
 *
 * Call freya_cjson_init(api) once before any cJSON call: cJSON's memory
 * then comes from api->malloc() and goes back with api->free().  Before
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

#endif /* FREYA_CJSON_H */
