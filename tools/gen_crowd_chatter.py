#!/usr/bin/env python3
"""Generate the crowd chatter WAVs (murmur walla + worker grumble) — the
gen_elevator_audio.py pattern: deterministic pure-stdlib synthesis committed as
repo-local PCM under assets/audio/crowd/, so THE PEOPLE mumble on a fresh clone
with no external pack.

Why baked walla and not the Nexus pitch-down trick (canon_45.cpp pitches the
creature-bucket vocal to 0.5-0.62 for its whisper dread): that take reads as
MONSTROUS on purpose — breathy, hostile, wrong for benign street/facility
chatter. These takes are proper unintelligible walla: a voiced buzz (rolled-off
saw at a wobbling f0) + band-passed noise (~200-2000 Hz), chopped by a syllabic
amplitude envelope. Nobody says words; everybody clearly TALKS.

  assets/audio/crowd/murmur_a.wav   — 1.45 s, f0 ~150 Hz, 7 syllables (take A)
  assets/audio/crowd/murmur_b.wav   — 1.25 s, f0 ~205 Hz, 6 syllables (take B,
                                      brighter; alternates with A per speaker)
  assets/audio/crowd/grumble_low.wav — 1.05 s, f0 ~95 Hz, 4 slow syllables,
                                      dark lowpass (the worker grumble/grunt)

Deterministic (fixed seeds), pure stdlib. Run from the repo root:
    python tools/gen_crowd_chatter.py
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


def murmur(path, seed, dur, f0_base, syllables, lp_hz, bright):
    """Syllabic voiced walla: saw buzz at a wobbling f0 through a one-pole
    lowpass + band-passed noise, gated by per-syllable envelopes."""
    rng = random.Random(seed)
    n = int(dur * SR)
    buf = [0.0] * n

    # Syllable schedule: on/off bursts with jittered lengths + gaps, a falling
    # "sentence" contour (each syllable a touch quieter + lower, like trailing
    # off mid-sentence).
    sylls = []
    t = 0.02
    for k in range(syllables):
        on = 0.09 + rng.random() * 0.09          # 90-180 ms voiced burst
        gap = 0.03 + rng.random() * 0.07         # 30-100 ms consonant gap
        sylls.append((t, on, 1.0 - 0.06 * k))
        t += on + gap
    total_on = sylls[-1][0] + sylls[-1][1]
    stretch = (dur - 0.05) / max(0.2, total_on)  # fit the schedule to dur
    sylls = [(s * stretch, o * stretch, v) for (s, o, v) in sylls]

    phase = 0.0
    lp = 0.0        # voiced lowpass state
    nlp = 0.0       # noise band state (low side)
    nhp = 0.0       # noise band state (high side)
    for i in range(n):
        tt = i / SR
        # Envelope: sum of syllable gates with 12 ms edges.
        env = 0.0
        pitch_arc = 1.0
        for (s, o, v) in sylls:
            if s - 0.012 < tt < s + o + 0.012:
                a = min(1.0, max(0.0, (tt - s) / 0.012))
                r = min(1.0, max(0.0, (s + o - tt) / 0.030))
                env = max(env, v * a * r)
                # Within a syllable the pitch rises then falls (speech-ish).
                u = min(1.0, max(0.0, (tt - s) / max(1e-3, o)))
                pitch_arc = 1.0 + 0.10 * math.sin(math.pi * u)
        # f0 wobble: slow drift + per-syllable arc = conversational prosody.
        f0 = f0_base * pitch_arc * (1.0 + 0.06 * math.sin(2 * math.pi * 1.7 * tt)
                                    - 0.08 * (tt / dur))
        phase += f0 / SR
        saw = 2.0 * (phase - math.floor(phase + 0.5))
        # Voiced buzz through a one-pole lowpass (muffles it to a mumble).
        lp += (saw - lp) * min(0.9, 2 * math.pi * lp_hz / SR)
        # Consonant-ish noise, band-passed ~200-2000 Hz (LP chain minus LP).
        noise = rng.random() * 2 - 1
        nlp += (noise - nlp) * min(0.9, 2 * math.pi * 2000.0 / SR)
        nhp += (nlp - nhp) * min(0.9, 2 * math.pi * 200.0 / SR)
        band = nlp - nhp
        buf[i] = (lp * 0.75 + band * (0.30 if bright else 0.20)) * env

    peak = max(abs(s) for s in buf) or 1.0
    buf = [s * (0.62 / peak) for s in buf]
    write_wav(path, buf)


murmur('assets/audio/crowd/murmur_a.wav', seed=1847, dur=1.45,
       f0_base=150.0, syllables=7, lp_hz=750.0, bright=False)
murmur('assets/audio/crowd/murmur_b.wav', seed=4207, dur=1.25,
       f0_base=205.0, syllables=6, lp_hz=950.0, bright=True)
murmur('assets/audio/crowd/grumble_low.wav', seed=95, dur=1.05,
       f0_base=95.0, syllables=4, lp_hz=420.0, bright=False)
