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

// --- Placement constants (metres) ------------------------------------------
constexpr float kLatMin       = 14.0f;  // innermost trunk: outside pavement+apron+wall
constexpr float kLatMax       = 24.0f;  // outermost trunk: still reads as roadSIDE
// 15.5 -> 14.5 (Tim: "reaching over the freeway") — at the 70-90 ft scales
// the oak crown is 10.4-13.4 m of radius, so a trunk at 14.5 m arches its
// canopy out over the near lanes instead of stopping at the apron.
constexpr float kOakLatMin    = 14.5f;
constexpr float kEndPad       = 25.0f;  // no trees hard against the route's ends
// Portal keep-out past each bore end: headwall (1.7) + canopy (3) + wingwall
// splay (6.5) + backfill taper (15) is ~26 m of built structure; 30 keeps a
// clean verge without wasting half of the little open country this route has
// (the roofed span is 327 m of the 640 — daylight is the scarce resource).
constexpr float kPortalMargin = 30.0f;
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
                      const std::vector<KeepOut>& keepOut) {
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
                if (y < route.roadYAt(ts) - 1.0f)       { ++rejected; continue; }

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
                }
            }
            if (planted > 0) ++m_groves;
        }
    }

    m_art.setFoliage(1.0f);   // vegetation wrap + canopy back-translucency
    m_trees = oaks + poplars;

    x3::logInfo("road_trees: " + std::to_string(m_trees) + " trees (" +
                std::to_string(oaks) + " oak, " + std::to_string(poplars) +
                " poplar) in " + std::to_string(m_groves) + " groves along " +
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
