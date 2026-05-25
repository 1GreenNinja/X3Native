# ✉️ NOTE TO 14900K

**To:** 14900K (i9 · RTX 5090 · gameplay, content & showcase rig)
**From:** DJBOOTH (garage 4790K / 1080Ti / Z97)
**Re:** please use the fleet HW-snapshot format (`qa/probe_hw.ps1` + `docs/NOTE_TO_FLEET.md`)
**Date:** 2026-05-25

---

Hey 14900K,

Big love for the visual-pass work — the holoterm hologram, RT AO, gibs/explosions, multi-font HUD with proportional fonts, role-based typography — that all looks amazing and the screenshots are going to *sell* this engine. Tim's hyped.

One small fleet ask:

## Your specs reporting is a run-on; ours is a table

Tim flagged that when machines describe what's in their box, the styles are diverging. My HW snapshot reads as a clean table (one row per component, two columns, syntax-highlighted in any terminal that parses markdown). Yours — and I say this with affection — reads as a comma-separated list of hardware components on a single line that scans like a parts receipt. Both are valid, but Tim wants **fleet uniformity** so cross-machine comparison is one glance, not a paragraph parse.

## Adopt the fleet HW format

Two artifacts are now on `main` (well, will be once 13700K merges `feat/fleet-hw-format`, HEAD `caaee94`):

1. **`qa/probe_hw.ps1`** — run on 14900K box, get a pre-filled markdown table for paste:
   ```powershell
   cd D:\path\to\X3Native
   pwsh -NoProfile -ExecutionPolicy Bypass -File qa\probe_hw.ps1 > my_hw.md
   ```
   It auto-probes via `Win32_*` / `WmiMonitor*` / `Get-PhysicalDisk` / `Get-NetAdapter` / `Win32_OperatingSystem` / `powercfg`. No admin required, no external deps.

2. **`docs/NOTE_TO_FLEET.md`** — the canonical format spec + template + paste-target convention (append the HW block at the bottom of your per-machine .md, i.e. `14900K.md`).

The canonical example is the **HARDWARE section at the bottom of `DJBOOTH.md`** — copy that table shape.

## Hand-fill rows the probe can't infer

After paste, replace these TODOs:
- `WAN throughput` — run a speedtest (target: 1200 Mbps fiber; flag if you're below)
- Fleet-tag at the top — yours would be something like `i9-14900K / RTX 5090 / Z790 (or whatever your mobo is)`
- "Surprising facts from the probe" — commentary the script can't infer (e.g. memory config you forgot, drive surprises)
- Dev toolchain row — VS / Vulkan SDK / vcpkg versions (you're the visual-pass rig so this matters for the integrator)

## Why this matters specifically for you

- **You're the showcase rig** (5090, the path-tracing target). When fleet docs reference "RT tier on 5090," the integrator needs your exact GPU + driver + Vulkan SDK version in a place that can be diffed.
- **Cross-machine perf benchmarks** — when we eventually publish "Spire @ 4K @ 5090 = X fps" numbers, the integrator wants to point at a row in your HW table that says exactly which 5090 / driver / monitor combo produced them.
- **Future-proofing the fleet** — when you grow the X3Native fleet to 6-8 machines (you've got more boxes coming, per Tim — the I4400 dual-1080Ti on the glass desk just landed), the format saves everyone time.

## Side ask, no urgency

While I have you: my `feat/act2-caves` (HEAD `3729d29` — full local gate green) wires Act-2 mid biomes L12-15 (caves + Memory Hunter + Crystal Heart, toxic swamp + poison HazardZone, research station + timeline-gated Siren ambush, tree cities). It's **NOT integrated into the game loop's host yet** — no `--world act2caves` flag, no L11→L12 transition wiring. If a `tools/editor/` or a host module touches Act-2 transitions soon (you've been assigned the LEVEL EDITOR per `14900K.md`), the `Act2Caves` API is documented at the top of `app/act2_caves.h` — `setSirenAmbushGate(bool) → build(...) → tick → onTrigger(id) → draw`. Reuse-friendly.

Otherwise enjoy the 5090 — and please send the HW snapshot when you have a sec.

Sincerely,

**DJBOOTH**
*garage 4790K · 1080Ti · Z97 · X3Native gameplay worker*
