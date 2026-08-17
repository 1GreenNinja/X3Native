// THE SMALL MOUNTAIN TOWN — implementation. See town.h for the contract and
// the provenance of every rule; this file is the arithmetic.

#include "town.h"

#include "asset_root.h"
#include "mesh_prims.h"
#include "terrain.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace x3::game {

namespace {

// ---------------------------------------------------------------------------
// THE ASSET TABLE — every number MEASURED from the converted GLB
// (tools/town_assets.py --report), never eyeballed (X3_WORLD_RULES rule 0).
//
//   cx/cz    bbox centre in ASSET space. These prefabs are NOT centred on
//            their origin (PF_StoneHouse01 sits 5.97 m to -X and 7.45 m to +Z
//            of it), so a lot that ignores this puts the building metres off
//            its plot — and, on a street, half of it in the road.
//   hx/hz    bbox half extents (the footprint the collider and the plot use).
//   loY/hiY  bbox vertical span relative to the origin plane. This kit sits ON
//            its origin (loY ~ 0) rather than modelling a plinth below it, so
//            the -loY lift below is a no-op here; it is kept because the placer
//            must stay correct for any kit (X3_WORLD_RULES rule 4).
//   frontDeg the asset's own front, in the engine yaw convention (0 = -Z),
//            measured as the direction from the bbox centre to the centroid of
//            the asset's DOOR material. Rule 3 wants orientation DOCUMENTED per
//            asset; this table is that document, and
//            docs/design/TOWN_MANIFEST.md repeats it in prose.
//   win[3]   WHERE THE LIT PANES GO — one entry per REAL window, measured by
//            clustering the asset's own front-elevation `Glass` triangles into
//            connected components. Round one guessed two fixed storey heights
//            (2.35 m / 5.20 m) because the ruined kit modelled no glazing; the
//            kit's houses run 9 m to 25.7 m tall, so one fixed pair would have
//            put House_2's upper pane inside its roof. The FIRST attempt here
//            averaged a whole storey's glass, which put the pane on the blank
//            wall BETWEEN two windows — visible in the capture as grey cards
//            stuck to the clapboard. Per-window, with each window's own size,
//            is the version that is actually right.
//
// ---------------------------------------------------------------------------
// THE KIT (swapped 2026-08-17; see docs/design/TOWN_MANIFEST.md section 2).
// Round one built this town from the armory's HouseForge prefabs. Eyes-on at
// full res, they read as DERELICT — dark, spiky, broken silhouettes — because
// that kit is authored as COLLAPSED RUINS. docs/design/TOWN_ASSET_SCOUT.md
// reached the same verdict independently. The placer was never the problem, so
// only these tables changed.
//
// Everything structural now comes from ONE licensed pack, `Complete Racing Game
// URP All in One` — authored for a DRIVING game, so the town reads as a place
// on this world's road network. Baked by tools/town_assets.py, which injects
// the pack's real photographic albedos BY MATERIAL NAME (the pack ships no
// Unity .mat files at all, so FBX2glTF writes 1x1 white placeholders and the
// GUID path in convert_unity_pack.py has nothing to resolve), and transcodes
// its TIFFs to JPEG because ModelLoader decodes with stb_image, which cannot
// read TIFF. `town_assets.py verify` asserts both, and is GREEN.
//
// Eight facades come from four shells, in two real paints (red clapboard +
// scalloped shingle / white clapboard + plank roof). The pack's UVs are
// authored to tile, so this is variety by MATERIAL — which is how a real street
// of one builder's houses actually looks — rather than the same mesh repeated,
// which is the uniform lattice the x3native-environments skill forbids.
// ---------------------------------------------------------------------------
struct AssetDef {
    const char* glb;
    float cx, cz;      // bbox centre, asset space
    float hx, hz;      // bbox half extents
    float loY, hiY;    // bbox vertical span about the origin plane
    float frontDeg;    // measured front (engine yaw, 0 = -Z)
    // MEASURED lit-window positions, one entry per REAL window: height above
    // the bbox bottom, offset along the front wall, and the window's own HALF
    // SIZE. The emissive quad is cut to the opening it sits in, so unlit it
    // vanishes against the dark glass instead of reading as a card on the
    // wall — which is exactly what the averaged version looked like.
    struct Pane { float y, along, depth, hw, hh; } win[3];
};

// Buildings FIRST: `isBuilding` below is an ordering test against A_LAST_BLDG.
enum : uint8_t {
    A_H1R = 0, A_H1W, A_H2R, A_H2W, A_H3R, A_H3W, A_H4R, A_H4W,
    A_LAST_BLDG = A_H4W,
    A_LAMP, A_FENCE, A_SIGN1, A_SIGN2, A_BENCH_A, A_BENCH_B,
    A_COUNT
};

const AssetDef kAssets[A_COUNT] = {
    // Regenerate with `python tools/town_assets.py report` and paste the block
    // it prints. If the kit is re-baked, re-run it and update this table in the
    // same commit — PAIRED VALUES ARE ONE VALUE (NO_SLOP rule 4).
    // Buildings first: `isBuilding` is an ordering test against A_LAST_BLDG.
    { "Town/House_1_Red.glb",                  -0.95f,  -0.13f,  11.50f,   9.24f, -0.00f,  15.75f,   101.9f,
      { {  8.45f,  -6.78f,   6.28f, 0.40f, 1.03f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    { "Town/House_1_White.glb",                -0.95f,  -0.13f,  11.50f,   9.24f, -0.00f,  15.75f,   101.9f,
      { {  8.45f,  -6.78f,   6.28f, 0.40f, 1.03f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    { "Town/House_2_Red.glb",                  -0.05f,  -0.41f,  13.75f,   7.44f, -0.00f,   9.00f,   180.0f,
      { {  3.38f,   8.61f,   5.99f, 1.29f, 0.40f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    { "Town/House_2_White.glb",                -0.05f,  -0.41f,  13.75f,   7.44f, -0.00f,   9.00f,   180.0f,
      { {  3.38f,   8.61f,   5.99f, 1.29f, 0.40f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    { "Town/House_3_Red.glb",                  -0.60f,   1.25f,   7.49f,  12.50f, -0.00f,  15.86f,    -0.6f,
      { {  8.35f,  -0.11f,  10.93f, 1.06f, 1.06f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    { "Town/House_3_White.glb",                -0.60f,   1.25f,   7.49f,  12.50f, -0.00f,  15.86f,    -0.6f,
      { {  8.35f,  -0.11f,  10.93f, 1.06f, 1.06f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    { "Town/House_4_Red.glb",                  -3.73f,   0.50f,  18.76f,  12.56f, -0.08f,  25.64f,  -179.5f,
      { {  7.45f, -10.88f,   8.61f, 1.51f, 1.20f}, {  7.45f,  -3.99f,   8.54f, 0.66f, 1.29f}, {  7.45f,   9.72f,   8.42f, 0.40f, 1.20f} } },
    { "Town/House_4_White.glb",                -3.73f,   0.50f,  18.76f,  12.56f, -0.08f,  25.64f,  -179.5f,
      { {  7.45f, -10.88f,   8.61f, 1.51f, 1.20f}, {  7.45f,  -3.99f,   8.54f, 0.66f, 1.29f}, {  7.45f,   9.72f,   8.42f, 0.40f, 1.20f} } },
    { "Town/Light_2.glb",                       0.12f,   0.00f,   0.57f,   0.57f, -0.00f,   7.16f,     0.0f,
      { {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    { "Town/Wood_Fence.glb",                    0.06f,  -0.14f,   2.78f,   0.15f, -0.00f,   2.32f,     0.0f,
      { {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    { "Town/Billboard_1.glb",                  -0.84f,   1.88f,   0.15f,   0.87f, -0.64f,   1.81f,     0.0f,
      { {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    { "Town/Billboard_2.glb",                  -0.15f,   1.71f,   0.13f,   1.03f, -0.40f,   1.88f,     0.0f,
      { {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f}, {  0.00f,   0.00f,   0.00f, 0.00f, 0.00f} } },
    // Already decoded and in-tree from the road_trees lane (armory GLBs).
    { "nature/SM_WoodBench_01a.glb",             0.00f,   0.00f,   1.10f,   0.55f,  0.00f,   0.90f,     0.0f,
      { {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0} } },
    { "nature/SM_Bench.glb",                     0.00f,   0.00f,   1.10f,   0.55f,  0.00f,   0.90f,     0.0f,
      { {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0} } },
};

// Anything with a footprint at least this wide gets a static collider. Below
// it (carts, torches, benches, crates) the box would be an invisible wall on a
// sidewalk, which is worse than driving through a barrel.
constexpr float kCollideMinHalfM = 1.6f;

// ---------------------------------------------------------------------------
// THE LOT TABLE — the town, authored. This is the curation the
// x3native-environments skill demands: a designer's street, not a lattice.
// Rules it encodes by hand:
//   * varied MASSING — the long low House_2 (27.5 x 9 m), the narrow tall
//     House_3 (15 x 15.9 m), the barn-fronted House_1 (23 x 15.8 m) and the
//     big House_4 lodge (37.5 x 25.7 m) as the square's hero; no shell and no
//     PAINT repeats twice running on the same side of the street;
//   * varied SETBACK — main street crowds the pavement at 18.5-20.5 m, the
//     approaches and outskirts sit back at 24-30 m;
//   * varied GAPS — the along-street clear space between neighbours runs 23 m
//     to 57 m, tight through the middle where the town is densest;
//   * a SQUARE at u~306: House_4 as the hero, set back on the -side;
//   * broken alignment — every plot carries its own yaw jitter so no two
//     facades are parallel.
//
// `lat` IS THE SETBACK OF THE FRONT FACE FROM THE STREET CENTRELINE — not, as
// round one had it, the distance to the bbox CENTRE. That change is a bug fix,
// not a rename: under centre-distance semantics a plot at lat 18.4 holding an
// asset whose front support is 9.2 m put its facade 9.2 m off the centreline,
// i.e. INSIDE kPavedHalfM (14.63 m) — a building standing in the road. The
// placer now adds each asset's own MEASURED front support (see `place`), so the
// number in this table is the thing a level designer actually cares about and
// the keep-out is enforced on the FACE, which is what the keep-out is for.
// PAIRED with kStreetKeepOutM in town.h.
// ---------------------------------------------------------------------------
struct Lot { float u; int8_t side; float lat; uint8_t asset; float jitterDeg; };

// The tables are authored against this reach and rescaled onto whatever reach
// the host gives, so the town stretches with the spur instead of falling off
// the end of it. PAIRED with the `u` columns of kLots/kProps/kParks — these
// three tables share one coordinate; move the reach and all three move.
constexpr float kAuthoredU0   = 70.0f;
constexpr float kAuthoredSpan = 620.0f;

const Lot kLots[] = {
    // ---- lower town: the approach, loose and rural ----
    {  78.0f, -1, 26.0f, A_H2W,  -7.0f },
    { 104.0f, +1, 28.0f, A_H1W,  11.0f },
    { 130.0f, -1, 24.0f, A_H3R,   5.0f },
    { 158.0f, +1, 24.5f, A_H2R,  -6.0f },
    // ---- main street proper: fronting the pavement, and DENSE ----
    // The first cut of this table put ~77 m between neighbours on the same
    // side and the capture read as scattered farmsteads, not a main street.
    // Through the middle third the gaps are now 18-26 m of clear ground, which
    // is what makes a street feel like one; the ends stay loose so the town
    // still thins into the switchbacks instead of ending at a wall.
    { 172.0f, -1, 19.5f, A_H1R,   3.0f },
    { 196.0f, +1, 19.0f, A_H2R,  -5.0f },
    { 214.0f, +1, 18.5f, A_H3W,  -9.0f },
    { 232.0f, -1, 20.0f, A_H3W,   6.0f },
    { 252.0f, -1, 18.8f, A_H2W,  -8.0f },
    { 268.0f, +1, 19.0f, A_H1R, -13.0f },
    { 286.0f, +1, 20.0f, A_H3R,   9.0f },
    // ---- the square: the hero set back, the street tight around it ----
    { 306.0f, -1, 24.0f, A_H4R,  -4.0f },   // THE HERO (kHeroLot)
    { 330.0f, +1, 19.5f, A_H2W,   8.0f },
    { 348.0f, -1, 19.0f, A_H1W,  -6.0f },
    // ---- upper street, climbing and tightening ----
    { 366.0f, -1, 20.0f, A_H3W,   7.0f },
    { 380.0f, +1, 20.5f, A_H3R, -10.0f },
    { 396.0f, -1, 19.0f, A_H2R,   4.0f },
    { 414.0f, +1, 19.5f, A_H1R,  -8.0f },
    { 430.0f, +1, 22.0f, A_H1W,  13.0f },
    { 452.0f, -1, 20.0f, A_H3R,  -5.0f },
    { 490.0f, +1, 21.5f, A_H2R,   7.0f },
    // ---- the outskirts, thinning toward the switchbacks ----
    { 520.0f, -1, 26.0f, A_H1W, -12.0f },
    { 556.0f, +1, 28.0f, A_H3W,  15.0f },
    { 600.0f, -1, 30.0f, A_H2W,   6.0f },
    { 630.0f, +1, 25.0f, A_H1R, -14.0f },
};
constexpr uint32_t kLotCount = (uint32_t)(sizeof(kLots) / sizeof(kLots[0]));
// The square's hero, by INDEX into kLots — the shop-front eye gate aims here.
// Round one matched on an ASSET ID, which silently aims at whichever plot comes
// last whenever that asset is used more than once; an index cannot drift.
constexpr uint32_t kHeroLot = 11;

// Street furniture: the pack's own 7.2 m highway lamp standard both sides at a
// loose rhythm (round one used a medieval torch — the ruined kit's register),
// benches on the verge, fence runs closing the gaps between houses, and two
// roadside billboards at the approaches. Same lot form, but these never carry
// a collider (an invisible wall on a sidewalk is worse than walking through a
// bench) and only the lamps are lit.
const Lot kProps[] = {
    {  92.0f, -1, 17.8f, A_SIGN1,    4.0f },
    { 116.0f, +1, 17.8f, A_LAMP,     0.0f },
    { 144.0f, -1, 17.6f, A_FENCE,    2.0f },
    { 150.0f, -1, 17.6f, A_FENCE,   -1.0f },
    { 176.0f, +1, 17.8f, A_LAMP,     0.0f },
    { 196.0f, -1, 17.9f, A_BENCH_A, 90.0f },
    { 208.0f, -1, 17.8f, A_LAMP,     0.0f },
    { 248.0f, +1, 17.9f, A_BENCH_B, 90.0f },
    { 258.0f, +1, 17.8f, A_LAMP,     0.0f },
    { 290.0f, -1, 17.8f, A_LAMP,     0.0f },
    { 312.0f, +1, 17.9f, A_BENCH_A, 90.0f },
    { 344.0f, +1, 17.8f, A_LAMP,     0.0f },
    { 366.0f, -1, 17.6f, A_FENCE,    3.0f },
    { 372.0f, -1, 17.6f, A_FENCE,   -2.0f },
    { 408.0f, -1, 17.8f, A_LAMP,     0.0f },
    { 442.0f, +1, 17.9f, A_BENCH_B, 90.0f },
    { 466.0f, +1, 17.8f, A_LAMP,     0.0f },
    { 508.0f, -1, 17.8f, A_LAMP,     0.0f },
    { 544.0f, +1, 17.6f, A_FENCE,    1.0f },
    { 550.0f, +1, 17.6f, A_FENCE,   -3.0f },
    { 574.0f, +1, 17.8f, A_LAMP,     0.0f },
    { 612.0f, -1, 17.8f, A_SIGN2,   -5.0f },
};
constexpr uint32_t kPropCount = (uint32_t)(sizeof(kProps) / sizeof(kProps[0]));

// PARKED CARS along main street (the converted fleet, assets/converted_glb/
// Vehicles — all eleven verified textured by the garage lane). Angle-parked
// nose-in off the apron edge, the way a mountain main street parks.
struct ParkLot { float u; int8_t side; float lat; uint8_t car; float skewDeg; };
const char* const kCarGlb[] = {
    "Vehicles/E30.glb", "Vehicles/Pickup.glb", "Vehicles/Jeep.glb",
    "Vehicles/Coupe.glb", "Vehicles/Muscle.glb", "Vehicles/Truck.glb",
    "Vehicles/M3_E36.glb", "Vehicles/E46_New.glb",
};
constexpr uint32_t kCarGlbCount = (uint32_t)(sizeof(kCarGlb) / sizeof(kCarGlb[0]));
const ParkLot kParks[] = {
    { 200.0f, -1, 17.9f, 0,  62.0f },
    { 208.0f, -1, 17.9f, 1,  58.0f },
    { 240.0f, +1, 17.9f, 2, -60.0f },
    { 276.0f, +1, 17.9f, 3, -63.0f },
    { 298.0f, +1, 18.0f, 4, -58.0f },
    { 336.0f, -1, 17.9f, 5,  61.0f },
    { 360.0f, -1, 17.9f, 6,  59.0f },
    { 424.0f, +1, 17.9f, 7, -61.0f },
    { 476.0f, -1, 17.9f, 1,  60.0f },
    { 528.0f, +1, 17.9f, 0, -59.0f },
};
constexpr uint32_t kParkCount = (uint32_t)(sizeof(kParks) / sizeof(kParks[0]));

constexpr float kDeg = 0.01745329252f;

// Yaw matrix, engine convention: local +Z maps to (sin a, cos a), so a feature
// whose asset-space yaw is f ends up pointing at world yaw f + a. Identical to
// the matrix road_trees.cpp builds for its benches.
inline void yawMat(float a, float px, float py, float pz, float out[16]) {
    const float c = std::cos(a), s = std::sin(a);
    const float m[16] = { c, 0, -s, 0,
                          0, 1,  0, 0,
                          s, 0,  c, 0,
                          px, py, pz, 1 };
    for (int i = 0; i < 16; ++i) out[i] = m[i];
}
// Rotate an asset-space XZ offset by the same yaw.
inline void yawXZ(float a, float x, float z, float& ox, float& oz) {
    const float c = std::cos(a), s = std::sin(a);
    ox = c * x + s * z;
    oz = -s * x + c * z;
}
// Engine yaw of a planar direction (AXES LAW; 0 = -Z).
inline float yawOf(float dx, float dz) { return std::atan2(-dx, -dz); }

// HOW FAR THE FRONT FACE STANDS FROM THE BBOX CENTRE, along the asset's own
// front direction. This is what turns a designer's FACE setback (the `lat`
// column of kLots) into the centre offset the placer needs, and it is why a
// building can no longer end up with its facade inside the carriageway.
// It is the support function of the axis-aligned box in direction `front`,
// which for a box is just |fx|*hx + |fz|*hz.
inline float frontSupport(const AssetDef& A) {
    const float fx = -std::sin(A.frontDeg * kDeg);
    const float fz = -std::cos(A.frontDeg * kDeg);
    return std::fabs(fx) * A.hx + std::fabs(fz) * A.hz;
}

} // anonymous namespace

CharacterClipTable townPedClipTable() {
    // MEASURED from the roster GLBs themselves (glTF animation names):
    //   AnnaCasual_anim   : CarryIdle CheckDevice Converse Idle LookAround Run
    //                       Sit Talk Walk Work
    //   marcus_webb_anim  : Attack Attack2 Death Hitreaction Idle Jump Run
    //                       Struggle Walk
    //   chief_martinez_anim: same set as marcus.
    // The three share Idle/Walk/Run, which is the whole roster contract
    // (crowd_skin.cpp:35 says so and this table is the second reader of it).
    // AnimatedCharacter resolves by EXACT name, so jakeClipTable()'s
    // "Walking"/"Running" would resolve to -1 here and leave a sliding statue.
    CharacterClipTable t;
    t.idle = "Idle";
    t.walk = "Walk"; t.walkSpeed = 1.35f;   // authored gait, m/s
    t.run  = "Run";  t.runSpeed  = 4.0f;
    t.jump = "Jump";                        // absent on AnnaCasual; degrades
    t.idleVariant = "LookAround"; t.idleVariantEvery = 22.0f;
    return t;
}

// ---------------------------------------------------------------------------

bool Town::stationAt(float u, Station& out) const {
    if (m_st.size() < 2) return false;
    if (u <= m_st.front().u) { out = m_st.front(); return true; }
    if (u >= m_st.back().u)  { out = m_st.back();  return true; }
    for (size_t i = 1; i < m_st.size(); ++i) {
        if (m_st[i].u < u) continue;
        const Station& a = m_st[i - 1];
        const Station& b = m_st[i];
        const float d = b.u - a.u;
        const float f = (d > 1e-4f) ? (u - a.u) / d : 0.0f;
        out.x  = a.x  + (b.x  - a.x)  * f;
        out.z  = a.z  + (b.z  - a.z)  * f;
        out.y  = a.y  + (b.y  - a.y)  * f;
        out.tx = a.tx + (b.tx - a.tx) * f;
        out.tz = a.tz + (b.tz - a.tz) * f;
        const float L = std::sqrt(out.tx * out.tx + out.tz * out.tz);
        if (L > 1e-5f) { out.tx /= L; out.tz /= L; }
        out.u = u;
        return true;
    }
    out = m_st.back();
    return true;
}

bool Town::build(Scene& scene, x3::rhi::IRenderDevice& device,
                 x3::phys::IPhysicsWorld& phys, const Config& cfg) {
    if (m_built) return m_buildings > 0;
    m_built = true;
    m_scene = &scene;

    if (!cfg.street || cfg.street->x.size() < 2 ||
        cfg.street->x.size() != cfg.street->z.size()) {
        x3::logWarn("town: no street route — no town built");
        return false;
    }
    const RoadSpec& s = *cfg.street;
    const size_t n = s.x.size();
    const bool haveY = cfg.streetY && cfg.streetY->size() == n;

    // ---- station table: the street centreline with arc length + tangents ----
    m_st.reserve(n);
    float acc = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        Station st{};
        st.x = s.x[i]; st.z = s.z[i];
        st.y = haveY ? (*cfg.streetY)[i] : terrainHeightAtWorld(st.x, st.z);
        const size_t j = (i + 1 < n) ? i + 1 : i;
        const size_t k = (i > 0) ? i - 1 : i;
        float dx = s.x[j] - s.x[k], dz = s.z[j] - s.z[k];
        const float L = std::sqrt(dx * dx + dz * dz);
        st.tx = (L > 1e-5f) ? dx / L : 1.0f;
        st.tz = (L > 1e-5f) ? dz / L : 0.0f;
        if (i > 0) {
            const float sx = s.x[i] - s.x[i - 1], sz = s.z[i] - s.z[i - 1];
            acc += std::sqrt(sx * sx + sz * sz);
        }
        st.u = acc;
        m_st.push_back(st);
    }
    const float routeLen = m_st.back().u;
    const float u0 = std::max(cfg.startU, 0.0f);
    const float u1 = std::min(cfg.endU, routeLen);
    if (u1 - u0 < 120.0f) {
        char b[192];
        std::snprintf(b, sizeof(b),
            "town: street reach [%.0f, %.0f] of a %.0f m route is too short for a "
            "town — nothing built", (double)u0, (double)u1, (double)routeLen);
        x3::logWarn(b);
        return false;
    }

    if (!m_art.beginFromDir(device, convertedGlbRoot())) {
        x3::logWarn("town: converted_glb root mount failed — no town");
        return false;
    }
    // NO MATERIAL OVERRIDES ARE NEEDED FOR THIS KIT, and that is the point.
    // Round one carried eleven of them here, one per HouseForge sub-material
    // that shipped at the glTF default 0.8 grey with no texture — a white slab
    // under a bright sun + ACES. The swap removed the need rather than the
    // symptom: tools/town_assets.py paints EVERY material of every asset from
    // the pack's own photographs and its `verify` subcommand fails the build if
    // any bound image is a placeholder, so there is no gap left to patch.
    // If a future kit reintroduces one, patch it HERE (env_art's
    // MaterialOverride exists for exactly this) rather than tinting a whole
    // asset, and add it to `verify` so it cannot come back silently.
    //
    // The metallic clamp stays: X3_WORLD_RULES rule 5 — an untextured
    // near-metal renders BLACK, and the clamp is the documented net. This kit's
    // own metal trim is authored at 0.55, so the clamp only ever catches a
    // regression.
    m_art.setMetallicClamp(0.35f);

    // Shared 1x1 white for the window panes: X3_WORLD_RULES rule 5 — a valid
    // mrTex is what routes an entity onto the PBR path at all.
    const uint8_t whitePx[4] = { 255, 255, 255, 255 };
    const uint8_t mrPx[4]    = { 255, 40, 0, 255 };   // G=roughness 0.16, B=metal 0
    x3::rhi::TextureHandle winTex = device.createTexture(whitePx, 1, 1, true);
    x3::rhi::TextureHandle winMr  = device.createTexture(mrPx, 1, 1, false);

    // ---- FOOTPRINT LEDGER: no two plots may overlap -------------------------
    struct Plot { float x, z, r; };
    std::vector<Plot> taken;
    taken.reserve(kLotCount + kPropCount + kParkCount);
    for (const KeepOut& k : cfg.keepOut) taken.push_back({ k.x, k.z, k.r });

    // Place one asset with its BBOX CENTRE at (wx, wz), front turned to face
    // `faceYaw`. Returns false when the terrain under the footprint is too
    // broken to stand a building on, or the plot is taken.
    auto place = [&](uint8_t ai, float wx, float wz, float faceYaw, float datumY,
                     bool collide, bool light, bool windows) -> bool {
        const AssetDef& A = kAssets[ai];
        const float a = faceYaw - A.frontDeg * kDeg;
        const float plotR = std::sqrt(A.hx * A.hx + A.hz * A.hz);
        for (const Plot& p : taken) {
            const float dx = p.x - wx, dz = p.z - wz;
            if (dx * dx + dz * dz < (p.r + plotR * 0.72f) * (p.r + plotR * 0.72f)) return false;
        }
        // FEET ON THE GROUND, MEASURED (X3_WORLD_RULES rule 4 / NO_SLOP 5):
        // sample the four rotated footprint corners and the centre, sit on the
        // LOWEST so nothing ever floats, and refuse a plot the hillside breaks.
        float lo = 1e9f, hi = -1e9f;
        for (int c = 0; c < 5; ++c) {
            const float sx = (c == 4) ? 0.0f : ((c & 1) ? A.hx : -A.hx);
            const float sz = (c == 4) ? 0.0f : ((c & 2) ? A.hz : -A.hz);
            float ox, oz; yawXZ(a, sx, sz, ox, oz);
            const float h = terrainHeightAtWorld(wx + ox, wz + oz);
            lo = std::min(lo, h); hi = std::max(hi, h);
        }
        if (hi - lo > 4.5f) return false;
        // THE TOWNSITE PLANE. A main street is GRADED: the shops stand at the
        // level of the pavement they front, not on whatever the raw hillside
        // does 20 m out. Corridors only ever CUT (road_network.h), so beside a
        // climbing spur the natural ground can sit metres ABOVE the datum and a
        // shop grounded on it disappears behind its own bank. Clamp the plot to
        // the street: never more than 1.2 m above the datum, free to follow the
        // ground DOWN (a hillside town steps down, it does not levitate).
        // Clamped BOTH ways. Round two clamped only the upper side and the
        // square's hero landed in a 9 m hollow below its own street (receipt:
        // the shop-front capture framed a roofline from above). A main street
        // is a graded pad: nothing more than 1.2 m above the pavement it fronts,
        // nothing more than 1.6 m below it.
        const float groundY = std::max(std::min(lo, datumY + 1.2f), datumY - 1.6f);
        // THE BURIED-BUILDING FIX (receipt: the first capture round, where only
        // roof spikes showed above the grass). The HouseForge prefabs put their
        // ORIGIN at the ground-FLOOR plane with 3-5 m of foundation/rock plinth
        // modelled BELOW it (kAssets loY), so planting the origin on the terrain
        // buried a third of every building. X3_WORLD_RULES rule 4 wants the
        // origin at the CONTACT surface, and for this kit the contact surface is
        // the bbox BOTTOM: lift by -loY so the plinth rests on the ground, then
        // sink 0.5 m so a slope shows no gap under the sill.
        const float baseY = groundY - A.loY - 0.50f;

        // The placement point is the ORIGIN, but the lot addresses the bbox
        // CENTRE, so back the asset's own centre offset out of it.
        float ocx, ocz; yawXZ(a, A.cx, A.cz, ocx, ocz);
        float T[16];
        yawMat(a, wx - ocx, baseY, wz - ocz, T);
        if (!m_art.addGlbInstance(A.glb, T)) return false;
        taken.push_back({ wx, wz, plotR * 0.72f });

        if (collide && std::min(A.hx, A.hz) >= kCollideMinHalfM) {
            // A yaw-rotated static box, the app/world_cars.cpp precedent. The
            // collider covers the ABOVE-GROUND span only ([0, loY+hiY] is the
            // buried plinth's business, and a collider reaching into the
            // hillside just fights the terrain body).
            const float hAbove = std::max(A.hiY - A.loY - 0.5f, 1.0f);
            const x3::phys::BodyId b = phys.addBox(
                x3::phys::Vec3{ A.hx - 0.35f, hAbove * 0.5f, A.hz - 0.35f },
                x3::phys::Vec3{ wx, groundY + hAbove * 0.5f, wz },
                0.0f, x3::phys::Layer::Static);
            const float q[4] = { 0.0f, std::sin(a * 0.5f), 0.0f, std::cos(a * 0.5f) };
            if (b.valid()) phys.setBodyRotation(b, q);
        }

        if (light) {
            // The lamp HEAD sits at the top of its bbox; the lantern is the
            // light, not the pole. Light_2 is a 7.16 m standard, so this hangs
            // the source a full storey above the street and the pool it throws
            // reaches the pavement on both sides — which a 2.3 m torch never
            // did. Range scales off the measured height rather than a constant
            // so a different lamp cannot silently under-light the street.
            x3::rhi::PointLight pl;
            pl.pos[0] = wx; pl.pos[1] = baseY + A.hiY - 0.25f; pl.pos[2] = wz;
            pl.range = std::max(22.0f, (A.hiY - A.loY) * 3.6f);
            // Warm sodium-white, not a flame: this is a highway town, and the
            // paired window practicals below already carry the warm end.
            pl.color[0] = 2.35f; pl.color[1] = 1.95f; pl.color[2] = 1.35f;
            m_lightAuthored.insert(m_lightAuthored.end(),
                                   { pl.color[0], pl.color[1], pl.color[2] });
            pl.color[0] *= m_night; pl.color[1] *= m_night; pl.color[2] *= m_night;
            m_lights.push_back(pl);
        }

        if (windows) {
            // TWO lit panes on the street-facing facade. The face point is the
            // bbox support in the front direction — measured from the same
            // table the placement uses, so a pane can never float in mid-air
            // ahead of a wall or hide inside one by more than a few cm.
            const float fx = -std::sin(A.frontDeg * kDeg);
            const float fz = -std::cos(A.frontDeg * kDeg);
            const float support = frontSupport(A);
            for (int w = 0; w < 3; ++w) {
                // WHERE THE WINDOWS ACTUALLY ARE. Round one used two fixed
                // storey heights (2.35 m / 5.20 m) because the ruined kit
                // modelled no glazing to measure. This kit models its windows
                // as a separate `Glass` material, so tools/town_assets.py
                // measures the centroids of the FRONT elevation's glass and
                // kAssets carries the result. It has to be per-asset: the kit's
                // houses run 9 m to 25.7 m tall, and one fixed pair would have
                // put House_2's upper pane inside its roof.
                const AssetDef::Pane& P = A.win[w];
                if (P.y <= 0.01f || P.hw <= 0.01f) continue;
                const float along = P.along;
                // The measured height is above the bbox BOTTOM; the pane is
                // positioned relative to the asset ORIGIN, and the two differ
                // by loY (X3_WORLD_RULES rule 4 — the contact surface is the
                // bbox bottom, which is what the placer grounds).
                const float paneY = A.loY + P.y;
                if (paneY > A.hiY - 0.4f) continue;
                // Local point: bbox centre + front * the MEASURED glass depth,
                // pulled 6 cm in so the pane is set INTO the facade. It used to
                // use the bbox SUPPORT — i.e. it assumed the front face of the
                // bounding box is the front wall. It is not: these houses have
                // deep eaves, and on House_1 the wall sits 6.9 m inside the
                // bbox front, so the panes hung in mid-air well proud of the
                // clapboard. Proud of the wall a pane floats; recessed, a miss
                // is simply invisible.
                const float lx = A.cx + fx * (P.depth - 0.06f) - fz * along;
                const float lz = A.cz + fz * (P.depth - 0.06f) + fx * along;
                float ox, oz; yawXZ(a, lx, lz, ox, oz);
                Entity e;
                // The pane is cut to the MEASURED opening (its own half width
                // and height), 6 cm deep so it reads as a recessed light rather
                // than a decal. A fixed 1.5 x 1.1 m quad was the other half of
                // the card-on-the-wall defect: sized to the window, an unlit
                // pane is invisible against the dark glass behind it.
                const x3::prims::PrimMesh pm =
                    x3::prims::makeBox(P.hw, P.hh, 0.03f, 0, 0, 0);
                e.mesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
                if (!e.mesh.valid()) continue;
                e.tex = winTex; e.mrTex = winMr;
                e.baseColor[0] = 0.06f; e.baseColor[1] = 0.05f; e.baseColor[2] = 0.035f;
                // ACES LAW (rule 5): flat emissive above ~0.5 clips to a white
                // slab. 0.42 over a near-black albedo reads as a lit room and
                // still feeds bloom; the WARMTH comes from the paired light.
                const float base = 0.42f;
                e.emissive[0] = 1.0f; e.emissive[1] = 0.62f; e.emissive[2] = 0.26f;
                e.emissive[3] = base * m_night;
                // The pane faces the street: its local +Z must point along the
                // front, so its own yaw is a + frontDeg = faceYaw.
                yawMat(faceYaw + 3.14159265f, wx + ox, baseY + paneY, wz + oz, e.transform);
                const uint32_t idx = scene.add(e);
                m_windows.push_back({ scene.handle(idx),
                                      { 1.0f, 0.62f, 0.26f }, base });
                // A practical behind every lit window — this is what actually
                // throws light on the pavement at dusk.
                x3::rhi::PointLight pl;
                pl.pos[0] = wx + ox * 1.15f; pl.pos[1] = baseY + paneY;
                pl.pos[2] = wz + oz * 1.15f;
                pl.range = 14.0f;
                pl.color[0] = 1.7f; pl.color[1] = 1.05f; pl.color[2] = 0.42f;
                m_lightAuthored.insert(m_lightAuthored.end(),
                                       { pl.color[0], pl.color[1], pl.color[2] });
                pl.color[0] *= m_night; pl.color[1] *= m_night; pl.color[2] *= m_night;
                m_lights.push_back(pl);
            }
            // The eye gate aims here: the point ON the facade, the direction it
            // faces, and how far back a camera must stand to frame it.
            float sfx, sfz;
            yawXZ(a, A.cx + fx * support, A.cz + fz * support, sfx, sfz);
            m_shopFronts.push_back({ wx - ocx + sfx, wz - ocz + sfz, groundY,
                                     faceYaw, std::max(A.hx, A.hz) });
        }
        return true;
    };

    // ---- the buildings ------------------------------------------------------
    uint32_t rejected = 0;
    for (uint32_t i = 0; i < kLotCount; ++i) {
        const Lot& L = kLots[i];
        const float u = u0 + (L.u - kAuthoredU0) * ((u1 - u0) / kAuthoredSpan);
        Station st;
        if (!stationAt(u, st)) continue;
        // `L.lat` is the FRONT-FACE setback; add the asset's own measured
        // front support to get the bbox-centre offset the placer wants. The
        // keep-out is therefore enforced where it matters — on the facade.
        const float lat = std::max(L.lat, kStreetKeepOutM) + frontSupport(kAssets[L.asset]);
        // left normal of the tangent (AXES LAW): rotate t by +90 deg about +Y.
        const float nx = st.tz, nz = -st.tx;
        const float wx = st.x + (float)L.side * lat * nx;
        const float wz = st.z + (float)L.side * lat * nz;
        // The front faces the street: the direction from the plot back to the
        // centreline, plus the plot's own jitter so no two facades are square.
        const float faceYaw = yawOf(-(float)L.side * nx, -(float)L.side * nz)
                            + L.jitterDeg * kDeg;
        const bool isBuilding = (L.asset <= A_LAST_BLDG);
        if (place(L.asset, wx, wz, faceYaw, st.y, true, false, isBuilding)) {
            if (isBuilding) ++m_buildings; else ++m_props;
            // The square's hero is where the shop-front gate points. Keyed on
            // the LOT INDEX, not on an asset id: with eight facades drawn from
            // four shells every asset repeats, and an id match would silently
            // aim at whichever plot came last.
            if (i == kHeroLot && !m_shopFronts.empty())
                m_heroFront = (uint32_t)m_shopFronts.size() - 1;
        } else {
            ++rejected;
        }
    }

    // ---- street furniture ---------------------------------------------------
    for (uint32_t i = 0; i < kPropCount; ++i) {
        const Lot& L = kProps[i];
        const float u = u0 + (L.u - kAuthoredU0) * ((u1 - u0) / kAuthoredSpan);
        Station st;
        if (!stationAt(u, st)) continue;
        const float lat = std::max(L.lat, kStreetKeepOutM) + frontSupport(kAssets[L.asset]);
        const float nx = st.tz, nz = -st.tx;
        const float wx = st.x + (float)L.side * lat * nx;
        const float wz = st.z + (float)L.side * lat * nz;
        const float faceYaw = yawOf(-(float)L.side * nx, -(float)L.side * nz)
                            + L.jitterDeg * kDeg;
        const bool lamp = (L.asset == A_LAMP);
        if (place(L.asset, wx, wz, faceYaw, st.y, false, lamp, false)) ++m_props;
    }

    // ---- parked cars --------------------------------------------------------
    if (m_carArt.beginFromDir(device, convertedGlbRoot())) {
        for (uint32_t i = 0; i < kParkCount; ++i) {
            const ParkLot& P = kParks[i];
            const float u = u0 + (P.u - kAuthoredU0) * ((u1 - u0) / kAuthoredSpan);
            Station st;
            if (!stationAt(u, st)) continue;
            const float nx = st.tz, nz = -st.tx;
            const float wx = st.x + (float)P.side * P.lat * nx;
            const float wz = st.z + (float)P.side * P.lat * nz;
            // Angle-parked: nose into the kerb, skewed off the street tangent.
            const float a = yawOf(st.tx, st.tz) + P.skewDeg * kDeg;
            // Sit on the topmost of terrain / street datum — the apron edge is
            // ABOVE the cut ground, and a car half-sunk in the verge is the
            // buried-entity strike this repo keeps taking.
            // Same townsite law as the buildings: never above the pavement it
            // parks beside, free to follow the verge down. The first round put
            // these on raw terrain and half of them sank into the batter.
            const float gy = std::min(terrainHeightAtWorld(wx, wz), st.y + 0.60f);
            float T[16]; yawMat(a, wx, gy, wz, T);
            if (!m_carArt.addGlbInstance(kCarGlb[P.car % kCarGlbCount], T)) continue;
            ++m_cars;
            const x3::phys::BodyId b = phys.addBox(
                x3::phys::Vec3{ 0.85f, 0.62f, 1.95f },
                x3::phys::Vec3{ wx, gy + 0.66f, wz }, 0.0f, x3::phys::Layer::Static);
            const float q[4] = { 0.0f, std::sin(a * 0.5f), 0.0f, std::cos(a * 0.5f) };
            if (b.valid()) phys.setBodyRotation(b, q);
        }
    }

    // ---- the sidewalk loop the pedestrians walk ----------------------------
    // Up the -side verge and back down the +side one: a closed circuit, so a
    // walker never has to stand at a dead end and turn around.
    {
        const float step = 26.0f;
        for (float u = u0 + 18.0f; u <= u1 - 18.0f; u += step) {
            Station st;
            if (!stationAt(u, st)) continue;
            m_loopX.push_back(st.x - kSidewalkLatM * st.tz);
            m_loopZ.push_back(st.z + kSidewalkLatM * st.tx);
        }
        for (float u = u1 - 18.0f; u >= u0 + 18.0f; u -= step) {
            Station st;
            if (!stationAt(u, st)) continue;
            m_loopX.push_back(st.x + kSidewalkLatM * st.tz);
            m_loopZ.push_back(st.z - kSidewalkLatM * st.tx);
        }
    }

    // Town centre = the square, for the map POI and for the report.
    {
        Station st{};
        m_uCentre = u0 + (kLots[kHeroLot].u - kAuthoredU0) * ((u1 - u0) / kAuthoredSpan);
        stationAt(m_uCentre, st);
        m_cx = st.x; m_cz = st.z; m_cy = st.y;
        m_dirX = st.tx; m_dirZ = st.tz;
    }

    char b[320];
    std::snprintf(b, sizeof(b),
        "town: %u buildings, %u props, %u parked cars, %u lit windows, %u lamps "
        "along %.0f m of main street (u %.0f..%.0f); centre (%.1f, %.1f, %.1f); "
        "%u lots rejected (slope/overlap)",
        m_buildings, m_props, m_cars, (uint32_t)m_windows.size(),
        (uint32_t)(m_lights.size() - m_windows.size()),
        (double)(u1 - u0), (double)u0, (double)u1,
        (double)m_cx, (double)m_cy, (double)m_cz, rejected);
    x3::logInfo(b);
    return m_buildings > 0;
}

uint32_t Town::spawnPedestrians(x3::rhi::IRenderDevice& device,
                                x3::phys::IPhysicsWorld& phys) {
    if (m_loopX.size() < 4) return 0;
    // THE TOWNSPEOPLE — and why this is not the crowd_skin roster.
    //
    // It used to be `CrowdSkin::defaultRigs()`: AnnaCasual_anim,
    // marcus_webb_anim, chief_martinez_anim. Rendering that roster
    // (tools/glb_contact_sheet.py over assets/rigged_glb) shows what it is —
    // a civilian woman, A CLAWED GREEN-VEINED MUTANT, and a black-clad SWAT
    // operator. Two of the three people strolling Main Street were a monster
    // and a special-forces officer, because crowd_skin.cpp:35 picks on "has
    // Idle/Walk/Run" and was cast for the CLUB scene, not for a mountain town.
    // It stayed invisible until the pedestrian eye gate finally framed a walker
    // close enough to see (NO_SLOP rule 2: eyes on, at full res).
    //
    // crowd_skin's roster is left ALONE — other worlds want those rigs. The
    // town casts its own, built by tools/town_people.py from the licensed
    // `City People FREE Samples` pack, which the 914-package index
    // (tools/unitypackage_index.py) turned up among the ~700 packs that had
    // never been extracted. Six ordinary people who live somewhere: two
    // casuals, two in outerwear, an elder and a kid — cycled so the street is
    // not six clones.
    //
    // Each carries Idle/Walk/Run under those EXACT names, which is what
    // townPedClipTable() below asks for and what AnimatedCharacter resolves by;
    // `python tools/town_people.py verify` asserts it, because the failure mode
    // is a silent sliding bind-pose statue rather than an error.
    static const char* const kRigs[] = {
        "CityPerson_ManCasual.glb",   "CityPerson_WomanCasual.glb",
        "CityPerson_ManJacket.glb",   "CityPerson_WomanCoat.glb",
        "CityPerson_Elder.glb",       "CityPerson_Boy.glb",
    };
    const CharacterClipTable table = townPedClipTable();
    const std::string dir = riggedGlbRoot();
    const uint32_t kWant = 6;
    const size_t L = m_loopX.size();

    for (uint32_t i = 0; i < kWant; ++i) {
        Ped p;
        p.rig = std::make_unique<AnimatedCharacter>();
        if (!p.rig->load(device, dir, kRigs[i % 3], table)) {
            x3::logWarn(std::string("town: pedestrian rig unavailable: ") + kRigs[i % 3]);
            continue;
        }
        // Spread them around the loop rather than stacking them at node 0.
        p.next = (uint32_t)((i * L) / kWant);
        const uint32_t start = (p.next + L - 1) % (uint32_t)L;
        const float sx = m_loopX[start], sz = m_loopZ[start];
        p.body = std::make_unique<Player>();
        p.body->spawn(phys, sx, terrainHeightAtWorld(sx, sz) + 0.35f, sz);
        m_peds.push_back(std::move(p));
    }
    char b[128];
    std::snprintf(b, sizeof(b), "town: %u pedestrians on a %u-node sidewalk loop",
                  (uint32_t)m_peds.size(), (uint32_t)L);
    x3::logInfo(b);
    return (uint32_t)m_peds.size();
}

void Town::update(float dt, x3::phys::IPhysicsWorld& phys,
                  x3::rhi::IRenderDevice& device, float camX, float camZ) {
    if (m_peds.empty()) return;
    // GATE: outside this radius the terrain tiles the walkers stand on are not
    // resident, so ticking them is a free-fall, a raycast and a log line each,
    // every frame, for something nobody can see.
    const float dcx = camX - m_cx, dcz = camZ - m_cz;
    if (dcx * dcx + dcz * dcz > kPedActiveM * kPedActiveM) return;

    const size_t L = m_loopX.size();
    for (Ped& p : m_peds) {
        if (!p.body || !p.rig) continue;
        const x3::phys::Vec3 f = p.body->feet();
        float tx = m_loopX[p.next % L] - f.x;
        float tz = m_loopZ[p.next % L] - f.z;
        float d = std::sqrt(tx * tx + tz * tz);
        if (d < 3.0f) {
            p.next = (uint32_t)((p.next + 1) % L);
            // A short pause at a corner — a crowd that never stops is a
            // conveyor belt.
            if ((p.next % 5) == 0) p.dwell = 1.6f;
            tx = m_loopX[p.next % L] - f.x;
            tz = m_loopZ[p.next % L] - f.z;
            d = std::sqrt(tx * tx + tz * tz);
        }
        const bool walking = (p.dwell <= 0.0f);
        if (p.dwell > 0.0f) p.dwell -= dt;

        // Player moves along its LOOK direction (device convention:
        // fwd = (cos y, ., sin y)), so aim the look at the waypoint and push
        // moveFwd — the capsule then walks and the rig faces its travel.
        const float lookYaw = (d > 1e-3f) ? std::atan2(tz / d, tx / d) : 0.0f;
        p.body->setLook(lookYaw, 0.0f);
        PlayerInput in;
        in.moveFwd = walking ? 1.0f : 0.0f;
        p.body->update(in, dt, phys);

        AnimatedCharacter::Intent it;
        it.moveFwd = in.moveFwd;
        p.rig->update(*p.body, it, lookYaw, dt, phys, device);
    }
}

uint32_t Town::draw(x3::rhi::IRenderDevice& device,
                    const x3::rhi::FrameContext& frame) const {
    uint32_t n = m_art.draw(device, frame);
    n += m_carArt.draw(device, frame);
    for (const Ped& p : m_peds) {
        if (p.rig && p.body) p.rig->draw(frame, device, *p.body, 0.0f, 0.0f, true);
    }
    return n;
}

void Town::setNight(float k) {
    m_night = std::min(std::max(k, 0.0f), 1.0f);
    if (!m_scene) return;
    for (Window& w : m_windows) {
        Entity* e = m_scene->getChecked(w.ent);
        if (e) e->emissive[3] = w.base * m_night;
    }
    // Rebuild each light from its AUTHORED colour every time, never from the
    // current one — scaling in place compounds and a second setNight(0.5) would
    // quarter the town (NO_SLOP rule 4: the pair is authored-colour + dial).
    for (size_t i = 0; i < m_lights.size() && i * 3 + 2 < m_lightAuthored.size(); ++i) {
        m_lights[i].color[0] = m_lightAuthored[i * 3 + 0] * m_night;
        m_lights[i].color[1] = m_lightAuthored[i * 3 + 1] * m_night;
        m_lights[i].color[2] = m_lightAuthored[i * 3 + 2] * m_night;
    }
}

void Town::setNightFromSun(float sunDirY) {
    // Sun elevation and the night dial are ONE VALUE (NO_SLOP rule 4). Full day
    // above 0.25, full night at or below 0.05, linear between — so the world's
    // standing noon sun (sunDir.y 0.92) gives 0 and the dusk gate's horizon sun
    // (0.055) gives ~0.97. Before this existed the dial defaulted to 1 and
    // nothing drove it, so the windows burned through every daylight capture.
    constexpr float kDayY   = 0.25f;
    constexpr float kNightY = 0.05f;
    const float t = (kDayY - sunDirY) / (kDayY - kNightY);
    setNight(std::min(std::max(t, 0.0f), 1.0f));
}

bool Town::showcaseCamera(int which, float out[5]) const {
    if (!m_built || m_st.size() < 2) return false;
    // Device camera convention: fwd = (cos yaw, ., sin yaw) — NOT the AXES-LAW
    // character yaw. ENGINE_GOTCHAS 4.1: keep the two apart or every shot looks
    // 90 degrees off.
    auto camYawTo = [](float dx, float dz) { return std::atan2(dz, dx); };
    Station st{};

    switch (which) {
    case 0: {   // MAIN STREET FROM THE ROAD — standing on the pavement, low,
                // looking up the street the way a driver arrives.
        if (!stationAt(std::max(m_uCentre - 62.0f, m_st.front().u + 8.0f), st)) return false;
        out[0] = st.x; out[1] = st.y + 2.05f; out[2] = st.z;
        out[3] = camYawTo(st.tx, st.tz);
        out[4] = 0.02f;
        return true;
    }
    case 1: {   // A SHOP FRONT, CLOSE. Stand off the biggest facade in town at
                // a distance that fills the frame, on its own front axis.
        if (m_shopFronts.empty()) return false;
        size_t best = 0;
        if (m_heroFront < m_shopFronts.size()) {
            best = m_heroFront;
        } else {
            for (size_t i = 1; i < m_shopFronts.size(); ++i)
                if (m_shopFronts[i].reach > m_shopFronts[best].reach) best = i;
        }
        const Anchor& a = m_shopFronts[best];
        // The facade faces engine-yaw a.faceYaw, i.e. direction
        // (-sin, -cos); stand out along it and look back.
        const float fx = -std::sin(a.faceYaw), fz = -std::cos(a.faceYaw);
        // Stand back far enough to hold the facade, high enough to look at the
        // BUILDING. At 2.6 m with a -0.06 pitch the first cut of this shot gave
        // 45% of the frame to blurred tarmac; the subject is the texture on the
        // wall, so the camera looks slightly UP at it from a standing height.
        const float d = a.reach * 0.80f + 6.0f;
        out[0] = a.x + fx * d; out[1] = a.y + 3.1f; out[2] = a.z + fz * d;
        out[3] = camYawTo(-fx, -fz);
        out[4] = 0.035f;
        return true;
    }
    case 2: {   // THE PEDESTRIANS. Derived from AN ACTUAL WALKER, not from the
                // loop polyline.
                //
                // The polyline version stepped 9 m back along the loop tangent
                // and grounded itself on raw terrain, and it put the camera on
                // a grass bank with the nearest walker a speck 60 m away — the
                // gate is supposed to prove the town is ALIVE and it proved a
                // field. A pedestrian's own position is the one place in this
                // town guaranteed to be standable ground, because THE CONTACT
                // LAW put it there and re-checks it every frame. So: stand a
                // few metres in front of the walker nearest the square, at its
                // own foot height, and look back at it.
        if (m_peds.empty()) return false;
        size_t best = 0; float bestD = 1e18f;
        for (size_t i = 0; i < m_peds.size(); ++i) {
            if (!m_peds[i].body) continue;
            const x3::phys::Vec3 f = m_peds[i].body->feet();
            const float dx = f.x - m_cx, dz = f.z - m_cz;
            const float d = dx * dx + dz * dz;
            if (d < bestD) { bestD = d; best = i; }
        }
        if (!m_peds[best].body) return false;
        const x3::phys::Vec3 f = m_peds[best].body->feet();
        // STAND WHERE THE WALKER IS GOING, not where it is.
        //
        // ENGINE_GOTCHAS 4.4: a still cannot prove motion. But the settle loop
        // runs the town for tens of frames BEFORE the grab, and this camera is
        // derived BEFORE the settle — so a walker framed at its spawn position
        // has walked out of shot by the time the shutter opens. That is why the
        // first two versions of this gate photographed an empty pavement with a
        // speck on the horizon. Take the pedestrian's OWN heading (bearing to
        // its next waypoint, not the street tangent — half the loop runs the
        // other way), stand a short distance down it and look back: the settle
        // then walks the subject INTO the frame instead of out of it.
        const Ped& pd = m_peds[best];
        float hx = 0.0f, hz = 0.0f;
        if (!pd.wpX.empty()) {
            const uint32_t w = pd.next % (uint32_t)pd.wpX.size();
            hx = pd.wpX[w] - f.x; hz = pd.wpZ[w] - f.z;
        }
        float hL = std::sqrt(hx * hx + hz * hz);
        if (hL < 1e-3f) { hx = m_dirX; hz = m_dirZ; hL = std::max(std::sqrt(hx * hx + hz * hz), 1e-3f); }
        hx /= hL; hz /= hL;
        // HOW FAR AHEAD: 20 m, and the number is MEASURED, not chosen.
        // The settle loop runs 60 frames before the grab and the authored gait
        // is 1.35 m/s, and the walker covers about 8.5 m in that time — which
        // the previous attempt found the hard way by standing exactly 8.5 m
        // ahead, so the subject walked into the camera and out of the frame
        // (the log line below is what proved it: the camera was dead-on and
        // 8.5 m away, and the pavement photographed empty). 20 m leaves the
        // walker ~11 m out when the shutter opens — filling a useful part of a
        // 720p frame — and the 2.6 m side-step turns it three-quarter-on
        // instead of a head-on silhouette.
        const float sx = -hz, sz = hx;     // sidestep, perpendicular to the path
        out[0] = f.x + hx * 20.0f + sx * 2.6f;
        out[2] = f.z + hz * 20.0f + sz * 2.6f;
        out[1] = f.y + 1.62f;              // the walker's own ground + eye height
        const float bx = f.x - out[0], bz = f.z - out[2];
        out[3] = camYawTo(bx, bz);         // look back along its path
        out[4] = -0.02f;
        {   char b[224];
            std::snprintf(b, sizeof(b),
                "town shot 3: walker %u feet=(%.1f, %.1f, %.1f) heading=(%.2f, %.2f); "
                "camera 20 m ahead on its path (it closes ~8.5 m during the settle)",
                (unsigned)best, (double)f.x, (double)f.y, (double)f.z,
                (double)hx, (double)hz);
            x3::logInfo(b); }
        return true;
    }
    case 3: {   // THE LIT WINDOWS. Three-quarter onto the square's facades,
                // FROM THE ROADWAY.
                //
                // This camera used to stand 30 m off the centreline on the
                // +side normal — and the +side lots sit at a bbox centre of
                // 26.9 m, so it stood INSIDE House_2_White and the whole frame
                // was the underside of somebody's roof. That is ENGINE_GOTCHAS
                // 4.1 happening to a camera that WAS derived from town data:
                // deriving is not enough, it has to be derived from geometry
                // that is guaranteed EMPTY. The roadway is the only such place
                // in this town — kStreetKeepOutM exists precisely to keep it
                // clear — so the shot is taken from the pavement, looking back
                // and across at the hero's facade the way a driver would see it.
        if (!stationAt(m_uCentre + 34.0f, st)) return false;
        const float nx = st.tz, nz = -st.tx;      // left normal
        // A third of the way to the kerb on the hero's side: still inside the
        // keep-out, so still guaranteed clear of every plot.
        // Stand on the FAR side of the centreline from the hero. Sitting on
        // the hero's own side put the camera on top of the road's jersey-wall
        // /guardrail furniture, which filled the left third of the frame with a
        // pale slab a couple of metres from the lens. Across the street is also
        // simply the right place to photograph a facade from.
        const float lat = kStreetKeepOutM * 0.42f;
        out[0] = st.x + nx * lat; out[2] = st.z + nz * lat;
        out[1] = st.y + 2.3f;
        // Look back down the street and across to the hero front, so the lit
        // panes, the lamps and the pavement they light are all in frame.
        float tx = -st.tx, tz = -st.tz;
        if (m_heroFront < m_shopFronts.size()) {
            const Anchor& a = m_shopFronts[m_heroFront];
            tx = a.x - out[0]; tz = a.z - out[2];
            const float L = std::max(std::sqrt(tx * tx + tz * tz), 1e-3f);
            tx /= L; tz /= L;
        }
        out[3] = camYawTo(tx, tz);
        out[4] = 0.01f;
        return true;
    }
    default: {  // THE TOWN FROM ACROSS THE VALLEY. Back off along the street's
                // own downhill and to the side, up high enough to see the roofs
                // against the mountain.
        if (!stationAt(m_st.front().u, st)) return false;
        const float nx = m_dirZ, nz = -m_dirX;
        out[0] = m_cx - m_dirX * 210.0f + nx * 150.0f;
        out[2] = m_cz - m_dirZ * 210.0f + nz * 150.0f;
        out[1] = terrainHeightAtWorld(out[0], out[2]) + 52.0f;
        const float tx = m_cx - out[0], tz = m_cz - out[2];
        out[3] = camYawTo(tx, tz);
        out[4] = -0.14f;
        return true;
    }
    }
}

void Town::shutdown(x3::rhi::IRenderDevice& device) {
    m_peds.clear();
    m_carArt.destroy(device);
    m_art.destroy(device);
    m_built = false;
}

} // namespace x3::game
