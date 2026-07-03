// EFLZ Portal Hub — see rifthub.h for the design overview.
//
// Clean-room: built ONLY from X3Native's own Scene / trigger / mesh_prims
// systems + the engine interfaces. No RBDOOM / id Tech / Doom / Quake — or any
// other game-engine — source was consulted. CONTENT/LEVEL-SCRIPT ONLY.
#include "rifthub.h"
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// Hub layout: 8 portals arranged on a circle of radius kRingRadius around the
// spawn point at world origin. Each portal is a vertical ROUND tech-gate ring
// — N tangent box segments arranged around a vertical circle (the ring's plane
// is perpendicular to the outward radial direction so the portal's doorway
// face points back at the hub center). Two concentric layers (outer + inner)
// give the gate a framed-doorway read. A small octagonal floor-plate sits at
// the base and a single thin "shimmer disk" floats at the ring center as the
// portal's energy core.
constexpr float kHubHalf       = 20.0f;   // 40 m square floor (footprint half)
constexpr float kRingRadius    = 14.0f;   // portal placement radius (around spawn)

// ---- Tech-gate ring geometry --------------------------------------------------
constexpr uint32_t kRingSegments   = 24;     // box segments per ring (N at 15° each)
constexpr float    kRingY          = 2.2f;   // ring center height above the floor
constexpr float    kOuterRingR     = 2.00f;  // outer ring radius (the visible "frame")
constexpr float    kInnerRingR     = 1.70f;  // inner ring radius (concentric layer)
constexpr float    kSegBoxThick    = 0.13f;  // segment half-thickness (radial)
constexpr float    kSegBoxDepth    = 0.13f;  // segment half-depth (along outward axis)
// Segment "long-axis" half-extent = half the chord between adjacent segment
// centers, with a small overlap so neighboring boxes butt cleanly without gaps.
// chord = 2 * R * sin(pi / N); half-chord = R * sin(pi / N).
inline float segHalfTangent(float ringR) {
    const float pi = 3.14159265358979f;
    return ringR * std::sin(pi / (float)kRingSegments) * 1.04f;  // 4% overlap
}

// ---- Floor plate (octagonal) --------------------------------------------------
// 8 small box wedges in a ring on the floor — a low-poly "disk" the player
// steps onto. Slightly elevated so it reads against the dark ground checker.
constexpr uint32_t kPlateSegments  = 8;
constexpr float    kPlateRingR     = 1.30f;  // plate ring radius (center of each wedge)
constexpr float    kPlateHalfY     = 0.05f;  // plate slab half-Y (flat)
constexpr float    kPlateBoxThick  = 0.50f;  // plate wedge half-extent (radial)
// Floor wedges butt at their outer edge — half-tangent = R * sin(pi/8) * 1.04 overlap.
inline float plateHalfTangent() {
    const float pi = 3.14159265358979f;
    return kPlateRingR * std::sin(pi / (float)kPlateSegments) * 1.04f;
}

// ---- Pool center (energy-core hot spot) -----------------------------------------
// Two small thin vertical slabs at the exact ring center — the brightest
// blue-white point of the event-horizon pool (the membrane bands fill the rest
// of the opening around them). Small so the round membrane silhouette wins.
constexpr float    kCoreHalfW      = 0.30f;  // 0.6m wide hot-center slab
constexpr float    kCoreHalfH      = 0.30f;  // 0.6m tall
constexpr float    kCoreHalfT      = 0.025f; // 0.05m thick (thin disk)

// ---- Trigger volume -----------------------------------------------------------
constexpr float kTrigHalfXZ    = 2.5f;    // trigger volume half-extent (wider than ring)
constexpr float kTrigHalfY     = 2.5f;    // trigger volume vertical half-extent

// Player spawn Y (capsule feet): standing on the ground plane (which sits at
// y = -0.10 below the world origin; feet at +0 is fine).
constexpr float kSpawnFeetY    = 0.05f;

// ---- Shimmer pulse constants (used by Rifthub::tick) --------------------------
constexpr float    kShimmerFreqHz     = 2.5f;            // ring oscillation rate (Hz)
constexpr float    kShimmerPhaseStep  = 0.5f;            // per-portal phase offset (rad)
constexpr float    kRingMinEmissive   = 1.5f;            // ring pulse min
constexpr float    kRingMaxEmissive   = 4.0f;            // ring pulse max

// ---- Electric-blue energy core ------------------------------------------------
// The destination tint stays on the OUTER + INNER ring frame (so portals are
// distinguishable), but the energy core + rim emitter nodes are a unifying
// electric blue — every portal reads as a blue wormhole inside a colored housing.
constexpr float    kCoreBlue[3]       = { 0.15f, 0.55f, 1.00f };  // electric blue
constexpr float    kCoreInnerBlue[3]  = { 0.82f, 0.93f, 1.00f };  // hot center (near-white-blue)
constexpr float    kCoreInnerHalfW    = 0.17f;          // inner hot-spot half-width
constexpr float    kCoreInnerHalfH    = 0.17f;          // inner hot-spot half-height
constexpr float    kCoreInnerHalfT    = 0.020f;         // inner disk half-thickness
constexpr float    kCoreBlueMinEm     = 4.0f;           // core blue pulse min
constexpr float    kCoreBlueMaxEm     = 9.0f;           // core blue pulse max (bright)
constexpr float    kCoreFreqHz        = 3.2f;           // core pulses faster than the frame

