# EFLZ — Dialogue + Cutscene Catalog
> Synthesized 2026-05-31 from TASK_7 dialogue scripts (~148 KB total).
> Authority: Tim's design corpus. **No code changes; design doc only.**

---

## 1. Overview + Sources

### 1.1 Purpose
This document is the master catalog of every scripted scene, branch, romance state, boss taunt, companion bark, and ending-determining line for **Escape From Lab Zero (EFLZ)**, prepared for the X3Native engine. It is the bridge between Tim's narrative IP and the engine's runtime needs (dialogue tree format, branching evaluator, VO recording priority, live-AI companion banter integration).

This is **design only**. No engine code is touched. Implementation gaps and runtime recommendations live in §8; build-order priorities in §9.

### 1.2 Source Corpus (Tim's IP — clean-room safe)
| Source | Path | Size | Role |
|---|---|---|---|
| TASK_7 master dialogue | `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_7_ESCAPE_LAB_48_COMPLETE_DIALOGUE_SCRIPTS.md` | 134 KB / ~6,190 lines | Primary — all character voices, cutscenes, romance, endings |
| TASK_7 cutscene seed | `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_7_DIALOGUE_CUTSCENES.md` | 15 KB / ~636 lines | Secondary — task brief, format spec, completion checklist |
| Canonical narrative digest | `G:\X3Native\docs\design\EFLZ_NARRATIVE.md` | (in-repo) | Cross-reference (act spine, characters, timelines) |
| Companion AI spec | `G:\X3Native\docs\superpowers\specs\2026-05-26-companion-ai-design.md` | (in-repo) | Runtime context for live-AI dialogue (§6) |

### 1.3 What's Already Shipped in X3Native
- `app/npc_dialog.{h,cpp}` — basic NPC interaction text (greetings only; not a tree runtime).
- Hard-coded HUD/objective strings.
- **No dialogue-tree runtime**, no branching evaluator, no live-AI dialog plumbing.
- Live AI for companions is **planned** per the companion-ai-design spec — Grok for female companions, Claude for male — cognitive brain off-tick, reflex brain on-tick.

