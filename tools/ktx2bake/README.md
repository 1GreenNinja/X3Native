# tools/ktx2bake — KTX2/Basis Texture Bake Tool

Batch-encodes PBR source textures (PNG/JPG/TGA) into GPU-ready **KTX2** files
using **UASTC** (high-quality, Vulkan/GPU-native) or **ETC1S/BasisLZ**
(smaller, universal transcoding) compression, generates mipmaps, and writes a
`manifest.json` that feeds the glTF loader and `.x3pak` pipeline.

## Quick Start

```powershell
# Bake a folder of textures (auto mode: heuristic picks UASTC vs ETC1S)
.\bake.ps1 -InputDir C:\path\to\textures -OutputDir C:\path\to\ktx2_out

# Bake a small sample (first 10 files only)
.\bake.ps1 -InputDir . -OutputDir .\out -MaxFiles 10

# Force re-encode (ignore cache)
.\bake.ps1 -InputDir . -OutputDir .\out -Force

# Force a specific format for all files
.\bake.ps1 -InputDir . -OutputDir .\out -Mode uastc
.\bake.ps1 -InputDir . -OutputDir .\out -Mode etc1s
```

The first run auto-downloads `toktx.exe` (KTX-Software v4.4.2, ~6 MB) into
`tools/ktx2bake/bin/` if it is not already on `PATH` or in the Vulkan SDK.

## Encoder: `toktx` (KhronosGroup/KTX-Software)

| Property | Value |
|---|---|
| Tool | `toktx` v4.4.2 |
| Source | [KhronosGroup/KTX-Software](https://github.com/KhronosGroup/KTX-Software/releases) |
| License | Apache 2.0 |
| Fetch method | NSIS silent installer → extracts to `bin/ktx_extract/` (gitignored) |

**Why toktx over basisu?**  
`toktx` is the official Khronos tool, directly outputs KTX2, supports both
UASTC and ETC1S in a single binary, and has a stable release with a Windows
x64 installer. `basisu` (BinomialLLC) works equally well but its GitHub
releases currently ship no pre-built Windows binaries (source-only), requiring
a build step.

## Format Heuristic (auto mode)

| Map type | Chosen format | Reasoning |
|---|---|---|
| Normal maps | **UASTC** | Directional data; ETC1S banding causes specular artefacts |
| Roughness / metalness / AO | **UASTC** | Single-channel data; precision matters for PBR |
| Height / displacement | **UASTC** | Linear precision required |
| Emissive | **UASTC** | Can be HDR-ish; avoids color shift |
| Albedo / color / diffuse | **ETC1S** | Perceptually tolerant; much smaller output |

**Filename keywords that select UASTC:**
`normal`, `_norm`, `nrm`, `roughness`, `rough`, `metal`, `metallic`, `_mr`,
`metalrough`, `ao`, `occlusion`, `ambient_occ`, `height`, `disp`,
`displacement`, `emissive`, `emission`

Everything else → ETC1S.

## Disk Cache / Skip Logic

If `OutputDir/manifest.json` exists and a texture's source SHA-256 matches the
recorded hash, the file is **skipped** (not re-encoded). Pass `-Force` to
override. This makes incremental re-bakes on large corpora very fast.

## manifest.json Schema

```json
{
  "generated": "2026-05-20T21:30:00Z",
  "encoder": "toktx v4.4.2 (KhronosGroup/KTX-Software)",
  "encoderPath": "C:/...bin/toktx.exe",
  "inputDir": "C:/textures/_originals",
  "outputDir": "C:/textures/ktx2",
  "mode": "auto",
  "textures": [
    {
      "sourceRelPath": "environment/floor_albedo.png",
      "sourceSHA256":  "ABCDEF...",
      "outputRelPath": "environment/floor_albedo.ktx2",
      "format":        "etc1s",
      "width":         2048,
      "height":        2048,
      "sourceSizeBytes": 8388608,
      "outputSizeBytes": 524288,
      "compressionRatio": 16.0,
      "status":        "encoded"
    }
  ]
}
```

`status` is one of `encoded`, `skipped`, or `failed`.

## Running on the Full Babylon-X3 Corpus

The 2.8GB master corpus lives in the **Babylon X3 repo** on another machine
(`textures/_originals/`). To bake it:

1. Mount the source share or copy `textures/_originals/` to the local 4TB NVMe.
2. Run:

```powershell
cd G:\X3Native-wt-ktx2\tools\ktx2bake

.\bake.ps1 `
    -InputDir  "D:\BabylonX3\textures\_originals" `
    -OutputDir "D:\BabylonX3\textures\ktx2" `
    -Mode auto
```

The 13700K (128 GB RAM, 4 TB NVMe) is ideal for parallel batch jobs. `toktx`
uses all CPU cores by default.

For maximum throughput on 100+ files, you can shard by subdirectory and invoke
multiple `bake.ps1` instances from PowerShell jobs:

```powershell
$subdirs = Get-ChildItem "D:\BabylonX3\textures\_originals" -Directory
$subdirs | ForEach-Object -Parallel {
    & "G:\X3Native-wt-ktx2\tools\ktx2bake\bake.ps1" `
        -InputDir  $_.FullName `
        -OutputDir ("D:\BabylonX3\textures\ktx2\" + $_.Name) `
        -Mode auto
} -ThrottleLimit 4
```

## Connecting to the glTF Loader (Task 3)

The glTF loader (`engine/asset/GltfLoader`) reads KTX2 textures via
`basis_universal`'s C++ transcoder (already a vcpkg dependency). The
`manifest.json` provides:

- The `outputRelPath` for each source texture, allowing the loader to resolve
  `.png` → `.ktx2` at load time.
- `format` to know whether to use the UASTC or ETC1S transcoder path.
- `sourceSHA256` for asset-integrity checks.

## Connecting to `.x3pak` (Task 2)

The pak builder (`tools/x3pakbuild/`) ingests the KTX2 output directory and
the `manifest.json`. Each `.ktx2` entry becomes a virtual-path asset in the
pak, preserving the relative path structure. The manifest's `compressionRatio`
field is written into the pak's own asset-table for runtime budgeting.

## Requirements

- Windows 10/11 x64
- PowerShell 5.1+ (ships with Windows)
- Internet access **only for first run** (to download toktx ~6 MB), OR:
  - `toktx.exe` on `PATH`, **or**
  - Vulkan SDK installed (provides `toktx` in SDK 1.3.261+)
- `System.Drawing` .NET assembly (for image dimension reading — included with
  .NET Framework on Windows)
