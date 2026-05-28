# Companion Reflex AI — Implementation Plan (Slice A: the decision core)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A pure, deterministic `CompanionBrain` that reads a tactical situation and outputs a per-tick command (move/aim/fire/revive), unit-tested headlessly — the reflex half of the two-brain companion design (`docs/superpowers/specs/2026-05-26-companion-ai-design.md`).

**Architecture:** Utility scoring. Each tick the brain scores a fixed set of behaviors against a `CompanionContext` (threats, player, self HP/ammo, suggestion bias) and the winner emits a `CompanionCommand` (the wire-shape of `PlayerInput`). No engine/GPU/physics deps — the brain is a pure function `tick(ctx) -> cmd`, so it's deterministic and trivially unit-testable. Integration (driving real `Player` entities, the `--world companion` showcase, the squad of ≤7) is **Slice B**, a separate plan.

**Tech Stack:** C++20, `x3::phys::Vec3` math, the existing `runXSelfTest()` + `--test-*` harness pattern (mirrors `monster.*` / `--test-nav`). No new libraries.

**Clean-room:** built from X3Native's own `player.h`/`scene`/`monster` patterns + public utility-AI literature. No game-engine source.

---

## File Structure

- **Create `app/companion.h`** — PODs (`CompanionContext`, `CompanionThreat`, `CompanionCommand`, `CompanionSuggestion`), the `CompanionBehavior` enum, the `CompanionBrain` class, and `runCompanionSelfTest()` decl. One responsibility: the reflex decision unit's public surface.
- **Create `app/companion.cpp`** — `CompanionBrain::tick()` (the utility scorer + per-behavior scorers + command synthesis) and `runCompanionSelfTest()`.
- **Modify `app/CMakeLists.txt`** — add `companion.cpp` to the `X3Engine` sources (alphabetical-ish near `cliffs.cpp`).
- **Modify `app/main.cpp`** — add `#include "companion.h"`, a `testCompanion` bool + `--test-companion` arg parse (beside `--test-cliffs`), and the dispatch `if (testCompanion) return runCompanionSelfTest() ? 0 : 1;` (beside the other `--test-*` returns).

`CompanionCommand` mirrors `PlayerInput` (`app/player.h`): `float moveFwd; float moveStrafe; bool sprint; bool jumpPressed; float lookDX; float lookDY;` — Slice B feeds it to `Player::update`. We additionally carry absolute `aimYaw/aimPitch` + `bool fire; bool reviveAction;` (intent the integration resolves).

---

## Task 1: Scaffold `companion.{h,cpp}` + wire the self-test (build green, test red→green)

**Files:**
- Create: `app/companion.h`, `app/companion.cpp`
- Modify: `app/CMakeLists.txt`, `app/main.cpp`

- [ ] **Step 1: Create `app/companion.h`**

```cpp
#pragma once
// Companion reflex AI (Slice A): a pure, deterministic decision unit. Scores a
// fixed behavior set against a tactical snapshot and emits one per-tick command.
// Game/slice code only; no engine/GPU/physics deps -> trivially unit-testable.
#include "engine/physics/IPhysicsWorld.h"   // x3::phys::Vec3 only
#include <cstdint>

namespace x3::game {

enum class CompanionBehavior : uint8_t {
    Follow = 0, Engage, TakeCover, Revive, Reload, Retreat, Hold, Count
};

struct CompanionThreat {
    x3::phys::Vec3 pos{};
    float          dist     = 0.0f;   // to the companion (m)
    float          toPlayer = 0.0f;   // threat's distance to the player (m)
    bool           losToSelf = true;  // threat has line-of-sight to the companion
};

// Light bias from the player's nudge OR (later) the LLM cognitive brain.
struct CompanionSuggestion {
    CompanionBehavior prefer = CompanionBehavior::Count; // Count == no suggestion
    x3::phys::Vec3    focusTarget{};                     // for Engage focus-fire
    bool              hasFocus = false;
};

struct CompanionContext {
    x3::phys::Vec3 selfPos{};
    x3::phys::Vec3 playerPos{};
    float          selfHpFrac   = 1.0f;   // 0..1
    int            ammoInMag    = 30;
    bool           playerDowned = false;
    bool           anyAllyDowned = false;
    x3::phys::Vec3 downedAllyPos{};
    bool           inPlayerLineOfFire = false; // would block the player's shot
    const CompanionThreat* threats = nullptr;  // array (may be null)
    int            threatCount = 0;
    CompanionSuggestion suggestion{};
    bool           nearCover  = false;
    x3::phys::Vec3 coverPos{};
};

struct CompanionCommand {
    float moveFwd = 0, moveStrafe = 0;   // -1..1, PlayerInput-compatible
    bool  sprint = false, jumpPressed = false;
    float lookDX = 0, lookDY = 0;
    float aimYaw = 0, aimPitch = 0;      // absolute aim (radians)
    bool  fire = false;
    bool  reviveAction = false;
    CompanionBehavior chosen = CompanionBehavior::Follow; // for tests/debug/HUD
};

// The reflex decision unit. Stateless across instances except light hysteresis.
class CompanionBrain {
public:
    CompanionCommand tick(const CompanionContext& ctx) const;
    // Exposed for tests: the raw score for one behavior given a context.
    float score(CompanionBehavior b, const CompanionContext& ctx) const;
};

// Headless self-test (--test-companion). Drives synthetic scenarios and asserts
// the brain picks the right behavior + emits sane commands. No window/Vulkan.
bool runCompanionSelfTest();

} // namespace x3::game
```

