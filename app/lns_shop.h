#pragma once
// ============================================================================
// LATE NIGHT SPEED — the SHOP MATERIAL KIT, shared.
//
// PROVENANCE. Every texture recipe in this file was AUTHORED in app/club1127.cpp
// (the "LNS GARAGE" pass, Tim 2026-07-18) from photographs of Tim's REAL Miami
// auto shop — docs/design/RACING_WORLD.md: "Club 1127 IS Tim's real Miami auto
// shop." The recipes were MOVED here (inspx/lnss-garage), not copied: club1127
// now calls these same functions, so the club and the tunnel vehicle bay dress
// from ONE authored source and a re-tune of the shop's paint lands in both.
//
// WHAT LIVES HERE (the SHOP's identity — what the place looks like with the
// work lights on):
//   * makeCmuBlockRGBA / makeCmuNormalRGBA — painted concrete-block walls,
//     running bond, recessed mortar, paint grit.
//   * kCheckerBright / kCheckerDark + makeCheckerFloorRGBA — the glossy
//     checkerboard on sheened shop concrete, with the wet-mirror MR texel
//     values (kFloorRoughPx / kFloorMetalPx) that make light POOL in it.
//   * makeSignRGBA — the 5x7 neon-letter baker (the shop branding).
//   * makeMr1x1 — the 1x1 metallic-roughness texel helper both callers need.
//
// WHAT DELIBERATELY DOES NOT LIVE HERE (the CLUB's identity — what the same
// room looks like after dark): the DIY dome party-projectors, the ceiling
// starburst, the laser floor patterns, the mirror ball. Those are Club 1127's
// NIGHT signature and stay in club1127.cpp; a working bay does not inherit
// the party just because it inherits the paint.
//
// DETERMINISM: hash-based only (lnsHash), no rand(), no clocks — the shop is
// byte-identical every boot, in both rooms.
// ============================================================================
#include <cstdint>
#include <vector>

namespace x3::game::lns {

// Integer hash (the club's clubHash, moved). Both rooms' walls MUST hash alike
// or the "same paint" claim is a lie at the texel level.
uint32_t lnsHash(uint32_t x);

// ---- CANONICAL LNS VALUES — the single source both callers read. ----------
// The checkerboard albedo split, straight from the club's tile loop (bright vs
// dark squares; the split carries the checker now that the self-glow is a
// whisper). LINEAR values (they were authored as baseColor floats, which the
// shader reads linearly) — so makeCheckerFloorRGBA bakes them into a texture
// created with srgb = FALSE, or the values shift.
constexpr float kCheckerBright[4] = { 0.090f, 0.092f, 0.105f, 1.0f };
constexpr float kCheckerDark[4]   = { 0.020f, 0.020f, 0.028f, 1.0f };
// The wet-look shop-floor MR texel ("SHINY + DIMMER", rough 8 / metal 255 —
// wetter than chrome, so beams pool and shimmer in the surface).
constexpr uint8_t kFloorRoughPx = 8,   kFloorMetalPx = 255;
// Matte painted concrete block.
constexpr uint8_t kCmuRoughPx   = 205, kCmuMetalPx   = 0;
// The shop's neon red (hot red letters on a near-black panel).
constexpr float kNeonRed[3] = { 1.0f, 0.10f, 0.10f };

// PAINTED CMU BLOCK wall (LNS GARAGE, Tim 2026-07-18): the real Late Night
// Speed walls are painted concrete-block — gritty industrial white/grey.
// Running-bond courses (16x8 in / 0.4x0.2 m units => the 256px texture spans
// 1.6 m x 1.6 m at true scale), recessed darker mortar joints, faint per-block
// value variation + fine paint grit. Albedo only — tint at the call site.
std::vector<uint8_t> makeCmuBlockRGBA(uint32_t n);

// Companion normal map: mortar joints push IN (a soft groove), block faces
// stay flat — so raking light catches the coursing.
std::vector<uint8_t> makeCmuNormalRGBA(uint32_t n);

// LNS GARAGE FLOOR as a bakeable texture: `tiles` x `tiles` checker squares of
// kCheckerBright/kCheckerDark with a thin near-black seam between tiles (the
// 2 cm gap the club leaves between its tile boxes). Create with srgb=false and
// pair with makeMr1x1(kFloorRoughPx, kFloorMetalPx) — at 1 m per tile this IS
// the club's floor, minus the beat-pulsed under-glow (a workshop does not
// pulse; the club adds that on top of the same albedo).
std::vector<uint8_t> makeCheckerFloorRGBA(uint32_t n, uint32_t tiles);

// LATE NIGHT SPEED neon SIGN (LNS GARAGE, Tim 2026-07-18): a compact 5x7
// uppercase bitmap font bakes `line` onto a dark panel as a hot neon glow
// (letters + a soft 1-cell halo). Only the glyphs the signs need are defined.
std::vector<uint8_t> makeSignRGBA(uint32_t w, uint32_t h, const char* line,
                                  float rr, float rg, float rb);

// 1x1 metallic-roughness texel (glTF packing: G=roughness, B=metallic).
std::vector<uint8_t> makeMr1x1(uint8_t rough, uint8_t metal);

} // namespace x3::game::lns
