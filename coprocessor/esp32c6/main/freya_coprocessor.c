#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "miniz.h"

#define FRAME_SIZE 512
#define HEADER_SIZE 20
#define PAYLOAD_SIZE (FRAME_SIZE - HEADER_SIZE)
#define MAGIC 0x31505345U
#define VERSION 1
#define MAX_SOCKETS 4
#define READY_GPIO GPIO_NUM_4
#define AGAIN (-8)
#define ARG (-3)
#define IO (-7)
#define NACK (-5)
#define EVENT 0x8000
#define HTTP_HEADERS_MAX 400
#define HTTP_BODY_MAX (192 * 1024)

enum {
    OP_FETCH, OP_WIFI_ON, OP_WIFI_OFF, OP_CREDENTIALS, OP_CONNECT,
    OP_DISCONNECT, OP_STATUS, OP_SCAN_START, OP_SCAN_NEXT, OP_PING_START,
    OP_PING_RESULT, OP_SOCKET, OP_CLOSE, OP_SOCK_CONNECT, OP_BIND, OP_LISTEN,
    OP_ACCEPT, OP_SEND, OP_RECV, OP_SENDTO, OP_RECVFROM, OP_TLS_CONNECT,
    OP_HTTP_START, OP_HTTP_INFO, OP_HTTP_READ, OP_HTTP_CLOSE
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t sequence;
    uint16_t length;
    int16_t status;
    uint32_t crc;
    uint8_t payload[PAYLOAD_SIZE];
} frame_t;

typedef struct {
    esp_tls_t *tls;
    esp_tls_cfg_t cfg;
    char host[254];
    uint16_t port;
    bool connected;
} tls_slot_t;

typedef struct {
    SemaphoreHandle_t done;
    uint32_t address;
    uint32_t elapsed;
    uint32_t replies;
    bool active;
    bool finished;
} ping_state_t;

typedef struct {
    volatile bool active;
    volatile bool finished;
    int error;
    uint16_t status;
    uint16_t header_length;
    char headers[HTTP_HEADERS_MAX];
    uint8_t *body;
    size_t body_length;
    size_t body_capacity;
    size_t body_pos;
    bool gzip;
} http_state_t;

typedef struct {
    uint16_t length;
    uint8_t payload[PAYLOAD_SIZE];
} http_job_t;

static const char *TAG = "freya-c6";
static int sockets[MAX_SOCKETS] = { -1, -1, -1, -1 };
static tls_slot_t tls_slots[MAX_SOCKETS];
static wifi_ap_record_t *scan_records;
static uint16_t scan_count;
static uint16_t scan_pos;
static ping_state_t ping_state;
static esp_netif_t *sta_netif;
static bool wifi_started;
static bool sntp_started;
static http_state_t http_state;

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < length; ++i) {
        uint8_t value = data[i];
        if (i >= 16 && i < 20) value = 0;
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ 0xffffffffU;
}

static bool valid_frame(const frame_t *frame)
{
    return frame->magic == MAGIC && frame->version == VERSION &&
           frame->length <= PAYLOAD_SIZE &&
           frame->crc == crc32((const uint8_t *)frame, sizeof(*frame));
}

static void encode(frame_t *frame, uint16_t opcode, uint32_t sequence,
                   int status, const void *payload, uint16_t length)
{
    memset(frame, 0, sizeof(*frame));
    frame->magic = MAGIC;
    frame->version = VERSION;
    frame->opcode = opcode;
    frame->sequence = sequence;
    frame->length = length;
    frame->status = (int16_t)status;
    if (length) memcpy(frame->payload, payload, length);
    frame->crc = crc32((const uint8_t *)frame, sizeof(*frame));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void tls_drop(int slot)
{
    if (tls_slots[slot].tls) esp_tls_conn_destroy(tls_slots[slot].tls);
    memset(&tls_slots[slot], 0, sizeof(tls_slots[slot]));
}

static void close_slot(int slot)
{
    if (slot < 0 || slot >= MAX_SOCKETS) return;
    tls_drop(slot);
    if (sockets[slot] >= 0) close(sockets[slot]);
    sockets[slot] = -1;
}

static int free_slot(void)
{
    for (int i = 0; i < MAX_SOCKETS; ++i)
        if (sockets[i] < 0 && !tls_slots[i].tls) return i;
    return -1;
}

static bool slot_valid(int slot)
{
    return slot >= 0 && slot < MAX_SOCKETS &&
           (sockets[slot] >= 0 || tls_slots[slot].tls);
}

static int errno_status(void)
{
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS ||
        errno == ETIMEDOUT || errno == EALREADY)
        return AGAIN;
    return IO;
}

