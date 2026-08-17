#pragma once
// ============================================================================
// GAS STATIONS — "places for cars to go, to fuel up" (W-STATIONS, Lane 5).
//
// Three service stations on the road network, each a real forecourt you can
// drive onto: a poured CONCRETE APRON with collision, connected to the
// pavement by a driveway that laps the road's own cement apron, carrying a
// textured canopy / pump islands / kiosk / price totem mined from the
// licensed "Mega Open World City Pack (Mobile-Optimized for Driving
// Simulation Games)" (see tools/w5_build_station_glb.py for the conversion
// receipt and why the armory's ready GLB of the same model could not be used
// as-is).
//
// THE THREE PHASES, and why the order is not negotiable
// -----------------------------------------------------
//   1. plan()        — PURE-ish siting. Reads the natural height field and the
//                      registered routes; picks each forecourt by MEASUREMENT
//                      (the flattest candidate wins), never by a hand-typed
//                      coordinate that the world can quietly grow away from.
//                      NO_SLOP rule 9: every site carries its measured cut/fill.
//   2. registerPads()— carves the forecourt pads as TerrainCorridors and NOTES
//                      each driveway mouth as a road junction. app/terrain.h's
//                      contract: corridors register at BOOT, BEFORE the first
//                      height query / TerrainStreamer::init(). The junction note
//                      must also precede buildRoadRibbon(), because that is
//                      where planRoadBarriers() runs — an unnoted mouth gets a
//                      continuous jersey barrier laid straight across it and the
//                      station is walled shut (road_network.h, JUNCTION
//                      EXCLUSION ZONES).
//   3. build()       — apron + driveway meshes with collision (the car drives ON
//                      them), plus the structure GLB instances. Must run after
//                      the streamer exists and BEFORE phys->optimizeBroadphase().
//
// CONTACT / MATERIAL LAW: the apron top rides `datum + kPaveProud` — the SAME
// 2 cm the road ribbon uses — so forecourt and pavement are one continuous
// surface with no step (NO_SLOP rule 4: that constant is named at both sites).
// The slab carries a concrete SKIRT down to below the carved ground on every
// closed edge, because a bare surface ribbon reads as paper wherever the ground
// falls away ("THICK CONCRETE in the base and aprons.. not floating on top!!!").
//
// THE CANOPY IS A COLLIDING STATIC MESH ON PURPOSE. host_tunnel.cpp's
// skyVisibleAt() casts one ray straight up against Layer::Static to decide
// whether the camera is under cover; its own comment says it "will do the same
// job under a bridge, an overpass or a gas-station canopy the day those exist,
// with no new code." Colliding the canopy is what buys rain suppression and the
// covered-space reverb for free.
//
// FUEL is a STUB, deliberately: the tank fills, the gauge reads, and
// consumption is OFF by default behind `fuel_on`, so the campaign can arm it
// later without touching this file. See FuelTank below.
// ============================================================================

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x3::con { class IConsole; }

