# X3Native — Rendering Speed Roadmap

**Goal:** maximum frame rate. This is the whole reason for going native. The target is to beat the Babylon/WebGL X3 by a wide margin on the same hardware, and to scale across the many-core CPUs (14900K / 13700K) + modern GPUs (5090 / A2000 / 1080 Ti).

**Sourcing:** every technique here is from PUBLIC references (Vulkan spec, vkguide.dev, "Real-Time Rendering 4th ed", GPU-driven rendering papers, GDC talks). None require reading RBDOOM/GPL source.

---

## Where RBDOOM sits (accurate version)

- **id Tech 4** = the 2004 base (Doom 3). The entity/material/frame *architecture* lineage.
- **RBDOOM-3-BFG** (Robert Beckebans, ~2013→present) **modernized the renderer in ~2016-2021**: PBR, a **Vulkan backend**, soft shadow mapping (replacing stencil shadow volumes), image-based lighting / env probes, glTF2. So RBDOOM's rendering is genuinely modern (2016-2021) — a **solid, proven Vulkan+PBR baseline worth studying** (architecture notes on the 14900K).

What RBDOOM is **not**: the bleeding-edge frame-rate ceiling. Its Vulkan renderer is modern but still fairly traditional — per-draw descriptor binding, mostly CPU-side culling, forward/forward+. The 2021+ "GPU-driven" techniques (below) sit *above* it. So the plan is: **treat RBDOOM as the proven baseline reference, and add the GPU-driven layer on top for absolute max FPS.** Not "ignore RBDOOM" — extend past it where it matters for frame rate.

---

## The frame-rate stack (priority order)

### Tier 1 — foundational (build these into the renderer from the start)
1. **Bindless textures (descriptor indexing).** One big descriptor array; index per-draw via push-constant/SSBO. Eliminates per-draw descriptor set binding — the classic CPU bottleneck. *Already enabled in the D1 device (`descriptorIndexing=true`).*
2. **Persistent-mapped buffers + bump/ring allocators (VMA).** Map once, write each frame, never per-frame `vkAllocate`. Per-frame transient data (uniforms, instance data) goes in a ring buffer. *VMA lands in D2.*
3. **Pipeline State Objects cached to disk** (`VkPipelineCache` serialized). Kills first-encounter shader-compile stutter. Pre-warm common pipelines at load.
4. **Sort draws by pipeline → material → depth.** Minimize state changes; front-to-back for early-Z.
5. **Depth pre-pass** (optional, scene-dependent). Cheap Z-only pass so the expensive shading pass benefits from early-Z rejection. (This one IS an id-Tech-era idea — and it's public/standard.)

### Tier 2 — the big multipliers
6. **Multidraw indirect** (`vkCmdDrawIndexedIndirect` / `...Count`). Batch thousands of draws into one CPU call. Combined with bindless, the CPU stops being the bottleneck.
7. **GPU-driven culling.** Do frustum + Hi-Z occlusion culling in a compute shader; the GPU writes the indirect draw buffer. CPU just dispatches. This is *the* modern frame-rate technique — scenes with 10k+ objects render with near-zero CPU draw cost.
8. **Multithreaded command recording.** Record secondary command buffers across worker threads (one per core). The 14900K (24 cores) / 13700K (16 cores) make this a large win. id Tech 4 does not do this.

### Tier 3 — scaling + polish
9. **Instancing** for repeated geometry (foliage, props, crowds) — one draw, N instances.
10. **Mesh LOD** (distance-based) — fewer triangles far away. (X3 Babylon already had a LOD philosophy; port the distance tiers.)
11. **Async compute** — overlap post-processing / particle sim with graphics on a separate queue.
12. **Upscaling** (DLSS on RTX / FSR everywhere) — render at lower internal res, upscale. Single biggest "free" FPS on the 5090/A2000.
13. **Frame pacing** via timeline semaphores; triple-buffer; cap or uncap per setting.

---

## Quality presets carry over from Babylon X3

The Babylon X3 quality-preset philosophy (potato/low/medium/high/ultra scaling resolution, shadows, post-FX, particle density, draw distance) ports directly — it's how "average PC" hits 60. The native engine adds upscaling (DLSS/FSR) as another preset axis. Reuse the *tuning values* from `x3-quality-presets.js`, not code.

---

## Physics frame rate

Use **Jolt** (M3), not id Tech 4's `idPhysics`. Jolt is multithreaded, cache-friendly, and built for many-core CPUs (Horizon Forbidden West). Run physics on a fixed timestep on worker threads; interpolate render transforms. Don't crib id's 2004 physics — it'd be a downgrade.

---

## Measurement discipline (non-negotiable)

- **Tracy** from the start (CPU + GPU timeline). Every optimization is measured, not guessed.
- **GPU timestamp queries** per pass — know where the frame goes.
- A `stats`/`r_speeds`-style console command (D6 console) showing draw calls, triangles, pass timings, FPS.
- Validation-clean always (a CI gate) — sync hazards silently tank frame rate.

---

## What's done vs planned (2026-05-20)

- ✅ D1 device with Vulkan 1.3 dynamic rendering + descriptor-indexing/sync2/timeline-semaphore features enabled (the prerequisites for the whole Tier-1/2 stack).
- 🔜 Next: mesh rendering (pipeline + VMA buffers + shader + depth) → then bindless + multidraw + GPU culling layer on top.

The renderer is being built so the fast path IS the default path — not bolted on later.
