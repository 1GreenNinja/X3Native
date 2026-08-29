# Why Three Render Surveys Missed Motion Blur

**Written 2026-08-28.** A forensic post-mortem, not a feature plan. The companion
document is `RENDER_FRAMEWORK_GUIDE_2026-08.md`, which contains the replacement survey.

---

## 0. The provoking fact

X3Native computes a **full per-pixel screen-space velocity buffer** every frame:

- `shaders/velocity.vert` / `shaders/velocity.frag` — a dedicated pre-pass writing
  `prevUV - curUV` into an RG16F target, with per-frame jitter removed from *both*
  endpoints so the vector is true surface motion.
- `engine/rhi/vk/vk_gi_rt.cpp:244-422` — pipeline, descriptor layout, velocity UBO ring,
  and a **per-object previous-model-matrix SSBO ring**, so skinned and dynamic geometry
  reprojects on its own motion rather than the camera's.
- `engine/rhi/vk/vk_graph.cpp:598-652` — the pass, slotted between the depth pre-pass and
  the main opaque pass.
- `engine/rhi/vk/vk_passes.cpp:1993-2025` — the `velWant` gate, the `VelUBO` fill with
  unjittered current/previous view-projections, and the per-frame jitter lanes.
- `engine/rhi/vk/vk_passes.cpp:2706-2713` — the prev-model SSBO fill, gated by
  `m_velActiveThisFrame` so non-velocity frames pay nothing.

That buffer has **exactly one consumer**: `shaders/taa_resolve.frag`, which reads it at
`binding = 4` and uses it when `params1.z` (velocityValid) is set.

Per-pixel velocity is the single most expensive prerequisite of motion blur, and it is the
*only* prerequisite that is hard. It was built, verified, shipped green, and documented
(`docs/VELOCITY_DLSS_REPORT.md`). And then no motion-blur pass was written — not because
it was considered and deferred with reasons, but because **no survey in the tree was
capable of noticing that a built input had an unbuilt consumer.**

Three documents exist whose stated job was to notice exactly this:

| Document | Date | Shape |
|---|---|---|
| `docs/design/RENDER_QUALITY_UPLIFT.md` | ~2026-06 | 17 named "dimensions", ranked impact/effort |
| `docs/design/RENDER_FIDELITY_GAPS_PLAN.md` | 2026-06-06 | 4 named "GAPs" + a "beyond the four" list |
| `docs/design/RENDER_GAP_ROADMAP.md` | 2026-06-11 | 8 ranked rows, fleet lane assignments |

None of them produced a motion-blur work item. The rest of this document explains why, and
the explanation is **not** "they forgot."

---

## 1. The finding that reframes everything: it was never actually missed

Grepping the whole `docs/` tree for motion-blur language returns four hits. Three are real,
and all three are **correct**:

**(a) `docs/design/RENDER_GAP_ROADMAP.md:35`** — it is *in* the survey, at rank 8 of 8:

> `| 8 | **Decals / motion blur / DoF / physical sky** | Polish tier; each small, none load-bearing. |`

**(b) `docs/design/VEHICLE_UPGRADES.md:130`** — a precise, accurate diagnosis:

> "**Speed sensation** — FOV punch, motion blur, screen shake, edge distortion, all driven
> by velocity. The post stack exists; it isn't wired to speed."

**(c) `docs/design/RACING_WORLD.md:642`** — someone actually went and checked the shader
directory:

> `| Motion blur | **no shader exists** (shaders/ has no motion-blur pass) | The one missing
> *speed-sensation* effect; VEHICLE_UPGRADES.md:118 lists it |`

So the repository **knew**, three separate times, in three separate voices, that motion blur
was absent and wanted. The knowledge existed. It simply never reached a survey, a lane, or a
punchlist.

This is a much more useful failure than forgetting, because forgetting is fixed by trying
harder and this is not. **The defect is in how findings are routed and re-evaluated, not in
how thoroughly the tree was read.** Five distinct structural mechanisms produced it.

---

## 2. Mechanism 1 — The taxonomy had rows for features, not for data

All three surveys are organised as a **list of named effects**. `RENDER_QUALITY_UPLIFT.md`
has 17 named dimensions; `RENDER_FIDELITY_GAPS_PLAN.md` has four named GAPs;
`RENDER_GAP_ROADMAP.md` has eight named rows. Every row answers the same question:

> *Does the engine have feature X?*

That question has no way to express the actual state of motion blur, which is:

> *A resource that cost a pipeline, a descriptor set, two UBO rings, an SSBO ring and a
> graph pass to produce is being read by one consumer when it could serve three.*

