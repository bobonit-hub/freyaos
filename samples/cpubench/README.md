# cpubench

CPU performance tests: Dhrystone 2.1 for the integer unit and Whetstone
for floating point. The tests are `bench/cpubench.c`, plain C, and the
Linux shell builds the same file in as its `cpubench()` command, so a
board and a PC run the same code.

```
freya: run("cpubench.bin")                      both tests, 2 s each
freya: run("cpubench.bin", "-i")                Dhrystone alone
freya: run("cpubench.bin", "-f", "-t", "5")     Whetstone alone, 5 s
```

`-t` is the seconds each test runs, 1 to 20. Ctrl-C stops it.

## The tests

* **Dhrystone 2.1** is Reinhold Weicker's C version, with its procedures,
  records and strings as he wrote them. The score is Dhrystones a second
  and DMIPS, which is that divided by 1757, the VAX 11/780's score. Once
  the runs are done every global is compared with the value Weicker
  gives, and a wrong one prints `check : FAILED` and the program fails.
* **Whetstone** is the eight sections of Roy Longbottom's C version, in
  single precision: the `float` the FPU of a Cortex-M4F, M33F or M7F
  computes. One pass is a million Whetstone instructions and the score
  is MWIPS. The values the sections leave after one pass are printed
  after it. They are the same on every target, to the last digit or
  two, which the C library's `sinf()`, `expf()` and the rest may round
  differently.

Both procedures are called, never expanded in place, which is the
ground rule of both tests. With the CPU's clock known, from
`api->cpu_hz()`, the scores are also given per MHz. The Linux command
does not know the host's clock and leaves that line out.

The program is built with `-O2`, not the `-Os` of the other samples,
since the published scores are, and links the toolchain's `libm` for
Whetstone's functions.

## Output

The format, here from `fsh` on an x86 PC:

```
freya:/home/me> cpubench("-t", 1)
Dhrystone 2.1, 1 s
  runs          : 14680063 in 1000 ms
  Dhrystones/s  : 14680063
  DMIPS         : 8355.19
  check         : ok
Whetstone, single precision, 1 s
  passes        : 1855 in 1013 ms
  MWIPS         : 1831.19
  N1 result     : -1.133247
  N2 result     : -1.133047
  N3 result     : 1.000000
  N4 result     : 12.000000
  N5 result     : 0.499910
  N6 result     : 1.000000
  N7 result     : 3.000000
  N8 result     : 0.750733
```

On a board, `DMIPS/MHz` and `MWIPS/MHz` lines follow the scores.

## Boards

Every board but the Blue Pill. Dhrystone's `Arr_2_Glob` is 10 000 bytes,
more than the Blue Pill's whole program RAM window.
