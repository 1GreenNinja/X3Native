# Regenerate engine/rhi/font_robotomono.h from assets/fonts/RobotoMono-Regular.ttf.
# Embeds the TTF as a C byte array so the HUD/menu glyph atlas always has a font
# to bake (zero external-file dependency). Run from the repo root:
#   pwsh -File tools/embed_font.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'assets/fonts/RobotoMono-Regular.ttf'
$out  = Join-Path $root 'engine/rhi/font_robotomono.h'
if (-not (Test-Path $src)) { throw "font not found: $src" }
$bytes = [System.IO.File]::ReadAllBytes($src)
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// AUTO-GENERATED — do not edit by hand.")
[void]$sb.AppendLine("// Embedded TTF: Roboto Mono Regular (Apache License 2.0, (c) Google Inc.).")
[void]$sb.AppendLine("// A clean modern monospace font baked into the HUD/menu glyph atlas via")
[void]$sb.AppendLine("// stb_truetype at device init. Source: assets/fonts/RobotoMono-Regular.ttf.")
[void]$sb.AppendLine("// Regenerate with tools/embed_font.ps1 if the .ttf is replaced.")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <cstddef>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("namespace x3::rhi {")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("inline const unsigned char kRobotoMonoTTF[] = {")
$line = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $bytes.Length; $i++) {
    [void]$line.Append($bytes[$i]); [void]$line.Append(',')
    if ((($i + 1) % 20) -eq 0) { [void]$sb.AppendLine($line.ToString()); $line.Clear() | Out-Null }
}
if ($line.Length -gt 0) { [void]$sb.AppendLine($line.ToString()) }
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("inline const size_t kRobotoMonoTTFSize = sizeof(kRobotoMonoTTF);")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("} // namespace x3::rhi")
[System.IO.File]::WriteAllText($out, $sb.ToString())
Write-Output ("Wrote $out (" + $bytes.Length + " font bytes)")
