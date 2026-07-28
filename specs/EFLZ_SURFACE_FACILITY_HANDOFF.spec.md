# Spec: EFLZ Escaped-Branch Surface → Facility Handoff

- **Ledger ID:** EFLZ-GP-1B  
- **Status:** SPEC  
- **Parent:** `specs/EFLZ_GOLDEN_PATH.spec.md`  
- **Plan:** `docs/design/EFLZ_GOLDEN_PATH_PLAN.md` Phase 1  
- **Closes gap:** `host_surface_start` currently advances objective to “RESCUE SARAH” then only logs — no interior load

---

## 1. Purpose

When the interactive intro yields **Escaped**, the player lands outside the glass facility. This spec defines the **hand-off into the live facility game** so skill-earned freedom is not a dead-end demo.

---

## 2. Interface contract (host-level)

No new engine API required. Use existing:

```text
HostContext::switchWorldTo   // request world load
HostContext::switchDestKey   // optional destination key for spawn
HostContext::spawnAtKey      // set by main world-load loop for next host
StoryFlags                   // intro.outcome, intro.landed
destinations registry        // entrance / surface-adjacent keys
```

Recommended destination keys (names may match registry additions):

| Key | Meaning |
|---|---|
| `surface` | Standalone surface world (prologue) |
| `entrance` or `crash` / facility entrance | Spawn at tower exterior entrance after load |
| `canonlevel` | Full facility world flag |

Exact key strings must be registered in `app/destinations.cpp` and pass destinations self-test.

---

## 3. Behavior

### 3.1 Preconditions

- `StoryFlags["intro.outcome"] == "escaped"`  
- Ion descent completed → `StoryFlags["intro.landed"]` set (or equivalent proof that surface host is the rescuer start)  
- Surface host is running (`worldMode == "surface"`)

### 3.2 Trigger

Any one of (implementation may ship the first; document which):

1. **Interact** at the facility breach / main doors (`[E] ENTER FACILITY`)  
2. **Volume** enter within N meters of breach center for T seconds  
3. **Objective accept** UI confirm  

Trigger must be discoverable (prompt + objective text).

### 3.3 Handoff sequence (preferred Option A)

1. Surface host sets `hc.switchWorldTo = "canonlevel"`.  
2. Sets `hc.switchDestKey` / spawn key for **rescuer entrance** (not cell).  
3. Returns through main’s world-load loop (same device/window — no process restart).  
4. Default host builds canon world **without** replaying interactive intro.  
5. Player spawns at entrance/apron:  
   - Free (not cell-locked)  
   - Armed with product combat loadout  
   - `intro.outcome` / `intro.landed` still readable  
6. Objective becomes interior-appropriate (e.g. reach Sarah / enter detention / clear approach) — not stuck on surface-only text.

### 3.4 What must not happen

- Fall back to cell prisoner start while `intro.outcome=escaped`  
- Drop StoryFlags  
- Leave player on surface with no way in  
- Cosmetic-only weapon after handoff  
- Double free of device/window (hosts already own teardown rules)

### 3.5 ShotDown regression

ShotDown path must remain byte-behavior identical for cell start when this feature is off-path (default force shot_down / natural roll).

---

## 4. Edge cases

| Case | Expected |
|---|---|
| Player dies on surface before enter | Respawn on surface; flags kept |
| Trigger before descent flag | Should not fire if not landed; or treat as invalid |
| Headless `--test-surfacestart` | May simulate handoff without full window |
| World load failure | Log error; remain on surface with message |
| Player already has arsenal mid-surface | Preserve or re-grant equivalent on spawn |

---

## 5. Performance targets

- Handoff load time acceptable for Act-1 (same class as manual `--world canonlevel` boot).  
- Loading screen pattern from default host should run if already used for world switch.

---

## 6. Acceptance tests

1. **H1 — Force escaped:** intro force escaped → surface host active; `intro.landed` set after descent.  
2. **H2 — Trigger:** simulated breach interact sets `switchWorldTo=canonlevel` with rescuer spawn key.  
3. **H3 — Spawn:** after load, player position is outside cell (entrance/apron bounds); `hasWeapon`/arsenal armed true.  
4. **H4 — Flags:** `intro.outcome=escaped` readable after load.  
5. **H5 — ShotDown intact:** force shot_down still cell start.  
6. **H6 — Eyes-on:** screenshot surface approach + first interior frame after handoff.

---

## 7. Notes for implementers

- Today surface Sarah display-only rescue must not be confused with F2 product rescue; after handoff, F2 `RescueSystem` is authoritative.  
- Prefer reusing `destinations` + world menu load path so hub/menu stay consistent.  
- If entrance key missing, add it in the same PR as the handoff.
