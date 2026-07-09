#!/usr/bin/env python3
"""Generate the elevator cabin WAVs (muzak loop + cable creak) — offline port of
the Babylon x3-elevator.js PROCEDURAL audio (Web Audio oscillators) to committed
PCM, so the C++ cab plays the same charm through the miniaudio path.

  assets/audio/interact/muzak_loop.wav — the 72 BPM pentatonic elevator melody
      (the JS MUZAK_MELODY table, verbatim) over the soft A3 pad (220/223 beat +
      330 triangle), gentle lowpass feel via harmonic rolloff. Exactly 24 beats
      (20.0 s) so startLoop() wraps seamlessly.
  assets/audio/interact/cable_creak.wav — a 0.55 s cable groan: a descending
      saw sweep (like the JS playCreak 30-70 Hz -> x0.7) with a touch of noise.

Deterministic (fixed seed), pure stdlib. Run from the repo root:
    python tools/gen_elevator_audio.py
"""
import math
import random
import struct
import wave

SR = 44100

def write_wav(path, samples):
    data = b''.join(struct.pack('<h', max(-32767, min(32767, int(s * 32767)))) for s in samples)
    w = wave.open(path, 'wb')
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(data)
    w.close()
    print(f'wrote {path}: {len(samples)/SR:.2f}s')

# ---- MUZAK (x3-elevator.js MUZAK_MELODY, 72 BPM, root A3 220 Hz) ------------
MELODY = [  # [semitone, beats, velocity]
    (0, 2, 0.6), (4, 1, 0.4), (7, 1, 0.5), (12, 2, 0.3),
    (9, 1, 0.4), (7, 2, 0.5), (4, 1, 0.3), (0, 1, 0.4),
    (2, 2, 0.5), (4, 1, 0.4), (7, 2, 0.6), (9, 1, 0.3),
    (12, 2, 0.5), (9, 1, 0.4), (7, 1, 0.5), (4, 2, 0.3),
]
BPM = 72.0
ROOT = 220.0
BEAT = 60.0 / BPM
TOTAL_BEATS = sum(d for _, d, _ in MELODY)          # 24 beats
DUR = TOTAL_BEATS * BEAT                             # 20.0 s

n = int(DUR * SR)
buf = [0.0] * n

# Pad: 220 + 223 (slow amplitude beat) sine + 330 triangle, with a very slow
# tremolo standing in for the JS's 0.1 Hz LFO filter sweep.
for i in range(n):
    t = i / SR
    lfo = 0.75 + 0.25 * math.sin(2 * math.pi * 0.1 * t)
    pad = (math.sin(2 * math.pi * 220.0 * t) + math.sin(2 * math.pi * 223.0 * t)) * 0.5
    tri = 2.0 / math.pi * math.asin(math.sin(2 * math.pi * 330.0 * t))
    buf[i] += (pad * 0.030 + tri * 0.018) * lfo

# Melody: soft sines w/ the JS envelope (50 ms attack, hold to 70 %, exp tail).
pos = 0.0
for semi, beats, vel in MELODY:
    f = ROOT * (2.0 ** (semi / 12.0))
    nd = beats * BEAT * 0.9
    s0 = int(pos * SR)
    for i in range(int(nd * SR)):
        t = i / SR
        if t < 0.05:
            env = t / 0.05
        elif t < nd * 0.7:
            env = 1.0
        else:
            env = math.exp(-6.0 * (t - nd * 0.7) / max(1e-3, nd * 0.3))
        # Fundamental + a whisper of the 2nd harmonic (the lowpass'd sine feel).
        v = math.sin(2 * math.pi * f * t) + 0.15 * math.sin(2 * math.pi * 2 * f * t)
        j = s0 + i
        if j < n:
            buf[j] += v * env * 0.045 * vel
    pos += beats * BEAT

# 30 ms loop-seam crossfade (fade the tail into the head so wrap is click-free).
xf = int(0.03 * SR)
for i in range(xf):
    a = i / xf
    buf[n - xf + i] = buf[n - xf + i] * (1 - a) + buf[i] * a
write_wav('assets/audio/interact/muzak_loop.wav', buf)

# ---- CABLE CREAK (playCreak: saw ~30-70 Hz gliding to x0.7, bandpassy) ------
random.seed(1127)
dur = 0.55
n2 = int(dur * SR)
buf2 = [0.0] * n2
f0 = 30.0 + random.random() * 40.0
phase = 0.0
lp = 0.0
for i in range(n2):
    t = i / SR
    f = f0 * (1.0 - 0.3 * (t / dur))                 # glide down to x0.7
    phase += f / SR
    saw = 2.0 * (phase - math.floor(phase + 0.5))
    noise = (random.random() * 2 - 1) * 0.25
    # One-pole lowpass ~250 Hz stands in for the JS bandpass Q=3 resonance.
    x = saw * 0.8 + noise * 0.2
    lp += (x - lp) * (2 * math.pi * 250.0 / SR)
    if t < 0.05:
        env = t / 0.05 * 0.9
    else:
        env = 0.9 * math.exp(-5.5 * (t - 0.05))
    buf2[i] = lp * env * 0.85
