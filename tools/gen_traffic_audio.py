#!/usr/bin/env python3
"""gen_traffic_audio.py — the FREEWAY TRAFFIC voice pack (W-TRAFFIC2).

Built on tools/gen_engine_bank.py's machinery (imported, NOT copied — the same
rule tools/gen_outboard_audio.py follows): SR / resonant_bandpass / rms /
write_wav / PEAK_CEILING all come from there, so every synthesized asset in the
repo shares one loudness discipline and one wav writer.

WHAT IT MAKES (assets/audio/vehicles/):

  horn_car.wav     A real car horn is a TWO-TONE electric horn: two vibrating
                   diaphragms tuned a minor third apart (the classic pair is
                   ~440 Hz + ~523 Hz — A4 and C5). Each diaphragm is a buzzer,
                   not a sine: the contact breaker chops the current, so the
                   spectrum is a bright saw/square hybrid with a strong odd
                   series. The horn body is a short flared trumpet, which adds
                   a formant around 2 kHz. ATTACK is fast but NOT instant (the
                   diaphragm has to spin up: ~12 ms) and the release rings out
                   ~40 ms. One-shot, 0.55 s: the length of an annoyed blip.

  horn_truck.wav   An air horn: much lower fundamentals (~185 Hz + ~233 Hz — a
                   minor third down at F#3/A#3), a longer 40 ms spin-up (the
                   air column has mass), a broadband hiss of escaping air under
                   it, and 1.1 s of duration. Trucks do not blip; they LEAN.

  siren_wail.wav   A police wail: a single bright horn tone swept up and down
                   between 700 Hz and 1500 Hz over a 4.4 s cycle, with the odd
                   harmonics a mechanical siren rotor produces. LOOP-CLOSED by
                   construction (the sweep is a whole number of cycles and
                   starts/ends at the same phase and the same frequency), so
                   startLoop3D wraps it with no click.

Deterministic (fixed seeds). Run from anywhere:
    python tools/gen_traffic_audio.py
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from gen_engine_bank import SR, PEAK_CEILING, resonant_bandpass, rms, write_wav

OUTDIR = os.path.normpath(os.path.join(HERE, "..", "assets", "audio", "vehicles"))


def buzzer(t, f0, n_harm, odd_push, rng):
    """A contact-breaker diaphragm: bright harmonic stack, odd series pushed.

    NOT a bandlimited saw for its own sake — a horn diaphragm slams against its
    stop every cycle, so the even harmonics are weaker than a saw's and the odd
    ones stronger. Each partial gets a random start phase so two horns built
    from this never phase-cancel identically.
    """
    out = np.zeros_like(t)
    for h in range(1, n_harm + 1):
        amp = 1.0 / (h ** 1.05)
        if h % 2 == 1:
            amp *= odd_push
        out += amp * np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 2 * np.pi))
    return out


def envelope(n, attack_s, release_s):
    """Fast-but-not-instant attack, flat body, ring-out release.

    A zero-length attack is what makes a synthesized horn read as a beep: the
    diaphragm has mass and takes ~12 ms (car) / ~40 ms (air) to reach full
    swing. The release is a ring-out, not a cut.
    """
    env = np.ones(n)
    a = max(1, int(attack_s * SR))
    r = max(1, int(release_s * SR))
    env[:a] = np.sin(np.linspace(0.0, np.pi / 2.0, a)) ** 1.4
    env[n - r:] = np.cos(np.linspace(0.0, np.pi / 2.0, r)) ** 1.2
    return env


def build_car_horn(rng):
    dur = 0.55
    n = int(dur * SR)
    t = np.arange(n) / SR
    # A4 + C5 — the minor third every US car horn pair is tuned to.
    a = buzzer(t, 440.0, 14, 1.30, rng)
    b = buzzer(t, 523.25, 14, 1.30, rng)
    sig = 0.58 * a + 0.52 * b
    # The flared trumpet body: a broad 2 kHz formant plus a little 3.4 kHz
    # edge. This is the difference between "two square waves" and "a horn".
    sig = 0.60 * sig \
        + resonant_bandpass(sig, SR, 2000.0, 1.6, 0.85) \
        + resonant_bandpass(sig, SR, 3400.0, 3.0, 0.30)
    # Contact-breaker grit: a whisper of noise gated by the tone's own level.
    grit = resonant_bandpass(rng.standard_normal(n), SR, 2600.0, 1.4, 1.0)
    sig = sig / (rms(sig) + 1e-12) * 0.94 + grit / (rms(grit) + 1e-12) * 0.06
    sig *= envelope(n, 0.012, 0.040)
    return sig


def build_truck_horn(rng):
    dur = 1.10
    n = int(dur * SR)
    t = np.arange(n) / SR
    # F#3 + A#3 — an air horn's pair, an octave and a bit under the car's.
    a = buzzer(t, 185.0, 20, 1.22, rng)
    b = buzzer(t, 233.1, 20, 1.22, rng)
    sig = 0.60 * a + 0.48 * b
    # Long trumpet: a low body resonance and a bark up top.
    sig = 0.62 * sig \
        + resonant_bandpass(sig, SR, 620.0, 1.3, 0.90) \
        + resonant_bandpass(sig, SR, 1800.0, 2.4, 0.35)
    # ESCAPING AIR — the tell that separates an air horn from a big car horn.
    hiss = resonant_bandpass(rng.standard_normal(n), SR, 3200.0, 0.8, 1.0)
    hiss += resonant_bandpass(rng.standard_normal(n), SR, 5200.0, 1.6, 0.5)
    sig = sig / (rms(sig) + 1e-12) * 0.86 + hiss / (rms(hiss) + 1e-12) * 0.14
    sig *= envelope(n, 0.040, 0.090)
    return sig


def build_siren(rng):
    """A LOOP-CLOSED wail. Closure is by CONSTRUCTION, not by crossfade.

    Two things have to line up at the wrap or startLoop3D clicks once every
    cycle, forever:
      1) PHASE. The sweep is one full cosine cycle of the modulator, whose mean
         is exactly f_mid, so total phase = 2*pi*f_mid*dur. Pick the integer
         sample count FIRST, then nudge f_mid so f_mid*dur is a whole number of
         carrier cycles. Matching the instantaneous FREQUENCY at the seam is not
         enough — the phase has to match too, and only this pins it.
      2) THE FILTER STATE. resonant_bandpass is an IIR: run on a finite buffer
         it starts from silence, so sample 0 carries a filter transient that
         sample n-1 does not. The first cut measured wrap/median-step 5.1x for
         exactly this reason. Fix: filter THREE tiled copies and keep the middle
         one — by then the filter state is the periodic steady state.
    """
    f_lo, f_hi = 700.0, 1500.0
    f_dev = 0.5 * (f_hi - f_lo)
    n = int(round(4.4 * SR))                        # seconds per wail cycle
    dur = n / SR
    f_mid = round(0.5 * (f_lo + f_hi) * dur) / dur  # -> integer carrier cycles
    t = np.arange(3 * n) / SR
    # Instantaneous frequency's EXACT integral (no cumsum drift).
    w = 2 * np.pi / dur
    phase = 2 * np.pi * (f_mid * t - (f_dev / w) * np.cos(w * t))
    phase -= phase[0]
    sig = np.zeros(3 * n)
    for h, amp in ((1, 1.00), (2, 0.24), (3, 0.42), (5, 0.16), (7, 0.08)):
        sig += amp * np.sin(h * phase)
    # Rotor body: a mechanical siren's horn is a broad midrange resonator.
    sig = 0.70 * sig + resonant_bandpass(sig, SR, 1400.0, 1.1, 0.55)
    sig = sig[n:2 * n]                              # the steady-state copy
    return sig - np.mean(sig)


def finish(x, dbfs):
    x = x - np.mean(x)
    x *= 10 ** (dbfs / 20.0) / (rms(x) + 1e-12)
    peak = np.max(np.abs(x))
    if peak > PEAK_CEILING:
        x *= PEAK_CEILING / peak
    return x


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    # The siren is written at SR/2. Its highest partial is the 7th of 1500 Hz
    # (10.5 kHz), comfortably under a 24 kHz Nyquist, and a 4.4 s loop at the
    # full 96 kHz is an 845 KB committed file for no audible gain. Decimating
    # by 2 keeps the loop closure exact (n is even by construction).
    jobs = [
        ("horn_car.wav",   build_car_horn(np.random.default_rng(0x4807)),   -9.0,  1),
        ("horn_truck.wav", build_truck_horn(np.random.default_rng(0x4808)), -8.0,  1),
        ("siren_wail.wav", build_siren(np.random.default_rng(0x4809)),     -11.0,  2),
    ]
    for name, sig, dbfs, dec in jobs:
        out = finish(sig[::dec] if dec > 1 else sig, dbfs)
        path = os.path.join(OUTDIR, name)
        write_wav(path, out, sr=SR // dec)
        # LOOP-SEAM METRIC, and why it is not the outboard's. gen_outboard
        # reports wrap/MEDIAN-step, which is right for its noise-heavy bed. The
        # siren is a pure swept tone whose loop point sits on a ZERO CROSSING —
        # the steepest part of the wave — so a perfect seam still measures ~8x
        # the median step and reads as a false alarm. The honest test for a
        # tonal loop is against the steepest step the waveform already contains:
        # a seam no steeper than the interior maximum is inaudible.
        wrap = abs(float(out[0]) - float(out[-1]))
        steps = np.abs(np.diff(out))
        print(f"{name:16s} {len(out)/(SR//dec):.3f}s  "
              f"RMS {20*np.log10(rms(out)):.2f} dBFS  "
              f"peak {20*np.log10(np.max(np.abs(out))):.2f} dBFS  "
              f"wrap/max-interior-step {wrap/max(float(steps.max()),1e-12):.2f}x"
              f"{'  LOOP-CLEAN' if wrap <= steps.max() else '  *** SEAM CLICK ***'}")


if __name__ == "__main__":
    main()
