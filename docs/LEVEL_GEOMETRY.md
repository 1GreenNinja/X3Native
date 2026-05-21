# X3Native — Level Geometry & Authoring

## Decision: CSG brush authoring → mesh bake → modern runtime (no runtime BSP)

**Authoring (editor-time):** support a **CSG brush** workflow — convex brushes combined with **additive + subtractive** boolean ops. This is the fast, proven graybox paradigm (Quake/Radiant/TrenchBroom):
- Ramps = wedge brushes
- Doorways / windows / negative space = subtractive carving
- Stairs = stacked/clipped brushes
- **Curves = Bezier patch meshes** (a separate primitive — CSG booleans don't make smooth curves well; id Tech used patch meshes for this)

**Bake (build-time):** triangulate brushes + patches into **static meshes** + a collision mesh. This is the only thing the runtime sees.

**Runtime:** meshes feed the modern renderer (GPU-driven culling — frustum + Hi-Z occlusion, see `RENDERING_SPEED.md`) and **Jolt** static collision (`addStaticMesh`). 

### Why NOT BSP at runtime
BSP was a 1996-2004 visibility solution (PVS, software/early-GPU rasterization). Modern GPU-driven culling supersedes it. Carrying a BSP tree into the runtime would add complexity and *cost* frame rate vs. mesh + GPU culling. So: keep CSG as an **authoring** convenience; never make BSP the runtime structure.

## Phasing
1. **Now (walkable test):** procedural graybox meshes generated in code (floor, boxes, wedge ramps, steps). Same triangulated output a CSG tool would emit → feeds renderer + Jolt. No editor needed yet.
2. **Later (M8 editor):** a CSG brush + patch editor (Dear ImGui + viewport gizmos) that bakes to the same mesh/collision format. The runtime is unchanged.
3. **Import path:** also accept `.map` (Quake/Radiant) or glTF level exports → same bake. Lets you author in TrenchBroom today if desired.

## Collision
Static level geometry → one Jolt `MeshShape` per region (or per material zone). Dynamic props → convex/box shapes. Character → `CharacterVirtual` capsule. (See `specs/M3-physics-world.spec.md`.)

## Note on id Tech lineage
id Tech 4 (2011 GPL source) used brush CSG for authoring + a BSP/portal runtime. We keep the *authoring* idea (brushes/CSG/patches are genuinely good) and drop the *runtime* BSP in favor of mesh + GPU culling. Best of both eras.
