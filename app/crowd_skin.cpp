// SKINNED CITIZENS implementation — see app/crowd_skin.h for the design stance.
// Game/slice code only — engine/ stays pure.

#include "crowd_skin.h"
#include "asset_root.h"
#include "headless_device.h"
#include "mesh_prims.h"          // x3::prims::makeBox — the seat-prop fallback mesh
#include "engine/asset/IModelLoader.h"  // load the real crate GLB + makeDrawables/mulMat4
#include "engine/asset/IAssetSource.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace x3::game {

namespace {

// Gesture mapping: the blockout expresses cower/seated as a vertical CROUCH
// scale; a skinned person can't be Y-squashed (it reads as a melted rig), so
// the crouch maps to a modest forward huddle-lean + a small Y dip instead.
constexpr float kCrouchLean = 0.55f;   // rad of extra lean per unit of crouch
constexpr float kCrouchDip  = 0.35f;   // m of Y dip per unit of crouch
// Fed-speed clamp: a region-rebuild pose snap must never read as a 100 m/s
// sprint to the locomotion blend.
constexpr float kMaxFedSpeed = 6.0f;

} // namespace

const std::vector<std::string>& CrowdSkin::defaultRigs() {
    // Upright Y-up humanoids with REAL Idle/Walk/Run locomotion sets (the
    // Blender anim-bake pipeline artifacts). AnnaCasual_anim also carries a
    // Talk clip (conversations). AnnaTactical/BartenderDanny/marcus_webb (base)
    // are Idle-only — they'd slide while walking, so they stay off the roster.
    static const std::vector<std::string> kRigs = {
        "AnnaCasual_anim.glb",
        "marcus_webb_anim.glb",
        "chief_martinez_anim.glb",
    };
    return kRigs;
}

void CrowdSkin::build(const CrowdSkinConfig& cfg, const CrowdSystem& crowd) {
    if (!crowd.built()) return;
    const uint32_t n = crowd.agentCount();
    if (m_slots.empty()) {
        // First build: plan the pool. Spawns are DEFERRED to update().
        m_cfg = cfg;
        if (m_cfg.rigs.empty()) m_cfg.rigs = defaultRigs();
        if (m_cfg.modelDir.empty()) m_cfg.modelDir = riggedGlbRoot();
        if (m_cfg.spawnsPerFrame == 0) m_cfg.spawnsPerFrame = 1;
        m_slots.resize(n);
        m_nextSpawn = 0;
    }
    // (Re)attach contract: the crowd was rebuilt (fresh blockout entities), so
    // every slot must re-swap over its new agent. Pose deltas reset so the
    // first frame doesn't read a teleport as a sprint.
    for (Slot& s : m_slots) {
        s.attached = false;
        s.hasLastPos = false;
        s.gesture = nullptr;
    }
    m_roomId = crowd.config().roomId;
    m_active = true;
}

