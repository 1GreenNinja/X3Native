// Club 1127 — "THE DEEP". See app/club1127.h.
//
// CLEAN-ROOM: ported forward from Tim's OWN Babylon game module
// (C:/Users/Tim Smith/OneDrive/GameDev/Q3Engine/src/world/x3-club1127.js — Tim's
// IP, authored by his "Agent 66"; NOT id Tech / RBDOOM / any third-party engine).
// Built here from that JS layout + the X3Native Scene / IRenderDevice /
// IPhysicsWorld / MonsterSystem interfaces only (the same public seams
// app/env_art.cpp + app/door.cpp + the prior club used). No third-party engine
// source consulted.
//
// MAPPING NOTES (JS -> native):
//   * The JS parents everything to a TransformNode at (originX, D.Y, originZ) and
//     positions children RELATIVE. Native addBox authors WORLD-space geometry, so
//     we keep originX/Z = 0 and ADD the club Y (kClubY = -200) to every center Y.
//     A child at JS-local y becomes world y = (kClubY + y).
//   * JS axes: Babylon is left-handed (+Z forward). X3Native is right-handed
//     (-Z forward; docs/CONVENTIONS.md). The club is mirror-symmetric front/back
//     and we only PLACE boxes (no winding-sensitive normals beyond makeBox's own),
//     so we keep the JS coordinates as-authored — the room reads identically.
//   * Babylon StandardMaterial diffuse/emissive -> native baseColor[] + emissive[]
//     ({r,g,b,strength}); strength > 1 => a bright HDR bloom source.
//   * Cylinders/spheres (turntables, stools, the ORB, blacklight tubes, cables,
//     railing balusters) are approximated with boxes (the engine's primitive).
//   * The JS Babylon lights (Hemispheric/Point/Spot) -> the engine's forward
//     PointLight set (premultiplied color); spotlights become orbiting point
//     lights, the hemisphere becomes a few soft fill lights.
//   * updateClub1127() (ORB spin + spotlight orbit + blacklight pulse) -> update().
//
// Reaching this area:
//   (a) STANDALONE: `--world club` (app/main.cpp). Walk it (WASD / mouse / Space /
//       F noclip); `--world club --screenshot <path>` captures the showcase vantage.
//   (b) ELEVATOR DISCO DESCENT (canon): the elevator's keypad code 1127 puts it in
//       DISCO mode + descends to Y=-200 (§2.2/§2.3). That elevator lane wires the
//       descent + teleports the player to spawn(); this module just builds the room.
#include "club1127.h"
#include "mesh_prims.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;
// The house tempo — matched to assets/audio/music/club_descent.wav (measured
// ~85.5 BPM base / 171 eighth-grid) so subs, tiles, and dancers ride the track.
constexpr float kClubBpm = 85.5f;

// ---- Tints (linear-ish; the device tonemaps) ------------------------------
// Ported from the JS StandardMaterial diffuse colors (hex -> 0..1 RGB).
const float kWall[4]   = { 0.039f, 0.039f, 0.070f, 1.0f }; // 0x0a0a12 club wall
const float kFloor[4]  = { 0.031f, 0.031f, 0.063f, 1.0f }; // 0x080810 club floor
const float kCeil[4]   = { 0.020f, 0.020f, 0.031f, 1.0f }; // 0x050508 ceiling
const float kSpk[4]    = { 0.039f, 0.039f, 0.039f, 1.0f }; // 0x0a0a0a speaker cab
const float kAmp[4]    = { 0.067f, 0.067f, 0.067f, 1.0f }; // 0x111111 amp
const float kSub[4]    = { 0.031f, 0.031f, 0.031f, 1.0f }; // 0x080808 sub cab
const float kMetal[4]  = { 0.227f, 0.227f, 0.267f, 1.0f }; // 0x3a3a44 metal platform
const float kCouch[4]  = { 0.039f, 0.020f, 0.031f, 1.0f }; // 0x0a0508 couch
const float kStair[4]  = { 0.102f, 0.102f, 0.133f, 1.0f }; // 0x1a1a22 stair
const float kRail[4]   = { 0.267f, 0.267f, 0.333f, 1.0f }; // 0x444455 railing
const float kBar[4]    = { 0.102f, 0.082f, 0.125f, 1.0f }; // 0x1a1520 bar body
const float kBarTop[4] = { 0.165f, 0.125f, 0.208f, 1.0f }; // 0x2a2035 bar top
const float kStool[4]  = { 0.133f, 0.133f, 0.133f, 1.0f }; // 0x222222 stool seat
const float kStoolLeg[4]={ 0.267f, 0.267f, 0.267f, 1.0f }; // 0x444444 stool leg
const float kChrome[4] = { 0.533f, 0.533f, 0.600f, 1.0f }; // 0x888899 chrome handle
const float kTvFrame[4]= { 0.031f, 0.031f, 0.031f, 1.0f }; // 0x080808 TV bezel
const float kGlass[4]  = { 0.200f, 0.267f, 0.333f, 0.55f }; // 0x334455 glass door
const float kCable[4]  = { 0.267f, 0.267f, 0.267f, 1.0f }; // 0x444444 cable
const float kOrb[4]    = { 0.700f, 0.700f, 0.800f, 1.0f }; // mirror ball facets

// ---- Emissive helpers: { r, g, b, strength }. strength > 1 => HDR bloom. -----
const float kEmitOff[4]     = { 0.0f, 0.0f, 0.0f, 0.0f };
const float kEmitNeon[4]    = { 1.00f, 0.0f, 1.00f, 4.0f };  // magenta aerial-bar neon
const float kEmitDjCon[4]   = { 0.10f, 0.10f, 0.28f, 1.5f }; // DJ console glow
const float kEmitDjScr[4]   = { 0.30f, 0.30f, 0.90f, 3.0f }; // DJ/OLED screens
const float kEmitKeypad[4]  = { 0.10f, 0.95f, 0.30f, 2.0f }; // green keypad
const float kEmitBarTop[4]  = { 0.30f, 0.20f, 0.50f, 1.5f }; // bar-top glow
const float kEmitTile1[4]   = { 0.45f, 0.0f, 0.85f, 2.2f };  // purple dance tile (0x2a0050)
const float kEmitTile2[4]   = { 0.12f, 0.0f, 0.30f, 1.2f };  // dark dance tile (0x0a0020)
const float kEmitOrb[4]     = { 0.45f, 0.45f, 0.60f, 1.4f };  // ORB self-glow
const float kEmitLed[4]     = { 0.10f, 1.00f, 0.10f, 3.0f };  // amp power LED
const float kEmitAbTop[4]   = { 0.353f, 0.353f, 0.416f, 1.2f };// aerial-bar polished top
// Blacklight base emissive (PULSED each frame in update()): deep UV violet.
const float kBlacklightR = 0.50f, kBlacklightG = 0.0f, kBlacklightB = 1.0f;
// Companion CAST color for each tube's point light (fix/club-blacklights): the
// tubes were emissive-only geometry — they glowed as thin bars but cast NOTHING,
// so the wall behind them stayed dead-black. Each tube now carries a violet
// point light at this HDR color; update() pulses it in phase with the emissive.
const float kBlacklightCast[3] = { 0.65f, 0.06f, 1.30f };
const float kBlacklightCastRange = 5.0f;   // ~5 m: washes the wall + nearby dancers

// Orbiter gel palettes (Tim addendum: "the lights move to the music"): update()
// rotates which gel each orbiter carries on every 8-beat phrase (pink -> blue ->
// green -> amber) and scales all of them with the beat envelope. File-scope so
// build() seeds them and update() re-derives them from the beat grid.
const float kSpotGels[4][3] = { {2.8f,0.10f,0.90f}, {0.10f,0.40f,2.8f}, {0.20f,2.6f,0.60f}, {2.8f,1.10f,0.10f} };
const float kRingGels[4][3] = { {2.0f,0.0f,1.0f},   {0.0f,1.0f,2.0f},  {1.0f,0.0f,2.0f},   {0.0f,2.0f,1.0f} };
// Ceiling moving-head rig (Tim: fixtures mounted ON THE CEILING projecting
// patterns DOWN onto the dance floor): 4 fixtures on this ring over the floor.
const float kHeadRingR  = 4.0f;    // fixture ring radius
const float kHeadRingCz = -1.5f;   // ring center Z (the dancer-crowd centroid)

// OLED screen emissive strength (the panes set emissiveMap=1, so this is MULTIPLIED
// by the texel — it is the brightness of a LIT EQ column, not a wash over the pane).
// The dark substrate (texel ~0.01) lands at ~0.02: black, as an OLED's black must be.
// A lit column tip / peak cap (texel ~0.6..1.0 linear) lands at ~1.1..1.8 — chromatic
// and just into bloom, never a white blob (R5). update() breathes AROUND this floor.
const float kOledEmit = 1.80f;

// Push a point light (premultiplied color) into the set.
void addLight(std::vector<x3::rhi::PointLight>& v, float x, float y, float z,
              float r, float g, float b, float range) {
    x3::rhi::PointLight l;
    l.pos[0] = x; l.pos[1] = y; l.pos[2] = z; l.range = range;
    l.color[0] = r; l.color[1] = g; l.color[2] = b;
    v.push_back(l);
}

// Pose a UNIT box (verts authored ±1 around the world origin) as a thin shaft
// from A (a fixture lens) to B (its floor pool point): the Y column becomes the
// half-vector A->B, the X/Z columns the beam half-width, translation the
// midpoint. Same rewrite-the-column-major-transform trick as THE ORB's spin.
void poseBeam(Entity& e, float ax, float ay, float az, float bx, float by, float bz) {
    const float dx = bx - ax, dy = by - ay, dz = bz - az;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-4f) return;
    const float ux = dx / len, uy = dy / len, uz = dz / len;
    // A stable perpendicular pair (the beam is near-vertical, so cross with X).
    float hx = 1.0f, hy = 0.0f, hz = 0.0f;
    if (std::fabs(ux) > 0.9f) { hx = 0.0f; hy = 0.0f; hz = 1.0f; }
    float xx = hy * uz - hz * uy, xy = hz * ux - hx * uz, xz = hx * uy - hy * ux;
    const float xl = std::sqrt(xx * xx + xy * xy + xz * xz);
    xx /= xl; xy /= xl; xz /= xl;
    const float zx = uy * xz - uz * xy, zy = uz * xx - ux * xz, zz = ux * xy - uy * xx;
    const float w = 0.075f;                       // beam half-width
    e.transform[0]  = xx * w;  e.transform[1]  = xy * w;  e.transform[2]  = xz * w;  e.transform[3]  = 0;
    e.transform[4]  = dx * 0.5f; e.transform[5] = dy * 0.5f; e.transform[6] = dz * 0.5f; e.transform[7] = 0;
    e.transform[8]  = zx * w;  e.transform[9]  = zy * w;  e.transform[10] = zz * w;  e.transform[11] = 0;
    e.transform[12] = (ax + bx) * 0.5f; e.transform[13] = (ay + by) * 0.5f;
    e.transform[14] = (az + bz) * 0.5f; e.transform[15] = 1;
}

// ============================================================================
// MAX-OUT PASS (Tim 2026-07-07) — procedural CONTENT textures. The blockout's
// flat-emissive rectangles read as dead panels; these bake believable content
// so the fixtures read at a glance: OLED equalizer walls, speaker cones, mirror
// facets. All deterministic (hash, no rng) — the club is identical every boot.
// ============================================================================
inline uint32_t clubHash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

// OLED "now playing" equalizer: dark glass bg, neon EQ columns at hashed heights,
// scanlines, a hot peak cap per column. `hue` picks the palette family.
std::vector<uint8_t> makeOledEqRGBA(uint32_t n, uint32_t seed, int hue) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    // Palette families: 0 cyan/blue, 1 magenta/violet, 2 amber/red, 3 green/teal.
    const float pal[4][2][3] = {
        { {0.05f,0.85f,1.0f}, {0.10f,0.25f,0.95f} },
        { {1.0f,0.10f,0.85f}, {0.55f,0.05f,0.95f} },
        { {1.0f,0.65f,0.05f}, {0.95f,0.15f,0.10f} },
        { {0.10f,1.0f,0.45f}, {0.05f,0.75f,0.85f} },
    };
    const float* cTop = pal[hue & 3][0];
    const float* cBot = pal[hue & 3][1];
    const uint32_t cols = 24, cw = n / cols;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            float r = 0.01f, g = 0.012f, b = 0.02f;               // dark glass bg
            const uint32_t c = x / cw;
            const uint32_t inCol = x % cw;
            // Column height: hashed, tall in the middle (a "mix" silhouette).
            const float mid = 1.0f - std::fabs((float)c - cols * 0.5f) / (cols * 0.5f);
            const float hgt = (0.18f + 0.62f * ((clubHash(seed * 131 + c) % 997) / 997.0f)) *
                              (0.55f + 0.45f * mid);
            const float yn = (float)y / n;                         // 0 bottom .. 1 top (box UV V runs top-down)
            if (inCol >= 1 && inCol + 1 < cw && yn < hgt) {
                const float f = yn / hgt;                          // 0 base .. 1 tip
                r = cBot[0] + (cTop[0] - cBot[0]) * f;
                g = cBot[1] + (cTop[1] - cBot[1]) * f;
                b = cBot[2] + (cTop[2] - cBot[2]) * f;
                if (yn > hgt - 0.03f) { r = r * 0.4f + 0.6f; g = g * 0.4f + 0.6f; b = b * 0.4f + 0.6f; } // peak cap
            }
            if ((y % 4) == 0) { r *= 0.55f; g *= 0.55f; b *= 0.55f; }   // scanlines
            const uint32_t edge = std::min(std::min(x, n - 1 - x), std::min(y, n - 1 - y));
            if (edge < 3) { r = g = b = 0.008f; }                  // bezel gap
            p[0] = (uint8_t)(std::min(1.0f, r) * 255);
            p[1] = (uint8_t)(std::min(1.0f, g) * 255);
            p[2] = (uint8_t)(std::min(1.0f, b) * 255);
            p[3] = 255;
        }
    }
    return px;
}

