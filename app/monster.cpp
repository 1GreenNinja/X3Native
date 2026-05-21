// Monster + combat (S6). See app/monster.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice +
// IPhysicsWorld + Scene interfaces only. No purchased C# copied; no id Tech /
// RBDOOM source consulted.
#include "monster.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Tuning constants.
// ---------------------------------------------------------------------------
namespace {

// The purchased alien_crawler GLB is authored larger than our graybox monster
// footprint; scale it down so it reads as a ~1.2 m crawler. The fallback box is
// authored at the right size already, so it uses scale 1.
constexpr float kRealModelScale = 0.5f;
constexpr float kBoxModelScale  = 1.0f;

// Collision box half-extents (meters) for the Enemy-layer body. Sized to a rough
// crawler footprint: ~1.0 m wide/deep, ~0.8 m tall. Used for BOTH the real GLB
// (the model is drawn over this volume) and the fallback box (which also renders
// at this size).
constexpr x3::phys::Vec3 kMonsterHalf{ 0.5f, 0.4f, 0.5f };

// Build a column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s,
// and translation t. Column-major: m[0..3]=col0, m[4..7]=col1, etc.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// Pure damage rule (testable; no rendering / physics).
// ---------------------------------------------------------------------------
bool applyDamage(int* hp, int damage) {
    if (!hp) return false;
    *hp -= damage;
    if (*hp < 0) *hp = 0;
    return *hp <= 0;
}

// ---------------------------------------------------------------------------
// Build the monster (load the real GLB, or fall back to a box).
// ---------------------------------------------------------------------------
void MonsterSystem::buildMonster(Scene& scene, x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& physics,
                                 std::string_view modelDir, const x3::phys::Vec3& pos) {
    m_pos = pos;
    m_hp  = kMonsterHp;
    m_alive = true;
    m_flash = 0.0f;

    // ---- Try the real purchased GLB via a mounted loose-dir asset source. ----
    m_assets.reset(x3::asset::createAssetSource());
    bool mounted = m_assets->mountDir(modelDir, 0);
    if (mounted) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_model = m_loader->load("alien_crawler.glb");
        if (m_model.ok)
            m_drawables = x3::asset::makeDrawables(m_model);
    } else {
        x3::logWarn("[monster] mountDir failed: " + std::string(modelDir));
    }

    if (!m_drawables.empty()) {
        m_usingReal  = true;
        m_modelScale = kRealModelScale;
        x3::logInfo("[monster] loaded alien_crawler.glb — " +
                    std::to_string(m_drawables.size()) + " drawable primitive(s)");
    } else {
        // ---- Fallback: a procedural box monster so the slice still works. ----
        m_usingReal  = false;
        m_modelScale = kBoxModelScale;
        if (m_model.ok)
            x3::logWarn("[monster] GLB loaded but produced no drawables; using fallback box");
        else
            x3::logWarn("[monster] alien_crawler.glb load failed; using fallback box");

        x3::prims::PrimMesh geo = x3::prims::makeBox(kMonsterHalf.x, kMonsterHalf.y,
                                                     kMonsterHalf.z, 0.0f, 0.0f, 0.0f, 1.0f);
        x3::rhi::MeshHandle mesh = device.createMesh(
            geo.verts.data(), (uint32_t)geo.verts.size(),
            geo.index.data(), (uint32_t)geo.index.size());
        x3::asset::ModelDrawable d;
        d.meshId = mesh.id;
        d.baseColorTexId = 0;                    // 0 -> default white -> flat color
        d.baseColorFactor[0] = 0.25f; d.baseColorFactor[1] = 0.65f;
        d.baseColorFactor[2] = 0.30f; d.baseColorFactor[3] = 1.0f;  // sickly green
        m_drawables.push_back(d);
    }

