# The Interrupt-Rescue Set-Piece — Design Doc (W4-4)

Floor 2 (Medical Bay), the wards holding Aria / Keisha / Emily (+ Lena and later
Sarah on F7, same grammar). Tradition: Dead Space / Alien — dread and stakes,
never explicit. The horror is medical and monstrous; the camera speaks in
silhouette, sound, and suggestion. Builds directly on the systems that exist
today (`RescueSystem`, chat-tree flags, `StoryFlags`); flags the one real new
asset. Companion piece to `WAVE3_WAVE4_PLAN.md` item 4.5.

## 0. What already exists (read before designing anything "new")

The engine is closer to this set-piece than the brief implies — most of it is
data-tuning, not new systems:

- **`RescueSystem`/`RescueVictim`** (`app/rescue.h`, `app/rescue.cpp`): three
  captives per F2 (Aria/Keisha/Emily), each a 5-min countdown gated behind
  `hubReached()`, each with 1-2 room-tagged attacker enemies spawned "next to
  the captive... the mid-attack tableau" (`app/canon_play.cpp:451-466`) —
  **the attackers exist today as static guards, not as an active assault**.
  Timer expiry already transforms the victim into her boss (Aria->Siren,
  Keisha->Breeder Queen, Emily->Oracle) via the existing `MonsterManager`.
- **Chat-tree tier data ALREADY AUTHORED, unused by any trigger yet**: every
  girl's `first_meeting` tree branches on a `<girl>.interrupted` flag
  (`aria.json:13`, `emily.json:13`, `keisha.json:13`, `lena.json:13`).
  `fm0` (flag true) = raw, mid-attack trauma ("Is it off me. Yes or no.");
  `fm0_alt` (flag false/absent) = wary-but-composed. Nobody sets this flag
  today — the E-interact dialog (`app/npc_dialog.h`) always takes the
  uninterrupted path. **This is tier 1 vs tier 2, already written, just not
  wired to anything.** Tier 3 (lost) is the pre-existing expiry->boss path,
  paid off by each girl's `infected_lost` tree (e.g. `keisha.json` `il0/il1`).
- **`StoryFlags`/`ChatFx::SetFlag`** (`app/story_ops.h`): the exact mechanism
  to set `<girl>.interrupted` from gameplay — no new flag plumbing needed.
- **`GameCue`/`CueKind`** (`app/cues.h`): `EnemyTaunt`/`EnemyAttack` already
  fire from `MonsterSystem`; this is the hook for the through-the-door tell.
- **`RescueVictim::setTint()`** (`rescue.h:116`): the existing mechanism for a
  visible-state variant (used today to darken the collapse ragdoll) — reused
  below for the wounded-tier look, no new render path needed.
- **`tools/attack_death_bake.py`**: the headless-Blender append-clip pipeline
  (Idle/Walk/Run kept, Attack+Death baked on) — the template for the one new
  asset this design needs (§3).

So the real gap is narrow: **there is no moment where entering the ward starts
an active assault the player must stop under a short clock** — today you just
walk up to a passive captive and press E anytime inside 5 minutes. The set-
piece is a new *inner* timing layer on top of the existing systems, not a
rebuild.

## 1. The set-piece loop

1. **Trigger.** Two flavors, both fine for F2 variety: (a) *scripted ward* —
   crossing the ward-room threshold trigger volume (the same room-tag
   `canon_play.cpp` already uses) arms the encounter; (b) *systemic* — the
   existing 5-min captive timer reaching a low threshold (e.g. <90s) auto-arms
   it, so a slow player gets ambushed by urgency instead of a script. F2 uses
   (a) for the 3 named wards, (b) is the template for later floors/Lena.
2. **The read.** Before the door: an `EnemyTaunt`-cadence bark, muffled/low-
   passed by the closed door, plus light bleeding under the door per
   `ART_BIBLE.md` §3 (hazard-amber wards, one accent, never a wash past the
   doorway). No visual of the act — sound and light are the whole tell. This
   is the dread beat; give the player 2-4 seconds of it before the door opens.