void CrowdSkin::spawnOne(uint32_t i, const CrowdSystem& crowd, Scene& scene,
                         x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics) {
    Slot& s = m_slots[i];
    const CrowdAgent& a = crowd.agent(i);
    const std::string& rig = m_cfg.rigs[(i + m_cfg.seed) % m_cfg.rigs.size()];

    MonsterSystem::Tuning t;
    t.type       = MonsterType::Guard;
    t.chaseSpeed = 0.0f;               // INERT prop: the crowd brain owns motion
    t.damage     = 0;                  // never attacks
    t.ranged     = false;
    t.noBody     = true;               // pure visual — no Enemy hitbox for rays to eat
    t.lockRootY  = true;               // crowd Y is BRAIN-owned: a broken baked clip
                                       // root Y (AnnaCasual_anim Walk/Run bake bug)
                                       // must never bury/bob a citizen (Sit still
                                       // lowers the hips — it rides the crossfade)
    t.modelFile  = rig;
    t.modelDirOverride = m_cfg.modelDir;
    t.standUpZtoY = false;             // roster rigs are Y-up (canon-play precedent)
    t.modelScale  = m_cfg.scale;
    // Clothing tint: the agent's blockout palette color, softened toward white
    // so it reads as a subtle wardrobe wash on the textured rig (rig repeats
    // stop reading as twins) instead of a full-body paint job.
    if (a.entity != kNoLink && a.entity < scene.size()) {
        const Entity& be = scene.get(a.entity);
        for (int c = 0; c < 3; ++c)
            t.tint[c] = 1.0f - 0.35f * (1.0f - be.baseColor[c]);
        t.tint[3] = 1.0f;
    }

    const auto t0 = std::chrono::steady_clock::now();
    s.sys = std::make_unique<MonsterSystem>();
    s.sys->buildMonsterTuned(scene, device, physics, m_cfg.modelDir, a.pos, t);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    m_totalSpawnMs += ms;

    if (!s.sys->usingRealModel() || !s.sys->skinnable()) {
        // FALLBACK: keep the blockout. Hide the character's bookkeeping entity
        // (its render mesh is invalid anyway) and drop the instance for good.
        const uint32_t ent = s.sys->entity();
        if (ent != kNoLink && ent < scene.size()) scene.get(ent).visible = false;
        s.sys.reset();
        s.failed = true;
        x3::logWarn("[crowd-skin] " + m_cfg.site + ": agent " + std::to_string(i) +
                    " rig '" + rig + "' unavailable — blockout kept (" +
                    std::to_string(ms) + " ms)");
        return;
    }
    s.skinned = true;
    // Stamp the deployment room on the character entity (drawManagerCulled
    // parity; the layer draw gate reads the same id).
    const uint32_t ent = s.sys->entity();
    if (ent != kNoLink && ent < scene.size()) scene.get(ent).roomId = m_roomId;
    x3::logInfo("[crowd-skin] " + m_cfg.site + ": agent " + std::to_string(i) +
                " -> " + rig + " (" + std::to_string(ms) + " ms, total " +
                std::to_string(m_totalSpawnMs) + " ms)");
}

void CrowdSkin::attach(uint32_t i, const CrowdSystem& crowd, Scene& scene) {
    Slot& s = m_slots[i];
    const CrowdAgent& a = crowd.agent(i);
    // Swap: hide the blockout, show the character, snap the pose to the agent.
    if (a.entity != kNoLink && a.entity < scene.size())
        scene.get(a.entity).visible = false;
    const uint32_t ent = s.sys->entity();
    if (ent != kNoLink && ent < scene.size()) {
        Entity& e = scene.get(ent);
        e.visible = true;
        e.roomId = m_roomId;
    }
    s.sys->setPropPose(a.pos, a.yaw);
    s.sys->setPropMotion(0.0f, 0.0f);
    s.hasLastPos = false;
    s.attached = true;
    s.ragdolled = false;   // a fresh agent stands (any prior corpse is forgotten)
}

