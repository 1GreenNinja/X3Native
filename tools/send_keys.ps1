# send_keys.ps1 — hold keys on the X3Engine window for a while.
#
# Companion to grab_window.ps1: lets an agent DRIVE an interactive run to a
# viewpoint worth photographing, because the --screenshot-* harness runs
# headless and cannot reproduce interactive-only code paths.
#
#   pwsh tools/send_keys.ps1 -Keys W,D -Ms 4000
# Keys are single letters / digits, or the names Up Down Left Right Space Shift.
# With -Text instead of -Keys it TYPES a string (for the ~ console), e.g.
#   pwsh tools/send_keys.ps1 -Text 'tp lot' -Console
# -Console wraps the text in a ~ to open the console and Enter + ~ to run it and
# close again.
param(
    [string[]]$Keys,
    [string]$Text,
    [switch]$Console,
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
    [DllImport("user32.dll")] public static extern short VkKeyScan(char c);
    // GLFW's win32 backend decides WHICH key you pressed from the SCANCODE in
    // lParam, not from the virtual key. keybd_event with bScan = 0 leaves that
    // field empty, so GLFW resolves the event to GLFW_KEY_UNKNOWN and the
    // host's `~` console toggle never fires — the letters that follow then land
    // in the game as gameplay binds (which is how "tp lot" turned traction
    // control off instead of teleporting). Always send the real scancode.
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
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

function Scan([int]$code) { return [byte]([KeySend]::MapVirtualKey([uint32]$code, 0)) }

function Tap([int]$code, [bool]$shift) {
    $sc = Scan $code
    if ($shift) { [KeySend]::keybd_event(0x10, (Scan 0x10), 0, [IntPtr]::Zero) }
    [KeySend]::keybd_event([byte]$code, $sc, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [KeySend]::keybd_event([byte]$code, $sc, 2, [IntPtr]::Zero)
    if ($shift) { [KeySend]::keybd_event(0x10, (Scan 0x10), 2, [IntPtr]::Zero) }
    Start-Sleep -Milliseconds 40
}

if ($Text) {
    if ($Console) { Tap 0xC0 $false; Start-Sleep -Milliseconds 500 }   # VK_OEM_3 = ~
    foreach ($ch in $Text.ToCharArray()) {
        $s = [KeySend]::VkKeyScan($ch)
        Tap ($s -band 0xFF) ((($s -shr 8) -band 1) -eq 1)
    }
    if ($Console) {
        Start-Sleep -Milliseconds 200
        Tap 0x0D $false                                   # Enter
        Start-Sleep -Milliseconds 800
        Tap 0xC0 $false                                   # close the console
    }
    Write-Host "typed '$Text'"
    exit 0
}

if (-not $Keys) { Write-Error "give -Keys or -Text"; exit 1 }
$codes = foreach ($k in $Keys) {
    if ($vk.ContainsKey($k)) { $vk[$k] } else { [byte][char]($k.ToUpper()) }
}
foreach ($c in $codes) { [KeySend]::keybd_event([byte]$c, (Scan $c), 0, [IntPtr]::Zero) }
Start-Sleep -Milliseconds $Ms
foreach ($c in $codes) { [KeySend]::keybd_event([byte]$c, (Scan $c), 2, [IntPtr]::Zero) }  # KEYEVENTF_KEYUP
Write-Host "held $($Keys -join '+') for $Ms ms"
