/*
 * Freya - remote syslog: settings, datagram format, the queue.
 *
 * src/syslog.c runs against the real settings code on its host flash
 * array.  The C6 link is a stub that records each ESP_OP_SYSLOG request.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freya.h"
#include "esp_link.h"

void settings_test_reset(void);
uint8_t *settings_test_flash(void);
int settings_test_writes(void);

app_state_t g_app;
static int link_open = 1, in_handler, link_busy;
static int sent;
static uint8_t last[ESP_FRAME_PAYLOAD];
static uint16_t last_len;

int app_in_handler(void) { return in_handler; }
int esp_link_is_open(void) { return link_open; }
void uart_putc(char c) { (void)c; }

int net_syslog_send(const void *req, uint16_t len)
{
    if (link_busy) return FREYA_ERR_AGAIN;
    memcpy(last, req, len);
    last_len = len;
    sent++;
    return (int)len - 6;
}

#include "../src/syslog.c"

static int fails;

static void check(const char *what, int cond)
{
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        fails++;
    }
}

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    uint32_t v;

    memcpy(&v, p + off, 4);
    return v;
}

/* The text of the last datagram, NUL-ended. */
static const char *text(void)
{
    static char s[ESP_FRAME_PAYLOAD + 1];

    memcpy(s, last + 6, (size_t)(last_len - 6));
    s[last_len - 6] = '\0';
    return s;
}

static void reload(void)
{
    s_loaded = 0;
    s_count = 0;
}