static esp_err_t wifi_ensure_started(void)
{
    if (wifi_started) return ESP_OK;
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) wifi_started = true;
    return err;
}

static int read_credentials(char *ssid, size_t ssid_size,
                            char *password, size_t password_size)
{
    nvs_handle_t nvs;
    size_t sn = ssid_size;
    size_t pn = password_size;
    if (nvs_open("freya", NVS_READONLY, &nvs) != ESP_OK) return ARG;
    esp_err_t a = nvs_get_str(nvs, "ssid", ssid, &sn);
    esp_err_t b = nvs_get_str(nvs, "pass", password, &pn);
    nvs_close(nvs);
    return a == ESP_OK && b == ESP_OK ? 0 : ARG;
}

static int write_credentials(const uint8_t *data, uint16_t length)
{
    const char *ssid = (const char *)data;
    size_t sn = strnlen(ssid, length);
    if (!sn || sn >= length || sn > 32) return ARG;
    const char *password = ssid + sn + 1;
    size_t left = length - sn - 1;
    size_t pn = strnlen(password, left);
    if (pn >= left || pn > 64) return ARG;
    nvs_handle_t nvs;
    if (nvs_open("freya", NVS_READWRITE, &nvs) != ESP_OK) return IO;
    esp_err_t err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK ? 0 : IO;
}

static void ping_success(esp_ping_handle_t handle, void *arg)
{
    ping_state_t *state = arg;
    uint32_t elapsed = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP,
                         &elapsed, sizeof(elapsed));
    state->elapsed += elapsed;
    state->replies++;
}

static void ping_end(esp_ping_handle_t handle, void *arg)
{
    (void)handle;
    ping_state_t *state = arg;
    state->finished = true;
    xSemaphoreGive(state->done);
}

static void ping_worker(void *arg)
{
    char *host = arg;
    uint32_t timeout = get32((const uint8_t *)host);
    const char *name = host + 4;
    struct addrinfo hint = { .ai_family = AF_INET };
    struct addrinfo *ai = NULL;
    esp_ping_handle_t handle = NULL;
    esp_ping_callbacks_t callbacks = {
        .on_ping_success = ping_success,
        .on_ping_end = ping_end,
        .cb_args = &ping_state,
    };
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();

    if (getaddrinfo(name, NULL, &hint, &ai) == 0) {
        struct sockaddr_in *address = (struct sockaddr_in *)ai->ai_addr;
        memcpy(&config.target_addr.u_addr.ip4, &address->sin_addr,
               sizeof(config.target_addr.u_addr.ip4));
        config.target_addr.type = IPADDR_TYPE_V4;
        ping_state.address = ntohl(address->sin_addr.s_addr);
        freeaddrinfo(ai);
        config.count = 1;
        config.timeout_ms = timeout;
        config.interval_ms = 10;
        if (esp_ping_new_session(&config, &callbacks, &handle) == ESP_OK) {
            esp_ping_start(handle);
            xSemaphoreTake(ping_state.done, pdMS_TO_TICKS(timeout + 500));
            esp_ping_stop(handle);
            esp_ping_delete_session(handle);
        }
    }
    ping_state.finished = true;
    free(host);
    vTaskDelete(NULL);
}

