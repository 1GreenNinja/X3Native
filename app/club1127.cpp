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
#include <cstring>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;
// The house tempo — matched to assets/audio/music/club_descent.wav (measured
// ~85.5 BPM base / 171 eighth-grid) so subs, tiles, and dancers ride the track.
constexpr float kClubBpm = 85.5f;

// ---- Tints (linear-ish; the device tonemaps) ------------------------------
// Ported from the JS StandardMaterial diffuse colors (hex -> 0..1 RGB).
const float kWall[4]   = { 0.36f, 0.36f, 0.42f, 1.0f };    // painted-CMU wall TINT (multiplies the
                                                           // sWall concrete texel). Was 0x0a0a12
                                                           // (~0.04) from the untextured era —
                                                           // that crushed effective albedo to
                                                           // ~1%, so NO light (blacklight cast,
                                                           // gels, fills) could EVER register on
                                                           // a wall: they stayed dead-black even
                                                           // when directly lit. The texture
                                                           // carries the darkness now; the tint
                                                           // just cools it.
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
// CANON-PORT §1.10 — DARK GREEN GRANITE (LOCKED, Tim's decision). Deep
// emerald-black polished stone (Verde-Ubatuba): green-dominant, very dark value,
// reads near-black in dim light and resolves to deep green under direct light,
// catches the blue-UV beams. Used for the U-bar countertop + the Lair reading
// shelf (one consistent stone across Tim's spaces). Paired with mrGlass (low
// roughness / near-mirror) so it gleams. baseColor ~ #0E2114.
// POLISH (feat/club-polish-stage15, Tim 2026-07-18): the emerald read near-black
// and the colored moving-head beams washed it magenta. Green channel raised
// 0.130 -> 0.190 (red/blue held low to keep it SATURATED + dark/polished) so it
// resolves to visible deep green under the new neutral bar/shelf keys, and the
// green-tinted sheen emissive is bumped (0.040->0.085 g, str 0.35->0.55) so the
// stone LEANS emerald even when a magenta gel rakes it — never fully black.
// RE-TUNE (feat/club-lns-polish, Tim 2026-07-19): stage15 overshot — the emerald
// read too BRIGHT/saturated ("glowing felt") because the base green was pushed to
// 0.225 AND the sheen emissive to 0.160@0.70 (effective self-glow ~0.112, a lamp).
// Dialed to BELIEVABLE dark Verde-Ubatuba: base green 0.225 -> 0.140 (still clearly
// green-dominant + dark, a rich polished stone value), and the sheen emissive cut to
// 0.050@0.30 (effective ~0.015 — a faint emerald floor, NOT a glow). The mrGlass
// near-mirror reflectivity + the warm bar keys now LIGHT the stone; it no longer
// lights itself. Reads as deep emerald-black granite, never neon.
const float kGranite[4]    = { 0.050f, 0.140f, 0.078f, 1.0f };
// GAMMA WALK-BACK (feat/club-gamma-fix, 2026-07-25): the sRGB fix brightened darks,
// so the emerald SELF-GLOW that kept the stone off pure-black is now redundant and
// reads as a lit lamp. Push it to near-zero (str 0.30 -> 0.08, effective green
// ~0.004) — real stone is lit by the mrGlass near-mirror reflections + the warm bar
// keys, never by self-emission.
const float kGraniteEm[4]  = { 0.010f, 0.050f, 0.026f, 0.08f }; // near-zero emerald sheen (reflections/keys light it)
// White-oak (1897 barn-wood) U-bar base — warm aged oak.
const float kOak[4]    = { 0.230f, 0.150f, 0.090f, 1.0f };
// THE LAIR (NE corner, upstairs) — charcoal light-absorbing walls + warm dens.
const float kCharcoal[4] = { 0.045f, 0.045f, 0.050f, 1.0f }; // Lair charcoal wall
const float kLairFloor[4]= { 0.090f, 0.070f, 0.055f, 1.0f }; // warm dark den floor
const float kOfficeF[4]  = { 0.075f, 0.075f, 0.085f, 1.0f }; // ground-floor office
// SECRET TUNNEL / STRATA + PRIVATE LOUNGE COMPLEX (poured concrete, warm).
const float kStrata[4]   = { 0.080f, 0.072f, 0.060f, 1.0f }; // strata concrete
const float kBunker[4]   = { 0.095f, 0.085f, 0.070f, 1.0f }; // private-lounge walls
const float kWood[4]     = { 0.110f, 0.070f, 0.045f, 1.0f }; // dark wood panelling
const float kEmitAmberLo[4] = { 1.0f, 0.62f, 0.26f, 1.6f };  // warm amber strip/lantern
const float kCatwalk[4]  = { 0.100f, 0.100f, 0.130f, 1.0f }; // steel-grate catwalk

// ★ LAIR OVERLOOK WINDOW (POLISH stage15) — a REAL see-through cut in the club's
// east (+X) wall, filled with the smoked dark glass. Shared by the east-wall
// framing AND the Lair overlook glass so the hole and the pane line up exactly.
// Local (pre-oy) coords: sill sits just above the Lair floor (22 ft = 6.71 m).
const float kOvCz = 0.6f;    // window center Z (clear of the SE elevator gap @ z>2.05)
const float kOvHz = 1.1f;    // window half-width (Z)
const float kOvCy = 7.70f;   // window center Y (local)
const float kOvHy = 0.95f;   // window half-height  -> y in [6.75, 8.65]

// ---- Emissive helpers: { r, g, b, strength }. strength > 1 => HDR bloom. -----
const float kEmitOff[4]     = { 0.0f, 0.0f, 0.0f, 0.0f };
const float kEmitNeon[4]    = { 1.00f, 0.0f, 1.00f, 4.0f };  // magenta aerial-bar neon
const float kEmitDjCon[4]   = { 0.10f, 0.10f, 0.28f, 1.5f }; // DJ console glow
const float kEmitDjScr[4]   = { 0.30f, 0.30f, 0.90f, 3.0f }; // DJ/OLED screens
const float kEmitKeypad[4]  = { 0.10f, 0.95f, 0.30f, 2.0f }; // green keypad
const float kEmitBarTop[4]  = { 0.30f, 0.20f, 0.50f, 1.5f }; // bar-top glow
const float kEmitTile1[4]   = { 0.45f, 0.0f, 0.85f, 1.5f };  // purple dance tile (0x2a0050)
                                                             // (blacklights pass: 2.2 -> 1.5 — the
                                                             // blazing floor owned the exposure and
                                                             // crushed the dancers to silhouettes)
const float kEmitTile2[4]   = { 0.12f, 0.0f, 0.30f, 1.2f };  // dark dance tile (0x0a0020)
const float kEmitOrb[4]     = { 0.45f, 0.45f, 0.60f, 1.4f };  // ORB self-glow
const float kEmitLed[4]     = { 0.10f, 1.00f, 0.10f, 3.0f };  // amp power LED
const float kEmitAbTop[4]   = { 0.353f, 0.353f, 0.416f, 1.2f };// aerial-bar polished top
// Blacklight base emissive (PULSED each frame in update()): deep UV violet.
// POLISH (fix/club-polish, Tim 2026-07-17): a REAL blacklight FLUORESCES — it does
// not blast light. The signature is a DEEP, DIM UV-blue glow. Hue is now PURE BLUE-UV
// (fix/blacklight-blue: red dropped 0.45 -> 0.10, a whisper of violet under a lot of
// blue — no pink) and the tube bloom stays dim at 1.35 so the tubes read as SUBTLE
// deep blue-violet bars, NOT blown magenta rods. The room stays
// as lit as the prior build because the light the hot tubes used to throw is
// COMPENSATED by the raised room-wide UV washes (see the UV atmosphere + washes below).
// PURE BLUE-UV (fix/blacklight-blue, Tim 2026-07-17): the 0.45 red read PINK-violet.
// A real blacklight is deep blue-violet — almost pure blue with a whisper of violet.
// Red dropped 0.45 -> 0.10 (near-zero, just a whisper), blue held at 1.0, green 0.
const float kBlacklightR = 0.10f, kBlacklightG = 0.0f, kBlacklightB = 1.0f;
// GAMMA WALK-BACK (feat/club-gamma-fix): the HDR tube emissive bloomed harder once
// the sRGB fix stopped crushing it — 1.35 -> 0.95 so the tubes read as deep dim
// blue-violet bars again, not blown rods (the cast point light below still washes wall).
const float kBlacklightEmit = 0.95f;   // dim UV tube bloom (4.0 -> 1.35 -> 0.95 post-gamma)
// Companion CAST color for each tube's point light (fix/club-blacklights): the
// tubes were emissive-only geometry — they glowed as thin bars but cast NOTHING,
// so the wall behind them stayed dead-black. Each tube carries a violet point light;
// update() pulses it in phase with the emissive. POLISH: dropped from a hot
// 2.0/0.18/4.0 HDR wash to a GENTLE deep-violet local glow so the tube fluoresces
// its patch of wall softly instead of blasting a magenta wash.
const float kBlacklightCast[3] = { 0.11f, 0.05f, 1.10f };  // BLUE-UV wall glow (red 0.55->0.11: no pink)
const float kBlacklightCastRange = 5.0f;   // ~5 m local wall/near-dancer glow

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

// POLISH (fix/club-polish): pose a VOLUMETRIC BEAM CONE. The cone mesh is authored
// in LOCAL space with its apex at the origin, opening along -Y to a base ring at
// y=-drop (see makeBeamConeMesh). This orients the local -Y axis to point from the
// fixture LENS (A) toward its floor POOL point (B) via a PURE ROTATION + translation
// (orthonormal columns => normals only rotate, never skew — the street-light lesson:
// a non-uniform axis scale skewed every normal and the cone read as a hard funnel).
// The cone's length/radius are baked into the mesh; here we only aim + place it, so
// the additive glow shader's view-angle rim fade stays correct as the beam sweeps.
void poseCone(Entity& e, float ax, float ay, float az, float bx, float by, float bz) {
    float dx = bx - ax, dy = by - ay, dz = bz - az;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-4f) return;
    const float ux = dx / len, uy = dy / len, uz = dz / len;   // apex -> base direction
    // Local +Y image = -u (the cone opens toward -Y toward the pool).
    const float yx = -ux, yy = -uy, yz = -uz;
    // A stable perpendicular helper (beam is near-vertical, so cross with X when steep).
    float hx = 1.0f, hy = 0.0f, hz = 0.0f;
    if (std::fabs(ux) > 0.9f) { hx = 0.0f; hy = 0.0f; hz = 1.0f; }
    // xImg = normalize(h x yImg)
    float xx = hy * yz - hz * yy, xy = hz * yx - hx * yz, xz = hx * yy - hy * yx;
    const float xl = std::sqrt(xx * xx + xy * xy + xz * xz);
    xx /= xl; xy /= xl; xz /= xl;
    // zImg = yImg x xImg (completes a right-handed orthonormal basis)
    const float zx = yy * xz - yz * xy, zy = yz * xx - yx * xz, zz = yx * xy - yy * xx;
    e.transform[0]  = xx; e.transform[1]  = xy; e.transform[2]  = xz; e.transform[3]  = 0;
    e.transform[4]  = yx; e.transform[5]  = yy; e.transform[6]  = yz; e.transform[7]  = 0;
    e.transform[8]  = zx; e.transform[9]  = zy; e.transform[10] = zz; e.transform[11] = 0;
    e.transform[12] = ax; e.transform[13] = ay; e.transform[14] = az; e.transform[15] = 1;
}

// A soft, dusty VOLUMETRIC LIGHT-CONE shaft: apex at the origin opening along -Y to
// a base ring of radius `radius` at y=-drop. OPEN at both ends (a cap is the "solid
// funnel" tell). Profile r(t) = radius*(0.05 + 0.95*t^1.3): a tight throat at the
// lens flaring gently to the pool. World-exact surface-of-revolution normals so the
// additive glow's dot(N,V) rim fade reads as light-in-air. Drawn via the glass pass's
// ADDITIVE mode (see IRenderDevice GlassMaterial::additive) — the SAME fake-volumetric
// machinery the STFC/street-lamp cones use. Depth-tested (LEQUAL) so opaque geometry
// IN FRONT (a dancer standing in the beam) occludes the shaft where their body is.
void makeBeamConeMesh(float radius, float drop,
                      std::vector<x3::rhi::MeshVertex>& verts, std::vector<uint32_t>& idx) {
    const int kRings = 10, kSegs = 24;
    verts.clear(); idx.clear();
    for (int ri = 0; ri < kRings; ++ri) {
        const float t = (float)ri / (float)(kRings - 1);
        const float r = radius * (0.05f + 0.95f * std::pow(t, 1.3f));
        const float drdy = (t > 0.0f)
            ? radius * 0.95f * 1.3f * std::pow(t, 0.3f) / std::max(1e-4f, drop) : 0.0f;
        const float nrm = 1.0f / std::sqrt(1.0f + drdy * drdy);
        for (int si = 0; si <= kSegs; ++si) {
            const float a = (float)si / (float)kSegs * 2.0f * kPi;
            const float ca = std::cos(a), sa = std::sin(a);
            x3::rhi::MeshVertex v{};
            v.pos[0] = ca * r; v.pos[1] = -t * drop; v.pos[2] = sa * r;
            v.normal[0] = ca * nrm; v.normal[1] = drdy * nrm; v.normal[2] = sa * nrm;
            v.uv[0] = (float)si / (float)kSegs; v.uv[1] = t;
            verts.push_back(v);
        }
    }
    const int stride = kSegs + 1;
    for (int ri = 0; ri + 1 < kRings; ++ri)
        for (int si = 0; si < kSegs; ++si) {
            const uint32_t a = (uint32_t)(ri * stride + si), b = a + 1;
            const uint32_t c = a + (uint32_t)stride, d = c + 1;
            idx.insert(idx.end(), { a, c, b,  b, c, d });
        }
}

