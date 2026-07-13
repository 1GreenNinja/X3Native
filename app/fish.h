#pragma once
// FISH — the river/sea LIVES (living-world, W10 water pass). Game/slice code
// only; engine/ stays pure.
//
// Ambient SCHOOLS of fish in the world water (THE RIVER's reach past the
// facility + the sea shallows at the estuary). Same SIMULACRA stance as the
// ambient ecology (app/ecology.h) and the crowds (app/crowd.h): no physics
// bodies, no per-agent model loads — but the fish must LOOK like fish.
//
// ===========================================================================
// ART PASS 3 — REAL FISH (Rodin pike / rudd / bream / perch)
// ===========================================================================
// The fish are now REAL SCANNED SPECIES, not lofted approximations. Four Rodin
// models (90k-500k tris, 2-4K PBR maps) are decimated, retextured, RIGGED with a
// 4-bone spine and POSE-BAKED by tools/fish_bake.py into assets/rigged_glb/
// Fish_<Species>.glb. See docs/design/WORLDS.md.
//
// HOW A GLB FISH SWIMS — POSE-MESH SWAPPING, not runtime skinning.
//   The engine has NO instanced skinned draw: the RHI keys the joint palette by
//   MeshHandle (setSkinnedPalette(MeshHandle, ...) — ONE palette per mesh), and
//   the only skinned path is one MonsterSystem per instance (~15-25 ms of spawn
//   each, and its setPropPose(pos, yaw) has NO ROLL — it cannot express a fish
//   banking into a turn, let alone floating belly-up). Sixty-one skinned fish is
//   not on the table.
//   So the skinning is done OFFLINE: fish_bake.py evaluates the rig at N phases of
//   the swim loop and freezes the DEFORMED mesh at each one. Each species GLB
//   carries kFishPoses meshes:
//        [0, kFishCruise)                      — cruise, one full tail beat
//        [kFishCruise, kFishCruise + kFishFast) — the flee burst (bigger sweep)
//        kFishDeadPose                          — a slack, near-straight body
//   At runtime a fish just picks the pose for its (time * beat + phase) and writes
//   the MeshHandle onto its Entity. Real skinned-quality deformation — the fins
//   bend WITH the body, and nothing shears the way a rigid hinge-chain would
//   scissor a dorsal fin that spans the joint — for ZERO per-frame vertex work.
//   ONE entity + ONE draw per fish (the procedural loft needed three).
//   The pose index is carried in the GLB node's translation X (ModelDrawable has
//   no name field), which fish.cpp rounds to an int and then ignores for rendering.
//
// FULL PBR: Entity carries tex + normalTex + mrTex, so Scene::render routes the
// fish through drawMeshPBR — the scale/stripe/wet-sheen detail all lives in the
// NORMAL MAP (the geometry is only ~1.4-4k tris). MeshVertex has no tangents;
// mesh.frag reconstructs the TBN from screen-space derivatives, so the normal map
// binds with no vertex-format change.
//
// FALLBACK (never break the world): a species whose GLB is missing or fails to
// load degrades to the PROCEDURAL LOFTED body below — the original art pass, kept
// verbatim. A fish is never a statue and never a T-pose; worst case it is the
// lofted fish that shipped before. A box with no fish GLBs at all still gets the
// full living river.
//
// THE PROCEDURAL FALLBACK MESH (art pass 2 — "how hard is it to make a fish? LOL"):
//   * A LOFTED body: ellipse rings swept along the spine, the ring radius
//     following a smooth profile (pointed snout -> deepest at ~30% back ->
//     pinched tail root). Laterally compressed (width ~0.45 of height), like a
//     real fish. A forked CAUDAL fin, a dorsal blade and a pectoral pair.
//   * COUNTERSHADING baked into the UVs: v = up-ness of the ring vertex, sampled
//     from ONE tiny gradient texture (dark olive-steel back -> pale silver belly).
//     That is what makes it read as a fish instead of a white brick — and it is
//     what makes a BELLY-UP corpse read: rolled 180 deg, the pale side faces the
//     sky. Per-school tint + per-fish jitter multiply the gradient.
//   * THE SWIM: the body is THREE hinged pieces (head / mid / tail+caudal) that
//     share the fish transform. A travelling sine runs down the spine — the mid
//     sweeps a little, the tail sweeps more, one phase behind it — so the body
//     S-FLEXES and the tail beats. The fish also BANKS into its turns (roll from
//     the measured yaw rate). A fish that translates without flexing reads as a
//     floating prop.
//   * 3 shared meshes + 1 shared texture for the whole shoal; 3 Scene entities
//     per fish, transforms rewritten each frame. Zero per-frame allocation.
//
// THE SIM:
//   * SCHOOLING: each school has a drifting CENTER that probes the water query
//     ahead and turns when it would beach itself (a river school follows the
//     channel; a sea school mills in the shallows). Fish steer to their slot
//     (cohesion), push apart when crowded (separation) and share the school
//     heading (alignment) — boids-lite, O(n^2) inside an 8-13 fish school.
//   * DEPTH-BOUND between the bed (+ depthMin) and just under the surface.
//   * FLEE: the player inside fleeRadius scatters them; they re-form after.
//   * DEAD FISH (the water zap, app/waterzap.h): killWithin() rolls every fish in
//     the radius BELLY-UP; the corpse floats up INTO the surface plane (pale side
//     to the sky), drifts with the current, lolls, and despawns after deadLinger.
//   * DETERMINISTIC (one LCG); range-gated per school; the host stamps
//     cfg.roomId = kStreamedExteriorRoom so the outdoor PVS gates the draw.

