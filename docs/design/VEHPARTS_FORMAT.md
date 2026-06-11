# x3.vehparts/1 — vehicle performance-parts catalog format

The data behind the PERFORMANCE SHOP (drive in, build your car). One JSON file
(`assets/vehicles/parts.json`) describes every purchasable part; the game composes
the installed set + the ECU tune into a `VehicleBuild` and lowers it onto the
running Jolt vehicle via the engine's `x3::phys::WheeledTuning` (live re-tune — no
respawn, drive out and FEEL it).

Parsed by `app/vehparts.{h,cpp}` (`x3::game::vehparts::Catalog`). Self-test:
`--test-vehparts` (composition math + REAL physics deltas + dyno pop thresholds).

## Top level

```json
{
  "format": "x3.vehparts/1",
  "baseline": { ... },          // the STOCK car the parts modify
  "categories": [ { "id": "...", "label": "...", "parts": [ {...} ] } ]
}
```

`format` MUST be `"x3.vehparts/1"`. Unknown keys are ignored (forward-compatible).

## `baseline` — the stock car

| key            | meaning                                            | stock |
|----------------|----------------------------------------------------|-------|
| `torqueNm`     | stock peak engine torque (Nm)                      | 700   |
| `maxRpm`       | stock redline                                      | 6500  |
| `massKg`       | stock curb mass (kg) — matches DriveDemo's chassis | 1300  |
| `brakeTorque`  | stock brake torque (Nm)                            | 2200  |
| `suspFreq`     | stock suspension spring frequency (Hz)             | 2.2   |
| `suspDamp`     | stock damping ratio                                | 0.7   |
| `curve`        | stock normalized torque curve `[[rpmFrac,torqueFrac],...]` | see file |

## Part records

Every part: `id` (stable, unique), `name`, `tier` (1..4, ascending = better),
`price` (credits). Category-specific stat fields below. A part REPLACES the
installed part of its category (one per category).

### `camshaft` — shifts the torque CURVE
* `curve`: `[[rpmFrac, torqueFrac], ...]` (2..8 points, ascending X) — REPLACES the
  baseline normalized torque curve. Street cams fatten the low end; race cams trade
  low-end for a screaming top end.
* `redlineBonus`: rpm ADDED to `maxRpm` (race cams rev higher).

### `exhaust` — power % + the NOTE
* `powerPct`: engine torque gain in percent (composes additively with intake/IC).
* `noteId`: exhaust-note variant id (engine-loop audio selection; see AUDIO below).
* `pitchOffset`: added to the engine-loop pitch ramp (deeper/raspier per tier).
* `timbre`: 0..1 — drives the loop's volume shaping (burble emphasis).

### `intake` — power %
* `powerPct` as above.

### `intercooler` — supports more boost
* `powerPct`: small direct gain.
* `safeBoostBonus`: ADDED to the ECU's `safeBoost` threshold (a better core keeps
  intake temps down, so more boost before knock). THIS is how the IC matters on
  the dyno.

### `forced_induction`
* `fiType`: `"supercharger"` or `"turbo"`.
* `boostPowerPct`: torque gain percent AT FULL ECU BOOST (scales linearly with the
  ECU boost slider / `maxBoost`).
* Supercharger: gain is FLAT across the rev range; `whine` (0/1) = pitched whine
  audio layer gated on throttle.
* Turbo: `spoolLagS` (seconds to full boost at WOT — the laggy hit), `topEndBias`
  (0..1: how much of the gain is pushed to the top half of the curve; big turbos
  starve the low end a little and EXPLODE up top), `whistle` (0/1) = spool whistle
  + lift-throttle blowoff audio.

### `ecu` — the tune (dyno sliders live here)
* `maxBoost`: bar — the boost slider's upper limit on this ECU.
* `safeBoost`: bar — boost above this (plus intercooler `safeBoostBonus`) builds
  knock.
* `safeLean`: fuel-slider value above which (leaner than) knock builds. The fuel
  slider is a mixture scale: `1.0` = stoich, `< 1` rich (safe, slightly less
  power), `> 1` lean (more power, RISK).
* `safeTiming`: timing-slider value (0..1 advance) above which knock builds.
* `knockLimit`: knock index at/above which a dyno pull POPS the engine (bang,
  `engineDamaged`, power penalty until repaired).
* `powerPerBoost`: % torque per bar of effective boost (needs forced induction;
  NA picks up a token 25% of this).
* `powerPerTiming`: % torque at full timing advance.
* `leanPowerPct`: % torque bonus at the lean edge (the reason to flirt with it).

Knock index on a PULL:
`knock = max(0, boost - (safeBoost + icBonus)) * 2.5 + max(0, fuel - safeLean) * 8.0 + max(0, timing - safeTiming) * 5.0`
POP iff `knock >= knockLimit`. Damaged engine = `x0.85` torque until repaired
(repair cost = `repairCost` credits, on the shop screen).

### `tires` — grip
* `gripScale`: multiplies BOTH Jolt tire friction curves (longitudinal + lateral).
* `compound`: display name.

### `suspension` — ride height + spring
* `rideHeightDelta`: metres added to suspension min/max lengths (negative = lower).
* `suspFreq` / `suspDamp`: spring frequency (Hz) / damping ratio.

### `brakes`
* `brakeTorque`: Nm (replaces baseline; hand-brake wheels keep their 2.5x factor).

### `weight` — weight reduction
* `massDelta`: kg ADDED to baseline mass (negative; carbon panels, cage, lexan).

### `nitrous`
* `nitrousMult`: temporary engine-torque multiplier while the key (LEFT SHIFT in
  the drive world) is held.
* `tankSeconds`: seconds of spray per fill. Refill at the shop (`refillCost`).

## Composition (catalog + build -> WheeledTuning)

```
powerMult = (1 + (exhaust.powerPct + intake.powerPct + intercooler.powerPct)/100)
          * (1 + fi.boostPowerPct/100 * (tune.boost / ecu.maxBoost))      [SC: flat]
          * (1 + ecu.powerPerBoost/100 * effBoost + ecu.powerPerTiming/100 * timing
               + lean bonus - rich penalty)
          * (engineDamaged ? 0.85 : 1)
torque    = baseline.torqueNm * powerMult
curve     = camshaft.curve (or baseline.curve), turbo topEndBias re-shapes the top half
mass      = baseline.massKg + weight.massDelta
grip/suspension/brakes -> straight into WheeledTuning
```

## Audio (documented route)

Only `assets/audio/vehicles/engine_loop.wav` ships in-repo, so exhaust NOTE
variants are pitch/timbre variants of that loop: each exhaust tier's `noteId`
selects a documented `pitchOffset` + `timbre` shaping applied to the live
`setLoopParams` ramp (deeper idle, raspier top). Supercharger whine = the same
loop as a second voice at 2.4x+ pitch, volume gated on throttle. Turbo = a third
voice whose pitch/volume ride the spool state; lifting the throttle above 60%
spool fires a one-shot blowoff chirp (the loop played 1-shot at 4.2x pitch). If
distinct loops land in `assets/audio/vehicles/` later, `noteId` maps to files
instead (no format change).

## Persistence

`VehicleBuild` (installed parts, ECU tune, credits, damage, nitrous tank) saves to
`vehbuild.json` beside the engine's checkpoint saves (same dialog-flags-adjacent
pattern: tiny JSON, versioned by the same `format` tag, gracefully rejected on
mismatch). Loaded on `--world drive` boot; saved on every shop transaction + exit.
