# Milestone M7 (advanced) — Cinematic Post-FX

**Branch:** `feat/m7-postfx-advanced` · **Carved from** `X3_NATIVE_SLICES.md` on 2026-06-05
**Slices:** 74, 75, 76, 81, 82

## Context

Core post-FX already shipped on `cull-combined` and is **not** in scope here:
SSAO/GTAO (77 ✅), screen-space GI (78 🚧), ACES tonemap (79 ✅), bloom (80 ✅),
GPU compute particles (73 ✅). This milestone adds the remaining *cinematic*
passes. Everything builds on the existing Vulkan 1.3 dynamic-rendering backend
(`engine/rhi/VulkanRenderDevice.cpp`) and the composite pass
(`shaders/composite.frag`).

## Gating prerequisite — velocity buffer

A per-object **velocity G-buffer target does not exist yet** and is required by
both motion blur (75) and TAA (81). Build it first; it is the single dependency
that unblocks two slices. Depth is already present (SSAO uses it) and DoF (74)
reuses it.

## Slices

### 74 📝 Depth of field
Cinematic bokeh DoF post-pass (reuses depth). **Gate:** focus pull looks clean.

### 75 📝 Motion blur (per-object + camera) — *needs velocity buffer*
Velocity-buffer-driven blur. **Gate:** fast motion blurs without smearing static geo.

### 76 📝 Order-independent transparency (OIT)
Weighted-blended or per-pixel linked-list OIT for glass/particles. Glass currently
uses forward refraction, not OIT — decide whether to route glass through OIT or
keep it separate. **Gate:** overlapping transparents sort correctly.

### 81 📝 TAA / DLSS hook — *needs velocity buffer + jitter*
Temporal AA; optionally wire DLSS/FSR (5090 → DLSS). Note the existing SSGI
temporal reprojection is GI-only and not reusable as scene TAA. **Gate:** clean
edges, no ghosting; DLSS toggle works.

### 82 📝 Post-FX quality presets
Low/Med/High/Ultra gating all of the above **plus** the already-shipped passes.
Settings currently exposes individual toggles only. **Gate:** each preset a
measurable FPS delta.

## Recommended build order

1. **Velocity buffer** (shared dependency)
2. **TAA (81)** — biggest image-quality win, also the path to temporal upsampling
3. **Motion blur (75)**
4. **DoF (74)**
5. **OIT (76)**
6. **Presets (82)** last — wraps every pass into the preset matrix

## Milestone gate

Presets toggle each pass with a measurable FPS delta on the 5090, validation-clean.
Capture before/after for the cinematic-parity review (full-res, per the
render-judging rule).
