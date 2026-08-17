#!/usr/bin/env python3
"""gen_engine_bank.py — the multi-RPM engine-note bank (W-ENGINE-NOTE, rev 2).

REV 2 — OWNER EAR-TEST ("Engine sound is Horrible. The idle was better the
first day."). The first-day sound was the ORIGINAL RECORDING — organic, noisy,
alive; its defect was loop STRUCTURE, not character. The pure-synth rev 1 was
mathematically perfect and lifeless. So every bank point is now a HYBRID:

  ORGANIC LAYER (dominant)   — SND-SONNET's mined loops from the original
      recording (extracted_idle_18hz / extracted_high_24hz: the two most
      pitch-stable plateaus, loop-closed), pitch-aligned to each point's
      firing frequency (f0 = rpm/60*3) by resampling, tiled to the loop
      length. This is the breath/grit/asymmetry no oscillator makes.
  SYNTH LAYER (underneath)   — the rev-1 flat-six harmonic stack (10
      harmonics, chuff, exhaust formants), mixed UNDER the organic layer as
      a harmonic supplement that pins the pitch identity at high RPM where
      the pitched-up recording alone thins out.
  TURBULENCE LAYER           — filtered broadband noise, share rising with
      RPM. A real engine is roughly half broadband (SND-OPUS measured the
      synth stopgap at ~0%); the validator now GATES on >25% broadband.

Energy split (of total): organic .60/.45 (low/high points), synth .20,
turbulence .20/.35. The whole mix gets 1-2 dB of slow loop-periodic
pseudo-random pulse-level variation (mandatory per the Opus findings — a
real engine's cycles are never identical).

Carried over from rev 1 (all still apply):
  * loop length snapped to an integer number of firing cycles; 40-60 ms
    equal-power close; wrap ROTATED onto the flattest interior step
    (wrap-step ~0x the median sample step, below 16-bit quantization);
  * RMS loudness match across each family (on-load -11 dBFS, overrun -6 dB
    under it), one global headroom trim, DC removed (mean subtraction —
    the loop-safe 5 Hz high-pass equivalent);
  * OVERRUN family: darker rolloff, irregular sub-harmonic AM burble
    (components clustered on the half-order, exactly loop-periodic),
    crackle transients kept clear of the crossfade;
  * engine_noise_bed.wav — the runtime's 5th voice, gain riding LOAD
    (baked noise cannot follow load; a separate voice can);
  * whine_loop.wav / turbo_whistle_loop.wav — the whine/turbo layers'
    own assets (no more pitched-up copies of the engine wav).

Sources: the mined extracts are copied under assets/audio/vehicles/
engine_bank/src/ so the bank regenerates from the repo alone.
Output: <repo>/assets/audio/vehicles/engine_bank/*.wav (mono 16-bit 96 kHz).
Deterministic (fixed seeds). Validate with tools/validate_engine_bank.py.
"""
import os
import wave
from fractions import Fraction

import numpy as np
from scipy.signal import butter, sosfilt, resample_poly

SR = 96000
HERE = os.path.dirname(os.path.abspath(__file__))
OUTDIR = os.path.normpath(os.path.join(HERE, "..", "assets", "audio", "vehicles", "engine_bank"))
SRCDIR = os.path.join(OUTDIR, "src")

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
ONLOAD_RMS_DBFS = -11.0     # bank-wide loudness target
OVERRUN_DB_BELOW = 6.0      # overrun family sits this far under the on-load family
PEAK_CEILING = 10 ** (-1.0 / 20.0)   # -1 dBFS

# The mined organic sources (f0 measured by autocorrelation, 12-45 Hz band).
# idle plateau feeds the lower half of the bank, high plateau the upper half.
ORGANIC_SRC = {
    "idle": ("extracted_idle_18hz.wav", 18.161),
    "high": ("extracted_high_24hz.wav", 23.869),
}
ORGANIC_FOR_RPM = { 900: "idle", 1500: "idle", 2500: "idle",
                    4000: "high", 5500: "high", 7000: "high" }


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getframerate() == SR and w.getsampwidth() == 2 and w.getnchannels() == 1
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768.0


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


def rms(x):
    return float(np.sqrt(np.mean(x ** 2))) + 1e-12


