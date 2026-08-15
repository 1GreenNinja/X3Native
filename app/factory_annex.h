#pragma once
// THE CONFECTION ANNEX — a Roald-Dahl-STYLE (original, IP-clean) wonder-works
// hidden beside the Spire, reachable only by the Anywhere Elevator's hidden
// lateral rail (elevator.h: Stop::hidden + kAnnexCode 4790). Plan:
// docs/superpowers/plans/2026-08-15-dahl-factory-annex-plan.md, PHASE 2.
//
// CLEAN-ROOM, original work. NO Dahl/Wonka names, characters, lyrics or
// trademarks anywhere — Dahl-style whimsy, original content only. Built ONLY
// from X3Native's own systems (Scene, mesh_prims, surface_library, trigger)
// + the engine interfaces.
//
// SHAPE (T5): the SHELL — five stacked wonder-room floors (40x40 m footprint,
// 11 m clear height, baseY {2,15,28,41,54}: a 13 m pitch), centered +60 m X
// from the elevator shaft, dressed aubergine-iron with brass trim and a
// shaft-facing GLASS CURTAIN wall (riders SEE the rooms during the lateral
// approach — the money shot), plus the 4 m octagonal BORE corridor the cab
// traverses at y=15 (floor B), brass-ribbed every 3 m. Physics floors/walls,
// the 10 trigger AABBs (ids 300-313), and the per-room content HOOKS
// (factory_rooms.cpp — EMPTY in Phase 2; Phase 3 fills the five rooms).
//
// Authoring style mirrors app/rifthub.{h,cpp} exactly: everything is authored
// ONCE at build() (contiguous entity spans per animated group), tick(dt)
// mutates emissive/transform in place with NO per-frame heap, every device
// mesh handle is collected in ONE vector and freed uniformly in shutdown().

#include "elevator.h"          // ElevatorSystem::Stop — the combined-graph builder
#include "scene.h"
#include "surface_library.h"
#include "trigger.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IAssetSource.h"    // Phase 5: Glimvale dressing + hero hooks
#include "engine/asset/IModelLoader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace x3::game {

// Annex trigger ids — a fresh range starting at 300 (rifthub owns 200-207;
// nothing else is near). 300 = bore entry, 301-305 = per-floor room entry
// (A..E), 310-313 = the in-room event volumes Phase 3 wires to real props.
enum class FactoryTrigger : uint32_t {
    BoreEntry     = 300,   // stepping into the bore corridor mouth (shaft side)
    RoomMixture   = 301,   // Floor A (y=2)  — THE MIXTURE ATRIUM
    RoomInvention = 302,   // Floor B (y=15) — THE INVENTION WORKS
    RoomFizz      = 303,   // Floor C (y=28) — THE FIZZ GALLERY
    RoomSorting   = 304,   // Floor D (y=41) — THE SORTING HALL
    RoomTube      = 305,   // Floor E (y=54) — THE TUBE JUNCTION
    SorterChute   = 310,   // Floor D: the Chute of Dubious Quality hatch
    FizzLowGrav   = 311,   // Floor C: the central low-grav zone (jump x1.8)
    BurstArm      = 312,   // Floor E: the golden burst dais (keypad hint 9999)
    TubeRide      = 313,   // Floor E: the pneumatic tube boarding point
};
constexpr uint32_t kFactoryTrigBase  = 300;
constexpr uint32_t kFactoryTrigCount = 10;    // 300-305 + 310-313

// One wonder-room floor. `name` is an ORIGINAL name (IP-clean law); the
// prop/glow entity spans are CONTIGUOUS ranges tick() pokes in place —
// EMPTY (count 0) until Phase 3 authors the room's machines.
struct AnnexRoom {
    const char* name = "";                 // e.g. "MIXTURE ATRIUM" (original ONLY)
    float       baseY = 0.0f;              // walking-floor Y of this room
    float       centerX = 0.0f, centerZ = 0.0f;   // room center (== annex center)
    float       accent[3] = { 1, 1, 1 };   // room accent color (linear)
    uint32_t    propEntFirst = 0, propEntCount = 0;   // animated span (pose pokes)
    uint32_t    glowEntFirst = 0, glowEntCount = 0;   // emissive-pulse span
    bool        visited = false;           // latched by its entry trigger
    // ---- Phase-3 room state (POD, authored at build; tick mutates in place —
    // no heap). stateA/stateB are room-specific clocks (Sorting: seconds since
    // the chute tripped; Tube: last capsule leg, for arrival edges). eventCount
    // bumps on host-audible beats (capsule docking thunk, hatch fully open)
    // with eventPos = where — the host edge-detects it for 3D cues.
    float    stateA = 0.0f, stateB = 0.0f;
    uint32_t eventCount = 0;
    float    eventPos[3] = { 0, 0, 0 };
    // The Sorting Hall's hatch is a MOVED-STATIC (the elevator-cab technique):
    // its physics body slides aside with the visual so the floor really opens.
    // The room keeps the world pointer it was built against for that one poke.
    x3::phys::IPhysicsWorld* physRef = nullptr;
    x3::phys::BodyId         hatchBody{};
    // ---- Phase-5 hero hooks (load-if-present). When a StarForge hero GLB is
    // found for an ANIMATED prop (the pneumatic capsule / the two sorter arms),
    // the prop authors ONE Scene entity per GLB drawable instead of the single
    // procedural mesh; the drawables' baked node transforms are kept here (16
    // floats each, filled ONCE at build) so tick() re-poses obj * heroXf[i]
    // with no per-frame heap. Counts are PER INSTANCE; 0 == procedural fallback
    // (one entity per prop, exactly the Phase-3 shape).
    std::vector<float> heroXf;
    uint32_t heroArmPrims  = 0;   // Sorting Hall: drawables per sorter arm
    uint32_t heroCapsPrims = 0;   // Tube Junction: drawables in the capsule
};