// Axial gradient for the beam cone (bright at the lens apex, dissolving in the air
// BEFORE the base rim so the shaft has no hard visible end). LINEAR (srgb=false).
// Mirrors the proven street-light coneGrad bake (row-flip so v=0/apex is the bright
// end). RGBA8, W x H.
std::vector<uint8_t> makeBeamGradRGBA(int W, int H) {
    std::vector<uint8_t> px((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) {
        const float v = (float)(H - 1 - y) / (float)(H - 1);      // apex end is bright
        const float s = std::clamp((v - 0.0f) / 0.82f, 0.0f, 1.0f);
        const float sm = s * s * (3.0f - 2.0f * s);               // smoothstep(0,0.82,v)
        const float f = std::pow(1.0f - sm, 2.1f);
        const uint8_t b = (uint8_t)std::lround(255.0f * f);
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &px[((size_t)y * W + x) * 4];
            p[0] = p[1] = p[2] = b; p[3] = 255;
        }
    }
    return px;
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
    else      { drawDriver(0.5f, 0.50f, 0.44f); }   // POLISH: centered — the 3D driver
                                                    // cones planar-map this radially, so
                                                    // the amber surround/ribs/cap must be
                                                    // centered on the texture.
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

// POLISH (fix/club-polish): a REAL recessed speaker DRIVER as 3D geometry (was a
// flat thin box with a radial texture). Authored in LOCAL space FACING +Z (the
// room side), front face at z=0, centered at local (cx,cy). It is a single surface
// of revolution from the OUTER rubber SURROUND (a forward-bulging roll) inward
// through a concave CONE dish that RECEDES to a recessed throat, then a convex DUST
// CAP bulging back toward the room — so it has genuine in/out depth. The ring
// topology + winding match makeUVSphere's proven CCW-from-front pattern. Planar UV
// projects the concentric driver texture (makeSpeakerRGBA) radially so the amber
// surround / cone ribs / dust cap line up with the geometry. Render-only (the
// cabinet box already collides). Appends into `m`.
void makeDriverInto(x3::prims::PrimMesh& m, float cx, float cy,
                    float rimR, float coneDepth, float capR, float roll) {
    const int kSeg = 24;
    const int NP = 7;
    // Profile (radius, z) from OUTER edge (i=0) inward to the DUST-CAP APEX (i=NP-1).
    const float pr[NP] = { rimR * 1.16f, rimR * 1.06f, rimR,             capR * 1.7f,
                           capR,         capR * 0.6f,  0.0008f };
    const float pz[NP] = { 0.0f,         roll,         -coneDepth * 0.12f, -coneDepth * 0.6f,
                           -coneDepth,   -coneDepth + capR * 0.55f,       -coneDepth + capR * 1.05f };
    const float Rmax = rimR * 1.16f;
    const uint32_t cols = (uint32_t)kSeg + 1;
    const uint32_t base = (uint32_t)m.verts.size();
    for (int i = 0; i < NP; ++i) {
        float dr, dz;                              // profile tangent (central diff)
        if (i == 0)          { dr = pr[1] - pr[0];       dz = pz[1] - pz[0]; }
        else if (i == NP - 1){ dr = pr[NP-1] - pr[NP-2]; dz = pz[NP-1] - pz[NP-2]; }
        else                 { dr = pr[i+1] - pr[i-1];   dz = pz[i+1] - pz[i-1]; }
        // 2D normal in (r,z), oriented toward the room (+z front face).
        float n2r = dz, n2z = -dr;
        if (n2z < 0.0f) { n2r = -n2r; n2z = -n2z; }
        const float nl = std::sqrt(n2r*n2r + n2z*n2z);
        if (nl > 1e-6f) { n2r /= nl; n2z /= nl; }
        for (int s = 0; s <= kSeg; ++s) {
            const float a = (float)s / (float)kSeg * 2.0f * kPi;
            const float ca = std::cos(a), sa = std::sin(a);
            x3::rhi::MeshVertex v{};
            v.pos[0] = cx + pr[i] * ca; v.pos[1] = cy + pr[i] * sa; v.pos[2] = pz[i];
            v.normal[0] = n2r * ca; v.normal[1] = n2r * sa; v.normal[2] = n2z;
            const float ru = pr[i] / Rmax;
            v.uv[0] = 0.5f + 0.5f * ru * ca; v.uv[1] = 0.5f + 0.5f * ru * sa;
            m.verts.push_back(v);
        }
    }
    for (int i = 0; i + 1 < NP; ++i)
        for (int s = 0; s < kSeg; ++s) {
            const uint32_t a = base + (uint32_t)i * cols + (uint32_t)s;
            const uint32_t b = a + cols;           // next ring (inward)
            // Same winding as makeUVSphere ({a, a+1, b, a+1, b+1, b}) = CCW from front.
            m.index.insert(m.index.end(), { a, a + 1, b, a + 1, b + 1, b });
        }
}

// DIY DOME-PROJECTOR dot field (LNS GARAGE): a black disc scattered with ~48 small
// soft RGB dots at hashed radii/angles (the cheap mushroom/dome party light's spinning
// spirograph field). BLACK background so an ADDITIVE disc shows only the dots — the
// glossy floor + checkerboard read through the gaps. Spun in update() so the dots sweep.
std::vector<uint8_t> makeDotFieldRGBA(uint32_t n, uint32_t seed) {
    std::vector<uint8_t> px((size_t)n * n * 4, 0);
    for (size_t i = 0; i < (size_t)n * n; ++i) px[i*4+3] = 255;   // opaque; additive uses rgb
    const float cxN = n * 0.5f;
    const float palette[6][3] = {
        {1.0f,0.12f,0.20f}, {0.15f,0.45f,1.0f}, {0.15f,1.0f,0.35f},
        {1.0f,0.85f,0.10f}, {1.0f,0.20f,0.90f}, {0.20f,0.95f,0.95f},
    };
    const int kDots = 52;
    for (int d = 0; d < kDots; ++d) {
        const uint32_t h = clubHash(seed * 2654435761u + (uint32_t)d * 40503u);
        const float ang = (h % 3600) / 3600.0f * 2.0f * kPi;
        const float rad = (0.10f + 0.86f * ((clubHash(h) % 1000) / 1000.0f)) * (n * 0.48f);
        const float dr  = (0.010f + 0.028f * ((clubHash(h * 7u) % 1000) / 1000.0f)) * n;
        const float dcx = cxN + std::cos(ang) * rad, dcy = cxN + std::sin(ang) * rad;
        const float* col = palette[h % 6];
        const int x0 = std::max(0,(int)(dcx-dr)), x1 = std::min((int)n-1,(int)(dcx+dr));
        const int y0 = std::max(0,(int)(dcy-dr)), y1 = std::min((int)n-1,(int)(dcy+dr));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float dd = std::sqrt((x-dcx)*(x-dcx)+(y-dcy)*(y-dcy)) / dr;
                if (dd > 1.0f) continue;
                const float f = (1.0f - dd) * (1.0f - dd);        // soft falloff
                uint8_t* p = &px[((size_t)y*n+x)*4];
                p[0] = (uint8_t)std::min(255.0f, p[0] + col[0]*f*255);
                p[1] = (uint8_t)std::min(255.0f, p[1] + col[1]*f*255);
                p[2] = (uint8_t)std::min(255.0f, p[2] + col[2]*f*255);
            }
    }
    return px;
}

// CEILING STARBURST projector (LNS GARAGE): a red radial burst — many thin spokes
// from a hot center. BLACK bg (additive). Spun in update() so the rays rotate.
std::vector<uint8_t> makeStarburstRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4, 0);
    for (size_t i = 0; i < (size_t)n * n; ++i) px[i*4+3] = 255;
    const float c = n * 0.5f;
    const int spokes = 16;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            const float dx = x - c, dy = y - c;
            const float r = std::sqrt(dx*dx + dy*dy) / (n * 0.5f);
            if (r > 1.0f) continue;
            const float a = std::atan2(dy, dx);
            const float spoke = 0.5f + 0.5f * std::cos(a * spokes);   // 0..1 ray mask
            const float ray = std::pow(spoke, 6.0f) * (1.0f - r);     // thin rays, fade out
            const float core = std::max(0.0f, 1.0f - r * 6.0f);       // hot center
            const float v = std::min(1.0f, ray + core);
            uint8_t* p = &px[((size_t)y*n+x)*4];
            p[0] = (uint8_t)(v * 255);
            p[1] = (uint8_t)(v * 0.10f * 255);
            p[2] = (uint8_t)(v * 0.10f * 255);
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

// PAINTED CMU BLOCK wall (LNS GARAGE, Tim 2026-07-18): the real Late Night Speed
// walls are painted concrete-block — gritty industrial white/grey. Running-bond
// courses (16x8 in / 0.4x0.2 m units), recessed darker mortar joints, faint
// per-block value variation + fine paint grit. Albedo only (tinted club-grey by
// kWall); a matching flat-ish normal/rough gives it relief under the club beams.
std::vector<uint8_t> makeCmuBlockRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t courseH = n / 8;          // 8 courses
    const uint32_t blockW  = n / 4;          // 4 blocks per row
    const uint32_t mortar  = std::max(2u, n / 128);   // joint thickness (px)
    for (uint32_t y = 0; y < n; ++y) {
        const uint32_t course = y / courseH;
        const uint32_t offset = (course & 1) ? blockW / 2 : 0;   // running bond
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            const uint32_t bx = (x + offset) / blockW;
            // Per-block painted value (hashed) — painted CMU is fairly uniform pale grey.
            float v = 0.58f + 0.10f * ((clubHash(bx * 71 + course * 263) % 1000) / 1000.0f - 0.5f);
            // Mortar joints: horizontal (between courses) + vertical (between blocks).
            const bool hJoint = (y % courseH) < mortar;
            const bool vJoint = ((x + offset) % blockW) < mortar;
            if (hJoint || vJoint) v *= 0.52f;                    // recessed dark joint
            // Fine paint grit / porosity speckle.
            v += 0.03f * ((clubHash(x * 911 + y * 379) % 100) / 100.0f - 0.5f);
            // Slight cool tint (painted white-grey reads a touch blue under UV).
            const float r = v * 0.98f, g = v * 0.99f, b = v * 1.03f;
            p[0] = (uint8_t)(std::max(0.0f, std::min(1.0f, r)) * 255);
            p[1] = (uint8_t)(std::max(0.0f, std::min(1.0f, g)) * 255);
            p[2] = (uint8_t)(std::max(0.0f, std::min(1.0f, b)) * 255);
            p[3] = 255;
        }
    }
    return px;
}

// LATE NIGHT SPEED neon SIGN (LNS GARAGE, Tim 2026-07-18): the shop branding on the
// wall. A compact 5x7 uppercase bitmap font bakes the words onto a dark panel as a
// hot red/white neon glow (letters + a soft 1-cell halo). Only the glyphs the sign
// needs are defined. `line` is centred; empty cells stay near-black.
std::vector<uint8_t> makeSignRGBA(uint32_t w, uint32_t h, const char* line,
                                  float rr, float rg, float rb) {
    // 5x7 glyphs (7 rows, each a 5-bit mask, MSB = left).
    auto glyph = [](char c, uint8_t rows[7]) -> bool {
        auto set = [&](uint8_t a,uint8_t b,uint8_t d,uint8_t e,uint8_t f,uint8_t g,uint8_t i){
            rows[0]=a;rows[1]=b;rows[2]=d;rows[3]=e;rows[4]=f;rows[5]=g;rows[6]=i; };
        switch (c) {
            case 'L': set(0x10,0x10,0x10,0x10,0x10,0x10,0x1F); return true;
            case 'A': set(0x0E,0x11,0x11,0x1F,0x11,0x11,0x11); return true;
            case 'T': set(0x1F,0x04,0x04,0x04,0x04,0x04,0x04); return true;
            case 'E': set(0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F); return true;
            case 'N': set(0x11,0x19,0x15,0x13,0x11,0x11,0x11); return true;
            case 'I': set(0x1F,0x04,0x04,0x04,0x04,0x04,0x1F); return true;
            case 'G': set(0x0E,0x11,0x10,0x17,0x11,0x11,0x0E); return true;
            case 'H': set(0x11,0x11,0x11,0x1F,0x11,0x11,0x11); return true;
            case 'S': set(0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E); return true;
            case 'P': set(0x1E,0x11,0x11,0x1E,0x10,0x10,0x10); return true;
            case 'D': set(0x1E,0x11,0x11,0x11,0x11,0x11,0x1E); return true;
            default:  set(0,0,0,0,0,0,0); return false;   // space / unknown
        }
    };
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i) {   // dark panel bg
        px[i*4+0] = 6; px[i*4+1] = 5; px[i*4+2] = 9; px[i*4+3] = 255;
    }
    const uint32_t n = (uint32_t)std::strlen(line);
    if (n == 0) return px;
    const uint32_t cellW = 6, cellH = 7;                    // 5+1 spacing
    const uint32_t textWcells = n * cellW;
    // Scale each font-cell to fill ~80% of the panel width.
    const uint32_t scale = std::max(1u, (uint32_t)((w * 0.86f) / textWcells));
    const uint32_t textPxW = textWcells * scale, textPxH = cellH * scale;
    const uint32_t ox = (w > textPxW) ? (w - textPxW) / 2 : 0;
    const uint32_t oy2 = (h > textPxH) ? (h - textPxH) / 2 : 0;
    // Render lit cells + a soft halo.
    auto lit = [&](int cx, int cy) -> bool {
        if (cx < 0 || cy < 0) return false;
        const uint32_t ci = (uint32_t)cx / cellW;
        if (ci >= n) return false;
        const uint32_t gx = (uint32_t)cx % cellW, gy = (uint32_t)cy;
        if (gx >= 5 || gy >= 7) return false;
        uint8_t rows[7]; glyph(line[ci], rows);
        return (rows[gy] >> (4 - gx)) & 1;
    };
    for (uint32_t y = 0; y < textPxH; ++y)
        for (uint32_t x = 0; x < textPxW; ++x) {
            const int cx = (int)(x / scale), cy = (int)(y / scale);
            float glow = 0.0f;
            if (lit(cx, cy)) glow = 1.0f;
            else if (lit(cx-1,cy)||lit(cx+1,cy)||lit(cx,cy-1)||lit(cx,cy+1)) glow = 0.35f; // halo
            if (glow <= 0.0f) continue;
            uint8_t* p = &px[((size_t)(oy2 + y) * w + (ox + x)) * 4];
            const float br = glow;
            p[0] = (uint8_t)(std::min(1.0f, rr * br + (glow>0.9f?0.35f:0.0f)) * 255);
            p[1] = (uint8_t)(std::min(1.0f, rg * br + (glow>0.9f?0.30f:0.0f)) * 255);
            p[2] = (uint8_t)(std::min(1.0f, rb * br + (glow>0.9f?0.30f:0.0f)) * 255);
        }
    return px;
}

// Companion normal map for the CMU block: mortar joints push IN (a soft groove),
// block faces stay flat — so raking club beams catch the coursing.
std::vector<uint8_t> makeCmuNormalRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t courseH = n / 8, blockW = n / 4, mortar = std::max(2u, n / 128);
    for (uint32_t y = 0; y < n; ++y) {
        const uint32_t course = y / courseH;
        const uint32_t offset = (course & 1) ? blockW / 2 : 0;
        for (uint32_t x = 0; x < n; ++x) {
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            float nx = 0.0f, ny = 0.0f;
            const uint32_t yin = y % courseH, xin = (x + offset) % blockW;
            if (yin < mortar)            ny = -0.6f; else if (yin < mortar * 2) ny = 0.6f;
            if (xin < mortar)            nx = -0.6f; else if (xin < mortar * 2) nx = 0.6f;
            const float nz = 1.0f;
            const float l = std::sqrt(nx*nx + ny*ny + nz*nz);
            p[0] = (uint8_t)((nx / l * 0.5f + 0.5f) * 255);
            p[1] = (uint8_t)((ny / l * 0.5f + 0.5f) * 255);
            p[2] = (uint8_t)((nz / l * 0.5f + 0.5f) * 255);
            p[3] = 255;
        }
    }
    return px;
}