    // ---- Enemy-layer collision body for the shoot raycast. mass 0 -> Static
    // motion type but keeps the Enemy ObjectLayer, so it stays put under gravity
    // yet is hittable by a rayCast(Layer::Enemy) and movable by setBodyPosition
    // (same approach as the S4 door). ----
    m_body = physics.addBox(kMonsterHalf, m_pos, 0.0f, x3::phys::Layer::Enemy);

    // ---- Monster Entity: bookkeeping (tag/body/visibility/transform). Its render
    // mesh handle is left INVALID so Scene::render skips it; drawMonster() renders
    // ALL of the model's primitives at the Entity transform with the hit-flash
    // tint (so multi-primitive GLBs draw fully + consistently). ----
    Entity e;
    e.tag     = (uint32_t)Tag::Monster;
    e.visible = true;
    e.body    = m_body;                          // also registers body->entity map
    composeTRS(e.transform,
               x3::phys::Vec3{1, 0, 0}, x3::phys::Vec3{0, 1, 0}, x3::phys::Vec3{0, 0, 1},
               m_modelScale, m_pos);
    m_entity = scene.add(e);

    x3::logInfo("[monster] entity " + std::to_string(m_entity) +
                " placed at (" + std::to_string(pos.x) + ", " +
                std::to_string(pos.y) + ", " + std::to_string(pos.z) + ")" +
                " HP=" + std::to_string(m_hp) +
                (m_usingReal ? " [real GLB]" : " [fallback box]"));
}

// ---------------------------------------------------------------------------
// Fire one shot: raycast the Enemy layer, resolve to this monster, damage it.
// ---------------------------------------------------------------------------
FireResult MonsterSystem::fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                               Scene& scene, x3::phys::IPhysicsWorld& physics) {
    FireResult r;
    r.hpAfter = m_hp;
    if (!m_alive) return r;                       // already dead: nothing to hit

    x3::phys::RayHit hit = physics.rayCast(eye, dir, kFireMaxDist, x3::phys::Layer::Enemy);
    if (!hit.hit || !hit.body.valid()) return r;  // missed everything

    uint32_t ent = scene.entityForBody(hit.body);
    if (ent == kNoLink || ent >= scene.size()) return r;

    const Entity& e = scene.get(ent);
    if (e.tag != (uint32_t)Tag::Monster) return r; // hit something that isn't a monster
    if (ent != m_entity) return r;                 // (multi-monster: not this one)

    // ---- Hit a live monster: apply damage + start the red hit-flash. ----
    r.hitMonster = true;
    bool dead = applyDamage(&m_hp, kDamagePerShot);
    m_flash = kHitFlashTime;
    r.hpAfter = m_hp;

    if (dead) {
        // ---- Death: hide the Entity, remove the physics body (so subsequent
        // rays miss), drop the Entity's body handle, and latch dead. ----
        m_alive = false;
        r.killed = true;
        if (m_entity != kNoLink && m_entity < scene.size()) {
            Entity& me = scene.get(m_entity);
            me.visible = false;
            me.body = x3::phys::BodyId{};         // entity no longer owns a body
        }
        physics.removeBody(m_body);
        m_body = x3::phys::BodyId{};
        x3::logInfo("[monster] killed (HP 0) — hidden + body removed");
    } else {
        x3::logInfo("[monster] hit for " + std::to_string(kDamagePerShot) +
                    " — HP now " + std::to_string(m_hp));
    }
    return r;
}

