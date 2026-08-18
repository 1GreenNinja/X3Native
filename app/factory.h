#pragma once
// ===========================================================================
// THE GLIMVALE CONFECTIONERY WORKS — the driving world's hero landmark, and
// the GOLDEN TICKET hunt that opens its gate.
//
// SCOPE. This is the EXTERIOR in `--world tunnel`: a building you see from the
// freeway, drive up to, and stand at the gate of. It is NOT the interior — the
// five-floor Confection Annex lives on its own branch and is another session's
// lane. The two never share a file, a symbol or an asset path; this module is
// `factory.*`, that one is `factory_annex.*`.
//
// CLEAN-ROOM. Dahl-STYLE, not Dahl. No author, film, studio or character name
// appears in this code, in a string it prints, in a texture it bakes or in an
// asset path it loads. "Glimvale" is ours. The owner's standing brief
// (docs/design/GAME_BACKLOG.md) is the whole art direction: "based on the BOOK
// VISUALS, not the insanely craptastic movie... glassy, and the tubes and
// pipes be so shiny and cool and alive and breathing and pulsing" — so the
// works is a GLASSHOUSE over an industrial base, wrapped in gleaming external
// pipework whose cores pulse, and its two chimneys breathe real smoke.
//
// ---------------------------------------------------------------------------
// WHERE IT STANDS, AND WHY (the sketch, ROAD_NETWORK_SKETCH_V2.png)
// ---------------------------------------------------------------------------
// The brief allowed two sites — "NE forest edge or riverside" — and the world
// picks between them by MEASUREMENT, not by taste:
//
//   * RIVERSIDE IS NOT VISIBLE FROM THE FREEWAY. The river runs x 320..900,
//     z 180..-1120 (terrain.cpp kRiver*); the inner tour — the freeway, the
//     main drag — orbits (-592, -352) at r 3.4-4.6 km. Nearest approach of the
//     two is ~1.5 miles (river_bridge.h records the same measurement). A
//     landmark there is a landmark nobody driving ever sees, and the brief's
//     hard requirement is "visible from the freeway".
//   * THE NE FOREST EDGE IS. The centre-north forest patch (forest.cpp region
//     2, ellipse centre (1700, 3900), radii 1150 x 600) sits just outside the
//     tour's north-east arc, which passes within a few hundred metres of its
//     southern edge. A works standing in the clearing at that edge has the
//     woods behind it (depth, and a silhouette that is not sky-on-both-sides),
//     open country in front, and the freeway running past its gate.
//
// So the site is the NE arc, and WHICH node of it is chosen by planning: every
// freeway node in the north-east sector is offered an outward radial site, the
// pad footprint is sampled against the natural height field, and the flattest
// candidate that is dry, off every registered corridor and reachable at the
// freeway's own 7% grade wins. planFactoryWorks() is PURE — no registration,
// no GPU, no state — exactly like planRiverBridge(), so --test-factory
// interrogates the same plan the world is built from.
//
// ---------------------------------------------------------------------------
// THE PAD IS A PLATFORM, NOT A CARVE
// ---------------------------------------------------------------------------
// terrain.h corridors only ever LOWER ground (road_network.h says so at
// length), so a factory floor cannot be filled up to a level. The works
// therefore stands on a poured CONCRETE PLATFORM whose top is one datum and
// whose skirt is extruded DOWN past the lowest ground under the footprint and
// lapped beneath it — the same "THICK CONCRETE in the base and aprons.. not
// floating on top!!!" the road prism was built for. Site scoring minimises the
// platform's exposed face, which is the honest measure of "is this ground flat
// enough for a factory".
//
// The DRIVE's terminal datum is PINNED to the platform top, so the pavement
// meets the forecourt flush instead of stepping onto it (the defect the ring
// landings exist to prevent).
//
// ---------------------------------------------------------------------------
// ART SOURCES — real textures or it does not ship (NO_SLOP rule 3)
// ---------------------------------------------------------------------------
//   * MASSING: procedural geometry wearing surface_library PBR sets
//     (albedo + normal + MR), the same route the river bridge and the tunnel
//     portals take. Not tinted graybox.
//   * PACK KIT: assets/converted_glb/Factory/*.glb, mined from the licensed
//     ScansFactory packs by tools/convert_scansfactory.py. That converter
//     exists because these packs ship with every .mat and .meta stripped, so
//     the repo's GUID-walking converter would have emitted grey — see its
//     header. Measured sizes (rule 0/1, ortho-free because the accessor bounds
//     ARE the measurement):
//         sm_Chimney_01_01   8.19 x 61.11 x 8.19 m, origin at the base, XZ centred
//         sm_Chimney_01_02   7.00 x 61.11 x 7.00 m, ditto
//         sm_MainGate_01_01  4.68 x  5.47 x 1.27 m, leaf long axis +X
//         sm_ConcreteFence_01_01  2.24 x 1.99 x 0.17 m, panel spans x[-2.22, 0.02]
//         sm_Pipe_Straight_8m_03_14  8 m along +X from the origin, 0.8 m bore
//         sm_Pipe_Elbow_03_02  1.19 x 1.18 x 0.77 m
//         sm_Barrel_01_01 / sm_Container_Body_01_01  yard dressing
//     Every one has its origin on its contact plane already (min Y ~= 0), so
//     placement Y is the platform top — rule 4 with nothing to correct.
//   * REJECTED BY MEASUREMENT: sm_Smokestack_01_01 from the same vendor is
//     1.05 m tall. It is a rooftop vent, not a smokestack, and the name is a
//     trap. The 61 m sm_Chimney_* pair are the real thing and are what the
//     skyline read is built on.
//
// ---------------------------------------------------------------------------
// THE GOLDEN TICKETS
// ---------------------------------------------------------------------------
// Five cards hidden at five landmarks. `E` inside 3.5 m collects one, the HUD
// carries "TICKETS n/5", the console `tickets` command reads and sets the
// count (THE ARG CONVENTION, engine/core/IConsole.h: `tickets 3` arrives as
// args[0] == "3"), and the fifth one slides the works gate open.
//
// The card is texture-gated emissive over a near-black field (X3_WORLD_RULES
// rule 5 — a flat emissive above ~0.5 clips to a white slab under ACES), so it
// GLINTS rather than glows, with an additive spark billboard riding it.
// ===========================================================================