def xfade_ms_for(rpm):
    return 60 if rpm >= 4000 else 40


def close_loop(seg_ext, loop_len, xfade_n):
    """Equal-power blend of the loop head into the material that follows the
    loop point, then ROTATE the closed circle so the file boundary lands on
    the flattest interior step (the wrap becomes an ordinary small step)."""
    loop = seg_ext[:loop_len].copy()
    tt = np.linspace(0, 1, xfade_n, endpoint=False)
    fade_in, fade_out = np.sin(tt * np.pi / 2), np.cos(tt * np.pi / 2)
    loop[:xfade_n] = loop[:xfade_n] * fade_in + seg_ext[loop_len:loop_len + xfade_n] * fade_out
    steps = np.abs(np.diff(loop))
    r = int(np.argmin(steps)) + 1
    return np.roll(loop, -r)


def organic_layer(rpm, f0, n_gen, rng):
    """The recording, made this RPM's engine: pitch-align the mined plateau's
    fundamental to the firing frequency by resampling, then tile (the extract
    is itself seamless, so tiling is continuous) with a random start phase."""
    name, f_src = ORGANIC_SRC[ORGANIC_FOR_RPM[rpm]]
    src = read_wav(os.path.join(SRCDIR, name))
    ratio = Fraction(f0 / f_src).limit_denominator(200)   # pitch UP by f0/f_src
    shifted = resample_poly(src, ratio.denominator, ratio.numerator)
    reps = int(np.ceil((n_gen + len(shifted)) / len(shifted)))
    tiled = np.tile(shifted, reps)
    start = rng.integers(0, len(shifted))
    return tiled[start:start + n_gen]


def synth_layer(rpm, f0, t, rng, *, overrun):
    """Rev-1 flat-six harmonic stack + exhaust formants — the pitch anchor,
    mixed UNDER the organic layer."""
    rolloff = 1.20 if overrun else 0.95   # tone pass: was 1.35/1.15 — gentler, more top end
    sig = np.zeros(len(t))
    for h in range(1, NHARM + 1):
        sig += (1.0 / h ** rolloff) * np.sin(2 * np.pi * f0 * h * t + 0.15 * h)
    sig /= np.max(np.abs(sig))
    sig *= 1.0 + 0.15 * np.sin(2 * np.pi * f0 * t)        # firing-rate chuff
    boom = resonant_bandpass(sig, SR, center_hz=max(70, f0 * 3), q=2.5, gain=0.55)
    rasp = resonant_bandpass(sig, SR, center_hz=min(1800, f0 * 14), q=1.8,
                             gain=0.28 if overrun else 0.35)
    # FUNDAMENTAL REINFORCEMENT at the top of the range.
    #
    # `boom` centres on the THIRD harmonic and the presence shelf lifts the
    # upper series, which is right for body and bite but leaves the firing
    # fundamental outgunned once f0 climbs. Measured on the shipped bank at
    # 7000 rpm (f0 = 350 Hz): the strongest peak was 703 Hz — the SECOND
    # harmonic — and 350 was nowhere in the top six. The note read an octave
    # high and thin at precisely the revs a flat-six should be most vocal.
    # Below ~250 Hz the fundamental already wins on its own, so this fades in
    # rather than applying everywhere and muddying idle.
    fund = 0.0
    if f0 > 250.0:
        k = min(1.0, (f0 - 250.0) / 200.0)
        fund = resonant_bandpass(sig, SR, center_hz=f0, q=3.0, gain=0.85 * k)
    return 0.55 * sig + boom + rasp + fund


def turbulence_layer(rpm, n_gen, rng):
    """Broadband intake/exhaust turbulence, tilt brightening with RPM."""
    noise = rng.standard_normal(n_gen)
    hi = 0.3 + 0.7 * (rpm / 7000.0)
    return (resonant_bandpass(noise, SR, center_hz=250, q=0.7, gain=1.6)   # broadband floor lives in the LOW roar, not the hiss band
            + resonant_bandpass(noise, SR, center_hz=1200, q=0.6, gain=0.5 + 0.3 * hi)
            + resonant_bandpass(noise, SR, center_hz=3600, q=0.6, gain=0.06 + 0.16 * hi))   # static fix: was 0.15+0.45*hi — hiss


