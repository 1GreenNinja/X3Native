// Cooperative ally NPCs — Phase A (build / asset load / draw). See app/ally.h.
//
// Phase A scope: load the 3-ally squad's character GLBs + the weapon GLBs they
// equip, fall back to per-slot tinted procedural boxes if a GLB is missing,
// place the squad in a small fan behind the player spawn, and render each
// ally + its weapon at a fixed third-person hand offset. NO AI in Phase A —
// the squad stands at attention.  The state machine, fireOnce, and the
// per-ally cooldowns live in app/ally_ai.cpp (Phase B), and makeBenchArena
// + the --bench-combat CLI live in app/ally_arena.cpp + main.cpp (Phase C).
//
// Patterns mirrored from app/monster.cpp:
//   * mountDir + createModelLoader + load -> fallback box on miss.
//   * Entity's render mesh left INVALID (so Scene::render skips it) — this
//     system owns the multi-primitive draw, same as MonsterSystem.
//   * composeTRS for column-major 4x4 from a yaw basis + uniform scale + pos.
//   * Tinted procedural-box fallback so the squad is always visible even on
//     a clean checkout with no GLBs (per-slot tints differ so the player can
//     tell them apart).
//
// Cross-phase dependencies:
//   * Tag::Ally and Layer::Ally are added by Phase C (scene.h + IPhysicsWorld.h).
//     Until that merge lands, this file references them by their post-merge
//     symbolic names; compile fails cleanly if Phase C hasn't landed yet,
//     which is the right behaviour — these phases compose, they don't ship
//     in isolation.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice +
// IPhysicsWorld + Scene interfaces only. No third-party AI/combat source
// consulted.

#include "ally.h"
#include "faction.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Local tuning (file-private). The numbers in ally.h are PUBLIC tuning that
// other code reads (engagement ranges, take-cover threshold). The constants
// here are PRIVATE to the build/draw paths (asset paths, formation offsets,
// fallback-box sizing).
// ---------------------------------------------------------------------------
namespace {

// Real character GLBs (when loaded) are authored around human proportions
// — a touch under 2 m tall. Match the player's hitbox-ish scale.
constexpr float kRealAllyScale  = 1.0f;
// Fallback box dimensions: roughly human-sized so allies can't hide in the
// floor when their GLB is missing.
constexpr float kBoxHalfX       = 0.40f;
constexpr float kBoxHalfY       = 0.95f;   // ~1.9 m tall
constexpr float kBoxHalfZ       = 0.40f;
constexpr float kBoxScale       = 1.0f;

// Per-slot fallback tints (rgb) so the procedural-box squad reads as 3
// distinct people. Pulled from the EFLZ palette: warm cyan / amber / magenta.
constexpr float kSarahFallbackTint[3]      = {0.55f, 0.85f, 1.00f};   // canon cool-cyan
constexpr float kMartinezFallbackTint[3]   = {0.95f, 0.65f, 0.30f};   // warm amber
constexpr float kAnnaFallbackTint[3]       = {1.00f, 0.45f, 0.85f};   // signal magenta

// Formation: a small fan BEHIND the player spawn so the squad is visible from
// a forward-facing first-person camera but doesn't block the player's view of
// the level. +X is "right of facing", -Z is "behind facing" in level1's
// coords; the player's spawn convention is forward = -Z (see env_art.cpp).
struct AllyFormationOffset { float x; float y; float z; };
constexpr AllyFormationOffset kFormationOffset[(uint32_t)AllyKind::Count] = {
    { -1.5f, 0.0f, -2.0f },   // Sarah: back-left
    {  0.0f, 0.0f, -2.5f },   // Martinez: center-back
    { +1.5f, 0.0f, -2.0f },   // AnnaBodySuit: back-right
};

// Third-person weapon offset relative to the ally's local frame. Right-hand
// height + forward of the chest. Authored to look natural at idle; a proper
// hand-bone attachment is Phase B+ work (would consult the GLB's skinning).
constexpr float kWeaponLocalRight   = +0.35f;
constexpr float kWeaponLocalUp      = +0.95f;   // mid-torso height
constexpr float kWeaponLocalForward = +0.20f;
constexpr float kWeaponScale        = 0.6f;     // weapons render player-viewmodel-size; scale down for third-person

// Compose a column-major 4x4 from a planar yaw + uniform scale + translation.
// Matches monster.cpp::composeTRS exactly so per-frame transform writebacks
// from the Phase-B state machine produce the same row layout the renderer
// expects. col0=(c,0,-s)*s_, col1=(0,1,0)*s_, col2=(s,0,c)*s_.
void composeYawTRS(float m[16], float yaw, float s, float x, float y, float z) {
    const float c = std::cos(yaw);
    const float sy = std::sin(yaw);
    m[0]  =  c * s; m[1]  = 0.0f; m[2]  = -sy * s; m[3]  = 0.0f;
    m[4]  = 0.0f;   m[5]  = s;    m[6]  = 0.0f;    m[7]  = 0.0f;
    m[8]  = sy * s; m[9]  = 0.0f; m[10] =  c * s;  m[11] = 0.0f;
    m[12] = x;      m[13] = y;    m[14] = z;       m[15] = 1.0f;
}

// Multiply a 4x4 (column-major) by a local-space translation (right, up,
// fwd) to get the world-space anchor for an attachment (the weapon). The
// ally's transform encodes its facing; the weapon sits a fixed offset in
// the ally's LOCAL frame.
void localToWorld(const float allyTransform[16],
                  float lx, float ly, float lz,
                  float& outX, float& outY, float& outZ) {
    // outV = M * (lx,ly,lz,1)
    outX = allyTransform[0]*lx + allyTransform[4]*ly + allyTransform[8] *lz + allyTransform[12];
    outY = allyTransform[1]*lx + allyTransform[5]*ly + allyTransform[9] *lz + allyTransform[13];
    outZ = allyTransform[2]*lx + allyTransform[6]*ly + allyTransform[10]*lz + allyTransform[14];
}

} // namespace

