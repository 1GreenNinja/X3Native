#pragma once
// app/world_hosts/echo_region_builders.h — WP-2 (TIER2_STREAMING_PLAN.md §4).
//
// Free-function region content builders for the Echotropolis WorldStreamer
// adoption (Tier 2). Each function is a VERBATIM port of the corresponding
// host_echotropolis.cpp build block (see the plan's §1 region table "owns"
// column) into the EchoRegion/EchoRegionCtx contract defined by WP-1's
// app/world_hosts/echo_regions.h. Same constants, same ECHO_* env-var
// overrides, same asset paths, same deterministic hashes — only the
// destination (region container vs. raw host locals) and the log prefix
// ("[region]" instead of "--world echotropolis:") change.
//
// OWNERSHIP: this header + echo_region_builders.cpp + assets/world/
// regions.echotropolis.json are WP-2's exclusively. Never edits
// host_echotropolis.cpp (the integrator, WP-0, deletes the ported originals
// there after wiring EchoRegionSet — copy-then-delete keeps single-writer).
//
// See echo_region_builders.cpp's top comment for the full port-status ledger
// and every `// INTEGRATOR:` gap left for WP-0/WP-1 (host state referenced by
// the original blocks — npcLife, TimeOfDay — that EchoRegionCtx does not
// expose, plus a couple of object-lifetime notes around Scene/StreetLights
// ownership that EchoRegion's §3 API doesn't cover).

#include "echo_regions.h"

namespace x3::game {

// crown: towers, houses, condos, crown-portion infra (streets + metro deck +
// subwayTrain — NOT the freeway network, which stays host-persistent per
// plan §1), hackProps/hackDrone/vtolPolice, streetProps (INTEGRATOR gap —
// needs npcLife), streetLamps + lampScene, drones, crown's (empty) slice of
// districtLights.
void buildCrown(EchoRegion& region, EchoRegionCtx& ctx);

// west_shoulder: mineProps, mineForest, mineGlowScene (+ GoldMineWorld
// mouth-glow author step), beam (lighthouse — INTEGRATOR gap: night-only
// gate not reproducible, see .cpp). Miners-crew re-attach is lifecycle
// wiring (§2), not builder content — out of scope for this function.
void buildWestShoulder(EchoRegion& region, EchoRegionCtx& ctx);

// district_urban / district_recife / district_hivemind: the matching
// districts.txt row (tag-filtered, data-driven exactly like the host).
// Meshes via this file's loadDistrictInto(); lights via WP-3's
// echo_woodlands.h harvestDistrictLights() (the per-region light-slicing
// plan §4 WP-3 describes) — each district builder calls both and feeds the
// result to EchoRegion::addLights. See .cpp top comment.
void buildDistrictUrban(EchoRegion& region, EchoRegionCtx& ctx);
void buildDistrictRecife(EchoRegion& region, EchoRegionCtx& ctx);
void buildDistrictHivemind(EchoRegion& region, EchoRegionCtx& ctx);

// harbor_bay: boats (3 lanes: south bay eastbound/westbound + SW inlet
// northbound) + their pose updates.
void buildHarborBay(EchoRegion& region, EchoRegionCtx& ctx);

} // namespace x3::game
