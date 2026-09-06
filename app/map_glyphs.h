// ===========================================================================
// map_glyphs.h — the world map's ICON SHEET (W-MAP v4).
//
// Every positional mark on the map (POI blips, the player arrow, the waypoint,
// the objective diamond, the compass rose) is one textured HUD quad sampling a
// glyph from a sheet rasterized at init: white shapes on transparent, built from
// signed-distance functions at 4x4 supersampling, so they are crisp at any size
// and tint per class via the quad colour. Before this the POIs were 5 stamped
// quads + a mono letter each, aliased at every zoom and eating the HUD ring.
//
// The sheet is 8 cells wide, 64 px per cell, with a 6 px transparent apron in
// every cell so the REPEAT/bilinear sampler never bleeds a neighbour in. The
// player arrow and the compass rose ALSO get their own single-glyph textures
// (drawHudImageQuad carries no sub-rect UVs, and those two rotate).
// ===========================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace x3::game {

enum class MapGlyph : uint8_t {
    Plate = 0,     // rounded square backplate (POI blip body)
    PlateRing,     // rounded square outline (hover / selected)
    Disc,          // filled circle
    Ring,          // circle outline
    Diamond,       // filled diamond
    DiamondRing,   // diamond outline (objective)
    Pin,           // map pin (generic landmark)
    Star,          // 5-point star (The Spire)
    Tower,         // tall block + antenna (city / district)
    Base,          // dome on a slab (undersea disc base)
    Car,           // side-view car (dealership)
    Wrench,        // spanner (performance shop)
    Mountain,      // two peaks (ranges)
    Interchange,   // ring + four bars (freeway interchange)
    Portal,        // arch (tunnel / under-river daylight portal)
    Flag,          // pennant (waypoint)
    Arrow,         // heading chevron (player)
    Cross,         // thin plus (crash site / waypoint centre)
    Door,          // door frame
    Elevator,      // box with up/down chevrons
    Skull,         // boss
    Lock,          // security / armory
    Cell,          // barred window (jail cell)
    Secret,        // disc with a hole (hidden things)
    Club,          // note-ish (club 1127)
    Hall,          // wide low block
    Rose,          // compass rose
    Count
};

constexpr uint32_t kMapGlyphCell = 64;
constexpr uint32_t kMapGlyphCols = 8;
constexpr uint32_t kMapGlyphRows = ((uint32_t)MapGlyph::Count + kMapGlyphCols - 1) / kMapGlyphCols;

// The sheet: white RGB, alpha = coverage. w = kMapGlyphCols*kMapGlyphCell.
void rasterizeMapGlyphSheet(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h);
// A single glyph at `size` px (square), same white/alpha convention.
void rasterizeMapGlyph(MapGlyph g, uint32_t size, std::vector<uint8_t>& rgba);
// UV sub-rect of a glyph in the sheet (drawHudImage's u0,v0,u1,v1).
void mapGlyphUv(MapGlyph g, float& u0, float& v0, float& u1, float& v1);
// Coverage of one glyph at normalized cell coords p in [-1,1]^2 (1 = inside).
// Exposed so the self-test can probe shapes without a texture.
float mapGlyphCoverage(MapGlyph g, float px, float py);

} // namespace x3::game
