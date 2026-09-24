# Symmetric encryption

A program can encrypt and decrypt with XTEA in CTR mode. The key is 16
bytes, the nonce is 8, and the same call does both directions. It is
arithmetic: nothing is kept between calls, and a handler may use it.
The console drives the same call with `crypt`.

`samples/crypt` checks the published block vector, or encrypts a file
on the card. Build it with `make` and run `run crypt.bin`.

## Calls

```c
int (*crypt)(const void *key, const void *nonce, uint32_t off,
             const void *in, void *out, int len);
```

`key` is `FREYA_CRYPT_KEY_LEN` (16) bytes. `nonce` is
`FREYA_CRYPT_NONCE_LEN` (8) bytes. `off` is the position of `in[0]` in
the message, so a message longer than one call is split by the caller:

```c
uint8_t key[16], nonce[8], buf[256];

/* fill key and nonce, then: */
api->crypt(key, nonce, 0, plain, buf, 256);          /* encrypt */
api->crypt(key, nonce, 0, buf, buf, 256);            /* decrypt, in place */
api->crypt(key, nonce, 256, plain2, buf2, 100);      /* the next piece */
```

`in` and `out` may be the same buffer, or they may overlap. A length of
zero does nothing and the pointers may be null. A length below zero,
above `FREYA_CRYPT_MAX_LEN` (4096), or that would run past the last
byte of a 4 GiB message, returns `FREYA_ERR_ARG`.

These calls were appended to the service table. A program built against
this header and handed an older kernel checks before it calls:

```c
if (!FREYA_API_HAS(api, crypt)) {
    api->puts("this kernel has no cipher\r\n");
    return FREYA_EXIT_FAIL;
}
```

## What a call does

XTEA is 32 rounds (`FREYA_CRYPT_ROUNDS`). A block is 8 bytes, two
big-endian 32-bit words, which is the order the bytes are written in
hex. The constant added each round is `0x9E3779B9`.

The nonce is the counter for byte 0, as a big-endian 64-bit number.
Block `n` of the keystream is that nonce plus `n`. Byte `off` is xored
with byte `off % 8` of block `off / 8`. The key is not stored, and two
calls with the same key, nonce and offset produce the same keystream,
so encrypting twice returns the original bytes. Reusing a nonce under
one key repeats that keystream.

The published block vector is the first block of that keystream. The
key `000102030405060708090a0b0c0d0e0f` encrypts the block
`4142434445464748` to `497df3d072612cb5`. CTR of eight zero bytes with
that block as the nonce is the same eight bytes:

```
freya: crypt 000102030405060708090a0b0c0d0e0f 4142434445464748 0000000000000000
497df3d072612cb5
freya: crypt 000102030405060708090a0b0c0d0e0f 4142434445464748 497df3d072612cb5
0000000000000000
```

`crypt` with no arguments prints the usage and succeeds. The key is 32
hex digits, the nonce is 16 and the data is one hex word, with no `0x`.
Upper and lower case are the same. The line holds about 50 data bytes.
A file is `samples/crypt`.
