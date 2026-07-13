# Ship Flight Modes + Glassy-Neon Cockpit — Design

**Date:** 2026-07-09
**Owner:** Tim (Commander) + Integrator (13700K)
**Goal:** Make piloting the ship FUN and the cockpit interior SLEEK.
**Base branch:** `feat/intro-cockpit` (has the yaw-axis fix + arcade nose-follow the
baseline lacks). Work in a dedicated worktree; do not disturb Snake's checkout.

## Two components (independent; controls first)

### A. Three selectable FLIGHT MODES
All feel params already live in one struct — `SpacePilotController::Tuning`
(`app/space_pilot.h`). Three named presets + a mode switcher. A shared "smooth +
juice" layer applies to all three, tuned per mode.

| Mode | Feel target | Character |
|---|---|---|
| **ARCADE** (default) | Star Fox | Snappy, strong auto-bank + auto-level, forgiving, light inertia, moderate FOV punch |
| **ASSIST** | Elite | Weighty momentum + glide/drift, flight-assist damping (satisfying not fighty), FOV punch at speed, subtle bank, low auto-level |
| **LOOSE** | drift/adrenaline | High top speed, aggressive roll/yaw, drifty, big FOV punch + screen-shake, minimal auto-level |

**Shared smooth + juice layer** (in `space_pilot.cpp`, NOT `main.cpp`):
1. **Look-smoothing** — lerp the raw mouse delta before applying (the core "not
   smooth" fix; today it's applied instantly at space_pilot.cpp:162-164).
2. **Auto-bank** into yaw turns + **auto-level** hands-off (rate per mode).
3. **FOV-by-speed** — widen FOV with speed/boost (add `fov` output to `camera()`;
   replaces the hardcoded `65.0f` literal so `main.cpp` isn't touched per-world).
4. **Chase-cam follow-smoothing** + look-ahead (replaces the rigid 3P cam).
5. **Screen-shake** on boost (amplitude per mode).
6. Per-mode drag/accel/speed/nose-follow tuning (snappy vs weighty vs loose).

**Selection (both):**
- **Settings menu:** "Flight Mode" radio — Arcade / Assist / Loose. Persisted.
- **Game console:** `flightmode arcade|assist|loose` — hot-swap while flying
  (A/B the modes back-to-back). Registered with the existing console commands.

**Banking fork (decided):** true first-person horizon-roll needs a renderer
"view-up" axis that doesn't exist yet (camera() outputs yaw+pitch only). **v1:
bank in 3rd-person (ship rolls) + camera-roll where the renderer allows; defer
true 1P horizon-roll** to a fast follow-up. Ships all three modes now without a
renderer detour.

### B. Glassy-neon cyberpunk COCKPIT INTERIOR
Procedural reskin of `app/space/ship_interior.cpp` (NOT the warm-amber Firefly
GLB kit — wrong vibe for the cyberpunk pick). The Slick look in 3D:
- Dark **glassy panels** (low albedo, cool tint) replacing the flat blue-grey.
- **Cyan/magenta emissive light-strips** along floor/wall/ceiling seams.
- **Animated holographic console sweeps** on the helm/nav station markers.
- Cool **neon point-lights** (cyan key + magenta accent) replacing the neutral fill.
- Glassy window frames tying into the existing S6 portal windows.

## Gates
- Builds clean: `cmake --build --preset windows-vs2026`, `X3Engine.exe` runs.
- `--test-space` self-test stays green; add a mode-switch assertion.
- Real-GPU eyes-on captures (`--world space --screenshot`, `--world ship-interior
  --screenshot`) inspected at full-res before "done" (anti-slop).
- Determinism of the sim unaffected (flight feel is presentation/control, but the
  velocity integration is sim — keep mode switching deterministic given same input).

## Sequencing
1. Worktree off `feat/intro-cockpit`.
2. 3-mode flight system + juice layer + console command + menu entry.
3. Neon cockpit interior.
4. Captures, self-test, push a branch for Predator's compare-and-merge review.

## Not doing (v1)
- True 1P horizon-roll (renderer view-up) — fast follow-up.
- GLB Firefly kit interior — procedural neon first; kit detail can layer later.
- Refactoring the 8,700-line `main.cpp` monolith — out of scope; keep changes in
  the controller/interior TUs.
