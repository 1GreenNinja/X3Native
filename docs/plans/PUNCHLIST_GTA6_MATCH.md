# PUNCHLIST — MATCH THE GTA VI LEAK (Tim's reference, 2026-08-20)

Source: Tim's 2:21 reference clip (GTA VI gameplay leak — Vice City biplane flyover,
radio wheel, third-person shooting, foliage walk) + the two Beebom leak articles
(weapon wheel / taunting NPCs / fast-food joints; nighttime footage / tasers /
disarm mechanic). NOTE: the clip circulating has a crypto-spam overlay (QR + mirror
domains) baked in — analysis ignores it; do not scan or reproduce it.

Ranked by leverage: what makes THAT footage read photoreal, mapped to what we have.

## RENDERING (the look)
- [ ] **G1. Aerial perspective grade** — distance haze that desaturates, blue-shifts
      and lifts blacks progressively. THE single highest-leverage move for flyover
      shots. We have `fix/exterior-atmosphere` merged; `feat/filmic-post` is UNFOLDED
      — fold it, then add a height-fog + distance LUT. Gate: our own biplane-altitude
      capture over the city vs frame f015/f045 of the reference.
- [ ] **G2. mr-factors eyes-on + fold** (`wip/mr-factors`, f174eaef) — correct
      metallic/roughness everywhere is the foundation of "nothing reads as game."
      Chalky ghost cars are the counter-example. IN FLIGHT on this lane.
- [ ] **G3. Cloud light transport** — their cumulus self-shadows and rims silver at
      the sun. W-CLOUDS fBm deck + ground shadows shipped; add thickness-based
      transmittance + sun-rim term in the cloud shader.
- [ ] **G4. Filmic tonemap + soft highlight rolloff** — `feat/filmic-post` branch
      exists. Bloom subtle, highlights never clip. (HDR10/PQ output = later flourish.)
- [ ] **G5. Foliage translucency** — leaves transmit sun (two-sided + transmission
      term in mesh.frag) so close-up foliage doesn't go black. Reference frame f070.
- [ ] **G6. Decal system** — persistent bullet holes on walls (their stucco scene
      accumulates impacts). We have gibs/debris but no surface decals.
- [ ] **G7. Grime & litter pass** — two-tone wall bands, garbage bags with specular
      sheen, litter drifts at wall bases, wires/conduit props. Asset-scatter work
      (914 Unity packs via tools/unitypackage_extract.py).
- [ ] **G8. Roofscape detail** — AC units, parapets, antennae, water tanks on every
      flat roof; aerial shots die without roof furniture.
- [ ] **G9. UI depth-of-field** — full-screen blur + desaturate behind wheel/pause
      UI (their radio-wheel shot). Shared-shell post hook.

## GAMEPLAY (the feel)
- [ ] **P1. Weapon wheel** — radial selector in the shared HostShell UI (we have
      12-weapon roster on number keys; wheel is the console-parity layer).
- [ ] **P2. Radio wheel + stations** — we have Ogg Vorbis decode + jukebox lanes;
      station chips UI + per-station playlists.
- [ ] **P3. Reactive/taunting NPCs** — NPCs comment/taunt on player actions (aim at
      them, near-miss, fender-bender). We have npcLife converge + LLM talk — wire
      reaction triggers to short barks.
- [ ] **P4. Enterable fast-food joints** — order, buy, eat, health tick. DODOG cart
      + noodle-bar patron loop are the seed; this is the interiors lane's next rung.
- [ ] **P5. Taser + disarm mechanic** — non-lethal branch: taser stun (we have freeze
      tint precedent) + shooting/striking a weapon out of an enemy's hands.
- [ ] **P6. Wing-walking** — player stands on a moving aircraft surface. Deck-aware
      contact law + planes exist; extend the law to airframe surfaces.
- [ ] **P7. Nighttime reference pass** — their night footage vs our W-NIGHT + real
      flashlight cone: capture-pair comparison, fix the deltas.

## FROM THE FULL LEAK ROUNDUP (Beebom "everything we spotted")
Items where we ALREADY have the seed in the tree are marked ◆.
- [ ] **P8. Wanted system** — star display, cops converge, killing the last pursuer
      clears the level; GPS tracker on police vehicles. ◆ wanted-lite is Lane 1's
      brief (hack alarm -> cop converge exists in npcLife).
- [ ] **P9. Fuel system** — ◆ W-STATIONS shipped refuel prompts + pumps; add a fuel
      gauge that actually drains and gates the engine.
- [ ] **P10. Gun-mounted flashlight** — ◆ the engine just gained the spot cone;
      parent one to the weapon muzzle, toggleable.
- [ ] **P11. Weapon wheel spec** — 8 weapon slots + 2 healing-item slots (refines P1).
- [ ] **P12. Healing items w/ diminishing returns** — pills consumable; each use
      heals less. Pairs with the vendor economy (DODOG precedent).
- [ ] **P13. Finishers + injury states** — slow-mo cinematic finisher after a stun
      (extends P5 taser->disarm chain); visible cuts/wounds on the player model.
- [ ] **P14. Car-theft depth** — key cloning vs window smash choice; vehicle storage
      at safehouse.
- [ ] **P15. Grab/wrestle + NPC accessory pickup** — grab a civilian; knock a hat
      off and wear it. (Citizen-depth pillar #28 material.)
- [ ] **P16. District danger profile** — hostile NPC zones (their homeless-hostile
      areas): per-district aggression tuning in npcLife.
- [ ] **P17. Basketball micro-activity** — one court, one ball, one shot arc near a
      safehouse. Small, high-charm.

## VERIFY DISCIPLINE
Every item gates on an A/B capture against a named reference frame, read at full
res, before it is called done (capture-review law).
