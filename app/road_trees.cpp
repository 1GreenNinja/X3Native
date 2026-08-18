// ROAD TREES — implementation. See road_trees.h for the contract and the
// provenance of every rule; this file is only the arithmetic.

#include "road_trees.h"

#include "asset_root.h"
#include "terrain.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// The two published broadleaf species (assets/manifest.json, 53a2b560),
// relative to convertedGlbRoot(). Native sizes (Y-up, base at y=0, verified
// from the GLB accessor bounds through the root 0.01/X+90 transform):
//   oak    36.8 m tall, crown radius ~18 m  -> scaled 0.580-0.829 (21.3-30.5 m)
//   poplar 22.6 m tall, crown radius  ~4 m  -> scaled 0.945-1.350 (21.4-30.5 m)
// THE OWNER'S NUMBER IS SPEC (NO_SLOP rule 8). Tim, reading the
// spawn-approach capture: "there are some trees but wayy tooo small and
// short!" then, exactly: "70-90 feet high", then "they can be taller..
// 100 feet" = 21.3-30.5 m. The old rolls (0.50-0.75 oak) bottomed out at
// 18 m — a 60 ft shrub next to a 29 m-wide paved road. Both species now
// land inside 70-100 ft, no short rolls.
// PAIRED with the sc rolls in build() below.
constexpr const char* kOakGlb    = "nature/OakBigTree01.glb";
constexpr const char* kPoplarGlb = "nature/PoplarTree001.glb";
// BENCHES UNDER THE TREES (Tim: "benches under them", "Not procedural. we
// have nice bench models in armory"). Both are armory GLBs (localhost:8787
// gallery), textures embedded, metre-scale, origin at the feet (verified
// from accessor bounds: base y=0, ~0.5 m tall, ~1.8-2.2 m long in Z).
constexpr const char* kBenchGlbA = "nature/SM_WoodBench_01a.glb";
constexpr const char* kBenchGlbB = "nature/SM_Bench.glb";
constexpr float kBenchLat      = 12.9f;  // on the verge: apron edge ~9.9 m, front tree row 14 m
constexpr float kBenchFrac     = 0.55f;  // fraction of groves that get one
constexpr float kBenchMaxTilt  = 0.35f;  // reject ground steeper than this per 1.5 m
constexpr float kBenchSink     = 0.03f;  // legs settled in, never floating

// --- Placement constants (metres) ------------------------------------------
constexpr float kLatMin       = 14.0f;  // innermost trunk: outside pavement+apron+wall
constexpr float kLatMax       = 24.0f;  // outermost trunk: still reads as roadSIDE
// 15.5 -> 14.5 (Tim: "reaching over the freeway") — at the 70-90 ft scales
// the oak crown is 10.4-13.4 m of radius, so a trunk at 14.5 m arches its
// canopy out over the near lanes instead of stopping at the apron.
constexpr float kOakLatMin    = 14.5f;
constexpr float kEndPad       = 25.0f;  // no trees hard against the route's ends
// Portal keep-out past each bore end: headwall (1.7) + canopy (3) + wingwall
// splay (6.5) + backfill taper (15) is ~26 m of built structure.
//
// THE RECEIPT (owner, 2026-08-17, driving the bore): "tunnel looks great! It
// is blocked" — a full oak crown sat dead centre in the far portal, seen from
// inside. 30 m was sized against the TRUNK and the built structure, and the
// trunk was indeed clear: the defect is the CROWN. An oak's crown radius is
// ~18 m at scale 1 and these plant at 0.580-0.829, so a trunk on the innermost
// row (kOakLatMin 14.5 m) throws canopy to lat -0.5 m — over the centreline,
// which is exactly the shade Tim asked for in open country and exactly what
// plugs a 12 m portal mouth when it happens 30 m from one. The margin has to
// clear the CROWN, not the bole: 30 -> 80 m, i.e. the deepest crown reach
// (~15 m) plus the throat plus enough approach that the mouth frames sky.
// PAIRED with the oak scale rolls in build() — a bigger tree needs a bigger
// margin, and both numbers live in this file for that reason.
constexpr float kPortalMargin = 80.0f;
constexpr float kSink         = 0.15f;  // trunk sunk so no root plate floats on a slope
constexpr float kMaxLocalDrop = 2.2f;   // reject batter positions steeper than this per 2 m
constexpr float kMinSpacing   = 4.5f;   // trunks never closer than this inside a grove
// --- Grove layout -----------------------------------------------------------
// Grove slots are stratified over the OPEN spans (route minus the roofed span
// + portal margins), NOT the whole route — on this route the tunnel swallows
// half the length, and stratifying over all of it starved the daylight
// stretches down to a single grove (first build: 10 trees).
// THICK WOODS (Tim: "A whole grove.. not just 2", "Tthick woods on much of
// the road!!!!") — slots halved in length, every slot keeps its grove, and
// each grove is a stand of 16-40 trunks over up to 90 m. kMinSpacing still
// bounds trunk density, so "thick" comes from coverage, not clipping crowns.
constexpr float kSlotLen        = 25.0f; // one grove slot per this much open road
constexpr int   kSlotsMaxPerSpan= 12;
constexpr float kGroveKeep      = 1.0f;  // fraction of slots that actually get a grove
constexpr int   kTreesMin       = 16;    // per grove
constexpr int   kTreesMax       = 40;
constexpr float kGroveLenMin    = 30.0f;
constexpr float kGroveLenMax    = 90.0f;
constexpr float kOffSideFrac    = 0.30f; // chance a double-sided grove's tree crosses over
constexpr uint32_t kSeed        = 0x5EEDA11u;