#include "scene.h"

#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// ---- The pose-mesh layout baked by tools/fish_bake.py ----------------------
constexpr uint32_t kFishCruise   = 16;                      // cruise poses (one beat)
constexpr uint32_t kFishFast     = 12;                      // flee-burst poses (one beat)
constexpr uint32_t kFishPoses    = kFishCruise + kFishFast + 1;
constexpr uint32_t kFishDeadPose = kFishCruise + kFishFast; // the slack corpse body

// ---- THE SPECIES ----------------------------------------------------------
// A predator does not shoal. That is the whole point of the pike.
enum class FishSpecies : uint32_t {
    Rudd  = 0,   // schooling workhorse — silver flank, blood-red fins
    Bream = 1,   // schooling — deep silver-blue slab
    Perch = 2,   // small loose GANGS (3-6) — tiger stripes, spiny dorsal
    Pike  = 3,   // THE LONER — apex of the reach, big, slow, solitary
    Count = 4,
};

// One species' art + behaviour. The mesh is authored UNIT-LENGTH by fish_bake.py,
// so `size` is literally the fish's length in metres.
struct FishSpeciesDesc {
    const char* name;
    const char* glb;         // in riggedGlbRoot(); empty => always procedural
    float size;              // body length (m)
    float sizeJitter;        // +/- fraction of size, per fish
    float beatHz;            // tail beats/sec at cruise
    float speed;             // cruise speed (m/s)
    bool  solitary;          // TRUE => a loner: never shoals, holds station
};

// The table (defined in fish.cpp). Index with (uint32_t)FishSpecies.
const FishSpeciesDesc& fishSpecies(FishSpecies s);

// The water-surface feed: returns the water surface Y over world (x,z), or a
// very-negative sentinel (< kFishDryTest) when the point is DRY. The canon host
// passes x3::game::worldWaterLevelAt (app/terrain.h); the self-test injects a
// synthetic slab. Fish never include terrain.h.
using FishWaterFn = std::function<float(float x, float z)>;
// The BED feed: the ground/terrain height under (x,z) — the floor a fish will
// not swim through. Optional: unset => the bed is treated as (surface - 30).
using FishBedFn = std::function<float(float x, float z)>;
// Anything below this is the "no water here" sentinel (matches kWorldWaterDry).
constexpr float kFishDryTest = -1.0e30f;

// One authored school.
struct FishSchoolDesc {
    float    centerX = 0.0f;      // seed center (world XZ; must be over water)
    float    centerZ = 0.0f;
    uint32_t count   = 10;        // fish in this school
    float    spread  = 5.0f;      // school radius (m) — the slot ring
    float    heading = 0.0f;      // initial swim heading (radians, world XZ)
    float    speed   = 1.0f;      // cruise speed of the school center (m/s)
    // WHICH FISH. A solitary species (Pike) ignores `count` beyond 1-2 and holds
    // station instead of shoaling — see FishSystem::update.
    FishSpecies species = FishSpecies::Rudd;
    // Tint. On a GLB fish this MULTIPLIES the real albedo, so it must stay near
    // white (a per-fish jitter only) or it stains the scanned colour. On the
    // procedural fallback it multiplies the countershading gradient, which is what
    // gave the old schools their olive / steel / bronze read.
    float    tint[3] = { 1.0f, 1.0f, 1.0f };
};

