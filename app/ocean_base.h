#pragma once
// EFLZ Act-2 open world — the OCEAN + the undersea BASE + submarine combat.
// Game/slice content only — engine/ stays pure.
//
// CLEAN-ROOM, original work. Built ONLY from X3Native's OWN Scene / mesh_prims
// systems + the engine interfaces + the EFLZ design (Tim's own Q3Engine
// x3-world-ocean.js / x3-seafloor-base.js / x3-sub-docking-bay.js /
// x3-submarine-combat.js as the content reference). NO RBDOOM / id Tech / Doom /
// Quake engine source consulted.
//
// SCOPE (open-world lane): the offshore deep-water zone — an ocean surface plane,
// the 3-level undersea disc base (sub-dock + airlock + reactor) on the seafloor, and
// a submarine-combat model that is PRESENT but INERT until engaged. Graybox at
// absolute deep Y (the undersea zone is reached by submarine, decoupled from the
// land terrain). Mirrors act2_world.* / world_regions.* authoring (build / queries /
// a headless --test-oceanbase). Base position from the blueprint gazetteer §1:
// (1100, -346, -1350); here laid at native graybox depth offshore.

#include "scene.h"
#include "surface_library.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// The undersea base (a 3-level disc on the seafloor) + ocean depth profile.
struct OceanBasePlan {
    float    cx = 0.0f, cz = 0.0f;   // base center XZ (offshore)
    float    surfaceY  = 0.0f;       // ocean surface Y
    float    baseDeckY = 0.0f;       // base top-deck Y (deep)
    float    seafloorY = 0.0f;       // seafloor Y (below the base)
    float    radius    = 0.0f;       // disc radius (m)
    uint32_t levels    = 0;          // disc levels (3)
    bool     hasSubDock = false;
    bool     hasAirlock = false;
    bool     hasReactor = false;
};

// Submarine-combat model — PRESENT but INERT until engage() (gated, never at load).
struct SubCombat {
    int      playerHull        = 200;   // standard hull (heavy variant = 400)
    int      playerHullMax      = 200;
    uint32_t enemySubs          = 0;    // hostile subs present
    int      torpedoDamage      = 40;
    int      depthChargeDamage  = 30;
    float    hullRegenPerSec    = 2.0f; // after a grace period
    bool     engaged            = false; // false until combat starts
};

// Offshore undersea zone host. Build once; graybox props are Scene entities (drawn by
// the host's scene.render()). The undersea zone uses absolute deep Y (not the land
// terrain surface) — it is reached by submarine.
class OceanBase {
public:
    void build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               SurfaceLibrary* sharedSurf = nullptr);   // W8-3: streamer-shared PBR cache

    // Begin submarine combat (gated — the host calls this on engagement, NOT at load).
    void engage() { m_combat.engaged = true; }

    // ---- Queries (host HUD + self-test) ----
    bool built() const { return m_built; }
    const OceanBasePlan& plan() const { return m_plan; }
    const SubCombat&     combat() const { return m_combat; }
    uint32_t enemySubCount() const { return m_combat.enemySubs; }
    bool playerSubPresent() const { return m_playerSub; }
    uint32_t propCount() const { return (uint32_t)m_props.size(); }

private:
    bool m_built = false;
    OceanBasePlan m_plan{};
    SubCombat     m_combat{};
    bool          m_playerSub = false;
    std::vector<uint32_t> m_props;   // Scene entity ids
};

// Headless self-test (--test-oceanbase). Builds the undersea zone on a HeadlessDevice
// + Jolt world and asserts: the base sits offshore with 3 levels + a sub-dock/airlock/
// reactor, the depth profile is ordered (surface > base deck > seafloor); enemy subs +
// a player sub are placed; the submarine-combat model is INERT at load (engaged==false,
// full hull) and arms via engage(); graybox props placed. Prints "oceanbase: X/Y
// passed"; returns true iff all pass. No window/Vulkan.
bool runOceanBaseSelfTest();

} // namespace x3::game