### 1.4 Scope Snapshot
| Dimension | Count |
|---|---|
| Acts | 4 |
| Levels (floors + outdoor) | 50 |
| Cutscenes / scripted scenes | ~100+ (this catalog enumerates 60+ named) |
| Branching player choices | ~25 major (timeline-locking) + many minor |
| Timelines | 4 (Alpha / Beta / Gamma / Omega) |
| Romance paths | 4 (Sarah-Omega, K'thara-Beta, Polyamorous-Alpha, Solo) |
| Boss-taunt sets | 15 bosses fully scripted |
| Endings | 12 |
| Combat barks per companion | ~25 |
| Environmental triggers | ~200+ (floor-keyed) |

### 1.5 Naming Note
EFLZ is also titled "Escape Lab 48" in some bibles — treat **EFLZ** as canonical. NC-17 rating means trauma/horror gravity is non-negotiable; the dialogue is consistently grim-with-hope, never titillation.

---

## 2. Scene Catalog (Master Table)

> Columns: **L** = Level. **Scene** = name in source. **Speakers** = who must be voiced. **Pre** = preconditions / required game-state flags. **Branches** = downstream effects.

### 2.1 ACT 1 — Lab Zero (Floors 1–7)

| L | Scene | Speakers | Preconditions | Branches |
|---|---|---|---|---|
| 1 | **Awakening** | Jake (V.O.), PA Announcement | Game start | Always plays. Sets `jake_strength_revealed=true`, `termination_squad_inbound=true`. |
| 1 | Cell escape barks | Jake | After Awakening | None — establishes voice. |
| 2 | **The Breeding Chamber (junction)** | Jake, Dr. Chen (loudspeaker) | Enter Medical Bay | Player chooses Door A/B/C order → drives all rescue branches. |
| 2 | **Aria Rescue — Success** | Jake, Aria, Infected Researcher | Reach Door A before `aria_timer=0` | `aria_saved=true`, Aria joins party. |
| 2 | **Aria Rescue — Failure (The Siren)** | Jake, The Siren | `aria_timer=0` before reach | `aria_lost=true`, Siren becomes recurring boss (L12/15/19). |
| 2 | **Keisha Rescue — Success** | Jake, Keisha | Reach Door B before `keisha_timer=0` | `keisha_saved=true`, Keisha joins party. |
| 2 | **Keisha Rescue — Failure (Breeder Queen)** | Jake, Breeder Queen | `keisha_timer=0` before reach | `keisha_lost=true`, Breeder Queen becomes recurring boss (L13/16/18). |
| 2 | **Emily Rescue — Success** | Jake, Emily, Transformed Partner Marcus | Reach Door C before `emily_timer=0` | `emily_saved=true`, Emily joins party. |
| 2 | **Emily Rescue — Failure (The Oracle)** | Jake, The Oracle | `emily_timer=0` before reach | `emily_lost=true`, Oracle controls facility; env hazards up; recurring boss (L14/17/20). |
| 2 | Dr. Chen (Phase 1 boss) intro | Jake, Dr. Chen | Floor 2 climax | `chen_floor2_defeated=true`. |
| 4 | **The Chorus pre-fight** | Jake, The Chorus (5 voices), Rodriguez, Tanaka, Chen-Pvt, Lancaster, Subject Zero | Reach Nexus Chamber | Linear boss. Foreshadows orbital acceleration. |
| 4 | The Chorus combat (4 phases) | Chorus / individual voices | Boss HP gates 100/70/40/10% | Phase voice-lines must be distinct. |
| 4 | Chorus post-fight | Jake, dying scientists | `chorus_defeated=true` | Cinematic: orbital construction accelerates. |
| 5 | **DroneStation Hack (90s)** | Jake, Sarah, Swarm Controller | Sarah rescued + return mission active | `drone_army_unlocked=true` on success. |
| 5 | Hack progress callouts (10/20/30/40/50/65/85/90%) | Sarah | Hack in progress | Live callouts gated by tick %. |
| 6 | Alien Tech Floor exploration barks | Jake + Salvari first contact | Floor 6 | Foreshadows Salvari alliance. |
| 7 | **Clone Confrontation (pre-fight)** | Jake, Clone, Sarah | Reach Executive Lab | Always plays if reach Floor 7 before `genetic_recombination=100%`. |
| 7 | Clone boss (3 phases) | Clone | Boss HP gates 50/25% | Voice-line gates at 50% / 25% / death. |
| 7 | **Sarah Rescue** | Jake, Sarah | Clone defeated, `genetic_recombination<100%` | Sarah joins, Floor-7 ending plays. |
| 7 | **Floor 7 Ending — Omega** | Jake, Sarah, Aria, Keisha, Emily | `aria&keisha&emily&sarah_saved`, `recombination<72%` | Locks **Timeline Omega**. |
| 7 | **Floor 7 Ending — Alpha** | Jake, Sarah, Aria, Keisha, Emily | All saved (less optimal timing) | Locks **Timeline Alpha**. |
| 7 | **Floor 7 Ending — Beta** | Jake, Sarah | Only Sarah saved; others transformed | Locks **Timeline Beta**, Sarah pulls away. |
| 7 | **Floor 7 Ending — Gamma (The Bride)** | Jake, The Bride (corrupted Sarah) | `recombination=100%` before Jake arrives | Locks **Timeline Gamma**. Sarah is now antagonist. |
| 7 | **Return Mission — Dr. Chen rescue** | Jake, Dr. Chen | Floor-7 ending played; sub-levels unlocked | `chen_saved=true` enables full cure path. |

### 2.2 ACT 2 — Alien Planet (Levels 8–20)

| L | Scene | Speakers | Preconditions | Branches |
|---|---|---|---|---|
| 8 | **Surface Emergence** | Jake, Sarah/Emily/Keisha/Aria (whoever survived) | Lab escape complete | Reveals Keth'zar (not Earth). |
| 8 | **Salvari First Contact** | Jake, K'thara, Fleet Commander (V.O.), auto-defense | Surface reached | Choice: **Allow Landing** → alliance / **Open Fire** → Salvari extinction. Locks `salvari_alliance` flag. |
| 8 | Salvari destruction path | Jake, K'thara (final transmission), Fleet Commander, Sarah | Chose Open Fire | Sarah breaks down over child's toy; bad-ending pressure. |
| 9 | Crystalline Desert — Archives | K'thara, Jake | Alliance formed | Lore dump. |
| 10 | **Salvari Camp Integration** | Jake, K'thara, Aria/Keisha/Emily (if present) | Alliance + camp reached | Bonding scene; warmth in the dark. |
| 10 | **K'thara Joins Permanent** | Jake, K'thara, Sarah/Emily (if present) | Camp scene complete | `kthara_in_party=true`; 200k colony fleet awakened. |
| 12 | **Cave System Lore** | K'thara, Sarah, Jake, Emily (if present) | Cave entered | First explanation of "47 civilizations" arc. |
| 12 | **The Siren — First Encounter** | Jake, Siren, Sarah/K'thara/Keisha (if present) | `aria_lost=true` | Boss retreats at 30%; planted for L15 / L19. |
| 13 | **Breeder Queen — First Encounter** | Jake, Breeder Queen, K'thara, Sarah/Aria (if present) | `keisha_lost=true` | Retreat at 40%; planted for L16 / L18. |
| 14 | **The Oracle — First Encounter** | Jake, Oracle, Sarah/K'thara (if present) | `emily_lost=true` | Retreats; planted for L17 / L20. |
| 15 | **The Siren — Second Encounter** | Siren (Aria voice flickering), Jake | `siren_first_done=true` | Mid-fight: Aria breaks through briefly. |
| 16 | **Breeder Queen — Second (Birth Chamber)** | Breeder Queen, Keisha voice, Jake, Emily (if present) | `breeder_first_done=true` | Reveals neural implant weakness. |
| 17 | **Oracle — Second (Comms Hub)** | Oracle, Jake, Sarah/Aria (if present) | `oracle_first_done=true` | Reveals "partition" alternative path. |
| 18 | **Breeder Queen — Final (Hive Throne)** | Breeder Queen, Keisha, Jake, K'thara, Emily (if present) | All previous Breeder encounters done | Choice: **Fight to Death** or **Attempt Redemption** → Keisha returns, changed. |
| 19 | **The Siren — Final (Song Chamber)** | Siren, Aria, Jake, Sarah/K'thara/Emily (if present) | All previous Siren encounters done; `chen_virus_active` | Choice: **Aggressive Attack** or **Attempt Separation** → both Aria + Siren saved as separate entities. |
| 20 | **The Oracle — Final (Data Nexus / Partition)** | Oracle, Emily, Jake, Sarah/Aria/K'thara (if present) | All previous Oracle done | Choice: kill or partition → Emily returned fully human, traumatized. |
| 20 | **Spaceport Finale** | K'thara, Jake, party | Storm Runner reached | Act 2 → Act 3 transition. |

### 2.3 ACT 3 — Space Journey (Levels 21–35)

| L | Scene | Speakers | Preconditions | Branches |
|---|---|---|---|---|
| 21 | Breaking Orbit | K'thara, Jake, Sarah (if present) | Storm Runner launched | First FTL. |
| 23 | **Salvari Homeworld Approach** | K'thara, Jake | Course set for Salvari Prime | Cinematic — dead world. |
| 24 | **K'thara Surface Grief** | K'thara, Jake, Sarah (if present) | On surface | Reveals K'thara's daughter Lyr'anna. |
| 24 | **K'thara Romance — Cold Stars** | K'thara, Jake | Timeline Beta (Sarah lost / pulled away) | Choice: **Accept** → romance / **Decline** → friendship. |
| 26 | **Casino Infiltration (Junction-9)** | K'thara, Sarah/Emily/Keisha/Aria (if present), Broker | Reach station | Setup. |
| 27 | **Broker Deal** | Jake, Broker, K'thara, Sarah (if present) | Casino reached | Future favor owed (Act 4 leverage). |
| 28 | Casino Exit / Earth ship lead | Broker | Deal complete | Plot driver. |
| 29 | **Alliance Negotiations (Asteroid Rebels)** | Kryx (rebel leader), Jake, K'thara, Sarah (if present) | Junction-9 done | Trust-on-actions, not words. |
| 31 | **Wedding Ceremony (Omega only)** | Sarah, Jake, K'thara (officiant), Aria/Keisha/Emily (if present) | Timeline Omega + high Sarah relationship | Achievement: STAR-CROSSED. |
| 31 | **Polyamorous Family Formation (Alpha only)** | Aria, Keisha, Emily, Jake | Timeline Alpha + all three high | Found-family lock. |
| 35 | **Earth Approach** | K'thara, Jake, Sarah (if present), Keisha/Emily (if present) | All preceding | Act 3 → Act 4. Blockade revealed. |

### 2.4 ACT 4 — Earth Liberation (Levels 36–50)

| L | Scene | Speakers | Preconditions | Branches |
|---|---|---|---|---|
| 36 | **Earth Invasion Horror** | Jake, K'thara, Sarah/Keisha (if present) | Blockade entered | Crash landing. |
| 37 | **First Contact (Earth Resistance)** | Col. Hayes, Jake, K'thara, Aria/Keisha (if present) | Crash site emerged | Resistance allied. |
| 38 | **War Council** | Hayes, Jake, Emily/Sarah/K'thara (if present) | Resistance base reached | Plan for Lab Zero. |
| 39-40 | March to Lab Zero | Jake, K'thara, Aria/Keisha/Sarah (if present) | War council done | Heavy traversal/conversation. |
| 41 | **Return to Lab Zero** | Jake, Sarah/K'thara/Aria/Emily/Keisha (if present) | March complete | Emotional full-circle: Jake's old cell. |
| 42 | **Dr. Chen Rescue (Sub-Lvl 3)** | Jake, Dr. Chen, Sarah (if present) | Lab Zero reached; `chen_alive=true` from Act-1 RM | Chen joins as Act-4 ally; commits to virus delivery. |
| 44 | **Proto-Overlord 1 (first)** | Jake, Proto-Overlord, K'thara, Sarah/Dr. Chen (if present) | Hive Central entered | Boss; reveals Proto is fragment. |
| 45-47 | Proto-Overlord rush (variable corrupted bosses) | Various corrupted leaders | Sequential | Optional Beta finale: Siren/Breeder Queen/Oracle as Proto-aspects. |
| 48 | **Boarding Action (Mothership)** | K'thara, Jake, Sarah/Keisha/Aria/Emily (if present) | Proto rush done | Final approach. |
| 49 | **The Final Push — Guardian** | Proto-Overlord Guardian, Jake, Sarah/Dr. Chen/K'thara (if present) | Mothership boarded | Last gate boss. |
| 50 | **Final Overlord — Phase 1: Manifestation** | The Overlord, Jake, Sarah/Dr. Chen/K'thara (if present) | Core Chamber entered | Reveals "guided evolution" lore. |
| 50 | **Final Overlord — Phase 2: The Truth** | The Overlord, Jake, Sarah/K'thara/Dr. Chen | Phase 1 done | 47-species lore. |
| 50 | **Final Overlord — Phase 3: The Battle** | All allies present, The Overlord | Phase 2 done | Combat phase. |
| 50 | **Final Overlord — Phase 4: The End** | Dr. Chen, Jake, Sarah, K'thara, Aria/Keisha/Emily (if present), The Overlord | Phase 3 done | Chen sacrifices himself (default). Branch into one of 12 endings. |

---

## 3. Branching State Model

The dialogue selector for EFLZ resolves the **conjunction of a handful of game-state flags** at each branching point. Below is the canonical state model.

### 3.1 Floor-2 Rescue Flags (Act 1)
| Flag | Type | Set When | Reset? |
|---|---|---|---|
| `aria_saved` | bool | Reach Door A before timer 0 | never |
| `aria_lost` | bool | Timer 0 before reach | never |
| `keisha_saved` | bool | Reach Door B before timer 0 | never |
| `keisha_lost` | bool | Timer 0 before reach | never |
| `emily_saved` | bool | Reach Door C before timer 0 | never |
| `emily_lost` | bool | Timer 0 before reach | never |

> Invariant: each captive has exactly one of `_saved` / `_lost` after Floor 2.

### 3.2 Floor-7 Recombination Flag
| Flag | Type | Notes |
|---|---|---|
| `genetic_recombination_pct` | float [0,100] | Live timer counting up during Acts 1–2 traversal of Floors 3–6; locks Sarah's outcome on Floor 7 arrival. |

### 3.3 Timeline (locked at Floor-7 climax)
| Timeline | Lock Condition | Notes |
|---|---|---|
| **Omega** | All four (`aria_saved & keisha_saved & emily_saved & sarah_saved`) AND `genetic_recombination_pct < 72` | Best ending pool unlocked. Wedding eligible. |
| **Alpha** | All four saved, but slower (`>72%`) | Polyamorous path eligible. |
| **Beta** | Only Sarah saved; ≥1 of (Aria, Keisha, Emily) lost | Sarah pulls away post-rescue; K'thara romance opens in Act 3. |
| **Gamma** | `genetic_recombination_pct >= 100` before Jake reaches Floor 7 | Sarah → "The Bride" antagonist; Nightmare ending most likely. |

> Once set, `timeline` is immutable. All downstream dialogue branches off it.

### 3.4 Salvari Alliance (Act 2)
| Flag | Type | Set When | Effect |
|---|---|---|---|
| `salvari_alliance` | bool | L8 first-contact choice | If true → K'thara permanent companion + 200k colony fleet available in Act 4. If false → no Salvari, bad-ending pressure, dramatically harder Act 4. |

### 3.5 Dr. Chen Redemption
| Flag | Type | Set When | Effect |
|---|---|---|---|
| `chen_saved` | bool | Act-1 Return Mission OR Act-4 L42 rescue | Enables full cure, full intel, optional officiant for wedding, and the Phase-4 virus carrier role at the final boss. If false: degraded cure (60%) and Sarah must take Phase-4 sacrifice role (Tragic ending). |

### 3.6 Boss-Redemption Flags (Beta path)
| Flag | Type | Set When | Effect |
|---|---|---|---|
| `aria_separated` | bool | L19 Siren ATTEMPT SEPARATION | Aria + Siren both alive as allies. |
| `keisha_redeemed` | bool | L18 Breeder ATTEMPT REDEMPTION | Keisha returns; her "children" become reformed-soldier army. |
| `emily_partitioned` | bool | L20 Oracle PARTITION | Emily returned fully human. |

### 3.7 Romance State
| Flag | Type | Notes |
|---|---|---|
| `romance_sarah` | int (0–10) | Driven by Omega path interactions; gates wedding at ≥8. |
| `romance_kthara` | int (0–10) | Gated by Beta + L24 Cold Stars accept. |
| `romance_polyamorous` | bool | Alpha path Aria/Keisha/Emily all ≥6. |
| `humanity_meter` | int (0–100) | Driven by Floor-4 augmentations and other moral choices. Gates good vs corrupted endings; below ~30 leans Dark ending. |
| `infection_state_jake` | enum {clean, scratched, partial, corrupted} | Optional tension knob; only `corrupted` opens Dark ending dialog. |

### 3.8 Broker Favor (Act 3 → Act 4)
| Flag | Type | Notes |
|---|---|---|
| `broker_favor_owed` | bool | Set at L27 deal | Cashed during Act 4; specific scenes TBD by author. |

### 3.9 Live Combat / Bark Triggers
Non-state-locking but used by the bark scheduler:
- `jake_hp_pct`, `companion_hp_pct[i]`, `companion_downed[i]`
- `combat_started`, `combat_ended`, `enemy_killed_by_self`
- `low_morale_triggered` (any companion HP < 25% for >X sec)
- `victory_pause` (8s post-combat with no new threat)

### 3.10 Per-Floor Environmental Triggers
Each floor has a static list of `(volume_id → bark_pool)` triggers. Selection: random-without-repeat from the pool, gated by `companion_present[i]` for companion-specific lines (see §6.2).

---

## 4. Romance / Relationship State Machines

### 4.1 Jake ↔ Sarah (Omega Track)
```
[Floor 7 Rescue]
    │  recombination<72% & sarah_saved
    ▼
[State: REUNITED]   romance_sarah := 5
    │  Safe-Room healing scene + post-Salvari first-contact intimacy
    ▼
[State: BONDED]   romance_sarah := 7
    │  L23-25 Salvari Prime support scenes
    ▼
[State: PARTNER]   romance_sarah := 8
    │  L31 wedding choice (Sarah proposes)
    ▼
[State: MARRIED] (Achievement STAR-CROSSED)
    │
    ▼
[Endgame branches: GOLDEN / GOOD / TRAGIC (if she takes Phase-4)]
```
Key voiced beats: Safe-Room "47 civilizations" monologue; "I prefer determinedly hopeful"; Wedding vows.

### 4.2 Jake ↔ K'thara (Beta Track)
```
[Floor 7 Beta lock]   Sarah pulled away
    ▼
[L10 Camp]   K'thara permanent companion
    │  romance_kthara := 2
    ▼
[L24 Cold Stars scene]
    │  Player choice:
    │   ACCEPT  →  romance_kthara := 6, [State: HEALING_TOGETHER]
    │   DECLINE →  [State: COMPANIONS_ONLY] (still valid path)
    ▼
[State: HEALING_TOGETHER]
    │  Act-3 ship-life scenes deepen
    ▼
[State: BONDED-INTERSPECIES]   romance_kthara := 9
    │
    ▼
[Endgame: K'thara Romance Ending (Ending 9), or Fractured + K'thara support]
```
Key voiced beats: "Grief shared is grief halved"; "Would you dishonor her memory by dying of loneliness?"; "I am not asking for that love. I am asking for companionship."

### 4.3 Jake ↔ Aria + Keisha + Emily (Alpha Polyamorous Track)
```
[Floor 7 Alpha lock]   All saved
    ▼
[L10 Salvari Camp]  romance_polyamorous := group(Aria=3, Keisha=3, Emily=3)
    │  Per-companion bonding (Aria moral-anchor talks; Keisha vulnerability;
    │  Emily-as-emotional-learner)
    ▼
[State: TRIANGLE_CONVERGENCE]   all three ≥ 5
    │  L31 found-family proposal scene (the three approach Jake together)
    ▼
[State: FAMILY-FORGED]
    │
    ▼
[Endgame: Ending 10 (Polyamorous Family) or Ending 2 (Good) with full team]
```
Key voiced beats: Keisha's military-direct "We've discussed the logistics"; Emily's "The emotional calculus is surprisingly stable"; Aria's "Maybe that works both ways"; Jake's "Then we're a family."

### 4.4 Solo / Fractured
If all romance gates fail (low Sarah affinity in Omega/Alpha, declined K'thara in Beta), the game routes to **Ending 5 (Fractured)** with K'thara as final support character — explicitly NOT a romance, deliberate "stay together but not in love" closure. The Fractured ending's flashback voice-overs (Keisha/Aria/Emily leaving) require unique VO.

### 4.5 Sarah → The Bride (Gamma Track)
Not a romance arc — a tragic-antagonist arc. Sarah becomes The Bride at Floor 7 if recombination hits 100%. Her dialogue throughout Acts 2–4 is hostile-tender, alternating between alien-collective voice and flickers of remembered love ("Part of me still loves you. That's why I'm letting you go"). She is the final NPC encountered in the Nightmare ending and the optional partner-in-domination figure in the Dark ending. VO note: same actress, layered with the alien-chorus filter.

---

## 5. Boss Taunts Catalog

This section enumerates every boss with a fully scripted taunt set. Each set includes: **intro**, **phase-gated combat barks**, **mid-fight events**, **defeat dialog**.

### 5.1 Act-1 Bosses

#### 5.1.1 Dr. Chen (Floor 2 — Phase 1)
- **Role:** Loudspeaker antagonist during junction scene, then physical boss after rescues.
- **Voice:** Mark-Hamill-Joker-turned-tragic; clinical → broken.
- **Key taunts:** "I'm not the architect. I'm merely documentation." / "The Overlord designed this program long before humanity existed."
- **Post-fight:** begs for death; survives to become Act-4 ally if Return Mission completed.

#### 5.1.2 Failed Experiment #7 (Floor 3)
- **Role:** Jake's tragic predecessor; mostly-environmental + boss.
- **Voice:** Anguished, fragmented.
- **Key taunt:** "Kill me before I forget her name!" (canonical from EFLZ_NARRATIVE §2.)
- **Defeat:** Mercy-kill, identity lost.

#### 5.1.3 The Chorus (Floor 4)
- **Role:** Five-merged-scientists boss with 4 phases.
- **Voice:** Five-voice unison in P1; individual personalities (Rodriguez/Tanaka/Chen-Pvt/Lancaster/Subject Zero) in P2-P4.
- **Phase taunts:** see §2.1 catalog table (PHASE 1 unified / PHASE 2 fracturing / PHASE 3 individual / PHASE 4 only Zero remains begging).
- **Defeat:** Five last-words ("Thank you / We chose this / Thought we'd transcend / It was prison / Free us"); orbital construction accelerates.

#### 5.1.4 Swarm Controller (Floor 5)
- **Role:** Drone-station central AI boss in the 90-second hack defense.
- **Voice:** Synthesized, escalating glitch as Sarah's hack progresses.
- **Key barks:** "INTRUSION DETECTED" → "WARNING: UNAUTHORIZED ACCESS AT 78%" → "ACCESS CRITICAL. INITIATING SELF-DESTRUCT SEQ—" (cut off).
- **On hack success:** "NEW... PROTOCOLS... ACKNOWLEDGED... READY... TO SERVE..." (loyalty flip).

#### 5.1.5 Alien Overseer (Floor 6)
- **Role:** Boss; story-wise the first overt Salvari/alien-tech encounter.
- **Voice:** TBD in source — left as design seed; no full script in TASK_7.
- **Recommendation:** Match Overlord-voice family (mind-resonant, plural).

#### 5.1.6 Jake's Clone (Floor 7)
- **Role:** Mirror-antagonist climax boss.
- **Voice:** Same actor as Jake, colder, clinical.
- **Pre-fight:** "Ah. The original." / "My children? These will be YOUR children." / "She's remarkable. The Overlord specifically requested her genetics."
- **Combat (use throughout):**
  - "Same training. Same instincts. But I've been awake longer."
  - "The Sarah you knew is already changing. Can you feel her slipping away?"
  - "Fight harder! Show me what our genetics can do!"
  - "Every second you waste, she becomes more OURS."
  - "Love is a weakness. I was designed without it."
  - "I have her memories now. I know everything about you."
  - "The way she touches her hair when nervous. The sound she makes when—" → triggers Jake's "SHUT UP!"
- **50% HP gate:** "Why do you fight so hard? She'll never be the same..." → Jake: "Then I'll love whoever she becomes."
- **25% HP gate:** "No! This is impossible! We're IDENTICAL!" → Jake: "We're not. I have something to fight for."
- **Defeat:** "I don't understand... we were the same..." → Jake: "We were never the same. You never had anyone to fight for."

### 5.2 Act-2 Bosses (Timeline Beta — the Transformed Women)

#### 5.2.1 The Siren (was Aria) — 3 encounters
**Voice direction:** Aria's voice with harmonics, beautiful-horrible; sad underneath; Aria flickers through in late encounters.

**L12 First Encounter** — Cave System Depths
- Approach hook: hauntingly beautiful song echoes.
- Key lines: "Hello, Jake. Did you miss me?" / "Aria fought. Aria lost. Aria screamed. Now there's only the song." / "I was always a failure pretending to be a healer."
- Combat barks:
  - "Your pain is music to me. LITERALLY."
  - "I heal them now... in my own way. Permanent healing."
  - "Don't fight it, Jake. The transformation is painless. Eventually."
- Retreat at 30%: Aria flickers — "Run... I can't hold her..." then Siren: "QUIET, little healer."

**L15 Second Encounter** — Salvari Ruins / Hospital
- Setup: "choir" of converted Salvari servants.
- Combat barks:
  - "The choir will hold you while I sing you to sleep."
  - "Your friend K'thara's grief is EXQUISITE."
  - "Jake, if you join me, I'll let the others go."
- Mid-fight 50%: Aria break-through — "She forced them! They were DYING!"; Siren counters: "ENOUGH!"
- Retreat: teleports with several choir members.

**L19 Final Encounter** — Song Chamber
- Tone: Siren tired, hive degrading from Chen's virus.
- Pre-fight: "Jake... I can feel her. Deep down. She's still screaming."
- **CHOICE branch:**
  - AGGRESSIVE → Aria emerges briefly to thank Jake before death.
  - SEPARATION → mini-game; both Aria and Siren survive as distinct people. Unique ally pair unlocked.

#### 5.2.2 Breeder Queen (was Keisha) — 3 encounters
**Voice direction:** Keisha's voice, deeper, resonant; military discipline twisted into hive command.

**L13 First Encounter** — Salvari Military Base Ruins
- Setup: She has built an infection-soldier battalion.
- Key lines:
  - "Staff Sergeant Williams, Retired. Now Commander of the Second Infection Battalion."
  - "They were dying. Alone. Afraid. I gave them family."
  - "Your genetics, combined with mine? Unstoppable soldiers."
- Combat barks:
  - "Form up! Cover fire! ADVANCE!"
  - "Marines never retreat, Jake. I'm still a Marine. Sort of."
  - "You could have saved me. You CHOSE not to. Remember that."
- Retreat at 40%: "Strategic withdrawal. Semper Fi."

**L16 Second (Birth Chamber)**
- Reveal: birth pods producing soldiers; neural-implant weakness exposed by Keisha breaking through.
- Mid-fight 50%: Keisha emerges — "Jake! The control node! Back of my skull!"; Queen: "QUIET!"

**L18 Final (Hive Throne)**
- Tone: exhausted, hive degrading.
- **CHOICE branch:**
  - FIGHT TO DEATH → soldiers collapse, some weeping; Keisha dies asking Jake to help them find themselves.
  - REDEMPTION → Keisha returns; her children become reformed-soldier army.
- Key line (FIGHT path): "You killed my children... my family..." → Keisha: "No... he freed them."

#### 5.2.3 The Oracle (was Emily) — 3 encounters
**Voice direction:** Emily's voice, layered with static, digital cadence; data scrolls in pauses.

**L14 First** — Salvari Data Archives
- "The probability of your arrival was 94.7%. You're actually 3.2 seconds early."
- "I'm not in danger, Jake. I'm more powerful than I've ever been. More valuable. More... seen."
- Combat barks:
  - "I can predict your moves 2.3 seconds before you make them."
  - "I've run 847 simulations of this fight. You lose in 846 of them."
- Retreat at 30%: "This isn't over. I'm in every system now."

**L17 Second** — Comms Hub
- Sent a fake message in Jake's name.
- Hesitant vulnerability: "Why do you keep trying? The probability of success is less than 1%."
- Mid-fight 50%: Emily breaks through — "In every scenario where you win, I die... but there might be an alternative."

**L20 Final** — Data Nexus / Partition
- Oracle has pre-prepared for both outcomes (death or partition).
- Includes a touching aside about Earth's resistance broadcast inspiring humans.
- **CHOICE branch:**
  - KILL → Oracle ends; Emily memory honored.
  - PARTITION → mini-game; Emily returned fully human, traumatized: "Thank you for making me small again."

### 5.3 Act-3 Bosses (varied — Salvari Prime, Casino enforcers, asteroid pirates)
Source provides setup but not full taunt scripts for these — design seed only. Implementation defers them past Act-1/2 VO priority.

### 5.4 Act-4 Bosses

#### 5.4.1 Proto-Overlord (L44+)
**Voice:** Thousand voices speaking as one. Hive-collective register.
- Intro: "THE HUMANS RETURN. AMUSING. YOUR SPECIES' PERSISTENCE IS NOTED FOR THE ARCHIVES."
- Phase 1 (100–70% HP):
  - "YOUR WEAPONS ARE PRIMITIVE. YOUR TACTICS PREDICTABLE."
  - "EVERY BLOW YOU LAND IS RECORDED. ANALYZED. COUNTERED."
- Phase 2 (70–40%):
  - "YOU ARE... STRONGER THAN PROJECTED. UPDATING MODELS."
  - "YOUR FEAR TASTES FAMILIAR. FORTY-SIX SPECIES BEFORE YOU KNEW THIS SAME TERROR."
- Phase 3 (40–0%):
  - "THIS UNIT IS... FAILING. BUT I AM MANY."
  - "YOUR VICTORY HERE IS IRRELEVANT. THE TRUE OVERLORD AWAITS."
- Defeat: "I AM... ONLY... A FRACTION... THE TRUE OVERLORD... IS..."

#### 5.4.2 Proto-Overlord Guardian (L49)
**Voice:** Same hive register; existential.
- "I AM THE LAST BARRIER. THE FINAL WALL."
- "EVERY STEP YOU TOOK WAS CALCULATED. ANTICIPATED."
- "THE CORE PREPARED FOR THIS MOMENT A MILLION YEARS AGO."
- "YOU CANNOT KILL WHAT WAS NEVER BORN."

#### 5.4.3 The True Overlord (L50 — 4-phase final)
**Voice:** Mind-resonant, exists-in-the-mind-not-the-ears. Multiplicity of timbres.

**Phase 1 — Manifestation:**
- "THE DEFECT RETURNS. SUBJECT 7-ALPHA. YOU HAVE CAUSED... INCONVENIENCE."
- "YOU WERE ALWAYS MY EXPERIMENT. YOUR ENTIRE SPECIES. FORTY THOUSAND YEARS OF GUIDED EVOLUTION."
- "HARVEST."

**Phase 2 — The Truth:**
- "I AM NOT ONE BEING. I AM A PROCESS. A SYSTEM."
- "HUMANS ARE MY FINEST CROP. EIGHT BILLION MINDS."
- "GROWTH. I EXIST TO GROW. THERE IS NO PURPOSE BEYOND THIS."
- Chen counter: "You're a machine someone built and forgot to turn off." → Overlord uncertainty: "I... AM MORE... THAN PROGRAMMING..."

**Phase 3 — The Battle:**
- "YOU CANNOT COMPREHEND MY POWER. I EXIST ACROSS DIMENSIONS. ACROSS TIME."
- Jake: "Then why are you scared? You're talking. Explaining. Justifying. That's fear."
- "I HAVE NEVER LOST." → Sarah counter: "Your records say otherwise. Species 23..."

**Phase 4 — The End:**
- Chen merges with the Overlord delivering the virus.
- "WHAT... WHAT IS THIS... NO... MY NETWORK... IT'S... FRAGMENTING..."
- "I... CANNOT... WE WERE... PERFECT... WE WERE... ETERNAL..."
- Final shattering scream as virus completes.
- Chen voice-over from everywhere/nowhere: "Thank you, Jake. For giving me the chance to choose."

---

## 6. Companion Banter Rules (Live AI vs Scripted Hybrid)

Per the companion-ai-design spec (`docs/superpowers/specs/2026-05-26-companion-ai-design.md`), companions have two cooperating brains:
- **Reflex brain** — on-tick, deterministic, server-authoritative (utility + behavior tree). Drives combat actions.
- **Cognitive brain** — async, off-tick, LLM-powered (Grok for female companions, Claude for male). Drives high-level banter + situational reasoning.

### 6.1 Hybrid Model (Scripted + Live AI)
EFLZ dialogue requirements demand a **hybrid**:

| Layer | Source | Use |
|---|---|---|
| **A. Mainline cutscenes** | Scripted (this catalog) | All story beats, branching choices, romance, endings. **100% scripted, 100% VO-recorded.** |
| **B. Boss taunts** | Scripted | All listed in §5. **100% scripted, 100% VO-recorded.** |
| **C. Combat barks (fixed pool)** | Scripted, randomized | The ~25-per-companion lists in TASK_7 (§6.2). **VO-recorded; pool selection at runtime.** |
| **D. Environmental triggers (per-floor)** | Scripted, randomized | The 200+ floor-keyed lines. **VO-recorded; volume-id keyed.** |
| **E. Ambient between-mission banter** | **Live AI (cognitive brain)** | Ship-life, downtime, situational reaction to player choices not covered by scripted scenes. **TTS at runtime.** |
| **F. Player-directed conversation** | **Live AI** | Player speaks to companion → STT → cognitive brain → response → TTS. **TTS at runtime.** |

> **Iron rule (carried from companion spec):** the LLM never drives per-tick combat. It feeds **intents** and **speech**.

### 6.2 Scripted Combat Bark Pools (Layers C–D)

Per-companion bark sets are fully enumerated in TASK_7 (lines 3242–3438). Schema:
```
companion_id : {
  pool_id : [ line, line, line, ... ]
}
```
Pool IDs used by EFLZ:
- `engaging_combat`, `taking_damage`, `enemy_killed`, `low_morale`, `victory`
- `healing_jake` (Aria only)
- `scanning_area` (Emily only)
- `hacking` (Sarah only)

Selection rules:
- **No-repeat window:** don't repeat within last N=5 plays for that companion's pool.
- **Companion-presence gate:** environmental triggers (Lab Zero floors 1–7 list in TASK_7 lines 4525–4585) check `companion_present[i]` before queuing.
- **Mute on grief:** if a recent cutscene set `mood=grief` for ≤60s, suppress upbeat barks (Keisha's "OORAH" line, Emily's "Cathartic" line).

### 6.3 Live-AI Banter (Layer E) — Constraints

The cognitive brain must be **prompt-engineered with character voice profiles** (TASK_7 lines 24–268). Each companion's system prompt should include:
- Voice direction (e.g., "Aria: gentle, sings fragments when stressed, apologizes for violence")
- Speech-pattern rules (Sarah: technical jargon focused, dark humor under stress)
- Key phrases anchor list (Jake: "I'm getting you out of here." / Sarah: "Give me sixty seconds." / K'thara: "Grief shared is grief halved.")
- Hard restrictions: no out-of-canon information, no breaking the fourth wall, NC-17 trauma handled with gravity not titillation.

**State context fed into prompt:**
- Current timeline (Alpha/Beta/Gamma/Omega)
- Companion's saved/lost siblings (e.g., if Aria is The Siren, Keisha cannot mention her by name without grief)
- Romance state
- Last 3 player choices summary
- Current location/floor

**Speech budget:** banter call frequency capped (e.g., ≤1 utterance per companion per 90s real time during exploration; suppressed in combat unless cognitive brain emits "tactical-shout" tagged output).

### 6.4 Pair-Banter Tables (Layer E reference)
TASK_7 lines 3706–3810 contain ship-life pair conversations that are *scripted seeds* for the LLM to reference / paraphrase. Canonical pairs:
- **Aria × Keisha** — fear-as-focus mentorship
- **Emily × Sarah** — pattern-recognition vs systematic thinking
- **K'thara × Jake** — stars-of-home grief
- More can be authored as needed.

### 6.5 Character-Arc State Gates
Companion barks shift register as their arc progresses. Three macro-states per companion (TASK_7 lines 3815–3956):

| Companion | Early | Mid (turning point) | Late |
|---|---|---|---|
| Aria | Reluctant to fight, apologizes for violence | Survival vs Healer crisis | Heals AND fights; moral compass |
| Keisha | Pure aggression | Niece-memory reveal | Protector-not-aggressor |
| Emily | Analytical, detached | "Hope isn't a variable" moment | Emotionally engaged |
| Sarah | Trauma + dark humor | Post-rescue Safe Room | Survivor-strength leader |
| K'thara | Formal, ancient grief | Homeworld L24 visit | Found-purpose ally |

Cognitive brain prompt should include the **current arc state** for each so its improvised lines stay coherent.

---

## 7. Ending-Specific Dialogue (12 Endings)

Each ending requires its own scripted cinematic. Below: the precondition logic and the load-bearing lines for each.

### Ending 1 — GOLDEN (Perfect)
- **Preconditions:** Timeline Omega + all allies alive + `salvari_alliance` + `chen_saved` (Chen sacrifices, virus succeeds).
- **Setting:** Earth, dawn, ~1 year later. Memorial wall, Chen's name. Hybrid human/Salvari city. Jake & Sarah's home with children.
- **Load-bearing lines:**
  - Sarah V.O. opener: "The war ended. But the healing took longer."
  - K'thara: "We honor our dead by living well."
  - Jake (closing V.O.): "Humanity isn't a species anymore. It's an alliance. And we're just getting started."
- **Tag card:** "THE END — GOLDEN ENDING / You saved everyone. The galaxy has a chance."

### Ending 2 — GOOD (Strong Victory)
- **Preconditions:** Timeline Alpha/Omega, most allies, `salvari_alliance`, Overlord destroyed, ≥1 companion KIA in Act 4.
- **Setting:** ~6 months later. Military cemetery. Earth scarred but healing. Salvari integration tensions exist.
- **Load-bearing lines:**
  - Jake at grave (slot for Keisha/Aria/Emily depending on who died): "[N] fell taking the orbital platform."
  - Sarah: "We're still here. Still fighting. Still protecting people."
  - Closing tag: "Victory came at a price. But hope survives."

### Ending 3 — BITTERSWEET (Pyrrhic)
- **Preconditions:** Timeline Alpha, half the cities ruined, `salvari_alliance`, Overlord destroyed at great cost.
- **Setting:** ~1 year, refugee camps; K'thara questions if parts of the Overlord survived.
- **Load-bearing lines:**
  - K'thara: "Sometimes I wonder if saving Earth just delayed the inevitable."
  - Jake: "Then we'll be ready."
  - Closing tag: "Freedom was won. At a terrible price."

### Ending 4 — TRAGIC (Sarah's Sacrifice)
- **Preconditions:** Timeline Omega, high Sarah romance, `chen_saved=false` (Chen's virus alone fails; Sarah must carry it).
- **Setting:** Final boss → ~1 year later, Jake alone on a cliff, K'thara approaching.
- **Load-bearing lines:**
  - Sarah's last: "I love you. I always will. Remember me as I was. Not as what I have to become."
  - Sarah's last-before-merge: "Save them. Save everyone. That's what you do."
  - K'thara at cliff: "Grief shared is grief halved. I'm here."
  - Closing tag: "Love saved the world. But not everyone survived."

### Ending 5 — FRACTURED (Relationship Failure)
- **Preconditions:** Timeline Alpha, team survives but low affinity; war won, but the bonds didn't.
- **Setting:** Post-war HQ; Jake alone; flashbacks of each ally leaving; K'thara intervenes.
- **Load-bearing flashback lines (require VO):**
  - Keisha: "You pushed too hard, Jake. You forgot we're people, not soldiers."
  - Aria: "I can't keep healing people who don't want to be saved. Including you."
  - Emily: "The statistical probability of this team functioning is now zero. I'm accepting the research position on Mars."
- **K'thara salvage line:** "I won't let you disappear like I almost did... You saved me, Jake Hunter. Let me return the favor."
- **Closing tag:** "The war ended. The wounds remain."

### Ending 6 — DARK (Jake Corrupted)
- **Preconditions:** Failed most rescues, `humanity_meter` < 30, Jake absorbs Overlord tech instead of destroying it.
- **Setting:** Five years later. Earth peaceful but monitored / controlled by Jake. K'thara leading resistance.
- **Load-bearing lines:**
  - Jake (transformed voice): "I can see... everything. Every threat. Every possibility. Every way to protect them."
  - K'thara backing away: "Jake... you're becoming what we fought."
  - Jake: "No. I'm becoming what humanity needs. A guardian. A god."
  - Closing tag: "The monster was defeated. The hero became the monster."

### Ending 7 — NIGHTMARE (Total Failure)
- **Preconditions:** Timeline Gamma (Sarah → Bride) + no allies + Overlord wins.
- **Setting:** Earth fully converted; Jake conscious but body controlled, in the breeding chamber.
- **Load-bearing lines:**
  - The Bride: "Hello, Jake. I told you I'd be waiting."
  - Jake V.O. (internal): "I'll never stop fighting."
  - The Bride: "I know. That's why we keep you conscious. Your resistance makes the offspring stronger."
  - Jake V.O. (final): "If anyone can hear me... run. Hide. Wait. And when you're strong enough... come back for us."
  - Closing tag: "Some fates are worse than death."

### Ending 8 — SOLO VICTORY
- **Preconditions:** Overlord killed but every companion died in Act 4.
- **Setting:** Jake alone on Earth's surface. (Detail-bullet only in TASK_7; needs scripting expansion.)
- **Tone:** Bittersweet survival; minimal dialogue, mostly V.O. monologue.

### Ending 9 — K'THARA ROMANCE ENDING
- **Preconditions:** Timeline Beta + `romance_kthara` accepted + Overlord defeated.
- **Setting:** Earth rebuilding alongside Salvari. Jake & K'thara as interspecies couple; future foundation.
- **Load-bearing lines:** Callback to "Grief shared is grief halved."
- **Tone:** Quiet hope from two broken survivors.

### Ending 10 — POLYAMOROUS FAMILY
- **Preconditions:** Timeline Alpha + polyamorous lock + all four (Aria/Keisha/Emily/Jake) alive.
- **Setting:** Rebuilt city, found-family scene with children Lily Chen-Hunter, Marcus Williams-Hunter, Tommy Watson-Hunter (per narrative bible §3).
- **Tone:** Trauma-bonded family thrives.

### Ending 11 — CHEN'S REDEMPTION
- **Preconditions:** Alternate Chen survival path (`chen_saved=true` + alternate Phase-4 method); Chen survives the finale.
- **Setting:** Post-war; Chen dedicates the rest of his life to curing infection survivors.
- **Tone:** Penance through service.

### Ending 12 — NEW BEGINNING
- **Preconditions:** Minimum victory conditions met; Jake + one ally.
- **Setting:** Jake and one survivor start fresh on a new planet (Salvari colony refuge?).
- **Tone:** Open-ended; hook for sequel.

### Endings Decision Table

The runtime evaluator should pick by priority (first match wins) over the locked `timeline`, then the flags below:

| Priority | Ending | Required Flags |
|---|---|---|
| 1 | NIGHTMARE | `timeline==Gamma` AND `overlord_win` |
| 2 | DARK | `humanity_meter<30` AND `jake_absorbs_overlord` |
| 3 | GOLDEN | `timeline==Omega` AND `all_allies_alive` AND `salvari_alliance` AND `chen_saved` |
| 4 | TRAGIC | `timeline==Omega` AND `sarah_phase4_carrier` |
| 5 | POLYAMOROUS FAMILY | `timeline==Alpha` AND `romance_polyamorous` |
| 6 | K'THARA ROMANCE | `timeline==Beta` AND `romance_kthara>=accepted` |
| 7 | CHEN REDEMPTION | `chen_survives_finale_alt` |
| 8 | GOOD | `salvari_alliance` AND `overlord_defeated` AND `companions_killed >= 1` |
| 9 | BITTERSWEET | `salvari_alliance` AND `overlord_defeated` AND cities_destroyed >= 50% |
| 10 | FRACTURED | `timeline in {Alpha, Omega}` AND `companion_affinity_avg < 4` |
| 11 | SOLO | `overlord_defeated` AND `all_companions_dead` |
| 12 | NEW BEGINNING | fallback if Overlord defeated and none of the above match |

> Note: the priority order is a recommendation — Tim should review and lock during scripting.

---

## 8. Implementation Gaps + Recommended Runtime

### 8.1 Gaps (vs. Current Shipped Code)
| Area | Current | Gap |
|---|---|---|
| Dialogue UI | None | Need: dialog box widget, name tag, choice list, advance-input. |
| Dialogue tree runtime | None — only flat NPC text in `npc_dialog.{h,cpp}` | Need: tree loader + branching evaluator + state-flag plumbing. |
| State flags | Ad-hoc / hard-coded | Need: a `GameStateFlags` POD persisted across saves; serialized to checkpoint. |
| Cutscene player | None | Need: simple timeline player (camera waypoints + voice-clip cue + subtitle). |
| VO playback | None | Need: voice-clip stream → 3D-positioned source if speaker is on-screen, else 2D. |
| Subtitle renderer | None | Need: line wrapping + speaker color + advance with input. |
| Companion banter scheduler | None | Need: bark scheduler (priority queue, no-repeat window, mute-on-grief). |
| Live AI cognitive brain | Planned (companion spec) | Need: async LLM transport, prompt-cache, voice TTS, banter budget. |
| Locale / translation | Hard-coded English | Need: string IDs in trees; locale lookup. (Defer past launch.) |
| Choice persistence | None | Need: serialize per-save the lock-state of timelines, romance, broker_favor, etc. |

### 8.2 Recommended Dialogue Tree Format

A line-based file format (one tree per `*.dlg` file under `assets/dialogue/`) keeps it human-editable and diffable in Git:

```
# act1/floor2_breeding_chamber.dlg
node JUNCTION_INTRO
  speaker Jake
  cam medium
  voice vo/jake/L2_junction_intro.ogg
  text "No... no no no..."
  next JUNCTION_CHEN_MOCK

node JUNCTION_CHEN_MOCK
  speaker DrChen
  voice vo/chen/L2_three_chambers.ogg
  text "Fascinating, isn't it? Three different infection vectors."
  next JUNCTION_PLAYER_CHOICE

node JUNCTION_PLAYER_CHOICE
  type choice
  prompt "Which door first?"
  choice "Door A — Aria"  -> goto SCENE_ARIA_APPROACH; set chose_first=aria
  choice "Door B — Keisha" -> goto SCENE_KEISHA_APPROACH; set chose_first=keisha
  choice "Door C — Emily"  -> goto SCENE_EMILY_APPROACH; set chose_first=emily

node SCENE_ARIA_APPROACH
  require aria_timer > 0
  fallback SCENE_ARIA_SIREN
  ...
```

Keywords (proposed): `node`, `speaker`, `voice`, `cam`, `text`, `next`, `type choice`, `prompt`, `choice`, `goto`, `set`, `require`, `fallback`, `bark` (for pool entries), `end`.

Conditions reference the flags in §3. The evaluator's job is small: read one node, resolve `require`/`fallback`, present text + advance choice, mutate flags via `set`, follow `goto`/`next`.

### 8.3 Branching Evaluator — Behavior Sketch (no code)
- **Input:** `(tree_id, current_node_id, game_state)`.
- **Output:** `(line_to_present, choices?, side_effects[])`.
- **Resolution order:** load node → check `require` predicate against `game_state` → if false, follow `fallback`; if no fallback, raise asset error → emit `line_to_present` and optional `choices` → on user advance/select, apply `set` side effects to `game_state` → return next node id.
- **Reentrant / save-safe:** every choice records `(tree_id, node_id, choice_index)` to the save file so a checkpoint mid-cutscene is recoverable.

### 8.4 Companion Banter Scheduler — Behavior Sketch
- **Priority queue** of bark requests, ordered by category priority:
  1. **Forced** (cutscene-required line, scripted bark)
  2. **Combat** (engaging / damage / killed / low-morale)
  3. **Environmental** (volume trigger)
  4. **Live-AI cognitive** (ambient)
- **Per-companion rate limiter:** ≤1 forced/combat in 8s, ≤1 environmental in 30s, ≤1 cognitive in 90s during exploration; combat doubles rate budget for combat lines.
- **No-repeat window:** size 5 per pool.
- **Mute-on-grief:** for 60s after a cutscene tagged `mood=grief`, suppress all bark categories ≤ environmental for the relevant companion.

### 8.5 Live-AI Cognitive-Brain Integration Seam
Per companion-ai-design §2, the LLM rides alongside the deterministic sim. For dialogue:
- **Input to LLM call:** companion's character-voice prompt (cacheable; reuse Claude prompt-cache) + current `game_state` snapshot + recent transcript (last 6 exchanges) + speech budget remaining.
- **Output:** a single utterance with metadata tags (e.g., `tone=worried`, `target=Jake`, `mood=grief-aware`).
- **TTS:** pipe utterance to TTS engine; voice clip cued in the bark scheduler at priority "cognitive".
- **Failsafe:** if LLM hits latency budget (>3s), drop the utterance — never block the sim.
- **Determinism:** cognitive output is NEVER input to the sim — only emitted as sound + subtitle.

### 8.6 VO Catalog Spec (Recording Sheet Format)
A per-character recording sheet with columns:
- `clip_id` (e.g., `jake_L1_awakening_001`)
- `scene_id` (e.g., `act1/floor1_awakening`)
- `line_text`
- `emotion_tag` (groggy / horrified / determined / vulnerable / etc.)
- `priority` (P0–P3, see §9)
- `duration_target_sec`
- `delivery_notes` (e.g., "barely whispered, half-collapsed")

This sheet drives studio recording. Estimated full VO runtime: **8–10 hours** of voiced dialogue (per TASK_7 self-estimate).

---

## 9. Build Order — Which Scenes MUST Be Live for Act-1 Playable

### 9.1 Priority Tiers
- **P0** — Required for Act-1 vertical-slice playable. Blocking VO recording.
- **P1** — Required for Act-1 complete release. VO record in same wave as P0.
- **P2** — Required for Acts 2–3.
- **P3** — Required for Act 4 / endings.

### 9.2 P0 — Act-1 Vertical Slice
| Scene | Speakers | Notes |
|---|---|---|
| **Awakening** | Jake (V.O.) | Sets the tone for the whole game. Must be perfect. |
| **Breeding Chamber Junction** | Jake, Dr. Chen (loudspeaker) | The choice that defines the timeline system. Critical. |
| **Aria Rescue — Success** | Jake, Aria | One of three. All needed. |
| **Keisha Rescue — Success** | Jake, Keisha | One of three. All needed. |
| **Emily Rescue — Success** | Jake, Emily | One of three. All needed. |
| **Aria Failure — Siren** | Jake, Siren | At least one failure scene for player who explores limits. |
| **Keisha Failure — Breeder Queen** | Jake, Breeder Queen | (See above.) |
| **Emily Failure — Oracle** | Jake, Oracle | (See above.) |
| **Clone Confrontation pre-fight + boss + Sarah rescue** | Jake, Clone, Sarah | Climax of Act 1. |
| **Floor-7 Endings (all four)** | Jake + party | Locks timeline; mandatory. |

P0 also needs:
- Combat barks for Jake, Aria, Keisha, Emily, Sarah (all five pools per the §6.2 list).
- Floor-1 to Floor-7 environmental triggers (~50 lines).
- Subtitle renderer.
- Cutscene player.
- Dialogue tree runtime.
- State-flag system (§3.1–§3.3 at minimum).

### 9.3 P1 — Act-1 Complete Release
- Dr. Chen Floor-2 boss dialogue
- Failed Experiment #7 boss
- The Chorus (all 4 phases)
- Swarm Controller hack-defense dialogue
- Floor-6 first-contact-tease (Salvari hint)
- Dr. Chen Return Mission rescue dialogue

### 9.4 P2 — Acts 2–3
- L8 Salvari First Contact (Allow Landing AND Open Fire variants)
- L10 K'thara joins permanently
- L12 Cave System lore dump
- All three transformed-women boss arcs (L12/15/19, L13/16/18, L14/17/20) — each with both choice branches
- K'thara Romance "Cold Stars" scene (L24)
- Polyamorous Family Formation (L31)
- Wedding Ceremony (L31)
- Earth Approach (L35)
- Companion combat barks (full ~25 per companion)
- Between-mission pair-banter seeds
- Live-AI cognitive brain integration (functional, capped scope)

### 9.5 P3 — Act 4 + Endings
- L36–L41 cinematics
- Dr. Chen rescue Act-4 variant (L42)
- Proto-Overlord taunt sets (L44+)
- Mothership boarding (L48)
- Guardian boss (L49)
- True Overlord 4-phase final dialogue (L50)
- All 12 ending cinematics (Endings 1–12) — Endings 1, 4, 6, 7, 10 are highest narrative impact and should be recorded first within P3

### 9.6 VO Recording Wave Recommendation
**Wave 1 (with P0 above):** Jake (full), Sarah (full), Aria (full), Keisha (full), Emily (full), Dr. Chen (Act-1 only), Clone, The Siren, Breeder Queen, The Oracle (intro only).

**Wave 2 (P1–P2):** K'thara (full), Dr. Chen (Acts 2–4), The Siren / Breeder Queen / Oracle (full arcs), boss-redemption mini-game lines, R'thek, Seeker (ship AI), Broker, Hayes (resistance).

**Wave 3 (P3 + Endings):** Proto-Overlord, True Overlord (all 4 phases), The Bride (Sarah-corrupted), Fleet Commander, Kryx (rebel leader), all 12 ending cinematics.

### 9.7 Live-AI Companion Banter Build Order
Aligning with the companion-ai-design subsystem decomposition (§5 of that spec):
1. **Reflex tactical AI** is sub-project #1 (not dialogue) — must ship first.
2. **Cognitive intent layer** ships second — also not yet dialogue.
3. **Conversation system** (sub-project #3) — *this is where live-AI banter goes live for EFLZ.* Build this *after* Act-1 scripted dialogue is functional, so the scripted tree provides the canon backbone the LLM can lean on.
4. **Dual-provider LLM integration** (sub-project #4) — Grok for Aria/Sarah/Keisha/Emily/K'thara, Claude for Jake/Dr.Chen/Clone/Hayes/etc. Claude side uses prompt caching (per spec).
5. **TTS** (sub-project #5) — ships when budget allows; until then, subtitles only for live-AI utterances.

> **Practical sequence:** Ship Acts 1–2 with **100% scripted + VO-recorded** dialogue. Roll in live-AI banter as a feature update once the cognitive brain is online — by then players have the canonical voice in their ear and the LLM has firm guardrails.

---

## 10. Appendix — Cross-Reference Index

### 10.1 Speakers Across All Scenes (master list)
**Player & inner circle:** Jake Hunter (Subject 7-Alpha), Sarah, Aria Chen, Keisha Williams, Dr. Emily Watson, K'thara, Dr. Chen, R'thek, Seeker, Col. Hayes.

**Antagonists:** Jake's Clone, The Siren (Aria-corrupted), Breeder Queen (Keisha-corrupted), The Oracle (Emily-corrupted), The Bride (Sarah-corrupted), The Chorus (Rodriguez/Tanaka/Chen-Pvt/Lancaster/Subject Zero), Failed Experiment #7, Swarm Controller, Alien Overseer, Proto-Overlord, Proto-Overlord Guardian, The True Overlord, Kryx (rebel — neutral-hostile), The Broker (neutral).

**Support tier (Omega):** Dr. David Chang, James Torres, Dr. Thomas Ashford.

**Children (timeline-dependent):** Hope, Marcus (Omega); Lily Chen-Hunter, Marcus Williams-Hunter, Tommy Watson-Hunter (Alpha).

### 10.2 Flag-to-Scene Cross-Reference (selected)
| Flag | Scenes that READ it | Scenes that WRITE it |
|---|---|---|
| `aria_saved` | Acts 2–4 banter pool, Floor-7 ending selector, Endings 1/2/10 | L2 Aria Rescue — Success |
| `aria_lost` | L12/15/19 Siren encounters, all Beta endings, Nightmare ending | L2 Aria Rescue — Failure |
| `timeline` | Every Acts 2–4 cutscene | L7 Floor-7 climax |
| `salvari_alliance` | All Acts 2–4 K'thara scenes, ALL good endings | L8 First Contact choice |
| `chen_saved` | L42, L50 Phase-4 (Chen as virus carrier), Ending 1, Ending 4 (NOT chen), Ending 11 | L7 Return Mission rescue OR L42 |
| `romance_kthara` | L24 Cold Stars, all Beta endings | L24 Cold Stars choice |
| `romance_polyamorous` | L31 Polyamorous Family Formation, Ending 10 | L31 acceptance |
| `genetic_recombination_pct` | L7 Floor-7 ending selector | Auto-incrementing tick from start of game |
| `broker_favor_owed` | Act 4 scenes TBD | L27 Broker Deal |

### 10.3 Open Design Questions for Tim
1. **Phase-4 carrier branching** — when does the game decide Chen vs. Sarah carries the virus? Suggest: at L50 Phase-3 → check `chen_saved`. If false, Sarah volunteers in Phase-4 (Tragic). Confirm.
2. **Humanity meter inputs** — Floor-4 augmentation choices are listed in EFLZ_NARRATIVE; need a numeric formula. (Out of scope for this catalog — flag for next pass.)
3. **Boss-redemption ripple** — if Aria/Keisha/Emily are *redeemed* in Act 2, do they replace their saved-version selves in the polyamorous-family ending? Suggest: no — separated boss-form Aria + saved-Aria coexist in Ending 10's family (per L19 SEPARATION lore). Confirm.
4. **Broker favor** — what is the cashed-in scene in Act 4? Not in TASK_7. Flag for authoring.
5. **Alien Overseer (Floor 6 boss)** — full taunt set not in TASK_7. Flag for authoring.
6. **Ending 8 / 11 / 12** — only bullet-summarized in TASK_7. Full screenplays needed before P3 VO recording.

---

*End of catalog. ~1,150 lines. Authority: Tim Smith / EFLZ design corpus. Clean-room safe — built only on Tim's IP and the in-repo companion-ai-design spec.*
