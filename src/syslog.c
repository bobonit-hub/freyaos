/*
 * Freya - remote syslog over UDP.
 *
 * Every line klog() keeps is also handed to syslog_send().  When remote
 * syslog is on, the line goes to the configured IPv4 server as an
 * RFC 3164 datagram:
 *
 *   <PRI>Mmm dd hh:mm:ss freya freya: message
 *
 * PRI is facility user (1) times eight plus the severity of the level:
 * error 3, warning 4, info 6, debug 7.  The ESP32-C6 sends it from a UDP
 * socket of its own, so the four program sockets are not touched.
 *
 * The on flag, the server address and the port are system settings.  They
 * are read on first use and kept in RAM; the setters update both.
 *
 * The link to the C6 carries one request at a time.  A line logged while
 * another request is out waits in a short queue and leaves on the next
 * klog() or term_pump().  With the link closed (Wi-Fi off) a line is not
 * queued.  A full queue drops its oldest line.  A line from an interrupt
 * handler is not sent.
 */
#include "freya.h"
#include "esp_link.h"

#if BOARD_ESP_LINK

#define SYSLOG_FACILITY  1U               /* user-level messages */
#define SYSLOG_HDR       6U               /* address and port before text */
#define SYSLOG_MAX       224U             /* one request, header included */
#define SYSLOG_QUEUE     4U

typedef struct {
    uint16_t len;
    uint8_t  data[SYSLOG_MAX];
} syslog_req_t;

static syslog_req_t s_queue[SYSLOG_QUEUE];
static uint8_t  s_head, s_count;
static uint8_t  s_loaded, s_on, s_busy;
static uint32_t s_addr;
static uint16_t s_port;

static const char s_months[12][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void load(void)
{
    uint32_t v, srv[2];

    if (s_loaded) return;
    s_loaded = 1;
    v = 0xFFFFFFFFUL;
    s_on = settings_get("syslog", &v, sizeof v) == 0 &&
           v == FREYA_SYSLOG_MAGIC;
    if (settings_get("syslog_server", srv, sizeof srv) != 0)
        srv[0] = srv[1] = 0xFFFFFFFFUL;
    s_addr = (srv[0] == 0xFFFFFFFFUL) ? 0 : srv[0];
    s_port = (srv[1] == 0 || srv[1] > 0xFFFFU) ? (uint16_t)FREYA_SYSLOG_PORT
                                               : (uint16_t)srv[1];
}

int syslog_enabled(void)
{
    load();
    return s_on;
}

uint32_t syslog_server(uint16_t *port)
{
    load();
    if (port) *port = s_port;
    return s_addr;
}

int syslog_set_enabled(int on)
{
    uint32_t v = on ? FREYA_SYSLOG_MAGIC : 0xFFFFFFFFUL;
    int rc;

    load();
    if (on && !s_addr) return FREYA_ERR_ARG;
    rc = settings_set("syslog", &v, sizeof v);
    if (rc == FLASH_OK) s_on = (uint8_t)(on ? 1 : 0);
    if (!s_on) s_count = 0;
    return rc;
}

int syslog_set_server(uint32_t addr, uint16_t port)
{
    uint32_t srv[2];
    int rc;

    if (!addr || addr == 0xFFFFFFFFUL || !port) return FREYA_ERR_ARG;
    load();
    srv[0] = addr;
    srv[1] = port;
    rc = settings_set("syslog_server", srv, sizeof srv);
    if (rc == FLASH_OK) {
        s_addr = addr;
        s_port = port;
        s_count = 0;
    }
    return rc;
}

/* Four decimal parts of 0..255, nothing else.  Host byte order out. */
int syslog_parse_ipv4(const char *s, uint32_t *addr)
{
    uint32_t a = 0;
    int part;

    if (!s || !addr) return -1;
    for (part = 0; part < 4; part++) {
        uint32_t v = 0;
        int digits = 0;

        while (*s >= '0' && *s <= '9') {
            v = v * 10U + (uint32_t)(*s++ - '0');
            if (++digits > 3 || v > 255U) return -1;
        }
        if (!digits) return -1;
        a = (a << 8) | v;
        if (part < 3 && *s++ != '.') return -1;
    }
    if (*s) return -1;
    *addr = a;
    return 0;
}

static int severity(int level)
{
    switch (level) {
    case FREYA_LOG_ERROR: return 3;
    case FREYA_LOG_WARN:  return 4;
    case FREYA_LOG_INFO:  return 6;
    default:              return 7;
    }
}

void syslog_flush(void)
{
    int rc;

    if (s_busy || !s_count) return;
    if (!esp_link_is_open()) {
        s_count = 0;
        return;
    }
    s_busy = 1;
    while (s_count) {
        syslog_req_t *r = &s_queue[s_head];

        rc = net_syslog_send(r->data, r->len);
        if (rc == FREYA_ERR_AGAIN) break;
        /* Sent, or refused by the C6: UDP has no retry either way. */
        s_head = (uint8_t)((s_head + 1U) % SYSLOG_QUEUE);
        s_count--;
    }
    s_busy = 0;
}

void syslog_send(int level, const rtc_time_t *t, const char *msg, int len)
{
    syslog_req_t *r;
    int n, room;

    load();
    if (!s_on || !s_addr || !msg || len < 0) return;
    if (app_in_handler() || !esp_link_is_open()) return;

    if (s_count == SYSLOG_QUEUE) {
        s_head = (uint8_t)((s_head + 1U) % SYSLOG_QUEUE);
        s_count--;
    }
    r = &s_queue[(s_head + s_count) % SYSLOG_QUEUE];
    r->data[0] = (uint8_t)s_addr;
    r->data[1] = (uint8_t)(s_addr >> 8);
    r->data[2] = (uint8_t)(s_addr >> 16);
    r->data[3] = (uint8_t)(s_addr >> 24);
    r->data[4] = (uint8_t)s_port;
    r->data[5] = (uint8_t)(s_port >> 8);
    room = (int)(SYSLOG_MAX - SYSLOG_HDR);
    n = ksnprintf((char *)r->data + SYSLOG_HDR, room,
                  "<%u>%s %2u %02u:%02u:%02u freya freya: ",
                  SYSLOG_FACILITY * 8U + (unsigned)severity(level),
                  (t && t->mon >= 1 && t->mon <= 12) ? s_months[t->mon - 1]
                                                     : "Jan",
                  t ? t->day : 1U, t ? t->hour : 0U, t ? t->min : 0U,
                  t ? t->sec : 0U);
    if (n < 0) return;
    if (n >= room) n = room - 1;
    if (len > room - n) len = room - n;
    memcpy(r->data + SYSLOG_HDR + n, msg, (uint32_t)len);
    r->len = (uint16_t)(SYSLOG_HDR + (uint32_t)n + (uint32_t)len);
    s_count++;
    syslog_flush();
}

#endif /* BOARD_ESP_LINK */
