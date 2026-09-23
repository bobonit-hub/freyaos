/*
 * spi - loop a bus back to itself, or read a flash chip's JEDEC id.
 *
 *     run spi.bin              MOSI wired to MISO, bus 1, 1 MHz, mode 0
 *     run spi.bin id           JEDEC id, chip select on PB12
 *     run spi.bin id PB10      the same, chip select on PB10
 *
 * Bus 1 is SPI2 on both boards: SCK PB13, MISO PB14, MOSI PB15.  The
 * card keeps SPI1, so these pins are not the socket.  Chip select is
 * whichever spare pin the device uses; the loopback test does not
 * need one.
 */
#include "freya_api.h"

#define BUS_HZ      1000000u
#define DEFAULT_CS  FREYA_PB(12)

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

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
    if (rc == FREYA_ERR_BUSY)    return "bus or its pins are taken";
    if (rc == FREYA_ERR_ARG)     return "out of range";
    if (rc == FREYA_ERR_TIMEOUT) return "timed out";
    if (rc == FREYA_ERR_IO)      return "bus error";
    if (rc == FREYA_ERR_PIN)     return "not a pin Freya hands out";
    return "refused";
}

static int loopback(const freya_api_t *api)
{
    static const uint8_t pat[] = {
        0x00, 0xFF, 0xA5, 0x5A, 0x01, 0x80, 0x0F, 0xF0
    };
    uint8_t rx[sizeof pat];
    int rc, bad = 0;

    rc = api->spi_transfer(1, pat, rx, (int)sizeof pat);
    if (rc != 0) {
        api->printf("spi: %s\r\n", why(rc));
        return FREYA_EXIT_FAIL;
    }
    for (unsigned i = 0; i < sizeof pat; i++) {
        api->printf("  %02x -> %02x%s\r\n", pat[i], rx[i],
                    rx[i] == pat[i] ? "" : "  mismatch");
        if (rx[i] != pat[i]) bad++;
    }
    if (bad) {
        api->puts("spi: tie MOSI to MISO and try again\r\n");
        return FREYA_EXIT_FAIL;
    }
    api->puts("spi: loopback ok\r\n");
    return FREYA_EXIT_OK;
}

/* 0x9F then three clocks.  The id comes back in the three bytes after
 * the command; the first received byte is whatever the line was doing
 * while the command went out. */
static int jedec(const freya_api_t *api, int cs)
{
    uint8_t tx[4] = { 0x9F, 0xFF, 0xFF, 0xFF };
    uint8_t rx[4];
    int rc;

    rc = api->pin_write(cs, 1);
    if (rc == 0) rc = api->pin_mode(cs, FREYA_PIN_OUT);
    if (rc == 0) rc = api->pin_write(cs, 0);
    if (rc != 0) {
        api->printf("spi: chip select: %s\r\n", why(rc));
        return FREYA_EXIT_FAIL;
    }
    rc = api->spi_transfer(1, tx, rx, 4);
    (void)api->pin_write(cs, 1);
    if (rc != 0) {
        api->printf("spi: %s\r\n", why(rc));
        return FREYA_EXIT_FAIL;
    }
    if (rx[1] == 0x00 || rx[1] == 0xFF) {
        api->printf("spi: no device (got %02x %02x %02x)\r\n",
                    rx[1], rx[2], rx[3]);
        return FREYA_EXIT_FAIL;
    }
    api->printf("spi: manufacturer %02x  type %02x  capacity %02x\r\n",
                rx[1], rx[2], rx[3]);
    return FREYA_EXIT_OK;
}

/* --------------------------------------------------------------- main */
int app_main(const freya_api_t *api, int argc, char **argv)
{
    int cs = DEFAULT_CS;
    int id = 0;
    int rc;

    if (!FREYA_API_HAS(api, spi_transfer)) {
        api->puts("spi: this kernel has no SPI\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc > 1) {
        if (!same(argv[1], "id")) {
            api->puts("usage: spi [id [cs-pin]]\r\n");
            return FREYA_EXIT_USAGE;
        }
        id = 1;
    }
    if (argc > 2) {
        cs = parse_pin(argv[2]);
        if (cs < 0) {
            api->puts("spi: chip select is a pin, as in PB12\r\n");
            return FREYA_EXIT_USAGE;
        }
    }
    if (argc > 3) {
        api->puts("usage: spi [id [cs-pin]]\r\n");
        return FREYA_EXIT_USAGE;
    }

    rc = api->spi_open(1, BUS_HZ, FREYA_SPI_MODE0);
    if (rc != 0) {
        api->printf("spi: bus 1: %s\r\n", why(rc));
        return FREYA_EXIT_FAIL;
    }
    rc = id ? jedec(api, cs) : loopback(api);
    api->spi_close(1);
    return rc;
}
