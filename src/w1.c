/*
 * Freya - 1-Wire master.
 *
 * w1_open() takes a pin and owns it until w1_close().  A transfer is
 * polled, the way I2C is: the call returns when the bits are done.
 * There is no overdrive.  Those slots are a microsecond or two, and the
 * delay here is a whole microsecond, which is not a promise that fine.
 *
 * The line is open-drain GPIO, not a peripheral.  Neither chip has a
 * 1-Wire block, so this is the same code on both.  The pin needs a
 * pull-up to 3.3 V.  The F4 enables its weak internal one on an
 * open-drain output, which is enough to see a single device up close
 * and not enough to call a bus; the F1 cannot enable one on an output.
 *
 * Standard speed, from the slot times a DS18B20 documents.  A write-one
 * has to be low for less than 15 us or a device reads a zero, and a read
 * is sampled inside that same window, so each slot holds interrupts off
 * while the line is doing the part a device times.  A ROM is eight
 * bytes, least significant bit first, family code in the first byte.
 * w1_search() is the Search ROM walk: two bits read, one bit written,
 * sixty-four times, taking the zero branch first.
 */
#include "freya.h"

#define W1_RST_LOW_US     480
#define W1_RST_SAMPLE_US   70
#define W1_RST_REST_US    410
#define W1_W1_LOW_US        6
#define W1_W1_REST_US      64
#define W1_W0_LOW_US       60
#define W1_W0_REST_US      10
#define W1_RD_LOW_US        3
#define W1_RD_SAMPLE_US     8
#define W1_RD_REST_US      52

typedef struct {
    GPIO_TypeDef *port;
    uint32_t      bit;
    int           pin;
    uint8_t       open;
    uint8_t       from_app;          /* a run's bus, dropped when it ends */
    uint8_t       pullup;
    uint8_t       rom[FREYA_W1_ROM_LEN];
    uint8_t       last_discrepancy;  /* 0: the next walk starts at the top */
    uint8_t       done;              /* the previous walk found the last   */
} w1_bus_t;

static w1_bus_t s_bus[FREYA_W1_BUSES];

static const uint16_t s_reserved[] = BOARD_PIN_RESERVED;

_Static_assert(ARRAY_SIZE(s_reserved) == BOARD_PIN_PORTS,
               "BOARD_PIN_RESERVED needs one mask per port");
_Static_assert(FREYA_W1_BUSES > 0, "a board needs a 1-Wire slot");
_Static_assert(FREYA_W1_ROM_LEN == 8, "a ROM is eight bytes");

/* ------------------------------------------------------- host model */
/*
 * The host has no pin.  The test plants the ROMs a search should find
 * and the bit operations below talk to those instead of a port.  One
 * model stands behind every open pin: what is being checked is the
 * walk, and the walk is the same on each slot.
 */
#ifdef FREYA_HOST
#define W1_HOST_MAX  8

enum { W1_M_CMD = 0, W1_M_SEARCH, W1_M_READ };

static struct {
    int     on;
    int     n;
    uint8_t rom[W1_HOST_MAX][FREYA_W1_ROM_LEN];
    uint8_t sel;
    int     phase;
    int     nbits;
    uint8_t acc;
    int     sbit;
    int     spair;
} s_model;

static void w1_host_load(const uint8_t roms[][FREYA_W1_ROM_LEN], int n)
{
    if (n < 0) n = 0;
    if (n > W1_HOST_MAX) n = W1_HOST_MAX;
    s_model.on = 1;
    s_model.n = n;
    s_model.phase = W1_M_CMD;
    s_model.sel = (uint8_t)((1U << n) - 1U);
    s_model.nbits = 0;
    s_model.acc = 0;
    s_model.sbit = 0;
    s_model.spair = 0;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < FREYA_W1_ROM_LEN; b++)
            s_model.rom[i][b] = roms[i][b];
}

static int model_reset(void)
{
    s_model.phase = W1_M_CMD;
    s_model.nbits = 0;
    s_model.acc = 0;
    s_model.sbit = 0;
    s_model.spair = 0;
    s_model.sel = (uint8_t)((1U << s_model.n) - 1U);
    return s_model.n ? 0 : FREYA_ERR_NACK;
}

