#pragma once
// ECHO INTERIORS — Tim's order (2026-07-29): "Upgrade the vendors, the building
// interiors.. ALL BUILDINGS that a character COULD enter.. need textures, drawn
// when near or entered.. streaming world style."
//
// DESIGN: every enterable building's interior is a SUB-REGION riding the
// existing WorldStreamer + EchoRegionSet machinery (plan §2 — Lane A/B was
// left "wired and empty by design" for exactly this). An interior builds its
// textured kit content when the player nears its door (small load radius) and
// stops drawing when they leave — the "drawn when near or entered" contract is
// literally the streamer's wants + the M-B draw gate, no new engine work.
// The condo ROOM quads move OUT of buildCrown into the `int_condo_rooms`
// sub-region here — which simultaneously kills the #1 slop from the 07-29
// sweep (shell-less lit rooms floating in daylight at any distance) because
// (a) buildCrown now wraps each stack in a real textured SHELL, and (b) the
// rooms only render inside their sub-region's radius.
//
// VERIFIED ASSETS (glb_audit, 2026-07-29): TheHotel_Model.glb 21.2x23.0x25.7m
// base-centered (the condo shell body); BusStand01/02/03 ~4.8x3.2x2.8m
// (stall/bar shells); Shops01_Model 55.7x7.3x72.7m (harbor storefront block);
// cond_*.glb rooms 3.68x3.08x2.84m back-face-at-z0; SB_* signboards;
// Seaside Town standing-torch props; assets/meshy/props/ctos_terminal.glb.
//
// ============================== INTEGRATION ==============================
// (WP-0 / host integrator — this module is complete but UNWIRED)
//  1. CMake: add world_hosts/echo_interiors.cpp.
//  2. Host (next to the other registerBuilder calls):
//         reg("int_condo_rooms", x3::game::buildCondoRooms);
//         reg("int_noodle_bar",  x3::game::buildNoodleBar);
//         reg("int_harbor_shop", x3::game::buildHarborShop);
//     (the three JSON entries are already in regions.echotropolis.json; at
//      M-A/M-B forceAllResident boot they build like everything else — true
//      near-radius realize/evict arrives free with M-C.)
//  3. VENDOR BUY LOOP: in the walk-mode E handling, before car interact,
//     check kVendorInteractions: if the player is within `radius` of `pos`,
//     draw `promptLine` (the worldCars.prompt() slot style) and on E deduct
//     `price` from the treasury, +1 karma tick — the DODOG economy. The table
//     is data; the host owns the input+treasury wiring.
//  4. buildCrown already calls buildVendorDressing() (stalls/signs/torches are
//     ALWAYS-VISIBLE exterior dressing, not gated interiors).
//  5. Door markers: each interior cell's door has a ctOS terminal prop beside
//     it (the entry kiosk). A real swinging door + wall-cutout portal is the
//     M-C-era polish, honestly out of v1 scope.
// =========================================================================

#include "echo_regions.h"

namespace x3::game {

// One enterable building (v1 registry — the host may draw door prompts from it).
struct InteriorCell {
    const char* id;          // matches the regions.json sub-region id
    float doorX, doorZ;      // world door position (prompt anchor)
    float radius;            // interaction/prompt radius at the door (m)
};
// v1 cells: condo lobby door (center stack), noodle bar, harbor shop.
extern const InteriorCell kInteriorCells[3];

// Vendor buy-interaction contract (host wires E + treasury; see INTEGRATION 3).
struct VendorInteraction {
    float x, z;              // stand-here position
    float radius;            // prompt radius (m)
    const char* promptLine;  // HUD line while in radius
    int   price;             // treasury delta on E (negative = the player PAYS)
};
extern const VendorInteraction kVendorInteractions[3];

// Sub-region builders (EchoRegionSet contract — same as every other region).
void buildCondoRooms(EchoRegion& region, EchoRegionCtx& ctx);   // the moved lit rooms
void buildNoodleBar(EchoRegion& region, EchoRegionCtx& ctx);    // the Programmer's haunt
void buildHarborShop(EchoRegion& region, EchoRegionCtx& ctx);   // boulevard storefront

// ALWAYS-VISIBLE vendor dressing (stalls + signage + torches), called from
// buildCrown — exterior street furniture, deliberately NOT a gated interior.
void buildVendorDressing(EchoRegion& region, EchoRegionCtx& ctx);

} // namespace x3::game
