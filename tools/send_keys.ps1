# send_keys.ps1 — hold keys on the X3Engine window for a while.
#
# Companion to grab_window.ps1: lets an agent DRIVE an interactive run to a
# viewpoint worth photographing, because the --screenshot-* harness runs
# headless and cannot reproduce interactive-only code paths.
#
#   pwsh tools/send_keys.ps1 -Keys W,D -Ms 4000
# Keys are single letters / digits, or the names Up Down Left Right Space Shift.
param(
    [Parameter(Mandatory = $true)][string[]]$Keys,
    [int]$Ms = 1000,
    [string]$Proc = 'X3Engine'
)

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class KeySend {
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
'@

$vk = @{
    'Up' = 0x26; 'Down' = 0x28; 'Left' = 0x25; 'Right' = 0x27
    'Space' = 0x20; 'Shift' = 0x10; 'Ctrl' = 0x11; 'Tab' = 0x09; 'Enter' = 0x0D
}

$p = Get-Process -Name $Proc -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "no $Proc window found"; exit 1 }
[void][KeySend]::ShowWindow($p.MainWindowHandle, 9)
[void][KeySend]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Milliseconds 400

$codes = foreach ($k in $Keys) {
    if ($vk.ContainsKey($k)) { $vk[$k] } else { [byte][char]($k.ToUpper()) }
}
foreach ($c in $codes) { [KeySend]::keybd_event([byte]$c, 0, 0, [IntPtr]::Zero) }
Start-Sleep -Milliseconds $Ms
foreach ($c in $codes) { [KeySend]::keybd_event([byte]$c, 0, 2, [IntPtr]::Zero) }   # KEYEVENTF_KEYUP
Write-Host "held $($Keys -join '+') for $Ms ms"
