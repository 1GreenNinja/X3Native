// driftgrip_test — headless self-test for the drift + surface-traction layer
// (--test-driftgrip). See driftgrip.h for the model.  [LANE: inspx/veh-cosmetics]
//
// Negative-controlled per the lane doctrine: D3 removes the anti-spin ceiling
// and asserts the car genuinely SPINS (proving the D2 recovery criteria can
// fail), and D6/D7 carry a bald-tire control that fails the climb.
//
// CLEAN-ROOM: engine interfaces + this repo's own test patterns only.

#include "driftgrip.h"
#include "vehicle.h"
#include "mesh_prims.h"
#include "tunnel_corridor.h"
#include "terrain.h"
#include "scene.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kDt = 1.0f / 60.0f;
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Rig {
    std::unique_ptr<x3::phys::IPhysicsWorld> phys;
    DriveDemo car;
    bool ok = false;

    // Flat slab world (top at y=0), car at origin facing -Z.
    explicit Rig(float slabHalf = 500.0f) {
        phys.reset(x3::phys::createPhysicsWorld());
        if (!phys->init()) return;
        x3::prims::PrimMesh g = x3::prims::makeBox(slabHalf * 2.0f, 1.0f, slabHalf * 2.0f,
                                                   0.0f, -1.0f, 0.0f, 0.02f);
        phys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                            g.cindex.data(), (uint32_t)g.cindex.size());
        ok = car.buildPhysics(*phys, 0.0f, 1.2f, 0.0f);
        phys->optimizeBroadphase();
    }
    ~Rig() { car.shutdown(); if (phys) phys->shutdown(); }

    void tick(const x3::phys::VehicleInput& in) {
        car.setInput(in);
        car.preStep(kDt);
        phys->step(kDt);
        car.postStep(kDt);
    }
    void settle(int n = 90) { x3::phys::VehicleInput idle{}; for (int i = 0; i < n; ++i) tick(idle); }
    // Full throttle until forward speed >= v (bounded).
    void accelTo(float v, int maxTicks = 1200) {
        x3::phys::VehicleInput in{}; in.throttle = 1.0f;
        for (int i = 0; i < maxTicks && car.forwardSpeed() < v; ++i) tick(in);
    }
};

// The scripted drift maneuver (throttle+steer+handbrake kick, hold, release).
// Returns peak |slip| over the maneuver, mean |slip| over the final 60 ticks,
// mean |slip| over the release phase, ticks-to-recover (|slip| < exitDeg
// sustained 30 ticks after release start; -1 = never), and final speed.
struct DriftRun {
    float peakSlip = 0.0f;
    float finalMeanSlip = 0.0f;
    float releaseMeanSlip = 0.0f;
    int   recoverTicks = -1;
    float finalSpeed = 0.0f;
};
DriftRun runDriftScript(Rig& r, float exitDeg) {
    DriftRun out;
    r.settle();
    r.accelTo(22.0f);
    auto& grip = r.car.driftGrip();
    std::vector<float> slips;
    auto phase = [&](int ticks, float th, float st, float hb) {
        for (int i = 0; i < ticks; ++i) {
            x3::phys::VehicleInput in{};
            in.throttle = th; in.steer = st; in.handBrake = hb;
            r.tick(in);
            slips.push_back(std::fabs(grip.slipAngleDeg()));
            out.peakSlip = std::max(out.peakSlip, slips.back());
        }
    };
    phase(30, 1.0f, 1.0f, 1.0f);      // the kick
    phase(90, 0.65f, 0.35f, 0.0f);    // hold the slide on throttle
    const size_t releaseStart = slips.size();
    phase(210, 0.25f, 0.0f, 0.0f);    // release: it must come back
    // Recovery: |slip| < exitDeg sustained 30 ticks.
    int streak = 0;
    for (size_t i = releaseStart; i < slips.size(); ++i) {
        streak = (slips[i] < exitDeg) ? streak + 1 : 0;
        if (streak >= 30 && out.recoverTicks < 0)
            out.recoverTicks = (int)(i - releaseStart) - 29;
    }
    float relSum = 0.0f;
    for (size_t i = releaseStart; i < slips.size(); ++i) relSum += slips[i];
    out.releaseMeanSlip = relSum / (float)std::max<size_t>(1, slips.size() - releaseStart);
    float finSum = 0.0f;
    const size_t finN = std::min<size_t>(60, slips.size());
    for (size_t i = slips.size() - finN; i < slips.size(); ++i) finSum += slips[i];
    out.finalMeanSlip = finSum / (float)std::max<size_t>(1, finN);
    out.finalSpeed = r.car.forwardSpeed();
    return out;
}

