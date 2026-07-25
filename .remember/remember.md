# Handoff

## State
Playability wave committed (`ee16657`) but NOT YET BUILT — Tim's running exe locks the link;
build+verify the moment his game closes (5x retry loop). Fixes in it: volumetrics default-OFF
(100ms/frame, ECHO_VOL=1 opts in), npcLife LLM disabled (23 boot generations ground the CPU;
10->30 FPS measured), llama threads 16->6, sunLight washout 3.2->1.05, chat feed at human pace
(20s cadence, 22 chars/s reveal, bottom-right CITY FEED), FPS in HUD bar, play-as A/D flipped.
Meshy: Tim topped up to ~3000 credits after the double-spend (~$20, forgiven once). 7 finished
assets in assets/meshy/ (2 rigged chars w/ 6 clips, articulated turret, cruiser, van, kiosk,
terminal) + paid-but-unrefined preview task IDs in scratchpad/eh_meshy/meshy_state.json — THEY
EXPIRE ~3 DAYS (download free). Gamma fix (00fea56) + sun elevation fix (a5a56a6e) both landed.

## Next
1. Build + verify ee16657 in-game (FPS, washout, chat pacing). Then remaining ~33ms = draw-record
   volume from districts/woodlands — culling/instancing pass.
2. PLAY-AS BUG (Tim live report): character model visible only through a narrow yaw arc —
   culling or skinned-pose bug. Also wanted: default FLY mode (WASD+mouselook, QE roll, Space/C
   up/down, arrows turn — note setCamera has no roll param) and a console for this world.
3. Integrate Meshy assets (scale multipliers in the lane report / meshy_state.json) + finish
   expiring previews with the new credits — ONE lane only.

## Context
- ⚠ RULE (cost $20): NEVER run two agent lanes against one paid API balance. One asset lane at a
  time, with an explicit budget, and check for in-flight lanes first.
- Exe locked = LNK1104: Tim playing blocks builds. Ask/check before building.
- The talk system works (bubbles + persona chats, seen in Tim's screenshots). Smallest-gguf
  auto-selection + ECHO_LLM_MODEL override are in. Feed at bottom-right; resident card bottom-left.
- docs/HANDOFF_2026-07-24.md §8-11 = engine roadmap (DDGI never enabled = biggest visual win).