class FactoryAnnex;

// Phase-5 art-pass services shared by every room hook: the GLB loader (Glimvale
// dressing + StarForge hero hooks) and the branch tallies the self-test pins
// (F16). `loader == nullptr` (mountDir failed / assets absent) means EVERY hook
// takes its procedural-fallback branch — the annex still builds and the
// headless self-test still passes on an asset-less clone.
struct FactoryArtHooks {
    x3::asset::IModelLoader*       loader = nullptr;
    std::vector<x3::asset::Model>* models = nullptr;  // annex-owned; unloaded at shutdown
    uint32_t heroPresent  = 0;    // hero hooks that found their GLB
    uint32_t heroFallback = 0;    // hero hooks that ran the procedural fallback
    uint32_t dressEntities = 0;   // Glimvale dressing entities authored
};

// Per-room authoring/animation context handed to the factory_rooms.cpp hooks so
// Phase 3 can fill the five rooms without touching the annex core. `meshes` is
// the annex's ONE uniform mesh vector (push every created handle there);
// `centerX/centerZ` is the room's center (== the annex center — the cab's
// vertical chain passes through it, so room content rings the center).
struct FactoryRoomCtx {
    Scene&                            scene;
    x3::rhi::IRenderDevice&           device;
    x3::phys::IPhysicsWorld&          physics;
    std::vector<x3::rhi::MeshHandle>& meshes;
    SurfaceLibrary&                   surf;
    float                             centerX = 0.0f;
    float                             centerZ = 0.0f;
    // Floor A registers the confection river's water params through here
    // (points at the annex's m_river; see FactoryAnnex::riverWater()).
    x3::rhi::IRenderDevice::WaterParams* river = nullptr;
    // Phase 5: model-loading services + tallies (never null after build()).
    FactoryArtHooks* art = nullptr;
};

// The Confection Annex. Build once after device + physics + a TriggerSystem are
// up; tick() each frame; forward fired trigger ids to onTrigger(). All Scene
// props are authored at build(); tick() does NO heap work.
class FactoryAnnex {
public:
    // ---- Shell geometry constants (shared by the host, the capture rig and
    // the self-test — ONE source for the numbers the plan locks) -------------
    static constexpr float    kAnnexXOff   = 60.0f;  // annex center +X from the shaft
    static constexpr float    kFloorHalf   = 20.0f;  // 40x40 m footprint
    static constexpr float    kClearH      = 11.0f;  // clear height per floor
    static constexpr uint32_t kFloorCount  = 5;
    static constexpr float    kFloorPitch  = 13.0f;  // baseY step (11 clear + 2 slab/void)
    static constexpr float    kBoreY       = 15.0f;  // bore centerline == floor B baseY
    static constexpr float    kBoreRadius  = 2.0f;   // 4 m octagonal bore
    static constexpr float    kRoofY       = 65.0f;  // top floor 54 + 11 clear (burst plane)
    static constexpr float    kCabHalfX    = 1.6f;   // cab half-extents (match E7-E11)
    static constexpr float    kCabHalfY    = 0.25f;
    static constexpr float    kCabHalfZ    = 1.6f;
    // THE CONFECTION RIVER (Floor A, Task 7): a sunken channel cut clean
    // through the Floor-A slab at local x [kRiverX0, kRiverX1] (offsets from
    // the annex center), running the full Z extent. The water surface sits
    // BELOW every walkable deck (the water plane is engine-global). The plan
    // sketches the band "diagonally"; slab segmentation + IPhysicsWorld::addBox
    // are axis-aligned (no yaw), so the channel runs straight along Z — the
    // documented Task-7 adaptation.
    static constexpr float kRiverX0    = -16.0f;
    static constexpr float kRiverX1    = -4.0f;
    static constexpr float kRiverSurfY = 1.55f;   // water level (kFloorBaseY[0]-0.45)
    static constexpr float kRiverBedY  = 0.65f;   // channel bed (wading floor)
    // THE CHUTE OF DUBIOUS QUALITY (Floor D, Task 10): the drop-shaft column at
    // local (kChuteX, kChuteZ); floors B/C/D slabs carry a kChuteHoleHalf hole
    // (Floor A keeps its slab — the padded room's floor).
    static constexpr float kChuteX        = 8.0f;
    static constexpr float kChuteZ        = 8.0f;
    static constexpr float kChuteHoleHalf = 1.1f;
    // Walking-floor Y per room (A..E). Cab-center stop Y = baseY - kCabHalfY +
    // 2*kCabHalfY... no: cab TOP flush with the walking floor => center =
    // baseY - kCabHalfY (see makeElevatorGraph in factory_annex.cpp).
    static const float kFloorBaseY[kFloorCount];

