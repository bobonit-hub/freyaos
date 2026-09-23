/*
 * adc - print raw 12-bit samples from a pin and the internal sources.
 *
 *     run adc.bin          PA0, temperature sensor and internal reference
 *     run adc.bin B1       another external ADC pin
 */
#include "freya_api.h"

static int parse_pin(const char *s)
{
    uint32_t n = 0;
    int port;

    if (*s == 'P' || *s == 'p') s++;
    if (*s >= 'A' && *s <= 'C') port = *s - 'A';
    else if (*s >= 'a' && *s <= 'c') port = *s - 'a';
    else return -1;
    s++;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        n = n * 10U + (uint32_t)(*s++ - '0');
    }
    return (*s || n > 15) ? -1 : FREYA_PIN(port, (int)n);
}

static const char *why(int rc)
{
    if (rc == FREYA_ERR_PIN) return "not an ADC pin";
    if (rc == FREYA_ERR_BUSY) return "pin is taken";
    if (rc == FREYA_ERR_HANDLER) return "called from a handler";
    if (rc == FREYA_ERR_TIMEOUT) return "conversion timed out";
    return "refused";
}

static int show(const freya_api_t *api, const char *name, int source)
{
    int value = api->adc_read(source);

    if (value < 0) {
        api->printf("adc: %s: %s\r\n", name, why(value));
        return value;
    }
    api->printf("%s = %d / %d\r\n", name, value, FREYA_ADC_MAX);
    return 0;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    int pin = (argc > 1) ? parse_pin(argv[1]) : FREYA_PA(0);
    char name[5];

    if (!FREYA_API_HAS(api, adc_read)) {
        api->puts("adc: this kernel has no ADC\r\n");
        return FREYA_EXIT_FAIL;
    }
    if (argc > 2 || pin < 0) {
        api->puts("usage: adc [pin]    e.g. 'adc B1'\r\n");
        return FREYA_EXIT_USAGE;
    }

    name[0] = 'P';
    name[1] = (char)('A' + FREYA_PIN_PORT(pin));
    if (FREYA_PIN_NUM(pin) >= 10) {
        name[2] = '1';
        name[3] = (char)('0' + FREYA_PIN_NUM(pin) - 10);
        name[4] = '\0';
    } else {
        name[2] = (char)('0' + FREYA_PIN_NUM(pin));
        name[3] = '\0';
    }

    if (show(api, name, pin) != 0) return FREYA_EXIT_FAIL;
    if (show(api, "temp", FREYA_ADC_TEMP) != 0) return FREYA_EXIT_FAIL;
    if (show(api, "vref", FREYA_ADC_VREF) != 0) return FREYA_EXIT_FAIL;
    return FREYA_EXIT_OK;
}