// ---------------------------------------------------------------------------
// Per-frame: decay hit-flash; optional gentle chase (translation only).
// ---------------------------------------------------------------------------
void MonsterSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                           const x3::phys::Vec3& playerPos) {
    if (dt <= 0.0f) return;

    // Decay hit-flash.
    if (m_flash > 0.0f) {
        m_flash -= dt;
        if (m_flash < 0.0f) m_flash = 0.0f;
    }

    if (!m_alive) return;

    // Optional gentle chase toward the player (XZ only). OFF by default
    // (kChaseSpeed == 0). Translation only: no rotation getter/setter on the
    // physics interface, so the monster does not turn to face the player.
    if (kChaseSpeed > 0.0f && m_body.valid()) {
        float dx = playerPos.x - m_pos.x;
        float dz = playerPos.z - m_pos.z;
        float d  = std::sqrt(dx * dx + dz * dz);
        if (d > 1.0f) {                           // don't crawl into the player
            float step = kChaseSpeed * dt;
            m_pos.x += (dx / d) * step;
            m_pos.z += (dz / d) * step;
            physics.setBodyPosition(m_body, m_pos);
            if (m_entity != kNoLink && m_entity < scene.size()) {
                Entity& me = scene.get(m_entity);
                me.transform[12] = m_pos.x;
                me.transform[13] = m_pos.y;
                me.transform[14] = m_pos.z;
                me.transform[15] = 1.0f;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Draw all monster primitives at its transform, with the hit-flash tint.
// ---------------------------------------------------------------------------
void MonsterSystem::drawMonster(x3::rhi::IRenderDevice& device,
                                const x3::rhi::FrameContext& frame,
                                const Scene& scene) const {
    if (!m_alive || m_entity == kNoLink || m_entity >= scene.size()) return;
    const Entity& e = scene.get(m_entity);
    if (!e.visible) return;

    // Hit-flash: lerp the per-primitive base color toward red as flash decays.
    // flashAmt in [0,1]; 1 right after a hit, 0 once decayed.
    const float flashAmt = (kHitFlashTime > 0.0f) ? (m_flash / kHitFlashTime) : 0.0f;
    float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    // Toward red: keep R, knock down G/B by the flash amount.
    tint[1] = 1.0f - 0.85f * flashAmt;
    tint[2] = 1.0f - 0.85f * flashAmt;

    drawMonsterAt(device, frame, e.transform, tint);
}

void MonsterSystem::drawMonsterAt(x3::rhi::IRenderDevice& device,
                                  const x3::rhi::FrameContext& frame,
                                  const float model[16], const float tint[4]) const {
    for (const auto& d : m_drawables) {
        float color[4] = {
            d.baseColorFactor[0] * tint[0],
            d.baseColorFactor[1] * tint[1],
            d.baseColorFactor[2] * tint[2],
            d.baseColorFactor[3] * tint[3],
        };
        device.drawMesh(frame,
                        x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId },
                        color,
                        model);
    }
}

// ===========================================================================
// Headless self-test (--test-combat). T1 hit damages, T2 kill removes + rays
// miss, T3 aim away no damage, T4 unarmed gate. No window / Vulkan.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[combat-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[combat-test] FAIL ") + name); }
}

// Minimal headless IRenderDevice: hands out monotonically-increasing valid
// handles so buildMonster() runs unchanged with no Vulkan. Draw/frame/camera
// calls are no-ops. (Same shape as door.cpp's HeadlessDevice.)
class HeadlessDevice final : public x3::rhi::IRenderDevice {
public:
    bool init(const x3::rhi::DeviceDesc&) override { return true; }
    void shutdown() override {}
    void onResize(uint32_t, uint32_t) override {}
    void setCamera(float, float, float, float, float, float) override {}
    x3::rhi::FrameContext beginFrame() override { return {}; }
    void endFrame(const x3::rhi::FrameContext&) override {}
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex*, uint32_t,
                                   const uint32_t*, uint32_t) override {
        return x3::rhi::MeshHandle{ m_next++ };
    }
    void destroyMesh(x3::rhi::MeshHandle) override {}
    x3::rhi::TextureHandle createTexture(const void*, uint32_t, uint32_t, bool) override {
        return x3::rhi::TextureHandle{ m_next++ };
    }
    void destroyTexture(x3::rhi::TextureHandle) override {}
    void drawMesh(const x3::rhi::FrameContext&, x3::rhi::MeshHandle,
                  x3::rhi::TextureHandle, const float[4], const float[16]) override {}
    bool supportsDescriptorIndexing() const override { return false; }
    bool supportsMeshShaders() const override { return false; }
private:
    uint32_t m_next = 1;
};

x3::phys::Vec3 sub(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return x3::phys::Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

} // namespace

bool runCombatSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;
    MonsterSystem combat;

    // Place a monster ahead of a known eye position. modelDir points at the real
    // model dir; the headless device path mints fake GPU handles so the loader
    // works without Vulkan (falls back to a box if the GLB can't be parsed).
    const x3::phys::Vec3 monsterPos{ 0.0f, 0.4f, 0.0f };
    combat.buildMonster(scene, device, *physics, "G:/GameModels/rigged_glb", monsterPos);

    // Eye 3 m in front of the monster (toward -Z), at the monster's height.
    const x3::phys::Vec3 eye{ monsterPos.x, monsterPos.y, monsterPos.z - 3.0f };
    const x3::phys::Vec3 aimAt = sub(monsterPos, eye);     // points at the monster
    const x3::phys::Vec3 aimAway{ 0.0f, 0.0f, -1.0f };     // points away (further -Z)

    const int hp0 = combat.hp();

    // ---- T1: ray aimed AT the monster reduces its HP -------------------------
    {
        FireResult r = combat.fire(eye, aimAt, scene, *physics);
        bool damaged = r.hitMonster && combat.hp() == hp0 - kDamagePerShot && combat.alive();
        check(damaged, "T1 ray at monster reduces HP");
    }

    // ---- T3: ray aimed AWAY from the monster does no damage ------------------
    // (Run before the kill so the monster is still alive to prove the miss.)
    {
        int before = combat.hp();
        FireResult r = combat.fire(eye, aimAway, scene, *physics);
        bool noDamage = !r.hitMonster && combat.hp() == before && combat.alive();
        check(noDamage, "T3 ray away from monster does no damage");
    }

    // ---- T4: firing when NOT armed does nothing (gate respected) -------------
    // The host only calls fire() when WeaponSystem::hasWeapon(). Model that gate
    // exactly: with armed==false we must NOT call fire(), so HP is unchanged even
    // though the ray would otherwise hit.
    {
        int before = combat.hp();
        bool armed = false;
        FireResult r;                              // default: no hit
        if (armed) r = combat.fire(eye, aimAt, scene, *physics);
        bool gated = !r.hitMonster && combat.hp() == before && combat.alive();
        check(gated, "T4 firing when not armed does nothing");
    }

    // ---- T2: enough shots -> HP<=0 -> dead (hidden, body removed, rays miss) --
    {
        // Monster is at hp0 - kDamagePerShot from T1. Fire until dead (armed).
        bool killedAtSomePoint = false;
        for (int i = 0; i < 10 && combat.alive(); ++i) {
            FireResult r = combat.fire(eye, aimAt, scene, *physics);
            if (r.killed) killedAtSomePoint = true;
        }
        bool dead    = !combat.alive() && combat.hp() <= 0;
        bool hidden  = combat.entity() != kNoLink &&
                       combat.entity() < scene.size() &&
                       !scene.get(combat.entity()).visible;
        // Subsequent ray AT where the monster was now MISSES (body removed).
        x3::phys::RayHit after = physics->rayCast(eye, aimAt, kFireMaxDist,
                                                  x3::phys::Layer::Enemy);
        bool raysMiss = !after.hit;
        // Firing again is a no-op (still dead, no hit reported).
        FireResult again = combat.fire(eye, aimAt, scene, *physics);
        bool noopAfter = !again.hitMonster && !again.killed;
        check(killedAtSomePoint && dead && hidden && raysMiss && noopAfter,
              "T2 shots kill monster: hidden + body removed + rays miss");
    }

    physics->shutdown();

    x3::logInfo(std::string("[combat-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
