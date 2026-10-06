/* pretend.c - the pins of a board that is not there.
 *
 * The system calls PIN, PWM and ADC make, for the builds that run on a
 * PC: hostrt.c, which basic-host and runbasic share, and armrt.c, the
 * board's own code under qemu-arm.  No C library; included by both.
 */

/* The pins of a pretend board, so that PIN, PWM and ADC can be tested
 * where there is no hardware: ports A, B and C of 16 pins, every one
 * an input reading 0 until it is driven, PA2 and PA3 kept back as the
 * console's are on the boards, and an input with a pull-up or a
 * pull-down reads what the pull makes it until it is driven.  A PWM
 * channel is any pin of ports A
 * and B.  The ADC reads PA0-PA7, PB0, PB1 and PC0-PC5, and answers
 * 2048 from a pin, 1000 from TEMP and 1500 from VREF. */
#define HOST_PORTS 3

static struct {
    uint8_t mode, level, pwm;
} pins[HOST_PORTS * 16];

static int pin_ok(int pin)
{
    if (pin < 0 || pin >= HOST_PORTS * 16) return 0;
    return pin != 2 && pin != 3;
}

int sys_pin_mode(int pin, int mode)
{
    if (!pin_ok(pin)) return SYS_EPIN;
    if (mode < SYS_PIN_IN || mode > SYS_PIN_ANALOG) return SYS_EARG;
    pins[pin].mode = (uint8_t)mode;
    /* an input nothing drives follows its pull */
    if (mode == SYS_PIN_IN_PULLUP) pins[pin].level = 1;
    if (mode == SYS_PIN_IN_PULLDOWN) pins[pin].level = 0;
    return 0;
}

int sys_pin_read(int pin)
{
    if (!pin_ok(pin)) return SYS_EPIN;
    return pins[pin].level;
}

int sys_pin_write(int pin, int level)
{
    if (!pin_ok(pin)) return SYS_EPIN;
    pins[pin].level = level ? 1 : 0;
    return 0;
}

int sys_pin_toggle(int pin)
{
    if (!pin_ok(pin)) return SYS_EPIN;
    pins[pin].level ^= 1;
    return 0;
}

int sys_pwm(int pin, uint32_t hz, uint32_t duty)
{
    if (!pin_ok(pin) || pin >= 32) return SYS_EPIN;
    if (hz == 0) {
        if (!pins[pin].pwm) return SYS_EARG;
        pins[pin].pwm = 0;
        pins[pin].mode = SYS_PIN_IN;
        return 0;
    }
    if (hz > 1000000 || duty > SYS_PWM_FULL) return SYS_EARG;
    pins[pin].pwm = 1;
    return 0;
}

int sys_adc(int source)
{
    if (source == SYS_ADC_TEMP) return 1000;
    if (source == SYS_ADC_VREF) return 1500;
    if (!pin_ok(source)) return SYS_EPIN;
    if (!(source <= 7 || source == 16 || source == 17 ||
          (source >= 32 && source <= 37)))
        return SYS_EPIN;
    pins[source].mode = SYS_PIN_ANALOG;
    return 2048;
}
