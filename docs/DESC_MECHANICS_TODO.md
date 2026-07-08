# DESC MECHANICS TODO — the gameplay half of the desc-field gold
*W8-1 (feat/floor-life). The canonical level data (EscapeLab48_AllFloors_v2.project.json)
carries 71 authored room `desc` fields on F2-F7 — a whole content pass written in data
(W5-1's find, CAMPAIGN_LEDGER backlog). W8-1 shipped the VISUAL half: every named story
room now reads as its desc (room_dressing.cpp desc-gold layer). This document is the
MECHANICS half — every gameplay verb the descs promise, cataloged with its anchor so a
future wave (or Tim) can green-light each as a scoped task. Nothing here is built unless
marked; the visual anchors ARE built.*

Format: **room (floor)** — desc verbatim → mechanic → state today → anchor.

## Already live (no work needed)
- **Wards A/B/C (F2)** — "Rescue Target N. X min timer." → tiered interrupt-rescue.
  LIVE: RescueSystem (W4-1/W5-2), `--test-rescue` 26/26.
- **Sarah's Holding Cell (F7)** — "7-9 min rescue timer. Branching." → clone-gated
  rescue → helipad win. LIVE: `--test-goldenpath` 9/9.
- **Helipad (F7)** — "Extraction point. Signal beacon." → win trigger. LIVE.
- **Server Room (F7)** — keycard pickup exists (hack REWARD is live; the hack VERB isn't).

## Tier A — mechanics with systems mostly in place (cheap, high payoff)
**STATUS: ALL FIVE BUILT (W9-1, feat/desc-mechanics, --test-descmech D1-D12).**
Framework: app/interactables.{h,cpp} (InteractPoints + ItemStore + StatusEffects)
+ app/desc_mechanics.{h,cpp} (the verbs + the self-test). Host seams in app_run:
descMech.build/tick/onUse/onUseItem/onCue + the npcBark prompt + HUD status tag.
Tier B/C: register new points via Interactables, statuses via StatusEffects —
do NOT add bespoke E-branches. Decon cure (#8) already landed with Tier A.
1. **Coolant System (F4)** — "Liquid nitrogen. Sabotage = boss weakness."
   → Interact at the coolant control console → F4 boss (The Collective, 2000HP) takes a
   damage-taken multiplier or loses a phase. Anchor: desc-gold console + cryo drums are
   placed; lore-terminal breadcrumb live (canon_play). Needs: one interact + a boss flag
   (boss ladder is W4-1's; flag plumbs through CanonPlay).
2. **Power Junction (F4)** — "EMP device craftable here."
   → Interact at the EMP bench (crate + console placed) → grants a one-shot EMP item that
   stuns drones/synths (F5 payoff). Breadcrumb live. Needs: inventory slot + use verb +
   a stun hook on drone/synth AI.
3. **Central Control Hub (F5)** — "Master hack terminal. Sarah's objective."
   → Hack interact → disables F5 drone waves (or flips some patrol drones friendly for
   the boss fight). Console arc + task pool placed; lore terminal live. Ties into
   backlog item "hall/lobby terminal interactivity" (W4-2's interact plumbing).
4. **Cold Room (F3)** — "-40C. Timer: 30s before damage."
   → Standing in the room >30 s applies chill damage ticks (reuse the zone/roomAt test
   the fog system already runs per frame). Visual anchor (cryo drums, icy light) placed.
5. **Quarantine Zone / Pharmacy (F2)** — "Infection research." / "Antidote components."
   → Pick up components (Pharmacy pickups live) + research note (new lore terminal
   live) → craft antidote; cures an infected-status debuff if/when infection damage
   type lands. Pairs with Decontamination (below).

## Tier B — needs a small new system
**STATUS: ALL BUILT (W9-2, feat/desc-mechanics-bc, --test-descmech-bc B1-B9).**
Module: app/desc_mechanics_bc.{h,cpp} (DescMechanicsBC, beside W9-1's DescMechanics)
+ CanonPlay::allyStrike/spawnBonusCache + RoomDressing::killRoomEmissives.
Tier C #12-#16 also landed (see below); #11 stays W5-1's. Flags:
f6.salvari_freed / f6.nexus_fuse_a/b / f6.portal_sealed / f4.course_beaten /
f4.aug_used(+_strength/_speed/_armor) / f6.first_contact / f7.clone_seen /
f7.beacon_activated / f4.memory_viewed.
6. **Salvari Containment (F6)** — "3 prisoners. Can be freed as allies."
   → Free-the-prisoners interact → 3 Salvari allies fight alongside (companion-follow
   already exists for rescue girls/Sarah — reuse follow+ally targeting). Cots + lore
   breadcrumb placed. The ONE-GAME manifest lists a companion/ally system UNLANDED —
   this is its F6 hook.
7. **Energy Nexus (F6)** — "Portal power. Overload = seal forever."
   → Interact chain (2 fuseboxes + core placed) → overload: portal glass + glow in
   Portal Chamber dies, F6 reinforcement spawns stop, maybe a story flag. Lore
   breadcrumb live at the Nexus.
8. **Decontamination (F3)** — "UV flood chamber. Kills infection."
   → Standing in the UV violet pool clears infection status (the cure verb for #5's
   debuff). Visual anchor placed.
9. **Prototype Testing (F4)** — "Testing arena. Obstacle course."
   → Timed traversal over the crate slalom (placed) → reward cache unlock (ammo pickup
   placed as the reward). Needs: trigger volume pair + timer + HUD readout.
10. **Augmentation Bay (F4)** — "8 aug chairs. Strength/speed/armor."
    → One-use chair interact: pick ONE buff (dmg/speed/armor) for the rest of the run.
    Chairs (4 placed) + cyan pool are the anchor. Roguelite-flavored choice point.

## Tier C — story/scene beats (design-owned, bigger)
**STATUS (W9-2): #12 minimal seal beat, #13 First Contact dialog (chat_trees/
salvari_elder.json + the elder NPC), #14 clone-tank pre-scene, #15 comms beacon,
#16 memory holo (HoloTerminal single-panel bake) BUILT. #11 unchanged (W5-1's).
The #12 full portal set-piece (something comes through) stays design-owned.**
11. **Nexus Chamber Access (F4) / The Chorus** — "3,472 consciousnesses." → W5-1's 4.5
    descent owns this; whisper beat live. No action here.
12. **Portal Chamber (F6)** — "Active portal to alien homeworld." → portal visual +
    lore note live. A portal EVENT (something comes through / escape tease) is a
    set-piece candidate, design doc first.
13. **First Contact Chamber (F6)** — "Salvari explain the invasion's origin." → dialog
    scene on the placed meeting circle; chat-tree system exists (VIGIL pattern), needs
    a Salvari speaker + tree JSON.
14. **Clone Lab / F7 Boss** — "Jake's clone. Mirror confrontation." → boss fight LIVE
    (G-gates); the Clone Lab pre-scene (see your own body in the tank — glass pane +
    cryo cot placed, lore note live) is unbuilt flavor.
15. **Executive Offices / Comms Center (F7)** — "Invasion timeline." / "Distress
    beacon." → lore terminals live. A beacon INTERACT that foreshadows the ending
    (radio voice) is cheap flavor once terminal interactivity (Tier A #3) lands.
16. **Neural Interface Lab (F4)** — "Memory extraction." + Tier 2 Memory Maze links —
    holographic victim-memory playback; needs the text/holo-in-world path (reported
    missing since W3; do not fake).

## Explicitly NOT queued (needs Tim)
- **Ward timer values** (7/5/9 min) differ from the shipped rescue pacing — Tim owns
  difficulty tuning in playtest.
- **"Dr. Chen"** naming (ledger OPEN #1) — the F2 office/boss desc anchors exist but
  strings stay "Mutated Overseer" until his ruling.
- **Two-moon skybox** (Observation Deck desc) — W6-2 ships-PBR/space territory.
