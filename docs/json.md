# cJSON

`make` builds [cJSON](https://github.com/DaveGamble/cJSON) 1.7.19 as
`build/<board>/json/libfreya_cjson.a`, a library a program links, beside the
HTTP client library ([http.md](http.md)), which is how the JSON usually
arrives. It is built on the boards with the ESP32-C6 link and 192 KiB of
SRAM or more, which say `CJSON := 1` in their `board.mk`: the STM32U585,
STM32H523, STM32H562 and STM32H723. cJSON needs about six times the JSON
text in memory (see [Memory](#memory)), and the 96 and 128 KiB boards
have 36 to 52 KiB of heap, a document of 6 to 9 KB at best.

| | |
|---|---|
| `cJSON.h` | parse, print (indented or compact), build, find and change values |
| `cJSON_Utils.h` | JSON Pointer (RFC 6901), JSON Patch (RFC 6902), JSON Merge Patch (RFC 7386) |
| `freya_cjson.h` | includes both, and `freya_cjson_init()` |

The cJSON sources in `third_party/cjson` are upstream's, unchanged; their
API is the documentation (the comments in `cJSON.h`, and upstream's
README). The kernel does not carry the library: a program that does not
link it does not pay for it. It is about 15 KB of code; with
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
`freya_cjson_init_pool(api, pool, size)` does the same and gives cJSON
`size` bytes of the program's own as well, used before the heap: see
[Memory](#memory).

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

## Memory

cJSON takes a block for every value and one for every string, a key
included: about ten blocks of 19 bytes on average for an object like
`{"id":7,"name":"sensor-7","value":7.0}`. Measured on the Cortex-M4 under
QEMU, with the test document of such objects:

| | per object | for its 42 bytes of text |
|---|---|---|
| what cJSON asks for, the tree | 189 bytes | 4.5 × |
| with this library's allocator | about 245 bytes | 5.8 × |
| parse and `cJSON_Print()` at their peak | about 300 bytes | 7 × |

So a parse takes about six times the JSON text, and printing it back
needs room for the text twice more while the print buffer grows. With
the kernel heap alone, the boards hold:

| Board | SRAM | kernel heap | largest document parsed |
|---|---|---|---|
| STM32H523, STM32H723 | 272, 320 KiB | 78 KiB | about 13 KB of JSON |
| STM32H562 | 640 KiB | 117 KiB | about 20 KB |
| STM32U585 | 768 KiB | 242 KiB | about 42 KB |

That is the whole heap; what the kernel holds at the time (the shell's
variables, open files) comes off it.

**A pool.** The program region is larger than the heap on every one of
these boards: 168 KiB of RAM on the STM32H523, 216 KiB on the STM32H723,
504 KiB on the STM32U585 and STM32H562, all of it free for data in a
program installed in flash. `freya_cjson_init_pool(api, pool, size)`
gives cJSON memory of the program's own, a static array, which it uses
first and goes on into the heap when it is full. `samples/jsonget` keeps
a 112 KiB pool and its 24 KiB body buffer this way.

```c
static uint8_t pool[256 * 1024];          /* STM32U585: in the 504 KiB window */
freya_cjson_init_pool(api, pool, sizeof pool);
```

**The allocator.** The kernel lends a program at most 32 heap blocks at
a time, so the library does not ask it for each of cJSON's: it takes
chunks and divides them itself.

- A block has a 4-byte header, its size and two flags, and is 4-byte
  aligned. A Cortex-M loads a double from any word address, so 8-byte
  alignment would only waste room on blocks this small.
- A chunk is searched from where the last block was cut, joining free
  neighbours on the way: a parse, which only adds, finds room at once,
  three or four headers looked at for each block.
- A block that grows takes the free room after it when there is enough,
  as a print buffer usually can, instead of being copied.
- A new chunk is as large as all the others together, from 4 KiB up to
  64 KiB, and smaller when the heap has no room that large. A chunk that
  is all free again goes back to the kernel, so a run that has deleted
  its documents holds none.
- A request of more than 16 KiB that no chunk or pool has room for, such
  as the buffer of a long print, is a kernel block of its own.

A few thousand values then take a handful of the 32 blocks, and the rest
stay the program's. To print without the growing buffer, size one and
use `cJSON_PrintPreallocated()`. The allocator is not for threads: use
cJSON from one thread of a program at a time.

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
them weak, so a program's own win. `FREYA_CJSON_CHUNK` and
`FREYA_CJSON_CHUNK_MAX`, compiled into the library, set the first and the
largest chunk.
