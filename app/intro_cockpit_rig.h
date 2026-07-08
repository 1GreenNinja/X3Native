#pragma once
// intro_cockpit_rig — the intro cold-open's VISIBLE LAYER, shared between the
// --world introcockpit showcase host and the intro orchestrator's interactive
// window beats (intro_orchestrator.cpp): the two-seat fighter cockpit GLB stood
// up as Scene entities (PBR route + emissiveTex content screens + alpha-blended
// canopy glass) that can be POSED to the pilot camera each frame so the player
// flies the dodge/dogfight beats from INSIDE the cockpit.
//
// Built by app/world_hosts/host_introcockpit.cpp (which also owns the
// --test-introcockpit gate). See that TU for the routing recipes + the engine
// findings (scene-copy / alphaBlend) this design encodes.

#include "scene.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace x3 { namespace apphost {

// Everything the cockpit scene owns. The loader + Model own the GPU handles, so
// the rig must outlive every frame that draws the entities (the showcase host
// keeps it on the stack for the loop; the orchestrator owns one per intro run).
struct IntroCockpitRig {
    std::unique_ptr<x3::asset::IAssetSource> assets;
    std::unique_ptr<x3::asset::IModelLoader> loader;
    x3::asset::Model                         model;
    x3::game::Scene                          scene;
    x3::rhi::TextureHandle                   mrShared;   // 1x1 MR (forces PBR route)
    x3::rhi::TextureHandle                   mrGlassy;   // 1x1 polished MR (canopy panes)
    // Pose bookkeeping: per-entity Scene id + the BASE (cockpit-local) transform,
    // so poseIntroCockpit can recompute world = shipPose * base each frame.
    std::vector<uint32_t>                entityIds;
    std::vector<std::array<float, 16>>   baseXf;
    // Gate diagnostics.
    uint32_t drawables = 0, entities = 0, glassPanes = 0, screens = 0;

    void shutdown(x3::rhi::IRenderDevice& device) {
        if (mrShared.valid()) device.destroyTexture(mrShared);
        if (mrGlassy.valid()) device.destroyTexture(mrGlassy);
        if (model.ok && loader) loader->unload(model);
    }
};

// Build the cockpit Scene from assets/converted_glb/Cockpit/fighter_cockpit.glb.
// includeBackdrop=false skips the GLB's baked far-field panels (milky-way plane +
// planet-horizon strip, |translation| > 30 m) — the orchestrator wants the
// engine's analytic-sky starfield as the world-fixed backdrop instead, so the
// view doesn't pitch with the ship. Returns false if the GLB is missing.
bool buildIntroCockpitRig(IntroCockpitRig& rig, x3::rhi::IRenderDevice& device,
                          bool includeBackdrop = true);

// Deep-space look: analytic-sky starfield + cool ambient + IBL probe + the
// cockpit-scale interior light rig (host_space's recipe, cockpit-sized).
void setIntroCockpitLook(x3::rhi::IRenderDevice& device);

// Lock the cockpit to the pilot camera (classic first-person cockpit): recompute
// every entity's world transform as shipPose * base, where shipPose places the
// cockpit's pilot-eye point at (cx,cy,cz) facing the camera's (yaw,pitch) per
// docs/CONVENTIONS.md §3. Call each frame before scene.render().
void poseIntroCockpit(IntroCockpitRig& rig,
                      float cx, float cy, float cz, float yaw, float pitch);

}} // namespace x3::apphost
