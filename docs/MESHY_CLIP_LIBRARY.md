# Meshy Clip Library — portfolio-wide animation pipeline

**Status:** pipeline built and validated end-to-end except the paid bake step. **0 credits spent building it.**
**Balance at time of writing: 1985 credits** (not ~3136 — see [Corrections](#corrections-to-prior-assumptions)).

One sentence: **rig a character once (5 cr), bake a ≤20-clip role set onto it (3 cr/clip), then merge those clips onto any character in the portfolio for free, forever.**

---

## 1. The action catalog — SOLVED

`action_id` is an integer in `[0, 696]`. There is **no API that enumerates it**. Confirmed dead:

| Probe | Result |
|---|---|
| `GET /openapi/v1/animations/actions` | 404 |
| `GET /openapi/v1/animation/actions` | 404 |
| `GET /openapi/v1/animations/library` | 404 |
| `GET /openapi/v2/animations/actions` | 404 |
| `GET /openapi/v1/actions` | 404 |
| `GET /openapi.json` | 404 (no published spec) |
| `GET /openapi/v1/animations` | 200, but lists **your past tasks**, not the catalog |

**But the catalog is public** — as a server-rendered HTML table in the docs:

> https://docs.meshy.ai/en/api/animation-library

Meshy's own API error confirms this is the authoritative source. Posting a bogus id returns:

```
400 {"message":"Invalid action_id. See https://docs.meshy.ai/en/api/animation-library
     for available animation IDs."}
```

So we scrape it and check the result in:

```bash
python tools/meshy_scrape_actions.py     # -> tools/meshy_actions.json
```

**678 actions, ids 0–696** (19 ids are retired gaps), all `biped`. Breakdown:

| Category | n | Subcategories |
|---|---|---|
| WalkAndRun | 176 | Walking 87, Running 47, TurningAround 20, CrouchWalking 19, Swimming 3 |
| BodyMovements | 158 | Acting 68, HangingfromLedge 21, Climbing 18, Jumping 16, FallingFreely 14, PerformingStunt 11, VaultingOverObstacle 10 |
| DailyActions | 157 | Interacting 45, Transitioning 34, Idle 25, WorkingOut 14, PickingUpItem 12, LookingAround 11, Sleeping 10, Pushing 4, Drinking 2 |
| Fighting | 154 | AttackingwithWeapon 38, Punching 38, Transitioning 26, Blocking 18, CastingSpell 12, Dying 11, GettingHit 11 |
| Dancing | 33 | Dancing 33 |

Search it offline, free:

```bash
python tools/meshy_clip_library.py search reload
python tools/meshy_clip_library.py search walk --inplace-only
```

### The `_inplace` discovery — read this before picking locomotion

Meshy ships **90 clips twice**: a root-motion original and an `_inplace` twin (ids 601–696) with the root translation stripped. Every one of the 90 has a root-motion twin; there are no orphans.

**Use `_inplace` whenever the ENGINE drives movement** — character controller, navmesh, RTS steering. The root-motion originals translate the skeleton and will slide the mesh out of its collision capsule. All locomotion in the shipped role sets already uses the `_inplace` twin where one exists.

Not everything has one. Notably `Walking_Woman` (1), `Run_02` (14), `RunFast` (16) and `Idle` (0) do **not**. Where a twin was needed, the sets pick a clip that has one — e.g. forward walk is `Casual_Walk_inplace` (613), forward run is `run_fast_2_inplace` (658).

---

## 2. The 20-action cap is an architecture, not an annoyance

A model can only export the **last 20 actions** selected on it. So:

> **VARIANTS BY ROLE, MERGE BY CHARACTER-WHO-NEEDS-BOTH.**

Bake each role as its own ≤20-clip variant. Then fuse whatever a given character actually needs into one GLB locally with `tools/glb-merge-anims.mjs` — which is **free, offline, and has no cap**. A character who both fights and dances is a *merge*, not a second purchase.

The cap is enforced in code: `resolve()` refuses any role set over 20 clips.

---

## 3. Role sets (`tools/meshy_role_sets.json`)

Sets are authored **by name**; ids are resolved against `meshy_actions.json` at runtime, so no magic numbers are ever hand-typed. All 140 entries across 8 sets resolve cleanly.

| Set | Project | Role | Clips | Credits |
|---|---|---|---|---|
| `x3_npc_normal` | X3Native | NORMAL | 20 | 60 |
| `x3_fps_combat` | X3Native | FIGHTING | 20 | 60 |
| `x3_club_dancer` | X3Native | DANCING | 20 | 60 |
| `x3_club_crowd` | X3Native | NORMAL | 20 | 60 |
| `rf_sword` | Riftforged | FIGHTING | 20 | 60 |
| `rf_caster` | Riftforged | CASTING | 18 | 54 |
| `eos_rts_unit` | EmpiresOfShadow | FIGHTING | 12 | 36 |
| `eos_rts_worker` | EmpiresOfShadow | NORMAL | 10 | 30 |
| **ALL** | | | **140** | **420** (+5/character rigged) |

420 credits of 1985 buys the entire portfolio's motion vocabulary once. Every character after that is a free merge.

Highlights:

- **`x3_club_dancer`** — 20 *distinct* dance loops, so a floor of dancers never twins. This is the set that **replaces the hand-baked Blender dance loops** in Club 1127.
- **`rf_caster`** — Meshy has a real `CastingSpell` subcategory (12 clips) plus charged casts and ground slams. Note Meshy's catalog **misspells these `mage_soell_cast`** — that typo *is* the real action name. Do not "fix" it; the set uses the literal name and the resolver would reject anything else.
- **`x3_fps_combat`** — gun-ready locomotion, three reload variants (standing/kneeling/running) and **four differentiated deaths** (back, forward, blown-back, gunshot reaction) so enemies don't all die identically.
- **`eos_rts_*`** — deliberately small. The RTS camera is far away; read-at-distance beats variety, and 12 clips cost 36 credits instead of 60.

Inspect any set:

```bash
python tools/meshy_clip_library.py sets            # all sets + costs
python tools/meshy_clip_library.py show rf_caster  # resolved to concrete ids
python tools/meshy_clip_library.py cost x3_club_dancer rf_caster   # price a batch
```

---

## 4. Commands

### Free / offline
```bash
python tools/meshy_scrape_actions.py                    # refresh the catalog
python tools/meshy_clip_library.py sets
python tools/meshy_clip_library.py show  <set>
python tools/meshy_clip_library.py search <keyword>... [--inplace-only]
python tools/meshy_clip_library.py cost  <set>...
python tools/meshy_clip_library.py balance
```

### Paid
```bash
# 5 credits, ONCE per character
python tools/meshy_clip_library.py rig assets/characters/jake.glb \
    --height 1.8 --out assets/rigged_glb/jake_rigged.glb --yes-spend 5
# -> prints RIG TASK ID

# 3 credits per clip
python tools/meshy_clip_library.py bake x3_fps_combat \
    --rig <rig_task_id> --out assets/anim/meshy/x3_fps_combat --yes-spend 60
```

### Free again — the merge that makes it portfolio-wide
```bash
python tools/meshy_clip_library.py merge x3_fps_combat \
    --mesh  assets/rigged_glb/Jake_22_actions.glb \
    --clips assets/anim/meshy/x3_fps_combat \
    --out   assets/rigged_glb/Jake_combat.glb
```

`bake` writes a `clips.json` manifest into the output dir; `merge` reads it, so the engine-facing clip names survive automatically and merges are reproducible.

---

## 5. Cost discipline

Credits are a **shared fleet resource**. Guards in the tool:

1. `bake` and `rig` **refuse to run** unless `--yes-spend N` exactly equals the cost the tool independently computed. A typo can't overspend; a wrong number just aborts.
2. `bake` **skips clips already on disk** (0 credits) unless `--force`, so a resumed or re-run batch never double-charges.
3. `bake` prints a running `spent/total` counter and records `credits_spent` in `clips.json`.
4. Everything except `rig`/`bake` is offline and free.

```
$ python tools/meshy_clip_library.py bake x3_club_dancer --rig fake --out /tmp/x --yes-spend 3
[bake] set 'x3_club_dancer': 20 clips x 3 = 60 CREDITS
REFUSING to spend. This batch costs 60 credits; re-run with --yes-spend 60 to confirm.
```

---

## 6. Gotchas

- **20-action export cap.** Hard limit per baked variant. Combine locally, never by re-buying.
- **`_inplace` vs root motion.** See §1. Picking the wrong twin produces characters that slide out of their capsules.
- **Cross-rig bone naming.** Meshy emits **bare** bone names (`Hips`, `LeftFoot`); our existing cast is Mixamo (`mixamorigHips`). `glb-merge-anims.mjs` normalizes by stripping `mixamorig`/`Armature|`/`bip` prefixes and folding zero-padded indices (`Spine01`→`spine1`), so Meshy clips merge onto Mixamo characters for free. Use `--bone-prefix` if auto-normalization ever isn't enough.
- **Leaf-bone drops are normal.** Merging Meshy clips onto Mixamo Jake drops 6 of 72 channels — they are `head_end` and `headfront`, Meshy helper leaves with no Mixamo equivalent. Hips, full spine chain and all limbs match. **Watch the merge output**: dropped channels on a *spine* or *hips* bone would mean a real retarget failure (that class of bug once produced clips animating limbs over a rigid torso).
- **Membrane artifact.** Arms-raised clips (cheers, casts, `Cheer_with_Both_Hands_Up`, overhead dances) can show a webbed "membrane" between arm and torso on **suited or gloved** characters, where auto-rig weights bleed across the gap. Review arms-overhead clips on clothed characters specifically before shipping.
- **`mage_soell_cast` is spelled wrong by Meshy.** Intentional in the manifest.
- **Task history expires.** `GET /openapi/v1/rigging` and `/animations` returned `[]` — past tasks age out, so **save your `rig_task_id`**. Losing it costs 5 credits to re-rig. `bake` records it in `clips.json`.
- **Catalog can drift.** Re-run `meshy_scrape_actions.py` if a bake ever 400s with `Invalid action_id`. The scraper aborts rather than writing a truncated file if the docs layout changes.

---

## 7. Validation performed (0 credits)

| Step | How | Result |
|---|---|---|
| Catalog scrape | docs table → JSON | 678 actions, 0 dup ids, 90 `_inplace` all paired |
| Role-set resolution | all 8 sets, 140 names | all resolve; no set exceeds 20 |
| Animation request shape | POST with a valid-format but nonexistent rig id | `404 Task not found` — route/auth/body **correct**, no task created |
| `action_id` validation | POST `action_id: 99999` | `400 Invalid action_id` + docs link — confirms catalog source |
| Credit guard | `bake` with wrong `--yes-spend` | refused, exit 1 |
| Merge (same rig) | 6 real Meshy clips → Meshy mesh | 6 groups, **72/72 channels, 0 dropped** |
| Merge (cross rig) | same clips → Mixamo `Jake_22_actions.glb` | 6 groups, **66/72 channels**, only `head_end`/`headfront` dropped |

The merge tests used **already-paid** Meshy clips from `D:/GameDev/EchoHarbor/assets/meshy/characters` (street_cop idle/walk/run/talk/phone/alert), so the whole chain was exercised for free.

**Not validated:** a successful `bake` POST → poll → download. That path is byte-identical to the proven `EchoHarbor/tools/meshy_rig.py` flow and its request shape is server-confirmed above, so it was not worth spending shared credits on. To smoke-test it for 8 credits (5 rig + 3 one clip):

```bash
python tools/meshy_clip_library.py rig assets/rigged_glb/JakeClone_player.glb --yes-spend 5
python tools/meshy_clip_library.py bake eos_rts_worker --rig <id> --out /tmp/smoke --yes-spend 30
#   ^ or hand-run a single action first via EchoHarbor/tools/meshy_rig.py anim <rig> 0 out.glb
```

---

## 8. What must be done by a human in the Meshy web UI

Nothing is required for the API path. The API bypasses the UI entirely — `bake` fetches each clip as its own GLB, so **the 20-action export cap never actually binds when using this tool**; it binds only if you export a multi-action model from the web UI by hand.

Human-only, if wanted:
- **Visual browsing.** The catalog has preview GIFs (`preview` field in `meshy_actions.json`, e.g. `https://cdn.meshy.ai/webapp-assets/feature-demo/animation/preview/biped/Idle.gif`). Picking dance loops by *look* is faster in a browser than by name.
- **Buying credits** and watching the balance.
- **Judging quality** — which of the 47 running variants actually reads best on our characters. Names alone don't tell you.

---

## 9. Corrections to prior assumptions

- **Balance is 1985, not ~3136.** Roughly 1150 has been consumed since that figure was recorded. The full 420-credit portfolio bake still fits comfortably.
- **"The action catalog is not obtainable"** — it is. Not via API, but the docs page is a plain server-rendered table, and Meshy's own 400 error names it as the reference. Now checked in at `tools/meshy_actions.json`.
- **`GET /openapi/v1/balance` works** (returns `{"balance": N}`), even though `/users/me` 404s.
- **The cross-rig merge fix is NOT on `main`.** It lives on `origin/feat/jake-clip-merge` (commit `657ebd33`). `origin/main` still has the older 9210-byte `glb-merge-anims.mjs` without prefix normalization. This work is therefore branched off `feat/jake-clip-merge`, not `main` — merging it to main first, or landing both together, is required or the pipeline's merge step silently drops every channel.
