# ADC

Freya exposes one synchronous 12-bit conversion through ADC1. It can sample
an external analog pin, the chip temperature sensor, or the internal voltage
reference. The call is polled: it returns after one conversion and does not
use an interrupt or DMA.

## Call

```c
int (*adc_read)(int source);       /* 0..FREYA_ADC_MAX, or FREYA_ERR_* */
```

`source` is an ADC pin, `FREYA_ADC_TEMP`, or `FREYA_ADC_VREF`.
`FREYA_ADC_MAX` is 4095. Internal sources return raw counts; converting the
temperature or reference reading to engineering units requires the
chip-specific factory calibration data and supply voltage.

The external pins supported on both boards are PA0, PA1, PB0, PB1 and
PC0..PC5. A read puts that pin in `FREYA_PIN_ANALOG` mode and leaves it
there. PA0, PA1, PB0 and PB1 are also PWM pins, so an active PWM channel
must be closed first. ADC likewise refuses a pin owned by I2C, SPI, 1-Wire,
or a pin interrupt.

```c
int raw = api->adc_read(FREYA_PA(0));
if (raw >= 0)
    api->printf("%d / %d\r\n", raw, FREYA_ADC_MAX);
```

The input range is ground through VDDA. Do not drive a pin below ground or
above VDDA. For a known VDDA in millivolts, an ideal external voltage is
`raw * VDDA / FREYA_ADC_MAX`.

This call was appended to the service table. Programs that may meet an older
kernel check it first:

```c
if (!FREYA_API_HAS(api, adc_read)) {
    api->puts("this kernel has no ADC\r\n");
    return FREYA_EXIT_FAIL;
}
```

## Errors

- `FREYA_ERR_PIN`: the source is not an ADC-capable pin.
- `FREYA_ERR_BUSY`: another peripheral or interrupt owns the pin.
- `FREYA_ERR_HANDLER`: called from a pin or timer handler.
- `FREYA_ERR_TIMEOUT`: calibration or conversion did not finish.

Neither currently supported MCU has a hardware DAC, so Freya does not expose
a DAC call. PWM remains available when a filtered duty-cycle output is
appropriate.

## Console

`adc PA0`, `adc temp`, and `adc vref` perform the same call and print the raw
count:

```
freya: adc PA0
PA0 = 2037 / 4095
```
