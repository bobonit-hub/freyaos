#include <string.h>
#include <time.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ping.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"

#define FRAME_SIZE 512
#define READY_GPIO GPIO_NUM_4
#define TLS_SLOTS 4
#define TLS_HOST_MAX 253
#define TLS_IO_MAX 480

static uint8_t *s_tx;
static uint8_t *s_rx;
static spi_slave_transaction_t s_trans;
static int s_queued;

typedef struct {
    esp_tls_t *tls;
    esp_tls_cfg_t cfg;
    char host[TLS_HOST_MAX + 1];
    uint16_t port;
    uint8_t connected;
} tls_slot_t;

static tls_slot_t s_tls[TLS_SLOTS];
static int s_sntp_started;

static mp_obj_t link_init(void)
{
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
    esp_err_t err;

    if (s_tx) return mp_const_none;
    s_tx = heap_caps_aligned_alloc(4, FRAME_SIZE, MALLOC_CAP_DMA);
    s_rx = heap_caps_aligned_alloc(4, FRAME_SIZE, MALLOC_CAP_DMA);
    if (!s_tx || !s_rx) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("DMA buffers"));
    gpio_set_direction(READY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(READY_GPIO, 0);
    err = spi_slave_initialize(SPI2_HOST, &bus, &slave, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) mp_raise_OSError(err);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(link_init_obj, link_init);

/* Queue first, then assert READY.  The buffers remain owned by the SPI
 * driver until poll() reports completion. */
static mp_obj_t link_queue(size_t n_args, const mp_obj_t *args)
{
    mp_buffer_info_t b;
    esp_err_t err;
    int ready = n_args > 1 ? mp_obj_is_true(args[1]) : 1;

    if (!s_tx || s_queued) mp_raise_OSError(MP_EBUSY);
    mp_get_buffer_raise(args[0], &b, MP_BUFFER_READ);
    if (b.len != FRAME_SIZE) mp_raise_ValueError(MP_ERROR_TEXT("frame size"));
    memcpy(s_tx, b.buf, FRAME_SIZE);
    memset(s_rx, 0, FRAME_SIZE);
    memset(&s_trans, 0, sizeof s_trans);
    s_trans.length = FRAME_SIZE * 8;
    s_trans.tx_buffer = s_tx;
    s_trans.rx_buffer = s_rx;
    err = spi_slave_queue_trans(SPI2_HOST, &s_trans, 0);
    if (err != ESP_OK) mp_raise_OSError(err);
    s_queued = 1;
    gpio_set_level(READY_GPIO, ready);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(link_queue_obj, 1, 2, link_queue);

static mp_obj_t link_poll(void)
{
    spi_slave_transaction_t *done;
    esp_err_t err;

    if (!s_queued) return mp_const_none;
    err = spi_slave_get_trans_result(SPI2_HOST, &done, 0);
    if (err == ESP_ERR_TIMEOUT) return mp_const_none;
    if (err != ESP_OK) mp_raise_OSError(err);
    gpio_set_level(READY_GPIO, 0);
    s_queued = 0;
    return mp_obj_new_bytes(s_rx, FRAME_SIZE);
}
static MP_DEFINE_CONST_FUN_OBJ_0(link_poll_obj, link_poll);

/* Announce an event while the ordinary receive transaction is already
 * queued.  The master's first FETCH consumes that slot; Python then queues
 * the event response and READY rises again for the actual fetch. */
static mp_obj_t link_signal(void)
{
    if (!s_queued) mp_raise_OSError(MP_EBUSY);
    gpio_set_level(READY_GPIO, 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(link_signal_obj, link_signal);

static tls_slot_t *tls_slot(mp_obj_t slot_obj)
{
    int slot = mp_obj_get_int(slot_obj);

    if (slot < 0 || slot >= TLS_SLOTS) mp_raise_ValueError(MP_ERROR_TEXT("TLS slot"));
    return &s_tls[slot];
}

static void tls_drop(tls_slot_t *slot)
{
    if (slot->tls) esp_tls_conn_destroy(slot->tls);
    memset(slot, 0, sizeof *slot);
}

/*
 * Advance one nonblocking TLS 1.3 client handshake.  The public API has no
 * insecure mode: ESP-IDF's certificate bundle and hostname verification are
 * always active, and tls_version forbids negotiation down to TLS 1.2.
 */
static mp_obj_t link_tls_connect(size_t n_args, const mp_obj_t *args)
{
    tls_slot_t *slot = tls_slot(args[0]);
    size_t host_len;
    const char *host = mp_obj_str_get_data(args[1], &host_len);
    int port = mp_obj_get_int(args[2]);
    int rc;

    if (!host_len || host_len > TLS_HOST_MAX || port < 1 || port > 65535)
        mp_raise_ValueError(MP_ERROR_TEXT("TLS endpoint"));
    /* Certificate validation needs real time.  SNTP runs in ESP-IDF's own
     * task; returning 0 keeps the STM32 API nonblocking while it converges. */
    if (time(NULL) < 1700000000) {
        if (!s_sntp_started) {
            esp_sntp_config_t sntp =
                ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&sntp) != ESP_OK)
                mp_raise_OSError(MP_EIO);
            s_sntp_started = 1;
        }
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    if (!slot->tls) {
        slot->tls = esp_tls_init();
        if (!slot->tls) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("TLS"));
        memcpy(slot->host, host, host_len);
        slot->host[host_len] = '\0';
        slot->port = (uint16_t)port;
        slot->cfg.non_block = true;
        slot->cfg.timeout_ms = 10000;
        slot->cfg.crt_bundle_attach = esp_crt_bundle_attach;
        slot->cfg.skip_common_name = false;
        slot->cfg.tls_version = ESP_TLS_VER_TLS_1_3;
    } else if (slot->port != port || strlen(slot->host) != host_len ||
               memcmp(slot->host, host, host_len) != 0) {
        mp_raise_OSError(MP_EBUSY);
    }
    if (slot->connected) return MP_OBJ_NEW_SMALL_INT(1);
    rc = esp_tls_conn_new_async(slot->host, (int)strlen(slot->host),
                                slot->port, &slot->cfg, slot->tls);
    if (rc < 0) {
        tls_drop(slot);
        mp_raise_OSError(MP_EIO);
    }
    if (rc == 1) slot->connected = 1;
    return MP_OBJ_NEW_SMALL_INT(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(link_tls_connect_obj, 3, 3,
                                            link_tls_connect);

static mp_obj_t link_tls_write(mp_obj_t slot_obj, mp_obj_t data_obj)
{
    tls_slot_t *slot = tls_slot(slot_obj);
    mp_buffer_info_t data;
    ssize_t n;

    if (!slot->connected) mp_raise_OSError(MP_ENOTCONN);
    mp_get_buffer_raise(data_obj, &data, MP_BUFFER_READ);
    if (data.len > TLS_IO_MAX) mp_raise_ValueError(MP_ERROR_TEXT("TLS write"));
    n = esp_tls_conn_write(slot->tls, data.buf, data.len);
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE)
        return mp_const_none;
    if (n < 0) {
        tls_drop(slot);
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_int(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(link_tls_write_obj, link_tls_write);

static mp_obj_t link_tls_read(mp_obj_t slot_obj, mp_obj_t length_obj)
{
    tls_slot_t *slot = tls_slot(slot_obj);
    uint8_t data[TLS_IO_MAX];
    int length = mp_obj_get_int(length_obj);
    ssize_t n;

    if (!slot->connected) mp_raise_OSError(MP_ENOTCONN);
    if (length < 0 || length > TLS_IO_MAX)
        mp_raise_ValueError(MP_ERROR_TEXT("TLS read"));
    n = esp_tls_conn_read(slot->tls, data, (size_t)length);
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE)
        return mp_const_none;
    if (n < 0) {
        tls_drop(slot);
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_bytes(data, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(link_tls_read_obj, link_tls_read);

static mp_obj_t link_tls_close(mp_obj_t slot_obj)
{
    tls_drop(tls_slot(slot_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(link_tls_close_obj, link_tls_close);

typedef struct {
    SemaphoreHandle_t done;
    uint32_t elapsed;
    uint32_t replies;
} ping_wait_t;

static void ping_success(esp_ping_handle_t h, void *arg)
{
    ping_wait_t *p = arg;
    uint32_t elapsed;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof elapsed);
    p->elapsed += elapsed;
    p->replies++;
}

static void ping_end(esp_ping_handle_t h, void *arg)
{
    ping_wait_t *p = arg;
    xSemaphoreGive(p->done);
}

static mp_obj_t link_ping(mp_obj_t host_obj, mp_obj_t timeout_obj)
{
    const char *host = mp_obj_str_get_str(host_obj);
    uint32_t timeout = mp_obj_get_int(timeout_obj);
    struct addrinfo hint = { .ai_family = AF_INET };
    struct addrinfo *ai = NULL;
    ip_addr_t target;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    esp_ping_callbacks_t cb = { 0 };
    esp_ping_handle_t ping;
    ping_wait_t wait = { .done = xSemaphoreCreateBinary() };
    mp_obj_t out[4];

    if (!wait.done || getaddrinfo(host, NULL, &hint, &ai) != 0)
        mp_raise_OSError(MP_EHOSTUNREACH);
    memcpy(&target.u_addr.ip4, &((struct sockaddr_in *)ai->ai_addr)->sin_addr,
           sizeof(target.u_addr.ip4));
    target.type = IPADDR_TYPE_V4;
    freeaddrinfo(ai);
    cfg.target_addr = target;
    cfg.count = 1;
    cfg.timeout_ms = timeout;
    cfg.interval_ms = 10;
    cb.on_ping_success = ping_success;
    cb.on_ping_end = ping_end;
    cb.cb_args = &wait;
    if (esp_ping_new_session(&cfg, &cb, &ping) != ESP_OK)
        mp_raise_OSError(MP_EIO);
    esp_ping_start(ping);
    xSemaphoreTake(wait.done, pdMS_TO_TICKS(timeout + 100));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    vSemaphoreDelete(wait.done);
    out[0] = mp_obj_new_int_from_uint(ip4_addr_get_u32(&target.u_addr.ip4));
    out[1] = mp_obj_new_int_from_uint(wait.elapsed);
    out[2] = mp_obj_new_int_from_uint(wait.replies);
    out[3] = mp_obj_new_int_from_uint(1 - wait.replies);
    return mp_obj_new_tuple(4, out);
}
static MP_DEFINE_CONST_FUN_OBJ_2(link_ping_obj, link_ping);

static const mp_rom_map_elem_t link_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_freya_link) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&link_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_queue), MP_ROM_PTR(&link_queue_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&link_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_signal), MP_ROM_PTR(&link_signal_obj) },
    { MP_ROM_QSTR(MP_QSTR_ping), MP_ROM_PTR(&link_ping_obj) },
    { MP_ROM_QSTR(MP_QSTR_tls_connect), MP_ROM_PTR(&link_tls_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_tls_write), MP_ROM_PTR(&link_tls_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_tls_read), MP_ROM_PTR(&link_tls_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_tls_close), MP_ROM_PTR(&link_tls_close_obj) },
};
static MP_DEFINE_CONST_DICT(link_globals, link_globals_table);

const mp_obj_module_t freya_link_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&link_globals,
};

MP_REGISTER_MODULE(MP_QSTR_freya_link, freya_link_module);
