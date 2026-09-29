# Ascon-AEAD128

Reference implementation of Ascon-AEAD128 from
[ascon-c](https://github.com/ascon/ascon-c), commit `446347f`
(28 January 2026), the NIST SP 800-232 final version. The files are
`crypto_aead/asconaead128/ref` plus the `crypto_aead.h` declaration
that implementation includes. `printstate.c` is left out: it is only
compiled when `ASCON_PRINT_STATE` is set, and that pulls in stdio.

The code is CC0 1.0. See `LICENSE`.

Freya calls `crypto_aead_encrypt` and `crypto_aead_decrypt` from
`src/aead.c`. The STM32F103 build does not link this code.
