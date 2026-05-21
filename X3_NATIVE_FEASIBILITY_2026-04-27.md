# X3 Engine → BabylonNative — Feasibility Audit (2026-04-27)

**Goal:** ship X3 as a native binary on Steam by porting it onto [BabylonNative](https://github.com/BabylonJS/BabylonNative) instead of Electron-wrapping the WebGL build.

**Method:** static analysis of all 164 modules (~148.6K LOC) under `src/` for browser-API surface area; classification by porting difficulty.

---

## Executive Summary

| Verdict | **TRACTABLE.** A motivated 4-6 week solo effort, or 2-3 weeks with focused agent help. |
|---|---|
| **Tier A (zero-change pure logic)** | **56 files, 42,342 LOC (28%)** — pure Babylon.js + math, no browser APIs. |
| **Tier B (window globals only — light polyfill)** | 25 files, 23,713 LOC (16%). |
| **Tier C (DOM-bound — UI rewrite)** | **65 files, 68,257 LOC (46%)** — must move to Babylon GUI. |
| **Tier D (subsystem replacement)** | Audio: 15 files / Save+IO: 27 files. Bounded scope; clean swap-out points. |
| **Risk-1: Havok physics** | Babylon team integrated Havok into BabylonNative; verify version match before committing. **133 `PhysicsAggregate` callsites** in X3 ride on this. |
| **Risk-2: GPU particles** | 6 callsites use `GPUParticleSystem`. BabylonNative's bgfx backend has supported them on D3D11/12/Vulkan/Metal for a while, but worth a hello-world test. |
| **Risk-3: Recast NavMesh** | X3 uses `babylonjs.recast.js`. Must ship the native build of recastnavigation; doable but not auto. |

---

## Section 1 — Browser-API Inventory

### DOM usage (top offenders, sorted by hit count)

| Hits | File | Role |
|---:|---|---|
| 101 | `features/x3-pause-menu.js` | menu UI — full rewrite |
| 93 | `systems/x3-input-bindings.js` | keyboard/mouse → engine input |
| 92 | `features/x3-photo-mode.js` | photo-mode UI |
| 91 | `features/x3-dialog.js` | NPC dialog UI + speech bubbles |
| 77 | `systems/x3-endings.js` | end-of-game cinematic UI |
| 74 | `systems/x3-rescue.js` | rescue minigame UI |
| 72 | `features/x3-death-cam.js` | death-cam overlay + respawn prompt |
| 68 | `systems/x3-game-loop.js` | main loop — has DOM hooks scattered in |
| 56 | `systems/x3-tutorial.js` | tutorial overlay |
| 52 | `features/x3-cutscenes.js` | cutscene UI (skip-prompt, subtitles) |
| 52 | `features/x3-console.js` | dev console |
| 51 | `gameplay/x3-bosses.js` | boss-bar UI |
| 50 | `gameplay/x3-powerups.js` | powerup HUD |
| 47 | `systems/x3-infection.js` | infection overlay |
| 43 | `systems/x3-achievements.js` | achievement toast |
| 41 | `effects/x3-respawn-effects.js` | floating text + screen-edge flash (we just wired this) |
| 40 | `systems/x3-ai-dialog.js` | AI dialog UI |
| 38 | `world/x3-world-ocean.js` | oxygen overlay, swim UI |
| ... | 47 more files | smaller UI pieces, helpers |

**Pattern:** the heavy hitters are *features*, *systems*, and *UI* — not core gameplay or rendering. Most of `entities/`, `world/`, and a lot of `gameplay/` is DOM-free.

### WebAudio usage (concentrated)

| Hits | File | Role |
|---:|---|---|
| 315 | `audio/x3-audio-ambient.js` | ambient zone audio |
| 172 | `audio/x3-sound-engine.js` | spatial 3D sound (HRTF PannerNode) |
| 61 | `audio/x3-music-engine.js` | procedural music |
| 55 | `features/x3-elevator.js` | elevator click/whir SFX (uses raw WebAudio, not SFX module — unusual) |
| 19 | `audio/x3-sfx.js` | SFX bank |
| 16 | `audio/x3-reverb-zones.js` | reverb convolution |
| 16 | `audio/x3-audio-core.js` | core context, helpers |
| 10 | `audio/x3-wav-audio.js` | WAV decoder |
| 8 | `audio/x3-music-tracks.js` | track defs |
| ...7 more | scattered | small touches in weather/vehicles/powerups |

**15 files total. ~700 WebAudio API references.** This is the biggest single subsystem rewrite. Recommended approach: use BabylonNative's audio API (or wrap a tiny WebAudio polyfill on top of miniaudio/OpenAL) and rewrite the 8 files in `audio/`. The ~7 scattered usages outside `audio/` should be refactored to call `G.SFX.*` first (already mostly done) — saves work.

### Storage / Network / Workers

- **localStorage**: 12 files — `save-system`, settings, achievements, tutorial, AI dialog, console, intro, pause-menu, cutscenes, input-bindings, loading-screen, constants. **Trivial swap to filesystem** via a `Storage` shim (~50 lines).
- **fetch / XHR**: 14 files — all are **JSON config loaders** (skills, quests, story, crafting, dialog, level definitions, etc.) and asset loaders (`scene-init` loads the wasm physics). BabylonNative provides a native loader and `XMLHttpRequest`-shim for asset URLs.
- **`new Worker`**: 2 — `bot-ai-worker.js` and `lod-culling-worker.js`. BabylonNative supports Web Workers on Windows/macOS; verify on target platform.
- **`window.location.reload`**: 4 (3 restart-game buttons + 1 endings flow). Replace with a native restart flow.
- **`navigator.*`**: 0. ✅
- **`window.*` general**: 25 files use it for global state (e.g., `window._ps5b`, `window._a31Fleet`). These are just JS globals on `window`; BabylonNative still has a `globalThis` / `global` — most will Just Work after a search-and-replace.

---

## Section 2 — Tier Classification

### Tier A — Zero-change ports (56 files, 42,342 LOC, 28%)

Pure ES6 logic that touches only Babylon.js + math. Examples:
- `core/x3-state.js`
- All `entities/x3-npc-*.js` (NPC body/face/hair/outfit/anim systems)
- `gameplay/x3-bot-data.js`, `bot-ai.js`, `bot-lifecycle.js`, `bot-weapons.js`
- `gameplay/x3-aiming.js`, `x3-weapons-fire.js`, `x3-fp-weapons.js`
- All `world/` builders that just create meshes
- `effects/x3-respawn-effects.js` — wait, this DOES create floating-text DOM elements; we just wired it. So it's actually Tier C. Reclassifying.

These compile-and-run as-is on day 1 of the port.

### Tier B — Window-only / light polyfill (25 files, 23,713 LOC, 16%)

References to `window.*`, `performance.now()`, `setTimeout`, etc. — all available natively in JavaScriptCore/V8 once we ship a small global-shim. Mostly mechanical work.

### Tier C — DOM-bound UI (65 files, 68,257 LOC, 46%)

The biggest bucket of work. Two sub-strategies:

**C1 — "Move to Babylon GUI":** The pause menu, dialog, dev console, HUD, tutorial, achievements toast, photo-mode, cutscene captions, etc. all use raw `document.createElement`. Babylon GUI (`BABYLON.GUI.*`) is fully supported in BabylonNative and gives you AdvancedDynamicTexture-based UI that renders into the Babylon scene. Re-author each panel as a GUI tree. Big chunk of work but rote.

**C2 — "Engine-side input":** `input-bindings.js`, `input.js`, `game-loop.js` use `addEventListener('keydown')`. BabylonNative provides `NativeInput` — same `KeyboardEventTypes` surface as Babylon's `scene.onKeyboardObservable`. Refactor to use the Babylon observable instead of `window.addEventListener`, which makes the code run on both web and native.

### Tier D — Subsystem swap-outs

| Subsystem | Files | LOC range | Strategy |
|---|---:|---|---|
| Audio | 15 | ~9,000 | Rewrite `audio/x3-audio-core.js` against BabylonNative's audio (or a thin OpenAL/miniaudio wrapper). Keep the `SFX`/`MusicEngine`/`SoundEngine` public APIs identical so the 7 scattered consumers don't change. |
| Save / settings | ~14 | ~3,500 | Provide a `localStorage` shim backed by a JSON file in `%APPDATA%/X3/saves/`. ~50-line drop-in. |
| Worker bots | 1 (+ manager) | ~2,000 | Verify BabylonNative Workers on Windows; if missing, fall back to main-thread tick (perf cost). |
| Worker LOD | 1 (+ manager) | ~1,500 | Same. |
| Asset loaders | 8 | ~7,000 | All use `fetch('./data/foo.json')`. BabylonNative's `XMLHttpRequest` shim handles relative URLs once we set the asset root. |

---

## Section 3 — Babylon Feature Parity (used → BabylonNative supported?)

| Feature | X3 callsites | bgfx / BabylonNative support |
|---|---:|---|
| `StandardMaterial` / `PBRMetallicRoughnessMaterial` | hundreds | ✅ full |
| `MeshBuilder.*` (CreateBox/Sphere/etc.) | hundreds | ✅ full |
| `PhysicsAggregate` (Havok) | 133 | ⚠️ Havok integration is in BabylonNative as of 2025 — **verify version compatibility before committing** |
| `PhysicsCharacterController` | 1 | ⚠️ part of Havok plugin — same caveat |
| `GPUParticleSystem` | 6 | ⚠️ supported on most bgfx backends but worth a hello-world |
| `NodeMaterial` | 11 | ✅ runtime-compiled GLSL/HLSL; works in BabylonNative |
| `GlowLayer` | 1 | ✅ |
| `CascadedShadowGenerator` | 1 | ✅ |
| `ReflectionProbe` | 3 | ✅ |
| `DefaultRenderingPipeline` | 1 | ✅ — most post-processes carry over |
| Recast NavMesh (`babylonjs.recast.js`) | yes | ⚠️ ship a native recast build alongside the binary |
| Shadow maps, env maps, IBL | many | ✅ |
| WebGPU compute / RTX | 0 | n/a — X3 doesn't use these |

**107 unique `BABYLON.*` APIs in use total.** Most are stable surface area covered by BabylonNative.

---

## Section 4 — Recommended Milestone Plan

### M0 — Toolchain spike (2-3 days)
- Build BabylonNative on Windows + i9-14900K. Get the `Playground` sample running.
- Verify Havok plugin loads in BabylonNative. Spawn a single PhysicsAggregate cube falling onto a plane.
- Verify Recast loads. Spawn a NavMesh on a box.
- **Gate:** if any of these three (engine, Havok, Recast) blocks here, stop and reassess. Otherwise green-light the port.

### M1 — Bootstrap (1 week)
- New repo `C:\GameDev\X3Native\` (sibling of `Q3Engine/`). Include the BabylonNative submodule + a CMake host shell (Windows DXR / Vulkan).
- Bundle X3's `src/` as resources.
- Wire the global shims: `localStorage` → file, `fetch` → asset loader, basic `window` polyfill.
- **Gate:** `x3-main.js` loads, runs `wireConstants()` + `wireFunctions()` without crashing. Empty Babylon scene visible.

### M2 — Render the arena (1 week)
- Pull in all Tier-A modules (56 files). They should compile and run unchanged.
- Stub out everything in Tier C with no-ops at first (UI absent but not crashing).
- Replace `audio/x3-audio-core.js` with a stub (silence) so dependent modules don't crash on `G.SFX.foo()`.
- **Gate:** floating arena visible, sky/space, no input, no UI, no audio. The world geometry should "just work" because it's all `MeshBuilder` + materials.

### M3 — Input + player + bots (1 week)
- Refactor `x3-input-bindings.js` and `x3-input.js` to use `scene.onKeyboardObservable` instead of `document.addEventListener`. Same for mouse.
- Bring `x3-player.js`, `x3-fp-weapons.js`, `x3-weapons-fire.js`, `x3-bot-lifecycle.js` online (all Tier A — should drop in).
- **Gate:** WASD walks. Bots spawn (with the respawn FX we just wired — minus the floating text). You can shoot a bot.

### M4 — Audio (1-1.5 weeks)
- Rewrite `audio/x3-audio-core.js` against BabylonNative audio (or miniaudio). Preserve the `SFX`/`MusicEngine`/`SoundEngine` API.
- 3D positional audio via Babylon's `AnalyserNode` substitute or a custom HRTF if needed.
- **Gate:** weapon SFX play, music plays, footsteps spatialize.

### M5 — UI in Babylon GUI (2 weeks — biggest chunk)
- Rebuild HUD (kill feed, damage flash, weapon HUD, ammo, scoreboard) in Babylon GUI. The `features/x3-hud.js` module is the smallest UI rewrite.
- Then pause menu, dialog, dev console, achievements toast, settings, photo mode, death cam, tutorial, cutscene captions, end-of-game UI.
- This is the longest tail — about 65 files but most are tiny.
- **Gate:** every UI panel that exists on web exists on native, even if visually rougher.

### M6 — Save + settings + ship (1 week)
- File-based save slots under `%APPDATA%/X3/saves/`.
- Settings file. Steamworks SDK integration. Achievements.
- Build a release executable with full asset bundling. Test on a clean Windows machine.

**Total: ~6-7 weeks of focused work.** With agents picking off the rote Babylon-GUI conversions in parallel, ~3-4 weeks of calendar time is realistic.

---

## Section 5 — Things to verify before M0

1. **Havok in BabylonNative** — check `babylon-react-native` / `BabylonNative` repo for which Havok wasm/native version they ship and confirm it covers `PhysicsCharacterController`. If it doesn't, a fallback to `BABYLON.AmmoJSPlugin` or in-house CC is a multi-day detour.
2. **GPU particles on Windows D3D12 backend** of bgfx — `GPUParticleSystem` should work but the 6 callsites in X3 should be tested early.
3. **`scene.onKeyboardObservable` parity** — confirm the modifier-key chord behavior used by `input-bindings.js` is faithfully reproduced.
4. **Asset packaging** — X3 currently lazy-loads weapon GLBs, NPC GLBs, terrain textures. Decide upfront whether to bundle everything in the executable, ship a `data/` folder alongside, or fetch from disk via a virtual filesystem. (Recommendation: ship a `data/` folder; simplest and patch-friendly.)

---

## Section 6 — What this audit DID NOT do

- **No actual BabylonNative build attempted.** The toolchain spike is M0 above.
- **No measurement of fixable-via-shim vs hard-rewrite for Tier C files.** The 68K LOC C-tier number is an upper bound; many of those files have only a handful of DOM lines and could fall to Tier B with a small `document` shim. Worth a follow-up sub-audit if scope is tight.
- **No content-pipeline analysis.** X3's GLB / texture / JSON assets at `~/G/Textures/`, `~/G/GameDev/`, the bible PDFs, etc. all need to either ship with the binary or load from a known root. Out of scope for this audit but trivial.
- **No Steam / Steamworks scoping.** Out of scope.

---

## Recommendation

**Run M0 (the 2-3 day toolchain spike) before any further commitment.** If it lights up, X3 has a clear path to native. If Havok or Recast turn out to be missing/broken in BabylonNative, the spike will show that for ~$0 sunk cost and you can pivot back to either:
- (a) RBDOOM as the native target with X3 as the design sandbox (your current memory-stated direction), or
- (b) Electron-wrap X3 for a quick non-native ship.

The 28% Tier-A LOC + the well-isolated audio/storage subsystems are a strong "go" signal. The 46% DOM-bound LOC is the real cost — manageable, but it's where the calendar time goes.
