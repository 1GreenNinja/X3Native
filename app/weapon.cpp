// Weapon pickup + first-person viewmodel (S5). See app/weapon.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice +
// IPhysicsWorld + Scene interfaces only. No purchased C# copied.
#include "weapon.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Tuning constants.
// ---------------------------------------------------------------------------
namespace {
constexpr float kPi = 3.14159265358979f;

// Pickup animation: gentle vertical bob + slow yaw spin.
constexpr float kBobAmplitude = 0.12f;   // m, peak vertical excursion
constexpr float kBobHz        = 0.7f;    // bobs per second
constexpr float kSpinRate     = 1.2f;    // rad/s yaw

// ---------------------------------------------------------------------------
// Viewmodel placement + ORIENTATION OFFSET — now LIVE-TUNABLE via console cvars.
// ---------------------------------------------------------------------------
// The pose is supplied per-frame by the caller (see drawViewmodel below), driven
// by the cvars vm_fwd/vm_right/vm_down (meters) and vm_yaw/vm_pitch/vm_roll
// (radians, converted from the console's degrees). The baked defaults live in
// weapon.h (kVmDef*). The orientation offsets are applied about the CAMERA basis
// (yaw about up, pitch about right, roll about forward) so the mismapped pistol
// GLB barrel can be dialed onto the look direction in-game. VISUAL ONLY — no
// gameplay effect (the fire ray uses the camera look dir, not the viewmodel).

// The purchased pistol GLB is authored at a world scale that is too large for a
// viewmodel; scale it down so it reads as a held pistol. The fallback box is
// authored small already, so it uses scale 1.
constexpr float kRealModelScale = 0.18f;
constexpr float kBoxModelScale  = 1.0f;

// Pickup pedestal size for the fallback procedural "weapon" box.
constexpr float kBoxHalf = 0.18f;

// Build a column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s,
// and translation t. Column-major layout: m[0..3]=col0, m[4..7]=col1, etc.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

// Rotate vector v about unit axis k by angle (radians) — Rodrigues' formula.
// Used to apply the viewmodel orientation offsets about the camera basis axes.
x3::phys::Vec3 rotateAboutAxis(const x3::phys::Vec3& v, const x3::phys::Vec3& k, float angle) {
    if (angle == 0.0f) return v;
    const float c = std::cos(angle), s = std::sin(angle);
    // cross(k, v)
    const x3::phys::Vec3 kxv{
        k.y * v.z - k.z * v.y,
        k.z * v.x - k.x * v.z,
        k.x * v.y - k.y * v.x };
    const float kdotv = k.x * v.x + k.y * v.y + k.z * v.z;
    return x3::phys::Vec3{
        v.x * c + kxv.x * s + k.x * kdotv * (1.0f - c),
        v.y * c + kxv.y * s + k.y * kdotv * (1.0f - c),
        v.z * c + kxv.z * s + k.z * kdotv * (1.0f - c) };
}

} // namespace

// ---------------------------------------------------------------------------
// Pure arming rule (testable; no rendering / physics).
// ---------------------------------------------------------------------------
bool shouldArm(const x3::phys::Vec3& playerPos, const x3::phys::Vec3& pickupPos,
               float radius, bool alreadyArmed) {
    if (alreadyArmed) return false;            // latch: never re-arm
    const float dx = playerPos.x - pickupPos.x;
    const float dz = playerPos.z - pickupPos.z;
    return (dx * dx + dz * dz) <= radius * radius;
}