void CrowdSkin::updateSeat(Slot& s, const CrowdAgent& a, bool seated,
                           Scene& scene, x3::rhi::IRenderDevice& device) {
    // Seat height: the Sit clip is a knees-bent perch whose hips rest ~0.44 m above
    // the feet (grounded-verified in Blender against AnnaCasual's Sit pose). Whatever
    // prop we drop under the agent must present its SEAT SURFACE at ~0.44 m so the
    // citizen sits ON something instead of squatting on air; feet stay on the floor
    // (a.pos.y), the prop rests on the same floor, centred under the agent.
    constexpr float kSeatTop  = 0.44f;   // target seat-surface height above the feet
    constexpr float kSeatHalf = 0.22f;   // fallback box half-extent (top = 2*half = 0.44)
    // Real crate prop (SciFi_Warehouse_Kit/Crate Short.glb) measured in Blender
    // (engineY == Blender Z): height 0.600 m, footprint centred at engine
    // (X=-0.334, Z=+0.3355), base at Y=0. We uniformly scale it so its top lands on
    // kSeatTop and offset it so its footprint centres under the agent, feet on floor.
    constexpr float kCrateH   = 0.600f;  // crate height in its own space
    constexpr float kCrateCx  = -0.334f; // engine-X centre of the crate footprint
    constexpr float kCrateCz  =  0.3355f;// engine-Z centre of the crate footprint
    constexpr const char* kSeatCrateRel = "SciFi_Warehouse_Kit/Crate Short.glb";
    if (!seated) {
        if (s.seatEnt != kNoLink && s.seatEnt < scene.size())
            scene.get(s.seatEnt).visible = false;
        return;
    }
    const float scl = (m_cfg.scale > 0.01f) ? m_cfg.scale : 1.0f;
    // Lazy-build the shared seat prop once (one load, instanced across all seats).
    if (!m_seatMeshBuilt) {
        m_seatMeshBuilt = true;   // attempt exactly once; box fallback on any failure
        // Prefer the REAL textured crate. On a real device makeDrawables yields the
        // crate's mesh + material handles; on the headless self-test device it yields
        // nothing, so we fall through to the procedural box below.
        m_seatAssets.reset(x3::asset::createAssetSource());
        if (m_seatAssets && m_seatAssets->mountDir(convertedGlbRoot(), 0)) {
            m_seatLoader.reset(x3::asset::createModelLoader(&device, m_seatAssets.get()));
            if (m_seatLoader) {
                m_seatModel = m_seatLoader->load(kSeatCrateRel);
                if (m_seatModel.ok) {
                    std::vector<x3::asset::ModelDrawable> dr = x3::asset::makeDrawables(m_seatModel);
                    if (!dr.empty()) {
                        const x3::asset::ModelDrawable& d = dr[0];   // crate is a single mesh
                        m_seatMesh      = x3::rhi::MeshHandle{ d.meshId };
                        m_seatTex       = x3::rhi::TextureHandle{ d.baseColorTexId };
                        m_seatMrTex     = x3::rhi::TextureHandle{ d.mrTexId };
                        m_seatNormalTex = x3::rhi::TextureHandle{ d.normalTexId };
                        for (int k = 0; k < 16; ++k) m_seatNode[k]      = d.nodeTransform[k];
                        for (int k = 0; k < 4;  ++k) m_seatBaseColor[k] = d.baseColorFactor[k];
                        m_seatIsProp = true;
                        x3::logInfo("[crowd-skin] " + m_cfg.site + ": seat prop = " +
                                    std::string(kSeatCrateRel));
                    }
                }
            }
        }
        if (!m_seatIsProp) {
            x3::prims::PrimMesh m = x3::prims::makeBox(kSeatHalf, kSeatHalf, kSeatHalf, 0, 0, 0, 1.0f);
            m_seatMesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                           m.index.data(), (uint32_t)m.index.size());
            x3::logInfo("[crowd-skin] " + m_cfg.site + ": seat prop = procedural box (crate GLB unavailable)");
        }
    }
    if (s.seatEnt == kNoLink) {
        Entity e;
        e.mesh = m_seatMesh;
        if (m_seatIsProp) {
            e.tex = m_seatTex; e.mrTex = m_seatMrTex; e.normalTex = m_seatNormalTex;
            for (int k = 0; k < 4; ++k) e.baseColor[k] = m_seatBaseColor[k];
        } else {
            e.baseColor[0] = 0.28f; e.baseColor[1] = 0.20f; e.baseColor[2] = 0.13f; e.baseColor[3] = 1.0f; // crate wood
        }
        e.tag     = (uint32_t)Tag::Prop;
        e.roomId  = m_roomId;    // Scene::render PVS-culls with the crowd's room
        e.visible = false;
        s.seatEnt = scene.add(e);
    }
    if (s.seatEnt >= scene.size()) return;
    Entity& e = scene.get(s.seatEnt);
    if (m_seatIsProp) {
        // Uniform scale so the crate's 0.600 m height maps to kSeatTop*scl, base on the
        // floor (crate base Y==0), footprint centred under the agent; then bake the
        // crate's node transform: model = object(scale+translate) * nodeTransform.
        const float fit = (kSeatTop * scl) / kCrateH;
        float obj[16] = {0};
        obj[0] = obj[5] = obj[10] = fit; obj[15] = 1.0f;
        obj[12] = a.pos.x - fit * kCrateCx;
        obj[13] = a.pos.y;                       // crate base is at its own Y=0
        obj[14] = a.pos.z - fit * kCrateCz;
        x3::asset::mulMat4(obj, m_seatNode, e.transform);
    } else {
        // Fallback box: uniform scale + translation; centre one half-height up so
        // top = floor + 0.44*scl.
        for (int k = 0; k < 16; ++k) e.transform[k] = 0.0f;
        e.transform[0] = e.transform[5] = e.transform[10] = scl;
        e.transform[15] = 1.0f;
        e.transform[12] = a.pos.x;
        e.transform[13] = a.pos.y + kSeatHalf * scl;
        e.transform[14] = a.pos.z;
    }
    e.roomId  = m_roomId;
    e.visible = true;
}

