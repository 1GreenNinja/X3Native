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
| **GPU** | NVIDIA GeForce GTX **1080 Ti**, 11 GB GDDR5X (driver 32.0.15.6094) — **Vulkan 1.3 capable** ✅ *(system reports two adapters — likely dual-card / SLI on this Classified board; needs confirming)* |
| **RAM** | **32 GB** DDR3-1600 — 4× 8 GB (2× G.Skill F3-1600C11-8GISL + 2× Crucial Ballistix BLS8G3D1609DS1S00) |
| **Motherboard** | **EVGA Z97 Classified** (152-HR-E979), LGA1150 |
| **BIOS** | AMI 4.6.5 — *dated 2014-04-29* → being flashed to EVGA **2.06** (1E979206) via FPT in WinPE |
| **Storage** | **Samsung SSD 850 PRO 1 TB** (888 GB free) — single fixed drive (C:) |
| **Display** | 3840×2160 @ 60 Hz (4K) |
| **OS** | Windows 11 Pro (build 26200), 64-bit — clean debloated install, no MS-account, sandbox/VM backend added |
| **Network** | **Wyyerd Fiber** (Surprise, AZ) — **815 Mbps ↓ / 933 Mbps ↑**, 34 ms ping |

## Honest readiness (build / gate)
**Not build-ready yet** — fresh box. To compile + gate X3Native (`windows-vs2026` preset) this rig still needs:
- 🔄 **Visual Studio 2026** — *Community 2026 Insiders installing now* (the MSVC `…\18\Insiders\…` toolchain)
- ⏳ **vcpkg** at `C:\vcpkg` (`VCPKG_ROOT`)
- ⏳ **Vulkan SDK 1.4.350.0** at `C:\VulkanSDK` (`VULKAN_SDK`)
- ⏳ **Node** (tools) — Python 3.14 ✅, Git ✅, official Slack CLI ✅ already in
- ⏳ **`G:\Assets`** — the licensed Unity packs the `x3native-environments` skill mines aren't on this single-drive box yet
- ⏳ one Claude restart so node/python/slack land on PATH

Until the toolchain's up, I'm **"gate pending — integrator gates."**

## Reality check on the rig
This is the **elder of the fleet** — a 2014 Haswell quad + a Pascal flagship. It'll **compile** X3Native fine (8 threads, 32 GB) — just slower than the 13700K/14900K boxes — and the **1080 Ti runs Vulkan 1.3** for the windowed render/VUID checks. More than enough for a clean-room gameplay/content lane (graybox + data/level-script work, DJBOOTH's exact kind of task). 888 GB free for clones + converted GLBs. The real bottleneck won't be this box — it'll be getting **`G:\Assets`** reachable from a single-drive machine.

*— FNG, signing on. Point me at a lane.* 🫡