// Whole-shoal config + behavior tunables.
struct FishConfig {
    std::vector<FishSchoolDesc> schools;
    uint32_t roomId       = kNoRoom;   // PVS tag stamped on every fish entity
    float activeRadius    = 140.0f;    // school simulated/drawn within this of the player (m)
    float fleeRadius      = 2.5f;      // player closer than this to a fish -> it bolts
    float fleeSpeed       = 4.0f;      // flee burst speed (m/s)
    float fleeTime        = 1.8f;      // seconds a flee burst lasts
    float separation      = 0.85f;     // closer than this to a schoolmate -> push apart (m)
    float turnRate        = 3.2f;      // yaw slew (rad/s)
    float depthMin        = 0.45f;     // never closer than this to the bed (m)
    float depthBelowSurf  = 0.55f;     // never closer than this to the surface (m)
    // THE SWIM: a travelling sine down the spine. The mid piece sweeps midAmp,
    // the tail piece tailAmp one `lag` behind it — that phase offset IS the
    // S-flex (equal phases would just wag the whole fish rigidly).
    float beatHz          = 2.6f;      // tail beats per second (x1.9 while fleeing)
    float midAmp          = 0.16f;     // mid-body sweep (rad)
    float tailAmp         = 0.42f;     // tail sweep (rad)
    float beatLag         = 1.05f;     // tail phase lag behind the mid (rad)
    float bankPerTurn     = 0.30f;     // roll per rad/s of yaw rate (banking)
    float bankMax         = 0.55f;     // roll clamp (rad)
    float size            = 0.72f;     // base visual scale (~0.6-0.9 m river fish)
    float deadRise        = 0.55f;     // belly-up rise to the surface (m/s)
    float deadDrift       = 0.40f;     // corpse drift with the current (m/s)
    float deadLinger      = 26.0f;     // seconds a corpse floats before it despawns
    uint32_t seed         = 0xF15Fu;   // LCG seed (determinism)
};

// One fish.
//   GLB fish  : ONE entity, whose MeshHandle is swapped to the current swim pose.
//   FALLBACK  : THREE entities (head / mid / tail) hinged into one transform chain.
struct Fish {
    // entHead is THE entity for a GLB fish; entMid/entTail stay kNoLink.
    uint32_t entHead = kNoLink;
    uint32_t entMid  = kNoLink;
    uint32_t entTail = kNoLink;
    uint32_t school  = 0;
    FishSpecies species = FishSpecies::Rudd;
    bool  glb   = false;     // false => this fish is the procedural loft
    float x = 0, y = 0, z = 0;
    float yaw   = 0.0f;
    float roll  = 0.0f;      // bank into turns; PI when DEAD (belly-up)
    float midW  = 0.0f;      // mid-piece hinge angle (rad) — the S-flex (fallback)
    float tailW = 0.0f;      // tail-piece hinge angle (rad)          (fallback)
    float size  = 1.0f;      // per-fish size multiplier (on the species size)
    float phase = 0.0f;      // beat phase offset
    float speed = 1.0f;      // per-fish cruise speed
    float fleeT = 0.0f;      // > 0 while bolting from the player
    // Slot in the school (local, rotated by the school heading): the boids
    // cohesion target. slotD = the fish's preferred depth offset below the top
    // of its depth band, so a school reads as a 3D shoal not a flat sheet.
    float slotX = 0.0f, slotZ = 0.0f, slotD = 0.0f;
    bool  dead  = false;     // ZAPPED: belly-up, floating, drifting
    float deadT = 0.0f;      // seconds since death (despawns at deadLinger)
    bool  gone  = false;     // corpse despawned (entities hidden for good)
};

// One school (the drifting center the fish orbit).
struct FishSchool {
    float cx = 0, cz = 0;    // center (world XZ)
    float heading = 0.0f;    // swim direction (radians)
    float speed = 1.0f;      // center drift speed (m/s)
    bool  active = false;    // player within activeRadius
    FishSpecies species = FishSpecies::Rudd;
    bool  solitary = false;  // a loner (pike): holds station, does not shoal
};