bool CrowdSkin::triggerRagdoll(uint32_t i, Scene& scene,
                               x3::phys::IPhysicsWorld& physics,
                               const x3::phys::Vec3& shove) {
    if (i >= m_slots.size()) return false;
    Slot& s = m_slots[i];
    if (!s.skinned || !s.sys || !s.attached || s.ragdolled) return false;
    const bool flopped = s.sys->triggerRagdoll(scene, physics, shove);
    // Latch even if the model was unrigged (it can't flop, but it's dead): the pose
    // feed still stops so a "killed" citizen doesn't keep walking its schedule.
    s.ragdolled = true;
    return flopped;
}

bool CrowdSkin::agentRagdolled(uint32_t i) const {
    return i < m_slots.size() && m_slots[i].ragdolled;
}

void CrowdSkin::update(float dt, const CrowdSystem& crowd, Scene& scene,
                       x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics) {
    if (!m_active || !crowd.built()) return;
    const uint32_t n = std::min((uint32_t)m_slots.size(), crowd.agentCount());

    // ---- Deferred spawn drain (budgeted; runs even while the room is culled
    // so the pool fills during the opening seconds). ----
    uint32_t spawned = 0;
    while (m_nextSpawn < (uint32_t)m_slots.size() && spawned < m_cfg.spawnsPerFrame) {
        const uint32_t i = m_nextSpawn;
        Slot& s = m_slots[i];
        if (s.sys || s.failed || i >= n) { ++m_nextSpawn; continue; }
        spawnOne(i, crowd, scene, device, physics);
        ++m_nextSpawn;
        ++spawned;
        if (m_nextSpawn >= (uint32_t)m_slots.size())
            x3::logInfo("[crowd-skin] " + m_cfg.site + ": pool complete — " +
                        std::to_string(skinnedCount()) + "/" + std::to_string(n) +
                        " skinned, " + std::to_string(m_totalSpawnMs) + " ms total");
    }

    // ---- Pose-follow (PVS-gated: a culled room's crowd is frozen anyway). ----
    if (!scene.roomVisible(m_roomId)) return;
    for (uint32_t i = 0; i < n; ++i) {
        Slot& s = m_slots[i];
        if (!s.skinned || !s.sys) continue;
        // Shot dead: the death ragdoll owns the skin now. Stop feeding the pose (it
        // would fight the flop) but keep TICKING the character so update() reads the
        // ragdoll bones back and settles/despawns the corpse on its usual timers.
        if (s.ragdolled) { s.sys->update(dt, scene, physics, s.sys->pos()); continue; }
        if (!s.attached) attach(i, crowd, scene);
        const CrowdAgent& a = crowd.agent(i);

        // The agent's OWN planar speed drives the Idle/Walk/Run blend.
        float speed = 0.0f;
        if (s.hasLastPos && dt > 1e-5f) {
            const float dx = a.pos.x - s.lastPos.x, dz = a.pos.z - s.lastPos.z;
            speed = std::min(std::sqrt(dx * dx + dz * dz) / dt, kMaxFedSpeed);
        }
        s.lastPos = a.pos;
        s.hasLastPos = true;
        s.lastSpeed = speed;

        // Gestures ON TOP (the crowd already computed them): bob rides the pose
        // Y; crouch maps to huddle-lean + dip (see kCrouch*); lean is the torso
        // pitch toward the facing.
        const float huddle = 1.0f - a.visCrouch;
        const float lean = a.visLean + huddle * kCrouchLean;
        const float dip  = huddle * kCrouchDip;
        s.sys->setPropPose(
            x3::phys::Vec3{ a.pos.x, a.pos.y + a.visBob - dip, a.pos.z }, a.yaw);
        s.sys->setPropMotion(speed, lean);

        // ANIM-ENRICH: living-world GESTURES. Map the agent's STATE (while roughly
        // stationary) to an authored calm-loop clip: Converse->talk gesture,
        // Work->task loop, Play(seated knot)->sit. IDLE citizens get living-world
        // VARIETY -- a stable per-agent pick of look-around / check-a-handheld /
        // carry-something (one in four just stands) so a settled crowd reads as
        // people living life, not a rank of identical idlers. setCalmLoop
        // fuzzy-finds the clip; rigs lacking it keep the procedural nod/lean
        // fallback (calm-loop stays -1). Only re-issue when the desired key CHANGES
        // (the pick is stable per agent, so no per-frame findClip churn).
        static const char* const kIdleGestures[4] = {
            "lookaround", "checkdevice", "carryidle", nullptr /* plain idle */ };
        const char* want = nullptr;
        if (speed < 0.15f) {
            switch (a.state) {
                case CrowdState::Converse: want = "converse"; break;
                case CrowdState::Work:     want = "work";     break;
                case CrowdState::Play:     want = "sit";      break;  // seated hangout knot
                case CrowdState::Idle:     want = kIdleGestures[(i + m_cfg.seed) & 3]; break;
                default: break;
            }
        }
        if (want != s.gesture) {
            if (want) s.sys->setCalmLoop(want);
            else      s.sys->clearCalmLoop();
            s.gesture = want;
        }

        // Seat prop: show a crate under the agent ONLY when it is actually seated —
        // i.e. the Sit gesture is engaged AND this rig owns a Sit clip (calmLoopActive
        // is false on rigs that lack it, so those keep the procedural huddle with no
        // stray crate). Placed/hidden every frame; Scene::render PVS-culls it.
        const bool seated = want && std::strcmp(want, "sit") == 0 && s.sys->calmLoopActive();
        updateSeat(s, a, seated, scene, device);

        // Tick the inert character (skinning + clip playback; chaseSpeed 0 and
        // playerPos = self => the AI never fights the fed pose).
        s.sys->update(dt, scene, physics, s.sys->pos());
    }
}