static int endpoint(const uint8_t *data, uint16_t length, size_t offset,
                    struct sockaddr_in *address)
{
    if (length < offset + 8) return ARG;
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(get32(data + offset));
    address->sin_port = htons(get16(data + offset + 4));
    return 0;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(event->header_key, "Content-Encoding") == 0 &&
            strcasecmp(event->header_value, "gzip") == 0)
            http_state.gzip = true;
        int room = HTTP_HEADERS_MAX - http_state.header_length;
        if (room > 1) {
            int n = snprintf(http_state.headers + http_state.header_length,
                             (size_t)room, "%s: %s\r\n",
                             event->header_key, event->header_value);
            if (n > 0) {
                if (n >= room) n = room - 1;
                http_state.header_length += (uint16_t)n;
            }
        }
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t needed = http_state.body_length + (size_t)event->data_len;
        if (needed > HTTP_BODY_MAX) {
            http_state.error = IO;
            return ESP_FAIL;
        }
        if (needed > http_state.body_capacity) {
            size_t capacity = http_state.body_capacity
                ? http_state.body_capacity * 2 : 4096;
            while (capacity < needed) capacity *= 2;
            if (capacity > HTTP_BODY_MAX) capacity = HTTP_BODY_MAX;
            uint8_t *body = realloc(http_state.body, capacity);
            if (!body) {
                http_state.error = IO;
                return ESP_ERR_NO_MEM;
            }
            http_state.body = body;
            http_state.body_capacity = capacity;
        }
        memcpy(http_state.body + http_state.body_length,
               event->data, (size_t)event->data_len);
        http_state.body_length = needed;
    }
    return ESP_OK;
}

static int gzip_offset(const uint8_t *data, size_t length, size_t *offset)
{
    if (length < 18 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 8)
        return -1;
    uint8_t flags = data[3];
    size_t pos = 10;
    if (flags & 4) {
        if (pos + 2 > length) return -1;
        size_t extra = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
        pos += 2;
        if (pos + extra > length) return -1;
        pos += extra;
    }
    if (flags & 8) {
        while (pos < length && data[pos]) pos++;
        if (++pos > length) return -1;
    }
    if (flags & 16) {
        while (pos < length && data[pos]) pos++;
        if (++pos > length) return -1;
    }
    if (flags & 2) {
        if (pos + 2 > length) return -1;
        pos += 2;
    }
    if (pos + 8 > length) return -1;
    *offset = pos;
    return 0;
}

