#include "freya.h"

#define KEXT __attribute__((noinline, section(".text.kext_script")))
#define CURL_TIMEOUT_MS 30000U

#if BOARD_ESP_LINK

static int KEXT curl_wait_info(freya_http_info_t *info)
{
    uint32_t start = sys_ticks();
    int rc;

    do {
        rc = net_http_info(info);
        if (rc != FREYA_ERR_AGAIN) return rc;
        if (net_poll(20) < 0 || uart_take_ctrlc()) return FREYA_ERR_IO;
    } while ((uint32_t)(sys_ticks() - start) < CURL_TIMEOUT_MS);
    return FREYA_ERR_TIMEOUT;
}

static int KEXT curl_write_all(int fd, const void *buf, int length)
{
    const uint8_t *p = buf;

    if (fd < 0) {
        uart_write(buf, length);
        return length;
    }
    while (length) {
        int n = fs_fd_write(fd, p, length);
        if (n <= 0) return -1;
        p += n;
        length -= n;
    }
    return 0;
}

int KEXT cmd_curl(int argc, char **argv)
{
    const char *url = NULL, *basic = "", *data = "", *output = NULL;
    const char *user_agent = "Freya-curl/1.0";
    freya_http_info_t info;
    uint8_t flags = 0;
    uint8_t body[FREYA_NET_PAYLOAD_MAX];
    uint32_t received = 0;
    uint8_t last = '\n';
    int fd = -1, rc;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--compressed") == 0)
            flags |= HTTP_FLAG_COMPRESSED;
        else if (strcmp(arg, "--insecure") == 0)
            flags |= HTTP_FLAG_INSECURE;
        else if (strcmp(arg, "--verbose") == 0)
            flags |= HTTP_FLAG_VERBOSE;
        else if (strcmp(arg, "--basic") == 0 || strcmp(arg, "--data") == 0 ||
                 strcmp(arg, "--output") == 0 ||
                 strcmp(arg, "--user-agent") == 0) {
            if (++i >= argc) goto usage;
            if (strcmp(arg, "--basic") == 0) basic = argv[i];
            else if (strcmp(arg, "--data") == 0) data = argv[i];
            else if (strcmp(arg, "--output") == 0) output = argv[i];
            else user_agent = argv[i];
        } else if (arg[0] == '-' || url) {
            goto usage;
        } else {
            url = arg;
        }
    }
    if (!url) goto usage;
    if (strncmp(url, "http://", 7) != 0 &&
        strncmp(url, "https://", 8) != 0) {
        kprintf("curl: URL must start with http:// or https://\r\n");
        return -1;
    }
    if (basic[0] && !strchr(basic, ':')) {
        kprintf("curl: --basic needs user:password\r\n");
        return -1;
    }
    if (flags & HTTP_FLAG_VERBOSE) {
        kprintf("> %s %s\r\n", data[0] ? "POST" : "GET", url);
        kprintf("> User-Agent: %s\r\n", user_agent);
        if (flags & HTTP_FLAG_COMPRESSED)
            kprintf("> Accept-Encoding: gzip\r\n");
        if (flags & HTTP_FLAG_INSECURE)
            kprintf("* TLS certificate verification disabled\r\n");
    }

    do {
        rc = net_http_start(flags, url, user_agent, basic, data);
        if (rc != FREYA_ERR_AGAIN) break;
        if (net_poll(20) < 0 || uart_take_ctrlc()) return -1;
    } while (1);
    if (rc != 0) goto fail;
    rc = curl_wait_info(&info);
    if (rc != 0) goto fail_close;

    if (flags & HTTP_FLAG_VERBOSE) {
        kprintf("< HTTP status %u\r\n", info.status);
        if (info.header_length) uart_write(info.headers, info.header_length);
        kprintf("< body: %u bytes\r\n", info.body_length);
    }
    if (output) {
        fd = fs_fd_open(output, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
        if (fd < 0) {
            kprintf("curl: cannot create %s\r\n", output);
            goto fail_close;
        }
    }
    for (;;) {
        do {
            rc = net_http_read(body, sizeof body);
            if (rc != FREYA_ERR_AGAIN) break;
            if (net_poll(20) < 0 || uart_take_ctrlc()) goto fail_output;
        } while (1);
        if (rc < 0) goto fail_output;
        if (rc == 0) break;
        if (curl_write_all(fd, body, rc) < 0) goto fail_output;
        last = body[rc - 1];
        received += (uint32_t)rc;
    }
    if (fd >= 0) fs_fd_close(fd);
    net_http_close();
    if (!output && received && last != '\n')
        kprintf("\r\n");
    return info.status >= 200 && info.status < 400 ? 0 : -1;

fail_output:
    if (fd >= 0) fs_fd_close(fd);
fail_close:
    net_http_close();
fail:
    kprintf("curl: error %d\r\n", rc);
    return -1;
usage:
    kprintf("usage: curl [--basic user:password] [--compressed] [--data text] "
            "[--output file] [--user-agent text] [--insecure] [--verbose] URL\r\n");
    return -1;
}

#else

int KEXT cmd_curl(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("curl: network unsupported on this board\r\n");
    return -1;
}

#endif
