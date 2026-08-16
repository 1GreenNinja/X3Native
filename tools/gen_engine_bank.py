#!/usr/bin/env python3
"""gen_engine_bank.py — the multi-RPM engine-note bank (W-ENGINE-NOTE).

Descends from SND-SONNET's diagnostic generator (synth_flat6.py, job 434bd27a):
sum of the first 10 harmonics of the flat-six firing frequency f0 = rpm/60*3,
1/n^1.15 rolloff, firing-rate "chuff" AM, two resonant exhaust formants
(pipe boom ~3*f0, mid rasp ~14*f0), RPM-scaled turbulence noise, loop length
snapped to an INTEGER number of firing cycles so the tonal component is
phase-exact at the wrap.

What this version adds over the diagnostic candidates (the README's two
flagged pre-ship steps, plus the overrun family the runtime needs):

 1. WIDER LOOP CROSSFADES — 40 ms on the lower half of the bank, 60 ms on the
    top three RPM points (the diagnostic's 25 ms left a wrap residual up to
    3.5x the median sample step on the redline point).
 2. WRAP-POINT ROTATION — after the loop is closed it is a continuous circle,
    so we rotate it to place the file boundary on the FLATTEST interior step.
    The wrap step is then <= the median inter-sample step by construction
    (this is the "match state at the wrap point explicitly" fix the README
    suggested, done exactly).
 3. LOUDNESS MATCHING — every on-load member is RMS-normalized to one target
    (-11 dBFS); every overrun member to target-6 dB. The diagnostic candidates
    were peak-normalized independently and spread ~1.7 dB.
 4. OVERRUN VARIANTS — one per RPM point: same engine, off-throttle character.
    Lower level (baked -6 dB), darker harmonic rolloff, IRREGULAR sub-harmonic
    AM (components at n/2 +/- 1..2 cycles-per-loop around the half-order, so
    the burble beats irregularly yet stays exactly loop-periodic), and crackle
    transients (short bandpassed noise bursts, rate scaling with RPM) kept out
    of the crossfade region so the seam stays clean.
 5. DEDICATED WHISTLE ASSETS — whine_loop.wav (supercharger gear whine) and
    turbo_whistle_loop.wav (breathy narrowband spool whistle), so the hosts'
    whine/turbo layers stop being pitched-up copies of the engine wav
    (SND-FABLE finding #3).
 6. SND-OPUS engine-internals items: every output is DC-FREE (mean-subtracted
    — the loop-safe equivalent of a 5 Hz high-pass; the old stopgap measured
    -4.8% DC); every engine member carries ~1-2 dB of SLOW loop-periodic
    pseudo-random pulse-level variation (a real engine's cycles are never
    identical — the pure-harmonic stack's zero variation is the machine tell);
    and engine_noise_bed.wav is a broadband turbulence loop the runtime rides
    as a 5th voice with gain following LOAD (the synth stack is ~99% harmonic
    while a real engine is ~half broadband — baked noise can't follow load,
    a separate voice can).

Output: <repo>/assets/audio/vehicles/engine_bank/*.wav (mono 16-bit 96 kHz).
Deterministic (fixed seeds). Validate with tools/validate_engine_bank.py.
"""
import os
import wave

import numpy as np
from scipy.signal import butter, sosfilt

SR = 96000
HERE = os.path.dirname(os.path.abspath(__file__))
OUTDIR = os.path.normpath(os.path.join(HERE, "..", "assets", "audio", "vehicles", "engine_bank"))

RPM_POINTS = [
    ("idle",     900),
    ("low",     1500),
    ("mid1",    2500),
    ("mid2",    4000),
    ("high",    5500),
    ("redline", 7000),
]
TARGET_LOOP_S = 1.2
NHARM = 10
ONLOAD_RMS_DBFS = -11.0     # bank-wide loudness target (README pre-ship step 2)
OVERRUN_DB_BELOW = 6.0      # overrun family sits this far under the on-load family
PEAK_CEILING = 10 ** (-1.0 / 20.0)   # -1 dBFS


