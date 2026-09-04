// THE DRIVE LAYER — see app/drive_layer.h.
#include "drive_layer.h"

#include "city.h"
#include "world_regions.h"
#include "terrain.h"
#include "scene.h"
#include "headless_device.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// The natural (pre-carve) hillside — the same derivation interchange.cpp,
// ridge_road.cpp and the outer connector all use. Scoring against the CARVED
// field would let a site score well because somebody else already cut there.
float naturalAt(float x, float z) {
    return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
}

// The lateral reach a dual carriageway OWNS: the widest cross-section plus the
// carve batter. Nothing may be inside this and survive.
constexpr float kDualCarveHalfM = kFwyDualMaxHalfM;   // ~57 m — registerRoad's halfWidth
constexpr float kDualBatterM    = 18.0f;              // RoadSpec::falloff for a freeway
constexpr float kDualOwnedHalfM = kDualCarveHalfM + kDualBatterM;

// Survey sampling + acceptance. Every number here is a LAW the survey applies,
// not a tuning knob: they are the cross-section the freeway machinery builds
// and the limits registerRoad would otherwise have to bulldoze past.
constexpr float kSurveyStepM     = 25.0f;   // longitudinal sample step
constexpr float kSurveyReachM    = 2200.0f; // how far either way from the anchor point
// The window a freeway's vertical curve spans, and the natural relief allowed
// across it. 30 m of relief over 400 m is a ~15 m cut or fill at the extreme —
// a real highway earthwork, and about what the inner tour's own worst reaches
// carve (RoadBuildResult::maxCutM). Beyond it the "road" is a trench.
constexpr float kGradeWindowM     = 200.0f;   // +- along the alignment
constexpr float kMaxWindowReliefM = 30.0f;
constexpr float kMaxCrossFallM    = 30.0f;   // natural fall across the full dual span
// The shortest run worth calling a freeway. Below this the spawn ring
// (TrafficConfig::ringFarM 1500 m) has nothing to work with and the lane
// splines wrap on themselves.
constexpr float kMinFreewayLenM  = 500.0f;
// The run an interchange needs to have a CHANCE: registerInterchange scores a
// +-kInterchangeZoneR straightness window and demands 90% of it on both sides.
constexpr float kInterchangeCapableLenM = 2.0f * kInterchangeZoneR * 0.9f + 60.0f;

struct Alignment {
    const char* name = "";
    float px = 0.0f, pz = 0.0f;   // a point on the line
    float dx = 1.0f, dz = 0.0f;   // unit direction
};

// Two alignments are the SAME line when their directions are parallel and the
// second's anchor lies on the first. The city declares its E-W freeway three
// times (the West tunnel, the East tunnel, the Scrapyard<->District connector)
// and surveying it three times would inflate the candidate count into a lie.
bool sameLine(const Alignment& a, const Alignment& b) {
    if (std::fabs(a.dx * b.dz - a.dz * b.dx) > 1e-3f) return false;   // not parallel
    const float ox = b.px - a.px, oz = b.pz - a.pz;
    return std::fabs(ox * (-a.dz) + oz * a.dx) < 1.0f;                // no offset
}

} // namespace

// ---------------------------------------------------------------------------
// INTERCHANGE STAND-UP
// ---------------------------------------------------------------------------
InterchangeResult standUpInterchange(const char* worldLabel, bool enabled,
                                     const RoadSpec& fwySpec,
                                     const std::vector<float>& fwyRoadY,
                                     const std::vector<const RoadSpec*>* avoid) {
    InterchangeResult out;
    const char* label = (worldLabel && worldLabel[0]) ? worldLabel : "drive layer";
    const char* e = std::getenv("X3_INTERCHANGE");
    if (!enabled) {
        out.whyNot = "the host has no freeway to site against";
        return out;
    }
    if (e && e[0] == '0') {              // DEFAULT ON (NO_SLOP rule 6)
        out.whyNot = "X3_INTERCHANGE=0";
        x3::logInfo(std::string(label) + ": interchange disabled by X3_INTERCHANGE=0");
        return out;
    }
    out = registerInterchange(fwySpec, fwyRoadY, avoid);
    if (!out.built) {
        x3::logWarn(std::string(label) + ": interchange NOT built — " + out.whyNot);
        return out;
    }
    char b[256];
    std::snprintf(b, sizeof(b),
                  "%s: interchange sited on '%s' at (%.0f, %.0f) — deck %.1f m, "
                  "clearance %.2f m (%.1f ft), %d/4 ramps",
                  label, fwySpec.name.c_str(), out.cx, out.cz, out.deckY,
                  out.clearanceM, out.clearanceM * 3.28084f,
                  (int)(out.ramp[0].built + out.ramp[1].built +
                        out.ramp[2].built + out.ramp[3].built));
    x3::logInfo(b);
    return out;
}

