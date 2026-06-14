# Mission — First Light (`act2_first_light`)

**Act 2 · Level 8 · Surface Emergence** — the lab-exit tunnel and the Emergence Point.
Opens Act 2 directly off the engine's L8 host (`app/act2_world.*`).

> Fight clear of the collapsing facility and stand, for the first time in six months,
> under an open sky that isn't Earth's.

## Beats (the spine + branches)

1. **Get out.** (`o0`) Push the service tunnel — five kills clears the pursuit gauntlet
   (5 Pursuit Drones + 3 Infected Soldiers, the existing L8 roster). Death just re-arms the
   objective; no karma cost for dying. Sets `act2.surfaced`.
2. **Reach the Emergence Point.** (`o1`) The safe-zone reveal. The **two-suns cutscene**
   fires here — the awe beat. Quietly sets `cradle.core_seen`: the player is now standing ON
   the seeded world, so the Cradle/Biomesh recontextualization can begin paying off.
3. **Take stock.** (`o2`) Whoever made it out is standing with you (the L8 allied companion
   markers). Branches on the Floor-2 triage:
   - **All three saved** → `o3_full`: the warm gather (a beat of Aria banter, +trust).
   - **Someone lost** → `o3_grief`: Vesper's surface-grief beat (`act2_grief/g0`) — say her
     name somewhere with sky.
   - **Mixed / default** → `o3`: just mark the route.
4. **Into the desert.** (`o4`) Cross the Emergence Point's edge. Chains directly into
   **The Mourner's Debt** (`act2_mourners_debt`, L10).

## Hooks
- Sits on top of `act2_world`'s L8 (lab-exit gauntlet → Emergence-Point awe beat → companion
  markers Sarah/Aria/Keisha/Emily).
- `cradle.core_seen` is the first thread-(b)/(e) seed of Act 2.
- The grief branch reuses Vesper (no new NPC) and reads `girl_lost` — pure additive
  recontextualization of the F2 outcome.

## Voice note
Awe, not exposition. The reveal does the talking; objective text stays terse and physical
("Don't stop moving."). No lore dump on the surface — that comes later, earned.
