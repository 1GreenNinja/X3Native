# Task #21 — auto-weapon fire sound roars 5-7s after release

CAUSE: `IAudioSystem` has only one-shot `playSound2D/3D` (no stoppable SFX voice — only
`playMusic`/`stopMusic` for the single music bed). The auto weapons (chaingun, smg) set a
LONG "Rapid-Fires" burst WAV as `fireSfx`, and main.cpp:~5960 plays it as a one-shot PER
ROUND. At ~14 rounds/s a brief trigger pull stacks a dozen overlapping 1-2s burst clips →
5-7s of gunfire after you let go.

## FIX A (no engine change — RECOMMENDED): short single-shot per round
In `app/weapon.cpp`, change the auto weapons' `fireSfx` from the long rapid-burst WAV to a
SHORT single-shot WAV (like the pistol uses), so each bullet is one short crack that stops
the instant you stop firing. (At 14/s short cracks still read as rapid auto fire.)

```cpp
// ChainGun (was "weapons/rapid/Rapid-Fires_Sci-Fi_Gun-49.wav"):
w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-30.wav";  // short -> stops on release
w.fireSfxLoop = false;   // not looped; per-round one-shot
// SMG (was "weapons/rapid/Rapid-Fires_Sci-Fi_Gun-01.wav"):
w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-57.wav";
w.fireSfxLoop = false;
```
Pick any short `weapons/single/*.wav` that isn't already used by pistol/shotgun (verify the
file exists; pitch them down slightly in the play call if a heavier auto feel is wanted —
`playSound3D(snd, x,y,z, 0.85f, /*pitch*/ 0.9f)`). weapon.cpp is NOT in the level agent's
scope, so this is safe to apply independently of the canonlevel rebuild.

## FIX B (proper, later — needs an engine-side API): managed loop voice
Extend `engine/audio/IAudioSystem.h` (13700K's lane) with a stoppable looping voice:
`VoiceHandle startLoop(SoundHandle, vol, pitch); void stopLoop(VoiceHandle);`. Then in
main.cpp: on the first auto-fire frame `startLoop(rapidWav)`, and on `!fireHeld` (or weapon
switch) `stopLoop()`. Gives a true sustained minigun whine that cuts on release. Coordinate
with the 13700K since it touches the audio interface. FIX A is enough to kill the bug now.

Gate: keep `--test-audio` + all `--test-*` green; smoketest 0 VUID + allocationCount=0.
```