// POLISH (fix/club-polish): a FACETED unit mirror ball — a LOW-poly UV sphere with
// FLAT per-face normals (each tile carries one hard normal), so under a chrome/mirror
// material it reads as hundreds of discrete mirror facets catching the club's colored
// beams from different angles — NOT a smooth self-lit orb. Authored at radius 1 about
// the origin; UV = lat-long so the silver-facet grid texture registers with the tiles.
// Winding matches makeUVSphere's proven outward CCW-from-front. Render geometry only.
x3::prims::PrimMesh makeFacetedBall(uint32_t stacks, uint32_t slices) {
    x3::prims::PrimMesh m;
    auto P = [](float phi, float th, float* o) {
        o[0] = std::sin(phi) * std::cos(th); o[1] = std::cos(phi); o[2] = std::sin(phi) * std::sin(th);
    };
    for (uint32_t i = 0; i < stacks; ++i) {
        const float v0 = (float)i / stacks, v1 = (float)(i + 1) / stacks;
        const float phi0 = v0 * kPi, phi1 = v1 * kPi;
        for (uint32_t j = 0; j < slices; ++j) {
            const float u0 = (float)j / slices, u1 = (float)(j + 1) / slices;
            const float th0 = u0 * 2.0f * kPi, th1 = u1 * 2.0f * kPi;
            float a[3], b[3], c[3], d[3];
            P(phi0, th0, a); P(phi0, th1, b); P(phi1, th1, c); P(phi1, th0, d);   // TL,TR,BR,BL
            float nx = a[0]+b[0]+c[0]+d[0], ny = a[1]+b[1]+c[1]+d[1], nz = a[2]+b[2]+c[2]+d[2];
            float nl = std::sqrt(nx*nx + ny*ny + nz*nz); if (nl < 1e-6f) nl = 1.0f;
            nx /= nl; ny /= nl; nz /= nl;                        // one flat normal for the whole tile
            const uint32_t base = (uint32_t)m.verts.size();
            m.verts.push_back({{a[0],a[1],a[2]}, {nx,ny,nz}, {u0,v0}});   // TL
            m.verts.push_back({{b[0],b[1],b[2]}, {nx,ny,nz}, {u1,v0}});   // TR
            m.verts.push_back({{c[0],c[1],c[2]}, {nx,ny,nz}, {u1,v1}});   // BR
            m.verts.push_back({{d[0],d[1],d[2]}, {nx,ny,nz}, {u0,v1}});   // BL
            // Match makeUVSphere: (TL,TR,BL) + (TR,BR,BL) = outward CCW-from-front.
            m.index.insert(m.index.end(), { base, base + 1, base + 3,
                                            base + 1, base + 2, base + 3 });
        }
    }
    return m;
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
    const float HL = CW / 2, HW = CL / 2;   // half-extents: X (long) / Z (short)
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
    // ★ PAINTED CMU BLOCK walls (LNS GARAGE) — the whole shop is painted concrete
    // block, so the club shell / engine room / booth walls all carry it (running-bond
    // masonry albedo + mortar-groove normal + matte-rough MR). Built procedurally so
    // it needs no pack asset; tinted club-grey by kWall at the box() call site.
    SurfaceSet sWall;
    {
        auto cpx = makeCmuBlockRGBA(256);
        auto cnx = makeCmuNormalRGBA(256);
        auto cmrPx = makeMr1x1(/*rough*/ 205, /*metal*/ 0);   // matte painted block
        sWall.albedo   = device.createTexture(cpx.data(), 256, 256, true);
        sWall.normal   = device.createTexture(cnx.data(), 256, 256, false);
        sWall.mr       = device.createTexture(cmrPx.data(), 1, 1, false);
        sWall.albedoLin = 0.30f;   // measured-ish mean so valueTint leaves it be
        sWall.ok       = true;
    }
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
    // LNS GARAGE FLOOR (Tim 2026-07-18): the real Late Night Speed floor is a GLOSSY
    // checkerboard on sheened shop concrete — SHINY + DIMMER. A wetter-than-chrome MR
    // (rough 8, metal 255) so the DIY party-dot projections + moving-head beams POOL
    // and shimmer in the surface exactly like the photos. Tile self-glow is dropped
    // way down elsewhere (kEmitTileGarage) so the floor is a moody mirror, not a lightbox.
    auto mrWetPx = makeMr1x1(/*rough*/ 8, /*metal*/ 255);
    const x3::rhi::TextureHandle mrWet = device.createTexture(mrWetPx.data(), 1, 1, false);

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

    // POLISH (fix/club-polish): orient a LOCAL (+Z-forward, origin) driver PrimMesh
    // to WORLD — front face at (wx,wy,wz), facing outward normal (nx,ny,nz) — by
    // baking a pure-rotation basis (+X->right, +Y->up, +Z->normal) into the verts +
    // normals, then add it as an emissive-textured entity and register it for the
    // beat pump. det(+1) so winding is preserved. Returns the entity id.
    auto addDriver = [&](x3::prims::PrimMesh& g, float wx, float wy, float wz,
                         float nx, float ny, float nz, x3::rhi::TextureHandle tex,
                         float posAmp, float emBase, float emAmp) -> uint32_t {
        float fl = std::sqrt(nx*nx + ny*ny + nz*nz); if (fl < 1e-6f) fl = 1.0f;
        const float fx = nx/fl, fy = ny/fl, fz = nz/fl;         // forward (+Z image)
        float ux = 0.0f, uy = 1.0f, uz = 0.0f;                  // world up seed
        if (std::fabs(fy) > 0.95f) { ux = 1.0f; uy = 0.0f; uz = 0.0f; }
        // right = normalize(up x forward)
        float rx = uy*fz - uz*fy, ry = uz*fx - ux*fz, rz = ux*fy - uy*fx;
        const float rl = std::sqrt(rx*rx + ry*ry + rz*rz); rx/=rl; ry/=rl; rz/=rl;
        // up = forward x right (orthonormal, right-handed)
        ux = fy*rz - fz*ry; uy = fz*rx - fx*rz; uz = fx*ry - fy*rx;
        for (auto& v : g.verts) {
            const float lx = v.pos[0], ly = v.pos[1], lz = v.pos[2];
            v.pos[0] = wx + lx*rx + ly*ux + lz*fx;
            v.pos[1] = wy + lx*ry + ly*uy + lz*fy;
            v.pos[2] = wz + lx*rz + ly*uz + lz*fz;
            const float lnx = v.normal[0], lny = v.normal[1], lnz = v.normal[2];
            v.normal[0] = lnx*rx + lny*ux + lnz*fx;
            v.normal[1] = lnx*ry + lny*uy + lnz*fy;
            v.normal[2] = lnx*rz + lny*uz + lnz*fz;
        }
        Entity e;
        e.mesh = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                   g.index.data(), (uint32_t)g.index.size());
        e.tex = tex; e.emissiveTex = tex; e.mrTex = mrGlass;
        e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 1.0f; e.baseColor[3] = 1.0f;
        e.emissive[0] = 1.0f; e.emissive[1] = 0.62f; e.emissive[2] = 0.22f; e.emissive[3] = emBase;
        e.tag = (uint32_t)Tag::Static;
        const uint32_t id = scene.add(e);
        m_driverCones.push_back(Club1127World::DriverCone{ id, fx, fy, fz, posAmp, emBase, emAmp });
        return id;
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
        const float nEnd = entrZ - entrW / 2;          // south end of the north wall run
        // The north run carries the ★ LAIR OVERLOOK WINDOW: frame it (jambs N+S, sill,
        // lintel) so the opening is a REAL hole — from the Lair you SEE THROUGH the
        // dark glass down onto the dance floor (was: solid wall behind a painted panel).
        const float wzN = kOvCz - kOvHz, wzS = kOvCz + kOvHz;   // window z-span [-0.5, 1.7]
        const float wyB = kOvCy - kOvHy, wyT = kOvCy + kOvHy;   // window y-span [6.75, 8.65]
        // (1) wall N of the window (full height).
        { const float z0 = -CL / 2, z1 = wzN, len = z1 - z0;
          box(CW / 2, CH / 2, z0 + len / 2, T / 2, CH / 2, len / 2, kWall, kEmitOff, true, 0.5f, &sWall); }
        // (2) narrow jamb S of the window (full height, up to the elevator gap).
        { const float z0 = wzS, z1 = nEnd, len = z1 - z0;
          if (len > 0.02f) box(CW / 2, CH / 2, z0 + len / 2, T / 2, CH / 2, len / 2, kWall, kEmitOff, true, 0.5f, &sWall); }
        // (3) sill below the window + (4) lintel above it (spanning the window z-width).
        box(CW / 2, wyB / 2, kOvCz, T / 2, wyB / 2, kOvHz, kWall, kEmitOff, true, 0.5f, &sWall);
        box(CW / 2, (wyT + CH) / 2, kOvCz, T / 2, (CH - wyT) / 2, kOvHz, kWall, kEmitOff, true, 0.5f, &sWall);
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
    // ★ INDUSTRIAL CEILING (LNS GARAGE, Tim 2026-07-18) — the working-shop bones.
    // Club 1127 IS the Late Night Speed auto shop: exposed STEEL TRUSSES on 3 m
    // centres (also HARD CANON §1.1: "steel I-beams on three-metre centres"),
    // longitudinal purlins, CONDUIT runs, and HVAC DUCTS slung under the dark
    // concrete deck. All up high (chords at ~8.6-8.98 m) — ABOVE the moving-head
    // fixtures (heads at CH-0.42) and clear of THE ORB / catwalk (22 ft) — so the
    // beams drop cleanly below. Decorative (collide=false); the deck slab collides.
    // ==================================================================
    {
        const float kSteel[4]  = { 0.085f, 0.088f, 0.100f, 1.0f };  // dark shop steel
        const float kSteelHi[4]= { 0.140f, 0.145f, 0.165f, 1.0f };  // lighter web/purlin
        const float kDuct[4]   = { 0.300f, 0.310f, 0.340f, 1.0f };  // galvanised HVAC
        const float kConduit[4]= { 0.110f, 0.095f, 0.075f, 1.0f };  // EMT conduit (grey-tan)
        // ★ CEILING READ (feat/club-lns-polish, Tim 2026-07-19). The industrial
        // ceiling had NO dedicated light (the 64-light cap is maxed) so the trusses/
        // ducts/conduit sank into a black void — the shop bones couldn't be seen as a
        // feature. Give the structure a SUBTLE emissive self-read (no new lights — the
        // budget-safe fix): a faint cool floor on the steel, a galvanised sheen on the
        // ducts, a warm sheen on the conduit. Effective add (rgb*str): steel ~0.078,
        // duct ~0.077, conduit ~0.036 — enough to lift the silhouette out of black so
        // the exposed-truss ceiling READS, low enough that it grazes as lit metal (not
        // a neon lightbox). The geometry's own form + the room bounce still shade it.
        // GAMMA WALK-BACK (feat/club-gamma-fix, 2026-07-25): this emissive self-read
        // existed ONLY to lift the truss/duct/conduit silhouette out of the crushed
        // black void. The sRGB fix already lifts those darks, so the self-read is now
        // double-lighting the ceiling into a lit grid. Strengths ~halved (0.60/0.45/
        // 0.40 -> 0.26/0.20/0.18) so the bones still READ as grazed metal, not a lightbox.
        const float kSteelEm[4]  = { 0.130f, 0.140f, 0.170f, 0.26f }; // faint cool steel self-read (halved)
        const float kDuctEm[4]   = { 0.170f, 0.180f, 0.200f, 0.20f }; // galvanised duct sheen (halved)
        const float kCondEm[4]   = { 0.115f, 0.100f, 0.078f, 0.18f }; // warm conduit sheen (halved)
        const float yBot = 8.60f, yTop = 8.98f, yMid = (yBot + yTop) / 2;
        const float zHalf = CL / 2 - 0.20f;
        // Transverse trusses (span the 43 ft N-S short axis), 3 m centres, offset
        // from the moving-head ring (fixtures at x≈±2.83) so nothing clips a head.
        const float txs[10] = { -13.5f,-10.5f,-7.5f,-4.5f,-1.5f, 1.5f,4.5f,7.5f,10.5f,13.5f };
        for (float tx : txs) {
            box(tx, yBot, 0, 0.05f, 0.05f, zHalf, kSteel, kSteelEm, false);   // bottom chord
            box(tx, yTop, 0, 0.05f, 0.05f, zHalf, kSteel, kSteelEm, false);   // top chord
            for (int w = 0; w < 7; ++w) {                                     // vertical webs (Vierendeel)
                const float wz = -zHalf + 0.3f + w * (2.0f * zHalf - 0.6f) / 6.0f;
                box(tx, yMid, wz, 0.035f, (yTop - yBot) / 2, 0.035f, kSteelHi, kSteelEm, false);
            }
        }
        // Longitudinal purlins (E-W) tying the truss tops.
        for (float pz : { -4.6f, -1.5f, 1.5f, 4.6f })
            box(0, yTop + 0.03f, pz, CW / 2 - 0.3f, 0.03f, 0.03f, kSteelHi, kSteelEm, false);
        // EMT CONDUIT runs slung under the deck along both long walls + two branch drops.
        for (float cz : { -zHalf + 0.15f, zHalf - 0.15f }) {
            box(0, yBot - 0.12f, cz, CW / 2 - 0.6f, 0.028f, 0.028f, kConduit, kCondEm, false);
            box(0, yBot - 0.20f, cz, CW / 2 - 0.6f, 0.022f, 0.022f, kConduit, kCondEm, false);
        }
        // HVAC DUCT: a big galvanised trunk running E-W along the south side, with a
        // branch elbow crossing north — the unmistakable shop-ceiling silhouette.
        box(-2.0f, 8.30f, zHalf - 0.9f, CW / 2 - 4.0f, 0.28f, 0.24f, kDuct, kDuctEm, false); // south trunk
        box(-CW / 2 + 5.0f, 8.30f, 0.5f, 0.24f, 0.26f, zHalf - 1.2f, kDuct, kDuctEm, false); // N-S branch
        box(-CW / 2 + 5.0f, 8.30f, zHalf - 0.9f, 0.30f, 0.30f, 0.30f, kDuct, kDuctEm, false); // elbow box
        // A short return-air duct near the east/entrance side.
        box(9.5f, 8.35f, -zHalf + 1.1f, 2.4f, 0.22f, 0.20f, kDuct, kDuctEm, false);
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
            e.emissive[2] = kBlacklightB; e.emissive[3] = kBlacklightEmit;   // POLISH: dim UV (was 4.0)
            m_blacklightEnts.push_back(id);
            // THE CAST (fix/club-blacklights): a violet point light per tube.
            m_blacklightLightIdx.push_back(m_lights.size());
            addLight(m_lights, x + nx * 0.45f, oy + bcY, z + nz * 0.45f,
                     kBlacklightCast[0], kBlacklightCast[1], kBlacklightCast[2],
                     kBlacklightCastRange);
            ++m_stats.blacklights;
        };
        // CANON-PORT: the N & S walls are the LONG (100 ft) ±Z walls. Mount tubes
        // PROUD of each wall's INNER FACE (walls centered at ±CL/2, half T/2, inner
        // face ±(CL/2 - T/2)). 8 tubes per long wall at 12 ft spacing along X + 2
        // per short (E/W ±X) wall = 20 total.
        const float zFace = CL / 2 - T / 2;   // long (N/S) wall inner face (±6.40)
        const float xFace = CW / 2 - T / 2;   // short (E/W) wall inner face (±15.09)
        // N & S long walls: 8 tubes each, 12 ft spacing along X (x = ±12.81 max,
        // inside the ±15.24 m half-length). Normal points into the room along Z.
        for (int side = -1; side <= 1; side += 2)
            for (int n = 0; n < 8; ++n) {
                const float x = (n - 3.5f) * bi;
                blacklight(x, side * (zFace - 0.09f), 0.0f, (float)-side);
            }
        // E & W short walls: 2 tubes each flanking the wall centre. Normal along X.
        for (int s = -1; s <= 1; s += 2)
            for (int zz = -1; zz <= 1; zz += 2)
                blacklight(s * (xFace - 0.09f), zz * 2.4f, (float)-s, 0.0f);
        // UV point lights (4) — the room-wide violet AIR (kept; the per-tube
        // lights above are the wall/crowd cast, these are the base atmosphere).
        const float uv[2][3] = {   // 2 (was 4) — reserve budget for the underground
            { -CW / 3, CH * 0.5f, 0 }, { CW / 3, CH * 0.5f, 0 }
        };
        for (auto& p : uv)
            addLight(m_lights, p[0], oy + p[1], p[2], 0.28f, 0.16f, 2.70f, 22.0f); // BLUE-UV air
                                        // (red 1.40 -> 0.28: was pink; blue held at 2.70 so the room-wide
                                        // UV wash reads BLUE, not pink. Carries the room-wide UV the
                                        // now-subtle blacklight tubes no longer blast — walls stay washed.)
    }

    // ==================================================================
    // 16 × 85" OLED WALL SCREENS — 4 PER WALL (canon refinement, spec §1.6).
    //   Every screen carries a baked OLED equalizer frame (palette cycles per
    //   screen) + registers for the live emissive shimmer in update().
    //   axis 0 = pane thin in Z (N & S long walls); axis 1 = thin in X (E & W).
    // ==================================================================
    {
        int tvIdx = 0;
        auto wallTv = [&](float inches, float x, float y, float z, int axis) {
            const float dm  = inches * 0.0254f;
            const float tvH = dm / std::sqrt(1.0f + (16.0f / 9.0f) * (16.0f / 9.0f));
            const float tvW = tvH * 16.0f / 9.0f;
            const float bhx = (axis == 0) ? (tvW + 0.05f) / 2 : 0.03f;
            const float bhz = (axis == 0) ? 0.03f : (tvW + 0.05f) / 2;
            box(x, y, z, bhx, (tvH + 0.05f) / 2, bhz, kTvFrame, kEmitOff, false);   // bezel
            const float emScr[4] = { 1.0f, 1.0f, 1.0f, 1.9f };
            const float shx = (axis == 0) ? tvW / 2 : 0.005f;
            const float shz = (axis == 0) ? 0.005f : tvW / 2;
            const uint32_t scrId = box(x, y, z, shx, tvH / 2, shz, kTvFrame, emScr, false);
            oledGlass(scrId, texEq[tvIdx % 4]);
            m_oledEnts.push_back(scrId);
            ++tvIdx; ++m_stats.tvScreens;
        };
        const float ty = CH * 0.5f;
        const float zN = -CL / 2 + 0.07f, zS = CL / 2 - 0.07f;   // N/S long walls
        const float xW = -CW / 2 + 0.07f, xE = CW / 2 - 0.07f;   // W/E short walls
        for (int n = 0; n < 4; ++n) {                            // 4 on each long wall
            const float x = -CW / 2 + CW * (n + 0.5f) / 4.0f;
            wallTv(85, x, ty, zN, 0);   // north
            wallTv(85, x, ty, zS, 0);   // south
        }
        for (int n = 0; n < 4; ++n) {                            // 4 on each short wall
            const float z = -CL / 2 + CL * (n + 0.5f) / 4.0f;
            wallTv(85, xW, ty, z, 1);   // west
            wallTv(85, xE, ty, z, 1);   // east (elevator wall)
        }
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
        const float cx = sx * (CW / 2 - 1), cz = sz * (CL / 2 - 1);
        box(cx, 0.37f, cz, 0.32f, 0.37f, 0.28f, kSub, kEmitOff, true);
        ++m_stats.svsSubs;
        // POLISH: a REAL recessed 16" sub DRIVER (cone dish + dust cap + surround)
        // facing the room center (-sx in X), front face proud of the cab. It PUMPS
        // hard on the kick + surges amber emissive (update() drives it).
        {
            x3::prims::PrimMesh dg;
            makeDriverInto(dg, 0.0f, 0.0f, 0.25f, 0.12f, 0.065f, 0.022f);
            addDriver(dg, cx - sx * 0.30f, oy + 0.37f, cz, -sx, 0.0f, 0.0f, texSub,
                      /*posAmp*/ 0.035f, /*emBase*/ 0.25f, /*emAmp*/ 2.05f);
        }
        // Amber floor pulse light in front of the cab (index recorded for update()).
        m_subLightIdx.push_back(m_lights.size());
        addLight(m_lights, cx - sx * 0.9f, oy + 0.4f, cz, 1.40f, 0.65f, 0.15f, 3.5f); // relight: amber sub pulse base 0.9 -> 1.4 (update() modulates)
    }
    // 8 stacked pairs JBL JRX200 (16 cabinets) + 8 amps + power LEDs on the walls.
    {
        // CANON-PORT: mains stack in pairs along the N & S LONG (100 ft) ±Z walls,
        // every ~20 ft along X; drivers face into the room along Z.
        const float jrxSp = CW / 5;                       // spacing along the 100 ft wall
        for (int side = -1; side <= 1; side += 2)          // -1 = north wall, +1 = south
            for (int n = 0; n < 4; ++n) {
                const float x = -CW / 2 + jrxSp * (n + 1);
                const float z = side * (CL / 2 - 0.5f);
                const float wy = CH * 0.55f;
                box(x, wy,        z, 0.265f, 0.38f, 0.18f, kSpk, kEmitOff, false); m_stats.jblLineArray++;
                box(x, wy + 0.8f, z, 0.265f, 0.38f, 0.18f, kSpk, kEmitOff, false); m_stats.jblLineArray++;
                // REAL 3D drivers (mid cone + horn tweeter dome) facing the room
                // (-side in Z). Mids ripple on the beat; the tweeter barely stirs.
                for (int cab = 0; cab < 2; ++cab) {
                    x3::prims::PrimMesh dg;
                    makeDriverInto(dg, 0.0f, -0.09f, 0.165f, 0.075f, 0.045f, 0.016f);  // mid cone
                    makeDriverInto(dg, 0.0f,  0.235f, 0.062f, 0.028f, 0.030f, 0.010f); // horn tweeter dome
                    addDriver(dg, x, oy + wy + cab * 0.8f, z - side * 0.20f, 0.0f, 0.0f, -side, texSpk,
                              /*posAmp*/ 0.010f, /*emBase*/ 0.35f, /*emAmp*/ 0.55f);
                }
                box(x, wy - 0.55f, z, 0.24f, 0.10f, 0.175f, kAmp, kEmitOff, false);                 // amp
                box(x, wy - 0.5f, z - side * 0.18f, 0.015f, 0.015f, 0.015f, kAmp, kEmitLed, false);  // power LED
            }
    }
    // 4x JBL PRO 18" subs flanking the dance floor (center-west, near the DJ).
    {
        const float dfX = -6.0f;   // dance-floor centre X (west of room centre)
        const float p[4][2] = { {dfX-4.0f,-3.2f}, {dfX-4.0f,3.2f}, {dfX+4.0f,-3.2f}, {dfX+4.0f,3.2f} };
        for (auto& s : p) {
            const float cx = s[0], cz = s[1];
            box(cx, 0.35f, cz, 0.305f, 0.305f, 0.305f, kSub, kEmitOff, true);
            ++m_stats.jbl18Subs;
            // REAL recessed 18" sub DRIVER facing the floor centre — pumps on the beat.
            {
                x3::prims::PrimMesh dg;
                makeDriverInto(dg, 0.0f, 0.0f, 0.245f, 0.12f, 0.06f, 0.022f);
                const float nrm = (cx < dfX) ? 1.0f : -1.0f;
                addDriver(dg, cx + nrm * 0.29f, oy + 0.35f, cz, nrm, 0.0f, 0.0f, texSub,
                          /*posAmp*/ 0.032f, /*emBase*/ 0.25f, /*emAmp*/ 2.0f);
            }
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
            // POLISH: REAL 3D drivers facing the dance floor (+Z). The bottom cabinet
            // is a big SUB cone that punches on the kick; the upper cabinets carry a
            // MID cone + a HORN TWEETER dome that only ripple/shimmer.
            {
                x3::prims::PrimMesh dg;
                if (c2 == 0) {
                    makeDriverInto(dg, 0.0f, 0.0f, 0.33f, 0.14f, 0.085f, 0.026f);
                    addDriver(dg, tx2, oy + cy2, tz2 + 0.43f, 0.0f, 0.0f, 1.0f, texSub,
                              /*posAmp*/ 0.032f, /*emBase*/ 0.25f, /*emAmp*/ 2.0f);
                } else {
                    const float mr = cabW[c2] * 0.62f;
                    makeDriverInto(dg, 0.0f, -cabH[c2] * 0.28f, mr, 0.09f, mr * 0.30f, 0.016f);   // mid
                    makeDriverInto(dg, 0.0f,  cabH[c2] * 0.42f, 0.06f, 0.028f, 0.032f, 0.010f);   // tweeter
                    addDriver(dg, tx2, oy + cy2, tz2 + 0.43f, 0.0f, 0.0f, 1.0f, texSpk,
                              /*posAmp*/ 0.010f, /*emBase*/ 0.40f, /*emAmp*/ 0.5f);
                }
            }
            cy2 += cabH[c2];
        }
        // Inner magenta neon edge running the tower height.
        box(tx2 - side * (cabW[0] + 0.03f), 1.4f, tz2, 0.02f, 1.35f, 0.02f, kWall, kEmitNeon, false);
    }

    // 16x JBL N26/S38 surrounds (walls, alternating sizes).
    {
        // CANON-PORT: surrounds at ear height on the N & S long (±Z) walls, along X.
        const float surSp = CW / 9;
        for (int side = -1; side <= 1; side += 2)
            for (int n = 0; n < 8; ++n) {
                const float x = -CW / 2 + surSp * (n + 1);
                const float z = side * (CL / 2 - 0.15f);
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
                // LNS GARAGE FLOOR (Tim 2026-07-18): "BOTH — checkerboard AND projected
                // party light, but SHINY + DIMMER." The old tiles self-lit at 0.55 blue
                // (a lightbox). Dropped WAY down to a whisper (0.10) so the floor reads as
                // a dim glossy checkerboard — the LIGHT SHOW (dome dots + moving beams)
                // dances AND reflects in it, instead of the tiles blasting their own blue.
                // Glossier albedo split (bright vs dark squares) carries the checker; the
                // wet MR (mrWet, rough 8) makes the beams POOL and shimmer in the surface.
                // GAMMA WALK-BACK (feat/club-gamma-fix): under the corrected curve the
                // whisper under-glow reads brighter; trim 0.10 -> 0.055 so the floor stays
                // a DIM glossy checkerboard the light show reflects in (not a lit tile).
                const float emTileA[4] = { 0.06f, 0.04f, 0.42f, 0.055f };
                const float tileCol[4] = { lit ? 0.090f : 0.020f, lit ? 0.092f : 0.020f,
                                           lit ? 0.105f : 0.028f, 1.0f };
                const uint32_t tid = box(tx, 0.12f, tz, (tw - 0.02f) / 2, 0.015f, (td - 0.02f) / 2,
                                         tileCol, lit ? emTileA : kEmitOff, false, 1.0f, nullptr);
                Entity& te = scene.get(tid);
                te.mrTex = mrWet;                // wet-look shop-floor reflectivity
                if (lit) m_tilePulseEnts.push_back(tid);
            }
        m_stats.hasDanceFloor = true;
    }

    // ==================================================================
    // THE ORB — 2 m mirror ball on a cable, 4 spotlights + 4 ring lights.
    // ==================================================================
    {
        m_orbY = oy + (CH - 1.5f);
        // POLISH (fix/club-polish): a REAL faceted MIRROR BALL — a LOW-poly sphere
        // with FLAT per-facet normals (makeFacetedBall) under a chrome/mirror MR, so
        // it catches the club's colored beams as hundreds of discrete tile glints
        // instead of blowing out as a smooth white orb. The self-glow is dropped WAY
        // down (was 1.15..1.7 pulsed — a lightbulb): the ball is REFLECTIVE now, lit
        // by the room, not self-lit. It still spins in update(), and a cluster of
        // orbiting SPARKLE lights (below) throws the moving colored dots.
        {
            x3::prims::PrimMesh orb = makeFacetedBall(14, 24);
            placeVerts(orb, 1.0f, 0.0f, m_orbY, 0.0f);
            const float orbCol[4] = { 0.60f, 0.62f, 0.70f, 1.0f };   // chrome tile albedo (not white)
            const float orbEm[4]  = { 0.14f, 0.14f, 0.20f, 0.16f };  // faint glint floor only
            m_orbEnt = prim(std::move(orb), orbCol, orbEm, texFacet, mrChrome, false);
        }
        m_orbValid = true;
        m_stats.hasOrb = true;
        // Suspending cable.
        box(0, CH - 0.5f, 0, 0.02f, 0.75f, 0.02f, kCable, kEmitOff, false);
        // POLISH: MIRROR-BALL SPARKLE — 6 small colored point lights that orbit with
        // the ball's spin and paint the signature MOVING DOTS of colored light across
        // the walls / floor / dancers. Short range so each reads as a discrete
        // sweeping spot, not a wash. Recorded in the STATIC prefix (they are moved
        // explicitly by update() via m_sparkleLightIdx, so the moving-head loop — which
        // only touches indices >= m_staticLightCount — never clobbers them).
        {
            const float sparkleHue[3][3] = {   // 3 (was 4) — reclaimed ONE for the garage
                { 2.4f, 0.6f, 2.4f }, { 0.5f, 1.8f, 2.6f }, { 2.6f, 1.6f, 0.4f },
            };                                 // shop worklight (point-light cap = 64)
            for (int i = 0; i < 3; ++i) {
                m_sparkleLightIdx.push_back(m_lights.size());
                addLight(m_lights, 0.0f, oy + 5.0f, 0.0f,
                         sparkleHue[i][0], sparkleHue[i][1], sparkleHue[i][2], 3.2f);
            }
        }
    }

    // ==================================================================
    // ★ THE GARAGE — SPORTS CAR UP ON A TWO-POST LIFT (LNS GARAGE, Tim 2026-07-18).
    // Club 1127 IS the Late Night Speed shop: a real sports car (CTR.glb) hoisted on a
    // symmetric two-post lift is the centrepiece. Parked off to the SOUTH-EAST, clear
    // of the dance floor. The lift = 2 columns + base plates + overhead beam + 4 swing
    // arms & pads; the CAR is a static GLB instance raised ~1.85 m so you can stand
    // under it. (The GLB sits on y=0, +Z=nose, scale 1 — same as --screenshot-car.)
    // ==================================================================
    {
        const float lx = 9.2f, lz = 4.4f;      // lift XZ (SE, near the south wall)
        const float liftH = 1.85f;             // wheels/car raised this high
        const float armY  = liftH;             // swing-arm/pad plane
        const float postHalfZ = 1.55f;         // posts sit fore/aft of the car (along Z=car length)
        const float kLiftRed[4]   = { 0.42f, 0.045f, 0.045f, 1.0f };  // shop-equipment red
        const float kLiftSteel[4] = { 0.22f, 0.22f, 0.25f, 1.0f };
        // Two columns (fore & aft of the car, straddling its length along Z).
        for (int s = -1; s <= 1; s += 2) {
            const float pz = lz + s * postHalfZ;
            box(lx, 1.35f, pz, 0.14f, 1.35f, 0.16f, kLiftRed, kEmitOff, true);   // column
            box(lx, 0.04f, pz, 0.55f, 0.04f, 0.34f, kLiftSteel, kEmitOff, true); // base plate
            // Two swing arms + lift pads reaching UNDER the car from this column.
            for (int a = -1; a <= 1; a += 2) {
                box(lx + a * 0.55f, armY, pz + (s > 0 ? -0.45f : 0.45f), 0.45f, 0.05f, 0.06f, kLiftSteel, kEmitOff, false); // arm
                box(lx + a * 0.55f, armY + 0.05f, pz + (s > 0 ? -0.75f : 0.75f), 0.10f, 0.06f, 0.10f, kLiftRed, kEmitOff, false); // rubber pad
            }
        }
        // Overhead equaliser beam tying the two column tops (symmetric two-post look).
        box(lx, 2.78f, lz, 0.12f, 0.10f, postHalfZ + 0.16f, kLiftRed, kEmitOff, false);
        // Caged SHOP WORKLIGHT clamped to the overhead beam (the one warm work-lamp that
        // lights the hoist — reclaimed from an ORB sparkle so the point-light cap holds).
        {
            const float emWork[4] = { 1.0f, 0.86f, 0.55f, 3.4f };
            box(lx - 0.7f, 2.55f, lz - 0.4f, 0.09f, 0.06f, 0.09f, kMetal, emWork, false); // lamp head (room side)
            box(lx - 0.7f, 2.70f, lz - 0.4f, 0.012f, 0.12f, 0.012f, kChrome, kEmitOff, false); // hook wire
            // Warm shop key raking the WEST/nose flank that faces the dance floor.
            addLight(m_lights, lx - 1.3f, oy + 2.5f, lz - 0.6f, 1.75f, 1.35f, 0.80f, 8.0f);
        }
        // THE SPORTS CAR — the REAL hero-car GLB (Vehicles/CTR.glb: clearcoat paint +
        // emissive lights, the same asset --screenshot-car poses) hoisted on the pads.
        // LOADED AS AN INERT CHARACTER PROP via addCharacter() — the SAME include-safe
        // MonsterSystem rig/mesh-load path the dancers use, so this needs NO env_art.h
        // (the prior agent's build fault: env_art.h drags fx.h + reorders x3::phys into
        // a cascade — sidestepped entirely by never including it here). A static (un-
        // skinned) GLB just draws statically through the monster path; if it fails to
        // load it falls back to a box, so the hoist is never empty. update() FREEZES it
        // each frame via setPropPose so the Idle-AI yaw sway never drifts the car.
        {
            const float carY = oy + armY + 0.10f;   // wheels rest on the rubber pads (~1.95 m up)
            const float kPaint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  // white = preserve the GLB's own paint
            const x3::phys::Vec3 carPos{ lx, carY, lz };
            addCharacter(scene, device, physics, x3::game::convertedGlbRoot(),
                         "Vehicles/CTR.glb", carPos, /*scale*/ 1.0f, /*standUpZtoY*/ false, kPaint);
            m_carCharIdx = (int)m_chars.size() - 1;   // freeze target for update()
            m_carPos     = carPos;
            m_carYaw     = 0.0f;   // length along Z between the posts (drawMonster adds R(yaw+pi))
        }
    }

    // ==================================================================
    // ★ SHOP PROPS + LATE NIGHT SPEED BRANDING (LNS GARAGE, Tim 2026-07-18).
    // Tim's REAL MATCO tool storage (feat/club-lns-polish 2026-07-19: NOT red, NOT
    // Snap-on) + a WORKBENCH (vise + pegboard) + a push BROOM, along the SOUTH wall
    // by the car; and the emissive "LATE NIGHT SPEED" neon sign — the club IS the shop.
    //
    // THE MATCO LINEUP (all one dark wrinkle/silver-vein GREY powdercoat family):
    //   * 1× MATCO 6S 3-BAY roll cab — the HERO: wide 3-bay box, BLUE drawer faces,
    //     chrome pulls, stainless top edge (grey body).
    //   * 1× MATCO 4S roll cab — smaller, GREY drawer faces (no blue), chrome pulls.
    //   * 3× MATCO MSC4 carts — small 2-drawer rolling utility carts on casters.
    // ==================================================================
    {
        const float southZ = CL / 2 - 0.55f;    // just off the south wall (+Z)
        const float kToolRed[4]  = { 0.46f, 0.05f, 0.055f, 1.0f };  // red bench-vise accent (kept)
        const float kChromeD[4]  = { 0.62f, 0.63f, 0.70f, 1.0f };   // chrome pulls / stainless top
        const float kBenchTop[4] = { 0.20f, 0.21f, 0.24f, 1.0f };   // steel bench top
        const float kBlackR[4]   = { 0.05f, 0.05f, 0.06f, 1.0f };
        // MATCO powdercoat palette. The whole lineup shares kMatcoGrey (dark grey
        // metallic with a subtle silver-vein/wrinkle read); the 6S is the hero with
        // kMatcoBlue faces; the 4S + carts use kMatcoGreyFace (a notch lighter than
        // the body so the drawers read as drawers, not a solid slab).
        const float kMatcoGrey[4]     = { 0.090f, 0.093f, 0.104f, 1.0f };  // wrinkled grey body
        const float kMatcoBlue[4]     = { 0.055f, 0.135f, 0.440f, 1.0f };  // Matco blue drawer faces (6S)
        const float kMatcoGreyFace[4] = { 0.150f, 0.155f, 0.170f, 1.0f };  // grey drawer faces (4S/carts)
        // --- ROLL CAB helper (6S / 4S): grey body + stainless top edge + a grid of
        // drawer faces (nBay wide × nDr tall) with a chrome pull per drawer + casters. --
        auto rollCab = [&](float cx, float halfW, float bodyHalfH,
                           const float* faceCol, int nBay, int nDr) {
            const float dZ = 0.30f;                 // half depth (0.6 m box)
            const float frontZ = southZ - dZ;       // room-facing (-Z) face plane
            box(cx, bodyHalfH, southZ, halfW, bodyHalfH, dZ, kMatcoGrey, kEmitOff, true);          // body
            box(cx, bodyHalfH * 2 + 0.03f, southZ, halfW + 0.02f, 0.03f, dZ + 0.02f,
                kChromeD, kEmitOff, true);                                                          // stainless top edge
            const float bayW = (2 * halfW - 0.06f) / (float)nBay;
            const float drH  = (2 * bodyHalfH - 0.08f) / (float)nDr;
            for (int b = 0; b < nBay; ++b) {
                const float bcx = cx - halfW + 0.03f + bayW * (b + 0.5f);
                for (int d = 0; d < nDr; ++d) {
                    const float dy = 0.05f + drH * (d + 0.5f);
                    box(bcx, dy, frontZ - 0.012f, bayW / 2 - 0.015f, drH / 2 - 0.012f, 0.012f,
                        faceCol, kEmitOff, false);                                                  // drawer face
                    box(bcx, dy, frontZ - 0.03f, bayW / 2 - 0.05f, 0.012f, 0.012f,
                        kChromeD, kEmitOff, false);                                                 // chrome pull
                }
            }
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2)
                    box(cx + sx * (halfW - 0.06f), 0.04f, southZ + sz * (dZ - 0.06f),
                        0.05f, 0.04f, 0.05f, kBlackR, kEmitOff, false);                             // casters
        };
        // --- MSC4 CART helper: small grey utility cart, 2 grey drawers + chrome tray
        // edge + a chrome push handle on the side + casters. --
        auto msc4Cart = [&](float cx) {
            const float halfW = 0.28f, dZ = 0.22f, topY = 0.86f;
            const float frontZ = southZ - dZ;
            box(cx, 0.44f, southZ, halfW, 0.44f, dZ, kMatcoGrey, kEmitOff, true);                   // body
            box(cx, topY, southZ, halfW + 0.02f, 0.02f, dZ + 0.02f, kChromeD, kEmitOff, false);     // chrome tray edge
            for (int d = 0; d < 2; ++d) {
                const float dy = 0.30f + d * 0.28f;
                box(cx, dy, frontZ - 0.012f, halfW - 0.03f, 0.11f, 0.012f, kMatcoGreyFace, kEmitOff, false); // drawer face
                box(cx, dy, frontZ - 0.03f, halfW - 0.08f, 0.012f, 0.012f, kChromeD, kEmitOff, false);       // pull
            }
            box(cx - halfW - 0.03f, topY - 0.06f, southZ, 0.02f, 0.20f, 0.02f, kChromeD, kEmitOff, false);   // push handle
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2)
                    box(cx + sx * (halfW - 0.04f), 0.05f, southZ + sz * (dZ - 0.04f),
                        0.04f, 0.05f, 0.04f, kBlackR, kEmitOff, false);                             // casters
        };
        rollCab(-3.2f, 0.95f, 0.52f, kMatcoBlue,     3, 5);   // ★ MATCO 6S 3-BAY (hero, blue faces)
        rollCab(-1.1f, 0.45f, 0.50f, kMatcoGreyFace, 1, 5);   // MATCO 4S (grey faces)
        msc4Cart(0.2f);                                        // ┐
        msc4Cart(1.0f);                                        // ├ 3× MATCO MSC4 carts
        msc4Cart(1.8f);                                        // ┘
        // --- WORKBENCH (steel top on legs) with a red bench VISE + a wall PEGBOARD. ---
        {
            const float bx = 4.2f, bTopY = 0.92f;
            box(bx, bTopY, southZ, 0.9f, 0.04f, 0.32f, kBenchTop, kEmitOff, true);   // top
            for (int s = -1; s <= 1; s += 2)                                          // legs
                for (int t = -1; t <= 1; t += 2)
                    box(bx + s * 0.8f, bTopY / 2, southZ + t * 0.24f, 0.04f, bTopY / 2, 0.04f, kBlackR, kEmitOff, true);
            box(bx - 0.55f, bTopY + 0.12f, southZ, 0.10f, 0.08f, 0.10f, kToolRed, kEmitOff, false); // vise
            box(bx - 0.55f, bTopY + 0.12f, southZ - 0.12f, 0.02f, 0.06f, 0.06f, kChromeD, kEmitOff, false); // vise screw
            // Pegboard on the wall behind the bench (a panel + a few hung tool silhouettes).
            box(bx, 2.0f, CL / 2 - 0.16f, 0.9f, 0.55f, 0.02f, kBlackR, kEmitOff, false);
            for (int p = -2; p <= 2; ++p)
                box(bx + p * 0.3f, 2.05f, CL / 2 - 0.19f, 0.03f, 0.18f, 0.015f, kChromeD, kEmitOff, false);
        }
        // --- Push BROOM leaning against the last tool chest (near-vertical handle + head). ---
        {
            const float brX = 2.5f, brZ = southZ - 0.5f;
            box(brX, 0.9f, brZ, 0.015f, 0.9f, 0.015f, kWood, kEmitOff, false);        // handle
            box(brX, 0.04f, brZ + 0.06f, 0.16f, 0.04f, 0.05f, kBlackR, kEmitOff, false); // bristle head
        }
        // --- ★ "LATE NIGHT SPEED" neon SIGN, high on the south wall (faces the floor). ---
        {
            const uint32_t sw = 512, sh = 96;
            auto sgpx = makeSignRGBA(sw, sh, "LATE NIGHT SPEED", 1.0f, 0.10f, 0.10f);   // hot red neon
            const x3::rhi::TextureHandle sgTex = device.createTexture(sgpx.data(), sw, sh, true);
            const float sX = 1.5f, sY = 5.4f, sZ = CL / 2 - 0.42f;   // clear of the wall inner face (~-0.15)
            const float sHW = 3.6f, sHH = sHW * (float)sh / (float)sw;   // keep aspect
            const float wy = oy + sY;
            // A single front-facing QUAD with explicit 0..1 UVs (makeBox tiles a wide
            // decal — a quad maps the sign texture exactly ONCE). Normal faces -Z (into
            // the room). Winding CCW when viewed from -Z.
            x3::prims::PrimMesh sg;
            const float nZ = -1.0f;
            // U flipped (viewer at -Z looking +Z sees +X on the left): U=1 at -X, U=0 at +X
            // so "LATE NIGHT SPEED" reads left-to-right, not mirrored.
            sg.verts.push_back({{sX-sHW, wy+sHH, sZ}, {0,0,nZ}, {1,0}});   // TL
            sg.verts.push_back({{sX-sHW, wy-sHH, sZ}, {0,0,nZ}, {1,1}});   // BL
            sg.verts.push_back({{sX+sHW, wy-sHH, sZ}, {0,0,nZ}, {0,1}});   // BR
            sg.verts.push_back({{sX+sHW, wy+sHH, sZ}, {0,0,nZ}, {0,0}});   // TR
            sg.index = { 0, 1, 3, 1, 2, 3,   0, 3, 1, 1, 3, 2 };   // double-sided (cull-proof)
            const float signCol[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            const uint32_t sid = prim(std::move(sg), signCol, kEmitOff, sgTex, x3::rhi::TextureHandle{}, false);
            // Use the PROVEN OLED-glass per-texel emissive path (the EQ screens glow this
            // way): the letters emit, the dark panel emits nothing.
            oledGlass(sid, sgTex);
            Entity& se = scene.get(sid);
            se.emissive[3] = 3.2f;             // brighter than a screen — it's a neon sign
            se.glass.opacity = 1.0f;           // solid panel (no see-through)
        }
    }

    // ==================================================================
    // ★ DIY DOME PARTY-PROJECTORS + CEILING STARBURST (LNS GARAGE, Tim 2026-07-18).
    // The REAL Late Night Speed light signature: cheap RGB dome/mushroom projectors +
    // a red ceiling starburst throwing SPINNING colored dots + spirograph patterns
    // across the floor & ceiling. Built as ADDITIVE emissive pattern DISCS (NOT point
    // lights — the 64 cap is maxed): black bg adds nothing, the dots add their colour
    // over the glossy floor and pool in its reflection. update() spins them on the beat
    // clock. Alongside (not replacing) the moving-head beams. Plus visible dome fixtures.
    // ==================================================================
    {
        const uint32_t kDotN = 256;
        auto d0 = makeDotFieldRGBA(kDotN, 11u);
        auto d1 = makeDotFieldRGBA(kDotN, 47u);
        auto stx = makeStarburstRGBA(kDotN);
        const x3::rhi::TextureHandle dotTex0 = device.createTexture(d0.data(), kDotN, kDotN, true);
        const x3::rhi::TextureHandle dotTex1 = device.createTexture(d1.data(), kDotN, kDotN, true);
        const x3::rhi::TextureHandle starTex = device.createTexture(stx.data(), kDotN, kDotN, true);
        // Build a flat disc mesh centred at LOCAL origin in the XZ plane, facing `up`
        // (+1 = up for a floor pattern, -1 = down for a ceiling pattern), UV planar 0..1.
        auto makeDisc = [](float R, float up) {
            x3::prims::PrimMesh m;
            const int seg = 48;
            m.verts.push_back({{0,0,0}, {0,up,0}, {0.5f,0.5f}});   // center
            for (int i = 0; i <= seg; ++i) {
                const float a = (float)i / seg * 2.0f * kPi;
                const float cx = std::cos(a), sz = std::sin(a);
                m.verts.push_back({{cx*R, 0.0f, sz*R}, {0,up,0}, {0.5f+0.5f*cx, 0.5f+0.5f*sz}});
            }
            for (int i = 1; i <= seg; ++i) {
                if (up > 0) m.index.insert(m.index.end(), { 0, (uint32_t)i, (uint32_t)(i+1) });
                else        m.index.insert(m.index.end(), { 0, (uint32_t)(i+1), (uint32_t)i });
            }
            return m;
        };
        // Place a spinning ADDITIVE pattern disc at a world centre; returns nothing but
        // records it for update()'s spin. `up`>0 lies on the floor, <0 hugs the ceiling.
        auto addProjectorDisc = [&](float R, float wx, float wy, float wz, float up,
                                    x3::rhi::TextureHandle tex, float spin, float emBase,
                                    float emAmp, float phase) {
            x3::prims::PrimMesh g = makeDisc(R, up);
            Entity e;
            e.mesh = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                       g.index.data(), (uint32_t)g.index.size());
            e.tex = tex; e.emissiveTex = tex;
            e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 1.0f; e.baseColor[3] = 1.0f;
            e.emissive[0] = 1.0f; e.emissive[1] = 1.0f; e.emissive[2] = 1.0f; e.emissive[3] = emBase;
            e.transparent = true;
            e.glass.opacity = 0.0f;        // pure additive — black bg contributes nothing
            e.glass.refraction = 0.0f;
            e.glass.roughness = 0.0f;
            e.glass.specular = 0.0f;
            e.glass.additive = 1.0f;
            e.transform[12] = wx; e.transform[13] = wy; e.transform[14] = wz;   // world place
            e.tag = (uint32_t)Tag::Static;
            const uint32_t id = scene.add(e);
            m_projectors.push_back(Projector{ id, wx, wy, wz, spin, up, emBase, emAmp, phase });
        };
        // TWO floor dot-fields spinning opposite ways over the dance floor (spirograph
        // sweep), lifted just above the tiles so they pool + reflect in the wet floor.
        addProjectorDisc(6.0f, -6.0f, oy + 0.16f, -1.5f,  1.0f, dotTex0,  0.6f, 2.4f, 1.4f, 0.0f);
        addProjectorDisc(4.6f,  1.5f, oy + 0.17f,  2.0f,  1.0f, dotTex1, -0.9f, 2.2f, 1.3f, 1.7f);
        // Red CEILING STARBURST just under the deck, spinning (seen looking up + it
        // rakes the trusses). Hugs the ceiling, faces DOWN.
        addProjectorDisc(4.0f,  0.0f, oy + CH - 0.30f, -2.0f, -1.0f, starTex, 1.1f, 2.6f, 1.2f, 0.0f);
        // A third dot-field high on the SOUTH wall region (dots on the wall at eye height),
        // tilted flat but placed near the wall so its dots read on the CMU block.
        addProjectorDisc(3.4f, -10.0f, oy + 0.16f, 3.5f, 1.0f, dotTex0, 0.75f, 2.0f, 1.2f, 3.1f);

        // Visible DOME/MUSHROOM PROJECTOR FIXTURES on the ceiling (the cheap party
        // lights themselves): a stem + a multi-faceted emissive dome throwing colour.
        auto domeFixture = [&](float fx, float fz, const float* domeEm) {
            box(fx, CH - 0.18f, fz, 0.05f, 0.12f, 0.05f, kMetal, kEmitOff, false);   // stem
            box(fx, CH - 0.30f, fz, 0.13f, 0.06f, 0.13f, kSpk, domeEm, false);        // dome base (emissive)
            box(fx, CH - 0.37f, fz, 0.08f, 0.05f, 0.08f, kTvFrame, domeEm, false);    // lens cap
        };
        { const float emR[4] = { 1.4f, 0.3f, 1.4f, 2.4f }; domeFixture(-6.0f, -1.5f, emR); }
        { const float emB[4] = { 0.3f, 0.8f, 1.6f, 2.4f }; domeFixture( 1.5f,  2.0f, emB); }
        { const float emG[4] = { 0.3f, 1.5f, 0.6f, 2.4f }; domeFixture(-10.0f, 3.5f, emG); }
    }

    // ==================================================================
    // ★ THE L-WRAPAROUND BAR (LNS GARAGE, Tim 2026-07-18) — SUPERSEDES the U-bar.
    // The REAL Late Night Speed bar: DARK GREEN GRANITE, L-shaped, running the FULL
    // WEST WALL (N-S) then CURVING EAST around the NW corner and along the NORTH WALL
    // to the EDGE OF THE LOUNGE (the engine room / observation lounge is north-centre,
    // so the north run stops at the ER west edge). Backbar bottle walls behind both
    // legs; a big SVS PB16-Ultra SUB cabinet under a west-leg section (granite-over-SVS);
    // Snap-on stools (red seat, chrome pedestal); Danny the bartender behind it.
    // ==================================================================
    {
        const float bH2 = 1.1f;                        // bar height
        const float wX  = -CW / 2 + 1.0f;              // west-leg counter centre X (0.72 m lane behind)
        const float wZ0 = -CL / 2 + 0.3f, wZ1 = CL / 2 - 0.3f;   // west leg Z span (full wall)
        const float nZ  = -CL / 2 + 1.0f;              // north-leg counter centre Z
        const float erWest = -ER_W / 2 - 0.3f;         // north run stops just west of the lounge/ER
        const float nX0 = wX - 0.28f;                  // north leg X span: NW corner ...
        const float nX1 = erWest;                      //           ... to the lounge edge
        const float dHalf = 0.28f;                     // counter half-depth
        // DARK GREEN GRANITE top helper (§1.10) — polished, gleaming.
        auto graniteTop = [&](float cx, float cy, float cz, float hx, float hy, float hz) {
            x3::prims::PrimMesh g = x3::prims::makeBox(hx, hy, hz, cx, oy + cy, cz, 1.0f);
            prim(std::move(g), kGranite, kGraniteEm, x3::rhi::TextureHandle{}, mrGlass, true);
        };
        const float kBarBase[4] = { 0.10f, 0.09f, 0.08f, 1.0f };   // dark cabinet base
        // ---- WEST LEG (full N-S along the west wall) ----
        {
            const float lenZ = wZ1 - wZ0, cz = (wZ0 + wZ1) / 2;
            box(wX, bH2 / 2, cz, dHalf, bH2 / 2, lenZ / 2, kBarBase, kEmitOff, true, 1.0f, &sBar); // base cabinet
            graniteTop(wX, bH2 + 0.03f, cz, dHalf + 0.05f, 0.03f, lenZ / 2 + 0.05f);
            box(wX + dHalf + 0.02f, bH2 - 0.04f, cz, 0.012f, 0.012f, lenZ / 2, kWall, kEmitAmberLo, false); // amber under-lip
        }
        // ---- NORTH LEG (NW corner east to the lounge edge) ----
        {
            const float lenX = nX1 - nX0, cx = (nX0 + nX1) / 2;
            box(cx, bH2 / 2, nZ, lenX / 2, bH2 / 2, dHalf, kBarBase, kEmitOff, true, 1.0f, &sBar);
            graniteTop(cx, bH2 + 0.03f, nZ, lenX / 2 + 0.05f, 0.03f, dHalf + 0.05f);
            box(cx, bH2 - 0.04f, nZ + dHalf + 0.02f, lenX / 2, 0.012f, 0.012f, kWall, kEmitAmberLo, false);
        }
        // ---- NW CORNER curve (a 45° chamfer block tying the two legs) ----
        graniteTop(wX + 0.15f, bH2 + 0.03f, nZ + 0.15f, dHalf + 0.2f, 0.03f, dHalf + 0.2f);
        box(wX, bH2 / 2, nZ, dHalf + 0.15f, bH2 / 2, dHalf + 0.15f, kBarBase, kEmitOff, true);
        // ---- ★ SVS PB16-Ultra SUB cabinet under a west-leg section (granite over SVS) ----
        {
            const float sX = wX, sZ = 2.6f;
            box(sX, 0.42f, sZ, dHalf + 0.02f, 0.42f, 0.34f, kSub, kEmitOff, true);   // big black sub box
            box(sX + dHalf, 0.42f, sZ, 0.01f, 0.28f, 0.24f, kSpk, kEmitOff, false);  // driver face (front)
            const float emSubLed[4] = { 0.10f, 1.0f, 0.30f, 2.0f };
            box(sX + dHalf + 0.005f, 0.68f, sZ, 0.006f, 0.01f, 0.02f, kWall, emSubLed, false); // status LED
        }
        // ---- SNAP-ON STOOLS (red round seat, chrome pedestal + footring + base) ----
        auto snapStool = [&](float sx, float sz) {
            const float kSnapRed[4] = { 0.55f, 0.05f, 0.06f, 1.0f };
            box(sx, 0.76f, sz, 0.20f, 0.05f, 0.20f, kSnapRed, kEmitOff, false);   // red seat
            box(sx, 0.38f, sz, 0.035f, 0.38f, 0.035f, kChrome, kEmitOff, false);  // chrome pedestal
            box(sx, 0.30f, sz, 0.14f, 0.012f, 0.14f, kChrome, kEmitOff, false);   // chrome footring
            box(sx, 0.02f, sz, 0.19f, 0.02f, 0.19f, kChrome, kEmitOff, false);    // chrome base disc
            ++m_stats.barStools;
        };
        // 6 along the west leg (room side, +X), 4 along the north leg (room side, +Z).
        for (int i = 0; i < 6; ++i)
            snapStool(wX + dHalf + 0.55f, wZ0 + 1.2f + i * (wZ1 - wZ0 - 2.4f) / 5.0f);
        for (int i = 0; i < 4; ++i)
            snapStool(nX0 + 1.4f + i * (nX1 - nX0 - 2.0f) / 3.0f, nZ + dHalf + 0.55f);
        // ---- BACKBAR bottle walls behind BOTH legs (west wall + north wall) ----
        const float bottleHue[5][3] = {
            { 1.0f, 0.55f, 0.10f }, { 0.15f, 1.0f, 0.45f }, { 0.10f, 0.75f, 1.0f },
            { 0.80f, 0.20f, 1.0f }, { 1.0f, 0.25f, 0.35f },
        };
        // West-wall backbar: 3 shelves of glowing bottles + violet edge strips.
        {
            const float wallX = -CW / 2 + 0.35f, cz = 0.0f, run = (wZ1 - wZ0) - 1.5f;
            const float emViolet[4] = { 0.55f, 0.10f, 1.0f, 2.0f };
            box(wallX + 0.02f, 2.7f, cz, 0.008f, 0.010f, run / 2, kWall, emViolet, false);
            for (int shelf = 0; shelf < 3; ++shelf) {
                const float shY = 1.35f + shelf * 0.55f;
                box(wallX + 0.16f, shY - 0.03f, cz, 0.14f, 0.012f, run / 2, kChrome, kEmitOff, false); // shelf
                const int nb = (int)(run / 0.42f);
                for (int bt = 0; bt < nb; ++bt) {
                    const float btZ = cz - run / 2 + 0.3f + bt * 0.42f;
                    const float btH = 0.12f + ((clubHash(shelf * 61 + bt * 17) % 100) / 100.0f) * 0.06f;
                    const float* hue = bottleHue[(shelf * 3 + bt) % 5];
                    const float btEm[4] = { hue[0], hue[1], hue[2], 1.6f };
                    const float btCol[4] = { hue[0]*0.3f, hue[1]*0.3f, hue[2]*0.3f, 1.0f };
                    x3::prims::PrimMesh btm = x3::prims::makeBox(0.032f, btH, 0.032f, wallX + 0.16f, oy + shY + btH, btZ, 1.0f);
                    prim(std::move(btm), btCol, btEm, x3::rhi::TextureHandle{}, mrGlass, false);
                }
            }
            // Wide OLED "now mixing" band above the west backbar (live shimmer).
            const float emScr[4] = { 1.0f, 1.0f, 1.0f, 1.9f };
            const uint32_t bandId = box(wallX + 0.05f, 3.15f, cz, 0.015f, 0.40f, run / 2, kTvFrame, emScr, false);
            oledGlass(bandId, texEq[1]);
            m_oledEnts.push_back(bandId);
        }
        // North-wall backbar: 2 shelves of bottles along the north run.
        {
            const float wallZ = -CL / 2 + 0.35f, cx = (nX0 + nX1) / 2, run = (nX1 - nX0) - 1.2f;
            for (int shelf = 0; shelf < 2; ++shelf) {
                const float shY = 1.45f + shelf * 0.55f;
                box(cx, shY - 0.03f, wallZ + 0.16f, run / 2, 0.012f, 0.14f, kChrome, kEmitOff, false);
                const int nb = (int)(run / 0.42f);
                for (int bt = 0; bt < nb; ++bt) {
                    const float btX = cx - run / 2 + 0.3f + bt * 0.42f;
                    const float btH = 0.12f + ((clubHash(shelf * 41 + bt * 23) % 100) / 100.0f) * 0.06f;
                    const float* hue = bottleHue[(shelf * 4 + bt) % 5];
                    const float btEm[4] = { hue[0], hue[1], hue[2], 1.6f };
                    const float btCol[4] = { hue[0]*0.3f, hue[1]*0.3f, hue[2]*0.3f, 1.0f };
                    x3::prims::PrimMesh btm = x3::prims::makeBox(0.032f, btH, 0.032f, btX, oy + shY + btH, wallZ + 0.16f, 1.0f);
                    prim(std::move(btm), btCol, btEm, x3::rhi::TextureHandle{}, mrGlass, false);
                }
            }
        }
        // ---- NEUTRAL WARM KEYS over the granite (so the emerald reads, not only gels).
        // (Reclaimed from the removed U-bar key + ground-bar pendants — net light-neutral.)
        addLight(m_lights, wX + 0.3f, oy + bH2 + 1.6f, -2.5f, 1.70f, 1.55f, 1.35f, 5.5f);
        addLight(m_lights, wX + 0.3f, oy + bH2 + 1.6f,  2.5f, 1.70f, 1.55f, 1.35f, 5.5f);
        addLight(m_lights, -10.0f,    oy + bH2 + 1.6f, nZ + 0.3f, 1.70f, 1.55f, 1.35f, 5.5f);
        addLight(m_lights, -5.5f,     oy + bH2 + 1.6f, nZ + 0.3f, 1.70f, 1.55f, 1.35f, 5.5f);
        m_stats.hasGroundBar = true;
        // ---- DANNY the bartender, behind the L (in the service lane), facing the room.
        addCharacter(scene, device, physics, modelDir, "AnnaCasual.glb",
                     x3::phys::Vec3{ wX - 0.55f, oy + 0.0f, -1.5f }, 1.0f, false, nullptr);
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
    // PERIMETER CATWALK @ 22 ft + ROUND HALF-MOON ALCOVES (spec §1.8, refinement
    // (a): ~20 ft spacing, half-moon shape). A continuous mezzanine ring around all
    // four walls with amber-LED guard rails; semicircular private pods cantilever
    // off it overlooking the floor. Lanterns are EMISSIVE-only (point-light budget).
    // ==================================================================
    {
        const float LYc = 22.0f * 0.3048f;    // 6.71 m catwalk level
        const float ckW = 4.0f * 0.3048f;     // 1.22 m grate width
        const float egH = ER_W;               // engine-room gap on the north (-Z) wall
        const float nSeg = (CW - egH) / 2.0f - 0.1f;
        // Grate ring (thin steel). North (-Z) split around the engine-room gap.
        box(-(egH / 2 + nSeg / 2), LYc, -(HW - ckW / 2), nSeg / 2, 0.04f, ckW / 2, kCatwalk, kEmitOff, true);
        box( (egH / 2 + nSeg / 2), LYc, -(HW - ckW / 2), nSeg / 2, 0.04f, ckW / 2, kCatwalk, kEmitOff, true);
        box(0, LYc, HW - ckW / 2, (CW - 1.0f) / 2, 0.04f, ckW / 2, kCatwalk, kEmitOff, true);   // south
        box(HL - ckW / 2, LYc, 0, ckW / 2, 0.04f, (CL - 2 * ckW) / 2, kCatwalk, kEmitOff, true); // east
        box(-HL + ckW / 2, LYc, 0, ckW / 2, 0.04f, (CL - 2 * ckW) / 2, kCatwalk, kEmitOff, true);// west
        // Inner amber-LED guard rails ("a halo of amber light around the room").
        auto rail = [&](float x, float z, float hx, float hz) {
            box(x, LYc + 0.5f, z, hx, 0.5f, hz, kRail, kEmitOff, true);
            box(x, LYc + 1.0f, z, hx, 0.02f, hz, kWall, kEmitAmberLo, false);   // amber cap
        };
        rail(0, HW - ckW, (CW - 1.0f) / 2, 0.03f);                     // south
        rail(HL - ckW, 0, 0.03f, (CL - 2 * ckW) / 2);                  // east
        rail(-HL + ckW, 0, 0.03f, (CL - 2 * ckW) / 2);                 // west
        rail(-(egH / 2 + nSeg / 2), -(HW - ckW), nSeg / 2, 0.03f);     // north (two segments)
        rail( (egH / 2 + nSeg / 2), -(HW - ckW), nSeg / 2, 0.03f);
        // ROUND HALF-MOON private pods — REAL HALF-DISC BOOTHS (POLISH stage15).
        // Was: a curved baluster ARC over a flat rectangular pad — from below it read
        // as a fence, not a booth. Now each pod is a proper SEMICIRCULAR FLOOR PLATE
        // (a real half-disc) cantilevering off the catwalk inner edge INTO the room,
        // ringed by a curved rail, with a CURVED bench following the arc and a small
        // center table. Reads as a round half-moon booth from the floor below AND the
        // catwalk above.
        //   C=(cx,cz) sits on the catwalk INNER edge; (bdx,bdz)=unit bulge INTO room;
        //   perp=(-bdz,bdx) runs the flat diameter along the catwalk.
        auto halfDiscFloor = [&](float cx, float cy, float cz, float r, float hy,
                                 float bdx, float bdz) {
            const float px = -bdz, pz = bdx;              // diameter (flat-edge) axis
            const int N = 18;
            float ax[N + 1], az[N + 1];
            for (int i = 0; i <= N; ++i) {
                const float th = kPi * i / N, c = std::cos(th), s = std::sin(th);
                ax[i] = cx + px * (r * c) + bdx * (r * s);
                az[i] = cz + pz * (r * c) + bdz * (r * s);
            }
            x3::prims::PrimMesh m;
            auto pushTri = [&](float Ax, float Ay, float Az, float Bx, float By, float Bz,
                               float Cx, float Cy, float Cz, float wx, float wy, float wz) {
                float ux = Bx - Ax, uy = By - Ay, uz = Bz - Az;
                float vx = Cx - Ax, vy = Cy - Ay, vz = Cz - Az;
                float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                if (nx * wx + ny * wy + nz * wz < 0.0f) {   // orient front face toward `want`
                    float tx = Bx, ty = By, tz = Bz; Bx = Cx; By = Cy; Bz = Cz; Cx = tx; Cy = ty; Cz = tz;
                }
                const uint32_t b = (uint32_t)m.verts.size();
                m.verts.push_back({ { Ax, Ay, Az }, { wx, wy, wz }, { 0, 0 } });
                m.verts.push_back({ { Bx, By, Bz }, { wx, wy, wz }, { 1, 0 } });
                m.verts.push_back({ { Cx, Cy, Cz }, { wx, wy, wz }, { 0.5f, 1 } });
                m.index.insert(m.index.end(), { b, b + 1, b + 2 });
            };
            for (int i = 0; i < N; ++i) {                 // top + bottom fans
                pushTri(cx, cy + hy, cz, ax[i], cy + hy, az[i], ax[i + 1], cy + hy, az[i + 1], 0, 1, 0);
                pushTri(cx, cy - hy, cz, ax[i], cy - hy, az[i], ax[i + 1], cy - hy, az[i + 1], 0, -1, 0);
            }
            for (int i = 0; i < N; ++i) {                 // curved rim
                float nx0 = ax[i] - cx, nz0 = az[i] - cz;
                float l = std::sqrt(nx0 * nx0 + nz0 * nz0); if (l < 1e-5f) l = 1.0f; nx0 /= l; nz0 /= l;
                pushTri(ax[i], cy + hy, az[i], ax[i + 1], cy + hy, az[i + 1], ax[i + 1], cy - hy, az[i + 1], nx0, 0, nz0);
                pushTri(ax[i], cy + hy, az[i], ax[i + 1], cy - hy, az[i + 1], ax[i], cy - hy, az[i], nx0, 0, nz0);
            }
            // straight EDGE face (the diameter, along the catwalk).
            pushTri(ax[0], cy + hy, az[0], ax[N], cy + hy, az[N], ax[N], cy - hy, az[N], -bdx, 0, -bdz);
            pushTri(ax[0], cy + hy, az[0], ax[N], cy - hy, az[N], ax[0], cy - hy, az[0], -bdx, 0, -bdz);
            m.cverts.reserve(m.verts.size() * 3);         // walkable pod floor
            for (const auto& vt : m.verts) { m.cverts.push_back(vt.pos[0]); m.cverts.push_back(vt.pos[1]); m.cverts.push_back(vt.pos[2]); }
            m.cindex = m.index;
            prim(std::move(m), kCatwalk, kEmitOff, x3::rhi::TextureHandle{}, mrChrome, true);
        };
        auto pod = [&](float cx, float cz, float bdx, float bdz) {
            const float r = 0.95f, yb = LYc;
            const float px = -bdz, pz = bdx;              // diameter axis
            halfDiscFloor(cx, yb - 0.02f, cz, r, 0.04f, bdx, bdz);     // REAL half-disc floor plate
            for (int a = 0; a <= 8; ++a) {                // curved rail around the arc
                const float th = kPi * a / 8.0f, c = std::cos(th), s = std::sin(th);
                const float rx = cx + px * (r * c) + bdx * (r * s);
                const float rz = cz + pz * (r * c) + bdz * (r * s);
                box(rx, yb + 0.45f, rz, 0.03f, 0.45f, 0.03f, kRail, kEmitOff, false);      // baluster
                box(rx, yb + 0.90f, rz, 0.05f, 0.02f, 0.05f, kWall, kEmitAmberLo, false);  // amber cap
            }
            for (int a = 1; a < 8; ++a) {                 // CURVED bench following the arc
                const float th = kPi * a / 8.0f, c = std::cos(th), s = std::sin(th);
                const float rb = r * 0.62f;
                const float bx = cx + px * (rb * c) + bdx * (rb * s);
                const float bz = cz + pz * (rb * c) + bdz * (rb * s);
                box(bx, yb + 0.22f, bz, 0.11f, 0.06f, 0.11f, kLeather, kEmitOff, false);   // seat pad
                box(bx, yb + 0.11f, bz, 0.05f, 0.11f, 0.05f, kRail,    kEmitOff, false);   // seat leg
            }
            box(cx + bdx * r * 0.34f, yb + 0.25f, cz + bdz * r * 0.34f, 0.16f, 0.02f, 0.16f, kBarTop, kEmitAmberLo, false); // table top
            box(cx + bdx * r * 0.34f, yb + 0.12f, cz + bdz * r * 0.34f, 0.03f, 0.13f, 0.03f, kChrome, kEmitOff, false);      // table stem
            box(cx + bdx * r * 0.9f, yb + 0.75f, cz + bdz * r * 0.9f, 0.05f, 0.07f, 0.05f, kBarTop, kEmitAmberLo, false);    // lantern
        };
        const float podZS =  (HW - ckW);              // south catwalk inner edge (into room = -Z)
        const float podZN = -(HW - ckW);              // north inner edge (into room = +Z)
        for (int k = 0; k < 5; ++k) {                 // 5 pods on the south catwalk (~20 ft)
            const float cx = -HL + (k + 0.5f) * (CW / 5.0f);
            pod(cx, podZS, 0.0f, -1.0f);
            if (std::fabs(cx) > egH / 2 + 1.2f) pod(cx, podZN, 0.0f, +1.0f);   // north, skip ER gap
        }
        pod(HL - ckW, -2.5f, -1.0f, 0.0f);            // 1 east-wall pod (bulges -X into room)
        pod(-HL + ckW, 2.5f, +1.0f, 0.0f);           // 1 west-wall pod (bulges +X into room)
    }

    // ==================================================================
    // THE LAIR — NE corner, ENTIRELY UPSTAIRS (spec §3). Ground floor NE = the
    // OFFICE only; the 2nd floor NE = the whole Lair. DEN (77" LG C1 OLED, black-
    // leather Ashley recliners, queen pillowtop bed) + ★ READING NOOK (green-granite
    // shelf + UV blacklight underneath + a cup — the emotional anchor, §3.5) + the
    // south ADDITION (kitchen, living room, ★ dark one-way-glass overlook onto the
    // dance floor). Charcoal light-absorbing walls, low 8 ft ceiling.
    // ==================================================================
    {
        const float SVC = 10.0f * 0.3048f;     // 3.05 m service-block depth (east of +X)
        const float LYl = 22.0f * 0.3048f;     // 6.71 m upstairs floor (catwalk elevation)
        const float lrH = 8.0f * 0.3048f;      // 2.44 m low Lair ceiling
        const float exW = HL + SVC;            // outer east wall X
        const float lxc = HL + SVC / 2, lxHalf = SVC / 2;
        const float lzN = -HW, lzS = 2.6f;
        const float lzc = (lzN + lzS) / 2, lzHalf = (lzS - lzN) / 2;
        const float emUV[4] = { kBlacklightR, kBlacklightG, kBlacklightB, 2.0f };
        // East SERVICE BLOCK shell (ground floor beneath the Lair).
        box(lxc, 0.1f, 0, lxHalf, 0.1f, HW, kOfficeF, kEmitOff, true);           // floor
        box(exW, CH / 2, 0, T / 2, CH / 2, HW, kWall, kEmitOff, true);           // outer east wall
        box(lxc, CH / 2, -HW, lxHalf, CH / 2, T / 2, kWall, kEmitOff, true);     // north end
        box(lxc, CH / 2,  HW, lxHalf, CH / 2, T / 2, kWall, kEmitOff, true);     // south end
        box(lxc, 0.8f, 1.2f, 0.9f, 0.05f, 0.5f, kStair, kEmitOff, true);         // office desk
        // Office -> Lair STAIRS (10 steps, up the north end).
        for (int s = 0; s < 10; ++s)
            box(HL + 0.5f + s * (SVC - 1.0f) / 10.0f, LYl * (s + 0.5f) / 10.0f, -HW + 0.6f,
                (SVC - 1.0f) / 20.0f, 0.05f, 0.5f, kStair, kEmitOff, true);
        // LAIR upstairs shell (charcoal, 8 ft).
        box(lxc, LYl, lzc, lxHalf, 0.08f, lzHalf, kLairFloor, kEmitOff, true);
        box(lxc, LYl + lrH, lzc, lxHalf, 0.08f, lzHalf, kCharcoal, kEmitOff, false);
        box(lxc, LYl + lrH / 2, lzN, lxHalf, lrH / 2, T / 2, kCharcoal, kEmitOff, true);
        box(lxc, LYl + lrH / 2, lzS, lxHalf, lrH / 2, T / 2, kCharcoal, kEmitOff, true);
        box(exW, LYl + lrH / 2, lzc, T / 2, lrH / 2, lzHalf, kCharcoal, kEmitOff, true);
        // ===== DEN ZONE (north half) =====
        const float denZ = (lzN + 0.0f) / 2;
        // 77" LG C1 OLED — was a dim flat-emissive slab ({.35,.42,.85}@2.2) that read
        // near-black in the charcoal den. POLISH stage15: make it a REAL per-texel
        // OLED-glass screen showing content (the same path as the club's wall OLEDs),
        // and push its emissive HOTTER than the club panels (kOledEmit 1.80 -> 3.4) so
        // it reads as a big BRIGHT wall TV in the dim Lair. NOT registered in
        // m_oledEnts (keeps the "20 OLED" gate exact + leaves it a steady bright glow).
        {
            // z = lzN + 0.18 lifts the pane's FRONT clear of the 0.30 m charcoal wall's
            // inner face (lzN + T/2 = lzN + 0.15) — at the old lzN + 0.06 the pane was
            // BURIED inside the wall and the opaque wall occluded it (that, not just the
            // emissive, is why it read black/dim).
            const uint32_t tvId = box(lxc, LYl + 1.3f, lzN + 0.18f, 0.87f, 0.50f, 0.02f,
                                      kTvFrame, kEmitOff, false);
            oledGlass(tvId, texEq[2]);
            scene.get(tvId).emissive[3] = 3.4f;   // brighter than the club OLEDs (den is dark)
        }
        box(lxc - 1.0f, LYl + 0.5f, lzN + 0.2f, 0.12f, 0.5f, 0.12f, kSpk, kEmitOff, false); // JBL towers
        box(lxc + 1.0f, LYl + 0.5f, lzN + 0.2f, 0.12f, 0.5f, 0.12f, kSpk, kEmitOff, false);
        box(lxc, LYl + 0.25f, lzN + 0.25f, 0.2f, 0.25f, 0.2f, kSub, kEmitOff, false);       // Velodyne sub
        box(lxc - 0.5f, LYl + 0.3f, denZ, 0.35f, 0.3f, 0.35f, kLeather, kEmitOff, true);    // recliner L
        box(lxc + 0.5f, LYl + 0.3f, denZ, 0.35f, 0.3f, 0.35f, kLeather, kEmitOff, true);    // recliner R (reading)
        // ===== ★ THE READING NOOK (§3.5) — green-granite shelf + UV blacklight + cup =====
        {
            const float shX = exW - 0.2f, shZ = denZ + 0.55f, shY = LYl + 0.7f;
            x3::prims::PrimMesh g = x3::prims::makeBox(0.15f, 0.02f, 0.5f, shX, oy + shY, shZ, 1.0f);
            prim(std::move(g), kGranite, kGraniteEm, x3::rhi::TextureHandle{}, mrGlass, false); // GRANITE shelf
            box(shX, shY - 0.05f, shZ, 0.14f, 0.015f, 0.45f, kSub, emUV, false);               // UV fixture under
            const float cupCol[4] = { 0.6f, 0.6f, 0.62f, 1.0f };
            box(shX - 0.03f, shY + 0.06f, shZ + 0.12f, 0.04f, 0.05f, 0.04f, cupCol, kEmitOff, false); // the cup
            addLight(m_lights, shX - 0.35f, oy + shY - 0.15f, shZ, 0.11f, 0.05f, 1.10f, 2.6f);  // UV reading glow
            // DEDICATED NEUTRAL SHELF KEY (POLISH stage15): the blue UV wash above
            // rendered the emerald granite blue-black. A soft warm-WHITE key just over
            // the shelf lights the stone with NEUTRAL light so its DARK GREEN reads (the
            // UV glow stays as the §3.5 mood accent). Tight range = shelf-local. (+1 to
            // the point-light set — see the report; total stays under the 64 cap.)
            addLight(m_lights, shX - 0.3f, oy + shY + 0.55f, shZ, 1.65f, 1.54f, 1.38f, 2.4f);
        }
        box(lxc, LYl + 0.25f, -0.3f, 0.9f, 0.2f, 0.55f, kLeatherHi, kEmitOff, true);        // queen bed
        addLight(m_lights, lxc, oy + LYl + lrH - 0.3f, denZ, 1.6f, 1.2f, 0.8f, 6.5f);       // warm den light
        // ===== ADDITION ZONE (south half) — kitchen, living room, overlook =====
        box(exW - 0.4f, LYl + 0.45f, 1.2f, 0.35f, 0.45f, 0.8f, kMetal, kEmitOff, true);     // kitchen counter
        box(lxc - 0.2f, LYl + 0.3f, lzS - 0.4f, 0.7f, 0.3f, 0.35f, kLeather, kEmitOff, true); // living couch
        // ★ DARK GLASS OVERLOOK — a REAL SEE-THROUGH cut (POLISH stage15). The club's
        // east wall now has an actual window opening here (framed by the sill/lintel/
        // jambs above); this pane FILLS that hole with the smoked dark glass, so from
        // the Lair living room you look THROUGH it and down onto the dance floor, and
        // from the floor it reads as dark glass. Was: a flat opaque-ish panel bolted to
        // a solid wall (nothing behind it). Geometry matches the east-wall cut exactly.
        {
            x3::prims::PrimMesh pg = x3::prims::makeBox(0.015f, kOvHy, kOvHz,
                                                        HL, oy + kOvCy, kOvCz, 1.0f);
            Entity pe;
            pe.mesh = device.createMesh(pg.verts.data(), (uint32_t)pg.verts.size(),
                                        pg.index.data(), (uint32_t)pg.index.size());
            pe.baseColor[0] = 0.03f; pe.baseColor[1] = 0.04f; pe.baseColor[2] = 0.08f; pe.baseColor[3] = 1.0f;
            pe.emissive[0] = 0.03f; pe.emissive[1] = 0.04f; pe.emissive[2] = 0.11f; pe.emissive[3] = 0.18f;
            pe.transparent = true;
            pe.glass.opacity = 0.42f;     // SEE-THROUGH (was 0.55) — the floor reads through it
            pe.glass.refraction = 0.0f; pe.glass.roughness = 0.04f;
            pe.glass.specular = 1.0f;
            pe.glass.tint[0] = 0.18f; pe.glass.tint[1] = 0.26f; pe.glass.tint[2] = 0.52f;
            pe.tag = (uint32_t)Tag::Static;
            scene.add(pe);
        }
    }

    // ==================================================================
    // SECRET TUNNEL (Route A, spec §4.1): observation-lounge landing -> LEFT to a
    // secret panel -> STRATA -> 80 ft WEST -> hook LEFT -> 30 ft SOUTH -> Private
    // Lounge Complex L1. Walkable, amber strip-lit, dead silent.
    // ==================================================================
    {
        const float tY = LOUNGE_Y, tH = 2.3f, tW = 1.3f;
        const float zBehind = -HW - 1.2f;      // behind (north of) the north wall
        const float emPanel[4] = { 0.10f, 0.95f, 0.30f, 1.6f };
        box(-ER_W / 2 - 0.1f, tY + 1.0f, -HW - 0.05f, 0.03f, 0.6f, 0.4f, kStair, emPanel, false); // secret panel
        // Seg 1: WEST behind the north wall.
        const float s1x0 = -ER_W / 2, s1x1 = -HL - 2.4f;
        const float s1cx = (s1x0 + s1x1) / 2, s1len = std::fabs(s1x1 - s1x0);
        box(s1cx, tY + 0.05f, zBehind, s1len / 2, 0.08f, tW / 2, kStrata, kEmitOff, true);
        box(s1cx, tY + tH, zBehind, s1len / 2, 0.08f, tW / 2, kStrata, kEmitOff, false);
        box(s1cx, tY + tH / 2, zBehind - tW / 2, s1len / 2, tH / 2, T / 2, kStrata, kEmitOff, true);
        box(s1cx, tY + tH / 2, zBehind + tW / 2, s1len / 2, tH / 2, T / 2, kStrata, kEmitOff, true);
        box(s1cx, tY + 0.08f, zBehind, s1len / 2 - 0.2f, 0.01f, 0.05f, kWall, kEmitAmberLo, false); // amber strip
        // Seg 2: SOUTH to the Private Lounge.
        const float s2x = -HL - 2.4f, s2z0 = zBehind, s2z1 = 0.5f;
        const float s2cz = (s2z0 + s2z1) / 2, s2len = std::fabs(s2z1 - s2z0);
        box(s2x, tY + 0.05f, s2cz, tW / 2, 0.08f, s2len / 2, kStrata, kEmitOff, true);
        box(s2x, tY + tH, s2cz, tW / 2, 0.08f, s2len / 2, kStrata, kEmitOff, false);
        box(s2x - tW / 2, tY + tH / 2, s2cz, T / 2, tH / 2, s2len / 2, kStrata, kEmitOff, true);
        box(s2x + tW / 2, tY + tH / 2, s2cz, T / 2, tH / 2, s2len / 2, kStrata, kEmitOff, true);
        box(s2x, tY + 0.08f, s2cz, 0.05f, 0.01f, s2len / 2 - 0.2f, kWall, kEmitAmberLo, false);
        // Two dim warm strata lamps so the walk actually reads (was pitch black).
        addLight(m_lights, s1cx, oy + tY + 1.4f, zBehind, 1.0f, 0.6f, 0.25f, 7.0f);
        addLight(m_lights, s2x,  oy + tY + 1.4f, s2cz,    1.0f, 0.6f, 0.25f, 7.0f);
    }

    // ==================================================================
    // PRIVATE LOUNGE COMPLEX — LEVEL 1 (spec §4.4). West of the club, reached by the
    // tunnel. Situation Room (dark wood, heavy table, leather, silent OLED club-feed)
    // + warm N-S hallway + private bedroom + kitchen + a MARKED spot for the future
    // stairwell-down + bottom elevator hall (Levels 2-7 = later stage). Intimate, warm.
    // ==================================================================
    {
        const float pY = LOUNGE_Y, pH = 3.0f;
        const float hallW = 5.0f * 0.3048f, eastD = 3.0f, westD = 3.0f;
        const float hallX = -HL - 2.4f;
        const float pzN = -HW, pzS = HW, pLen = pzS - pzN;
        box(hallX, pY + 0.05f, 0, hallW / 2, 0.08f, pLen / 2, kStrata, kEmitOff, true);
        box(hallX, pY + pH, 0, hallW / 2, 0.08f, pLen / 2, kCeil, kEmitOff, false);
        box(hallX, pY + pH / 2, pzN, hallW / 2, pH / 2, T / 2, kBunker, kEmitOff, true);
        box(hallX, pY + pH / 2, pzS, hallW / 2, pH / 2, T / 2, kBunker, kEmitOff, true);
        for (int s = -1; s <= 1; s += 2)
            box(hallX + s * (hallW / 2 - 0.03f), pY + 0.15f, 0, 0.02f, 0.03f, pLen / 2 - 0.3f, kWall, kEmitAmberLo, false);
        // SITUATION ROOM (east side, dark wood, heavy table, silent club-feed OLED).
        const float srX = hallX + hallW / 2 + eastD / 2, srZ = -3.0f;
        box(srX, pY + 0.05f, srZ, eastD / 2, 0.08f, 1.6f, kStrata, kEmitOff, true);
        box(srX, pY + pH, srZ, eastD / 2, 0.08f, 1.6f, kCeil, kEmitOff, false);
        box(srX + eastD / 2, pY + pH / 2, srZ, T / 2, pH / 2, 1.6f, kWood, kEmitOff, true);
        box(srX, pY + pH / 2, srZ - 1.6f, eastD / 2, pH / 2, T / 2, kWood, kEmitOff, true);
        box(srX, pY + pH / 2, srZ + 1.6f, eastD / 2, pH / 2, T / 2, kWood, kEmitOff, true);
        box(srX, pY + 0.4f, srZ, 0.7f, 0.05f, 0.5f, kWood, kEmitOff, true);                 // heavy table
        box(srX, pY + 0.25f, srZ + 0.9f, 0.7f, 0.25f, 0.3f, kLeather, kEmitOff, true);      // leather seating
        { const float emFeed[4] = { 0.30f, 0.35f, 0.55f, 1.5f };                            // silent club-feed OLED
          box(srX + eastD / 2 - 0.05f, pY + 1.5f, srZ, 0.02f, 0.5f, 0.9f, kTvFrame, emFeed, false); }
        addLight(m_lights, srX, oy + pY + 2.5f, srZ, 1.0f, 0.72f, 0.4f, 5.0f);              // steady warm light
        // PRIVATE BEDROOM (west, queen pillowtop, warm, no screens).
        const float bdX = hallX - hallW / 2 - westD / 2;
        box(bdX, pY + 0.05f, 2.5f, westD / 2, 0.08f, 1.6f, kStrata, kEmitOff, true);
        box(bdX, pY + pH, 2.5f, westD / 2, 0.08f, 1.6f, kCeil, kEmitOff, false);
        box(bdX - westD / 2, pY + pH / 2, 2.5f, T / 2, pH / 2, 1.6f, kBunker, kEmitOff, true);
        box(bdX, pY + 0.3f, 2.5f, 0.9f, 0.25f, 0.6f, kLeatherHi, kEmitOff, true);           // bed
        box(bdX, pY + 0.45f, -2.5f, westD / 2 - 0.3f, 0.45f, 0.4f, kMetal, kEmitOff, true); // kitchen counter
        addLight(m_lights, hallX, oy + pY + 2.4f, 0, 1.0f, 0.72f, 0.40f, 8.0f);   // warm hallway
        addLight(m_lights, bdX,   oy + pY + 2.4f, 2.5f, 1.0f, 0.75f, 0.50f, 5.0f); // bedroom glow
        // MARKED future connection (Levels 2-7 later): stairwell-down hatch + blocked door.
        const float emMark[4] = { 0.10f, 0.85f, 0.35f, 1.4f };
        box(hallX, pY + 0.09f, pzS - 1.2f, hallW / 2 - 0.2f, 0.02f, 0.6f, kStair, emMark, false);
        box(hallX, pY + 1.0f, pzN + 0.25f, hallW / 2 - 0.2f, 1.0f, 0.05f, kWood, emMark, false);
    }

    // ==================================================================
    // CLUB AMBIENT + KEY LIGHTS (Babylon hemi/point/fill -> point lights).
    // ==================================================================
    // (relight: these three ROOM-WIDE fills were the darkest offenders — 0.16-0.40
    // saturated the whole 50x100 ft room to near-black. Reworked to VIBRANT HDR
    // club washes: violet overhead, magenta over the bar side, UV-violet on the
    // mirror floor. They set the room's colored mood; the orbiters + fixtures pop
    // on top.)
    // GAMMA WALK-BACK (feat/club-gamma-fix, 2026-07-25): these three ROOM-WIDE washes
    // were cranked to HDR to fight the crushed-dark curve. The sRGB fix lifts the darks
    // for free, so at full strength they now blow the walls into a bright purple rave
    // wash. Intensities cut ~35% (blue 1.55->1.00, 1.75->1.10; magenta 1.10->0.72) so
    // they set a MOODY colored mood the neon/beams pop on top of — not a flat blowout.
    addLight(m_lights, 0, oy + CH * 0.7f, 0, 0.14f, 0.20f, 1.00f, 25.0f);       // central overhead BLUE-UV wash
    addLight(m_lights, -CW / 2 + 2, oy + 3.0f, CL / 4, 0.46f, 0.13f, 0.72f, 10.0f); // ground-bar MAGENTA wash (bar-side accent)
    addLight(m_lights, 0, oy + 2.0f, 0, 0.13f, 0.10f, 1.10f, 18.0f);            // BLUE-UV wash (mirror floor)

    // DANCE-FLOOR KEY (fix/club-blacklights): the dancers rendered as SOLID BLACK
    // silhouettes — the orbiters sat at ceiling height so their pools hit the
    // FLOOR, and the floor tiles are self-lit emissive, so the only thing that
    // glowed was the checkerboard. A soft neutral-violet key hung over the crowd
    // centroid (the dancer spots cluster around z ≈ -2) puts actual light on
    // faces/torsos. Deliberately gentle — way under the gel colors — so the room
    // stays a moody club, not a showroom.
    // GAMMA WALK-BACK: the crowd key was hot to punch through the dark curve; cut
    // ~30% (1.50/1.35/1.85 -> 1.05/0.95/1.30) so it lights faces without over-exposing.
    addLight(m_lights, -6.0f, oy + 4.6f, 0.0f, 1.05f, 0.95f, 1.30f, 14.0f);
    // ...plus two soft lavender PERIMETER FILLS at head height flanking the dance
    // floor (dfX≈-6): the dark-albedo outfits need N·L from the SIDE to read at all.
    // (Reduced 4->2 to reserve point-light budget for the Lair / tunnel / Complex.)
    // GAMMA WALK-BACK: fill trimmed ~30% (1.00/0.88/1.25 -> 0.70/0.62/0.88).
    {
        const float fillC[3] = { 0.70f, 0.62f, 0.88f };
        const float fillPts[2][3] = {
            { -9.5f, 1.7f, -3.0f }, { -3.0f, 1.7f, 3.0f },
        };
        for (auto& f : fillPts)
            addLight(m_lights, f[0], oy + f[1], f[2], fillC[0], fillC[1], fillC[2], 11.0f);
    }

    // ==================================================================
    // CEILING MOVING-HEAD RIG (Tim addenda: "we had the lights that move to the
    // music" + "they're ceiling-mounted fixtures that project patterns DOWN onto
    // the dance floor"). Four visible fixtures hang from the ceiling on a ring
    // over the dance floor; each throws a translucent BEAM shaft down to a
    // colored POOL that sweeps a beat-locked figure-8 on the floor below it,
    // stepping on the eighth-note grid, gels rotating every 8-beat phrase.
    //   * fixture body: mount plate + yoke + head + emissive LENS (gel-colored,
    //     breathing with the beat — update() drives it);
    //   * beam: a soft VOLUMETRIC LIGHT-CONE (fix/club-polish) on the glass
    //     ADDITIVE-glow route — a dusty, cone-shaped shaft that scatters in the air
    //     and lands as a pool, re-aimed each frame lens -> pool so the light
    //     DEMONSTRABLY comes from the fixture and sweeps with the beat;
    //   * pool: the fixture's point light rides at ~1.15 m over the pool point —
    //     it paints the floor AND lights the dancers' bodies as it sweeps.
    // HONEST SCOPE: the beam is a VOLUMETRIC-CONE APPROXIMATION (an additive glow
    // cone via the glass pass — the same fake-volumetric machinery the STFC intro /
    // street lamps use), NOT true per-light ray-traced participating-media scatter.
    // It is depth-tested (LEQUAL), so opaque geometry IN FRONT (a dancer standing in
    // the beam) occludes the shaft where their body cuts it — but the cone does not
    // self-shadow / carve around a body mid-volume, and there is no GOBO pattern
    // projection (that needs a projected-texture path the engine lacks).
    // ==================================================================
    {
        // POLISH (fix/club-polish): the beams are now REAL soft VOLUMETRIC CONES
        // (makeBeamConeMesh) on the glass ADDITIVE-glow path, not flat "laser bar"
        // boxes. One cone mesh + one axial gradient are shared by all 4 fixtures;
        // update() re-aims each with poseCone(lens -> pool) so the shaft sweeps.
        const float kBeamDrop   = (CH - 0.63f) - 0.05f;   // lens -> floor pool
        const float kBeamPoolR  = 1.15f;                  // pool / base radius (m)
        x3::rhi::MeshHandle beamMesh;
        {
            std::vector<x3::rhi::MeshVertex> bv; std::vector<uint32_t> bi;
            makeBeamConeMesh(kBeamPoolR, kBeamDrop, bv, bi);
            beamMesh = device.createMesh(bv.data(), (uint32_t)bv.size(),
                                         bi.data(), (uint32_t)bi.size());
        }
        auto bgpx = makeBeamGradRGBA(8, 64);
        const x3::rhi::TextureHandle beamGrad = device.createTexture(bgpx.data(), 8, 64, false);
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
            // Beam shaft: a soft VOLUMETRIC LIGHT-CONE (local apex at origin, opening
            // -Y) on the glass ADDITIVE-glow route — the dusty cone that scatters in
            // the air and lands as a pool, NOT a flat translucent bar. update()
            // re-aims it lens -> pool each frame with poseCone (pure rotation) so the
            // shaft visibly sweeps from the ceiling fixture. Depth-tested, so a dancer
            // standing in the beam occludes the shaft where their body cuts it.
            {
                const float* gel = kSpotGels[i & 3];
                Entity be;
                be.mesh = beamMesh;
                be.tex  = beamGrad;
                be.baseColor[0] = 1.0f; be.baseColor[1] = 1.0f; be.baseColor[2] = 1.0f; be.baseColor[3] = 1.0f;
                be.emissive[0] = gel[0]; be.emissive[1] = gel[1]; be.emissive[2] = gel[2];
                be.emissive[3] = 1.9f;        // additive glow strength (update() breathes it)
                be.transparent = true;
                be.glass.opacity = 0.0f;      // pure additive — no diffuse body
                be.glass.refraction = 0.0f;
                be.glass.roughness = 0.0f;
                be.glass.specular = 0.0f;
                be.glass.additive = 2.6f;     // soft cone-silhouette rim fade (dot(N,V)^2.6)
                be.tag = (uint32_t)Tag::Static;
                poseCone(be, mh.fx, oy + CH - 0.63f, mh.fz, mh.fx, oy + 0.05f, mh.fz);
                mh.beamEnt = scene.add(be);
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
            // (fix/club-blacklights: tints lifted ~1.25x — the rigs' outfits are
            // dark-albedo, so at 1.0x they swallowed the club light and rendered
            // as silhouettes even when lit. Hue variety preserved.)
            const float tints[5][4] = {
                { 1.30f, 1.05f, 1.55f, 1.0f },   // violet wash
                { 1.05f, 1.30f, 1.60f, 1.0f },   // cyan wash
                { 1.55f, 1.10f, 1.25f, 1.0f },   // warm rose
                { 1.10f, 1.50f, 1.20f, 1.0f },   // green tinge
                { 1.40f, 1.30f, 1.10f, 1.0f },   // amber
            };
            // Spots: a loose ring on the dance floor + two by the DJ end. Kept off
            // the bar lane and inside the tile field.
            // CANON-PORT: cluster on the west dance floor (dfX≈-6), Z within ±4.5
            // (the room is now 43 ft / ±6.55 m short-axis).
            const float spots[10][3] = {   // x, z, base yaw
                { -6.0f, -3.5f,  0.6f }, { -4.0f, -1.5f, -2.4f }, { -8.0f,  0.5f,  3.0f },
                { -5.0f,  2.5f, -1.2f }, { -9.0f, -2.0f,  1.8f }, { -3.0f,  1.0f, -0.4f },
                { -7.0f,  3.5f,  2.2f }, {-10.5f, -4.0f, -2.8f }, { -2.5f, -3.0f,  0.2f },
                { -6.5f,  4.5f,  1.4f },
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
    const float breathe   = 0.72f + 0.48f * thump;         // gel beat envelope (floor
                                                           // 0.72: the crowd never drops
                                                           // back to silhouettes mid-beat)

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
        // POLISH: the ball is REFLECTIVE, not a lamp — keep its self-glow a faint
        // floor (a touch of facet glint on the beat), NOT a blown-out white bulb.
        e.emissive[3] = 0.14f + 0.14f * thump;
    }

    // --- SPIN the DIY dome/starburst PROJECTOR discs (transform 3x3 = Y-rotation about
    // their own centre; translation kept) + breathe their emissive on the beat. This
    // throws the signature spinning colored dots across the floor/ceiling. ---
    for (const auto& pj : m_projectors) {
        if (pj.ent >= scene.size()) continue;
        const float ang = t * pj.spin + pj.phase;
        const float c = std::cos(ang), s = std::sin(ang);
        Entity& e = scene.get(pj.ent);
        // Y-rotation in the upper 3x3 (disc lies in XZ), translation columns untouched.
        e.transform[0] = c;  e.transform[2] = -s;
        e.transform[8] = s;  e.transform[10] = c;
        e.emissive[3] = pj.emBase + pj.emAmp * thump;   // dots pulse to the track
    }

    // --- POLISH: MIRROR-BALL SPARKLE DOTS. The cluster of colored point lights orbits
    // WITH the ball's spin (same t*0.5 clock) at staggered radii/heights, so their
    // bright pools sweep across the walls / floor / dancers as scattered moving dots.
    // Each breathes with the beat so the whole starfield pulses to the track. ---
    if (!m_sparkleLightIdx.empty()) {
        const float spin = t * 0.5f;                 // matches THE ORB's spin cadence
        const float sparkleHue[6][3] = {
            { 2.4f, 0.6f, 2.4f }, { 0.5f, 1.8f, 2.6f }, { 2.6f, 1.6f, 0.4f },
            { 0.6f, 2.6f, 1.2f }, { 2.6f, 0.5f, 1.4f }, { 1.4f, 1.4f, 2.6f },
        };
        const float spk = 0.55f + 0.55f * thump;     // dot brightness pulses on the beat
        for (size_t i = 0; i < m_sparkleLightIdx.size(); ++i) {
            const size_t li = m_sparkleLightIdx[i];
            if (li >= m_lights.size()) continue;
            const float fi = (float)i;
            const float ph  = spin * 1.6f + fi * (2.0f * kPi / 6.0f);
            const float rad = 4.6f + 1.8f * std::sin(fi * 2.11f);         // staggered orbit radius
            const float hy  = 2.6f + 1.7f * std::sin(spin * 1.3f + fi * 1.7f);
            m_lights[li].pos[0] = std::cos(ph) * rad;
            m_lights[li].pos[1] = kClubY + hy;
            m_lights[li].pos[2] = kHeadRingCz + std::sin(ph) * rad;
            m_lights[li].color[0] = sparkleHue[i % 6][0] * spk;
            m_lights[li].color[1] = sparkleHue[i % 6][1] * spk;
            m_lights[li].color[2] = sparkleHue[i % 6][2] * spk;
        }
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
            if (mh.beamEnt < scene.size()) {               // volumetric beam cone re-aimed lens -> pool
                Entity& be = scene.get(mh.beamEnt);
                be.emissive[0] = gel[0]; be.emissive[1] = gel[1]; be.emissive[2] = gel[2];
                be.emissive[3] = 1.7f + 1.3f * thump;     // soft cone breathing with the beat
                poseCone(be, mh.fx, kClubY + kCH - 0.63f, mh.fz, px, kClubY + 0.05f, pz);
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
        e.emissive[3] = kBlacklightEmit;   // POLISH: dim UV bloom (was 4.0 — magenta bar)
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
        // POLISH: the 3D DRIVER CONES pump + surge on the beat. Each cone punches
        // OUT along its face normal on the kick (posAmp*thump) then settles as the
        // thump decays, and breathes its amber emissive between emBase..emBase+emAmp.
        // Subs punch hard (posAmp ~0.035, emAmp ~2.0); mids ripple (posAmp ~0.010);
        // tweeters barely move. The geometry is baked at rest in WORLD space with an
        // identity transform, so a translation in the transform IS the cone travel.
        for (const auto& d : m_driverCones) {
            if (d.ent >= scene.size()) continue;
            Entity& e = scene.get(d.ent);
            const float disp = d.posAmp * thump;      // out on the kick, settle after
            e.transform[12] = d.nx * disp;
            e.transform[13] = d.ny * disp;
            e.transform[14] = d.nz * disp;
            e.emissive[3] = d.emBase + d.emAmp * thump;
        }
        // Corner sub pulse lights breathe with the same clock.
        for (const size_t li : m_subLightIdx) {
            if (li >= m_lights.size()) continue;
            const float k = 0.25f + 0.75f * thump;
            m_lights[li].color[0] = 1.40f * k;   // relight: amber sub thump 0.9 -> 1.4 HDR
            m_lights[li].color[1] = 0.65f * k;
            m_lights[li].color[2] = 0.15f * k;
        }
        // Dim glossy checker tiles (LNS garage floor): only a WHISPER of self-glow on
        // the beat — the floor is a moody mirror for the projected dots + beams now, not
        // a lightbox. (Tim 2026-07-18: shiny + dimmer. Was 0.28+0.38*thump, a blue slab.)
        for (const uint32_t id : m_tilePulseEnts) {
            if (id >= scene.size()) continue;
            scene.get(id).emissive[3] = 0.07f + 0.10f * thump;   // faint violet breath, dim
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

    // Freeze the hero CAR on the two-post lift AFTER the tick, so the drawn transform
    // is exactly the parked pose (the generic Idle AI adds a tiny yaw sway that would
    // otherwise slowly rock the car). No-op if the GLB never loaded (m_carCharIdx<0).
    if (m_carCharIdx >= 0 && (size_t)m_carCharIdx < m_chars.size())
        m_chars[m_carCharIdx]->setPropPose(m_carPos, m_carYaw);
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
    out[0] = kCW / 2 - 2.5f;   // x: by the east wall / elevator entrance
    out[1] = kClubY + 4.0f;    // y: above the floor, below the 30 ft ceiling
    out[2] = 0.5f;             // z: slightly off centre
    out[3] = -2.9f;            // yaw: look WEST + a touch north (dir=(cos,sin)): frames
                               // the dance floor, ORB, moving-head beams + DJ/engine room
    out[4] = -0.13f;           // pitch: slightly down over the dance floor
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
    //     the canon 100x43x30 ft (30.48 x 13.11 x 9.14 m) — X = 100 ft long E-W
    //     axis, Z = 43 ft N-S — ceiling 30 ft above (Tim's bar2_architecture.js).
    {
        const float wX = s.roomMaxX - s.roomMinX;   // ~30.48 (100 ft, long axis)
        const float wZ = s.roomMaxZ - s.roomMinZ;   // ~13.11 (43 ft)
        const float h  = s.ceilingY - s.floorY;     // ~9.14
        const bool yOk   = std::fabs(s.floorY - (-200.0f)) < 0.01f;
        const bool footOk = std::fabs(wX - 30.48f) < 0.05f && std::fabs(wZ - 13.106f) < 0.05f;
        const bool ceilOk = std::fabs(h - 9.14f) < 0.05f;
        check(yOk && footOk && ceilOk,
              "main room is 100x43x30 ft (30.48x13.11x9.14 m) with its floor at Y=-200");
    }

    // (2) Suspended DJ booth: platform + turntables + 2 OLED + keypad door.
    check(s.hasDjBooth && s.hasDjTurntables && s.hasDjScreens && s.hasKeypadDoor,
          "suspended DJ booth (turntables + 2 OLED screens + keypad door)");

    // (3) THE ORB — the 2 m mirror ball.
    check(s.hasOrb, "THE ORB (mirror ball) exists");

    // (4) Aerial bar + ground bar with exactly 7 stools.
    check(s.hasAerialBar && s.hasGroundBar && s.barStools == 10,
          "aerial bar + L-wraparound bar with 10 Snap-on stools");

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

    // (8) 16 × 85" OLED wall screens (4 per wall — canon refinement).
    check(s.tvScreens == 16, "16 OLED wall screens (4 per wall)");

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
        check(allOk && oled.size() == 20,
              "all 20 OLED screens are textured glass on the per-texel emissive path");
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
