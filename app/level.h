#pragma once
// Procedural graybox test level (S2). Phase-1 authoring per docs/LEVEL_GEOMETRY.md:
// meshes generated in code, the same triangulated output a CSG tool would emit,
// feeding both the renderer and Jolt static collision.
//
// buildTestLevel populates the Scene with: floor, 4 walls (one with a doorway
// gap), and a low step/ledge. Each piece gets a graybox texture (distinct color
// per surface), a static collision mesh, and an Entity.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

namespace x3::game {

// Build the graybox room into `scene`. Creates GPU meshes/textures via `device`
// and registers static collision via `physics`. Resources are owned by the
// device/physics world (the app tears them down on shutdown). Idempotent only
// if called once on a fresh scene.
void buildTestLevel(Scene& scene,
                    x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics);

} // namespace x3::game