// ---------------------------------------------------------------------------
// Build the pickup (load the real GLB, or fall back to a box).
// ---------------------------------------------------------------------------
void WeaponSystem::buildWeaponPickup(Scene& scene, x3::rhi::IRenderDevice& device,
                                     std::string_view modelDir,
                                     const x3::phys::Vec3& pickupPos) {
    m_pickupPos = pickupPos;

    // ---- Try the real purchased GLB via a mounted loose-dir asset source. ----
    m_assets.reset(x3::asset::createAssetSource());
    bool mounted = m_assets->mountDir(modelDir, 0);
    if (mounted) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_model = m_loader->load("WeaponEnergyPistol.glb");
        if (m_model.ok) {
            m_drawables = x3::asset::makeDrawables(m_model);
        }
    } else {
        x3::logWarn("[weapon] mountDir failed: " + std::string(modelDir));
    }

    if (!m_drawables.empty()) {
        m_usingReal  = true;
        m_modelScale = kRealModelScale;
        x3::logInfo("[weapon] loaded WeaponEnergyPistol.glb — " +
                    std::to_string(m_drawables.size()) + " drawable primitive(s)");
    } else {
        // ---- Fallback: a small procedural box so the slice still works. ----
        m_usingReal  = false;
        m_modelScale = kBoxModelScale;
        if (m_model.ok)
            x3::logWarn("[weapon] GLB loaded but produced no drawables; using fallback box");
        else
            x3::logWarn("[weapon] WeaponEnergyPistol.glb load failed; using fallback box");

        x3::prims::PrimMesh geo = x3::prims::makeBox(kBoxHalf, kBoxHalf * 0.5f, kBoxHalf * 1.6f,
                                                     0.0f, 0.0f, 0.0f, 1.0f);
        x3::rhi::MeshHandle mesh = device.createMesh(
            geo.verts.data(), (uint32_t)geo.verts.size(),
            geo.index.data(), (uint32_t)geo.index.size());
        x3::asset::ModelDrawable d;
        d.meshId = mesh.id;
        d.baseColorTexId = 0;                    // 0 -> default white -> flat color
        d.baseColorFactor[0] = 0.85f; d.baseColorFactor[1] = 0.85f;
        d.baseColorFactor[2] = 0.20f; d.baseColorFactor[3] = 1.0f;  // weapon-yellow
        m_drawables.push_back(d);
    }

    // ---- Pickup Entity. Purely visual (no physics body): arming is a distance
    // check and the bob/spin are driven by overwriting its transform each frame
    // (a body would fight that). The Entity is bookkeeping only — tag, visibility
    // flag and the animated transform. Its mesh handle is left INVALID so
    // Scene::render skips it; this system's drawPickup() renders ALL of the
    // model's primitives at the Entity transform (so multi-primitive GLBs draw
    // fully and consistently with the viewmodel).
    Entity e;
    e.tag     = (uint32_t)Tag::Weapon;
    e.visible = true;
    // Authored transform = base position with the model scale baked into the 3x3.
    composeTRS(e.transform,
               x3::phys::Vec3{1, 0, 0}, x3::phys::Vec3{0, 1, 0}, x3::phys::Vec3{0, 0, 1},
               m_modelScale, m_pickupPos);
    m_pickupEntity = scene.add(e);

    x3::logInfo("[weapon] pickup entity " + std::to_string(m_pickupEntity) +
                " placed at (" + std::to_string(pickupPos.x) + ", " +
                std::to_string(pickupPos.y) + ", " + std::to_string(pickupPos.z) + ")" +
                (m_usingReal ? " [real GLB]" : " [fallback box]"));
}