// ---- Wormhole-generator housing -----------------------------------------------
// Emitter struts: radial "machine arms" reaching outward from the ring at the
// cardinal angles, with a dim structural blue-grey body. They make the portal
// read as a generated field, not a free-floating hoop.
constexpr uint32_t kStrutCount        = 4;              // 4 arms (N/E/S/W of the ring)
constexpr float    kStrutHalfLen      = 0.85f;          // radial half-length
constexpr float    kStrutHalfThick    = 0.11f;          // cross-section half-thickness
constexpr float    kStrutBlueGrey[3]  = { 0.16f, 0.24f, 0.38f };  // dim structural
constexpr float    kStrutEmissive     = 0.6f;           // dim — it's housing, not energy
// Rim emitter nodes: bright blue cubes seated on the OUTER ring at even angles.
// They chase-pulse in sequence (a peak sweeps around the ring) so the generator
// looks like it's cycling energy into the field.
constexpr uint32_t kNodeCount         = 8;              // 8 emitter nodes
constexpr float    kNodeHalf          = 0.17f;          // node cube half-extent
constexpr float    kNodeBlue[3]       = { 0.25f, 0.62f, 1.00f };  // emitter blue
constexpr float    kNodeChaseHz       = 0.85f;          // chase peak revolutions/sec
constexpr float    kNodeMinEm         = 0.5f;           // node trough
constexpr float    kNodeMaxEm         = 7.5f;           // node peak (when the chase hits it)
constexpr float    kNodeChaseSharp    = 6.0f;           // higher = tighter/brighter peak

// ---- Event-horizon membrane (the visible portal SURFACE) -----------------------
// A vertical POOL of blue-white energy filling the ring opening — the portal's
// "liquid surface". Built ONCE in build() as kMembraneBands concentric bands of
// kMembraneSegs thin tangent box segments each (same tangent-segment technique
// as the frame ring, but flat thin sheets in the ring plane), butted radially so
// the opening reads as an unbroken membrane. Bands alternate between three
// slight depth planes along the outward axis for parallax + no z-fighting.
//
// tick() animates the pool as a standing pond hit by a stone: each band's
// emissive strength follows sin(m_time*w - bandRadius*k), so bright crests
// travel OUTWARD from the center to the rim (radially-propagating ripple), plus
// a slow low-amplitude per-segment swirl sin(theta - t*ws + r*twist) so the
// surface shimmers and churns instead of strobing. Colors are fixed at build:
// blue-white at the center fading to deep blue at the rim; ONLY the outermost
// band lerps a fraction toward the portal's destination tint (accent bleed) so
// the pool itself always reads Stargate-blue.
constexpr uint32_t kMembraneBands     = 5;               // concentric ripple bands
constexpr uint32_t kMembraneSegs      = 14;              // tangent segments per band
constexpr float    kMembraneInnerR    = 0.45f;           // innermost band center radius
constexpr float    kMembraneStepR     = 0.25f;           // radial spacing between bands
constexpr float    kMembraneHalfR     = 0.135f;          // band radial half-width (bands butt)
constexpr float    kMembraneHalfT     = 0.018f;          // band half-thickness (thin sheet)
constexpr float    kMembraneDepthStep = 0.030f;          // parallax depth-plane offset (outward)
// Pool gradient: near-white-blue center -> deep blue rim.
constexpr float    kPoolCenterBlue[3] = { 0.72f, 0.90f, 1.00f };
constexpr float    kPoolRimBlue[3]    = { 0.08f, 0.32f, 0.95f };
constexpr float    kPoolEdgeTintMix   = 0.22f;           // rim-band lerp toward destination tint
// Ripple drive: crests travel center -> rim (phase = t*w - r*k). Subtle + continuous.
constexpr float    kRippleFreqHz      = 1.15f;           // ripple oscillation rate (Hz)
constexpr float    kRippleK           = 9.5f;            // radial wavenumber (rad/m, ~0.66 m wavelength)
constexpr float    kSwirlHz           = 0.30f;           // slow angular swirl (rev/s)
constexpr float    kSwirlAmp          = 0.55f;           // swirl emissive contribution (subtle)
constexpr float    kSwirlTwist        = 2.2f;            // radial twist of the swirl arm (rad/m)
// Membrane emissive: base fades center -> rim; the ripple wave rides on top.
constexpr float    kPoolBaseEmCenter  = 5.5f;            // innermost band base strength
constexpr float    kPoolBaseEmRim     = 2.4f;            // outermost band base strength
constexpr float    kPoolRippleAmp     = 1.8f;            // travelling-crest amplitude
constexpr float    kPoolMinEm         = 0.35f;           // trough clamp (never fully dark)
// Membrane band center radius for band j (0 = innermost).
inline float membraneBandR(uint32_t band) {
    return kMembraneInnerR + (float)band * kMembraneStepR;
}
// Band segment half-tangent — half the chord between adjacent segment centers
// with a 6% overlap so the bands read as unbroken liquid, not a bead chain.
inline float membraneHalfTangent(float bandR) {
    const float pi = 3.14159265358979f;
    return bandR * std::sin(pi / (float)kMembraneSegs) * 1.06f;
}

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