#include "road_network.h"
#include "surface_library.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

class Scene;

// ---------------------------------------------------------------------------
// THE PLAN — every decision, computed headlessly from the public terrain and
// road queries. Pure: planFactoryWorks() registers nothing and touches no GPU.
// ---------------------------------------------------------------------------
struct FactoryPlan {
    bool  ok = false;
    const char* whyNot = "";      // a plan that fails says why, out loud

    // The site frame. `f` points from the freeway INTO the site (the drive's
    // direction of travel); `r` is its right-hand normal, r = (-fz, fx).
    float cx = 0.0f, cz = 0.0f;   // platform centre, world XZ
    float fX = 1.0f, fZ = 0.0f;
    float rX = 0.0f, rZ = 1.0f;

    // The platform.
    float padY     = 0.0f;        // finished forecourt level (the ONE datum)
    float padLowY  = 0.0f;        // lowest natural ground under the footprint
    float exposedM = 0.0f;        // padY - padLowY: the platform's tallest face
    float reliefM  = 0.0f;        // max-min natural over the footprint (flatness)

    // The freeway landing.
    uint32_t fwyNode = 0;         // inner-tour node the drive leaves from
    float jx = 0.0f, jz = 0.0f, jy = 0.0f;   // landing point + the tour's datum
    float mainTX = 1.0f, mainTZ = 0.0f, mainGrade = 0.0f;
    float mainShoulderEdgeM = kShoulderHalfM;   // near-carriageway edges (divided
    float mainPavedEdgeM    = kPavedHalfM;      // freeway — see registerSpawnConnector)
    float offsetM  = 0.0f;        // site centre distance from the tour centreline
    float driveGradePct = 0.0f;   // required average grade, freeway -> platform

    // Where the gate stands (world XZ + the platform datum).
    float gateX = 0.0f, gateZ = 0.0f;
};

// THE LOCAL FRAME, in metres, `a` along f (away from the freeway) and `b`
// along r. Everything the works is made of is authored here once, so the
// self-test and the builder measure the same building.
constexpr float kFacPadA0   = -60.0f, kFacPadA1 =  70.0f;   // 130 m deep
constexpr float kFacPadB0   = -60.0f, kFacPadB1 =  60.0f;   // 120 m wide
constexpr float kFacHallA0  =   6.0f, kFacHallA1 = 52.0f;   // production hall
constexpr float kFacHallB0  = -48.0f, kFacHallB1 = 14.0f;
constexpr float kFacHallH   =  18.0f;                        // eaves
constexpr float kFacSawH    =   4.6f;                        // north-light sawtooth ridge
constexpr float kFacGlassA0 =  10.0f, kFacGlassA1 = 38.0f;   // the Hothouse
constexpr float kFacGlassB0 =  18.0f, kFacGlassB1 = 46.0f;
constexpr float kFacGlassH  =  46.0f;
// GATE OPENING. Half-width 4.68 m == exactly one sm_MainGate_01_01 leaf, so
// the two leaves meet on the centreline UNSTRETCHED (a 1.5x horizontal scale
// to fill the freeway's 14.6 m running surface would have smeared the pack's
// bar spacing — a real works necks its drive down at the gate instead, and
// 9.36 m is still three lanes of opening). Piers fill out to the fence.
constexpr float kFacGateB   =   4.68f;                       // gate half-opening
constexpr int   kFacSiloCount = 3;
constexpr float kFacSiloR   =   5.0f, kFacSiloH = 26.0f;
constexpr float kFacChimneyH = 61.11f;                       // MEASURED, the GLB

