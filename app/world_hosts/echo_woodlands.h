#pragma once
// WP-3 (TIER2_STREAMING_PLAN.md §4) — woodlands cell split + district light
// slices. Owned exclusively by WP-3; do not add unrelated content here.
//
// Two independent deliverables live in this pair of files:
//
//  1. buildWoodlandsCell(cellIx, cellIz, region, ctx) — the SAME deterministic
//     island-wide woodlands scatter host_echotropolis.cpp's WOODLANDS block
//     (~lines 1152-1248) runs today as ONE EnvArtSystem, rebuilt as 9
//     cell-local EnvArtSystems (plan §1's `woodlands_NW/N/NE/W/C/E/SW/S/SE`
//     regions), one per call. Same hh() hash, same iteration domain, same
//     keep-outs, same density gate — a placement is only additionally
//     filtered by "does it fall inside THIS cell's rect", applied strictly
//     AFTER every other gate. See echoWoodlandsSliceSelfTest() below for the
//     bit-identity proof and echo_woodlands.cpp's `scatterWoodlands()` for
//     why the filter placement guarantees it.
//
//  2. harvestDistrictLights(...) — extracted from the host's loadDistrict
//     lambda (host_echotropolis.cpp ~1716-1816): the neon/lamp PointLight
//     harvest, WITHOUT the mesh-instancing side (that stays WP-2's
//     buildDistrict's job — this function needs no IRenderDevice). Returns
//     one district's own light slice, to be handed to EchoRegion::addLights
//     (plan §3) by that district's builder.
//
// ASSUMPTION (flagged for the integrator, see final WP-3 report): both
// functions below take `Heightfield&` / `EchoRegionCtx&` per plan §3's
// `EchoRegionCtx { ... Heightfield& hf; ... }`. Today Heightfield is defined
// in an ANONYMOUS namespace inside host_echotropolis.cpp (internal linkage —
// not visible outside that translation unit). This header assumes WP-1's
// echo_regions.h relocates that struct (verbatim, it's a pure host-side PNG
// heightmap sampler with zero RHI/device dependency) somewhere both
// echo_regions.h and this file can see it. Until that happens this header
// will not compile standalone against echo_regions.h — WP-3 has no file
// ownership to fix that itself.

#include "echo_regions.h"

#include "engine/rhi/IRenderDevice.h"   // x3::rhi::PointLight

#include <string>
#include <vector>

namespace x3::game {

// Build ONE of the 9 woodlands cells (cellIx, cellIz each in [0,2]; see
// echo_woodlands.cpp's woodlandsCellRect() for the exact grid — 3x3 over the
// plan §1 island domain x in [-800,1600), z in [100,1500), with the OUTER
// edge cells opened to +/-infinity so every legacy placement lands in
// exactly one cell regardless of how far actual land extends past the
// nominal rect — see the self-test below, which is what actually proves
// this). Loads assets/veg (ctx.vegDir) once per cell (EnvArtSystem's own
// path-cache still dedups the 6 shared pine GLBs across cells — each cell
// mounts its own IAssetSource, so GPU uploads are NOT shared cell-to-cell;
// that's an EnvArtSystem/WP-4 concern, not a determinism concern). No-op
// (logs and returns without touching `region`) if cellIx/cellIz are out of
// range or the cell contains zero surviving placements.
void buildWoodlandsCell(int cellIx, int cellIz, EchoRegion& region, EchoRegionCtx& ctx);

// Port of loadDistrict's light-harvesting half (host_echotropolis.cpp
// ~1798-1815): parses `layoutPath` (the SAME .layout format loadDistrict
// reads), replays the SAME pad transform (padX/padZ/padYaw/padScale +
// meshFix's mesh-local rotation is irrelevant to a light's POSITION so it is
// correctly NOT ported here — meshFix only ever affected instance
// orientation) to get each piece's world position, and classifies each piece
// by name into a warm-sodium streetlamp light, a magenta neon-sign light, or
// no light at all (screens/holograms stay emissive-only) — SAME constants
// (color/range/lift) as the host. `tag` is used only for the retagged log
// line. Returns an empty vector (with a warn log) if `layoutPath` can't be
// opened or has zero valid piece lines — mirrors loadDistrict's early-return.
std::vector<x3::rhi::PointLight> harvestDistrictLights(
    EchoRegionCtx& ctx,
    const char* layoutPath,
    float padX, float padZ, float padYaw, float padScale,
    float padYOff, const char* tag);

// SELF-TEST / DONE-CRITERION PROOF for buildWoodlandsCell: runs the exact
// legacy single-system scatter (no cell filter) and the 9 cell-filtered
// scatters, in pure math — no IRenderDevice, no GLB loads, no EnvArtSystem —
// and checks (a) sum of the 9 cell counts == the global count, and (b) the
// GLOBAL first and last surviving placements' transforms exactly match the
// first/last transform produced by whichever single cell actually contains
// them (located by the same cell-membership test buildWoodlandsCell uses).
// Requires a REAL loaded heightfield (`hf.ok()`); returns false immediately
// (logged) if it isn't loaded, since with no terrain every placement is
// trivially rejected and count==0==0 would "pass" without proving anything.
//
// INTEGRATOR: call once at boot, right after the host's `hf.load(...)`
// succeeds and BEFORE any woodlands cell is actually built/drawn (e.g. gate
// behind `ECHO_STREAM_SELFTEST=1`, or just always run it once — it's cheap,
// pure CPU math, see echo_woodlands.cpp for the cost estimate). On false,
// treat it as a hard integration bug: Milestone A's byte-compare capture
// WILL regress if this doesn't pass, because the 9-cell union would no
// longer equal the legacy single-system scatter it must replace.
bool echoWoodlandsSliceSelfTest(Heightfield& hf);

} // namespace x3::game
