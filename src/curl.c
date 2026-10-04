#include "freya.h"
#include "freya_http.h"

#define KEXT __attribute__((noinline, section(".text.kext_script")))
#define CURL_HEADERS_MAX   8
#define CURL_REDIRECTS_MAX 5
#define CURL_URL_MAX       256

#if BOARD_ESP_LINK

typedef struct {
    int  verbose;
    char location[CURL_URL_MAX];
} curl_ctx_t;

static int KEXT curl_cancel(void *ctx)
{
    (void)ctx;
    return uart_take_ctrlc();
}

static void KEXT curl_header(void *arg, const char *line)
{
    static const char name[] = "location:";
    curl_ctx_t *ctx = arg;
    int i = 0;

    if (ctx->verbose) kprintf("< %s\r\n", line);
    while (name[i] && (line[i] | 0x20) == name[i]) i++;
    if (!name[i]) {
        line += i;
        while (*line == ' ' || *line == '\t') line++;
        strncpy(ctx->location, line, sizeof ctx->location - 1);
        ctx->location[sizeof ctx->location - 1] = '\0';
    }
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

/*
 * The URL a redirect goes to, written into next: ctx->location when it
 * is absolute, or the scheme, host and port of url in front of it when
 * it is an absolute path.  -1 for any other form, or one too long.
 */
static int KEXT curl_follow(const char *url, const char *location, char *next)
{
    size_t origin, n = strlen(location);

    if (strncmp(location, "http://", 7) == 0 ||
        strncmp(location, "https://", 8) == 0) {
        if (n >= CURL_URL_MAX) return -1;
        memmove(next, location, n + 1);
        return 0;
    }
    if (location[0] != '/' || location[1] == '/') return -1;
    origin = strchr(url, ':') - url + 3;
    while (url[origin] && url[origin] != '/' && url[origin] != '?' &&
           url[origin] != '#')
        origin++;
    if (origin + n >= CURL_URL_MAX) return -1;
    memmove(next + origin, location, n + 1);
    memmove(next, url, origin);
    return 0;
}

/* Sends the request with its headers and body, and reads the head. */
static int KEXT curl_request(freya_http_t *h, const char *method,
                             const char *url, const char *user_agent,
                             const char *basic, const char *data,
                             const char **headers, int header_count,
                             int verbose)
{
    char name[48];
    int rc;

    if (verbose) {
        kprintf("> %s %s\r\n", method, url);
        kprintf("> User-Agent: %s\r\n", user_agent);
        for (int i = 0; i < header_count; i++)
            kprintf("> %s\r\n", headers[i]);
    }
    rc = http_open(h, method, url);
    if (rc) return rc;
    rc = http_header(h, "User-Agent", user_agent);
    if (!rc) rc = http_header(h, "Accept", "*/*");
    if (!rc && basic[0]) rc = http_basic_auth(h, basic);
    for (int i = 0; !rc && i < header_count; i++) {
        const char *colon = strchr(headers[i], ':');
        const char *value = colon + 1;
        size_t n = (size_t)(colon - headers[i]);

        memcpy(name, headers[i], n);
        name[n] = '\0';
        while (*value == ' ' || *value == '\t') value++;
        rc = http_header(h, name, value);
    }
    if (!rc && data) {
        if (verbose) kprintf("> body: %u bytes\r\n", (unsigned)strlen(data));
        rc = http_send(h, data, strlen(data));
    }
    if (!rc) rc = http_response(h);
    return rc;
}

int KEXT cmd_curl(int argc, char **argv)
{
    const char *url = NULL, *basic = "", *data = NULL, *output = NULL;
    const char *method = NULL, *user_agent = "Freya-curl/2.0";
    const char *headers[CURL_HEADERS_MAX];
    char next[CURL_URL_MAX];
    curl_ctx_t ctx;
    freya_http_t h;
    uint8_t body[256];
    uint32_t received = 0;
    uint8_t last = '\n';
    int header_count = 0, location = 0, redirects = 0;
    int fd = -1, rc;

    ctx.verbose = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--verbose") == 0)
            ctx.verbose = 1;
        else if (strcmp(arg, "--location") == 0)
            location = 1;
        else if (strcmp(arg, "--basic") == 0 || strcmp(arg, "--data") == 0 ||
                 strcmp(arg, "--output") == 0 ||
                 strcmp(arg, "--user-agent") == 0 ||
                 strcmp(arg, "--request") == 0 ||
                 strcmp(arg, "--header") == 0) {
            if (++i >= argc) goto usage;
            if (strcmp(arg, "--basic") == 0) basic = argv[i];
            else if (strcmp(arg, "--data") == 0) data = argv[i];
            else if (strcmp(arg, "--output") == 0) output = argv[i];
            else if (strcmp(arg, "--request") == 0) method = argv[i];
            else if (strcmp(arg, "--user-agent") == 0) user_agent = argv[i];
            else {
                const char *colon = strchr(argv[i], ':');
                if (header_count == CURL_HEADERS_MAX || !colon ||
                    colon == argv[i] || colon - argv[i] >= 48) {
                    kprintf("curl: --header needs \"Name: value\", at most %d\r\n",
                            CURL_HEADERS_MAX);
                    return -1;
                }
                headers[header_count++] = argv[i];
            }
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
    if (!method) method = data ? "POST" : "GET";

    http_init(&h, app_api());
    h.cancel = curl_cancel;
    h.header = curl_header;
    h.ctx = &ctx;
    for (;;) {
        ctx.location[0] = '\0';
        rc = curl_request(&h, method, url, user_agent, basic, data,
                          headers, header_count, ctx.verbose);
        if (rc < 0) goto fail;
        if (!location || !ctx.location[0] ||
            (rc != 301 && rc != 302 && rc != 303 && rc != 307 && rc != 308))
            break;
        http_close(&h);
        if (++redirects > CURL_REDIRECTS_MAX) {
            kprintf("curl: more than %d redirects\r\n", CURL_REDIRECTS_MAX);
            return -1;
        }
        if (curl_follow(url, ctx.location, next) != 0) {
            kprintf("curl: cannot follow Location: %s\r\n", ctx.location);
            return -1;
        }
        url = next;
        /* As curl does: 303 always, and 301 and 302 after a POST, go on
         * as a GET without the body. */
        if (rc == 303 || ((rc == 301 || rc == 302) &&
                          strcmp(method, "POST") == 0)) {
            method = "GET";
            data = NULL;
        }
    }

    if (ctx.verbose) {
        if (h.length >= 0) kprintf("< body: %ld bytes\r\n", (long)h.length);
        else kprintf("< body: length not given\r\n");
    }
    if (output) {
        fd = fs_fd_open(output, FREYA_O_WRONLY | FREYA_O_CREATE | FREYA_O_TRUNC);
        if (fd < 0) {
            kprintf("curl: cannot create %s\r\n", output);
            http_close(&h);
            return -1;
        }
    }
    for (;;) {
        int n = http_read(&h, body, sizeof body);
        if (n < 0) {
            rc = n;
            goto fail_output;
        }
        if (n == 0) break;
        if (curl_write_all(fd, body, n) < 0) {
            kprintf("curl: cannot write %s\r\n", output);
            if (fd >= 0) fs_fd_close(fd);
            http_close(&h);
            return -1;
        }
        last = body[n - 1];
        received += (uint32_t)n;
    }
    if (fd >= 0) fs_fd_close(fd);
    rc = h.status;
    http_close(&h);
    if (!output && received && last != '\n')
        kprintf("\r\n");
    return rc >= 200 && rc < 400 ? 0 : -1;

fail_output:
    if (fd >= 0) fs_fd_close(fd);
fail:
    http_close(&h);
    if (!output && received && last != '\n')
        kprintf("\r\n");
    kprintf("curl: error %d\r\n", rc);
    return -1;
usage:
    kprintf("usage: curl [--basic user:password] [--data text] [--header \"Name: value\"] "
            "[--location] [--output file] [--request method] [--user-agent text] "
            "[--verbose] URL\r\n");
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