// Plan the works against the freeway. `freeway` / `freewayY` are the inner
// tour's registered spec + graded datum (registerRoad's outRoadY). Reads the
// NATURAL height field, so it must run in the boot registration block —
// before TerrainStreamer::init(), like every other producer.
FactoryPlan planFactoryWorks(const RoadSpec& freeway,
                             const std::vector<float>& freewayY);

// ---------------------------------------------------------------------------
// THE DRIVE — the gated approach road off the freeway. Registered exactly like
// the spawn connector: authored straight-in on the outward radial, terminal
// datums pinned at BOTH ends (the tour's graded datum at the landing, the
// platform top at the gate) so neither joint steps.
// ---------------------------------------------------------------------------
struct FactoryDriveResult {
    RoadBuildResult    road;
    RoadSpec           spec;
    std::vector<float> roadY;
    RoadJunction       jct;        // the mouth onto the freeway
    bool               ok = false;
    float              lengthM = 0.0f;
};
FactoryDriveResult registerFactoryDrive(const FactoryPlan& plan,
                                        const RoadSpec& freeway,
                                        const std::vector<float>& freewayY);

// ---------------------------------------------------------------------------
// THE WORKS
// ---------------------------------------------------------------------------
class FactoryWorks {
public:
    // Build the platform, the hall, the Hothouse, the silos, the pipework, the
    // chimneys, the sign, the fence and the gate. Static collision on every
    // solid; render-only on the glow bands and the smoke. Call AFTER the
    // terrain streamer exists (the platform skirt reads the carved field) and
    // after registerFactoryDrive.
    bool build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& phys, const FactoryPlan& plan);

    // Per-frame life: the pipe cores breathe, the machinery-shake wobble runs,
    // the gate slides when it has been opened. Cheap — it edits a handful of
    // Scene entity transforms and emissive terms.
    void update(Scene& scene, float dt);

    // Chimney smoke. MUST be called between beginFrame/endFrame every frame:
    // the device clears its particle batches in beginFrame, so a hoisted
    // submit draws nothing (river_life.cpp carries the same warning).
    void drawSmoke(x3::rhi::IRenderDevice& device, const float cam[3]);

    // THE GATE. Idempotent; the slide is animated by update().
    void openGate();
    bool gateOpen()  const { return m_gateOpen; }
    float gateSlide() const { return m_gateT; }   // 0 shut .. 1 fully open

    void shutdown(x3::rhi::IRenderDevice& device);

    bool     built() const { return m_built; }
    uint32_t meshCount() const { return m_meshCount; }
    uint32_t triCount()  const { return m_triCount; }
    uint32_t propCount() const { return m_propCount; }   // pack GLB instances placed
    // Where the player is told to stand. Gate centre, on the platform.
    void gatePoint(float out[3]) const;
    const FactoryPlan& plan() const { return m_plan; }

private:
    struct SlideLeaf { uint32_t ent = 0; float base[16] = {}; float dir = 1.0f; };
    struct PulseTube { uint32_t ent = 0; float phase = 0.0f; };

    FactoryPlan m_plan{};
    SurfaceLibrary m_surf;
    std::unique_ptr<x3::asset::IAssetSource>  m_assets;
    std::unique_ptr<x3::asset::IModelLoader>  m_loader;

    // THE GATE'S COLLISION is one invisible slab body, not the pack leaves'
    // hulls: a sliding half-open collider is a state nobody can test, and a
    // car nosed into a leaf mid-slide is a physics bug waiting. Opening
    // REMOVES the body outright, once.
    x3::phys::BodyId       m_gateBody{};
    x3::phys::IPhysicsWorld* m_physRef = nullptr;
    uint32_t               m_gateBlockEnt = 0xFFFFFFFFu;

    std::vector<SlideLeaf> m_gateLeaves;
    std::vector<PulseTube> m_tubes;      // glass tube cores that breathe
    std::vector<uint32_t>  m_shakers;    // machinery boxes with a running wobble
    std::vector<float>     m_shakerBase; // their authored transforms (16 each)

    // Chimney mouths, world space, and the smoke pool.
    struct Puff { float x, y, z, vy, drift, age, life, size0; };
    float m_stackTop[2][3] = {};
    std::vector<Puff> m_puffs;
    std::vector<x3::rhi::IRenderDevice::ParticleInstance> m_smokeOut;
    float m_emitAcc = 0.0f;
    uint32_t m_rng = 0x9E3779B9u;

    float m_clock = 0.0f;
    bool  m_gateOpen = false;
    float m_gateT = 0.0f;
    bool  m_built = false;
    uint32_t m_meshCount = 0, m_triCount = 0, m_propCount = 0;
};

