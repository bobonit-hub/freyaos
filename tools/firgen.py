#!/usr/bin/env python3
"""
Freya - the resampling filters of src/audio.c.

A USB headset may run at 2, 3 or 6 times the rate a program asks for
(16 kHz from 32 or 48 kHz, 8 kHz from 16, 24 or 48 kHz).  One low-pass
FIR per ratio k serves both ways: decimation (the microphone) runs it at
the headset's rate and keeps every k-th output, interpolation (the
speaker) runs its k polyphase branches.

Kaiser-windowed sinc, 24 taps per phase, -6 dB at the program's Nyquist
rate.  The telephone band is flat to 0.03 dB up to 0.85 of Nyquist (6.8 kHz
at 16 kHz, 3.4 kHz at 8 kHz), and what would alias from beyond 1.2 of
Nyquist is down more than 60 dB.  The taps are Q15 and sum to exactly
32768.

    python3 tools/firgen.py            # print the C tables
    python3 tools/firgen.py --check    # print the response as well
"""
import math
import sys

TAPS_PER_PHASE = 24
BETA = 6.0
CUTOFF = 0.5           # of the program's Nyquist rate: -6 dB there
RATIOS = (2, 3, 6)


def i0(x):
    s, t, k = 1.0, 1.0, 1
    while t > 1e-12 * s:
        t *= (x / (2 * k)) ** 2
        s += t
        k += 1
    return s


def design(k):
    n = TAPS_PER_PHASE * k
    fc = CUTOFF / k                     # cycles per headset sample / 2
    mid = (n - 1) / 2
    h = []
    for i in range(n):
        x = i - mid
        sinc = 2 * fc if x == 0 else math.sin(2 * math.pi * fc * x) / (math.pi * x)
        w = i0(BETA * math.sqrt(1 - (2 * i / (n - 1) - 1) ** 2)) / i0(BETA)
        h.append(sinc * w)
    s = sum(h)
    q = [round(v / s * 32768) for v in h]
    q[n // 2] += 32768 - sum(q)         # exact unity gain at DC
    return q


def response(q, f):
    """Gain at f, in cycles per headset sample."""
    re = sum(c * math.cos(2 * math.pi * f * i) for i, c in enumerate(q))
    im = sum(c * math.sin(2 * math.pi * f * i) for i, c in enumerate(q))
    return math.hypot(re, im) / 32768


def db(g):
    return 20 * math.log10(max(g, 1e-9))


def check(k, q):
    nyq = 0.5 / k                       # the program's Nyquist, per headset sample
    ripple = max(abs(db(response(q, 0.85 * nyq * i / 50))) for i in range(51))
    lo = 1.2 * nyq
    stop = max(db(response(q, lo + (0.5 - lo) * i / 400)) for i in range(401))
    print(f"/* k={k}: passband ripple {ripple:.2f} dB to 0.85 Nyquist, "
          f"stopband {stop:.1f} dB from 1.2 Nyquist */")


def main():
    show = "--check" in sys.argv
    for k in RATIOS:
        q = design(k)
        if show:
            check(k, q)
        print(f"static const int16_t s_fir{k}[{len(q)}] = {{")
        for i in range(0, len(q), 8):
            print("    " + ", ".join(f"{c:6d}" for c in q[i:i + 8]) + ",")
        print("};")


if __name__ == "__main__":
    main()
