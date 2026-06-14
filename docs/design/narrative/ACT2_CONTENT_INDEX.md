# ACT 2 — Content Offensive · Index

> Authored 2026-06-13 on `feat/act2-content-offensive` (off `integration/empire-fold`).
> Pure **data + readable scripts** — no engine code touched, default-inert until referenced.
> Quality bar: the existing narrative pack (subtext over exposition; menace and aftermath
> over explicitness; distinct voices; the trauma/rescue dial held exactly where the existing
> set put it).

This batch adds a coherent slate of **Act-2 open-world missions** (in the new
`x3.mission/1` format) plus the **new NPC chat trees** they need, every piece hooked into
the canon arc and the existing `feat/act2-*` engine branches. Nothing here contradicts or
duplicates the existing pack; where a thread already had a tree (Lena's carrier arc, Vesper's
hub, Reyes's flee outcome, the F2 victim→boss transforms), these missions **drive the
existing trees and flags** rather than re-author them.

---

## 0. Schema & validation (what I authored against)

- **Chat trees** conform to `x3.chattree/1` — `docs/design/narrative/NPC_CHAT_TREE_FORMAT.md`
  (read from the narrative-pack docs branch; cited there, not copied here). All new trees
  pass the existing structure validator `tools/check_chattrees.py` (6/6) and the op-vocab
  check (6/6).
- **Missions** conform to a NEW sibling format **`x3.mission/1`**, defined here in
  `docs/design/narrative/MISSION_FORMAT.md`. It **shares the chat-tree's exact `story_ops`
  condition/effect vocabulary** (the `app/story_ops` set) — no new ops invented. The only
  additions are *structural, non-moral* quest plumbing: `objectives`, `complete_when`,
  `branch`, `fail`, `timer`, and a `counter`/`counter_lt`/`counter_set`/`counter_add` op for
  reading host-tracked integers (enemy kills, crystals scanned, Salvari freed, evidence
  taken). All 14 missions pass `tools/check_missions.py` (14/14).
- **Story_ops I used** (all from `NPC_CHAT_TREE_FORMAT.md §3`):
  - *Conditions:* `flag`, `karma_gte`/`karma_lte`, `humanity_gte`/`love_gte`/`trust_gte`/
    `mercy_gte`/`redemption_gte` (selectively), `timeline`, `girl_saved`/`girl_lost`,
    `item`, `rel_gte`, `lua` (sparingly, e.g. `player_dead`, `companions_gathered`), plus the
    structural `counter`/`counter_lt`; composed with `any`/`not`.
  - *Effects:* `karma`/`mercy`/`trust`/`love`/`redemption` (±n), `set`/`clear`, `fire`
    (every world verb — `spawn_boss`, `map_ping`, `cutscene`, `dialog_open`, `grant_vehicle`,
    `evidence_register`, `hub_register`, `mission_start`, `act_transition`, …), `give`/`take`,
    `follow`, `ally`, `rel`, plus the structural `counter_*`.
  - **No new moral state.** Karma/Humanity/Trust/Mercy/Love/Redemption + timeline +
    captive-fate are all `timeline.h`'s existing axes (per `IMPLEMENTATION_NOTES.md`).
- **Tools added** (data-side, no engine): `tools/check_missions.py` (mission validator +
  op-vocab check, also `--trees` mode), `tools/gen_mission_md.py` and `tools/gen_tree_md.py`
  (derive the review `.md` scripts from the JSON so scripts can never drift from data).

---

## 1. Missions (14) — `missions/*.json` (+ `.md` review script each)

| # | id | Lvl | Location | Canon thread / hook | Drives / reads |
|---|----|-----|----------|---------------------|----------------|
| 1 | `act2_first_light` | 8 | Surface Emergence | Opening; sets `act2.surfaced`, `cradle.core_seen` (Cradle/Biomesh seed) | `act2_world` L8 host; Vesper `act2_grief`; chains → #2 |
| 2 | `act2_mourners_debt` | 10 | Crystalline Desert Depths | Injured-Salvari side-quest; Reptilian+Grey patrol; **Saurian Warlord** arena; **Mantis Arbiter** wildcard | `act2-desert` (warlord/grey-patrol/mantis-ambush); K'thara `first_contact`; chains → #3 |
| 3 | `act2_refugee_haven` | 11 | Salvari Camp | Alliance + cure; recruit **K'thara**; **Nordic Steward** mentor; camp = hub | `act2-desert` L11; `nordic-mentor`; K'thara/Steward trees |
| 4 | `act2_the_root_remembers` | 12 | Advanced Cave System / Crystal Heart | **Biomesh** reveal (e); earns the **bio-mesh scan**; **Memory Hunter**; Carrier **whisper** (reveal #1) | `act2-caves` L12; sets `mole.suspected`, `architect.markings_readable` |
| 5 | `act2_cradle_protocol` | 12 | Salvari Archives | **Cradle Protocol** (b) deepening; caste map; Clone-as-final-assembly; the 65-Myr orchard | reads `batch_records`/`harvest.known` (cross-checks #8/#12) |
| 6 | `act2_the_quiet_channel` | 11–12 | wherever the squad camps | **Carrier/mole** arc (c) — cure / double-agent / knife / buried | **DRIVES existing `lena.json` `carrier_confront/c0`**; reads `lena.cure_path`/`mole.weaponized`/`lena.final_choice` |
| 7 | `act2_the_siren_call` | 14 | Research Station | **Beta**: Aria → **The Siren** (only if `girl_lost:aria`); release/kill | `act2-roster` `BossTheSiren.glb`; Aria `infected_lost` |
| 8 | `act2_queens_territory` | 16 | Ruined Metropolis | **Beta**: Keisha → **Breeder Queen** (only if `girl_lost:keisha`); release/kill | `act2-roster` `BossBreederQueen.glb`; Keisha `infected_lost` |
| 9 | `act2_the_third_room` | 11–12 / −400 | Below Club 1127, substrate boundary | **Architect** arc (d); deep tunnels; lullaby; keep/silence the station | needs `biomesh.scan` (#4) + `rumor.third_room` (Vesper); Vesper `act2_architect` |
| 10 | `act2_the_drowned_disc` | 13–18 | Undersea Disc Base / trench | **Biomesh sea** (e) reveal #2; **Leviathan** = root's immune cell; kill/seal | systems-catalog Leviathan; K'thara `fc_disc`; sets `biomesh.root_living` |
| 11 | `act2_many_hands` | 18 | Underground Resistance HQ | Multi-species alliance forms; karma branch (unity vs strong-arm) | Steward `m_resistance`; sets `act2.alliance_formed`; chains → #14 |
| 12 | `act2_quartermasters_ledger` | 11 / 19 | Club 1127 + freight pipe | **Quartermaster** heist (a); the pre-dated order; evidence package | Vesper `act2_quartermaster`; sets `quartermaster.ledger_held` (pays off at #14) |
| 13 | `act2_batch_records` | 11 | Club 1127 (Reyes) | Reyes flee payoff; 3rd evidence piece; witness vs extort | reads `reyes.fled`; Vesper `act2_reyes` + Reyes `club_hideout` |
| 14 | `act2_storm_runner` | 19–20 | Spaceport | **Act-2 climax**: Defense + **Garrison Commander**, capture the **Storm Runner**; optional **Quartermaster** in-person confront | needs `act2.alliance_formed`; `act_transition → 3` |

**Spine flow:** #1 → #2 → #3 → (#4 ↔ #5 knowledge) → #11 → #14 is the critical path. #6
(carrier), #7/#8 (Beta bosses), #9 (Architect), #10 (drowned disc), #12/#13 (collaborator
evidence) are optional, gated, discoverable side content keyed to player state — exactly the
open-world shape Act 2 calls for.

---

## 2. New chat trees (6) — `chat_trees/*.json` (+ `.md` review script each)

| file | npc | trees | role |
|------|-----|-------|------|
| `kthara.json` | `kthara` | `first_contact` (multi-mission, node-fired), `banter`, `bond` | **K'thara**, Salvari leader/companion; canon Beta-romance (gated to Beta). Drives missions #2/#3/#4/#10/#5. |
| `nordic_steward.json` | `nordic_steward` | `mentor`, `banter` | **Nordic Steward** mentor at the L11 upgrade station; vouches you into the Resistance (#3/#11). |
| `mantis_arbiter.json` | `mantis_arbiter` | `wildcard` | **Mantis Arbiter** neutral wildcard; the Carrier-suspicion bargain (#2, gated on `mole.suspected`). |
| `quartermaster.json` | `quartermaster` | `confront` | **The Quartermaster** — civilian antagonist; the only in-person scene, cornered at the Spaceport (#14). Spare→witness / kill / arrest. |
| `club1127_vesper_act2.json` | `vesper` | `act2_quartermaster`, `act2_architect`, `act2_reyes`, `act2_grief` | **ADDITIVE Vesper trees** (merge into existing `club1127_vesper.json` by npc id; `hub` untouched). |
| `act2_reyes_club.json` | `reyes` | `club_hideout` | **ADDITIVE Reyes tree** (merge into existing `dr_reyes.json` by npc id). The fled-collaborator handover (#13). |

**Reused, not re-authored (per the no-duplication rule):** Lena's `carrier_confront` +
`infected_lost` (existing — mission #6 drives them); Aria/Keisha `infected_lost` (existing —
#7/#8 drive them); Vesper's `hub` + the codes/architect/quartermaster *lore* nodes (existing
— the new Vesper trees are the mission *scenes*, the hub stays the rumor index); Reyes's
flee/testify flags (existing).

---

## 3. How it hooks the existing `feat/act2-*` engine branches

The engine branches are C++ slices (`app/act2_world.*`, `act2_desert.*`, `act2_caves.*`,
roster rows + boss GLBs). This pack is data; it **references their canon entities and level
numbers** so the two lanes meet:

- **`feat/act2-world`** — L8 Surface Emergence + the `Act2Level` enum (L8..L20, canonical
  numbering): mission #1 sits on it; all `level` fields use these numbers.
- **`feat/act2-desert` (+ warlord / grey-patrol / mantis-ambush / nordic-mentor)** — L10
  injured-Salvari hook, Reptilian+`GreyTasked` patrol, the **Saurian Warlord** gated arena,
  the **Mantis Arbiter** wildcard, the **Nordic Steward** mentor at L11: missions #2/#3 wrap
  all of these into quests; the Steward + Mantis trees are authored here.
- **`feat/act2-caves`** — L12 Crystal Heart: mission #4.
- **`feat/act2-roster`** — `SalvariAlly`, `NativeDesertFauna`, and the Beta bosses
  **The Siren** (`BossTheSiren.glb`) / **Breeder Queen** (`BossBreederQueen.glb`): missions
  #7/#8 spawn them via `spawn_boss` with the right model names; K'thara is the companion the
  `SalvariAlly` row gestures at.
- **Canon-aliens** (Reptilian Overlords, Greys, Nordics, Mantis): referenced by the species
  names the branches already shipped (`SaurianWarlord`, `GreyTasked`, `NordicSteward`,
  `MantisArbiter`) — no renames, no new roster rows requested.

`spawn_boss` events name bosses the engine boss machine already handles (per
`IMPLEMENTATION_NOTES.md`: trees/missions exit into the existing `BossPhase` fight; missions
never define combat).

---

## 4. The two cross-act evidence/ending payoffs (overlay-ready)

Per `STORYLINE_EXPANSIONS.md (f)`, these missions *write the state the epilogue overlays
read* — no new endings, no new gates:
- **Collaborator-trials package** (f1 LANTERNS): three pieces, each `evidence_register`-ed
  into `collaborator_trials` — Aria's manifest (existing `aria_manifest`), the
  **Quartermaster's ledger** (#12), the **batch records** (#13). Martinez (talked-down/spared)
  is the natural prosecutor. The Quartermaster (#14, spared) is the witness.
- **Carrier final-whisper** (f3 HOLLOW CROWN): #6's `mole.weaponized` path is the
  never-cured strand the dark epilogue pays off.

---

## 5. Canon questions flagged for Tim

1. **`x3.mission/1` is a NEW format.** No mission-format doc existed on any ref (the prompt
   said it was "now-landed" but I found none on `integration/empire-fold` or the docs
   branches). I defined it to **share the chat-tree `story_ops` vocabulary** exactly, so the
   engine's mission runner can reuse the chat-tree evaluator. **Confirm the format** (or
   point me at the real one and I'll re-key the files).
2. **Carrier whisper delivery.** Mission #4's whisper beat fires a host event
   `carrier_whisper {npc, volume:"low"}` rather than opening Lena's full `infected_lost`
   tree (which is her boss-intro). Confirm the host should play one ambient line at whisper
   volume from her `infected_lost` pool (the STORYLINE_EXPANSIONS (c) intent), or whether you
   want a dedicated low-volume line authored.
3. **K'thara romance lane.** I followed `IMPLEMENTATION_NOTES.md`: K'thara romance lives in
   **Beta** (where the F2 women are bosses, not companions), gated by `timeline:["Beta"]`.
   Confirm she's also available in Alpha (poly-family), or Beta-exclusive as written.
4. **Drowned Disc level placement.** Canon L8–20 has no explicit "Undersea Disc Base" as a
   numbered level (the undersea/Leviathan content is a systems-catalog biome, and L13 is the
   Toxic Swamplands). I scoped #10 as a **side excursion spanning L13–18** off the surface.
   Confirm a home level, or keep it as a discoverable optional descent.
5. **Beta boss "release" vs canon "kill."** #7/#8 offer a mercy **release** path (lay the
   transformed woman to rest) alongside the canon kill. This is additive and feeds the
   redemption overlays; confirm you want the release path or kill-only.
6. **Evidence ids.** I used `quartermaster_ledger`, `batch_records`, and (existing)
   `aria_manifest` as the three trial pieces, registered under `collaborator_trials`.
   Confirm these string ids (the host inventory uses string ids per the format doc).

---

## 6. Constraints honored

- Branch `feat/act2-content-offensive` only; `main` and `integration/empire-fold` untouched;
  no force-push, no history rewrite; incremental commits with the required co-author trailer.
- Text-only (no LFS guard risk). Additive; changes no code; default-inert until referenced.
- Trauma/rescue restraint held: the Cradle horror is *explained with grief, never depicted*;
  the Beta-boss and Carrier beats trade in recognition and aftermath, not explicitness.