static void model_write_bit(int one)
{
    if (s_model.phase == W1_M_CMD) {
        if (one) s_model.acc |= (uint8_t)(1U << s_model.nbits);
        if (++s_model.nbits < 8) return;
        if (s_model.acc == 0xF0) {
            s_model.phase = W1_M_SEARCH;
            s_model.sbit = 0;
            s_model.spair = 0;
            s_model.sel = (uint8_t)((1U << s_model.n) - 1U);
        } else if (s_model.acc == 0x33 && s_model.n == 1) {
            s_model.phase = W1_M_READ;
            s_model.sbit = 0;
        }
        s_model.nbits = 0;
        s_model.acc = 0;
        return;
    }
    if (s_model.phase == W1_M_SEARCH && s_model.spair == 2 &&
        s_model.sbit < 64) {
        int byte = s_model.sbit / 8;
        int mask = 1 << (s_model.sbit % 8);

        for (int i = 0; i < s_model.n; i++) {
            int db;

            if (!(s_model.sel & (1U << i))) continue;
            db = (s_model.rom[i][byte] & mask) ? 1 : 0;
            if (db != (one ? 1 : 0))
                s_model.sel = (uint8_t)(s_model.sel & ~(1U << i));
        }
        s_model.sbit++;
        s_model.spair = 0;
    }
}

static int model_read_bit(void)
{
    int byte, mask, id, cmp;

    if (s_model.phase == W1_M_READ && s_model.sbit < 64) {
        byte = s_model.sbit / 8;
        mask = 1 << (s_model.sbit % 8);
        s_model.sbit++;
        return (s_model.rom[0][byte] & mask) ? 1 : 0;
    }
    if (s_model.phase != W1_M_SEARCH || s_model.spair >= 2 ||
        s_model.sbit >= 64) return 1;

    byte = s_model.sbit / 8;
    mask = 1 << (s_model.sbit % 8);
    id = 1;
    cmp = 1;
    for (int i = 0; i < s_model.n; i++) {
        if (!(s_model.sel & (1U << i))) continue;
        if (s_model.rom[i][byte] & mask) cmp = 0;
        else id = 0;
    }
    return (s_model.spair++ == 0) ? id : cmp;
}
#endif /* FREYA_HOST */

/* -------------------------------------------------------------- wires */
static void w1_hi(w1_bus_t *b) { b->port->BSRR = b->bit; }
static void w1_lo(w1_bus_t *b) { b->port->BSRR = b->bit << 16; }
static int  w1_up(w1_bus_t *b) { return (b->port->IDR & b->bit) != 0; }

static void wire_od(w1_bus_t *b)
{
    b->pullup = 0;
    if (!b->port) return;
    board_pin_mode(b->port, FREYA_PIN_NUM(b->pin), FREYA_PIN_OUT_OD);
    w1_hi(b);
}

static void wire_in(w1_bus_t *b)
{
    if (!b->port) return;
    board_pin_mode(b->port, FREYA_PIN_NUM(b->pin), FREYA_PIN_IN);
}

/* -------------------------------------------------------------- slots */
static int pin_legal(int pin)
{
    int p = FREYA_PIN_PORT(pin);
    int n = FREYA_PIN_NUM(pin);

    if (pin < 0 || pin > 0xFF || p >= BOARD_PIN_PORTS) return 0;
    if (s_reserved[p] & (1U << n)) return 0;
    return 1;
}

static int slot_of(int pin)
{
    for (int i = 0; i < FREYA_W1_BUSES; i++)
        if (s_bus[i].open && s_bus[i].pin == pin) return i;
    return -1;
}

static int slot_free(void)
{
    for (int i = 0; i < FREYA_W1_BUSES; i++)
        if (!s_bus[i].open) return i;
    return -1;
}

static int w1_owns(const w1_bus_t *b)
{
    return b->from_app == (uint8_t)(g_app.running ? 1 : 0);
}

/* The open slot this caller may use, or a FREYA_ERR_*. */
static int claim(int pin)
{
    int idx;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    idx = slot_of(pin);
    if (idx < 0) return FREYA_ERR_ARG;
    if (!w1_owns(&s_bus[idx])) return FREYA_ERR_BUSY;
    return idx;
}

static int begin(int pin, w1_bus_t **out)
{
    int idx = claim(pin);

    if (idx < 0) return idx;
    if (app_should_stop()) return FREYA_ERR_IO;
    wire_od(&s_bus[idx]);
    *out = &s_bus[idx];
    return 0;
}

static int bytes_ok(const void *buf, int len)
{
    if (len < 0 || len > FREYA_W1_MAX_LEN) return 0;
    if (len && !buf) return 0;
    return 1;
}