// Build a column-major 4x4 model transform from three orthonormal basis vectors
// (the rendered box's local +X / +Y / +Z, each in WORLD space) plus a world-space
// translation. The mesh is authored centered at the local origin (makeBox with
// cx=cy=cz=0), so the transform fully relocates + reorients it. Mirrors the
// column layout used by valley.cpp's placeTilted().
inline void makeXform(float m[16],
                      const float xAxis[3], const float yAxis[3], const float zAxis[3],
                      float wx, float wy, float wz) {
    m[ 0] = xAxis[0]; m[ 1] = xAxis[1]; m[ 2] = xAxis[2]; m[ 3] = 0.0f;
    m[ 4] = yAxis[0]; m[ 5] = yAxis[1]; m[ 6] = yAxis[2]; m[ 7] = 0.0f;
    m[ 8] = zAxis[0]; m[ 9] = zAxis[1]; m[10] = zAxis[2]; m[11] = 0.0f;
    m[12] = wx;       m[13] = wy;       m[14] = wz;       m[15] = 1.0f;
}

// Add an origin-centered box with a per-entity orientation + translation written
// directly into the Entity transform. Returns BOTH the mesh handle (so shutdown
// can free it) and the entity id (so tick() can pulse its emissive[3]).
struct AddedEntity {
    x3::rhi::MeshHandle mesh;
    uint32_t            entityId;
};
AddedEntity addOrientedEmissiveBox(Scene& scene, x3::rhi::IRenderDevice& device,
                                   float hx, float hy, float hz,
                                   const float xAxis[3], const float yAxis[3], const float zAxis[3],
                                   float wx, float wy, float wz,
                                   const float tint[3], float emStrength) {
    // Origin-centered mesh; reorient + translate via Entity::transform.
    x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, 0.0f, 0.0f, 0.0f);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = 1.0f;
    e.emissive[0] = tint[0]; e.emissive[1] = tint[1]; e.emissive[2] = tint[2];
    e.emissive[3] = emStrength;
    e.tag = (uint32_t)Tag::Prop;
    makeXform(e.transform, xAxis, yAxis, zAxis, wx, wy, wz);
    uint32_t id = scene.add(e);
    return AddedEntity{ e.mesh, id };
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
    // Each portal authors: (2 layers * kRingSegments ring boxes) + kPlateSegments
    // floor wedges + kStrutCount emitter struts + kNodeCount rim nodes + 2 core
    // disks + (kMembraneBands * kMembraneSegs) event-horizon membrane segments
    // = 2*24 + 8 + 4 + 8 + 2 + 5*14 = 140 meshes.
    m_portalMeshes.reserve(kPortalCount *
        (2 * kRingSegments + kPlateSegments + kStrutCount + kNodeCount + 2 +
         kMembraneBands * kMembraneSegs));

    const float twoPi = 6.2831853f;
    for (uint32_t i = 0; i < kPortalCount; ++i) {
        const PortalSpec& sp = kPortalTable[i];
        // Hub angle for portal i — clockwise starting at +X.
        const float hubAng = (float)i * (twoPi / (float)kPortalCount);
        const float cx = std::cos(hubAng) * kRingRadius;
        const float cz = std::sin(hubAng) * kRingRadius;

        RiftPortal p;
        p.worldName  = sp.worldName;
        p.triggerId  = sp.triggerId;
        p.worldPos   = x3::phys::Vec3{ cx, 0.0f, cz };
        p.tint[0] = sp.tint[0]; p.tint[1] = sp.tint[1]; p.tint[2] = sp.tint[2];
        p.activated  = false;

        // ---- Portal-local basis ---------------------------------------------
        // The "outward" axis (radial from hub center to portal center) is the
        // ring's NORMAL — the ring's plane is perpendicular to it, so the
        // portal's doorway face points back toward the hub center. Up is world
        // +Y. "Right" (along the ring's plane, horizontal) is up x outward.
        const float invR = 1.0f / kRingRadius;
        const float outwardX = cx * invR;
        const float outwardZ = cz * invR;
        // Right = (0,1,0) x (outwardX, 0, outwardZ) = (-outwardZ, 0, outwardX). Unit.
        const float rightX = -outwardZ;
        const float rightZ =  outwardX;

        // ---- Ring segments (outer + inner concentric layers) -----------------
        // For each segment at ring angle θ (0..2π going CCW in the ring's local
        // plane starting from local +right = world (rightX,0,rightZ)):
        //   - center in ring plane = R*cos(θ)*right + R*sin(θ)*up
        //   - segment tangent      = -sin(θ)*right + cos(θ)*up
        //   - segment radial-out   =  cos(θ)*right + sin(θ)*up
        //   - outward stays the outward axis (depth through the ring)
        // The box's LOCAL axes map as: local +X = tangent (long axis = arc),
        // local +Y = radial-out (thickness), local +Z = outward (depth).
        const uint32_t ringEntFirst = scene.size();
        for (int layer = 0; layer < 2; ++layer) {
            const float ringR    = (layer == 0) ? kOuterRingR : kInnerRingR;
            const float halfTang = segHalfTangent(ringR);
            for (uint32_t s = 0; s < kRingSegments; ++s) {
                const float th  = (float)s * (twoPi / (float)kRingSegments);
                const float ct  = std::cos(th);
                const float st  = std::sin(th);

                // Segment center in WORLD (portal center + ring offsets).
                const float segCX = cx     + ringR * ct * rightX;            // right axis x-component
                const float segCY = kRingY + ringR * st;                     // up axis is world +Y
                const float segCZ = cz     + ringR * ct * rightZ;

                // Local +X (tangent), +Y (radial-out), +Z (outward).
                const float locX[3] = { -st * rightX, ct, -st * rightZ };
                const float locY[3] = {  ct * rightX, st,  ct * rightZ };
                const float locZ[3] = {  outwardX,   0.0f, outwardZ };

                AddedEntity ae = addOrientedEmissiveBox(
                    scene, device,
                    halfTang, kSegBoxThick, kSegBoxDepth,
                    locX, locY, locZ,
                    segCX, segCY, segCZ,
                    sp.tint, /*emStrength=*/3.0f);
                m_portalMeshes.push_back(ae.mesh);
            }
        }
        p.ringEntFirst = ringEntFirst;
        p.ringEntCount = 2 * kRingSegments;

        // ---- Octagonal floor plate (8 wedge boxes) ---------------------------
        // Each wedge is a thin box tangent to a circle of radius kPlateRingR,
        // axis-aligned in Y (flat on the ground). Reuses the same local basis
        // (right axis vs world Z) — for the floor plate the ring lives in the
        // WORLD XZ plane around the portal center, so the wedge tangent is the
        // tangent to the floor-ring circle.
        for (uint32_t s = 0; s < kPlateSegments; ++s) {
            const float th = (float)s * (twoPi / (float)kPlateSegments);
            const float ct = std::cos(th);
            const float st = std::sin(th);
            // Floor-ring local frame: local +right = (rightX,0,rightZ),
            //                         local +outward = (outwardX,0,outwardZ).
            // Wedge center: portal center + R*(ct*right + st*outward).
            const float wcx = cx + kPlateRingR * (ct * rightX + st * outwardX);
            const float wcy = kPlateHalfY;
            const float wcz = cz + kPlateRingR * (ct * rightZ + st * outwardZ);
            // Tangent (long axis, horizontal) = -st*right + ct*outward.
            const float tgX = -st * rightX + ct * outwardX;
            const float tgZ = -st * rightZ + ct * outwardZ;
            // Radial-out (in-plane on the floor) = ct*right + st*outward.
            const float rdX =  ct * rightX + st * outwardX;
            const float rdZ =  ct * rightZ + st * outwardZ;
            // Local +X = tangent (long arc), +Y = world up, +Z = radial-out.
            const float locX[3] = { tgX, 0.0f, tgZ };
            const float locY[3] = { 0.0f, 1.0f, 0.0f };
            const float locZ[3] = { rdX, 0.0f, rdZ };
            AddedEntity ae = addOrientedEmissiveBox(
                scene, device,
                plateHalfTangent(), kPlateHalfY, kPlateBoxThick,
                locX, locY, locZ,
                wcx, wcy, wcz,
                sp.tint, /*emStrength=*/1.5f);
            m_portalMeshes.push_back(ae.mesh);
        }

        // ---- Wormhole-generator emitter struts (4 radial arms) ---------------
        // Dim structural blue-grey boxes reaching outward from just past the
        // outer ring at the 4 cardinal ring angles (θ = 0, 90, 180, 270°). Each
        // strut's long axis is the in-ring-plane radial direction at its angle.
        for (uint32_t a = 0; a < kStrutCount; ++a) {
            const float th = (float)a * (twoPi / (float)kStrutCount);
            const float ct = std::cos(th);
            const float st = std::sin(th);
            // In-ring-plane radial-out at θ = ct*right + st*up (unit; right ⊥ up).
            const float radX = ct * rightX;             // right x-component
            const float radY = st;                      // up is world +Y
            const float radZ = ct * rightZ;
            // In-ring-plane tangent = -st*right + ct*up.
            const float tanX = -st * rightX;
            const float tanY =  ct;
            const float tanZ = -st * rightZ;
            // Strut center: just beyond the outer ring along the radial.
            const float sr  = kOuterRingR + kStrutHalfLen;
            const float scx = cx     + sr * radX;
            const float scy = kRingY + sr * radY;
            const float scz = cz     + sr * radZ;
            // Local +X = radial (long axis), +Y = tangent (thin), +Z = outward (thin).
            const float locX[3] = { radX, radY, radZ };
            const float locY[3] = { tanX, tanY, tanZ };
            const float locZ[3] = { outwardX, 0.0f, outwardZ };
            AddedEntity ae = addOrientedEmissiveBox(
                scene, device,
                kStrutHalfLen, kStrutHalfThick, kStrutHalfThick,
                locX, locY, locZ,
                scx, scy, scz,
                kStrutBlueGrey, kStrutEmissive);
            m_portalMeshes.push_back(ae.mesh);
        }

        // ---- Rim emitter nodes (chase-pulsing blue cubes) --------------------
        // Bright blue cubes seated on the OUTER ring at even angles. Authored
        // contiguously so tick() can chase-pulse them in sequence. The chase
        // makes the generator look like it's cycling energy into the field.
        const uint32_t nodeEntFirst = scene.size();
        for (uint32_t n = 0; n < kNodeCount; ++n) {
            const float th = (float)n * (twoPi / (float)kNodeCount);
            const float ct = std::cos(th);
            const float st = std::sin(th);
            // Node center: on the outer ring at angle θ.
            const float ncx = cx     + kOuterRingR * ct * rightX;
            const float ncy = kRingY + kOuterRingR * st;
            const float ncz = cz     + kOuterRingR * ct * rightZ;
            // Axis-aligned-ish cube (orientation cosmetic for a small cube).
            const float locX[3] = { rightX, 0.0f, rightZ };
            const float locY[3] = { 0.0f, 1.0f, 0.0f };
            const float locZ[3] = { outwardX, 0.0f, outwardZ };
            AddedEntity ae = addOrientedEmissiveBox(
                scene, device,
                kNodeHalf, kNodeHalf, kNodeHalf,
                locX, locY, locZ,
                ncx, ncy, ncz,
                kNodeBlue, /*emStrength=*/kNodeMinEm);
            m_portalMeshes.push_back(ae.mesh);
        }
        p.nodeEntFirst = nodeEntFirst;
        p.nodeEntCount = kNodeCount;

        // ---- Pool center: electric-blue disk + near-white-blue hot spot ------
        // The brightest point of the event-horizon pool — a small vertical blue
        // slab at the exact ring center facing back toward the hub, with an even
        // smaller near-white-blue disk in front of it for depth. Both pulse blue
        // (NOT the destination tint); the membrane bands below fill the rest of
        // the ring opening around them.
        const float coreLocX[3] = { rightX, 0.0f, rightZ };          // along the ring's right axis
        const float coreLocY[3] = { 0.0f,    1.0f, 0.0f    };        // world up
        const float coreLocZ[3] = { outwardX, 0.0f, outwardZ };      // thin axis = outward
        AddedEntity core = addOrientedEmissiveBox(
            scene, device,
            kCoreHalfW, kCoreHalfH, kCoreHalfT,
            coreLocX, coreLocY, coreLocZ,
            cx, kRingY, cz,
            kCoreBlue, /*emStrength=*/kCoreBlueMinEm);
        m_portalMeshes.push_back(core.mesh);
        p.coreEnt = core.entityId;

        // Inner brighter disk, nudged a hair toward the hub (along -outward) so
        // it sits just in front of the main core and doesn't z-fight.
        AddedEntity coreInner = addOrientedEmissiveBox(
            scene, device,
            kCoreInnerHalfW, kCoreInnerHalfH, kCoreInnerHalfT,
            coreLocX, coreLocY, coreLocZ,
            cx - outwardX * 0.03f, kRingY, cz - outwardZ * 0.03f,
            kCoreInnerBlue, /*emStrength=*/kCoreBlueMaxEm);
        m_portalMeshes.push_back(coreInner.mesh);
        p.coreInnerEnt = coreInner.entityId;

        // ---- Event-horizon membrane (the portal SURFACE) ----------------------
        // Concentric bands of thin tangent segments filling the ring opening — a
        // vertical, unbroken pool of blue-white energy standing on its edge. Same
        // tangent-segment construction as the frame ring, but each segment is a
        // flat thin sheet in the ring plane (local +X = tangent, +Y = radial,
        // +Z = outward/thin) so the bands butt radially into a solid membrane.
        // Authored band 0 (innermost, brightest, blue-white) outward to band
        // N-1 (rim, deep blue + a bleed of the destination tint), contiguous so
        // tick() can phase each band's emissive by its radius for the ripple.
        // Bands cycle through three slight outward depth planes for parallax.
        const uint32_t membraneEntFirst = scene.size();
        for (uint32_t b = 0; b < kMembraneBands; ++b) {
            const float bandR    = membraneBandR(b);
            const float halfTang = membraneHalfTangent(bandR);
            // Color gradient center -> rim; only the RIM band bleeds the tint.
            const float g = (kMembraneBands > 1)
                          ? (float)b / (float)(kMembraneBands - 1) : 0.0f;
            float bandCol[3];
            for (int c = 0; c < 3; ++c)
                bandCol[c] = kPoolCenterBlue[c] + (kPoolRimBlue[c] - kPoolCenterBlue[c]) * g;
            if (b == kMembraneBands - 1)
                for (int c = 0; c < 3; ++c)
                    bandCol[c] += (sp.tint[c] - bandCol[c]) * kPoolEdgeTintMix;
            // Depth plane: -1 / 0 / +1 steps along outward (parallax, no z-fight).
            const float depth = ((float)(b % 3) - 1.0f) * kMembraneDepthStep;
            const float bcx = cx + outwardX * depth;
            const float bcz = cz + outwardZ * depth;
            const float baseEm = kPoolBaseEmCenter +
                                 (kPoolBaseEmRim - kPoolBaseEmCenter) * g;
            for (uint32_t s = 0; s < kMembraneSegs; ++s) {
                const float th = (float)s * (twoPi / (float)kMembraneSegs);
                const float ct = std::cos(th);
                const float st = std::sin(th);
                // Segment center in the ring plane (right/up), off the depth plane.
                const float mcx = bcx    + bandR * ct * rightX;
                const float mcy = kRingY + bandR * st;
                const float mcz = bcz    + bandR * ct * rightZ;
                // Local +X = tangent, +Y = radial-out (band width), +Z = outward.
                const float locX[3] = { -st * rightX, ct, -st * rightZ };
                const float locY[3] = {  ct * rightX, st,  ct * rightZ };
                const float locZ[3] = {  outwardX,   0.0f, outwardZ };
                AddedEntity ae = addOrientedEmissiveBox(
                    scene, device,
                    halfTang, kMembraneHalfR, kMembraneHalfT,
                    locX, locY, locZ,
                    mcx, mcy, mcz,
                    bandCol, baseEm);
                m_portalMeshes.push_back(ae.mesh);
            }
        }
        p.membraneEntFirst = membraneEntFirst;
        p.membraneEntCount = kMembraneBands * kMembraneSegs;

        m_portals.push_back(p);

        // Trigger volume: wider than the ring so the player only needs to
        // step into the plate area (not thread the ring) to fire the rift.
        const x3::phys::Vec3 tmin{ cx - kTrigHalfXZ, -kTrigHalfY, cz - kTrigHalfXZ };
        const x3::phys::Vec3 tmax{ cx + kTrigHalfXZ,  kTrigHalfY, cz + kTrigHalfXZ };
        triggers.add(tmin, tmax, sp.triggerId, /*enabled=*/true);
    }

    physics.optimizeBroadphase();
    m_built = true;
    x3::logInfo("[rifthub] hub built with " + std::to_string(m_portals.size()) +
                " portals (round tech-gate rings, " + std::to_string(kRingSegments) +
                " segments/ring x 2 layers + octagonal plate + event-horizon membrane, " +
                std::to_string(kMembraneBands) + " bands x " +
                std::to_string(kMembraneSegs) + " segments)");
}

