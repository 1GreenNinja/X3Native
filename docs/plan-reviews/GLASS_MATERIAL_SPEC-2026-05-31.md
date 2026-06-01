# Plan Audit: docs/GLASS_MATERIAL_SPEC.md
**Verdict: GO**
Date: 2026-05-31  ·  Plan: docs/GLASS_MATERIAL_SPEC.md

## Verdict rationale
GO — but with an important caveat the dispatcher must understand. This document is a
**dated authoritative design spec (2026-05-27)**, not a forward-looking implementation
plan, and the rework it prescribes has **already been fully implemented** in the
codebase. The "Shader recipe", entire "ADD" list, "DROP" list, and the holo-terminal
fix are all present in real source today (`shaders/glass.frag`,
`engine/rhi/IRenderDevice.h`, `engine/rhi/VulkanRenderDevice.cpp`, `app/holo_terminal.cpp`).
`glass.frag:4-8` even cites this exact spec as its authority. No High finding will
break an implementation pass, because there is no remaining implementation to do —
following this spec reproduces what already exists. Zero auto-fixes were applied: every
contradiction is between the spec's **"root cause" snapshot of the OLD broken code** and
the **current refactored code**, i.e. the spec is now historically stale, not mechanically
wrong in a way with one clear evidenced replacement. Rewriting a dated design record is a
human judgement call (keep as history vs. mark superseded), so per the auto-fix rule
("do NOT guess; flag ambiguous/design-gap items") I flagged rather than edited.
Clean-room: clean — math sourced from Filament/LearnOpenGL/UE5 public docs only; no
forbidden/RBDOOM reference.

## Auto-fixes applied (0)
None. All defects are stale-snapshot contradictions in a dated reference doc, not
single-value mechanical errors — auto-fixing would be guessing at intent.

## Remaining High findings (block implementation) (0)
None. (See note below: the High-class contradictions are all "spec describes already-fixed
code", which does not block — the work is done. Listed as Medium "stale" items instead.)

## Medium findings (8)
- **Root-cause §1 is stale** — "Why the old glass read…" #1: "`makeHologramRGBA` fills every
  pixel ~(0.05,0.16,0.34), alpha=255". The current function bakes a FULLY BLACK +
  fully-transparent background (alpha tracks ink brightness, 0 where unlit) and routes the
  line-art through EMISSIVE — exactly the "holo-terminal fix" this same doc prescribes at
  the bottom. The doc contradicts itself: §1 describes pre-fix code, the fix section
  describes the shipped code. — Evidence: `app/holo_terminal.cpp:283-296,489-494`. Fix: a
  human should mark the root-cause section as "historical (pre-2026-05-27)" or move it to a
  changelog; do not present it as current behavior.
- **Root-cause §2 (`m_emBase` flat teal flood) is stale** — the runtime build() overwrites
  `m_emBase` to multiplicative white {1,1,1,2.6} so emissive MULTIPLIES the texel (only
  stroked pixels light up); it is no longer a uniform teal flood. (The header default
  {0.18,0.70,1.0,1.9} is dead — overwritten before first use.) — Evidence:
  `app/holo_terminal.cpp:789-790` (override); `app/holo_terminal.h:103` (overridden default).
- **Root-cause §3 (`opacity 0.55`) is stale** — clear panes now ship at opacity 0.10, not
  0.55. — Evidence: `app/holo_terminal.cpp:781`, `app/glass_lounge.cpp:120`.
- **Root-cause §4 ("F0 hard-coded 0.04") is stale** — F0 is now computed from IOR via
  `computeF0` (`((ior-1)/(ior+1))^2`, with metallic mix and a reflectance fallback), not a
  hardcoded constant. — Evidence: `shaders/glass.frag:121-126,198`.
- **Root-cause §5 ("no real reflection term, only flat ambient sheen") is stale** — a real
  fresnel-weighted screen-space environment reflection + sky gradient is implemented. —
  Evidence: `shaders/glass.frag:227-240`.
- **Shader-recipe "Replace the ad-hoc `ggxSpec*0.25`" references a symbol that does not
  exist** — `ggxSpec` appears nowhere in any shader or source (searched `shaders/`,
  `engine/`, `app/`; only matches are this spec + docs mirrors). The replacement it asks for
  (D_GGX · V_SmithGGX · F_Schlick Cook-Torrance) is already present. — Evidence: absence
  across repo; real BRDF at `shaders/glass.frag:128-158`. Fix: drop the dangling reference;
  the "ad-hoc" term it names is already gone.
- **Entire "ADD" list is already done** — (1) metallic/ior/reflectance/transmittanceColor
  params + payload passthrough, (2) computeF0 from ior, (3) fresnel env reflection,
  (4) kT·background composite, (5) D·V·F BRDF, (6) roughness-driven mip LOD — all present.
  — Evidence: struct `engine/rhi/IRenderDevice.h:185-195`; packing
  `engine/rhi/VulkanRenderDevice.cpp:1302-1318`; SSBO `:1599-1601`; shader
  `glass.frag:121-126,198,208-240,279-291,220,236`. An implementer reading this as a TODO
  would redo finished work.
- **Entire "DROP" list is already done** — the sin() animated glint band and the flat
  ambient sheen are both absent from `glass.frag`; no `sin(` call exists in the shader. —
  Evidence: `shaders/glass.frag` (no `sin(`; reflection is fresnel-weighted env, not a flat
  sheen).

## Low findings (2)
- **CLEAR opacity table value drift** — spec table prescribes CLEAR opacity **0.08**;
  shipped clear presets use **0.10**. Spec target vs. shipped value drift; not auto-fixed
  (cannot prove which is canonical). — `GLASS_MATERIAL_SPEC.md` params table, row "opacity".
  Evidence: `app/holo_terminal.cpp:781`, `app/glass_lounge.cpp:120`.
- **No status banner** — the doc carries no "IMPLEMENTED / SUPERSEDED" marker, so a reader
  cannot tell at a glance that it is a completed-work record rather than pending work.

## Coverage
Claims checked: 18  ·  Verified against source: 18  ·  Files inspected:
app/holo_terminal.cpp, app/holo_terminal.h, app/scene.h, app/glass_lounge.cpp,
engine/rhi/IRenderDevice.h, engine/rhi/VulkanRenderDevice.cpp, shaders/glass.frag