write_wav('assets/audio/interact/cable_creak.wav', buf2)


# ---- LAYERED DOOR SFX (playDoorSfx port): hydraulic hiss + metal slide, and a
# separate seat THUNK the FSM fires exactly when the panels hit end-of-travel
# (better than the JS fixed 450 ms delay). ----
def door_layer(path, f_hz0, f_hz1, saw0, saw1):
    dur = 0.55
    n = int(dur * SR)
    buf = [0.0] * n
    random.seed(48)
    lp = 0.0
    phase = 0.0
    for i in range(n):
        t = i / SR
        u = t / dur
        # Hiss: noise through a moving one-pole toward the sweep frequency.
        f = f_hz0 + (f_hz1 - f_hz0) * u
        noise = random.random() * 2 - 1
        lp += (noise - lp) * min(0.95, 2 * math.pi * f / SR)
        hiss = (noise - lp) * 0.5                    # crude highpassed band
        env_h = min(1.0, u * 8) * (1.0 - u) * 0.5
        # Metal slide: gliding saw.
        sf = saw0 + (saw1 - saw0) * u
        phase += sf / SR
        saw = 2.0 * (phase - math.floor(phase + 0.5))
        env_s = min(1.0, u * 5) * max(0.0, 1.0 - u * 1.1) * 0.28
        buf[i] = hiss * env_h + saw * env_s
    write_wav(path, buf)

door_layer('assets/audio/interact/door_hiss_open.wav', 2000, 800, 150, 200)
door_layer('assets/audio/interact/door_hiss_close.wav', 1500, 2500, 200, 150)

# Thunk: 55 Hz square with a hard decay + a click transient.
dur = 0.16
n = int(dur * SR)
buf = [0.0] * n
for i in range(n):
    t = i / SR
    sq = 1.0 if math.sin(2 * math.pi * 55.0 * t) >= 0 else -1.0
    env = math.exp(-22.0 * t) * 0.85
    click = math.exp(-400.0 * t) * 0.4
    buf[i] = sq * env + click * (1 if i % 7 else -1)
write_wav('assets/audio/interact/door_thunk.wav', buf)


# ---- DISCO CLUB TRACK (startDiscoMusic port): the 128 BPM four-on-the-floor
# kick / hat / off-beat hat / Cm7 stab sequencer, baked as a seamless 2-bar
# (8-beat, 3.75 s) loop. Start/stop rides the 1127 disco toggle exactly like
# the muzak loop rides the door seal. ----
CLUB_BPM = 128.0
CB = 60.0 / CLUB_BPM                                  # one beat, 0.46875 s
club_dur = 8 * CB                                     # 2 bars
n3 = int(club_dur * SR)
buf3 = [0.0] * n3
random.seed(128)

def add_kick(at):
    s0 = int(at * SR)
    for i in range(int(0.24 * SR)):
        t = i / SR
        f = 150.0 * math.exp(-9.0 * t) + 48.0        # pitch drop 150 -> ~50 Hz
        v = math.sin(2 * math.pi * f * t) * math.exp(-7.0 * t)
        j = s0 + i
        buf3[j % n3] += v * 0.50                      # wrap: seam-free loop

def add_hat(at, vol):
    s0 = int(at * SR)
    hp = 0.0
    for i in range(int(0.06 * SR)):
        noise = random.random() * 2 - 1
        hp += (noise - hp) * 0.55                     # crude ~8 kHz highpass
        v = (noise - hp) * math.exp(-60.0 * (i / SR))
        j = s0 + i
        buf3[j % n3] += v * vol

def add_stab(at):
    # Cm7: C3 / Eb3 / G3 / Bb3 saws, tight envelope (the JS stab voicing).
    s0 = int(at * SR)
    for f in (130.81, 155.56, 196.00, 233.08):
        phase = 0.0
        for i in range(int(0.22 * SR)):
            t = i / SR
            phase += f / SR
            saw = 2.0 * (phase - math.floor(phase + 0.5))
            env = min(1.0, t / 0.01) * math.exp(-11.0 * t)
            j = s0 + i
            buf3[j % n3] += saw * env * 0.055

for b in range(8):
    add_kick(b * CB)                                  # four-on-the-floor
    add_hat(b * CB, 0.10)                             # on-beat hat (soft)
    add_hat((b + 0.5) * CB, 0.22)                     # OFF-beat hat (the drive)
for at in (1.5, 3.5, 5.5, 7.75):                      # stabs on the and-of-2/4 + a pickup
    add_stab(at * CB)

peak = max(abs(s) for s in buf3)
if peak > 0.95:
    buf3 = [s * (0.95 / peak) for s in buf3]
write_wav('assets/audio/interact/club_track.wav', buf3)
