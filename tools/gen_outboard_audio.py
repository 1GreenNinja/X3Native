#!/usr/bin/env python3
"""gen_outboard_audio.py — the SPEEDBOAT OUTBOARD loop (W-RIVER).

Built on tools/gen_engine_bank.py's machinery (imported, not copied): the same
harmonic-stack + formant + loop-close pipeline that makes the car's engine
bank, retuned for a small planing-hull outboard:

  * HIGHER FIRING RATE — a 2-stroke outboard fires every rev; at ~4,800 rpm
    cruise that is an 80 Hz fundamental (the car bank idles at 45 Hz), with a
    brighter harmonic rolloff (small unmuffled cylinders).
  * PROP-CHURN AM — the propeller's blade rate beats the exhaust note as it
    ventilates near the surface. Loop-periodic by construction: the churn
    frequency is locked to an exact integer number of cycles in the loop, so
    the wrap point is inaudible (the gen_engine_bank close_loop contract).
  * WATER-SLAP NOISE — broadband noise band-passed around the hull-slap
    range (250-2200 Hz), amplitude-ridden by the churn so the spray reads as
    part of the same machine, plus a low sluicing band for the displaced wake.

One loop, both boats: the host detunes pitch per boat (startLoop3D pitch), so
the two engines beat against each other exactly like two real outboards.

Deterministic (fixed seed). Output: assets/audio/vehicles/outboard_loop.wav
(mono 16-bit 96 kHz, ~1.2 s, loop-closed). Run from anywhere:
    python tools/gen_outboard_audio.py
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# THE MACHINERY — gen_engine_bank owns the loop-close, band-pass and wav I/O.
from gen_engine_bank import (SR, close_loop, resonant_bandpass, rms, write_wav,
                             xfade_ms_for, PEAK_CEILING)

OUTDIR = os.path.normpath(os.path.join(HERE, "..", "assets", "audio", "vehicles"))

RPM        = 4800.0          # planing cruise
F0         = RPM / 60.0      # 2-stroke: fires every rev -> 80 Hz fundamental
TARGET_S   = 1.2             # loop length target (snapped to whole cycles)
CHURN_DIV  = 10              # prop churn = F0 / CHURN_DIV = 8 Hz blade beat
RMS_DBFS   = -12.0           # a notch under the car bank's on-load -11


def build_outboard(rng):
    # Loop length: an integer number of BOTH firing cycles and churn cycles.
    # churn = F0/CHURN_DIV, so any multiple of CHURN_DIV firing cycles closes
    # both. Pick the multiple nearest TARGET_S.
    cycles = max(CHURN_DIV, int(round(TARGET_S * F0 / CHURN_DIV)) * CHURN_DIV)
    loop_len = int(round(cycles / F0 * SR))
    xfade_n = int(SR * xfade_ms_for(RPM) / 1000.0)
    n_gen = loop_len + xfade_n + SR // 10
    t = np.arange(n_gen) / SR

    # ---- EXHAUST: bright small-cylinder harmonic stack. Odd harmonics get a
    # push (single-cylinder rasp); rolloff shallower than the car's (no real
    # muffler on a leg exhaust — half of it exits underwater, see slap below).
    sig = np.zeros(n_gen)
    for h in range(1, 13):
        amp = 1.0 / (h ** 1.15)
        if h % 2 == 1:
            amp *= 1.35
        # Slow, loop-periodic phase wobble so cycles are never identical
        # (the gen_engine_bank "no two cycles alike" finding).
        wob = 0.05 * np.sin(2 * np.pi * (F0 / cycles) * t * (1 + h % 3) +
                            rng.uniform(0, 2 * np.pi))
        sig += amp * np.sin(2 * np.pi * F0 * h * t + wob + rng.uniform(0, 2 * np.pi))
    # Leg-exhaust formants: the tuned pipe + the underwater exit's dull bark.
    sig = 0.55 * sig \
        + resonant_bandpass(sig, SR, 420.0, 2.2, 0.9) \
        + resonant_bandpass(sig, SR, 1350.0, 3.0, 0.5)

    # ---- PROP-CHURN AM: blade-rate beat, loop-periodic by construction.
    churn_hz = F0 / CHURN_DIV
    churn = 0.5 + 0.5 * np.sin(2 * np.pi * churn_hz * t)
    am = 0.78 + 0.22 * churn ** 1.5          # 22% depth, sharpened crests
    sig *= am

    # ---- WATER-SLAP NOISE: hull/spray band riding the churn, plus a low
    # sluicing band for the displaced wake. Noise is aperiodic; the close_loop
    # crossfade below is what seals its wrap (same treatment as the car bank's
    # turbulence layer).
    noise = rng.standard_normal(n_gen)
    slap = resonant_bandpass(noise, SR, 900.0, 0.9, 1.0)     # 250-2200-ish band
    slap += resonant_bandpass(noise, SR, 2600.0, 2.5, 0.35)  # spray sizzle
    slap *= (0.35 + 0.65 * churn)                            # spray follows the prop
    sluice = resonant_bandpass(noise, SR, 160.0, 1.2, 1.0)
    mix_noise = slap * 0.32 + sluice * 0.18

    # Energy split (the rev-2 doctrine: a real engine is ~half broadband).
    sig = sig / (rms(sig) + 1e-12) * 0.62
    mix_noise = mix_noise / (rms(mix_noise) + 1e-12) * 0.38
    out = sig + mix_noise

    # ---- Close the loop (equal-power blend + wrap rotated onto the flattest
    # interior step — gen_engine_bank's close_loop, verbatim).
    out = close_loop(out, loop_len, xfade_n)

    # DC removal + loudness target + headroom.
    out = out - np.mean(out)
    out *= 10 ** (RMS_DBFS / 20.0) / (rms(out) + 1e-12)
    peak = np.max(np.abs(out))
    if peak > PEAK_CEILING:
        out *= PEAK_CEILING / peak
    return out


def main():
    rng = np.random.default_rng(0xB0A7)
    out = build_outboard(rng)
    os.makedirs(OUTDIR, exist_ok=True)
    path = os.path.join(OUTDIR, "outboard_loop.wav")
    write_wav(path, out)
    wrap = abs(out[0] - out[-1])
    med = float(np.median(np.abs(np.diff(out))))
    print(f"outboard_loop.wav  {len(out)/SR:.3f}s  RMS {20*np.log10(rms(out)):.2f} dBFS  "
          f"peak {20*np.log10(np.max(np.abs(out))):.2f} dBFS  "
          f"wrap/median-step {wrap/max(med,1e-12):.2f}x")


if __name__ == "__main__":
    main()
