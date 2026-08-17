// Weapon pickup + first-person viewmodel (S5). See app/weapon.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice +
// IPhysicsWorld + Scene interfaces only. No purchased C# copied.
#include "weapon.h"
#include "mesh_prims.h"
// kCryoSlowFactor / kCryoSlowDuration — W17f asserts the engine-side chill constants
// still mirror the FreezeRay's own freezeSlowFactor/freezeDuration (they are set in
// two places because the host cannot thread the per-shot payload; see monster.h).
#include "monster.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
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

// WeaponEnergyPistol.glb's BARREL LINE in its own scene space. The model stands
// on y=0 and is 1.8 units tall; its barrel rides at y=1.603 (MEASURED with
// tools/weapon_muzzle_probe.py). The FP viewmodel anchors on this, not on the
// GLB origin — see the kVmDef* block in weapon.h for why.
constexpr float kRealModelPivotY = 1.603f;

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
        m_vmPivotY   = kRealModelPivotY;   // barrel line, not the model's floor plane
        x3::logInfo("[weapon] loaded WeaponEnergyPistol.glb — " +
                    std::to_string(m_drawables.size()) + " drawable primitive(s)");
    } else {
        // ---- Fallback: a small procedural box so the slice still works. ----
        m_usingReal  = false;
        m_modelScale = kBoxModelScale;
        m_vmPivotY   = 0.0f;   // the box is already authored around its own origin
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
    // distances come from the live-tunable vm_fwd/vm_right/vm_down cvars, and they
    // position the gun's BARREL LINE — the GLB origin is corrected off below.
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

    // BARREL-LINE ANCHOR. WeaponEnergyPistol.glb stands on y=0 with its barrel at
    // the TOP of the model (y = 1.603 of 1.8), so pinning the GLB ORIGIN `down`
    // metres under the eye left the barrel only 0.06 m below eye level — the gun
    // read as held over the player's head. Slide the origin so the point
    // (0, m_vmPivotY, 0) is what `pos` actually places. Identical maths to
    // Arsenal::currentViewmodelFrame; 0 pivot (fallback box) = old behaviour.
    pos.x -= by.x * (m_vmPivotY * m_modelScale);
    pos.y -= by.y * (m_vmPivotY * m_modelScale);
    pos.z -= by.z * (m_vmPivotY * m_modelScale);

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

// Phase B3 — canon 12-weapon key row, in the CANON order from the original
// game's arsenal (see the CANONICAL 12 block below): Pistol · Shotgun ·
// Bazooka(rocket) · Laser · Plasma · ChainGun · LightningGun · RailGun ·
// FlameThrower · NapalmLauncher · FreezeRay · BFG11k. Keys 1..9 map to canon
// slots 1-9; 0 - = map to canon slots 10-12. Names, not roster indices — the
// live arsenal interleaves smg/plasma_rifle (X3Native-only, scroll-wheel only).
const char* canonKeyWeaponName(int i) {
    static const char* kCanonOrder[kCanonKeyCount] = {
        "pistol", "shotgun", "rocket",  "laser",        "plasma",   "chaingun",
        "lightning", "railgun", "flamethrower", "napalm", "freezeray", "bfg11k" };
    return (i >= 0 && i < kCanonKeyCount) ? kCanonOrder[i] : nullptr;
}

std::vector<WeaponDef> makeDefaultRoster() {
    std::vector<WeaponDef> r;

    // ---- 1) Energy Pistol — the starting firearm (have today). --------------
    // Docs: 15 dmg, 3 shots/s, mag 12, reserve 60, reload 1.5 s, 50 m, hitscan.
    {
        WeaponDef w;
        w.name        = "pistol";
        w.kind        = FireKind::Hitscan;
        w.automatic   = false;
        w.damage      = 16;       // +1: a clean 4-shot kill on 60-HP trash
        w.type        = x3::DamageType::Kinetic;
        w.fireRate    = 4.5f;     // snappier semi-auto (was a sluggish 3/s)
        w.pellets     = 1;
        w.spreadDeg   = 0.35f;    // tighter — the sidearm is a precision tool
        w.recoilDeg   = 0.9f;     // a touch less kick so the snappier rate stays controllable
        w.range       = 55.0f;
        w.magSize     = 12;
        w.reserveAmmo = 72;
        w.reloadTime  = 1.3f;     // quick sidearm reload
        w.viewmodelGlb = "WeaponEnergyPistol2.glb";  // real energy_pistol.obj (PBR-textured)
        w.vmScale     = 0.18f;                        // proven pistol read (~0.33 m held)
        w.vmMuzzle    = { -0.003f, 0.652f, 0.855f };  // WeaponEnergyPistol2.glb barrel tip (MEASURED: tools/weapon_muzzle_probe.py)
        w.muzzleFx    = "muzzle_pistol";
        w.impactFx    = "impact_bullet";
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-01.wav";   // punchy single shot
        w.impactSfx   = "weapons/impact/Bullet_Impact_14.wav";               // ballistic impact tick
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
        w.damage      = 11;       // slightly trimmed per-round (rate carries the DPS)
        w.type        = x3::DamageType::Kinetic;
        w.fireRate    = 12.0f;    // ~720 rpm — crisp full-auto
        w.pellets     = 1;
        w.spreadDeg   = 2.2f;     // a bit more bloom so it isn't a laser at range (auto)
        w.recoilDeg   = 0.45f;    // low, controllable kick per shot
        w.range       = 60.0f;
        w.magSize     = 45;
        w.reserveAmmo = 225;
        w.reloadTime  = 1.9f;
        w.viewmodelGlb = "WeaponRailgun.glb";  // railgun reads as a rifle for the auto SMG
        w.vmScale     = 0.24f;                 // longarm (~0.46 m held)
        w.vmMuzzle    = {  0.000f, 0.494f, 0.909f };  // WeaponRailgun.glb barrel tip (measured)
        w.muzzleFx    = "muzzle_smg";
        w.impactFx    = "impact_bullet";
        w.impactSfx   = "weapons/impact/Bullet_Impact_14.wav";   // ballistic impact tick
        // Task #21 FIX B (stoppable loop voice): autos play ONE sustained looping WAV
        // started on the rising edge of held fire and stopped the instant the trigger
        // releases (or on switch/empty/death/menu) — see the fire block in main.cpp.
        // The old per-round one-shot stacked a dozen overlapping reverb tails into a
        // 5-7s roar after release; a single stoppable loop cuts within a frame.
        w.fireSfx     = "weapons/loops/Loopable_Rapid-Fires_Sci-Fi_Gun_7.wav";  // sustained auto loop
        w.fireSfxLoop = true;   // host starts/stops a single loop voice (no per-round one-shot)
        r.push_back(w);
    }

    // ---- 3) Shotgun — pellets + wide spread, close range. -------------------
    // Docs: 20 dmg per pellet x8, 1 shot/s, 15 m. mag 8 / reserve 32 / reload 2.5 s.
    {
        WeaponDef w;
        w.name        = "shotgun";
        w.kind        = FireKind::Hitscan;
        w.automatic   = false;
        w.damage      = 14;       // PER pellet x10 = 140 point-blank (close-range deleter, falloff via range)
        w.type        = x3::DamageType::Kinetic;
        w.fireRate    = 1.2f;     // a touch faster pump
        w.pellets     = 10;       // denser pattern (was 8) so the spread reads as a wall of pellets
        w.spreadDeg   = 6.0f;     // slightly tighter cone -> more pellets connect at usable range
        w.recoilDeg   = 4.5f;     // big satisfying kick
        w.range       = 18.0f;
        w.magSize     = 8;
        w.reserveAmmo = 40;
        w.reloadTime  = 2.3f;
        w.viewmodelGlb = "WeaponShotgun2.glb";  // real shotgun.obj (PBR-textured; long barrel -> small scale)
        w.vmScale     = 0.11f;                   // longest source model (4.4 m) -> ~0.48 m held
        w.vmMuzzle    = { -0.017f, 0.680f, 2.111f };  // WeaponShotgun2.glb barrel tip (measured; a 4.4 m source model — THE worst offender under the old shared guess)
        w.muzzleFx    = "muzzle_shotgun";
        w.impactFx    = "impact_bullet";
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-57.wav";   // heavy single boom
        w.impactSfx   = "weapons/impact/Bullet_Impact_21.wav";               // heavier impact thump
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
        w.type        = x3::DamageType::Energy;
        w.fireRate    = 2.2f;
        w.pellets     = 1;
        w.spreadDeg   = 0.25f;    // tight — an aimed energy bolt
        w.recoilDeg   = 1.3f;
        w.range       = 80.0f;
        w.magSize     = 20;
        w.reserveAmmo = 80;
        w.reloadTime  = 2.1f;
        w.projSpeed   = 55.0f;    // m/s bolt — a hair faster so it leads less at range
        w.viewmodelGlb = "WeaponBFG.glb";  // bfg.obj energy-cannon look for the plasma bolt
        w.vmScale     = 0.24f;             // bulky energy weapon (~0.42 m held)
        w.vmMuzzle    = { -0.003f, 0.528f, 0.864f };  // WeaponBFG.glb emitter tip (measured)
        w.muzzleFx    = "muzzle_plasma";
        w.impactFx    = "impact_plasma";
        // Sci-fi energy single (a distinct, higher-tech single shot).
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-30.wav";
        w.impactSfx   = "weapons/impact/Laser_Impact_Light_6.wav";   // energy splat on impact
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
        w.damage      = 13;
        w.type        = x3::DamageType::Kinetic;
        w.fireRate    = 15.0f;        // ~900 rpm at full spin
        w.pellets     = 1;
        w.spreadDeg   = 3.0f;         // heavy auto bloom — area suppression, not a sniper
        w.recoilDeg   = 0.35f;        // low per-shot kick, but it adds up fast
        w.range       = 70.0f;
        w.magSize     = 100;          // large belt
        w.reserveAmmo = 400;          // large reserve
        w.reloadTime  = 3.0f;         // slow to reload the belt
        w.spinUpTime     = 0.7f;      // 0.7 s of fire to reach full RoF (heavier wind-up read)
        w.spinUpStartFrac= 0.35f;     // starts at 35% of fireRate (cold barrel)
        w.viewmodelGlb = "WeaponRocketLauncher.glb";  // heaviest model -> the chaingun read
        w.vmScale     = 0.26f;                        // heavy weapon (~0.49 m held)
        w.vmMuzzle    = {  0.001f, 0.368f, 0.916f };  // WeaponRocketLauncher.glb barrel tip (measured)
        w.muzzleFx    = "muzzle_chaingun";
        w.impactFx    = "impact_bullet";
        w.impactSfx   = "weapons/impact/Bullet_Impact_14.wav";   // ballistic impact tick
        // Task #21 FIX B (stoppable loop voice): the chaingun's continuous minigun whine
        // is ONE looping WAV the host starts on the rising edge of held fire and stops the
        // instant fire is released (or on switch/empty/death/menu). The old per-round
        // one-shot at ~14 rounds/s stacked a dozen reverb tails into a 5-7s roar after
        // release; the single loop cuts within a frame of letting go.
        w.fireSfx     = "weapons/loops/Loopable_Rapid-Fires_Sci-Fi_Gun_7.wav";  // sustained minigun whine
        w.fireSfxLoop = true;   // host starts/stops a single loop voice (no per-round one-shot)
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
        w.type        = x3::DamageType::Energy;
        w.fireRate    = 4.5f;         // medium-fast RoF (bolt stream)
        w.pellets     = 1;
        w.spreadDeg   = 0.5f;
        w.recoilDeg   = 0.7f;
        w.range       = 90.0f;
        w.magSize     = 30;
        w.reserveAmmo = 120;
        w.reloadTime  = 2.2f;
        w.projSpeed   = 72.0f;        // faster, flatter bolt than the plasma pistol
        w.splashRadius= 2.2f;         // small AoE on impact
        w.splashDamage= 15;           // splash damage at the center
        w.viewmodelGlb = "WeaponBFG.glb";  // bfg.obj energy-cannon look (shared with plasma)
        w.vmScale     = 0.24f;             // bulky energy weapon (~0.42 m held)
        w.vmMuzzle    = { -0.003f, 0.528f, 0.864f };  // WeaponBFG.glb emitter tip (measured)
        w.muzzleFx    = "muzzle_plasma";
        w.impactFx    = "impact_plasma";
        // Bigger energy single than the plasma pistol (largest single = fuller bolt).
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-66.wav";
        w.impactSfx   = "weapons/impact/Laser_Impact_Light_6.wav";   // energy splat on impact
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
        // R-1 (ATTENTION_FableAAA fold): TIM'S LIVE PLAYTEST TUNE from the
        // playable-build line — heavier tick (14), slower cadence (8/s), a real
        // charge cell (200/600). Preserved from the aborted merge; lands first.
        w.damage      = 14;                  // per beam tick / per chained target
        w.type        = x3::DamageType::Energy;
        w.fireRate    = 8.0f;                // Tim's tune: weightier, less buzzsaw
        w.pellets     = 1;                   // 1 primary; chains add rays
        w.spreadDeg   = 0.0f;                // a beam is dead-accurate
        // TIM'S LIVE TUNE (33530f5) KEPT: recoil 0.12, range 30 (the R-1 fold values),
        // alongside damage 14 / fireRate 8 / DamageType::Energy above.
        w.recoilDeg   = 0.12f;               // almost none (steady beam)
        w.range       = 30.0f;               // SHORT range (slightly extended)
        // CHARGE model (Tim spec): no magazine, no reload. A full charge is a pool of
        // SECONDS OF BEAM, not a count of shots — drain is per-SECOND and continuous
        // while the beam is held (Arsenal::tick), and fire() consumes NO charge. So the
        // economy is independent of fireRate: sustainedSeconds = chargeMax / drainPerSec.
        //
        // TIM'S CALL: "Make the charge last for 3 min of sustained fire."
        //   full charge (100) / 180 s  =>  drain 0.5556 /s   (was 10/s = a 10 s pool)
        //   battery-stacked cap (300)  =>  540 s (9 min) — the crystal cells are a real
        //   stockpile you bank, not a chore you re-run every ten seconds.
        // Gated by --test-lightning-charge LC7/LC8, which SIMULATE continuous fire and
        // assert the observed duration is 180 s +/-5 (and 540 s +/-15 at the cap), with
        // a negative control proving the probe rejects the old 10/s drain.
        w.usesCharge  = true;
        w.chargeMax   = 100.0f;                        // base full charge
        w.chargeCap   = 300.0f;                        // battery-stacking ceiling
        w.chargeDrainPerSec = 100.0f / 180.0f;         // 3 MINUTES of sustained fire
        // PASSIVE REGEN — TIM'S CALL: "lets let the lightning recharge when not in use",
        // refined to "regen all the way, but half speed over 150".
        //   * 2 s after firing STOPS, the pool starts refilling (firing RESETS that
        //     timer, so a burst can't free-refill mid-fight — you must let it cool).
        //   * FAST band (< 150): chargeMax / 60 = 1.667 /s — the base 100 comes back
        //     from empty in ~60 s. 180 s of fire therefore costs 60 s of waiting (3x
        //     the drain rate): generous, not free. You are never stranded with a dead
        //     gun; the lightning is never dead weight.
        //   * SLOW band (>= 150): HALF that (0.833 /s) — a long crawl up to the 300 cap.
        //   * Ceiling is the CAP (300), reached from empty in ~90 s + ~180 s = ~270 s.
        // The crystal BATTERY CELLS keep their weight because they let you SKIP the slow
        // crawl — they are the FAST way to a stocked gun, not the only way.
        // Gated by --test-lightning-charge LC9..LC13, which MEASURE both band rates
        // separately (an endpoint-only probe would pass on a single wrong-but-averaging
        // rate) with a negative control proving a uniform (un-halved) rate is REJECTED.
        w.chargeRegenPerSec   = 100.0f / 60.0f;        // fast band: base refills in 60 s
        w.chargeRegenDelay    = 2.0f;                  // cool-down beat after firing stops
        w.chargeRegenTo       = 300.0f;                // regen ceiling = the CAP
        w.chargeRegenSlowAbove= 150.0f;                // half-speed above this
        w.chargeRegenSlowMult = 0.5f;
        // ⚠ DEAD FOR THIS WEAPON. Under usesCharge the Lightning Gun has no magazine and
        // no reserve: canFire()/fire()/reload() never read these. They are zeroed rather
        // than left at Tim's old 200/600 cell precisely because that ambiguity is what
        // made the HUD print "200 / 600" for a gun that had NEITHER. The mag model is
        // gone; the charge pool above is the whole economy.
        w.magSize     = 0;                             // (dead under usesCharge)
        w.reserveAmmo = 0;                             // (dead under usesCharge)
        w.reloadTime  = 0.0f;                          // (dead under usesCharge)
        w.beam        = true;                // render as a solid beam (host hint)
        w.chainTargets= 2;                   // primary + 2 chains = 3 targets
        w.falloffStart= 15.0f;               // half-range: damage falls off past 15 m
        w.viewmodelGlb = "WeaponRailgun.glb"; // railgun rifle for the precision beam
        w.vmScale     = 0.24f;                // longarm (~0.46 m held)
        w.vmMuzzle    = {  0.000f, 0.494f, 0.909f };  // WeaponRailgun.glb barrel tip (measured)
        w.muzzleFx    = "muzzle_lightning";
        w.impactFx    = "impact_lightning";
        w.impactSfx   = "weapons/impact/Laser_Impact_Light_6.wav";   // electric splat on impact
        // Continuous electric ZAP (Vefects Zap pack) — looped while the beam is held so
        // it reads as a sustained lightning crackle (not a generic gun loop).
        w.fireSfx     = "weapons/loops/Vefects_Zap_Medium_01.wav";
        w.fireSfxLoop = true;   // continuous beam: loopable
        r.push_back(w);
    }

    // ---- 8) Rocket Launcher — the first EXPLOSIVE-type weapon. -------------
    // Heavy projectile with a large impact radius. The canon-aliens DamageType
    // table needs an Explosive entry; until a future grenade/cryo row is added,
    // the rocket is the only weapon that stamps Explosive on every shot. Bosses
    // that opt into adaptiveHideResist against Explosive (none today) would
    // resist after the first hit + force the player to rotate. Tuning: 80 direct
    // + 60 splash @ 4 m, mag 4 / reserve 16, slow projectile so the bolt reads.
    // At 0.8/s sustained DPS is ~64 direct + ~48 splash; a 400-HP boss is still
    // ~3 rockets (no one-shot), holding the W11 power-ladder invariant.
    {
        WeaponDef w;
        w.name        = "rocket";
        w.kind        = FireKind::Projectile;
        w.automatic   = false;
        w.damage      = 80;                   // direct-hit damage
        w.type        = x3::DamageType::Explosive;
        w.fireRate    = 0.8f;                 // slow heavy weapon
        w.pellets     = 1;
        w.spreadDeg   = 0.4f;
        w.recoilDeg   = 5.0f;                 // big kick
        w.range       = 100.0f;
        w.magSize     = 4;                    // small tube
        w.reserveAmmo = 16;
        w.reloadTime  = 3.5f;
        w.projSpeed   = 30.0f;                // slow rocket — readable bolt
        w.splashRadius= 4.0f;                 // large AoE
        w.splashDamage= 60;                   // significant splash
        w.viewmodelGlb = "WeaponRocketLauncher.glb"; // dedicated launcher mesh
        w.vmScale     = 0.26f;                // heavy weapon
        w.vmMuzzle    = {  0.001f, 0.368f, 0.916f };  // WeaponRocketLauncher.glb barrel tip (measured)
        w.muzzleFx    = "muzzle_rocket";
        w.impactFx    = "impact_explosion";
        // Heavy boom on launch (one-shot).
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-57.wav";
        r.push_back(w);
    }

    // =====================================================================
    // THE CANONICAL 12 (2026-08). The original game's arsenal is 12 weapons:
    //   Pistol · Shotgun · Bazooka · Laser · Plasma · ChainGun · LightningGun ·
    //   RailGun · FlameThrower · NapalmLauncher · FreezeRay · BFG11k
    // Eight of those already had a slot above (the Bazooka is slot 8 "rocket" —
    // there is deliberately NO second rocket weapon), plus two X3Native-only
    // weapons that are NOT in the canon 12 and are kept as-is (smg, plasma_rifle).
    // The six below close the gap: Laser, RailGun, FlameThrower, NapalmLauncher,
    // FreezeRay, BFG11k.
    //
    // PROVENANCE — every stat is harvested from the C++ port of the original game at
    // D:\GameDev\EscapeLab3D (read-only), specifically src/game/weapon.cpp
    // InitWeaponDefs() and src/game/game_types.h WEAPON_STATS/DEFAULT_MAX_AMMO. That
    // port is the design authority for the canon roster.
    //
    // UNIT CONVERSION (the port is a 2D game in pixel-ish units; X3Native is metric).
    // The scale factors are derived from the weapons the two rosters already SHARE,
    // not invented:
    //   * DAMAGE  ~1:1  — port Pistol 15 vs ours 16; port Bazooka 75 direct vs our
    //                     rocket 80. Damage numbers therefore carry over directly.
    //   * RADIUS  x0.05 — port Bazooka explosionRadius 80 vs our rocket splashRadius 4.0 m.
    //   * SPEED   x0.05 — port Bazooka projectileSpeed 600 vs our rocket projSpeed 30 m/s.
    //   * RANGE   x0.022 — port Pistol maxRange 2500 vs our pistol range 55 m.
    // Where a converted range would be absurd for the facility's interiors (the
    // RailGun's 10000 -> 220 m) it is clamped and the clamp is called out at the site.
    // =====================================================================

    // ---- 9) Laser — the canon CONTINUOUS beam. ------------------------------
    // Port: 35 dmg/tick, fireRate 60 ("fires every frame"), isHitscan + isContinuous,
    // maxRange 3000, ammo 200, blue. This is the roster's precision sustained-damage
    // tool: dead accurate, no travel time, no chaining — it just does not stop while
    // you hold it. That is what separates it from the Lightning Gun (which chains but
    // zaps discretely and dies at 30 m).
    //
    // The port's literal 60 shots/s at 35 dmg = 2100 DPS, which would delete a 400-HP
    // Act-1 boss in 0.19 s and break the W11 power-ladder invariant every other weapon
    // is tuned against. The CONTINUOUS FEEL is the canon intent, not that DPS, so the
    // tick is cut to 9 at 20/s = 180 sustained DPS — a clear step over the pistol (72)
    // and in the same band as the chaingun (~195), with the beam read preserved by
    // `continuous` + a fire rate fast enough to look unbroken.
    {
        WeaponDef w;
        w.name        = "laser";
        w.kind        = FireKind::Hitscan;   // port: isHitscan
        w.automatic   = true;                // held = keeps firing
        w.damage      = 9;                   // per beam tick (port 35/tick at 60/s rescaled)
        w.type        = x3::DamageType::Energy;   // x3_damage.h lists "laser" under Energy
        w.fireRate    = 20.0f;               // fast enough to read as unbroken
        w.pellets     = 1;
        w.spreadDeg   = 0.0f;                // port spread 0.0 — a beam is dead-accurate
        w.recoilDeg   = 0.08f;               // essentially none (steady beam)
        w.range       = 66.0f;               // port 3000 x0.022
        w.magSize     = 200;                 // port DEFAULT_MAX_AMMO[Laser] = 200
        w.reserveAmmo = 400;
        w.reloadTime  = 2.4f;
        w.beam        = true;                // render as a solid beam (host hint)
        w.continuous  = true;                // port isContinuous: ONE unbroken beam
        w.viewmodelGlb = "WeaponEnergyPistol2.glb";  // compact emitter reads as a beam gun
        w.vmScale     = 0.20f;
        w.vmMuzzle    = { -0.003f, 0.652f, 0.855f };  // WeaponEnergyPistol2.glb barrel tip (MEASURED: tools/weapon_muzzle_probe.py)
        w.muzzleFx    = "muzzle_lightning";  // closest existing kind: electric-blue energy
        w.impactFx    = "impact_lightning";
        w.fireSfx     = "weapons/loops/Vefects_Zap_Medium_01.wav";  // sustained beam tone
        w.fireSfxLoop = true;                // continuous beam: ONE loop voice
        w.impactSfx   = "weapons/impact/Laser_Impact_Light_6.wav";
        r.push_back(w);
    }

    // ---- 10) RailGun — the canon PIERCING slug. ----------------------------
    // Port: 300 dmg, fireRate 0.5, isHitscan, penetrates = true ("through ALL
    // enemies"), maxRange 10000, ammo 15, white. The sniper: one deliberate,
    // expensive shot that skewers a whole line of enemies.
    //
    // Damage 300 carries over 1:1 (the port's damage scale matches ours). The port's
    // "penetrates ALL" is expressed as pierceTargets = 6 extra bodies, which is every
    // enemy a facility corridor can physically line up and gives the host a bound to
    // walk instead of an unbounded loop. Range is CLAMPED from the converted 220 m to
    // 150 m — still by far the longest reach in the roster (next is the rocket at
    // 100 m) but not larger than the streamed world the ray would have to resolve in.
    {
        WeaponDef w;
        w.name        = "railgun";
        w.kind        = FireKind::Hitscan;
        w.automatic   = false;
        w.damage      = 300;                 // port: 300 (1:1 damage scale)
        w.type        = x3::DamageType::Energy;   // x3_damage.h lists "railgun" under Energy
        w.fireRate    = 0.5f;                // port: 0.5 shots/s — one deliberate shot
        w.pellets     = 1;
        w.spreadDeg   = 0.0f;                // port spread 0.0 — perfect accuracy
        w.recoilDeg   = 5.5f;                // heaviest kick in the roster
        w.range       = 150.0f;              // port 10000 x0.022 = 220 m, CLAMPED to 150 m
        w.magSize     = 5;
        w.reserveAmmo = 15;                  // port DEFAULT_MAX_AMMO[RailGun] = 15
        w.reloadTime  = 3.2f;                // slow, deliberate reload
        w.pierceTargets = 6;                 // port penetrates=true ("through ALL enemies")
        w.viewmodelGlb = "WeaponRailgun.glb"; // the purpose-built railgun mesh, finally on the railgun
        w.vmScale     = 0.24f;                // longarm (~0.46 m held)
        w.vmMuzzle    = {  0.000f, 0.494f, 0.909f };  // WeaponRailgun.glb barrel tip (MEASURED: tools/weapon_muzzle_probe.py)
        w.muzzleFx    = "muzzle_rocket";     // big bright discharge (no dedicated rail kind)
        w.impactFx    = "impact_plasma";
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-66.wav";  // heaviest single crack
        w.impactSfx   = "weapons/impact/Laser_Impact_Light_6.wav";
        r.push_back(w);
    }

    // ---- 11) FlameThrower — the canon short-range burn cone. ---------------
    // Port: 12 dmg/tick, fireRate 30, pelletsPerShot 3 ("3 flame puffs per tick"),
    // spread 12 deg, maxRange 300 (the shortest in the game), appliesBurn = true,
    // isContinuous, ammo 250, gravityScale -0.2 ("flames rise slightly").
    //
    // 2026-08-15 (weapon-feel lane) — NOW A REAL PROJECTILE STREAM. The roster lane
    // shipped this as a multi-ray hitscan cone and said so plainly, because
    // Arsenal::fire could only push a SINGLE ProjectileSpawn: a 3-puff burst was not
    // expressible. That producer limit is gone — a Projectile weapon now emits
    // `pellets` spawns — so the port's shape is reproduced honestly: 3 travelling
    // FlamePuffs per tick, each with its own draw from the 12-degree cone, its own
    // staggered despawn range, and the burn DOT.
    //
    // ⚠ COUPLED TO PHASE B2. The host has two consumers of ResolvedFire::projectiles;
    // app_run.cpp:7148 already loops correctly, but the LIVE fire path at :10935 still
    // reads projectiles[0] and drops the rest. Until that one-site fix lands (it is
    // blocked on inspx/la-exe) this weapon puts 1 puff in the air per tick instead of 3.
    // The producer is correct; the arithmetic is stated here rather than hidden.
    {
        WeaponDef w;
        w.name        = "flamethrower";
        w.kind        = FireKind::Projectile; // port isHitscan = false: travelling FlamePuffs
        w.automatic   = true;                // held = continuous stream
        w.damage      = 6;                   // per puff; x3 puffs x 12/s = ~216 DPS point-blank
        w.type        = x3::DamageType::Bio; // closest existing tag for chemical fuel burn
        w.fireRate    = 12.0f;               // port 30/s rescaled to keep the DPS in-band
        w.pellets     = 3;                   // port pelletsPerShot = 3 flame puffs
        w.spreadDeg   = 12.0f;               // port spread = 12 deg — a wide cone
        w.recoilDeg   = 0.05f;               // almost none (a fuel stream, not a gun)
        w.range       = 9.0f;                // port 300 x0.022 = 6.6 m, opened slightly to 9 m
        w.magSize     = 250;                 // port DEFAULT_MAX_AMMO[FlameThrower] = 250
        w.reserveAmmo = 500;
        w.reloadTime  = 3.0f;                // swapping a fuel canister is slow
        w.projSpeed   = 30.0f;               // port projectileSpeed 600 x0.05 — a puff crosses 9 m in 0.3 s
        // Port gravityScale -0.2 ("flames rise slightly") x GRAVITY 800 x0.05 = -8 m/s^2.
        // NEGATIVE = the puffs drift UPWARD, which is what fire does. Same harvest rule
        // as napalm's +32; see WeaponDef::projectileGravity.
        w.projectileGravity = -8.0f;         // HARVESTED: flames rise
        w.continuous  = true;                // port isContinuous
        w.burnDuration= 3.0f;                // port appliesBurn — the DOT is the point
        w.burnDps     = 8;
        w.viewmodelGlb = "WeaponRocketLauncher.glb";  // bulky tube/nozzle reads as a projector
        w.vmScale     = 0.24f;
        w.vmMuzzle    = {  0.001f, 0.368f, 0.916f };  // WeaponRocketLauncher.glb barrel tip (MEASURED: tools/weapon_muzzle_probe.py)
        w.muzzleFx    = "muzzle_rocket";     // hot orange discharge (nearest existing kind)
        w.impactFx    = "impact_explosion";
        w.fireSfx     = "weapons/loops/Loopable_Rapid-Fires_Sci-Fi_Gun_7.wav";
        w.fireSfxLoop = true;                // continuous stream: ONE loop voice
        r.push_back(w);
    }

    // ---- 12) Napalm Launcher — the canon AREA-DENIAL weapon. ---------------
    // Port: 120 dmg, fireRate 0.8, explosionRadius 120, projectileSpeed 700,
    // gravityScale 0.8 ("heavy arcing trajectory"), firePoolDuration 5.0 s,
    // firePoolDPS 25, appliesBurn, ammo 10.
    //
    // This is the weapon that is NOT just "a second rocket": the blast is the opener,
    // and the BURNING GROUND POOL it leaves behind is the actual weapon. It denies a
    // corridor for 5 seconds. Direct 120 + 70 splash over 6 m, then 25 DPS standing
    // fire in a 3 m pool.
    //
    // THE ARC (2026-08-15 weapon-feel lane) — HARVESTED, not chosen. The port's
    // `gravityScale = 0.8f  // heavy arcing trajectory` runs against its fixed
    // GRAVITY = 800 port-units/s^2 (src/game/projectile.cpp ApplyGravity), i.e. 640
    // port-units/s^2 of real acceleration. An acceleration converts on the LENGTH
    // ratio, which for this roster is the same x0.05 the projectile SPEEDS used
    // (port 700 -> our 35 m/s), giving 640 x 0.05 = 32.0 m/s^2.
    //
    // The number is independently CORROBORATED by its own ballistics rather than
    // taken on faith: a 45-degree lob at this weapon's 35 m/s reaches v^2/g =
    // 1225/32 = 38 m, which sits just inside its authored 44 m despawn range. That
    // is the signature of a properly-tuned lobbed weapon — you must arc it to reach
    // the far end of its envelope. (For contrast, at Earth's 9.81 the same shell
    // would carry 125 m, so no arc would ever be visible inside 44 m and the weapon
    // would simply read as a slower rocket.)
    //
    // NOTE the value is carried onto every ProjectileSpawn correctly, but the host's
    // live-projectile integrator does not yet READ it — that is app/app_run.cpp,
    // frozen behind inspx/la-exe, and is Phase B1 of this lane's plan.
    {
        WeaponDef w;
        w.name        = "napalm";
        w.kind        = FireKind::Projectile;
        w.automatic   = false;
        w.damage      = 120;                 // port: 120 direct (1:1 damage scale)
        w.type        = x3::DamageType::Explosive;
        w.fireRate    = 0.8f;                // port: 0.8 shots/s
        w.pellets     = 1;
        w.spreadDeg   = 0.0f;                // port spread 0.0
        w.recoilDeg   = 4.0f;
        w.range       = 44.0f;               // port 2000 x0.022
        w.magSize     = 4;
        w.reserveAmmo = 10;                  // port DEFAULT_MAX_AMMO[NapalmLauncher] = 10
        w.reloadTime  = 3.4f;
        w.projSpeed   = 35.0f;               // port 700 x0.05
        w.projectileGravity = 32.0f;         // port gravityScale 0.8 x GRAVITY 800 x0.05 — HARVESTED
        w.splashRadius= 6.0f;                // port explosionRadius 120 x0.05
        w.splashDamage= 70;
        w.firePoolDuration = 5.0f;           // port firePoolDuration = 5.0 s — AREA DENIAL
        w.firePoolDps      = 25;             // port firePoolDPS = 25
        w.firePoolRadius   = 3.0f;           // the standing-fire footprint
        w.burnDuration= 4.0f;                // port appliesBurn
        w.burnDps     = 10;
        w.viewmodelGlb = "WeaponRocketLauncher.glb";  // a launcher tube — literally the right read
        w.vmScale     = 0.26f;
        w.vmMuzzle    = {  0.001f, 0.368f, 0.916f };  // WeaponRocketLauncher.glb barrel tip (MEASURED: tools/weapon_muzzle_probe.py)
        w.muzzleFx    = "muzzle_rocket";
        w.impactFx    = "impact_explosion";
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-57.wav";
        r.push_back(w);
    }

    // ---- 13) Freeze Ray — the canon CONTROL weapon. ------------------------
    // Port: 5 dmg, fireRate 20, pelletsPerShot 4 ("3-5 crystalline particles"),
    // spread 10 deg, maxRange 400, appliesFreeze = true, isContinuous, ammo 120.
    // The port's own comment says it plainly: "5 damage/tick but the real power is
    // the freeze". This is a CONTROL tool, not a damage tool — it is the only weapon
    // in the roster that makes an enemy stop being a threat without killing it.
    //
    // 2026-08-15 (weapon-feel lane) — a REAL crystalline stream, and it finally CHILLS.
    // Two things changed together, and neither was reachable from config:
    //   (1) kind is now Projectile, so the port's 4 travelling particles are emitted as
    //       4 spawns instead of collapsing to one (see the flamethrower's note, incl.
    //       the same Phase-B2 host coupling: 1 of 4 reaches the air until :10935 loops).
    //   (2) type is DamageType::Cryo, a NEW enum row. That tag is the mechanism, not a
    //       label: MonsterSystem::onDamaged reads it and applies the timed chase-speed
    //       slow, so the freeze is now a thing that HAPPENS to an enemy rather than a
    //       number sitting unread on a ray. Before this it was tagged Energy — which
    //       also meant it shared an Adaptive-Hide resist window with the laser, railgun,
    //       plasma and BFG, so hosing a Warlord built the WRONG resist.
    // The freeze is modelled as a slow rather than a hard stun: freezeSlowFactor 0.35 =
    // down to 35% move speed for 2.5 s, refreshed by every tick you hold it on target.
    {
        WeaponDef w;
        w.name        = "freezeray";
        w.kind        = FireKind::Projectile; // port isHitscan = false: crystalline particles
        w.automatic   = true;                // held = continuous cone
        w.damage      = 5;                   // port: 5 — deliberately feeble, the slow is the payload
        w.type        = x3::DamageType::Cryo;    // THE mechanism — drives MonsterSystem's slow
        w.fireRate    = 12.0f;               // port 20/s eased slightly
        w.pellets     = 4;                   // port pelletsPerShot = 4 crystalline particles
        w.spreadDeg   = 10.0f;               // port spread = 10 deg
        w.recoilDeg   = 0.05f;
        w.range       = 12.0f;               // port 400 x0.022 = 8.8 m, opened slightly to 12 m
        w.magSize     = 120;                 // port DEFAULT_MAX_AMMO[FreezeRay] = 120
        w.reserveAmmo = 240;
        w.reloadTime  = 2.6f;
        w.projSpeed   = 35.0f;               // port projectileSpeed 700 x0.05
        // Port gravityScale 0.0 — ice crystals fly flat. Left at the 0 default DELIBERATELY
        // (stated, not omitted): this weapon harvests a zero, it does not merely lack a value.
        w.continuous  = true;                // port isContinuous
        w.freezeDuration   = 2.5f;           // port appliesFreeze — THE payload
        w.freezeSlowFactor = 0.35f;          // down to 35% move speed while frozen
        w.viewmodelGlb = "WeaponBFG.glb";    // wide emitter aperture reads as a projector
        w.vmScale     = 0.22f;
        w.vmMuzzle    = { -0.003f, 0.528f, 0.864f };  // WeaponBFG.glb emitter tip (MEASURED: tools/weapon_muzzle_probe.py)
        w.muzzleFx    = "muzzle_plasma";     // cool-tint energy flash (nearest existing kind)
        w.impactFx    = "impact_plasma";
        w.fireSfx     = "weapons/loops/Vefects_Zap_Medium_01.wav";
        w.fireSfxLoop = true;                // continuous cone: ONE loop voice
        w.impactSfx   = "weapons/impact/Laser_Impact_Light_6.wav";
        r.push_back(w);
    }

    // ---- 14) BFG 11k — the canon ultimate weapon. --------------------------
    // Port: 150 direct dmg, fireRate 0.3, projectileSpeed 350 ("slow, menacing"),
    // explosionRadius 100, secondaryBolts 8, ammoPerShot 5, maxAmmo 5, BFG-green.
    // One full pickup is exactly ONE shot — that scarcity IS the weapon's design.
    //
    // Kept as the top of the ladder without trivializing it: 150 direct + 90 splash
    // over 5 m, and at 5 rounds per shot on a 5-round magazine you fire once and then
    // hunt for more. The 8 secondary bolts at detonation are what make it read as the
    // BFG rather than "a bigger rocket".
    {
        WeaponDef w;
        w.name        = "bfg11k";
        w.kind        = FireKind::Projectile;
        w.automatic   = false;
        w.damage      = 150;                 // port: 150 direct (1:1 damage scale)
        w.type        = x3::DamageType::Energy;   // x3_damage.h lists "BFG" under Energy
        w.fireRate    = 0.3f;                // port: 0.3 shots/s
        w.pellets     = 1;
        w.spreadDeg   = 0.0f;                // port spread 0.0
        w.recoilDeg   = 6.0f;                // the biggest shove in the game
        w.range       = 66.0f;               // port 3000 x0.022
        w.magSize     = 5;                   // port DEFAULT_MAX_AMMO[BFG11k] = 5
        w.reserveAmmo = 10;
        w.reloadTime  = 4.0f;                // slowest reload in the roster
        w.ammoPerShot = 5;                   // port ammoPerShot = 5 — one pickup, one shot
        w.projSpeed   = 17.5f;               // port 350 x0.05 — slow and menacing
        w.splashRadius= 5.0f;                // port explosionRadius 100 x0.05
        w.splashDamage= 90;                  // devastating blast at the center
        w.secondaryBolts = 8;                // port secondaryBolts = 8
        w.viewmodelGlb = "WeaponBFG.glb";    // the purpose-built BFG mesh, finally on the BFG
        w.vmScale     = 0.26f;               // bulkiest energy cannon read
        w.vmMuzzle    = { -0.003f, 0.528f, 0.864f };  // WeaponBFG.glb emitter tip (MEASURED: tools/weapon_muzzle_probe.py)
        w.muzzleFx    = "muzzle_plasma";     // green-ish energy discharge
        w.impactFx    = "impact_explosion";
        w.fireSfx     = "weapons/single/Single_Gunshot_Sci-Fi_Gun-66.wav";
        w.impactSfx   = "weapons/impact/Laser_Impact_Light_6.wav";
        r.push_back(w);
    }

    // W2-C: every weapon shares the repo-local reload + dry-fire WAVs (committed by
    // the sound department under assets/audio/weapons/; resolveAudio silent-skips
    // if absent). Per-weapon bespoke reload foley can override these later.
    for (WeaponDef& w : r) {
        w.reloadSfx  = "weapons/reload_generic.wav";
        w.dryfireSfx = "weapons/dryfire_click.wav";
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
        m_state[i].charge      = m_defs[i].usesCharge ? m_defs[i].chargeMax : 0.0f;
    }
    if (m_defs.empty()) m_sel = -1; else m_sel = 0;

    // FP viewmodel LENS levers, seeded from the environment so they are reachable
    // from a command line with no rebuild (and without plumbing new cvars through
    // app_run.cpp, which the EXE split is about to rewrite). See weapon.h.
    auto envF = [](const char* k, float dflt) {
        const char* v = std::getenv(k);
        if (!v || !*v) return dflt;
        char* end = nullptr;
        const float f = std::strtof(v, &end);
        return (end && end != v) ? f : dflt;
    };
    setViewmodelLens(envF("X3_VM_FOV",      kVmDefFovDeg),
                     envF("X3_VM_WORLDFOV", kVmWorldFovDeg),
                     envF("X3_VM_SCALE",    1.0f));
    if (m_vmFovDeg > 0.0f || m_vmScaleMul != 1.0f)
        x3::logInfo("[arsenal] viewmodel lens: vm_fov=" + std::to_string(m_vmFovDeg) +
                    " world_fov=" + std::to_string(m_vmWorldFovDeg) +
                    " scale_mul=" + std::to_string(m_vmScaleMul) +
                    " -> magnification " + std::to_string(viewmodelMagnification()));
}

void Arsenal::setViewmodelLens(float vmFovDeg, float worldFovDeg, float scaleMul) {
    // Clamp to sane lenses so a fat-fingered value can't produce a degenerate or
    // negative magnification (tan blows up at 180 deg and flips past it).
    m_vmFovDeg      = (vmFovDeg   > 0.0f) ? std::fmin(vmFovDeg,   179.0f) : 0.0f;
    m_vmWorldFovDeg = (worldFovDeg > 1.0f) ? std::fmin(worldFovDeg, 179.0f) : kVmWorldFovDeg;
    m_vmScaleMul    = (scaleMul   > 0.01f) ? std::fmin(scaleMul,   10.0f)  : 1.0f;
}

float Arsenal::viewmodelMagnification() const {
    if (m_vmFovDeg <= 0.0f) return 1.0f;   // share the world lens
    const float halfV = m_vmFovDeg      * 0.5f * (kPi / 180.0f);
    const float halfW = m_vmWorldFovDeg * 0.5f * (kPi / 180.0f);
    const float tv = std::tan(halfV);
    if (!(tv > 1e-4f)) return 1.0f;
    return std::tan(halfW) / tv;
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
    const WeaponDef&   d = m_defs[(size_t)m_sel];
    if (s.cooldown    > 0.0f) return false;   // fire-rate gate
    if (d.usesCharge) {
        // CHARGE weapon (Lightning): no mag / no reload — fire while charge remains
        // (IDKFA bypasses). The continuous drain happens in tick() while beam held.
        return m_infiniteAmmo || s.charge > 0.0f;
    }
    if (s.reloadTimer > 0.0f) return false;   // mid-reload: can't fire
    if (s.ammoInMag  <= 0 && !m_infiniteAmmo) return false;   // empty mag (IDKFA bypasses)
    return true;
}

ResolvedFire Arsenal::fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, uint32_t& rngState) {
    ResolvedFire out;
    if (!canFire()) {
        // W2-C dry-fire: flag the EMPTY-MAG case specifically (cooldown elapsed,
        // not mid-reload) so the host plays dryfireSfx. The click ARMS the normal
        // fire-rate cooldown (no ammo consumed) — without that, holding fire on an
        // auto weapon would click every frame instead of at trigger cadence.
        if (m_sel >= 0) {
            WeaponState& s = m_state[(size_t)m_sel];
            if (s.cooldown <= 0.0f && s.reloadTimer <= 0.0f &&
                s.ammoInMag <= 0 && !m_infiniteAmmo) {
                out.dryFire = true;
                const WeaponDef& d = m_defs[(size_t)m_sel];
                s.cooldown = (d.fireRate > 0.0f) ? (1.0f / d.fireRate) : 0.25f;
            }
        }
        return out;                           // gated -> nothing consumed
    }

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
    // CHARGE weapons (Lightning) do NOT consume a mag round — their charge pool is
    // drained continuously in tick() while the beam is held (see setBeamHeld).
    // CANON-12: a shot may cost more than one round (the BFG eats 5 of its 5). Clamp at
    // 0 so a partially-stocked mag still fires its last shot rather than going negative.
    const int roundsPerShot = (d.ammoPerShot > 1) ? d.ammoPerShot : 1;
    if (!m_infiniteAmmo && !d.usesCharge) {
        s.ammoInMag -= roundsPerShot;   // IDKFA: never deplete
        if (s.ammoInMag < 0) s.ammoInMag = 0;
    }
    // PASSIVE REGEN: pulling the trigger INTERRUPTS regen and restarts the cool-down
    // beat. tick() also does this off m_beamHeld (the host's held-fire flag), but doing
    // it here too means the rule holds for ANY fire path — a caller that fires without
    // ever setting beamHeld (tests, scripted/AI fire) cannot regen through its own shots.
    if (d.usesCharge) s.regenDelay = d.chargeRegenDelay;
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
            ray.type         = d.type;          // canon-aliens Adaptive-Hide tag
            // CANON-12 payloads: railgun pierce, laser continuity, flame burn, freeze slow.
            ray.pierceTargets    = d.pierceTargets;
            ray.continuous       = d.continuous;
            ray.burnDuration     = d.burnDuration;
            ray.burnDps          = d.burnDps;
            ray.freezeDuration   = d.freezeDuration;
            ray.freezeSlowFactor = d.freezeSlowFactor;
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
            ray.type         = d.type;          // chain rays inherit the firing weapon's type
            // Chain links inherit the firing weapon's status payloads too.
            ray.continuous       = d.continuous;
            ray.burnDuration     = d.burnDuration;
            ray.burnDps          = d.burnDps;
            ray.freezeDuration   = d.freezeDuration;
            ray.freezeSlowFactor = d.freezeSlowFactor;
            out.rays.push_back(ray);
        }
    } else { // Projectile
        // ---- MULTI-SPAWN (2026-08-15 weapon-feel lane) -----------------------
        // A Projectile weapon emits `pellets` spawns, exactly as a Hitscan weapon
        // emits `pellets` rays. That ONE rule covers both shapes with no per-weapon
        // special-casing and no name checks:
        //   * every AIMED projectile weapon (plasma, plasma_rifle, rocket, napalm,
        //     bfg11k) is authored pellets = 1, so it emits exactly ONE spawn and is
        //     bit-identical to the pre-lane single-push below it. W17d asserts that.
        //   * a STREAM weapon (flamethrower 3 puffs, freezeray 4 crystals) emits its
        //     whole cone as travelling particles instead of collapsing to one bolt.
        // N is clamped to kMaxStreamSpawns (see weapon.h for why that bound, and for
        // why holding the trigger cannot grow anything without limit).
        int n = (d.pellets > 0) ? d.pellets : 1;
        if (n > kMaxStreamSpawns) n = kMaxStreamSpawns;
        out.projectiles.reserve((size_t)n);
        for (int p = 0; p < n; ++p) {
            // PER-SPAWN CONE JITTER: each particle draws its OWN direction from the
            // weapon's spread cone off the SAME shared rng stream the hitscan path
            // uses, so a stream reads as a spraying cone rather than n co-linear
            // bolts. spreadDeg 0 (every aimed weapon) returns `dir` unchanged.
            x3::phys::Vec3 nd = applySpread(dir, d.spreadDeg, rngState);
            ProjectileSpawn pj;
            pj.pos    = eye;
            pj.vel    = x3::phys::Vec3{ nd.x * d.projSpeed, nd.y * d.projSpeed, nd.z * d.projSpeed };
            // PER-PARTICLE DAMAGE: each particle carries the def's full per-pellet
            // damage, which is exactly what each of the `pellets` hitscan rays carried
            // before. The conversion is therefore damage-neutral — the roster lane's
            // tuned DPS band survives it unchanged.
            pj.damage = d.damage;
            // STAGGERED LIFETIME: the host despawns a bolt at `range`, so varying the
            // range per particle is how a stream stops dying as one flat wall. Only a
            // real stream (n > 1) staggers; a single aimed bolt keeps its def range
            // EXACTLY, which is what keeps napalm/rocket/plasma bit-identical.
            //   spread: 0.75x .. 1.00x of range, evenly across the burst.
            pj.range  = (n > 1) ? d.range * (0.75f + 0.25f * ((float)p / (float)(n - 1)))
                                : d.range;
            pj.gravity      = d.projectileGravity;  // ballistic arc (0 = flat; see WeaponDef)
            pj.splashRadius = d.splashRadius;   // Plasma Rifle: small AoE on impact
            pj.splashDamage = d.splashDamage;
            pj.type         = d.type;            // canon-aliens Adaptive-Hide tag
            // CANON-12 payloads: napalm's ground pool + burn, freeze slow, BFG secondaries.
            pj.firePoolDuration = d.firePoolDuration;
            pj.firePoolDps      = d.firePoolDps;
            pj.firePoolRadius   = d.firePoolRadius;
            pj.burnDuration     = d.burnDuration;
            pj.burnDps          = d.burnDps;
            pj.freezeDuration   = d.freezeDuration;
            pj.freezeSlowFactor = d.freezeSlowFactor;
            pj.secondaryBolts   = d.secondaryBolts;
            out.projectiles.push_back(pj);
        }
    }
    return out;
}