void Rifthub::tick(float dt, Scene& scene) {
    if (!m_built) return;
    m_time += dt;

    const float twoPi = 6.2831853f;
    const float ringOmega = twoPi * kShimmerFreqHz;   // destination-tint frame pulse
    const float coreOmega = twoPi * kCoreFreqHz;       // faster blue energy pulse
    const float chaseOmega = twoPi * kNodeChaseHz;     // rim-node chase rotation
    const float ripOmega   = twoPi * kRippleFreqHz;    // membrane ripple oscillation
    const float swirlOmega = twoPi * kSwirlHz;         // membrane slow swirl rotation
    auto& ents = scene.entities();
    const uint32_t sceneN = (uint32_t)ents.size();

    for (uint32_t i = 0; i < m_portals.size(); ++i) {
        const RiftPortal& p = m_portals[i];
        const float phase = (float)i * kShimmerPhaseStep;

        // --- Frame ring: destination-tint pulse (gates ripple around the hub) ---
        const float ringS   = std::sin(m_time * ringOmega + phase);
        const float ringT01 = 0.5f * (ringS + 1.0f);
        const float ringEm  = kRingMinEmissive + (kRingMaxEmissive - kRingMinEmissive) * ringT01;
        const uint32_t end = p.ringEntFirst + p.ringEntCount;
        for (uint32_t e = p.ringEntFirst; e < end && e < sceneN; ++e) {
            ents[e].emissive[3] = ringEm;
        }

        // --- Energy core: faster electric-blue pulse (core + brighter inner) ---
        const float coreS   = std::sin(m_time * coreOmega + phase);
        const float coreT01 = 0.5f * (coreS + 1.0f);
        const float coreEm  = kCoreBlueMinEm + (kCoreBlueMaxEm - kCoreBlueMinEm) * coreT01;
        if (p.coreEnt < sceneN)      ents[p.coreEnt].emissive[3]      = coreEm;
        // Inner disk runs a touch brighter + counter-phased so the core "breathes".
        if (p.coreInnerEnt < sceneN) ents[p.coreInnerEnt].emissive[3] = coreEm * 1.15f + 1.0f;

        // --- Rim emitter nodes: a chase peak sweeps around the ring ---------
        // node n's phase lags by n*(2pi/N); a sharpened cosine makes one bright
        // peak travel the ring, like the generator cycling energy into the field.
        const uint32_t nend = p.nodeEntFirst + p.nodeEntCount;
        for (uint32_t n = 0; p.nodeEntFirst + n < nend && p.nodeEntFirst + n < sceneN; ++n) {
            const float nodePhase = m_time * chaseOmega - (float)n * (twoPi / (float)kNodeCount);
            const float c01  = 0.5f * (std::cos(nodePhase) + 1.0f);   // [0,1]
            const float peak = std::pow(c01, kNodeChaseSharp);        // sharpen to a tight peak
            ents[p.nodeEntFirst + n].emissive[3] = kNodeMinEm + (kNodeMaxEm - kNodeMinEm) * peak;
        }

        // --- Event-horizon membrane: outward-travelling liquid ripple --------
        // Band b (center radius r) rides sin(t*w - r*k): as t advances the
        // crest's radius grows, so bright rings expand from the center to the
        // rim like a stone dropped in a standing pond. A slow low-amplitude
        // swirl term sin(theta - t*ws + r*twist) rotates a soft bright arm
        // through the pool so the surface churns instead of strobing. Base
        // strength fades center -> rim (the middle stays the brightest point).
        for (uint32_t m = 0; m < p.membraneEntCount; ++m) {
            const uint32_t e = p.membraneEntFirst + m;
            if (e >= sceneN) break;
            const uint32_t band = m / kMembraneSegs;
            const uint32_t seg  = m % kMembraneSegs;
            const float r  = membraneBandR(band);
            const float th = (float)seg * (twoPi / (float)kMembraneSegs);
            const float g  = (kMembraneBands > 1)
                           ? (float)band / (float)(kMembraneBands - 1) : 0.0f;
            const float base   = kPoolBaseEmCenter +
                                 (kPoolBaseEmRim - kPoolBaseEmCenter) * g;
            const float ripple = std::sin(m_time * ripOmega - r * kRippleK + phase);
            const float swirl  = std::sin(th - m_time * swirlOmega + r * kSwirlTwist);
            float em = base + kPoolRippleAmp * ripple + kSwirlAmp * swirl;
            if (em < kPoolMinEm) em = kPoolMinEm;
            ents[e].emissive[3] = em;
        }
    }
}