def hf_gain(sig, lp):
    return sig - lp    # content above the one-pole corner


def build_point(rpm, *, overrun, rng):
    f0 = rpm / 60.0 * 3.0                       # flat-six firing frequency
    n_cycles = max(2, round(TARGET_LOOP_S * f0))
    if overrun and n_cycles % 2:
        n_cycles += 1                           # half-order AM must be loop-periodic
    loop_s = n_cycles / f0
    loop_len = int(round(loop_s * SR))
    xfade_n = int(SR * xfade_ms_for(rpm) / 1000)
    n_gen = loop_len + xfade_n
    t = np.arange(n_gen) / SR

    org = organic_layer(rpm, f0, n_gen, rng)
    syn = synth_layer(rpm, f0, t, rng, overrun=overrun)
    nz = turbulence_layer(rpm, n_gen, rng)

    # ENERGY SPLIT: organic DOMINANT, synth a supplement underneath, noise
    # share rising with RPM (more air moving). These fractions are what the
    # validator's >25%-broadband gate audits downstream. (The low points
    # carry a bigger turbulence share than rev 2's first cut: at idle the
    # organic layer's own breath sits partly ON the harmonic comb, so the
    # free-standing noise layer has to supply the broadband floor itself.)
    high = rpm >= 4000
    # Tone pass: organic keeps the low-mid body but hands the top end to the
    # synth harmonics + turbulence (the recording HAS no top end to give —
    # 90% of its energy sits below 100 Hz; that was the muffle).
    w_org, w_syn, w_nz = (0.42, 0.38, 0.20) if high else (0.46, 0.30, 0.24)   # low points keep more (LOW-band) noise for the broadband floor
    if rpm <= 1000:
        # Idle is the most MECHANICAL point in a real engine — valvetrain
        # clatter and accessory whir are broadband. Without this the idle
        # pair lands ~21% broadband against the 25% gate.
        w_org, w_syn, w_nz = (0.40, 0.26, 0.34)
    # PRESENCE TILT on the HARMONIC content only (static fix): shelving the
    # full mix also shelved the noise into hiss. Brightness must come from
    # the engine's own harmonics; the turbulence stays a low bed.
    harm = org / rms(org) * np.sqrt(w_org) + syn / rms(syn) * np.sqrt(w_syn)
    # THE SHELF CORNER HAS TO TRACK f0, NOT SIT AT A FIXED 700 Hz.
    #
    # A fixed corner shelves a DIFFERENT part of the harmonic series at every
    # point. At 900 rpm (f0 = 45 Hz) the first fifteen harmonics live below
    # 700 and are untouched; at 7000 rpm (f0 = 350 Hz) only the FUNDAMENTAL is
    # below it, so every harmonic except the one that defines the pitch got a
    # +3.8 dB lift. Measured on the shipped bank: at 7000 the strongest peak
    # was ~703 Hz — the second harmonic — and 350 Hz did not appear in the top
    # six at all. The note reads an octave high and thin exactly where a
    # flat-six should be at its most vocal, which is the top of the range.
    #
    # Cornering at 6 x f0 shelves the SAME harmonic index everywhere: the
    # fundamental and the low harmonics that carry the body are never touched,
    # and the presence lift lands on the upper series at every rpm. Clamped so
    # idle does not shelve at 270 Hz and the redline does not shelve above the
    # useful band.
    shelf_hz = float(np.clip(f0 * 6.0, 420.0, 2600.0))
    a = float(np.exp(-2.0 * np.pi * shelf_hz / SR))
    lp = np.empty_like(harm); acc = 0.0
    for i in range(len(harm)):
        acc = a * acc + (1.0 - a) * harm[i]
        lp[i] = acc
    harm = harm + 0.55 * hf_gain(harm, lp)     # +3.8 dB shelf, harmonics only
    sig = harm + nz / rms(nz) * np.sqrt(w_nz)
    # (Family level only — the FINAL per-file loudness is set by the target
    # dBFS table in main(), which normalizes every file. Anything set here is
    # overwritten there, which is exactly why the rev-with-rpm swell had to be
    # applied to the TARGET and not to this line.)
    sig = sig / rms(sig) * 0.22

    # ---- THE HEADER RUMBLE (owner's target, 2026-08-17: "if this sounds like
    # a subaru engine with headers and exhaust when done, that is WINNING").
    #
    # What people actually mean by that sound is not the boxer layout — it is
    # UNEQUAL-LENGTH HEADERS. When one bank's runners are longer than the
    # other's, the exhaust pulses stop arriving evenly: they group into pairs,
    # and the ear hears a beat at HALF the firing frequency. That half-order
    # component is the rumble. An engine with equal-length headers has the same
    # firing order and does not do it, which is why the aftermarket sells
    # unequal headers to put it BACK.
    #
    # The bank already had half-order AM — but only in the OVERRUN family,
    # where it reads as burble on a trailing throttle. On load there was none,
    # so the note was even and smooth: a well-sorted six, not a rumble. This
    # puts a gentler version of the same modulation on the ON-LOAD family, so
    # the character is there while you are driving, not only when you lift.
    #
    # Depth is deliberately about a third of the overrun's (0.14 -> 0.05): the
    # goal is a lope you feel underneath the note, not a wobble. Loop-periodic
    # by construction (k cycles per loop), so it cannot buzz at the seam.
    k_half_on = n_cycles // 2
    if not overrun and k_half_on >= 1:
        rumble = np.zeros(n_gen)
        for dk, depth in ((0, 0.050), (-1, 0.028), (+1, 0.022)):
            rumble += depth * np.sin(2 * np.pi * (k_half_on + dk) / loop_s * t
                                     + rng.uniform(0, 2 * np.pi))
        sig *= 1.0 + np.clip(rumble, -0.45, 0.45)

    # SLOW PSEUDO-RANDOM PULSE-LEVEL VARIATION on the WHOLE mix (~1-2 dB):
    # loop-periodic sinusoids (k cycles/loop, small coprime k, random phase).
    drift = np.zeros(n_gen)
    for k_cpl, depth in ((2, 0.07), (3, 0.05), (5, 0.04), (7, 0.03)):
        drift += depth * np.sin(2 * np.pi * k_cpl / loop_s * t + rng.uniform(0, 2 * np.pi))
    sig *= 1.0 + drift

    if overrun:
        # IRREGULAR SUB-HARMONIC AM (burble): components clustered on the
        # half-order, every one exactly loop-periodic, neighbours beating.
        k_half = n_cycles // 2
        am = np.zeros(n_gen)
        for dk, depth in ((0, 0.14), (-1, 0.08), (+1, 0.07), (-2, 0.05), (+2, 0.04)):   # BRRAAHPP fix: mutter, not bark
            am += depth * np.sin(2 * np.pi * (k_half + dk) / loop_s * t + rng.uniform(0, 2 * np.pi))
        sig *= 1.0 + np.clip(am, -0.85, 0.85)

        # CRACKLE: unburnt-mixture pops, kept clear of the crossfade region.
        n_crackle = rng.poisson((2.5 + 8.0 * rpm / 7000.0) * loop_s)   # BRRAAHPP fix: sparser
        ref = rms(sig)
        for _ in range(n_crackle):
            dur = int(SR * rng.uniform(0.0015, 0.005))
            pos = int(rng.uniform(xfade_n, loop_len - dur - 1))
            burst = rng.standard_normal(dur) * np.exp(-np.arange(dur) / (0.25 * dur + 1))
            burst = resonant_bandpass(burst, SR, center_hz=rng.uniform(700, 2200), q=1.2, gain=1.0)
            sig[pos:pos + dur] += burst * ref * rng.uniform(0.5, 1.2)   # BRRAAHPP fix: quieter, rounder pops

    sig /= np.max(np.abs(sig)) + 1e-9
    return close_loop(sig, loop_len, xfade_n)