// ---------------------------------------------------------------------------
// Public name + GLB lookup tables.
// ---------------------------------------------------------------------------
const char* allyKindName(AllyKind k) {
    switch (k) {
    case AllyKind::Sarah:        return "Sarah";
    case AllyKind::Martinez:     return "Martinez";
    case AllyKind::AnnaBodySuit: return "AnnaBodySuit";
    case AllyKind::Count:        return "?";
    }
    return "?";
}

const char* allyKindGlb(AllyKind k) {
    switch (k) {
    case AllyKind::Sarah:        return "Sarah.glb";
    case AllyKind::Martinez:     return "chief_martinez_anim.glb";
    case AllyKind::AnnaBodySuit: return "AnnaBodySuit.glb";
    case AllyKind::Count:        return nullptr;
    }
    return nullptr;
}

const char* allyWeaponName(AllyWeapon w) {
    switch (w) {
    case AllyWeapon::None:        return "None";
    case AllyWeapon::Pistol:      return "Pistol";
    case AllyWeapon::SMG:         return "SMG";
    case AllyWeapon::Shotgun:     return "Shotgun";
    case AllyWeapon::Plasma:      return "Plasma";
    case AllyWeapon::Chaingun:    return "Chaingun";
    case AllyWeapon::PlasmaRifle: return "PlasmaRifle";
    case AllyWeapon::Lightning:   return "Lightning";
    case AllyWeapon::Count:       return "?";
    }
    return "?";
}

const char* allyWeaponGlb(AllyWeapon w) {
    // Names match the player's existing weapon GLBs (per ASSET_INVENTORY.md +
    // weapon.h's modelDir convention). None -> empty so the load path skips.
    switch (w) {
    case AllyWeapon::None:        return "";
    case AllyWeapon::Pistol:      return "WeaponEnergyPistol.glb";
    case AllyWeapon::SMG:         return "WeaponSMG.glb";
    case AllyWeapon::Shotgun:     return "WeaponShotgun.glb";
    case AllyWeapon::Plasma:      return "WeaponPlasmaLauncher.glb";
    case AllyWeapon::Chaingun:    return "WeaponChaingun.glb";
    case AllyWeapon::PlasmaRifle: return "WeaponPlasmaRifle.glb";
    case AllyWeapon::Lightning:   return "WeaponLightning.glb";
    case AllyWeapon::Count:       return "";
    }
    return "";
}

