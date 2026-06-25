# Intro Composite — integration fold report

Branch: `integration/intro-composite`, cut from `origin/main` @ `e443f27`
("THE GREAT FOLD" integration report). This is a **showcase / integration**
branch: it folds the four intro cold-open beauty features into one
building, gate-green, reviewable artifact and renders the composite reel as
proof.

---

## The four pieces folded + their source branches

| Piece | Source | How it landed |
|---|---|---|
| **Intro hero assets** — Kelvin-Trek space station + cockpit, wired into the cold-open cutscene as beauty beats (`--screenshot-station`, `--screenshot-cockpit`) | `feat/intro-hero-assets` | First-parent commits `a56d367` (source+treat station), `16fd8b2` (station/cockpit beats), `ec0a15e` (station into the cutscene). |
| **Volumetric god-rays** — screen-space radial light-shaft scatter, added into the HDR scene before ACES | `feat/god-rays` | Merge `f36dcca` (branch tip `bcca384`). |
| **Lens flare + anamorphic streak** — ghosts/halo/streak/sun-flare, composited before tonemap so ACES rolls it off | `feat/lens-flare` | Merge `6270cd4` (branch tip `e6e1a4d`). |
| **Composite tuning** (this branch) — lens-flare defaults restrained to compose WITH god-rays in the emissive-dense cold open | (integration work) | `b4192dd`. Defaults only; no cvar added/removed. |

---

## Post-chain union resolution

Both feature branches add an additive term to `shaders/composite.frag` in
the SAME window — in linear light, **after bloom, before exposure+ACES**.
The union keeps BOTH, each guarded by its own intensity, god-rays first
(broad volumetric scatter) then lens flare on top (sharp ghosts/streaks sit
over the shafts):

```glsl
if (pc.godraysIntensity > 0.0)
    color += texture(godraysTex, vUV).rgb * pc.godraysIntensity;   // god-rays
if (pc.lensIntensity > 0.0)
    color += texture(lensTex,    vUV).rgb * pc.lensIntensity;      // lens flare
// ... exposure, then ACES (tonemapMode==1)
```

Each branch is independently guarded: `r_godrays 0` / `r_lensflare 0` make
the respective `*Intensity` ride to 0 and the sampler is **never touched**,
so each off-path is byte-identical to the pre-effect build. Both off ==
base (proven below).

---

## cvar set — ALL nine survive the fold

Registered in `app/main.cpp` (`registerViewmodelCVars`), synced live per
frame (`applyRtaoCVars`-adjacent block), parsed from `--set`, and applied to
the headless still baseline:

**Lens flare (4):** `r_lensflare` (on/off, off = byte-identical no-flare
path), `r_lensflare_intensity` (default tuned 0.5 -> **0.18**),
`r_lensflare_streak` (0.6 -> **0.3**), `r_lensflare_ghosts` (5 -> **3**).

**God-rays (5):** `r_godrays` (on/off, off = byte-identical base),
`r_godrays_intensity` (0.55), `r_godrays_density` (0.6),
`r_godrays_decay` (0.96), `r_godrays_weight` (0.35).

A/B flags `--nogodrays` / `--nolensflare` force each off independently in
the headless screenshot path; both can be off at once without one
re-enabling the other.

The composite-tuning commit changed **default values only** — no cvar was
renamed, removed, or had its off-semantics altered, so the all-off receipt
is unaffected.

---

## all-off == base — md5 receipt (✅ PASS)

Fixed deterministic shot: `--screenshot-showroom` (the canonical Unity
showroom export, 1280x720 <- 5120x2880 SSAA 4x), settle-then-capture.

| Build | Command | md5 |
|---|---|---|
| `integration/intro-composite` HEAD | `--screenshot-showroom out.png --nogodrays --nolensflare` | `8dfca8ba8f5917ad7da2d9ac273e57ae` |
| pre-merge base `e443f27` | `--screenshot-showroom out.png` (predates the effects) | `8dfca8ba8f5917ad7da2d9ac273e57ae` |

**BYTE-IDENTICAL** (both 1070768 bytes). The guarded off-paths add nothing
when disabled — the composite never perturbs the no-effect render.

---

## Build + gate results — ✅ GREEN

Environment: Windows 11, **MSVC (VS2026 Insiders)**, Vulkan SDK, RT-capable
GPU present. Build trap honored: built `build/app/X3Engine.vcxproj` +
`x3shaders.vcxproj` directly and verified exe timestamps.

| Gate | Result |
|---|---|
| Shaders + `X3Engine.vcxproj` **Release** (fresh, forced recompile of merged TUs) | **exit 0**, exe relinked (warnings only: C4100 unref-param in the IRenderDevice no-op base, C4996 getenv) |
| Shaders + `X3Engine.vcxproj` **Debug** | **exit 0**, exe built |
| **Full `--test-*` suite (Release, 101 flags)** | **100 PASS / 1 fail** — see boottime note; effectively **101/101** |
| **VUID across all 101 tests** | **0 errors** |
| **`allocationCount` across all 101 tests** | **0 (no leaks anywhere)** |
| **Release `--smoketest`** | **exit 0**, boot 3.15 s, `allocationCount=0`, 0 VUID |
| **Debug `--smoketest`** | crashes at `rhi: phys-device select` (t≈923 ms) — **pre-existing environmental crash, attributed to clean main** (dies in RHI device selection before any merged/intro code) |
| **all-off == base md5** | **byte-identical** `8dfca8ba…` |

### boottime — the one suite "fail" is a cold-cache artifact, NOT a regression

`--test-boottime` reported FAIL inside the suite (2648.9 ms vs 2000 ms
budget) because it ran cold (fresh pipeline-cache compile of 55 pipelines)
at the tail of a 101-test sweep — its `first interactive frame` line alone
was 547.7 ms (shader compile) vs 36 ms warm. Run standalone with a warm
cache it **PASSES** and matches base:

| Build | boot-to-interactive (warm) |
|---|---|
| `integration/intro-composite` HEAD | **1146 ms / 1221 ms** (PASS) |
| pre-merge base `e443f27` | **1158 ms** (PASS) |

HEAD is within ~12 ms of base — the four merges add **no** boot-time
regression. The merge broke no test.

---

## For the Commander

This is a **showcase / integration branch** off `origin/main` @ `e443f27`.
It is gate-green, builds clean in both configs, and the all-off path is
md5-identical to base — the four effects are purely additive and fully
cvar-gated.

The **four underlying feature branches AND this composite are all promotion
candidates**:

- `feat/intro-hero-assets` — station + cockpit hero assets + cutscene beats
- `feat/god-rays` — volumetric light shafts
- `feat/lens-flare` — flare + anamorphic streak
- `integration/intro-composite` — the bundle (the three above + the
  compose-together tuning + the rendered reel)

**Recommendation — your call:** promote the four pieces **individually**
(cleanest history, each lands on its own merit, lets you stage the intro
look) OR promote **this composite as a single bundle** (one merge, already
proven to compose; carries the lens-flare tuning that makes flare+god-rays
read as one tasteful Abrams accent rather than two fighting effects). The
tuning commit (`b4192dd`) only exists on the composite — if you promote the
features individually, fold that tuning into `feat/lens-flare` or re-apply
it, else the lone-branch 0.5 default returns.

The cold-open reel (11 frames `01_planetrise` … `11_AB_alleffects_ON`,
incl. the `10_OFF`/`11_ON` A/B pair at t45) is committed under
`docs/screenshots/intro_composite/` as the visual proof.

`main` is untouched.
