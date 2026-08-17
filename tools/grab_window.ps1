# grab_window.ps1 — capture the X3Engine window to a PNG.
#
# WHY THIS EXISTS. The --screenshot-* capture harness runs the world HEADLESS,
# and headless takes different code paths from an interactive run (host_tunnel's
# terrain stream radius is 14 headless vs 9 interactive, for one). A defect that
# only exists interactively is structurally invisible to that harness. This
# grabs the real window of a real interactive run so it can be looked at.
#
#   pwsh tools/grab_window.ps1 -Out shots/foo.png [-Proc X3Engine]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Proc = 'X3Engine'
)

Add-Type -AssemblyName System.Drawing
# NOTE: no -ReferencedAssemblies here. Referencing System.Drawing/Forms from the
# inline C# makes Add-Type fail silently under pwsh 7 (the type never lands and
# every later call reports "Unable to find type [WinGrab]"), so the interop
# surface stays pure primitives and the bitmap work happens in PowerShell.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class WinGrab {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct PT   { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref PT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
'@

$p = Get-Process -Name $Proc -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "no $Proc window found"; exit 1 }

$h = $p.MainWindowHandle
[void][WinGrab]::ShowWindow($h, 9)          # SW_RESTORE
[void][WinGrab]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 600

$r = New-Object WinGrab+RECT
[void][WinGrab]::GetClientRect($h, [ref]$r)
$origin = New-Object WinGrab+PT
[void][WinGrab]::ClientToScreen($h, [ref]$origin)
$w = $r.R - $r.L; $ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { Write-Error "client rect is empty ($w x $ht)"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g   = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($origin.X, $origin.Y, 0, 0, (New-Object System.Drawing.Size $w, $ht))
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $Out ($w x $ht)"