3. **Burst-in beat.** Door opens (kick/E), camera holds a beat on the tableau
   in **silhouette against the ward's key light** — creature over the table,
   never framed for detail, ART_BIBLE's "light is spent, not sprayed" doing
   the restraint for us. The attacker(s) that already spawn per ward
   (`canon_play.cpp:451-466`) are what the player fights; they gain a new
   **Assault** idle state (see §3) instead of standing generically nearby.
4. **The interrupt window.** A short inner clock starts the instant the
   tableau is revealed (independent of the outer 5-min captive timer, which
   keeps gating "can I still reach this ward at all"). Suggested: ~18s to
   first blood, ~35s hard cutoff. Player kills the attacker(s) — `EnemyDeath`
   cue — before the cutoff. Tier is computed from **time-to-kill**, not
   binary in-range-or-not (this is the actual design delta from today's
   walk-up-and-E).
5. **Creature behavior during.** Vulnerable-but-lethal: the Assault state
   should NOT be a free hit — the creature still turns to defend/counter-
   attack if approached carelessly (reuses existing combat AI, just entered
   from a scripted pose instead of idle-patrol), so a reckless burst-in can
   still cost the player HP even on a clean-tier kill. This preserves tension
   without punishing the girl for player recklessness.
6. **Aftermath.** Tier resolves into a `StoryFlags::set` call before the
   rescue dialog fires (`npc_dialog.h`'s `onComplete`/`RescueSystem::tryRescue`
   is the exact call site) — then the existing chat-tree branch does the rest.

## 2. Timing-tier consequence table

| Tier | Trigger condition | Flag/state set | Dialog path (exists today) | Visible state | Companion-arc effect |
|---|---|---|---|---|---|
| **Clean save** | Attacker(s) killed before first-blood threshold (~18s) | none (`<girl>.interrupted` stays unset) | `fm0_alt` — wary, composed, "almost had it" register | default tint, upright idle/walk, no wince anim | full `karma`/`love` gains per existing tree; fastest romance-lane opening |
| **Wounded save** | Killed after first-blood but before hard cutoff (~35s) | `SetFlag <girl>.interrupted` | `fm0` — raw, mid-trauma register ("Is it off me. Yes or no.") | `setTint()` blood-multiply (subtle, per ART_BIBLE — no gore excess) + hunched walk-only locomotion (suppress run clip for a scene or two) | same eventual romance lane, but the `trust`/`night` scenes (already gated on `rel_gte`+`love_gte`) read against a harder-won trust; a couple of `interrupted`-flavored banter lines already exist per girl to reuse |
| **Lost** | Hard cutoff passes / outer 5-min timer expires first | victim -> `VictimState::Expired`, boss spawned (existing pipeline) | `infected_lost` tree (e.g. Keisha `il0/il1`) | model swapped to boss (existing) | companion slot never opens; grief banter fires from the OTHER saved girls (`girl_saved`/`girl_lost` conds already in the `ChatCond` vocabulary) — the loss is felt by the survivors, not just narrated |

No new dialog needs to be authored for tiers 1-2 on F2's three girls — it's
already in the JSON, unused. New authoring is only needed for floors/girls
that don't yet have a chat tree (verify Lena's is F2-adjacent or elsewhere
before reusing this exact wiring there).

## 3. Systemic hooks — map to host system, and the one new ask

