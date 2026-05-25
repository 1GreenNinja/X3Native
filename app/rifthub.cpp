// EFLZ Portal Hub — see rifthub.h for the design overview.
//
// Clean-room: built ONLY from X3Native's own Scene / trigger / mesh_prims
// systems + the engine interfaces. No RBDOOM / id Tech / Doom / Quake — or any
// other game-engine — source was consulted. CONTENT/LEVEL-SCRIPT ONLY.
#include "rifthub.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace x3::game {

namespace {

// Hub layout: 8 portals arranged on a circle of radius kRingRadius around
// the spawn point at world origin. Each portal "ring" is two stacked thin
// emissive boxes (lower + upper arc-readable slabs) at the same XZ; a small
// emissive floor-plate sits at its base. Trigger volumes are wider than the
// ring so the player only needs to step ONTO the floor plate (not thread
// the ring) to fire the rift.
constexpr float kHubHalf       = 20.0f;   // 40 m square floor (footprint half)
constexpr float kRingRadius    = 14.0f;   // portal placement radius (around spawn)
constexpr float kRingHeight    = 2.5f;    // visible ring height (top slab top)
constexpr float kRingHalfThick = 0.15f;   // thin slab half-thickness
constexpr float kRingHalfArc   = 1.25f;   // slab half-width (the "ring" reads as a wide bar)
constexpr float kPlateHalfXZ   = 1.50f;   // floor-plate half-extent
constexpr float kPlateHalfY    = 0.05f;   // floor-plate thin slab half-Y
constexpr float kTrigHalfXZ    = 2.5f;    // trigger volume half-extent (wider than plate)
constexpr float kTrigHalfY     = 2.0f;    // trigger volume vertical half-extent

// Player spawn Y (capsule feet): standing on the ground plane (which sits at
// y = -kPlateHalfY*2 = -0.10 below the world origin; feet at +0 is fine).
constexpr float kSpawnFeetY    = 0.05f;

// Portal authoring table — ORDER is the clockwise arrangement around the hub
// starting at +X (angle 0 -> -X around -Y rotation; we iterate i=0..7 at
// angle = i * (2pi/8) so portal 0 is at +X, 1 at +X+Z, 2 at +Z, etc.).
struct PortalSpec {
    const char*    worldName;
    uint32_t       triggerId;
    float          tint[3];   // emissive color (HDR-ish; strength applied below)
};
// Tint palette per the task spec:
//   act2caves: violet | act2: orange-amber | valley: cyan | cliffs: white-gold
//   club: magenta | destruct: red | ragdoll: green | terrain: sky-blue
constexpr PortalSpec kPortalTable[] = {
    { "act2caves", (uint32_t)RifthubTrigger::Act2Caves, { 0.75f, 0.30f, 1.00f } }, // violet
    { "act2",      (uint32_t)RifthubTrigger::Act2,      { 1.00f, 0.55f, 0.15f } }, // orange-amber
    { "valley",    (uint32_t)RifthubTrigger::Valley,    { 0.20f, 0.85f, 1.00f } }, // cyan
    { "cliffs",    (uint32_t)RifthubTrigger::Cliffs,    { 1.00f, 0.92f, 0.65f } }, // white-gold
    { "club",      (uint32_t)RifthubTrigger::Club,      { 1.00f, 0.20f, 0.85f } }, // magenta
    { "destruct",  (uint32_t)RifthubTrigger::Destruct,  { 1.00f, 0.20f, 0.15f } }, // red
    { "ragdoll",   (uint32_t)RifthubTrigger::Ragdoll,   { 0.30f, 1.00f, 0.40f } }, // green
    { "terrain",   (uint32_t)RifthubTrigger::Terrain,   { 0.45f, 0.70f, 1.00f } }, // sky-blue
};
constexpr uint32_t kPortalCount = (uint32_t)(sizeof(kPortalTable) / sizeof(kPortalTable[0]));
static_assert(kPortalCount == kRifthubTrigCount, "kRifthubTrigCount must match the portal table");

// Add a thin emissive slab to the scene (one of the two ring bars or the
// floor plate). Returns the mesh handle so the hub can free it at shutdown.
x3::rhi::MeshHandle addEmissiveSlab(Scene& scene, x3::rhi::IRenderDevice& device,
                                    float hx, float hy, float hz,
                                    const x3::phys::Vec3& center,
                                    const float tint[3], float emStrength) {
    x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, center.x, center.y, center.z);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = 1.0f;
    e.emissive[0] = tint[0]; e.emissive[1] = tint[1]; e.emissive[2] = tint[2];
    e.emissive[3] = emStrength;
    e.tag = (uint32_t)Tag::Prop;
    scene.add(e);
    return e.mesh;
}

} // namespace

