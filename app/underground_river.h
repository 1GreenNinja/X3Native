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
        int   beachChunks = 0;     // rock-beach apron entities
        int   waterSegs   = 0;     // CaveRiver ribbon segments
        int   lightCount  = 0;     // lights appended
        int   mistSources = 0;     // rush/pool emitters
        float portalX = 0.0f, portalZ = 0.0f;   // where the river surfaces
    };

    // Build vault + water + lights from worldUnderRiverChain(). `surf` may be
    // null (a local library is mounted). Lights are APPENDED to outLights.
    Result build(Scene& scene, x3::rhi::IRenderDevice& device,
                 SurfaceLibrary* surf,
                 std::vector<x3::rhi::PointLight>* outLights);

    // Per-frame flow (CaveRiver crest scroll + whitewater churn) and the mist
    // simulation. Safe to call when nothing was built.
    void update(float dt, Scene& scene);

    // THE MIST. Spray off the whitewater steps and a cold breath lying on the
    // pools, as billboards through IRenderDevice::submitParticles — the same
    // pass and the same alpha/additive contract RiverLife's wake foam uses
    // (NO_SLOP rule 1: this is that system aimed underground, not a new one).
    // Call once per frame from the host's render phase.
    void render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame);

    bool built() const { return m_built; }

    // THE CAVERN LIGHTS, delivered NEAREST-FIRST rather than as a boot-time
    // block. host_tunnel re-uploads ONE merged point-light array every frame
    // (uploadTunnelLights) — a boot-time setPointLights is overwritten by the
    // next frame, which is exactly how the town's lamps went dark once
    // already. So the run carries its own dense set and hands the host only
    // the few that are near the camera. Returns the number written.
    uint32_t nearestLights(const float cam[3], x3::rhi::PointLight* out,
                           uint32_t maxN) const;
    // Is this point inside the river's corridor (the derived carve box)? Used
    // to decide whether a capture needs the cavern lane at all.
    static bool insideCorridor(const float p[3]);

    // Headless gate: --test-underriver. Asserts the derived table descends,
    // the trench + beaches carved as authored, one water truth, the vault
    // closes the hill, and the gorge stays open. Prints the measured table.
    static bool runSelfTest();

private:
    struct MistSource {                 // one emitter on the run
        float x = 0, y = 0, z = 0;
        float dx = 0, dz = 0;           // downstream unit direction
        float rush = 0;                 // 0 = pool breath, 1 = whitewater spray
        float acc = 0;                  // spawn accumulator
    };
    struct Puff {
        float x = 0, y = 0, z = 0;
        float vx = 0, vy = 0, vz = 0;
        float age = 0, life = 1, size0 = 1.0f;
        bool  spray = false;            // additive droplet vs alpha haze
    };
    CaveRiver m_water;
    std::vector<x3::rhi::PointLight> m_lights;   // the whole run's accents
    std::vector<MistSource> m_mist;
    std::vector<Puff>       m_puffs;    // fixed pool, round-robin reuse
    uint32_t  m_puffNext = 0;
    std::vector<x3::rhi::IRenderDevice::ParticleInstance> m_hazeOut, m_sprayOut;
    uint32_t  m_seed = 12345u;
    bool      m_built = false;
};

} // namespace x3::game
