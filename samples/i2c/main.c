/*
 * i2c - scan a bus, or read a register from one device on it.
 *
 *     run i2c.bin                 scan bus 1 at 100 kHz
 *     run i2c.bin 2               scan bus 2
 *     run i2c.bin 1 0x68          read one byte from address 0x68
 *     run i2c.bin 1 0x68 0x75     write 0x75, then read one byte
 *
 * Bus 1 is SCL on PB6 and SDA on PB7 on both boards.  Bus 2 is the
 * other controller, and 'i2c' at the console prints which pins it
 * uses.  Both lines need a pull-up to 3.3 V.
 */
#include "freya_api.h"
#include <stddef.h>

#define DEFAULT_HZ  100000u

static int is_digit(char c, int hex)
{
    if (c >= '0' && c <= '9') return 1;
    if (!hex) return 0;
    return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decimal, or 0x hex.  -1 is not a number. */
static int parse_uint(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int hex = 0, digits = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        hex = 1;
        s += 2;
    }
    while (is_digit(*s, hex)) {
        v = v * (hex ? 16u : 10u) + (uint32_t)digit(*s);
        s++;
        digits++;
    }
    if (!digits || *s != '\0') return -1;
    *out = v;
    return 0;
}

static const char *why(int rc)
{
    if (rc == FREYA_ERR_BUSY)    return "bus or its pins are taken";
    if (rc == FREYA_ERR_ARG)     return "out of range";
    if (rc == FREYA_ERR_NACK)    return "no answer";
    if (rc == FREYA_ERR_TIMEOUT) return "timed out";
    if (rc == FREYA_ERR_IO)      return "bus error";
    return "refused";
}

/* --------------------------------------------------------------- main */
int app_main(const freya_api_t *api, int argc, char **argv)
{
    uint32_t bus = 1, addr = 0, reg = 0;
    int have_addr = 0, have_reg = 0;
    int found = 0, rc;

    if (!FREYA_API_HAS(api, i2c_transfer)) {
        api->puts("i2c: this kernel has no I2C\r\n");
        return FREYA_EXIT_FAIL;
    }

    if (argc > 1 && parse_uint(argv[1], &bus) != 0) {
        api->puts("usage: i2c [bus] [addr] [register]\r\n");
        return FREYA_EXIT_USAGE;
    }
    if (argc > 2) {
        if (parse_uint(argv[2], &addr) != 0 || addr > 0x7F) {
            api->puts("i2c: address is a 7-bit number, decimal or 0x\r\n");
            return FREYA_EXIT_USAGE;
        }
        have_addr = 1;
    }
    if (argc > 3) {
        if (parse_uint(argv[3], &reg) != 0 || reg > 0xFF) {
            api->puts("i2c: register is one byte\r\n");
            return FREYA_EXIT_USAGE;
        }
        have_reg = 1;
    }
    if (argc > 4) {
        api->puts("usage: i2c [bus] [addr] [register]\r\n");
        return FREYA_EXIT_USAGE;
    }

    rc = api->i2c_open((int)bus, DEFAULT_HZ);
    if (rc != 0) {
        api->printf("i2c: bus %u: %s\r\n", bus, why(rc));
        return FREYA_EXIT_FAIL;
    }

    if (!have_addr) {
        api->printf("i2c: scanning bus %u at %u Hz\r\n", bus, DEFAULT_HZ);
        for (unsigned a = 0x08; a <= 0x77 && !api->should_stop(); a++) {
            rc = api->i2c_write((int)bus, (int)a, NULL, 0);
            if (rc == 0) {
                api->printf("  0x%02x\r\n", a);
                found++;
            } else if (rc != FREYA_ERR_NACK) {
                api->printf("i2c: %s\r\n", why(rc));
                api->i2c_close((int)bus);
                return FREYA_EXIT_FAIL;
            }
        }
        api->printf("i2c: %d device%s\r\n", found, found == 1 ? "" : "s");
    } else {
        uint8_t tx = (uint8_t)reg, rx = 0;

        rc = api->i2c_transfer((int)bus, (int)addr,
                               have_reg ? &tx : NULL, have_reg ? 1 : 0,
                               &rx, 1);
        if (rc != 0) {
            api->printf("i2c: 0x%02x: %s\r\n", addr, why(rc));
            api->i2c_close((int)bus);
            return FREYA_EXIT_FAIL;
        }
        api->printf("i2c: 0x%02x -> 0x%02x\r\n", addr, rx);
    }

    api->i2c_close((int)bus);
    return FREYA_EXIT_OK;
}
