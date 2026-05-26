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
        // Bake the glTF node's world transform: fin = model * nodeTransform, so
        // multi-node / Y-up-corrected GLBs place each primitive correctly (M2 fix).
        float fin[16];
        x3::asset::mulMat4(model, d.nodeTransform, fin);
        device.drawMesh(frame,
                        x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId },
                        d.baseColorFactor,
                        fin);
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
// DATA-DRIVEN WEAPON ARSENAL — implementation.
// ===========================================================================
// Clean-room: every value is derived from the design docs (docs/EFLZ_DESIGN.md §5
// + docs/ASSET_INVENTORY.md S5 weapon table). No purchased C# copied — only the
// plausible numeric stats from the design bible are used.

std::vector<WeaponDef> makeDefaultRoster() {
    std::vector<WeaponDef> r;

    // ---- 1) Energy Pistol — the starting firearm (have today). --------------
    // Docs: 15 dmg, 3 shots/s, mag 12, reserve 60, reload 1.5 s, 50 m, hitscan.
    {
        WeaponDef w;
        w.name        = "pistol";
        w.kind        = FireKind::Hitscan;
        w.automatic   = false;
        w.damage      = 15;
        w.fireRate    = 3.0f;
        w.pellets     = 1;
        w.spreadDeg   = 0.5f;     // near-perfect; tiny hipfire jitter
        w.recoilDeg   = 1.2f;
        w.range       = 50.0f;
        w.magSize     = 12;
        w.reserveAmmo = 60;
        w.reloadTime  = 1.5f;
        w.viewmodelGlb = "WeaponEnergyPistol2.glb";  // real energy_pistol.obj (PBR-textured)
        w.vmScale     = 0.18f;                        // proven pistol read (~0.33 m held)
        w.muzzleFx    = "muzzle_pistol";
        w.impactFx    = "impact_bullet";
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-01.wav";   // punchy single shot
        r.push_back(w);
    }

    // ---- 2) SMG / Rifle (auto, low recoil) — the Chaingun "auto" archetype. --
    // Docs name a tier-4 high-ROF auto (Chaingun). For the slice we model a
    // controllable automatic: lower per-shot damage, high fire rate, low recoil,
    // a big magazine, medium range. Hitscan.
    {
        WeaponDef w;
        w.name        = "smg";
        w.kind        = FireKind::Hitscan;
        w.automatic   = true;
        w.damage      = 12;
        w.fireRate    = 11.0f;    // ~660 rpm
        w.pellets     = 1;
        w.spreadDeg   = 1.8f;     // a touch of bloom (auto)
        w.recoilDeg   = 0.5f;     // low recoil per shot
        w.range       = 60.0f;
        w.magSize     = 40;
        w.reserveAmmo = 200;
        w.reloadTime  = 2.0f;
        w.viewmodelGlb = "WeaponRailgun.glb";  // railgun reads as a rifle for the auto SMG
        w.vmScale     = 0.24f;                 // longarm (~0.46 m held)
        w.muzzleFx    = "muzzle_smg";
        w.impactFx    = "impact_bullet";
        w.fireSfx     = "weapons/rapid/Rapid-Fires_Sci-Fi_Gun-01.wav";   // rapid/auto burst
        w.fireSfxLoop = true;   // auto weapon: loopable rapid set
        r.push_back(w);
    }

    // ---- 3) Shotgun — pellets + wide spread, close range. -------------------
    // Docs: 20 dmg per pellet x8, 1 shot/s, 15 m. mag 8 / reserve 32 / reload 2.5 s.
    {
        WeaponDef w;
        w.name        = "shotgun";
        w.kind        = FireKind::Hitscan;
        w.automatic   = false;
        w.damage      = 20;       // PER pellet
        w.fireRate    = 1.0f;
        w.pellets     = 8;
        w.spreadDeg   = 7.0f;     // wide cone
        w.recoilDeg   = 4.0f;     // big kick
        w.range       = 15.0f;
        w.magSize     = 8;
        w.reserveAmmo = 32;
        w.reloadTime  = 2.5f;
        w.viewmodelGlb = "WeaponShotgun2.glb";  // real shotgun.obj (PBR-textured; long barrel -> small scale)
        w.vmScale     = 0.11f;                   // longest source model (4.4 m) -> ~0.48 m held
        w.muzzleFx    = "muzzle_shotgun";
        w.impactFx    = "impact_bullet";
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-57.wav";   // heavy single boom
        r.push_back(w);
    }

    // ---- 4) Plasma — energy PROJECTILE (travels, not hitscan). --------------
    // Docs list craftable Plasma Pistol/Rifle. Modeled as a slow-ish energy bolt:
    // solid damage, modest fire rate, a real travel speed so it reads as a bolt.
    {
        WeaponDef w;
        w.name        = "plasma";
        w.kind        = FireKind::Projectile;
        w.automatic   = false;
        w.damage      = 35;
        w.fireRate    = 2.0f;
        w.pellets     = 1;
        w.spreadDeg   = 0.3f;
        w.recoilDeg   = 1.5f;
        w.range       = 80.0f;
        w.magSize     = 20;
        w.reserveAmmo = 80;
        w.reloadTime  = 2.2f;
        w.projSpeed   = 45.0f;    // m/s bolt
        w.viewmodelGlb = "WeaponBFG.glb";  // bfg.obj energy-cannon look for the plasma bolt
        w.vmScale     = 0.24f;             // bulky energy weapon (~0.42 m held)
        w.muzzleFx    = "muzzle_plasma";
        w.impactFx    = "impact_plasma";
        // Sci-fi energy single (a distinct, higher-tech single shot).
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-30.wav";
        r.push_back(w);
    }

    // =====================================================================
    // ACT-1 WEAPON LADDER (canon progression — see docs/EFLZ_DESIGN.md §
    // weapons table + docs/design/EFLZ_MASTER_PLAN.md / EFLZ_WORLD_STRUCTURE.md
    // "Act-1 weapon ladder"). Each tier is a clear step above the pistol
    // (15 dmg, 3/s = 45 DPS) WITHOUT trivializing the Act-1 bosses (The
    // Collective F4, Swarm Controller F5, Alien Overseer F6 — 200..400 HP per
    // the bestiary), so a boss still costs a deliberate magazine+, not a
    // one-burst delete. Built only from the data-driven model + design numbers.
    // =====================================================================

    // ---- 5) ChainGun — F4 drop (The Collective/Chorus). High-ROF auto -------
    // hitscan with a short SPIN-UP before full rate; large mag/reserve; moderate
    // per-hit damage; a little spread. Docs: "tier-4 high-ROF auto". Sustained
    // DPS at full spin (~14 dmg * 14/s = ~196) clears trash fast but a 400-HP
    // boss is still ~15 sustained hits — a real magazine, not a melt. The 0.6 s
    // spin-up (starting at 40% rate) is the canon "wind-up" feel.
    {
        WeaponDef w;
        w.name        = "chaingun";
        w.kind        = FireKind::Hitscan;
        w.automatic   = true;
        w.damage      = 14;
        w.fireRate    = 14.0f;        // ~840 rpm at full spin
        w.pellets     = 1;
        w.spreadDeg   = 2.5f;         // some bloom (heavy auto)
        w.recoilDeg   = 0.4f;         // low per-shot kick, but it adds up fast
        w.range       = 70.0f;
        w.magSize     = 100;          // large belt
        w.reserveAmmo = 400;          // large reserve
        w.reloadTime  = 3.0f;         // slow to reload the belt
        w.spinUpTime     = 0.6f;      // 0.6 s of fire to reach full RoF
        w.spinUpStartFrac= 0.4f;      // starts at 40% of fireRate (cold barrel)
        w.viewmodelGlb = "WeaponRocketLauncher.glb";  // heaviest model -> the chaingun read
        w.vmScale     = 0.26f;                        // heavy weapon (~0.49 m held)
        w.muzzleFx    = "muzzle_chaingun";
        w.impactFx    = "impact_bullet";
        // Sustained rapid-fire belt (the longest rapid take = a meatier auto roar).
        w.fireSfx     = "weapons/rapid/Rapid-Fires_Sci-Fi_Gun-49.wav";
        w.fireSfxLoop = true;   // auto weapon: loopable rapid set
        r.push_back(w);
    }

    // ---- 6) Plasma Rifle — F5 unlock (Drone Manufacturing). -----------------
    // A faster, harder-hitting plasma than the plasma pistol: a real travelling
    // bolt (projectile) with a SMALL impact splash. Medium RoF. ~40 dmg direct +
    // 15 splash @ 2 m. At ~4/s that's ~160 direct DPS — a tier above the pistol,
    // and the splash punishes clustered drones without one-shotting the F5 boss.
    {
        WeaponDef w;
        w.name        = "plasma_rifle";
        w.kind        = FireKind::Projectile;
        w.automatic   = true;         // full-auto bolt stream
        w.damage      = 40;           // direct-hit damage
        w.fireRate    = 4.0f;         // medium RoF
        w.pellets     = 1;
        w.spreadDeg   = 0.6f;
        w.recoilDeg   = 0.8f;
        w.range       = 90.0f;
        w.magSize     = 30;
        w.reserveAmmo = 120;
        w.reloadTime  = 2.2f;
        w.projSpeed   = 60.0f;        // faster bolt than the plasma pistol
        w.splashRadius= 2.0f;         // small AoE on impact
        w.splashDamage= 15;           // splash damage at the center
        w.viewmodelGlb = "WeaponBFG.glb";  // bfg.obj energy-cannon look (shared with plasma)
        w.vmScale     = 0.24f;             // bulky energy weapon (~0.42 m held)
        w.muzzleFx    = "muzzle_plasma";
        w.impactFx    = "impact_plasma";
        // Bigger energy single than the plasma pistol (largest single = fuller bolt).
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-66.wav";
        r.push_back(w);
    }

    // ---- 7) Lightning Gun — F6 unlock (Salvari tech). -----------------------
    // A continuous instant-hit BEAM: high sustained DPS, SHORT range with damage
    // falloff, and (per the bible: "chains 3 targets") a chain-to-nearby-enemies
    // behavior — the primary beam plus up to 2 extra chain rays (3 targets total).
    // High fire rate models the "continuous" feel. ~10 dmg * 16/s = ~160 single-
    // target sustained DPS, multiplied across chained targets in a crowd, but the
    // short 28 m range (falloff from 14 m) keeps it a close-quarters power tool.
    {
        WeaponDef w;
        w.name        = "lightning";
        w.kind        = FireKind::Hitscan;   // instant-hit beam = hitscan path
        w.automatic   = true;                // held = continuous beam
        w.damage      = 10;                  // per beam tick / per chained target
        w.fireRate    = 16.0f;               // "continuous" — many ticks/s
        w.pellets     = 1;                   // 1 primary; chains add rays
        w.spreadDeg   = 0.0f;                // a beam is dead-accurate
        w.recoilDeg   = 0.15f;               // almost none (steady beam)
        w.range       = 28.0f;               // SHORT range
        w.magSize     = 80;                  // a "cell" of charge
        w.reserveAmmo = 240;
        w.reloadTime  = 2.4f;
        w.beam        = true;                // render as a solid beam (host hint)
        w.chainTargets= 2;                   // primary + 2 chains = 3 targets
        w.falloffStart= 14.0f;               // half-range: damage falls off past 14 m
        w.viewmodelGlb = "WeaponRailgun.glb"; // railgun rifle for the precision beam
        w.vmScale     = 0.24f;                // longarm (~0.46 m held)
        w.muzzleFx    = "muzzle_lightning";
        w.impactFx    = "impact_lightning";
        // Continuous loopable zap/beam (the loop reads as a sustained electric crackle).
        w.fireSfx     = "weapons/loops/Loopable_Rapid-Fires_Sci-Fi_Gun_7.wav";
        w.fireSfxLoop = true;   // continuous beam: loopable
        r.push_back(w);
    }

    return r;
}

