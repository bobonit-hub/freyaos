/*
 * wget - fetch a URL with the HTTP client library (http/freya_http.h).
 *
 *     run("wget.bin", "https://example.com/")             print the body
 *     run("wget.bin", "https://example.com/", "/page.html") save it
 *
 * The ESP32-C6 has to be associated first: wifi("on"), wifi("connect").
 * Ctrl-C stops the transfer.  The status code, the length and the time
 * are printed when it ends.  The program links build/<board>/http/
 * libfreya_http.a; the kernel's curl is the same library.
 */
#include "freya_api.h"
#include "freya_http.h"

static freya_http_t h;
static uint8_t buf[FREYA_NET_PAYLOAD_MAX];

int app_main(const freya_api_t *api, int argc, char **argv)
{
    uint32_t start, total = 0;
    int fd = -1, rc;

    if (argc < 2 || argc > 3) {
        api->printf("usage: wget URL [file]\r\n");
        return 1;
    }
    if (!FREYA_API_HAS(api, net_poll)) {
        api->printf("wget: this kernel has no network calls\r\n");
        return 1;
    }
    start = api->ticks_ms();
    http_init(&h, api);
    rc = http_open(&h, "GET", argv[1]);
    if (rc == 0) rc = http_header(&h, "User-Agent", "Freya-wget/1.0");
    if (rc == 0) rc = http_response(&h);
    if (rc < 0) goto fail;
    if (argc == 3) {
        fd = api->open(argv[2], FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
        if (fd < 0) {
            api->printf("wget: cannot create %s\r\n", argv[2]);
            http_close(&h);
            return 1;
        }
    }
    for (;;) {
        int n = http_read(&h, buf, sizeof buf);
        if (n < 0) {
            rc = n;
            goto fail;
        }
        if (n == 0) break;
        if (fd >= 0) {
            if (api->write(fd, buf, n) != n) {
                api->printf("\r\nwget: cannot write %s\r\n", argv[2]);
                rc = FREYA_ERR_IO;
                goto fail;
            }
        } else {
            for (int i = 0; i < n; i++) api->putc((char)buf[i]);
        }
        total += (uint32_t)n;
    }
    if (fd >= 0) api->close(fd);
    http_close(&h);
    api->printf("\r\nwget: HTTP %d, %lu bytes in %lu ms\r\n", h.status,
                (unsigned long)total, (unsigned long)(api->ticks_ms() - start));
    return h.status >= 200 && h.status < 300 ? 0 : 1;

fail:
    if (fd >= 0) api->close(fd);
    http_close(&h);
    api->printf("\r\nwget: error %d after %lu bytes\r\n", rc, (unsigned long)total);
    return 1;
}