static int http_decompress_gzip(void)
{
    size_t offset;
    if (gzip_offset(http_state.body, http_state.body_length, &offset) != 0)
        return IO;
    uint8_t *trailer = http_state.body + http_state.body_length - 8;
    uint32_t output_length = get32(trailer + 4);
    if (output_length > HTTP_BODY_MAX ||
        output_length + http_state.body_length > HTTP_BODY_MAX + 65536U)
        return IO;
    uint8_t *output = malloc(output_length ? output_length : 1);
    if (!output) return IO;
    size_t made = tinfl_decompress_mem_to_mem(
        output, output_length, http_state.body + offset,
        http_state.body_length - offset - 8,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (made == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || made != output_length) {
        free(output);
        return IO;
    }
    free(http_state.body);
    http_state.body = output;
    http_state.body_length = made;
    http_state.body_capacity = made;
    return 0;
}

static char *http_field(char **cursor, char *end)
{
    if (*cursor >= end) return NULL;
    char *field = *cursor;
    size_t length = strnlen(field, (size_t)(end - field));
    if (field + length >= end) return NULL;
    *cursor = field + length + 1;
    return field;
}

static void http_worker(void *arg)
{
    http_job_t *job = arg;
    uint8_t flags = job->payload[0];
    char *cursor = (char *)job->payload + 1;
    char *end = (char *)job->payload + job->length;
    char *url = http_field(&cursor, end);
    char *user_agent = http_field(&cursor, end);
    char *basic = http_field(&cursor, end);
    char *data = http_field(&cursor, end);
    char *password = NULL;
    bool insecure = (flags & 2) != 0;
    esp_http_client_handle_t client = NULL;

    if (!url || !user_agent || !basic || !data) {
        http_state.error = ARG;
        goto done;
    }
    if (*basic) {
        password = strchr(basic, ':');
        if (!password) {
            http_state.error = ARG;
            goto done;
        }
        *password++ = '\0';
    }
    esp_http_client_config_t config = {
        .url = url,
        .username = *basic ? basic : NULL,
        .password = password,
        .auth_type = *basic ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
        .user_agent = *user_agent ? user_agent : "Freya-curl/1.0",
        .method = *data ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 20000,
        .event_handler = http_event,
        .skip_cert_common_name_check = insecure,
        .crt_bundle_attach = insecure ? NULL : esp_crt_bundle_attach,
        .tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_3,
    };
    client = esp_http_client_init(&config);
    if (!client) {
        http_state.error = IO;
        goto done;
    }
    if (flags & 1)
        esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    if (*data) {
        esp_http_client_set_header(client, "Content-Type",
                                   "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, data, (int)strlen(data));
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK || http_state.error) {
        if (!http_state.error) http_state.error = IO;
        goto done;
    }
    http_state.status = (uint16_t)esp_http_client_get_status_code(client);
    if ((flags & 1) && http_state.gzip)
        http_state.error = http_decompress_gzip();

done:
    if (client) esp_http_client_cleanup(client);
    free(job);
    http_state.finished = true;
    vTaskDelete(NULL);
}

static int dispatch(uint16_t op, const uint8_t *data, uint16_t length,
                    uint8_t *reply, uint16_t *reply_length)
{
    *reply_length = 0;
    if (op == OP_WIFI_ON)
        return wifi_ensure_started() == ESP_OK ? 0 : IO;
    if (op == OP_WIFI_OFF) {
        for (int i = 0; i < MAX_SOCKETS; ++i) close_slot(i);
        esp_wifi_disconnect();
        if (wifi_started) esp_wifi_stop();
        wifi_started = false;
        return 0;
    }
    if (op == OP_CREDENTIALS) return write_credentials(data, length);
    if (op == OP_CONNECT) {
        char ssid[33], password[65];
        if (read_credentials(ssid, sizeof(ssid), password, sizeof(password)))
            return ARG;
        if (wifi_ensure_started() != ESP_OK) return IO;
        wifi_config_t config = { 0 };
        memcpy(config.sta.ssid, ssid, strlen(ssid));
        memcpy(config.sta.password, password, strlen(password));
        if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) return IO;
        esp_err_t err = esp_wifi_connect();
        return err == ESP_OK || err == ESP_ERR_WIFI_CONN ? 0 : IO;
    }
    if (op == OP_DISCONNECT)
        return esp_wifi_disconnect() == ESP_OK ? 0 : IO;
    if (op == OP_STATUS) {
        wifi_ap_record_t ap = { 0 };
        esp_netif_ip_info_t ip = { 0 };
        int state = 0, rssi = 0;
        if (wifi_started) {
            state = 1;
            wifi_ap_record_t current;
            if (esp_wifi_sta_get_ap_info(&current) == ESP_OK) {
                ap = current;
                rssi = current.rssi;
                state = 3;
                esp_netif_get_ip_info(sta_netif, &ip);
            }
        }
        put32(reply, (uint32_t)state);
        put32(reply + 4, (uint32_t)rssi);
        put32(reply + 8, ntohl(ip.ip.addr));
        put32(reply + 12, ntohl(ip.gw.addr));
        put32(reply + 16, ntohl(ip.netmask.addr));
        memcpy(reply + 20, ap.ssid, strnlen((char *)ap.ssid, 32));
        *reply_length = 56;
        return 0;
    }
    if (op == OP_SCAN_START) {
        if (wifi_ensure_started() != ESP_OK) return IO;
        free(scan_records);
        scan_records = NULL;
        scan_count = scan_pos = 0;
        if (esp_wifi_scan_start(NULL, true) != ESP_OK) return IO;
        if (esp_wifi_scan_get_ap_num(&scan_count) != ESP_OK) return IO;
        if (!scan_count) return 0;
        scan_records = calloc(scan_count, sizeof(*scan_records));
        if (!scan_records) return IO;
        if (esp_wifi_scan_get_ap_records(&scan_count, scan_records) != ESP_OK)
            return IO;
        return 0;
    }
    if (op == OP_SCAN_NEXT) {
        if (!scan_records || scan_pos >= scan_count) {
            free(scan_records);
            scan_records = NULL;
            return NACK;
        }
        wifi_ap_record_t *ap = &scan_records[scan_pos++];
        put32(reply, (uint32_t)ap->rssi);
        reply[4] = ap->primary;
        reply[5] = ap->authmode;
        memcpy(reply + 8, ap->ssid, strnlen((char *)ap->ssid, 32));
        *reply_length = 44;
        return 0;
    }
    if (op == OP_PING_START) {
        if (length < 6 || ping_state.active) return ping_state.active ? AGAIN : ARG;
        size_t hn = strnlen((const char *)data + 4, length - 4);
        if (!hn || hn >= length - 4) return ARG;
        char *job = malloc(hn + 5);
        if (!job) return IO;
        memcpy(job, data, hn + 5);
        if (!ping_state.done) ping_state.done = xSemaphoreCreateBinary();
        if (!ping_state.done) { free(job); return IO; }
        ping_state.address = ping_state.elapsed = ping_state.replies = 0;
        ping_state.finished = false;
        ping_state.active = true;
        if (xTaskCreate(ping_worker, "freya_ping", 4096, job, 5, NULL) != pdPASS) {
            ping_state.active = false;
            free(job);
            return IO;
        }
        return 0;
    }
    if (op == OP_PING_RESULT) {
        if (!ping_state.active || !ping_state.finished) return AGAIN;
        put32(reply, ping_state.address);
        put32(reply + 4, ping_state.elapsed);
        put32(reply + 8, ping_state.replies);
        put32(reply + 12, 1U - ping_state.replies);
        *reply_length = 16;
        ping_state.active = false;
        return 0;
    }
    if (op == OP_HTTP_START) {
        if (length < 5) return ARG;
        if (http_state.active && !http_state.finished) return AGAIN;
        if (http_state.active) {
            free(http_state.body);
            memset(&http_state, 0, sizeof(http_state));
        }
        http_job_t *job = malloc(sizeof(*job));
        if (!job) return IO;
        memset(&http_state, 0, sizeof(http_state));
        http_state.active = true;
        job->length = length;
        memcpy(job->payload, data, length);
        if (xTaskCreate(http_worker, "freya_http", 8192, job, 5, NULL)
                != pdPASS) {
            http_state.active = false;
            free(job);
            return IO;
        }
        return 0;
    }
    if (op == OP_HTTP_INFO) {
        if (!http_state.active) return ARG;
        if (!http_state.finished) return AGAIN;
        if (http_state.error) return http_state.error;
        memset(reply, 0, 408);
        put32(reply, (uint32_t)http_state.body_length);
        put16(reply + 4, http_state.status);
        put16(reply + 6, http_state.header_length);
        memcpy(reply + 8, http_state.headers, http_state.header_length);
        *reply_length = 408;
        return 0;
    }
    if (op == OP_HTTP_READ) {
        if (!http_state.active || !http_state.finished || length != 2)
            return ARG;
        if (http_state.error) return http_state.error;
        uint16_t wanted = get16(data);
        if (!wanted || wanted > PAYLOAD_SIZE) return ARG;
        size_t remaining = http_state.body_length - http_state.body_pos;
        if (wanted > remaining) wanted = (uint16_t)remaining;
        if (wanted) {
            memcpy(reply, http_state.body + http_state.body_pos, wanted);
            http_state.body_pos += wanted;
        }
        *reply_length = wanted;
        return wanted;
    }
    if (op == OP_HTTP_CLOSE) {
        if (!http_state.active) return 0;
        if (!http_state.finished) return AGAIN;
        free(http_state.body);
        memset(&http_state, 0, sizeof(http_state));
        return 0;
    }
    if (op == OP_SOCKET) {
        if (length != 6) return ARG;
        int domain = get16(data), type = get16(data + 2);
        if (domain != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM))
            return ARG;
        int slot = free_slot();
        if (slot < 0) return IO;
        int fd = socket(AF_INET, type, get16(data + 4));
        if (fd < 0) return IO;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        sockets[slot] = fd;
        return slot;
    }

    if (length < 2) return ARG;
    int slot = get16(data);
    if (!slot_valid(slot)) return ARG;
    if (op == OP_CLOSE) {
        close_slot(slot);
        return 0;
    }
    if (op == OP_TLS_CONNECT) {
        if (length < 6) return ARG;
        uint16_t port = get16(data + 2);
        size_t hn = strnlen((const char *)data + 4, length - 4);
        if (!port || !hn || hn >= length - 4 || hn > 253) return ARG;
        if (time(NULL) < 1700000000) {
            if (!sntp_started) {
                esp_sntp_config_t config =
                    ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
                if (esp_netif_sntp_init(&config) != ESP_OK) return IO;
                sntp_started = true;
            }
            return AGAIN;
        }
        tls_slot_t *tls = &tls_slots[slot];
        if (!tls->tls) {
            if (sockets[slot] < 0) return ARG;
            close(sockets[slot]);
            sockets[slot] = -1;
            tls->tls = esp_tls_init();
            if (!tls->tls) return IO;
            memcpy(tls->host, data + 4, hn);
            tls->host[hn] = 0;
            tls->port = port;
            tls->cfg.non_block = true;
            tls->cfg.timeout_ms = 10000;
            tls->cfg.crt_bundle_attach = esp_crt_bundle_attach;
            tls->cfg.skip_common_name = false;
            tls->cfg.tls_version = ESP_TLS_VER_TLS_1_3;
        } else if (tls->port != port || strlen(tls->host) != hn ||
                   memcmp(tls->host, data + 4, hn) != 0) {
            return ARG;
        }
        if (tls->connected) return 0;
        int rc = esp_tls_conn_new_async(tls->host, strlen(tls->host), tls->port,
                                        &tls->cfg, tls->tls);
        if (rc < 0) { tls_drop(slot); return IO; }
        if (rc == 1) { tls->connected = true; return 0; }
        return AGAIN;
    }
    if (tls_slots[slot].tls) {
        tls_slot_t *tls = &tls_slots[slot];
        if (!tls->connected) return AGAIN;
        if (op == OP_SEND) {
            ssize_t n = esp_tls_conn_write(tls->tls, data + 2, length - 2);
            if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE)
                return AGAIN;
            if (n < 0) { tls_drop(slot); return IO; }
            return (int)n;
        }
        if (op == OP_RECV) {
            if (length < 4) return ARG;
            uint16_t wanted = get16(data + 2);
            if (wanted > PAYLOAD_SIZE) return ARG;
            ssize_t n = esp_tls_conn_read(tls->tls, reply, wanted);
            if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE)
                return AGAIN;
            if (n < 0) { tls_drop(slot); return IO; }
            *reply_length = (uint16_t)n;
            return (int)n;
        }
        return ARG;
    }

    int fd = sockets[slot];
    if (op == OP_SOCK_CONNECT || op == OP_BIND) {
        struct sockaddr_in address;
        if (endpoint(data, length, 2, &address)) return ARG;
        int rc = op == OP_BIND
            ? bind(fd, (struct sockaddr *)&address, sizeof(address))
            : connect(fd, (struct sockaddr *)&address, sizeof(address));
        if (rc == 0 || (op == OP_SOCK_CONNECT && errno == EISCONN)) return 0;
        return errno_status();
    }
    if (op == OP_LISTEN) {
        if (length < 4) return ARG;
        return listen(fd, get16(data + 2)) == 0 ? 0 : IO;
    }
    if (op == OP_ACCEPT) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int client = accept(fd, (struct sockaddr *)&peer, &peer_length);
        if (client < 0) return errno_status();
        int client_slot = free_slot();
        if (client_slot < 0) { close(client); return IO; }
        fcntl(client, F_SETFL, fcntl(client, F_GETFL) | O_NONBLOCK);
        sockets[client_slot] = client;
        put32(reply, ntohl(peer.sin_addr.s_addr));
        put16(reply + 4, ntohs(peer.sin_port));
        *reply_length = 8;
        return client_slot;
    }
    if (op == OP_SEND || op == OP_SENDTO) {
        size_t offset = 2;
        struct sockaddr_in peer;
        ssize_t n;
        if (op == OP_SENDTO) {
            if (endpoint(data, length, 2, &peer)) return ARG;
            offset = 10;
            n = sendto(fd, data + offset, length - offset, 0,
                       (struct sockaddr *)&peer, sizeof(peer));
        } else {
            n = send(fd, data + offset, length - offset, 0);
        }
        return n < 0 ? errno_status() : (int)n;
    }
    if (op == OP_RECV || op == OP_RECVFROM) {
        if (length < 4) return ARG;
        uint16_t wanted = get16(data + 2);
        if (wanted > 480) return ARG;
        ssize_t n;
        if (op == OP_RECVFROM) {
            struct sockaddr_in peer;
            socklen_t peer_length = sizeof(peer);
            n = recvfrom(fd, reply + 8, wanted, 0,
                         (struct sockaddr *)&peer, &peer_length);
            if (n >= 0) {
                put32(reply, ntohl(peer.sin_addr.s_addr));
                put16(reply + 4, ntohs(peer.sin_port));
                *reply_length = (uint16_t)n + 8;
            }
        } else {
            n = recv(fd, reply, wanted, 0);
            if (n >= 0) *reply_length = (uint16_t)n;
        }
        return n < 0 ? errno_status() : (int)n;
    }
    return ARG;
}

