// ===========================================================================
// SEA-LEVEL CONSISTENCY GATE  (--test-sealevel)
//
// Echo Harbor shipped FOUR live answers to "where is the sea" (+0.10 wave
// patch, 0.00 heightfield zero-crossing, -0.40 baked ocean quad, -0.30 swim
// entry). Unifying them once is worth little on its own: they were unified
// once before, and drifted, because nothing could tell. This file is the part
// that lasts. It makes the wrong state UNSHIPPABLE rather than merely fixed.
//
// The gate deliberately reaches ACROSS the two boundaries the drift actually
// crossed:
//   * C++ constant vs C++ constant is the easy half (S1-S6).
//   * C++ vs the BAKE TOOL (tools/echo_terrain_gen.py) is the half that
//     mattered — OCEAN_Y and SEANORM live in Python, are consumed by a 27 MB
//     committed GLB, and no compiler has ever seen them. S7 PARSES the tool and
//     compares. A constant that only agrees with itself in one language is not
//     a constraint.
//   * C++ vs the shipped ASSET is the third half nobody had: S8 loads the real
//     heightmap and checks the datum against the coastline it actually
//     produces, and against the sand band the albedo bake paints.
//
// S9 is not an assertion — it is the honest residual, printed every run: the
// one place the unification does NOT reach, and how big it is.
// ===========================================================================

#include "world_hosts/echo_sea.h"
#include "world_hosts/echo_water.h"
#include "world_hosts/echo_heightfield.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace x3::game {

