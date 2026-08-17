#pragma once
// THE UNDERGROUND RIVER (W-UNDERRIVER) — the owner's sketch line from the NW
// lake down past the city ("we want an underground river... with rock
// beaches... movie grade.. rushing water" / "under the mountain.. That will
// be Amazinng").
//
// The TRENCH is terrain.cpp's business (authoredLandforms carves bed + rock
// beach shelves + walls from the derived worldUnderRiverChain() table — the
// same one-truth table worldWaterLevelAt answers from). This module adds the
// three things the heightfield cannot be:
//   * THE VAULT — a displaced rock ceiling (ring-stitched arch strips, the
//     mine_fx bore idiom) closing the trench from head grotto to the gorge,
//     inner face wet cave rock (cv_rock_wet), outer back dry country rock
//     (terrain_rock), so the hill reads shut from above and a CAVERN from
//     the beaches. The last kURGorgeLen metres before the plunge pool stay
//     OPEN — the river steps down into daylight there.
//   * THE WATER — the CaveRiver machinery (app/cave_river.h) pointed at the
//     open world: self-luminescent ribbon (there is no sun down here), pools
//     that breathe, and the new CaveRiverNode::rush whitewater at the drops.
//   * THE LIGHT — CaveRiver's pool bank lights plus sparse cool accents down
//     the run, appended to the host's point-light array.
//
// Reuse ledger (NO_SLOP rule 1): water = CaveRiver (extended, not forked);
// carve = the authored-landforms river pattern; vault = the mine_fx
// ring-stitch idiom with the surface library's published rock sets.

#include "scene.h"
#include "cave_river.h"
#include "surface_library.h"
#include "engine/rhi/IRenderDevice.h"

#include <vector>

namespace x3::game {

class UndergroundRiver {
public:
    struct Result {
        bool  built = false;
        int   vaultChunks = 0;     // rock vault entities
        int   waterSegs   = 0;     // CaveRiver ribbon segments
        int   lightCount  = 0;     // lights appended
        float portalX = 0.0f, portalZ = 0.0f;   // where the river surfaces
    };

    // Build vault + water + lights from worldUnderRiverChain(). `surf` may be
    // null (a local library is mounted). Lights are APPENDED to outLights.
    Result build(Scene& scene, x3::rhi::IRenderDevice& device,
                 SurfaceLibrary* surf,
                 std::vector<x3::rhi::PointLight>* outLights);

    // Per-frame flow (CaveRiver crest scroll + whitewater churn).
    void update(float dt, Scene& scene) { m_water.update(dt, scene); }

    bool built() const { return m_built; }

    // Headless gate: --test-underriver. Asserts the derived table descends,
    // the trench + beaches carved as authored, one water truth, the vault
    // closes the hill, and the gorge stays open. Prints the measured table.
    static bool runSelfTest();

private:
    CaveRiver m_water;
    bool      m_built = false;
};

} // namespace x3::game