namespace x3::game {

class Scene;
class EnvArtSystem;
struct RoadSpec;

// ---------------------------------------------------------------------------
// THE STRUCTURE, measured (tools/_w5_parts.py over the converted GLB, engine
// metres, origin at the forecourt's ground-level centre, local +X = the OPEN
// frontage that faces the road).
//
//   perimeter wall   X -21.4..21.4   Z -23.4..23.4  (+ flare wings to Z +-32.4);
//                    the whole +X edge is open — that is the frontage
//   canopy roof      X -10.9.. 7.6   Y 5.72..6.56   Z  -5.1.. 4.5
//   3 pump islands   X  -8.3 / -0.7 / 5.9,  4.0 m long in Z, kerb 0.3 m
//   kiosk building   X -20.7..-15.2  Y 0..4.14      Z  -6.2.. 5.5
//   price totem      X  17.3.. 20.9  Y 2.0..8.2     Z  -0.7.. 0.1
// ---------------------------------------------------------------------------
constexpr float kStationHalfX   = 21.6f;   // lot half-extent across the frontage axis
constexpr float kStationHalfZ   = 32.6f;   // lot half-extent along the frontage
constexpr float kCanopyMinX     = -11.2f;  // refuel zone, station-local
constexpr float kCanopyMaxX     = 7.9f;
constexpr float kCanopyHalfZ    = 5.4f;
constexpr float kStationClearM  = 14.0f;   // frontage stand-off from the road's paved edge
constexpr float kDriveHalfZ     = 13.0f;   // driveway throat half-width

// One sited station. Local +X points AT the road; the transform that places the
// structure is  col0 = (dirX,0,dirZ), col1 = +Y, col2 = (-dirZ,0,dirX).
struct GasStationSite {
    std::string name;
    bool        ok    = false;
    const char* whyNot = "";
    float x = 0.0f, z = 0.0f;      // forecourt origin, world XZ
    float dirX = 1.0f, dirZ = 0.0f;// unit vector, local +X -> world (toward the road)
    float padY = 0.0f;             // forecourt DATUM (apron top = padY + kPaveProud)
    float roadX = 0.0f, roadZ = 0.0f;  // the point on the host road's centreline
    float roadEdgeM = 0.0f;        // that road's outer paved-edge offset there
    float driveLenM = 0.0f;        // frontage -> pavement lap, along local +X
    float cutM = 0.0f, fillM = 0.0f;   // MEASURED worst cut / fill over the footprint
    const char* host = "";         // which route it sits on
};

// ---------------------------------------------------------------------------
// FUEL — the stub, structured so the campaign can switch consumption on.
//
// `armed` is the HUD gate the lane spec asks for: the gauge is INVISIBLE until
// the player has actually used a pump once (or until `fuel_on 1` turns the
// mechanic on), so a feature that does nothing yet does not clutter the screen.
// Consumption is off by default — `fuel_on 1` starts the burn, and everything
// the burn needs (rate, capacity) is a cvar, so arming it later is a config
// change, not a code change.
// ---------------------------------------------------------------------------
struct FuelTank {
    float capacityL   = 68.0f;    // fuel_cap
    float litres      = 68.0f;    // live
    bool  armed       = false;    // HUD gauge visible
    bool  consume     = false;    // fuel_on
    float burnLPer100 = 14.5f;    // fuel_burn — a 590 ft-lb V8 driven hard
    float refuelLPerS = 22.0f;    // fuel_rate — ~3 s per 10 % of the tank
    float pumpedL     = 0.0f;     // this visit, for the console receipt
    float frac() const { return capacityL > 0.0f ? litres / capacityL : 0.0f; }
};

struct GasStationBuildResult {
    bool     ok = false;
    uint32_t stations   = 0;   // forecourts built
    uint32_t meshCount  = 0;   // apron/driveway/skirt meshes
    uint32_t triCount   = 0;
    uint32_t structures = 0;   // structure GLB instances placed
    uint32_t colliderTris = 0; // triangles handed to the physics world
};

// ---------------------------------------------------------------------------
// The world's gas stations, as one object. Owns the sites, the apron geometry,
// the structure overlay and the fuel state.
// ---------------------------------------------------------------------------
class GasStationWorld {
public:
    GasStationWorld();
    // Out-of-line: m_art is a unique_ptr to a type this header only forward-
    // declares, and an implicit destructor would need it complete at every
    // include site (the host would have to pull in env_art.h to own one).
    ~GasStationWorld();

    // ---- PHASE 1 — siting, by measurement -------------------------------
    // Each (spec, roadY) pair may be null: a station whose host route is absent
    // is simply not planned (whyNot says so) — the world never breaks because
    // one route was disabled.
    //   ring  : the inner tour (dual carriageway). Freeway station — sited at
    //           the turnaround crossover whose surroundings measure flattest.
    //   river : the valley road east of the ring. TOWN APPROACH station — the
    //           Small Mountain Town is Lane 4's build zone, so this one stands
    //           on the approach road, off the town itself.
    //   conn  : the spawn connector. COUNTRY CROSSROADS station — sited at the
    //           range-circuit access junction if one is noted, else mid-route.
    uint32_t plan(const RoadSpec* ring,  const std::vector<float>* ringY,
                  const RoadSpec* river, const std::vector<float>* riverY,
                  const RoadSpec* conn,  const std::vector<float>* connY);

    // ---- PHASE 2 — carve + note. BOOT ONLY, before the first height query
    // and before buildRoadRibbon(). Returns the corridors registered.
    uint32_t registerPads();

