# EFLZ Act 1 "The Spire" — Balance / Playtest Report (B1 → F7)

Generated from the live code on branch `feat/spire-capture`. Every number below is
cited to the source constant. Per-floor renders are in `captures/spire/spire_<floor>.png`,
produced by the new headless `--capture-spire` harness (a dev tool — it changes no
gameplay/balance).

**This report only ANALYZES and RECOMMENDS. No tuning was changed.** The coordinator
owns the actual enemy-count / damage / cooldown / timer edits.

---

## 0. The numbers everything depends on (sources)

### Player (`app/player.h`, `app/player.cpp`)
| Stat | Value | Source |
|---|---|---|
| Max HP | **100** | `kPlayerMaxHp` |
| Health regen | **NONE** | `Player::update`/`updateHealth` — no regen path exists; `heal()` is only called on respawn/pickup |
| Invulnerability window (iframe) | **0.5 s** after any landed hit | `kPlayerIFrame`; `Player::takeDamage` sets `m_iframe=kPlayerIFrame` and **returns false (absorbs) while `m_iframe>0`** |
| Respawn delay | 2.0 s | `kRespawnDelay` |

> **The single most important balance fact in Act 1:** the iframe is a **GLOBAL**
> gate on the player, not per-attacker. The moment ANY attack lands, the player is
> invulnerable for 0.5 s and *every* other incoming hit (melee or ranged, from any
> number of enemies) in that window is discarded (`takeDamage` returns false).
> Therefore the player's worst-case **sustained incoming DPS is capped at
> `largest_single_hit × (1 / 0.5 s) = largest_single_hit × 2`**, no matter how many
> enemies are shooting. Enemy *count* mostly drives time-to-clear and target
> priority, not lethality; **single-hit size and boss phase scaling drive lethality.**

### Player weapon — the level's gating arm is the **pistol** (`app/weapon.cpp` `makeDefaultRoster`)
| Weapon | Dmg | Rate | Mag | Reload | Raw DPS | Sustained DPS (incl. reload) |
|---|---|---|---|---|---|---|
| **pistol** (pickup-gated) | 15 | 3/s | 12 | 1.5 s | **45** | 12 shots/4 s + 1.5 s = 180 dmg / 5.5 s ≈ **32.7** |
| smg | 12 | 11/s | 40 | 2.0 s | 132 | ≈ 85.7 |
| shotgun | 20 ×8 pellet | 1/s | 8 | 2.5 s | 160/shot | — |
| plasma | 35 | 2/s | 20 | 2.2 s | 70 | ≈ 56 |

The full arsenal exists (number keys), but Act 1's only weapon **pickup** is the
pistol, and firing is gated on `WeaponSystem::hasWeapon()`. Outgoing-DPS estimates
below use the **pistol** (worst realistic case). Hitscan + 50 m range means the player
can hit any enemy in LOS, so TTC is `total_floor_HP / pistol_sustained_DPS`.

### Enemy roster (`app/monster.cpp buildMonsterDefs` + `app/monster.h` `namespace combat`)
| Species | Archetype | HP | Per-hit dmg | Range (m) | Cooldown (s) | Windup (s) | Notes |
|---|---|---|---|---|---|---|---|
| DominionTrooper | Guard / melee | **100** | 8 (`kMeleeDamageDefault`) | 1.9 (`kMeleeRange`) | 1.1 (`kMeleeCooldownDefault`) | 0.25 | baseline |
| Verthani | Guard / melee | **130** | 10 (`kMeleeDamageMax`) | 1.9 | 1.0 (`kMeleeCooldownMin`) | 0.25 | fast (4.2 m/s), strafe-heavy |
| BlueSynth | Drone / ranged | **150** | 5 (`kRangedDamageDefault`) | 14 | 1.4 (`kRangedCooldownDefault`) | 0.30 | standoff 7 m |
| Illuminated | Drone / ranged (elite) | **220** | 6 (`kRangedDamageMax`) | 18 | 0.8 (`kRangedCooldownMin`) | 0.35 | standoff 11 m; tanky |