void buildInterchangeGeometry(const InterchangeResult& ic, Scene& scene,
                              x3::rhi::IRenderDevice& device,
                              x3::phys::IPhysicsWorld& phys) {
    if (!ic.built) return;
    // The crossroad's ribbon (its span reach is a gap — the deck owns it), the
    // overpass deck itself, four ramp ribbons at half cross-section, and EIGHT
    // junction mouths: every ramp blends into BOTH roads with the same ruled
    // twist + swooping fillets every at-grade branch gets.
    buildRoadRibbon(ic.spec, scene, device, phys, &ic.roadY);
    buildOverpassDeck(ic, scene, device, phys);
    for (int q = 0; q < 4; ++q) {
        const auto& rp = ic.ramp[q];
        if (!rp.built) continue;
        buildRoadRibbon(rp.spec, scene, device, phys, &rp.roadY);
        buildJunctionMouth(rp.fwyJct, scene, device, phys);
        buildJunctionMouth(rp.crossJct, scene, device, phys);
    }
}

bool interchangeRampMouthsOpen(const InterchangeResult& ic) {
    if (!ic.built) return false;
    int built = 0;
    for (int q = 0; q < 4; ++q) {
        const auto& rp = ic.ramp[q];
        if (!rp.built) continue;
        ++built;
        // A mouth is OPEN when the ramp actually registered a junction on both
        // roads: registerRoadJunctionThroat notes the throat box, and
        // planRoadBarriers refuses to rail any segment within
        // kJunctionBarrierClearM of a noted junction. A ramp whose mouth was
        // never noted would be walled shut by the freeway's own jersey run.
        if (!rp.fwyJct.valid || !rp.crossJct.valid) return false;
        if (distToNearestRoadJunction(rp.fwyJct.jx, rp.fwyJct.jz) > 1.0f) return false;
        if (distToNearestRoadJunction(rp.crossJct.jx, rp.crossJct.jz) > 1.0f) return false;
    }
    return built > 0;
}

// ---------------------------------------------------------------------------
// TRAFFIC STAND-UP
// ---------------------------------------------------------------------------
TrafficLayer::~TrafficLayer() {
    // The contact callback holds a pointer to m_ctx, which is about to die.
    // A host that forgot to shut us down would otherwise leave the physics
    // world holding a dangling user pointer.
    if (m_hookInstalled && m_ctx.p) m_ctx.p->setContactCallback(nullptr, nullptr);
}

bool TrafficLayer::build(const char* worldLabel, bool enabled,
                         const RoadSpec& fwySpec, const std::vector<float>& fwyRoadY,
                         x3::rhi::IRenderDevice* device, x3::phys::IPhysicsWorld* phys,
                         x3::audio::IAudioSystem* audio, std::string_view glbDir,
                         const TrafficConfig& cfg, bool installContactHook) {
    const char* label = (worldLabel && worldLabel[0]) ? worldLabel : "drive layer";
    if (m_traffic.built()) return true;              // idempotent re-realize
    if (!enabled) return false;
    const char* e = std::getenv("X3_TRAFFIC");
    if (e && e[0] == '0') {                          // DEFAULT ON (NO_SLOP rule 6)
        x3::logInfo(std::string(label) + ": traffic disabled by X3_TRAFFIC=0");
        return false;
    }
    if (!m_traffic.build(fwySpec, fwyRoadY, device, phys, glbDir, cfg, audio)) {
        x3::logWarn(std::string(label) + ": traffic did NOT build on '" +
                    fwySpec.name + "' — not a dual carriageway, or a degenerate spec");
        return false;
    }
    if (installContactHook && phys) {
        // THE ONE global contact callback. A hard hit converts the struck car
        // to a dynamic body — the work-zone drum pattern, car-sized.
        m_ctx.t = &m_traffic;
        m_ctx.p = phys;
        phys->setContactCallback(
            [](x3::phys::BodyId a, x3::phys::BodyId b, const float*, const float*,
               float impulse, void* user) {
                auto* c = static_cast<ContactCtx*>(user);
                c->t->onContact(a, b, impulse, c->p);
            }, &m_ctx);
        m_hookInstalled = true;
    } else if (phys) {
        m_ctx.p = phys;
        x3::logInfo(std::string(label) + ": traffic contact hook NOT installed — "
                    "this host already owns IPhysicsWorld's one contact callback "
                    "(hard hits shove cars but do not convert them to wrecks)");
    }
    char b[192];
    std::snprintf(b, sizeof(b), "%s: traffic LIVE on '%s' — %u cars, %u models",
                  label, fwySpec.name.c_str(), m_traffic.liveCount(),
                  m_traffic.modelCount());
    x3::logInfo(b);
    return true;
}

