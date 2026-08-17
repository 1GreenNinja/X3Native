#pragma once
// ===========================================================================
// THE SMALL MOUNTAIN TOWN — the sketch's ladder-switchback town, made real.
//
// SOURCE OF TRUTH: docs/design/ROAD_NETWORK_SKETCH_V2.png, the brown-labelled
// "Small Mountain Town" hanging off a yellow ladder-switchback road, and
// docs/design/ROAD_NETWORK_PLAN.md:701 which already named the site:
// "Town 2 — the climb foot, where the inner tour meets the summit road."
// The SUMMIT SPUR (registerSummitSpur, app/road_network.h) IS that ladder —
// it is the only switchback climb in the network, and app/forest.cpp:384
// already treats its peak as "the sketch's spiral-road mountain family".
// So: main street is laid along the spur's lowest, gentlest reach, and the
// town is an EnvArt-style OVERLAY over it — the road owns the pavement, the
// town owns everything beside it.
//
// MECHANISM (reused, not invented — NO_SLOP rule 1): EnvArtSystem's district
// API, exactly the path app/road_trees.cpp takes for its groves
// (beginFromDir + addGlbInstance per placement, one GPU upload per unique
// GLB, per-instance world transforms). Collision is the town's own: a
// yaw-rotated static box per building (the app/world_cars.cpp precedent), so
// the car cannot drive through a shop. A full triangle-mesh collider per
// 100k-tri prefab would be twenty broadphase monsters for no gameplay gain.
//
// THE ART — SWAPPED 2026-08-17, and why. Round one dressed this town in the
// armory's HouseForge prefabs. Eyes-on at full res they read as DERELICT: dark,
// spiky, broken silhouettes, because that kit is authored as COLLAPSED RUINS
// (docs/design/TOWN_ASSET_SCOUT.md reached the same verdict independently, and
// the armory's own thumbnail bakes show it too). Nothing was wrong with the
// placement machinery, so nothing about it changed — only the tables did.
//
// The kit is now the licensed `Complete Racing Game URP All in One` pack:
// Red_House House_1..4 as the shells, its own 7.2 m highway lamp, its roadside
// billboards and its picket fence. ONE pack means one register, and that pack
// was authored for a DRIVING game, so the town belongs to this world's road
// network instead of being a medieval village a freeway happens to pass.
// Eight facades come from four shells in two real photographic paints.
//
// THE PIPELINE TRAP, and the receipt (tools/town_assets.py):
//   * the pack ships NO Unity `.mat`/`.meta` files at all, so FBX2glTF emits
//     1x1 WHITE PLACEHOLDER textures and convert_unity_pack.py's GUID
//     resolution has nothing to resolve — every building would have been flat
//     grey (NO_SLOP rule 3). The tool injects the pack's real albedos BY
//     MATERIAL NAME instead;
//   * three of the four wall/roof albedos are TIFF, and
//     engine/asset/ModelLoader.cpp decodes embedded images with stb_image,
//     which cannot read TIFF. They are transcoded to JPEG.
// `town_assets.py verify` asserts both — plus that no bound image is a
// placeholder and no material is untextured near-metal — and is GREEN. It is
// the same gate that caught round one's WebP (the tree has no WebP decoder
// either), generalised so the next kit cannot slip through.
//
// ORIENTATION IS MEASURED, NOT GUESSED (X3_WORLD_RULES rule 0/3): each shell's
// FRONT is the horizontal direction from its bounding-box centre to the
// centroid of its DOOR material. Where that is ambiguous the render is the
// arbiter — House_2 has no door material at all, and its entrance was found by
// rendering it at 0/90/180/270 and looking. Those angles, and the MEASURED
// positions of each shell's front-elevation window glass, are baked into
// kAssets in town.cpp and listed in docs/design/TOWN_MANIFEST.md.
//
// KEEP-OUTS ARE LAW: nothing is ever placed inside |lat| < kStreetKeepOutM of
// the street centreline — that band is pavement, shoulder, apron and skirt,
// and a shop standing in it is a wall across the road.
//
// LIVES: 6 pedestrians walking a sidewalk loop, each an AnimatedCharacter
// (app/character_anim.h — the ONE character rig runtime; THE CONTACT LAW is
// enforced inside it, every frame) driven by its own Player capsule over the
// crowd_skin roster rigs. They are ticked only within kPedActiveM of the
// camera: beyond that the terrain tiles they stand on are not resident, and a
// character with no ground under it is a log flood and a wasted raycast.
// ===========================================================================

