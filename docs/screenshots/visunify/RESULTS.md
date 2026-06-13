# Vis-Unify — ONE culling brain (r_vis) + TLAS double-buffer

Branch `feat/vis-unify` (on top of `feat/gpu-cull`). Measured 2026-06-12 on the
14900K + RTX 5090, Release build unless noted.

## What was unified

Four visibility systems, four cvars, four stat vocabularies — now ONE policy
(`engine/rhi/Visibility.{h,cpp}`), resolved per frame against device caps:

| r_vis | PVS (rooms) | CPU frustum | GPU cull | HZB | notes |
|---|---|---|---|---|---|
| 0  | off | ON  | off | off | reference floor (CPU-only) |
| 1  | ON  | ON  | off | off | legacy default behaviour (the cvar default) |
| 2  | ON  | (GPU does it) | auto tier 0/1 | off | degrades to 1 without GPU cull |
| 3  | ON  | (GPU does it) | auto tier 0/1 | ON  | degrades to 2 without HZB |
| -1 | auto: best supported (3 -> 2 -> 1) | | | | |

The stages now COMPOSE: the PVS produces the room-visible instance set at
submission, and that set IS the GPU cull's input (`tested`). The CPU frustum
cull remains the universal fallback tier AND the bit-equivalence reference
(`--test-gpucull` 40/40, still EXACT).

Compat aliases (deprecation line logged): `r_cullpath`/`r_hzb` remap onto
`r_vis` (explicit tier preserved, so `--cullpath 2` still forces Tier 1);
`r_roomcull` is the PVS sub-override (0 = noclip draw-all). `r_frustumcull`
stays the predicate bypass (test knob). New CLI seed: `--vis N`.

## ONE stats block (numbers CONSERVE)

`rooms + frustum + hzb + drawn == candidates` — on the HUD panel, and one log
line under the smoketest:

```
vis L2(tier1) cand 1042: rooms 960 -> frustum 32 -> hzb 0 -> drawn 50 | pvs 0.01 cull 0.00/0.00 hzb 0.00 ms [CONSERVED]
```

## Level-1 indoor (canonical floor, `--smoketest --world canonlevel`)

The PVS-prefilter effect on the GPU cull's tested set — rooms hide most of the
level, so the GPU evaluates 82 instances instead of 1055:

| r_vis | active path | candidates | rooms culled | tested | frustum | hzb | drawn | conserves |
|---|---|---|---|---|---|---|---|---|
| 0 | cpu       | 1055 | 0   | 1055 | 408 | 0 | 647 | YES |
| 1 | cpu       | 1042 | 960 | 82   | 32  | 0 | 50  | YES |
| 2 | **tier1** | 1042 | 960 | **82** | 32 | 0 | **50** | YES |
| 3 | tier0+hzb | 1042 | 960 | 82   | 32  | 0 | 50  | YES |

* `drawn` IDENTICAL (50) across r_vis 1/2/3 on the still camera — PVS+GPU does
  not over-cull vs the CPU reference.
* r_vis 2: GPU `tested == 82 == the PVS survivor set` — the prefilter feeds the
  GPU cull instead of both running independently (r_vis 0 shows the un-filtered
  1055).
* (L0 candidates differ by 13: with the cull off, the room-gated character
  draws submit too — they are part of the candidate set by definition.)
* HZB on the indoor pose culls 0 at the spawn cell (nothing occluded beyond the
  PVS+frustum survivors — honest negative case; the 100k demo below is where it
  pays).

## Legacy 7-floor tower (`--smoketest`, the D15 reference)

| run | tested | drawn | frustum | conserves |
|---|---|---|---|---|
| `--vis 2` (unified) | 8568 | 1192 | 7376 | YES |
| `--cullpath 1` (legacy alias seed -> `r_vis 2 (tier 1 forced)`) | 8568 | 1192 | 7376 | YES |

Identical to the D15 bring-up reference (1192/8568 EXACT) on both vocabularies.

## TLAS double-buffer — the last declared hitch is dead

The scene-mutation TLAS rebuild used to `vkDeviceWaitIdle` + a fenced one-time
submit (~2x a median frame, the ONE declared zero-stutter exemption). Now:

* TWO TLAS backings + per-slot instance/scratch buffers; a mutation RECORDS the
  build into the frame's own command buffer against the INACTIVE slot (global
  sync2 AS barriers — entirely on the GPU timeline).
* Flip at the next frame boundary; each ring slot's RT descriptor re-points
  lazily AFTER its own fence wait (validation-silent by construction).
* Grown-out backings ride a retire queue (the engine's defer-free discipline).
* `vkDeviceWaitIdle` is GONE from the mutation path.

`--test-visunify` part D — 120 frames of per-frame spawn/despawn with RT-AO on:

| metric | before (sync rebuild) | after (double-buffer) |
|---|---|---|
| CPU waits in the mutation path | waitIdle + fence wait per mutation (~2x median frame) | **0** (`tlasSyncWaits == 0`) |
| steady-state mutation CPU max | — | **0.12 ms** (`tlasCpuMsMax`, grow builds excluded) |
| typical mutation CPU | — | **0.03 ms** |
| rebuilds recorded | 1 per mutation (blocking) | 120/120 (async, in-frame) |
| grows (one-time allocations) | n/a | 19 (instance-count high-water marks; bounded) |

## Acceptance (all measured 2026-06-12)

* `--test-visunify` **32/32** — Release AND Debug (validation ON, **0 VUID**):
  policy table, conservation across r_vis levels on a still camera, alias
  mapping through the real cvar sync, TLAS mutation zero-wait under load
  (Debug numbers: 120 builds, 0 syncWaits, maxCpuMs 0.16).
* `--test-gpucull` **40/40** Release + Debug (**0 VUID**) — the CPU/GPU
  equivalence stays EXACT.
* Full `--test-*` suite (all 82 flags), Release: **0 failures**.
* `--smoketest` across `--vis 0/1/2/3`: Release AND Debug all **0 VUID,
  `allocationCount=0`**, every unified line `[CONSERVED]`. Debug vis 3 on the
  legacy tower reproduces the D15 HZB split exactly: `hzb 475 -> drawn 717`
  (717 + 475 == 1192, the CPU reference).
* Debug `--smoketest --world canonlevel --vis 3`: 0 VUID, conserved
  (`cand 1042: rooms 960 -> frustum 32 -> hzb 0 -> drawn 50`).
* Stills across `r_vis 1/2/3` (canonlevel): **BYTE-IDENTICAL PNGs**
  (md5 `b6510e9575895fe4041ec326afc34906` for all three).

## Stills

* `canonlevel_vis1.png` / `canonlevel_vis2.png` / `canonlevel_vis3.png` —
  canonical Floor 1 from the spawn cell; byte-identical across the three
  policies (the no-over-cull proof at pixel level).
* `demo100k_vis3.png` — the 100k-object density demo on the full unified path
  with the unified stats overlay.
