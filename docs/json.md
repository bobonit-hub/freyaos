# cJSON

`make` builds [cJSON](https://github.com/DaveGamble/cJSON) 1.7.19 as
`build/<board>/json/libfreya_cjson.a`, a library a program links. It is
built on every board with the ESP32-C6 link, all but the Blue Pill, beside the
HTTP client library ([http.md](http.md)), which is how the JSON usually
arrives.

| | |
|---|---|
| `cJSON.h` | parse, print (indented or compact), build, find and change values |
| `cJSON_Utils.h` | JSON Pointer (RFC 6901), JSON Patch (RFC 6902), JSON Merge Patch (RFC 7386) |
| `freya_cjson.h` | includes both, and `freya_cjson_init()` |

The cJSON sources in `third_party/cjson` are upstream's, unchanged; their
API is the documentation (the comments in `cJSON.h`, and upstream's
README). The kernel does not carry the library: a program that does not
link it does not pay for it. It is about 14 KB of code; with
`--gc-sections` a program keeps the part it calls.

## Using it

In this tree a sample links it with two Makefile variables, as
`samples/jsonget` has them (with the HTTP library too):

```make
SMPL_CFLAGS_mysample := $(JSON_INC)
SMPL_LIBS_mysample   := $(JSON_LIB)
```

Call `freya_cjson_init(api)` once, before any other cJSON call. cJSON then
takes its memory with `api->malloc()` and gives it back with `api->free()`;
before it, every allocation fails, so `cJSON_Parse()` returns NULL.

```c
#include "freya_cjson.h"

int app_main(const freya_api_t *api, int argc, char **argv)
{
    freya_cjson_init(api);

    cJSON *doc = cJSON_Parse("{\"sensor\":\"t1\",\"values\":[21.5,21.75]}");
    if (!doc) return 1;
    cJSON *v = cJSONUtils_GetPointer(doc, "/values/1");
    api->printf("%s: %d.%02d\r\n",
                cJSON_GetObjectItem(doc, "sensor")->valuestring,
                (int)v->valuedouble, (int)(v->valuedouble * 100) % 100);
    cJSON_Delete(doc);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "board", "stm32u585");
    cJSON_AddNumberToObject(out, "uptime", api->ticks_ms() / 1000);
    char *text = cJSON_PrintUnformatted(out);  /* {"board":"stm32u585",...} */
    /* ... http_send(&h, text, strlen(text)) ... */
    cJSON_free(text);
    cJSON_Delete(out);
    return 0;
}
```

Free what cJSON hands out: a tree with `cJSON_Delete()`, a printed string
with `cJSON_free()`. Whatever a run leaves is the kernel's to reclaim when
it ends.

The kernel lends a program at most 32 heap blocks at a time, and cJSON
takes a block or two for every value, so the library does not ask the
kernel for each one. It takes chunks, the first of 4 KiB and each next one
twice the last up to 64 KiB, and divides them up itself; a chunk that is
all free again goes back. A request of more than 16 KiB, such as the
buffer of a long print, is a block of its own. A document of a few
thousand values then uses a handful of the 32 blocks, and the rest of them
stay the program's. A value costs about 50 bytes and its strings their
length; how large a document fits is up to the heap of the board.
`cJSON_PrintPreallocated()` prints into a buffer the program already has.

The nesting limit is upstream's, `CJSON_NESTING_LIMIT` (1000). cJSON
recurses once per level, so on these stacks a document nested more than a
few dozen levels deep is better refused by its source than parsed.

## Numbers

A JSON number is a `double` (`valuedouble`), and `valueint` the same value
cut to an `int`. On the Cortex-M4 and M33 boards double arithmetic is done
in software, through libgcc, as for any program.

Conversion is exact, as with a C library that rounds correctly:

- Reading gives the double nearest to the number, ties to even. The first
  40 significant digits are kept; a number of more digits that lies within
  10^-40 of a point halfway between two doubles may round the wrong way.
- Printing follows cJSON: an integer value as an integer, else 15
  significant digits, or 17 when 15 do not read back as the same number.
  The digits are correctly rounded, so a printed number reads back as the
  same double.

The test checks both against the host's C library on hundreds of
thousands of numbers ([tests.md](tests.md)). NaN and infinity print as
`null`, as upstream does.

## How it is built

A Freya program has no C library, and cJSON calls one: `malloc`, `free`
and `realloc`, string functions, `sprintf` and `sscanf` for numbers,
`strtod`, `fabs`, `tolower`. `json/port.c` has them. So that a program
that links the library gets no C library names it did not ask for, they
are all named `freya_cjson_*`: `json/cjson_port.h` is included ahead of
the cJSON sources (`-include`) and renames each call. The exceptions are
`memcpy` and `memset`, which GCC also calls on its own; `port.c` defines
them weak, so a program's own win. Each block `port.c` allocates keeps its
size in front of it, which `realloc()` needs to copy. `FREYA_CJSON_CHUNK`
and `FREYA_CJSON_CHUNK_MAX`, compiled into the library, set the first and
the largest chunk.
