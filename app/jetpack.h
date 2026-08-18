#pragma once
// THE JETPACK — the `fly` command's visible pack + thrust FX (W-JETPACK).
//
// Owner: "we need a fly command.. that spawns a jetpack... that flies at
// 300MPH.. so jake can get over the whole world quickly to observe."
//
// Split of responsibilities (each half lives with its own kind):
//   * flight physics  = Player::setJetpack / the jetpack block in player.cpp
//     (collision on, CONTACT LAW landing, 134.1 m/s clamp);
//   * the pose        = AnimatedCharacter::setJetpack (the shared module owns
//     every clip decision — no host re-wires a rig state by hand);
//   * THIS FILE       = what you SEE: the pack on his back and the thrust
//     plume. Visuals only — it moves nothing.
//
// THE PACK IS ARMORY PIECES, NOT PROCEDURAL SLOP (NO_SLOP rule 3): the 914
// package catalog has no dedicated jetpack (searched: jetpack / thruster /
// backpack / booster — nozzles exist but untextured gray), so the unit is
// composed from the two TEXTURED sci-fi kit props already store-served in
// this repo:
//   * tanks:   SciFiKit3/Big_Oxygen_Tank_01.glb  (Wall_Atlas ribbed drum)
//   * housing: SciFi_Warehouse_Kit/Duct Vent.glb (textured duct/vent box —
//              the flat backplate AND, scaled small, the two down-thrusters)
// Store-served GLBs — no new asset bytes enter git (ENGINE_GOTCHAS 2.5).
//
// Attachment: the module's boneWorld("mixamorigSpine2") — the same socket
// pattern the held rifle uses with the hand bone (host_tunnel
// heldRifleWorld). The pack rides the SPINE bone matrix, so when the flight
// pose leans, the pack leans with the back instead of floating upright.
//
// Thrust FX: submitParticles billboards (NO_SLOP rule 1 — the engine's
// particle path, not cubes), ADDITIVE with a glow floor (X3_WORLD_RULES
// rule 5: additive VFX only ever add light) + a thin ALPHA heat-haze tail.

#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "engine/rhi/IRenderDevice.h"

#include <memory>
#include <string>
#include <vector>

namespace x3::game {

class JetpackRig {
public:
    // Load the two textured pieces from assets/converted_glb. Lazy — the
    // host calls this on the first `fly`, the way Jake himself loads on the
    // first E-exit. Returns true if BOTH pieces loaded (a half-pack is worse
    // than none — NO_SLOP rule 3: hold, don't ship a stand-in).
    bool load(x3::rhi::IRenderDevice& device, const std::string& convertedGlbRoot);
    bool loaded() const { return m_loaded; }

    // Draw the pack mounted on the wearer's upper back. spineWorld is the
    // column-major world matrix of the spine bone under the SAME transform
    // the character draws with (AnimatedCharacter::boneWorld). Also caches
    // the two nozzle mouths + the plume direction for submitThrustFx.
    void draw(const x3::rhi::FrameContext& frame, x3::rhi::IRenderDevice& device,
              const float spineWorld[16]);

    // Advance + submit the thrust FX. thrust is 0..1 (0 = cold pack, no
    // spawn), vel is the wearer's world velocity (the plume inherits a share
    // so it trails honestly at speed). Call once per frame INSIDE a valid
    // frame (particle batches are cleared by beginFrame). Safe to call with
    // thrust 0 — live puffs still age out.
    void submitThrustFx(x3::rhi::IRenderDevice& device, float dt,
                        float thrust, const float vel[3]);

    // True while any FX puffs are alive (lets a host keep submitting after
    // thrust cuts so the plume dies out instead of vanishing).
    bool fxAlive() const { return !m_puffs.empty(); }

private:
    struct Piece {
        std::unique_ptr<x3::asset::IAssetSource> src;
        std::unique_ptr<x3::asset::IModelLoader> loader;
        x3::asset::Model                         model;
        std::vector<x3::asset::ModelDrawable>    draw;
        bool ok = false;
    };
    bool loadPiece(x3::rhi::IRenderDevice& device, const std::string& dir,
                   const std::string& file, Piece& out);
    void drawPiece(const x3::rhi::FrameContext& frame,
                   x3::rhi::IRenderDevice& device, const Piece& piece,
                   const float world[16]);

    Piece m_tank;     // Big_Oxygen_Tank_01 (drawn twice: left/right tank)
    Piece m_vent;     // Duct Vent (backplate + the two nozzles)
    bool  m_loaded = false;

    // FX pool — deterministic LCG jitter (the precip_fx discipline, no rand).
    struct Puff {
        float x, y, z, vx, vy, vz;
        float age, life, size0, size1;
        int   kind;   // 0 = additive core, 1 = alpha haze
    };
    std::vector<Puff> m_puffs;
    float    m_spawnAcc = 0.0f;
    uint32_t m_seed = 0x9E3779B9u;

    // Nozzle mouths + plume direction, cached by draw() each frame.
    float m_nozzle[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
    float m_plumeDir[3]  = { 0, -1, 0 };
    bool  m_haveNozzles  = false;
};

} // namespace x3::game