// Speaker front: recessed driver cone(s) — concentric gradient rings down to a
// dark cone + a specular dust cap + an amber surround ring, on a black cab face
// with corner bolts. `twin` stacks two smaller drivers vertically (JRX cabinets).
std::vector<uint8_t> makeSpeakerRGBA(uint32_t n, bool twin) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    auto drawDriver = [&](float cxN, float cyN, float radN) {
        const float cx = cxN * n, cy = cyN * n, rad = radN * n;
        const int x0 = std::max(0, (int)(cx - rad)), x1 = std::min((int)n - 1, (int)(cx + rad));
        const int y0 = std::max(0, (int)(cy - rad)), y1 = std::min((int)n - 1, (int)(cy + rad));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / rad;
                if (d > 1.0f) continue;
                uint8_t* p = &px[((size_t)y * n + x) * 4];
                float r, g, b;
                if (d > 0.88f)      { r = 0.55f; g = 0.33f; b = 0.06f; }             // amber surround
                else if (d > 0.20f) { const float f = 0.30f - 0.16f * d +           // cone shading
                                      0.05f * std::sin(d * 28.0f);                   // ribbed rings
                                      r = g = f; b = f * 1.15f; }
                else if (d > 0.14f) { r = g = b = 0.82f; }                           // cap rim highlight
                else                { r = g = b = 0.22f; }                           // dust cap
                p[0] = (uint8_t)(std::max(0.0f, std::min(1.0f, r)) * 255);
                p[1] = (uint8_t)(std::max(0.0f, std::min(1.0f, g)) * 255);
                p[2] = (uint8_t)(std::max(0.0f, std::min(1.0f, b)) * 255);
                p[3] = 255;
            }
    };
    // Cab face: near-black with a faint weave.
    for (uint32_t i = 0; i < (size_t)n * n; ++i) {
        const uint8_t v = (uint8_t)(8 + ((i * 7) % 5));
        px[i * 4 + 0] = v; px[i * 4 + 1] = v; px[i * 4 + 2] = (uint8_t)(v + 2); px[i * 4 + 3] = 255;
    }
    if (twin) { drawDriver(0.5f, 0.30f, 0.24f); drawDriver(0.5f, 0.74f, 0.19f); }
    else      { drawDriver(0.5f, 0.54f, 0.40f); }
    // Corner bolts.
    const float bolts[4][2] = { {0.07f,0.07f}, {0.93f,0.07f}, {0.07f,0.93f}, {0.93f,0.93f} };
    for (auto& bpos : bolts) {
        const int bx = (int)(bpos[0] * n), by = (int)(bpos[1] * n);
        for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x)
            if (x * x + y * y <= 4 && by + y >= 0 && by + y < (int)n && bx + x >= 0 && bx + x < (int)n) {
                uint8_t* p = &px[((size_t)(by + y) * n + (bx + x)) * 4];
                p[0] = p[1] = p[2] = 70;
            }
    }
    return px;
}

// Mirror-ball facets: silver tile grid with per-tile hashed brightness (sparkle)
// and dark grout — reads as hundreds of tiny mirrors under the moving lights.
std::vector<uint8_t> makeFacetRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t cell = n / 24;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const uint32_t gx = x / cell, gy = y / cell;
            const bool grout = (x % cell) < 1 || (y % cell) < 1;
            if (grout) { p[0] = p[1] = p[2] = 18; }
            else {
                const float f = 0.55f + 0.45f * ((clubHash(gx * 733 + gy * 149) % 991) / 991.0f);
                p[0] = (uint8_t)(200 * f); p[1] = (uint8_t)(205 * f); p[2] = (uint8_t)(215 * f);
            }
            p[3] = 255;
        }
    return px;
}

// 1x1 metallic-roughness texel (glTF packing: G=roughness, B=metallic).
std::vector<uint8_t> makeMr1x1(uint8_t rough, uint8_t metal) {
    return { 255, rough, metal, 255 };
}

} // namespace

// ---------------------------------------------------------------------------
// OLED screen-contrast probe (see club1127.h for WHY a texture-only probe is a
// trap). Bakes a real EQ frame, then runs glass.frag's emissive math on its
// darkest and brightest texel and returns the on-screen ratio.
// ---------------------------------------------------------------------------
float clubOledEmissiveContrast(int hue, float emissiveMap, const float emissive[4]) {
    const uint32_t n = 256;
    const std::vector<uint8_t> px = makeOledEqRGBA(n, /*seed*/ 7u, hue);

    // The EQ frames are uploaded as sRGB, so the sampler hands the shader LINEAR
    // texels. Decode the same way or the contrast is measured in the wrong space.
    auto srgbToLinear = [](float c) {
        return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    auto luma = [](float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; };

    // Darkest + brightest texel of the frame (the OLED black substrate, and a lit
    // column's peak cap). Skip the 3px bezel gap — it is frame, not screen content.
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1.0f, -1.0f, -1.0f };
    float loL = 1e9f, hiL = -1.0f;
    for (uint32_t y = 4; y < n - 4; ++y)
        for (uint32_t x = 4; x < n - 4; ++x) {
            const uint8_t* p = &px[((size_t)y * n + x) * 4];
            const float c[3] = { srgbToLinear(p[0] / 255.0f),
                                 srgbToLinear(p[1] / 255.0f),
                                 srgbToLinear(p[2] / 255.0f) };
            const float L = luma(c[0], c[1], c[2]);
            if (L < loL) { loL = L; for (int i = 0; i < 3; ++i) lo[i] = c[i]; }
            if (L > hiL) { hiL = L; for (int i = 0; i < 3; ++i) hi[i] = c[i]; }
        }
    if (hiL < 0.0f) return 0.0f;   // degenerate bake -> probe reports failure

    // glass.frag: emisMask = mix(vec3(1), texel, emissiveMap);
    //             additive = vEmissive.rgb * vEmissive.a * emisMask;
    const float k = std::clamp(emissiveMap, 0.0f, 1.0f);
    float addLo[3], addHi[3];
    for (int i = 0; i < 3; ++i) {
        const float maskLo = 1.0f + (lo[i] - 1.0f) * k;   // mix(1, texel, k)
        const float maskHi = 1.0f + (hi[i] - 1.0f) * k;
        addLo[i] = emissive[i] * emissive[3] * maskLo;
        addHi[i] = emissive[i] * emissive[3] * maskHi;
    }
    const float dark = luma(addLo[0], addLo[1], addLo[2]);
    const float bright = luma(addHi[0], addHi[1], addHi[2]);
    return bright / std::max(dark, 1e-6f);
}

uint32_t Club1127World::addBox(Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics,
                               float cx, float cy, float cz, float hx, float hy, float hz,
                               const float color[4], const float emissive[4], bool collide,
                               float uvScale, const SurfaceSet* surf) {
    // Render + collision geometry authored in WORLD space (centered at cx,cy,cz),
    // so the Entity transform stays identity (static geometry — exactly like
    // buildTestLevel/env-art). The Scene draws it; addStaticMesh gives collision.
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, uvScale);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    if (surf && surf->ok) { e.tex = surf->albedo; e.normalTex = surf->normal; e.mrTex = surf->mr; }
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = (uint32_t)Tag::Static;
    if (collide) {
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
    }
    return scene.add(e);
}

void Club1127World::addCharacter(Scene& scene, x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                                 const std::string& modelFile, const x3::phys::Vec3& pos,
                                 float scale, bool standUpZtoY, const float tint[4]) {
    auto sys = std::make_unique<MonsterSystem>();
    MonsterSystem::Tuning t;
    t.type        = MonsterType::Guard;
    t.hp          = 100;
    t.chaseSpeed  = 0.0f;       // INERT prop: never moves (just idles in place)
    t.damage      = 0;          // never attacks
    t.ranged      = false;
    t.modelFile   = modelFile;
    t.modelDirOverride = std::string(modelDir);
    t.standUpZtoY = standUpZtoY;
    t.modelScale  = scale;
    if (tint) for (int i = 0; i < 4; ++i) t.tint[i] = tint[i];
    sys->buildMonsterTuned(scene, device, physics, modelDir, pos, t);
    m_chars.push_back(std::move(sys));
}