namespace {

int gPass = 0, gFail = 0;

void sCheck(bool ok, const std::string& what) {
    if (ok) { ++gPass; x3::logInfo("  [PASS] " + what); }
    else    { ++gFail; x3::logError("  [FAIL] " + what); }
}

std::string f2(float v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", (double)v); return b; }

// Tolerance for "these two constants are the same datum". Deliberately TIGHT:
// these are supposed to be one number, not two numbers that nearly agree. A
// millimetre is already three orders of magnitude below the 0.5 m the drift
// cost, so anything looser would let the bug back in.
constexpr float kTol = 1.0e-3f;

// ---- S7 support: read a scalar assignment out of the bake tool -------------
// Matches a line like `OCEAN_Y    = -0.4              # comment`.
bool pyConst(const std::string& src, const char* name, float& out) {
    std::istringstream in(src);
    std::string line;
    const std::string key = name;
    while (std::getline(in, line)) {
        // Must be an assignment at column 0 (module scope), not a mention.
        if (line.compare(0, key.size(), key) != 0) continue;
        size_t i = key.size();
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size() || line[i] != '=') continue;
        ++i;
        try { out = std::stof(line.substr(i)); } catch (...) { continue; }
        return true;
    }
    return false;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The repo-relative files this gate reads. Tests run from the repo root in CI
// and from build/bin/Release by hand, so try both plus the asset root.
std::string findFile(const std::string& rel) {
    const std::string cands[] = {
        rel,
        "../../../" + rel,
        x3::game::assetRoot() + "/../" + rel,
    };
    for (const auto& c : cands) { std::ifstream f(c); if (f) return c; }
    return {};
}

} // namespace

bool runSeaLevelSelfTest() {
    gPass = gFail = 0;
    x3::logInfo("=== ECHO HARBOR SEA-LEVEL CONSISTENCY GATE ===");
    x3::logInfo("  datum kEchoSeaLevelY = " + f2(kEchoSeaLevelY) +
                "   ring kEchoOceanRingY = " + f2(kEchoOceanRingY) +
                "   max amplitude = " + f2(echoMaxAmplitude()));

    // ---- S1. The heightfield encoding puts its zero-crossing ON the datum.
    // This is the definition the datum was CHOSEN for; if kSeaNorm/kScale ever
    // stop producing it, every seat in the world silently moves relative to the
    // sea and nothing else here would notice.
    sCheck(std::fabs(echoHeightfieldSeaY() - kEchoSeaLevelY) < kTol,
           "S1 heightfield zero-crossing == datum (heightAt(kSeaNorm) = " +
           f2(echoHeightfieldSeaY()) + ", datum " + f2(kEchoSeaLevelY) + ")");

    // ---- S2. EVERY swell preset states the datum, not a sea height of its own.
    // This is the exact drift that happened: the table said 0.10 while the rest
    // of the world said 0.
    for (int i = 0; i < 3; ++i) {
        const SwellPreset& p = kSwellPresets[i];
        sCheck(std::fabs(p.tune.seaLevel - kEchoSeaLevelY) < kTol,
               std::string("S2 swell preset '") + p.name + "' seaLevel == datum (" +
               f2(p.tune.seaLevel) + ")");
    }

    // ---- S3. The default-constructed WaterTuning also rides the datum, so a
    // caller that forgets to assign seaLevel cannot introduce a fifth sea.
    {
        WaterTuning t{};
        sCheck(std::fabs(t.seaLevel - kEchoSeaLevelY) < kTol,
               "S3 default WaterTuning::seaLevel == datum (" + f2(t.seaLevel) + ")");
    }

    // ---- S4. THE RING CONSTRAINT. The ring is a FLOOR, not a datum: every
    // preset's worst-case 4-octave trough must clear it by the stated margin.
    // This is what replaced "lift the sea by 0.10 m" as the way the shards are
    // kept away, so it is the assertion holding the whole choice together.
    for (int i = 0; i < 3; ++i) {
        const SwellPreset& p = kSwellPresets[i];
        const float trough = echoWorstTroughY(p.tune.amplitude);
        const float margin = trough - kEchoOceanRingY;
        sCheck(margin >= kEchoRingMargin - kTol,
               std::string("S4 swell preset '") + p.name + "' trough " + f2(trough) +
               " clears the ring by " + f2(margin) + " m (need >= " +
               f2(kEchoRingMargin) + ")");
        // And the table's own documented numbers must equal the derived ones —
        // a comment that disagrees with the code is how this started.
        sCheck(std::fabs(p.worstTroughY - trough) < kTol &&
               std::fabs(p.ringMarginM  - margin) < kTol,
               std::string("S4b preset '") + p.name +
               "' published trough/margin == derived");
    }

    // ---- S5. Amplitude bound is self-consistent: the largest shipped
    // amplitude must not exceed echoMaxAmplitude(), and echoMaxAmplitude()
    // itself must sit exactly at the margin.
    {
        float maxAmp = 0.0f;
        for (int i = 0; i < 3; ++i) maxAmp = std::max(maxAmp, kSwellPresets[i].tune.amplitude);
        sCheck(maxAmp <= echoMaxAmplitude() + kTol,
               "S5 largest shipped amplitude " + f2(maxAmp) +
               " <= echoMaxAmplitude() " + f2(echoMaxAmplitude()));
        sCheck(std::fabs((echoWorstTroughY(echoMaxAmplitude()) - kEchoOceanRingY)
                         - kEchoRingMargin) < kTol,
               "S5b echoMaxAmplitude() lands exactly on the ring margin");
    }

    // ---- S6. DERIVED OFFSETS. Each of these was an absolute literal before;
    // the assertion is that it is now the datum PLUS a stated distance, so
    // moving the datum moves it too. If someone re-hardcodes one, its offset
    // from the datum changes and this fails.
    struct Derived { const char* name; float value; float expectOffset; };
    const Derived derived[] = {
        { "boat hull Y (freeboard)",   echoBoatY(),        +kEchoBoatFreeboard  },
        { "keel draft",                echoKeelDraft(),    -kEchoKeelDepth      },
        { "roads kWaterMinLand",       echoWaterMinLand(), +kEchoLandMinClear   },
        { "roads kGateLandSafe",       echoGateLandSafe(), +kEchoGateClear      },
        { "roads/lots kLandSafe",      echoLandSafe(),     +kEchoLandSafeClear  },
        { "swim floor",                echoSwimFloorY(),   -kEchoSwimMinDepth   },
    };
    for (const Derived& d : derived)
        sCheck(std::fabs((d.value - kEchoSeaLevelY) - d.expectOffset) < kTol,
               std::string("S6 ") + d.name + " = datum " +
               (d.expectOffset >= 0 ? "+ " : "- ") + f2(std::fabs(d.expectOffset)) +
               " -> " + f2(d.value));

    // Ordering sanity: these thresholds only mean anything if they stack in the
    // right order around the sea. A sign flip on any offset would pass the
    // arithmetic above and still be nonsense.
    sCheck(echoKeelDraft() < echoSwimFloorY() &&
           echoSwimFloorY() < kEchoSeaLevelY &&
           kEchoSeaLevelY < echoWaterMinLand() &&
           echoWaterMinLand() < echoGateLandSafe() &&
           echoGateLandSafe() < echoLandSafe(),
           "S6b thresholds stack in order: keel < swim < SEA < minLand < gate < landSafe");

    // ---- S7. CROSS-LANGUAGE. tools/echo_terrain_gen.py owns the bake; its
    // constants are consumed by a committed GLB no compiler can check. THIS is
    // the boundary the -0.4-vs-+0.10 drift lived on.
    {
        const std::string p = findFile("tools/echo_terrain_gen.py");
        if (p.empty()) {
            x3::logWarn("  [SKIP] S7 tools/echo_terrain_gen.py not found from CWD — "
                        "cross-language check not run (run from the repo root)");
        } else {
            const std::string src = slurp(p);
            float oceanY = 0.0f, seaNorm = 0.0f, hscale = 0.0f, minLand = 0.0f;
            const bool gotO = pyConst(src, "OCEAN_Y", oceanY);
            const bool gotS = pyConst(src, "SEANORM", seaNorm);
            const bool gotH = pyConst(src, "HSCALE", hscale);
            const bool gotW = pyConst(src, "WATER_MIN_LAND", minLand);
            sCheck(gotO && std::fabs(oceanY - kEchoOceanRingY) < kTol,
                   "S7 bake OCEAN_Y (" + (gotO ? f2(oceanY) : std::string("MISSING")) +
                   ") == kEchoOceanRingY " + f2(kEchoOceanRingY));
            sCheck(gotS && std::fabs(seaNorm - Heightfield::kSeaNorm) < kTol,
                   "S7b bake SEANORM (" + (gotS ? f2(seaNorm) : std::string("MISSING")) +
                   ") == Heightfield::kSeaNorm " + f2(Heightfield::kSeaNorm));
            sCheck(gotH && std::fabs(hscale - Heightfield::kScale) < 0.5f,
                   "S7c bake HSCALE (" + (gotH ? f2(hscale) : std::string("MISSING")) +
                   ") == Heightfield::kScale " + f2(Heightfield::kScale));
            // The bake's own land threshold must be the same distance above the
            // sea as the engine's. These are two copies of one rule.
            sCheck(gotW && std::fabs((minLand - kEchoSeaLevelY) - kEchoLandMinClear) < kTol,
                   "S7d bake WATER_MIN_LAND (" + (gotW ? f2(minLand) : std::string("MISSING")) +
                   ") == datum + " + f2(kEchoLandMinClear));
        }
    }

    // ---- S8. AGAINST THE ISLAND THAT ACTUALLY LOADS. Constants agreeing with
    // each other still says nothing about the bake the game renders — and this
    // world has TWO. host_echotropolis.cpp prefers an out-of-repo authoring-box
    // bake (2048^2) over the committed regen bake (1025^2) when it exists, so a
    // gate that only ever read `assets/island_mesa` would be certifying a
    // heightmap the running game never opens. That is the same class of mistake
    // as the datum drift itself, so S8 resolves the island EXACTLY the way the
    // host does, and PRINTS which one it got.
    {
        Heightfield hf;
        std::string hp;
        if (const char* e = std::getenv("ECHO_ISLAND_DIR"))
            hp = std::string(e) + "/island_height_20260530.png";
        if (hp.empty() || !std::ifstream(hp)) {
            const char* hostPref = "D:/GameDev/EchoHarbor/assets/island_mesa"
                                   "/island_height_20260530.png";
            if (std::ifstream(hostPref)) hp = hostPref;
            else {
                hp = findFile("assets/island_mesa/island_height_20260530.png");
                if (hp.empty())
                    hp = x3::game::assetRoot() + "/island_mesa/island_height_20260530.png";
            }
        }
        if (!hf.load(hp)) {
            x3::logWarn("  [SKIP] S8 island heightmap not found (" + hp +
                        ") — asset checks not run");
        } else {
            x3::logInfo("  [S8] island heightmap = " + hp + "  (" +
                        std::to_string(hf.w) + "^2)");
            hf.setMeshGrid(513);   // the RENDERED surface, per fix/echo-road-surface
            // (a) The datum must lie inside the sand band the albedo bake paints
            // (-1.0 .. +1.5 m). If the waterline sits outside the painted beach,
            // the art and the geometry disagree no matter how consistent the
            // code is.
            sCheck(kEchoSeaLevelY > kEchoSandBandLo && kEchoSeaLevelY < kEchoSandBandHi,
                   "S8a datum " + f2(kEchoSeaLevelY) + " lies inside the baked sand band [" +
                   f2(kEchoSandBandLo) + ", " + f2(kEchoSandBandHi) + "]");

            // (b) The datum must actually separate sea from land on THIS bake:
            // sample the whole island and require a real coastline — a
            // meaningful fraction wet and a meaningful fraction dry. A datum
            // above every peak or below every trough would pass every check
            // above and be absurd.
            const int N = 257;
            int wet = 0, dry = 0, total = 0;
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c) {
                    const float x = -2048.0f + 4096.0f * (float)c / (float)(N - 1);
                    const float z = -2048.0f + 4096.0f * (float)r / (float)(N - 1);
                    const float h = hf.heightAt(x, z);
                    ++total;
                    if (h < kEchoSeaLevelY) ++wet; else ++dry;
                }
            const float wetFrac = (float)wet / (float)total;
            sCheck(wetFrac > 0.05f && wetFrac < 0.95f,
                   "S8b the datum cuts a real coastline on the shipped bake (" +
                   f2(wetFrac * 100.0f) + "% of the 4 km frame is below it)");

            // (c) THE POINT OF THE WHOLE LANE. Land that every placement gate
            // calls dry (heightAt >= datum) must not be underwater in the frame
            // the player sees. Before unification the drawn sea was 0.10 m
            // above the datum, so every cell in [datum, datum+0.10) was
            // "dry land" that rendered submerged. Assert that band is empty.
            const float drawnSeaStill = kSwellHarbor.seaLevel;
            int falseDry = 0;
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c) {
                    const float x = -2048.0f + 4096.0f * (float)c / (float)(N - 1);
                    const float z = -2048.0f + 4096.0f * (float)r / (float)(N - 1);
                    const float h = hf.heightAt(x, z);
                    if (h >= kEchoSeaLevelY && h < drawnSeaStill) ++falseDry;
                }
            sCheck(falseDry == 0,
                   "S8c no cell is 'dry land' by the datum yet under the DRAWN still "
                   "water (drawn sea " + f2(drawnSeaStill) + ", " +
                   std::to_string(falseDry) + " such cells)");
        }
    }

    // ---- S9. THE HONEST RESIDUAL (reported, not asserted).
    // One thing this lane does NOT unify: the baked ocean quad is the ONLY sea
    // surface drawn when the Gerstner patch is off — at night, and above the
    // 140 m eye-height cutover. There it is still kEchoOceanRingY, i.e. 0.40 m
    // below the datum. It is not fixable without re-baking the island, and it
    // is accepted because it only applies where it cannot be seen. Printed
    // every run so it stays a known limitation instead of becoming a surprise.
    {
        const float residual = kEchoSeaLevelY - kEchoOceanRingY;
        x3::logInfo("  [NOTE] residual: with the wave patch OFF (night, or eye > 140 m) the "
                    "drawn sea is the baked ring at " + f2(kEchoOceanRingY) + " — " +
                    f2(residual) + " m below the datum. Not asserted: it needs a re-bake, "
                    "and at >140 m eye height 0.4 m of waterline is sub-pixel.");
        x3::logInfo("  [NOTE] storm swell cost: amplitude capped at " +
                    f2(echoMaxAmplitude()) + " m by the ring; raising it requires "
                    "re-baking OCEAN_Y deeper, not lifting the sea.");
    }

    x3::logInfo("=== SEA-LEVEL GATE: " + std::to_string(gPass) + " passed, " +
                std::to_string(gFail) + " failed ===");
    return gFail == 0;
}

} // namespace x3::game
