#pragma once
// ===========================================================================
// ROADSIDE CAMPFIRES — "fires on the side of the road with the benches..
// where people roast hot dogs" (owner ask, 2026-08-17).
//
// A handful of the existing GROVE + BENCH sites (app/road_trees.h — the
// benches record their placement in RoadTrees::benchSites()) each get:
//   * a STONE RING — procedural faceted stones textured with the REAL
//     cv_rock_flume surface-library set (X3_WORLD_RULES rule 5: no flat-tint
//     stand-ins; the owner's directive allows procedural geometry for the
//     stones only), pavilion points sunk into the earth (rule 4: contact);
//   * CHARRED LOGS leaning in a loose teepee over an EMBER BED whose emissive
//     stays under the 0.5 ACES clip law (rule 5) — the GLOW comes from the
//     additive particle fire + the warm flickering point light, not from a
//     white slab;
//   * FIRE through IRenderDevice::submitParticles (NO_SLOP rule 1 — the same
//     billboard pass that carries the rain and the boat wakes): additive
//     flame tongues + ember sparks feeding bloom, alpha smoke above. All
//     stateless functions of the fire clock — no per-particle heap, no pools;
//   * a WARM FLICKERING POINT LIGHT merged into the host's one per-frame
//     light upload (uploadTunnelLights' extra lane);
//   * a CRACKLE LOOP (assets/audio/ambient/campfire_crackle_loop.wav —
//     synthesized by tools/gen_campfire_audio.py, the gen_water_audio.py
//     offline-bake pattern) through IAudioSystem::startLoop3D;
//   * PEOPLE: 2-3 characters per fire through the shared AnimatedCharacter
//     module (app/character_anim.h — the ONE way characters animate), cast
//     from the CIVILIAN roster — the six CityPerson_* rigs the town walks
//     (tools/town_people.py), NOT crowd_skin's club cast, which is a civilian
//     woman plus a clawed mutant and a SWAT operator. Clips resolve by
//     MEASURED exact name (town.cpp::townPedClipTable: Idle/Walk/Run/
//     LookAround; AnnaCasual additionally Sit/CarryIdle). One AnnaCasual
//     SITS ON THE BENCH (her Sit
//     clip's hips perch at ~0.44 m — the bench seat height; the bench is the
//     seat prop) holding a STICK toward the fire through the rig's own hand
//     bone (AnimatedCharacter::boneWorld) with a hot dog on the end — an
//     AUTHORED pose doing a believable job, never a contorted rig. Rigs
//     without a fitting clip simply STAND at Idle facing the fire (the
//     directive: two people standing beat one broken sitter).
//     THE CONTACT LAW is enforced inside AnimatedCharacter::update (feet
//     clamp, every frame) — no fire-sitter can ship buried.
//
// Characters tick only within kActiveM of the camera (the Town::kPedActiveM
// discipline: beyond it their terrain tiles are not resident). The fire FX
// submit only within kFxM. Deterministic: one seeded LCG per site.
// ===========================================================================

#include "character_anim.h"
#include "env_art.h"          // the bench model, when the fires site themselves
#include "road_network.h"     // RoadSpec — the fallback siting walks a polyline
#include "road_trees.h"
#include "scene.h"
#include "surface_library.h"

#include "engine/audio/IAudioSystem.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/rhi/IRenderDevice.h"

#include <memory>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// FALLBACK SITING — bench sites along a plain road polyline.
//
// THE RECEIPT (2026-08-17, W-NIGHT). RoadTrees plants groves (and seats a bench
// under some of them) along the TunnelRoute the tunnel demo registers — and on
// that route it plants NOTHING, every boot, silently:
//
//   [INFO] tunnel corridor: 'demo ridge' — 640 m spine ... ROOFED s=88..546
//   [INFO] road_trees: 0 trees in 0 groves, 0 benches, along 640 m (0 rejected)
//   [WARN] road_trees: NOTHING PLANTED — tree GLBs missing? ...
//
// The GLBs are fine. The route is 640 m long, 458 m of it is roofed, and
// RoadTrees keeps kPortalMargin = 80 m clear either side of the bore: the two
// open spans are 25..8 and 626..615 — both NEGATIVE. Zero rejections is the
// tell (nothing was even tried). "0 positions rejected" plus a warning blaming
// the assets is why this survived: the diagnostic pointed at the wrong thing
// (NO_SLOP rule 9 — the number was there, nobody read it).
//
// The fires are not going to wait for the tree lane. They site themselves on
// a real road polyline — the VALLEY ROAD (registerRiverRoad: ~3.9 miles of
// two-lane country road through the river valley, which is where a roadside
// fire belongs anyway, not on the 16-lane freeway) — using the same verge
// lateral band, the same 4-corner flatness probe, the same water/datum
// clearances and the same yaw convention RoadTrees' own bench block uses.
// PAIRED VALUES (rule 4): those constants live in road_trees.cpp's bench block
// and are mirrored here; a change to either is a change to both.
//
// When RoadTrees DOES seat benches (a route with open country), pass its
// benchSites() and `placeBench = false` — the model is already in the world.
// These sites carry no model, so `placeBench = true` puts one there.
std::vector<RoadTrees::BenchSite> benchSitesAlongRoad(
        const RoadSpec& spec, const std::vector<float>& roadY,
        float spacingM, uint32_t maxSites, uint32_t seed);