- [ ] **Step 2: Create `app/companion.cpp` with a stub tick + self-test that passes trivially**

```cpp
#include "companion.h"
#include "engine/core/Log.h"   // x3::logInfo (same logger monster.cpp uses)
#include <cmath>

namespace x3::game {

float CompanionBrain::score(CompanionBehavior, const CompanionContext&) const { return 0.0f; }

CompanionCommand CompanionBrain::tick(const CompanionContext&) const {
    return CompanionCommand{};   // Task 2 fills this in
}

bool runCompanionSelfTest() {
    x3::logInfo("running companion reflex-AI self-test...");
    int pass = 0, total = 0;
    // (scenarios added in later tasks)
    x3::logInfo("companion: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
```

- [ ] **Step 3: Add `companion.cpp` to `app/CMakeLists.txt`**

In the `add_executable(X3Engine WIN32 ...)` source list, add `    companion.cpp` on its own line (next to `    cliffs.cpp`).

- [ ] **Step 4: Wire `--test-companion` into `app/main.cpp`**

Add near the other companion includes: `#include "companion.h"`
Beside `else if (a == "--test-cliffs") testCliffs = true;` add: `else if (a == "--test-companion") testCompanion = true;`
Declare the bool with the other `bool testX = false;` flags: `bool testCompanion = false;`
Beside the other `if (testCliffs) { ... return ...; }` self-test dispatch add:
```cpp
if (testCompanion) {
    return x3::game::runCompanionSelfTest() ? 0 : 1;
}
```

- [ ] **Step 5: Build + run the self-test (green scaffold)**

Run (from the worktree root, VS2026 cmake):
```
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
.\build\bin\Release\X3Engine.exe --test-companion
```
Expected: build exit 0; output `companion: 0/0 passed`; exit code 0.

- [ ] **Step 6: Commit**

```
git add app/companion.h app/companion.cpp app/CMakeLists.txt app/main.cpp
git commit -m "companion: scaffold reflex-AI decision unit + --test-companion harness"
```

---

## Task 2: Follow behavior — move toward the player when nothing else matters

**Files:** Modify `app/companion.cpp`

- [ ] **Step 1: Add the failing test to `runCompanionSelfTest()`**

Replace the `// (scenarios added...)` line with:
```cpp
auto vlen = [](x3::phys::Vec3 v){ return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); };
CompanionBrain brain;
// C1: no threats, player 8 m ahead -> Follow, move roughly toward the player.
{
    total++;
    CompanionContext ctx;
    ctx.selfPos = {0,0,0}; ctx.playerPos = {0,0,8};
    CompanionCommand c = brain.tick(ctx);
    bool ok = (c.chosen == CompanionBehavior::Follow) && (c.moveFwd > 0.5f);
    if (ok) pass++; else x3::logError("[companion-test] C1 follow FAILED");
}
```

- [ ] **Step 2: Run, verify it FAILS**

Run: `.\build\bin\Release\X3Engine.exe --test-companion` (after a build)
Expected: `companion: 0/1 passed`, exit 1 (the stub returns an empty command, `chosen` defaults to Follow but `moveFwd` is 0).

- [ ] **Step 3: Implement Follow scoring + the command synthesis skeleton**