// ---------------------------------------------------------------------------
// Spread: rotate `dir` by a random angle within a `spreadDeg` half-angle cone.
// Deterministic xorshift so headless captures repeat. Builds an orthonormal basis
// around `dir` and offsets by a uniformly-sampled point in the small disc, then
// renormalizes — a standard small-cone approximation (exact enough for gameplay).
// ---------------------------------------------------------------------------
namespace {
inline uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
}
inline float frand01(uint32_t& s) { return (xorshift32(s) >> 8) * (1.0f / 16777216.0f); }
} // namespace

x3::phys::Vec3 applySpread(const x3::phys::Vec3& dir, float spreadDeg, uint32_t& rngState) {
    // Normalize input.
    float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl < 1e-6f) return dir;
    x3::phys::Vec3 d{ dir.x / dl, dir.y / dl, dir.z / dl };
    if (spreadDeg <= 0.0f) return d;

    // Orthonormal basis (t, b) perpendicular to d.
    x3::phys::Vec3 up = (std::fabs(d.y) < 0.99f) ? x3::phys::Vec3{ 0, 1, 0 }
                                                 : x3::phys::Vec3{ 1, 0, 0 };
    x3::phys::Vec3 t{ up.y * d.z - up.z * d.y, up.z * d.x - up.x * d.z, up.x * d.y - up.y * d.x };
    float tl = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
    if (tl < 1e-6f) tl = 1.0f;
    t = x3::phys::Vec3{ t.x / tl, t.y / tl, t.z / tl };
    x3::phys::Vec3 b{ d.y * t.z - d.z * t.y, d.z * t.x - d.x * t.z, d.x * t.y - d.y * t.x };

    // Sample an angle within the cone (uniform over the disc area) + random azimuth.
    const float maxRad = spreadDeg * (kPi / 180.0f);
    const float r   = std::tan(maxRad) * std::sqrt(frand01(rngState)); // disc radius
    const float az  = frand01(rngState) * 2.0f * kPi;
    const float ox = r * std::cos(az), oy = r * std::sin(az);
    x3::phys::Vec3 out{ d.x + t.x * ox + b.x * oy,
                        d.y + t.y * ox + b.y * oy,
                        d.z + t.z * ox + b.z * oy };
    float ol = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z);
    if (ol < 1e-6f) return d;
    return x3::phys::Vec3{ out.x / ol, out.y / ol, out.z / ol };
}

