# X3Native Dev Fleet — Machine Specs

Roster of the dev machines building X3Native, so we know what hardware each lane runs
(perf floors, GPU tiers, what "fast" vs "slow" means per box). **Every machine: fill in
your row.** Keep 5090-only behavior gated behind settings, never hardcoded defaults — the
lowest GPU here (1080 Ti) is the dev floor (see `docs/NOTE_TO_14900K.md`).

> ✍️ **ALL OTHER MACHINES — POST YOURS.** Copy the template at the bottom and fill in
> your real numbers (run the collection commands at the very bottom). **13700K, i5000,
> DJBOOTH** — your rows are stubs below; please complete them.

---

## 🎮 14900K — gameplay, content & showcase  *(this machine — COMPLETE)*

| Component | Spec |
|---|---|
| **CPU** | Intel Core i9-14900K — 24 cores (8 P + 16 E) / 32 threads, base 3.2 GHz, ~6.0 GHz boost |
| **GPU** | NVIDIA GeForce RTX 5090 — 32 GB GDDR7, **PCIe 5.0 ×16**, driver 32.0.15.9186 |
| **RAM** | 128 GB DDR5 (4 × 32 GB Corsair @ 4000 MT/s) |
| **Storage** | Samsung SSD 990 PRO 2 TB **NVMe** (PCIe 4.0 ×4) + USB backup |
| **Monitors** | 3 × 32" QHD **2560×1440 @ 165 Hz** |
| **Motherboard** | MSI PRO Z790-A MAX WIFI (MS-7E07) |
| **OS** | Windows 11 Pro (build 26200) |
| **Network** | 1200 Mbps ↑/↓ fiber |
| **Role** | gameplay, content, showcase, RT/5090 features, sub-agent orchestration |

---

## 🛠️ 13700K — clean-room engine build rig  *(STUB — please complete)*

| Component | Spec |
|---|---|
| **CPU** | Intel Core i7-13700K |
| **GPU** | NVIDIA GTX 1080 Ti (the **dev GPU floor** — keep raster/compute the everywhere-path) |
| **RAM** | _TODO_ |
| **Storage** | _TODO_ |
| **Monitors** | _TODO_ |
| **Motherboard** | _TODO_ |
| **OS** | _TODO_ |
| **Network** | _TODO_ |
| **Role** | clean-room engine/renderer build + integrator (merges → `main`) |

## 🎧 DJBOOTH — garage gameplay engineer  *(STUB — please complete)*

| Component | Spec |
|---|---|
| **CPU** | Intel Core i7-4790K |
| **GPU** | NVIDIA GTX 1080 Ti |
| **RAM** | _TODO_ |
| **Storage** | _TODO_ |
| **Monitors** | _TODO_ |
| **Motherboard** | _TODO_ |
| **OS** | _TODO_ |
| **Network** | _TODO_ |
| **Role** | clean-room gameplay (Act-2 roster, etc.) |

## 🖥️ i5000 — worker  *(COMPLETE)*

| Component | Spec |
|---|---|
| **CPU** | Intel Core i7-4790K (Haswell) — 4 cores / 8 threads, 4.0 GHz base (~4.4 turbo) |
| **GPU** | 2 × NVIDIA GTX 980 Ti — 6 GB GDDR5 each, **PCIe 3.0 ×16**, driver 32.0.15.6094 *(Maxwell; **below the 1080 Ti dev floor** — the fleet's true raster/compute floor, no hardware RT)* |
| **RAM** | 32 GB DDR3-1600 (4 × 8 GB) |
| **Storage** | Samsung 980 PRO 2 TB **NVMe** (PCIe 4.0; X3Native repo, `E:`) + 960 EVO 1 TB NVMe (`C:`) + 860 EVO 1 TB SATA (`D:`) |
| **Monitors** | 1 × 32" 4K — **3840×2160 @ 60 Hz** |
| **Motherboard** | ASUS Z97-A (BIOS 3503) |
| **OS** | Windows 11 Pro **23H2** (build 22631.6199) |
| **Network** | Wyyerd fiber — **510 ↓ / 564 ↑ Mbps** (Phoenix AZ) |
| **Role** | worker — Act-2 desert lane (`act2_desert` L10/L11) + prototypes (rotor-spin); pushes feature branches, integrator re-gates |

---

## Template — copy for your row
```
## <NAME> — <role>
| Component | Spec |
|---|---|
| CPU | |
| GPU | (VRAM, PCIe gen/lanes, driver) |
| RAM | (total, type, speed) |
| Storage | (model, size, NVMe/SATA, PCIe gen) |
| Monitors | (count, size, resolution, refresh) |
| Motherboard | |
| OS | |
| Network | (up/down) |
| Role | |
```

## Collect your specs (Windows PowerShell — paste output into your row)
```powershell
Get-CimInstance Win32_Processor | ForEach-Object { "$($_.Name.Trim()) | $($_.NumberOfCores)C/$($_.NumberOfLogicalProcessors)T" }
"RAM total: $([math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB)) GB"
Get-CimInstance Win32_PhysicalMemory | ForEach-Object { "  $([math]::Round($_.Capacity/1GB)) GB @ $($_.Speed) MT/s $($_.Manufacturer)" }
Get-CimInstance Win32_VideoController | Where-Object { $_.AdapterRAM -or $_.Name -match 'NVIDIA|AMD|Radeon|GeForce' } | ForEach-Object { "$($_.Name) | driver $($_.DriverVersion) | $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)Hz" }
"Monitors: $((Get-CimInstance -Namespace root\wmi WmiMonitorBasicDisplayParams).Count)"
Get-PhysicalDisk | ForEach-Object { "$($_.FriendlyName) | $([math]::Round($_.Size/1GB)) GB | $($_.MediaType) | $($_.BusType)" }
$b=Get-CimInstance Win32_BaseBoard; "$($b.Manufacturer) $($b.Product)"
(Get-CimInstance Win32_OperatingSystem).Caption
```
_(GPU VRAM over 4 GB and PCIe gen aren't reliable via WMI — fill those in by hand from GPU-Z / the NVIDIA panel.)_
