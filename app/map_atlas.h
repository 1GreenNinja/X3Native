// ===========================================================================
// map_atlas.h — the GTA-style world-map RASTERIZER (W-MAP v4).
//
// WHAT THIS IS
//   A pure CPU bake: world features in -> one RGBA8 tile out. Nothing here
//   touches the device, the scene or the streamer; world_map.cpp snapshots the
//   live world into a MapFeatureSet on the main thread and hands it to a worker
//   with a MapBakeRequest, then uploads the pixels when the worker is done.
//   That split is WHY the map can bake 2048^2 tiles while the game keeps
//   rendering: the only shared state is the feature snapshot, which the worker
//   owns for the duration of the bake.
//
// WHY A BAKE AT ALL (and not per-frame HUD quads)
//   The HUD vertex ring holds 4096 quads per frame, shared with every panel and
//   glyph on screen. A city grid + a freeway + 3000 building footprints is far
//   past that at any zoom where they matter, and quads cannot anti-alias a
//   ribbon edge anyway. So everything with AREA is baked into a texture; only
//   things with a POSITION (POIs, labels, player, waypoint, chrome) are quads.
//
// LAYERS (paint order, bottom -> top — the GTA V pause-map stack)
//   1. TERRAIN  hypsometric tint from the real height field (terrainHeightAtWorld)
//               x a north-west hillshade from the slope of the same samples.
//               Heights are sampled on a coarse grid (~2 m or 1 texel, whichever
//               is coarser) and bilinearly lifted to texels — the field has no
//               structure finer than that, and it cuts a 2048^2 street-zoom bake
//               from 4M height queries to ~66k.
//   2. WATER    worldWaterLevelAt over the same grid: sea, river, under-river
//               pools. Flat desaturated blue, deepening slightly with depth, a
//               1-texel lighter shoreline on the water side, and a sand band on
//               the land side where the ground sits within ~4 m of the surface.
//   3. DISTRICT soft radial tint discs (cityDistrictFootprint) so a district
//               reads as a zone before its label does.
//   4. FOOTPRINTS building/paved/walk/landmark rects from the region ledger's
//               world AABBs (drawn via exact rect coverage, so edges are AA at
//               any zoom): flat grey fills by height band with a 1-texel darker
//               edge; the Spire is its own landmark tone.
//   1b. TERRAIN FADE  GTA lets the map die off outside the world: the played
//               envelope is the union of MapFeatureSet::envelope discs (built
//               by the snapshot from districts, roads, footprints, river, sea,
//               POIs). Inside: full hillshade. Across kEnvelopeMarginM the
//               land lerps to kWild — one flat desaturated tone a shade under
//               the low-land tint — with the hillshade contrast collapsing to
//               kWildReliefHint of itself (a ghost of relief, not paint), and
//               a further kWildFarDarken settles by kEnvelopeFarM. The
//               distance field is sampled on a coarse grid (<= 192 cells
//               across the tile) and bilinearly lifted — the union-of-discs
//               distance is smooth enough that the ramp shows no facets.
//               Water, beaches, roads and everything above are untouched.
//   5. ROADS    ribbons by class (Street < Arterial < Ramp < Freeway) each as a
//               DISTANCE FIELD to its polylines — the union-of-min-distance
//               gives rounded joins for free and the casing/fill edges are
//               analytic coverage (no supersampling needed). Dark casing, light
//               fill, a median stripe on dual carriageways, DASHED fill where a
//               segment is bored (tunnel) and a heavier casing where it is
//               decked (bridge).
//
// COLOUR SPACE
//   The palette is authored in sRGB bytes and the tile is created with
//   srgb=true, so the sampler decodes it and the HUD blend happens in linear
//   like every other HUD quad. Hillshade is multiplied in sRGB space — this is
//   cartography, not lighting, and the perceptual ramp is what we want.
// ===========================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

enum class MapRoadClass : uint8_t { Street = 0, Arterial, Ramp, Freeway, Count };

struct MapRoad {
    std::string   name;
    MapRoadClass  cls    = MapRoadClass::Street;
    float         halfW  = 4.0f;        // drawn pavement half-width (m)
    bool          median = false;       // dual carriageway: paint the median stripe
    // Dual carriageway: the median's TRUE half-width (m). > 0 splits the ribbon
    // into two roadways with the median showing between them (the canon city
    // freeway is two 44 m carriageways around a 13 m median — drawn as one
    // 101 m slab it read as a runway). 0 keeps the single-stripe look.
    float         medianHalfW = 0.0f;
    std::vector<float>   x, z;          // centreline nodes (world XZ)
    std::vector<uint8_t> tunnel;        // per SEGMENT (size n-1, optional): 1 = bored reach
    std::vector<uint8_t> bridge;        // per SEGMENT (size n-1, optional): 1 = decked reach
};

enum class MapFootKind : uint8_t { Building = 0, Paved, Walk, Landmark };

struct MapFootprint {
    float x0, z0, x1, z1;   // world AABB (XZ)
    float h;                // height above ground (m) — picks the grey band
    MapFootKind kind;
};

struct MapDistrict {
    std::string name;
    float cx, cz, r;        // tint disc (m) — massRadius, not the flat-pad radius
    float rgb[3];           // sRGB 0..1 tint
};