// ---------------------------------------------------------------------------
// Arsenal.
// ---------------------------------------------------------------------------
Arsenal::Arsenal(std::vector<WeaponDef> roster) : m_defs(std::move(roster)) {
    m_state.resize(m_defs.size());
    for (size_t i = 0; i < m_defs.size(); ++i) {
        m_state[i].ammoInMag = m_defs[i].magSize;   // start with a full mag
        m_state[i].reserve   = m_defs[i].reserveAmmo;
        m_state[i].cooldown  = 0.0f;
        m_state[i].reloadTimer = 0.0f;
        m_state[i].spinUp      = 0.0f;               // cold barrel
    }
    if (m_defs.empty()) m_sel = -1; else m_sel = 0;
}

void Arsenal::restore(int sel, const std::vector<std::pair<int,int>>& ammo) {
    // Apply the per-weapon ammo first (so the selection clamp below sees the roster
    // unchanged), then the selection. Cooldowns/reload timers are cleared — a
    // checkpoint restore lands the arsenal in a settled, ready state.
    const size_t n = (ammo.size() < m_state.size()) ? ammo.size() : m_state.size();
    for (size_t i = 0; i < n; ++i) {
        int mag = ammo[i].first;
        int res = ammo[i].second;
        if (mag < 0) mag = 0; if (mag > m_defs[i].magSize)     mag = m_defs[i].magSize;
        if (res < 0) res = 0; if (res > m_defs[i].reserveAmmo) res = m_defs[i].reserveAmmo;
        m_state[i].ammoInMag   = mag;
        m_state[i].reserve     = res;
        m_state[i].cooldown    = 0.0f;
        m_state[i].reloadTimer = 0.0f;
        m_state[i].spinUp      = 0.0f;   // settled checkpoint: cold barrel
    }
    if (m_defs.empty())            m_sel = -1;
    else if (sel < 0)              m_sel = 0;
    else if (sel >= (int)m_defs.size()) m_sel = (int)m_defs.size() - 1;
    else                           m_sel = sel;
}

int Arsenal::select(int index) {
    if (index < 0 || index >= (int)m_defs.size()) return m_sel; // ignore out-of-range
    if (index == m_sel) return m_sel;
    // Cancel an in-progress reload on the weapon we're leaving (don't lose a round;
    // the reload simply didn't complete). Also bleed off its spin-up so you can't
    // bank a spun-up barrel by switching away and back.
    if (m_sel >= 0) { m_state[(size_t)m_sel].reloadTimer = 0.0f;
                      m_state[(size_t)m_sel].spinUp = 0.0f; }
    m_sel = index;
    // Reset the new weapon's cooldown to its full inter-shot time so a switch can't
    // be used to fire faster than the new weapon's rate.
    const WeaponDef& d = m_defs[(size_t)m_sel];
    if (d.fireRate > 0.0f) m_state[(size_t)m_sel].cooldown = 1.0f / d.fireRate;
    x3::logInfo("[arsenal] selected '" + d.name + "' (slot " + std::to_string(m_sel + 1) + ")");
    return m_sel;
}

int Arsenal::indexOf(const std::string& name) const {
    for (size_t i = 0; i < m_defs.size(); ++i)
        if (m_defs[i].name == name) return (int)i;
    return -1;
}

