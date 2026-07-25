#pragma once
// ============================================================================
// SURVIVAL COMPLEX — the 7-LEVEL underground survival structure WEST of Club
// 1127 ("Danny's second Haven" / "the ark"). Stage-2 of the Club 1127 canon
// build. Benign game content, ported from Tim's Sovereign Rising novels.
//
// SOURCE OF TRUTH: docs/design/CLUB_1127_CANON_SPEC.md §4 (the Complex
// topology, two-ended dungeon, level list) + the book reveal scripts
// (D:/Writing/fix_ch1_complex.py, fix_complex_seven_levels.py).
//
// This module builds ONLY the underground Complex — it does NOT touch the club
// room interior or the Lair (owned elsewhere). It attaches to the Stage-1 club:
//   * LEVEL 1 (Private Lounge / Situation Room) is ALREADY built in
//     club1127.cpp (the green-marked stub at the L1 hallway south end). This
//     module CONNECTS to it — it does not rebuild L1.
//   * LEVELS 2-7 (this module) descend beneath L1, ~100 ft down to L7:
//       L2  Recreation           (HARD canon — couches/screens/workstations/
//                                 gaming table/bookshelves)
//       L3  Medical + Security   [INVENT, on-theme]
//       L4  Deep Storage / Armory[INVENT, on-theme]
//       L5  Water + Air / Life-support plant  [INVENT, on-theme]
//       L6  Power / Generators + Workshop     [INVENT, on-theme]
//       L7  Hydroponics bay      (HARD canon — "green growing things a hundred
//                                 feet underground"; the hero level, lush)
//   * STAIRWELL — the full-height walkable spine connecting all 7 levels
//     (switchback flights L1<->L7), plus Danny's internal 4-person elevator
//     (steel car + a button panel with one stop per level) as a shortcut.
//   * ROUTE A connection: the top of the stairwell reaches the L1 hatch (the
//     secret club walk arrives at L1 top — Stage 1 already built that tunnel).
//   * ROUTE B: the ELEVATOR route — a 130-ft hall running WEST UNDER the club
//     (~30 ft down) that arrives at L7 (bottom) + a marked hook where the
//     club's freight elevator would deliver the player into it.
//
// The Complex is a SEPARATE space from the club (deep west/below), so it
// manages its OWN point-light budget (reported by Stats::pointLights).
//
// Construction mirrors club1127.cpp exactly: geometry = x3::prims::makeBox ->
// device->createMesh (render) + physics->addStaticMesh (collision), registered
// as Scene entities (Tag::Static). Lights = a vector<PointLight> the host
// applies via setPointLights. Lighting gets progressively more utilitarian /
// industrial as you descend; L7 hydroponics has a grow-light magenta/white glow.
// ============================================================================

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

class SurvivalComplex {
public:
    // World Y of the club main floor — MUST match Club1127World::kClubY (-200).
    static constexpr float kClubY   = -200.0f;
    // L1 (Private Lounge) floor, club-local Y — matches club1127.cpp LOUNGE_Y.
    static constexpr float kLoungeY = 4.57f;
    // Vertical pitch between levels (m). 6 drops L1->L7 = 28.8 m ~ 94.5 ft
    // ("a hundred feet underground", spec §4.2). Interior clear height per
    // level is kRoomH; the remainder (kLevelH-kRoomH) is inter-floor structure.
    static constexpr float kLevelH  = 4.80f;
    static constexpr float kRoomH   = 3.20f;   // interior clear ceiling per level
    static constexpr int   kLevels  = 7;

    // Club footprint anchors (must match club1127.cpp kCW/kCL): HL = 100 ft/2 X,
    // HW = 43 ft/2 Z. Used to place the Complex west of the club + the Route-B
    // hall under it.
    static constexpr float kHL = 15.24f;    // club half-length (X, E-W)
    static constexpr float kHW = 6.553f;    // club half-width  (Z, N-S)