def synth_noise_bed(rng):
    """Broadband turbulence — the runtime's 5th voice (gain rides LOAD)."""
    loop_s = 1.2
    loop_len = int(loop_s * SR)
    xfade_n = int(SR * 60 / 1000)
    n_gen = loop_len + xfade_n
    noise = rng.standard_normal(n_gen)
    # THE HISS, AND WHY IT SURVIVED THE FIRST FIX (owner, 2026-08-16: "Static
    # hiss at higher rpms.. their concept is GOOD.. the execution is NOT").
    #
    # turbulence_layer above was already de-hissed — its 3.6 kHz band was pulled
    # from 0.15+0.45*hi down to 0.06+0.16*hi. THIS bed was not, and it is the
    # worse offender of the two, because it is the voice whose gain RIDES LOAD:
    # at high rpm on throttle it comes up to full while the engine note it sits
    # under has almost all of its energy below 2 kHz.
    #
    # MEASURED, before: the bed put 18.4% of its energy above 5 kHz against
    # 1.8% for flat6_onload_0900 and 5.1% for _7000 — roughly TEN TIMES the
    # high-frequency content of the thing it is supposed to thicken. That is
    # not a turbulence bed, it is white noise with a gain envelope. The commit
    # that introduced it says "LOW roar and idle clatter, never hiss"; the
    # intent was right and only the numbers were wrong.
    #
    # Fixed by tilting it like real intake/exhaust turbulence: the low roar
    # carries it, the presence band is halved, and the 3.4 kHz band drops to a
    # trace. A one-pole rolloff above ~1.4 kHz then gives the whole bed a
    # continuous downward slope instead of a flat shelf with a bump on top.
    sig = (resonant_bandpass(noise, SR, center_hz=170, q=0.7, gain=1.35)
           + resonant_bandpass(noise, SR, center_hz=520, q=0.6, gain=0.62)
           + resonant_bandpass(noise, SR, center_hz=1500, q=0.6, gain=0.22)
           + resonant_bandpass(noise, SR, center_hz=3400, q=0.6, gain=0.045))
    # Continuous HF rolloff (one-pole, ~1.4 kHz). Keeps the bed a ROAR at every
    # load instead of turning into air as the runtime brings its gain up.
    a_bed = float(np.exp(-2.0 * np.pi * 1400.0 / SR))
    acc = 0.0
    for i in range(len(sig)):
        acc = a_bed * acc + (1.0 - a_bed) * sig[i]
        sig[i] = acc
    t = np.arange(n_gen) / SR
    sig *= 1.0 + 0.12 * np.sin(2 * np.pi * 3 / loop_s * t + rng.uniform(0, 2 * np.pi)) \
               + 0.08 * np.sin(2 * np.pi * 7 / loop_s * t + rng.uniform(0, 2 * np.pi))
    sig /= np.max(np.abs(sig)) + 1e-9
    return close_loop(sig, loop_len, xfade_n)


