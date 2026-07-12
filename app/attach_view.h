#pragma once
// ============================================================================
// ATTACHMENT PARTS — the visible hardware bolted onto the gun.
//
// GENERATE, DON'T HAND-CARVE (DECISIONS.md): these are small, clean PROCEDURAL
// parts (a can, a brake, a scope tube, a mag box, a laser, a grip, an energy
// cell), built once and drawn on the FP viewmodel — and on the third-person
// held weapon, which costs nothing extra because it is the same mesh at a
// different matrix.
//
// MOUNTING. Every part rides the MEASURED per-weapon barrel tip (WeaponDef::
// vmMuzzle, 346f5e7) through attachMountLocal() — expressed as a FRACTION of that
// gun's own barrel run — so an 0.9 m pistol and a 4.4 m shotgun both mount
// correctly with no per-weapon constants. The world transform is composed from
// Arsenal::currentViewmodelFrame(), the ONE source of the viewmodel's world frame,
// so a part cannot drift off the gun: it is on the gun by construction.
//
// HONEST LIGHTING. Parts carry real PBR values from the item data (albedo well
// under white, real metallic/roughness via a 1x1 MR texel — landmines L4/L5). The
// ONLY things that emit are small lit CORES: the laser's diode and the energy
// cell's window. No fake self-emissive, no over-unity albedo.
// ============================================================================

#include "attachments.h"
#include "scene.h"
#include "weapon.h"

#include "engine/rhi/IRenderDevice.h"

#include <vector>

namespace x3::game {

class AttachView {
public:
    // Build the procedural part meshes + their 1x1 MR texels. Call once, after the
    // device is up (inside the upload batch, like every other build).
    void init(x3::rhi::IRenderDevice& device);
    // Free every GPU resource init() created (the smoketest gates allocationCount==0).
    void shutdown(x3::rhi::IRenderDevice& device);
    bool built() const { return m_built; }

    // Draw the CURRENT weapon's fitted parts on the FIRST-PERSON viewmodel. Same
    // camera args as Arsenal::drawCurrentViewmodel (pass the host's live vm_* cvar
    // DELTAS + any ADS offsets so the parts follow a scoped/nudged gun exactly).
    void drawFirstPerson(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                         const Arsenal& arsenal,
                         float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
                         float extraYawOff = 0.0f, float extraPitchOff = 0.0f,
                         float extraRollOff = 0.0f, float extraFwd = 0.0f,
                         float extraRight = 0.0f, float extraDown = 0.0f) const;

    // Draw the current weapon's fitted parts at an arbitrary world matrix — the
    // THIRD-PERSON hand-socket matrix ThirdPersonView already builds for the held
    // weapon (same drawables, same scale), so the mods show in 3P for free.
    void drawAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                const Arsenal& arsenal, const float model[16], float modelScale) const;

private:
    struct Part {
        x3::rhi::MeshHandle  mesh{};
        x3::rhi::TextureHandle mr{};   // 1x1 metallic-roughness texel (glTF packing: G=rough, B=metal)
    };
    // Draw one fitted attachment given the composed viewmodel basis.
    void drawSpec(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                  const WeaponDef& def, const AttachSpec& a,
                  const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                  const x3::phys::Vec3& origin, float scale) const;

    bool m_built = false;
    // One mesh per AttachPart shape (index by (int)AttachPart). Unit-ish geometry in
    // a local frame with +Z down-barrel, +Y up — the same convention as vmMuzzle.
    Part m_parts[16];
    // Small extra pieces the composites need.
    x3::rhi::MeshHandle m_lensGlass{};   // the optic's dark glass disc
    x3::rhi::MeshHandle m_core{};        // the laser diode / energy-cell lit core
    x3::rhi::TextureHandle m_mrGlass{};
    x3::rhi::TextureHandle m_white{};   // 1x1 white albedo (the PBR route needs a real one)
    std::vector<x3::rhi::MeshHandle>    m_meshes;    // everything init() made (shutdown frees)
    std::vector<x3::rhi::TextureHandle> m_textures;
};

} // namespace x3::game
