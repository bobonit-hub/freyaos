# aead

`samples/aead` checks Ascon-AEAD128, or seals and opens a file with it.

With no arguments it seals an empty message under the key
`000102030405060708090a0b0c0d0e0f` and the nonce
`101112131415161718191a1b1c1d1e1f`, compares the tag with the NIST
SP 800-232 answer, then seals and opens a 16-byte block and rejects a
tag with one bit flipped.

With a key, a nonce and two paths it reads the first file and writes
the second. `-d` checks the tag and writes the plaintext. The key and
the nonce are 32 hex digits each. They are generated on the PC with
`tools/aead`, not on the board. A file that tool seals, with the same
key and nonce and no associated data, opens here, and the other way
round. The ciphertext is the plaintext with the 16-byte tag appended.
There is no header.

The calls are described in `docs/aead.md`.

```
freya: run("aead.bin")
aead: self-test ok

freya: run("aead.bin", "000102030405060708090a0b0c0d0e0f", "101112131415161718191a1b1c1d1e1f", "/notes.txt", "/notes.ct")
aead: 128 -> 144 B

freya: run("aead.bin", "-d", "000102030405060708090a0b0c0d0e0f", "101112131415161718191a1b1c1d1e1f", "/notes.ct", "/notes.txt")
aead: 144 -> 128 B
```

The STM32F103 build has no cipher. There the self-test says so.