void Rifthub::build(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers) {
    if (m_built) return;

    // ===== Spawn point (center of the ring) =====
    m_spawn = x3::phys::Vec3{ 0.0f, kSpawnFeetY, 0.0f };

    // ===== Ground (static collision + a render quad) =====
    // 40x40 m flat slab at y=-0.10 so the slab TOP sits at y=0 (the world Y=0
    // plane every other graybox uses). Mirrors destruct_demo's ground pattern.
    {
        x3::prims::PrimMesh g = x3::prims::makeBox(kHubHalf, 0.10f, kHubHalf,
                                                    0.0f, -0.10f, 0.0f, 0.25f);
        m_groundMesh = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                         g.index.data(), (uint32_t)g.index.size());
        // A muted grey-blue checker so the emissive portals read brightly against it.
        auto groundPx = x3::prims::makeCheckerRGBA(64, 8, 110, 116, 130, 60, 64, 76);
        m_groundTex = device.createTexture(groundPx.data(), 64, 64, true);
        Entity ge;
        ge.mesh = m_groundMesh;
        ge.tex  = m_groundTex;
        ge.baseColor[0] = 1.0f; ge.baseColor[1] = 1.0f; ge.baseColor[2] = 1.0f;
        ge.baseColor[3] = 1.0f;
        ge.tag = (uint32_t)Tag::Static;
        scene.add(ge);
        physics.addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                              g.cindex.data(), (uint32_t)g.cindex.size());
    }

    // ===== Portals (clockwise ring around the spawn) =====
    m_portals.clear();
    m_portals.reserve(kPortalCount);
    m_portalMeshes.reserve(kPortalCount * 3);   // 2 ring slabs + 1 plate per portal

    const float twoPi = 6.2831853f;
    for (uint32_t i = 0; i < kPortalCount; ++i) {
        const PortalSpec& sp = kPortalTable[i];
        // Angle for portal i — clockwise starting at +X.
        const float ang = (float)i * (twoPi / (float)kPortalCount);
        const float cx = std::cos(ang) * kRingRadius;
        const float cz = std::sin(ang) * kRingRadius;

        RiftPortal p;
        p.worldName  = sp.worldName;
        p.triggerId  = sp.triggerId;
        p.worldPos   = x3::phys::Vec3{ cx, 0.0f, cz };
        p.tint[0] = sp.tint[0]; p.tint[1] = sp.tint[1]; p.tint[2] = sp.tint[2];
        p.activated  = false;
        m_portals.push_back(p);

        // Lower ring bar (knee-height) + upper ring bar (head-height): two
        // thin emissive slabs giving a visible "ring" silhouette from any
        // angle without needing a torus mesh.
        const x3::phys::Vec3 lowerCenter{ cx, 0.60f, cz };
        const x3::phys::Vec3 upperCenter{ cx, kRingHeight - kRingHalfThick, cz };
        m_portalMeshes.push_back(addEmissiveSlab(scene, device,
            kRingHalfArc, kRingHalfThick, kRingHalfArc,
            lowerCenter, sp.tint, /*emStrength=*/3.0f));
        m_portalMeshes.push_back(addEmissiveSlab(scene, device,
            kRingHalfArc, kRingHalfThick, kRingHalfArc,
            upperCenter, sp.tint, /*emStrength=*/3.2f));

        // Floor plate: a soft-glowing thin emissive slab on the ground at
        // the portal base. Doubles as the visible "step here" cue.
        const x3::phys::Vec3 plateCenter{ cx, kPlateHalfY, cz };
        m_portalMeshes.push_back(addEmissiveSlab(scene, device,
            kPlateHalfXZ, kPlateHalfY, kPlateHalfXZ,
            plateCenter, sp.tint, /*emStrength=*/1.5f));

        // Trigger volume: wider than the plate so the player only needs to
        // step ONTO the plate (not thread the ring) to fire the rift.
        const x3::phys::Vec3 tmin{ cx - kTrigHalfXZ, -kTrigHalfY, cz - kTrigHalfXZ };
        const x3::phys::Vec3 tmax{ cx + kTrigHalfXZ,  kTrigHalfY, cz + kTrigHalfXZ };
        triggers.add(tmin, tmax, sp.triggerId, /*enabled=*/true);
    }

    physics.optimizeBroadphase();
    m_built = true;
    x3::logInfo("[rifthub] hub built with " + std::to_string(m_portals.size()) +
                " portals (one per known --world target)");
}

void Rifthub::shutdown(x3::rhi::IRenderDevice& device) {
    if (!m_built) return;
    if (m_groundMesh.valid()) device.destroyMesh(m_groundMesh);
    if (m_groundTex.valid())  device.destroyTexture(m_groundTex);
    for (auto h : m_portalMeshes) if (h.valid()) device.destroyMesh(h);
    m_portalMeshes.clear();
    m_portals.clear();
    m_built = false;
}

void Rifthub::onTrigger(uint32_t triggerId) {
    for (auto& p : m_portals) {
        if (p.triggerId == triggerId) {
            if (!p.activated) {
                p.activated = true;
                std::string name = p.worldName ? p.worldName : "?";
                x3::logInfo(std::string("[rifthub] entered ") + name +
                            " rift — relaunch with --world " + name +
                            " to traverse");
            }
            return;
        }
    }
}

bool Rifthub::hudPromptForEye(const x3::phys::Vec3& eye, std::string& outPrompt,
                              float hudRadiusM) const {
    if (!m_built) return false;
    // Find the CLOSEST portal within hudRadiusM (XZ distance — eye Y is the
    // player's head height, portal anchors sit on the ground).
    int   bestIdx  = -1;
    float bestD2   = hudRadiusM * hudRadiusM;
    for (uint32_t i = 0; i < m_portals.size(); ++i) {
        const auto& p = m_portals[i];
        const float dx = eye.x - p.worldPos.x;
        const float dz = eye.z - p.worldPos.z;
        const float d2 = dx*dx + dz*dz;
        if (d2 < bestD2) { bestD2 = d2; bestIdx = (int)i; }
    }
    if (bestIdx < 0) return false;
    const auto& p = m_portals[(uint32_t)bestIdx];
    if (p.activated) {
        outPrompt = std::string("Rift activated: ") + (p.worldName ? p.worldName : "?");
    } else {
        outPrompt = std::string("Rift: ") + (p.worldName ? p.worldName : "?") +
                    " — walk in to activate";
    }
    return true;
}

bool Rifthub::allActivated() const {
    for (const auto& p : m_portals) if (!p.activated) return false;
    return !m_portals.empty();
}

} // namespace x3::game