def synth_bov(rng):
    """BLOW-OFF VALVE — the PSSSHT on lift (owner, 2026-08-17: "we need turbo
    blow off valve noises... PSSSSHT").

    WHY IT EXISTS. host_tunnel fired the blow-off by playing the ENGINE LOOP as
    a one-shot at 0.45 gain and 4.2x pitch — a 1.2 s engine wav turned into a
    screech on every lift. That is the "awful loud sound when you let off the
    gas". A BOV is not a pitched anything: it is a slug of compressed air
    dumping through a valve, so it is NOISE with a fast attack, a formant that
    falls as the plenum empties, and a tail.

    Shape, in order: a 6 ms attack (the valve cracking open), a bright rush
    while the pressure is high, a downward formant sweep ~4.2 kHz -> 1.5 kHz as
    the plenum empties, and a ~260 ms exponential tail. One-shot, not a loop, so
    it does not need to close seamlessly — only to start and end at zero.
    """
    dur_s = 0.34
    n = int(dur_s * SR)
    t = np.arange(n) / SR
    noise = rng.standard_normal(n)

    # DOWNWARD FORMANT SWEEP. A fixed band reads as a hiss burst; the falling
    # centre is what makes the ear hear a volume of air EMPTYING.
    sweep = np.zeros(n)
    for f_c, w in ((4200.0, 0.55), (2600.0, 0.85), (1500.0, 0.70)):
        sweep += w * resonant_bandpass(noise, SR, center_hz=f_c, q=1.1, gain=1.0)
    # Cross-fade the three bands over time: bright first, low last.
    k = np.clip(t / dur_s, 0.0, 1.0)
    hi = resonant_bandpass(noise, SR, center_hz=4200.0, q=1.1, gain=1.0)
    mid = resonant_bandpass(noise, SR, center_hz=2600.0, q=1.1, gain=1.0)
    lo = resonant_bandpass(noise, SR, center_hz=1500.0, q=1.2, gain=1.0)
    wh = np.clip(1.0 - k * 2.2, 0.0, 1.0)
    wm = np.clip(1.0 - np.abs(k - 0.35) * 3.0, 0.0, 1.0)
    wl = np.clip((k - 0.25) * 1.8, 0.0, 1.0)
    sig = hi * wh + mid * wm + lo * wl

    # A trace of valve flutter — the mechanical edge that separates a BOV from
    # a plain noise burst. Deliberately faint; this is a texture, not a tone.
    sig += 0.06 * np.sin(2 * np.pi * 95.0 * t) * np.exp(-t / 0.05)

    # ENVELOPE: 6 ms attack, exponential decay. Starts and ends at zero.
    atk = int(SR * 0.006)
    env = np.exp(-t / 0.09)
    env[:atk] *= np.linspace(0.0, 1.0, atk)
    tail = int(SR * 0.02)
    env[-tail:] *= np.linspace(1.0, 0.0, tail)
    sig *= env

    sig /= np.max(np.abs(sig)) + 1e-9
    return sig


