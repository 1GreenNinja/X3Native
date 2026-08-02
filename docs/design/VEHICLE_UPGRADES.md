# Vehicle Upgrade System — Full Outline

X3Native / EFLZ. Status: **performance layer SHIPPED, cosmetic layer NOT BUILT.**
Companion to `docs/design/VEHPARTS_FORMAT.md` (the data format) and `app/vehparts.{h,cpp}` (the implementation).

Reference points: the depth target is Forza/GT-style *mechanical* tuning (which we already exceed most arcade racers on), with NFS Underground 2 / Carbon-style *visual* customization (which we have none of) and Hot Pursuit-style *feel* (which is a handling layer, not an upgrade layer — see §5).

---

## 1. WHAT ALREADY EXISTS (do not rebuild)

`assets/vehicles/parts.json` (format `x3.vehparts/1`) — **11 categories, 36 parts**, each with real stat semantics, parsed by `vehparts::Catalog`, composed by `compose()` into an engine-layer `x3::phys::WheeledTuning` and lowered onto the LIVE Jolt vehicle via `DriveDemo::applyTuning`. All headless-testable (`--test-vehparts`).

| # | Category | Parts | Real stats modeled |
|---|---|---|---|
| 1 | **camshaft** | cam_street, cam_sport, cam_race | replaces the normalized torque curve (`camCurve`), `redlineBonus` rpm |
| 2 | **exhaust** | catback, turboback, titanium, sidepipe | `powerPct`, plus audio: `noteId`, `pitchOffset`, `timbre` |
| 3 | **intake** | panel, cai, itb | `powerPct` |
| 4 | **intercooler** | stock+, fmic, race | `safeBoostBonus` (bar added to the ECU knock threshold) |
| 5 | **forced_induction** | sc_roots, sc_centri, turbo_small, turbo_big | `fiType`, `boostPowerPct`, `spoolLagS`, `topEndBias`, `whine` (SC), `whistle`+blowoff (turbo) |

> **GAP — add `fi_sc_twinscrew`.** The catalog has Roots, centrifugal and two turbos but no **twin-screw** positive-displacement blower, which is the "best of both" tier and the obvious premium SC. All the stat fields already exist; no format change needed:
> `fiType: "supercharger"`, `boostPowerPct` high (above Roots), `spoolLagS: 0.0` (positive displacement = boost from idle, no lag), `topEndBias` low-to-mid (flat torque from low rpm — the opposite of a big turbo), `whine: true` (the signature sound; a distinct `noteId`/`timbre` from the Roots whine is worth authoring), `tier: 4`, priced at/above `fi_turbo_big`.
> With it, the FI decision space is complete and each option is a real character: **Roots** cheap/instant/heat-limited · **twin-screw** instant *and* efficient, expensive · **centrifugal** turbo-shaped curve, belt-driven · **small turbo** quick spool · **big turbo** monster top end, laggy.
> *Naming note:* model the technology, not a brand. Real blower manufacturers are live trademarks and racing games license them; ship an original name (or license properly if real brands are ever wanted).
| 6 | **ecu** | piggy, flash, standalone | `maxBoost`, `safeBoost`, `safeLean`, `safeTiming`, `knockLimit`, `powerPerBoost`, `powerPerTiming`, `leanPowerPct`, `repairCost` |
| 7 | **tires** | touring, summer, semislick, slick | `gripScale`, `compound` |
| 8 | **suspension** | lowering, coilover_s, coilover_r | `rideHeightDelta`, `suspFreq`, `suspDamp` |
| 9 | **brakes** | pads, bbk4, carbon | `brakeTorque` |
| 10 | **weight** | interior, carbon, full | `massDelta` (kg, negative) |
| 11 | **nitrous** | nos_50, nos_100, nos_200 | `nitrousMult`, `tankSeconds`, `refillCost` |

Also shipped: **`EcuTune`** live sliders (boost bar / fuel mixture / timing advance) with **knock modeling** — push past `safeBoost`/`safeLean`/`safeTiming` and knock builds until `knockLimit` → **LIMIT-POP**, engine damaged (×0.85 power) until paid `repairCost`. **`VehicleBuild`** persists installed parts + tune + credits + damage + nitrous tank as JSON beside checkpoint saves. A **dyno** samples the torque/power curve. Baseline stock car: 700 Nm, 6500 rpm, 1300 kg, 2200 Nm brakes.

**Verdict:** the mechanical half is deeper than any NFS. It does not need expanding to be good — it needs the *cosmetic* half and a *shop UX*.

---

## 2. NEW: COSMETIC / VISUAL CATEGORIES (the gap)

The whole "drive in, build your car" fantasy is currently invisible — you can build a 900 hp car that looks stock. These categories are new and mostly *render-side*, so they cost art + shader work, not physics.