bool Arsenal::reload() {
    if (m_sel < 0) return false;
    const WeaponDef& d = m_defs[(size_t)m_sel];
    WeaponState&     s = m_state[(size_t)m_sel];
    if (d.usesCharge)                return false; // CHARGE weapons never reload
    if (s.reloadTimer > 0.0f)        return false; // already reloading
    if (s.ammoInMag   >= d.magSize)  return false; // mag already full
    if (s.reserve     <= 0)          return false; // no spare ammo
    // [W9-3 RPG] the skill/mod reload multiplier LAYERS on the def's base time
    // (the WeaponDef table is never mutated).
    s.reloadTimer = d.reloadTime * m_reloadMult;
    x3::logInfo("[arsenal] reloading '" + d.name + "' (" + std::to_string(s.reloadTimer) + "s)");
    return true;
}

// [W9-3 RPG] add reserve rounds to a weapon, capped at reserveAmmo * ammoCapMult
// (the multiplier layer — base def untouched). Returns the rounds actually added.
int Arsenal::addReserve(int index, int rounds) {
    if (index < 0 || index >= (int)m_defs.size() || rounds <= 0) return 0;
    const WeaponDef& d = m_defs[(size_t)index];
    WeaponState&     s = m_state[(size_t)index];
    const int cap = (int)((float)d.reserveAmmo * m_ammoCapMult + 0.5f);
    const int room = cap - s.reserve;
    if (room <= 0) return 0;
    const int take = rounds < room ? rounds : room;
    s.reserve += take;
    return take;
}