| Design element | Host system | Status |
|---|---|---|
| Ward trigger + door tell | room-tag trigger volumes (`canon_play.cpp` room tagging) + `GameCue::EnemyTaunt` muffled by closed-door low-pass | EXISTS — wiring only |
| Burst-in silhouette framing | camera + ART_BIBLE lighting law (key light on tableau, no detail) | EXISTS — art/lighting discipline, no code |
| Interrupt window / tiering | new inner timer in `RescueVictim`/`RescueSystem`, keyed to attacker `EnemyDeath` cues | NEW — small, additive state machine field (see §6 task 3) |
| Tier -> flag | `StoryFlags::set` / `ChatFx::SetFlag` via `applyChatFx` | EXISTS — call-site wiring only |
| Per-girl dialog variants | chat-tree `if [{"flag": "<girl>.interrupted"}]` | EXISTS, fully authored for Aria/Keisha/Emily/Lena |
| Visible wounded state | `RescueVictim::setTint()` + suppressing the run clip in `driveAnim()` | EXISTS — parameter tuning only |
| Creature vulnerable-but-lethal during interrupt | existing `MonsterSystem` combat AI, entered from a new Assault pose | Assault **entry pose** is NEW (small anim ask, below); the combat itself is EXISTING |
| Deaths / gib restraint | existing death ragdoll + `deathFX`-style collapse (mirrors `RescueVictim::ragdoll()`) | EXISTS |
| Escort-to-safe-room after rescue | W4-1's companion-follow work in flight (`RescueVictim` companion-follow already ships; safe-room destination + escort-complete flag is W4-1's scope, not this doc's) | IN FLIGHT elsewhere — this doc only consumes the flag it sets |
| Zone fog/audio dread bed | `ART_BIBLE.md` §5 (per-zone tinted fog) + detention-block ambience (W3.3) | EXISTS/queued in Wave 3, not this doc's build |

**The one real asset ask:** a **Struggle** loop clip (creature-over-captive,
~2s idle loop, restrained/menacing — no explicit content, reads as
threat-posture not act) baked the same way `tools/attack_death_bake.py` bakes
Attack/Death onto the existing Idle/Walk/Run set. Propose a sibling script
(`tools/struggle_bake.py` or a third clip appended by the existing baker) so
`RescueVictim`/attacker rigs carry `Idle/Walk/Run/Attack/Death/Struggle` in one
GLB. This is the only new content; everything else is wiring + tuning.

## 4. Companion/romance arc skeleton