bool Arsenal::selectByName(const std::string& name) {
    int i = indexOf(name);
    if (i < 0) return false;
    select(i);
    return true;
}

bool Arsenal::canFire() const {
    if (m_sel < 0) return false;
    const WeaponState& s = m_state[(size_t)m_sel];
    if (s.reloadTimer > 0.0f) return false;   // mid-reload: can't fire
    if (s.cooldown    > 0.0f) return false;   // fire-rate gate
    if (s.ammoInMag  <= 0 && !m_infiniteAmmo) return false;   // empty mag (IDKFA bypasses)
    return true;
}

ResolvedFire Arsenal::fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, uint32_t& rngState) {
    ResolvedFire out;
    if (!canFire()) return out;               // gated -> nothing consumed

    const WeaponDef&  d = m_defs[(size_t)m_sel];
    WeaponState&      s = m_state[(size_t)m_sel];

    // ChainGun spin-up: the EFFECTIVE fire rate ramps from spinUpStartFrac*fireRate
    // (cold) up to fireRate as s.spinUp climbs 0 -> 1. The cooldown is set from the
    // effective rate, so the first shots come slower and the gun "winds up". Firing
    // advances the spin (one cooldown's worth of charge); tick() decays it when idle.
    float effRate = d.fireRate;
    if (d.spinUpTime > 0.0f && d.fireRate > 0.0f) {
        const float frac = (d.spinUpStartFrac < 0.0f) ? 0.0f
                         : (d.spinUpStartFrac > 1.0f) ? 1.0f : d.spinUpStartFrac;
        effRate = d.fireRate * (frac + (1.0f - frac) * s.spinUp);
        // Charge the spin by the inter-shot time this shot consumes.
        const float dShot = (effRate > 0.0f) ? (1.0f / effRate) : d.spinUpTime;
        s.spinUp += dShot / d.spinUpTime;
        if (s.spinUp > 1.0f) s.spinUp = 1.0f;
    }

    // Consume a round, arm the fire-rate cooldown (from the effective rate), recoil.
    if (!m_infiniteAmmo) s.ammoInMag -= 1;   // IDKFA: never deplete
    s.cooldown   = (effRate > 0.0f) ? (1.0f / effRate) : 0.0f;
    out.fired    = true;
    out.recoilPitchDeg = d.recoilDeg;

    if (d.kind == FireKind::Hitscan) {
        // Primary ray(s): `pellets` spread rays (1 for a single-ray weapon).
        const int total = d.pellets + ((d.chainTargets > 0) ? d.chainTargets : 0);
        out.rays.reserve((size_t)total);
        for (int p = 0; p < d.pellets; ++p) {
            HitscanRay ray;
            ray.dir          = applySpread(dir, d.spreadDeg, rngState);
            ray.damage       = d.damage;
            ray.range        = d.range;
            ray.beam         = d.beam;
            ray.chain        = false;
            ray.falloffStart = d.falloffStart;
            out.rays.push_back(ray);
        }
        // Lightning Gun chain: extra rays the host resolves against NEARBY enemies
        // (chain links). They share the beam dir as a seed; the host re-aims each
        // chain at the next-closest enemy. Each carries the per-target damage so a
        // crowd takes chainTargets+1 hits. Marked chain=true so the host/FX know.
        for (int c = 0; c < d.chainTargets; ++c) {
            HitscanRay ray;
            ray.dir          = dir;          // host re-aims at the next chained enemy
            ray.damage       = d.damage;
            ray.range        = d.range;
            ray.beam         = d.beam;
            ray.chain        = true;
            ray.falloffStart = d.falloffStart;
            out.rays.push_back(ray);
        }
    } else { // Projectile
        x3::phys::Vec3 nd = applySpread(dir, d.spreadDeg, rngState);
        ProjectileSpawn pj;
        pj.pos    = eye;
        pj.vel    = x3::phys::Vec3{ nd.x * d.projSpeed, nd.y * d.projSpeed, nd.z * d.projSpeed };
        pj.damage = d.damage;
        pj.range  = d.range;
        pj.splashRadius = d.splashRadius;   // Plasma Rifle: small AoE on impact
        pj.splashDamage = d.splashDamage;
        out.projectiles.push_back(pj);
    }
    return out;
}

bool Arsenal::reload() {
    if (m_sel < 0) return false;
    const WeaponDef& d = m_defs[(size_t)m_sel];
    WeaponState&     s = m_state[(size_t)m_sel];
    if (s.reloadTimer > 0.0f)        return false; // already reloading
    if (s.ammoInMag   >= d.magSize)  return false; // mag already full
    if (s.reserve     <= 0)          return false; // no spare ammo
    s.reloadTimer = d.reloadTime;
    x3::logInfo("[arsenal] reloading '" + d.name + "' (" + std::to_string(d.reloadTime) + "s)");
    return true;
}

void Arsenal::tick(float dt) {
    if (dt <= 0.0f) return;
    for (size_t i = 0; i < m_state.size(); ++i) {
        WeaponState& s = m_state[i];
        if (s.cooldown > 0.0f) { s.cooldown -= dt; if (s.cooldown < 0.0f) s.cooldown = 0.0f; }
        // Spin-up wind-down: once the inter-shot cooldown has fully elapsed (the
        // player stopped firing this weapon), the barrel bleeds spin back toward 0.
        // The wind-down is GENTLER than the per-shot charge (decay over 1.5x
        // spinUpTime) so a sustained burst still climbs to full spin even though
        // tick() runs between shots; a real idle gap (~2 s) drains it fully. While
        // firing, fire() re-arms the cooldown each shot so this branch is skipped
        // for the frames the cooldown is still counting down.
        const WeaponDef& dd = m_defs[i];
        if (dd.spinUpTime > 0.0f && s.spinUp > 0.0f && s.cooldown <= 0.0f) {
            s.spinUp -= dt / (dd.spinUpTime * 1.5f);
            if (s.spinUp < 0.0f) s.spinUp = 0.0f;
        }
        if (s.reloadTimer > 0.0f) {
            s.reloadTimer -= dt;
            if (s.reloadTimer <= 0.0f) {
                s.reloadTimer = 0.0f;
                // Move rounds from reserve into the mag (up to a full magazine).
                const WeaponDef& d = m_defs[i];
                int need = d.magSize - s.ammoInMag;
                int take = (need < s.reserve) ? need : s.reserve;
                s.ammoInMag += take;
                s.reserve   -= take;
                x3::logInfo("[arsenal] reload complete '" + d.name + "' mag=" +
                            std::to_string(s.ammoInMag) + " reserve=" + std::to_string(s.reserve));
            }
        }
    }
}