static bool make_event(uint8_t event[2])
{
    static wifi_ps_type_t unused;
    (void)unused;
    fd_set read_set;
    FD_ZERO(&read_set);
    int highest = -1;
    for (int i = 0; i < MAX_SOCKETS; ++i) {
        if (sockets[i] >= 0) {
            FD_SET(sockets[i], &read_set);
            if (sockets[i] > highest) highest = sockets[i];
        }
    }
    struct timeval zero = { 0 };
    if (highest >= 0 && select(highest + 1, &read_set, NULL, NULL, &zero) > 0) {
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            if (sockets[i] >= 0 && FD_ISSET(sockets[i], &read_set)) {
                event[0] = 2;
                event[1] = (uint8_t)i;
                return true;
            }
        }
    }
    return false;
}

static void spi_service(void *arg)
{
    (void)arg;
    frame_t *tx = heap_caps_aligned_alloc(4, sizeof(frame_t), MALLOC_CAP_DMA);
    frame_t *rx = heap_caps_aligned_alloc(4, sizeof(frame_t), MALLOC_CAP_DMA);
    frame_t cached;
    uint32_t cached_sequence = UINT32_MAX;
    bool queued_reply = false;
    bool event_pending = false;
    uint8_t event[2];
    spi_slave_transaction_t transaction = { 0 };

    if (!tx || !rx) abort();
    memset(tx, 0, sizeof(*tx));
    gpio_set_direction(READY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(READY_GPIO, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = GPIO_NUM_7,
        .miso_io_num = GPIO_NUM_2,
        .sclk_io_num = GPIO_NUM_6,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_SIZE,
    };
    spi_slave_interface_config_t slave = {
        .spics_io_num = GPIO_NUM_10,
        .queue_size = 1,
        .mode = 0,
    };
    ESP_ERROR_CHECK(spi_slave_initialize(SPI2_HOST, &bus, &slave,
                                          SPI_DMA_CH_AUTO));

    for (;;) {
        memset(rx, 0, sizeof(*rx));
        memset(&transaction, 0, sizeof(transaction));
        transaction.length = FRAME_SIZE * 8;
        transaction.tx_buffer = tx;
        transaction.rx_buffer = rx;
        ESP_ERROR_CHECK(spi_slave_queue_trans(SPI2_HOST, &transaction,
                                               portMAX_DELAY));
        if (queued_reply || event_pending) gpio_set_level(READY_GPIO, 1);
        spi_slave_transaction_t *done;
        ESP_ERROR_CHECK(spi_slave_get_trans_result(SPI2_HOST, &done,
                                                    portMAX_DELAY));
        gpio_set_level(READY_GPIO, 0);
        if (!valid_frame(rx)) {
            memset(tx, 0, sizeof(*tx));
            queued_reply = false;
            continue;
        }
        if (rx->opcode == OP_FETCH) {
            if (queued_reply) {
                queued_reply = false;
                memset(tx, 0, sizeof(*tx));
            } else if (event_pending) {
                encode(tx, EVENT, rx->sequence, 0, event, sizeof(event));
                event_pending = false;
                queued_reply = true;
            } else {
                memset(tx, 0, sizeof(*tx));
            }
            continue;
        }
        if (rx->sequence == cached_sequence) {
            *tx = cached;
        } else {
            uint16_t response_length = 0;
            int status = dispatch(rx->opcode, rx->payload, rx->length,
                                  tx->payload, &response_length);
            uint8_t response[PAYLOAD_SIZE];
            memcpy(response, tx->payload, response_length);
            encode(tx, rx->opcode, rx->sequence, status,
                   response, response_length);
            cached = *tx;
            cached_sequence = rx->sequence;
        }
        queued_reply = true;
        if (!event_pending) event_pending = make_event(event);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG, "Freya network coprocessor ready");
    xTaskCreate(spi_service, "freya_spi", 8192, NULL, 8, NULL);
}