// One disc of the PLAYED ENVELOPE: the union of these discs is where the game
// happens (districts, every road, footprints, river, sea, POIs). Terrain
// outside it fades to a quiet flat wash over kEnvelopeMarginM (see TERRAIN
// FADE in the header). Empty = no fade (the pure bake tests).
struct MapEnvDisc { float cx, cz, r; };
constexpr float kEnvelopeMarginM = 350.0f;   // full hillshade -> flat wash across this band
constexpr float kEnvelopeFarM    = 1400.0f;  // ...then a very subtle darkening settles by here

struct MapFeatureSet {
    std::vector<MapRoad>      roads;
    std::vector<MapFootprint> footprints;
    std::vector<MapDistrict>  districts;
    std::vector<MapEnvDisc>   envelope;
};

// Distance (m, >= 0) from world (wx,wz) to the played envelope; 0 inside it.
// Brute force over the discs — the bake uses a coarse grid of this, the tests
// call it directly to pick in-world vs wilderness probes.
float mapEnvelopeDistance(const MapFeatureSet& features, float wx, float wz);

struct MapBakeRequest {
    float    wx0 = 0, wz0 = 0, wx1 = 0, wz1 = 0;   // world rect covered
    uint32_t res = 1024;                          // square tile edge (texels)
    // Fade the outer `featherTexels` ring to alpha 0 so a detail tile composites
    // seamlessly onto the overview tile beneath it (and the REPEAT-wrap sampler
    // can never show the far edge as a seam).
    uint32_t featherTexels = 0;
    // Skip footprints/roads finer than this many texels (overview bakes drop
    // street-level noise instead of aliasing it into mush).
    float    minFeatureTexels = 0.6f;
};

struct MapBakeStats {
    double   heightMs   = 0;   // grid sampling (terrainHeightAtWorld + water)
    double   terrainMs  = 0;   // per-texel tint/shade/water pass
    double   districtMs = 0;
    double   footMs     = 0;
    double   roadMs     = 0;
    double   totalMs    = 0;
    uint32_t heightSamples = 0;
    uint32_t threads = 1;
};

// The palette (sRGB bytes) — exported so the legend, the HUD chrome and the
// --test-worldmap pixel probes read the SAME constants the rasterizer paints.
namespace mappal {
constexpr uint8_t kWaterDeep[3]    = { 78, 112, 142 };
constexpr uint8_t kWaterShallow[3] = { 96, 132, 160 };
constexpr uint8_t kShore[3]        = { 168, 196, 212 };
constexpr uint8_t kSand[3]         = { 214, 204, 174 };
constexpr uint8_t kLandLow[3]      = { 170, 178, 154 };
constexpr uint8_t kLandMid[3]      = { 180, 178, 158 };
constexpr uint8_t kLandHigh[3]     = { 198, 192, 180 };
constexpr uint8_t kLandPeak[3]     = { 228, 226, 222 };
constexpr uint8_t kRoadFill[3]     = { 244, 244, 240 };   // street / arterial / ramp
constexpr uint8_t kFwyFill[3]      = { 250, 246, 230 };   // freeway — a warmer white
constexpr uint8_t kRoadCase[3]     = { 118, 120, 126 };
constexpr uint8_t kFwyCase[3]      = { 92, 94, 102 };
constexpr uint8_t kBridgeCase[3]   = { 56, 58, 66 };
constexpr uint8_t kMedian[3]       = { 196, 190, 168 };
constexpr uint8_t kBldgLow[3]      = { 208, 206, 200 };
constexpr uint8_t kBldgMid[3]      = { 190, 189, 185 };
constexpr uint8_t kBldgTall[3]     = { 170, 171, 170 };
constexpr uint8_t kBldgTower[3]    = { 150, 152, 156 };
constexpr uint8_t kPaved[3]        = { 200, 198, 192 };
constexpr uint8_t kWalk[3]         = { 218, 216, 210 };
constexpr uint8_t kLandmark[3]     = { 134, 142, 166 };
constexpr uint8_t kLandmarkEdge[3] = { 70, 76, 100 };
constexpr uint8_t kVoid[3]         = { 150, 156, 142 };   // beyond the overview tile
constexpr uint8_t kWild[3]         = { 156, 158, 148 };   // wilderness wash past the envelope (flat, desaturated, a shade under kLandLow)
constexpr float   kWildReliefHint  = 0.035f;              // fraction of the hillshade kept in the wash (0 = painted flat)
constexpr float   kWildFarDarken   = 0.06f;               // extra darkening settled by kEnvelopeFarM
}

// Bake one tile. `outRgba` is resized to res*res*4. Thread-safe (pure); the
// bake itself fans out across hardware threads internally.
void bakeMapTilePixels(const MapFeatureSet& features, const MapBakeRequest& rq,
                       std::vector<uint8_t>& outRgba, MapBakeStats* stats = nullptr);

// Drawn half-width in TEXELS for a road class at `metresPerTexel`: the true
// pavement width, floored so a road never vanishes when zoomed out (streets
// keep a 1.5-texel presence, the freeway 3). Exposed for the tests.
float mapRoadHalfWidthTexels(MapRoadClass cls, float halfWidthM, float metresPerTexel);

// Pixel classifiers for the tests / the legend (sRGB bytes in, tolerance in).
bool mapPixelIsWater(const uint8_t* rgba, int tol = 22);
bool mapPixelIsRoadFill(const uint8_t* rgba, int tol = 18);
bool mapPixelIsRoadCasing(const uint8_t* rgba, int tol = 22);

} // namespace x3::game
