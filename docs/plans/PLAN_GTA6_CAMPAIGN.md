# THE GTA VI CAMPAIGN — the plan to reach (and beat) the leak

Companion to `PUNCHLIST_GTA6_MATCH.md` (the itemized list). This is the SEQUENCE —
what order, why, who, and the gate each phase must pass. Tim's thesis, verified
against the tree 2026-08-20: **we are not missing systems, we are missing
integrity and density.** The engine already ships traffic with driver profiles,
one-truth water with swimming, weather with monsoon rain, day/night, 12 weapons,
vehicles with NFS handling + fuel stations, interiors, a GTA-style map, radio
audio (Ogg), a mission runner, LLM-backed NPCs, destruction, and RT on the 5090.
GTA VI beats us today on: material/lighting integrity, content density per
square meter, and reaction polish. Attack in that order — each phase compounds
the next.

## PHASE 0 — INTEGRITY (the multiplier, ~days)
Nothing else pays off until every asset renders what its artist authored.
- [x] 0.1 **mr-factors opt-in sweep** — DONE 2026-08-25 (fold/phase0-integrity):
      all 11 ModelDrawable sites + SceneEntity pass-through fields. 115/152
      GLBs author factor-dependent materials, now honored everywhere. No-regression
      proven (complex/crowd byte-identical, tunnel at noise floor); 9 suites green.
- [x] 0.2 **filmic-post** — VERIFIED 2026-08-25: already folded; ACES filmic is
      the default tonemap (r_tonemap=1), cutscene extras behind r_filmic by design.
- [x] 0.3 **Aerial perspective grade** — DONE 2026-08-25 (3169e87e): fog.frag
      height-falloff integral + two-tone sky melt; the dry outdoor world (which
      ran NO fog) now breathes graded air in gameplay AND captures. Receipts:
      40 dB long views / 56 dB close, vista eyes-on clean. NEXT REFINEMENT:
      A/B against reference f015/f045 once city-density content lands (the flat
      far terrain + grey water the vista exposed are Phase 1/terrain work).
- [x] 0.4 **Normal-map audit follow-through** — DONE 2026-08-25: loader decode
      failures already fall back + warn; every load now prints a `nrm=X/Y` bind
      receipt. Content gap quantified: **85/152 GLBs author ZERO normal maps**
      (all 8 town houses, CTR/Muscle/Skyline, 3 traffic sedans, the sci-fi kits)
      — list + fix recipe in NORMAL_MAP_AUDIT_0825.md, feeds Phase 1 (Lane B/E).
- [x] 0.x **r_fog live dials** (Tim's ask) — r_fogdensity/start/height/skyblend/max
      reach the outdoor host from console AND `--set` (audit-claimed, logged);
      heavy/off A/Bs at 24.8/27.6 dB from default.

## PHASE 1 — THE LOOK (the leak's screenshots, ~1-2 weeks)
- 1.1 Cloud light transport (thickness transmittance + sun rim on W-CLOUDS).
- 1.2 Roofscape furniture pass (AC/parapets/antennae via unitypackage kits).
- 1.3 Decal system: persistent bullet holes (engine gib/impact FX exist; decals
      are the missing persistence layer).
- 1.4 Grime & litter scatter along wall bases; garbage bags, conduit props.
- 1.5 Foliage translucency — the engine TERM ALREADY EXISTS (foliage param on
      drawMeshPBR); set it per-material on tree/bush assets. Cheap, huge.
- 1.6 Night-city pass vs their night footage (W-NIGHT + real flashlight cone
      landed; A/B against the leak's night frames, fix deltas).
- 1.7 UI depth-of-field blur behind wheel/pause (shared-shell post hook).

## PHASE 2 — THE FEEL (systems polish, ~1-2 weeks, parallel-safe with 1)
- 2.1 Weapon wheel (8 slots + 2 healing) in the shared HostShell — we have the
      12-key roster; the wheel is presentation + the two consumable slots.
- 2.2 Radio wheel + stations (Ogg decode + jukebox lanes exist; station chips,
      per-station playlists, Symphony-FM-style now-playing line).
- 2.3 Wanted v1: stars, cop convergence (npcLife converge exists), kill-the-
      last-pursuer-clears, police-vehicle GPS blip.
- 2.4 Fuel gauge that drains + gates the engine (stations/pumps SHIPPED).
- 2.5 Gun-mounted flashlight (spot cone SHIPPED — parent to muzzle, toggle).
- 2.6 Taser -> stun -> DISARM -> finisher chain (freeze-tint + unarmed strikes
      exist as precedents; slow-mo cam for the finisher).
- 2.7 Healing pills w/ diminishing returns, bought at vendors (DODOG economy).

## PHASE 3 — THE WORLD (density + charm, ongoing)
- 3.1 Enterable fast-food joints: order/buy/eat/health (noodle-bar patron loop
      is the template; 2-3 franchises across the map).
- 3.2 NPC reactions & taunts: aim-at/near-miss/bump triggers -> short barks;
      LLM line generation for the close-range conversations GTA CANNOT DO.
- 3.3 District danger profiles (per-district npcLife aggression).
- 3.4 Car-theft depth: window smash vs key clone; safehouse vehicle storage.
- 3.5 Grab/wrestle + hat knock-off/pickup (citizen-depth pillar).
- 3.6 Basketball micro-activity; wing-walking (deck-aware contact law on
      airframes) for the trailer moment.
- 3.7 City block-fill density sweep — the 914 Unity packs, kit by kit, on the
      crown grid (#34's spirit, now on the new frame).

## WHERE WE BEAT THEM (press these, don't just chase parity)
- **LLM citizens** — every NPC can actually converse; theirs cannot.
- **WD2-style hacking + skilltree** — wired and city-wide (383 hackables).
- **Full engine ownership** — RT tier, day-one mod console, cvar everything.
- **The fleet** — parallel agent lanes ship a wave per day at current cadence.

## DISCIPLINE (unchanged, non-negotiable)
One session owns a file; capture-review before Tim sees anything; every item
gates on an A/B against a named reference frame; suites hold or raise their
numbers; no refiling — an item leaves this plan by SHIPPING, not by moving to
a newer document.

## SUGGESTED LANE ASSIGNMENTS (next fresh wave)
- LANE A (render): 0.2 -> 0.3 -> 1.1 -> 1.6   - LANE B (materials): 0.1 -> 0.4 -> 1.5 -> 1.3
- LANE C (UI/feel): 2.1 -> 2.2 -> 2.4 -> 2.5   - LANE D (combat): 2.3 -> 2.6 -> 3.2
- LANE E (world): 1.2 -> 1.4 -> 3.7 -> 3.1     - Integrator: folds + the flyover gate
