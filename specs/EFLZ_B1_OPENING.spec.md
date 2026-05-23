# EFLZ — B1 Detention Cell: The Opening (Level 1, "Awakening")

> The first 90 seconds of the game. Canon from Tim (the original Escape Lab Zero), 2026-05-22.
> Rebuilt native in X3Native — the strength-reveal + bend-the-bars escape that Babylon handled badly.

## The sequence (beat by beat)

1. **Wake on your back.** Jake starts SUPINE on a medical slab/floor, camera looking UP at the cell
   ceiling. Restraints on **both wrists and both ankles** (visible cuffs/straps). Player input is
   limited (look only) for the first beat.
2. **Struggle (10–15s).** Jake instinctively tries to MOVE and can't — pressing WASD/move makes the
   view jerk/strain against the restraints (he physically tests them), building helplessness. This runs
   ~10–15 seconds of escalating struggle BEFORE the game tells the player what to do — the player feels
   trapped first.
3. **Realize → strain → SNAP, one at a time.** After the struggle builds, a `HOLD [key]` prompt
   appears. Holding fills a strain meter and snaps restraints **sequentially** — one restraint breaks,
   then another (each its own SNAP SFX + jolt): e.g. right wrist → left wrist → ankles. The last snap
   is the +400% reveal payoff. Then Jake RISES to standing and full FPS control is handed over
   (screen-shake / heavy SFX on the final break).
4. **The terminal.** Jake walks to the wall **terminal** (the SM_Console). Its SCREEN shows the
   readout IN-WORLD (not a floating HUD overlay):
   `SUBJECT: 7-ALPHA (JAKE)` · `MUSCULOSKELETAL OUTPUT: +400%` · `RESTRAINT INTEGRITY: FAILED`.
   This is the holographic terminal (SD-gen translucent panel, or emissive textured quad on the screen).
5. **The locked door.** Jake tries the cell door — it's **locked, needs a keypad code**. The code is
   **posted on the OUTSIDE of the cell** (a sign/decal on the far side of the bars) — readable through
   the bars but the keypad is outside reach, so you can't just punch it in from inside. (Teaches: brute
   force, not the code, is your way out — for now.)
6. **Bend the bars.** The cell front is **BARS** (not a solid wall). With +400% strength, Jake grabs
   **two adjacent bars** and **bends them outward** (interact + hold; the bars deform/animate apart),
   opening a gap. Jake **squeezes through** the gap → out of the cell. Strength mechanic payoff.
7. Out into the B1 security wing → existing Spire flow (find pistol, Martinez gate, elevator).

## Cleaner-than-Babylon implementation notes (X3Native)

- **Cell is BIGGER** than the current graybox — give the wake-up + walk-to-terminal room to breathe.
- **Bars**: real thin cylinder/box geometry, a row of vertical bars forming the cell front + door.
  The two "bendable" bars are individually addressable props.
- **Bend mechanic**: interact (E/hold) on the bendable bars → lerp their top/middle vertices outward
  (a simple bend = rotate/translate the bar mesh around its base, or a 2-keyframe deform) over ~1s,
  widening the gap to a walk-through width. Gate behind `game.strengthRevealed()` (post-snap). Once
  bent, a trigger lets the player pass (collision opens).
- **Restraint-snap intro**: a short scripted state at level start (supine camera, strain input → snap),
  then release to normal FPS control. Skippable after first play (or hold-to-skip).
- **Terminal text → world space**: render the readout onto the terminal screen — either (a) an emissive
  textured quad (SD holographic panel) on the SM_Console's screen face, or (b) world-anchored text via
  a screen-facing quad. REMOVE the floating-HUD version (main.cpp awakenTimer block) once the world
  version reads well.
- **External code decal**: a sign/quad on the OUTSIDE face of the cell bars showing the door code
  (e.g. matches the keypad code). Visible from inside through the bars.

## Acceptance
On `--world` default (B1): wake supine → strain → restraints snap → stand → read the terminal screen
in-world (+400%) → door locked (code shown outside) → bend two bars → squeeze through → into B1 wing.
Headless `--test-*` covers: strength-reveal gating, bend-opens-gap, door-locked-without-code.

## Status / build order
- [ ] World-space terminal readout (move off floating HUD) — FIRST (small, addresses "it's on the terminal")
- [ ] Bigger cell + bar geometry (front wall + door as bars)
- [ ] Bendable-bars mechanic (gated on strength reveal) + squeeze-through trigger
- [ ] Restraint-snap wake-up intro sequence
- [ ] External code decal on the cell exterior