Per-girl personality (align to W4-1's trees, already legible in the JSON):
**Keisha** = defiant/command-voice (drill-sergeant coping), **Aria** =
dissociated-into-caretaking (calm as the tell she's at her limit), **Emily** =
dissociated-into-narration (clinical self-observation as armor). Lena reads
closer to defiant-stoic per her `interrupted` line style.

- **Trust progression** rides the existing `rel` stage (0 Stranger..4 Romance,
  `StoryFlags::raiseRel`) and `love` axis, already the gate for each girl's
  `trust` -> `romance` -> `desire` -> `night` -> `afterglow` chain. This
  design's only addition is that a **wounded-tier** rescue should read as a
  *harder-won* version of the same trust track, not a separate track — reuse
  the existing `interrupted`-flavored lines already threaded into `banter`
  and `trust` nodes (e.g. Aria's `fm0` raw register colors her later "I
  policed the language" caretaking beats).
- **Where romance gates live:** the existing `loc.private` flag + safe-room
  destination (W4-1 escort scope) is exactly the "safe-room scene" the brief
  asks for — `night`/`desire` trees already gate on it. Nothing new to build
  here; this doc just confirms F2's interrupt tiers feed the SAME gate.
- **Payoff at F7 Sarah rescue:** F7's Sarah rescue is already documented as a
  **timed rescue against a recombination timer** where the **timeline locks**
  (`EFLZ_MASTER_PLAN.md`, `EFLZ_WORLD_STRUCTURE.md`). Recommend F7 reuse this
  exact tier mechanic at higher stakes (no "clean" tier available — Sarah's
  scene should force at minimum the wounded read, raising the stakes as the
  final instance of the pattern) rather than inventing a fourth mechanic.
  Endgame epilogue slate (`EFLZ_MASTER_PLAN.md` axis-triggered endings) already
  reads `girl_saved`/`girl_lost`/tier-flavored `love` totals — the interrupt
  tiers compound into those totals for free via the existing axis fx.

## 5. Pacing map (Floor 2)

Three wards (Aria/Keisha/Emily) is the right count for F2 — enough for the
pattern to land without becoming a chore before Wave 3's room-dressing pass
even lands elsewhere on the floor. Variation axes so three instances don't
feel like one:

| Ward | Creature | Timing pressure | Environment state |
|---|---|---|---|
| Ward A (Aria) | Verthani (fast flanker) | tightest window — she's the "soft, not weak" read, urgency sells her later calm | first ward the player meets — teach the loop clean |
| Ward B (Keisha) | DominionTrooper (disciplined) | slightly longer window, but the trooper counter-attacks harder if approached carelessly (her personality rewards aggression, not caution) | mid-fight interruption possible (guard patrol nearby, per Wave-4 4.4 guard routes) |
| Ward C (Emily) | Verthani + DominionTrooper pair (2 attackers) | hardest — two kills inside one window | most "clinical" room dressing (Research-Lab-adjacent per canon_play comment), reinforces her narration-as-armor read even before she speaks |

Repetition doesn't dull because: (a) creature type changes the read and the
combat shape, (b) window/attacker-count escalates A->B->C, (c) each girl's
authored personality re-colors the SAME three tiers completely differently
(the tier system is mechanical; the dialog is not), (d) environment/room
dressing differs per Wave 3's per-room recipes. Later floors (Lena, Sarah)
reuse the mechanic at new stakes rather than repeating F2's shape.

## 6. Build plan (ordered)

1. **Ward trigger + door tell.** Wire the room-tag trigger volume to arm the
   encounter and low-pass an `EnemyTaunt` cue through the closed door.
   *Files:* `app/canon_play.cpp` (trigger wiring near the existing attacker
   spawn block, ~line 451), `app/cues.cpp` (muffle helper if not already
   generic). Small.
2. **Struggle clip + Assault pose (the one asset ask).** Bake a third clip
   onto attacker rigs; `MonsterSystem` fuzzy-resolves it like Attack/Death and
   enters it on trigger instead of idle-patrol. *Files:* new
   `tools/struggle_bake.py` (or extend `attack_death_bake.py`), `app/monster.h`/
   `.cpp` (Assault state resolution, mirrors existing Attack/Death lookup).
   Medium — the only task with a real content dependency (Blender bake).
3. **Interrupt-window timer + tier resolution.** Add the inner clock to
   `RescueVictim`/`RescueSystem`, fed by attacker `EnemyDeath` cues, computing
   clean/wounded/lost and calling `StoryFlags::set("<girl>.interrupted")` on
   wounded before `tryRescue`'s dialog fires. *Files:* `app/rescue.h`,
   `app/rescue.cpp`, `app/canon_play.cpp` (flag call-site), `--test-rescue`
   self-test extended with an R6 tier assertion. Medium.
4. **Wounded visible-state tuning.** Wire `setTint()` blood-multiply + suppress
   run-clip on wounded tier for a couple of scenes (clears on a story beat,
   e.g. reaching the safe room). *Files:* `app/rescue.cpp` only. Small.
5. **F2 content pass.** Apply the Ward A/B/C variation table (creature mix,
   window length, attacker count) to the existing attacker array
   (`canon_play.cpp:455-466`) — data-only tuning, no new code paths. Small.

Order matters: (1) and (2) can run in parallel (trigger/audio vs. the
Blender-dependent asset); (3) depends on both; (4) and (5) are cheap tuning
passes once (3) lands. Recommend landing after Floor 2 gets its Wave-3
room-dressing recipe (per `WAVE3_WAVE4_PLAN.md` 4.5's own note) so the burst-in
beat reads against a dressed ward, not a grey box.