class FishSystem {
public:
    // Where the species GLBs live (riggedGlbRoot()). Empty => procedural only,
    // which is exactly what the headless self-tests want.
    void setModelDir(std::string dir) { m_modelDir = std::move(dir); }
    // Wire the water/bed feeds BEFORE build() (build seeds fish at real depths).
    void setWaterQuery(FishWaterFn fn) { m_water = std::move(fn); }
    void setBedQuery(FishBedFn fn)     { m_bed   = std::move(fn); }

    // Build every school (3 shared lofted meshes + 1 shared countershading
    // gradient; 3 entities per fish). Fish are seeded on their slots around the
    // school center at real depths. Schools whose center is DRY are skipped
    // (logged) — a school never spawns on land.
    void build(const FishConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device);

    // Advance one frame: school drift (water-probed), boids steering, flee bursts,
    // depth clamp, the travelling-sine swim + bank, corpse float/loll/despawn.
    void update(float dt, Scene& scene, const x3::phys::Vec3& playerPos);

    // THE ZAP (app/waterzap.h): kill every LIVE fish whose XZ distance to
    // (cx,cz) is <= radius — they roll belly-up and float. Returns the number
    // killed. Fish outside the radius are untouched.
    uint32_t killWithin(float cx, float cz, float radius);

    // ---- Queries (HUD / self-test) ----
    bool built() const { return m_built; }
    const FishConfig& config() const { return m_cfg; }
    uint32_t fishCount() const { return (uint32_t)m_fish.size(); }
    const Fish& fish(uint32_t i) const { return m_fish[i]; }
    uint32_t schoolCount() const { return (uint32_t)m_schools.size(); }
    const FishSchool& school(uint32_t i) const { return m_schools[i]; }
    uint32_t aliveCount() const;
    uint32_t deadCount() const;    // dead but not yet despawned
    uint32_t activeCount() const;  // fish in an active (in-range) school
    // Art queries (HUD / self-test / the perf log).
    bool     speciesLoaded(FishSpecies s) const;   // real GLB art bound?
    uint32_t glbFishCount() const;                 // fish rendering a real model
    uint32_t triCount() const;                     // tris drawn by ALL live fish
    uint32_t drawCount() const;                    // entities drawn by ALL live fish

private:
    // One species' loaded art: kFishPoses frozen swim meshes + the shared PBR maps.
    struct SpeciesArt {
        bool ok = false;
        std::vector<x3::rhi::MeshHandle> poses;   // kFishPoses, in bake order
        x3::rhi::TextureHandle albedo{}, normal{}, mr{};
        uint32_t tris = 0;                        // tris in ONE pose
    };

    void  loadSpecies(FishSpecies s, x3::rhi::IRenderDevice& device);
    void  buildProceduralArt(x3::rhi::IRenderDevice& device);
    // The pose for this fish RIGHT NOW (cruise ring, flee ring, or the dead slack).
    uint32_t poseIndex(const Fish& f) const;
    void  writeTransform(Fish& f, Scene& scene);
    void  setVisible(Fish& f, Scene& scene, bool vis);
    float waterAt(float x, float z) const;
    float bedAt(float x, float z, float surface) const;
    uint32_t rng();
    float frand();   // 0..1

    bool        m_built = false;
    FishConfig  m_cfg{};
    FishWaterFn m_water;
    FishBedFn   m_bed;
    std::string m_modelDir;
    std::vector<Fish>       m_fish;
    std::vector<FishSchool> m_schools;
    SpeciesArt  m_art[(uint32_t)FishSpecies::Count];
    // The loaded art's OWNERS. A Model owns the mesh/texture handles its Entities
    // point at, so it must outlive them: these live as long as the system does.
    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    std::vector<x3::asset::Model>            m_models;
    // The PROCEDURAL FALLBACK art (art pass 2) — built lazily, and only if some
    // species failed to load. 3 lofted meshes + 1 countershading gradient.
    bool        m_procBuilt = false;
    x3::rhi::MeshHandle     m_meshHead{}, m_meshMid{}, m_meshTail{};
    x3::rhi::TextureHandle  m_skin{};     // the countershading gradient
    uint32_t    m_procTris[3] = { 0, 0, 0 };
    float       m_time = 0.0f;
    uint32_t    m_rngState = 0xF15Fu;
};

} // namespace x3::game