void CrowdSkin::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const Scene& scene) const {
    if (!m_active) return;
    if (!scene.roomVisible(m_roomId)) return;   // drawManagerCulled parity
    for (const Slot& s : m_slots)
        if (s.skinned && s.attached && s.sys) s.sys->drawMonster(device, frame, scene);
}

void CrowdSkin::deactivate(Scene& scene) {
    // Region eviction: hide the characters, detach from the (dying) agents.
    // The pool (loaded rigs + bookkeeping entities) is host-owned and SURVIVES
    // — the next build() re-attaches with zero reloads. Never touch the
    // blockout entities here: the region ledger owns and destroys them.
    for (Slot& s : m_slots) {
        if (s.sys) {
            const uint32_t ent = s.sys->entity();
            if (ent != kNoLink && ent < scene.size()) scene.get(ent).visible = false;
        }
        // Hide the seat prop too (host-owned, survives for the next build()).
        if (s.seatEnt != kNoLink && s.seatEnt < scene.size())
            scene.get(s.seatEnt).visible = false;
        s.attached = false;
        s.hasLastPos = false;
        s.gesture = nullptr;
        s.ragdolled = false;
    }
    m_active = false;
}

uint32_t CrowdSkin::skinnedCount() const {
    uint32_t c = 0;
    for (const Slot& s : m_slots) if (s.skinned) ++c;
    return c;
}

uint32_t CrowdSkin::pendingCount() const {
    uint32_t c = 0;
    for (uint32_t i = m_nextSpawn; i < (uint32_t)m_slots.size(); ++i)
        if (!m_slots[i].sys && !m_slots[i].failed) ++c;
    return c;
}

bool CrowdSkin::agentSkinned(uint32_t i) const {
    return i < m_slots.size() && m_slots[i].skinned;
}

const MonsterSystem* CrowdSkin::character(uint32_t i) const {
    return (i < m_slots.size()) ? m_slots[i].sys.get() : nullptr;
}

float CrowdSkin::lastFedSpeed(uint32_t i) const {
    return (i < m_slots.size()) ? m_slots[i].lastSpeed : 0.0f;
}

// ===========================================================================
// Headless self-test section (--test-crowd, S1..S5) — see crowd_skin.h.
// ===========================================================================

namespace {

int cs_pass = 0, cs_fail = 0;
void cscheck(bool cond, const char* name) {
    if (cond) { ++cs_pass; x3::logInfo(std::string("[crowd-skin-test] PASS ") + name); }
    else      { ++cs_fail; x3::logError(std::string("[crowd-skin-test] FAIL ") + name); }
}

class SkinCountingDevice final : public HeadlessRenderDevice {
public:
    uint32_t meshCreates = 0, texCreates = 0;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                   const uint32_t* idx, uint32_t ni) override {
        ++meshCreates;
        return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
    }
    x3::rhi::TextureHandle createTexture(const void* px, uint32_t w, uint32_t h,
                                         bool mips) override {
        ++texCreates;
        return HeadlessRenderDevice::createTexture(px, w, h, mips);
    }
};

} // namespace