const char* allyStateName(AllyState s) {
    switch (s) {
    case AllyState::Follow:     return "Follow";
    case AllyState::Engage:     return "Engage";
    case AllyState::Reposition: return "Reposition";
    case AllyState::Reload:     return "Reload";
    case AllyState::TakeCover:  return "TakeCover";
    case AllyState::Search:     return "Search";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// loadKindAsset: load one AllyKind GLB -> m_kindAssets[(uint32_t)k]. Returns
// true on real load, false (with a tinted procedural-box fallback installed)
// otherwise. The fallback box keeps the squad visible on a clean checkout
// without the purchased character GLBs.
// ---------------------------------------------------------------------------
bool AllyManager::loadKindAsset(x3::rhi::IRenderDevice& device, AllyKind k) {
    AllyAsset& slot = m_kindAssets[(uint32_t)k];
    if (slot.ok) return true;   // already loaded

    // Per-slot fallback tint -- copied into the slot up front; the real GLB
    // path will leave this as a no-op tint if it loads ok (drawables carry
    // their own baseColorFactor in that case).
    const float* tint = kSarahFallbackTint;
    if (k == AllyKind::Martinez)     tint = kMartinezFallbackTint;
    if (k == AllyKind::AnnaBodySuit) tint = kAnnaFallbackTint;
    slot.tint[0] = tint[0]; slot.tint[1] = tint[1]; slot.tint[2] = tint[2];

    // Try the real GLB. m_modelAssets is mounted by build() before this is
    // called, so a missing source here is a build() bug, not a content issue.
    if (m_loader) {
        const char* file = allyKindGlb(k);
        if (file) {
            slot.model = m_loader->load(file);
            if (slot.model.ok) {
                slot.drawables = x3::asset::makeDrawables(slot.model);
                if (!slot.drawables.empty()) {
                    slot.ok = true;
                    x3::logInfo(std::string("[ally] loaded ") + file + " -> " +
                                std::to_string(slot.drawables.size()) +
                                " drawable primitive(s) for " + allyKindName(k));
                    return true;
                }
                x3::logWarn(std::string("[ally] ") + file +
                            " loaded but produced no drawables; using tinted fallback box");
            } else {
                x3::logWarn(std::string("[ally] ") + file +
                            " load failed; using tinted fallback box for " + allyKindName(k));
            }
        }
    }

    // ---- Fallback: tinted procedural box, human-shaped. -----------------
    x3::prims::PrimMesh geo = x3::prims::makeBox(kBoxHalfX, kBoxHalfY, kBoxHalfZ,
                                                  tint[0], tint[1], tint[2], 1.0f);
    x3::rhi::MeshHandle mesh = device.createMesh(
        geo.verts.data(), (uint32_t)geo.verts.size(),
        geo.index.data(), (uint32_t)geo.index.size());

    x3::asset::ModelDrawable d;
    d.meshId               = mesh.id;
    d.baseColorTexId       = 0;            // default white -> flat color
    d.baseColorFactor[0]   = tint[0];
    d.baseColorFactor[1]   = tint[1];
    d.baseColorFactor[2]   = tint[2];
    d.baseColorFactor[3]   = 1.0f;
    slot.drawables.push_back(d);
    slot.ok = false;                       // signals "fallback in use" (draw still works)
    return false;
}

// ---------------------------------------------------------------------------
// loadWeaponAsset: load one AllyWeapon GLB -> m_weaponAssets[(uint32_t)w].
// Multiple allies sharing a weapon share the GPU upload. None / load-failure
// leaves slot.drawables empty -> drawn weapon is silently skipped (the ally
// still fires per the AI rules; the visible weapon is cosmetic).
// ---------------------------------------------------------------------------
bool AllyManager::loadWeaponAsset(x3::rhi::IRenderDevice& device, AllyWeapon w) {
    if (w == AllyWeapon::None || w == AllyWeapon::Count) return false;
    AllyAsset& slot = m_weaponAssets[(uint32_t)w];
    if (slot.ok || !slot.drawables.empty()) return slot.ok;

    if (m_loader) {
        const char* file = allyWeaponGlb(w);
        if (file && file[0]) {
            slot.model = m_loader->load(file);
            if (slot.model.ok) {
                slot.drawables = x3::asset::makeDrawables(slot.model);
                if (!slot.drawables.empty()) {
                    slot.ok = true;
                    x3::logInfo(std::string("[ally] loaded weapon ") + file + " -> " +
                                std::to_string(slot.drawables.size()) +
                                " drawable primitive(s)");
                    return true;
                }
            } else {
                x3::logWarn(std::string("[ally] weapon ") + file + " load failed; allies will fire invisibly");
            }
        }
    }

    // No fallback box for weapons -- a missing weapon model is purely visual
    // and the AI fire path is unaffected. (Unlike a missing ally body, which
    // would leave the squad invisible and break the feature read.)
    (void)device;
    return false;
}

// ---------------------------------------------------------------------------
// build: stand the 3-ally squad up in formation behind `spawnPos`. Loads
// each AllyKind GLB + the default loadout's weapons; falls back gracefully
// for any miss. Creates a scene Entity (Tag::Ally) + kinematic capsule per
// ally so hostiles can rayCast(Layer::Ally) and damage them.
// ---------------------------------------------------------------------------
void AllyManager::build(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics,
                        std::string_view modelDir,
                        std::string_view weaponDir,
                        const x3::phys::Vec3& spawnPos) {
    // ---- Mount the two asset sources + create one shared loader. ---------
    m_modelAssets.reset(x3::asset::createAssetSource());
    if (!m_modelAssets->mountDir(std::string(modelDir), 0)) {
        x3::logWarn(std::string("[ally] mountDir failed (characters): ") + std::string(modelDir));
    }
    m_weaponAssetSrc.reset(x3::asset::createAssetSource());
    if (!m_weaponAssetSrc->mountDir(std::string(weaponDir), 1)) {
        x3::logWarn(std::string("[ally] mountDir failed (weapons): ") + std::string(weaponDir));
    }
    // One loader -- mount BOTH sources via a chained-source pattern. The
    // existing asset source supports multiple mountDir calls on one instance;
    // chain by mounting weaponDir into the model source at lower priority.
    // (Keeps a single IModelLoader so the m_loader->load() lookups in
    // loadKindAsset / loadWeaponAsset both go through it.)
    if (m_modelAssets) {
        m_modelAssets->mountDir(std::string(weaponDir), 10);
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_modelAssets.get()));

    // ---- Load the 3 ally character GLBs. ---------------------------------
    for (uint32_t ki = 0; ki < (uint32_t)AllyKind::Count; ++ki) {
        loadKindAsset(device, (AllyKind)ki);
    }

    // ---- Default loadout: Sarah=Pistol, Martinez=SMG, Anna=Shotgun. ------
    const AllyWeapon kDefaultLoadout[(uint32_t)AllyKind::Count] = {
        AllyWeapon::Pistol,
        AllyWeapon::SMG,
        AllyWeapon::Shotgun,
    };
    for (uint32_t i = 0; i < (uint32_t)AllyKind::Count; ++i) {
        loadWeaponAsset(device, kDefaultLoadout[i]);
    }

    // ---- Spawn 3 allies into formation behind spawnPos. ------------------
    m_allies.clear();
    m_allies.reserve((uint32_t)AllyKind::Count);
    for (uint32_t i = 0; i < (uint32_t)AllyKind::Count; ++i) {
        AllyInstance a{};
        a.kind   = (AllyKind)i;
        a.weapon = kDefaultLoadout[i];
        a.state  = AllyState::Follow;
        a.hp     = kAllyHp;
        a.alive  = true;
        a.magRemaining    = kAllyMagSize;
        a.repositionTimer = kAllyRepositionPeriod;

        const AllyFormationOffset off = kFormationOffset[i];
        const float ax = spawnPos.x + off.x;
        const float ay = spawnPos.y + off.y;
        const float az = spawnPos.z + off.z;

        // Face FORWARD initially (same heading the player typically spawns
        // facing). Phase B's state machine slews this toward the target.
        a.yaw = 0.0f;
        a.yawTarget = 0.0f;
        composeYawTRS(a.transform, a.yaw, kRealAllyScale, ax, ay, az);

        // Kinematic-ish hitbox: Static mass-0 in the Ally layer so hostiles'
        // raycasts can hit it and the AI moves it via setBodyPosition (same
        // teleport pattern MonsterSystem uses).
        const x3::phys::Vec3 half{kBoxHalfX, kBoxHalfY, kBoxHalfZ};
        const x3::phys::Vec3 center{ax, ay + kBoxHalfY, az}; // origin at feet -> raise box center
        a.bodyId = physics.addBox(half, center, 0.0f, x3::phys::Layer::Ally).id;

        // Scene entity (Tag::Ally). Render mesh left INVALID so Scene::render
        // skips it -- this system owns the multi-primitive draw, mirroring
        // the MonsterSystem pattern.
        Entity e{};
        e.tag = Tag::Ally;
        std::memcpy(e.transform, a.transform, sizeof(e.transform));
        e.meshId = 0;   // invalid -> skipped by Scene::render
        a.entityId = scene.addEntity(e);

        m_allies.push_back(a);
        x3::logInfo(std::string("[ally] spawned ") + allyKindName(a.kind) +
                    " weapon=" + allyWeaponName(a.weapon) +
                    " hp=" + std::to_string(a.hp) +
                    " pos=(" + std::to_string(ax) + "," +
                    std::to_string(ay) + "," + std::to_string(az) + ")");
    }
    x3::logInfo("[ally] squad built: " + std::to_string(m_allies.size()) + " allies");
}

// ---------------------------------------------------------------------------
// draw: for each live ally, draw its character GLB (or fallback box) at the
// ally's current transform, then draw its equipped weapon at a fixed hand
// offset in the ally's local frame. Dead allies are skipped (Phase B's
// resolveHit clears `alive` and hides the entity at the moment of death).
// ---------------------------------------------------------------------------
void AllyManager::draw(x3::rhi::IRenderDevice& device,
                       const x3::rhi::FrameContext& frame) const {
    if (m_allies.empty()) return;

    for (const AllyInstance& a : m_allies) {
        if (!a.alive) continue;

        // ---- Character body ----
        const AllyAsset& kind = m_kindAssets[(uint32_t)a.kind];
        for (const x3::asset::ModelDrawable& d : kind.drawables) {
            float model[16];
            std::memcpy(model, a.transform, sizeof(model));
            device.drawMesh(frame, d.meshId, d.baseColorTexId,
                            d.baseColorFactor, model);
        }

        // ---- Equipped weapon (third-person, hand offset). -----------------
        if (a.weapon == AllyWeapon::None) continue;
        const AllyAsset& wpn = m_weaponAssets[(uint32_t)a.weapon];
        if (wpn.drawables.empty()) continue;   // weapon GLB missing -> skip cosmetic draw

        // Compose the weapon's world transform from the ally's transform + the
        // local-frame offset. The ally transform encodes (yaw + uniform scale +
        // position); we rebuild a scaled-down weapon transform anchored at the
        // hand offset, sharing the ally's yaw so the weapon points the same
        // direction the ally faces.
        float anchorX, anchorY, anchorZ;
        localToWorld(a.transform,
                     kWeaponLocalRight, kWeaponLocalUp, kWeaponLocalForward,
                     anchorX, anchorY, anchorZ);

        float wm[16];
        composeYawTRS(wm, a.yaw, kWeaponScale, anchorX, anchorY, anchorZ);

        for (const x3::asset::ModelDrawable& d : wpn.drawables) {
            device.drawMesh(frame, d.meshId, d.baseColorTexId,
                            d.baseColorFactor, wm);
        }
    }
}

} // namespace x3::game
