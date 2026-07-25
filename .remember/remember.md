# Handoff

## State
Branch echotropolis @ `359d3142` — big day, all committed + built (exe current):
FLY MODE default (V orbit, G ground<->flight w/ land+takeoff), ENGINE console wired
(` toggle; tod/todpause/fly/orbit/walk/tp/pos/vol/sun/amb/haze/vsync/cull/screenshot),
NpcSkin rigged named citizens (Meshy cop/vendor + roster, 23/23), and TWO root-cause
fixes for "model visible only in narrow arc": (1) glTF skinned-node transform ignored
per spec in ModelLoader makeDrawables (Meshy rigs rendered displaced — fleet notified),
(2) ride-along boom now cranes over terrain (LOS raycast). Proof captures in captures/
playas_*.png show the Meshy vendor center-frame through a full 360.

## Next
1. Tim to relaunch + verify: rigged citizens, play-as visibility, G toggle, console.
   His tuning knobs: `sun <x>`, `amb <x>`, `haze <x>` — bake whatever numbers he likes.
2. UFO "narrow arc" report — may be fixed by the skinned-node fix if the UFO GLB is
   animated; if still arcing, have Tim run `cull off` and report (isolates frustum cull).
3. Grass texture reads as green blur close-up (visible in captures) — terrain detail
   texture/tiling pass. Also FPS ~17-27 in dense views: draw-record culling pass next.
4. Meshy previews expire ~2 days (scratchpad/eh_meshy/meshy_state.json), 3000 credits,
   ONE lane only. Babylon console (Q3Engine x3-console.js, ~180 cmds) = mine for wave 3.

## Context
- Harness: ECHO_PLAYAS_DEMO=1 (+_SPIN=0 static, +_FLYCAM=1 inspect) writes
  captures/playas_*.png; needs ~50s run (captureFrame after endFrame finalizes).
- Don't launch test instances while Tim plays (GPU contention + he sees the windows).
- hud.h Hud + IConsole are the engine console — never hand-roll one again.
