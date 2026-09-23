# adc

Reads one external ADC pin, then the internal temperature sensor and voltage
reference. All three values are raw 12-bit counts.

```
run adc.bin          # PA0
run adc.bin B1       # PB1
```

Connect an external voltage only between ground and 3.3 V. The common ADC
pins are PA0, PA1, PB0, PB1 and PC0..PC5. See `docs/adc.md` for pin conflicts,
the voltage formula, and internal-source calibration notes.