int main(void)
{
    rtc_time_t t = { 2026, 10, 2, 9, 5, 7 };
    uint32_t a = 0;
    uint16_t port = 0;
    uint8_t *flash;
    uint8_t pass[FREYA_PASSWORD_LEN];
    int writes;

    check("a dotted quad parses",
          syslog_parse_ipv4("192.168.1.20", &a) == 0 && a == 0xC0A80114UL);
    check("a part over 255 is refused", syslog_parse_ipv4("192.168.1.256", &a) != 0);
    check("three parts are refused", syslog_parse_ipv4("10.0.1", &a) != 0);
    check("a trailing dot is refused", syslog_parse_ipv4("10.0.0.1.", &a) != 0);
    check("a hostname is refused", syslog_parse_ipv4("logs.lan", &a) != 0);
    check("an empty part is refused", syslog_parse_ipv4("10..0.1", &a) != 0);

    settings_test_reset();
    flash = settings_test_flash();
    memcpy(pass, "12345678", sizeof pass);
    settings_set("password", pass, sizeof pass);
    reload();
    check("erased settings: remote syslog is off", syslog_enabled() == 0);
    check("erased settings: no server, default port",
          syslog_server(&port) == 0 && port == FREYA_SYSLOG_PORT);

    writes = settings_test_writes();
    check("turning on with no server is refused",
          syslog_set_enabled(1) == FREYA_ERR_ARG && syslog_enabled() == 0 &&
          settings_test_writes() == writes);
    check("a zero port is refused", syslog_set_server(0xC0A80114UL, 0) == FREYA_ERR_ARG);
    check("a zero address is refused", syslog_set_server(0, 514) == FREYA_ERR_ARG);

    check("the server is stored", syslog_set_server(0xC0A80114UL, 5514) == 0);
    check("the server is one flash write", settings_test_writes() == writes + 1);
    check("the address and port sit after the copy's checksum",
          rd32(flash, FREYA_SET_SYSLOG_ADDR_OFF) == 0xC0A80114UL &&
          rd32(flash, FREYA_SET_SYSLOG_PORT_OFF) == 5514 &&
          rd32(flash, FREYA_SETTINGS_BLOCK + FREYA_SET_SYSLOG_ADDR_OFF) == 0xC0A80114UL);
    check("remote syslog is on", syslog_set_enabled(1) == 0);
    check("the flag is stored after the checksum",
          rd32(flash, FREYA_SET_SYSLOG_OFF) == FREYA_SYSLOG_MAGIC && settings_ok());
    memset(pass, 0, sizeof pass);
    check("the password is kept",
          settings_get("password", pass, sizeof pass) == 0 &&
          memcmp(pass, "12345678", sizeof pass) == 0);

    reload();
    check("the settings survive a reset",
          syslog_enabled() == 1 && syslog_server(&port) == 0xC0A80114UL &&
          port == 5514);

    sent = 0;
    syslog_send(FREYA_LOG_INFO, &t, "hello world", 11);
    check("a line is sent at once", sent == 1);
    check("the request carries the server",
          rd32(last, 0) == 0xC0A80114UL && last[4] == (5514 & 255) &&
          last[5] == (5514 >> 8));
    check("the datagram is RFC 3164, user facility, info severity",
          strcmp(text(), "<14>Oct  2 09:05:07 freya freya: hello world") == 0);
    syslog_send(FREYA_LOG_ERROR, &t, "x", 1);
    check("an error is severity 3", strncmp(text(), "<11>", 4) == 0);
    syslog_send(FREYA_LOG_WARN, &t, "x", 1);
    check("a warning is severity 4", strncmp(text(), "<12>", 4) == 0);
    syslog_send(FREYA_LOG_DEBUG, &t, "x", 1);
    check("a debug line is severity 7", strncmp(text(), "<15>", 4) == 0);

    {
        char big[400];

        memset(big, 'z', sizeof big);
        syslog_send(FREYA_LOG_INFO, &t, big, (int)sizeof big);
        check("a long line is cut to one request", last_len == SYSLOG_MAX);
    }

    sent = 0;
    link_busy = 1;
    syslog_send(FREYA_LOG_INFO, &t, "one", 3);
    syslog_send(FREYA_LOG_INFO, &t, "two", 3);
    check("a busy link queues the lines", sent == 0 && s_count == 2);
    link_busy = 0;
    syslog_flush();
    check("a flush sends them in order",
          sent == 2 && strstr(text(), ": two") != NULL && s_count == 0);

    sent = 0;
    link_busy = 1;
    for (int i = 0; i < 6; i++) {
        char m[2] = { (char)('a' + i), 0 };
        syslog_send(FREYA_LOG_INFO, &t, m, 1);
    }
    check("a full queue keeps the newest lines", s_count == SYSLOG_QUEUE);
    link_busy = 0;
    syslog_flush();
    check("the oldest lines were dropped",
          sent == (int)SYSLOG_QUEUE && strstr(text(), ": f") != NULL);

    sent = 0;
    in_handler = 1;
    syslog_send(FREYA_LOG_INFO, &t, "irq", 3);
    in_handler = 0;
    check("a line from a handler is not sent", sent == 0 && s_count == 0);

    link_open = 0;
    syslog_send(FREYA_LOG_INFO, &t, "off", 3);
    link_open = 1;
    check("with the link closed nothing is queued", sent == 0 && s_count == 0);

    check("remote syslog turns off", syslog_set_enabled(0) == 0);
    syslog_send(FREYA_LOG_INFO, &t, "quiet", 5);
    check("off sends nothing", sent == 0);
    reload();
    check("off survives a reset, the server stays",
          syslog_enabled() == 0 && syslog_server(NULL) == 0xC0A80114UL);

    /* A copy sealed before the syslog fields existed: they are erased. */
    {
        uint8_t block[FREYA_SETTINGS_BLOCK];

        settings_test_reset();
        settings_block(block, (int)sizeof block);
        memcpy(flash, block, sizeof block);
        memcpy(flash + FREYA_SETTINGS_BLOCK, block, sizeof block);
        reload();
        check("an older settings copy is still valid", settings_ok() == 1);
        check("an older copy reads remote syslog as off, no server",
              syslog_enabled() == 0 && syslog_server(&port) == 0 &&
              port == FREYA_SYSLOG_PORT);
    }

    return fails ? 1 : 0;
}