### 2.1 paint
The headline. Model as a **material override** on the body mesh, not a texture swap.
- `paintType`: `solid` | `metallic` | `pearlescent` | `matte` | `chrome` | `candy`
- `baseColor` (RGB, player-picked from a wheel — not a fixed list)
- `flakeAmount`, `flakeColor` — metallic sparkle (a second specular lobe)
- `pearlShift` — hue shift by view angle (Fresnel-driven; this is what makes pearlescent read)
- `clearcoat`, `clearcoatRoughness` — the deep-gloss layer. **This is the single most important parameter**; a car reads as "expensive" almost entirely through clearcoat response.
- Cost tiers: solid cheapest → candy/pearlescent most expensive.
- *Tech note:* needs a clearcoat term in the car material (a second specular lobe over the base BRDF). Pairs with **RT reflections** — a mirror-finish paint job is exactly the case screen-space reflections fail on.

### 2.2 lighting
Both cosmetic and functional, and it directly showcases **clustered lighting**.
- **Headlights**: `halogen` | `hid` | `led` | `laser` — color temperature (warm 3200K → cold 6000K), intensity, beam cone angle, throw distance.
- **Underglow**: color (player-picked), intensity, `mode`: static | pulse | strobe | reactive-to-rpm. *Underglow is the NFS Underground signature and is nearly free once clustered lighting lands.*
- **Interior/dash glow**, **neon washer jets**, **light bars** (for the pursuit fantasy).
- **Taillight/brake style**: strip, halo, sequential-blink.
- *Tech note:* every one of these is a small dynamic light. Under today's 64-light scene cap a single tricked-out car could eat the budget. **Blocked on Lane 2 (clustered lighting).**

### 2.3 wheels
- Rim model (Meshy-generated set), diameter (17"–22"), width, offset.
- Rim finish: painted / polished / chrome / anodized / two-tone — reuses the paint material params.
- Brake caliper color (visible through spokes — a classic detail; ties to the brakes category).
- *Physics link:* wheel diameter should feed the existing suspension/ride-height math so a visual choice has a real consequence.

### 2.4 body
- **Bumpers** (front/rear), **side skirts**, **spoilers/wings**, **hoods** (incl. vented/scooped), **diffusers**, **splitters**, **fender flares**, **roof scoops**, **exhaust tips** (visible, tied to the exhaust part).
- *Physics link (optional, do later):* a real wing adds downforce — a `downforceDelta` stat would make body parts more than cosmetic. Note this is the one physics param the current system lacks entirely.
- *Tech note:* attachment sockets on the car mesh; each part is a swappable child mesh. Needs a socket convention in the vehicle GLB.

### 2.5 vinyls / livery
- Layered decal system: base layer + N stacked decals, each with position/rotation/scale/color/opacity/mirror.
- Player-composable (the Underground 2 fantasy) or preset packs (much cheaper to ship).
- *Tech note:* decal projection onto the body, or a paint-layer render-to-texture. **Start with preset liveries** — a full layered vinyl editor is its own multi-week project.

### 2.6 window tint
- Cheap, visible, high satisfaction-per-line-of-code. Tint darkness + optional color. Feeds the existing glass material.

---

## 2.7 RARE / LEGENDARY PARTS (new tier concept)

Parts that are **found, won, or story-gated** rather than purchased — and which change *how a system behaves* rather than just moving a number. These are the memorable ones.

**Flagship example — an active knock-control box.** (Inspired by the real J&S Vampire, a per-cylinder knock-detection unit that automatically retards timing on detonation. **Ship it under our own name** — the real product is a live trademark; call it e.g. "Revenant Knock Control" or similar and describe the function, not the brand.)

Why it's the perfect rare part *for this game specifically*: we already model knock (`safeBoost`/`safeLean`/`safeTiming` → knock index → `knockLimit` → LIMIT-POP → engine damage). A knock-control box doesn't add power — **it deletes an entire risk axis.**

REAL BEHAVIOR (match it): it is CLOSED-LOOP and per-cylinder. You may advance timing as far as you like; the box listens for detonation and pulls *exactly enough* timing, continuously, to never ping. It does not "reduce the chance" of knock — it makes knock-from-timing structurally impossible.

Proposed model:
- While installed, the **timing slider can be maxed with zero pop risk** — the box clamps the knock index below `knockLimit` automatically, every tick.
- It retards only as much as conditions demand, so on a good day (cool air, good fuel) you keep nearly all your advance; on a bad day it quietly costs you power instead of costing you an engine.
- **The trade is not a flat power tax** — it is that a *perfect manual tune still beats it by a hair*, because the box reacts to knock rather than preempting it. Expert players who nail the tune by hand get slightly more; everyone else gets ~95% of a perfect tune with none of the risk. That is a genuinely good risk/skill tradeoff and it is exactly how the real part behaves.
- It does NOT protect against the other knock sources — running lean (`safeLean`) or over-boosting past what fuel supports still bites. So it removes *one* axis, not all of them, and the tune bench stays interesting.
- Per-cylinder knock readout on the tune bench UI — turns an abstract gauge into a diagnostic instrument, and visibly shows the box working (cylinder 3 always the first to complain, etc.). Great texture.

