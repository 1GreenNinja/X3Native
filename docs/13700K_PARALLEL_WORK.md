# 13700K — Parallel Clean-Room Work Queue

**The 13700K does not wait for the 14900K.** It builds the *clean* engine foundation (the eventual shipping engine) in parallel with the 14900K's RBDOOM prototype. Both converge: clean impls replace the GPL scaffold as they pass acceptance tests (`GPL_DEBT.md`).

**Machine:** i7-13700K, 128GB DDR5, 2x GTX 1080 Ti, 4TB NVMe (most disk of any rig).
**Rule:** clean-room — NEVER read RBDOOM source. Build only from `specs/*.spec.md` + public references + permissive libs. Run `tools/cleanroom-setup.ps1` first (it physically omits the GPL dir and hard-fails if present).

---

## Why the 13700K can start immediately

Most engine subsystems are **"adopt a permissive lib + write glue behind a clean interface."** Those need zero RBDOOM knowledge — they're standard, spec-able from public docs. Only the bespoke renderer internals (D2 materials, D3 shadows, D4 scene submission) need the 14900K spec team to study RBDOOM's approach first.

```
14900K (spec team + prototype)        13700K (clean-room foundation)
  fork RBDOOM, get X3 playable    │     build clean engine from specs + perm libs
  study RBDOOM for D2/D3/D4 specs │     D1 RHI, pak, glTF, Jolt, Lua, audio NOW
            └──────── converge: clean impls swap in per GPL_DEBT.md ────────┘
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

### Blocked-until-14900K (bespoke renderer — needs RBDOOM-derived specs)
- D2 Material/shader pipeline, D3 Cascaded shadows, D4 Scene submission/culling.
- The 14900K spec team writes these `*.spec.md` after surveying RBDOOM; then the 13700K implements them clean.

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
3. Builds with `USE_GPL_SCAFFOLD=OFF` (i.e., needs no RBDOOM code).
4. Flips its `GPL_DEBT.md` row to DONE-CLEAN with SHA + machine = 13700K (independent-creation evidence).
