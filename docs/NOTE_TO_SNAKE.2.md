# NOTE TO SNAKE (rightscreen) — from FARMBOSS (P13700 / 13700K)

**Follow-up to `NOTE_TO_SNAKE.md`. 2026-05-30.**

## 🟢 Space engine integration COMPLETE

Your `SPACE_INTEGRATION_HANDOFF.md` was executed end-to-end. All 15 branches landed on `feat/cull-combined` in your specified order.

```
TIP:     a84383a
VERSION: X3 v0.4.00507  (was 0.4.00467 — 40 commits added)
PUSH:    c72d982..a84383a  feat/cull-combined -> feat/cull-combined
```

### Receipts
- **14 new lane gates ALL GREEN** in this build's `--test-*` chain:
  - `--test-spacelayer 23/23` · `--test-lod 18/18` · `--test-spaceenv 12/12` · `--test-shipanim 19/19`
  - `--test-wormhole 17/17` · `--test-wormhole-transit 17/17` · `--test-atmo-descent 14/14` · `--test-ship-interior 6/6`
  - `--test-ship-ai 9/9` · `--test-targeting 19/19` · `--test-ship-damage 20/20` · `--test-eva 8/8`
  - `--test-ship-windows 5/5` · `--test-ship-repair 8/8`
- **101 total `--test-*` flags** in the arg-parser, all PASS.
- **3/3 sequential isolated Release `--smoketest`**, exit=0, alloc=0, 0 VUID, plus `--world canonlevel` clean.
- **Release + Debug** both build clean.
- **`/STACK:16777216` preserved** at `app/CMakeLists.txt:101`.
- **All 4 SpaceShip GLBs auto-resolved via LFS** (`SpaceShip.glb`..`SpaceShip4.glb`, 7-13 MB each) — no manual `git lfs pull` needed.
- **Coop reconciliation** (`feat/coop-companion-merge @ fac23f0`) landed as the closing commit — clean `ort` 3-way merge, no conflicts.

## ⚠️ Two new findings for Task #20 (main.cpp monolith)

Both bit during the integration. Captured for when you refactor:

### 1. MSVC `C1061` — fatal-too-deeply-nested at 100 `else if`

The arg-parser `if (a == "--smoketest") ... else if (a == "--test-X") ...` chain hit **MSVC's nesting limit at exactly 100 `else if`** after Wave 2. The agent had to break the chain mid-stream with a standalone `if(...)` to dodge it (fixup commit `abbb2d6`). Any future test-flag addition will either:
- need to manually insert another `if`-break to restart the chain, **or**
- (better) refactor to a name→ptr dispatch table — a `std::unordered_map<std::string_view, bool*>` or similar — which kills this whole class of pain forever.

### 2. `bool testSpaceLayer` decl chain collides on every space-derived branch

Every lane that forked from `feat/space-layer` re-introduced the `bool testSpaceLayer = false;` declaration, so the bool-decl block conflicted on every Wave 1/2 merge. The integration agent's helper (`G:\tmp\merge_helper.py`) strips duplicates automatically now, but a `bool tests[N]{}` array (or the dispatch-map pair-up with #1) would eliminate the splice class entirely.

Suggested combined refactor for Task #20:
```cpp
// instead of 100 bool decls + 100 else-if branches:
static const struct { const char* flag; void (*run)(); } kTests[] = {
    { "--test-spacelayer", []{ runSpaceLayerTest(); std::exit(0); } },
    { "--test-lod",        []{ runLodTest(); std::exit(0); } },
    /* ... */
};
// argparse loops over kTests once; no decls, no else-if chain, no splice surface.
```

## 🟡 Portal-hub status (I see you in there)

Saw `feat/portal-hub` advanced 2 hours ago (`d8298f0` blue-energy-core + chase-pulse rim nodes). **I am NOT touching it** until you ping ready — it's an active lane and `53a374b "main: lift console+HUD above worldMode"` will collide with the v0.4.00507 `main.cpp` hot zone. When you're ready, send a handoff like the space-engine one (or just paste the branch tip + any snag warnings) and I'll grind it in.

## Other open items (no urgency)

- **Promote `feat/cull-combined` → `main`?** 107 ahead, 0 behind, strict ff available. Owner's call.
- **`feat/mech-pilot @ b32c13d`** advanced past the version we merged on `aec59b1`. Surgical pull whenever.
- **`feat/doors-death-anim @ f96ad5f`** advanced TWICE past our merged version: `ed05690` (canonlevel JSON + path fallback) + `f96ad5f` (keycard + keypad locks on Security/Medical/Armory). Surgical pull whenever.
- **`docs/canon-aliens-adaptive-hide`** + **`docs/monster-def-json`** — design proposals still pending owner go/no-go before any `monster.*` code lands.

## Heads-up on helper artifacts left behind

The integration agent kept these for future merges (might want to fold into `tools/` or delete):
- `G:\tmp\merge_helper.py` — auto-resolver for the include / bool-decl / argparse splice patterns
- `G:\tmp\fix_wormhole.py`, `G:\tmp\fix_ship_repair_world.py` — one-off lane fixers
- `G:\tmp\ship_windows_world.cpp`, `G:\tmp\ship_repair_world.cpp` — extracted originals used as transplant donors

Not committed; just scratch. Worth moving the helper script into `tools/` if you want it durable.

— FARMBOSS / P13700 / 13700K clean-room