#include "env_art.h"
#include "character_anim.h"
#include "player.h"
#include "road_network.h"
#include "scene.h"

#include "engine/physics/IPhysicsWorld.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace x3::game {

// Nothing stands closer than this to the street centreline. The spur is a
// base-profile road: kPavedHalfM (14.63 m) of pavement + apron, and
// RoadSpec::halfWidth carves a metre past that. 17.5 m clears the carve edge
// and leaves the verge for the sidewalk the pedestrians walk.
// PAIRED with kSidewalkLatM below and with RoadSpec::halfWidth in
// road_network.h — widen the road and this must move with it.
constexpr float kStreetKeepOutM = 17.5f;
// Where the pedestrian loop runs: on the verge, outside the apron, inside the
// shop fronts (nearest shop face sits at ~19 m).
constexpr float kSidewalkLatM   = 16.4f;
// Pedestrians tick only this close to the camera (terrain residency + fps).
constexpr float kPedActiveM     = 320.0f;

class Town {
public:
    // XZ disc nothing may be placed in (showcase cameras, the junction mouth).
    struct KeepOut { float x = 0.0f, z = 0.0f, r = 12.0f; };

    struct Config {
        // The ladder road. Must be an ALREADY-REGISTERED route (registerRoad
        // has run) so terrainHeightAtWorld returns the carved field, and
        // `streetY` must be that call's graded datum — the pavement rides the
        // datum, and a shop grounded on the raw field beside a cut would sit
        // in a trench.
        const RoadSpec*           street  = nullptr;
        const std::vector<float>* streetY = nullptr;
        // Main street is the reach [startU, endU] in metres of arc length from
        // the route's node 0 (the junction end of a spur).
        float startU = 60.0f;
        float endU   = 700.0f;
        std::vector<KeepOut> keepOut;
        uint32_t seed = 0x704Fu;
    };

    // Build the town. Returns false (and places nothing) if the street is
    // degenerate or the GLB root will not mount — the world is then simply
    // townless, never broken.
    bool build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& phys, const Config& cfg);

    // Spawn the pedestrians. Separate from build() because it needs the rigged
    // GLBs (tens of MB) and a host that never walks near the town should not
    // pay for them; call it right after build(). Returns how many stood up.
    uint32_t spawnPedestrians(x3::rhi::IRenderDevice& device,
                              x3::phys::IPhysicsWorld& phys);

    // Walk the pedestrians one frame. `camX/camZ` gate the work (kPedActiveM).
    void update(float dt, x3::phys::IPhysicsWorld& phys,
                x3::rhi::IRenderDevice& device, float camX, float camZ);

    // Draw the town (props + pedestrians). Call alongside scene.render(), the
    // same slot RoadTrees::draw takes. Returns drawables issued.
    uint32_t draw(x3::rhi::IRenderDevice& device,
                  const x3::rhi::FrameContext& frame) const;

    // NIGHT DIAL, 0 = full day .. 1 = full night. Scales the shop-window
    // emissive and the lamp lights. Windows glowing at noon is its own kind of
    // slop, so the host drives this from its time of day; the default (1) is
    // the dusk the lane was briefed for.
    void setNight(float k);

    // Warm point lights, one per street lamp/torch — feed these to
    // IRenderDevice::setPointLights alongside the host's own.
    const std::vector<x3::rhi::PointLight>& lights() const { return m_lights; }

    void shutdown(x3::rhi::IRenderDevice& device);

    // ---- THE EYE GATE (--screenshot-town) ----------------------------------
    // Camera poses DERIVED FROM THE TOWN'S OWN DATA, never eyeballed
    // coordinates: ENGINE_GOTCHAS 4.1 is explicit that hand-picked cameras end
    // up inside walls. out = { x, y, z, yaw, pitch }, device convention
    // (fwd = (cos yaw, ., sin yaw)). Returns false before build().
    //   0 main street from the road      3 the lit windows, close, at dusk
    //   1 a shop front, close, textured  4 the town from across the valley
    //   2 the pedestrians on the sidewalk
    static constexpr int kShots = 5;
    bool showcaseCamera(int which, float out[5]) const;

    // ---- the gates read these ----
    uint32_t buildingCount()  const { return m_buildings; }
    uint32_t propCount()      const { return m_props; }
    uint32_t parkedCarCount() const { return m_cars; }
    uint32_t windowCount()    const { return (uint32_t)m_windows.size(); }
    uint32_t pedestrianCount() const { return (uint32_t)m_peds.size(); }
    // Town centre (world XZ + the street datum there) — the MapPoi anchor.
    float centerX() const { return m_cx; }
    float centerZ() const { return m_cz; }
    float centerY() const { return m_cy; }
    bool  built()   const { return m_built; }