### Bosses
| Boss | Floor | HP | Dmg (P1) | P2 dmg ×1.4 | P3 dmg ×1.8 | Cooldown | Source |
|---|---|---|---|---|---|---|---|
| Chief Martinez | B1 arena | 340 | 15 | 21 | 27 (+summon 2 Guards) | 1.1 | `level1_game.cpp martinezTuning` |
| "Lena" (F5 captive→boss on expiry) | F5 | 460 | 13 | 18.2 | 23.4 | 1.1 | `spire_mid.cpp f5VictimBossTuning` |
| The Clone | F7 finale | 620 | 14 | 19.6 | **25.2** (+summon 2) | 1.05 | `spire_top.cpp cloneBossTuning` |
| "Sarah" (F7 captive→boss on expiry) | F7 | 500 | 13 | 18.2 | 23.4 | 1.1 | `spire_top.cpp sarahVictimBossTuning` |

Boss phases (`monster.h`): enrage at ≤66% HP, desperate at ≤33% HP (`phase2Frac`/`phase3Frac`).

### Dogpile / rescue constants
- Melee attacker cap **2** (`combat::kMaxMeleeAttackers`) — only the nearest 2 melee
  enemies get an attack permit per frame; the rest hold at the standoff ring
  (`kStandoffRing = 2.6 m`). **Ranged enemies are NOT capped.**
- Rescue timer **300 s** (`kRescueTimer`), reach **3.0 m** (`kRescueReach`). Timers are
  **gated** on the floor hub being reached (`activate()` / `m_*HubReached`) — they do
  **not** run at load.

### Incoming-DPS methodology
Two values are reported per floor:
- **Naive DPS** = Σ over all attackers in reach/range of `dmg / cooldown` (the number a
  designer fears) — *ignores* the iframe.
- **Real DPS (iframe-capped)** = `largest_single_hit × 2` (the actual ceiling, since one
  hit per 0.5 s is all that lands). **TTK-player = 100 / Real DPS.**

The gap between the two is the safety margin the iframe buys.

---

## B1 — Basement Security (baseY 0; capture `spire_b1.png`)

The B1 plate is internally partitioned (cross-walls at x = 5 / 9 / 12.5 / 15 with
z=0 doorway gaps) into cell → corridor → armory → checkpoint → arena. Three distinct
encounters live here on **beats**, not all at load:

| Encounter | When it spawns | Composition | HP total |
|---|---|---|---|
| Checkpoint squad | **at build** | 2 DominionTrooper + 1 Verthani + 1 BlueSynth | 100+100+130+150 = **480** |
| Corridor "alarm" wave | beat: Door A opens | 2 DominionTrooper + 1 Verthani + 2 BlueSynth | 200+130+300 = **630** |
| Chief Martinez (boss) | beat: arena trigger (x[16,19]) | Boss 340 HP | **340** |

