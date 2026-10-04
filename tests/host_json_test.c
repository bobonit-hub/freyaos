/*
 * cJSON as Freya builds it (json/, third_party/cjson) on the host: the
 * upstream sources compiled with json/cjson_port.h in front, so every C
 * library call they make goes to json/port.c, against a service table
 * whose malloc and free count what is outstanding and, as the kernel
 * does, refuse a 33rd block.  The number
 * conversions are then checked against the host C library's, which
 * rounds correctly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "freya_cjson.h"

int    freya_cjson_sprintf(char *out, const char *fmt, ...);
double freya_cjson_strtod(const char *s, char **end);

static long blocks, allocs;

/* The kernel lends a program 32 blocks at a time (src/loader.c). */
static void *m_malloc(uint32_t size)
{
    if (blocks >= 32) return NULL;
    blocks++;
    allocs++;
    return malloc(size);
}

static void m_free(void *p)
{
    if (p) blocks--;
    free(p);
}

static freya_api_t api = {
    .size = sizeof(freya_api_t),
    .malloc = m_malloc,
    .free = m_free,
};

static int checks, fails;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fails++; printf("  FAIL  %s\n", what); }
    else printf("  ok    %s\n", what);
}

/* cJSON_PrintUnformatted() compared with want, and freed. */
static int prints(const cJSON *item, const char *want)
{
    char *text = cJSON_PrintUnformatted(item);
    int same = text && strcmp(text, want) == 0;

    if (!same) printf("        got %s\n        want %s\n", text ? text : "(null)", want);
    cJSON_free(text);
    return same;
}

static uint64_t rnd(void)
{
    static uint64_t x = 0x2545F4914F6CDD1DULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return x;
}

