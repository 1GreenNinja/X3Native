# Settings audio polish — apply in the build AFTER the door/crouch agent lands

(ui.cpp only; the door agent doesn't touch ui.cpp, so just don't build concurrently.)

## 1. Separate the audio group from the render toggles with a little space
`app/ui.cpp` ~line 438, BEFORE the Music toggle row (after the RT-AO row at ~436):
```cpp
    if (ui.toggle("RT AO (ray-traced)", model.rtao, rx, ry, rw, rh)) { model.rtao = !model.rtao; outChanged = true; } ry += rh + gap;

    ry += gap * 1.5f;   // <-- ADD: a little gap separating AUDIO from the render/display group

    // ---- Audio rows: Music on/off + Music & SFX volume ----
    if (ui.toggle("Music", ...
```
(Bump the panel height `panelH` by ~`gap*1.5f` too so the rows still fit — see line ~389 "Taller panel".)

## 2. Sliders slightly shorter so the track doesn't overrun the label text
`app/ui.cpp` `UiContext::slider()` line ~220: the label gets only `labelW = 132.0f`, so
"Music Volume"/"SFX Volume" overrun into the track. Bump it:
```cpp
const float labelW = 164.0f;   // was 132 — reserve more so "Music Volume"/"SFX Volume" don't overrun the track
```
That starts the track further right (slightly shorter track), clearing the label. (The track
auto-recomputes from labelW via trackX/trackW, so no other change needed.)

Gate after applying: `--test-ui` still green (the U22-U27 slider-drag tests use the track
geometry; a wider labelW shifts the track right — if a drag test pins an exact x, nudge the
test's click x, don't revert the fix), smoketest 0-VUID/alloc=0.