    struct Stats {
        int   entities     = 0;   // total Scene entities authored by the Complex
        int   levelsBuilt  = 0;   // L2..L7 rooms this module builds (target 6)
        // Per-level (index 1..7) club-local floor Y + presence flags.
        float levelFloorY[8]    = { 0 };
        bool  levelHasRoom[8]   = { false };
        bool  levelHasDoorway[8]= { false };   // room -> stairwell doorway
        int   stairSteps   = 0;
        float stairMinY    = 0.0f;   // world Y of the lowest stair step (~L7)
        float stairMaxY    = 0.0f;   // world Y of the highest stair step (~L1)
        bool  hasElevator     = false;
        int   elevatorButtons = 0;   // one per level (target 7)
        bool  hasHydroRacks   = false;  // L7 grow racks
        int   growLights      = 0;      // L7 grow-light fixtures
        int   workstations    = 0;      // L2 salvaged-computer workstations
        bool  hasGamingTable  = false;  // L2
        bool  hasBookshelves  = false;  // L2 east wall
        bool  hasMedicalBay   = false;  // L3
        bool  hasArmory       = false;  // L4
        bool  hasLifeSupport  = false;  // L5
        bool  hasGenerators   = false;  // L6
        bool  hasRouteBHall   = false;
        float hallEastX = 0.0f, hallWestX = 0.0f;   // Route-B hall X extent
        float hallY     = 0.0f;                      // world Y of the hall floor
        bool  hasRouteAConnect = false;  // stairwell top reaches the L1 hatch
        int   npcMarkers  = 0;           // Amara/Emma/Danny spawn markers
        int   pointLights = 0;
        // Footprint bounds (world X/Z) of the stacked levels.
        float minX = 0.0f, maxX = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    };

    // Build the whole Complex (L2-L7 + stairwell + elevator + Route-B hall +
    // NPC markers) into `scene`/`physics` via `device`. Anchored to the club at
    // world Y = kClubY. Call once; idempotent (a second call returns cached Stats).
    const Stats& build(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, std::string_view modelDir);

    // Advance the Complex one frame: pulse the hydroponics grow-lights + the
    // steady bunker practicals breathe faintly. Cheap; safe to skip.
    void update(float dt, Scene& scene, x3::rhi::IRenderDevice& device);

    // Player spawn (feet) — on L2 (the first level down from L1), the room the
    // player reaches first via the descent. Host poses the camera here.
    x3::phys::Vec3 spawn() const { return m_spawn; }

    // A fixed showcase camera (x,y,z,yaw,pitch) for a given level 1..7 (and the
    // stairwell = 0, the Route-B hall = 8). Fills 5 floats.
    void showcaseCamera(int level, float out[5]) const;

    // The Complex point-light set the host applies via setPointLights. Static
    // (set once); update() re-pushes the animated grow-light pulse.
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

    const Stats& stats() const { return m_stats; }
    bool built() const { return m_built; }

private:
    uint32_t addBox(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics,
                    float cx, float cy, float cz, float hx, float hy, float hz,
                    const float color[4], const float emissive[4], bool collide);

    bool                             m_built = false;
    Stats                            m_stats{};
    x3::phys::Vec3                   m_spawn{};
    std::vector<x3::rhi::PointLight> m_lights;
    // Grow-light entities (L7) + their companion cast-light indices; update()
    // breathes them so the hydroponics bay reads alive.
    std::vector<uint32_t>            m_growEnts;
    std::vector<size_t>              m_growLightIdx;
    float                            m_time = 0.0f;
    // Per-level showcase cameras (index 0=stairwell, 1..7 levels, 8=Route-B hall).
    float                            m_shotCam[9][5] = { { 0 } };
};

// Headless self-test for `--test-complex`: build the Complex with a stub
// render/physics device (no window/Vulkan), assert all 7 levels stand up, the
// stairwell spans top-to-bottom (connects L1..L7), every level has a doorway to
// the spine, both entrances (Route A top + Route B hall) reach the structure,
// the 4-person elevator has one stop per level, L7 is a hydroponics bay with
// grow-lights, NPC markers are placed, the point-light budget is respected, and
// it is leak-clean + idempotent. Logs "complex: X/Y passed"; returns true iff all pass.
bool runComplexSelfTest();

} // namespace x3::game
