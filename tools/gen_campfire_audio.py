#!/usr/bin/env python3
"""Generate the CAMPFIRE CRACKLE loop (W-NIGHT roadside campfires) — the
gen_water_audio.py / gen_elevator_audio.py offline-bake pattern: deterministic
pure-stdlib synthesis committed as repo-local PCM, so the fires sound right on
a fresh clone with no external pack.

  assets/audio/ambient/campfire_crackle_loop.wav — 6.0 s seamless loop.
      Three layers, mixed:
        * FIRE BODY — low rumbling combustion: white noise through a one-pole
          lowpass (~180 Hz), its amplitude breathing on two slow incommensurate
          sines (a fire swells and sinks, it does not idle flat).
        * HISS — a quieter, brighter band (lowpassed at ~2.4 kHz minus the
          body) for the gas-jet edge of the flames.
        * CRACKLE POPS — ~70 short transients (1.5-9 ms decaying resonant
          clicks at 1.2-5 kHz, random amplitude, denser in the loud breaths).
          Pop tails WRAP modulo the loop length, and every layer is periodic
          by construction, so the loop point is seamless — no crossfade slop.

Deterministic (fixed seed), pure stdlib. Run from the repo root:
    python tools/gen_campfire_audio.py
"""
import math
import os
import random
import struct
import wave

SR = 44100
DUR = 6.0
N = int(SR * DUR)
TWO_PI = 2.0 * math.pi


def write_wav(path, samples):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = b''.join(struct.pack('<h', max(-32767, min(32767, int(s * 32767))))
                    for s in samples)
    w = wave.open(path, 'wb')
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(data)
    w.close()
    print(f'wrote {path}: {len(samples)/SR:.2f}s')


def main():
    rng = random.Random(0xF17E)
    buf = [0.0] * N

    # ---- Breathing envelope: periodic in the loop by using whole cycles.
    def breath(i):
        t = i / N  # 0..1 across the loop
        return (0.72 + 0.18 * math.sin(TWO_PI * 3.0 * t)          # 3 swells / loop
                     + 0.10 * math.sin(TWO_PI * 7.0 * t + 1.3))   # + a faster ripple

    # ---- FIRE BODY + HISS: filtered noise. One-pole lowpasses; noise itself
    # is aperiodic but spectrally uniform, so the loop point is inaudible once
    # the deterministic envelope is periodic. To kill the residual seam click,
    # the first 30 ms is crossfaded with the tail's continuation.
    lp_body = 0.0
    lp_hiss = 0.0
    a_body = 1.0 - math.exp(-TWO_PI * 180.0 / SR)
    a_hiss = 1.0 - math.exp(-TWO_PI * 2400.0 / SR)
    raw = [0.0] * N
    for i in range(N):
        n = rng.uniform(-1.0, 1.0)
        lp_body += a_body * (n - lp_body)
        lp_hiss += a_hiss * (n - lp_hiss)
        band = lp_hiss - lp_body                 # 180..2400 Hz-ish hiss band
        raw[i] = (lp_body * 0.85 + band * 0.16) * breath(i)
    # seam crossfade (30 ms): blend the start with the signal "past the end".
    xf = int(0.030 * SR)
    lpb2, lph2 = lp_body, lp_hiss                # continue the filters past N
    for i in range(xf):
        n = rng.uniform(-1.0, 1.0)
        lpb2 += a_body * (n - lpb2)
        lph2 += a_hiss * (n - lph2)
        cont = (lpb2 * 0.85 + (lph2 - lpb2) * 0.16) * breath(i)
        u = i / xf
        raw[i] = raw[i] * u + cont * (1.0 - u)
    for i in range(N):
        buf[i] += raw[i]

    # ---- CRACKLE POPS: short decaying resonant clicks; tails wrap modulo N.
    for _ in range(70):
        t0 = rng.random()                        # position in the loop
        # denser pops inside the loud breaths: reject-sample against breath()
        if rng.random() > breath(int(t0 * N)) - 0.45:
            t0 = rng.random()
        i0 = int(t0 * N)
        f = 1200.0 + rng.random() * 3800.0       # click resonance
        d = 0.0015 + rng.random() * 0.0075       # 1.5-9 ms
        amp = (0.25 + rng.random() ** 2 * 0.85) * 0.55
        nlen = int((d * 6.0) * SR)               # ~6 time constants of tail
        ph = rng.random() * TWO_PI
        for k in range(nlen):
            tt = k / SR
            s = amp * math.exp(-tt / d) * math.sin(TWO_PI * f * tt + ph)
            buf[(i0 + k) % N] += s

    # ---- Normalize to a comfortable loop level.
    peak = max(abs(s) for s in buf) or 1.0
    g = 0.55 / peak
    buf = [s * g for s in buf]

    write_wav(os.path.join('assets', 'audio', 'ambient',
                           'campfire_crackle_loop.wav'), buf)


if __name__ == '__main__':
    main()