There is no row in a feature list for "a built input with a missing consumer." The finding
is a property of the **dependency graph between passes**, and none of the three documents
contains a dependency graph. `RENDER_GAP_ROADMAP.md` comes closest — line 29 explicitly
notes that "DLSS rides the TAA plumbing (jitter + motion vectors + history)" — but it draws
that arrow *forward from* TAA to one chosen successor and stops. It never asks the reverse
question: **once TAA lands, what else does its plumbing now make cheap?**

A survey that indexes by feature will always be blind to a shared-resource opportunity,
because the opportunity is not located in any feature. It is located between two of them.

**How the reference engine treats the same relationship.** Unreal's own architecture makes
the coupling explicit: its temporal upscaler is permitted to emit the motion-blur velocity
derivatives as a by-product of its own velocity dilation, and motion blur consumes them
rather than recomputing
(`Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalSuperResolution.cpp:2546-2559`,
consumed via `PostProcess/PostProcessMotionBlur.h:57-70` and gated by an "allow external"
switch at `PostProcessMotionBlur.cpp:61`). Motion blur is likewise the pass that produces
the half- and quarter-resolution scene-colour slices the bloom/DOF chain then consumes
(`PostProcess/PostProcessing.cpp:~1271`). In the reference engine these are not four
independent features that happen to sit near each other; they are **one temporal subsystem
with shared intermediates.** Our surveys split that subsystem across ranks 2 and 8 of the
same table and then declared the strategy stopped at rank 4.

---

## 3. Mechanism 2 — The ranking was computed once and never recomputed

`RENDER_GAP_ROADMAP.md:37-38` states the strategy plainly:

> "**Strategy:** 1 → 2 → 3 → 4 is ≈80% of the perceived gap for ≈20% of the effort"

Rank 2 was **TAA → DLSS**. Rank 8 was the polish bucket containing motion blur. The
strategy sentence declares ranks 5–8 out of scope *by construction*.

Then rank 2 shipped. TAA landed, jitter landed, and with it the velocity buffer, the
prev-model SSBO, and the unjittered matrix pair. **The cost of rank 8's motion-blur entry
collapsed at that moment** — from "build per-pixel velocity infrastructure, then build a
blur" to "write one resolve pass against a buffer that already exists and is already
correctly gated."

Nothing recomputed the ranking. The table was a **static artifact**: its effort column was
estimated against the engine as it stood in June, and it was never re-derived against the
engine as it stood in August. A ranked list whose numbers are stale is worse than no list,
because it carries the authority of having been thought about.

This is the single most generalisable lesson here. **Any survey that ranks by
impact-per-effort must be re-run whenever a prerequisite lands**, because landing a
prerequisite is precisely the event that invalidates the effort column for everything
downstream of it. The surveys had no re-run trigger, no expiry, and no dependency links
that would have identified which rows to re-price.

---

## 4. Mechanism 3 — The verification instrument is structurally blind to the motion domain

This is the deepest mechanism, and it is the one that guarantees the same class of miss will
recur unless it is fixed.

Every verification gate in the tree is a **still-frame comparison**:

- `docs/plans/PUNCHLIST_GTA6_MATCH.md:77-79` — "Every item gates on an A/B capture against a
  named reference frame, read at full res, before it is called done (capture-review law)."
- `docs/plans/PLAN_GTA6_CAMPAIGN.md:81-85` — "every item gates on an A/B against a named
  reference frame."
- `docs/design/RENDER_FIDELITY_GAPS_PLAN.md:102-107` — "Headless `--screenshot-showroom*`
  (DAY + NIGHT) A/B before/after."
- The receipts in `PLAN_GTA6_CAMPAIGN.md` are **dB deltas** — "40 dB long views / 56 dB
  close", "24.8/27.6 dB from default" — computed between two of our own captures on a
  *static camera*.
- `docs/design/RENDER_FIDELITY_GAPS_PLAN.md:91` concedes it outright: "SSAA-stills only today."

Three compounding consequences:

**(i) A still-camera dB metric cannot see a motion-domain effect.** Motion blur on a static
camera with a static scene is mathematically the identity function. Adding it would register
a 0 dB delta — indistinguishable from "no change," and under a regression-oriented reading,
indistinguishable from "no bug." The instrument literally cannot fail on this absence.

**(ii) The reference material was a video, and it was decomposed into stills.** The GTA VI
punchlist derives from a 2:21 gameplay clip but gates on numbered frames (f015, f045, f070).
A frame extracted from real gameplay footage **already has the reference's motion blur baked
into it**. Under still-vs-still comparison, that baked-in blur does not read as "they have an
effect we lack"; it reads as softness, compression artefacting, or low source quality — a
property of the *capture*, to be mentally discounted, rather than a property of the *renderer*,
to be matched. The method converts a temporal signal into a per-pixel sharpness signal and
then discards it as noise.

