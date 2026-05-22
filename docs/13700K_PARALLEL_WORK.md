# 13700K — Parallel Clean-Room Work Queue

**The 13700K is the primary build machine.** It builds the original engine from
scratch — from `specs/`, public references, and permissive libraries. (This was
originally framed as a "clean foundation" running *parallel* to a planned RBDOOM
prototype on the 14900K. That prototype never happened; the clean engine *is* the
engine. See `PROVENANCE.md`.)

**Machine:** i7-13700K, 128GB DDR5, 2x GTX 1080 Ti, 4TB NVMe (most disk of any rig).
**Rule:** clean-room — build only from `specs/*.spec.md` + public references + permissive libs. Never read or transcribe any third-party game-engine source. Run `tools/bootstrap.ps1` to clone + verify.

---

## Why the 13700K can start immediately

Most engine subsystems are **"adopt a permissive lib + write glue behind a clean interface."** They're standard and spec-able from public docs. The bespoke renderer internals (materials, shadows, scene submission/culling) are written from public rendering references (Vulkan spec, RTR4, GPU Gems, GDC/SIGGRAPH talks) + the author's research.

```
13700K (primary build machine)
  build the original engine from specs + public refs + permissive libs
  D1 RHI, pak, glTF, Jolt, Lua, audio → lighting, animation, renderer
  (record each subsystem in PROVENANCE.md)
```

---

## Work queue (ordered; items 1-6 have NO 14900K dependency)

### 1. D1 — Render Device (Vulkan 1.3)  ▶ START HERE
- Spec: `specs/D1-render-device.spec.md` (complete).
- Build: vk-bootstrap (instance/device/swapchain) + VMA (memory). Vulkan 1.3 dynamic rendering.
- Milestone: hello-triangle → clear-to-color → present, validation-clean, 600-frame run + resize storm (the 5 acceptance tests).
- This is the single most valuable parallel task — the clean RHI is the engine's spine.

### 2. Pak / VFS (.x3pak)
- Spec: `specs/D5-asset-source.spec.md` (written 2026-05-20 — ready).
- Build: miniz (MIT) zip pack + virtual-path mount + override order + manifest.
- Milestone: mount two paks, resolve an asset by virtual path with priority.

### 3. glTF loader + PBR import
- Spec: `specs/M2-gltf-loader.spec.md` ✅ ready. Build on cgltf (MIT) + basis_universal (KTX2).
- Disk-heavy: batch-import all 140 of Tim's converted GLBs; catalog clean vs needs-fixing → `docs/GLB_IMPORT_REPORT.md`.

### 4. Jolt physics world + character controller
- Spec: `specs/M3-physics-world.spec.md` ✅ ready. Build Jolt (MIT) behind `IPhysicsWorld`.
- Milestone: fixed-step world + falling box rests on floor + `JPH::CharacterVirtual` capsule walks/collides/steps. (8 acceptance tests)

### 5. Lua / sol3 embed
- Spec: `specs/M4-script-vm.spec.md` ✅ ready. sol3 + LuaJIT.
- Milestone: hello-from-Lua → bind log/cvar → spawn-entity API → hot-reload. (8 acceptance tests)

### 5b. Audio backend (parallel anytime)
- Spec: `specs/M9-audio-backend.spec.md` ✅ ready. miniaudio (MIT), 3D HRTF + music crossfade + buses.

### 5c. Console + cvars (parallel anytime, small)
- Spec: `specs/D6-console.spec.md` ✅ ready. Ports the Babylon X3 `quality` preset UX.

### 6. KTX2 / Basis texture bake  ▶ CAN RUN TODAY, no code
- Pure asset-pipeline tooling. The 2.8GB PBR masters in the Babylon X3 (`textures/_originals/`) → GPU-ready KTX2/Basis.
- Tool: `basisu` CLI or a small batch script. Output a compressed texture set + a manifest.
- This is the job tailor-made for the 128GB/4TB machine: parallel-compress hundreds of textures.
- Bonus: feeds straight into task 3 (glTF loader reads KTX2) and the eventual `.x3pak`.

### Bespoke renderer (spec from public rendering references)
- Material/shader pipeline, cascaded shadows, scene submission/GPU-driven culling.
- Spec these from public references (Vulkan spec, RTR4, GPU Gems, `RENDERING_SPEED.md`, public GDC/SIGGRAPH talks), then implement clean behind their interfaces.

---

## Heavy jobs that love this hardware

- **vcpkg build of all permissive deps** — RAM/CPU heavy, one-time. Do it early; everything links against it.
- **KTX2 bake** (task 6) — disk + CPU + RAM.
- **140-GLB batch import + validation** (task 3) — disk heavy.
- **Parallel agent farm** — Tim runs many Claude instances; shard tasks 1-5 across agents, each owning one subsystem behind its interface.

---

## Definition of done per task

1. Implements its clean interface (`engine/<sys>/I*.h` — header has NO third-party types leaking).
2. Passes the spec's acceptance tests.
3. Uses only permissive libraries (no copyleft deps).
4. Recorded in `PROVENANCE.md` with SHA + machine (independent-creation evidence); carries the in-file "no foreign source consulted" note.