int Arsenal::chargeWeaponIndex() const {
    for (size_t i = 0; i < m_defs.size(); ++i)
        if (m_defs[i].usesCharge) return (int)i;
    return -1;
}

bool Arsenal::chargeRegenerating() const {
    if (m_sel < 0 || m_sel >= (int)m_defs.size()) return false;
    const WeaponDef&   d = m_defs[(size_t)m_sel];
    const WeaponState& s = m_state[(size_t)m_sel];
    if (!d.usesCharge || d.chargeRegenPerSec <= 0.0f) return false;
    if (m_beamHeld) return false;              // firing: regen is interrupted
    if (s.regenDelay > 0.0f) return false;     // still in the cool-down beat
    const float ceil = (d.chargeRegenTo > 0.0f) ? d.chargeRegenTo : d.chargeCap;
    return s.charge < ceil;                    // done once the ceiling is reached
}

float Arsenal::chargeRegenWait() const {
    if (m_sel < 0 || m_sel >= (int)m_defs.size()) return 0.0f;
    const WeaponDef& d = m_defs[(size_t)m_sel];
    if (!d.usesCharge || d.chargeRegenPerSec <= 0.0f) return 0.0f;
    return m_state[(size_t)m_sel].regenDelay;
}

bool Arsenal::chargeRegenSlow() const {
    if (m_sel < 0 || m_sel >= (int)m_defs.size()) return false;
    const WeaponDef& d = m_defs[(size_t)m_sel];
    if (!d.usesCharge || d.chargeRegenSlowAbove <= 0.0f) return false;
    return m_state[(size_t)m_sel].charge >= d.chargeRegenSlowAbove;
}