**(iii) "Read at full res" actively inverts the judgement.** The capture-review law's
emphasis on full-resolution reading biases every comparison toward sharpness and detail
density. Against a motion-blurred reference still, our sharper frame can *score better*. The
gate does not merely fail to detect the gap — under adversarial reading it rewards having it.

The same inversion is visible elsewhere in the tree: `docs/design/TOWN_MANIFEST.md:405`
records blurred tarmac in a capture as a **defect** ("02 gave 45% of the frame to blurred
tarmac"). That is the correct call for that shot, but it shows the house style treats blur as
something to be eliminated, which is not a neutral prior when the question is whether to add
a blur pass.

---

## 5. Mechanism 4 — The framing statement excluded the axis before the search began

`docs/plans/PLAN_GTA6_CAMPAIGN.md:5-6` opens with the campaign's governing thesis:

> "**we are not missing systems, we are missing integrity and density.**"

and line 10-11 names the three axes of the gap:

> "GTA VI beats us today on: material/lighting integrity, content density per square meter,
> and reaction polish."

Both of the first two axes are **fully observable in a single frame**. Material integrity is
per-pixel. Content density is per-square-metre. Neither has a time component. Having fixed
the search axis to two still-observable quantities, everything downstream searched only
along it — and produced nine rendering items (G1-G9), of which the only blur-adjacent one is
**G9, "UI depth-of-field"**, justified explicitly by "(their radio-wheel shot)". That is a
full-screen gaussian behind a menu, wanted because it was visible *in a still*. There is no
in-world depth of field, no focus distance, no aperture, and no motion-domain item anywhere
in either GTA VI document.

Note also what the campaign's "what we have" inventory
(`PLAN_GTA6_CAMPAIGN.md:4-12`) enumerates: traffic, water, weather, day/night, weapons,
vehicles, interiors, map, radio, missions, NPCs, destruction, RT. **Not one post-process or
temporal effect appears.** The inventory was taken at the granularity of gameplay systems,
so the post stack was never enumerated — and a gap cannot be found in a list that was never
written.

---

## 6. Mechanism 5 — Findings were filed by owning domain, not by pipeline stage

The two accurate diagnoses live in:

- `docs/design/VEHICLE_UPGRADES.md:130` — under `## 5. HANDLING FEEL`, in a vehicle-upgrades
  design document.
- `docs/design/RACING_WORLD.md:642` — in a racing-world dependency table headed *"Everything
  else on the roadmap is a multiplier, not a blocker."*

Both are correct. Both are unreachable. An agent surveying the renderer reads `docs/design/RENDER_*`
and `docs/plans/`; it does not read a vehicle-upgrade document. The finding was filed
according to **who noticed it** (the vehicle lane, the racing lane) rather than **what part
of the pipeline it belongs to** (post-process, temporal domain).

Worse, `RACING_WORLD.md:642` is not just a mention — it is a *verified negative*, someone
having grepped `shaders/` and confirmed no motion-blur pass exists. That is the highest-value
form of survey evidence there is, and it was immediately neutralised by the table header
declaring the whole table "not a blocker."

**A related drift worth flagging while we are here.** The same `RACING_WORLD.md` table lists
"CSM / cascades — **not implemented**" and "Clustered lighting — not implemented, 64-light
forward cap." Both are now false. CSM is present as a 4-cascade 2D-array path with a blend
band (`shaders/mesh.frag:81-95`, `r_csm`), and clustered/froxel lighting is present
(`shaders/inc/mesh_lighting.glsl:5-14`, `engine/rhi/ClusterLights.cpp`, `r_clusterlights`).
Stale "not implemented" claims are the mirror image of the motion-blur miss and cost the
same thing: attention spent in the wrong place. Any survey that does not re-verify against
the tree inherits every stale claim it reads.

---

## 6a. This has happened before, and the tree already recorded it

The strongest evidence that this is a *class* of failure rather than a one-off is that the
repository contains a near-identical case, written up by whoever found it, in
`docs/KNOWN_BUGS.md` under **L10**:

> "The DEFAULT camera far plane is **200 m** — and it was hiding an entire mountain range. […]
> The showroom's Unity terrain is 6.6 km across and 780 m tall — the snowy peaks the reference
> is famous for were IN the asset, fully imported, and simply clipped. **Four art passes chased
> 'flat, empty horizon' as a *content* problem.** If an outdoor world looks like it ends in fog
> 200 m out, call `setCameraFar()` before you touch the art."

Structurally identical to motion blur:

| | Far plane (L10) | Motion blur |
|---|---|---|
| The expensive thing was already built | the terrain was imported, 6.6 km of it | the velocity buffer, full-res, per-object |
| A single cheap setting stood between it and being used | `setCameraFar()` | one resolve pass |
| The gap was invisible to the verification method | a still capture shows a horizon; it cannot show that something is *behind* the far plane | a still capture cannot show a missing temporal effect |
| It was repeatedly misdiagnosed along the framing axis | "flat empty horizon" read as a *content/density* problem — four times | motion blur triaged as "polish tier, none load-bearing" |
| The correct diagnosis, once found, was one line | far plane default | missing consumer |

Note especially the third and fourth rows. In both cases the team was **looking hard, in good
faith, four separate times**, and the search axis (content density) plus the instrument (a
still frame) guaranteed they would look past the actual cause. Effort was never the missing
ingredient.

L10's closing advice — *check the systemic cause before you touch the art* — is the same
lesson as this document's, arrived at independently from the opposite direction. It belongs in
the survey method, not buried in a bug list.

---

## 7. The structural blind spot, stated in one sentence

> **The surveys indexed the renderer by feature name and verified it with still frames — so
> they could only find gaps that are both nameable as a feature and visible in a frozen
> image, and motion blur is neither: it is a missing consumer of an existing resource, whose
> entire visible effect lives in the time domain.**

Every one of the five mechanisms is a corollary of that sentence. Feature-name indexing
produced Mechanisms 1 and 5 (no row for a data relationship; filing by owner instead of by
stage). Static ranking produced Mechanism 2. Still-frame verification produced Mechanisms 3
and 4 (the instrument cannot fail on it; the framing excluded the axis).

---

## 8. What this dictates about the replacement survey

The companion document is written to be structurally incapable of the same miss. Four
constraints follow directly from the five mechanisms, and each is testable:

**C1 — Index by pipeline stage, not by feature name.** Every row belongs to a stage of the
frame (geometry/visibility → shadowing → direct lighting → indirect/GI → reflections →
transparency → volumetrics → temporal → post → UI composite). A missing consumer of an
existing buffer then has an obvious home: it is a stage that reads a resource, and the
resource inventory is enumerated separately from the feature inventory. *(Fixes Mechanism 1.)*

**C2 — Enumerate resources and their consumers explicitly, and flag every producer whose
consumer count is one.** A resource with a single consumer is a standing question, not a
finished state. This is the check that would have caught motion blur in June, and it is
mechanical enough to run as a lint. *(Fixes Mechanism 1 and re-triggers Mechanism 2.)*

**C3 — Price effort against the tree as it is today, and mark rows whose inputs already
exist as a distinct high-leverage class.** "Inputs present, consumer absent" is its own
category and outranks anything requiring new infrastructure at equal impact. Any re-run of
the survey re-prices; the effort column has no permanent value. *(Fixes Mechanism 2.)*

**C4 — Motion-domain items require a motion-domain gate.** A still capture cannot verify
motion blur, temporal stability, disocclusion behaviour, ghosting, or history rejection.
These need a **camera-on-rails frame sequence** — a fixed spline flown at a fixed timestep,
captured as N sequential frames, compared as a sequence. This is a small tooling addition
(the headless screenshot rig already exists; it needs a deterministic multi-frame mode), and
without it the tree cannot honestly claim any temporal feature works. *(Fixes Mechanisms 3
and 4; note this is itself a delta and appears in the replacement survey.)*

A fifth constraint follows from Mechanism 5 and is process, not document structure:

**C5 — A render finding discovered in a non-render document must be routed to the render
survey.** `RACING_WORLD.md:642` and `VEHICLE_UPGRADES.md:130` were correct and orphaned. The
cheapest fix is a periodic tree-wide grep for render vocabulary outside `docs/design/RENDER_*`,
folded into whatever re-runs the survey.

---

## 9. Honest scope of this analysis

- The claim "three surveys missed motion blur" is, strictly, **false as stated**:
  `RENDER_GAP_ROADMAP.md` contains it at rank 8. The accurate claim is that all three failed
  to *re-price* it after its prerequisite landed, and that two of them omit it entirely.
- I have not audited whether the June effort estimates were reasonable **at the time**. They
  probably were. The failure is the absence of a re-run, not the original judgement.
- The dB-receipt criticism applies to the *motion domain specifically*. For the material and
  density axes the campaign actually targets, still-frame dB A/B is a reasonable and
  well-disciplined instrument, and the Phase-0 receipts it produced are real. The problem is
  that a good instrument for one axis was treated as the gate for all axes.

---

## Provenance

Unreal Engine 5.8 source on this machine was read **for architectural understanding only**.
No Epic code, shader text, or comments were copied into this document or into X3Native.
Unreal file paths appear as reference pointers so a reader can find the same material; the
descriptions of pass ordering and resource sharing are written from scratch in our own words.
