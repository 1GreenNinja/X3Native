# X3 Native — Open Questions for the 14900K Session

> **This is the "questions.md" for the 14900K kickoff.** Read after `X3_NATIVE_ENGINE_PLAN.md` + `X3_NATIVE_SLICES.md`. These are the unknowns that will block or slow M0 if not pre-answered. Tim: answer the ⛔ ones before the 14900K starts; the ⚠️/ℹ️ ones can be answered as you hit them.
>
> Authored 2026-05-20 on I9DevPC (laptop) after surveying the repo. Engine work has **NOT started in code** (no native/M0 branch on origin as of `f20a754`).

---

## 🔴 #0 — PLAN RECONCILIATION (resolve first — the two plan docs disagree)

`X3_NATIVE_ENGINE_PLAN.md` (updated) says **HYBRID**: fork RBDOOM for speed, then **clean-room rewrite the GPL parts (D1-D8) before commercial ship**, using the two-machine information barrier + `native-scaffold/` machinery.

`X3_NATIVE_SLICES.md` (written earlier same day) assumes **GPL-open forever**: "engine code is open; game data is the closed layer." It does **not** include the D1-D8 de-GPL slices.

**Question:** Which is authoritative?
- **(A) HYBRID** (plan doc): the 100 slices are the "get it working" track (M0-M10); the D1-D8 clean-room track runs in parallel from ~M4 and the ship gate is `GPL_DEBT.md` empty. *More work, clean commercial IP.*
- **(B) GPL-open** (slices doc): skip D1-D8 entirely, engine stays GPL/public forever, only game `.x3pak` is sold. *Less work, engine is permanently open-source.*

> My read: you chose HYBRID on 2026-05-19. If that still stands, the slices doc needs a header note pointing to the D1-D8 track, and the 13700K clean-room environment gets set up in parallel. **Confirm A or B.**

---

## ⛔ Blocking — answer before M0 (Slices 1-8)

### Q1 — Where is the RDOOM / "Ardoom 2019" source on the 14900K?
Slice 1 can't start without the path. Also:
- Is it **vanilla RBDOOM-3-BFG**, or a **modified fork** ("Ardoom")? What's the delta from upstream?
- Is it a git checkout (has history/remote) or a loose folder?
- Which **Vulkan backend version** does it currently target?