float Arsenal::grantCharge(float amount) {
    if (amount <= 0.0f) return 0.0f;
    const int idx = chargeWeaponIndex();
    if (idx < 0) return 0.0f;
    WeaponState&     s = m_state[(size_t)idx];
    const WeaponDef& d = m_defs[(size_t)idx];
    const float before = s.charge;
    s.charge += amount;
    if (s.charge > d.chargeCap) s.charge = d.chargeCap;   // stack to the cap
    return s.charge - before;
}

void Arsenal::tick(float dt) {
    if (dt <= 0.0f) return;
    // ---- CHARGE weapon (Lightning): DRAIN and PASSIVE REGEN --------------------
    // Both live in THIS ONE BLOCK, deliberately: drain and regen are two directions of
    // the same pool, and splitting them into separate paths is how they end up
    // disagreeing about the pool's state. Exactly one of them runs on any given tick.
    //
    //   BEAM HELD  -> bleed at chargeDrainPerSec (IDKFA never depletes) and RESET the
    //                 regen delay, so firing always INTERRUPTS regen and restarts the
    //                 cool-down beat. (Held-on-empty also counts as firing: you must
    //                 release the trigger to recharge.)
    //   RELEASED   -> count the delay down; once it hits 0, refill toward chargeRegenTo
    //                 on the two-speed curve (full rate below chargeRegenSlowAbove,
    //                 chargeRegenSlowMult of it at/above). Never overshoots the ceiling,
    //                 and never DRAINS a pool already above it.
    if (m_sel >= 0) {
        const WeaponDef& cd = m_defs[(size_t)m_sel];
        if (cd.usesCharge) {
            WeaponState& cs = m_state[(size_t)m_sel];
            if (m_beamHeld) {
                if (!m_infiniteAmmo) {
                    cs.charge -= cd.chargeDrainPerSec * dt;
                    if (cs.charge < 0.0f) cs.charge = 0.0f;
                }
                cs.regenDelay = cd.chargeRegenDelay;   // firing interrupts + restarts regen
            } else if (cs.regenDelay > 0.0f) {
                cs.regenDelay -= dt;                   // the "let it cool" beat
                if (cs.regenDelay < 0.0f) cs.regenDelay = 0.0f;
            } else if (cd.chargeRegenPerSec > 0.0f) {
                const float ceil = (cd.chargeRegenTo > 0.0f) ? cd.chargeRegenTo : cd.chargeCap;
                if (cs.charge < ceil) {
                    // Integrate the two-speed curve PIECEWISE across this step: if the
                    // pool crosses chargeRegenSlowAbove mid-tick, the part of dt before
                    // the crossing runs at the fast rate and the remainder at the slow
                    // one. (Applying one rate for the whole step would make the measured
                    // band rates dt-dependent — and the gate measures them.)
                    const float slowAt = cd.chargeRegenSlowAbove;
                    const float fast   = cd.chargeRegenPerSec;
                    const float slow   = fast * cd.chargeRegenSlowMult;
                    float rem = dt;
                    if (slowAt > 0.0f && cs.charge < slowAt && fast > 0.0f) {
                        const float tToSlow = (slowAt - cs.charge) / fast;   // s at the fast rate
                        const float step    = (tToSlow < rem) ? tToSlow : rem;
                        cs.charge += fast * step;
                        rem       -= step;
                    }
                    if (rem > 0.0f) {
                        const float rate = (slowAt > 0.0f && cs.charge >= slowAt) ? slow : fast;
                        cs.charge += rate * rem;
                    }
                    if (cs.charge > ceil) cs.charge = ceil;   // HARD STOP at the ceiling
                }
            }
        }
    }
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

    // W2-C: the shared FIRST-PERSON ARMS (see weapon.h m_arms). Same rigged dir;
    // a miss is graceful — the viewmodel simply draws without arms.
    m_arms.assets.reset(x3::asset::createAssetSource());
    if (m_arms.assets->mountDir(modelDir, 0)) {
        m_arms.loader.reset(x3::asset::createModelLoader(&device, m_arms.assets.get()));
        m_arms.model = m_arms.loader->load("FPArms_Jake.glb");
        if (m_arms.model.ok) m_arms.drawables = x3::asset::makeDrawables(m_arms.model);
    }
    if (!m_arms.drawables.empty())
        x3::logInfo("[arsenal] FP arms <- FPArms_Jake.glb (" +
                    std::to_string(m_arms.drawables.size()) + " prims)");
    else
        x3::logWarn("[arsenal] FPArms_Jake.glb missing — viewmodel draws without arms");
}

// The ONE place the FP viewmodel's world frame is solved. drawCurrentViewmodel DRAWS the
// gun with it; currentMuzzle() puts the BARREL TIP in the world with it. Sharing this is
// the whole point — a muzzle solved in any OTHER frame is exactly the bug Tim saw (fire
// spawning in mid-air beside the gun).
Arsenal::VmFrame Arsenal::currentViewmodelFrame(
        float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
        float extraYawOff, float extraPitchOff, float extraRollOff,
        float extraFwd, float extraRight, float extraDown) const {
    VmFrame f;
    if (m_sel < 0 || m_sel >= (int)m_defs.size()) {
        f.pos = x3::phys::Vec3{ eyeX, eyeY, eyeZ };
        return f;
    }
    const WeaponDef& d = m_defs[(size_t)m_sel];
    const float yawOff   = d.vmYawDeg   * (kPi / 180.0f) + extraYawOff;
    const float pitchOff = d.vmPitchDeg * (kPi / 180.0f) + extraPitchOff;
    const float rollOff  = d.vmRollDeg  * (kPi / 180.0f) + extraRollOff;
    // LENS: a narrower viewmodel FOV is reproduced by magnifying the gun's SIZE and
    // its OFF-AXIS offsets at a fixed forward distance (see kVmDefFovDeg, weapon.h).
    // mag == 1 by default, so this is a no-op unless the lens levers are set.
    const float mag   = viewmodelMagnification() * m_vmScaleMul;
    const float fwd   = d.vmFwd   + extraFwd;
    const float rgt   = (d.vmRight + extraRight) * mag;
    const float down  = (d.vmDown  + extraDown)  * mag;

    // Same camera-basis math as WeaponSystem::drawViewmodel (see 3 CONVENTIONS).
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    const x3::phys::Vec3 right{ -sy, 0.0f, cy };
    const x3::phys::Vec3 up{ right.y * forward.z - right.z * forward.y,
                             right.z * forward.x - right.x * forward.z,
                             right.x * forward.y - right.y * forward.x };
    const x3::phys::Vec3 negFwd{ -forward.x, -forward.y, -forward.z };
    auto applyOffsets = [&](x3::phys::Vec3 v) {
        v = rotateAboutAxis(v, up,      yawOff);
        v = rotateAboutAxis(v, right,   pitchOff);
        v = rotateAboutAxis(v, forward, rollOff);
        return v;
    };
    f.bx = applyOffsets(right);
    f.by = applyOffsets(up);
    f.bz = applyOffsets(negFwd);
    f.scale = d.vmScale * kVmScaleBoost * mag;

    // ---- THE ANCHOR (Tim 2026-08: "the gun is in your face way over your head") --
    // fwd/right/down place the weapon's BARREL LINE, i.e. the GLB scene-space point
    // (0, vmMuzzle.y, 0). They used to place the GLB ORIGIN, and every purchased
    // weapon GLB in this roster is authored STANDING ON THE FLOOR — origin on the
    // ground plane under the gun, barrel at the TOP of the model (scene y 0.37..0.68,
    // = 0.15..0.25 m at viewmodel scale). So `down 0.35` only lowered the gun's FEET:
    // the barrel came to rest ~0.08 m under eye level, put the sights ON the
    // crosshair and threw the gun's mass across the middle of the frame — and every
    // gun landed at a different height, because every model's barrel is a different
    // distance above its own floor plane. Subtracting the anchor here (rather than
    // re-tuning six numbers per weapon) makes `down` mean what it says, for the whole
    // roster, and keeps ONE frame shared with currentMuzzle() so the FX origin still
    // rides the drawn barrel tip (W13a).
    // NOTE the pivot is applied to `f.pos` only, so VmFrame stays exactly what its
    // consumers expect: the model matrix's translation column.
    const float pivotY = d.vmMuzzle.y;
    f.pos = x3::phys::Vec3{
        eyeX + forward.x * fwd + right.x * rgt - up.x * down - f.by.x * (pivotY * f.scale),
        eyeY + forward.y * fwd + right.y * rgt - up.y * down - f.by.y * (pivotY * f.scale),
        eyeZ + forward.z * fwd + right.z * rgt - up.z * down - f.by.z * (pivotY * f.scale) };
    return f;
}

x3::phys::Vec3 Arsenal::currentMuzzle(
        float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
        float extraYawOff, float extraPitchOff, float extraRollOff,
        float extraFwd, float extraRight, float extraDown) const {
    const VmFrame f = currentViewmodelFrame(eyeX, eyeY, eyeZ, yaw, pitch,
                                            extraYawOff, extraPitchOff, extraRollOff,
                                            extraFwd, extraRight, extraDown);
    const x3::phys::Vec3 m = currentMuzzleLocal();
    // The SAME transform the gun's own vertices take: model * (mx,my,mz).
    return x3::phys::Vec3{
        f.pos.x + (f.bx.x * m.x + f.by.x * m.y + f.bz.x * m.z) * f.scale,
        f.pos.y + (f.bx.y * m.x + f.by.y * m.y + f.bz.y * m.z) * f.scale,
        f.pos.z + (f.bx.z * m.x + f.by.z * m.y + f.bz.z * m.z) * f.scale };
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

    // ONE frame solve, SHARED with currentMuzzle() (see above).
    const VmFrame vf = currentViewmodelFrame(eyeX, eyeY, eyeZ, yaw, pitch,
                                             extraYawOff, extraPitchOff, extraRollOff,
                                             extraFwd, extraRight, extraDown);
    const x3::phys::Vec3 bx = vf.bx, by = vf.by, bz = vf.bz, pos = vf.pos;
    // The RAW camera basis is still needed below for the FP ARMS (which anchor to the
    // EYE, not the gun).
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    const x3::phys::Vec3 right{ -sy, 0.0f, cy };
    const x3::phys::Vec3 up{ right.y * forward.z - right.z * forward.y,
                             right.z * forward.x - right.x * forward.z,
                             right.x * forward.y - right.y * forward.x };

    float model[16];
    // Tim playtest 2026-05-25: the held weapons read tiny + dark. Enlarge the viewmodel
    // (~2x) and brighten its base color (HDR > 1) so it reads as a big, lit gun in the
    // dark interiors instead of a microscopic silhouette. d.vmScale stays the per-weapon
    // RELATIVE tuning; kVmScaleBoost is the global "hold it up bigger" multiplier.
    // ROUND 6 ENGINE FIX (5c35d65): kVmBright was 2.6 — an OVER-UNITY albedo multiplier
    // (physically impossible; it clipped the gun's own texture to white) added because GLB
    // meshes shaded at 1/PI of the prims around them. shaders/mesh.frag now lights the
    // viewmodel honestly, so the hack is gone: with it still in, the pistol blew out white.
    // Two lines of work converged on the same answer — the 14900K weapon-textures rework
    // (ba3ce7a) reached 1.0 from the art side after the owner reported "the texture looks
    // great ON the weapon, but when Jake HOLDS it, it transforms to garbage" (the viewmodel
    // was multiplying the correct baked albedo, clipping highlights, while drawWeaponAt drew
    // the same texture at 1.0 and looked right). DO NOT resurrect 2.6 or 1.4.
    // The stale `vmLitPBR` gunmetal branch (which DROPPED the texture for a flat dark factor,
    // never enabled for any weapon) is removed so the textured path is the single source of truth.
    constexpr float kVmBright     = 1.0f;   // true baked albedo (matches the world model)
    // SCALE comes from the VmFrame (346f5e7): currentViewmodelFrame() is the SINGLE source of
    // the viewmodel basis/pose/scale (vf.scale == d.vmScale * kVmScaleBoost, Tim's 2x kept), and
    // weaponMuzzle() maps WeaponDef::vmMuzzle through THIS SAME matrix. Recomputing the scale
    // inline here is what let the drawn gun and the FX origin drift apart in the first place.
    composeTRS(model, bx, by, bz, vf.scale, pos);
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

    // ---- W2-C FIRST-PERSON ARMS --------------------------------------------
    // Jake's baked aim-pose arms, anchored to the EYE (not the gun): the GLB
    // origin is his neck, hung slightly behind/below the camera and aligned to
    // the RAW camera basis (no per-weapon vm offsets — arms are the body; the
    // offsets are gun presentation). The bake reaches toward glTF +Z, which the
    // viewmodel basis maps to -forward, so the basis is yaw-flipped (bx/bz
    // negated) to aim the arms down the look direction. Scale 1.0 — deliberately
    // NOT kVmScaleBoost (guns are 2x by design; arms at 2x read as a giant).
    if (!m_arms.drawables.empty()) {
        // W2-A2 eye-round (first frame with the viewmodel actually IN a screenshot):
        // 0.24 down left the mid-bicep CROP STUMPS visible in-frame — sunk to 0.36
        // so the crop line sits below the frame edge at level pitch. 1.7x bright
        // blew the suit albedo into flesh-pink mottle under the graded cell light —
        // 1.15 keeps the sleeves readable without the blowout.
        constexpr float kArmsFwd    = -0.06f;  // neck sits just behind the eye
        constexpr float kArmsDown   =  0.36f;  // below it (chin/chest drop + hide crop)
        constexpr float kArmsBright =  1.15f;  // gentle lift; 1.7 read as pink blowout
        const x3::phys::Vec3 apos{
            eyeX + forward.x * kArmsFwd - up.x * kArmsDown,
            eyeY + forward.y * kArmsFwd - up.y * kArmsDown,
            eyeZ + forward.z * kArmsFwd - up.z * kArmsDown };
        const x3::phys::Vec3 abx{ -right.x, -right.y, -right.z };  // yaw-pi flip
        const x3::phys::Vec3 aby = up;
        const x3::phys::Vec3 abz = forward;                        // = -(negFwd)
        float amodel[16];
        composeTRS(amodel, abx, aby, abz, 1.0f, apos);
        for (const auto& dr : m_arms.drawables) {
            float fin[16];
            x3::asset::mulMat4(amodel, dr.nodeTransform, fin);
            const float skin[4] = {
                dr.baseColorFactor[0] * kArmsBright,
                dr.baseColorFactor[1] * kArmsBright,
                dr.baseColorFactor[2] * kArmsBright,
                dr.baseColorFactor[3],
            };
            device.drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                            x3::rhi::TextureHandle{ dr.baseColorTexId },
                            skin, fin);
        }
    }
}

bool Arsenal::currentHasDrawables() const {
    if (m_sel < 0 || m_views.empty()) return false;
    const ViewModel& sel = m_views[(size_t)m_sel];
    if (!sel.drawables.empty()) return true;
    return sel.fallbackIndex >= 0 && !m_views[(size_t)sel.fallbackIndex].drawables.empty();
}

