#pragma once
// ---------------------------------------------------------------------------
// THE DRIVE LAYER — the ONE stand-up path for AI traffic + the structural
// interchange, shared by every world that has a freeway.
//
// WHY THIS FILE EXISTS. Until this lane the whole drive layer was wired ONCE,
// inline, in app/world_hosts/host_tunnel.cpp — a CLI-only dev world. The game
// the owner actually plays (--world canonlevel) includes city.h / world_cars.h
// / perfshop.h and does NOT include traffic.h or interchange.h at all: the
// roads in the canon world are empty. Copy-pasting host_tunnel's ~60 lines of
// stand-up into app_run.cpp would have made TWO copies of an env gate, an
// avoid list, a contact-callback install and a teardown order — and this tree
// has six scars from exactly that. So the wiring moves HERE and BOTH hosts
// call it.
//
// WHAT IS SHARED (and what deliberately is not):
//   SHARED  the env gate + the whyNot logging + the avoid-list plumbing for
//           registerInterchange(); the interchange's whole geometry pass
//           (crossroad ribbon, deck, four ramp ribbons, eight junction
//           mouths); the traffic build + the ONE global contact hook + the
//           teardown ORDER (callback cleared BEFORE shutdown, kinematic boxes
//           out before the physics world).
//   NOT     the per-frame update/render/lights calls. Those are two lines each
//           and they sit inside host-specific frame code (host_tunnel has
//           three separate render paths; the canon host has two). Hiding them
//           behind a wrapper would buy nothing and cost a layer of indirection
//           in the hot loop.
//
// SITING IS PER WORLD, BY MEASUREMENT. host_tunnel sites the interchange
// against ITS freeway (the inner tour). The canon world has no such freeway:
// see surveyCityFreeway() below, which measures the CITY's own authored
// alignments and answers honestly.
// ---------------------------------------------------------------------------
#include "road_network.h"
#include "interchange.h"
#include "traffic.h"

#include <vector>

namespace x3::game {

class Scene;

// ---------------------------------------------------------------------------
// INTERCHANGE — the boot-slot registration.
//
// `worldLabel` is the prefix every log line gets ("--world tunnel", "--world
// canonlevel") so a failure names the world it failed in.
// `enabled` is the host's own precondition (host_tunnel: "the ring exists").
// X3_INTERCHANGE=0 turns it OFF — the flag is for disabling (NO_SLOP rule 6).
// Registers corridors and reads the carved field: call after every other road
// and BEFORE the first terrain height query / TerrainStreamer::init().
// ---------------------------------------------------------------------------
InterchangeResult standUpInterchange(const char* worldLabel, bool enabled,
                                     const RoadSpec& fwySpec,
                                     const std::vector<float>& fwyRoadY,
                                     const std::vector<const RoadSpec*>* avoid);

// The interchange's GEOMETRY: the crossroad's ribbon (its span reach is a gap
// the deck owns), the overpass deck, four ramp ribbons at half cross-section,
// and EIGHT junction mouths — every ramp blends into both roads with the same
// ruled twist + swooping fillets every at-grade branch gets. No-op when the
// interchange did not build. Call after the terrain streamer exists.
void buildInterchangeGeometry(const InterchangeResult& ic, Scene& scene,
                              x3::rhi::IRenderDevice& device,
                              x3::phys::IPhysicsWorld& phys);

// Every ramp mouth of a built interchange is OPEN — i.e. the ramp registered a
// junction on both the freeway and the crossroad, and the barrier planner
// therefore leaves the mouth unrailed. Pure query over the result; used by the
// gates so "the interchange is usable" is measured, not assumed.
bool interchangeRampMouthsOpen(const InterchangeResult& ic);

// ---------------------------------------------------------------------------
// TRAFFIC — the stand-up, the ONE global contact hook, and the teardown order.
//
// Lifetime note (why this is a class and the interchange is a function): the
// contact callback captures a POINTER to a context struct that must outlive
// every physics step, and getting that lifetime wrong is a use-after-free at
// world teardown rather than a compile error. Owning it here is the whole
// point of the extraction.
// ---------------------------------------------------------------------------
class TrafficLayer {
public:
    ~TrafficLayer();

    // `enabled`: the host's own precondition. X3_TRAFFIC=0 disables.
    // `installContactHook`: take IPhysicsWorld's ONE global contact callback so
    //   a hard hit converts the struck car to a dynamic wreck. FALSE when the
    //   host already owns that callback for something else — the canon world's
    //   BarrelSystem/DestructibleManager does (app/barrels.h), and clobbering
    //   it would silently kill barrel destruction. A false here costs the
    //   kinematic->dynamic wreck conversion and nothing else; it is logged.
    // Returns true iff traffic actually built.
    bool build(const char* worldLabel, bool enabled,
               const RoadSpec& fwySpec, const std::vector<float>& fwyRoadY,
               x3::rhi::IRenderDevice* device, x3::phys::IPhysicsWorld* phys,
               x3::audio::IAudioSystem* audio,
               std::string_view glbDir,
               const TrafficConfig& cfg = TrafficConfig{},
               bool installContactHook = true);

    // Clears the contact callback (if we installed it) BEFORE tearing the sim
    // down, then puts every kinematic box out of the physics world. Idempotent:
    // safe to call from a region-teardown hook AND again from the host's exit.
    void shutdown(x3::phys::IPhysicsWorld* phys);