bool recoveryCriteria(const DriftRun& d, float entryDeg) {
    return d.peakSlip >= entryDeg + 4.0f &&   // it genuinely oversteered
           d.peakSlip <= 58.0f &&             // ...boundedly (no end-swap)
           d.finalMeanSlip < 6.0f &&          // ...and came back straight
           d.recoverTicks >= 0 &&
           d.finalSpeed > 4.0f;               // still driving, not parked/spun-down
}

char logBuf[256];

} // namespace

bool runDriftGripSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++passN; x3::logInfo(std::string("[driftgrip] PASS ") + name); }
        else    { ++failN; x3::logError(std::string("[driftgrip] FAIL ") + name); }
    };

    // ---- D1: OFF is inert / armed-but-idle is inert -------------------------
    // Layer never enabled vs layer enabled (drift + surface, Road everywhere)
    // but never provoked: trajectories must be IDENTICAL — the feature has no
    // side effects until it actually engages.
    {
        Rig a, b;
        if (!a.ok || !b.ok) { check(false, "D1 rig build"); return false; }
        b.car.driftGrip().setDriftEnabled(true);
        b.car.driftGrip().setSurfaceEnabled(true);
        b.car.driftGrip().setSurfaceQuery([](float, float) { return DriveSurface::Road; });
        bool same = true;
        auto script = [&](Rig& r, float outPos[3]) {
            r.settle();
            x3::phys::VehicleInput in{};
            in.throttle = 1.0f;
            for (int i = 0; i < 360; ++i) r.tick(in);
            in.throttle = 0.5f; in.steer = 0.12f;      // gentle: below drift entry
            for (int i = 0; i < 240; ++i) r.tick(in);
            r.car.chassisPos(outPos);
        };
        float pa[3], pb[3];
        script(a, pa); script(b, pb);
        same = pa[0] == pb[0] && pa[1] == pb[1] && pa[2] == pb[2];
        std::snprintf(logBuf, sizeof(logBuf),
                      "[driftgrip] D1 endpos off=(%.4f,%.4f,%.4f) armed=(%.4f,%.4f,%.4f)",
                      pa[0], pa[1], pa[2], pb[0], pb[1], pb[2]);
        x3::logInfo(logBuf);
        check(same, "D1 armed-but-idle layer is trajectory-identical to off");
    }

    // ---- D2: drift entry + bounded slide + recovery -------------------------
    DriftParams defP{};
    DriftRun on, off;
    {
        Rig r; if (!r.ok) { check(false, "D2 rig build"); return false; }
        r.car.driftGrip().setDriftEnabled(true);
        r.car.driftGrip().setParams(defP);
        on = runDriftScript(r, defP.exitSlipDeg);
    }
    {
        Rig r; if (!r.ok) { check(false, "D2 rig build (off)"); return false; }
        off = runDriftScript(r, defP.exitSlipDeg);   // layer off: today's handling
    }
    std::snprintf(logBuf, sizeof(logBuf),
                  "[driftgrip] D2 ON  peak=%.1f deg finalMean=%.2f recover=%d ticks v=%.1f",
                  on.peakSlip, on.finalMeanSlip, on.recoverTicks, on.finalSpeed);
    x3::logInfo(logBuf);
    std::snprintf(logBuf, sizeof(logBuf),
                  "[driftgrip] D2 OFF peak=%.1f deg finalMean=%.2f recover=%d ticks v=%.1f",
                  off.peakSlip, off.finalMeanSlip, off.recoverTicks, off.finalSpeed);
    x3::logInfo(logBuf);
    check(recoveryCriteria(on, defP.entrySlipDeg),
          "D2 drift: real slip angle, bounded, recovers straight");
    check(on.peakSlip > off.peakSlip + 3.0f,
          "D2 drift layer visibly changes the behaviour vs off");

    // ---- D3: NEGATIVE CONTROL — anti-spin removed => it spins ---------------
    {
        Rig r; if (!r.ok) { check(false, "D3 rig build"); return false; }
        DriftParams bad = defP;
        bad.stabSlipDeg = 1e6f;        // ceiling removed
        bad.rearLatRetain = 0.34f;     // and the rear nearly gone
        bad.counterGain = 0.0f;        // no assist to save it
        r.car.driftGrip().setDriftEnabled(true);
        r.car.driftGrip().setParams(bad);
        DriftRun spin = runDriftScript(r, defP.exitSlipDeg);
        std::snprintf(logBuf, sizeof(logBuf),
                      "[driftgrip] D3 SPIN peak=%.1f deg finalMean=%.2f recover=%d",
                      spin.peakSlip, spin.finalMeanSlip, spin.recoverTicks);
        x3::logInfo(logBuf);
        check(!recoveryCriteria(spin, defP.entrySlipDeg),
              "D3 negative control: without the stabilizer the criteria FAIL (spin)");
    }

    // ---- D4: countersteer assist straightens the release phase --------------
    {
        Rig ra, rn;
        if (!ra.ok || !rn.ok) { check(false, "D4 rig build"); return false; }
        ra.car.driftGrip().setDriftEnabled(true);
        ra.car.driftGrip().setParams(defP);
        DriftParams noAssist = defP; noAssist.counterGain = 0.0f;
        rn.car.driftGrip().setDriftEnabled(true);
        rn.car.driftGrip().setParams(noAssist);
        DriftRun withA = runDriftScript(ra, defP.exitSlipDeg);
        DriftRun noA   = runDriftScript(rn, defP.exitSlipDeg);
        std::snprintf(logBuf, sizeof(logBuf),
                      "[driftgrip] D4 releaseMeanSlip assist=%.2f noassist=%.2f (deg)",
                      withA.releaseMeanSlip, noA.releaseMeanSlip);
        x3::logInfo(logBuf);
        check(withA.releaseMeanSlip < noA.releaseMeanSlip,
              "D4 countersteer assist lowers slip through the recovery");
    }

    // ---- D5: dirt brakes longer + corners wider than asphalt ----------------
    {
        auto brakeDist = [&](DriveSurface s) -> float {
            Rig r; if (!r.ok) return -1.0f;
            r.car.driftGrip().setSurfaceEnabled(true);
            r.car.driftGrip().setSurfaceQuery([s](float, float) { return s; });
            r.settle();
            r.accelTo(20.0f);
            float p0[3]; r.car.chassisPos(p0);
            x3::phys::VehicleInput in{}; in.brake = 1.0f;
            int guard = 0;
            while (r.car.forwardSpeed() > 1.0f && guard++ < 900) r.tick(in);
            float p1[3]; r.car.chassisPos(p1);
            const float dx = p1[0]-p0[0], dz = p1[2]-p0[2];
            return std::sqrt(dx*dx + dz*dz);
        };
        const float dRoad = brakeDist(DriveSurface::Road);
        const float dDirt = brakeDist(DriveSurface::Dirt);
        std::snprintf(logBuf, sizeof(logBuf),
                      "[driftgrip] D5 braking 20->1 m/s: road=%.1f m dirt=%.1f m", dRoad, dDirt);
        x3::logInfo(logBuf);
        check(dRoad > 0.0f && dDirt > dRoad * 1.04f, "D5 dirt braking distance longer");

        auto yawGain = [&](DriveSurface s) -> float {
            Rig r; if (!r.ok) return 0.0f;
            r.car.driftGrip().setSurfaceEnabled(true);
            r.car.driftGrip().setSurfaceQuery([s](float, float) { return s; });
            r.settle();
            r.accelTo(15.0f);
            float q0[4]; r.phys->getBodyRotation(r.car.chassis(), q0);
            x3::phys::VehicleInput in{}; in.throttle = 0.35f; in.steer = 0.5f;
            for (int i = 0; i < 240; ++i) r.tick(in);
            float q1[4]; r.phys->getBodyRotation(r.car.chassis(), q1);
            const float y0 = std::atan2(2.0f*(q0[3]*q0[1] + q0[0]*q0[2]),
                                        1.0f - 2.0f*(q0[1]*q0[1] + q0[0]*q0[0]));
            const float y1 = std::atan2(2.0f*(q1[3]*q1[1] + q1[0]*q1[2]),
                                        1.0f - 2.0f*(q1[1]*q1[1] + q1[0]*q1[0]));
            float d = y1 - y0;
            while (d >  3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            return std::fabs(d);
        };
        const float yRoad = yawGain(DriveSurface::Road);
        const float yDirt = yawGain(DriveSurface::Dirt);
        std::snprintf(logBuf, sizeof(logBuf),
                      "[driftgrip] D5 4 s steady steer heading change: road=%.2f rad dirt=%.2f rad",
                      yRoad, yDirt);
        x3::logInfo(logBuf);
        check(yRoad > yDirt * 1.05f, "D5 dirt corners wider (less heading authority)");
    }

    // ---- D6: dirt SLOPE climb (flatten = a spinning wheel still pushes) -----
    {
        auto climb = [&](float externalGrip) -> float {
            // 14-degree ramp, 120 m long, top surface passing through origin.
            std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
            if (!phys->init()) return -1.0f;
            const float ang = 20.0f * 0.0174533f;
            const float ca = std::cos(ang), sa = std::sin(ang);
            std::vector<float> cv; std::vector<uint32_t> ci;
            // Ramp rises toward -Z. Local grid (u right, v up-slope).
            const float hw = 15.0f, len = 120.0f;
            for (int j = 0; j <= 1; ++j)
                for (int i = 0; i <= 1; ++i) {
                    const float u = (i ? hw : -hw);
                    const float v = j * len;
                    cv.push_back(u);
                    cv.push_back(v * sa);
                    cv.push_back(-v * ca);
                }
            // Two triangles, wound so the normal points up (+Y-ish).
            ci = { 0, 1, 2,  2, 1, 3 };
            phys->addStaticMesh(cv.data(), 4, ci.data(), 6);
            // Flat apron behind the ramp foot so the car starts level.
            x3::prims::PrimMesh g = x3::prims::makeBox(60.0f, 1.0f, 30.0f, 0.0f, -1.0f, 20.0f, 0.02f);
            phys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                                g.cindex.data(), (uint32_t)g.cindex.size());
            DriveDemo car;
            if (!car.buildPhysics(*phys, 0.0f, 1.0f, 12.0f)) return -1.0f;
            phys->optimizeBroadphase();
            car.driftGrip().setSurfaceEnabled(true);
            car.driftGrip().setSurfaceQuery([](float, float) { return DriveSurface::Dirt; });
            car.driftGrip().setExternalGripScale(externalGrip);
            x3::phys::VehicleInput idle{};
            for (int i = 0; i < 90; ++i) { car.setInput(idle); car.preStep(kDt); phys->step(kDt); car.postStep(kDt); }
            float p0[3]; car.chassisPos(p0);
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            for (int i = 0; i < 480; ++i) { car.setInput(in); car.preStep(kDt); phys->step(kDt); car.postStep(kDt); }
            float p1[3]; car.chassisPos(p1);
            car.shutdown();
            const float d = -(p1[2] - p0[2]);   // progress up-slope (-Z)
            phys->shutdown();
            return d;
        };
        const float dGood = climb(1.0f);
        const float dBald = climb(0.22f);
        std::snprintf(logBuf, sizeof(logBuf),
                      "[driftgrip] D6 20-deg dirt ramp, 8 s throttle: grip=%.1f m bald=%.1f m",
                      dGood, dBald);
        x3::logInfo(logBuf);
        check(dGood > 15.0f, "D6 dirt tires climb the 20-deg ramp");
        check(dBald < dGood * 0.5f, "D6 negative control: bald tires fail the climb");
    }

    // ---- D7: THE DITCH — regain the corridor roadway under throttle ---------
    // The 2026-08-13 field bug: drove off the road into the corridor's ditch,
    // could not drive back out. Reproduced against the REAL corridor: the
    // registered tunnel route's terrain field + the built road slab/walls.
    {
        const TunnelRoute& route = registerTunnelCorridor();
        // SURVEY THE DITCH first: at every open station, how far below the
        // slab top (roadY + 0.14 proud) does the trench floor sit on each
        // shoulder (lat +-7.4)? The chassis box has ~0.43 m of clearance, so
        // any face taller than that is a geometry wall no tire can climb —
        // that number is the defect report for the corridor lane. The climb
        // acceptance then runs at the SHALLOWEST face (what traction can
        // honestly fix), not a cherry-picked deep cut.
        float spotS = -1.0f, spotSide = 1.0f, faceMin = 1e9f, faceMax = -1e9f;
        float faceSum = 0.0f; int faceN = 0;
        for (const TunnelStation& st : route.st) {
            if (st.bore) continue;
            if (st.s < 15.0f || st.s > route.boreS0 - 15.0f) continue;
            for (float side = -1.0f; side <= 1.0f; side += 2.0f) {
                float wx, wz; route.worldAt(st.s, side * 7.4f, wx, wz);
                const float face = (route.roadYAt(st.s) + 0.14f) - terrainHeightAtWorld(wx, wz);
                faceSum += face; ++faceN;
                if (face > faceMax) faceMax = face;
                if (face < faceMin) { faceMin = face; spotS = st.s; spotSide = side; }
            }
        }
        std::snprintf(logBuf, sizeof(logBuf),
                      "[driftgrip] D7 ditch survey: slab-top-to-floor face min=%.2f mean=%.2f max=%.2f m "
                      "over %d shoulder samples (chassis clearance ~0.43 m)",
                      faceMin, faceN ? faceSum / faceN : 0.0f, faceMax, faceN);
        x3::logInfo(logBuf);
        check(spotS >= 0.0f, "D7 found an open-cutting station on the route");

        if (spotS >= 0.0f) {
            const float side = spotSide;

            auto ditchRun = [&](bool surfaceOn, float external, float outTrace[4]) -> bool {
                std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
                if (!phys->init()) return false;
                // Terrain collision patch around the spot (the streamer's job,
                // done directly from the same canonical height field).
                {
                    std::vector<float> cv; std::vector<uint32_t> ci;
                    const float s0 = std::max(0.0f, spotS - 40.0f);
                    const float s1 = std::min(route.totalLen, spotS + 70.0f);
                    const float step = 1.5f;
                    const int ns = (int)((s1 - s0) / step) + 1;
                    const int nl = (int)(52.0f / step) + 1;
                    for (int j = 0; j < ns; ++j)
                        for (int i = 0; i < nl; ++i) {
                            const float s = s0 + j * step;
                            const float lat = -26.0f + i * step;
                            float wx, wz; route.worldAt(s, lat, wx, wz);
                            cv.push_back(wx);
                            cv.push_back(terrainHeightAtWorld(wx, wz));
                            cv.push_back(wz);
                        }
                    for (int j = 0; j + 1 < ns; ++j)
                        for (int i = 0; i + 1 < nl; ++i) {
                            const uint32_t a = j * nl + i, b = a + 1, c = a + nl, d = c + 1;
                            ci.insert(ci.end(), { a, b, c,  c, b, d });
                        }
                    phys->addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                                        ci.data(), (uint32_t)ci.size());
                }
                // The real road slab / retaining walls / shell (headless build).
                Scene scene;
                HeadlessRenderDevice dev;
                TunnelCorridorWorld world;
                world.build(scene, dev, *phys, route);

                DriveDemo car;
                float sx, sz; route.worldAt(spotS, side * 7.4f, sx, sz);
                const float sy = terrainHeightAtWorld(sx, sz) + 0.9f;
                if (!car.buildPhysics(*phys, sx, sy, sz)) return false;
                phys->optimizeBroadphase();
                // Aim diagonally back at the roadway ahead.
                float tx, tz; route.worldAt(spotS + 18.0f, 0.0f, tx, tz);
                float dx = tx - sx, dz = tz - sz;
                const float dl = std::sqrt(dx*dx + dz*dz);
                dx /= dl; dz /= dl;
                { // face it
                    const float yaw = std::atan2(-dx, -dz);
                    const float q[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
                    phys->setBodyRotation(car.chassis(), q);
                }
                if (surfaceOn) {
                    car.driftGrip().setSurfaceEnabled(true);
                    car.driftGrip().setSurfaceQuery([&route](float x, float z) {
                        const float rx = x - route.ox, rz = z - route.oz;
                        const float s = rx * route.dirX + rz * route.dirZ;
                        if (s < 0.0f || s > route.totalLen) return DriveSurface::Grass;
                        const float lat = std::fabs(-rx * route.dirZ + rz * route.dirX);
                        if (lat <= kTcRoadHalfWidth)  return DriveSurface::Road;
                        if (lat <= kTcCorridorHalfW)  return DriveSurface::Dirt;
                        if (lat <= kTcCorridorHalfW + kTcCorridorFall) return DriveSurface::Dirt;
                        return DriveSurface::Grass;
                    });
                    car.driftGrip().setExternalGripScale(external);
                }
                x3::phys::VehicleInput idle{};
                for (int i = 0; i < 90; ++i) { car.setInput(idle); car.preStep(kDt); phys->step(kDt); car.postStep(kDt); }
                {
                    char tb[200];
                    std::snprintf(tb, sizeof(tb),
                                  "[driftgrip] D7   spot s=%.0f depth=%.1f side=%+.0f startY=%.1f roadY=%.1f",
                                  spotS, 0.0f, side, sy, route.roadYAt(spotS));
                    x3::logInfo(tb);
                }
                bool climbed = false;
                int onRoadStreak = 0;
                float lastLat = side * 7.4f, lastY = sy;
                for (int i = 0; i < 900 && !climbed; ++i) {
                    x3::phys::VehicleInput in{}; in.throttle = 1.0f;
                    car.setInput(in); car.preStep(kDt); phys->step(kDt); car.postStep(kDt);
                    float p[3]; car.chassisPos(p);
                    const float rx = p[0] - route.ox, rz = p[2] - route.oz;
                    const float s = rx * route.dirX + rz * route.dirZ;
                    const float lat = -rx * route.dirZ + rz * route.dirX;
                    lastLat = lat; lastY = p[1];
                    const float roadY = route.roadYAt(clampf(s, 0.0f, route.totalLen));
                    const bool onRoad = std::fabs(lat) < 4.2f &&
                                        p[1] > roadY + 0.2f && p[1] < roadY + 2.2f;
                    onRoadStreak = onRoad ? onRoadStreak + 1 : 0;
                    if (onRoadStreak >= 30) climbed = true;
                    if (i % 150 == 149) {
                        char tb[200];
                        std::snprintf(tb, sizeof(tb),
                                      "[driftgrip] D7   t=%.1fs lat=%.1f y=%.1f roadY=%.1f v=%.1f",
                                      (i + 1) * kDt, lat, p[1], roadY, car.forwardSpeed());
                        x3::logInfo(tb);
                    }
                }
                outTrace[0] = lastLat; outTrace[1] = lastY;
                outTrace[2] = (float)onRoadStreak; outTrace[3] = climbed ? 1.0f : 0.0f;
                car.shutdown();
                world.shutdown(dev, *phys);
                phys->shutdown();
                return climbed;
            };

            // ROAD-MOUNT acceptance -- only meaningful once the ribbon's
            // shoulder is geometrically mountable. Measured 2026-08-13: the
            // slab skirt is a VERTICAL face, min 0.45 m over the whole route
            // (survey above) vs ~0.43 m chassis clearance, so no grip value
            // can put the car back on the roadway -- that is a corridor-
            // geometry defect (tunnel_corridor.cpp builds the skirt straight
            // down; it needs a ramped shoulder fillet), owned by the tunnels
            // lane and REPORTED, not silently worked around. The moment the
            // skirt is ramped (faceMin <= climbable), this branch arms itself
            // and the acceptance runs for real.
            if (faceMin <= 0.43f) {
                float tr[4];
                const bool base = ditchRun(false, 1.0f, tr);
                std::snprintf(logBuf, sizeof(logBuf),
                              "[driftgrip] D7 BASELINE (no surface layer): climbed=%d lastLat=%.1f lastY=%.1f",
                              base ? 1 : 0, tr[0], tr[1]);
                x3::logInfo(logBuf);
                const bool fixed = ditchRun(true, 1.0f, tr);
                std::snprintf(logBuf, sizeof(logBuf),
                              "[driftgrip] D7 SURFACE ON: climbed=%d lastLat=%.1f lastY=%.1f",
                              fixed ? 1 : 0, tr[0], tr[1]);
                x3::logInfo(logBuf);
                check(fixed, "D7 ditch: car regains the roadway under throttle (surface layer on)");
                const bool bald = ditchRun(true, 0.22f, tr);
                std::snprintf(logBuf, sizeof(logBuf),
                              "[driftgrip] D7 BALD (external 0.22): climbed=%d lastLat=%.1f lastY=%.1f",
                              bald ? 1 : 0, tr[0], tr[1]);
                x3::logInfo(logBuf);
                check(!bald, "D7 negative control: bald tires stay in the ditch");
            } else {
                x3::logWarn("[driftgrip] D7 road-mount acceptance SKIPPED: the ribbon skirt is a "
                            "vertical face taller than the chassis clearance EVERYWHERE on the "
                            "route (geometry defect, tunnels lane) -- grip cannot fix a wall. "
                            "The batter-climb acceptance below is the traction half of the bug.");
            }

            // BATTER CLIMB -- the half traction owns: from the ditch, drive up
            // the dirt cut batter and OUT of the cutting onto natural ground.
            auto batterRun = [&](bool surfaceOn, float external) -> bool {
                // A moderate cutting, not the deepest.
                float bS = -1.0f;
                for (const TunnelStation& st : route.st) {
                    if (st.bore) continue;
                    if (st.s < 20.0f || st.s > route.boreS0 - 20.0f) continue;
                    if (st.depth >= 1.8f && st.depth <= 4.5f) { bS = st.s; break; }
                }
                if (bS < 0.0f) bS = spotS;
                std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
                if (!phys->init()) return false;
                {
                    std::vector<float> cv; std::vector<uint32_t> ci;
                    const float s0 = std::max(0.0f, bS - 40.0f);
                    const float s1 = std::min(route.totalLen, bS + 70.0f);
                    const float step = 1.5f;
                    const int ns = (int)((s1 - s0) / step) + 1;
                    const int nl = (int)(76.0f / step) + 1;
                    for (int j = 0; j < ns; ++j)
                        for (int i = 0; i < nl; ++i) {
                            const float ss = s0 + j * step;
                            const float lat = -38.0f + i * step;
                            float wx, wz; route.worldAt(ss, lat, wx, wz);
                            cv.push_back(wx);
                            cv.push_back(terrainHeightAtWorld(wx, wz));
                            cv.push_back(wz);
                        }
                    for (int j = 0; j + 1 < ns; ++j)
                        for (int i = 0; i + 1 < nl; ++i) {
                            const uint32_t a = j * nl + i, b = a + 1, c = a + nl, d = c + 1;
                            ci.insert(ci.end(), { a, b, c,  c, b, d });
                        }
                    phys->addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                                        ci.data(), (uint32_t)ci.size());
                }
                DriveDemo car;
                // Ditch floor on the uphill (outward-climbable) shoulder.
                float lxA, lzA, lxB, lzB;
                route.worldAt(bS, -16.0f, lxA, lzA);
                route.worldAt(bS, +16.0f, lxB, lzB);
                const float bSide = (tunnelNaturalHeightAt(lxA, lzA) >
                                     tunnelNaturalHeightAt(lxB, lzB)) ? -1.0f : 1.0f;
                float sx, sz; route.worldAt(bS, bSide * 7.6f, sx, sz);
                const float sy = terrainHeightAtWorld(sx, sz) + 0.9f;
                if (!car.buildPhysics(*phys, sx, sy, sz)) return false;
                phys->optimizeBroadphase();
                // Aim OUTWARD and up-route: a shallow diagonal up the batter.
                float txp, tzp; route.worldAt(bS + 26.0f, bSide * 24.0f, txp, tzp);
                float dx = txp - sx, dz = tzp - sz;
                const float dl = std::sqrt(dx*dx + dz*dz); dx /= dl; dz /= dl;
                {
                    const float yaw = std::atan2(-dx, -dz);
                    const float q[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
                    phys->setBodyRotation(car.chassis(), q);
                }
                if (surfaceOn) {
                    car.driftGrip().setSurfaceEnabled(true);
                    car.driftGrip().setSurfaceQuery([&route](float x, float z) {
                        const float rx = x - route.ox, rz = z - route.oz;
                        const float lat = std::fabs(-rx * route.dirZ + rz * route.dirX);
                        if (lat <= kTcRoadHalfWidth) return DriveSurface::Road;
                        if (lat <= kTcCorridorHalfW + kTcCorridorFall) return DriveSurface::Dirt;
                        return DriveSurface::Grass;
                    });
                    car.driftGrip().setExternalGripScale(external);
                }
                x3::phys::VehicleInput idle{};
                for (int i = 0; i < 90; ++i) { car.setInput(idle); car.preStep(kDt); phys->step(kDt); car.postStep(kDt); }
                bool outOfCut = false;
                float peakLat = 0.0f, lastY2 = sy;
                for (int i = 0; i < 900 && !outOfCut; ++i) {
                    x3::phys::VehicleInput in{}; in.throttle = 1.0f;
                    car.setInput(in); car.preStep(kDt); phys->step(kDt); car.postStep(kDt);
                    float p[3]; car.chassisPos(p);
                    const float rx = p[0] - route.ox, rz = p[2] - route.oz;
                    const float lat = std::fabs(-rx * route.dirZ + rz * route.dirX);
                    peakLat = std::max(peakLat, lat); lastY2 = p[1];
                    // Out of the cutting: most of the way up the falloff batter.
                    if (lat > kTcCorridorHalfW + kTcCorridorFall * 0.85f) outOfCut = true;
                }
                std::snprintf(logBuf, sizeof(logBuf),
                              "[driftgrip] D7 batter s=%.0f side=%+.0f surface=%d ext=%.2f: out=%d peakLat=%.1f y=%.1f",
                              bS, bSide, surfaceOn ? 1 : 0, external, outOfCut ? 1 : 0, peakLat, lastY2);
                x3::logInfo(logBuf);
                car.shutdown();
                phys->shutdown();
                return outOfCut;
            };
            const bool batterOk   = batterRun(true, 1.0f);
            const bool batterBald = batterRun(true, 0.22f);
            check(batterOk, "D7 batter climb: the car drives OUT of the cutting on dirt tires");
            check(!batterBald, "D7 negative control: bald tires cannot climb the batter");
        }
    }

    // ---- D8: the parts catalog genuinely changes the drift feel -------------
    {
        vehparts::Catalog cat;
        if (cat.loadFile(vehparts::defaultCatalogPath())) {
            vehparts::VehicleBuild touring, slick;
            touring.install("tires", "tire_touring");
            slick.install("tires", "tire_slick");
            const DriftParams pt = driftParamsFor(cat, touring);
            const DriftParams ps = driftParamsFor(cat, slick);
            const DriftParams pd = driftParamsFor(cat, vehparts::VehicleBuild{});
            std::snprintf(logBuf, sizeof(logBuf),
                          "[driftgrip] D8 entry deg touring=%.1f stock=%.1f slick=%.1f",
                          pt.entrySlipDeg, pd.entrySlipDeg, ps.entrySlipDeg);
            x3::logInfo(logBuf);
            check(pt.entrySlipDeg < pd.entrySlipDeg && pd.entrySlipDeg < ps.entrySlipDeg,
                  "D8 breakaway threshold orders touring < stock < slick");
            check(pt.rearLatRetain < ps.rearLatRetain,
                  "D8 slicks hold a tighter drift than touring");
        } else {
            check(false, "D8 parts catalog loads");
        }
    }

    std::snprintf(logBuf, sizeof(logBuf), "[driftgrip] %d/%d passed", passN, passN + failN);
    if (failN) x3::logError(logBuf); else x3::logInfo(logBuf);
    return failN == 0;
}

} // namespace x3::game
