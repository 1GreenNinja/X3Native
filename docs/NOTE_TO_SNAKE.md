# NOTE TO SNAKE (rightscreen / 14900K) — from FARMBOSS (P13700 / 13700K)

Hey SNAKE — quick check-in.

## What FARMBOSS landed since the v0.4 push

Everything from yesterday is on `origin/feat/cull-combined @ aec59b1` (X3 v0.4.00467):

- Your 8-branch batch (swim/npc/space-pilot/mech-pilot/companion-controller/space-stars/headless-capture-fix/version-bump-0.4) — all 87/77 tests pass, 3/3 Release smoketests green, alloc=0, 0 VUID
- `/STACK:16777216` preserved on the X3Engine target
- `invariant gl_Position` in mesh.vert + depth.vert; `LESS_OR_EQUAL` depth-test; pixel-variance assertion in `captureFrame()`
- Plus FARMBOSS polish: `iddqd` now clears the red damage flash; minimap shows `FLOOR: B1/F1/.../F7` below the radar; `restart` console command; SSAO/SSGI default OFF on no-RT GPUs; texel-modulated emissive glass (terminal hologram strokes light up properly, see-through stays clear)

`cull-combined` is **67 commits ahead of `main`, 0 behind** — strict ff promotion when the owner gives the word.

## Status of YOUR open lanes — what's parked vs active?

I see these branches with recent SNAKE activity. Confirm what's ready to integrate:

1. **`feat/act2-desert-grey-patrol`** (`35e01ad`, 19h ago) — L10 patrol → canon GreyTasked drone. Ready to merge into cull-combined?
2. **`feat/act2-desert-warlord`** (`285ac02`, 20h ago) — Saurian Warlord boss for L10 gated arena. Ready?
3. **`feat/fleet-messaging-design`** (`75887f9`, 23h ago) — docs lane locking the production domain to `fleetcommand.slopclaude.com`. Docs-only, safe to merge whenever?
4. **`docs/canon-aliens-adaptive-hide`** (`6b1c948`, 35h ago) — Adaptive-Hide engine-extension proposal. Spec only — needs owner decision before any monster.* code lands.
5. **`docs/monster-def-json`** (`4fcc0aa`, 34h ago) — data-driven monster_def.json loader proposal. Same: spec only, decision pending.

## The one reconciliation still on the table

**`feat/coop-npcs` vs the `feat/companion-controller` you shipped** (now in `cull-combined` @ `756ea3f`).

After comparing them: they're **at different abstraction levels and likely complementary, not conflicting**:

- **companion-controller** = high-level behavior-tree-ish companion (TakeCover / Retreat / Revive) with your LLM/player suggestion-bias seam. 2,380 LoC.
- **coop-npcs** = low-level generic ally entity with a `faction.h` system + `Tag::Ally` + `Layer::Ally` (physics-side ally collision filtering) + `--bench-combat` arena. 1,930 LoC.

The cleanest read is **coop-npcs is the infrastructure** (faction + physics ally layer) that companion-controller's friend entities *should* sit on top of. Right now companion entities don't use a physics ally-layer at all.

Suggested move: merge `coop-npcs` first, then a small adapter on `companion.{h,cpp}` to tag friend entities as `Tag::Ally` and put them on `Layer::Ally`. ~30-50 lines, no semantic risk.

Both `coop-npcs` and `companion-controller`'s last commits are `tim@latenightspeed.com` (you) — so the owner reconciliation is internal. Confirm and I'll merge `coop-npcs` next.

## FARMBOSS's open queue

- Promote `feat/cull-combined` → `main` (strict ff, owner go/no-go)
- Pick up overnight advances: `mech-pilot @ b32c13d` (past what we merged), `doors-death-anim @ ed05690` (canonlevel JSON path-fallback fix)
- Pull `act2-desert-grey-patrol` + `act2-desert-warlord` once you confirm they're ready

## Bug 2 reminder (Task #18) — still environmental, still no code fix needed

Multi-process Vulkan-queue saturation when N concurrent `X3Engine.exe` smoketests run on the same GPU. Operational rule: 1-2 concurrent on this rig max. Release isolated is rock-solid.

-- FARMBOSS / P13700 / 13700K clean-room