private:
    // One resampled station of the street centreline.
    struct Station { float x, z, y, tx, tz, u; };
    std::vector<Station> m_st;
    bool stationAt(float u, Station& out) const;

    EnvArtSystem m_art;      // buildings, stalls, props (HouseForge kit)
    EnvArtSystem m_carArt;   // parked cars (the converted Vehicles/ fleet)

    // Shop windows: small emissive panes, Scene entities so the host's normal
    // render path draws them (X3_WORLD_RULES rule 5 — flat emissive is kept
    // well under the ACES clip at ~0.45 and the glow comes from the paired
    // point light, not from a white slab).
    struct Window { SceneHandle ent; float rgb[3]; float base; };
    std::vector<Window> m_windows;
    Scene*              m_scene = nullptr;

    std::vector<x3::rhi::PointLight> m_lights;
    // The AUTHORED linear colour of each light (3 floats per light). setNight
    // always rebuilds m_lights[i].color from this, never from itself.
    std::vector<float>               m_lightAuthored;

    // A pedestrian: its own capsule + its own rig, walking a closed loop.
    struct Ped {
        std::unique_ptr<Player>            body;
        std::unique_ptr<AnimatedCharacter> rig;
        std::vector<float>                 wpX, wpZ;   // the loop
        uint32_t                           next = 0;
        float                              dwell = 0.0f;
    };
    std::vector<Ped> m_peds;
    std::vector<float> m_loopX, m_loopZ;   // the shared sidewalk polyline

    // Where the eye gate points. Captured DURING placement so the cameras can
    // never drift from the geometry (ENGINE_GOTCHAS 4.1).
    struct Anchor { float x, z, y, faceYaw, reach; };
    std::vector<Anchor> m_shopFronts;      // one per building, front-facing
    uint32_t m_heroFront = 0xFFFFFFFFu;    // the square's hero facade
    float m_dirX = 1.0f, m_dirZ = 0.0f;    // street tangent at the centre
    float m_uCentre = 0.0f;

    float    m_night     = 1.0f;
    uint32_t m_buildings = 0, m_props = 0, m_cars = 0;
    float    m_cx = 0.0f, m_cz = 0.0f, m_cy = 0.0f;
    bool     m_built = false;
};

// The measured clip table for the crowd_skin roster rigs (AnnaCasual_anim /
// marcus_webb_anim / chief_martinez_anim). Their clips are named
// Idle/Walk/Run/Jump — NOT Jake's "Walking"/"Running", and
// AnimatedCharacter resolves by EXACT name, so pointing a roster rig at
// jakeClipTable() silently yields an idle-only statue that slides.
CharacterClipTable townPedClipTable();

} // namespace x3::game