### Q2 — Do you own Doom 3 BFG, and where are its base assets?  ✅ OWNED
**Confirmed 2026-05-20: Tim owns "ALL THE DOOMS."** So Doom 3 BFG base assets are available.
- Still need: the **path** to the Doom 3 BFG `base/` `.pk4` files (Steam default: `C:\Program Files (x86)\Steam\steamapps\common\DOOM 3 BFG Edition\base\`). Slice 4 copies/points RBDOOM at these to boot.
- (We only need these to verify the engine runs; X3 ships its own `.x3pak`, not Doom assets.)

### Q3 — New repo or branch of the existing X3Engine repo?  ✅ NEW X3Native
**Confirmed 2026-05-20: new repo `1GreenNinja/X3Native`.** Keeps RBDOOM (GPL) out of the Babylon `X3Engine` repo and gives the clean-room barrier a dedicated topology. Repo seeded from the laptop with the planning docs + `native-scaffold/` (GPL_DEBT.md + specs/ promoted to root) + `tools/cleanroom-setup.ps1`. The 14900K clones it and drops RBDOOM in under `engine/_gpl_rbdoom/`.

### Q4 — Toolchain present on the 14900K?
Slices 2-9 assume these are installed. Confirm or I'll add "install X" slices:
- [x] **Visual Studio 2026** (confirmed 2026-05-20) — CMake generator is "Visual Studio 18 2026"; note the newer toolset (MSVC v145) vs the VS2022 originally assumed. No issue, just newer.
- [ ] CMake ≥ 3.25 (VS2026 bundles a recent CMake; confirm on PATH or use the bundled one)
- [ ] vcpkg (confirm; clone if absent)
- [⚠️] **Vulkan SDK** — **runtime 1.4 present (driver-provided), but the dev SDK is NOT installed.** Install on the 14900K: `winget install KhronosGroup.VulkanSDK` (or https://vulkan.lunarg.com). Targeting Vulkan 1.3 features (subset of the 1.4 runtime). Confirm after install: `echo $env:VULKAN_SDK` is non-empty.
- [ ] Git LFS (for any large binaries that DO get tracked)

---

## ⚠️ Decisions needed soon (M1-M4)

### Q5 — Lua flavor: LuaJIT or Lua 5.4?
Slice 43 says "LuaJIT/Lua 5.4" — pick one.
- **LuaJIT**: ~3-10x faster, but frozen at Lua 5.1 syntax + maintenance-mode. Best for hot-path game logic.
- **Lua 5.4**: modern (integers, `<close>` vars, faster GC), official, but interpreter-only (slower).
> Recommendation: **LuaJIT** for a game engine (perf wins, sol3 supports it). Confirm.

### Q6 — Engine/product name — is "X3" final?
`X3Engine.exe`, `.x3pak`, repo name all bake in "X3." Is that the shipping brand or a working title? Cheaper to decide now than rename 100 files later. (Your GitHub handle is `1GreenNinja` — is there a studio name?)

### Q7 — Navmesh: reuse RBDOOM's, or Recast/Detour fresh?
Slice 57 needs navmesh. RBDOOM has its own AAS nav. Recast/Detour (zlib license, clean) is the modern standard and what Babylon X3 used.
> Recommendation: **Recast/Detour** (clean IP, well-documented, you already know it from Babylon). Confirm.

### Q8 — Is the 13700K clean-room environment ready?  ✅ READY
**Confirmed 2026-05-20: the 13700K is clean-room ready.** Remaining: a one-command bootstrap that clones the `*-cleanroom` checkout (omitting `engine/_gpl_rbdoom/`) + copies `native-scaffold/specs/` + `GPL_DEBT.md`. (Offered: laptop can draft this setup script.)

---

## ℹ️ Confirmations (assumptions to validate, non-blocking)

### Q9 — GPL-during-prototype is acceptable?
The engine is GPL while it contains RBDOOM code. It lives on a **public** branch and is **not sold** until the clean-room rewrite finishes (ledger empty). Commercial ship waits for that gate. OK?

### Q10 — Babylon X3 stays as the reference (don't delete)?
The native engine cribs gameplay feel, tuning values, and level layouts from the current Babylon X3 (`C:\GameDev\Q3Engine` / `1GreenNinja/X3Engine`). It's also the shippable fallback. Confirm we keep it intact.

### Q11 — Target platform scope for v1?
Plan targets Windows + RTX. Slice 81 mentions DLSS. Confirm v1 is **Windows-only** (Vulkan), with Linux/Steam Deck as a "free-ish later" (Vulkan ports cleanly) rather than a v1 requirement.

---

## Questions the 14900K should report BACK to Tim after M0

(Fill these in during Slices 1-8 and surface them.)
- Source survey result: vanilla RBDOOM or modified? Vulkan version? (from Slice 1)
- Did it build clean? What dependencies were missing? (Slice 3)
- Vulkan validation baseline: clean, or known errors? (Slice 5)
- **G2 go/no-go:** does RBDOOM build + run + pass the 3 risk checks on the 5090? → green-light M1, or fall back to BabylonNative.

---

## Quick-answer block (Tim: fill this in, commit, and the 14900K is unblocked)

```
#0  Plan path:        [ A=HYBRID / B=GPL-open ]            ← still need
Q1  RDOOM source at:  ____________________   (vanilla? / modified? / vulkan ver: ___)  ← still need PATH
Q2  Doom3 BFG assets: ✅ OWNED ("all the Dooms")  base/ path: ____________________  ← still need PATH
Q3  Repo:             ✅ NEW X3Native (1GreenNinja/X3Native; seeded from laptop)
Q4  Toolchain:        VS2026 ✅ | CMake[ ] | vcpkg[ ] | VulkanSDK ⚠️ INSTALL (winget install KhronosGroup.VulkanSDK) | GitLFS[ ]
Q5  Lua:              [ LuaJIT / Lua 5.4 ]                 ← still need
Q6  Name "X3" final:  [ yes / no → ______ ]               ← still need
Q7  Navmesh:          [ Recast/Detour / RBDOOM AAS ]       ← still need
Q8  13700K ready:     ✅ READY    (setup script: offered)
Q9  GPL-in-prototype OK: [ y / n ]                         ← still need
Q10 Keep Babylon X3:  [ y / n ]                            ← still need
Q11 v1 platform:      [ Windows-only / +Linux ]            ← still need

ANSWERED 2026-05-20: Q2 (own Dooms), Q3 (new X3Native repo), Q4 (VS2026; Vulkan SDK needs install), Q8 (13700K ready)
STILL OPEN: #0, Q1 path, Q2 path, Q5, Q6, Q7, Q9, Q10, Q11
```
