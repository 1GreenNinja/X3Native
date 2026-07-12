#!/usr/bin/env python3
"""Generate the player WATER WAVs (swim entry splash + surface exit) — the
gen_crowd_chatter.py / gen_elevator_audio.py offline-bake pattern: deterministic
pure-stdlib synthesis committed as repo-local PCM under assets/audio/water/, so
the river/sea sound right on a fresh clone with no external pack.

Until now CueKind::PlayerSplash reused the landing thud pitched to 0.55 ("reads
as a muffled water entry" — the W10 stand-in). These are the real takes:

  assets/audio/water/splash_enter.wav — 0.62 s. A LOW THUMP attack (a decaying
      ~65 Hz sine, the body displacing the water) under a BROADBAND NOISE BURST
      (the sheet of spray, band-passed and closing darker as it decays), with a
      sparse DROPLET TAIL (short pitched noise ticks through a resonant band,
      falling density) — the classic splash morphology.
  assets/audio/water/splash_exit.wav  — 0.45 s, the softer "surface exit":
      no deep thump, a gentler sluicing noise burst + a few droplets (water
      running off the body as the player climbs out of the swim state).

Deterministic (fixed seeds), pure stdlib. Run from the repo root:
    python tools/gen_water_audio.py
"""
import math
import random
import struct
import wave

SR = 44100


def write_wav(path, samples):
    data = b''.join(struct.pack('<h', max(-32767, min(32767, int(s * 32767))))
                    for s in samples)
    w = wave.open(path, 'wb')
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(data)
    w.close()
    print(f'wrote {path}: {len(samples)/SR:.2f}s')


def splash(path, seed, dur, thump_amp, burst_amp, droplets, peak):
    """One splash take: low thump attack + broadband burst + droplet tail."""
    rng = random.Random(seed)
    n = int(dur * SR)
    buf = [0.0] * n

    # ---- Droplet schedule: short resonant ticks, dense right after the burst,
    # thinning toward the tail (each a 6-14 ms ping at 900-2600 Hz).
    drops = []
    for k in range(droplets):
        # Quadratic bias toward the early tail.
        t0 = 0.06 + (rng.random() ** 1.6) * (dur - 0.10)
        f = 900.0 + rng.random() * 1700.0
        d = 0.006 + rng.random() * 0.008
        a = 0.25 + rng.random() * 0.45
        drops.append((t0, f, d, a))

    lp = 0.0    # burst lowpass state (closes darker over the decay)
    hp = 0.0    # burst highpass state (removes rumble from the noise)
    for i in range(n):
        t = i / SR
        s = 0.0
        # ---- LOW THUMP: the body hitting the water — a fast-decaying low sine
        # with a slight downward pitch bend (65 -> 45 Hz over the stroke).
        if thump_amp > 0.0:
            f0 = 65.0 - 20.0 * min(1.0, t / 0.12)
            s += thump_amp * math.sin(2 * math.pi * f0 * t) * math.exp(-t / 0.055)
        # ---- BROADBAND BURST: the spray sheet. 4 ms attack, ~90 ms body,
        # long-ish exponential tail; the lowpass corner CLOSES as it decays
        # (bright crash -> dark wash), highpassed at ~180 Hz.
        atk = min(1.0, t / 0.004)
        env = atk * math.exp(-t / 0.085)
        noise = rng.random() * 2.0 - 1.0
        corner = 5200.0 * math.exp(-t / 0.16) + 500.0
        lp += (noise - lp) * min(0.95, 2 * math.pi * corner / SR)
        hp += (lp - hp) * min(0.95, 2 * math.pi * 180.0 / SR)
        s += burst_amp * (lp - hp) * env
        # ---- DROPLET TAIL: sparse pitched ticks (rain-back into the surface).
        for (t0, f, d, a) in drops:
            if t0 <= t < t0 + d:
                u = (t - t0) / d
                s += a * math.sin(2 * math.pi * f * (t - t0)) * \
                     math.sin(math.pi * u) * 0.5
        buf[i] = s

    pk = max(abs(v) for v in buf) or 1.0
    buf = [v * (peak / pk) for v in buf]
    write_wav(path, buf)


# The ENTRY take: full thump + crash + a busy droplet tail (~0.62 s).
splash('assets/audio/water/splash_enter.wav', seed=1127, dur=0.62,
       thump_amp=0.85, burst_amp=1.00, droplets=26, peak=0.80)
# The EXIT take: softer, no deep thump, water sluicing off (~0.45 s).
splash('assets/audio/water/splash_exit.wav', seed=4048, dur=0.45,
       thump_amp=0.18, burst_amp=0.70, droplets=16, peak=0.55)
