# RT reflections — visual verification pass

Evidence for the three commits on `inspx/rt-reflections`:

| | commit | what it claims |
|---|---|---|
| **A** | `bc8c52a0` | RT reflection hits read **real per-object albedo/emissive** via `instanceCustomIndex` -> ObjectData SSBO, replacing a flat grey constant |
| **B** | `de12fbb5` | **Glossy roughness-aware** reflections — blur the traced reflection instead of fading it out |
| **C** | `2a908b06` | Performance parts reach the **canon car** + catalog path resolves |

All shots are headless (`DeviceDesc::headless`, `armCapture` / `captureFrame`), 1280x720,
90 settle frames so TAA / SSR / auto-exposure / the IBL probe all converge.

## The rig — `--screenshot-reflverify [outDir]`

Added in `app/screenshot_hosts.cpp`. **100% procedural** — it deliberately uses no GLBs,
because on this box every GLB in `assets/` is an unfetched Git-LFS pointer (the LFS
endpoint 404s and `tools/asset_store.py fetch --all` reports `167 failed`). Every other
reflective scene in the repo (`--screenshot-car`, `--world showroom`, `--world drive`'s
hero car) depends on those assets, so none of them could carry this verification.

* A metal floor (metallic 1.0) split into **8 strips whose roughness ramps 0.02 -> 0.85**
  left-to-right, so one frame shows the whole glossy range.
* Three wide bars hang at y=7 — **outside the vertical frustum**, so they are never on
  screen. The SSR march always walks off the top edge, which makes the RT ray-query
  fallback the *only* thing that can shade their reflection. They are a saturated RED,
  a saturated GREEN, and a bright **emissive** CYAN.
* Matte pillars standing on the ramp (`edges_*`) give the reflector hard internal depth
  edges — the blur disc has no depth rejection, so this is where over-blurring shows up.

## A/B method

The A/B is done by **swapping SPIR-V only** — no C++ rebuild between variants, so
everything except the one shader under test is bit-identical. `glslc` run by hand was
verified to reproduce the build's own SPIR-V byte-for-byte first.

| directory | refl.comp | mesh.frag |
|---|---|---|
| `both_old/` | pre-`bc8c52a0` | pre-`de12fbb5` |
| `A_off_old_reflcomp/` | **pre-`bc8c52a0`** | current |
| `B_off_old_meshfrag/` | current | **pre-`de12fbb5`** |
| `both_new/` | current | current (**+ the tuning below**) |
| `_tuning/blur06`, `_tuning/blur14_fixed_kernel`, `_tuning/blur24` | current | `kReflBlurPx` sweep |

Shots per directory: `ramp_ssr_off`, `ramp_ssr_on_rt_off`, `ramp_ssr_on_rt_on_halfres`,
`ramp_ssr_on_rt_on_fullres`, `close_ssr_on_rt_on_halfres`, `close_ssr_off`,
`mirror_all_ssr_on_rt_on`, `edges_ssr_on_rt_on`, `edges_ssr_off`. The `_tuning/*`
directories keep only the 5 shots the sweep argument actually cites.
`_carC/drive_world.png` is the change-C no-regression shot.

## Verdicts

### A — `bc8c52a0` per-object material: **CORRECT**

`A_off_old_reflcomp/ramp_ssr_on_rt_on_halfres.png` vs `both_new/…` is a textbook
isolation. Old: all three bars reflect as the **same flat blue-grey smudge**. New: red
reads red, green reads green, and the emissive bar reflects as a **bright cyan glow**.

Per-channel delta vs the reflections-off baseline, inside each bar band (`skew` = max
channel minus min channel; ~0 means a neutral grey smudge):

| | red bar | green bar | cyan bar |
|---|---|---|---|
| old refl.comp | skew 8.42 | skew 5.69 | R−5.4 G−7.5 B−7.2, **skew 2.10** |
| new refl.comp | skew 14.49 | skew 9.83 | R+12.3 G+31.2 B+20.5, **skew 18.83** |

The emissive case is the clearest: the old constant made the reflection *darker* than the
sky baseline (all channels negative); the new one adds light with a strong G+B bias — a
9x increase in channel skew plus a sign flip.

### B — `de12fbb5` glossy reflections: **CORRECT** (with one pre-existing artifact, not its fault)

Traced-reflection strength per strip (mean |on − off|, strip interiors):

| roughness | 0.25 | 0.35 | 0.50 |
|---|---|---|---|
| pre-`de12fbb5` | 6.67 | 5.44 | **1.31** |
| current | 6.84 | 7.46 | **5.14** |

At rough 0.5 the traced reflection went from essentially nothing to strong — a ~4x gain.
Visually, in `B_off_old_meshfrag/` the reflections **stop dead** partway across the ramp;
in `both_new/` they continue across every strip, just softer. Claim confirmed.