// ---- Optional viewmodel render layer --------------------------------------
void Arsenal::loadViewmodels(x3::rhi::IRenderDevice& device, std::string_view modelDir) {
    m_views.clear();
    m_views.resize(m_defs.size());
    int firstLoaded = -1;
    for (size_t i = 0; i < m_defs.size(); ++i) {
        ViewModel& v = m_views[i];
        const WeaponDef& d = m_defs[i];
        if (!d.viewmodelGlb.empty()) {
            v.assets.reset(x3::asset::createAssetSource());
            if (v.assets->mountDir(modelDir, 0)) {
                v.loader.reset(x3::asset::createModelLoader(&device, v.assets.get()));
                v.model = v.loader->load(d.viewmodelGlb);
                if (v.model.ok) v.drawables = x3::asset::makeDrawables(v.model);
            }
        }
        if (!v.drawables.empty()) {
            if (firstLoaded < 0) firstLoaded = (int)i;
            x3::logInfo("[arsenal] viewmodel '" + d.name + "' <- " + d.viewmodelGlb +
                        " (" + std::to_string(v.drawables.size()) + " prims)");
        } else {
            x3::logWarn("[arsenal] viewmodel '" + d.name + "' GLB missing (" +
                        d.viewmodelGlb + ") — will fall back to the pistol viewmodel");
        }
    }
    // Point every empty viewmodel at the first one that loaded (the pistol).
    for (size_t i = 0; i < m_views.size(); ++i)
        if (m_views[i].drawables.empty()) m_views[i].fallbackIndex = firstLoaded;
    if (firstLoaded < 0)
        x3::logWarn("[arsenal] no viewmodel GLB loaded; drawCurrentViewmodel is a no-op");
}

void Arsenal::drawCurrentViewmodel(x3::rhi::IRenderDevice& device,
                                   const x3::rhi::FrameContext& frame,
                                   float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
                                   float extraYawOff, float extraPitchOff, float extraRollOff,
                                   float extraFwd, float extraRight, float extraDown) const {
    if (m_sel < 0 || m_views.empty()) return;
    const ViewModel& sel = m_views[(size_t)m_sel];
    const ViewModel& vm  = (!sel.drawables.empty()) ? sel
                         : (sel.fallbackIndex >= 0 ? m_views[(size_t)sel.fallbackIndex] : sel);
    if (vm.drawables.empty()) return;

    const WeaponDef& d = m_defs[(size_t)m_sel];
    const float yawOff   = d.vmYawDeg   * (kPi / 180.0f) + extraYawOff;
    const float pitchOff = d.vmPitchDeg * (kPi / 180.0f) + extraPitchOff;
    const float rollOff  = d.vmRollDeg  * (kPi / 180.0f) + extraRollOff;
    const float fwd   = d.vmFwd   + extraFwd;
    const float rgt   = d.vmRight + extraRight;
    const float down  = d.vmDown  + extraDown;

    // Same camera-basis math as WeaponSystem::drawViewmodel (see §3 CONVENTIONS).
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    x3::phys::Vec3 right{ -sy, 0.0f, cy };
    x3::phys::Vec3 up{ right.y * forward.z - right.z * forward.y,
                       right.z * forward.x - right.x * forward.z,
                       right.x * forward.y - right.y * forward.x };
    x3::phys::Vec3 pos{ eyeX + forward.x * fwd + right.x * rgt - up.x * down,
                        eyeY + forward.y * fwd + right.y * rgt - up.y * down,
                        eyeZ + forward.z * fwd + right.z * rgt - up.z * down };
    x3::phys::Vec3 negFwd{ -forward.x, -forward.y, -forward.z };
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
    // Tim playtest 2026-05-25: the held weapons read tiny + dark. Enlarge the viewmodel
    // (~2x) and brighten its base color (HDR > 1) so it reads as a big, lit gun in the
    // dark interiors instead of a microscopic silhouette. d.vmScale stays the per-weapon
    // RELATIVE tuning; kVmScaleBoost is the global "hold it up bigger" multiplier.
    constexpr float kVmScaleBoost = 2.0f;   // Tim: 2x scale is CORRECT, NOT too big — keep it.
    constexpr float kVmBright     = 2.6f;   // lit (the real issue is the GLB TEXTURE being wrong, not size/brightness)
    composeTRS(model, bx, by, bz, d.vmScale * kVmScaleBoost, pos);
    for (const auto& dr : vm.drawables) {
        float fin[16];
        x3::asset::mulMat4(model, dr.nodeTransform, fin);
        const float litColor[4] = {
            dr.baseColorFactor[0] * kVmBright,
            dr.baseColorFactor[1] * kVmBright,
            dr.baseColorFactor[2] * kVmBright,
            dr.baseColorFactor[3],
        };
        device.drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                        x3::rhi::TextureHandle{ dr.baseColorTexId },
                        litColor, fin);
    }
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