    // Author EVERYTHING once: shell (floors/walls/glass curtain/brass trim),
    // the bore corridor + ribs, the shaft-side F1/F3 landings, physics
    // floors/walls, the 10 trigger AABBs, and the (Phase-3-empty) per-room
    // content hooks. (shaftX, shaftZ) is the elevator shaft's world XZ; the
    // annex centers at (shaftX + kAnnexXOff, shaftZ). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
               float shaftX, float shaftZ);

    // Per-frame animation: pulses the bore ribs / glass-curtain trim / room
    // glow spans in place + calls the per-room tick hooks. No per-frame heap.
    void tick(float dt, Scene& scene);

    // Dispatch a fired FactoryTrigger id (host forwards from its TriggerSystem).
    // Latches room `visited` / the low-grav zone / the chute / the burst-dais
    // hint. Idempotent.
    void onTrigger(uint32_t triggerId);

    // Free every device mesh (ONE vector, freed uniformly), the created
    // textures and the surface-library sets. Physics ownership stays with the
    // caller (the host shuts down its own world), mirroring Rifthub.
    void shutdown(x3::rhi::IRenderDevice& device);

    // THE CONFECTION RIVER's water parameters (Floor A, Task 7): the raspberry
    // Gerstner surface that fills the river channel cut through the Floor-A
    // slab (sea level BELOW every walkable deck — the plane is global, so the
    // channel is sunken; from outside the annex it reads as the confection sea
    // the works are built over). The HOST applies it (device.setWaterParams is
    // host territory, the plan's T7 step 2): copy, advance .time, push per
    // frame. enabled=false until Floor A is built.
    x3::rhi::IRenderDevice::WaterParams riverWater() const { return m_river; }

    // Host atmosphere opt-in (called once after build()): dark-iron interior
    // ambient + IBL so the aubergine shell + emissive brass read (values live
    // with the art in factory_annex.cpp — one knob).
    void applyAtmosphere(x3::rhi::IRenderDevice& device) const;

    // ---- Queries ------------------------------------------------------------
    bool     built() const { return m_built; }
    uint32_t roomCount() const { return (uint32_t)m_rooms.size(); }
    const AnnexRoom& room(uint32_t i) const { return m_rooms[i]; }
    // The annex center (world XZ) + the shaft XZ it was built against.
    float annexX() const { return m_annexX; }
    float annexZ() const { return m_annexZ; }
    float shaftX() const { return m_shaftX; }
    float shaftZ() const { return m_shaftZ; }
    // Low-grav zone latched (Floor C trigger 311): the HOST applies the jump
    // modifier (x1.8) — the annex only reports the state.
    bool lowGravActive() const { return m_lowGrav; }
    // The Sorting-Hall chute (310) / burst-dais (312) / tube-ride (313) latches.
    bool chuteTripped() const { return m_chute; }
    bool burstDaisVisited() const { return m_burstDais; }
    bool tubeRideTripped() const { return m_tubeRide; }
    bool boreEntered() const { return m_boreEntered; }
    // Bookkeeping the self-test asserts (mirrors Rifthub's span reasoning).
    uint32_t meshCount() const { return (uint32_t)m_meshes.size(); }
    uint32_t entityFirst() const { return m_entFirst; }
    uint32_t entityCount() const { return m_entCount; }
    // Phase-5 hero-hook / dressing tallies (self-test F16): how many of the 17
    // load-if-present hero hooks (6 vats + 8 machine bodies + capsule + 2 sorter
    // arms) found their GLB vs fell back, and how many Glimvale dressing
    // entities were authored (0 on an asset-less clone — never a failure).
    uint32_t heroHooksPresent()  const { return m_art.heroPresent; }
    uint32_t heroHooksFallback() const { return m_art.heroFallback; }
    uint32_t dressEntityCount()  const { return m_art.dressEntities; }

    // The annex's own point-light rig (one warm accent light per floor + the
    // bore's brass pair; built once at build(), static thereafter). The host
    // MERGES these with the elevator's lights into ONE setPointLights push per
    // frame (two writers calling setPointLights would fight, the rifthub
    // lesson). Empty until build().
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

    // ---- The combined Spire+Annex elevator graph ----------------------------
    // ONE builder for the host, the capture rig and the self-test, so the
    // graph can never drift between them. Stops (in index order):
    //   [0] F1 (shaft, y=2)   [1] F3 (shaft, y=15 — the bore level)
    //   [2..6] A1..A5 (annexX, baseY {2,15,28,41,54}) — ALL hidden until 4790.
    // Rails: F1<->F3 vertical; F3<->A2 the LATERAL bore leg; A1<->A2<->A3<->
    // A4<->A5 the annex vertical chain. Cab-center stop Y = baseY - kCabHalfY
    // (cab TOP flush with the walking floor). Burst stop = A5 (index 6).
    struct ElevatorGraph {
        std::vector<ElevatorSystem::Stop>  stops;
        std::vector<std::pair<int, int>>   rails;
        std::vector<std::string>           labels;
        int f1 = 0, f3 = 1, a1 = 2, a2 = 3, a5 = 6;
    };
    static ElevatorGraph makeElevatorGraph(float shaftX, float shaftZ);