    // ---- PHASE 3 — geometry, collision and art. After the streamer exists,
    // before phys.optimizeBroadphase().
    GasStationBuildResult build(Scene& scene, x3::rhi::IRenderDevice& device,
                                x3::phys::IPhysicsWorld& phys);

    // The structure GLBs are an EnvArt overlay (visual), so they need a draw
    // call alongside scene.render(). No-op when nothing loaded.
    uint32_t draw(x3::rhi::IRenderDevice& device,
                  const x3::rhi::FrameContext& frame) const;
    void     shutdown(x3::rhi::IRenderDevice& device);

    // ---- PER-FRAME ------------------------------------------------------
    // `distanceM` is the ground distance the car covered this tick (0 on foot);
    // `load01` is how hard it is working, 0..1 (the host passes |speed| against
    // a reference speed — a coasting car must not drink like one at full
    // throttle); `eHeld` is the raw held E. Returns true while fuel flows.
    bool update(float dt, float carX, float carZ, float distanceM,
                float load01, bool eHeld);

    // nullptr when there is nothing to say. Otherwise the HUD line, in the
    // host's existing two-space style ("E  GET OUT" / "E  REFUEL").
    const char* prompt() const { return m_prompt; }

    // Index of the forecourt the car is standing under, -1 if none.
    int  atStation() const { return m_at; }
    bool refuelling() const { return m_flowing; }

    FuelTank&       fuel()       { return m_fuel; }
    const FuelTank& fuel() const { return m_fuel; }

    const std::vector<GasStationSite>& sites() const { return m_sites; }

    // Keep-outs for the scatter passes (trees, forests): one disc per station
    // covering the lot + driveway. A tree through the canopy is the whole point
    // of asking. { x, z, radius }.
    void keepOutDiscs(std::vector<float>& outXZR) const;

    // Console: the LIVE commands (they need this object's state). The pure-data
    // cvars live in registerFuelCVars() below — one owner per value.
    void registerConsole(x3::con::IConsole& console);
    // Re-read fuel_on / fuel_burn / fuel_cap / fuel_rate from the console. Cheap;
    // call once a frame so a console edit lands without a restart.
    void syncCVars(const x3::con::IConsole& console);

private:
    std::vector<GasStationSite> m_sites;
    std::unique_ptr<EnvArtSystem> m_art;
    FuelTank    m_fuel;
    const char* m_prompt  = nullptr;
    int         m_at      = -1;
    bool        m_flowing = false;
    bool        m_built   = false;
    uint32_t    m_pads    = 0;
};

// ---------------------------------------------------------------------------
// THE PUMP'S HUD, in one place.
//
// Both of these used to be candidates for inline host code, and both are here
// instead for the same reason: the headless PROOF CAPTURE and the interactive
// loop must draw the SAME pixels from the SAME code, or a screenshot showing
// "E  REFUEL" is evidence of nothing but a screenshot (NO_SLOP rule 1 / rule 2).
//
// drawPumpPrompt positions itself from hudSize alone — the host's existing
// "E  GET OUT" line, same place, same two-space style, shadow then face.
// drawFuelBar is anchored on the GAUGE CLUSTER, so the host passes its tach
// centre/radius in rather than this module re-deriving a layout it does not own.
// ---------------------------------------------------------------------------
void drawPumpPrompt(x3::rhi::IRenderDevice& device,
                    const x3::rhi::FrameContext& frame, const char* text);
void drawFuelBar(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                 const FuelTank& tank, bool flowing, float R, float gcx, float gcy);

// The PURE-DATA fuel cvars, registered into the ONE shared console registry
// (app/engine_console.h) the same way registerLodCVars does — so `fuel_on`
// exists and is discoverable in every host, not just the one that drives.
// THE ARG CONVENTION (engine/core/IConsole.h) applies to the commands in
// GasStationWorld::registerConsole: the handler's first argument is args[0].
void registerFuelCVars(x3::con::IConsole& console);

// --test-gasstation — headless: plan against the live road network, assert the
// sites are on real ground, off the pavement, reachable from it, and that the
// pad carve actually flattens the footprint. No device, no pixels.
bool runGasStationSelfTest();

} // namespace x3::game
