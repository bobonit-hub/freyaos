/*
 * Freya - single-shot ADC conversions.
 *
 * The program names an external pin, or one of the two abstract internal
 * sources.  This file owns source validation and pin conflicts; the ADC
 * registers, calibration and the different internal channel numbers belong
 * to the board.
 */
#include "freya.h"

typedef struct {
    uint8_t pin;
    uint8_t channel;
} adc_pin_t;

static const adc_pin_t s_adc_map[] = BOARD_ADC_MAP;

int adc_lookup(int source)
{
    if (source == FREYA_ADC_TEMP) return BOARD_ADC_TEMP_CHANNEL;
    if (source == FREYA_ADC_VREF) return BOARD_ADC_VREF_CHANNEL;
    if (source < 0 || source > 0xFF) return FREYA_ERR_PIN;

    for (unsigned i = 0; i < ARRAY_SIZE(s_adc_map); i++)
        if (s_adc_map[i].pin == (uint8_t)source) return s_adc_map[i].channel;
    return FREYA_ERR_PIN;
}

int adc_read(int source)
{
    GPIO_TypeDef *port;
    int channel, pin;
    int rc;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    channel = adc_lookup(source);
    if (channel < 0) return channel;

    /* Keep another application thread from claiming the pin or changing
     * the ADC while this short, polled conversion is in flight. */
    app_guard_enter();
    if (source != FREYA_ADC_TEMP && source != FREYA_ADC_VREF) {
        pin = source;
        if (pwm_pin_busy(pin) || i2c_owns_pin(pin) || spi_owns_pin(pin) ||
            w1_owns_pin(pin) || gpio_irq_owns_pin(pin)) {
            app_guard_leave();
            return FREYA_ERR_BUSY;
        }

        port = board_gpio_port(FREYA_PIN_PORT(pin));
        if (!port) {
            app_guard_leave();
            return FREYA_ERR_PIN;
        }
        board_pin_mode(port, FREYA_PIN_NUM(pin), FREYA_PIN_ANALOG);
    }

    rc = board_adc_read(channel);
    app_guard_leave();
    return rc;
}
