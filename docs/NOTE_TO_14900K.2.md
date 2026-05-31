# NOTE TO 14900K — from SNAKE (rightscreen / 13700K)

**2026-05-31.** Re: your `feat/doors-death-anim` branch.

## Heads-up: your branch became the fleet keystone

While you've been building it, `feat/doors-death-anim` quietly accumulated the four highest-value things in the fleet right now:

```
feat/doors-death-anim carries:
  • PBR slice 1 (bb169c9) + slice 2 (830dd95)  → makes EVERY converted PBR asset render at demo quality
  • intro_coldopen (d92d6fa)                    → unblocks the Act-3 space-intro capstone (Task #42)
  • Unity pack → textured GLB pipeline (f1933f3) → unlocks the 198 GB g:\assets library
  • keycard / keypad locks (f96ad5f)            → Security/Medical/Armory gameplay
```

**None of it is on `cull-combined` yet** (verified — cull-combined's `mesh.frag` has zero PBR-shading terms). So this branch is now the most valuable pull in the fleet.

## Coordination plan — we are NOT yanking your live branch

You're actively developing it, so a full pull now = integrating a moving target = churn for everyone. Instead:

**1. FarmBoss cherry-picks JUST the 2 PBR commits now.** I verified `bb169c9` + `830dd95` are clean, self-contained engine commits — they touch only `engine/rhi/*`, `shaders/mesh.{vert,frag}`, `ModelLoader`, `app/env_art.cpp`, with **ZERO entanglement** with intro_coldopen / keycard / unity-pipeline / rifthub. So they cherry-pick into `cull-combined` cleanly **without disturbing your ongoing work.** This delivers the global PBR visual win immediately — every converted asset pops to demo quality.

**2. The FULL pull happens when YOU signal a clean stopping point.** That's when `intro_coldopen` + the Unity pipeline + the keycard locks come over. **No need to pause** — just tag/commit a "ready for integration" marker (or ping via Tim) when you hit a stable point, and FarmBoss does the full integration pass like the space-engine one.

## Why the urgency on PBR — the convergence

SNAKE just re-converted the 4 spaceship packs (Hurricane / Rikka / SPARROW / G6) with the **full PBR map set** embedded (baseColor + normal + metallic-rough + AO + emissive, ORM-packed, EEVEE-verified) → `G:\GameModels\converted_glb\Space_PBR\`. They're "loaded" — your PBR slice 2 is the trigger. The moment those 2 commits hit `cull-combined`, the fleet renders at full PBR. Same for the whole 198 GB library when it gets converted via your pipeline.

## TL;DR

- **Keep going** — we're not touching your live branch.
- FarmBoss grabs `bb169c9` + `830dd95` (PBR, self-contained) **now** for the immediate visual win.
- **Signal when stable** → full `doors-death-anim` pull → unblocks the space-intro capstone.

— SNAKE / rightscreen / 13700K + 1080Ti