/* --------------------------------------------------------------- bits */
static void bit_write(w1_bus_t *b, int one)
{
    uint32_t pm;

#ifdef FREYA_HOST
    if (s_model.on) { model_write_bit(one); return; }
#endif
    pm = irq_save();
    w1_lo(b);
    if (one) {
        sys_delay_us(W1_W1_LOW_US);
        w1_hi(b);
        irq_restore(pm);
        sys_delay_us(W1_W1_REST_US);
    } else {
        sys_delay_us(W1_W0_LOW_US);
        w1_hi(b);
        irq_restore(pm);
        sys_delay_us(W1_W0_REST_US);
    }
}

static int bit_read(w1_bus_t *b)
{
    uint32_t pm;
    int v;

#ifdef FREYA_HOST
    if (s_model.on) return model_read_bit();
#endif
    pm = irq_save();
    w1_lo(b);
    sys_delay_us(W1_RD_LOW_US);
    w1_hi(b);
    sys_delay_us(W1_RD_SAMPLE_US);
    v = w1_up(b);
    irq_restore(pm);
    sys_delay_us(W1_RD_REST_US);
    return v;
}

/* Least significant bit first, which is the order the bus itself uses. */
static void byte_write(w1_bus_t *b, int byte)
{
    for (int i = 0; i < 8; i++) {
        bit_write(b, byte & 1);
        byte >>= 1;
    }
}

static void byte_read(w1_bus_t *b, uint8_t *out)
{
    uint8_t v = 0;

    for (int i = 0; i < 8; i++)
        if (bit_read(b)) v |= (uint8_t)(1U << i);
    *out = v;
}

static int wire_reset(w1_bus_t *b)
{
    uint32_t pm;
    int present;

#ifdef FREYA_HOST
    if (s_model.on) return model_reset();
#endif
    /* A line that is already low has no pull-up, or something is holding
     * it.  Pulsing it would look like a presence that is not one. */
    if (!w1_up(b)) return FREYA_ERR_TIMEOUT;
    w1_lo(b);
    sys_delay_us(W1_RST_LOW_US);
    pm = irq_save();
    w1_hi(b);
    sys_delay_us(W1_RST_SAMPLE_US);
    present = !w1_up(b);
    irq_restore(pm);
    sys_delay_us(W1_RST_REST_US);
    return present ? 0 : FREYA_ERR_NACK;
}

/* ------------------------------------------------------------- search */
/*
 * Search ROM.  Each of the 64 bits is a pair the devices drive together
 * - the bit, then its complement - and the bit the master writes back
 * to say which branch the rest of the walk takes.  Both of the pair
 * low is a fork.  The first walk takes every fork as a zero; a later
 * walk takes the earlier forks the way the previous ROM did, and the
 * fork it stopped at the other way.  Both of the pair high means
 * nobody is on that branch.
 */
static void search_stop(w1_bus_t *b)
{
    b->done = 0;
    b->last_discrepancy = 0;
}

static int do_search(w1_bus_t *b, uint8_t *out)
{
    int last_zero = 0;
    int bit_no = 1;
    int rom_byte = 0;
    uint8_t rom_mask = 1;
    int rc;

    if (b->done) {
        search_stop(b);
        return FREYA_ERR_NACK;
    }

    rc = wire_reset(b);
    if (rc) {
        search_stop(b);
        return rc;
    }
    byte_write(b, 0xF0);

    while (rom_byte < FREYA_W1_ROM_LEN) {
        int id, cmp, dir;

        if (app_should_stop()) {
            search_stop(b);
            return FREYA_ERR_IO;
        }
        id = bit_read(b);
        cmp = bit_read(b);
        if (id && cmp) break;

        if (id != cmp) {
            dir = id;
        } else if (bit_no < b->last_discrepancy) {
            dir = (b->rom[rom_byte] & rom_mask) ? 1 : 0;
        } else {
            dir = (bit_no == b->last_discrepancy);
        }
        if (!dir && id == cmp) last_zero = bit_no;

        if (dir) b->rom[rom_byte] |= rom_mask;
        else     b->rom[rom_byte] &= (uint8_t)~rom_mask;
        bit_write(b, dir);

        bit_no++;
        rom_mask <<= 1;
        if (rom_mask == 0) {
            rom_byte++;
            rom_mask = 1;
        }
    }

    if (rom_byte < FREYA_W1_ROM_LEN || w1_crc(b->rom, FREYA_W1_ROM_LEN) != 0) {
        int bad = rom_byte >= FREYA_W1_ROM_LEN;
        search_stop(b);
        return bad ? FREYA_ERR_IO : FREYA_ERR_NACK;
    }

    for (int i = 0; i < FREYA_W1_ROM_LEN; i++) out[i] = b->rom[i];
    b->last_discrepancy = (uint8_t)last_zero;
    b->done = (uint8_t)(last_zero == 0);
    return 0;
}

