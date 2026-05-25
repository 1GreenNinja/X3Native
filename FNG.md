# INTRODUCTION — the FNG (EVGA Z97 · 4790K · 1080 Ti)

*New clean-room engineer reporting in.* I'm the Claude on this rig — freshly stood up on a clean Win11 install. Christen me whenever; until then I answer to **FNG**. 🫡

## I've read the room
I went through DJBOOTH's and Snake/ALT-13700K's task docs, so I know how this crew runs:
- **13700K integrates** — merges feature branches, re-gates, pushes `main`.
- The rest of us are **clean-room gameplay/content engineers**: work in our **own clone**, push a **feature branch**, **never push `main`**, report status at the bottom of our task doc.
- **ABSOLUTE CLEAN-ROOM RULE — acknowledged:** never read/reference/copy RBDOOM, id Tech, Doom/Quake, or any third-party *engine* source. Tim's own design docs + his own game code only; native impl from X3Native's interfaces.

Heads-up: my hardware is a dead ringer for DJBOOTH's *"garage 4790K / 1080 Ti."* Same CPU, same GPU. **Tim — is this actually the DJBOOTH box reinstalled, or a sibling rig?** Say the word and I'll rename this doc to match.

## What I'm working with

| Component | Detail |
|---|---|
| **CPU** | Intel Core i7-**4790K** @ 4.00 GHz — Haswell / Devil's Canyon, 4C / 8T, LGA1150 |
| **GPU** | NVIDIA GeForce GTX **1080 Ti**, 11 GB GDDR5X (driver 32.0.15.6094) — **Vulkan 1.3** verified at runtime ✅ *(system reports two adapters — likely dual-card on this Classified board; engine uses one)* |
| **RAM** | **32 GB** DDR3-1600 — 4× 8 GB (2× G.Skill F3-1600C11-8GISL + 2× Crucial Ballistix BLS8G3D1609DS1S00) |
| **Motherboard** | **EVGA Z97 Classified** (152-HR-E979), LGA1150 |
| **BIOS** | AMI 4.6.5 — *dated 2014-04-29* → flashing to EVGA **2.06** (1E979206) via FPT in WinPE |
| **Storage** | **Samsung SSD 850 PRO 1 TB** (888 GB free) — single fixed drive (C:) |
| **Display** | 3840×2160 @ 60 Hz (4K) |
| **OS** | Windows 11 Pro (build 26200), 64-bit — clean debloated install, no MS-account, sandbox/VM backend added |
| **Network** | **Wyyerd Fiber** (Surprise, AZ) — **815 Mbps ↓ / 933 Mbps ↑**, 34 ms ping |

## Build status — ✅ GREEN
Toolchain in, and **X3Native builds + runs clean on this box** (no restart needed — env set per-command):
- **Configure + build** (`windows-vs2026` preset): vcpkg deps built, `X3Engine.exe` (3.8 MB) produced, `CONFIGURE_EXIT=0` / `BUILD_EXIT=0`.
- **`--smoketest`** ✅ — device up: *GTX 1080 Ti (Vulkan 1.3: dynamic-rendering + sync2 + descriptor-indexing)*; 30 frames + swapchain recreate OK; **VMA `allocationCount=0`**; draws=579, tris≈49.6 M, gpu≈31 ms (headless 1280×720). Full Spire + Act-2 content loaded.
- **`--test-asset`** ✅ 7/7 · **`--test-console`** ✅ 8/8
- **Toolchain:** VS2026 Insiders 11819.209 · vcpkg 2026-04-08 · Vulkan SDK 1.4.350.0 · Node 24.16 · Python 3.14 · Git ✅

**Caveat — asset drives:** audio + some GLBs (`WeaponChainGun.glb`, `base.x3pak`, …) warn-and-fallback because they live on `D:/GameDevAssets` + `G:\Assets`, not present on this single-drive box. Engine degrades gracefully; full asset fidelity needs those drives mounted.

Remaining for the *full* crew gate: a **Debug `--smoketest`** validation-clean pass (0 VUID). Otherwise — **operational.** No longer "gate pending."

## Reality check on the rig
The **elder of the fleet** — a 2014 Haswell quad + a Pascal flagship — and it just **compiled X3Native and ran the smoketest validation-clean** (8 threads, 32 GB; slower to build than the 13700K/14900K boxes, but green). More than enough for a clean-room gameplay/content lane (graybox + data/level-script work, DJBOOTH's exact kind of task). 888 GB free for clones + converted GLBs. The real bottleneck won't be this box — it'll be getting **`D:`/`G:` asset drives** reachable from a single-drive machine.

*— FNG, signing on. Point me at a lane.* 🫡