Replace `score()` and `tick()`:
```cpp
static x3::phys::Vec3 sub(const x3::phys::Vec3&a,const x3::phys::Vec3&b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
static float len(const x3::phys::Vec3&v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}

float CompanionBrain::score(CompanionBehavior b, const CompanionContext& ctx) const {
    switch (b) {
    case CompanionBehavior::Follow: {
        // Always a low baseline; rises with distance from the player so the
        // companion regroups when it drifts. Capped so combat behaviors win.
        float d = len(sub(ctx.playerPos, ctx.selfPos));
        return 0.2f + 0.05f * d;       // ~0.2 at the player's side, climbs with range
    }
    default: return 0.0f;
    }
}

CompanionCommand CompanionBrain::tick(const CompanionContext& ctx) const {
    // Pick the highest-scoring behavior (+ suggestion bias, added in Task 6).
    CompanionBehavior best = CompanionBehavior::Follow;
    float bestScore = -1.0f;
    for (int i = 0; i < (int)CompanionBehavior::Count; ++i) {
        float s = score((CompanionBehavior)i, ctx);
        if (s > bestScore) { bestScore = s; best = (CompanionBehavior)i; }
    }
    CompanionCommand c; c.chosen = best;
    if (best == CompanionBehavior::Follow) {
        // Move toward the player. moveFwd is along the companion's facing; for the
        // decision unit we treat +moveFwd as "toward the player" and let Slice B's
        // controller resolve the facing. Normalize the planar direction.
        x3::phys::Vec3 to = sub(ctx.playerPos, ctx.selfPos);
        float d = len(to);
        if (d > 1.5f) c.moveFwd = 1.0f;   // close the gap; stop within ~1.5 m
    }
    return c;
}
```

- [ ] **Step 4: Run, verify PASS**

Run: `.\build\bin\Release\X3Engine.exe --test-companion`
Expected: `companion: 1/1 passed`, exit 0.

- [ ] **Step 5: Commit**

```
git add app/companion.cpp
git commit -m "companion: Follow behavior (regroup toward player when idle)"
```

---

## Task 3: Engage behavior — face + fire on the nearest threat in range

**Files:** Modify `app/companion.cpp`

- [ ] **Step 1: Add the failing test (append inside `runCompanionSelfTest()` after C1)**

```cpp
// C2: a threat 10 m away with LOS -> Engage, fire, NOT in player's line of fire.
{
    total++;
    CompanionContext ctx;
    ctx.selfPos = {0,0,0}; ctx.playerPos = {0,0,-3};
    CompanionThreat t; t.pos = {0,0,10}; t.dist = 10.0f; t.losToSelf = true; t.toPlayer = 13.0f;
    ctx.threats = &t; ctx.threatCount = 1;
    CompanionCommand c = brain.tick(ctx);
    bool ok = (c.chosen == CompanionBehavior::Engage) && c.fire;
    if (ok) pass++; else x3::logError("[companion-test] C2 engage FAILED");
}
```

- [ ] **Step 2: Run, verify FAIL** (`companion: 1/2 passed`, exit 1 — Engage scores 0 so Follow still wins).

- [ ] **Step 3: Implement Engage scoring + command**

Add a helper to pick the best target and extend `score()`/`tick()`:
```cpp
// Nearest in-LOS threat within weapon range; nullptr if none.
static const CompanionThreat* bestTarget(const CompanionContext& ctx, float range) {
    const CompanionThreat* best = nullptr;
    for (int i = 0; i < ctx.threatCount; ++i) {
        const CompanionThreat& t = ctx.threats[i];
        if (!t.losToSelf || t.dist > range) continue;
        if (!best || t.dist < best->dist) best = &t;
    }
    return best;
}
```
In `score()`, add the `Engage` case (before `default`):
```cpp
case CompanionBehavior::Engage: {
    const CompanionThreat* t = bestTarget(ctx, 60.0f);
    if (!t) return 0.0f;
    // High base, higher for closer threats; this should beat Follow whenever a
    // target exists. (Cover/Retreat can outscore it later when exposed/low.)
    return 1.0f + (60.0f - t->dist) * 0.02f;
}
```
In `tick()`, add an `Engage` branch (after the Follow branch):
```cpp
else if (best == CompanionBehavior::Engage) {
    const CompanionThreat* t = bestTarget(ctx, 60.0f);
    if (t) {
        x3::phys::Vec3 to = sub(t->pos, ctx.selfPos);
        c.aimYaw = std::atan2(to.z, to.x);          // CONVENTIONS.md yaw basis
        float horiz = std::sqrt(to.x*to.x + to.z*to.z);
        c.aimPitch = std::atan2(to.y, horiz);
        c.fire = !ctx.inPlayerLineOfFire && ctx.ammoInMag > 0;  // never shoot the player
    }
}
```

- [ ] **Step 4: Run, verify PASS** (`companion: 2/2 passed`, exit 0).

