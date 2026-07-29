# Spec: Destination Registry ↔ World Dispatch Sync

- **Ledger ID:** DEST-1  
- **Status:** SPEC  
- **Files:** `app/destinations.h`, `app/destinations.cpp`, `app/world_hosts/world_hosts.cpp`, default-host world modes in `app_run`  
- **Plan refs:** Unified board P0-2; EFLZ + Echo Harbor menus/hub

---

## 1. Purpose

There is **one** table of places (`destinations`) used by:

- World / place selection menu  
- Rift console TARGET cycle  
- Hub fast-travel resolver  

Every `worldFlag` in that table must name a mode the program **actually dispatches**. Every dispatched world that is player-reachable should appear in the table (or be explicitly marked internal-only).

This kills the class of bug where menus offer `act2caves` (or omit `echotropolis`) while no host exists (or the host exists but is invisible).

---

## 2. Interface contract

Existing:

```cpp
struct Destination {
  const char* key;
  const char* name;
  const char* desc;
  const char* worldFlag;  // "" = no standalone world
  DestGroup group;
  bool canonAnchor;
};

const Destination* findDestination(std::string_view);
bool runDestinationsSelfTest();
```

Dispatch sites that must stay in agreement:

1. `dispatchWorldHost` in `world_hosts.cpp`  
2. Default-host world branches in `app_run` (`canonlevel`, `intro`, `level1`, `elevator`, `terrain`, `ocean`, `fromdoc`, …)  
3. `kDispatchedWorlds` (or successor) inside destinations self-test  

---

## 3. Behavior

### 3.1 Completeness

- For every `Destination` with non-empty `worldFlag`, either:  
  - `dispatchWorldHost` handles it, **or**  
  - default host handles it as `worldMode`.  
- Self-test **fails** if a flag is not dispatched.

### 3.2 Known product worlds (minimum set)

Must be present and dispatched when the feature is in the build:

| worldFlag | Product |
|---|---|
| `canonlevel` | EFLZ main |
| `intro` | EFLZ cold-open entry |
| `surface` | Escaped landing |
| `rifthub` | Hub |
| `echotropolis` | Echo Harbor (**add if missing**) |
| `space` | Space combat slice |
| `strata`, `club`, `valley`, `cliffs`, `streamed`, … | Existing slices already in table |

Space extras if hosts exist: `introcockpit`, wormhole/tractor/ship-windows as **DevWorld** entries or explicitly internal (not in menu) but still in `kDispatchedWorlds` for the self-test.

### 3.3 Honesty

- Destinations with neither `canonAnchor` nor `worldFlag` are unreachable and UI-greyed.  
- Never list a worldFlag that 404s at dispatch.

### 3.4 findDestination

Loose matching (exact key → flag → name → aliases) remains; aliases must not map to undischarged flags.

---

## 4. Edge cases

| Case | Expected |
|---|---|
| Empty worldFlag + canonAnchor | Teleport-only place; OK |
| Internal bench (`destruct`) | May live as DevWorld |
| Removed host | Remove from table same PR |
| New host PR | Table + self-test update **same PR** |

---

## 5. Acceptance tests

1. **D1 — Existing:** `runDestinationsSelfTest` / `--test-rifthub` destinations section green.  
2. **D2 — echotropolis:** `findDestination("echotropolis")` non-null; flag dispatches.  
3. **D3 — Exhaustive:** every `dispatchWorldHost` string appears in `kDispatchedWorlds`.  
4. **D4 — No ghosts:** no table worldFlag outside dispatched set.  
5. **D5 — Round-trip:** every key and name resolves via `findDestination`.  

---

## 6. Notes

- Prefer generating `kDispatchedWorlds` comments from a single macro/list shared with dispatch in a later refactor; first ship can keep dual lists if self-test enforces equality.  
- Echo Harbor may remain launcher-primary; still register for hub/menu consistency when on mainline builds that include the host.