Because it removes a whole failure mode, it should be genuinely RARE — found/won/story-gated, not purchasable.

Design rule for the rare tier: **a rare part should alter a rule, not a number.** Other candidates in that spirit — anti-lag (turbo stays spooled off-throttle, at the cost of engine wear), a sequential/dog box (faster shifts, brutal on the drivetrain), active aero (downforce that changes with speed/braking), a limited-slip diff with adjustable lock, water/meth injection (raises the effective safe-boost threshold, consumable tank).

---

## 3. SHOP UX ("drive in, build your car")

The parts exist but there's no place to spend credits. Needed:
1. **The garage/shop location** — a world hub you drive into. Ties to the city generator (Lane 4): a shop is a *lot* with frontage on a street.
2. **Category browser** → part list with **stat deltas shown against currently installed** (▲ +42 hp, ▼ −80 kg). Never show raw numbers alone; show the *change*.
3. **The dyno** — already modeled. Make it a *scene*: strap the car down, run it, watch the power curve draw itself, hear the note. Knock/LIMIT-POP is a real risk and a real drama beat.
4. **The tune bench** — live boost/fuel/timing sliders with a knock gauge. The tension of creeping toward `knockLimit` is a genuinely good minigame that no NFS has.
5. **Test drive** — apply and immediately drive without leaving.
6. **Visual bay** — paint/wheels/body/vinyl preview on a turntable under controlled lighting (an RT-reflections showcase; light it like a car-configurator).

---

## 4. PROGRESSION

- **Credits** already exist in `VehicleBuild`. Sources: races, pursuits, bounties, story beats, found stashes.
- **Tier gating**: tier 1–4 parts already carry a `tier`. Gate tiers behind progression so a tier-4 turbo isn't purchasable in hour one.
- **Part condition/wear** (optional, later): engine damage already exists; extend to tire wear and brake fade for endurance events.
- **Multiple owned vehicles** — `VehicleBuild` is per-car today; needs a garage collection.

---

## 5. HANDLING FEEL (separate from upgrades — noted so it isn't conflated)

Upgrades change *numbers*; feel is a different layer. Current state is "grippy arcade RWD" on Jolt's simulation-oriented wheeled controller (front steered, rear powered + handbrake). To reach NFS Hot Pursuit feel:
1. **Drift layer** — cut rear lateral grip above a slip-angle threshold, add counter-steer assist + auto-stabilization so slides are *catchable*. This is the single biggest feel change, and it's a tuning layer over the existing controller.
2. **Speed sensation** — FOV punch, motion blur, screen shake, edge distortion, all driven by velocity. The post stack exists; it isn't wired to speed.
3. **Pursuit** — cop AI, heat levels, roadblocks, spike strips. Nothing exists; this is the biggest net-new system.
4. **Surface response** — grip/particle/audio per surface (asphalt/gravel/wet). Partially there via tire compound.

---

## 6. BUILD ORDER (recommended)

1. **Shop UX + dyno + tune bench** — makes the 36 shipped parts *reachable*. Highest value-per-effort by far; the system already works and nobody can touch it.
2. **Paint + window tint** — biggest visual payoff per line; clearcoat is the key parameter.
3. **Wheels** — high visibility, simple mesh swap + the paint material.
4. **Lighting** — *after* clustered lighting (Lane 2) lands; underglow is the signature look.
5. **Body kit** — needs a socket convention + Meshy art; add `downforceDelta` here.
6. **Vinyls** — presets first; a layered editor only if it earns its cost.
7. **Drift layer** (feel) — can run in parallel; it is independent of all the above.

---

## 7. FORMAT EXTENSION NOTES

The cosmetic categories do **not** fit the existing `Part` struct cleanly (it is a pragmatic union of *performance* stats). Recommend a sibling block in `parts.json`:

```
{ "format": "x3.vehparts/1",
  "baseline": {...},
  "categories": [...],          // performance (existing, unchanged)
  "cosmetic": [                  // NEW
    { "id": "paint", "label": "Paint", "kind": "material", "parts": [...] },
    { "id": "wheels", "label": "Wheels", "kind": "mesh_swap", "socket": "wheel", "parts": [...] },
    { "id": "body",  "label": "Body Kit", "kind": "mesh_attach", "parts": [...] }
  ] }
```
Keeping cosmetics in a separate block preserves the existing parser, keeps `compose()` (which targets `WheeledTuning`, a physics POD) untouched, and lets a second `composeVisual()` produce a render-side `VehicleAppearance`. Bump to `x3.vehparts/2` only if the performance block itself must change.