void TrafficLayer::shutdown(x3::phys::IPhysicsWorld* phys) {
    if (m_hookInstalled) {
        if (phys) phys->setContactCallback(nullptr, nullptr);
        else if (m_ctx.p) m_ctx.p->setContactCallback(nullptr, nullptr);
        m_hookInstalled = false;
    }
    m_ctx.t = nullptr;
    // Kinematic boxes out BEFORE the physics world dies. Idempotent: a region
    // can evict twice (stream out, stream out again after a failed realize).
    m_traffic.shutdown(phys ? phys : m_ctx.p);
    m_ctx.p = nullptr;
}

// ---------------------------------------------------------------------------
// THE CANON WORLD'S FREEWAY SURVEY
// ---------------------------------------------------------------------------
CityFreewaySurvey surveyCityFreeway(const CityFreewaySurveyInput& in) {
    CityFreewaySurvey s;
    const float regionAnchorX = in.regionAnchorX;
    const float regionAnchorZ = in.regionAnchorZ;
    const float regionReachM  = in.regionReachM;

    // ---- The candidate alignments, straight from the city's own tables. ----
    std::vector<Alignment> cand;
    auto addAlignment = [&](const char* nm, float px, float pz, float dx, float dz) {
        const float L = std::sqrt(dx * dx + dz * dz);
        if (L < 1e-4f) return;
        Alignment a{ nm, px, pz, dx / L, dz / L };
        for (const Alignment& o : cand) if (sameLine(o, a)) return;
        cand.push_back(a);
    };
    // The four freeway tunnels: the city's declaration of where a freeway
    // LEAVES it, heading for each mountain range.
    for (uint32_t i = 0; i < kFreewayTunnelCount; ++i) {
        const FreewayTunnelPlan& t = cityFreewayTunnelPlan(i);
        addAlignment(t.name, t.mouthX, t.mouthZ, t.dirX, t.dirZ);
    }
    // The connectors: the city's declaration of where through-traffic runs.
    for (uint32_t i = 0; i < cityConnectorCount(); ++i) {
        const CityRoadAlignment& a = cityConnector(i);
        addAlignment(a.name, (a.x0 + a.x1) * 0.5f, (a.z0 + a.z1) * 0.5f,
                     a.x1 - a.x0, a.z1 - a.z0);
    }
    s.candidatesTried = (uint32_t)cand.size();

    x3::logInfo("[drive-layer] city freeway survey: " +
                std::to_string(cand.size()) + " distinct authored alignments, "
                "dual carriageway owns +-" + std::to_string((int)kDualOwnedHalfM) +
                " m, residency reach " + std::to_string((int)regionReachM) +
                " m from (" + std::to_string((int)regionAnchorX) + ", " +
                std::to_string((int)regionAnchorZ) + ")");

    // ---- Score each alignment: the longest contiguous run that clears the
    //      districts, stays resident, and rides ground a freeway can be graded
    //      onto without a scar.
    struct Best { float t0 = 0, t1 = 0, clear = 0, relief = 0, reach = 0, open = 0;
                  const Alignment* a = nullptr; };
    Best best;
    float bestScore = -1e18f;
    // How much run length a metre of walk is worth. 0.15 means a run has to be
    // 150 m longer to justify being a kilometre further from the door.
    constexpr float kReachWeight = 0.15f;
    // ...and how much a metre of MEDIAN-OPEN country is worth. Open median is
    // what buys the interchange (see the station test below), so it is worth
    // more than plain length — but not so much that a 500 m flat stub beats a
    // 900 m freeway.
    constexpr float kOpenMedianWeight = 1.0f;
    const float residencyM = std::max(0.0f, regionReachM - 60.0f);

    for (const Alignment& al : cand) {
        // Per-sample validity, plus WHY the first failure failed (for the
        // report — a "no site" answer that cannot say why is not an answer).
        int rejD = 0, rejR = 0, rejT = 0, rejK = 0;
        float runT0 = 0.0f, runLen = -1.0f, runClear = 0.0f, runRelief = 0.0f;
        float curT0 = 0.0f, curClear = 1e18f, curLo = 1e18f, curHi = -1e18f;
        float curOpen = 0.0f, runOpen = 0.0f;
        bool  inRun = false;
        for (float t = -kSurveyReachM; t <= kSurveyReachM; t += kSurveyStepM) {
            const float qx = al.px + al.dx * t, qz = al.pz + al.dz * t;
            bool ok = true;
            bool openHere = false;      // median-open country at this station
            float loW = 0.0f, hiW = 0.0f;   // natural relief over the grading window
            float clearHere = 1e18f;
            // (1) DISTRICT CLEARANCE — the dual span plus its batter must stay
            //     off every district's built massing.
            for (uint32_t d = 0; d < cityDistrictCount() && ok; ++d) {
                const CityDistrictFootprint& fp = cityDistrictFootprint(d);
                const float ddx = qx - fp.cx, ddz = qz - fp.cz;
                const float gap = std::sqrt(ddx * ddx + ddz * ddz) - fp.massRadius;
                clearHere = std::min(clearHere, gap);
                if (gap < kDualOwnedHalfM) { ok = false; ++rejD; }
            }
            // (2) RESIDENCY — a freeway that streams out from under the player
            //     driving on it is worse than no freeway.
            if (ok) {
                const float rx = qx - regionAnchorX, rz = qz - regionAnchorZ;
                if (std::sqrt(rx * rx + rz * rz) > residencyM) { ok = false; ++rejR; }
            }
            // (3) THE SURFACE LANDMARKS — the crash site and the two
            //     outposts. world_regions.cpp sites the crash site
            //     DELIBERATELY between the two Spire-approach road legs "clear
            //     of both, so nothing straddles the asphalt"; a freeway
            //     widened onto either leg would swallow it whole. Mountain
            //     ranges are skipped: their footprints are kilometres across
            //     and 8-9 km out, and treating them as keep-outs would reject
            //     the whole world.
            if (ok) {
                for (uint32_t r = 0; r < worldRegionPlanCount() && ok; ++r) {
                    const WorldRegionPlan& wp = worldRegionPlanAuthored(r);
                    if (wp.isMountain) continue;
                    const float ddx = qx - wp.cx, ddz = qz - wp.cz;
                    if (std::sqrt(ddx * ddx + ddz * ddz) <
                        wp.radius + kDualOwnedHalfM) { ok = false; ++rejK; }
                }
            }
            // (3b) THE FACILITY — the canon world's tower is not city content
            //     and city.h cannot describe it; the host passes its measured
            //     footprint in. A freeway through the game's main building is
            //     the loudest possible version of "forced a placement".
            if (ok && in.haveKeepOut) {
                const float kx = std::max(in.keepOutX0,
                                          std::min(qx, in.keepOutX1));
                const float kz = std::max(in.keepOutZ0,
                                          std::min(qz, in.keepOutZ1));
                const float ddx = qx - kx, ddz = qz - kz;
                if (std::sqrt(ddx * ddx + ddz * ddz) < kDualOwnedHalfM) {
                    ok = false; ++rejK;
                }
            }
            // (4) TERRAIN — how much CUT a graded freeway would need here, and
            //     the fall ACROSS the full dual span. The longitudinal test is
            //     a WINDOW, not a per-step slope: a freeway is allowed to climb
            //     a hillside at 7%, and a 25 m step that happens to be steep on
            //     the raw field says nothing about the graded road. What
            //     matters is how far the natural ground departs from a
            //     straight run over the length a vertical curve spans, because
            //     THAT is the cut/fill registerRoad would have to dig.
            float y = 0.0f;
            if (ok) {
                y = naturalAt(qx, qz);
                float lo = y, hi = y;
                for (float w = -kGradeWindowM; w <= kGradeWindowM; w += kSurveyStepM) {
                    const float wy = naturalAt(qx + al.dx * w, qz + al.dz * w);
                    lo = std::min(lo, wy); hi = std::max(hi, wy);
                }
                loW = lo; hiW = hi;
                // Relief over the window a freeway's vertical curve spans. The
                // graded road runs somewhere through the middle of it, so half
                // the relief is the cut (or the fill) it costs.
                if (hi - lo > kMaxWindowReliefM) { ok = false; ++rejT; }
                if (ok) {
                    const float nx = -al.dz, nz = al.dx;
                    const float yL = naturalAt(qx + nx * kDualCarveHalfM,
                                               qz + nz * kDualCarveHalfM);
                    const float yR = naturalAt(qx - nx * kDualCarveHalfM,
                                               qz - nz * kDualCarveHalfM);
                    if (std::fabs(yL - yR) > kMaxCrossFallM ||
                        std::fabs(yL - y)  > kMaxCrossFallM ||
                        std::fabs(yR - y)  > kMaxCrossFallM) { ok = false; ++rejT; }
                }
                // MEDIAN-OPEN? computeMedianPlan() widens the median to
                // kFwyMedianMaxHalfM only where the natural country stays
                // within 2.5 m of the GRADED DATUM; everywhere else the
                // carriageways close onto a jersey wall. registerInterchange
                // then refuses any crossing whose median is under 6 m half —
                // the overpass's one pier stands in it. So "how much of this
                // run will keep an open median" decides whether the freeway can
                // carry a grade split at all, and the survey scores for it.
                //
                // The datum does not exist until registerRoad grades the route,
                // so the proxy is the natural RELIEF over the window a vertical
                // curve spans: a graded line runs through the middle of it, so
                // relief under 5 m means cut and fill both stay under ~2.5 m —
                // exactly computeMedianPlan's threshold. (Cross-sectional
                // flatness is NOT the test: it says nothing about the cut.)
                if (ok && (hiW - loW) < 2.0f * 2.5f) openHere = true;
            }
            if (ok) {
                if (!inRun) { inRun = true; curT0 = t; curClear = 1e18f; curLo = 1e18f;
                              curHi = -1e18f; curOpen = 0.0f; }
                curClear = std::min(curClear, clearHere);
                curLo = std::min(curLo, y); curHi = std::max(curHi, y);
                if (openHere) curOpen += kSurveyStepM;
                const float len = t - curT0;
                if (len > runLen) {   // the LONGEST run, for the report
                    runLen = len; runT0 = curT0;
                    runClear = curClear; runRelief = curHi - curLo; runOpen = curOpen;
                }
                // ...and score EVERY qualifying run as it grows, not just the
                // longest one on each alignment. The longest run on an
                // alignment is not necessarily the best PLACE on it: an
                // alignment can offer a 900 m stretch a kilometre from the
                // door and a 900 m stretch beside it, and only scoring the
                // first one found would pick by accident.
                if (len >= kMinFreewayLenM) {
                    const float mt = curT0 + len * 0.5f;
                    const float mx = al.px + al.dx * mt, mz = al.pz + al.dz * mt;
                    float reach = 0.0f;
                    if (in.haveReachFrom) {
                        const float rdx = mx - in.reachFromX, rdz = mz - in.reachFromZ;
                        reach = std::sqrt(rdx * rdx + rdz * rdz);
                    }
                    const float score = len + kOpenMedianWeight * curOpen
                                      - kReachWeight * reach;
                    if (score > bestScore) {
                        bestScore = score;
                        best.a = &al; best.t0 = curT0; best.t1 = t;
                        best.clear = curClear; best.relief = curHi - curLo;
                        best.reach = reach; best.open = curOpen;
                    }
                }
            } else {
                inRun = false;
            }
        }
        if (runLen < 0.0f) runLen = 0.0f;

        char b[300];
        std::snprintf(b, sizeof(b),
                      "[drive-layer]   '%s' along (%.2f, %.2f): longest clear run %.0f m"
                      " (district clearance %.0f m, relief %.0f m, open median %.0f m)"
                      " — rejected samples:"
                      " district %d, residency %d, facility %d, terrain %d",
                      al.name, al.dx, al.dz, runLen,
                      (runLen > 0.0f ? runClear : 0.0f), (runLen > 0.0f ? runRelief : 0.0f),
                      runOpen, rejD, rejR, rejK, rejT);
        x3::logInfo(b);

        s.rejectedDistrict  += (uint32_t)rejD;
        s.rejectedResidency += (uint32_t)rejR;
        s.rejectedTerrain   += (uint32_t)rejT;
        s.rejectedKeepOut   += (uint32_t)rejK;
        if (runLen < kMinFreewayLenM) ++s.rejectedLength;
        if (runLen > s.nearestMissLenM) {
            s.nearestMissName   = al.name;
            s.nearestMissLenM   = runLen;
            s.nearestMissClearM = runLen > 0.0f ? runClear : 0.0f;
        }

    }

    if (!best.a) {
        s.whyNot = "no authored city alignment offers a clear run long enough for a "
                   "dual carriageway";
        char b[300];
        std::snprintf(b, sizeof(b),
                      "[drive-layer] NO SITE: best was '%s' at %.0f m of clear run "
                      "(needs %.0f m). The city's roads are 4-8 m visual strips; a dual "
                      "carriageway owns +-%.0f m and every authored alignment runs "
                      "through a district or off the region.",
                      s.nearestMissName[0] ? s.nearestMissName : "(none)",
                      s.nearestMissLenM, kMinFreewayLenM, kDualOwnedHalfM);
        x3::logWarn(b);
        return s;
    }

    // ---- Build the spec on the winning run. -------------------------------
    const Alignment& a = *best.a;
    s.alignmentName = a.name;
    s.x0 = a.px + a.dx * best.t0; s.z0 = a.pz + a.dz * best.t0;
    s.x1 = a.px + a.dx * best.t1; s.z1 = a.pz + a.dz * best.t1;
    s.lengthM = best.t1 - best.t0;
    s.districtClearM = best.clear;
    s.terrainReliefM = best.relief;
    s.reachM = best.reach;

    std::vector<CourseWaypoint> wp;
    CourseWaypoint w0; w0.x = s.x0; w0.z = s.z0; w0.fillet = 600.0f;
    CourseWaypoint w1; w1.x = s.x1; w1.z = s.z1; w1.fillet = 600.0f;
    wp.push_back(w0); wp.push_back(w1);
    s.spec = makeRoadFromWaypoints("city freeway", wp, 61.0f, /*closed=*/false);
    // The freeway cross-section, identical to the inner tour's (road_network.h):
    // twin 8-lane carriageways about this centreline with a terrain-varied
    // median. Nothing here is a new number.
    s.spec.dualCarriageway  = true;
    s.spec.halfWidth        = kFwyDualMaxHalfM;
    s.spec.falloff          = kDualBatterM;
    s.spec.maxGrade         = 0.07f;
    s.spec.minTurnRadiusM   = 250.0f;
    s.spec.maxDeflectionDeg = 3.0f;
    smoothHorizontalCurves(s.spec);
    s.ok = true;

    char b[320];
    std::snprintf(b, sizeof(b),
                  "[drive-layer] SITE: '%s' — (%.0f, %.0f) to (%.0f, %.0f), %.0f m, "
                  "district clearance %.0f m, natural relief %.0f m, open-median "
                  "country %.0f m, %.0f m from the player's start, %s",
                  a.name, s.x0, s.z0, s.x1, s.z1, s.lengthM,
                  s.districtClearM, s.terrainReliefM, best.open, s.reachM,
                  s.lengthM >= kInterchangeCapableLenM
                      ? "long enough for an interchange window"
                      : "TOO SHORT for an interchange window (traffic only)");
    x3::logInfo(b);
    return s;
}

