# PL/M for Freya

`plmc.py` compiles a PL/M module to a GNU assembler listing for an
ARM Cortex-M3. The listing is only a Freya flash program: code is linked
for the Blue Pill program flash region (`0x0800C080`, 29568 bytes) and
started with `runflash`. There is no floating point. `ADDRESS` and
`POINTER` are 32 bits.

```sh
python3 pl-m/plmc.py pl-m/examples/hello.plm -o hello.s
arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -nostdlib \
    -T boards/bluepill/app_flash.ld -Wl,--emit-relocs \
    hello.s -o hello.elf
python3 tools/xip_image.py hello.elf hello.xip.bin
```

Install `hello.xip.bin` and run it with `runflash`. The compiler does not
assemble or link; the `.s` file is the whole output.

## Types

| Type | Bits | Signed |
|---|---|---|
| `BYTE` | 8 | no |
| `WORD` | 16 | no |
| `INTEGER` | 32 | yes |
| `DWORD` | 32 | no |
| `ADDRESS`, `POINTER` | 32 | no |

`INTEGER` is 32 bits because this is a 32-bit machine. `REAL` and decimal
points are rejected. A true relation is `0FFH` and a false one is `00H`.
`IF` and `DO WHILE` test the low bit, as in PL/M. `AND`, `OR`, `XOR` and
`NOT` are bitwise.

## Language

A program is one module. Its statements become `app_main`.

```
NAME: DO;
  DECLARE n INTEGER, buf(8) BYTE;
  DECLARE msg(*) BYTE DATA ('hi', 0);

  step: PROCEDURE (x) INTEGER REENTRANT;
    DECLARE x INTEGER;
    RETURN x + 1;
  END step;

  CALL puts(.msg);
END NAME;
```

Declarations cover `LITERALLY`, arrays, `STRUCTURE`, `INITIAL`, `DATA`,
`BASED`, `AT`, `PUBLIC` and `EXTERNAL`. Statements cover assignment,
`CALL`, `IF THEN ELSE`, `DO`, `DO WHILE`, `DO CASE`, `DO i = a TO b BY c`,
`GOTO`, `RETURN` and `HALT`. `HALT` returns 0 to the Freya shell.

Built-ins: `LENGTH`, `LAST`, `SIZE`, `LOW`, `HIGH`, `DOUBLE`, `SHL`,
`SHR`, `ROL`, `ROR` and `MOVE`. `.name` is the address of `name`.

Locals of an ordinary procedure are static. `REENTRANT` keeps parameters
and locals on the stack (at most four parameters). Arrays and structures
are passed by reference.

`API`, `ARGC` and `ARGV` are reserved. `API` is the Freya service table,
filled in before the module body runs. `$INCLUDE(freya.plm)` declares
`puts`, `exit`, the file calls and the other non-variadic services.
`printf` is not available.

`INPUT` and `OUTPUT` are rejected: this target has no port I/O.