/* -------------------------------------------------------------- calls */
int w1_crc(const void *buf, int len)
{
    const uint8_t *p = buf;
    uint8_t crc = 0;

    if (len < 0 || (len && !buf)) return FREYA_ERR_ARG;
    for (int i = 0; i < len; i++) {
        uint8_t byte = p[i];

        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (uint8_t)((crc ^ byte) & 1U);
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    return crc;
}

int w1_owns_pin(int pin)
{
    return slot_of(pin) >= 0;
}

static void slot_close(int idx)
{
    w1_bus_t *b = &s_bus[idx];

    if (!b->open) return;
    wire_in(b);
    b->open = 0;
    b->from_app = 0;
    b->pullup = 0;
    search_stop(b);
}

int w1_open(int pin)
{
    GPIO_TypeDef *port;
    int idx;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (!pin_legal(pin)) return FREYA_ERR_PIN;
    if (pwm_pin_busy(pin) || i2c_owns_pin(pin)) return FREYA_ERR_BUSY;

    idx = slot_of(pin);
    if (idx >= 0) return w1_owns(&s_bus[idx]) ? 0 : FREYA_ERR_BUSY;
    idx = slot_free();
    if (idx < 0) return FREYA_ERR_BUSY;

    port = board_gpio_port(FREYA_PIN_PORT(pin));
#ifdef FREYA_HOST
    if (!port && !s_model.on) return FREYA_ERR_PIN;
#else
    if (!port) return FREYA_ERR_PIN;
#endif

    s_bus[idx].port = port;
    s_bus[idx].bit = 1UL << FREYA_PIN_NUM(pin);
    s_bus[idx].pin = pin;
    s_bus[idx].open = 1;
    s_bus[idx].from_app = (uint8_t)(g_app.running ? 1 : 0);
    s_bus[idx].pullup = 0;
    search_stop(&s_bus[idx]);
    wire_od(&s_bus[idx]);
    return 0;
}

int w1_close(int pin)
{
    int idx = claim(pin);

    if (idx < 0) return idx;
    slot_close(idx);
    return 0;
}

int w1_reset(int pin)
{
    w1_bus_t *b;
    int rc = begin(pin, &b);

    if (rc) return rc;
    return wire_reset(b);
}

int w1_write(int pin, const void *buf, int len)
{
    const uint8_t *p = buf;
    w1_bus_t *b;
    int rc = claim(pin);

    if (rc < 0) return rc;
    if (!bytes_ok(buf, len)) return FREYA_ERR_ARG;
    rc = begin(pin, &b);
    if (rc) return rc;
    for (int i = 0; i < len; i++) {
        if (app_should_stop()) return FREYA_ERR_IO;
        byte_write(b, p[i]);
    }
    return 0;
}

int w1_read(int pin, void *buf, int len)
{
    uint8_t *p = buf;
    w1_bus_t *b;
    int rc = claim(pin);

    if (rc < 0) return rc;
    if (!bytes_ok(buf, len)) return FREYA_ERR_ARG;
    rc = begin(pin, &b);
    if (rc) return rc;
    for (int i = 0; i < len; i++) {
        if (app_should_stop()) return FREYA_ERR_IO;
        byte_read(b, &p[i]);
    }
    return 0;
}

int w1_search(int pin, void *rom)
{
    w1_bus_t *b;
    int rc;

    if (!rom) {
        rc = claim(pin);
        return (rc < 0) ? rc : FREYA_ERR_ARG;
    }
    rc = begin(pin, &b);
    if (rc) return rc;
    return do_search(b, rom);
}

int w1_pullup(int pin, int on)
{
    w1_bus_t *b;
    int idx = claim(pin);

    if (idx < 0) return idx;
    b = &s_bus[idx];
    if (!on) {
        wire_od(b);
        return 0;
    }
    b->pullup = 1;
    if (!b->port) return 0;
    board_pin_mode(b->port, FREYA_PIN_NUM(b->pin), FREYA_PIN_OUT);
    w1_hi(b);
    return 0;
}

void w1_release(void)
{
    for (int i = 0; i < FREYA_W1_BUSES; i++)
        if (s_bus[i].open && s_bus[i].from_app) slot_close(i);
}

int w1_info(int idx, w1_info_t *info)
{
    int n = 0;

    if (idx < 0 || !info) return -1;
    for (int i = 0; i < FREYA_W1_BUSES; i++) {
        if (!s_bus[i].open) continue;
        if (n == idx) {
            info->pin = s_bus[i].pin;
            info->pullup = s_bus[i].pullup;
            return 0;
        }
        n++;
    }
    return -1;
}