**Mirror invariance holds.** `mirror_all_ssr_on_rt_on.png` (every strip forced to rough
0.02) diffs old-vs-new at mean 0.04/255, max 3/255 — and that residual is *identical*
for blur 6, 14 and 24, so it is frame-global dither, not the kernel. At rough ≤ 0.05
the function returns the single tap and both smoothsteps evaluate to 0, so the path is
bit-identical by construction.

**The horizontal banding is NOT caused by this commit.** The reflections show venetian-blind
striping, which looked at first like a 6-tap pattern. It is not: the same striping is
present in `both_old/` with none of the new code, and it is *worst* in
`B_off_old_meshfrag/`. It originates in the reflection buffer itself. The glossy blur
measurably **reduces** it (banding energy 2.61 -> 2.01 -> 1.81 with the tuning below).
Worth a separate look by whoever owns `refl.comp`, but it is pre-existing.

### C — `2a908b06`: **half correct — the headline fix does not fire**

* Catalog path fix: **VERIFIED.** `--test-vehparts` gives `22 passed, 1 failed`, and P1
  now resolves `…/assets/vehicles/parts.json`. The 1 failure is the P3 acceleration
  ordering bug the commit already flagged as pre-existing.
* No rendering regression: **VERIFIED.** `--world drive` builds, renders and drives
  (`fwdSpeed=2.19`) — see `_carC/drive_world.png`. It is the **graybox** car, not the
  clearcoat hero paint, because `Vehicles/CTR.glb` is an unfetched LFS pointer.
* **The canon car is still never tuned.** `app_run.cpp:3404` calls
  `worldCars.applyTuning(...)` immediately after `worldCars.build(...)`, but
  `WorldCars::m_driveBuilt` is only ever set inside `WorldCars::enterCar()`
  (`world_cars.cpp:274`) — the live rig is built **lazily on first entry**. So at the
  call site `m_driveBuilt` is always false, `applyTuning()` returns false, and nothing
  happens. Confirmed empirically: a full `--world canonlevel` boot logs
  `[vehparts] catalog: 11 categories, 36 parts` but **never** logs
  `[vehparts] canon car tuned from …`, which is guarded on that return value. Nothing
  re-applies the tuning after `enterCar()` builds the rig either.

## Tuning applied

`kReflBlurPx` **stays at 14**, and a **per-pixel rotation** was added to the disc.

Sweep of 6 / 14 / 24 (`band` = horizontal-banding energy, `halo` = reflection bleed
across a depth silhouette — both lower is better):

| variant | band | grain | halo excess |
|---|---|---|---|
| pre-`de12fbb5` (single tap) | 2.614 | 1.934 | 0.828 |
| `kReflBlurPx` 6 | **3.782** | 1.795 | 1.227 |
| `kReflBlurPx` 14, fixed kernel (as committed) | 2.015 | 1.816 | 1.295 |
| **`kReflBlurPx` 14 + per-pixel rotation (shipped)** | **1.811** | 1.863 | **1.168** |
| `kReflBlurPx` 24 | **0.974** | 1.823 | **1.506** |

* **6 is worse than no blur at all** — a disc that narrow just duplicates a thin feature.
* **24 is the smoothest** (banding −52%) but widens the depth-edge halo by +16%, which is
  exactly the smear the shader's own "deliberate UNDER-estimate" comment guards against.
  Not taken: this rig is one flat reflector, so it cannot fully validate a wider disc.
* **The rotation improves both axes at once** (banding −10%, halo −10%) for ~2 ALU and
  **no change to the sampling footprint**. The original comment claimed "golden-angle
  spiral so the taps decorrelate … TAA integrates the residual", but every pixel used the
  *identical* 6 offsets, so the error was static and structured — something TAA can never
  integrate. Rotating per pixel makes that claim true. Grain rises 2.6% but stays *below*
  the pre-commit baseline.

## Could NOT be verified

* **Clearcoat car paint** (the case `refl.comp` names in its comments) and every other
  GLB-dependent scene — all model assets are unfetched LFS pointers on this box.
* **Smearing across depth discontinuities in a real scene** — the rig's pillars give
  synthetic edges only, on a single flat reflector.
* **The `--world drive` hero-car reflection** for the same asset reason.

## Reproducing

> **This branch does not compile standalone.** Pre-existing and unrelated to these three
> commits: `app/app_run.cpp` (committed) references `x3::ui::SettingsModel::skipIntro`,
> `advancedOpen` and `x3::apphost::readSkipIntro()`, which the committed `app/ui.h` and
> `app/settings_io.h` do not define. Those definitions exist only **uncommitted** in the
> `inspx/terrain-corridor` lane's working tree (the Settings > Advanced panel). Add
> `bool advancedOpen = false; bool skipIntro = false;` to `SettingsModel`, plus
> `readSkipIntro()` and a trailing `bool skipIntro = false` parameter on `writeSettings`,
> and it builds. That local unblock was **not committed here** to avoid colliding with the
> other lane's in-flight change.

```
X3Engine.exe --screenshot-reflverify docs/screenshots/rt-refl-verify/both_new
```