def write_wav(path, samples, sr=SR):
    samples = np.clip(samples, -1.0, 1.0)
    ints = (samples * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(ints.tobytes())


def resonant_bandpass(sig, sr, center_hz, q, gain):
    low = max(10.0, center_hz * (1 - 1 / (2 * q)))
    high = min(sr / 2 - 100, center_hz * (1 + 1 / (2 * q)))
    sos = butter(2, [low, high], btype="band", fs=sr, output="sos")
    return gain * sosfilt(sos, sig)


def xfade_ms_for(rpm):
    """README pre-ship step 1: 40-60 ms on the top three points; 40 ms is also
    enough to retire the mid1 seam the ranking flagged."""
    return 60 if rpm >= 4000 else 40


def close_loop(seg_ext, loop_len, xfade_n):
    """Blend the loop head into the material that follows the loop point
    (equal-power), then ROTATE so the file boundary lands on the flattest
    interior step — the wrap becomes just another (small) sample step."""
    loop = seg_ext[:loop_len].copy()
    tt = np.linspace(0, 1, xfade_n, endpoint=False)
    fade_in, fade_out = np.sin(tt * np.pi / 2), np.cos(tt * np.pi / 2)
    loop[:xfade_n] = loop[:xfade_n] * fade_in + seg_ext[loop_len:loop_len + xfade_n] * fade_out
    # rotation: the closed loop is continuous everywhere, so any rotation is
    # equally seamless — pick the boundary with the smallest step.
    steps = np.abs(np.diff(loop))
    r = int(np.argmin(steps)) + 1
    return np.roll(loop, -r)


def synth_engine(rpm, *, overrun, rng):
    f0 = rpm / 60.0 * 3.0                       # flat-six firing frequency
    n_cycles = max(2, round(TARGET_LOOP_S * f0))
    if overrun and n_cycles % 2:
        n_cycles += 1                           # half-order AM must be loop-periodic
    loop_s = n_cycles / f0
    loop_len = int(round(loop_s * SR))
    xfade_n = int(SR * xfade_ms_for(rpm) / 1000)
    n_gen = loop_len + xfade_n
    t = np.arange(n_gen) / SR

    # harmonic stack (darker rolloff off-throttle: the intake side shuts)
    rolloff = 1.35 if overrun else 1.15
    sig = np.zeros(n_gen)
    for h in range(1, NHARM + 1):
        sig += (1.0 / h ** rolloff) * np.sin(2 * np.pi * f0 * h * t + 0.15 * h)
    sig /= np.max(np.abs(sig))

    # firing-rate chuff
    sig *= 1.0 + 0.15 * np.sin(2 * np.pi * f0 * t)

    # SLOW PSEUDO-RANDOM PULSE-LEVEL VARIATION (SND-OPUS): ~1-2 dB of drift
    # built from loop-periodic sinusoids (k cycles per loop, k small + coprime)
    # with random phases — cycle-to-cycle level never repeats inside the loop,
    # yet the wrap stays exact.
    drift = np.zeros(n_gen)
    for k_cpl, depth in ((2, 0.07), (3, 0.05), (5, 0.04), (7, 0.03)):
        drift += depth * np.sin(2 * np.pi * k_cpl / loop_s * t + rng.uniform(0, 2 * np.pi))
    sig *= 1.0 + drift

    if overrun:
        # IRREGULAR SUB-HARMONIC AM (burble): components clustered on the
        # half-order. Frequencies are k / loop_s for integer k, so every
        # component is exactly loop-periodic; the +/-1, +/-2 neighbours beat
        # against the half-order and the burble never repeats inside the loop.
        k_half = n_cycles // 2
        am = np.zeros(n_gen)
        for dk, depth in ((0, 0.30), (-1, 0.16), (+1, 0.14), (-2, 0.09), (+2, 0.07)):
            am += depth * np.sin(2 * np.pi * (k_half + dk) / loop_s * t + rng.uniform(0, 2 * np.pi))
        sig *= 1.0 + np.clip(am, -0.85, 0.85)

    # exhaust formants
    boom = resonant_bandpass(sig, SR, center_hz=max(70, f0 * 3), q=2.5, gain=0.55)
    rasp = resonant_bandpass(sig, SR, center_hz=min(1800, f0 * 14), q=1.8,
                             gain=0.28 if overrun else 0.35)
    sig = 0.55 * sig + boom + rasp

    # turbulence noise floor
    noise = rng.standard_normal(n_gen)
    noise_level = 0.05 + 0.05 * (rpm / 7000.0)
    sig += noise_level * resonant_bandpass(noise, SR, center_hz=2500, q=0.6, gain=1.0)

    if overrun:
        # CRACKLE: unburnt-mixture pops in the exhaust. Short bandpassed noise
        # bursts, Poisson-placed OUTSIDE the crossfade head/tail so the seam
        # stays clean; rate and bite grow with RPM.
        n_crackle = rng.poisson((6.0 + 18.0 * rpm / 7000.0) * loop_s)
        ref = np.sqrt(np.mean(sig ** 2))
        for _ in range(n_crackle):
            dur = int(SR * rng.uniform(0.0015, 0.005))
            pos = int(rng.uniform(xfade_n, loop_len - dur - 1))
            burst = rng.standard_normal(dur) * np.exp(-np.arange(dur) / (0.25 * dur + 1))
            burst = resonant_bandpass(burst, SR, center_hz=rng.uniform(1200, 4200), q=1.2, gain=1.0)
            sig[pos:pos + dur] += burst * ref * rng.uniform(1.2, 3.0)

    sig /= np.max(np.abs(sig)) + 1e-9
    return close_loop(sig, loop_len, xfade_n)


def synth_noise_bed(rng):
    """Broadband intake/exhaust turbulence — the engine's noise half. Runtime
    rides this as a 5th voice with gain following LOAD and pitch tilting with
    RPM. Pink-ish tilt via three stacked bands; loop-closed + rotated."""
    loop_s = 1.2
    loop_len = int(loop_s * SR)
    xfade_n = int(SR * 60 / 1000)
    n_gen = loop_len + xfade_n
    noise = rng.standard_normal(n_gen)
    sig = (resonant_bandpass(noise, SR, center_hz=180, q=0.7, gain=1.0)
           + resonant_bandpass(noise, SR, center_hz=900, q=0.6, gain=0.55)
           + resonant_bandpass(noise, SR, center_hz=3400, q=0.6, gain=0.30))
    # slow loop-periodic swell so the bed breathes instead of hissing statically
    t = np.arange(n_gen) / SR
    sig *= 1.0 + 0.12 * np.sin(2 * np.pi * 3 / loop_s * t + rng.uniform(0, 2 * np.pi)) \
               + 0.08 * np.sin(2 * np.pi * 7 / loop_s * t + rng.uniform(0, 2 * np.pi))
    sig /= np.max(np.abs(sig)) + 1e-9
    return close_loop(sig, loop_len, xfade_n)


def synth_whine(rng):
    """Supercharger gear whine: a bright harmonic tone (~1.1 kHz fundamental)
    with a loop-periodic shimmer. Cycle-snapped -> seamless by construction."""
    loop_s = 1.0
    f = round(1100 * loop_s) / loop_s           # integer cycles per loop
    n = int(loop_s * SR)
    t = np.arange(n) / SR
    sig = np.zeros(n)
    for h, a in ((1, 1.0), (2, 0.35), (3, 0.18), (4, 0.08)):
        sig += a * np.sin(2 * np.pi * f * h * t + 0.4 * h)
    # gear-mesh shimmer: loop-periodic AM at a few cycles per loop
    sig *= 1.0 + 0.10 * np.sin(2 * np.pi * 6 / loop_s * t) + 0.06 * np.sin(2 * np.pi * 11 / loop_s * t + 1.3)
    sig += 0.04 * resonant_bandpass(rng.standard_normal(n), SR, center_hz=f, q=6.0, gain=1.0)
    sig /= np.max(np.abs(sig)) + 1e-9
    steps = np.abs(np.diff(sig))
    return np.roll(sig, -(int(np.argmin(steps)) + 1))


def synth_turbo_whistle(rng):
    """Turbo spool whistle: breathy narrowband noise around 3 kHz over a weak
    tone. Noise is not periodic, so close with a 60 ms crossfade + rotate."""
    loop_s = 1.0
    loop_len = int(loop_s * SR)
    xfade_n = int(SR * 60 / 1000)
    n_gen = loop_len + xfade_n
    t = np.arange(n_gen) / SR
    f = round(3000 * loop_s) / loop_s
    noise = rng.standard_normal(n_gen)
    sig = resonant_bandpass(noise, SR, center_hz=3000, q=9.0, gain=1.0)
    sig += resonant_bandpass(noise, SR, center_hz=6100, q=8.0, gain=0.25)   # harmonic hiss band
    sig += 0.18 * np.sin(2 * np.pi * f * t)
    sig /= np.max(np.abs(sig)) + 1e-9
    return close_loop(sig, loop_len, xfade_n)


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    files = []          # (path, samples, target_rms_dbfs)

    for i, (name, rpm) in enumerate(RPM_POINTS):
        rng = np.random.default_rng(1234 + i)            # deterministic per point
        on = synth_engine(rpm, overrun=False, rng=rng)
        files.append((f"flat6_onload_{rpm:04d}.wav", on, ONLOAD_RMS_DBFS))
        rng = np.random.default_rng(5678 + i)
        ov = synth_engine(rpm, overrun=True, rng=rng)
        files.append((f"flat6_overrun_{rpm:04d}.wav", ov, ONLOAD_RMS_DBFS - OVERRUN_DB_BELOW))

    files.append(("engine_noise_bed.wav", synth_noise_bed(np.random.default_rng(9000)), -14.0))
    files.append(("whine_loop.wav", synth_whine(np.random.default_rng(9001)), -16.0))
    files.append(("turbo_whistle_loop.wav", synth_turbo_whistle(np.random.default_rng(9002)), -16.0))

    # DC REMOVAL (SND-OPUS: the stopgap measured -4.8% DC). Mean subtraction is
    # the loop-safe 5 Hz-highpass equivalent: exact DC to zero, wrap untouched
    # (a constant offset shifts both endpoints equally).
    files = [(fn, s - np.mean(s), tgt) for fn, s, tgt in files]

    # loudness match: gain each file to its RMS target; if any peak would then
    # cross the ceiling, trim EVERY file by the same amount (offsets preserved).
    gains = []
    for _, s, tgt in files:
        rms = np.sqrt(np.mean(s ** 2))
        gains.append(10 ** (tgt / 20.0) / max(rms, 1e-9))
    trim = min(1.0, min(PEAK_CEILING / (np.max(np.abs(s)) * g)
                        for (_, s, _), g in zip(files, gains)))

    for (fn, s, tgt), g in zip(files, gains):
        out = s * g * trim
        path = os.path.join(OUTDIR, fn)
        write_wav(path, out)
        rms_db = 20 * np.log10(np.sqrt(np.mean(out ** 2)))
        peak_db = 20 * np.log10(np.max(np.abs(out)))
        wrap = abs(out[0] - out[-1])
        med = np.median(np.abs(np.diff(out)))
        print(f"{fn:28s} {len(out)/SR:6.3f}s  RMS {rms_db:6.2f} dBFS  peak {peak_db:6.2f}  "
              f"wrap/median-step {wrap/max(med,1e-12):5.2f}x")
    if trim < 1.0:
        print(f"(global headroom trim applied: {20*np.log10(trim):.2f} dB)")
    print(f"\n{len(files)} files -> {OUTDIR}")


if __name__ == "__main__":
    main()