void Rifthub::shutdown(x3::rhi::IRenderDevice& device) {
    if (!m_built) return;
    if (m_groundMesh.valid()) device.destroyMesh(m_groundMesh);
    if (m_groundTex.valid())  device.destroyTexture(m_groundTex);
    for (auto h : m_portalMeshes) if (h.valid()) device.destroyMesh(h);
    m_portalMeshes.clear();
    m_portals.clear();
    m_time = 0.0f;
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

// ===========================================================================
// Headless self-test (--test-rifthub). Mirrors --test-act2caves' structure:
// build on a HeadlessDevice + Jolt world, drive triggers/tick, assert.
// ===========================================================================
namespace {

int rh_pass = 0, rh_fail = 0;
void rhCheck(bool cond, const char* name) {
    if (cond) { ++rh_pass; x3::logInfo(std::string("[rifthub-test] PASS ") + name); }
    else      { ++rh_fail; x3::logError(std::string("[rifthub-test] FAIL ") + name); }
}

// The --world targets the current host actually launches into (main.cpp's
// worldMode dispatch). Every portal worldName must be one of these so a portal
// always signposts a slice the player can really relaunch into.
bool isKnownWorldTarget(const char* w) {
    if (!w) return false;
    const std::string s(w);
    return s == "act2caves" || s == "act2" || s == "valley" || s == "cliffs" ||
           s == "club" || s == "destruct" || s == "ragdoll" || s == "terrain";
}

} // namespace

bool runRifthubSelfTest() {
    rh_pass = rh_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    phys->init();
    HeadlessRenderDevice device;
    Scene scene;
    TriggerSystem triggers;
    Rifthub hub;
    hub.build(scene, device, *phys, triggers);

    // T0 — built with exactly 8 portals (one per --world target).
    rhCheck(hub.built() && hub.portalCount() == kRifthubTrigCount &&
            hub.portalCount() == 8,
            "T0 hub built with 8 portals (one per --world target)");

    // T1 — each portal owns a contiguous span of ring + node + event-horizon
    //      membrane entities + 2 core disks, and the spans index valid scene
    //      entities.
    {
        const uint32_t sceneN = scene.size();
        bool ok = sceneN > 0;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const RiftPortal& p = hub.portal(i);
            if (p.ringEntCount == 0 || p.nodeEntCount == 0) ok = false;
            if (p.membraneEntCount == 0)                    ok = false;
            if (p.ringEntFirst + p.ringEntCount > sceneN)   ok = false;
            if (p.coreEnt >= sceneN || p.coreInnerEnt >= sceneN) ok = false;
            if (p.nodeEntFirst + p.nodeEntCount > sceneN)   ok = false;
            if (p.membraneEntFirst + p.membraneEntCount > sceneN) ok = false;
        }
        rhCheck(ok, "T1 every portal owns valid ring/node/core/membrane entity spans");
    }

    // T2 — all 8 portal names map to REAL --world targets the host launches.
    {
        bool ok = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i)
            if (!isKnownWorldTarget(hub.portal(i).worldName)) ok = false;
        rhCheck(ok, "T2 all 8 portal names are real --world targets");
    }

    // T3 — at load NOTHING is activated; allActivated() is false; the HUD prompt
    //      reads "walk in to activate" for a portal we stand next to.
    {
        bool noneActive = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i)
            if (hub.portal(i).activated) noneActive = false;
        std::string prompt;
        const RiftPortal& p0 = hub.portal(0);
        bool hud = hub.hudPromptForEye(p0.worldPos, prompt) &&
                   prompt.find("walk in") != std::string::npos;
        rhCheck(noneActive && !hub.allActivated() && hud,
                "T3 inert at load: no portal activated, HUD prompts 'walk in'");
    }

    // T4 — entering each portal's trigger volume (a point inside it, via the
    //      shared TriggerSystem) latches THAT portal's `activated` flag + flips
    //      its HUD prompt to "Rift activated:"; only after ALL are entered does
    //      allActivated() become true.
    {
        bool perPortalOk = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const RiftPortal& p = hub.portal(i);
            // Stand at the portal center (inside its 5 m square trigger volume).
            const x3::phys::Vec3 inside{ p.worldPos.x, 0.0f, p.worldPos.z };
            const auto fired = triggers.update(inside);
            bool sawId = false;
            for (uint32_t id : fired) { hub.onTrigger(id); if (id == p.triggerId) sawId = true; }
            std::string prompt;
            hub.hudPromptForEye(inside, prompt);
            if (!sawId || !hub.portal(i).activated ||
                prompt.find("Rift activated:") == std::string::npos)
                perPortalOk = false;
            // allActivated() must stay false until the very last portal.
            if (i + 1 < hub.portalCount() && hub.allActivated()) perPortalOk = false;
            // Move away so the next update() re-arms an enter edge for this id.
            triggers.update(x3::phys::Vec3{ 1000.0f, 1000.0f, 1000.0f });
        }
        rhCheck(perPortalOk && hub.allActivated(),
                "T4 each trigger latches its portal; allActivated() only after all 8");
    }

    // T5 — tick(dt) advances the shimmer: ring-segment + core + membrane emissive
    //      values DIFFER between two ticks taken at different times (the pulse +
    //      ripple are live), AND two membrane bands at different radii sit at
    //      different emissive levels at the same instant (the ripple really is
    //      a radial wave, not a uniform strobe).
    {
        const RiftPortal& p = hub.portal(0);
        hub.tick(0.0f, scene);   // seed m_time at a known phase
        const float ring0 = scene.entities()[p.ringEntFirst].emissive[3];
        const float core0 = scene.entities()[p.coreEnt].emissive[3];
        const float mem0  = scene.entities()[p.membraneEntFirst].emissive[3];
        // Advance ~a tenth of a second — enough for sin(1.15..3.2 Hz * 2pi * dt)
        // to move appreciably.
        hub.tick(0.1f, scene);
        const float ring1 = scene.entities()[p.ringEntFirst].emissive[3];
        const float core1 = scene.entities()[p.coreEnt].emissive[3];
        const float mem1  = scene.entities()[p.membraneEntFirst].emissive[3];
        bool moved = std::fabs(ring1 - ring0) > 1e-3f &&
                     std::fabs(core1 - core0) > 1e-3f &&
                     std::fabs(mem1  - mem0)  > 1e-3f;
        // Ring pulse stays within its declared [min,max] band.
        bool bounded = ring1 >= kRingMinEmissive - 0.01f && ring1 <= kRingMaxEmissive + 0.01f;
        // Radial ripple: at the SAME instant, band 0 (segment 0) and band 2
        // (segment 0) — 2*kMembraneStepR apart in radius — must be phased apart
        // by their sin(t*w - r*k) terms, so their emissive levels differ.
        const float bandA = scene.entities()[p.membraneEntFirst].emissive[3];
        const float bandB = scene.entities()[p.membraneEntFirst + 2 * kMembraneSegs].emissive[3];
        bool radial = std::fabs(bandA - bandB) > 1e-3f;
        rhCheck(moved && bounded && radial,
                "T5 tick() advances ring + core + membrane ripple (live, bounded, radial)");
    }

    hub.shutdown(device);
    phys->shutdown();

    x3::logInfo("rifthub: " + std::to_string(rh_pass) + "/" +
                std::to_string(rh_pass + rh_fail) + " passed");
    return rh_fail == 0;
}

} // namespace x3::game
