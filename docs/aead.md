# Authenticated encryption

A program can seal and open messages with Ascon-AEAD128, NIST
SP 800-232. The key is 16 bytes, the nonce is 16 and the tag is 16,
appended to the ciphertext. Associated data is authenticated and is not
encrypted. The console drives the same calls with `aead`, using an empty
associated data.

The key is not generated on the board. `tools/aead key` and
`tools/aead nonce` read 16 bytes from `/dev/urandom` and print them as
hex. `tools/aead seal` and `tools/aead open` use the reference code in
`third_party/ascon`, so a file sealed on the PC opens on the board and
the other way round. The shell and the tool both use an empty associated
data and write the ciphertext with the tag and no header.

The STM32F103 has no room for the cipher: there both calls return
`FREYA_ERR_UNSUPPORTED` and `aead` says so. The Black Pill and the
STM32F405 have it.

`samples/aead` checks two published answers, or seals a file on the
card. Build it with `make` and run `run("aead.bin")`.

## Calls

```c
int (*aead_encrypt)(const void *key, const void *nonce,
                    const void *ad, int ad_len,
                    const void *in, int in_len,
                    void *out, int out_cap);
int (*aead_decrypt)(const void *key, const void *nonce,
                    const void *ad, int ad_len,
                    const void *in, int in_len,
                    void *out, int out_cap);
```

`aead_encrypt()` writes `in_len + FREYA_AEAD_TAG_LEN` bytes and returns
that length. `aead_decrypt()` takes a buffer whose last 16 bytes are the
tag and returns the plaintext length. `ad` may be null when `ad_len` is
0. A plaintext of length zero is a real message: the ciphertext is the
tag alone.

```c
uint8_t ct[sizeof text + FREYA_AEAD_TAG_LEN];
uint8_t back[sizeof text];
int n, m;

n = api->aead_encrypt(key, nonce, NULL, 0, text, sizeof text, ct, sizeof ct);
m = api->aead_decrypt(key, nonce, NULL, 0, ct, n, back, sizeof back);
```

* `FREYA_ERR_ARG`: a length below zero, a length above
  `FREYA_AEAD_MAX_LEN`, a null pointer where the length is above zero,
  a ciphertext shorter than the tag, `in` and `out` overlapping, `ad`
  and `out` overlapping, or `out` too small. The ciphertext of `n`
  bytes always fits in `n + FREYA_AEAD_TAG_LEN`.
* `FREYA_ERR_IO`: the tag does not match. `out` is then zeros.
* `FREYA_ERR_UNSUPPORTED`: the board has no cipher.

A handler may call either one. Neither allocates, and neither keeps the
key.

These calls were appended to the service table. A program built against
this header and handed an older kernel checks before it calls:

```c
if (!FREYA_API_HAS(api, aead_decrypt)) {
    api->puts("this kernel has no cipher\r\n");
    return FREYA_EXIT_FAIL;
}
```

## What a call does

Ascon-AEAD128 absorbs the associated data and the plaintext 16 bytes at
a time, with the 12-round permutation at the start and the end and the
8-round permutation in between. The key is mixed in at both ends. The
tag is the last 16 bytes of the state. Opening recomputes that tag and
compares it with the one on the message. A nonce is used with one
plaintext under one key. The next piece of a longer file is a new nonce,
not an offset: the tag covers the whole message, and a message longer
than `FREYA_AEAD_MAX_LEN` (4096 bytes) is refused.

`aead` at the console takes the key and the nonce as hex and two paths.
With no arguments it names the cipher and prints the usage. `-d` opens
instead of sealing. A line that carries a key is left out of the shell
history.

```
freya: aead("000102030405060708090a0b0c0d0e0f", "101112131415161718191a1b1c1d1e1f", "/notes.txt", "/notes.ct")
aead: 128 -> 144 B
freya: aead("-d", "000102030405060708090a0b0c0d0e0f", "101112131415161718191a1b1c1d1e1f", "/notes.ct", "/notes.txt")
aead: 144 -> 128 B
```
