#!/usr/bin/env python3
"""CRISP DOOR SOUND SETS (owner, 2026-08-30: "sliding door models, and crisp
door sounds"). The registry shipped ONE generic family for all three door
classes; this bakes a distinct mechanical voice per class, deterministic,
pure stdlib (the gen_elevator_audio.py pattern):

  doors/a_*.wav   door_a   — pneumatic office slider: sharp hiss, light servo,
                             soft seat click. Quick and clean.
  doors/s_*.wav   slider   — split glass door: airy dual whir, glide swish,
                             magnetic snick. The most refined of the three.
  doors/b_*.wav   bulkhead — heavy plate: clunk-release, deep motor rumble,
                             MASSIVE seated thunk with a short room tail.
  interact/card_swipe/accept/deny.wav — the card-access voice: a zip swipe,
                             a rising two-chirp grant, a flat double buzz.

Each family: _open, _close, _locked, _servo, _thunk (the DoorModelDef slots).
Run from the repo root:  python tools/gen_door_audio.py
"""
import math
import os
import random
import struct
import wave

SR = 44100
rng = random.Random(4790)   # the rift stop's own code, for determinism


def write_wav(path, samples):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = b''.join(struct.pack('<h', max(-32767, min(32767, int(s * 32767))))
                    for s in samples)
    w = wave.open(path, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes(data); w.close()
    print(f"  {path}  {len(samples)/SR:.2f}s")


def n_samples(sec):
    return int(sec * SR)


def env_ad(i, n, atk, dec):
    """Attack/decay envelope, exponential decay tail."""
    a = int(atk * SR)
    if i < a:
        return i / max(1, a)
    return math.exp(-(i - a) / max(1, dec * SR))


def onepole(samples, cutoff):
    """Simple one-pole lowpass, cutoff in Hz."""
    out, y = [], 0.0
    k = 1.0 - math.exp(-2.0 * math.pi * cutoff / SR)
    for s in samples:
        y += k * (s - y)
        out.append(y)
    return out


def hiss(sec, cutoff, atk=0.004, dec=None):
    dec = dec if dec is not None else sec * 0.5
    n = n_samples(sec)
    raw = [rng.uniform(-1, 1) for _ in range(n)]
    raw = onepole(raw, cutoff)
    return [raw[i] * env_ad(i, n, atk, dec) for i in range(n)]


def sweep(sec, f0, f1, wave_fn=math.sin, fm=0.0, atk=0.01, dec=None):
    dec = dec if dec is not None else sec * 0.6
    n = n_samples(sec)
    out, ph = [], 0.0
    for i in range(n):
        t = i / n
        f = f0 + (f1 - f0) * t
        if fm > 0.0:
            f *= 1.0 + fm * math.sin(2 * math.pi * 27.0 * i / SR)
        ph += 2 * math.pi * f / SR
        out.append(wave_fn(ph) * env_ad(i, n, atk, dec))
    return out


def thump(sec, f0, f1, punch=1.0):
    n = n_samples(sec)
    out, ph = [], 0.0
    for i in range(n):
        t = i / n
        f = f0 + (f1 - f0) * t
        ph += 2 * math.pi * f / SR
        body = math.sin(ph) * math.exp(-t * 9.0)
        click = rng.uniform(-1, 1) * math.exp(-t * 220.0) * 0.6 * punch
        out.append(body + click)
    return out


def mix(*layers):
    n = max(len(l) for l in layers)
    out = [0.0] * n
    for l in layers:
        for i, s in enumerate(l):
            out[i] += s
    peak = max(1e-6, max(abs(s) for s in out))
    return [s * 0.92 / peak for s in out]


def delay_into(samples, sec, offset):
    return [0.0] * n_samples(offset) + [s * 1.0 for s in samples]


def tail(samples, fb=0.25, ms=90):
    """A short single-tap room tail — sells 'a real place', keeps it crisp."""
    d = int(ms * SR / 1000)
    out = list(samples) + [0.0] * d * 3
    for i in range(len(samples)):
        out[i + d] += samples[i] * fb
        out[i + 2 * d] += samples[i] * fb * fb
    return out


ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "assets", "audio")
D = os.path.join(ROOT, "doors")
I = os.path.join(ROOT, "interact")

# ---- door_a: pneumatic office slider — quick, clean, LIGHT --------------------
write_wav(os.path.join(D, "a_open.wav"), tail(mix(
    hiss(0.30, 5200, atk=0.003, dec=0.10),                  # the PSSHT
    delay_into(sweep(0.42, 170, 340, fm=0.10, dec=0.30), 0.42, 0.05),
    delay_into(thump(0.10, 210, 150, punch=0.5), 0.10, 0.44)), fb=0.16))
write_wav(os.path.join(D, "a_close.wav"), tail(mix(
    sweep(0.40, 330, 160, fm=0.10, dec=0.32),
    delay_into(thump(0.14, 180, 110, punch=0.8), 0.14, 0.34),
    delay_into(hiss(0.16, 3600, dec=0.10), 0.16, 0.38)), fb=0.16))
