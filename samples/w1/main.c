/*
 * w1 - find every 1-Wire device on a pin and read the thermometers.
 *
 *     run w1.bin              DS18B20 / DS1822 on PB12
 *     run w1.bin B0           the same pin, named the way 'pin' names it
 *     run w1.bin PB1
 *
 * The data pin needs a pull-up to 3.3 V.  4.7 kΩ is the ordinary value.
 * A convert holds the line high for 750 ms, which is long enough for a
 * 12-bit conversion and is what a parasite-powered sensor draws its
 * current from.  A sensor with its own supply does not mind.
 */
#include "freya_api.h"

#define DEFAULT_PIN    FREYA_PB(12)
#define CONVERT_MS     750
#define DEV_MAX        8

static int parse_pin(const char *s)
{
    int port;
    uint32_t n = 0;

    if (*s == 'P' || *s == 'p') s++;
    if (*s >= 'A' && *s <= 'C') port = *s - 'A';
    else if (*s >= 'a' && *s <= 'c') port = *s - 'a';
    else return -1;
    s++;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        n = n * 10u + (uint32_t)(*s - '0');
        s++;
    }
    if (*s || n > 15) return -1;
    return FREYA_PIN(port, (int)n);
}

static const char *why(int rc)
{
    if (rc == FREYA_ERR_PIN)     return "not a pin Freya hands out";
    if (rc == FREYA_ERR_BUSY)    return "that pin is taken";
    if (rc == FREYA_ERR_ARG)     return "out of range";
    if (rc == FREYA_ERR_NACK)    return "no answer";
    if (rc == FREYA_ERR_TIMEOUT) return "the line stayed low";
    if (rc == FREYA_ERR_IO)      return "bus error";
    return "refused";
}

static void put_pin(const freya_api_t *api, int pin)
{
    api->printf("P%c%d", 'A' + FREYA_PIN_PORT(pin), FREYA_PIN_NUM(pin));
}

static void put_rom(const freya_api_t *api, const uint8_t *rom)
{
    for (int i = 0; i < FREYA_W1_ROM_LEN; i++) {
        if (i) api->putc(' ');
        api->printf("%02x", rom[i]);
    }
}

static void put_temp(const freya_api_t *api, int32_t milli)
{
    int32_t a = milli < 0 ? -milli : milli;

    api->printf("%s%d.%03d C", milli < 0 ? "-" : "",
                (int)(a / 1000), (int)(a % 1000));
}

/* --------------------------------------------------------------- main */
int app_main(const freya_api_t *api, int argc, char **argv)
{
    uint8_t rom[DEV_MAX][FREYA_W1_ROM_LEN];
    uint8_t skip[2] = { 0xCC, 0x44 };
    int pin = (argc > 1) ? parse_pin(argv[1]) : DEFAULT_PIN;
    int n = 0, printed = 0, rc;

    if (!FREYA_API_HAS(api, w1_crc)) {
        api->puts("w1: this kernel has no 1-Wire\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc > 2 || pin < 0) {
        api->puts("usage: w1 [pin]    e.g. 'w1 B12'\r\n");
        return FREYA_EXIT_USAGE;
    }

    rc = api->w1_open(pin);
    if (rc != 0) {
        api->printf("w1: P%c%d: %s\r\n", 'A' + FREYA_PIN_PORT(pin),
                    FREYA_PIN_NUM(pin), why(rc));
        return FREYA_EXIT_FAIL;
    }

    api->puts("w1: ");
    put_pin(api, pin);
    api->puts("\r\n");

    while (n < DEV_MAX && !api->should_stop()) {
        rc = api->w1_search(pin, rom[n]);
        if (rc == FREYA_ERR_NACK) break;
        if (rc != 0) {
            api->printf("w1: %s\r\n", why(rc));
            api->w1_close(pin);
            return FREYA_EXIT_FAIL;
        }
        n++;
    }
    if (api->should_stop()) {
        api->w1_close(pin);
        return FREYA_EXIT_OK;
    }
    if (n == 0) {
        api->puts("w1: no device\r\n");
        api->w1_close(pin);
        return FREYA_EXIT_FAIL;
    }

    rc = api->w1_reset(pin);
    if (rc == 0) rc = api->w1_write(pin, skip, 2);
    if (rc == 0) rc = api->w1_pullup(pin, 1);
    if (rc != 0) {
        api->printf("w1: %s\r\n", why(rc));
        api->w1_close(pin);
        return FREYA_EXIT_FAIL;
    }
    for (int t = 0; t < CONVERT_MS && !api->should_stop(); t += 10)
        api->delay_ms(10);
    api->w1_pullup(pin, 0);
    if (api->should_stop()) {
        api->w1_close(pin);
        return FREYA_EXIT_OK;
    }

    for (int i = 0; i < n && !api->should_stop(); i++) {
        uint8_t tx[10], sp[9];
        int16_t raw;

        tx[0] = 0x55;
        for (int b = 0; b < FREYA_W1_ROM_LEN; b++) tx[1 + b] = rom[i][b];
        tx[9] = 0xBE;
        rc = api->w1_reset(pin);
        if (rc == 0) rc = api->w1_write(pin, tx, 10);
        if (rc == 0) rc = api->w1_read(pin, sp, 9);
        if (rc != 0) {
            api->printf("w1: %s\r\n", why(rc));
            api->w1_close(pin);
            return FREYA_EXIT_FAIL;
        }
        put_rom(api, rom[i]);
        if (api->w1_crc(sp, 9) != 0) {
            api->puts("  bad crc\r\n");
            continue;
        }
        if (rom[i][0] != 0x28 && rom[i][0] != 0x22) {
            api->printf("  family %02x\r\n", rom[i][0]);
            continue;
        }
        raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
        api->puts("  ");
        put_temp(api, (int32_t)raw * 1000 / 16);
        api->puts("\r\n");
        printed++;
    }

    api->w1_close(pin);
    if (api->should_stop()) return FREYA_EXIT_OK;
    if (!printed) return FREYA_EXIT_FAIL;
    api->printf("w1: %d device%s\r\n", printed, printed == 1 ? "" : "s");
    return FREYA_EXIT_OK;
}
