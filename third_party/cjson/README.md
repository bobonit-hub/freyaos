# cJSON

[cJSON](https://github.com/DaveGamble/cJSON) v1.7.19, commit `c859b25`
(9 September 2025): `cJSON.c`, `cJSON.h`, `cJSON_Utils.c` and
`cJSON_Utils.h`, unchanged. The tests, fuzzing and build files are left
out.

The code is MIT licensed. See `LICENSE`.

Freya builds it as a library programs link,
`build/<board>/json/libfreya_cjson.a`, on the boards with the ESP32-C6
link and 192 KiB of SRAM or more. A Freya program has no C library, so `json/` supplies what cJSON
calls: `json/cjson_port.h` is included ahead of both sources and points
them at `json/port.c`. See `docs/json.md`.