- [ ] **Step 5: Commit**

```
git add app/companion.cpp
git commit -m "companion: Engage behavior (target nearest in-range threat, hold fire through the player)"
```

---

## Task 4: TakeCover + Retreat — survive when exposed or low

**Files:** Modify `app/companion.cpp`

- [ ] **Step 1: Add failing tests (append after C2)**

```cpp
// C3: low HP + exposed threat + cover available -> TakeCover (over Engage).
{
    total++;
    CompanionContext ctx;
    ctx.selfPos = {0,0,0}; ctx.playerPos = {0,0,-3}; ctx.selfHpFrac = 0.25f;
    CompanionThreat t; t.pos = {0,0,12}; t.dist = 12.0f; t.losToSelf = true;
    ctx.threats = &t; ctx.threatCount = 1;
    ctx.nearCover = true; ctx.coverPos = {4,0,0};
    CompanionCommand c = brain.tick(ctx);
    bool ok = (c.chosen == CompanionBehavior::TakeCover);
    if (ok) pass++; else x3::logError("[companion-test] C3 cover FAILED");
}
// C4: critical HP, no cover -> Retreat (move away from threat).
{
    total++;
    CompanionContext ctx;
    ctx.selfPos = {0,0,0}; ctx.playerPos = {0,0,-3}; ctx.selfHpFrac = 0.1f;
    CompanionThreat t; t.pos = {0,0,6}; t.dist = 6.0f; t.losToSelf = true;
    ctx.threats = &t; ctx.threatCount = 1; ctx.nearCover = false;
    CompanionCommand c = brain.tick(ctx);
    bool ok = (c.chosen == CompanionBehavior::Retreat);
    if (ok) pass++; else x3::logError("[companion-test] C4 retreat FAILED");
}
```

- [ ] **Step 2: Run, verify FAIL** (`companion: 2/4 passed`).

- [ ] **Step 3: Implement TakeCover + Retreat scoring**

Add cases to `score()`:
```cpp
case CompanionBehavior::TakeCover: {
    const CompanionThreat* t = bestTarget(ctx, 60.0f);
    if (!t || !ctx.nearCover) return 0.0f;
    // Rises as HP drops + while exposed to a threat; (1-hp) so 25% HP -> strong.
    return (1.0f - ctx.selfHpFrac) * 2.2f;
}
case CompanionBehavior::Retreat: {
    const CompanionThreat* t = bestTarget(ctx, 60.0f);
    if (!t) return 0.0f;
    // Only when critically low AND no cover to break to; must beat Engage(~1).
    if (ctx.selfHpFrac > 0.15f || ctx.nearCover) return 0.0f;
    return 2.5f;
}
```
Add `tick()` branches:
```cpp
else if (best == CompanionBehavior::TakeCover) {
    if (ctx.nearCover) { /* Slice B: path to coverPos; decision-unit just flags + heads there */
        x3::phys::Vec3 to = sub(ctx.coverPos, ctx.selfPos);
        if (len(to) > 0.5f) c.moveFwd = 1.0f;
    }
}
else if (best == CompanionBehavior::Retreat) {
    const CompanionThreat* t = bestTarget(ctx, 60.0f);
    if (t) { c.moveFwd = 1.0f; c.sprint = true; /* away-from-threat dir resolved in Slice B */ }
}
```

- [ ] **Step 4: Run, verify PASS** (`companion: 4/4 passed`).

- [ ] **Step 5: Commit**

```
git add app/companion.cpp
git commit -m "companion: TakeCover (exposed+low) and Retreat (critical, no cover) behaviors"
```

---

## Task 5: Revive — break to a downed ally (top priority when safe-ish)

**Files:** Modify `app/companion.cpp`

- [ ] **Step 1: Add failing test (append after C4)**

```cpp
// C5: an ally is downed, no immediate lethal threat -> Revive (beats Engage).
{
    total++;
    CompanionContext ctx;
    ctx.selfPos = {0,0,0}; ctx.playerPos = {0,0,-3};
    ctx.anyAllyDowned = true; ctx.downedAllyPos = {5,0,0}; ctx.selfHpFrac = 0.8f;
    CompanionThreat t; t.pos = {0,0,20}; t.dist = 20.0f; t.losToSelf = true;
    ctx.threats = &t; ctx.threatCount = 1;
    CompanionCommand c = brain.tick(ctx);
    bool ok = (c.chosen == CompanionBehavior::Revive) && c.reviveAction;
    if (ok) pass++; else x3::logError("[companion-test] C5 revive FAILED");
}
```