private:
    bool  m_built = false;
    float m_time  = 0.0f;
    float m_annexX = 0.0f, m_annexZ = 0.0f;
    float m_shaftX = 0.0f, m_shaftZ = 0.0f;

    std::vector<AnnexRoom> m_rooms;

    // Latches (onTrigger).
    bool m_lowGrav = false, m_chute = false, m_burstDais = false;
    bool m_tubeRide = false, m_boreEntered = false;

    // Owned render resources: EVERY device mesh authored by build() goes in
    // this ONE vector and is freed uniformly in shutdown() (shared handles —
    // e.g. the bore rib torus, instanced by all 13 ribs — are pushed ONCE).
    std::vector<x3::rhi::MeshHandle> m_meshes;
    SurfaceLibrary                   m_surf;
    // Phase-5 GLB services: loader + every Model kept alive for its device
    // meshes/textures. shutdown() unloads each Model (mesh buffers freed, the
    // loader's process-wide texture cache keeps its own refs — steady-state,
    // asserted by F15's double build/shutdown).
    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    std::vector<x3::asset::Model>            m_models;
    FactoryArtHooks                          m_art;

    // Animated shell spans (contiguous; tick() pokes emissive[3] in place).
    uint32_t m_ribGlowFirst  = 0, m_ribGlowCount  = 0;   // bore brass ribs
    uint32_t m_trimGlowFirst = 0, m_trimGlowCount = 0;   // glass-curtain brass trim
    uint32_t m_accentFirst   = 0, m_accentCount   = 0;   // per-floor accent coves

    // The annex's own point lights (see pointLights()).
    std::vector<x3::rhi::PointLight> m_lights;
    // The confection river's water params (filled by Floor A's builder via
    // setRiverWater — factory_rooms.cpp owns the art numbers).
    x3::rhi::IRenderDevice::WaterParams m_river;
public:
    // factory_rooms.cpp (Floor A) registers the river here. Not for hosts.
    void setRiverWater(const x3::rhi::IRenderDevice::WaterParams& wp) { m_river = wp; }
private:
    // Full annex entity span (self-test range checks).
    uint32_t m_entFirst = 0, m_entCount = 0;
};

// Headless self-test (--test-factory; the function LIVES in factory_annex.cpp).
// Builds the annex on a leak-counting HeadlessRenderDevice + Jolt world and
// asserts: the shell built (5 rooms, entity/mesh counts, spans in range); all
// 10 triggers (300-313) registered; the combined elevator graph reaches A5
// from F1 THROUGH the 4790 unlock (locked callTo is a no-op first); tick()
// animates the bore ribs; onTrigger latches visited/low-grav; and shutdown is
// clean (device create/destroy counts balance — the VMA-leak analogue a
// headless device can prove). Prints "factory: X/Y passed"; returns true iff
// all pass. No window/Vulkan.
bool runFactoryAnnexSelfTest();

} // namespace x3::game
