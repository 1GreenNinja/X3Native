# NPC Character Interface — the living-city BODY seam

`app/npc_character.{h,cpp}` gives the living city real, skinned, animated,
ragdoll-capable citizen **bodies**, built entirely from tools we already have
(`x3::anim::Skinner` + `x3::game::RagdollSkin` + `x3::phys::IRagdoll`, the same
machinery `MonsterSystem` and `RescueVictim` use). It is a deliberate,
rescue-stripped generalization of `RescueVictim`.

## The seam (who owns what)

| Owner | Owns |
|---|---|
| **`feat/living-city` (behavior branch)** | schedules, spawn logic, the robbery, scan-cards, WHERE a citizen is, WHAT it's doing, WHEN it appears |
| **`NpcCharacter` (this branch)** | the mesh, skeleton, anim-set, GPU skinning, ragdoll, textures — the BODY |

The two fold together through a tiny surface. Behavior code never touches a rig,
a joint palette, or a Jolt body.

## The interface

```cpp
NpcCharacter npc;
// Stand a body up (feet at pos, facing yaw). archetypeId indexes the archetype table.
npc.spawn(archetypeId, scene, device, physics, /*modelDir*/"", pos, yaw);

// Each frame the host owns:
npc.update(dt, scene, physics);      // drives skinner OR flops the ragdoll

// Behavior drives it purely through:
npc.setAnimState(NpcAnimState::Walk, speedMps);   // Idle/Walk/Run/Flee/Talk
npc.moveTo(scene, physics, newPos);               // teleport-style move; auto walk/run
npc.setFacing(yaw);                               // logical heading (visual flip handled)
npc.triggerRagdoll(impulseVec);                   // hit-react / death FLOP

// Batch owner for a crowd:
NpcCrowd crowd;
crowd.spawnRing(count, scene, device, physics, "", cx, groundY, cz, radius);
crowd.update(dt, scene, physics);
crowd.draw(device, frame, scene);
crowd.despawnAll(scene, physics);
```

### Animation states (`NpcAnimState`)
`Idle / Walk / Run / Flee / Talk / HitReact / Death`
- **Idle/Walk/Run/Flee** drive the 1D locomotion blend (Idle→Walk→Run) by speed,
  dt-scaled and phase-continuous. `Flee` is Run semantics (the robbery/alert sprint).
  `moveTo()` measures planar speed and auto-selects the state, so a body that is
  simply moved *walks* without the behavior layer setting a state at all.
- **Talk** plays a Talk/gesture clip when the rig has one (AnnaCasual does), else it
  falls back to Idle — for dialog and scan-card interactions.
- **HitReact / Death** are realized **physically** by the ragdoll (the AAA approach —
  no canned hit clip). `setAnimState(HitReact|Death)` forwards to `triggerRagdoll()`.

### Ragdoll (`triggerRagdoll`)
Builds a Jolt humanoid ragdoll (engine `makeHumanoidRagdollBones` + `createRagdoll`)
placed/yawed/scaled to the body and **seeded from the current animated pose** so the
flop starts seamlessly. The bodies are constructed on the next `update()` (which holds
the Scene + IPhysicsWorld), so `triggerRagdoll` needs no physics argument.

**Settling is per-instance, NOT hardwired y=0.** The ragdoll falls under gravity and
collides with whatever **static floor** is under the body — at spawn a downward
raycast records `groundY()` as the per-instance ground reference, and the rig is placed
so its feet sit at the body's real spawn Y. `ragdollSettled()` reports rest (bodies
asleep); `ragdollLowestY()` exposes the settled height. The `--test-npcchar` self-test
proves this by settling a ragdoll on a floor at **y=2.0** and asserting the corpse
lands there (never y=0, never floating).

## Archetypes (crowd variety)
`npcArchetype(id)` → `{ modelFile, scaleMul, tint, label }`. Six visually distinct
citizens mix three shared rigged humanoid bodies (`marcus_webb_anim`, `AnnaCasual_anim`,
`chief_martinez_anim` — each carrying the shared Idle/Walk/Run(/Talk) clip set baked via
`tools/retarget_from_jake.py`) with tint + size variety. Add more by dropping a rigged
`*_anim.glb` in `assets/rigged_glb` and extending `kArchetypes`.

## Performance
GPU compute-skinning is enabled per body when the device supports it (the crowd-scalable
path — palette upload, no per-frame CPU vertex re-upload). A headless / non-compute
device falls back to CPU linear-blend-skinning transparently, so the self-test runs with
no window/Vulkan. `despawn()` hands the GPU skinned-mesh registration back
(`disableGpuSkinning`) so a churning crowd never leaks device buffers.

## Fold notes (for the branch merge)
- New files only: `app/npc_character.{h,cpp}` + this doc + the CMake/CLI/registry wiring
  for `--test-npcchar`. No monster/rescue/crowd behavior code was touched.
- `NpcCrowd` is intentionally behavior-free (just a pool + batch update/draw/despawn).
  The living-city scheduler holds an `NpcCrowd`, decides positions/states, and calls the
  interface above. If the behavior branch already has a crowd owner, it can hold
  `NpcCharacter` bodies directly and drop `NpcCrowd`.
- Gate: `--test-npcchar` (rig loads, anim states switch, ragdoll triggers + settles on a
  non-zero floor, spawn/despawn no leak, crowd ticks clean). Existing `--test-rescue`,
  `--test-crowd`, `--test-deathragdoll`, `--test-anim` stay green.