// ---------------------------------------------------------------------------
// THE GOLDEN TICKETS
// ---------------------------------------------------------------------------
constexpr int   kTicketCount   = 5;
constexpr float kTicketReachM  = 3.5f;    // how close `E` has to be
constexpr float kTicketHoverM  = 1.15f;   // card centre above its anchor

// THE FIVE HIDING PLACES, derived from the world's own queries. ONE definition,
// consumed by the host AND by --test-factory — NO_SLOP rule 4: two places that
// must agree are one place. (The first cut had the host and the test each spell
// the list out, and the test's copy put the riverbank card in the middle of the
// river; the shared version walks out to a dry bank and both get it right.)
//
// TODO (Lane 4, W-TOWN): the fifth card belongs in the Small Mountain Town's
// square. Until that lane merges there is no square to hide it in, so it sits
// at the range circuit's start/finish — a real place a driver already goes.
// When town.* lands, change the fifth entry here and NOTHING else moves.
struct TicketSpotDef {
    const char* name;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
uint32_t factoryTicketSpots(const FactoryPlan& plan, TicketSpotDef out[kTicketCount]);

class GoldenTickets {
public:
    // Register a hiding place BEFORE build(). `y` is the ground/deck the card
    // hovers over. Names are what the pickup toast prints.
    void addSpot(const char* name, float x, float y, float z);
    uint32_t spotCount() const { return (uint32_t)m_spots.size(); }

    bool build(Scene& scene, x3::rhi::IRenderDevice& device);

    // Spin, bob, glint. `interactEdge` is the RISING edge of E for this frame
    // (the host owns the edge — see host_tunnel's other interacts). Returns
    // the index just collected, or -1.
    int update(Scene& scene, float dt, float px, float py, float pz,
               bool interactEdge);

    // The additive spark that makes a card catch the eye at range. Same
    // per-frame contract as FactoryWorks::drawSmoke.
    void drawGlints(x3::rhi::IRenderDevice& device);

    // "TICKETS n/5" plus the [E] prompt and the pickup toast.
    void drawHud(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                 float px, float py, float pz) const;

    void shutdown(x3::rhi::IRenderDevice& device);

    int  collected() const { return m_collected; }
    bool allFound()  const { return m_collected >= (int)m_spots.size() &&
                                    !m_spots.empty(); }
    // Console `tickets [n]` — clamps, re-shows/hides the cards to match.
    void setCollected(Scene& scene, int n);
    // For the self-test / the map: where spot i is.
    void spotPos(uint32_t i, float out[3]) const;
    const char* spotName(uint32_t i) const;
    bool  spotTaken(uint32_t i) const;

private:
    struct Spot {
        std::string name;
        float x = 0, y = 0, z = 0;
        uint32_t ent = 0;
        bool taken = false;
    };
    std::vector<Spot> m_spots;
    std::vector<x3::rhi::IRenderDevice::ParticleInstance> m_glintOut;
    x3::rhi::MeshHandle    m_cardMesh{};
    x3::rhi::TextureHandle m_cardTex{};
    x3::rhi::TextureHandle m_cardMr{};
    int   m_collected = 0;
    float m_clock = 0.0f;
    mutable float m_toast = 0.0f;
    mutable std::string m_toastText;
    bool  m_built = false;
};

// --test-factory — headless. F0 the negative control (the riverside site the
// brief also allowed is NOT freeway-visible, measured), F1 the plan lands in
// the sketch's NE sector at the forest edge, F2 the platform is flat enough to
// build on and its exposed face is bounded, F3 the drive reaches the freeway
// at a legal grade and lands at grade, F4 the works fits inside its fence and
// the gate opening is drivable, F5 the five ticket spots are distinct, spread,
// and each sits on real ground, F6 determinism (same plan twice).
bool runFactorySelfTest();

} // namespace x3::game