// ---------------------------------------------------------------------------
// Per-frame: bob/spin the pickup, then run arming.
// ---------------------------------------------------------------------------
void WeaponSystem::update(float dt, Scene& scene, const x3::phys::Vec3& playerPos) {
    if (dt > 0.0f) m_animT += dt;

    // Bob + spin the visible pickup (skip once picked up / hidden).
    if (!m_hasWeapon && m_pickupEntity != kNoLink && m_pickupEntity < scene.size()) {
        Entity& e = scene.get(m_pickupEntity);
        const float bobY = kBobAmplitude * std::sin(m_animT * kBobHz * 2.0f * kPi);
        const float spin = m_animT * kSpinRate;
        const float c = std::cos(spin), s = std::sin(spin);
        // Yaw spin about +Y: local +X -> (c,0,-s), +Z -> (s,0,c).
        composeTRS(e.transform,
                   x3::phys::Vec3{ c, 0, -s }, x3::phys::Vec3{ 0, 1, 0 }, x3::phys::Vec3{ s, 0, c },
                   m_modelScale,
                   x3::phys::Vec3{ m_pickupPos.x, m_pickupPos.y + bobY, m_pickupPos.z });
    }

    // Arming (latched): only false->true, never the reverse.
    if (shouldArm(playerPos, m_pickupPos, kPickupRadius, m_hasWeapon)) {
        m_hasWeapon = true;
        if (m_pickupEntity != kNoLink && m_pickupEntity < scene.size())
            scene.get(m_pickupEntity).visible = false;
        x3::logInfo("[weapon] picked up — player armed");
    }
}

void WeaponSystem::forceArm(Scene& scene) {
    m_hasWeapon = true;
    if (m_pickupEntity != kNoLink && m_pickupEntity < scene.size())
        scene.get(m_pickupEntity).visible = false;
}

// ---------------------------------------------------------------------------
// Draw the bobbing/spinning pickup (all primitives) at the Entity transform.
// ---------------------------------------------------------------------------
void WeaponSystem::drawPickup(x3::rhi::IRenderDevice& device,
                              const x3::rhi::FrameContext& frame,
                              const Scene& scene) const {
    if (m_hasWeapon || m_pickupEntity == kNoLink || m_pickupEntity >= scene.size())
        return;
    const Entity& e = scene.get(m_pickupEntity);
    if (!e.visible) return;
    drawWeaponAt(device, frame, e.transform);
}

// ---------------------------------------------------------------------------
// Draw all weapon primitives at one model transform.
// ---------------------------------------------------------------------------
void WeaponSystem::drawWeaponAt(x3::rhi::IRenderDevice& device,
                                const x3::rhi::FrameContext& frame,
                                const float model[16]) const {
    for (const auto& d : m_drawables) {
        device.drawMesh(frame,
                        x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId },
                        d.baseColorFactor,
                        model);
    }
}

// ---------------------------------------------------------------------------
// First-person viewmodel: place the weapon relative to the camera basis.
// ---------------------------------------------------------------------------
void WeaponSystem::drawViewmodel(x3::rhi::IRenderDevice& device,
                                 const x3::rhi::FrameContext& frame,
                                 float eyeX, float eyeY, float eyeZ,
                                 float yaw, float pitch,
                                 float yawOff, float pitchOff, float rollOff,
                                 float fwd, float right_, float down) const {
    if (!m_hasWeapon || m_drawables.empty()) return;

    // Device forward convention: fwd = (cos p cos y, sin p, cos p sin y).
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    // Horizontal right (independent of pitch): right = (-sin y, 0, cos y).
    x3::phys::Vec3 right{ -sy, 0.0f, cy };
    // Up completes a right-handed-ish basis: up = right x forward.
    x3::phys::Vec3 up{
        right.y * forward.z - right.z * forward.y,
        right.z * forward.x - right.x * forward.z,
        right.x * forward.y - right.y * forward.x
    };

    // Eye + forward*f + right*r - up*d  (lower-right of the view). The f/r/d
    // distances come from the live-tunable vm_fwd/vm_right/vm_down cvars.
    x3::phys::Vec3 pos{
        eyeX + forward.x * fwd + right.x * right_ - up.x * down,
        eyeY + forward.y * fwd + right.y * right_ - up.y * down,
        eyeZ + forward.z * fwd + right.z * right_ - up.z * down
    };

    // Orient the model so its local axes align to the camera basis. glTF forward
    // is local -Z, so local +Z -> -forward, local +X -> right, local +Y -> up.
    x3::phys::Vec3 negFwd{ -forward.x, -forward.y, -forward.z };

    // Apply the visual orientation offsets about the CAMERA basis axes so the
    // mismapped barrel can be dialed onto the look direction. Yaw about up,
    // pitch about right, roll about forward; rotate the model basis vectors
    // (right/up/negFwd) by the same offsets so the whole model swings together.
    // (No gameplay effect — the fire ray uses the camera look dir, see header.)
    // (yawOff/pitchOff/rollOff are RADIANS, from the live-tunable
    // vm_yaw/vm_pitch/vm_roll cvars — see main.cpp.)
    auto applyOffsets = [&](x3::phys::Vec3 v) {
        v = rotateAboutAxis(v, up,      yawOff);
        v = rotateAboutAxis(v, right,   pitchOff);
        v = rotateAboutAxis(v, forward, rollOff);
        return v;
    };
    x3::phys::Vec3 bx = applyOffsets(right);
    x3::phys::Vec3 by = applyOffsets(up);
    x3::phys::Vec3 bz = applyOffsets(negFwd);

    float model[16];
    composeTRS(model, bx, by, bz, m_modelScale, pos);
    drawWeaponAt(device, frame, model);
}