int main(void)
{
    cJSON *doc, *item;
    char *text;

    check(cJSON_Parse("{}") == NULL && allocs == 0,
          "before freya_cjson_init nothing is allocated, so nothing parses");
    freya_cjson_init(&api);

    doc = cJSON_Parse(" {\"name\": \"Freya\", \"boards\": [\"blackpill\", \"stm32h723\"],"
                      " \"flash\": 524288, \"ratio\": 0.1, \"on\": true, \"none\": null,"
                      " \"nested\": {\"a\": [1, 2, {\"b\": -3.5e-3}]}} ");
    check(doc != NULL, "a document parses");
    check(strcmp(cJSON_GetObjectItem(doc, "name")->valuestring, "Freya") == 0 &&
          cJSON_GetArraySize(cJSON_GetObjectItem(doc, "boards")) == 2 &&
          cJSON_GetObjectItem(doc, "flash")->valueint == 524288 &&
          cJSON_GetObjectItem(doc, "ratio")->valuedouble == 0.1 &&
          cJSON_IsTrue(cJSON_GetObjectItem(doc, "on")) &&
          cJSON_IsNull(cJSON_GetObjectItem(doc, "none")),
          "its values read back");
    item = cJSONUtils_GetPointer(doc, "/nested/a/2/b");
    check(item && item->valuedouble == -3.5e-3, "a JSON Pointer finds a value");
    check(prints(doc, "{\"name\":\"Freya\",\"boards\":[\"blackpill\",\"stm32h723\"],"
                      "\"flash\":524288,\"ratio\":0.1,\"on\":true,\"none\":null,"
                      "\"nested\":{\"a\":[1,2,{\"b\":-0.0035}]}}"),
          "it prints back the same, numbers in their shortest form");
    text = cJSON_Print(doc);
    check(text && strstr(text, "{\n\t\"name\":\t\"Freya\",\n") == text,
          "the formatted print indents with tabs");
    cJSON_free(text);
    cJSON_Delete(doc);
    check(blocks == 0, "deleting the document frees every block");

    /* Numbers, against cJSON's rule worked with the host's sprintf and
     * sscanf: an integer as %d, else 15 digits when they read back the
     * same (by cJSON's tolerance), else 17. */
    {
        static const double num[] = {
            0.1, -0.0, 1e300, 123456789012345678.0, 1.0 / 3.0,
            2.2250738585072014e-308, 4.9406564584124654e-324,
            1.7976931348623157e308, 25.5, 1e-7, 100000, 2147483648.0, -42,
            0.30000000000000004, 6154.804939965695, -2.5e-310,
        };
        int all = 1;
        for (size_t i = 0; i < sizeof num / sizeof num[0]; i++) {
            char want[32];
            double back;
            cJSON *n = cJSON_CreateNumber(num[i]);
            if (num[i] == (double)n->valueint) {
                sprintf(want, "%d", n->valueint);
            } else {
                sprintf(want, "%1.15g", num[i]);
                if (sscanf(want, "%lg", &back) != 1 ||
                    !(fabs(back - num[i]) <=
                      (fabs(back) > fabs(num[i]) ? fabs(back) : fabs(num[i])) * 2.220446049250313e-16))
                    sprintf(want, "%1.17g", num[i]);
            }
            all &= prints(n, want);
            cJSON_Delete(n);
        }
        check(all, "numbers print as cJSON prints them with a C library");
        doc = cJSON_CreateNumber(0.0 / 0.0);
        check(prints(doc, "null"), "NaN prints as null");
        cJSON_Delete(doc);
    }

    /* Strings: escapes in, UTF-8 out, control characters escaped. */
    doc = cJSON_Parse("\"caf\\u00e9 \\ud83d\\ude00 \\\"q\\\" \\t\"");
    check(doc && strcmp(doc->valuestring, "caf\xc3\xa9 \xf0\x9f\x98\x80 \"q\" \t") == 0,
          "\\u escapes and surrogate pairs decode to UTF-8");
    cJSON_Delete(doc);
    doc = cJSON_CreateString("a\x01" "b\n");
    check(prints(doc, "\"a\\u0001b\\n\""), "control characters print escaped");
    cJSON_Delete(doc);

    /* Thousands of values in a few kernel blocks, and a print that
     * outgrows its first buffer and goes through realloc(). */
    doc = cJSON_CreateArray();
    for (int i = 0; i < 2000; i++) cJSON_AddItemToArray(doc, cJSON_CreateNumber(i * 1.5));
    check(cJSON_GetArraySize(doc) == 2000 && blocks < 32,
          "2000 values fit in the kernel's 32 blocks");
    text = cJSON_PrintUnformatted(doc);
    check(text && strlen(text) > 10000 && strncmp(text, "[0,1.5,3,4.5,", 13) == 0 &&
          strcmp(text + strlen(text) - 8, ",2998.5]") == 0,
          "a large print grows its buffer and stays whole");
    cJSON_free(text);
    {
        /* parse the print of an object with strings of every size back */
        cJSON *obj = cJSON_CreateObject(), *back;
        char key[16], value[600];
        for (int i = 0; i < 300; i++) {
            int n = (int)(rnd() % sizeof value);
            for (int k = 0; k < n; k++) value[k] = (char)('a' + k % 26);
            value[n] = '\0';
            snprintf(key, sizeof key, "k%d", i);
            cJSON_AddStringToObject(obj, key, value);
        }
        text = cJSON_PrintUnformatted(obj);
        back = text ? cJSON_Parse(text) : NULL;
        check(back && cJSON_Compare(obj, back, 1),
              "300 strings of 0 to 600 bytes print and parse back the same");
        cJSON_free(text);
        cJSON_Delete(back);
        cJSON_Delete(obj);
    }
    cJSON_Delete(doc);
    check(blocks == 0, "chunks that are free again go back to the kernel");

    /* Building, and the Utils' patches with their "%lu" paths. */
    {
        cJSON *from = cJSON_Parse("{\"a\":[1,2,3],\"b\":\"x\"}");
        cJSON *to = cJSON_Parse("{\"a\":[1,5],\"c\":true}");
        cJSON *patch = cJSONUtils_GeneratePatches(from, to);
        check(patch && cJSONUtils_ApplyPatches(from, patch) == 0 &&
              cJSON_Compare(from, to, 1),
              "a JSON Patch made from two documents turns one into the other");
        cJSON_Delete(patch);
        cJSON_Delete(from);
        cJSON_Delete(to);
        from = cJSON_Parse("{\"a\":\"b\",\"c\":{\"d\":\"e\",\"f\":\"g\"}}");
        patch = cJSON_Parse("{\"a\":\"z\",\"c\":{\"f\":null}}");
        from = cJSONUtils_MergePatch(from, patch);
        check(prints(from, "{\"a\":\"z\",\"c\":{\"d\":\"e\"}}"), "a Merge Patch applies");
        cJSON_Delete(patch);
        cJSON_Delete(from);
        doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "board", "stm32u585");
        cJSON_AddNumberToObject(doc, "mhz", 160);
        cJSON_AddItemToObject(doc, "pins", cJSON_CreateIntArray((const int[]){ 1, 2 }, 2));
        check(prints(doc, "{\"board\":\"stm32u585\",\"mhz\":160,\"pins\":[1,2]}"),
              "a document built in code prints");
        cJSON_Delete(doc);
    }

    {
        /* larger than a 4 KiB chunk, kept in a chunk all the same */
        static char big[12000];
        memset(big, 'x', sizeof big - 1);
        doc = cJSON_CreateString(big);
        check(doc && strlen(doc->valuestring) == sizeof big - 1,
              "a 12000-byte string is held");
        cJSON_Delete(doc);
    }
    check(cJSON_Parse("{\"a\":}") == NULL && cJSON_Parse("[1,]") == NULL &&
          cJSON_Parse("") == NULL, "broken JSON is refused");
    check(blocks == 0, "and every block is back");
    {
        char version[16];
        sprintf(version, "%d.%d.%d", CJSON_VERSION_MAJOR, CJSON_VERSION_MINOR,
                CJSON_VERSION_PATCH);
        check(strcmp(cJSON_Version(), version) == 0, "cJSON_Version formats with the port");
    }

    /* The conversions themselves, against the host's. */
    {
        long bad_print = 0, bad_read = 0;
        for (int i = 0; i < 200000; i++) {
            uint64_t u = rnd();
            double d;
            char ours[40], host[40];
            memcpy(&d, &u, sizeof d);
            if (d != d || d - d != 0) continue;
            for (int p = 15; p <= 17; p += 2) {
                freya_cjson_sprintf(ours, p == 15 ? "%1.15g" : "%1.17g", d);
                sprintf(host, "%1.*g", p, d);
                bad_print += strcmp(ours, host) != 0;
            }
            snprintf(host, sizeof host, "%.*e", (int)(rnd() % 25), d);
            {
                double a = freya_cjson_strtod(host, NULL), b = strtod(host, NULL);
                bad_read += memcmp(&a, &b, sizeof a) != 0;
            }
        }
        check(bad_print == 0, "%1.15g and %1.17g match the host's on random doubles");
        check(bad_read == 0, "strtod matches the host's on 1 to 25 digits");
    }
    {
        /* halfway between two doubles, written out exactly */
        static const char *tie[] = {
            "9007199254740993",                 /* 2^53 + 1: down to even */
            "9007199254740995",                 /* 2^53 + 3: up to even */
            "9007199254740993.0000000000000000000000000000000001",
            "2.4703282292062327e-324",          /* under 2^-1075: to 0 */
            "2.4703282292062328e-324",          /* over: the least subnormal */
            "1.7976931348623158e308",           /* under the top: DBL_MAX */
            "1.7976931348623159e308",           /* over: infinity */
            "1e-400", "1e400", "-0.0", "0.000000000000000000000000000000000000001e-300",
        };
        int all = 1;
        for (size_t i = 0; i < sizeof tie / sizeof tie[0]; i++) {
            double a = freya_cjson_strtod(tie[i], NULL), b = strtod(tie[i], NULL);
            if (memcmp(&a, &b, sizeof a) != 0) {
                printf("        %s: %.17g, host %.17g\n", tie[i], a, b);
                all = 0;
            }
        }
        check(all, "ties, the subnormal edge and the ends of the range read right");
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