(The capture frames the **checkpoint** room — 4 enemies — because that group exists at
load and the camera is kept out of the arena trigger so the boss doesn't fill the lens.)

- **Enemy count / split (at load):** 4 → 3 melee (2 Trooper + 1 Verthani) + 1 ranged (BlueSynth).
- **Naive incoming DPS (checkpoint, all engaged):** 8/1.1 + 8/1.1 + 10/1.0 + 5/1.4 ≈ 7.3+7.3+10+3.6 = **28.2**.
- **Real incoming DPS (iframe-capped):** max hit = Verthani 10 → **20 DPS** → **TTK-player ≈ 5.0 s**.
- **Outgoing / TTC:** checkpoint 480 HP ÷ 32.7 ≈ **14.7 s**; +corridor 630 (≈19 s) +Martinez 340 (≈10 s burst, longer with phase dodging).
- **Verdict: OK.** The split-into-beats design keeps each fight ≤5 enemies, the melee
  cap (2) holds the dogpile, and the iframe keeps TTK at a forgiving 5 s. Martinez (340
  HP, dmg 15→27) is a fair gatekeeper boss for a freshly-armed player.
- **RECOMMENDED fix:** none — B1 is a well-paced tutorial-difficulty opener. (`B1 is fine`.)

---

## F1 — Atrium / Lobby (baseY 5; capture `spire_f1.png`)

- **Enemy count:** **0**. No encounter content authored on the F1 plate (it is the
  glass-curtain-wall **breather** between the B1 boss and the F2 rescue hub).
- **Incoming DPS:** 0 → **TTK-player = ∞**. **Outgoing / TTC:** 0 s.
- **Rescue timer:** none.
- **Verdict: TRIVIAL (intentionally).** A pacing breather is correct after a boss.
- **RECOMMENDED fix:** none for difficulty. *Optional polish:* if a longer beat is
  wanted, add 1–2 light scouts (e.g. 1 BlueSynth) for tension — but a true breather is a
  valid design choice. (`F1 breather is fine; optional +1 scout`.)

---

## F2 — Medical Wards / Rescue Hub (baseY 10; capture `spire_f2.png`)

- **Enemy count on the F2 plate:** **0** (no combatants authored on F2).
- **Rescue:** 3 captives (Aria / Keisha / Emily), 300 s timers, hub-gated
  (`RescueSystem`). On expiry each becomes a boss (Siren / Breeder Queen / Oracle).
- **BUG (placement):** in the current build the 3 F2 victims are **physically built in
  the B1 arena**, not on the F2 plate — `level1_game.cpp` anchors them at
  `arenaCenter ± offsets` with `kEnemyY` (B1 Y), with a code comment that the "7-floor
  Spire build will place these in wards A/B/C on F2." The canonical F2 ward centers
  (`Level1Layout::wardA/B/C`, `level1.cpp`: (4,10,-3) / (11.5,10,3) / (18,10,-3))
  already exist and are unused by the rescue build. So **F2 reads as empty in the
  capture** and the rescue objective spatially overlaps the B1 boss arena.
- **Timer reachability:** 300 s is ample for 3 wards on one plate (traversal is seconds).
  The hub-gating is correct (victims are NOT armed at load — verified by `--test-rescue`).
- **Verdict: OK (logic) / BUG (placement).** Timers are fine and winnable; the victims
  are mis-placed onto B1.
- **RECOMMENDED fix:** wire the F2 rescue build to the existing
  `layout.wardA/wardB/wardC` (F2 Y) instead of `arenaCenter` so the rescue hub renders
  on F2 where the player reaches it. (Numbers unchanged; `move F2 victims to wardA/B/C`.)

---

## F3 — Labs (baseY 15; capture `spire_f3.png`)

- **Enemy count / split:** **4** → 3 melee (2 DominionTrooper + 1 Verthani) + 1 ranged (BlueSynth). (`midFloors.plan(F3)`.)
- **HP total:** 100+100+130+150 = **480**.
- **Naive incoming DPS (all engaged):** 7.3+7.3+10+3.6 = **28.2**.
- **Real incoming DPS (iframe-capped):** max hit Verthani 10 → **20 DPS** → **TTK-player ≈ 5.0 s**.
- **Outgoing / TTC:** 480 ÷ 32.7 ≈ **14.7 s**.
- **Door:** keypad 3300 (locked, lab keycode).
- **Verdict: OK.** Identical melee/ranged shape to the B1 checkpoint — a clean
  difficulty *floor* for the mid wing. Off-spine placement (x[4,12]) means no shaft ambush.
- **RECOMMENDED fix:** none. (`F3 4 enemies is fine`.)

---

## F4 — Offices (baseY 20; capture `spire_f4.png`)

- **Enemy count / split:** **5** → 3 melee (3 DominionTrooper) + 2 ranged (1 BlueSynth + 1 Illuminated elite). (`midFloors.plan(F4)`.)
- **HP total:** 300 + 150 + 220 = **670**.
- **Naive incoming DPS:** 3×(8/1.1) + 5/1.4 + 6/0.8 = 21.8 + 3.6 + 7.5 = **32.9**.
- **Real incoming DPS (iframe-capped):** max hit = Trooper 8 (no Verthani here) → **16 DPS** → **TTK-player ≈ 6.25 s**.
- **Outgoing / TTC:** 670 ÷ 32.7 ≈ **20.5 s** (the Illuminated's 220 HP is a clear time sink).
- **Door:** keypad 4040 (door-override).
- **Verdict: OK**, but note F4 is the *softest-hitting* floor (no Verthani → max hit only
  8, so TTK-player is the **longest** of any combat floor at 6.25 s). The escalation here
  is in HP/time (the 220-HP elite) and target count, not lethality.
- **RECOMMENDED fix:** none required. *If a smoother lethality curve is wanted*, swap one
  Trooper → Verthani so F4's max-hit rises 8→10 (TTK 6.25→5.0 s) and the floor doesn't
  feel safer than F3. (`optional: F4 swap 1 Trooper→Verthani`.)

---

## F5 — R&D / Synth Bay (baseY 25; capture `spire_f5.png`)

- **Enemy count / split:** **6** → 1 melee (Verthani) + 5 ranged (3 BlueSynth + 2 Illuminated). (`midFloors.plan(F5)`.)
- **HP total:** 130 + 3×150 + 2×220 = 130+450+440 = **1020**.
- **Naive incoming DPS:** 10/1.0 + 3×(5/1.4) + 2×(6/0.8) = 10 + 10.7 + 15 = **35.7** (the highest pure-squad naive DPS of the mid wing — ranged-led, **uncapped by the melee limit**).
- **Real incoming DPS (iframe-capped):** if the lone Verthani is in melee, max hit 10 →
  **20 DPS**; if kiting at range, max hit = Illuminated 6 → **12 DPS** → **TTK-player ≈
  5.0–8.3 s**.
- **Outgoing / TTC:** 1020 ÷ 32.7 ≈ **31.2 s** (≈3 pistol mags + reloads; the 2×220 elites dominate).
- **Rescue:** 1 captive "Lena" (`AnnaTactical.glb`), 300 s, hub-gated (`m_f5HubReached`,
  default false — verified NOT armed at load by `--test-spiremid`). On expiry → 460-HP
  mini-boss. 300 s vs a ~31 s clear is **very** comfortable.
- **Door:** keypad 5500.
- **Verdict: OK**, leaning toward a *grind* (longest mid-floor TTC at ~31 s, 5 uncapped
  ranged attackers). It is **winnable** thanks to the iframe cap, but the naive 35.7 DPS
  + two 220-HP elites make it the mid-wing spike. Not unfair, but the *least* dodgeable
  (5 ranged = constant chip from all angles).
- **RECOMMENDED fix:** if F5 plays as a slog, the cleanest lever is the elite tax:
  reduce F5 ranged from 5 → 4 (drop 1 Illuminated: −220 HP, naive DPS 35.7→28.2). Timer
  is generous regardless. (`optional: F5 ranged 5→4 (drop 1 Illuminated)`.)

---

## F6 — Executive (baseY 30; capture `spire_f6.png`)

- **Enemy count / split:** **7** → 3 melee (2 DominionTrooper + 1 Verthani) + 4 ranged (2 BlueSynth + 2 Illuminated). (`topFloors.plan(F6)`.)
- **HP total:** 200 + 130 + 2×150 + 2×220 = 200+130+300+440 = **1070**.
- **Naive incoming DPS:** 2×(8/1.1) + 10/1.0 + 2×(5/1.4) + 2×(6/0.8) = 14.5 + 10 + 7.1 + 15 = **46.6** (highest naive DPS of any standard floor).
- **Real incoming DPS (iframe-capped):** max hit Verthani 10 → **20 DPS** → **TTK-player ≈ 5.0 s** (the melee cap of 2 + iframe both bite hard here).
- **Outgoing / TTC:** 1070 ÷ 32.7 ≈ **32.7 s** (two 220-HP elites again the time sink).
- **Doors:** TWO keypads — outer 6600, inner 6611 (door-override puzzle).
- **Verdict: OK.** This is the intended "hardest standard encounter" — naive DPS 46.6 is
  scary on paper, but the melee cap (2 of 3 swing) + iframe still pin real DPS at 20.
  Dense and tense without being a melt. Placement note: one Illuminated is at z=6.5
  (near the +Z wall) — verified clear of the exec-office partition.
- **RECOMMENDED fix:** none — F6 is correctly the standard-floor peak. (`F6 7 enemies is fine`.)

---

## F7 — Rooftop (the Act-1 finale; baseY 35; capture `spire_f7.png`)

- **Enemy count / split:** **8 combatants** = 1 Boss (The Clone) + 7 escort → escort is
  2 melee (1 Verthani + 1 DominionTrooper) + 5 ranged (2 Illuminated honor guard + 3
  BlueSynth). (`topFloors.plan(F7)`, `boss()`.)
- **HP total:** Clone 620 + escort (130 + 100 + 2×220 + 3×150) = 620 + 1120 = **1740**
  (+ Sarah-as-boss 500 if her rescue lapses).
- **Naive incoming DPS (boss P1 + full escort):** 14/1.05 + 10/1.0 + 8/1.1 + 2×(6/0.8) + 3×(5/1.4) = 13.3 + 10 + 7.3 + 15 + 10.7 = **56.3**.
- **Real incoming DPS (iframe-capped):** governed by the **largest single hit = the
  Clone's melee**: P1 14 → **28 DPS** (TTK 3.6 s); P2 19.6 → **39 DPS** (TTK 2.6 s);
  **P3 25.2 → 50 DPS → TTK-player ≈ 2.0 s.** This is the only floor where the iframe cap
  is genuinely dangerous, because one boss hit is large.
- **Outgoing / TTC:** 1740 ÷ 32.7 ≈ **53 s** with the **pistol alone** — a long, exposed
  fight against a 620-HP boss whose Phase 3 also summons 2 Guard adds. With plasma (56
  DPS) ≈ 31 s; with smg (85.7) ≈ 20 s — strongly favors switching off the pistol.
- **Rescue:** 1 captive "Sarah" (`AnnaCasual.glb`), 300 s, hub-gated (`m_f7HubReached`,
  default false — NOT armed at load, verified by `--test-spiretop`). 300 s vs a ~53 s
  pistol clear is reachable but the *tightest* rescue/clear ratio in the act, and the
  player must survive the boss to even approach her cell (-Z corner, behind keypad 7700).
- **Door:** keypad 7700 (rooftop airlock).
- **Verdict: TOO HARD with the pistol alone (boss P3 TTK-player ≈ 2 s vs a 53 s clear);
  OK if the player uses the arsenal.** The 8-combatant + boss-with-summons + boss high
  single-hit stack is a real spike — appropriate for a finale, but it leans on the player
  knowing to switch weapons.
- **RECOMMENDED fixes (pick one or combine):**
  1. `F7 escort ranged 5→4 (drop 1 BlueSynth)` — trims the constant chip while the player
     juggles the boss (naive DPS 56.3→52.7; HP 1740→1590).
  2. `Clone P3 dmg mul 1.8→1.5` (`phase3DamageMul`) — boss P3 hit 25.2→21, TTK-player
     2.0→2.4 s, keeping the threat without the near-instant punish.
  3. Leave the roster but **author a weapon/ammo pickup on F6/F7** so the pistol-only
     53 s clear isn't the expected path. (`add an arsenal pickup before the finale`.)

---

## Summary

### Difficulty curve B1 → F7 — does it escalate smoothly?
Counts escalate **monotonically and cleanly**: B1 checkpoint 4 → F3 4 → F4 5 → F5 6 →
F6 7 → F7 8 (the self-tests `--test-spiremid` / `--test-spiretop` assert F3<F4<F5<F6<F7).
HP-total and outgoing-TTC also climb smoothly: ~480 → 480 → 670 → 1020 → 1070 → 1740.

**But lethality (TTK-player) does NOT climb smoothly** because of two factors:
1. The global 0.5 s iframe caps real incoming DPS at `max_hit × 2`, so floors with no
   Verthani/boss are *safer* despite more enemies (F4's max hit is only 8 → TTK 6.25 s,
   the longest of any combat floor — F4 is "safer" than F3/F5/F6 even though it has more
   enemies). The standard floors (B1/F3/F5/F6) all sit at ~20 DPS / 5 s TTK-player.
2. F7 breaks the pattern hard: the Clone's large single-hit melee (25.2 in P3) pushes
   real DPS to ~50 and TTK-player to ~2 s — a 2.5× lethality jump that the pistol-only
   clear time (53 s) cannot absorb.

So the curve is **smooth in pressure/time but flat-then-spiked in lethality.** That is
mostly *fine* (a finale spike is desired) — the concern is the **pistol-only F7** combo.

### Top 3 things to fix
1. **F2 rescue victims are built in the B1 arena, not on F2** (placement bug). Re-anchor
   the rescue build to `layout.wardA/wardB/wardC` so F2 actually contains its hub.
2. **F7 finale is too hard with the pistol alone** (boss P3 TTK-player ≈ 2 s vs a 53 s
   pistol clear, plus 2 summoned adds). Either trim the escort (ranged 5→4), soften the
   Clone's P3 damage mul (1.8→1.5), or — best — place a stronger-weapon/ammo pickup
   before the finale so the 53 s pistol slog isn't the expected path.
3. **Lethality is flat across B1/F3/F5/F6** (all ~5 s TTK-player) and F4 is actually the
   *easiest* combat floor (max hit 8). For a smoother felt-difficulty ramp, nudge the
   later floors' *single-hit* size up (e.g. F4 swap a Trooper→Verthani; ensure the upper
   floors keep a Verthani in the mix) rather than just adding bodies — body count alone
   doesn't raise lethality under the iframe cap.

### Things that read as bugs / risks
- **[BUG] F2 victims spawn in the B1 arena** (see #1 above): the rescue hub is spatially
  detached from the F2 plate; F2 renders empty. Canonical ward centers exist but are
  unused. Logic is fine (`--test-rescue` green); only placement is wrong.
- **[OK — verified NOT a bug] No victim is armed at load.** The F5 ("Lena") and F7
  ("Sarah") captives and the F2 trio are all hub-gated (`m_f5HubReached` / `m_f7HubReached`
  / `RescueSystem::m_hubReached` default false). Settling 24 frames with the camera in
  each hub *does* start those clocks (logged), but at 300 s they cannot expire during a
  capture or a normal traversal. This is the explicit playtest-fix mirror in the code
  comments — confirmed correct.
- **[OK — verified NOT a bug] No enemy spawns in the elevator spine.** All Spire-floor
  placements sit in x[3,17] / |z|≤6.5, off the x=19.5 shaft doorway; the code comments
  call this out and the captures confirm it.
- **[Risk, not a bug] Stacked keypad doors.** F3/F4/F5 doors all sit at x=8 (different Y
  only); the code uses a **3D** (include-Y) proximity test to disambiguate the player's
  current floor — correct, but fragile if a future floor reuses an X/Y. F6 uses distinct
  X (14/10) and F7 x=6 to avoid ties. Worth a regression note.
- **[Risk] F7 keypad 7700 reachability.** Sarah's cell is behind the rooftop airlock
  (code 7700) in the -Z corner while the Clone boss + escort hold the helipad. The code
  is enterable (no gating bug), but a player must clear/survive the finale to reach the
  keypad — combined with the 53 s pistol clear, the *practical* window to also rescue
  Sarah (300 s) is the tightest in the act. Reachable, but tight (covered by fix #2).
- **B1 arena trigger is wide (x[16,19]).** Stepping anywhere into the arena spawns
  Martinez; benign in play, but it means the capture harness must deliberately frame the
  checkpoint room to avoid the boss filling the lens (noted, not a gameplay issue).
