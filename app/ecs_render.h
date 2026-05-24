#pragma once
// ECS -> GPU-driven render feed (the 10k-entity CPU->GPU play). The ECS holds
// entity data in packed component arrays; this iterates the renderable ones and
// issues device.drawMesh per entity. The device's EXISTING multidraw-indirect
// path (VulkanRenderDevice endFrame) then groups identical meshes into ONE
// vkCmdDrawIndexedIndirect per distinct mesh with all instances in an SSBO — so
// 10k entities sharing a few meshes collapse to a few GPU draws. The ECS just
// feeds compact instance data; the GPU does the heavy lifting.
//
// Game/slice code: bridges engine/ecs (World) + engine/rhi (IRenderDevice).
#include "engine/ecs/Ecs.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::game {

// ---- Render-feed components (cache-local POD in the ECS packed arrays). ----
struct EcsTransform  { float world[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }; // column-major
struct EcsRenderable { uint32_t meshId = 0; uint32_t texId = 0; float color[4] = {1,1,1,1}; };
struct EcsVelocity   { float v[3] = {0,0,0}; };

// Draw every entity that has Transform + Renderable via device.drawMesh (which the
// device batches into multidraw-indirect). Call between beginFrame/endFrame.
// Returns the number of draws issued (= matched entity count).
uint32_t renderEcs(x3::ecs::World& world, x3::rhi::IRenderDevice& device,
                   const x3::rhi::FrameContext& frame);

// Integrate Transform translation by Velocity*dt for every matching entity — a
// data-oriented movement system sweeping the packed arrays. Returns matched count.
uint32_t integrateEcs(x3::ecs::World& world, float dt);

// Headless self-test (--test-ecsrender): 10k renderables across a couple meshes
// feed the render path (draw count == matched entities), the movement system
// integrates only the moving subset, and destroying entities shrinks the feed.
// Asserts R0-R3. No window / Vulkan.
bool runEcsRenderSelfTest();

} // namespace x3::game
