# crypt

The worked example for the symmetric cipher. With no arguments it checks
the published XTEA block vector through `api->crypt()`: eight zero bytes,
nonce `4142434445464748`, come out as `497df3d072612cb5`, and a second
pass restores the zeros.

With a key, a nonce and two paths it reads one file and writes the other.
The same command decrypts. CTR applied twice is the original bytes.

The calls are described in `docs/crypt.md`.

## Run

```
freya: run crypt.bin
crypt: self-test ok

freya: run crypt.bin 000102030405060708090a0b0c0d0e0f 4142434445464748 /notes.txt /notes.enc
crypt: 128 bytes

freya: run crypt.bin 000102030405060708090a0b0c0d0e0f 4142434445464748 /notes.enc /notes.txt
crypt: 128 bytes
```

The key is 32 hex digits and the nonce is 16, with no `0x`. The two
paths have to differ; the output is created or truncated. Ctrl-C stops
the copy and leaves whatever was already written.
