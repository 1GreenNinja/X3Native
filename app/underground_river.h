#pragma once
// THE UNDERGROUND RIVER (W-UNDERRIVER) — the owner: "we want an underground
// river... with rock beaches... movie grade.. rushing water" / "under the
// mountain.. That will be Amazinng".
//
// The TRENCH is terrain.cpp's business: authoredLandforms carves bed + rock
// beach shelves + walls from the derived worldUnderRiverChain() table, the
// SAME table worldWaterLevelAt answers from, so swimming, the CONTACT LAW,
// collision and streaming all work down here for free. terrain.h carries the
// mechanism note — what cut-and-cover can and cannot express, and why the
// route follows the west valley instead of the 250 m massif.
//
// This module adds the four things a height field cannot be:
//   * THE VAULT — the lid that puts the hillside BACK. Every vertex samples
//     worldPreUnderRiverHeight, so the lid restores the surface the trench
//     removed rather than arching over it; the void between carved floor and
//     restored surface IS the cavern, a lens tallest over the channel and
//     closed at the rim. Both faces wear the natural country rock
//     (terrain_rock) — the inner face used to be cv_rock_wet, a MASONRY
//     tile that read as brown ceiling panels. The last kURGorgeLen metres stay OPEN
//     — the river steps down into daylight there. Gate U9 measures the
//     headroom (10-44 m over the beaches).
//   * THE BEACHES — an apron of water-worn cobble (terrain_rock_grey) over the
//     carved shelf. The shelf itself is height field and stays the collision
//     surface; the apron exists because the terrain splat picks its material
//     from height + slope and would paint a flat lowland shelf GRASS, indoors.
//   * THE WATER — drawn NOT here but by host_tunnel's applyRiverWater, which
//     switches WaterParams' polyline to worldUnderRiverChain() whenever the
//     focus is in this corridor. The cavern channel therefore gets the very
//     same Gerstner surface, clarity, Fresnel, contact foam and caustics as
//     the surface river — ONE water implementation in the world, and the one
//     JOB 1 already made honest. (It was a CaveRiver ribbon first; in an 88 m
//     cavern that photographed as flat blue construction paper.)
//   * THE MIST — spray off the steps, cold breath on the pools.
//
// Reuse ledger (NO_SLOP rule 1): water = the engine's own water pass;
// carve = the authored-landforms river pattern; mist = RiverLife's wake-puff
// system through the same submitParticles pass; rock = the surface library's
// published sets, checked for real bytes before use.

#include "scene.h"
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
        int   waterSegs   = 0;     // chain nodes handed to the water pass
        int   lightCount  = 0;     // lights appended
        int   mistSources = 0;     // rush/pool emitters
        float portalX = 0.0f, portalZ = 0.0f;   // where the river surfaces
    };

    // Build vault + beaches + water + mist + lights from
    // worldUnderRiverChain(). `surf` may be null (a local library is mounted).
    // outLights is unused: the cavern's accents are delivered per-frame,
    // nearest-K, instead (see nearestLights).
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
    // Is this point UNDER THE LID — in the corridor AND upstream of the open
    // gorge (the last kURGorgeLen metres have no vault and see the sky)? The
    // atmosphere below is gated on this, not on the corridor box, so a camera
    // in the gorge looking out at daylight keeps daylight.
    static bool underVault(const float p[3]);
    // THE CAVERN'S AIR. The canon world's zone recipe hands every point that
    // is not inside the facility the EXTERIOR air: a full daylight sky IBL
    // and a sky-fill ambient. Under the lid that is wrong in kind, not in
    // degree — the vault is lit by a sky it cannot see, so its rock reads a
    // flat sky-lit brown from wall to wall, the 44 bank lamps vanish into
    // that wash, and there is nothing dark for the water to be dark against
    // (the owner's target: a DARK cave, water reflecting the bank lights).
    // This applies what a cave actually has: a near-black cool ambient, the
    // sky IBL wound down to a trace (some light does come down the run from
    // the gorge and the head), and a thin dark haze so the far hall dissolves
    // into black instead of into a brightly lit back wall 300 m away. Same
    // contract as Rifthub::applyAtmosphere — call it every frame the eye is
    // underVault(), AFTER applyZoneAtmosphere, and call
    // RoomDressing::resetZoneAtmosphere() on the frame the eye leaves so the
    // zone recipe re-asserts the exterior air.
    void applyAtmosphere(x3::rhi::IRenderDevice& device) const;
    // THE HEADROOM MEASURE (gate U9's body, ONE producer — NO_SLOP rule 4):
    // walks the vaulted reach and measures the lowest the rough ceiling can
    // hang (vaultCeilingY, no jitter luck) against the built ground, across the
    // channel and both beaches. minHead = tightest anywhere under the lid;
    // minBeachHead = tightest over standable ground (channel + beaches);
    // probes = samples taken. --test-canonunderriver re-runs it on the canon
    // field, which is the same field — the number is what proves that.
    struct Headroom { float minHead = 1e9f, minBeachHead = 1e9f, maxHead = 0.0f;
                      float atX = 0.0f, atZ = 0.0f; int probes = 0; float vaultLen = 0.0f; };
    static Headroom measureHeadroom();

    // Headless gate: --test-underriver, 9 checks. The river descends (U1) under
    // ground it never breaks (U2), the bed is under the water and the beaches
    // are dry and walkable (U3), the query and the drawn table are one truth
    // (U4), it rushes at the steps and stills at the pools (U5), the table is
    // deterministic (U6), the route stays inside what cut-and-cover can build
    // (U7), the whole corridor is allowed to be dug at all (U8), and there is
    // a cavern in there you can stand up in (U9). Prints the measured table;
    // X3_UR_SCAN[=x0,x1,z0,z1,step] prints the pre-UR ground the route was
    // picked off.
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
    std::vector<x3::rhi::PointLight> m_lights;   // the whole run's accents
    // Irradiance the bank lamps deliver at a point in the air (isotropic —
    // a droplet has no normal). What the mist is lit BY, so it catches the
    // lamps and goes dark between them instead of glowing on its own.
    void roomIrradianceAt(float x, float y, float z, float out[3]) const;
    std::vector<MistSource> m_mist;
    std::vector<Puff>       m_puffs;    // fixed pool, round-robin reuse
    uint32_t  m_puffNext = 0;
    std::vector<x3::rhi::IRenderDevice::ParticleInstance> m_hazeOut, m_sprayOut;
    uint32_t  m_seed = 12345u;
    bool      m_built = false;
};

} // namespace x3::game