def synth_whine(rng):
    """Supercharger gear whine (~1.1 kHz), cycle-snapped, loop-periodic shimmer."""
    loop_s = 1.0
    f = round(1100 * loop_s) / loop_s
    n = int(loop_s * SR)
    t = np.arange(n) / SR
    sig = np.zeros(n)
    for h, a in ((1, 1.0), (2, 0.35), (3, 0.18), (4, 0.08)):
        sig += a * np.sin(2 * np.pi * f * h * t + 0.4 * h)
    sig *= 1.0 + 0.10 * np.sin(2 * np.pi * 6 / loop_s * t) + 0.06 * np.sin(2 * np.pi * 11 / loop_s * t + 1.3)
    # BREATH. The shipped whine measured a crest factor of 1.79 against 1.41
    # for a pure sine — four harmonics and 4% noise is a test tone, not a
    # blower. A real gear whine rides on the air it is moving, so the
    # narrowband breath goes up and a wider band joins it; crest lands ~2.4.
    sig += 0.14 * resonant_bandpass(rng.standard_normal(n), SR, center_hz=f, q=6.0, gain=1.0)
    sig += 0.07 * resonant_bandpass(rng.standard_normal(n), SR, center_hz=f * 0.55, q=1.4, gain=1.0)
    sig /= np.max(np.abs(sig)) + 1e-9
    steps = np.abs(np.diff(sig))
    return np.roll(sig, -(int(np.argmin(steps)) + 1))