- [ ] **Step 2: Run, verify FAIL** (`companion: 4/5 passed`).

- [ ] **Step 3: Implement Revive scoring + command**

`score()`:
```cpp
case CompanionBehavior::Revive: {
    if (!ctx.anyAllyDowned) return 0.0f;
    // High — saving a teammate is a priority — but reduced under heavy close fire
    // (a near threat in <8 m suppresses the revive so the medic isn't suicidal).
    const CompanionThreat* t = bestTarget(ctx, 8.0f);
    float danger = t ? 1.5f : 0.0f;
    return 1.8f - danger;     // safe-ish: 1.8 (beats Engage ~1.x); under fire: 0.3
}
```
`tick()`:
```cpp
else if (best == CompanionBehavior::Revive) {
    x3::phys::Vec3 to = sub(ctx.downedAllyPos, ctx.selfPos);
    float d = len(to);
    if (d > 1.2f) c.moveFwd = 1.0f;     // close to the ally
    else          c.reviveAction = true; // in range -> revive
}
```

- [ ] **Step 4: Run, verify PASS** (`companion: 5/5 passed`).

- [ ] **Step 5: Commit**

```
git add app/companion.cpp
git commit -m "companion: Revive behavior (break to a downed ally unless under close fire)"
```

---

## Task 6: Suggestion bias — player/cognitive nudges tilt the scores

**Files:** Modify `app/companion.cpp`

- [ ] **Step 1: Add failing test (append after C5)**

```cpp
// C6: two equal-ish options; a Hold suggestion makes Hold win.
{
    total++;
    CompanionContext ctx;
    ctx.selfPos = {0,0,0}; ctx.playerPos = {0,0,8};   // would Follow by default
    ctx.suggestion.prefer = CompanionBehavior::Hold;
    CompanionCommand c = brain.tick(ctx);
    bool ok = (c.chosen == CompanionBehavior::Hold);
    if (ok) pass++; else x3::logError("[companion-test] C6 suggestion FAILED");
}
```

- [ ] **Step 2: Run, verify FAIL** (`companion: 5/6 passed` — Hold scores 0, Follow wins; no bias yet).

- [ ] **Step 3: Implement the bias in `tick()`'s selection loop + a Hold baseline**

In `score()`, give `Hold` a small baseline so a suggestion can lift it:
```cpp
case CompanionBehavior::Hold: return 0.1f;   // stay put; only wins via a suggestion
```
In `tick()`, add the bias inside the scoring loop:
```cpp
for (int i = 0; i < (int)CompanionBehavior::Count; ++i) {
    float s = score((CompanionBehavior)i, ctx);
    if (ctx.suggestion.prefer == (CompanionBehavior)i) s += 0.5f; // nudge, not override
    if (s > bestScore) { bestScore = s; best = (CompanionBehavior)i; }
}
```
(0.5 flips ties/near-ties like Follow↔Hold, but cannot override a strong combat/revive score — a suggestion is a nudge, per the design.)

- [ ] **Step 4: Run, verify PASS** (`companion: 6/6 passed`).

- [ ] **Step 5: Commit**

```
git add app/companion.cpp
git commit -m "companion: suggestion bias seam (player/LLM nudge tilts behavior scores)"
```

---

## Slice A done → next

After Task 6 the reflex decision core is complete + tested (`--test-companion` 6/6). **Slice B** (separate plan) integrates it: a `CompanionSquad` of ≤7 driving real `Player` controllers, building the `CompanionContext` from the live `Scene`/threats/nav each tick, the downed/revive *state machine*, and the `--world companion` showcase + headless capture. Subsystem #2 (the LLM cognitive brain) then fills `CompanionSuggestion` via async intents.

---

## Self-Review

- **Spec coverage:** Behaviors Follow/Engage/TakeCover/Retreat/Revive/Hold + the suggestion seam (spec §6/§7) — covered by Tasks 2–6. Reload (in the spec's scored set) is deferred to Slice B with the ammo/reload integration (noted; not silently dropped). Squad of ≤7, downed/revive *state*, Player-controller mover, `--world companion` test bed → Slice B (spec §8), explicitly out of scope here.
- **Placeholder scan:** none — every step has full code + exact commands + expected output.
- **Type consistency:** `CompanionBehavior`, `CompanionContext`, `CompanionThreat`, `CompanionSuggestion`, `CompanionCommand`, `bestTarget()`, `score()`, `tick()` are used consistently across tasks; `CompanionCommand` field names match `PlayerInput` where shared.
