# probe_hw.ps1 — fleet HW snapshot generator
#
# Run on any Windows machine in the X3Native fleet to emit a markdown table
# in the canonical fleet format (matches DJBOOTH.md's HARDWARE section).
#
# Usage:
#   pwsh -NoProfile -ExecutionPolicy Bypass -File qa\probe_hw.ps1
#
# Output goes to stdout — pipe to a file or copy/paste into your machine's
# per-machine markdown (e.g. DJBOOTH.md, Snake13700k.md, i5000.md, 14900K.md).
#
# After paste, hand-fill the rows the script can't probe (marked TODO):
#   - WAN throughput  (run a speedtest)
#   - Custom notes / "Surprising facts from the probe" commentary

$ErrorActionPreference = 'SilentlyContinue'

function Fmt-Bytes($b) { if ($b -ge 1TB) { "{0:N1} TB" -f ($b/1TB) } elseif ($b -ge 1GB) { "{0:N0} GB" -f ($b/1GB) } elseif ($b -ge 1MB) { "{0:N0} MB" -f ($b/1MB) } else { "$b B" } }

# CPU
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$cpuStr = "$($cpu.Name.Trim()) — $($cpu.NumberOfCores)C/$($cpu.NumberOfLogicalProcessors)T, $([math]::Round($cpu.MaxClockSpeed/1000,1))GHz, $($cpu.SocketDesignation)"

# Computer system + Mobo + BIOS
$cs   = Get-CimInstance Win32_ComputerSystem
$bb   = Get-CimInstance Win32_BaseBoard
$bios = Get-CimInstance Win32_BIOS
$mobo = "$($bb.Manufacturer.Trim()) $($bb.Product.Trim())"
$biosStr = "$($bios.Manufacturer.Trim()) v$($bios.SMBIOSBIOSVersion), dated $($bios.ReleaseDate.ToString('yyyy-MM-dd'))"

# RAM
$ramSticks = Get-CimInstance Win32_PhysicalMemory
$ramTotalGB = [math]::Round(($ramSticks | Measure-Object -Property Capacity -Sum).Sum / 1GB, 0)
$ramSpeed = ($ramSticks | Select-Object -First 1).ConfiguredClockSpeed
$ramType = switch (($ramSticks | Select-Object -First 1).SMBIOSMemoryType) { 24 {'DDR3'} 26 {'DDR4'} 34 {'DDR5'} default {'DDR?'} }
$ramSummary = "$ramTotalGB GB $ramType-$ramSpeed = $($ramSticks.Count)x$([math]::Round($ramSticks[0].Capacity/1GB,0))GB"
$ramDimms = $ramSticks | ForEach-Object { "$($_.DeviceLocator): $($_.Manufacturer.Trim()) $($_.PartNumber.Trim()) @ $($_.ConfiguredClockSpeed) MT/s" }

# GPU (skip Microsoft Basic Display + Remote Desktop adapters)
$gpus = Get-CimInstance Win32_VideoController | Where-Object { $_.Name -notmatch 'Basic Display|Remote|Hyper-V' }
$gpuRows = $gpus | ForEach-Object {
  $vram = if ($_.AdapterRAM -gt 0 -and $_.AdapterRAM -lt 4GB) { ", $(Fmt-Bytes $_.AdapterRAM)" } else { '' }
  $drvDate = if ($_.DriverDate) { $_.DriverDate.ToString('yyyy-MM-dd') } else { '?' }
  "$($_.Name)$vram. Driver $($_.DriverVersion) ($drvDate)"
}

# Monitors
$monIds = Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorID
$monDims = Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBasicDisplayParams
$monRows = @()
for ($i = 0; $i -lt $monIds.Count; $i++) {
  $mfr  = -join ($monIds[$i].ManufacturerName | Where-Object {$_ -ne 0} | ForEach-Object {[char]$_})
  $name = -join ($monIds[$i].UserFriendlyName | Where-Object {$_ -ne 0} | ForEach-Object {[char]$_})
  $year = $monIds[$i].YearOfManufacture
  $w = $monDims[$i].MaxHorizontalImageSize
  $h = $monDims[$i].MaxVerticalImageSize
  $diag = if ($w -and $h) { " ~$([math]::Round([math]::Sqrt($w*$w + $h*$h) / 2.54, 0))""" } else { '' }
  $monRows += "$mfr $name (mfr $year)$diag $($w)x$($h)cm"
}
# Add display resolutions from the GPU rows (current desktop res)
$resList = $gpus | Where-Object { $_.CurrentHorizontalResolution } | ForEach-Object { "$($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)Hz" }
$monStr = ($monRows -join '; ')
if ($resList) { $monStr += " · resolutions: " + ($resList -join ', ') }

# Storage
$disks = Get-PhysicalDisk | Sort-Object DeviceID
$diskRows = $disks | ForEach-Object { "$($_.FriendlyName) ($($_.BusType), $($_.MediaType), $(Fmt-Bytes $_.Size))" }
$vols = Get-Volume | Where-Object DriveLetter | Sort-Object DriveLetter | ForEach-Object {
  "$($_.DriveLetter): $($_.FileSystemType) $(Fmt-Bytes $_.Size) ($(Fmt-Bytes $_.SizeRemaining) free)"
}

# NIC
$nic = Get-NetAdapter | Where-Object Status -eq 'Up' | Select-Object -First 1
$nicStr = if ($nic) { "$($nic.InterfaceDescription), link $($nic.LinkSpeed), MAC $($nic.MacAddress)" } else { '?' }

# OS
$os = Get-CimInstance Win32_OperatingSystem
$osStr = "$($os.Caption.Trim()) (build $($os.BuildNumber)), $($os.OSArchitecture), installed $($os.InstallDate.ToString('yyyy-MM-dd')), uptime $([math]::Round(((Get-Date)-$os.LastBootUpTime).TotalHours,1)) hrs"

# Power plan
$pp = (powercfg /getactivescheme) -replace '.*\((.*)\).*','$1'

# === EMIT MARKDOWN ===
$today = (Get-Date).ToString('yyyy-MM-dd')
$hostName = $env:COMPUTERNAME

@"
## HARDWARE — $hostName snapshot ($today)

Tag for fleet comparison: **TODO: fleet-tag (e.g. ``garage 4790K / 1080Ti / Z97``)**.

| Component | Value |
|---|---|
| **CPU** | $cpuStr |
| **Motherboard** | $mobo |
| **BIOS** | $biosStr |
| **RAM** | $ramSummary |
"@

foreach ($d in $ramDimms) { "| ↳ $($d -replace ':',':') |  |" }

@"
| **GPU** | $($gpuRows -join '<br>') |
| **Monitors** | $monStr |
| **Storage (physical)** | $($diskRows -join '<br>') |
| **Volumes** | $($vols -join '<br>') |
| **NIC** | $nicStr |
| **WAN throughput** | **TODO: <down> Mbps ↓ / <up> Mbps ↑ measured. Fleet target: 1200 Mbps fiber.** |
| **OS** | $osStr |
| **Power plan** | $pp |

### Surprising facts from the probe
- TODO: anything that contradicts what you thought was in this box.

### Build env on $hostName (if installed)
- TODO: VS / Vulkan SDK / vcpkg paths + versions if this machine builds X3Native.
"@
