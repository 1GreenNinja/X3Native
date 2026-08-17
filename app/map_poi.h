#pragma once
// MapPoi — the shared point-of-interest contract for the OPEN-WORLD ROAD MAP
// (Lane 7 / task #22, W-MAP). Lanes 4 (W-TOWN), 5 (W-STATIONS) and 6
// (W-FACTORY) each build a landmark somewhere in the driving world; this
// header is the tiny, dependency-free thing all four lanes agree on so a
// landmark can register itself the moment it exists and Lane 7's map draws
// it — no merge-order coupling beyond "this header exists".
//
// DELIBERATELY NOT x3::game::MapPoi (world_map.h): that struct is the
// heavier Spire-dungeon POI table — JSON-loaded (assets/world/map_pois.json),
// StoryFlags discovery-gated, floor-aware. Reusing its name in the same
// namespace would be an ODR collision the moment a road-world host includes
// both headers (host_tunnel.cpp already does, for the tunnel-mouth/LNSS
// MapMarker overlay). This contract lives in its own namespace instead:
// same struct NAME the plan asked for (`MapPoi`), zero risk of clashing.
//
// NO DEPENDENCIES: no Scene, no StoryFlags, no device, no json — just a
// name, a world XZ position and an icon class, held in a static registry any
// translation unit can push into at static-init or boot time and any host
// can read back once the world is built. Always-visible (a road world has no
// fog-of-war), same spirit as world_map.h's MapMarker.
//
// CONSUMERS (register): app/town.*, app/gas_station.*, app/factory.* — one
// registerMapPoi() call per landmark, at world-build time.
// CONSUMER (draw): app/world_hosts/host_tunnel.cpp (map v3) — draws every
// registered POI as an icon+name on the full map and an edge-clamped arrow
// on the minimap.
#include <string>
#include <vector>

namespace x3::worldpoi {

struct MapPoi {
    std::string name;
    float x = 0.0f, z = 0.0f;   // world XZ, metres (Y is derived from terrain at draw time)
    enum Icon { Town, Fuel, Factory, Shop, Parking, Bridge } icon = Town;
};

// Register one POI. Safe to call at any time before the map is first drawn —
// no init-order dependency on any other system (static vector, no ctor args).
void registerMapPoi(const MapPoi& poi);
inline void registerMapPoi(std::string name, float x, float z, MapPoi::Icon icon) {
    registerMapPoi(MapPoi{std::move(name), x, z, icon});
}

// Every POI registered so far, in registration order. Stable references are
// NOT guaranteed across further registerMapPoi() calls (std::vector may
// reallocate) — copy out what you need, don't hold a pointer across a call.
const std::vector<MapPoi>& allMapPois();

// Test/hot-reload seam: drop everything (a fresh boot re-registers).
void clearMapPois();

} // namespace x3::worldpoi