write_wav(os.path.join(D, "a_locked.wav"), mix(
    sweep(0.09, 240, 240, wave_fn=lambda p: 1.0 if math.sin(p) > 0 else -1.0, dec=0.07),
    delay_into(sweep(0.09, 200, 200, wave_fn=lambda p: 1.0 if math.sin(p) > 0 else -1.0,
                     dec=0.07), 0.09, 0.13)))
write_wav(os.path.join(D, "a_servo.wav"), sweep(0.55, 250, 265, fm=0.16, atk=0.05, dec=0.5))
write_wav(os.path.join(D, "a_thunk.wav"), thump(0.12, 200, 130, punch=0.6))

# ---- slider: split glass — airy, refined, MAGNETIC ---------------------------
write_wav(os.path.join(D, "s_open.wav"), tail(mix(
    sweep(0.55, 420, 660, fm=0.05, atk=0.02, dec=0.45),      # dual whir (bright)
    sweep(0.55, 424, 668, fm=0.05, atk=0.02, dec=0.45),      # detuned pair = shimmer
    hiss(0.50, 2400, atk=0.02, dec=0.35),                    # glide swish
    delay_into(sweep(0.06, 1900, 2600, dec=0.05), 0.06, 0.52)), fb=0.12))  # arrival snick
write_wav(os.path.join(D, "s_close.wav"), tail(mix(
    sweep(0.50, 640, 400, fm=0.05, atk=0.02, dec=0.42),
    hiss(0.45, 2200, atk=0.02, dec=0.32),
    delay_into(sweep(0.05, 2600, 1800, dec=0.04), 0.05, 0.46),
    delay_into(thump(0.07, 240, 170, punch=0.35), 0.07, 0.48)), fb=0.12))
write_wav(os.path.join(D, "s_locked.wav"), mix(
    sweep(0.16, 660, 520, dec=0.12),
    delay_into(sweep(0.16, 520, 410, dec=0.12), 0.16, 0.18)))   # polite falling deny
write_wav(os.path.join(D, "s_servo.wav"), mix(
    sweep(0.6, 480, 500, fm=0.08, atk=0.06, dec=0.55),
    sweep(0.6, 484, 505, fm=0.08, atk=0.06, dec=0.55)))
write_wav(os.path.join(D, "s_thunk.wav"), mix(
    sweep(0.05, 2400, 1700, dec=0.04),
    thump(0.06, 260, 190, punch=0.3)))

# ---- bulkhead: heavy plate — industrial MASS ---------------------------------
write_wav(os.path.join(D, "b_open.wav"), tail(mix(
    thump(0.16, 90, 55, punch=1.0),                          # clunk-release
    delay_into(sweep(0.85, 55, 88, fm=0.22, atk=0.05, dec=0.7,
                     wave_fn=lambda p: math.sin(p) + 0.35 * math.sin(2 * p)), 0.85, 0.12),
    delay_into(hiss(0.5, 900, atk=0.04, dec=0.4), 0.5, 0.15),
    delay_into(thump(0.2, 120, 70, punch=0.8), 0.2, 0.92)), fb=0.3, ms=120))
write_wav(os.path.join(D, "b_close.wav"), tail(mix(
    sweep(0.8, 85, 55, fm=0.22, atk=0.04, dec=0.68,
          wave_fn=lambda p: math.sin(p) + 0.35 * math.sin(2 * p)),
    delay_into(hiss(0.4, 800, dec=0.3), 0.4, 0.1),
    delay_into(thump(0.30, 70, 38, punch=1.2), 0.30, 0.74),   # THE seat
    delay_into(hiss(0.22, 1400, dec=0.14), 0.22, 0.80)), fb=0.34, ms=140))
write_wav(os.path.join(D, "b_locked.wav"), tail(mix(
    thump(0.16, 110, 70, punch=1.1),
    delay_into(thump(0.14, 95, 60, punch=0.9), 0.14, 0.20)), fb=0.3, ms=110))
write_wav(os.path.join(D, "b_servo.wav"),
          sweep(0.8, 62, 66, fm=0.3, atk=0.08, dec=0.75,
                wave_fn=lambda p: math.sin(p) + 0.35 * math.sin(2 * p)))
write_wav(os.path.join(D, "b_thunk.wav"), tail(thump(0.28, 75, 40, punch=1.2), fb=0.3, ms=120))

# ---- card access voice --------------------------------------------------------
write_wav(os.path.join(I, "card_swipe.wav"), hiss(0.13, 5600, atk=0.002, dec=0.05))
write_wav(os.path.join(I, "card_accept.wav"), mix(
    sweep(0.07, 1180, 1180, dec=0.05),
    delay_into(sweep(0.09, 1560, 1560, dec=0.07), 0.09, 0.09)))
write_wav(os.path.join(I, "card_deny.wav"), mix(
    sweep(0.11, 340, 340, wave_fn=lambda p: 1.0 if math.sin(p) > 0 else -1.0, dec=0.09),
    delay_into(sweep(0.11, 340, 340, wave_fn=lambda p: 1.0 if math.sin(p) > 0 else -1.0,
                     dec=0.09), 0.11, 0.15)))

print("[gen_door_audio] done — 3 door families + card voice")