// Deterministic LCG (numerical-recipes constants — the same family the
// codebase already uses; NO rand(), so the forest is identical every boot).
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    float next() {                       // [0,1)
        s = s * 1664525u + 1013904223u;
        return (float)((s >> 8) & 0xFFFFFFu) / 16777216.0f;
    }
};

} // anonymous namespace

bool RoadTrees::build(x3::rhi::IRenderDevice& device, const TunnelRoute& route,
                      const std::vector<KeepOut>& keepOut, float minBenchY,
                      x3::phys::IPhysicsWorld* phys) {
    if (m_built) return m_trees > 0;
    m_built = true;

    // The GLBs ship a far-LOD "Billboard" card node alongside the LOD0 mesh;
    // drawn together they would z-fight a flat grey card through every crown.
    // Skip it by node/material name (must be set BEFORE any load).
    m_art.setNodeSkip({ "billboard" });

    // Both species now ship REAL textures (harvested 2026-08-16 from the
    // source Unity packs — Big Oak Tree FREE / Big Poplar Tree FREE — via a
    // fresh FBX2glTF geometry pass + hand-injected bark/leaf maps, replacing
    // the earlier textureless GLBs). LOD0 carries two materials per species:
    // bark (albedo+normal, OPAQUE) and leaves (albedo+alpha+normal, MASK,
    // doubleSided) — see docs/design/X3_WORLD_RULES.md rule 5. No override
    // needed any more; the flat-tint patch this block used to apply is gone.

    if (!m_art.beginFromDir(device, convertedGlbRoot())) {
        x3::logWarn("road_trees: converted_glb root mount failed — treeless road");
        return false;
    }

    Lcg rng(kSeed);

    // Keep-out around the roofed span (portals, wingwalls, backfill taper).
    const bool  haveBore = route.boreValid;
    const float keep0 = haveBore ? route.boreS0 - kPortalMargin : 1e9f;
    const float keep1 = haveBore ? route.boreS1 + kPortalMargin : -1e9f;

    const float s0 = kEndPad, s1 = route.totalLen - kEndPad;

    // The OPEN spans: the route minus the roofed span + portal margins. Groves
    // exist only in daylight — there is nothing to shade inside the bore.
    struct Span { float a, b; };
    std::vector<Span> spans;
    if (!haveBore) {
        spans.push_back({ s0, s1 });
    } else {
        if (keep0 - s0 > kGroveLenMin) spans.push_back({ s0, std::min(keep0, s1) });
        if (s1 - keep1 > kGroveLenMin) spans.push_back({ std::max(keep1, s0), s1 });
    }

    struct Placed { float x, z; };
    uint32_t oaks = 0, poplars = 0, rejected = 0;

    for (const Span& span : spans) {
        const float spanLen = span.b - span.a;
        const int slots = std::min(kSlotsMaxPerSpan,
                                   std::max(1, (int)(spanLen / kSlotLen)));
        const float slotLen = spanLen / (float)slots;

        for (int slot = 0; slot < slots; ++slot) {
            // SOME areas, not all: a slot only grows a grove kGroveKeep of the
            // time, so shaded stretches alternate with open country.
            if (rng.next() >= kGroveKeep) continue;

            const float slotS0 = span.a + slotLen * (float)slot;
            const float gl  = std::min(kGroveLenMin +
                                       rng.next() * (kGroveLenMax - kGroveLenMin),
                                       slotLen);
            const float sC  = slotS0 + gl * 0.5f + rng.next() * (slotLen - gl);
            const int   want = kTreesMin + (int)(rng.next() * (float)(kTreesMax - kTreesMin + 1));
            // Species character per grove: pure oak stand, pure poplar stand,
            // or mixed — one-species stands are how real roadside groves read.
            const float kind = rng.next();        // <0.40 oak, <0.80 poplar, else mixed
            // Groves sit on the SUN side (-lat: shadows fall toward +lat,
            // across the pavement). Some groves are double-sided for enclosure.
            const bool twoSided = rng.next() < 0.35f;

            std::vector<Placed> inGrove;
            uint32_t planted = 0;
            // Up to 4 candidates per wanted tree: a draw that lands on a steep
            // cut batter / in the water is retried elsewhere in the grove
            // instead of silently shrinking the stand.
            const int attempts = want * 4;
            for (int t = 0; t < attempts && planted < (uint32_t)want; ++t) {
                const float ts = sC + (rng.next() * 2.0f - 1.0f) * gl * 0.5f;
                const bool oak = (kind < 0.40f) ? true
                               : (kind < 0.80f) ? false
                               : (rng.next() < 0.5f);
                const float sideRoll = rng.next();
                const float latRoll  = rng.next();
                const float yawRoll  = rng.next();
                const float sclRoll  = rng.next();

                if (ts < s0 || ts > s1) { ++rejected; continue; }
                if (ts > keep0 && ts < keep1) { ++rejected; continue; }

                const float side = (twoSided && sideRoll < kOffSideFrac) ? 1.0f : -1.0f;
                const float latMin = oak ? kOakLatMin : kLatMin;
                // latRoll SQUARED biases trunks toward the inner edge of the
                // band — the crowns overhanging the verge are what actually
                // throw shade onto the pavement, so the front row is the one
                // that matters and the back row is depth.
                const float lat  = side * (latMin + latRoll * latRoll * (kLatMax - latMin));

                float x = 0.0f, z = 0.0f;
                route.worldAt(ts, lat, x, z);
                const float y = terrainHeightAtWorld(x, z);

                // Feet on believable ground: not on a steep cut batter, not
                // in the water, never in a hole below the road datum.
                const float hX = terrainHeightAtWorld(x + 2.0f, z);
                const float hZ = terrainHeightAtWorld(x, z + 2.0f);
                if (std::fabs(hX - y) > kMaxLocalDrop ||
                    std::fabs(hZ - y) > kMaxLocalDrop) { ++rejected; continue; }
                if (y < worldWaterLevelAt(x, z) + 0.5f) { ++rejected; continue; }
                // -1.0 -> -8.0: the old cutoff rejected the entire FILL side
                // of the graded route (embankment verges sit metres below the
                // datum), which starved the thick-woods pass down to 10 trees
                // / 190 rejects on the demo route. A 70-100 ft tree on the
                // batter still towers over the road; only a real ravine is
                // out of bounds.
                if (y < route.roadYAt(ts) - 8.0f)       { ++rejected; continue; }

                bool inKeepOut = false;
                for (const KeepOut& k : keepOut) {
                    const float dx = k.x - x, dz = k.z - z;
                    if (dx * dx + dz * dz < k.r * k.r) { inKeepOut = true; break; }
                }
                if (inKeepOut) { ++rejected; continue; }

                bool tooClose = false;
                for (const Placed& p : inGrove) {
                    const float dx = p.x - x, dz = p.z - z;
                    if (dx * dx + dz * dz < kMinSpacing * kMinSpacing) { tooClose = true; break; }
                }
                if (tooClose) { ++rejected; continue; }

                const float sc  = oak ? (0.580f + sclRoll * 0.249f)  // 70-100 ft oak
                                      : (0.945f + sclRoll * 0.405f); // 70-100 ft poplar
                const float yaw = yawRoll * 6.2831853f;
                const float c = std::cos(yaw), sn = std::sin(yaw);
                // Column-major yaw*uniform-scale + translation, base sunk kSink
                // (X3_WORLD_RULES rule 4: origin at the contact surface).
                const float T[16] = { c * sc, 0, -sn * sc, 0,
                                      0,      sc, 0,       0,
                                      sn * sc, 0, c * sc,  0,
                                      x, y - kSink, z, 1 };
                if (m_art.addGlbInstance(oak ? kOakGlb : kPoplarGlb, T)) {
                    inGrove.push_back({ x, z });
                    ++planted;
                    if (oak) ++oaks; else ++poplars;
                    // ---- TRUNK COLLISION (owner ask, 2026-08-17) ----------
                    // MEASURED FROM THE ASSET, not guessed (NO_SLOP rule 9).
                    // tools/tree_bole.py scans 2%-height slices of the BARK
                    // primitive and takes the NARROWEST, because a fixed low
                    // band reads 15.4 m across on the oak — its bark mesh
                    // carries branches from 12% of the height up. At scale 1:
                    //   oak    35.33 m tall | bole 2.857 m across | branches 4.2 m
                    //   poplar 21.54 m tall | bole 0.997 m across | branches 3.0 m
                    // The box is INSCRIBED in the round bole (0.80 of the half
                    // width) so its corners do not stand out in thin air, and it
                    // rides this tree's own `sc` roll — the same scale the mesh
                    // got, which is what keeps the collider ON the wood instead
                    // of near it. 5 m of height clears both species' boles with
                    // margin, so a car on a batter cannot ride over one.
                    // The CROWN is drive-through ON PURPOSE: brushing leaf cards
                    // must not stop a car dead, and 200 crown hulls would be 200
                    // broadphase boxes for no gameplay.
                    if (phys) {
                        const float trunkHalfW = (oak ? 1.143f : 0.399f) * sc;
                        const float boleH      = 5.0f * sc;
                        const x3::phys::BodyId tb = phys->addBox(
                            x3::phys::Vec3{ trunkHalfW, boleH * 0.5f, trunkHalfW },
                            x3::phys::Vec3{ x, y - kSink + boleH * 0.5f, z },
                            0.0f, x3::phys::Layer::Static);
                        if (tb.valid()) ++m_trunkBodies;
                    }
                }
            }
            if (planted > 0) {
                ++m_groves;
                // ---- A BENCH UNDER THE GROVE (owner ask). Sits on the verge
                // between the apron and the front tree row, long axis along
                // the road, facing the pavement. Same ground rules as trunks:
                // flat-enough, dry, outside keep-outs, CONTACT LAW base.
                if (rng.next() < kBenchFrac) {
                    // Several tries per grove: the approach cuttings' batters
                    // fail the flat-ground test at many stations, and a
                    // one-and-done roll shipped "0 benches" on the whole
                    // route (first run's receipt).
                    for (int bt = 0; bt < 6; ++bt) {
                        const float bs   = sC + (rng.next() * 2.0f - 1.0f) * gl * 0.45f;
                        const float side = (bt < 4) ? -1.0f : 1.0f;  // prefer sun side
                        if (bs > keep0 && bs < keep1) continue;
                        // SCAN OUTWARD for a sittable spot. The bench lies
                        // ALONG THE CONTOUR (long axis on the road tangent),
                        // so the honest flatness test is its own four
                        // corners (1.8 x 0.6 m footprint), not a 3 m
                        // world-axis cross — the axis test rejected every
                        // station on this route (receipt: two 0-bench runs),
                        // while the batter-lip first-pass seated one half in
                        // the slope (receipt: shots_bench6/bench_side.png).
                        float ttx0, ttz0, ttx1, ttz1;
                        route.worldAt(std::max(bs - 2.0f, 0.0f), 0.0f, ttx0, ttz0);
                        route.worldAt(std::min(bs + 2.0f, route.totalLen), 0.0f, ttx1, ttz1);
                        float tdx = ttx1 - ttx0, tdz = ttz1 - ttz0;
                        const float tl = std::sqrt(tdx * tdx + tdz * tdz);
                        if (tl < 0.01f) continue;
                        tdx /= tl; tdz /= tl;
                        const float pdx = -tdz, pdz = tdx;   // across the road
                        float bx = 0.0f, bz = 0.0f, ty = 0.0f;
                        bool ok = false;
                        for (float blat = 12.5f; blat <= 19.5f; blat += 0.75f) {
                            route.worldAt(bs, side * blat, bx, bz);
                            float hMin = 1e9f, hMax = -1e9f;
                            for (int ci = 0; ci < 4; ++ci) {
                                const float su = (ci & 1) ? 0.9f : -0.9f;
                                const float sv = (ci & 2) ? 0.3f : -0.3f;
                                const float h = terrainHeightAtWorld(
                                    bx + tdx * su + pdx * sv,
                                    bz + tdz * su + pdz * sv);
                                hMin = std::min(hMin, h); hMax = std::max(hMax, h);
                            }
                            if (hMax - hMin > 0.22f) continue;
                            ty = (hMax + hMin) * 0.5f;
                            // Since task #32 the drawn river surface IS the
                            // worldWaterLevelAt table (the minBenchY shim is
                            // deleted). A bench also clears the rain-runoff
                            // head-room so a storm-swollen river never laps a
                            // seat (rain rise is bounded by
                            // kWorldRiverRainRiseMax; see terrain.h).
                            if (ty < worldWaterLevelAt(bx, bz) + 0.5f
                                     + kWorldRiverRainRiseMax) continue;
                            if (ty < route.roadYAt(bs) - 3.0f) continue;
                            ok = true;
                            break;
                        }
                        for (const KeepOut& k : keepOut) {
                            const float dx = k.x - bx, dz = k.z - bz;
                            if (dx * dx + dz * dz < k.r * k.r) { ok = false; break; }
                        }
                        if (!ok) continue;
                        float tx0, tz0, tx1, tz1;
                        route.worldAt(std::max(bs - 2.0f, 0.0f), 0.0f, tx0, tz0);
                        route.worldAt(std::min(bs + 2.0f, route.totalLen), 0.0f, tx1, tz1);
                        // Model long axis is +Z; yaw maps it onto the tangent.
                        const float yaw = std::atan2(tx1 - tx0, tz1 - tz0);
                        const float c = std::cos(yaw), sn = std::sin(yaw);
                        const float T[16] = { c, 0, -sn, 0,
                                              0, 1, 0,   0,
                                              sn, 0, c,  0,
                                              bx, ty - kBenchSink, bz, 1 };
                        const char* glb = (rng.next() < 0.5f) ? kBenchGlbA : kBenchGlbB;
                        if (m_art.addGlbInstance(glb, T)) {
                            ++m_benches;
                            // W-NIGHT: record the site for the campfire pass.
                            // toward-road = the direction that DECREASES |lat|,
                            // i.e. -side * (the +lat perpendicular pdx/pdz).
                            m_benchSites.push_back(BenchSite{
                                bx, ty, bz, yaw, -side * pdx, -side * pdz });
                            // Coords logged so the eye-gate can aim a camera
                            // at every bench (NO_SLOP rule 2).
                            x3::logInfo("road_trees: bench at (" +
                                        std::to_string(bx) + ", " +
                                        std::to_string(ty) + ", " +
                                        std::to_string(bz) + ")");
                            break;
                        }
                    }
                }
            }
        }
    }

    m_art.setFoliage(1.0f);   // vegetation wrap + canopy back-translucency
    m_trees = oaks + poplars;

    if (phys) {
        x3::logInfo("road_trees: " + std::to_string(m_trunkBodies) +
                    " trunk colliders (bole only; crowns stay drive-through)");
    } else {
        x3::logWarn("road_trees: no physics world passed — trees have NO trunk "
                    "collision (the owner asked for it 2026-08-17; check the "
                    "host's build() call)");
    }
    x3::logInfo("road_trees: " + std::to_string(m_trees) + " trees (" +
                std::to_string(oaks) + " oak, " + std::to_string(poplars) +
                " poplar) in " + std::to_string(m_groves) + " groves, " +
                std::to_string(m_benches) + " benches, along " +
                std::to_string((int)route.totalLen) + " m of road (" +
                std::to_string(rejected) + " positions rejected)");
    if (m_trees == 0)
        x3::logWarn("road_trees: NOTHING PLANTED — tree GLBs missing? run "
                    "`python tools/asset_store.py fetch --all`");
    return m_trees > 0;
}

uint32_t RoadTrees::draw(x3::rhi::IRenderDevice& device,
                         const x3::rhi::FrameContext& frame) const {
    if (m_trees == 0) return 0;
    return m_art.draw(device, frame);
}

void RoadTrees::shutdown(x3::rhi::IRenderDevice& device) {
    if (!m_built) return;
    m_art.destroy(device);
    m_trees = 0;
    m_groves = 0;
}

} // namespace x3::game
