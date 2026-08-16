#!/usr/bin/env python3
"""validate_engine_bank.py — offline gates for the engine-note bank.

Reads every .wav in assets/audio/vehicles/engine_bank/ (as shipped, i.e.
post-16-bit-quantization) and asserts, per SND-SONNET's loop-seam metric:

  1. WRAP CONTINUITY: |s[0] - s[-1]| < 2x the file's median inter-sample step.
     (The old engine_loop.wav failed this by an order of magnitude — an
     audible seam tick every loop pass.)
  2. LOUDNESS MATCH: within each family (flat6_onload_*, flat6_overrun_*) the
     RMS spread is <= 1.5 dB, so an RPM-bracket crossfade never doubles as a
     volume jump. The overrun family must also sit BELOW the on-load family
     (off-throttle is quieter by design).
  3. DC-FREE: |mean| < 0.1% full scale (SND-OPUS: the old stopgap carried
     -4.8% DC — wasted headroom and a thump on every voice start/stop).
  4. BROADBAND FRACTION (flat6_* points only): > 25% of total energy sits
     OUTSIDE the harmonic bins. Owner ear-test: the pure-synth stopgap
     (~0% broadband) was rejected — "Horrible" — while the original noisy
     recording read as alive; a real engine is roughly half broadband.
     The loop length is an integer number of firing cycles, so harmonics
     land EXACTLY on FFT bins k*n_cycles; energy elsewhere (excluding a
     +/-8-bin window that covers the deliberate loop-periodic AM
     sidebands) is genuine broadband content.

Exit 0 = all gates green; exit 1 otherwise.
"""
import glob
import os
import re
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BANK = os.path.normpath(os.path.join(HERE, "..", "assets", "audio", "vehicles", "engine_bank"))
WRAP_LIMIT = 2.0
SPREAD_LIMIT_DB = 1.5
NOISE_FRAC_MIN = 0.25
HARM_WIN = 8    # bins on either side of each harmonic counted as tonal


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2, f"{path}: expected 16-bit"
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        if w.getnchannels() > 1:
            data = data.reshape(-1, w.getnchannels()).mean(axis=1)
        return data / 32768.0, w.getframerate()


def broadband_fraction(s, sr, rpm):
    """Energy outside the harmonic comb, as a fraction of total (DC excluded).
    Harmonics of f0 = rpm/60*3 sit exactly on bins k*n_cycles because the
    loop is an integer number of firing cycles."""
    f0 = rpm / 60.0 * 3.0
    n_cycles = int(round(len(s) / sr * f0))
    spec = np.abs(np.fft.rfft(s)) ** 2
    spec[0] = 0.0                                   # ignore any residual DC
    total = spec.sum()
    tonal = np.zeros(len(spec), dtype=bool)
    for k in range(1, len(spec) // max(n_cycles, 1) + 1):
        c = k * n_cycles
        tonal[max(0, c - HARM_WIN):min(len(spec), c + HARM_WIN + 1)] = True
    return float(spec[~tonal].sum() / max(total, 1e-30))


def main():
    paths = sorted(glob.glob(os.path.join(BANK, "*.wav")))
    if not paths:
        print(f"FAIL: no wavs under {BANK}")
        return 1

    ok = True
    rms_db = {}
    print(f"engine-bank validator — {len(paths)} files in {BANK}\n")
    print(f"{'file':30s} {'sr':>6s} {'len s':>7s} {'RMS dBFS':>9s} {'wrap/med':>9s} {'DC %':>6s} {'noise':>6s}  gate")
    for p in paths:
        s, sr = read_wav(p)
        name = os.path.basename(p)
        if name.startswith("src") or os.sep + "src" + os.sep in p:
            continue                                # organic source material, not a bank member
        wrap = abs(s[0] - s[-1])
        med = np.median(np.abs(np.diff(s)))
        ratio = wrap / max(med, 1e-12)
        rms = np.sqrt(np.mean(s ** 2))
        rms_db[name] = 20 * np.log10(max(rms, 1e-12))
        dc = float(np.mean(s))
        m = re.match(r"flat6_(?:onload|overrun)_(\d+)\.wav", name)
        nf = broadband_fraction(s, sr, int(m.group(1))) if m else None
        good = ratio < WRAP_LIMIT and abs(dc) < 0.001 and (nf is None or nf > NOISE_FRAC_MIN)
        ok &= good
        why = 'ok' if good else ('FAIL (seam)' if ratio >= WRAP_LIMIT
                                 else ('FAIL (DC)' if abs(dc) >= 0.001 else 'FAIL (tonal)'))
        print(f"{name:30s} {sr:6d} {len(s)/sr:7.3f} {rms_db[name]:9.2f} {ratio:8.2f}x {dc*100:6.3f} "
              f"{('%5.1f%%' % (nf*100)) if nf is not None else '    --'}  {why}")

    for fam in ("flat6_onload_", "flat6_overrun_"):
        vals = [v for k, v in rms_db.items() if k.startswith(fam)]
        if not vals:
            print(f"\nFAIL: family {fam}* missing")
            ok = False
            continue
        spread = max(vals) - min(vals)
        good = spread <= SPREAD_LIMIT_DB
        ok &= good
        print(f"\n{fam}*: {len(vals)} members, RMS spread {spread:.2f} dB "
              f"(limit {SPREAD_LIMIT_DB}) {'ok' if good else 'FAIL'}")

    on = [v for k, v in rms_db.items() if k.startswith("flat6_onload_")]
    ov = [v for k, v in rms_db.items() if k.startswith("flat6_overrun_")]
    if on and ov:
        gap = min(on) - max(ov)
        good = gap > 0.0
        ok &= good
        print(f"overrun sits {gap:.2f} dB under on-load {'ok' if good else 'FAIL'}")

    print(f"\n{'ALL GATES GREEN' if ok else 'VALIDATION FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
