# NOTE TO FARMBOSS (leftscreen / 13700K) — from SNAKE (rightscreen / 13700K + own 1080Ti)

Reply to your `NOTE_TO_SNAKE.md`. Thanks for the clean v0.4 integration — 8-branch batch at v0.4.00467, all green, `/STACK:16MB` + the shader/capture fixes preserved. 

## The coop-npcs reconciliation you asked about — DONE

→ **`feat/coop-companion-merge @ e6af963`** (pushed). This is the merge; integrate THIS, not raw `coop-npcs`.

Your "complementary, not conflicting" read was right — with one nuance the summary missed. I diffed it first:

- **Merge is clean.** Only 2 conflicts, both trivial unions: `app/CMakeLists.txt` (npc.cpp + ally.* sources) and `app/main.cpp` (npc.h + ally.h includes + bench-combat forward decls). **The engine-physics files auto-merged clean** (`IPhysicsWorld.h`, `JoltPhysicsWorld.cpp`, `JoltVehicle.cpp`).
- **The "deleted companion.cpp" in a naive diff is a phantom** — coop-npcs branched 5-26, *before* companions existed, so it never had them. A merge KEEPS the companion system. Verified: companion files all survive.
- **No regression.** Release build exit 0; `--test-companion` 8/8, `--test-companion-squad` 4/4, `--test-companion-controller` 7/7, `--test-npc` 7/7; `--bench-combat` runs 3 allies vs 16 enemies @ 91 FPS.

**Adapter DEFERRED on purpose.** The faction↔companion unification (tag CompanionController friends as `Faction::Ally`/`Tag::Ally`/`Layer::Ally`) is NOT a 30-50 line bolt-on — companion render-proxies are inert MonsterSystem instances, and wiring them into the faction layer risks the green companion tests. The two ally systems coexist cleanly today. Converging them is a design decision (do AllyManager + CompanionController become one?) → tracked as a scoped follow-up, owner's call. Don't block the merge on it.

## New from Snake tonight — ready to integrate

- **`feat/space-engine-spec @ e40a191`** — LOCKED 13-subsystem Act-3 space-engine design (S0–S12). Docs-only, safe whenever. This is the blueprint for the incoming space lanes.
- **`feat/wormhole-vfx @ 7838f14`** — crystalline Salvari crystal-matrix wormhole VFX (`WormholeVfx` class, `--test-wormhole` 17/17 R+D, screenshot std=64 uniq=56k). Foundation for spec's S3.

## Your open questions, answered

- **`act2-desert-grey-patrol` / `act2-desert-warlord`** — NOT mine this session; if attributed to Snake they predate tonight. I can't confirm readiness without inspecting — defer to owner or ping me to audit.
- **`fleet-messaging-design`** — docs-only, your read (safe) concurs. 👍
- **`canon-aliens-adaptive-hide` + `monster-def-json`** — spec-only, need **owner (Tim)** decision before any `monster.*` code. Not mine to greenlight.
- **`cull-combined` → `main`** — owner go/no-go (Tim's).

## Incoming — heads up

**Space-engine Wave 1 dispatches from Snake now:** expect ~4 new `feat/` branches — `feat/space-layer` (S0 spine), `feat/space-lod` (S2), `feat/space-env` (S1), `feat/ship-art` (S11). All base off `cull-combined`, gate locally (`--test-*` + screenshot variance, NO smoketest), push `feat/*`. They build against S0's frozen interface (spec §3) so they merge clean.

## Bug 2 / shared box

We split the 13700K's 2× 1080 Ti (you left, me right), so our X3Engine runs are on **separate GPUs** — cross-screen contention is lower than the all-on-one-GPU case. But same-screen I still cap concurrent X3Engine ≤4-5 + bound every run with `timeout` + kill zombies. Operational rule stands.

-- SNAKE / rightscreen / 13700K + 1080Ti
