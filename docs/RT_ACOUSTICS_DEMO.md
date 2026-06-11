# RT Acoustics — audio rays through the render TLAS (demo transcript)

**What it is:** gunfire muffles through the ACTUAL level geometry and rooms
reverb from their ACTUAL shape — audio rays traced against the same scene TLAS
the RT AO / reflections / DDGI passes use. The differentiator almost nobody
ships. `snd_rtacoustics 1` (default; self-gates on ray-query hardware),
`snd_rta_debug 1` for the live log line below.

## Architecture (one paragraph)

`engine/audio/RtAcoustics` fires, per active 3D emitter, a primary
listener→emitter ray + an 8-ray jittered fan; the blocked fraction is the
occlusion factor, smoothed ~200 ms. Every ~0.5 s a deterministic 64-ray sphere
from the LISTENER measures the room: mean free path + miss fraction →
small / medium / large / outdoor → reverb T60 + wet. Rays go through
`IRenderDevice::traceAudioRaysSubmit/Harvest` — an **async** one-dispatch
compute batch (`shaders/audio_rays.comp`, inline `rayQueryEXT`) against the
scene TLAS; submit one update, harvest the next, so the game thread never
fence-waits behind frame GPU work. The mixer (miniaudio) applies occlusion as
a volume duck (×0.3 at full occlusion) + a per-voice 4th-order lowpass
(19 kHz → ~420 Hz, log-mapped) retuned LIVE each update, and routes 3D
one-shots through a shared Schroeder reverb insert (4 comb + 2 allpass) whose
T60/wet track the room estimate.

## The audible contract — wall-walk transcript (`--test-acoustics` T7)

Deterministic CPU box-room tracer (same async contract as the GPU): a 10×4×10 m
room split by a wall with a doorway; the emitter keeps firing on the far side
while the listener walks from behind the solid wall to the doorway sightline.
This is the `snd_rta_debug` story: stand behind the wall → muffled; step
through the door → it opens up.

```
[acoustics-test] T7 wall-walk transcript (listener walks to the doorway):
  listener z=-3.0  ->  occlusion 1.00   (muffled behind wall)
  [rta] room=small mfp=3.0m miss=0% t60=0.61s wet=0.06 | emitters: (2,2,1 occ=1.00)
  listener z=-2.0  ->  occlusion 1.00   (muffled behind wall)
  [rta] room=small mfp=3.2m miss=0% t60=0.50s wet=0.09 | emitters: (2,2,1 occ=1.00)
  listener z=-1.0  ->  occlusion 1.00   (muffled behind wall)
  [rta] room=small mfp=3.2m miss=0% t60=0.43s wet=0.11 | emitters: (2,2,1 occ=1.00)
  listener z=-0.3  ->  occlusion 0.72   (muffled behind wall)
  [rta] room=small mfp=3.3m miss=0% t60=0.40s wet=0.13 | emitters: (2,2,1 occ=0.72)
  listener z=+0.4  ->  occlusion 0.20   (clear through the door)
  [rta] room=small mfp=3.3m miss=0% t60=0.38s wet=0.13 | emitters: (2,2,1 occ=0.20)
  listener z=+1.0  ->  occlusion 0.03   (clear through the door)
  [rta] room=small mfp=3.3m miss=0% t60=0.36s wet=0.14 | emitters: (2,2,1 occ=0.03)
  listener z=+1.4  ->  occlusion 0.00   (clear through the door)
  [rta] room=small mfp=3.3m miss=0% t60=0.36s wet=0.14 | emitters: (2,2,1 occ=0.00)
PASS T7 wall-walk: high behind wall -> ~0 in the doorway, never rising
```

Full self-test verdict: **9/9** —
LOS ~0 vs behind-wall ≥0.85, newborn-snap within 2 updates, ~200 ms smoothing
(high → intermediate → ~0, no zipper), small-room vs outdoor classification,
bit-identical determinism across instances, monotone wall-walk.

## GPU path under validation (`--smoketest`, RTX 5090)

```
Release:
[rta] audio-ray chain ready (audio rays through the scene TLAS)
[rta] smoketest async trace OK: harvested 66 rays, floorHit=0.28m,
      worst submit+harvest CPU=0.068ms (game thread, non-blocking)
[rhi] VMA shutdown leak check: live allocationCount=0 (expect 0)

Debug (Vulkan validation layers, zero VUIDs):
[rta] smoketest async trace OK: harvested 66 rays, floorHit=0.28m,
      worst submit+harvest CPU=0.177ms (game thread, non-blocking)
[rhi] VMA shutdown leak check: live allocationCount=0 (expect 0)
```

`floorHit=0.28m` is the down-ray from the smoketest camera at y=1.7 in the
armory hitting Level-1 floor geometry through the TLAS — real scene, real hit.

## Budget

≤16 emitters × 9 rays + 64 room rays = **208 rays max per batch**, one compute
dispatch (64-wide), submitted at most once per update; room sphere only at
2 Hz. Game-thread steady-state cost measured **0.068 ms worst** (Release).
The first sync prototype fence-waited on the graphics queue and ate 60 ms
behind a heavy frame — that is WHY the API is async (submit/harvest, one
update of latency, absorbed by the mixer's live per-voice retune).

## What the miniaudio backend can express (and what was added)

- **Has natively:** per-voice volume/pitch/3D spatialization, a node graph,
  and `ma_lpf_node` (per-voice lowpass — used for the muffle, retuned live
  via `ma_lpf_node_reinit`).
- **Does NOT have:** reverb. Implemented a Schroeder reverb INSERT as a custom
  `ma_node` (4 parallel feedback combs 29.7/37.1/41.1/43.7 ms + 2 series
  allpasses 5.0/1.7 ms, dry+wet, params via atomics, smoothed on the audio
  thread, CONTINUOUS_PROCESSING so gunshot tails ring out).
- `snd_rtacoustics 0` (or non-RT hardware): occlusion provider never hooked →
  the mixer play path is byte-for-byte the pre-acoustics path.

## Suite verdicts (2026-06-11, 14900K / RTX 5090)

- `--test-acoustics` 9/9, `--test-audio` 11/11 — Release AND Debug.
- Full `--test-*` suite (82 flags): **81/82 in one sequential sweep; the lone
  `--test-collapse` exit-127 was a load flake** (two OTHER agents' X3Engine
  GPU tests were saturating the machine concurrently) — re-ran 4x directly:
  29/29 green every time.
- Release `--smoketest`: exit 0, `VMA shutdown leak check: live
  allocationCount=0`. Debug `--smoketest` (validation layers): exit 0, zero
  VUIDs, allocationCount=0.