def synth_turbo_whistle(rng):
    """Turbo spool whistle: breathy narrowband noise ~3 kHz over a weak tone."""
    loop_s = 1.0
    loop_len = int(loop_s * SR)
    xfade_n = int(SR * 60 / 1000)
    n_gen = loop_len + xfade_n
    t = np.arange(n_gen) / SR
    f = round(3000 * loop_s) / loop_s
    noise = rng.standard_normal(n_gen)
    sig = resonant_bandpass(noise, SR, center_hz=3000, q=9.0, gain=1.0)
    sig += resonant_bandpass(noise, SR, center_hz=6100, q=8.0, gain=0.25)
    sig += 0.18 * np.sin(2 * np.pi * f * t)
    sig /= np.max(np.abs(sig)) + 1e-9
    return close_loop(sig, loop_len, xfade_n)


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    for name, _ in ORGANIC_SRC.values():
        assert os.path.exists(os.path.join(SRCDIR, name)), \
            f"missing organic source {name} under {SRCDIR}"

    # THE ENGINE HAS TO GET LOUDER AS IT CLIMBS.
    #
    # Every on-load point was normalized to the SAME ONLOAD_RMS_DBFS, and the
    # shipped bank measured it: RMS 0.2001 at 900 rpm and 0.2001 at 7000, to
    # four decimals, at every point in both families. A real engine does not do
    # that — and the consequence is not subtle. With the bank dead flat, the
    # only thing that grew as the revs rose was the load-driven NOISE BED,
    # which is the other half of why the top of the range read as hiss instead
    # of as an engine opening up.
    #
    # A gentle monotonic swell of ~+3.7 dB idle -> redline, centred so the
    # midrange stays where it is (the whole bank does not just get louder).
    # Gentle on purpose: the runtime already scales with load, and the bracket
    # crossfade is equal-power, so a steep ramp would step at every boundary.
    def level_dbfs(rpm):
        k = float(np.clip(rpm / 7000.0, 0.0, 1.0))
        return ONLOAD_RMS_DBFS - 2.4 + 3.7 * k

    files = []          # (name, samples, target_rms_dbfs)
    for i, (name, rpm) in enumerate(RPM_POINTS):
        rng = np.random.default_rng(1234 + i)            # deterministic per point
        files.append((f"flat6_onload_{rpm:04d}.wav",
                      build_point(rpm, overrun=False, rng=rng), level_dbfs(rpm)))
        rng = np.random.default_rng(5678 + i)
        files.append((f"flat6_overrun_{rpm:04d}.wav",
                      build_point(rpm, overrun=True, rng=rng),
                      level_dbfs(rpm) - OVERRUN_DB_BELOW))

    files.append(("engine_noise_bed.wav", synth_noise_bed(np.random.default_rng(9000)), -14.0))
    files.append(("whine_loop.wav", synth_whine(np.random.default_rng(9001)), -16.0))
    files.append(("turbo_whistle_loop.wav", synth_turbo_whistle(np.random.default_rng(9002)), -16.0))

    # DC removal (loop-safe: a constant shifts both endpoints equally).
    files = [(fn, s - np.mean(s), tgt) for fn, s, tgt in files]

    # Loudness match; single global headroom trim preserves the offsets.
    gains = [10 ** (tgt / 20.0) / rms(s) for _, s, tgt in files]
    trim = min(1.0, min(PEAK_CEILING / (np.max(np.abs(s)) * g)
                        for (_, s, _), g in zip(files, gains)))

    # THE BOV IS A ONE-SHOT, AND IT IS NORMALIZED BY PEAK, NOT RMS.
    #
    # It is kept OUT of the loudness-match above on purpose. Every other member
    # is a steady loop whose RMS is the meaningful level, but the BOV is an
    # enveloped burst: most of its length is decayed tail, so RMS-matching it
    # would demand a large gain, its attack would then set the global PEAK
    # CEILING, and `trim` would quietly pull the ENTIRE BANK down to make room
    # for one sample. Its playback level belongs to the host anyway (it is
    # scaled by how much boost was actually dumped), so what it owes the file
    # is a clean peak and nothing else.
    bov = synth_bov(np.random.default_rng(9003))
    bov = bov - np.mean(bov)
    bov = bov / (np.max(np.abs(bov)) + 1e-9) * PEAK_CEILING
    write_wav(os.path.join(OUTDIR, "bov_psssht.wav"), bov)
    print(f"{'bov_psssht.wav':28s} {len(bov)/SR:6.3f}s  RMS {20*np.log10(rms(bov)):6.2f} dBFS  "
          f"peak {20*np.log10(np.max(np.abs(bov))):6.2f}  (one-shot, peak-normalized)")

    for (fn, s, tgt), g in zip(files, gains):
        out = s * g * trim
        write_wav(os.path.join(OUTDIR, fn), out)
        wrap = abs(out[0] - out[-1])
        med = np.median(np.abs(np.diff(out)))
        print(f"{fn:28s} {len(out)/SR:6.3f}s  RMS {20*np.log10(rms(out)):6.2f} dBFS  "
              f"peak {20*np.log10(np.max(np.abs(out))):6.2f}  "
              f"wrap/median-step {wrap/max(med,1e-12):5.2f}x")
    if trim < 1.0:
        print(f"(global headroom trim applied: {20*np.log10(trim):.2f} dB)")
    print(f"\n{len(files)} files -> {OUTDIR}")


if __name__ == "__main__":
    main()