class Campfires {
public:
    // Pick up to kMaxFires bench sites (spaced apart along the road) and build
    // the rings/logs/embers as Scene entities + spawn the people. Returns the
    // number of fires lit (0 if no bench sites — never fatal, the road is
    // simply fireless). `audio` may be null (silent fires).
    // `placeBench`: true when the sites came from benchSitesAlongRoad() and no
    // bench model exists there yet — the fires then place the same two armory
    // bench GLBs RoadTrees uses (nature/SM_WoodBench_01a, nature/SM_Bench).
    uint32_t build(Scene& scene, x3::rhi::IRenderDevice& device,
                   x3::phys::IPhysicsWorld& phys,
                   const std::vector<RoadTrees::BenchSite>& benches,
                   x3::audio::IAudioSystem* audio,
                   bool placeBench = false);

    // Draw the benches this system placed (no-op when placeBench was false).
    void drawProps(x3::rhi::IRenderDevice& device,
                   const x3::rhi::FrameContext& frame) const;

    // Advance the fire clock + tick the nearby people (camera-gated).
    void update(float dt, float camX, float camZ,
                x3::phys::IPhysicsWorld& phys, x3::rhi::IRenderDevice& device);

    // Draw the people + their roasting sticks (between beginFrame/endFrame,
    // alongside the other character draw fans).
    void drawCharacters(const x3::rhi::FrameContext& frame,
                        x3::rhi::IRenderDevice& device);

    // Submit the flame/ember/smoke particles for fires near the camera.
    void submitFx(x3::rhi::IRenderDevice& device, float camX, float camZ);

    // Append up to `max` flickering fire lights nearest the camera to `out`.
    // Returns how many were written (for the host's single light upload).
    uint32_t lights(x3::rhi::PointLight* out, uint32_t max,
                    const float camPos[3]) const;

    uint32_t fireCount()   const { return (uint32_t)m_fires.size(); }
    uint32_t peopleCount() const;
    // Eye-gate camera for fire `i`: {x,y,z,yaw,pitch} (device yaw convention),
    // framed on the ring + the people. False if `i` out of range.
    bool showcaseCamera(uint32_t i, float out[5]) const;
    // Fire i's centre (shot staging / logs).
    bool firePos(uint32_t i, float out[3]) const;

    void shutdown(x3::rhi::IRenderDevice& device);

private:
    struct Person {
        std::unique_ptr<Player>            body;
        std::unique_ptr<AnimatedCharacter> rig;
        float yawTrim = 0.0f;      // full desired facing (rig m_yaw stays 0 idle)
        float lookYaw = 0.0f;      // device-convention look toward the fire
        bool  stick   = false;     // holds the roasting stick (hand-bone probe)
        int   boneProbed = -1;     // -1 not yet, 0 no hand bone found, 1 found
        std::string boneName;      // the resolved hand bone
    };
    struct Fire {
        float x = 0, y = 0, z = 0;         // ring centre, on the ground
        float benchYaw = 0.0f;             // bench long-axis yaw (site record)
        float towardRoadX = 0, towardRoadZ = 0;
        uint32_t seed = 1;
        float lightPhase = 0.0f;
        std::vector<Person> people;
        x3::audio::LoopHandle crackle{};
    };

    void buildRing(Scene& scene, x3::rhi::IRenderDevice& device, Fire& f);
    void spawnPeople(Fire& f, uint32_t idx, x3::rhi::IRenderDevice& device,
                     x3::phys::IPhysicsWorld& phys,
                     const RoadTrees::BenchSite& bench);

    std::vector<Fire> m_fires;
    float m_clock = 0.0f;
    EnvArtSystem m_props;                  // the bench GLBs, when we place them
    bool  m_propsBuilt = false;
    SurfaceLibrary m_surf;                 // cv_rock_flume for the stones
    x3::rhi::MeshHandle m_stoneMesh[3]{};  // three jittered faceted variants
    x3::rhi::MeshHandle m_logMesh{};
    x3::rhi::MeshHandle m_emberMesh{};
    x3::rhi::MeshHandle m_stickMesh{};     // roasting stick + hot dog
    x3::rhi::MeshHandle m_hotdogMesh{};
    bool m_built = false;
};

} // namespace x3::game