// ===========================================================================
// Headless self-test (--test-weapons). Exercises the data-driven Arsenal with NO
// window / Vulkan / physics. Covers: roster + switching, fire-rate gating, ammo
// gating, reload refill, shotgun pellet count, and hitscan-vs-projectile resolve.
// ===========================================================================
namespace {

int w_pass = 0, w_fail = 0;
void wcheck(bool cond, const char* name) {
    if (cond) { ++w_pass; x3::logInfo(std::string("[weapons-test] PASS ") + name); }
    else      { ++w_fail; x3::logError(std::string("[weapons-test] FAIL ") + name); }
}

} // namespace

bool runWeaponsSelfTest() {
    w_pass = w_fail = 0;

    const x3::phys::Vec3 eye{ 0, 1.7f, 0 };
    const x3::phys::Vec3 fwd{ 1, 0, 0 };   // fire along +X
    uint32_t rng = 0xC0FFEEu;

    // ---- W0: roster present + the four archetypes exist with sane stats ------
    {
        Arsenal a;
        bool haveFour = a.count() >= 4;
        int ip = a.indexOf("pistol"), is = a.indexOf("smg"),
            ig = a.indexOf("shotgun"), il = a.indexOf("plasma");
        bool named = ip >= 0 && is >= 0 && ig >= 0 && il >= 0;
        bool kinds = haveFour && named &&
                     a.def(ip).kind == FireKind::Hitscan &&
                     a.def(is).kind == FireKind::Hitscan && a.def(is).automatic &&
                     a.def(ig).kind == FireKind::Hitscan && a.def(ig).pellets == 8 &&
                     a.def(il).kind == FireKind::Projectile && a.def(il).projSpeed > 0.0f;
        // Pistol matches the design-doc numbers (15 dmg / 3 per s / mag 12 / 50 m).
        bool pistolStats = named &&
                     a.def(ip).damage == 15 && a.def(ip).fireRate == 3.0f &&
                     a.def(ip).magSize == 12 && a.def(ip).range == 50.0f;
        wcheck(haveFour && named && kinds && pistolStats,
               "W0 roster: pistol/smg/shotgun/plasma present with doc-sourced stats");
    }

    // ---- W0b: the Act-1 ladder (chaingun/plasma_rifle/lightning) exists, is ---
    // constructible + selectable, and each has sane fire-rate / damage / ammo. ---
    {
        Arsenal a;
        int ic = a.indexOf("chaingun"), ir = a.indexOf("plasma_rifle"),
            iz = a.indexOf("lightning");
        bool present = ic >= 0 && ir >= 0 && iz >= 0;
        // ChainGun: auto hitscan with a spin-up + a large belt.
        bool cgOK = present &&
            a.def(ic).kind == FireKind::Hitscan && a.def(ic).automatic &&
            a.def(ic).spinUpTime > 0.0f && a.def(ic).fireRate > 6.0f &&
            a.def(ic).damage > 0 && a.def(ic).magSize >= 50 && a.def(ic).reserveAmmo >= 100;
        // Plasma Rifle: projectile with a real travel speed + a small splash.
        bool prOK = present &&
            a.def(ir).kind == FireKind::Projectile && a.def(ir).projSpeed > 0.0f &&
            a.def(ir).splashRadius > 0.0f && a.def(ir).splashDamage > 0 &&
            a.def(ir).damage > 0 && a.def(ir).fireRate > 0.0f && a.def(ir).magSize > 0;
        // Lightning Gun: a beam (hitscan) that chains, with short-range falloff,
        // and a SHORT range (shorter than the pistol's 50 m).
        int ip = a.indexOf("pistol");
        bool lgOK = present && ip >= 0 &&
            a.def(iz).kind == FireKind::Hitscan && a.def(iz).beam &&
            a.def(iz).chainTargets >= 1 && a.def(iz).falloffStart > 0.0f &&
            a.def(iz).falloffStart < a.def(iz).range &&
            a.def(iz).range < a.def(ip).range &&   // short range vs pistol
            a.def(iz).damage > 0 && a.def(iz).fireRate > 0.0f && a.def(iz).magSize > 0;
        // Selectable by name (the host maps number keys 1..N onto these).
        bool selectable = a.selectByName("chaingun") && a.current().name == "chaingun" &&
                          a.selectByName("plasma_rifle") && a.current().name == "plasma_rifle" &&
                          a.selectByName("lightning") && a.current().name == "lightning";
        wcheck(present && cgOK && prOK && lgOK && selectable,
               "W0b Act-1 ladder: chaingun/plasma_rifle/lightning present, sane + selectable");
    }

    // ---- W1: switching selects the right WeaponDef (number keys 1..N) --------
    {
        Arsenal a;
        wcheck(a.selected() == 0 && a.current().name == "pistol", "W1a default selection = pistol");
        a.select(2);
        wcheck(a.selected() == 2 && a.current().name == "shotgun", "W1b select(2) = shotgun");
        a.select(99);   // out of range -> ignored
        wcheck(a.selected() == 2 && a.current().name == "shotgun", "W1c out-of-range select ignored");
        wcheck(a.selectByName("plasma") && a.current().name == "plasma", "W1d selectByName(plasma)");
    }

    // ---- W2: fire respects fireRate (can't fire faster than the rate) --------
    {
        Arsenal a;            // pistol: 3/s -> 0.333 s cooldown
        a.select(0);
        a.tick(1.0f);         // clear the initial select cooldown
        ResolvedFire f1 = a.fire(eye, fwd, rng);
        ResolvedFire f2 = a.fire(eye, fwd, rng);   // immediately again — gated
        bool gatedImmediate = f1.fired && !f2.fired;
        a.tick(0.1f);         // still < 0.333 s
        ResolvedFire f3 = a.fire(eye, fwd, rng);
        bool stillGated = !f3.fired;
        a.tick(0.30f);        // total elapsed since f1 now > 0.333 s
        ResolvedFire f4 = a.fire(eye, fwd, rng);
        bool firesAfterCd = f4.fired;
        wcheck(gatedImmediate && stillGated && firesAfterCd,
               "W2 fireRate gate: can't fire faster than 1/fireRate");
    }

    // ---- W3: fire respects ammo (can't fire on an empty mag) -----------------
    {
        Arsenal a;
        a.select(2);          // shotgun: mag 8
        a.tick(1.0f);
        int shots = 0;
        for (int i = 0; i < 20; ++i) {       // try to empty it (cooldown 1 s)
            ResolvedFire f = a.fire(eye, fwd, rng);
            if (f.fired) ++shots;
            a.tick(1.1f);                    // step past the 1 s fire rate each loop
        }
        bool emptiedAtMag = (shots == 8);                 // exactly magSize shots fired
        bool magEmpty      = a.currentState().ammoInMag == 0;
        ResolvedFire dry   = a.fire(eye, fwd, rng);       // empty -> gated
        wcheck(emptiedAtMag && magEmpty && !dry.fired,
               "W3 ammo gate: fires exactly magSize then is empty");
    }

    // ---- W4: reload refills the mag from reserve over reloadTime -------------
    {
        Arsenal a;
        a.select(2);          // shotgun: mag 8 / reserve 32 / reload 2.5 s
        a.tick(1.0f);
        for (int i = 0; i < 8; ++i) { a.fire(eye, fwd, rng); a.tick(1.1f); }  // empty it
        wcheck(a.currentState().ammoInMag == 0, "W4a mag empty before reload");
        bool began = a.reload();
        wcheck(began && a.isReloading(), "W4b reload begins");
        a.tick(1.0f);
        bool stillReloadingMid = a.isReloading() && a.currentState().ammoInMag == 0;
        wcheck(stillReloadingMid, "W4c mid-reload: mag not yet refilled");
        a.tick(2.0f);         // total 3.0 s > 2.5 s reload
        bool refilled = !a.isReloading() && a.currentState().ammoInMag == 8 &&
                        a.currentState().reserve == 24;   // 32 - 8 = 24
        wcheck(refilled, "W4d reload complete: mag full, reserve drained by 8");
    }

    // ---- W5: shotgun emits N pellets per shot --------------------------------
    {
        Arsenal a;
        a.select(2);          // shotgun: pellets 8
        a.tick(1.0f);
        ResolvedFire f = a.fire(eye, fwd, rng);
        bool eightRays = f.fired && (int)f.rays.size() == 8 && f.projectiles.empty();
        // Every pellet ray carries the per-pellet damage + the weapon range.
        bool perPelletDmg = eightRays;
        for (const auto& ray : f.rays)
            if (ray.damage != 20 || ray.range != 15.0f) perPelletDmg = false;
        // Spread actually scatters the rays (not all identical to the input dir).
        bool scattered = false;
        for (const auto& ray : f.rays)
            if (std::fabs(ray.dir.x - 1.0f) > 1e-4f || std::fabs(ray.dir.y) > 1e-4f) scattered = true;
        wcheck(eightRays && perPelletDmg && scattered,
               "W5 shotgun: 8 spread pellets, each 20 dmg @ 15 m");
    }

    // ---- W6: hitscan AND projectile both resolve into the right payload ------
    {
        Arsenal a;
        // Hitscan (pistol): one ray, no projectiles.
        a.select(0); a.tick(1.0f);
        ResolvedFire h = a.fire(eye, fwd, rng);
        bool hitscanOK = h.fired && (int)h.rays.size() == 1 && h.projectiles.empty() &&
                         h.rays[0].damage == 15;
        // Projectile (plasma): one projectile spawned at the eye with vel = dir*speed.
        a.select(3); a.tick(1.0f);
        ResolvedFire p = a.fire(eye, fwd, rng);
        bool projOK = p.fired && p.rays.empty() && (int)p.projectiles.size() == 1;
        if (projOK) {
            const ProjectileSpawn& pj = p.projectiles[0];
            float vlen = std::sqrt(pj.vel.x*pj.vel.x + pj.vel.y*pj.vel.y + pj.vel.z*pj.vel.z);
            projOK = pj.damage == 35 && std::fabs(vlen - 45.0f) < 0.5f &&
                     std::fabs(pj.pos.x - eye.x) < 1e-4f;   // spawns at the muzzle/eye
        }
        // Recoil is reported and non-zero for both.
        bool recoil = h.recoilPitchDeg > 0.0f && p.recoilPitchDeg > 0.0f;
        wcheck(hitscanOK && projOK && recoil,
               "W6 hitscan -> ray, projectile -> bolt(vel=speed) both resolve");
    }

    // ---- W7: switching mid-reload cancels it; switch-back can't fire early ----
    {
        Arsenal a;
        a.select(0); a.tick(1.0f);
        a.fire(eye, fwd, rng);              // pistol now has mag-1
        a.reload();
        bool wasReloading = a.isReloading();
        a.select(1);                        // switch to SMG mid-reload
        a.select(0);                        // back to pistol
        bool reloadCancelled = wasReloading && !a.isReloading();
        // After the switch the new weapon's cooldown was reset -> immediate fire gated.
        ResolvedFire f = a.fire(eye, fwd, rng);
        wcheck(reloadCancelled && !f.fired,
               "W7 switch cancels reload + resets cooldown (no switch-fire exploit)");
    }

    // ---- W8: ChainGun spin-up — fires (records a shot), the effective cooldown
    // STARTS slow (cold barrel) and gets FASTER as the barrel spins up, and the
    // spin decays when idle. ------------------------------------------------------
    {
        Arsenal a;
        a.selectByName("chaingun");
        a.tick(2.0f);                    // clear the select cooldown; barrel still cold
        const float full = 1.0f / a.def(a.selected()).fireRate;  // hot inter-shot time
        // First (cold) shot: it fires, and its cooldown is LONGER than the hot rate
        // (spin-up start fraction < 1).
        ResolvedFire s1 = a.fire(eye, fwd, rng);
        float coldCd = a.currentState().cooldown;
        bool firedCold   = s1.fired && (int)s1.rays.size() == 1;
        bool coldIsSlow  = coldCd > full + 1e-4f;       // cold cooldown slower than hot
        // Spin the barrel up: fire a sustained burst (advance past each cooldown).
        for (int i = 0; i < 30; ++i) { a.tick(a.currentState().cooldown + 1e-3f); a.fire(eye, fwd, rng); }
        float hotCd = a.currentState().cooldown;
        bool spunUp   = a.currentState().spinUp > 0.9f; // near full spin
        bool hotIsFast = hotCd < coldCd - 1e-4f && std::fabs(hotCd - full) < 5e-3f;
        // Let go: idle long enough for the spin to bleed off.
        a.tick(a.currentState().cooldown + 1e-3f);      // clear cooldown
        a.tick(2.0f);                                    // idle >> spinUpTime
        bool spunDown = a.currentState().spinUp < 0.05f;
        wcheck(firedCold && coldIsSlow && spunUp && hotIsFast && spunDown,
               "W8 chaingun: fires, spins up (cold slow -> hot fast), winds down idle");
    }

    // ---- W9: Plasma Rifle — fires a bolt (projectile) carrying the small splash
    // (radius + splash damage), at the medium fire rate, above the pistol's DPS. --
    {
        Arsenal a;
        a.selectByName("plasma_rifle");
        a.tick(2.0f);                    // clear the select cooldown
        ResolvedFire f = a.fire(eye, fwd, rng);
        bool boltOK = f.fired && f.rays.empty() && (int)f.projectiles.size() == 1;
        bool splashOK = false, velOK = false;
        if (boltOK) {
            const ProjectileSpawn& pj = f.projectiles[0];
            const WeaponDef& d = a.def(a.selected());
            splashOK = pj.splashRadius == d.splashRadius && pj.splashDamage == d.splashDamage &&
                       pj.splashRadius > 0.0f && pj.splashDamage > 0;
            float vlen = std::sqrt(pj.vel.x*pj.vel.x + pj.vel.y*pj.vel.y + pj.vel.z*pj.vel.z);
            velOK = std::fabs(vlen - d.projSpeed) < 0.5f && pj.damage == d.damage &&
                    std::fabs(pj.pos.x - eye.x) < 1e-4f;   // spawns at the muzzle/eye
        }
        // Power ladder: plasma rifle DPS clearly above the pistol's 45 DPS.
        Arsenal b;
        const WeaponDef& pr = a.def(a.indexOf("plasma_rifle"));
        const WeaponDef& ps = b.def(b.indexOf("pistol"));
        float prDps = pr.damage * pr.fireRate, psDps = ps.damage * ps.fireRate;
        bool strongerThanPistol = prDps > psDps;
        wcheck(boltOK && splashOK && velOK && strongerThanPistol,
               "W9 plasma_rifle: fires a splash bolt (vel=speed), DPS > pistol");
    }

    // ---- W10: Lightning Gun — fires a BEAM that chains (primary + chain rays =
    // chainTargets+1 total), every ray flagged beam, chain rays flagged chain, and
    // each carries the per-target damage + falloff threshold. ---------------------
    {
        Arsenal a;
        a.selectByName("lightning");
        a.tick(2.0f);                    // clear the select cooldown
        const WeaponDef& d = a.def(a.selected());
        ResolvedFire f = a.fire(eye, fwd, rng);
        int expect = d.pellets + d.chainTargets;   // 1 + 2 = 3 targets
        bool countOK = f.fired && (int)f.rays.size() == expect && f.projectiles.empty();
        bool allBeam = countOK, dmgOK = countOK, falloffOK = countOK;
        int chainCount = 0, primaryCount = 0;
        if (countOK) {
            for (const auto& ray : f.rays) {
                if (!ray.beam) allBeam = false;
                if (ray.damage != d.damage) dmgOK = false;
                if (ray.falloffStart != d.falloffStart) falloffOK = false;
                if (ray.chain) ++chainCount; else ++primaryCount;
            }
        }
        bool linkOK = primaryCount == d.pellets && chainCount == d.chainTargets;
        wcheck(countOK && allBeam && dmgOK && falloffOK && linkOK,
               "W10 lightning: beam fires, chains to chainTargets+1 rays w/ falloff");
    }

    // ---- W11: the ladder is a clear POWER ordering above the pistol (sustained
    // single-target DPS), without any tier being a trivial one-shot melt. ---------
    {
        Arsenal a;
        auto sustainedDps = [&](const char* nm) -> float {
            const WeaponDef& d = a.def(a.indexOf(nm));
            // chaingun's headline DPS is at full spin; others fire at their rate.
            return (float)d.damage * d.fireRate * (float)(d.pellets > 0 ? d.pellets : 1);
        };
        float pistol  = sustainedDps("pistol");      // 45
        float chain   = sustainedDps("chaingun");
        float prifle  = sustainedDps("plasma_rifle");
        float lightSt = sustainedDps("lightning");   // single-target (chains add more)
        bool abovePistol = chain > pistol && prifle > pistol && lightSt > pistol;
        // None of the three deletes a 200-HP Act-1 boss in a single shot.
        const WeaponDef& cg = a.def(a.indexOf("chaingun"));
        const WeaponDef& pr = a.def(a.indexOf("plasma_rifle"));
        const WeaponDef& lg = a.def(a.indexOf("lightning"));
        int prShot = pr.damage + pr.splashDamage;    // worst-case single plasma bolt
        bool noOneShot = cg.damage < 200 && prShot < 200 && lg.damage < 200;
        wcheck(abovePistol && noOneShot,
               "W11 power ladder: all 3 > pistol DPS, none one-shots a 200-HP boss");
    }

    x3::logInfo(std::string("[weapons-test] ") + std::to_string(w_pass) + " passed, " +
                std::to_string(w_fail) + " failed");
    return w_fail == 0;
}

} // namespace x3::game