// ===========================================================================
// Headless self-test (--test-pickup). T1 far, T2 enter radius, T3 stays armed.
// Drives the pure arming rule + a minimal latching loop with synthetic player
// positions. No window / Vulkan / model loading required.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[pickup-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[pickup-test] FAIL ") + name); }
}

} // namespace

bool runPickupSelfTest() {
    g_pass = g_fail = 0;

    const x3::phys::Vec3 pickup{ 2.0f, 1.0f, 2.0f };
    const float r = kPickupRadius;

    // Mirror the latching logic in WeaponSystem::update() with a local flag so we
    // exercise exactly the gameplay-relevant decision without any device.
    bool armed = false;
    bool visible = true;
    auto step = [&](const x3::phys::Vec3& playerPos) {
        if (shouldArm(playerPos, pickup, r, armed)) { armed = true; visible = false; }
    };

    // ---- T1: player far from pickup -> stays unarmed, pickup visible ----------
    {
        x3::phys::Vec3 far{ 10.0f, 1.0f, 10.0f };   // ~11.3 m away (>> 1.2)
        step(far);
        check(!armed && visible, "T1 far: unarmed, pickup visible");
    }

    // ---- T2: move within radius -> becomes armed, pickup hidden ---------------
    {
        // 0.5 m away horizontally (height differs, ignored): inside 1.2 m.
        x3::phys::Vec3 near{ pickup.x + 0.5f, 0.05f, pickup.z + 0.0f };
        step(near);
        check(armed && !visible, "T2 enter radius: armed, pickup hidden");
    }

    // ---- T3: once armed, leaving the radius keeps hasWeapon true (no un-pickup)
    {
        x3::phys::Vec3 farAgain{ -8.0f, 1.0f, -8.0f };
        step(farAgain);
        // Also confirm shouldArm() itself refuses to re-arm when already armed,
        // even right on top of the pickup.
        bool reArm = shouldArm(pickup, pickup, r, /*alreadyArmed=*/true);
        check(armed && !visible && !reArm, "T3 leave radius: stays armed (no un-pickup)");
    }

    // ---- Bonus boundary sanity: just outside the radius does NOT arm a fresh
    // state; just inside does (guards the distance math).
    {
        bool a2 = false;
        // 1.30 m away (> 1.2) -> no arm.
        bool outside = shouldArm(x3::phys::Vec3{ pickup.x + 1.30f, 0, pickup.z }, pickup, r, a2);
        // 1.10 m away (< 1.2) -> arm.
        bool inside  = shouldArm(x3::phys::Vec3{ pickup.x + 1.10f, 0, pickup.z }, pickup, r, a2);
        check(!outside && inside, "T4 radius boundary (outside no-arm, inside arms)");
    }

    x3::logInfo(std::string("[pickup-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
