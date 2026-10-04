/*
 * jsonget - fetch JSON over HTTP(S) and print it, or one value of it.
 *
 *     run("jsonget.bin", URL)                print the whole document
 *     run("jsonget.bin", URL, "/a/0/b")      print the value at that
 *                                            JSON Pointer (RFC 6901)
 *
 * The body is read with the HTTP client library (http/freya_http.h)
 * into the heap, up to 64 KiB, and parsed with cJSON (json/freya_cjson.h).
 * A string is printed bare, anything else as JSON.  The ESP32-C6 has to
 * be associated first: wifi("on"), wifi("connect").
 */
#include "freya_api.h"
#include "freya_http.h"
#include "freya_cjson.h"

#define BODY_MAX (64 * 1024)

static freya_http_t h;

/* The body of a GET, NUL-terminated, from the heap; NULL on failure. */
static char *fetch(const freya_api_t *api, const char *url)
{
    char *body = NULL;
    uint32_t len = 0, cap = 0;
    int rc;

    http_init(&h, api);
    rc = http_open(&h, "GET", url);
    if (rc == 0) rc = http_header(&h, "Accept", "application/json");
    if (rc == 0) rc = http_header(&h, "User-Agent", "Freya-jsonget/1.0");
    if (rc == 0) rc = http_response(&h);
    if (rc < 0) {
        api->printf("jsonget: error %d\r\n", rc);
        goto fail;
    }
    if (h.status < 200 || h.status >= 300) {
        api->printf("jsonget: HTTP %d\r\n", h.status);
        goto fail;
    }
    for (;;) {
        if (len + 1 >= cap) {
            uint32_t want = cap ? cap * 2 : 2048;
            char *more;
            if (want > BODY_MAX) want = BODY_MAX;
            if (len + 1 >= want) {
                api->printf("jsonget: the body is over %u bytes\r\n", BODY_MAX);
                goto fail;
            }
            more = api->malloc(want);
            if (!more) {
                api->printf("jsonget: out of memory at %lu bytes\r\n",
                            (unsigned long)len);
                goto fail;
            }
            for (uint32_t i = 0; i < len; i++) more[i] = body[i];
            if (body) api->free(body);
            body = more;
            cap = want;
        }
        rc = http_read(&h, body + len, (int)(cap - 1 - len < 480 ? cap - 1 - len : 480));
        if (rc < 0) {
            api->printf("jsonget: error %d\r\n", rc);
            goto fail;
        }
        if (rc == 0) break;
        len += (uint32_t)rc;
    }
    http_close(&h);
    body[len] = '\0';
    return body;

fail:
    http_close(&h);
    if (body) api->free(body);
    return NULL;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    cJSON *doc, *item;
    char *body, *text;
    int status = 1;

    if (argc < 2 || argc > 3) {
        api->printf("usage: jsonget URL [/json/pointer]\r\n");
        return 1;
    }
    freya_cjson_init(api);
    body = fetch(api, argv[1]);
    if (!body) return 1;
    doc = cJSON_Parse(body);
    if (!doc) {
        const char *at = cJSON_GetErrorPtr();
        api->printf("jsonget: not JSON, at byte %ld\r\n",
                    at ? (long)(at - body) : -1L);
        api->free(body);
        return 1;
    }
    api->free(body);
    item = argc == 3 ? cJSONUtils_GetPointer(doc, argv[2]) : doc;
    if (!item) {
        api->printf("jsonget: no value at %s\r\n", argv[2]);
    } else if (cJSON_IsString(item)) {
        api->printf("%s\r\n", cJSON_GetStringValue(item));
        status = 0;
    } else if ((text = cJSON_Print(item)) != NULL) {
        for (const char *p = text; *p; p++) {    /* the console wants CR LF */
            if (*p == '\n') api->putc('\r');
            api->putc(*p);
        }
        api->printf("\r\n");
        cJSON_free(text);
        status = 0;
    }
    cJSON_Delete(doc);
    return status;
}