void Arsenal::drawCurrentAt(x3::rhi::IRenderDevice& device,
                            const x3::rhi::FrameContext& frame,
                            const float model[16]) const {
    if (m_sel < 0 || m_views.empty()) return;
    const ViewModel& sel = m_views[(size_t)m_sel];
    const ViewModel& vm  = (!sel.drawables.empty()) ? sel
                         : (sel.fallbackIndex >= 0 ? m_views[(size_t)sel.fallbackIndex] : sel);
    if (vm.drawables.empty()) return;
    // Same brightness boost the FP viewmodel uses so the held gun reads lit in dark
    // interiors. The caller owns the full world placement (hand-bone * grip * scale),
    // so unlike drawCurrentViewmodel this does NO camera-relative posing.
    constexpr float kVmBright = 1.0f;   // ROUND 6: see drawCurrentViewmodel (over-unity albedo hack removed).
    for (const auto& dr : vm.drawables) {
        // STRIP the node-transform's authored WORLD TRANSLATION (cols 12..14): these
        // weapon GLBs bake an FP-viewmodel placement offset into the root node, which
        // — when multiplied by the world-space hand matrix here — flings the gun off
        // to the floor (Tim 3P playtest: weapon lay flat mid-corridor, not in hand).
        // The caller's `model` already owns the full world placement (hand-bone * grip
        // * scale), so we keep only the node's orientation/scale (upper 3x3).
        float nt[16];
        std::memcpy(nt, dr.nodeTransform, 16 * sizeof(float));
        nt[12] = nt[13] = nt[14] = 0.0f;
        float fin[16];
        x3::asset::mulMat4(model, nt, fin);
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
                     a.def(ig).kind == FireKind::Hitscan && a.def(ig).pellets == 10 &&
                     a.def(il).kind == FireKind::Projectile && a.def(il).projSpeed > 0.0f;
        // Pistol matches the tuned numbers (polish pass: 16 dmg / 4.5 per s / mag 12 / 55 m).
        bool pistolStats = named &&
                     a.def(ip).damage == 16 && a.def(ip).fireRate == 4.5f &&
                     a.def(ip).magSize == 12 && a.def(ip).range == 55.0f;
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
        // AMMO: this used to assert magSize > 0. Under the CHARGE model the Lightning
        // Gun has no magazine at all (magSize/reserveAmmo are dead), so that assertion
        // was checking a field the weapon no longer uses. The real invariant is that it
        // carries a usable CHARGE pool — assert THAT instead.
        int ip = a.indexOf("pistol");
        bool lgOK = present && ip >= 0 &&
            a.def(iz).kind == FireKind::Hitscan && a.def(iz).beam &&
            a.def(iz).chainTargets >= 1 && a.def(iz).falloffStart > 0.0f &&
            a.def(iz).falloffStart < a.def(iz).range &&
            a.def(iz).range < a.def(ip).range &&   // short range vs pistol
            a.def(iz).damage > 0 && a.def(iz).fireRate > 0.0f &&
            a.def(iz).usesCharge && a.def(iz).chargeMax > 0.0f &&
            a.def(iz).chargeCap >= a.def(iz).chargeMax &&
            a.def(iz).chargeDrainPerSec > 0.0f;
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
        a.tick(2.0f);         // total 3.0 s > 2.3 s reload
        bool refilled = !a.isReloading() && a.currentState().ammoInMag == 8 &&
                        a.currentState().reserve == 32;   // reserve 40 - 8 = 32
        wcheck(refilled, "W4d reload complete: mag full, reserve drained by 8");
    }

    // ---- W5: shotgun emits N pellets per shot --------------------------------
    {
        Arsenal a;
        a.select(2);          // shotgun: pellets 10
        a.tick(1.0f);
        ResolvedFire f = a.fire(eye, fwd, rng);
        bool nRays = f.fired && (int)f.rays.size() == 10 && f.projectiles.empty();
        // Every pellet ray carries the per-pellet damage + the weapon range.
        bool perPelletDmg = nRays;
        for (const auto& ray : f.rays)
            if (ray.damage != 14 || ray.range != 18.0f) perPelletDmg = false;
        // Spread actually scatters the rays (not all identical to the input dir).
        bool scattered = false;
        for (const auto& ray : f.rays)
            if (std::fabs(ray.dir.x - 1.0f) > 1e-4f || std::fabs(ray.dir.y) > 1e-4f) scattered = true;
        wcheck(nRays && perPelletDmg && scattered,
               "W5 shotgun: 10 spread pellets, each 14 dmg @ 18 m");
    }

    // ---- W6: hitscan AND projectile both resolve into the right payload ------
    {
        Arsenal a;
        // Hitscan (pistol): one ray, no projectiles.
        a.select(0); a.tick(1.0f);
        ResolvedFire h = a.fire(eye, fwd, rng);
        bool hitscanOK = h.fired && (int)h.rays.size() == 1 && h.projectiles.empty() &&
                         h.rays[0].damage == 16;
        // Projectile (plasma): one projectile spawned at the eye with vel = dir*speed.
        a.select(3); a.tick(1.0f);
        ResolvedFire p = a.fire(eye, fwd, rng);
        bool projOK = p.fired && p.rays.empty() && (int)p.projectiles.size() == 1;
        if (projOK) {
            const ProjectileSpawn& pj = p.projectiles[0];
            float vlen = std::sqrt(pj.vel.x*pj.vel.x + pj.vel.y*pj.vel.y + pj.vel.z*pj.vel.z);
            projOK = pj.damage == 35 && std::fabs(vlen - 55.0f) < 0.5f &&
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

    // ---- W12 canon-aliens Adaptive-Hide tag: per-weapon DamageType is set per spec
    // (Kinetic for ballistic-feel guns, Energy for plasma/lightning) AND survives the
    // Arsenal::fire() resolve into HitscanRay::type / ProjectileSpawn::type (the data
    // path the host walks to ultimately pass type to MonsterManager::fire). ----
    {
        Arsenal a;
        // The 7 named weapons and their expected canon types.
        struct ExpectedType { const char* name; x3::DamageType type; };
        const ExpectedType expect[] = {
            { "pistol",       x3::DamageType::Kinetic },
            { "smg",          x3::DamageType::Kinetic },
            { "shotgun",      x3::DamageType::Kinetic },
            { "plasma",       x3::DamageType::Energy  },
            { "chaingun",     x3::DamageType::Kinetic },
            { "plasma_rifle", x3::DamageType::Energy  },
            { "lightning",    x3::DamageType::Energy  },
            { "rocket",       x3::DamageType::Explosive },
        };
        bool allDefsOk = true;
        for (const ExpectedType& e : expect) {
            const int i = a.indexOf(e.name);
            if (i < 0 || a.def(i).type != e.type) { allDefsOk = false; break; }
        }
        wcheck(allDefsOk,
               "W12a per-weapon DamageType matches the canon-aliens table (Kinetic/Energy split)");

        // Resolve: switching to each weapon + firing one shot stamps the same type
        // onto every HitscanRay (incl. shotgun pellets + lightning chain rays) /
        // ProjectileSpawn the Arsenal returns.
        uint32_t rng2 = 0xC0FFEEu;
        bool allResolveOk = true;
        for (const ExpectedType& e : expect) {
            const int i = a.indexOf(e.name);
            if (i < 0) { allResolveOk = false; break; }
            a.select(i);
            // select() sets cooldown == 1/fireRate so a switch can't fire faster than
            // the new weapon's rate. Advance past it so the shot lands in the test.
            a.tick(10.0f);
            ResolvedFire shot = a.fire(eye, fwd, rng2);
            if (!shot.fired) { allResolveOk = false; break; }
            for (const HitscanRay& r : shot.rays)
                if (r.type != e.type) { allResolveOk = false; break; }
            for (const ProjectileSpawn& p : shot.projectiles)
                if (p.type != e.type) { allResolveOk = false; break; }
            if (!allResolveOk) break;
        }
        wcheck(allResolveOk,
               "W12b Arsenal::fire() stamps WeaponDef::type onto every HitscanRay + ProjectileSpawn");
    }

    // ---- W13: THE MUZZLE — "the fire doesn't come from the barrel" (Tim 2026-07-11)
    // REGRESSION GUARD. The FX origin must be the BARREL TIP of the weapon we DRAW —
    // i.e. WeaponDef::vmMuzzle carried through the SAME world matrix drawCurrentViewmodel
    // composes — NOT a camera-relative guess. This test re-derives the expected barrel tip
    // with its OWN independent math and asserts Arsenal::currentMuzzle lands on it, for
    // EVERY weapon, at several yaw/pitch poses. If anyone ever re-routes the muzzle back
    // through a fixed camera offset, W13 goes red.
    {
        Arsenal a;
        const float kDeg = kPi / 180.0f;
        struct Pose { float yaw, pitch; };
        const Pose poses[] = { {0.0f, 0.0f}, {1.7f, 0.55f}, {-2.4f, -0.62f}, {3.0f, 0.2f} };

        bool onBarrel = true;     // muzzle == the gun's own barrel-tip transform
        bool perWeapon = true;    // the guns are DIFFERENT lengths -> different muzzles
        bool inFront = true;      // and it always leads the eye, at any pitch
        float minReach = 1e9f, maxReach = -1e9f;

        for (int wi = 0; wi < a.count(); ++wi) {
            a.select(wi);
            const WeaponDef& d = a.def(wi);
            // Every gun must actually carry a measured barrel tip down its +Z barrel axis.
            if (!(d.vmMuzzle.z > 0.3f)) perWeapon = false;

            for (const Pose& p : poses) {
                const float ex = 3.0f, ey = 1.7f, ez = -2.0f;
                // --- independent expected-muzzle math (mirrors the DRAW, not the solve) ---
                const float cp = std::cos(p.pitch), sp = std::sin(p.pitch);
                const float cy = std::cos(p.yaw),   sy = std::sin(p.yaw);
                const x3::phys::Vec3 fw{ cp * cy, sp, cp * sy };
                const x3::phys::Vec3 rt{ -sy, 0.0f, cy };
                const x3::phys::Vec3 up{ rt.y * fw.z - rt.z * fw.y,
                                         rt.z * fw.x - rt.x * fw.z,
                                         rt.x * fw.y - rt.y * fw.x };
                auto off = [&](x3::phys::Vec3 v) {
                    v = rotateAboutAxis(v, up, d.vmYawDeg   * kDeg);
                    v = rotateAboutAxis(v, rt, d.vmPitchDeg * kDeg);
                    v = rotateAboutAxis(v, fw, d.vmRollDeg  * kDeg);
                    return v;
                };
                const x3::phys::Vec3 bx = off(rt), by = off(up),
                                     bz = off(x3::phys::Vec3{ -fw.x, -fw.y, -fw.z });
                const float s = d.vmScale * kVmScaleBoost;
                // fwd/right/down place the BARREL LINE (0, vmMuzzle.y, 0), so the GLB
                // origin sits that far back down the viewmodel's own up axis.
                const x3::phys::Vec3 origin{
                    ex + fw.x * d.vmFwd + rt.x * d.vmRight - up.x * d.vmDown - by.x * (d.vmMuzzle.y * s),
                    ey + fw.y * d.vmFwd + rt.y * d.vmRight - up.y * d.vmDown - by.y * (d.vmMuzzle.y * s),
                    ez + fw.z * d.vmFwd + rt.z * d.vmRight - up.z * d.vmDown - by.z * (d.vmMuzzle.y * s) };
                const x3::phys::Vec3 mL = d.vmMuzzle;
                const x3::phys::Vec3 want{
                    origin.x + (bx.x * mL.x + by.x * mL.y + bz.x * mL.z) * s,
                    origin.y + (bx.y * mL.x + by.y * mL.y + bz.y * mL.z) * s,
                    origin.z + (bx.z * mL.x + by.z * mL.y + bz.z * mL.z) * s };

                const x3::phys::Vec3 got = a.currentMuzzle(ex, ey, ez, p.yaw, p.pitch);
                const float dx = got.x - want.x, dy = got.y - want.y, dz = got.z - want.z;
                if (std::sqrt(dx*dx + dy*dy + dz*dz) > 1e-3f) onBarrel = false;

                // It must LEAD the eye down the look direction (never behind the player).
                const float ahead = (got.x - ex) * fw.x + (got.y - ey) * fw.y + (got.z - ez) * fw.z;
                if (ahead < 0.2f) inFront = false;
                if (p.yaw == 0.0f && p.pitch == 0.0f) {
                    if (ahead < minReach) minReach = ahead;
                    if (ahead > maxReach) maxReach = ahead;
                }
            }
        }
        wcheck(onBarrel,
               "W13a muzzle == the held weapon's OWN barrel tip (vmMuzzle through the viewmodel matrix)");
        wcheck(inFront, "W13b muzzle leads the eye down the look dir at every yaw/pitch");
        // The whole bug: a SHARED offset cannot be right for guns of different lengths. The
        // shotgun (a 4.4 m source model) must reach visibly further than the pistol.
        wcheck(perWeapon && (maxReach - minReach) > 0.10f,
               "W13c the muzzle is PER-WEAPON (barrel reach differs across the roster)");
    }

    // ---- W14: THE FP VIEWMODEL TRANSFORM (Tim 2026-08, live play on ceb3c48:
    // "the gun is in your face way over your head") REGRESSION GUARD.
    // The gun used to be anchored on the GLB ORIGIN, which for every purchased
    // weapon model is the FLOOR PLANE UNDER THE GUN — so `vmDown` lowered the gun's
    // feet while its barrel (the TOP of the model) stayed level with the eye. These
    // assert the read a first-person viewmodel must have, in metres, for EVERY
    // weapon: barrel BELOW the eye by vmDown, muzzle below and to the RIGHT of the
    // look axis, and the whole silhouette off the eye line. No Vulkan needed.
    {
        Arsenal a;
        a.setViewmodelLens(0.0f, kVmWorldFovDeg);   // ignore any X3_VM_* in the harness env
        bool barrelAtDown = true;   // the barrel LINE lands exactly vmDown under the eye
        bool muzzleBelow  = true;   // and the barrel TIP is below the eye line too
        bool muzzleRight  = true;   // ...and to the right (lower-right read)
        bool consistent   = true;   // every gun at the SAME barrel height (was 0.15-0.25 m apart)
        float minDrop = 1e9f, maxDrop = -1e9f;
        const float ex = 3.0f, ey = 1.7f, ez = -2.0f;

        for (int wi = 0; wi < a.count(); ++wi) {
            a.select(wi);
            const WeaponDef& d = a.def(wi);
            // Level look down +X: forward=(1,0,0), right=(0,0,1), up=(0,1,0).
            const Arsenal::VmFrame f = a.currentViewmodelFrame(ex, ey, ez, 0.0f, 0.0f);
            // The barrel line is the GLB point (0, vmMuzzle.y, 0) — map it through the
            // SAME frame the gun is drawn with.
            const float by_ = f.pos.y + f.by.y * d.vmMuzzle.y * f.scale;
            const float drop = ey - by_;                       // metres below eye level
            if (std::fabs(drop - d.vmDown) > 1e-3f) barrelAtDown = false;
            if (drop < minDrop) minDrop = drop;
            if (drop > maxDrop) maxDrop = drop;

            const x3::phys::Vec3 m = a.currentMuzzle(ex, ey, ez, 0.0f, 0.0f);
            if (!(m.y < ey - 0.10f)) muzzleBelow = false;       // at least 10 cm under the eye
            if (!(m.z - ez > 0.02f)) muzzleRight = false;       // +Z is camera-right at yaw 0
        }
        consistent = (maxDrop - minDrop) < 1e-3f;
        wcheck(barrelAtDown, "W14a vm_down places the BARREL LINE, not the model's floor plane");
        wcheck(consistent,   "W14b every weapon's barrel sits at the SAME height under the eye");
        wcheck(muzzleBelow,  "W14c the muzzle is BELOW eye level (not on the crosshair)");
        wcheck(muzzleRight,  "W14d the muzzle sits RIGHT of the look axis (lower-right read)");

        // ---- W14e/f: the vm_fov LENS lever. A NARROWER viewmodel FOV magnifies the
        // gun and pushes it further off-axis; sharing the world FOV changes nothing.
        Arsenal b;
        b.setViewmodelLens(0.0f, 60.0f);                    // share the world lens
        const bool lensOff = std::fabs(b.viewmodelMagnification() - 1.0f) < 1e-6f;
        const Arsenal::VmFrame base = b.currentViewmodelFrame(ex, ey, ez, 0.0f, 0.0f);
        b.setViewmodelLens(45.0f, 60.0f);                   // narrower than the world
        const float mag = b.viewmodelMagnification();
        const Arsenal::VmFrame lens = b.currentViewmodelFrame(ex, ey, ez, 0.0f, 0.0f);
        const bool bigger = lens.scale > base.scale * 1.05f;
        const bool offAxis = (lens.pos.z - ez) > (base.pos.z - ez) * 1.05f;  // further right
        const bool magMath = std::fabs(mag - (std::tan(30.0f * kPi / 180.0f) /
                                              std::tan(22.5f * kPi / 180.0f))) < 1e-4f;
        wcheck(lensOff, "W14e vm_fov 0 == share the world lens (magnification 1, no change)");
        wcheck(bigger && offAxis && magMath,
               "W14f vm_fov 45 vs world 60 magnifies the gun + pushes it off-axis by tan ratio");
    }

    // =======================================================================
    // W15/W16 — THE CANONICAL 12. The original game's arsenal (per the C++ port at
    // D:\GameDev\EscapeLab3D, src/game/game_types.h WeaponType) is:
    //   Pistol Shotgun Bazooka Laser Plasma ChainGun LightningGun RailGun
    //   FlameThrower NapalmLauncher FreezeRay BFG11k
    // These tests assert the roster COVERS all twelve, that every newly-added weapon
    // carries its own PROBE-MEASURED barrel tip (not a guess, and not a value shared
    // with a different-length gun), and that each one's distinguishing behaviour
    // actually reaches the host through ResolvedFire.
    // =======================================================================

    // ---- W15a: all twelve canon weapons are present in the roster. ---------
    // The Bazooka is slot "rocket" (there is deliberately no second rocket weapon).
    {
        Arsenal a;
        struct Canon { const char* canonName; const char* slot; };
        const Canon canon[] = {
            { "Pistol",         "pistol"       }, { "Shotgun",        "shotgun"      },
            { "Bazooka",        "rocket"       }, { "Laser",          "laser"        },
            { "Plasma",         "plasma"       }, { "ChainGun",       "chaingun"     },
            { "LightningGun",   "lightning"    }, { "RailGun",        "railgun"      },
            { "FlameThrower",   "flamethrower" }, { "NapalmLauncher", "napalm"       },
            { "FreezeRay",      "freezeray"    }, { "BFG11k",         "bfg11k"       },
        };
        bool allPresent = true;
        for (const Canon& c : canon) {
            if (a.indexOf(c.slot) < 0) {
                allPresent = false;
                x3::logInfo(std::string("[weapons-test]   MISSING canon weapon ") +
                            c.canonName + " (expected roster slot '" + c.slot + "')");
            }
        }
        wcheck(allPresent, "W15a all 12 canonical weapons present in the arsenal");
    }

    // ---- W15b: every new weapon's vmMuzzle IS the probe-MEASURED barrel tip. -
    // This is the guard against the viewmodel-anchor bug: a guessed or copy-pasted
    // muzzle puts the gun's barrel line in the wrong place (the shotgun's source model
    // is 4.4 m — a shared guess was off by metres) and makes the FX spawn off-barrel.
    // Values are the output of `python tools/weapon_muzzle_probe.py` for each weapon's
    // OWN viewmodelGlb; re-run that tool if a viewmodel GLB is ever swapped.
    {
        Arsenal a;
        struct Measured { const char* slot; const char* glb; float x, y, z; };
        const Measured measured[] = {
            { "laser",        "WeaponEnergyPistol2.glb", -0.003f, 0.652f, 0.855f },
            { "railgun",      "WeaponRailgun.glb",        0.000f, 0.494f, 0.909f },
            { "flamethrower", "WeaponRocketLauncher.glb", 0.001f, 0.368f, 0.916f },
            { "napalm",       "WeaponRocketLauncher.glb", 0.001f, 0.368f, 0.916f },
            { "freezeray",    "WeaponBFG.glb",           -0.003f, 0.528f, 0.864f },
            { "bfg11k",       "WeaponBFG.glb",           -0.003f, 0.528f, 0.864f },
        };
        bool muzzlesMeasured = true, glbsMatch = true;
        for (const Measured& m : measured) {
            const int i = a.indexOf(m.slot);
            if (i < 0) { muzzlesMeasured = false; continue; }
            const WeaponDef& d = a.def(i);
            // The muzzle must be the measurement for the GLB this weapon actually draws —
            // a muzzle measured off a DIFFERENT model is exactly the bug being guarded.
            if (d.viewmodelGlb != m.glb) {
                glbsMatch = false;
                x3::logInfo(std::string("[weapons-test]   ") + m.slot + " draws " +
                            d.viewmodelGlb + " but its muzzle was measured on " + m.glb +
                            " — re-run tools/weapon_muzzle_probe.py");
            }
            if (std::fabs(d.vmMuzzle.x - m.x) > 1e-4f ||
                std::fabs(d.vmMuzzle.y - m.y) > 1e-4f ||
                std::fabs(d.vmMuzzle.z - m.z) > 1e-4f) {
                muzzlesMeasured = false;
                x3::logInfo(std::string("[weapons-test]   ") + m.slot + " vmMuzzle is not the measured value");
            }
        }
        wcheck(muzzlesMeasured, "W15b every canon-12 addition carries its PROBE-MEASURED barrel tip");
        wcheck(glbsMatch,       "W15c each measured muzzle belongs to the GLB that weapon actually draws");
    }

    // ---- W15d: the new weapons fire FROM their own muzzle, and the muzzle is --
    // genuinely per-weapon (a shared guess would collapse these onto one point).
    // W13a already proves muzzle == vmMuzzle-through-the-viewmodel-matrix for the WHOLE
    // roster; this pins the specific claim that the SIX ADDITIONS have distinct barrel
    // reaches rather than all inheriting one default.
    {
        Arsenal a;
        const char* added[] = { "laser", "railgun", "flamethrower", "napalm", "freezeray", "bfg11k" };
        const float ex = 0.0f, ey = 1.7f, ez = 0.0f;
        const x3::phys::Vec3 fwd{ 1, 0, 0 };
        bool allAhead = true, anyDistinct = false;
        float firstReach = -1.0f;
        for (const char* nm : added) {
            const int i = a.indexOf(nm);
            if (i < 0) { allAhead = false; continue; }
            a.select(i);
            const x3::phys::Vec3 m = a.currentMuzzle(ex, ey, ez, 0.0f, 0.0f);
            const float reach = (m.x - ex) * fwd.x + (m.y - ey) * fwd.y + (m.z - ez) * fwd.z;
            // The barrel tip must lead the eye (the shot leaves the gun, not the face)
            // and sit BELOW eye level (the fixed viewmodel-anchor read).
            if (reach < 0.2f || m.y >= ey) allAhead = false;
            if (firstReach < 0.0f) firstReach = reach;
            else if (std::fabs(reach - firstReach) > 1e-3f) anyDistinct = true;
        }
        wcheck(allAhead,    "W15d each added weapon's muzzle leads the eye and sits below eye level");
        wcheck(anyDistinct, "W15e the added weapons have DISTINCT barrel reaches (not one shared guess)");
    }

    // ---- W16a: RAILGUN PIERCES. The slug must carry a pierce budget through to --
    // the host on its ray; every other hitscan weapon must NOT (a stray pierce would
    // silently turn the shotgun into a wall-of-piercing-pellets).
    {
        Arsenal a;
        uint32_t rng = 0x1234u;
        const int i = a.indexOf("railgun");
        a.select(i);
        a.tick(5.0f);                    // clear the select cooldown (see Arsenal::select)
        ResolvedFire shot = a.fire(eye, fwd, rng);
        const bool pierces = shot.fired && !shot.rays.empty() && shot.rays[0].pierceTargets > 0;
        // Nothing else in the roster pierces.
        bool othersDontPierce = true;
        for (int wi = 0; wi < a.count(); ++wi)
            if (wi != i && a.def(wi).pierceTargets != 0) othersDontPierce = false;
        wcheck(pierces && othersDontPierce,
               "W16a RailGun fires a PIERCING slug (pierceTargets reaches the host; nothing else pierces)");
    }

    // ---- W16b: LASER IS CONTINUOUS. It must be an automatic beam flagged -------
    // continuous, and fire fast enough that held fire reads as one unbroken line
    // rather than discrete shots. Contrast with the Lightning Gun, which is a beam
    // but deliberately NOT continuous (discrete zaps).
    {
        Arsenal a;
        uint32_t rng = 0x2345u;
        const int i = a.indexOf("laser");
        const WeaponDef& d = a.def(i);
        a.select(i);
        a.tick(5.0f);                    // clear the select cooldown (see Arsenal::select)
        ResolvedFire shot = a.fire(eye, fwd, rng);
        const bool contDef = d.continuous && d.beam && d.automatic && d.fireRate >= 15.0f;
        const bool contRay = shot.fired && !shot.rays.empty() &&
                             shot.rays[0].continuous && shot.rays[0].beam;
        // The distinction from the lightning gun must be real, not nominal.
        const int li = a.indexOf("lightning");
        const bool distinct = (li < 0) || !a.def(li).continuous;
        wcheck(contDef && contRay && distinct,
               "W16b Laser is a CONTINUOUS beam (flagged through to the host; distinct from lightning)");
    }

    // ---- W16c: FREEZE RAY APPLIES A SLOW. Every particle must carry a real -----
    // slow (a factor genuinely below 1) for a real duration, and the weapon must be
    // feeble on raw damage — the port is explicit that the freeze, not the damage,
    // is the payload. Nothing else in the roster may freeze.
    {
        Arsenal a;
        uint32_t rng = 0x3456u;
        const int i = a.indexOf("freezeray");
        const WeaponDef& d = a.def(i);
        a.select(i);
        a.tick(5.0f);                    // clear the select cooldown (see Arsenal::select)
        ResolvedFire shot = a.fire(eye, fwd, rng);
        // 2026-08-15: the particles are PROJECTILES now (see the roster note), so the
        // payload is asserted on the spawns. The claim under test is unchanged.
        bool everyRayFreezes = shot.fired && !shot.projectiles.empty() && shot.rays.empty();
        for (const auto& pj : shot.projectiles)
            if (!(pj.freezeDuration > 0.0f && pj.freezeSlowFactor < 1.0f &&
                  pj.freezeSlowFactor > 0.0f)) everyRayFreezes = false;
        // A control weapon, not a damage weapon: it must be the weakest per-hit gun.
        bool weakest = true;
        for (int wi = 0; wi < a.count(); ++wi)
            if (wi != i && a.def(wi).damage < d.damage) weakest = false;
        // Cone: the port fires multiple crystalline particles, not one bolt.
        const bool isCone = (int)shot.projectiles.size() >= 3 && d.spreadDeg > 0.0f;
        bool othersDontFreeze = true;
        for (int wi = 0; wi < a.count(); ++wi)
            if (wi != i && a.def(wi).freezeDuration != 0.0f) othersDontFreeze = false;
        wcheck(everyRayFreezes && weakest && isCone && othersDontFreeze,
               "W16c FreezeRay applies a real SLOW on every particle (the payload, not its 5 damage)");
    }

    // ---- W16d: NAPALM LEAVES AREA DAMAGE. The bolt must carry a burning ground -
    // pool (duration AND dps AND radius) through to the host — that standing fire is
    // what separates napalm from the rocket. The rocket must NOT leave one.
    {
        Arsenal a;
        uint32_t rng = 0x4567u;
        const int i = a.indexOf("napalm");
        a.select(i);
        a.tick(5.0f);                    // clear the select cooldown (see Arsenal::select)
        ResolvedFire shot = a.fire(eye, fwd, rng);
        const bool leavesPool = shot.fired && !shot.projectiles.empty() &&
                                shot.projectiles[0].firePoolDuration > 0.0f &&
                                shot.projectiles[0].firePoolDps      > 0 &&
                                shot.projectiles[0].firePoolRadius   > 0.0f;
        // It must also still be an explosive (the pool is in ADDITION to the blast).
        const bool alsoExplodes = !shot.projectiles.empty() &&
                                  shot.projectiles[0].splashRadius > 0.0f;
        // The rocket is the control: a plain blast with no lingering fire.
        const int ri = a.indexOf("rocket");
        const bool rocketHasNoPool = (ri < 0) || a.def(ri).firePoolDuration == 0.0f;
        wcheck(leavesPool && alsoExplodes && rocketHasNoPool,
               "W16d NapalmLauncher leaves AREA DAMAGE (burning ground pool + blast; the rocket does not)");
    }

    // ---- W16e: FLAMETHROWER BURNS, at close range, in a cone. -----------------
    {
        Arsenal a;
        uint32_t rng = 0x5678u;
        const int i = a.indexOf("flamethrower");
        const WeaponDef& d = a.def(i);
        a.select(i);
        a.tick(5.0f);                    // clear the select cooldown (see Arsenal::select)
        ResolvedFire shot = a.fire(eye, fwd, rng);
        // 2026-08-15: travelling FlamePuffs now, not rays (see the roster note).
        bool everyRayBurns = shot.fired && !shot.projectiles.empty() && shot.rays.empty();
        for (const auto& pj : shot.projectiles)
            if (!(pj.burnDuration > 0.0f && pj.burnDps > 0)) everyRayBurns = false;
        const bool isCone = (int)shot.projectiles.size() >= 3 && d.spreadDeg >= 10.0f;
        // Shortest reach in the roster — the burn is paid for with range.
        bool shortest = true;
        for (int wi = 0; wi < a.count(); ++wi)
            if (wi != i && a.def(wi).range < d.range) shortest = false;
        wcheck(everyRayBurns && isCone && shortest,
               "W16e FlameThrower applies a BURN DOT in a wide cone at the shortest range in the roster");
    }

    // ---- W16f: BFG 11k detonates with secondary bolts and eats FIVE rounds. ----
    // ammoPerShot is the canon scarcity rule (a full 5-round pickup is ONE shot);
    // if it silently consumed 1 the weapon would be five times as available as designed.
    {
        Arsenal a;
        uint32_t rng = 0x6789u;
        const int i = a.indexOf("bfg11k");
        const WeaponDef& d = a.def(i);
        a.select(i);
        a.tick(5.0f);                    // clear the select cooldown (see Arsenal::select)
        const int before = a.state(i).ammoInMag;
        ResolvedFire shot = a.fire(eye, fwd, rng);
        const int spent = before - a.state(i).ammoInMag;
        const bool bolts = shot.fired && !shot.projectiles.empty() &&
                           shot.projectiles[0].secondaryBolts > 0;
        const bool blast = !shot.projectiles.empty() && shot.projectiles[0].splashRadius > 0.0f;
        const bool ammoCost = (d.ammoPerShot == 5) && (spent == 5);
        // Every other weapon still consumes exactly one round per pull.
        Arsenal b;
        const int pi = b.indexOf("pistol");
        b.select(pi);
        b.tick(5.0f);                    // clear the select cooldown (see Arsenal::select)
        const int pBefore = b.state(pi).ammoInMag;
        uint32_t rng2 = 0x789Au;
        b.fire(eye, fwd, rng2);
        const bool pistolStillOne = (pBefore - b.state(pi).ammoInMag) == 1;
        wcheck(bolts && blast && ammoCost && pistolStillOne,
               "W16f BFG11k detonates with secondary bolts and costs 5 rounds a shot (others still cost 1)");
    }

    // ---- W16g: the additions did not disturb the existing roster. -------------
    // Every pre-existing weapon must still carry ZERO of the new canon-12 payloads —
    // this is the "additive, byte-identical" claim made in the WeaponDef comments,
    // asserted rather than assumed.
    {
        Arsenal a;
        const char* preExisting[] = { "pistol", "smg", "shotgun", "plasma",
                                      "chaingun", "plasma_rifle", "lightning", "rocket" };
        bool untouched = true;
        for (const char* nm : preExisting) {
            const int i = a.indexOf(nm);
            if (i < 0) { untouched = false; continue; }
            const WeaponDef& d = a.def(i);
            if (d.pierceTargets != 0 || d.burnDuration != 0.0f || d.burnDps != 0 ||
                d.freezeDuration != 0.0f || d.firePoolDuration != 0.0f ||
                d.firePoolDps != 0 || d.secondaryBolts != 0 ||
                d.ammoPerShot != 1 || d.continuous) {
                untouched = false;
                x3::logInfo(std::string("[weapons-test]   pre-existing weapon '") + nm +
                            "' picked up a canon-12 payload it should not have");
            }
        }
        wcheck(untouched, "W16g the 8 pre-existing weapons are unchanged by the canon-12 additions");
    }

    // =======================================================================
    // W17 — WEAPON FEEL (2026-08-15 lane): the napalm arc, the flame/ice streams,
    // and the Cryo tag. Each of these was a MODEL limitation the roster lane could
    // not reach from config, so each gets an assertion that the model actually
    // changed — not merely that a number is present.
    // =======================================================================

    // ---- W17a: THE GRAVITY DEFAULT IS THE REGRESSION GUARD, PROVEN. -----------
    // The entire safety claim of WeaponDef::projectileGravity is "default 0.0 leaves
    // every pre-existing weapon bit-identical". This asserts it in three independent
    // ways instead of trusting the default:
    //   (i)  every weapon that did not opt in carries EXACTLY 0.0f, and so does every
    //        spawn it produces (a stamped-but-wrong value would pass a def-only check);
    //   (ii) replaying the Phase-B integrator shape (v.y -= g*dt; pos += v*dt) at g==0
    //        reproduces the OLD flat model's positions to the BIT across a long flight;
    //   (iii) the deterministic spread stream is untouched — an aimed projectile weapon
    //        still consumes exactly the rng draws it always did, so no weapon's random
    //        sequence shifted underneath it.
    {
        Arsenal a;
        uint32_t rngG = 0xA17A17u;
        bool defsZero = true, spawnsZero = true;
        for (int wi = 0; wi < a.count(); ++wi) {
            const WeaponDef& d = a.def(wi);
            const bool optedIn = (d.name == "napalm" || d.name == "flamethrower");
            if (!optedIn && d.projectileGravity != 0.0f) {
                defsZero = false;
                x3::logInfo("[weapons-test]   '" + d.name + "' picked up a gravity it did not opt into");
            }
            if (optedIn) continue;
            if (d.kind != FireKind::Projectile) continue;
            a.select(wi); a.tick(20.0f);
            ResolvedFire s = a.fire(eye, fwd, rngG);
            for (const ProjectileSpawn& pj : s.projectiles)
                if (pj.gravity != 0.0f) spawnsZero = false;
        }

        // (ii) BIT-EXACT trajectory replay. `flat` is the model every legacy bolt was
        // tuned against (app_run.cpp: pos += vel*dt, no gravity term). `grav` is the
        // Phase-B1 integrator with the field wired in. At gravity 0 they must agree
        // EXACTLY — float equality, not a tolerance, because "bit-identical" is the claim.
        bool replayIdentical = true;
        {
            Arsenal b;
            const int pi2 = b.indexOf("plasma");
            b.select(pi2); b.tick(20.0f);
            uint32_t rng3 = 0x5EED01u;
            ResolvedFire s = b.fire(eye, fwd, rng3);
            if (s.projectiles.size() != 1) replayIdentical = false;
            else {
                const ProjectileSpawn& pj = s.projectiles[0];
                x3::phys::Vec3 pFlat = pj.pos, vFlat = pj.vel;
                x3::phys::Vec3 pGrav = pj.pos, vGrav = pj.vel;
                const float dt = 1.0f / 165.0f;      // the rate that has bitten this project
                for (int step = 0; step < 400; ++step) {
                    pFlat.x += vFlat.x * dt; pFlat.y += vFlat.y * dt; pFlat.z += vFlat.z * dt;
                    vGrav.y -= pj.gravity * dt;      // dt-scaled, never per frame
                    pGrav.x += vGrav.x * dt; pGrav.y += vGrav.y * dt; pGrav.z += vGrav.z * dt;
                    if (pFlat.x != pGrav.x || pFlat.y != pGrav.y || pFlat.z != pGrav.z) {
                        replayIdentical = false; break;
                    }
                }
            }
        }

        // (iii) rng-stream identity: an aimed projectile weapon has spreadDeg 0, and
        // applySpread returns early WITHOUT drawing in that case — so firing it must
        // leave the shared spread stream exactly where it found it. If the multi-spawn
        // loop ever over-draws, every subsequent shotgun pattern in the game shifts.
        bool streamUntouched = true;
        {
            Arsenal b;
            const int ri2 = b.indexOf("rocket");
            b.select(ri2); b.tick(20.0f);
            uint32_t before = 0x1234ABCDu, rng4 = before;
            b.fire(eye, fwd, rng4);
            if (b.def(ri2).spreadDeg == 0.0f && rng4 != before) streamUntouched = false;
        }

        wcheck(defsZero && spawnsZero && replayIdentical && streamUntouched,
               "W17a gravity default 0 leaves every non-opted weapon bit-identical (defs, spawns, "
               "400-step trajectory replay at 165 Hz, and the rng stream)");
    }

    // ---- W17b: NAPALM ACTUALLY ARCS. -----------------------------------------
    // Not "carries a number" — the ballistic solution has to bend. Fired LEVEL the
    // shell must fall measurably below the flat path, and the arc must be sized to the
    // weapon rather than arbitrary: a 45-degree lob reaches v^2/g, which for a genuine
    // lobbed weapon lands INSIDE its own despawn range (if it sailed past `range` the
    // arc would never be seen). Flamethrower is the negative-gravity control.
    {
        Arsenal a;
        uint32_t rng = 0x7A17u;
        const int i = a.indexOf("napalm");
        const WeaponDef& d = a.def(i);
        a.select(i); a.tick(20.0f);
        ResolvedFire shot = a.fire(eye, fwd, rng);
        const bool carries = shot.fired && shot.projectiles.size() == 1 &&
                             shot.projectiles[0].gravity > 0.0f &&
                             shot.projectiles[0].gravity == d.projectileGravity;
        // Level shot: integrate and require a real drop.
        bool arcs = false, monotone = true;
        if (carries) {
            const ProjectileSpawn& pj = shot.projectiles[0];
            x3::phys::Vec3 p = pj.pos, v = pj.vel;
            const float dt = 1.0f / 120.0f;
            float lastY = p.y;
            for (int step = 0; step < 120; ++step) {   // 1 s of flight
                v.y -= pj.gravity * dt;
                p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
                if (p.y > lastY) monotone = false;      // must never climb on a level shot
                lastY = p.y;
            }
            // After 1 s under 32 m/s^2 the shell is ~16 m lower. Anything under a metre
            // would be a flat shot wearing a gravity field.
            arcs = (pj.pos.y - p.y) > 1.0f;
        }
        // The lob envelope matches the weapon: v^2/g must sit inside the authored range
        // (so the arc is usable) but be a real fraction of it (so it is not a mortar).
        const float lobRange = (d.projSpeed * d.projSpeed) / d.projectileGravity;
        const bool sized = lobRange < d.range && lobRange > d.range * 0.5f;
        // Control: the flamethrower's harvested gravity is NEGATIVE (flames rise).
        const int fi = a.indexOf("flamethrower");
        const bool flameRises = (fi >= 0) && a.def(fi).projectileGravity < 0.0f;
        wcheck(carries && arcs && monotone && sized && flameRises,
               "W17b napalm's spawn carries its harvested gravity and the ballistic solution "
               "genuinely arcs, sized to its own range (flames rise: negative g)");
    }

    // ---- W17c: MULTI-SPAWN — N for the streams, EXACTLY ONE for everyone else. --
    // The "exactly one" half is the regression guard: the host's live fire path still
    // reads projectiles[0], so any weapon that quietly grew a second spawn would start
    // silently dropping it. Asserted per weapon, not spot-checked.
    {
        Arsenal a;
        uint32_t rng = 0x11A5u;
        bool onesOk = true, streamsOk = true, boundedOk = true;
        for (int wi = 0; wi < a.count(); ++wi) {
            const WeaponDef& d = a.def(wi);
            if (d.kind != FireKind::Projectile) continue;
            a.select(wi); a.tick(20.0f);
            ResolvedFire s = a.fire(eye, fwd, rng);
            const int n = (int)s.projectiles.size();
            if (n > kMaxStreamSpawns) boundedOk = false;     // the hard bound, every weapon
            const bool isStream = (d.name == "flamethrower" || d.name == "freezeray");
            if (isStream) {
                if (n < 2 || n != d.pellets) {
                    streamsOk = false;
                    x3::logInfo("[weapons-test]   stream '" + d.name + "' emitted " +
                                std::to_string(n) + " spawns, expected " + std::to_string(d.pellets));
                }
            } else if (n != 1) {
                onesOk = false;
                x3::logInfo("[weapons-test]   aimed weapon '" + d.name + "' emitted " +
                            std::to_string(n) + " spawns, expected exactly 1");
            }
        }
        // A mis-authored def must be CLAMPED, not trusted — prove the bound bites.
        std::vector<WeaponDef> over = makeDefaultRoster();
        for (WeaponDef& w : over)
            if (w.name == "flamethrower") { w.pellets = 999; }
        Arsenal ov(over);
        const int oi = ov.indexOf("flamethrower");
        ov.select(oi); ov.tick(20.0f);
        uint32_t rng5 = 0x99u;
        ResolvedFire os = ov.fire(eye, fwd, rng5);
        const bool clamps = (int)os.projectiles.size() == kMaxStreamSpawns;
        wcheck(onesOk && streamsOk && boundedOk && clamps,
               "W17c streams emit N>1 spawns (== pellets), every other weapon emits exactly 1, "
               "and a runaway pellet count clamps at kMaxStreamSpawns");
    }

    // ---- W17d: the stream is a real STREAM, not N copies of one bolt. ---------
    // Per-spawn cone jitter (distinct directions) + staggered lifetimes (distinct
    // ranges, all inside the authored range) are what stop the burst reading as a
    // single fat projectile. Aimed weapons must show NEITHER — their one bolt keeps
    // the def's range EXACTLY, which is half of why they stay bit-identical.
    {
        Arsenal a;
        uint32_t rng = 0xC04Eu;
        bool jitterOk = true, staggerOk = true;
        const char* streams[] = { "flamethrower", "freezeray" };
        for (const char* nm : streams) {
            const int i = a.indexOf(nm);
            if (i < 0) { jitterOk = false; continue; }
            const WeaponDef& d = a.def(i);
            a.select(i); a.tick(20.0f);
            ResolvedFire s = a.fire(eye, fwd, rng);
            if ((int)s.projectiles.size() < 2) { jitterOk = false; continue; }
            bool anyDiffDir = false, anyDiffRange = false, allInRange = true;
            for (size_t p = 1; p < s.projectiles.size(); ++p) {
                const auto& A = s.projectiles[0]; const auto& B = s.projectiles[p];
                if (A.vel.x != B.vel.x || A.vel.y != B.vel.y || A.vel.z != B.vel.z) anyDiffDir = true;
                if (A.range != B.range) anyDiffRange = true;
            }
            for (const auto& pj : s.projectiles)
                if (!(pj.range > 0.0f && pj.range <= d.range)) allInRange = false;
            if (!anyDiffDir) jitterOk = false;
            if (!anyDiffRange || !allInRange) staggerOk = false;
        }
        // Aimed control: napalm's single bolt keeps the authored range untouched.
        const int ni = a.indexOf("napalm");
        a.select(ni); a.tick(20.0f);
        ResolvedFire ns = a.fire(eye, fwd, rng);
        const bool aimedExact = ns.projectiles.size() == 1 &&
                                ns.projectiles[0].range == a.def(ni).range;
        wcheck(jitterOk && staggerOk && aimedExact,
               "W17d stream spawns have per-particle cone jitter + staggered lifetimes; "
               "an aimed bolt keeps its authored range exactly");
    }

    // ---- W17e: THE CRYO TAG. FreezeRay stamps it; NOTHING else does. ----------
    // The exclusivity half matters as much as the presence half: MonsterSystem keys a
    // real chase-speed slow off this value, so a second weapon acquiring the tag would
    // silently gain crowd control. Also pins the APPEND-ONLY property of the enum —
    // Cryo must not collide with an older row, or every Adaptive-Hide window re-keys.
    {
        Arsenal a;
        uint32_t rng = 0xC0DE01u;
        const int i = a.indexOf("freezeray");
        const bool defTagged = (i >= 0) && a.def(i).type == x3::DamageType::Cryo;
        bool onlyOne = true;
        for (int wi = 0; wi < a.count(); ++wi)
            if (wi != i && a.def(wi).type == x3::DamageType::Cryo) onlyOne = false;
        // Resolve: the tag must survive fire() onto EVERY particle, not just the first.
        bool everySpawnTagged = false;
        if (i >= 0) {
            a.select(i); a.tick(20.0f);
            ResolvedFire s = a.fire(eye, fwd, rng);
            everySpawnTagged = s.fired && s.projectiles.size() >= 2;
            for (const auto& pj : s.projectiles)
                if (pj.type != x3::DamageType::Cryo) everySpawnTagged = false;
        }
        // Append-only: Cryo is a NEW value, distinct from every pre-existing row, and
        // Count still bounds the enum.
        const bool appended =
            x3::DamageType::Cryo != x3::DamageType::None &&
            x3::DamageType::Cryo != x3::DamageType::Kinetic &&
            x3::DamageType::Cryo != x3::DamageType::Energy &&
            x3::DamageType::Cryo != x3::DamageType::Explosive &&
            x3::DamageType::Cryo != x3::DamageType::Bio &&
            x3::DamageType::Cryo != x3::DamageType::Melee &&
            (uint32_t)x3::DamageType::Cryo < (uint32_t)x3::DamageType::Count;
        wcheck(defTagged && onlyOne && everySpawnTagged && appended,
               "W17e FreezeRay stamps DamageType::Cryo on every particle; no other weapon does; "
               "Cryo is appended, not renumbered");
    }

    // ---- W17f: the engine-side chill MIRRORS the weapon's own numbers. --------
    // MonsterSystem keys its slow off the TYPE and uses kCryoSlowFactor/kCryoSlowDuration
    // (the host cannot thread the per-shot payload without app_run.cpp — Phase B). That
    // duplication is only safe while the two agree, so the drift is asserted, not trusted.
    {
        Arsenal a;
        const int i = a.indexOf("freezeray");
        const bool mirrored = (i >= 0) &&
                              a.def(i).freezeSlowFactor == kCryoSlowFactor &&
                              a.def(i).freezeDuration   == kCryoSlowDuration;
        wcheck(mirrored,
               "W17f MonsterSystem's kCryoSlowFactor/kCryoSlowDuration mirror the FreezeRay def");
    }

    // ==== W18: Phase B — the app_run patch's data contracts, pinned here. ======

    // ---- W18a: the canon 12-key row resolves EVERY canon weapon in the live
    // arsenal (all 12 selectable by key), with 12 DISTINCT names. ---------------
    {
        Arsenal a;
        bool allResolve = true, distinct = true;
        for (int i = 0; i < kCanonKeyCount; ++i) {
            const char* nm = canonKeyWeaponName(i);
            if (!nm || a.indexOf(nm) < 0) allResolve = false;
            for (int j = 0; j < i; ++j)
                if (nm && canonKeyWeaponName(j) &&
                    std::string(nm) == canonKeyWeaponName(j)) distinct = false;
        }
        const bool bounds = canonKeyWeaponName(-1) == nullptr &&
                            canonKeyWeaponName(kCanonKeyCount) == nullptr;
        wcheck(allResolve && distinct && bounds,
               "W18a canon key row: 12 distinct names, every one resolves in the arsenal, "
               "out-of-range keys return null");
    }

    // ---- W18b: canon ORDER — 1=Pistol ... 9=FlameThrower, then 0 - = carry
    // canon slots 10-12 (Napalm / FreezeRay / BFG11k). ---------------------------
    {
        const bool order =
            std::string(canonKeyWeaponName(0))  == "pistol"       &&
            std::string(canonKeyWeaponName(1))  == "shotgun"      &&
            std::string(canonKeyWeaponName(2))  == "rocket"       &&   // canon Bazooka
            std::string(canonKeyWeaponName(8))  == "flamethrower" &&
            std::string(canonKeyWeaponName(9))  == "napalm"       &&   // the '0' key
            std::string(canonKeyWeaponName(10)) == "freezeray"    &&   // the '-' key
            std::string(canonKeyWeaponName(11)) == "bfg11k";           // the '=' key
        wcheck(order, "W18b canon key ORDER: 1..9 = canon 1-9, 0 - = bind canon slots 10-12");
    }

    // ---- W18c: the host integrator's ballistic form (v.y -= g*dt; pos += v*dt)
    // ARCS a napalm spawn and leaves a zero-gravity spawn exactly flat. This
    // mirrors app_run.cpp's LiveProjectile step (Phase B1) so a regression there
    // has a headless tripwire on the formula itself. -----------------------------
    {
        Arsenal a;
        bool arcs = false, flatStaysFlat = true;
        const float dt = 1.0f / 165.0f;   // the house refresh-rate case
        if (a.selectByName("napalm")) {
            a.tick(10.0f);
            ResolvedFire shot = a.fire(eye, fwd, rng);
            if (!shot.projectiles.empty() && shot.projectiles[0].gravity > 0.0f) {
                x3::phys::Vec3 p = shot.projectiles[0].pos, v = shot.projectiles[0].vel;
                const float y0 = p.y, vy0 = v.y;
                for (int s = 0; s < 165; ++s) {   // 1 simulated second
                    v.y -= shot.projectiles[0].gravity * dt;
                    p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
                }
                // Fired level: after 1 s the bolt must have DROPPED below the
                // flat path by g/2*t^2 (within integration tolerance).
                const float flatY = y0 + vy0 * 1.0f;
                arcs = p.y < flatY - 0.25f * shot.projectiles[0].gravity * 0.5f;
            }
        }
        if (a.selectByName("plasma")) {
            a.tick(10.0f);
            ResolvedFire shot = a.fire(eye, fwd, rng);
            if (!shot.projectiles.empty()) {
                flatStaysFlat = shot.projectiles[0].gravity == 0.0f;
            } else flatStaysFlat = false;
        }
        wcheck(arcs && flatStaysFlat,
               "W18c integrator form: napalm arcs under v.y-=g*dt at 165 Hz; "
               "zero-gravity bolts stay flat");
    }

    x3::logInfo(std::string("[weapons-test] ") + std::to_string(w_pass) + " passed, " +
                std::to_string(w_fail) + " failed");
    return w_fail == 0;
}

// ===========================================================================
// Headless self-test (--test-lightning-charge). Exercises the Lightning Gun
// CHARGE model (Tim spec): base charge, continuous drain while beam held, IDKFA
// never depletes, battery grant stacks to chargeCap, no mag/reload. NO Vulkan.
// ===========================================================================
namespace {
int lc_pass = 0, lc_fail = 0;
void lccheck(bool cond, const char* name) {
    if (cond) { ++lc_pass; x3::logInfo(std::string("[lightning-charge-test] PASS ") + name); }
    else      { ++lc_fail; x3::logError(std::string("[lightning-charge-test] FAIL ") + name); }
}
inline bool nearf(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }
} // namespace

bool runLightningChargeSelfTest() {
    lc_pass = lc_fail = 0;
    const x3::phys::Vec3 eye{ 0, 1.7f, 0 };
    const x3::phys::Vec3 fwd{ 1, 0, 0 };
    uint32_t rng = 0xBEEF01u;

    // ---- LC0: lightning is a charge weapon with the spec'd numbers ------------
    {
        Arsenal a;
        int iz = a.indexOf("lightning");
        bool ok = iz >= 0 &&
                  a.def(iz).usesCharge &&
                  nearf(a.def(iz).chargeMax, 100.0f) &&
                  nearf(a.def(iz).chargeCap, 300.0f) &&
                  // 3-minute pool: 100 / 180 s = 0.5556 /s (was 10/s).
                  nearf(a.def(iz).chargeDrainPerSec, 100.0f / 180.0f, 0.01f) &&
                  a.chargeWeaponIndex() == iz &&
                  nearf(a.state(iz).charge, 100.0f);   // seeded to base at construction
        lccheck(ok, "LC0 lightning uses charge: 100 base / 300 cap / 0.556/s drain / seeded full");
    }

    // ---- LC1: continuous drain ~chargeDrainPerSec while the beam is HELD ------
    {
        Arsenal a;
        a.selectByName("lightning");
        const float rate = a.def(a.indexOf("lightning")).chargeDrainPerSec;
        a.setBeamHeld(true);
        a.tick(1.0f);                         // 1 s held
        bool afterOne = nearf(a.currentState().charge, 100.0f - rate, 0.02f);
        a.tick(4.0f);                         // +4 s (5 s total)
        bool afterFive = nearf(a.currentState().charge, 100.0f - rate * 5.0f, 0.05f);
        const float held = a.currentState().charge;
        a.setBeamHeld(false);
        a.tick(2.0f);                         // released: NO drain while idle
        bool holdsWhenReleased = nearf(a.currentState().charge, held, 0.01f);
        lccheck(afterOne && afterFive && holdsWhenReleased,
                "LC1 charge drains at chargeDrainPerSec while held, holds steady when released");
    }

    // ---- LC2: drains to 0 then fire is gated (canFire false, no reload) -------
    {
        Arsenal a;
        a.selectByName("lightning");
        a.setBeamHeld(true);
        a.tick(400.0f);                       // well past the 180 s pool -> 0
        bool emptied = nearf(a.currentState().charge, 0.0f);
        bool gated   = !a.canFire();          // empty charge -> cannot fire
        bool noReload = !a.reload();          // charge weapons never reload
        lccheck(emptied && gated && noReload,
                "LC2 empty charge gates fire; charge weapon never reloads");
    }

    // ---- LC3: IDKFA never depletes charge + always canFire -------------------
    {
        Arsenal a;
        a.selectByName("lightning");
        a.setInfiniteAmmo(true);
        a.setBeamHeld(true);
        a.tick(30.0f);                        // 30 s held under IDKFA
        bool undrained = nearf(a.currentState().charge, 100.0f);
        a.tick(1.0f);                         // clear any residual cooldown
        bool canStill = a.canFire();
        lccheck(undrained && canStill, "LC3 IDKFA: charge never depletes, always canFire");
    }

    // ---- LC4: battery grant STACKS past base up to the cap -------------------
    {
        Arsenal a;
        a.selectByName("lightning");
        // Drain to 40 first so there's headroom (60 charge at 0.5556/s = 108 s).
        a.setBeamHeld(true); a.tick(108.0f); a.setBeamHeld(false);   // ~40 left
        float g1 = a.grantCharge(150.0f);     // 40 -> 190
        bool got1 = nearf(g1, 150.0f, 0.5f) && nearf(a.currentState().charge, 190.0f, 0.5f);
        float g2 = a.grantCharge(200.0f);     // 190 -> cap 300 (adds 110)
        bool capped = nearf(a.currentState().charge, 300.0f) && nearf(g2, 110.0f, 0.5f);
        float g3 = a.grantCharge(50.0f);      // already at cap -> 0 added
        bool atCap = nearf(g3, 0.0f) && nearf(a.currentState().charge, 300.0f);
        lccheck(got1 && capped && atCap, "LC4 battery grant stacks past 100 to the 300 cap");
    }

    // ---- LC5: firing consumes NO mag round (charge is the resource) ----------
    {
        Arsenal a;
        int iz = a.selectByName("lightning") ? a.indexOf("lightning") : -1;
        a.tick(1.0f);                         // clear the select cooldown
        int magBefore = a.currentState().ammoInMag;
        ResolvedFire f = a.fire(eye, fwd, rng);
        bool firedNoMagUse = f.fired && a.currentState().ammoInMag == magBefore && iz >= 0;
        lccheck(firedNoMagUse, "LC5 lightning fire does not consume a magazine round");
    }

    // ---- LC6: held beam eventually drains to empty and stops firing ----------
    {
        Arsenal a;
        a.selectByName("lightning");
        a.setBeamHeld(true);
        a.tick(1.0f);
        int fired = 0;
        bool stoppedWhenEmpty = false;
        for (int i = 0; i < 2000; ++i) {      // 250 s of held fire at 0.125 s steps (> the 180 s pool)
            ResolvedFire f = a.fire(eye, fwd, rng);
            if (f.fired) ++fired;
            a.tick(0.125f);
            if (a.currentState().charge <= 0.0f) { stoppedWhenEmpty = !a.canFire(); break; }
        }
        lccheck(fired > 0 && stoppedWhenEmpty,
                "LC6 held beam fires, drains to empty, then gates off");
    }

    // ---- LC7: SUSTAINED-FIRE DURATION == 3 MINUTES (Tim's call) --------------
    // Do not trust the constant — MEASURE it. Simulate continuous held fire at 60 Hz
    // and time how long a full charge actually lasts. Ships with a NEGATIVE CONTROL:
    // the same probe run against a roster mutated back to the old 10/s drain must be
    // REJECTED by the 180 s window. A gate that cannot fail is worthless.
    {
        // Drive a held beam to empty; return the observed seconds of sustained fire.
        auto measureSustainSec = [&](Arsenal& a) -> float {
            a.selectByName("lightning");
            a.setBeamHeld(true);
            const float dt = 1.0f / 60.0f;
            float t = 0.0f;
            for (int i = 0; i < 60 * 900 && a.currentState().charge > 0.0f; ++i) {
                a.fire(eye, fwd, rng);        // firing must not change the economy
                a.tick(dt);
                t += dt;
            }
            return t;
        };
        auto within180 = [](float s) { return s >= 175.0f && s <= 185.0f; };  // 180 +/-5

        Arsenal live;                                   // the SHIPPING roster
        const float secs = measureSustainSec(live);
        const bool  ok   = within180(secs);

        // NEGATIVE CONTROL: same probe, roster mutated back to the old 10 charge/sec.
        std::vector<WeaponDef> bad = makeDefaultRoster();
        for (auto& d : bad) if (d.usesCharge) d.chargeDrainPerSec = 10.0f;
        Arsenal mutated(bad);
        const float badSecs   = measureSustainSec(mutated);
        const bool  rejects   = !within180(badSecs);    // the probe MUST reject 10/s

        x3::logInfo("[lightning-charge-test] LC7 sustained fire = " + std::to_string(secs) +
                    " s (target 180 +/-5); negative control (10/s drain) = " +
                    std::to_string(badSecs) + " s -> " + (rejects ? "REJECTED" : "ACCEPTED (BUG)"));
        lccheck(ok && rejects,
                "LC7 a full charge lasts 3 MINUTES of sustained fire (+ negative control fails)");
    }

    // ---- LC8: battery-stacked cap (300) == ~9 minutes ------------------------
    {
        Arsenal a;
        a.selectByName("lightning");
        a.grantCharge(500.0f);                          // 100 -> clamped to the 300 cap
        const bool atCap = nearf(a.currentState().charge, 300.0f);
        a.setBeamHeld(true);
        const float dt = 1.0f / 60.0f;
        float t = 0.0f;
        for (int i = 0; i < 60 * 1200 && a.currentState().charge > 0.0f; ++i) { a.tick(dt); t += dt; }
        const bool ok = atCap && t >= 525.0f && t <= 555.0f;   // 540 +/-15
        x3::logInfo("[lightning-charge-test] LC8 battery-stacked (cap 300) sustained fire = " +
                    std::to_string(t) + " s (target 540 +/-15)");
        lccheck(ok, "LC8 battery cells stack to the cap -> ~9 min of sustained fire");
    }

    // =======================================================================
    // PASSIVE REGEN (Tim: "lets let the lightning recharge when not in use",
    //                     "regen all the way, but half speed over 150")
    // =======================================================================
    // Drive an IDLE (not-firing) lightning gun from EMPTY and profile the refill:
    // seconds spent in the fast band (0 -> 150), seconds in the slow band
    // (150 -> the ceiling), the final resting charge, and whether it overshoots.
    // The regen DELAY is subtracted so the returned times are pure regen seconds
    // (LC9 gates the delay itself). Times are measured, never assumed.
    auto regenProfile = [&](Arsenal& a, float& tFast, float& tSlow,
                            float& finalCharge, float& overshoot) {
        a.selectByName("lightning");
        const WeaponDef& d = a.def(a.indexOf("lightning"));
        a.setBeamHeld(true);
        a.tick(400.0f);                       // hold the beam down: drain the pool to 0
        a.setBeamHeld(false);                 // release -> the regen clock starts
        const float dt    = 1.0f / 60.0f;
        const float delay = d.chargeRegenDelay;
        const float ceil  = (d.chargeRegenTo > 0.0f) ? d.chargeRegenTo : d.chargeCap;
        float t = 0.0f, t150 = -1.0f, tCeil = -1.0f;
        for (int i = 0; i < 60 * 900; ++i) {  // up to 900 s of idle
            a.tick(dt);
            t += dt;
            const float c = a.currentState().charge;
            if (t150 < 0.0f && c >= d.chargeRegenSlowAbove) t150 = t - delay;
            if (tCeil < 0.0f && c >= ceil - 0.001f)       { tCeil = t - delay; break; }
        }
        tFast = t150;
        tSlow = (tCeil >= 0.0f && t150 >= 0.0f) ? (tCeil - t150) : -1.0f;
        // Keep ticking well past the ceiling: regen must HARD STOP, never creep past it.
        for (int i = 0; i < 60 * 60; ++i) a.tick(dt);    // +60 s of idle at the ceiling
        finalCharge = a.currentState().charge;
        overshoot   = finalCharge - ceil;
    };

    // ---- LC9: the 2 s COOL-DOWN BEAT — regen must not start early, and firing
    //          RESETS it (a burst can never free-refill mid-fight) ---------------
    {
        Arsenal a;
        a.selectByName("lightning");
        const float delay = a.def(a.indexOf("lightning")).chargeRegenDelay;
        a.setBeamHeld(true);
        a.tick(20.0f);                                   // burn ~11 charge
        const bool notWhileFiring = !a.chargeRegenerating();   // HUD must not claim regen
        a.setBeamHeld(false);
        const float c0 = a.currentState().charge;
        const float dt = 1.0f / 60.0f;

        // (a) idle for delay - 0.1 s: still NOTHING (charge dead flat).
        for (int i = 0; i < (int)((delay - 0.1f) / dt); ++i) a.tick(dt);
        const bool quietInDelay = nearf(a.currentState().charge, c0, 0.001f) &&
                                  !a.chargeRegenerating() && a.chargeRegenWait() > 0.0f;

        // (b) FIRE ONE SHOT right before the beat expires -> the delay RESTARTS.
        a.fire(eye, fwd, rng);
        const bool delayReset = nearf(a.chargeRegenWait(), delay, 0.001f);
        for (int i = 0; i < (int)((delay - 0.1f) / dt); ++i) a.tick(dt);
        const bool stillQuiet = nearf(a.currentState().charge, c0, 0.001f);   // reset held

        // (c) let the beat fully elapse -> regen ACTUALLY begins.
        for (int i = 0; i < (int)(0.5f / dt); ++i) a.tick(dt);
        const bool started = a.currentState().charge > c0 + 0.1f && a.chargeRegenerating();

        lccheck(notWhileFiring && quietInDelay && delayReset && stillQuiet && started,
                "LC9 regen waits the 2 s beat, firing RESETS it, then regen begins");
    }

    // ---- LC10/LC11: MEASURED two-speed refill + the HARD STOP at the cap -------
    // Measure BOTH BANDS SEPARATELY. An endpoint-only probe (0 -> 300 in ~270 s)
    // would happily pass on a single uniform wrong-but-averaging rate — which is
    // exactly the bug the negative control below manufactures.
    {
        Arsenal live;
        float tFast = -1, tSlow = -1, finalC = -1, over = 0;
        regenProfile(live, tFast, tSlow, finalC, over);
        const float fastRate = (tFast > 0.0f) ? 150.0f / tFast : 0.0f;   // 0 -> 150
        const float slowRate = (tSlow > 0.0f) ? 150.0f / tSlow : 0.0f;   // 150 -> 300
        const float total    = tFast + tSlow;

        x3::logInfo("[lightning-charge-test] LC10 regen 0->150 = " + std::to_string(tFast) +
                    " s (" + std::to_string(fastRate) + "/s, target 1.667) | 150->300 = " +
                    std::to_string(tSlow) + " s (" + std::to_string(slowRate) +
                    "/s, target 0.833) | TOTAL 0->300 = " + std::to_string(total) +
                    " s (target ~270)");

        const bool fastOk = nearf(fastRate, 100.0f / 60.0f, 0.03f) && tFast >= 87.0f && tFast <= 93.0f;
        const bool slowOk = nearf(slowRate, 100.0f / 120.0f, 0.03f) && tSlow >= 174.0f && tSlow <= 186.0f;
        const bool halved = nearf(slowRate, fastRate * 0.5f, 0.03f);   // THE two-segment rule
        const bool totOk  = total >= 261.0f && total <= 279.0f;        // ~270 +/-9
        lccheck(fastOk && slowOk && halved && totOk,
                "LC10 regen is 1.667/s below 150 and HALF (0.833/s) above -> ~270 s to the cap");

        // The ceiling: regen must land EXACTLY on the 300 cap and stop dead there —
        // 60 further seconds of idle must not move it one charge past.
        const bool stops = nearf(finalC, 300.0f, 0.001f) && over <= 0.001f;
        x3::logInfo("[lightning-charge-test] LC11 charge after +60 s idle at the ceiling = " +
                    std::to_string(finalC) + " (overshoot " + std::to_string(over) + ")");
        lccheck(stops, "LC11 regen HARD-STOPS at the 300 cap (no overshoot, no creep)");
    }

    // ---- LC12: NEGATIVE CONTROL — a UNIFORM (un-halved) rate must be REJECTED ---
    // Mutate the roster so the slow band runs at FULL speed (slowMult 1.0), i.e. the
    // "half speed over 150" rule is gone. The SAME two-segment probe that passed above
    // must now FAIL. A gate that cannot fail is worthless (docs/DECISIONS.md
    // REGRESSION DISCIPLINE) — this proves the LC10 assertion actually probes the rule.
    {
        std::vector<WeaponDef> bad = makeDefaultRoster();
        for (auto& d : bad) if (d.usesCharge) d.chargeRegenSlowMult = 1.0f;   // no halving
        Arsenal mutated(bad);
        float tFast = -1, tSlow = -1, finalC = -1, over = 0;
        regenProfile(mutated, tFast, tSlow, finalC, over);
        const float fastRate = (tFast > 0.0f) ? 150.0f / tFast : 0.0f;
        const float slowRate = (tSlow > 0.0f) ? 150.0f / tSlow : 0.0f;
        // Re-apply the EXACT LC10 predicate to the mutant.
        const bool slowOk = nearf(slowRate, 100.0f / 120.0f, 0.03f) && tSlow >= 174.0f && tSlow <= 186.0f;
        const bool halved = nearf(slowRate, fastRate * 0.5f, 0.03f);
        const bool totOk  = (tFast + tSlow) >= 261.0f && (tFast + tSlow) <= 279.0f;
        const bool rejects = !(slowOk && halved && totOk);   // the probe MUST reject it
        x3::logInfo("[lightning-charge-test] LC12 negative control (uniform rate, no halving): "
                    "0->150 = " + std::to_string(tFast) + " s, 150->300 = " + std::to_string(tSlow) +
                    " s (" + std::to_string(slowRate) + "/s) -> " +
                    (rejects ? "REJECTED" : "ACCEPTED (BUG: the gate cannot fail)"));
        lccheck(rejects, "LC12 negative control: an un-halved slow band FAILS the LC10 probe");
    }

    x3::logInfo(std::string("[lightning-charge-test] ") + std::to_string(lc_pass) + " passed, " +
                std::to_string(lc_fail) + " failed");
    return lc_fail == 0;
}

} // namespace x3::game