// ---------------------------------------------------------------------------
// --test-drivelayer
// ---------------------------------------------------------------------------
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[drivelayer-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[drivelayer-test] FAIL " + name); }
}

} // namespace

bool runDriveLayerSelfTest() {
    g_pass = g_fail = 0;

    // ---- D1 the interchange stand-up honours its gates ---------------------
    // Both directions: a host with no freeway gets a REASON, not a crash and
    // not a half-built result; and the env kill switch works.
    {
        RoadSpec empty; std::vector<float> emptyY;
        InterchangeResult r = standUpInterchange("--test-drivelayer", /*enabled=*/false,
                                                 empty, emptyY, nullptr);
        const bool offOk = !r.built && r.whyNot[0] != '\0';

        // With `enabled` true but a degenerate spec, the real siting runs and
        // refuses — the POSITIVE control that standUpInterchange is not simply
        // short-circuiting on the enable flag.
        InterchangeResult r2 = standUpInterchange("--test-drivelayer", /*enabled=*/true,
                                                  empty, emptyY, nullptr);
        const bool onOk = !r2.built && std::strstr(r2.whyNot, "dual") != nullptr;
        check(offOk && onOk,
              "D1 standUpInterchange gates on the host precondition AND still runs the "
              "measured siting when enabled");
        check(!interchangeRampMouthsOpen(r2),
              "D4 an interchange that did not build has no open ramp mouths");
    }

    // ---- D2 the city survey is a MEASUREMENT, not a constant ---------------
    {
        clearTerrainCorridors();
        // The canon `city` region: anchor (-200, 425), radius 750,
        // unloadRadius 600 (assets/world/regions.canon.json) => resident out to
        // 1350 m from the anchor. The facility keep-out is the canonical tower
        // footprint + its apron ring (app/facility_exterior.h's authored
        // facade x[-3..47] z[-34.5..55.5], apron ~24 m).
        CityFreewaySurveyInput in;
        in.regionAnchorX = -200.0f; in.regionAnchorZ = 425.0f;
        in.regionReachM  = 1350.0f;
        in.haveKeepOut = true;
        // The facility keep-out is its MEASURED facade footprint
        // (x[-4.5..47.0] z[-34.5..55.5]) grown by the 150 m soil skirt the
        // terrain streamer keeps tiles out of — a road inside that rect would
        // have no ground under it at all.
        in.keepOutX0 = -154.5f; in.keepOutZ0 = -184.5f;
        in.keepOutX1 =  197.0f; in.keepOutZ1 =  205.5f;
        in.haveReachFrom = true; in.reachFromX = 22.0f; in.reachFromZ = 10.5f;
        CityFreewaySurvey real = surveyCityFreeway(in);
        check(real.candidatesTried >= kFreewayTunnelCount,
              "D2 the survey walks every authored city alignment (" +
              std::to_string(real.candidatesTried) + " distinct lines)");
        // The verdict MOVES with the inputs. A residency reach of 1 m can admit
        // nothing anywhere, so the survey must fail and must blame residency.
        CityFreewaySurveyInput starvedIn = in;
        starvedIn.regionReachM = 1.0f;
        CityFreewaySurvey starved = surveyCityFreeway(starvedIn);
        check(!starved.ok && starved.rejectedResidency > 0 &&
              starved.nearestMissLenM < real.nearestMissLenM + 1.0f,
              "D2 the verdict is derived from the inputs (a 1 m residency reach "
              "rejects on residency and shortens every run)");
        // And it names a REAL alignment rather than an empty string.
        bool named = false;
        for (uint32_t i = 0; i < kFreewayTunnelCount && !named; ++i)
            named = std::strcmp(real.nearestMissName,
                                cityFreewayTunnelPlan(i).name) == 0;
        for (uint32_t i = 0; i < cityConnectorCount() && !named; ++i)
            named = std::strcmp(real.nearestMissName, cityConnector(i).name) == 0;
        check(named, std::string("D2 the survey's verdict names a real authored "
              "alignment ('") + real.nearestMissName + "')");
        // Whatever the verdict, say it out loud in the test log so a reader of
        // the gate table learns WHERE the canon world's freeway is.
        if (real.ok) {
            char b[256];
            std::snprintf(b, sizeof(b),
                          "[drivelayer-test] canon city freeway: '%s' (%.0f, %.0f)->(%.0f, %.0f) "
                          "%.0f m", real.alignmentName, real.x0, real.z0, real.x1, real.z1,
                          real.lengthM);
            x3::logInfo(b);
        } else {
            x3::logInfo(std::string("[drivelayer-test] canon city freeway: NONE — ") +
                        real.whyNot);
        }
    }

    // ---- D3 traffic is REGION-SHAPED: build -> live -> evict -> zero -------
    // The canon host builds traffic inside the `city` region realize and tears
    // it down in the teardown hook. Traffic cars are direct draws + Jolt
    // kinematic bodies, NOT Scene entities, so the region's ownership ledger
    // cannot capture them: the round trip below IS the region ownership, and
    // if it leaks, the freeway keeps 300 cars alive inside the facility.
    {
        clearTerrainCorridors();
        RoadSpec ringSpec = makeInnerCourse();
        std::vector<float> ringRoadY;
        const RoadBuildResult rr = registerRoad(ringSpec, &ringRoadY);

        std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
        phys->init();

        TrafficConfig cfg;
        cfg.envOverrides = false;    // a gate that moves with a shell export is not a gate
        cfg.targetCount  = 40;
        TrafficLayer layer;
        const bool built = layer.build("--test-drivelayer", rr.ok, ringSpec, ringRoadY,
                                       /*device=*/nullptr, phys.get(), /*audio=*/nullptr,
                                       "", cfg, /*installContactHook=*/true);
        // Drive it a little so cars actually spawn around the focus.
        float focus[3] = { ringSpec.x[8], 0.0f, ringSpec.z[8] };
        focus[1] = ringRoadY[8] + 1.5f;
        for (int i = 0; i < 120; ++i) layer.traffic().update(1.0f / 60.0f, focus, phys.get());
        const uint32_t live = layer.liveCount();
        uint32_t withBodies = 0;
        for (uint32_t i = 0; i < live; ++i)
            if (layer.traffic().carHasBody(i)) ++withBodies;
        // Cars MOVED, not just existed: a sim that never ran would leave every
        // car at its spawn station.
        bool moved = false;
        for (uint32_t i = 0; i < live && !moved; ++i)
            if (layer.traffic().carState(i).v > 1.0f) moved = true;
        check(built && live > 0 && withBodies == live && moved,
              "D3 traffic builds and DRIVES on a real freeway (" +
              std::to_string(live) + " cars, " + std::to_string(withBodies) +
              " with physics bodies, moving)");

        layer.shutdown(phys.get());
        check(layer.liveCount() == 0 && !layer.built(),
              "D3 eviction returns the car count to ZERO (" +
              std::to_string(layer.liveCount()) + ")");
        layer.shutdown(phys.get());     // a region can evict twice
        check(layer.liveCount() == 0,
              "D3 a second eviction is a no-op (regions evict more than once)");
        phys->shutdown();
        clearTerrainCorridors();
    }

    // ---- D5 THE CANON WORLD's OWN FREEWAY carries traffic, and EVICTS ------
    // D3 proved the round trip on the tunnel world's inner tour. This proves it
    // on the spec the CANON host actually builds — the one surveyCityFreeway()
    // sites against the city's authored alignments — because that is the spec
    // the owner will be driving on, and a dual-carriageway gate that passes on
    // a 16-mile closed ring says nothing about a 900 m open run.
    {
        clearTerrainCorridors();
        CityFreewaySurveyInput cin;
        cin.regionAnchorX = -200.0f; cin.regionAnchorZ = 425.0f;
        cin.regionReachM  = 1350.0f;
        cin.haveKeepOut = true;
        cin.keepOutX0 = -154.5f; cin.keepOutZ0 = -184.5f;   // facade + 150 m soil skirt
        cin.keepOutX1 =  197.0f; cin.keepOutZ1 =  205.5f;
        cin.haveReachFrom = true; cin.reachFromX = 22.0f; cin.reachFromZ = 10.5f;
        CityFreewaySurvey cs = surveyCityFreeway(cin);
        if (!cs.ok) {
            check(false, "D5 the canon city freeway survey found a site "
                         "(nothing to carry traffic on)");
        } else {
            std::vector<float> roadY;
            const RoadBuildResult rb = registerRoad(cs.spec, &roadY);
            std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
            phys->init();
            TrafficConfig cfg;
            cfg.envOverrides = false;
            cfg.targetCount  = 60;
            TrafficLayer layer;
            // installContactHook=false — exactly how the canon host builds it
            // (its BarrelSystem owns the world's one contact callback), so the
            // gate exercises the path that actually ships.
            const bool built = layer.build("--test-drivelayer(canon)", rb.ok,
                                           cs.spec, roadY, nullptr, phys.get(),
                                           nullptr, "", cfg,
                                           /*installContactHook=*/false);
            float focus[3] = { cs.spec.x[8], roadY[8] + 1.5f, cs.spec.z[8] };
            for (int i = 0; i < 180; ++i)
                layer.traffic().update(1.0f / 60.0f, focus, phys.get());
            const uint32_t live = layer.liveCount();
            bool moved = false;
            float maxS = 0.0f;
            for (uint32_t i = 0; i < live; ++i) {
                const auto st = layer.traffic().carState(i);
                if (st.v > 1.0f) moved = true;
                maxS = std::max(maxS, st.s);
            }
            check(rb.ok && built && live > 0 && moved && maxS > 5.0f,
                  "D5 traffic DRIVES on the canon city freeway (" +
                  std::to_string(live) + " cars on " + std::to_string((int)cs.lengthM) +
                  " m of '" + cs.alignmentName + "')");
            // THE INTERCHANGE VERDICT on the canon spec — the same call the
            // canon host makes at boot. Either it builds (and every ramp mouth
            // is open), or the refusal is MEASURED: the one site test this
            // terrain fails is the OPEN MEDIAN (interchange.cpp wants >= 6 m
            // half at the crossing for the pier; computeMedianPlan only opens
            // it where the graded datum stays within 2.5 m of the ground). A
            // refusal that cannot be stated in the freeway's own numbers is
            // not an honest refusal.
            {
                const InterchangeResult cic =
                    standUpInterchange("--test-drivelayer(canon)", rb.ok, cs.spec, roadY,
                                       nullptr);
                const std::vector<float> mp = computeMedianPlan(cs.spec, roadY);
                float mmax = 0.0f;
                for (float m : mp) mmax = std::max(mmax, m);
                const bool honest = cic.built
                    ? interchangeRampMouthsOpen(cic)
                    : (cic.whyNot[0] != '\0' && !mp.empty() && mmax < 6.0f);
                char ib[256];
                std::snprintf(ib, sizeof(ib),
                              "D5 the canon freeway's interchange verdict is measured: %s "
                              "(median opens to %.1f m half at best; the pier wants 6.0 m)",
                              cic.built ? "BUILT, every ramp mouth open"
                                        : "NOT built, median too narrow for the pier",
                              mmax);
                check(honest, ib);
            }
            // THE EVICTION PROOF the canon host relies on: the city region's
            // teardown hook calls exactly this, and if the count does not come
            // back to zero the freeway keeps simulating cars while the player
            // is inside the facility.
            layer.shutdown(phys.get());
            check(layer.liveCount() == 0 && !layer.built(),
                  "D5 the city region's eviction leaves ZERO traffic cars");
            phys->shutdown();
        }
        clearTerrainCorridors();
    }

    // ---- D6 THE INTERCHANGE WRAPPERS, POSITIVELY CONTROLLED ---------------
    // standUpInterchange + interchangeRampMouthsOpen are the two pieces
    // host_tunnel now depends on. D1/D4 proved the refusal path; this proves
    // the SUCCESS path on the one freeway in the tree that can carry a grade
    // split, so "the extraction still builds an interchange" is measured
    // rather than assumed.
    {
        clearTerrainCorridors();
        RoadSpec ring = makeInnerCourse();
        std::vector<float> ringY;
        const RoadBuildResult rr = registerRoad(ring, &ringY);
        InterchangeResult ic = standUpInterchange("--test-drivelayer", rr.ok,
                                                  ring, ringY, nullptr);
        check(ic.built && ic.clearanceM >= kOverpassClearM,
              "D6 standUpInterchange sites a real interchange on a real freeway "
              "(clearance " + std::to_string(ic.clearanceM) + " m)");
        check(interchangeRampMouthsOpen(ic),
              "D6 every built ramp mouth is OPEN (a junction is noted on both roads)");
        clearTerrainCorridors();
    }

    x3::logInfo("drive layer: " + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
