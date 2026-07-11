#!/usr/bin/env python3
"""Generate the SPACE-HOST engine WAVs (reactor thrum + thruster whoosh) — offline
synthesis, pure stdlib, same WAV-writing pattern as tools/gen_elevator_audio.py.

Root cause fixed: --world space reused assets/audio/vehicles/engine_loop.wav (the
DRIVE world's combustion-car loop) and rode speed as PITCH, which reads as engine
"gear shifts" in a spaceship. These two loops are wired at a FIXED pitch in
host_space.cpp and crossfaded by THROTTLE/speed VOLUME instead — no pitch sweep,
so it never sounds like shifting gears.

  assets/audio/space/engine_hum.wav    (8.0 s, seamless loop) — deep reactor thrum:
      layered low sines (45/45.5/60/90 Hz — the 45 vs 45.5 pair beats slowly at
      0.5 Hz) plus a whisper of low-passed noise for texture. No combustion
      character (no cylinder-firing rhythm), no pitch content that reads as RPM.
  assets/audio/space/engine_thrust.wav (8.0 s, seamless loop) — broadband
      filtered-noise WHOOSH (a slow band-sweep baked into the loop) plus a strong
      ~42 Hz sub layer. This is the "engines burning" layer, crossfaded in by
      throttle/boost.

Both loop periods were chosen so every periodic (sine) component completes an
INTEGER number of cycles across the 8 s buffer (perfectly phase-continuous at the
wrap with no crossfade needed); a short crossfade is still applied to the whole
mixed buffer to smooth the non-periodic noise layers.

Deterministic (fixed seed), pure stdlib. Run from the repo root:
    python tools/gen_space_audio.py
"""
import math
import random
import struct
import wave

SR = 44100


def write_wav(path, samples):
    peak = max((abs(s) for s in samples), default=0.0)
    if peak > 0.98:
        scale = 0.98 / peak
        samples = [s * scale for s in samples]
    data = b''.join(struct.pack('<h', max(-32767, min(32767, int(s * 32767)))) for s in samples)
    w = wave.open(path, 'wb')
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(data)
    w.close()
    print(f'wrote {path}: {len(samples)/SR:.2f}s')


def seam_crossfade(buf, xf_sec):
    """Blend the tail into the head over xf_sec so the loop wraps click-free."""
    xf = int(xf_sec * SR)
    n = len(buf)
    out = list(buf)
    for i in range(xf):
        a = i / xf
        out[n - xf + i] = buf[n - xf + i] * (1 - a) + buf[i] * a
    return out


# ============================================================================
# ENGINE HUM — deep reactor thrum (idle/cruise layer, volume tracks speed).
# ============================================================================
DUR = 8.0
n = int(DUR * SR)
buf = [0.0] * n

# Frequencies all chosen so f * DUR is an integer -> exact phase match at the
# wrap (45, 45.5, 60, 90 Hz all land on whole cycle counts over 8.0 s).
F1, F2, F3, F4 = 45.0, 45.5, 60.0, 90.0   # F1/F2 beat slowly at |F2-F1| = 0.5 Hz

random.seed(3300)
lp = 0.0
for i in range(n):
    t = i / SR
    thrum = (math.sin(2 * math.pi * F1 * t) * 0.42
             + math.sin(2 * math.pi * F2 * t) * 0.42          # slow 0.5 Hz beat vs F1
             + math.sin(2 * math.pi * F3 * t) * 0.22
             + math.sin(2 * math.pi * F4 * t) * 0.10)
    noise = random.random() * 2 - 1
    lp += (noise - lp) * (2 * math.pi * 180.0 / SR)            # ~180 Hz one-pole LP
    buf[i] = thrum * 0.09 + lp * 0.05

buf = seam_crossfade(buf, 0.05)
write_wav('assets/audio/space/engine_hum.wav', buf)


# ============================================================================
# ENGINE THRUST — broadband whoosh + sub weight (throttle/boost layer).
# ============================================================================
n2 = int(DUR * SR)
buf2 = [0.0] * n2
SUB_F = 42.0                                  # SUB_F * DUR = 336, integer -> seamless

random.seed(4200)
lp2 = 0.0
for i in range(n2):
    t = i / SR
    # Gentle band sweep baked into the loop: cutoff breathes once per loop
    # (period == DUR, so the sweep itself is phase-continuous at the wrap).
    cutoff = 900.0 + 500.0 * math.sin(2 * math.pi * (1.0 / DUR) * t)
    noise = random.random() * 2 - 1
    lp2 += (noise - lp2) * min(0.95, 2 * math.pi * cutoff / SR)
    band = (noise - lp2) * 0.6 + lp2 * 0.4       # broadband: mix of the band + its own lowpass
    sub = math.sin(2 * math.pi * SUB_F * t)
    buf2[i] = band * 0.16 + sub * 0.11

buf2 = seam_crossfade(buf2, 0.05)
write_wav('assets/audio/space/engine_thrust.wav', buf2)