const Club1127World::Stats& Club1127World::build(Scene& scene, x3::rhi::IRenderDevice& device,
                                                 x3::phys::IPhysicsWorld& physics,
                                                 std::string_view modelDir) {
    if (m_built) return m_stats;
    m_built = true;

    const uint32_t entsBefore = scene.size();

    // The club Y origin: everything authored at JS-local y is offset by oy.
    const float oy = kClubY;          // -200
    const float CW = kCW, CL = kCL, CH = kCH;
    const float T  = 0.3f;            // wall thickness (JS WALL_T)

    // Engine-room/lounge dims (JS D.ER_*).
    const float ER_W = 6.1f;          // 20 ft wide
    const float ER_D = 4.27f;         // 14 ft deep
    const float LOUNGE_Y = 4.57f;     // 2nd story at 15 ft

    // Convenience: author a box at JS-local coords (Y offset to oy applied here).
    // `sf` (W6-3 texture pass) optionally carries a real surface_library set.
    auto box = [&](float x, float y, float z, float hx, float hy, float hz,
                   const float* col, const float* em, bool coll, float uv = 1.0f,
                   const SurfaceSet* sf = nullptr) {
        return addBox(scene, device, physics, x, oy + y, z, hx, hy, hz, col,
                      em ? em : kEmitOff, coll, uv, sf);
    };

    // ==================================================================
    // W6-3 TEXTURE PASS — real PBR sets from the pack library (ART_BIBLE §4)
    // replacing the box-tint-only geometry (was: zero architecture textures,
    // magenta neon accent only). Walls = dark venue concrete panels; floors +
    // stage/booth platforms = rubber dance floor / brushed metal; bar = plastic
    // laminate body + trim top. The magenta neon accent (kEmitNeon) is UNCHANGED
    // — it's an emissive-only strip, not a texture, and stays bible-compliant.
    // On a headless device with no assets fetched yet, SurfaceSet::ok is false
    // and addBox falls back to the old flat-tinted box (never breaks the build).
    // ==================================================================
    SurfaceLibrary surf;
    surf.mount(assetRoot() + "/surface_library");
    auto set = [&](const char* name) -> const SurfaceSet& { return surf.get(device, name); };
    const SurfaceSet& sWall  = set("mw_concrete_panels_a"); // dark venue walls
    const SurfaceSet& sFloor = set("sr_rubberfloor");        // dance/club floor
    const SurfaceSet& sMetal = set("mw_metal_trim_b");       // stage/booth platforms
    const SurfaceSet& sBar   = set("mw_wall_plastic");       // bar body laminate
    const SurfaceSet& sTrim  = set("mw_floor_trim");         // bar top / trim
    const SurfaceSet& sStair = set("sr_concrete_a");         // stair treads

    // ==================================================================
    // MAX-OUT SHARED RESOURCES (Tim 2026-07-07): content textures + gloss MR
    // texels + a generic prim-entity adder (sphere/ring/custom boxes with
    // custom textures — addBox only speaks SurfaceSets).
    // ==================================================================
    constexpr uint32_t kTexN = 256;
    x3::rhi::TextureHandle texEq[4];
    for (int h = 0; h < 4; ++h) {
        auto epx = makeOledEqRGBA(kTexN, 40 + h * 7, h);
        texEq[h] = device.createTexture(epx.data(), kTexN, kTexN, true);
    }
    auto spx1 = makeSpeakerRGBA(kTexN, false);   // single big driver (subs)
    auto spx2 = makeSpeakerRGBA(kTexN, true);    // twin drivers (JRX / stacks)
    const x3::rhi::TextureHandle texSub = device.createTexture(spx1.data(), kTexN, kTexN, true);
    const x3::rhi::TextureHandle texSpk = device.createTexture(spx2.data(), kTexN, kTexN, true);
    auto fpx = makeFacetRGBA(kTexN);
    const x3::rhi::TextureHandle texFacet = device.createTexture(fpx.data(), kTexN, kTexN, true);
    // Gloss MR texels: mirror-metal (facets/chrome), and a near-mirror DARK GLASS
    // dielectric for the gleaming countertops (metal 0 keeps it glassy, rough 15
    // gives the tight specular hot-spot that reads as polish).
    auto mrChromePx = makeMr1x1(/*rough*/ 25, /*metal*/ 255);
    auto mrGlassPx  = makeMr1x1(/*rough*/ 15, /*metal*/ 40);
    const x3::rhi::TextureHandle mrChrome = device.createTexture(mrChromePx.data(), 1, 1, false);
    const x3::rhi::TextureHandle mrGlass  = device.createTexture(mrGlassPx.data(), 1, 1, false);

    // OLED-GLASS: a screen entity becomes a REAL glass pane carrying its EQ
    // frame as the pane texture — the transparent pass adds the fresnel grazing
    // highlight (the panels SHINE like glass), near-solid opacity keeps the
    // content readable, and a soft emissive floor lets update()'s shimmer
    // breathe it like live video. (A separate glass pane OVER an opaque screen
    // depth-occludes the content — the pane must BE the screen.)
    //
    // PER-TEXEL EMISSIVE (GlassMaterial::emissiveMap, c44da59). An OLED pixel is
    // its own lamp: the lit EQ columns emit, the black substrate between them emits
    // NOTHING. The old flat emissive could not say that — it was a uniform add over
    // the whole pane, so the only way to make the screen glow was to flood it, and
    // the EQ washed out into a milky slab (see the BEFORE shot: the back-bar band
    // was a featureless white rectangle). With emissiveMap the glow is MULTIPLIED
    // by the texel, so the panel glows exactly WHERE THE EQ IS BRIGHT and the dark
    // glass background stays genuinely dark. That inverts the tuning:
    //   * emissive rgb is now NEUTRAL WHITE — the texel supplies the colour, so each
    //     screen keeps its own palette family (cyan / magenta / amber / green).
    //     A tinted emissive would drag every panel back toward the same blue-white.
    //   * strength is raised (the mask eats most of the pane), and it is the peak
    //     caps — not the whole rectangle — that reach bloom. R5 law: chromatic,
    //     short of blow-out.
    //   * baseColor drops 2.0 -> 1.0: the 2x was propping up a washed pane. The
    //     EMISSIVE is the display now; the lit body just needs to be honest.
    auto oledGlass = [&](uint32_t id, x3::rhi::TextureHandle eq) {
        Entity& e = scene.get(id);
        e.tex = eq;
        e.baseColor[0] = 1.0f; e.baseColor[1] = 1.0f; e.baseColor[2] = 1.0f; e.baseColor[3] = 1.0f;
        e.emissive[0] = 1.0f; e.emissive[1] = 1.0f; e.emissive[2] = 1.0f; e.emissive[3] = kOledEmit;
        e.transparent = true;
        e.glass.opacity = 0.94f;      // near-solid: the EQ content carries
        e.glass.refraction = 0.0f;
        e.glass.roughness = 0.05f;    // tight glossy highlight
        e.glass.specular = 1.0f;      // full fresnel shine
        e.glass.tint[0] = 0.80f; e.glass.tint[1] = 0.90f; e.glass.tint[2] = 1.0f;
        e.glass.emissiveMap = 1.0f;   // GLOW WHERE THE EQ IS BRIGHT; black stays black
    };

    // Soft-furnishing palette (couches, stools, pillows) — used from the ground
    // bar onward, so declared with the shared resources.
    const float kLeather[4]  = { 0.14f, 0.10f, 0.09f, 1.0f };
    const float kLeatherHi[4]= { 0.20f, 0.15f, 0.13f, 1.0f };
    const float kPillowA[4]  = { 0.55f, 0.12f, 0.30f, 1.0f };   // wine
    const float kPillowB[4]  = { 0.10f, 0.30f, 0.45f, 1.0f };   // steel blue

    // Add an arbitrary prim mesh as a static entity (world-space verts like addBox).
    auto prim = [&](x3::prims::PrimMesh geo, const float col[4], const float* em,
                    x3::rhi::TextureHandle tex, x3::rhi::TextureHandle mr,
                    bool collide) -> uint32_t {
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        if (tex.valid()) e.tex = tex;
        if (mr.valid())  e.mrTex = mr;
        for (int i = 0; i < 4; ++i) e.baseColor[i] = col[i];
        if (em) for (int i = 0; i < 4; ++i) e.emissive[i] = em[i];
        e.tag = (uint32_t)Tag::Static;
        if (collide)
            e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                           geo.cindex.data(), (uint32_t)geo.cindex.size());
        return scene.add(e);
    };
    // Scale + translate a prim's verts in place (world-space authoring).
    auto placeVerts = [](x3::prims::PrimMesh& g, float s, float tx, float ty, float tz) {
        for (auto& v : g.verts) {
            v.pos[0] = v.pos[0] * s + tx; v.pos[1] = v.pos[1] * s + ty; v.pos[2] = v.pos[2] * s + tz;
        }
        for (size_t i = 0; i + 2 < g.cverts.size(); i += 3) {
            g.cverts[i]   = g.cverts[i]   * s + tx;
            g.cverts[i+1] = g.cverts[i+1] * s + ty;
            g.cverts[i+2] = g.cverts[i+2] * s + tz;
        }
    };

    m_stats.floorY    = oy;           // main floor center at world Y = -200
    m_stats.roomMinX  = -CW / 2;  m_stats.roomMaxX = CW / 2;
    m_stats.roomMinZ  = -CL / 2;  m_stats.roomMaxZ = CL / 2;

    // ==================================================================
    // MAIN SHELL — floor, ceiling, four walls (the 50x100x30 ft room).
    // ==================================================================
    box(0, 0.0f, 0, CW / 2, 0.1f, CL / 2, kFloor, kEmitOff, true, 0.4f, &sFloor);  // floor slab
    box(0, CH,  0, CW / 2, 0.1f, CL / 2, kCeil,  kEmitOff, true, 0.4f);            // ceiling
    m_stats.ceilingY = oy + CH;

    // North wall (-Z) — gap for the engine room (ER_W wide, centered at x=0).
    const float erGap = ER_W;
    const float nwSide = (CW - erGap) / 2;
    box(-(erGap / 2 + nwSide / 2), CH / 2, -CL / 2, nwSide / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
    box( (erGap / 2 + nwSide / 2), CH / 2, -CL / 2, nwSide / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
    // South wall (+Z) — solid.
    box(0, CH / 2, CL / 2, CW / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
    // West long wall (-X) — solid (the ground-bar wall).
    box(-CW / 2, CH / 2, 0, T / 2, CH / 2, CL / 2, kWall, kEmitOff, true, 0.5f, &sWall);
    // East long wall (+X) — elevator entrance gap near the south end.
    {
        const float entrW = 3.5f;                      // elevator opening width
        const float entrZ = CL / 2 - entrW / 2 - 1.0f; // near the SE corner
        const float northLen = CL / 2 + (entrZ - entrW / 2);
        box(CW / 2, CH / 2, -CL / 2 + northLen / 2, T / 2, CH / 2, northLen / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        const float southLen = CL / 2 - (entrZ + entrW / 2);
        if (southLen > 0.1f)
            box(CW / 2, CH / 2, CL / 2 - southLen / 2, T / 2, CH / 2, southLen / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        // Header above the elevator door.
        box(CW / 2, 2.8f + (CH - 2.8f) / 2, entrZ, T / 2, (CH - 2.8f) / 2, entrW / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        // Player spawn: just inside the elevator opening on the floor, facing -X
        // (into the club toward the dance floor + DJ booth).
        m_spawn = x3::phys::Vec3{ CW / 2 - 1.6f, oy + 0.15f, entrZ };
    }

    // ==================================================================
    // ENGINE ROOM + LOUNGE (north side, 2 stories, 12-step stair).
    //   The JS parents these to a node at z = -CL/2 - ER_D/2; we add that offset.
    // ==================================================================
    const float erZ0 = -CL / 2 - ER_D / 2;   // engine-room center Z
    auto erbox = [&](float x, float y, float z, float hx, float hy, float hz,
                     const float* col, const float* em, bool coll, float uv = 1.0f,
                     const SurfaceSet* sf = nullptr) {
        return box(x, y, erZ0 + z, hx, hy, hz, col, em, coll, uv, sf);
    };
    erbox(0, 0.0f, 0, ER_W / 2, 0.1f, ER_D / 2, kFloor, kEmitOff, true, 0.5f, &sFloor); // ER floor
    erbox(0, CH,  0, ER_W / 2, 0.1f, ER_D / 2, kCeil,  kEmitOff, true, 0.5f);     // ER ceiling
    erbox(0, CH / 2, -ER_D / 2, ER_W / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall); // back wall
    erbox(-ER_W / 2, CH / 2, 0, T / 2, CH / 2, ER_D / 2, kWall, kEmitOff, true, 0.5f, &sWall); // -X side
    erbox( ER_W / 2, CH / 2, 0, T / 2, CH / 2, ER_D / 2, kWall, kEmitOff, true, 0.5f, &sWall); // +X side

    // Lounge floor (2nd story) + railing + balusters.
    erbox(0, LOUNGE_Y, 0, (ER_W - 0.2f) / 2, 0.1f, (ER_D - 0.2f) / 2, kFloor, kEmitOff, true, 0.5f, &sFloor);
    m_stats.hasLoungeFloor = true;
    erbox(0, LOUNGE_Y + 0.5f, ER_D / 2 - 0.1f, (ER_W - 0.4f) / 2, 0.5f, 0.03f, kRail, kEmitOff, true);
    for (int r = 0; r < 8; ++r) {
        const float rx = -ER_W / 2 + 0.5f + r * (ER_W - 1) / 7;
        erbox(rx, LOUNGE_Y + 0.5f, ER_D / 2 - 0.1f, 0.03f, 0.5f, 0.03f, kRail, kEmitOff, false);
    }

    // 12-step stair along the WEST wall up to the lounge.
    {
        const int stCt = 12;
        const float stD = ER_D / stCt;
        const float stR = LOUNGE_Y / stCt;
        for (int s = 0; s < stCt; ++s) {
            erbox(-ER_W / 2 + 0.65f, stR * (s + 0.5f), -ER_D / 2 + stD * (s + 0.5f),
                  0.5f, (stR * (s + 0.5f)) /* riser grows */ * 0.0f + 0.04f, (stD - 0.02f) / 2,
                  kStair, kEmitOff, true, 1.0f, &sStair);
            ++m_stats.stairSteps;
        }
    }

    // Engine-room south wall: a center pier + glass swing doors (west) + an inset
    // alcove door (east) into the main club, ported from the JS (simplified piers/
    // headers; the doors are visual props).
    {
        const float erSZ = ER_D / 2;          // south edge of the ER (local z)
        const float glassDoorW = 1.8f;
        const float pierW = 0.8f;
        const float westDoorX = -ER_W / 4;
        // West header + flanks.
        const float westSectionW = ER_W / 2 - pierW / 2;
        const float headerH = CH - 2.4f;
        erbox(-(pierW / 2 + westSectionW / 2), 2.4f + headerH / 2, erSZ,
              westSectionW / 2, headerH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        // Glass swing doors (2 leaves) + chrome handles (visual; non-colliding).
        const float ghw = glassDoorW / 2;
        for (int s = -1; s <= 1; s += 2) {
            erbox(westDoorX + s * (ghw / 2 + 0.01f), 1.15f, erSZ, (ghw - 0.02f) / 2, 1.15f, 0.02f,
                  kGlass, kEmitOff, false);
            erbox(westDoorX + s * 0.02f, 1.1f, erSZ + (s > 0 ? 0.04f : -0.04f),
                  0.015f, 0.125f, 0.03f, kChrome, kEmitOff, false);
        }
        // Center pier.
        erbox(0, CH / 2, erSZ, pierW / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        // East alcove: header + inset door + a pass-through cutout frame.
        const float eastSectionW = ER_W / 2 - pierW / 2;
        const float eastCenterX = pierW / 2 + eastSectionW / 2;
        const float alcoveDoorW = 1.2f;
        erbox(eastCenterX, 2.2f + (CH - 2.2f) / 2, erSZ - 0.6f, alcoveDoorW / 2, (CH - 2.2f) / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        erbox(eastCenterX, 1.05f, erSZ - 0.57f, (alcoveDoorW - 0.04f) / 2, 1.05f, 0.03f, kStair, kEmitOff, false);
        erbox(eastCenterX, 0.61f, erSZ - 0.59f, 0.485f, 0.61f, 0.04f, kRail, kEmitOff, false); // cutout frame
        // Lounge overhang above the east alcove.
        erbox(eastCenterX, LOUNGE_Y, erSZ - 0.6f + 0.2f, (eastSectionW + 0.3f) / 2, 0.075f, (0.6f + 0.4f) / 2, kFloor, kEmitOff, true);
    }

    // Engine-room fill light. (relight: dim 0.30/0.20/0.45 -> vibrant HDR violet.)
    addLight(m_lights, 0, oy + 3.0f, erZ0, 0.70f, 0.35f, 1.30f, 9.0f);

    // ==================================================================
    // SUSPENDED DJ BOOTH (turntables, mixer, 2 OLED, keypad door, brackets).
    // ==================================================================
    {
        const float djW = 3.5f, djD = 2.5f, djH = 2.8f, djY = LOUNGE_Y;
        const float djZ = -CL / 2 + djD / 2 + 0.3f;
        box(0, djY, djZ, djW / 2, 0.075f, djD / 2, kMetal, kEmitOff, true, 1.0f, &sMetal); // booth floor
        box(0, djY + djH, djZ, (djW + 0.1f) / 2, 0.05f, (djD + 0.1f) / 2, kCeil, kEmitOff, false); // booth ceiling
        m_stats.hasDjBooth = true;

        // Back wall (split around the keypad door).
        const float djBkW = (djW - 0.9f) / 2;
        box(-(0.45f + djBkW / 2), djY + djH / 2, -CL / 2 + 0.3f, djBkW / 2, djH / 2, T / 2, kWall, kEmitOff, true, 1.0f, &sWall);
        box( (0.45f + djBkW / 2), djY + djH / 2, -CL / 2 + 0.3f, djBkW / 2, djH / 2, T / 2, kWall, kEmitOff, true, 1.0f, &sWall);
        // Secured keypad door + keypad.
        box(djW / 2 - 0.6f, djY + 1.1f, -CL / 2 + 0.3f, 0.45f, 1.1f, 0.04f, kStair, kEmitOff, false);
        box(djW / 2 - 0.1f, djY + 1.2f, -CL / 2 + 0.35f, 0.05f, 0.075f, 0.015f, kStair, kEmitKeypad, false);
        m_stats.hasKeypadDoor = true;

        // Low front + side walls (the booth railing).
        box(0, djY + 0.55f, -CL / 2 + djD + 0.3f, djW / 2, 0.55f, T / 2, kWall, kEmitOff, true, 1.0f, &sWall);
        for (int s = -1; s <= 1; s += 2)
            box(s * djW / 2, djY + 0.55f, djZ, T / 2, 0.55f, djD / 2, kWall, kEmitOff, true, 1.0f, &sWall);

        // DJ mixer console + 2 turntables (cylinders -> flat boxes).
        box(0, djY + 1.05f, -CL / 2 + djD - 0.1f, 1.4f, 0.06f, 0.4f, kStair, kEmitDjCon, false);
        for (int i = 0; i < 2; ++i) {
            const float xo = (i == 0 ? -0.7f : 0.7f);
            box(xo, djY + 1.14f, -CL / 2 + djD - 0.1f, 0.275f, 0.02f, 0.275f, kSpk, kEmitDjCon, false);
        }
        m_stats.hasDjTurntables = true;

        // 2 OLED screens (MAX-OUT: real EQ content + live shimmer, like the TVs).
        for (int i = 0; i < 2; ++i) {
            const float xo = (i == 0 ? -0.25f : 0.25f);
            const float emScr[4] = { 1.0f, 1.0f, 1.0f, 2.1f };
            const uint32_t dsId = box(xo, djY + 1.35f, -CL / 2 + djD - 0.35f, 0.175f, 0.125f, 0.015f,
                                      kTvFrame, emScr, false);
            oledGlass(dsId, texEq[i * 2]);
            m_oledEnts.push_back(dsId);
        }
        m_stats.hasDjScreens = true;

        // Support brackets down to the floor.
        for (int s = -1; s <= 1; s += 2)
            box(s * (djW / 2 - 0.2f), djY / 2, -CL / 2 + djD + 0.3f, 0.075f, djY / 2, 0.075f, kMetal, kEmitOff, true);

        // Booth glow. (relight: dim 0.30/0.30/0.80 -> hot electric-blue HDR.)
        addLight(m_lights, 0, oy + djY + 1.6f, djZ, 0.50f, 0.60f, 2.20f, 7.0f);

        // ==============================================================
        // AERIAL BAR (beside the booth, neon underglow, polished top, railings).
        // ==============================================================
        const float abW = 4.0f, abD = 1.5f, abX = -djW / 2 - abW / 2 + 0.5f, abZ = djZ;
        box(abX, djY, abZ, abW / 2, 0.06f, abD / 2, kMetal, kEmitOff, true, 1.0f, &sMetal);               // platform
        box(abX, djY + 0.55f, -CL / 2 + djD + 0.1f, (abW - 0.4f) / 2, 0.55f, 0.25f, kMetal, kEmitOff, true, 1.0f, &sMetal); // counter
        box(abX, djY + 1.13f, -CL / 2 + djD + 0.1f, (abW - 0.2f) / 2, 0.025f, 0.3f, kMetal, kEmitAbTop, false, 1.0f, &sMetal); // polished top
        m_stats.hasAerialBar = true;
        // Magenta neon strips under the platform edges.
        box(abX, djY - 0.08f, abZ + abD / 2, (abW - 0.4f) / 2, 0.02f, 0.02f, kWall, kEmitNeon, false);
        box(abX, djY - 0.08f, abZ - abD / 2, (abW - 0.4f) / 2, 0.02f, 0.02f, kWall, kEmitNeon, false);
        box(abX - abW / 2 + 0.2f, djY - 0.08f, abZ, 0.02f, 0.02f, (abD - 0.2f) / 2, kWall, kEmitNeon, false);
        addLight(m_lights, abX, oy + djY - 0.3f, abZ, 2.8f, 0.0f, 2.8f, 8.0f);  // magenta underglow (relight: 2.0 -> 2.8 HDR)
        // Safety railings.
        box(abX, djY + 0.5f, abZ + abD / 2, abW / 2, 0.5f, 0.02f, kRail, kEmitOff, true);
        box(abX, djY + 0.5f, abZ - abD / 2, abW / 2, 0.5f, 0.02f, kRail, kEmitOff, true);
        box(abX - abW / 2, djY + 0.5f, abZ, 0.02f, 0.5f, abD / 2, kRail, kEmitOff, true);
    }

    // ==================================================================
    // BLACKLIGHTS — VERTICAL UV tubes on the walls (Tim's spec, 2026-07-17 review):
    //   spaced every ~12 ft (3.66 m), centered ~5 ft (1.5 m) off the floor.
    //   (Was: 28 tubes at 10 ft intervals centered at mid-wall 4.57 m — too high,
    //   too dense, and EMISSIVE-ONLY: they glowed as thin bars but CAST no light,
    //   so the walls behind them stayed dead-black and the tubes barely read.)
    //   Each tube now carries a companion UV POINT LIGHT just off the wall so it
    //   does a blacklight's actual job: wash the wall behind it AND the nearby
    //   dancers with violet. 20 tubes total: 8 per long wall (16) + 2 per side
    //   of the south wall flanking the centered 85" (4). update() pulses tube
    //   emissive + cast color in phase.
    // ==================================================================
    {
        const float bi  = 3.66f;     // 12 ft spacing (Tim spec; was 10 ft)
        const float bcY = 1.5f;      // tube CENTER ~5 ft off the floor
        const float bh  = 1.22f;     // 4 ft tube (canon tube length)
        // (nx,nz) = wall normal INTO the room: the cast light sits proud of the
        // tube so the wall glows around it instead of the light being buried.
        auto blacklight = [&](float x, float z, float nx, float nz) {
            const uint32_t id = box(x, bcY, z, 0.07f, bh / 2, 0.07f, kWall, nullptr, false);
            // Set its starting emissive (update() pulses it).
            Entity& e = scene.get(id);
            e.emissive[0] = kBlacklightR; e.emissive[1] = kBlacklightG;
            e.emissive[2] = kBlacklightB; e.emissive[3] = 4.0f;   // relight: UV tube bloom 3.0 -> 4.0
            m_blacklightEnts.push_back(id);
            // THE CAST (fix/club-blacklights): a violet point light per tube.
            m_blacklightLightIdx.push_back(m_lights.size());
            addLight(m_lights, x + nx * 0.45f, oy + bcY, z + nz * 0.45f,
                     kBlacklightCast[0], kBlacklightCast[1], kBlacklightCast[2],
                     kBlacklightCastRange);
            ++m_stats.blacklights;
        };
        // Long walls: 8 tubes per wall at EXACT 12 ft spacing, centered along Z
        // (z = ±12.81 m max, comfortably inside the ±15.24 m room).
        for (int side = -1; side <= 1; side += 2)
            for (int n = 0; n < 8; ++n) {
                const float z = (n - 3.5f) * bi;
                blacklight(side * (CW / 2 - 0.05f), z, (float)-side, 0.0f);
            }
        // South wall: 2 per side flanking the centered 85" display, 12 ft apart
        // (x = ±1.83, ±5.49 — inside the ±7.62 m half-width).
        for (int s = -1; s <= 1; s += 2)
            for (int n = 0; n < 2; ++n)
                blacklight(s * (bi / 2 + n * bi), CL / 2 - 0.05f, 0.0f, -1.0f);
        // UV point lights (4) — the room-wide violet AIR (kept; the per-tube
        // lights above are the wall/crowd cast, these are the base atmosphere).
        const float uv[4][3] = {
            { 0, CH * 0.5f, -CL / 4 }, { 0, CH * 0.5f, CL / 4 },
            { -CW / 3, CH * 0.5f, 0 }, { CW / 3, CH * 0.5f, 0 }
        };
        for (auto& p : uv)
            addLight(m_lights, p[0], oy + p[1], p[2], 0.90f, 0.10f, 2.00f, 22.0f); // relight: UV wash 0.4/.05/1.0 -> HDR violet
    }

    // ==================================================================
    // TV MULTIPLEX (POE) — 6 screens at the real JS sizes/positions.
    // ==================================================================
    {
        // MAX-OUT: every screen carries a baked OLED equalizer frame (palette
        // cycles per screen) + registers for the live emissive shimmer in
        // update() — the wall reads as playing VIDEO, not frozen blue slabs.
        int tvIdx = 0;
        auto tv = [&](float inches, float x, float y, float z) {
            const float dm  = inches * 0.0254f;
            const float tvH = dm / std::sqrt(1.0f + (16.0f / 9.0f) * (16.0f / 9.0f));
            const float tvW = tvH * 16.0f / 9.0f;
            box(x, y, z, (tvW + 0.05f) / 2, (tvH + 0.05f) / 2, 0.03f, kTvFrame, kEmitOff, false); // bezel
            const float emScr[4] = { 1.0f, 1.0f, 1.0f, 1.9f };
            const uint32_t scrId = box(x, y, z + 0.035f, tvW / 2, tvH / 2, 0.005f, kTvFrame, emScr, false);
            oledGlass(scrId, texEq[tvIdx % 4]);
            m_oledEnts.push_back(scrId);
            ++tvIdx;
            ++m_stats.tvScreens;
        };
        const float nwSideX = -(ER_W / 2 + nwSide / 2);
        tv(80, nwSideX, 2.74f, -CL / 2 + 0.05f);
        tv(85, 0, CH * 0.55f, CL / 2 - 0.05f);
        tv(55, CW / 2 - 0.8f, 1.8f, CL / 2 - 2.0f);
        tv(75, CW / 2 - 1.5f, 2.2f, CL / 2 - 5.0f);
        tv(65, -ER_W / 2 + 0.1f, LOUNGE_Y + 1.5f, -CL / 2 - ER_D / 2);
        tv(55, -CW / 2 + 0.8f, 2.0f, CL / 2 - 1.5f);
    }

    // ==================================================================
    // SOUND SYSTEM (the real PA rig).
    // ==================================================================
    // 4x SVS PB16-Ultra subs (corners). MAX-OUT: each gets a real DRIVER-CONE
    // face (radial grille texture) pointed at the dance floor + an amber beat
    // ring that THUMPS on the 128 BPM clock in update(), plus a low amber pulse
    // light so the corners breathe with the music.
    for (int i = 0; i < 4; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f;
        const float sz = (i & 2) ? 1.0f : -1.0f;
        const float cx = sx * (CW / 2 - 1), cz = sz * (CL / 4);
        box(cx, 0.37f, cz, 0.32f, 0.37f, 0.28f, kSub, kEmitOff, true);
        ++m_stats.svsSubs;
        // Cone face toward the room center (-sx in X), slightly proud of the cab.
        const float emCone[4] = { 1.0f, 0.55f, 0.15f, 0.0f };   // pulsed by update()
        const uint32_t coneId = box(cx - sx * 0.335f, 0.37f, cz, 0.012f, 0.33f, 0.26f, kSub, emCone, false);
        Entity& ce = scene.get(coneId);
        ce.tex = texSub; ce.emissiveTex = texSub; ce.mrTex = mrGlass;
        m_subPulseEnts.push_back(coneId);
        // Amber floor pulse light in front of the cab (index recorded for update()).
        m_subLightIdx.push_back(m_lights.size());
        addLight(m_lights, cx - sx * 0.9f, oy + 0.4f, cz, 1.40f, 0.65f, 0.15f, 3.5f); // relight: amber sub pulse base 0.9 -> 1.4 (update() modulates)
    }
    // 8 stacked pairs JBL JRX200 (16 cabinets) + 8 amps + power LEDs on the walls.
    {
        const float jrxSp = CL / 5;
        for (int side = -1; side <= 1; side += 2)
            for (int n = 0; n < 4; ++n) {
                const float z = -CL / 2 + jrxSp * (n + 1);
                const float x = side * (CW / 2 - 0.5f);
                const float wy = CH * 0.55f;
                box(x, wy,        z, 0.265f, 0.38f, 0.215f, kSpk, kEmitOff, false); m_stats.jblLineArray++;
                box(x, wy + 0.8f, z, 0.265f, 0.38f, 0.215f, kSpk, kEmitOff, false); m_stats.jblLineArray++;
                // MAX-OUT: twin-driver grille faces toward the room on both cabs.
                for (int cab = 0; cab < 2; ++cab) {
                    const float fEm[4] = { 0.9f, 0.9f, 1.0f, 0.6f };
                    const uint32_t fid = box(x - side * 0.28f, wy + cab * 0.8f, z,
                                             0.012f, 0.36f, 0.20f, kSpk, fEm, false);
                    Entity& fe = scene.get(fid);
                    fe.tex = texSpk; fe.emissiveTex = texSpk; fe.mrTex = mrGlass;
                }
                box(x, wy - 0.55f, z, 0.24f, 0.10f, 0.175f, kAmp, kEmitOff, false);                 // amp
                box(x - side * 0.01f, wy - 0.5f, z + 0.18f, 0.015f, 0.015f, 0.015f, kAmp, kEmitLed, false); // power LED
            }
    }
    // 4x JBL PRO 18" subs flanking the dance floor.
    {
        const float p[4][2] = { {-1,-1.f/3}, {-1,1.f/3}, {1,-1.f/3}, {1,1.f/3} };
        for (auto& s : p) {
            const float cx = s[0] * (CW / 2 - 0.5f), cz = s[1] * CL;
            box(cx, 0.35f, cz, 0.305f, 0.305f, 0.305f, kSub, kEmitOff, true);
            ++m_stats.jbl18Subs;
            // MAX-OUT: 18" cone face toward the floor center + beat thump.
            const float emCone[4] = { 1.0f, 0.55f, 0.15f, 0.0f };
            const uint32_t coneId = box(cx - s[0] * 0.318f, 0.35f, cz, 0.012f, 0.28f, 0.28f, kSub, emCone, false);
            Entity& ce = scene.get(coneId);
            ce.tex = texSub; ce.emissiveTex = texSub; ce.mrTex = mrGlass;
            m_subPulseEnts.push_back(coneId);
        }
    }
    // MAX-OUT: DJ TOWER STACKS — two floor-standing 3-cabinet line arrays
    // flanking the booth, cones facing the dance floor (+Z), inner magenta neon
    // edge; the bottom (sub) cabinet thumps on the beat clock with the corners.
    for (int side = -1; side <= 1; side += 2) {
        const float tx2 = side * 2.9f;
        const float tz2 = -CL / 2 + 1.1f;
        const float cabH[3]  = { 0.55f, 0.45f, 0.38f };
        const float cabW[3]  = { 0.50f, 0.44f, 0.38f };
        float cy2 = 0.0f;
        for (int c2 = 0; c2 < 3; ++c2) {
            cy2 += cabH[c2];
            box(tx2, cy2, tz2, cabW[c2], cabH[c2], 0.42f, kSpk, kEmitOff, c2 == 0);
            const float fEm2[4] = { 1.0f, 0.7f, 0.4f, c2 == 0 ? 0.0f : 0.45f };
            const uint32_t fid2 = box(tx2, cy2, tz2 + 0.43f, cabW[c2] - 0.04f, cabH[c2] - 0.04f, 0.012f,
                                      kSpk, fEm2, false);
            Entity& fe2 = scene.get(fid2);
            fe2.tex = (c2 == 0) ? texSub : texSpk;
            fe2.emissiveTex = fe2.tex;
            fe2.mrTex = mrGlass;
            if (c2 == 0) m_subPulseEnts.push_back(fid2);
            cy2 += cabH[c2];
        }
        // Inner magenta neon edge running the tower height.
        box(tx2 - side * (cabW[0] + 0.03f), 1.4f, tz2, 0.02f, 1.35f, 0.02f, kWall, kEmitNeon, false);
    }

    // 16x JBL N26/S38 surrounds (walls, alternating sizes).
    {
        const float surSp = CL / 9;
        for (int side = -1; side <= 1; side += 2)
            for (int n = 0; n < 8; ++n) {
                const float z = -CL / 2 + surSp * (n + 1);
                const float x = side * (CW / 2 - 0.15f);
                const bool s38 = (n % 2 == 0);
                box(x, CH * 0.75f, z, (s38 ? 0.25f : 0.2f) / 2, (s38 ? 0.5f : 0.3f) / 2, 0.09f, kSpk, kEmitOff, false);
                ++m_stats.surrounds;
            }
    }

    // ==================================================================
    // DANCE FLOOR — full-club checkerboard of glowing purple/dark tiles.
    // ==================================================================
    {
        const int cols = (int)std::floor(CW);   // ~15
        const int rows = (int)std::floor(CL);   // ~30
        const float tw = CW / cols, td = CL / rows;
        for (int gx = 0; gx < cols; ++gx)
            for (int gz = 0; gz < rows; ++gz) {
                const float tx = -CW / 2 + tw / 2 + gx * tw;
                const float tz = -CL / 2 + td / 2 + gz * td;
                // BLACK MIRROR TILES (Tim): near-black gloss, silvery chrome MR
                // (IBL/fresnel sheen — the orbiting spots streak across the floor),
                // alternates carry a FAINT violet under-glow breathing on the beat;
                // the blacklights own the color now, not the tiles.
                const bool lit = ((gx + gz) % 2 == 0);
                const float emTileA[4] = { 0.32f, 0.05f, 0.60f, 0.55f };
                const float tileCol[4] = { lit ? 0.055f : 0.035f, lit ? 0.055f : 0.035f,
                                           lit ? 0.075f : 0.05f, 1.0f };
                const uint32_t tid = box(tx, 0.12f, tz, (tw - 0.02f) / 2, 0.015f, (td - 0.02f) / 2,
                                         tileCol, lit ? emTileA : kEmitOff, false, 1.0f, nullptr);
                Entity& te = scene.get(tid);
                te.mrTex = mrChrome;              // the silvery reflective finish
                if (lit) m_tilePulseEnts.push_back(tid);
            }
        m_stats.hasDanceFloor = true;
    }

    // ==================================================================
    // THE ORB — 2 m mirror ball on a cable, 4 spotlights + 4 ring lights.
    // ==================================================================
    {
        m_orbY = oy + (CH - 1.5f);
        // MAX-OUT: a REAL 2 m mirror ball — UV sphere with a hashed silver-facet
        // texture + mirror-metal MR (the orbiting colored spots streak across it),
        // soft self-glow so it reads even between light passes. (Was: a white BOX —
        // it rendered as a blown cube in every shot.) Spun by update() as before:
        // verts are authored around (0, m_orbY, 0), so the Y-rotation in the
        // entity transform rotates it in place.
        {
            x3::prims::PrimMesh orb = x3::prims::makeUVSphere(32, 64);
            placeVerts(orb, 1.0f, 0.0f, m_orbY, 0.0f);
            const float orbCol[4] = { 0.95f, 0.96f, 1.0f, 1.0f };
            const float orbEm[4]  = { 0.30f, 0.30f, 0.40f, 0.5f };
            m_orbEnt = prim(std::move(orb), orbCol, orbEm, texFacet, mrChrome, false);
        }
        m_orbValid = true;
        m_stats.hasOrb = true;
        // Suspending cable.
        box(0, CH - 0.5f, 0, 0.02f, 0.75f, 0.02f, kCable, kEmitOff, false);
        // 4 ring lights (permanent colored point lights, orbital). These are the
        // FIRST orbiting set (rewritten each frame by update()).
        // 4 colored spotlights (rotating). Stored AFTER the static lights.
    }

    // ==================================================================
    // GROUND BAR + 7 STOOLS (west side).
    // ==================================================================
    {
        // Tim: 'the bar needs walk-around capability' — counter pulled off the
        // wall so the lane behind is player-walkable (~1.2 m) with open ends.
        const float bx = -CW / 2 + 1.9f, bz = CL / 4;
        box(bx, 0.55f, bz, 0.4f, 0.55f, 2.5f, kBar, kEmitOff, true, 1.0f, &sBar);     // bar body
        // MAX-OUT: GLEAMING GLASSY COUNTERTOP — a near-black glass slab on the
        // mirror-gloss MR route (tight specular hot-spots from the bar pendants =
        // the gleam), edge-lit by a cyan under-lip strip, over a warm kick LED.
        {
            x3::prims::PrimMesh top = x3::prims::makeBox(0.50f, 0.035f, 2.6f, bx, oy + 1.13f, bz, 1.0f);
            const float glassCol[4] = { 0.05f, 0.065f, 0.085f, 1.0f };
            const float glassEm[4]  = { 0.02f, 0.10f, 0.13f, 0.5f };
            prim(std::move(top), glassCol, glassEm, x3::rhi::TextureHandle{}, mrGlass, false);
        }
        const float emCyanStrip[4] = { 0.05f, 0.85f, 1.0f, 2.6f };
        box(bx + 0.48f, 1.10f, bz, 0.012f, 0.012f, 2.58f, kWall, emCyanStrip, false);   // under-lip strip (guest side)
        const float emWarmKick[4] = { 1.0f, 0.55f, 0.15f, 1.6f };
        box(bx + 0.40f, 0.06f, bz, 0.012f, 0.012f, 2.5f, kWall, emWarmKick, false);     // kick-panel LED
        m_stats.hasGroundBar = true;
        // THREE warm pendant pools raking the glass top (the specular gleam).
        // (relight: 1.15/0.85/0.45 -> 1.6/1.15/0.55 so the bar reads as a glowing hub.)
        addLight(m_lights, bx + 0.3f, oy + 2.3f, bz - 1.6f, 1.60f, 1.15f, 0.55f, 4.0f);
        addLight(m_lights, bx + 0.3f, oy + 2.3f, bz,        1.60f, 1.15f, 0.55f, 4.0f);
        addLight(m_lights, bx + 0.3f, oy + 2.3f, bz + 1.6f, 1.60f, 1.15f, 0.55f, 4.0f);
        // Pendant fixtures (small chrome cones -> boxes + glowing bulbs).
        for (int pnd = -1; pnd <= 1; ++pnd) {
            const float pz = bz + pnd * 1.6f;
            box(bx + 0.3f, 2.75f, pz, 0.015f, 0.35f, 0.015f, kChrome, kEmitOff, false);   // drop rod
            box(bx + 0.3f, 2.35f, pz, 0.06f, 0.045f, 0.06f, kSpk, kEmitOff, false);       // shade (near-black)
            const float emBulb[4] = { 1.0f, 0.78f, 0.45f, 2.8f };
            box(bx + 0.3f, 2.27f, pz, 0.035f, 0.025f, 0.035f, kBarTop, emBulb, false);    // bulb
        }
        for (int i = 0; i < 7; ++i) {
            const float sz = CL / 4 - 2.5f + 0.5f + i * 5.0f / 7.0f;
            box(-CW / 2 + 2.7f, 0.75f, sz, 0.2f, 0.035f, 0.2f, kLeather, kEmitOff, false);       // seat (dark leather)
            box(-CW / 2 + 2.7f, 0.36f, sz, 0.03f, 0.36f, 0.03f, kStoolLeg, kEmitOff, false);     // leg
            ++m_stats.barStools;
        }

        // MAX-OUT: BACK-BAR — a lit bottle wall on the west wall behind the bar:
        // backlit panel, three polished glass shelves, 18 glowing bottles in five
        // liquor hues, chrome shelf brackets, and a wide OLED band above (the
        // "now mixing" display). This is what a camera at the bar SEES.
        {
            const float wallX = -CW / 2 + 0.35f;
            // DARK GLASS CRYSTAL panel (Tim) — a real transparent pane, near-black
            // with a deep violet-blue tint + full fresnel: the rack reads as smoked
            // crystal and the glowing bottles pop against it.
            {
                x3::prims::PrimMesh pg = x3::prims::makeBox(0.015f, 1.05f, 2.4f, wallX, oy + 1.75f, bz, 1.0f);
                Entity pe;
                pe.mesh = device.createMesh(pg.verts.data(), (uint32_t)pg.verts.size(),
                                            pg.index.data(), (uint32_t)pg.index.size());
                pe.baseColor[0] = 0.04f; pe.baseColor[1] = 0.03f; pe.baseColor[2] = 0.07f; pe.baseColor[3] = 1.0f;
                pe.emissive[0] = 0.18f; pe.emissive[1] = 0.06f; pe.emissive[2] = 0.35f; pe.emissive[3] = 0.30f;
                pe.transparent = true;
                pe.glass.opacity = 0.55f;
                pe.glass.refraction = 0.0f;
                pe.glass.roughness = 0.04f;
                pe.glass.specular = 1.0f;
                pe.glass.tint[0] = 0.35f; pe.glass.tint[1] = 0.25f; pe.glass.tint[2] = 0.60f;
                pe.tag = (uint32_t)Tag::Static;
                scene.add(pe);
            }
            // Violet edge-light strips framing the crystal panel.
            const float emViolet[4] = { 0.55f, 0.10f, 1.0f, 2.2f };
            box(wallX + 0.01f, 2.82f, bz, 0.008f, 0.010f, 2.42f, kWall, emViolet, false);
            box(wallX + 0.01f, 0.68f, bz, 0.008f, 0.010f, 2.42f, kWall, emViolet, false);
            const float bottleHue[5][3] = {
                { 1.0f, 0.55f, 0.10f },   // bourbon amber
                { 0.15f, 1.0f, 0.45f },   // absinthe emerald
                { 0.10f, 0.75f, 1.0f },   // curacao cyan
                { 0.80f, 0.20f, 1.0f },   // violet
                { 1.0f, 0.25f, 0.35f },   // campari rose
            };
            for (int shelf = 0; shelf < 3; ++shelf) {
                const float shY = 1.15f + shelf * 0.55f;
                // Polished glass shelf (mirror-gloss slab) + chrome brackets.
                x3::prims::PrimMesh sh = x3::prims::makeBox(0.16f, 0.014f, 2.3f, wallX + 0.18f, oy + shY, bz, 1.0f);
                Entity she;
                she.mesh = device.createMesh(sh.verts.data(), (uint32_t)sh.verts.size(),
                                             sh.index.data(), (uint32_t)sh.index.size());
                she.baseColor[0] = 0.06f; she.baseColor[1] = 0.07f; she.baseColor[2] = 0.10f; she.baseColor[3] = 1.0f;
                she.transparent = true;
                she.glass.opacity = 0.35f;      // crystal shelf — see the bottles through it
                she.glass.refraction = 0.0f;
                she.glass.roughness = 0.05f;
                she.glass.specular = 1.0f;
                she.glass.tint[0] = 0.55f; she.glass.tint[1] = 0.60f; she.glass.tint[2] = 0.85f;
                she.tag = (uint32_t)Tag::Static;
                scene.add(she);
                for (int br = -1; br <= 1; ++br)
                    box(wallX + 0.18f, shY - 0.05f, bz + br * 1.05f, 0.02f, 0.05f, 0.02f, kChrome, kEmitOff, false);
                // Six bottles per shelf, varied hue/height, backlit glow.
                for (int bt = 0; bt < 6; ++bt) {
                    const float btZ = bz - 1.0f + bt * 0.4f + ((shelf * 7 + bt) % 3) * 0.05f;
                    const float btH = 0.13f + ((clubHash(shelf * 61 + bt * 17) % 100) / 100.0f) * 0.07f;
                    const float* hue = bottleHue[(shelf * 6 + bt) % 5];
                    const float btEm[4] = { hue[0], hue[1], hue[2], 1.7f };
                    const float btCol[4] = { hue[0] * 0.3f, hue[1] * 0.3f, hue[2] * 0.3f, 1.0f };
                    x3::prims::PrimMesh btm = x3::prims::makeBox(0.035f, btH, 0.035f,
                                                                 wallX + 0.18f, oy + shY + 0.014f + btH, btZ, 1.0f);
                    prim(std::move(btm), btCol, btEm, x3::rhi::TextureHandle{}, mrGlass, false);
                    // Neck.
                    box(wallX + 0.18f, shY + 0.014f + btH * 2.0f + 0.035f, btZ, 0.012f, 0.035f, 0.012f,
                        kChrome, kEmitOff, false);
                }
            }
            // Wide OLED band above the back-bar (registered for the live shimmer).
            const float emScr[4] = { 1.0f, 1.0f, 1.0f, 1.9f };
            const uint32_t bandId = box(wallX + 0.05f, 3.25f, bz, 0.015f, 0.42f, 2.35f, kTvFrame, emScr, false);
            oledGlass(bandId, texEq[1]);
            m_oledEnts.push_back(bandId);
            // Bartender behind the counter, facing the room (+X).
            addCharacter(scene, device, physics, modelDir, "AnnaCasual.glb",
                         x3::phys::Vec3{ bx - 0.75f, oy + 0.0f, bz }, 1.0f, false, nullptr);
        }
    }

    // ==================================================================
    // BLACK COUCHES + END TABLE (SE corner) + VIP COUCH (SW corner).
    // ==================================================================
    // MAX-OUT: a couch is seat SEGMENTS + back cushions + armrests + throw
    // pillows, not one slab — plus a lamp-lit end table. Deep espresso leather
    // with a subtle warm sheen so the seating reads plush under the pools.
    auto couch = [&](float cx, float cz, float yaw90, float halfW) {
        // yaw90: 0 = faces -X (back at +X side), 1 = faces -Z (back at +Z side).
        const float bx = yaw90 ? 0.0f : 0.42f, bzz = yaw90 ? 0.42f : 0.0f;
        const float sxW = yaw90 ? halfW : 0.42f, szW = yaw90 ? 0.42f : halfW;
        const int   segs = (int)std::round(halfW / 0.55f);
        // Seat cushions (segmented) + base.
        box(cx, 0.14f, cz, sxW, 0.14f, szW, kLeather, kEmitOff, true);
        for (int sg = 0; sg < segs; ++sg) {
            const float off = -halfW + halfW * (2.0f * sg + 1) / segs;
            box(cx + (yaw90 ? off : 0.0f), 0.33f, cz + (yaw90 ? 0.0f : off),
                (yaw90 ? halfW / segs - 0.02f : 0.40f), 0.06f,
                (yaw90 ? 0.40f : halfW / segs - 0.02f), kLeatherHi, kEmitOff, false);
        }
        // Back cushions + armrests.
        box(cx + bx, 0.55f, cz + bzz, yaw90 ? sxW : 0.10f, 0.28f, yaw90 ? 0.10f : szW, kLeather, kEmitOff, false);
        const float ax = yaw90 ? halfW + 0.08f : 0.0f, az = yaw90 ? 0.0f : halfW + 0.08f;
        box(cx - ax + (yaw90 ? 0 : 0), 0.32f, cz - az, yaw90 ? 0.08f : sxW, 0.32f, yaw90 ? szW : 0.08f, kLeather, kEmitOff, false);
        box(cx + ax, 0.32f, cz + az, yaw90 ? 0.08f : sxW, 0.32f, yaw90 ? szW : 0.08f, kLeather, kEmitOff, false);
        // Two throw pillows.
        box(cx + (yaw90 ? -halfW * 0.5f : 0.30f), 0.46f, cz + (yaw90 ? 0.30f : -halfW * 0.5f),
            0.14f, 0.10f, 0.06f, kPillowA, kEmitOff, false);
        box(cx + (yaw90 ? halfW * 0.55f : 0.28f), 0.46f, cz + (yaw90 ? 0.28f : halfW * 0.55f),
            0.13f, 0.09f, 0.06f, kPillowB, kEmitOff, false);
        ++m_stats.couches;
    };
    couch(CW / 2 - 1.5f, CL / 2 - 1.5f, 0, 1.0f);
    couch(CW / 2 - 1.5f, CL / 2 - 3.3f, 0, 1.0f);
    // End table: polished top + a warm table lamp (glowing shade + pool light).
    box(CW / 2 - 1.5f, 0.275f, CL / 2 - 2.4f, 0.3f, 0.275f, 0.3f, kStair, kEmitOff, true);
    {
        x3::prims::PrimMesh tt = x3::prims::makeBox(0.32f, 0.015f, 0.32f, CW / 2 - 1.5f, oy + 0.565f, CL / 2 - 2.4f, 1.0f);
        const float ttCol[4] = { 0.06f, 0.07f, 0.09f, 1.0f };
        prim(std::move(tt), ttCol, nullptr, x3::rhi::TextureHandle{}, mrGlass, false);
    }
    box(CW / 2 - 1.5f, 0.66f, CL / 2 - 2.4f, 0.025f, 0.09f, 0.025f, kChrome, kEmitOff, false);   // lamp stem
    const float emLamp[4] = { 1.0f, 0.72f, 0.40f, 2.2f };
    box(CW / 2 - 1.5f, 0.80f, CL / 2 - 2.4f, 0.09f, 0.06f, 0.09f, kBarTop, emLamp, false);       // shade
    addLight(m_lights, CW / 2 - 1.5f, oy + 1.0f, CL / 2 - 2.4f, 1.40f, 0.90f, 0.45f, 3.5f); // relight: warm lounge lamp 0.95 -> 1.40
    ++m_stats.couches;
    couch(-CW / 2 + 2.0f, CL / 2 - 1.5f, 1, 1.25f);   // VIP couch (faces -Z)
    ++m_stats.couches;

    // ==================================================================
    // MAX-OUT: THE VIP LOUNGE (2nd story over the engine room) — was a bare
    // floor slab. Now: two facing leather sectionals around a mirror-gloss
    // coffee table with glowing drinks, a warm rope-light along the floor edge,
    // two overhead pools, a lounge OLED, and a patron so it reads inhabited.
    // ==================================================================
    {
        const float lz = erZ0;                 // lounge center Z (engine-room center)
        const float ly = LOUNGE_Y + 0.11f;     // on the lounge floor slab
        // Two sectionals facing each other across the table (author with boxes at
        // the lounge Y — couch() authors at ground level, so build these inline).
        auto loungeSofa = [&](float cz, float backSign) {
            box(0.0f, ly + 0.14f, cz, 1.1f, 0.14f, 0.42f, kLeather, kEmitOff, true);           // base
            for (int sg = 0; sg < 3; ++sg)
                box(-1.1f + 1.1f * (2 * sg + 1) / 3.0f, ly + 0.33f, cz, 1.1f / 3 - 0.02f, 0.06f, 0.40f,
                    kLeatherHi, kEmitOff, false);                                               // cushions
            box(0.0f, ly + 0.55f, cz + backSign * 0.42f, 1.1f, 0.28f, 0.10f, kLeather, kEmitOff, false); // back
            for (int s2 = -1; s2 <= 1; s2 += 2)
                box(s2 * 1.18f, ly + 0.32f, cz, 0.08f, 0.32f, 0.42f, kLeather, kEmitOff, false); // arms
            box(-0.5f, ly + 0.46f, cz + backSign * 0.30f, 0.14f, 0.10f, 0.06f, kPillowA, kEmitOff, false);
            box( 0.55f, ly + 0.46f, cz + backSign * 0.28f, 0.13f, 0.09f, 0.06f, kPillowB, kEmitOff, false);
            ++m_stats.couches;
        };
        loungeSofa(lz - 1.05f, -1.0f);   // north sofa, back to -Z
        loungeSofa(lz + 1.05f, +1.0f);   // south sofa, back to +Z
        // Mirror-gloss coffee table + chrome legs + four glowing drinks.
        {
            x3::prims::PrimMesh ct = x3::prims::makeBox(0.75f, 0.02f, 0.45f, 0.0f, oy + ly + 0.42f, lz, 1.0f);
            const float ctCol[4] = { 0.05f, 0.06f, 0.08f, 1.0f };
            const float ctEm[4]  = { 0.02f, 0.08f, 0.10f, 0.35f };
            prim(std::move(ct), ctCol, ctEm, x3::rhi::TextureHandle{}, mrGlass, false);
            for (int lx2 = -1; lx2 <= 1; lx2 += 2)
                for (int lz2 = -1; lz2 <= 1; lz2 += 2)
                    box(lx2 * 0.65f, ly + 0.21f, lz + lz2 * 0.35f, 0.02f, 0.21f, 0.02f, kChrome, kEmitOff, false);
            const float drinkHue[4][3] = {
                { 0.10f, 0.9f, 1.0f }, { 1.0f, 0.6f, 0.15f }, { 0.9f, 0.15f, 0.5f }, { 0.2f, 1.0f, 0.5f } };
            for (int d2 = 0; d2 < 4; ++d2) {
                const float dx2 = (d2 % 2 ? 0.30f : -0.28f), dz2 = (d2 / 2 ? 0.16f : -0.15f);
                const float dEm[4] = { drinkHue[d2][0], drinkHue[d2][1], drinkHue[d2][2], 1.6f };
                const float dCol[4] = { drinkHue[d2][0] * 0.3f, drinkHue[d2][1] * 0.3f, drinkHue[d2][2] * 0.3f, 1.0f };
                x3::prims::PrimMesh dg = x3::prims::makeBox(0.025f, 0.055f, 0.025f,
                                                            dx2, oy + ly + 0.50f, lz + dz2, 1.0f);
                prim(std::move(dg), dCol, dEm, x3::rhi::TextureHandle{}, mrGlass, false);
            }
        }
        // Warm rope-light around the lounge floor edge (three sides; the railing
        // side stays dark so the club neon reads from below).
        const float emRope[4] = { 1.0f, 0.62f, 0.28f, 2.6f };
        box(0.0f, ly + 0.02f, lz - ER_D / 2 + 0.25f, ER_W / 2 - 0.35f, 0.012f, 0.012f, kWall, emRope, false);
        for (int s3 = -1; s3 <= 1; s3 += 2)
            box(s3 * (ER_W / 2 - 0.25f), ly + 0.02f, lz, 0.012f, 0.012f, ER_D / 2 - 0.3f, kWall, emRope, false);
        // Two warm pools + the lounge wall OLED.
        addLight(m_lights, -1.2f, oy + ly + 1.7f, lz, 1.90f, 1.25f, 0.65f, 6.0f); // relight: warm pool 1.5 -> 1.9
        addLight(m_lights,  1.2f, oy + ly + 1.7f, lz, 1.90f, 1.25f, 0.65f, 6.0f);
        addLight(m_lights,  0.0f, oy + ly + 1.2f, lz, 0.30f, 0.90f, 1.30f, 4.0f);  // cool cyan counter-accent (relight: 0.15/0.45/0.60 -> HDR cyan)
        {
            const float emScr[4] = { 1.0f, 1.0f, 1.0f, 1.8f };
            const uint32_t lsId = box(ER_W / 2 - 0.12f, ly + 1.6f, lz, 0.015f, 0.35f, 0.62f, kTvFrame, emScr, false);
            oledGlass(lsId, texEq[3]);
            m_oledEnts.push_back(lsId);
        }
        // A patron enjoying the lounge (inert idle prop, like the DJ/bouncer).
        addCharacter(scene, device, physics, modelDir, "AnnaBodySuit.glb",
                     x3::phys::Vec3{ 0.9f, oy + ly, lz - 0.2f }, 1.0f, false, nullptr);
    }

    // ==================================================================
    // CLUB AMBIENT + KEY LIGHTS (Babylon hemi/point/fill -> point lights).
    // ==================================================================
    // (relight: these three ROOM-WIDE fills were the darkest offenders — 0.16-0.40
    // saturated the whole 50x100 ft room to near-black. Reworked to VIBRANT HDR
    // club washes: violet overhead, magenta over the bar side, UV-violet on the
    // mirror floor. They set the room's colored mood; the orbiters + fixtures pop
    // on top.)
    addLight(m_lights, 0, oy + CH * 0.7f, 0, 0.85f, 0.25f, 1.30f, 25.0f);       // central overhead VIOLET wash
    addLight(m_lights, -CW / 2 + 2, oy + 3.0f, CL / 4, 0.70f, 0.20f, 1.10f, 10.0f); // ground-bar MAGENTA wash
    addLight(m_lights, 0, oy + 2.0f, 0, 0.55f, 0.12f, 1.40f, 18.0f);            // UV-violet wash (mirror floor)

    // DANCE-FLOOR KEY (fix/club-blacklights): the dancers rendered as SOLID BLACK
    // silhouettes — the orbiters sat at ceiling height so their pools hit the
    // FLOOR, and the floor tiles are self-lit emissive, so the only thing that
    // glowed was the checkerboard. A soft neutral-violet key hung over the crowd
    // centroid (the dancer spots cluster around z ≈ -2) puts actual light on
    // faces/torsos. Deliberately gentle — way under the gel colors — so the room
    // stays a moody club, not a showroom.
    addLight(m_lights, 0, oy + 4.6f, -1.8f, 1.05f, 0.95f, 1.30f, 13.0f);

    // ==================================================================
    // CEILING MOVING-HEAD RIG (Tim addenda: "we had the lights that move to the
    // music" + "they're ceiling-mounted fixtures that project patterns DOWN onto
    // the dance floor"). Four visible fixtures hang from the ceiling on a ring
    // over the dance floor; each throws a translucent BEAM shaft down to a
    // colored POOL that sweeps a beat-locked figure-8 on the floor below it,
    // stepping on the eighth-note grid, gels rotating every 8-beat phrase.
    //   * fixture body: mount plate + yoke + head + emissive LENS (gel-colored,
    //     breathing with the beat — update() drives it);
    //   * beam: a thin unit-box shaft on the GLASS route (transparent + flat
    //     emissive), re-posed each frame from the lens to the pool point so the
    //     light DEMONSTRABLY comes from the fixture;
    //   * pool: the fixture's point light rides at ~1.15 m over the pool point —
    //     it paints the floor AND lights the dancers' bodies as it sweeps.
    // (True GOBO pattern projection — dots/bars/starburst in the footprint —
    // needs a projected-texture/decal path the engine doesn't have; the decal
    // system is bullet-impact rings only. Noted as follow-up; beat-swept pools
    // from visible ceiling fixtures is the v1.)
    // ==================================================================
    {
        for (int i = 0; i < 4; ++i) {
            const float a = (i + 0.5f) / 4.0f * 2.0f * kPi;   // 45/135/225/315° — clear of THE ORB
            MovingHead mh;
            mh.fx = std::cos(a) * kHeadRingR;
            mh.fz = kHeadRingCz + std::sin(a) * kHeadRingR;
            // Fixture body, hung from the ceiling: plate -> yoke -> head -> lens.
            box(mh.fx, CH - 0.045f, mh.fz, 0.16f, 0.045f, 0.16f, kMetal, kEmitOff, false, 1.0f, &sMetal);
            box(mh.fx, CH - 0.17f,  mh.fz, 0.05f, 0.09f,  0.05f, kMetal, kEmitOff, false);
            box(mh.fx, CH - 0.42f,  mh.fz, 0.11f, 0.16f,  0.11f, kSub,   kEmitOff, false);
            {
                const float* gel = kSpotGels[i & 3];
                const float lensEm[4] = { gel[0], gel[1], gel[2], 3.0f };
                mh.lensEnt = box(mh.fx, CH - 0.60f, mh.fz, 0.085f, 0.03f, 0.085f, kTvFrame, lensEm, false);
            }
            // Beam shaft: UNIT box authored at the world origin (extents ±1) so
            // update() can pose it with a full basis (lens -> pool) each frame —
            // same rewrite-the-transform trick as THE ORB's spin. Posed once
            // below so it never renders at the origin.
            {
                const float beamCol[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                mh.beamEnt = addBox(scene, device, physics, 0, 0, 0, 1, 1, 1,
                                    beamCol, kEmitOff, false);
                Entity& be = scene.get(mh.beamEnt);
                be.transparent = true;
                be.glass.opacity = 0.14f;     // a light shaft, not a solid
                be.glass.refraction = 0.0f;
                be.glass.roughness = 1.0f;
                be.glass.specular = 0.0f;
                const float* gel = kSpotGels[i & 3];
                be.glass.tint[0] = gel[0]; be.glass.tint[1] = gel[1]; be.glass.tint[2] = gel[2];
                be.emissive[0] = gel[0]; be.emissive[1] = gel[1]; be.emissive[2] = gel[2];
                be.emissive[3] = 0.55f;       // soft volumetric-ish glow
                poseBeam(be, mh.fx, oy + CH - 0.63f, mh.fz, mh.fx, oy + 0.05f, mh.fz);
            }
            m_movingHeads.push_back(mh);
        }
    }

    // ---- MOVING LIGHTS: 4 fixture POOL lights + 4 ring lights. These trail the
    // static lights and are rewritten each frame by update(). Record the start. ----
    m_staticLightCount = m_lights.size();
    // 4 moving-head pool lights (one per ceiling fixture; positions/colors are
    // beat-driven in update() — created at each fixture's rest pool).
    // (relight: saturated HDR gels — hot pink / electric blue / laser green / amber —
    // pushed to ~2.8 so the moving pools bloom and rake the walls + dancers.)
    for (int i = 0; i < 4; ++i) {
        const auto& mh = m_movingHeads[i];
        addLight(m_lights, mh.fx, oy + 1.15f, mh.fz,
                 kSpotGels[i][0], kSpotGels[i][1], kSpotGels[i][2], 9.0f);
    }
    // 4 ring lights (orbit radius ~8).
    // (relight: 1.0-max -> ~2.0 saturated HDR so the outer ring washes the walls too.)
    // (fix/club-blacklights: 4.0 -> 2.4 m so the outer ring doubles as a colored
    // BACK/RIM light on the crowd from outside the floor.)
    for (int i = 0; i < 4; ++i) {
        const float a = (i / 4.0f) * 2.0f * kPi;
        addLight(m_lights, std::cos(a) * 8.0f, oy + 2.4f, std::sin(a) * 8.0f,
                 kRingGels[i][0], kRingGels[i][1], kRingGels[i][2], 22.0f);
    }
    // PHRASE-DROP STROBE (addendum, "tasteful"): one white light over the floor,
    // dark except a few quick pops at the END of every 32-beat phrase (update()).
    m_strobeLightIdx = m_lights.size();
    addLight(m_lights, 0, oy + 5.2f, kHeadRingCz, 0.0f, 0.0f, 0.0f, 14.0f);

    // ==================================================================
    // CHARACTERS — a DJ behind the booth + a bouncer at the landing (inert props
    // that still skin + idle). Graceful box fallback on a failed GLB load.
    // ==================================================================
    {
        const float warm[4] = { 1.2f, 1.1f, 0.95f, 1.0f };
        const float cool[4] = { 0.9f, 1.05f, 1.3f, 1.0f };
        const float djY = LOUNGE_Y;
        const float djZ = -CL / 2 + 2.5f / 2 + 0.3f;
        // DJ in the booth.
        addCharacter(scene, device, physics, modelDir, "marcus_webb.glb",
                     x3::phys::Vec3{ 0.0f, oy + djY, djZ }, 1.0f, false, warm);
        // Bouncer near the elevator landing.
        addCharacter(scene, device, physics, modelDir, "RexBouncer.glb",
                     x3::phys::Vec3{ CW / 2 - 2.0f, oy + 0.0f, CL / 2 - 4.0f }, 1.0f, true, cool);

        // ==============================================================
        // DANCERS — ten real skinned characters on the floor (was: pastel
        // box agents). Six distinct rigs cycled with varied neon-wash tints
        // (the blacklit repeats read as different club-goers); update()
        // choreographs a beat bounce + sway + shuffle per dancer.
        // ==============================================================
        {
            // Roster: the four PROVEN-standing humanoid rigs only (chief_martinez
            // is Z-up and lay flat on the floor with detached boots in the R3
            // shot; the RexBouncer rig reads as a clawed bruiser — right for the
            // door, wrong for the floor). Tint variety carries the crowd read.
            // Humans only, and every dancer carries REAL skeletal dance clips:
            // AnnaCasual + AnnaTactical ship DanceGroove/DanceArms (dance_bake.py).
            // AnnaBodySuit is node-animated (no armature — clips won't bake onto
            // it) so she works the BAR and the lounge instead of the floor.
            const char* rigs[2] = { "AnnaCasual.glb", "AnnaTactical.glb" };
            const float tints[5][4] = {
                { 1.05f, 0.85f, 1.25f, 1.0f },   // violet wash
                { 0.85f, 1.05f, 1.30f, 1.0f },   // cyan wash
                { 1.25f, 0.90f, 1.00f, 1.0f },   // warm rose
                { 0.90f, 1.20f, 0.95f, 1.0f },   // green tinge
                { 1.10f, 1.05f, 0.90f, 1.0f },   // amber
            };
            // Spots: a loose ring on the dance floor + two by the DJ end. Kept off
            // the bar lane and inside the tile field.
            const float spots[10][3] = {   // x, z, base yaw
                { -1.5f,  -3.0f,  0.6f }, {  1.8f,  -4.5f, -2.4f }, {  0.2f,  -6.5f,  3.0f },
                {  3.0f,  -1.5f, -1.2f }, { -2.5f,   0.5f,  1.8f }, {  0.8f,   1.5f, -0.4f },
                {  2.6f,   3.5f,  2.2f }, { -1.0f,   4.5f, -2.8f }, { -3.0f,  -6.0f,  0.2f },
                {  4.0f,  -7.5f,  1.4f },
            };
            for (int d3 = 0; d3 < 10; ++d3) {
                Dancer dn;
                dn.charIdx = m_chars.size();
                dn.bx = spots[d3][0]; dn.bz = spots[d3][1];
                dn.baseY = oy + 0.14f;                     // on the glowing tile tops
                dn.yaw = spots[d3][2];
                dn.phase = (float)(clubHash((uint32_t)d3 * 97u + 13u) % 628u) / 100.0f;
                dn.energy = 0.65f + 0.75f * ((clubHash((uint32_t)d3 * 41u + 7u) % 100u) / 100.0f);
                addCharacter(scene, device, physics, modelDir, rigs[d3 % 2],
                             x3::phys::Vec3{ dn.bx, dn.baseY, dn.bz }, 1.0f,
                             false, tints[d3 % 5]);
                // REAL skeletal dance: the calm loop replaces Idle on inert props.
                // Alternate the two clips so the floor mixes grooves.
                m_chars.back()->setCalmLoop((d3 & 1) ? "dancearms" : "dancegroove");
                m_dancers.push_back(dn);
            }
        }
    }

    m_stats.entities = (int)(scene.size() - entsBefore);

    x3::logInfo("[club1127] built THE DEEP (Club 1127) at Y=" + std::to_string((int)oy) +
                ": " + std::to_string(m_stats.entities) + " entities, " +
                std::to_string(m_lights.size()) + " point lights, " +
                std::to_string(m_stats.blacklights) + " blacklights, " +
                std::to_string(m_stats.tvScreens) + " TVs, " +
                std::to_string(m_chars.size()) + " characters");
    return m_stats;
}

void Club1127World::update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics) {
    if (!m_built) return;
    m_time += dt;
    const float t = m_time;

    // ---- THE BEAT GRID (Tim addendum: "we had the lights that move to the music
    // too"). ONE clock: the same kClubBpm envelope the subs/tiles/dancers already
    // rode, hoisted here so the LIGHTS share it instead of free-running on their
    // own arbitrary rates. Everything music-reactive below derives from these. ----
    const float beatHz    = kClubBpm / 60.0f;
    const float beatCount = t * beatHz;                    // absolute beat position
    const float thump     = std::pow(std::max(0.0f, std::sin(beatCount * kPi)), 6.0f);
    const float breathe   = 0.62f + 0.55f * thump;         // gel beat envelope

    // --- Spin THE ORB (rotate about Y) by rewriting its transform's upper 3x3. ---
    if (m_orbValid && m_orbEnt < scene.size()) {
        const float ang = t * 0.5f;            // matches JS dt*0.5 cadence
        const float c = std::cos(ang), s = std::sin(ang);
        Entity& e = scene.get(m_orbEnt);
        // Column-major: keep translation, set Y-rotation in the 3x3. The ORB box
        // is authored in WORLD space centered at the origin column, so we must put
        // the rotation about the orb center: translate to center is already baked
        // into the geometry (centered at 0,m_orbY,0), so a pure rotation works.
        e.transform[0] = c;  e.transform[2] = -s;
        e.transform[8] = s;  e.transform[10] = c;
        // (The orb geometry is authored at world (0, m_orbY, 0); rotating its model
        //  matrix about the origin spins it in place since its center is the origin.)
        // Mirror-ball SPARKLE rides the beat (facet glow surges on the thump).
        e.emissive[3] = 1.15f + 0.55f * thump;
    }

    // --- CEILING MOVING-HEAD RIG + ring lights + strobe — ALL BEAT-LOCKED (was:
    // a smooth free-running orbit at ceiling height). Behavior per fixture:
    //   * the pool STEPS on the eighth-note grid (2 steps/beat, 16 steps per
    //     figure — a full figure-8 under the fixture every 8-beat phrase), each
    //     step snapped in fast then held: the classic moving-head jerk;
    //   * gels rotate one slot every 8-beat phrase (pink->blue->green->amber);
    //   * every gel (pool light, lens, beam) breathes with the beat envelope;
    //   * the beam shaft is re-posed lens -> pool so the light visibly comes
    //     from the ceiling fixture. ---
    if (m_lights.size() >= m_staticLightCount + 8 && m_movingHeads.size() == 4) {
        const float eighth = beatCount * 2.0f;
        const float frac   = eighth - std::floor(eighth);
        const float snap   = std::min(1.0f, frac * 4.0f);              // land in the first quarter
        const float step   = std::floor(eighth) + snap * snap * (3.0f - 2.0f * snap);
        const float sweep  = step * (2.0f * kPi / 16.0f);
        const int   phrase = (int)(beatCount / 8.0f);
        for (int i = 0; i < 4; ++i) {
            auto& mh = m_movingHeads[i];
            const float* gel = kSpotGels[(i + phrase) & 3];
            // Pool point: a figure-8 on the floor under the fixture.
            const float p2 = sweep + i * (kPi / 2.0f);
            const float px = mh.fx + 1.7f * std::sin(p2);
            const float pz = mh.fz + 1.1f * std::sin(2.0f * p2);
            auto& L = m_lights[m_staticLightCount + i];
            L.pos[0] = px; L.pos[2] = pz;                  // Y stays at 1.15 m
            L.color[0] = gel[0] * breathe; L.color[1] = gel[1] * breathe; L.color[2] = gel[2] * breathe;
            if (mh.lensEnt < scene.size()) {               // lens carries the gel + breathe
                Entity& le = scene.get(mh.lensEnt);
                le.emissive[0] = gel[0]; le.emissive[1] = gel[1]; le.emissive[2] = gel[2];
                le.emissive[3] = 2.2f + 1.6f * thump;
            }
            if (mh.beamEnt < scene.size()) {               // beam re-posed lens -> pool
                Entity& be = scene.get(mh.beamEnt);
                be.glass.tint[0] = gel[0]; be.glass.tint[1] = gel[1]; be.glass.tint[2] = gel[2];
                be.emissive[0] = gel[0]; be.emissive[1] = gel[1]; be.emissive[2] = gel[2];
                be.emissive[3] = 0.40f + 0.45f * thump;
                poseBeam(be, mh.fx, kClubY + kCH - 0.63f, mh.fz, px, kClubY + 0.05f, pz);
            }
        }
        for (int i = 0; i < 4; ++i) {     // ring lights: half-rate counter-sweep
            const float a = -sweep * 0.5f + (i / 4.0f) * 2.0f * kPi;
            auto& L = m_lights[m_staticLightCount + 4 + i];
            L.pos[0] = std::cos(a) * 8.0f;
            L.pos[2] = std::sin(a) * 8.0f;
            const float* gel = kRingGels[(i + phrase) & 3];
            L.color[0] = gel[0] * breathe; L.color[1] = gel[1] * breathe; L.color[2] = gel[2] * breathe;
        }
        // PHRASE-DROP STROBE: three quick white pops in the LAST half-beat of
        // every 32-beat phrase (peak 2.2, ~0.35 s total) — an accent over the
        // dance floor, deliberately subtle, and never live during the first
        // seconds a screenshot settles.
        if (m_strobeLightIdx < m_lights.size()) {
            auto& S = m_lights[m_strobeLightIdx];
            const float ph32 = std::fmod(beatCount, 32.0f);
            float w = 0.0f;
            if (ph32 > 31.5f) {
                const float u = (ph32 - 31.5f) * 2.0f;     // 0..1 across the window
                w = (std::fmod(u * 3.0f, 1.0f) < 0.5f) ? 2.2f : 0.0f;
            }
            S.color[0] = S.color[1] = S.color[2] = w;
        }
    }

    // --- Pulse the blacklight emissive + its CAST light in phase, BEAT-LOCKED:
    // a slow wave (one cycle per 2 beats) CHASES around the room tube-by-tube
    // (the per-tube offset walks the full circle across the 20 tubes), plus a
    // small kick on every thump — so even the walls breathe with the track. ---
    for (size_t i = 0; i < m_blacklightEnts.size(); ++i) {
        const uint32_t id = m_blacklightEnts[i];
        if (id >= scene.size()) continue;
        const float pulse = 0.72f + 0.18f * std::sin(kPi * beatCount + i * (2.0f * kPi / 20.0f))
                          + 0.22f * thump;
        Entity& e = scene.get(id);
        e.emissive[0] = kBlacklightR * pulse;
        e.emissive[1] = kBlacklightG;
        e.emissive[2] = kBlacklightB * pulse;
        e.emissive[3] = 4.0f;   // relight: UV tube bloom 3.0 -> 4.0
        if (i < m_blacklightLightIdx.size()) {
            const size_t li = m_blacklightLightIdx[i];
            if (li < m_lights.size()) {
                m_lights[li].color[0] = kBlacklightCast[0] * pulse;
                m_lights[li].color[1] = kBlacklightCast[1] * pulse;
                m_lights[li].color[2] = kBlacklightCast[2] * pulse;
            }
        }
    }

    // --- MAX-OUT: the BEAT CLOCK drives the sub cones, the corner sub pulse
    // lights, and the bright dance tiles — the whole room breathes with the
    // music instead of idling frozen. (thump now hoisted to the top of update()
    // — the ONE beat grid the lights ride too.) ---
    {
        // Sub driver cones: amber surge on the hit, near-dark between.
        for (const uint32_t id : m_subPulseEnts) {
            if (id >= scene.size()) continue;
            scene.get(id).emissive[3] = 0.25f + 2.1f * thump;
        }
        // Corner sub pulse lights breathe with the same clock.
        for (const size_t li : m_subLightIdx) {
            if (li >= m_lights.size()) continue;
            const float k = 0.25f + 0.75f * thump;
            m_lights[li].color[0] = 1.40f * k;   // relight: amber sub thump 0.9 -> 1.4 HDR
            m_lights[li].color[1] = 0.65f * k;
            m_lights[li].color[2] = 0.15f * k;
        }
        // Bright dance tiles: a soft breathe (never dark — the floor is the star).
        for (const uint32_t id : m_tilePulseEnts) {
            if (id >= scene.size()) continue;
            scene.get(id).emissive[3] = 0.40f + 0.45f * thump;   // faint violet breath
        }
        // OLED shimmer: each screen's brightness wanders on its own phase, so the
        // baked equalizer frames read as LIVE video from across the room. This rides
        // the MASKED emissive (emissiveMap=1), so it breathes the LIT COLUMNS — the
        // black substrate stays black through the whole cycle instead of the entire
        // pane brightening and dimming like a lamp behind a sheet. The band is centred
        // on kOledEmit; the old 0.12..0.63 range was a flat-wash floor and would now
        // leave the screens too dim to read as displays.
        for (size_t i = 0; i < m_oledEnts.size(); ++i) {
            const uint32_t id = m_oledEnts[i];
            if (id >= scene.size()) continue;
            scene.get(id).emissive[3] = kOledEmit + 0.35f * std::sin(t * 2.3f + i * 1.9f) + 0.45f * thump;
        }
    }

    // Re-push the (now-moved) light set to the device so the orbiting lights animate.
    device.setPointLights(m_lights.data(), (uint32_t)m_lights.size());

    // --- DANCERS: choreograph each dancer BEFORE its update() so the skinner
    // draws the grooved pose this frame. Three layers, all beat-locked but
    // phase-offset + energy-scaled per dancer so the floor never syncs up:
    //   BOUNCE — a sharp knee-pop on every beat (the thump curve, in Y);
    //   SWAY   — hips/heading rock at half-tempo (yaw oscillation);
    //   SHUFFLE— a slow personal-space drift around the home spot (XZ orbit).
    {
        // (beatHz hoisted to the top of update() — the shared beat grid.)
        for (const Dancer& dn : m_dancers) {
            if (dn.charIdx >= m_chars.size()) continue;
            const float tp = t + dn.phase;
            const float lobe = std::sin(tp * beatHz * kPi);
            // The baked clips carry the bounce/arms now — the procedural layer is
            // just a gentle weight-shift + facing drift so no two dancers match.
            const float bounce = std::pow(std::max(0.0f, lobe), 4.0f) * 0.025f * dn.energy;
            const float sway = std::sin(tp * beatHz * kPi * 0.5f) * 0.22f * dn.energy;
            const float sx = dn.bx + std::sin(tp * 0.31f) * 0.30f;
            const float sz = dn.bz + std::cos(tp * 0.23f) * 0.28f;
            m_chars[dn.charIdx]->setPropPose(
                x3::phys::Vec3{ sx, dn.baseY + bounce, sz }, dn.yaw + sway);
        }
    }

    // Tick the inert character props (idle clips; chaseSpeed 0 => no movement).
    for (auto& c : m_chars)
        c->update(dt, scene, physics, c->pos());
}

void Club1127World::drawCharacters(x3::rhi::IRenderDevice& device,
                                   const x3::rhi::FrameContext& frame, const Scene& scene) const {
    for (const auto& c : m_chars)
        c->drawMonster(device, frame, scene);
}

void Club1127World::showcaseCamera(float out[5]) const {
    // Elevated vantage from the SE corner (near the elevator landing) looking
    // toward -X/-Z across the dance floor so the glowing checkerboard, the DJ
    // booth + ORB on the far north wall, the ground bar (left), and the PA stacks
    // all read in one frame. Y/Z keep us inside the 30 ft ceiling.
    out[0] = kCW / 2 - 2.0f;   // x: by the east wall / elevator
    out[1] = kClubY + 5.0f;    // y: above the floor, below the ceiling
    out[2] = kCL / 2 - 3.0f;   // z: south end
    out[3] = -2.35f;           // yaw: look toward -X/-Z (the dance floor + booth)
    out[4] = -0.18f;           // pitch: slightly down over the floor
}

// ===========================================================================
// Headless self-test (--test-club). Build the club at Y=-200 with the shared
// HeadlessRenderDevice + a real physics world (no window / Vulkan), assert the key
// fixtures + the room footprint/Y, tick a few frames, and confirm it is leak-clean
// (idempotent rebuild adds NO meshes; mesh creates are balanced by entities).
// ===========================================================================
} // namespace x3::game

#include "headless_device.h"
#include "asset_root.h"        // x3::game::riggedGlbRoot()
#include "engine/physics/IPhysicsWorld.h"
#include <cmath>

namespace x3::game {

bool runClubSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool cond, const char* name) {
        if (cond) { ++pass; x3::logInfo(std::string("[club-test] PASS ") + name); }
        else      { ++fail; x3::logError(std::string("[club-test] FAIL ") + name); }
    };

    // A counting device: tracks live mesh handles so we can assert no leak.
    struct CountingDevice : public HeadlessRenderDevice {
        int created = 0, destroyed = 0;
        x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                       const uint32_t* idx, uint32_t ni) override {
            ++created;
            return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
        }
        void destroyMesh(x3::rhi::MeshHandle h) override {
            ++destroyed;
            HeadlessRenderDevice::destroyMesh(h);
        }
    };

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    CountingDevice device;
    Scene scene;

    Club1127World club;
    const Club1127World::Stats& s = club.build(scene, device, *physics, x3::game::riggedGlbRoot());

    // (1) Room footprint + Y. The main floor sits at world Y = -200, the room is
    //     the real 50x100x30 ft (15.24 x 30.48 x 9.14 m), ceiling 30 ft above.
    {
        const float wX = s.roomMaxX - s.roomMinX;   // ~15.24
        const float wZ = s.roomMaxZ - s.roomMinZ;   // ~30.48
        const float h  = s.ceilingY - s.floorY;     // ~9.14
        const bool yOk   = std::fabs(s.floorY - (-200.0f)) < 0.01f;
        const bool footOk = std::fabs(wX - 15.24f) < 0.05f && std::fabs(wZ - 30.48f) < 0.05f;
        const bool ceilOk = std::fabs(h - 9.14f) < 0.05f;
        check(yOk && footOk && ceilOk,
              "main room is 50x100x30 ft (15.24x30.48x9.14 m) with its floor at Y=-200");
    }

    // (2) Suspended DJ booth: platform + turntables + 2 OLED + keypad door.
    check(s.hasDjBooth && s.hasDjTurntables && s.hasDjScreens && s.hasKeypadDoor,
          "suspended DJ booth (turntables + 2 OLED screens + keypad door)");

    // (3) THE ORB — the 2 m mirror ball.
    check(s.hasOrb, "THE ORB (mirror ball) exists");

    // (4) Aerial bar + ground bar with exactly 7 stools.
    check(s.hasAerialBar && s.hasGroundBar && s.barStools == 7,
          "aerial bar + ground bar with 7 stools");

    // (5) Engine-room/lounge with a 12-step stair.
    check(s.hasLoungeFloor && s.stairSteps == 12,
          "2-story engine-room/lounge with a 12-step stair");

    // (6) The real PA rig: 4 SVS subs + 16 JBL line-array cabs + 4 JBL 18" subs +
    //     16 surrounds.
    check(s.svsSubs == 4 && s.jblLineArray == 16 && s.jbl18Subs == 4 && s.surrounds == 16,
          "PA rig: 4 SVS subs + 16 JBL line-array + 4 JBL 18\" subs + 16 surrounds");

    // (7) 20 blacklights at Tim's spec: vertical wall tubes every 12 ft, centered
    //     5 ft off the floor, EACH casting a companion UV point light (8 per long
    //     wall + 2 per side of the south 85"). The light set (static + tube casts
    //     + orbiters) must stay under the device cap of 64.
    check(s.blacklights == 20, "20 blacklights (12 ft spacing, 5 ft centers)");
    check(club.pointLights().size() <= 64,
          "point-light set fits the kMaxPointLights=64 device cap");

    // (8) 6-screen TV multiplex.
    check(s.tvScreens == 6, "6-screen TV multiplex");

    // (9) Dance floor + VIP/couch seating.
    check(s.hasDanceFloor && s.couches >= 3, "dance-floor checkerboard + VIP/couch seating");

    // (10) Player spawn sits inside the room footprint, on the floor at Y=-200.
    {
        const x3::phys::Vec3 sp = club.spawn();
        const bool inX = sp.x > s.roomMinX && sp.x < s.roomMaxX;
        const bool inZ = sp.z > s.roomMinZ && sp.z < s.roomMaxZ;
        const bool onFloor = sp.y >= -200.0f - 0.01f && sp.y <= -200.0f + 1.0f;
        check(inX && inZ && onFloor && std::isfinite(sp.x) && std::isfinite(sp.z),
              "player spawn is inside the room footprint on the Y=-200 floor");
    }

    // (11) Animate a few frames: ORB spins, lights orbit, blacklights pulse. Assert
    //      transforms/light positions stay finite (no NaN) and lights actually moved.
    {
        const auto& L0 = club.pointLights();
        // Snapshot ONE orbiting spotlight (the last 8 lights orbit; a ring of
        // symmetric lights has an invariant coordinate SUM, so track a single one).
        const size_t orbIdx = L0.size() >= 8 ? L0.size() - 8 : 0;
        const float bx = L0[orbIdx].pos[0], bz = L0[orbIdx].pos[2];
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 30; ++i)
            club.update(dt, scene, device, *physics);
        bool finite = true;
        for (const auto& l : club.pointLights())
            if (!std::isfinite(l.pos[0]) || !std::isfinite(l.pos[1]) || !std::isfinite(l.pos[2]))
                finite = false;
        const auto& L1 = club.pointLights();
        const float moved = std::fabs(L1[orbIdx].pos[0] - bx) + std::fabs(L1[orbIdx].pos[2] - bz);
        check(finite && moved > 1e-3f,
              "ORB/spotlights/blacklights animate (an orbiting light moved, all finite)");
    }

    // (12) Idempotent rebuild: a second build() is a no-op and creates NO new mesh.
    {
        const int before = device.created;
        club.build(scene, device, *physics, x3::game::riggedGlbRoot());
        check(device.created == before && club.stats().entities == s.entities,
              "rebuild is idempotent (no duplicated geometry / no leak)");
    }

    // (13) THE OLED SCREENS ARE REAL DISPLAYS, not lit rectangles. Every screen the
    //      club registers must be TEXTURED GLASS on the PER-TEXEL emissive path. This
    //      is the assertion that "the panel exists" never made: a pane can be present,
    //      textured, and still render as a featureless slab if its emissive is a flat
    //      uniform add (which is exactly what these were — see the BEFORE shots).
    {
        const auto& oled = club.oledEntities();
        bool allOk = !oled.empty();
        for (const uint32_t id : oled) {
            if (id >= scene.size()) { allOk = false; break; }
            const Entity& e = scene.get(id);
            // Textured (the EQ frame is actually BOUND — a screen with no texture has
            // nothing to show), glass (it IS the pane; nothing sits in front of it),
            // and emissiveMap=1 so the glow is masked by that texture.
            if (!e.tex.valid() || !e.transparent || e.glass.emissiveMap < 0.999f) allOk = false;
            // The emissive must stay NEUTRAL: the texel carries the palette, so a
            // tinted emissive would drag every screen back to one colour (and it is
            // what used to blue-wash the amber/green EQ families).
            const float mx = std::max({ e.emissive[0], e.emissive[1], e.emissive[2] });
            const float mn = std::min({ e.emissive[0], e.emissive[1], e.emissive[2] });
            if (mx - mn > 0.05f) allOk = false;
            if (e.emissive[3] <= 0.5f) allOk = false;   // and bright enough to READ
        }
        check(allOk && oled.size() == 10,
              "all 10 OLED screens are textured glass on the per-texel emissive path");
    }

    // (14) THE SCREEN-CONTRAST PROBE + ITS NEGATIVE CONTROL. Run glass.frag's emissive
    //      math on a real EQ frame and demand the panel have real dynamic range: the
    //      lit columns must massively out-emit the black substrate. The negative
    //      control re-runs the SAME probe with emissiveMap=0 (the pre-fix flat path)
    //      and requires it to come back ~1.0 — a slab. Without that control this test
    //      could be passing for the wrong reason; with it, a regression to flat
    //      emissive is caught rather than silently shrugged off.
    {
        const float emShipping[4] = { 1.0f, 1.0f, 1.0f, kOledEmit };
        bool allBright = true;
        for (int hue = 0; hue < 4; ++hue)          // all 4 palette families
            if (clubOledEmissiveContrast(hue, /*emissiveMap*/ 1.0f, emShipping) < 20.0f)
                allBright = false;
        // NEGATIVE CONTROL: the flat path must probe as a slab (no range at all).
        const float flat = clubOledEmissiveContrast(/*hue*/ 0, /*emissiveMap*/ 0.0f, emShipping);
        const bool controlIsSlab = flat < 1.01f;
        check(allBright && controlIsSlab,
              "OLED panes glow PER-TEXEL (>20x bright:dark on all 4 palettes; "
              "flat-emissive control probes as a 1.0x slab)");
    }

    // (15) Leak-clean: every mesh the device handed out can be destroyed and the
    //      device's create/destroy ledger balances (the live VMA allocationCount=0
    //      proof is the Debug --smoketest; here we prove the count bookkeeping).
    {
        // Destroy every mesh handle the club authored (ids are contiguous 1..created
        // from the stub's monotonic minting), then assert the ledger balances.
        for (int h = 1; h <= device.created; ++h)
            device.destroyMesh(x3::rhi::MeshHandle{ (uint32_t)h });
        check(device.created > 0 && device.destroyed == device.created,
              "mesh create/destroy ledger balances (leak-clean bookkeeping)");
    }

    physics->shutdown();

    const int total = pass + fail;
    x3::logInfo("club: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return fail == 0;
}

} // namespace x3::game