    bool built() const { return m_traffic.built(); }
    uint32_t liveCount() const { return m_traffic.built() ? m_traffic.liveCount() : 0u; }
    FreewayTraffic&       traffic()       { return m_traffic; }
    const FreewayTraffic& traffic() const { return m_traffic; }

private:
    struct ContactCtx {
        FreewayTraffic*          t = nullptr;
        x3::phys::IPhysicsWorld* p = nullptr;
    };
    FreewayTraffic m_traffic;
    ContactCtx     m_ctx;
    bool           m_hookInstalled = false;
};

// ---------------------------------------------------------------------------
// THE CANON WORLD'S FREEWAY — a SURVEY, not an assumption.
//
// app/city.cpp authors the metropolis: three districts, a street grid, the
// Scrapyard<->District connector it calls "freeway", a coast spur, and four
// freeway TUNNELS boring toward the four mountain ranges. Every one of those
// is a visual box strip laid by City::addRoad/addRoadSegmented — 4 to 8 m
// wide, no median, no RoadSpec, no collision. The drive layer needs something
// else entirely: FreewayTraffic::build() refuses any spec that is not
// RoadSpec::dualCarriageway, and registerInterchange() additionally wants an
// OPEN median (>= 6 m half) plus ~430 m of near-straight route each side of
// the crossing. A dual carriageway is 2 x kFwyPavedHalfM of pavement plus the
// median: 46 m at the jersey minimum, up to ~114 m open-country.
//
// So the question "where does the canon world's freeway go?" is a MEASUREMENT
// against the city's own authored data, and this is that measurement. It
// walks the alignments the city itself declares — the four freeway-tunnel
// headings and the E-W district connector — and for each one scores:
//
//   * DISTRICT CLEARANCE: the dual span (plus its carve batter) must stay off
//     every district's built massing. Measured against the authored massing
//     radius per district (cityDistrictMassRadius), which --test-city C9 gates
//     against the actual roster.
//   * RESIDENCY: the whole run must lie inside the `city` region's residency
//     reach, or the freeway (and its traffic) evicts out from under a player
//     driving on it. Passed in by the caller from regions.canon.json.
//   * TERRAIN: the natural surface along the run, so a "site" is not a 60 m
//     cut through a mountain flank dressed up as a road.
//
// If nothing qualifies it says so, with the numbers. A floating or clipping
// interchange is worse than none.
// ---------------------------------------------------------------------------
struct CityFreewaySurvey {
    bool        ok     = false;
    const char* whyNot = "";
    // The winning alignment (valid only when ok).
    RoadSpec    spec;
    const char* alignmentName = "";
    float       x0 = 0.0f, z0 = 0.0f, x1 = 0.0f, z1 = 0.0f;
    float       lengthM = 0.0f;
    float       districtClearM = 0.0f;   // measured worst district clearance
    float       terrainReliefM = 0.0f;   // measured natural relief along the run
    float       reachM = 0.0f;           // run midpoint -> the player's start
    // Bookkeeping for the report, filled whether or not anything qualified.
    uint32_t    candidatesTried = 0;
    uint32_t    rejectedDistrict = 0, rejectedResidency = 0, rejectedTerrain = 0,
                rejectedKeepOut  = 0, rejectedLength    = 0;
    // The best REJECTED candidate's numbers, so a "no site" answer is specific.
    const char* nearestMissName  = "";
    float       nearestMissClearM = 0.0f;
    float       nearestMissLenM   = 0.0f;
};

// The survey queries the NATURAL height field, which is a pure function of the
// terrain config and is safe to call before any corridor is registered.
struct CityFreewaySurveyInput {
    // The `city` region's anchor and the distance from it at which the region
    // is still RESIDENT — WorldStreamer measures from the footprint edge, so
    // this is radius + unloadRadius (assets/world/regions.canon.json).
    float regionAnchorX = 0.0f, regionAnchorZ = 0.0f;
    float regionReachM  = 0.0f;
    // A rectangle the freeway must stay off, in world XZ. The canon world puts
    // the FACILITY at the origin and the survey has no other way to know: the
    // tower is not city content and nothing in city.h describes it. The canon
    // host fills this from FacilityExterior::builtDesc() — measured, not typed.
    bool  haveKeepOut = false;
    float keepOutX0 = 0.0f, keepOutZ0 = 0.0f, keepOutX1 = 0.0f, keepOutZ1 = 0.0f;
    // WHERE THE PLAYER STARTS. Two runs of identical length are not equally
    // good: content the player has to hike a kilometre to reach is content that
    // effectively does not exist. Among candidates that clear every hard gate,
    // the survey prefers the one whose midpoint is nearest here. The canon host
    // passes the facility centre — the door Jake walks out of.
    bool  haveReachFrom = false;
    float reachFromX = 0.0f, reachFromZ = 0.0f;
};
CityFreewaySurvey surveyCityFreeway(const CityFreewaySurveyInput& in);

// --test-drivelayer — headless. Gates:
//   D1 the extraction is real: standUpInterchange honours its enable gate and
//      reports whyNot rather than returning a half-built result.
//   D2 the city survey is a MEASUREMENT: it tries every authored alignment and
//      its verdict moves when the inputs move (a residency reach that admits
//      nothing rejects everything; the recorded near-miss names a real
//      alignment).
//   D3 traffic is REGION-SHAPED: TrafficLayer::build/shutdown round-trips to a
//      live count of exactly ZERO, with no physics bodies left behind, and is
//      idempotent under a double shutdown (a region can evict twice).
//   D4 a built interchange has every ramp mouth open.
//   D5 the CANON world's own freeway: the survey sites it, the road registers,
//      traffic drives on it, the interchange BUILDS with every ramp mouth open
//      (the city freeway's authored median floor meets the pier's 6 m), the
//      terrain-decided-median CONTROL still refuses with a measured peak under
//      6 m, and eviction leaves zero cars.
//   D6 the success path: standUpInterchange sites a real interchange on the
//      inner tour and every ramp mouth is open.
bool runDriveLayerSelfTest();

} // namespace x3::game