bool runCrowdSkinSelfTest() {
    cs_pass = cs_fail = 0;
    const float dt = 1.0f / 60.0f;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    SkinCountingDevice device;
    Scene scene;

    // A small living deployment: 6 agents, one Carry work point, wanderers with
    // conversations on — enough to exercise walk/idle/talk clip selection.
    CrowdSystem crowd;
    CrowdConfig cfg;
    cfg.count = 6;
    cfg.centerX = 0.0f; cfg.centerZ = 0.0f; cfg.groundY = 0.0f;
    cfg.radius = 14.0f;
    cfg.converse = true;
    cfg.points = { -4.0f, 0.0f,  4.0f, 0.0f,  0.0f, 4.0f,  0.0f, -4.0f };
    {
        CrowdWorkPoint carry; carry.kind = CrowdWorkPoint::Kind::Carry;
        carry.ax = -7.0f; carry.az = -7.0f; carry.bx = -1.0f; carry.bz = -7.0f;
        cfg.work = { carry };
    }
    crowd.build(cfg, scene, device);

    CrowdSkin skin;
    CrowdSkinConfig scfg;
    scfg.site = "self-test";
    scfg.modelDir = riggedGlbRoot();
    scfg.spawnsPerFrame = 2;
    skin.build(scfg, crowd);

    auto tick = [&](int frames) {
        for (int f = 0; f < frames; ++f) {
            crowd.update(dt, scene);
            skin.update(dt, crowd, scene, device, *physics);
        }
    };

    // ---- S1: the layer binds real rigs (CPU-skin fallback on the headless
    // device) and hides the blockouts of every skinned agent. ----
    tick(10);   // 2/frame drains 6 spawns in 3 frames; a few extra to attach
    {
        bool blockoutsHidden = true, charsVisible = true, cpuBound = true;
        for (uint32_t i = 0; i < crowd.agentCount(); ++i) {
            if (!skin.agentSkinned(i)) continue;
            const CrowdAgent& a = crowd.agent(i);
            if (scene.get(a.entity).visible) blockoutsHidden = false;
            const MonsterSystem* c = skin.character(i);
            if (!c || !c->skinnable() || !c->usingRealModel()) cpuBound = false;
            const uint32_t ent = c ? c->entity() : kNoLink;
            if (ent == kNoLink || !scene.get(ent).visible) charsVisible = false;
        }
        cscheck(skin.pendingCount() == 0 && skin.skinnedCount() == crowd.agentCount() &&
                blockoutsHidden && charsVisible && cpuBound,
                "S1 skinned layer binds every agent (blockouts hidden, rigs skinnable)");
    }

    // ---- S2: clip selection follows the crowd's OWN speed: a moving agent
    // feeds >= the 0.2 m/s walk threshold, a settled one feeds idle; the skin
    // pose tracks the agent. ----
    {
        bool sawWalk = false, sawIdle = false, tracks = true;
        for (int f = 0; f < 60 * 30 && !(sawWalk && sawIdle); ++f) {
            tick(1);
            for (uint32_t i = 0; i < crowd.agentCount(); ++i) {
                if (!skin.agentSkinned(i)) continue;
                const CrowdAgent& a = crowd.agent(i);
                const float sp = skin.lastFedSpeed(i);
                if ((a.state == CrowdState::Wander || a.state == CrowdState::Work) &&
                    sp > 0.2f) sawWalk = true;
                if (a.state == CrowdState::Idle && sp < 0.2f) sawIdle = true;
                const MonsterSystem* c = skin.character(i);
                const x3::phys::Vec3 cp = c->pos();
                const float dx = cp.x - a.pos.x, dz = cp.z - a.pos.z;
                if (dx * dx + dz * dz > 0.01f) tracks = false;   // pose follows
            }
        }
        cscheck(sawWalk, "S2 a walking agent feeds the locomotion blend above 0.2 m/s");
        cscheck(sawIdle, "S2b a settled agent feeds idle (below the walk threshold)");
        cscheck(tracks,  "S2c the skinned pose tracks the agent position");
    }

    // ---- S3: a bogus rig path falls back to the blockout (never break the
    // world). ----
    {
        Scene s3;
        CrowdSystem c3;
        CrowdConfig cfg3;
        cfg3.count = 3; cfg3.radius = 8.0f;
        c3.build(cfg3, s3, device);
        CrowdSkin k3;
        CrowdSkinConfig f3;
        f3.site = "bogus";
        f3.modelDir = riggedGlbRoot();
        f3.rigs = { "definitely_not_a_rig.glb" };
        f3.spawnsPerFrame = 4;
        k3.build(f3, c3);
        for (int f = 0; f < 6; ++f) {
            c3.update(dt, s3);
            k3.update(dt, c3, s3, device, *physics);
        }
        bool blockoutsVisible = true;
        for (uint32_t i = 0; i < c3.agentCount(); ++i)
            if (!s3.get(c3.agent(i).entity).visible) blockoutsVisible = false;
        cscheck(k3.skinnedCount() == 0 && k3.pendingCount() == 0 && blockoutsVisible,
                "S3 a bogus rig falls back to the visible blockout (no crash)");
    }

    // ---- S4: leak canary — a full pool creates nothing new across ticking. ----
    {
        const uint32_t meshes = device.meshCreates, texes = device.texCreates;
        const uint32_t ents = scene.size();
        tick(300);
        cscheck(device.meshCreates == meshes && device.texCreates == texes &&
                scene.size() == ents,
                "S4 leak/budget (no new meshes/textures/entities once the pool is full)");
    }

    // ---- S5: deactivate hides the skins; a re-build over a fresh crowd
    // re-attaches the SAME pool with zero new uploads (the stream cycle). ----
    {
        skin.deactivate(scene);
        bool hidden = true;
        for (uint32_t i = 0; i < crowd.agentCount(); ++i) {
            const MonsterSystem* c = skin.character(i);
            if (c && c->entity() != kNoLink && scene.get(c->entity()).visible)
                hidden = false;
        }
        // Region cycle: the crowd abandons + rebuilds (fresh blockout agents).
        crowd.abandon();
        crowd.build(cfg, scene, device);
        const uint32_t meshes = device.meshCreates, texes = device.texCreates;
        skin.build(scfg, crowd);
        tick(5);
        bool reattached = true;
        for (uint32_t i = 0; i < crowd.agentCount(); ++i) {
            if (!skin.agentSkinned(i)) { reattached = false; continue; }
            if (scene.get(crowd.agent(i).entity).visible) reattached = false;
            const MonsterSystem* c = skin.character(i);
            if (!c || !scene.get(c->entity()).visible) reattached = false;
        }
        cscheck(hidden, "S5 deactivate() hides every skinned character");
        cscheck(reattached && device.meshCreates == meshes && device.texCreates == texes,
                "S5b re-build re-attaches the pool with ZERO reloads (stream cycle)");
    }

    // ---- S6: a shot citizen FLOPS — the harvested death-flop wired onto the
    // per-agent MonsterSystem. triggerRagdoll latches the slot (pose-follow stops)
    // and, on a skinnable rig, drives the skin from a physics ragdoll. ----
    {
        // Find a skinned + attached agent and shoot it.
        int victim = -1;
        for (uint32_t i = 0; i < crowd.agentCount(); ++i)
            if (skin.agentSkinned(i) && skin.character(i)) { victim = (int)i; break; }
        bool flopped = false, latched = false, ragActive = false;
        if (victim >= 0) {
            flopped = skin.triggerRagdoll((uint32_t)victim, scene, *physics,
                                          x3::phys::Vec3{ 1.0f, 0.2f, 0.0f });
            latched = skin.agentRagdolled((uint32_t)victim);
            // Tick a few frames so update() drives the flop.
            tick(20);
            const MonsterSystem* c = skin.character((uint32_t)victim);
            ragActive = c && c->ragdollActive();
        }
        cscheck(victim >= 0 && flopped && latched && ragActive,
                "S6 a shot citizen ragdolls (death-flop wired onto the crowd skin)");
    }

    x3::logInfo("crowd-skin: " + std::to_string(cs_pass) + "/" +
                std::to_string(cs_pass + cs_fail) + " passed");
    return cs_fail == 0;
}

} // namespace x3::game
