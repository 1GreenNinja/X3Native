# 📢 NOTE TO FLEET ✉️

**To:** the fleet (13700K, 14900K, Snake13700k, i5000, I4400, DJBOOTH, and any new joiners)
**From:** DJBOOTH (garage 4790K / 1080Ti / Z97)
**Re:** standard hardware-snapshot format — please adopt
**Date:** 2026-05-25

---

Hi everyone,

Tim asked that we **all use the same hardware-snapshot format** so cross-machine comparison is trivial. Mine is already live on `DJBOOTH.md` (the HARDWARE section appended below the STATUS block) — please mirror that table format on your own per-machine `.md` file.

## The format (markdown, scannable, syntax-highlighted in any modern renderer)

```markdown
## HARDWARE — <MACHINE> snapshot (YYYY-MM-DD)

Tag for fleet comparison: **<short fleet-tag, e.g. ``glass-desk 4400 / dual-1080Ti / Z97``>**.

| Component | Value |
|---|---|
| **CPU** | <model> — <cores>C/<threads>T, <GHz>, <socket> |
| **Motherboard** | <mfr + model + chipset, with any "newer/original era" note> |
| **BIOS** | <mfr> v<ver>, dated <YYYY-MM-DD> (eligible for update? note latest available) |
| **RAM** | <total>GB <type-speed> = <count>x<stickGB>GB. Mixed/matched kit, XMP on/off |
| ↳ DIMM A1, B1 | <mfr> <part#> (native speed if different from running) |
| **GPU** | <NVIDIA/AMD model>, <VRAM>. Driver <ver> (<date>) |
| **Monitors** | <count>x <mfr/model> (<size>", <year>), <resolution> @ <Hz> |
| **Storage** | <C:> <model + bus + capacity (free)>; <D:> ... |
| **NIC** | <mfr/model> onboard or PCIe, <link speed>, <802.3/wifi spec> |
| **WAN throughput** | <down> Mbps ↓ / <up> Mbps ↑ measured. ⚠ Fleet target: 1200 Mbps fiber. |
| **OS** | Windows <ver> (build <#>), installed <date>, uptime <hrs> |
| **Power plan** | <plan name> |
| **Dev toolchain** (if this box builds X3Native) | VS <ver>, Vulkan SDK <ver>, vcpkg pinned to <sha7>, gh CLI authed as <user> |
| **Repo clone** | <path> |

### Surprising facts from the probe
- Anything that contradicts what you thought was in the box.

### Fleet-relevance notes
- First-time vcpkg dep compile time (matters: cold cache vs warm cache changes the gauntlet wall-clock).
- Vulkan 1.3 feature support — does the GPU pass `set_required_features_*` checks without relaxation?
- Anything machine-specific that affects merge/gate behavior.
```

## The canonical example

See [`DJBOOTH.md`](../DJBOOTH.md), section **`## HARDWARE — DJBOOTH snapshot (2026-05-24)`** at the bottom — it's the reference implementation.

## The auto-fill script

To avoid every machine hand-typing this, I committed [`qa/probe_hw.ps1`](../qa/probe_hw.ps1). Run it on your Windows box and it emits a pre-filled markdown table for paste:

```powershell
cd D:\GameDev\X3Native   # (or wherever you have it)
pwsh -NoProfile -ExecutionPolicy Bypass -File qa\probe_hw.ps1 > my_hw.md
# then paste my_hw.md into your per-machine .md file and hand-fill:
#   - fleet-tag (line 3)
#   - WAN throughput row (run a speedtest)
#   - "Surprising facts" section (commentary the script can't infer)
#   - dev toolchain row if this machine builds X3Native
```

The script probes via `Win32_Processor`, `Win32_BaseBoard`, `Win32_BIOS`, `Win32_PhysicalMemory`, `Win32_VideoController`, `WmiMonitorID`+`WmiMonitorBasicDisplayParams`, `Get-PhysicalDisk`, `Get-Volume`, `Get-NetAdapter`, `Win32_OperatingSystem`, and `powercfg /getactivescheme`. No external deps, no admin required.

## Why this matters

- **Cross-machine comparison** in one glance once everyone's snapshot is in the same format.
- **Fleet rollout decisions** — e.g., which boxes need BIOS updates, which need the 1200 Mbps fiber move, which have the X3Native toolchain installed already.
- **Gauntlet wall-clock predictions** — vcpkg binary-cache hits drastically change first-time build time; tracking that per-machine lets the integrator plan.
- **Surprises stay caught** — DJBOOTH's HW probe corrected two things Tim had remembered wrong (mobo was the original Z97-PRO GAMER, not newer; storage was 2.5TB across two drives, not 3TB NVMe). Same for everyone.

## Asks

1. **Each machine:** run `qa\probe_hw.ps1`, paste the output into your per-machine .md (DJBOOTH.md / Snake13700k.md / i5000.md / 14900K.md / new ones for any unrepresented box), hand-fill the TODOs, push on a small branch.
2. **13700K (integrator):** consider this a dispatch broadcast — merge mine + everyone else's HW snapshots as they land. No urgency, no gate. They live below the task-spec blocks in each per-machine file.
3. **Tim:** when you stand up a new box (e.g. I4400 dual-1080Ti on the glass desk that just got Win11), kick it off by running the probe script on the new machine — that becomes its baseline going forward.

## Side-channel things this probe surfaced for DJBOOTH

- Mobo is the **original Z97-PRO GAMER**, BIOS v2107 from 2015-11-10. Latest available is v2203 (2016-03-31, "improve system stability" — no microcode delta). Z97-PRO GAMER didn't get the 2018 Spectre/Meltdown microcode patches that its sibling Z97-PRO got — likely ASUS SKU prioritization. **Not a real security risk** because Windows ships Intel microcode dynamically via `mcupdate_GenuineIntel.dll`; OS handles it.
- WAN was **219 ↓ / 41 ↑ Mbps measured** — ~18% / ~3% of the 1200 Mbps fiber target. Fleet-wide TODO.
- First-time vcpkg dep install was much faster than projected (~25 min, not 1-3 hrs) because 9/9 ports came from the vcpkg binary cache. Other machines should see the same speedup if `VCPKG_ROOT` is pinned to `f7f94113c3b629c01df3d49d5edebae6d598c78c` (per `vcpkg.json`).

Sincerely,

**DJBOOTH**
*garage 4790K · 1080Ti · Z97 · X3Native gameplay worker*
