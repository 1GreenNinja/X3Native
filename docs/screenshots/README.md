# X3Native — Development Screenshots

Visual reference / regression baselines of the engine layer (1280×720, captured headless on the 13700K / GTX 1080 Ti). Regenerate any of these with the headless flags below — no window opens.

| File | What it shows | Regenerate |
|------|---------------|-----------|
| `level1_corridor.png` | EFLZ Level 1 corridor — real ModularSciFi art, **HDR + bloom + emissive** (glowing fixtures), **directional shadows**, **16 point lights**, **SSAO**, animated character. The current canonical interior look. | `X3Engine.exe --screenshot out.png` |
| `terrain.png` | The streamed 1 km² procedural heightmap world (LOD, per-tile collision) under the sky. | `X3Engine.exe --screenshot-terrain out.png` |
| `sky.png` | Analytic sky (gradient + sun disk/glow + horizon haze), sun-direction-consistent with shadows. | `X3Engine.exe --screenshot-sky out.png` |
| `monster_ai.gif` | Combat-AI demo — guard advances, drone flanks, wounded retreats, scout searches (facing is a consequence of state). | `X3Engine.exe --capture-ai out_dir` |

These are committed deliberately as dev reference (Tim's call). Loose scratch screenshots in the repo root are git-ignored; put anything worth keeping here with a descriptive name.
