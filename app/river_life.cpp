// RIVER LIFE — fish + AI speedboats on the Bridge No.1 reach. See river_life.h.
//
// REUSE LEDGER (the point of this module is that it builds almost nothing):
//   fish        -> app/fish.h FishSystem, verbatim (the canon river's system)
//   water/bed   -> x3::game::worldWaterLevelAt / terrainHeightAtWorld
//   reach       -> x3::game::worldRiverNodes (the river's own spline)
//   boats       -> app/vehicle.h BoatDemo (Jolt buoyancy controller)
//   drivers     -> app/monster.h MonsterSystem inert-prop pattern (crowd_skin /
//                  Club 1127 precedent: chaseSpeed 0, damage 0, noBody,
//                  setPropPose fed per frame, update(playerPos=self))
//   wake        -> IRenderDevice::submitParticles (the rain/smoke billboard pass)
//   sound       -> IAudioSystem startLoop3D/setLoopPosition/setLoopParams +
//                  assets/audio/vehicles/outboard_loop.wav (tools/gen_outboard_audio.py)
#include "river_life.h"

#include "monster.h"
#include "terrain.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

float wrapPi(float a) {
    while (a >  kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

// Walk the river polyline from node `start` in direction `step` (+1 downstream
// / -1 upstream), returning the interpolated point `arc` metres along it,
// clamping at the polyline end.
// (The old `refY`/`maxDrop` early-out is GONE: it existed only because the
// rendered plane was FLAT at the bridge's waterY, so a lane could not run to
// where the real river had left that level. Since task #32 the drawn surface
// descends with the channel and every hull is fed its LOCAL level each step —
// a lane may now run as far as the river does.)
void pointAlongReach(const WorldRiverNode* rn, uint32_t n, uint32_t start,
                     int step, float arc, float& outX, float& outZ) {
    float x = rn[start].x, z = rn[start].z;
    uint32_t i = start;
    float left = arc;
    while (left > 0.0f) {
        const int j = (int)i + step;
        if (j < 0 || j >= (int)n) break;
        const float dx = rn[j].x - x, dz = rn[j].z - z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < 1e-3f) { i = (uint32_t)j; continue; }
        if (d >= left) {
            x += dx * (left / d); z += dz * (left / d);
            left = 0.0f;
        } else {
            x = rn[j].x; z = rn[j].z;
            left -= d;
            i = (uint32_t)j;
        }
    }
    outX = x; outZ = z;
}

// ---- COMPOSED-SKIN TRANSFORM (shared by the speedboat and the submarine) ---
// A composed skin draws tinted primitives on the LIVE physics transform. Each
// part is placed in hull-local space (lx,ly,lz), scaled (sx,sy,sz), optionally
// pitched about its local X (a raked bow, or pitch = pi/2 to stand a Y-aligned
// cylinder along the hull's Z axis), then carried by the hull's own rotation:
//   world = T(hullPos) * R(hullQuat) * T(local) * Rx(pitch) * S
void quatToBasis(const float q[4], float R[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0] = 1 - 2*(y*y + z*z); R[1] = 2*(x*y + z*w);     R[2] = 2*(x*z - y*w);
    R[3] = 2*(x*y - z*w);     R[4] = 1 - 2*(x*x + z*z); R[5] = 2*(y*z + x*w);
    R[6] = 2*(x*z + y*w);     R[7] = 2*(y*z - x*w);     R[8] = 1 - 2*(x*x + y*y);
}

void partWorld(const float R[9], const float hp[3],
               float lx, float ly, float lz,
               float sx, float sy, float sz, float pitch, float out[16]) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float bx[3] = { sx, 0.0f, 0.0f };
    const float by[3] = { 0.0f, sy * cp, sy * sp };
    const float bz[3] = { 0.0f, -sz * sp, sz * cp };
    auto rot = [&](const float v[3], float o[3]) {
        o[0] = R[0]*v[0] + R[3]*v[1] + R[6]*v[2];
        o[1] = R[1]*v[0] + R[4]*v[1] + R[7]*v[2];
        o[2] = R[2]*v[0] + R[5]*v[1] + R[8]*v[2];
    };
    float cX[3], cY[3], cZ[3], off[3];
    rot(bx, cX); rot(by, cY); rot(bz, cZ);
    const float l[3] = { lx, ly, lz };
    rot(l, off);
    out[0]=cX[0]; out[1]=cX[1]; out[2]=cX[2];  out[3]=0.0f;
    out[4]=cY[0]; out[5]=cY[1]; out[6]=cY[2];  out[7]=0.0f;
    out[8]=cZ[0]; out[9]=cZ[1]; out[10]=cZ[2]; out[11]=0.0f;
    out[12]=hp[0]+off[0]; out[13]=hp[1]+off[1]; out[14]=hp[2]+off[2]; out[15]=1.0f;
}

// The river surface at (x,z) — the ONE truth the water shader draws — with a
// fallback for the sentinel worldWaterLevelAt returns off the water table (a
// hull nosing past the ribbon edge must not be handed -3e38).
float riverSurfaceAt(float x, float z, float fallback) {
    const float w = worldWaterLevelAt(x, z);
    return (w > kWorldWaterDry + 1.0f) ? w : fallback;
}

} // namespace

RiverLife::RiverLife()  = default;
RiverLife::~RiverLife() = default;

bool RiverLife::build(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& phys, x3::audio::IAudioSystem* audio,
                      const RiverBridgePlan& plan) {
    if (m_built || !plan.ok) return false;
    m_waterY = plan.waterY;

    uint32_t rn = 0;
    const WorldRiverNode* nodes = worldRiverNodes(rn);
    if (!nodes || rn < 4) {
        x3::logWarn("[river-life] no river spline — nothing to populate");
        return false;
    }
    // The crossing's nearest spline node anchors everything.
    uint32_t nearest = 0; float best = 1e30f;
    for (uint32_t i = 0; i < rn; ++i) {
        const float dx = nodes[i].x - plan.cx, dz = nodes[i].z - plan.cz;
        const float d2 = dx * dx + dz * dz;
        if (d2 < best) { best = d2; nearest = i; }
    }

    // ==== DEPTH GATE (owner: "the water is 18 feet deep") ====================
    // Mid-channel depth (waterY - carved bed) at three stations around the
    // crossing. The shelf carve alone gave 3.2 m everywhere; the deep-channel
    // cut (terrain.cpp kWorldRiverMidDrop) must read ~5.5 m (18 ft) here.
    for (int s = -1; s <= 1; ++s) {
        const int idx = std::clamp((int)nearest + s * 2, 0, (int)rn - 1);
        const float depth = nodes[idx].waterY -
                            terrainHeightAtWorld(nodes[idx].x, nodes[idx].z);
        x3::logInfo("[river-life] depth station " + std::to_string(s + 2) + "/3 at (" +
                    std::to_string(nodes[idx].x) + ", " + std::to_string(nodes[idx].z) +
                    "): " + std::to_string(depth) + " m mid-channel (was 3.2 shelf-only)");
    }

    // ==== FISH — the canon species mix, scoped to the bridge reach ==========
    {
        FishConfig fc;                       // roomId stays kNoRoom: this host has no PVS
        fc.activeRadius = 340.0f;            // animate for the bridge/approach camera
        struct Plan { int offs; FishSpecies sp; uint32_t count; float spread; };
        const Plan sp[] = {
            { -2, FishSpecies::Rudd,  12u, 4.2f },   // silver shoal upstream
            { -1, FishSpecies::Perch,  5u, 2.4f },   // a loose gang off the west bank
            {  1, FishSpecies::Bream, 10u, 5.0f },   // the deep slab downstream
            {  2, FishSpecies::Pike,   1u, 2.0f },   // THE pike, alone, in the quiet water
        };
        for (const Plan& p : sp) {
            int idx = (int)nearest + p.offs;
            idx = std::clamp(idx, 0, (int)rn - 2);
            const WorldRiverNode& A = nodes[idx];
            const WorldRiverNode& B = nodes[idx + 1];
            FishSchoolDesc sd;
            sd.centerX = A.x; sd.centerZ = A.z;
            sd.heading = std::atan2(B.z - A.z, B.x - A.x);   // downstream
            sd.species = p.sp;
            sd.count   = p.count;
            sd.spread  = p.spread;
            sd.speed   = fishSpecies(p.sp).speed;
            fc.schools.push_back(sd);
        }
        // The VISIBLE surface IS worldWaterLevelAt (task #32): the drawn plane
        // now steps down the channel from the same node table, so the old
        // clamp-to-the-flat-plane (which would have cruised the upstream shoal
        // BELOW the water it swims in) is deleted. One truth, one query.
        m_fish.setWaterQuery([](float x, float z) { return worldWaterLevelAt(x, z); });
        m_fish.setBedQuery([](float x, float z) { return terrainHeightAtWorld(x, z); });
        m_fish.setModelDir(riggedGlbRoot());
        m_fish.build(fc, scene, device);
        x3::logInfo("[river-life] FISH: " + std::to_string(m_fish.fishCount()) +
                    " fish in " + std::to_string(m_fish.schoolCount()) +
                    " schools on the bridge reach");
    }

    // ==== TWO SPEEDBOATS — patrol lanes, one per side of the bridge =========
    // Sea level = worldWaterLevelAt at the hull, re-fed every prePhysics — the
    // SAME surface the water shader draws (task #32), so a hull sits on the
    // visible water by construction anywhere on the reach, and rises with it
    // in rain. Lanes clear the piers (lane ends start 45 m out).
    if (audio) {
        const std::string wav = assetRoot() + "/audio/vehicles/outboard_loop.wav";
        m_outboardSnd = audio->load(wav);
        if (!m_outboardSnd.valid())
            x3::logWarn("[river-life] outboard_loop.wav missing (" + wav +
                        ") — boats run silent");
    }
    const char* rigs[2]   = { "marcus_webb_anim.glb", "chief_martinez_anim.glb" };
    const float detune[2] = { 0.96f, 1.05f };        // the two engines BEAT
    for (int side = 0; side < 2; ++side) {
        const int step = side == 0 ? -1 : +1;        // 0 = upstream, 1 = downstream
        Boat b;
        // No level bound any more (task #32): the drawn surface descends with
        // the channel and prePhysics feeds each hull its LOCAL level, so the
        // lane simply runs 45..190 m along the reach.
        pointAlongReach(nodes, rn, nearest, step,  45.0f, b.ax, b.az);
        pointAlongReach(nodes, rn, nearest, step, 190.0f, b.bx, b.bz);
        const float laneLen = std::sqrt((b.bx - b.ax) * (b.bx - b.ax) +
                                        (b.bz - b.az) * (b.bz - b.az));
        if (laneLen < 30.0f) {
            x3::logWarn("[river-life] side " + std::to_string(side) +
                        ": lane too short (" + std::to_string(laneLen) + " m) — skipped");
            continue;
        }
        const float sx = (b.ax + b.bx) * 0.5f, sz = (b.az + b.bz) * 0.5f;
        // Spawn ON the river's own surface at mid-lane, not on the bridge's.
        const float sy = worldWaterLevelAt(sx, sz);
        b.ok = b.demo.build(device, phys, sx, sy + 0.35f, sz, sy,
                            /*isSub=*/false);
        if (!b.ok) {
            x3::logWarn("[river-life] boat build failed (side " +
                        std::to_string(side) + ")");
            continue;
        }
        b.target = 1;
        b.pitch  = detune[side];

        // The DRIVER — crowd_skin's inert-prop tuning, verbatim.
        {
            MonsterSystem::Tuning t;
            t.type        = MonsterType::Guard;
            t.chaseSpeed  = 0.0f;      // INERT: the hull owns his motion
            t.damage      = 0;         // never attacks
            t.ranged      = false;
            t.noBody      = true;      // pure visual — no Enemy hitbox
            t.lockRootY   = true;      // hull Y is fed; clips must not bob him
            t.modelFile   = rigs[side];
            t.modelDirOverride = riggedGlbRoot();
            t.standUpZtoY = false;     // roster rigs are Y-up
            t.modelScale  = 1.0f;
            auto sys = std::make_unique<MonsterSystem>();
            float hp[3]; b.demo.hullPos(hp);
            sys->buildMonsterTuned(scene, device, phys, riggedGlbRoot(),
                                   x3::phys::Vec3{ hp[0], hp[1] + 0.6f, hp[2] }, t);
            if (sys->usingRealModel() && sys->skinnable()) {
                b.driver = std::move(sys);
            } else {
                // Fallback: no rig on this box — hide the bookkeeping entity and
                // run the boat driverless (never a T-pose at the helm).
                const uint32_t ent = sys->entity();
                if (ent != kNoLink && ent < scene.size()) scene.get(ent).visible = false;
                x3::logWarn(std::string("[river-life] driver rig '") + rigs[side] +
                            "' unavailable — boat runs driverless");
            }
        }

        if (audio && m_outboardSnd.valid()) {
            float hp[3]; b.demo.hullPos(hp);
            b.loop = audio->startLoop3D(m_outboardSnd, hp[0], hp[1], hp[2],
                                        0.55f, b.pitch);
        }
        m_boats.push_back(std::move(b));
    }

    m_puffs.resize(512);
    for (Puff& p : m_puffs) p.age = p.life = 1.0f;   // spent
    m_foamOut.reserve(m_puffs.size());
    m_sprayOut.reserve(m_puffs.size());

    // The SPEEDBOAT skin's shared art: one unit cube + four solid tints
    // (hull white, trim red, glass smoke, motor black). No boat GLB exists in
    // any mounted pack (checked), so the hull look is composed from parts on
    // the live physics transform — same doctrine as the graybox car, one step
    // dressier.
    m_phys = &phys;
    if (!m_boats.empty()) {
        std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
        x3::prims::makeCube(0.5f, cv, ci);
        m_boatCube = device.createMesh(cv.data(), (uint32_t)cv.size(),
                                       ci.data(), (uint32_t)ci.size());
        auto tHull  = x3::prims::makeSolidRGBA(8, 226, 229, 233);
        auto tTrim  = x3::prims::makeSolidRGBA(8, 158, 28, 36);
        auto tGlass = x3::prims::makeSolidRGBA(8, 26, 34, 44);
        auto tMotor = x3::prims::makeSolidRGBA(8, 22, 24, 26);
        m_texHull  = device.createTexture(tHull.data(), 8, 8, true);
        m_texTrim  = device.createTexture(tTrim.data(), 8, 8, true);
        m_texGlass = device.createTexture(tGlass.data(), 8, 8, true);
        m_texMotor = device.createTexture(tMotor.data(), 8, 8, true);
    }

    // ==== THE PATROL SUB (owner: "a sub or 2") ==============================
    // Deep water only. The lane runs 210..430 m along the spine — well past the
    // speedboats' 45..190 m lanes, so the sub never shares water with a planing
    // hull — on whichever side of the crossing keeps the most water under the
    // keel. A lane that cannot clear kSubMinDepth simply does not get a sub
    // (a submarine dragging its belly is worse than no submarine).
    {
        constexpr float kSubMinDepth = 4.2f;    // hull is 1.2 m tall + clearance
        constexpr float kLaneNear = 210.0f, kLaneFar = 430.0f;
        float bestX0 = 0, bestZ0 = 0, bestX1 = 0, bestZ1 = 0;
        float bestDepth = -1e9f;
        for (int side = 0; side < 2; ++side) {
            const int step = side == 0 ? -1 : +1;
            float x0, z0, x1, z1;
            pointAlongReach(nodes, rn, nearest, step, kLaneNear, x0, z0);
            pointAlongReach(nodes, rn, nearest, step, kLaneFar,  x1, z1);
            const float len = std::sqrt((x1-x0)*(x1-x0) + (z1-z0)*(z1-z0));
            if (len < 40.0f) continue;          // the river ran out that way
            // Shallowest point along the candidate lane decides it.
            float minDepth = 1e9f;
            for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
                const float px = x0 + (x1-x0)*t, pz = z0 + (z1-z0)*t;
                const float wy = worldWaterLevelAt(px, pz);
                if (wy <= kWorldWaterDry + 1.0f) { minDepth = -1e9f; break; }
                minDepth = std::min(minDepth, wy - terrainHeightAtWorld(px, pz));
            }
            if (minDepth > bestDepth) {
                bestDepth = minDepth;
                bestX0 = x0; bestZ0 = z0; bestX1 = x1; bestZ1 = z1;
            }
        }
        if (bestDepth >= kSubMinDepth) {
            m_sub.ax = bestX0; m_sub.az = bestZ0;
            m_sub.bx = bestX1; m_sub.bz = bestZ1;
            const float sx = (bestX0 + bestX1) * 0.5f, sz = (bestZ0 + bestZ1) * 0.5f;
            m_sub.waterY = worldWaterLevelAt(sx, sz);
            // Hold the hull's TOP ~1.3 m under: deep enough to read as
            // SUBMERGED from the bridge deck, shallow enough that the
            // silhouette and the caustic light on its back still carry.
            m_sub.depth  = std::min(1.9f, bestDepth - 2.0f);
            m_sub.ok = m_sub.demo.build(device, phys, sx, m_sub.waterY - m_sub.depth, sz,
                                        m_sub.waterY, /*isSub=*/true);
            if (m_sub.ok) {
                m_sub.target = 1;
                x3::logInfo("[river-life] SUB: patrol lane (" +
                            std::to_string(bestX0) + ", " + std::to_string(bestZ0) +
                            ") -> (" + std::to_string(bestX1) + ", " + std::to_string(bestZ1) +
                            "), shallowest water on the lane " + std::to_string(bestDepth) +
                            " m, holding " + std::to_string(m_sub.depth) + " m down");
            } else {
                x3::logWarn("[river-life] SUB: buoyancy hull build failed");
            }
        } else {
            x3::logWarn("[river-life] SUB: no lane with " +
                        std::to_string(kSubMinDepth) + " m of water (best " +
                        std::to_string(bestDepth) + " m) — no sub");
        }
    }

    // The SUB skin's shared art (see drawSubSkin): a round pressure hull, a
    // near-black wet-steel tint, and one amber lamp on the sail.
    if (m_sub.ok) {
        if (!m_boatCube.valid()) {          // boats may have been skipped
            std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
            x3::prims::makeCube(0.5f, cv, ci);
            m_boatCube = device.createMesh(cv.data(), (uint32_t)cv.size(),
                                           ci.data(), (uint32_t)ci.size());
        }
        // Unit cylinder (r 1, half-height 1) and unit sphere — drawSubSkin's
        // scales are therefore metres.
        x3::prims::PrimMesh tube = x3::prims::makeCylinder(1.0f, 1.0f, 1.0f, 20u);
        m_subTube = device.createMesh(tube.verts.data(), (uint32_t)tube.verts.size(),
                                      tube.index.data(), (uint32_t)tube.index.size());
        x3::prims::PrimMesh ball = x3::prims::makeUVSphere(12u, 20u);
        m_subBall = device.createMesh(ball.verts.data(), (uint32_t)ball.verts.size(),
                                      ball.index.data(), (uint32_t)ball.index.size());
        auto tSub  = x3::prims::makeSolidRGBA(8, 44, 52, 54);   // wet steel, not black
        auto tDark = x3::prims::makeSolidRGBA(8, 26, 31, 33);   // sail / planes
        auto tLamp = x3::prims::makeSolidRGBA(8, 232, 148, 42); // amber running light
        m_texSubHull = device.createTexture(tSub.data(),  8, 8, true);
        m_texSubDark = device.createTexture(tDark.data(), 8, 8, true);
        m_texSubLamp = device.createTexture(tLamp.data(), 8, 8, true);
    }

    m_built = true;
    x3::logInfo("[river-life] built: " + std::to_string(m_boats.size()) +
                " speedboat(s) + " + std::to_string(m_sub.ok ? 1 : 0) +
                " sub on the reach, water Y " + std::to_string(m_waterY));
    return true;
}

void RiverLife::subPos(float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (m_sub.ok) const_cast<BoatDemo&>(m_sub.demo).hullPos(out);
}

float RiverLife::subSubmergence() const {
    if (!m_sub.ok) return 0.0f;
    float hp[3]; const_cast<BoatDemo&>(m_sub.demo).hullPos(hp);
    return m_sub.waterY - (hp[1] + 0.6f);      // surface minus the hull's top
}

void RiverLife::prePhysics(float dt) {
    if (!m_built) return;
    for (Boat& b : m_boats) {
        if (!b.ok || !b.demo.controller()) continue;
        float hp[3]; b.demo.hullPos(hp);
        // ONE WATER TRUTH (task #32): the surface under this hull is the level
        // the shader draws at the hull's XZ — it descends downstream and
        // swells in rain, so it is re-fed EVERY step. (Off the water table the
        // last good level stands; a hull is never left floating on a sentinel.)
        b.waterY = riverSurfaceAt(hp[0], hp[2], b.waterY);
        b.demo.setSeaLevel(b.waterY);
        const float tx = b.target == 0 ? b.ax : b.bx;
        const float tz = b.target == 0 ? b.az : b.bz;
        const float dx = tx - hp[0], dz = tz - hp[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < 12.0f) b.target ^= 1;             // arrive -> run the lane back

        // Heading PD toward the waypoint, on the hull's REAL attitude (heading
        // read off the body quaternion each postPhysics; hull forward is
        // rot*(0,0,-1) — the buoyancy controller's own axis). steer +1 applies
        // torque about -Y, which INCREASES atan2-style heading (RH, +Y up), so
        // positive error wants positive steer; the measured yaw rate damps it.
        const float bearing = std::atan2(dz, dx);
        const float heading = b.haveHeading ? b.heading : bearing;
        const float err = wrapPi(bearing - heading);
        float yawRate = 0.0f;
        if (b.haveHeading && dt > 1e-4f)
            yawRate = wrapPi(b.heading - b.headingPrev) / dt;

        x3::phys::VehicleInput in;
        in.steer = std::clamp(1.5f * err - 0.30f * yawRate, -1.0f, 1.0f);
        // Ease the throttle into the turn-around so the hull carves instead of
        // spinning out; full send down the lane.
        const float ahead = std::max(0.0f, std::cos(err));
        in.throttle = (dist < 26.0f ? 0.45f : 0.85f) * (0.35f + 0.65f * ahead);
        b.throttle = in.throttle;
        b.demo.setInput(in);
        b.demo.preStep(dt);
    }

    // ---- THE SUB: same lane autopilot, plus a depth hold. ------------------
    if (m_sub.ok && m_sub.demo.controller()) {
        Sub& s = m_sub;
        float hp[3]; s.demo.hullPos(hp);
        // The surface over the hull — the ONE truth again, and the reason a
        // depth hold works at all on this river: the level DESCENDS downstream,
        // so "1.9 m down" is a moving target, and against the old flat plane
        // the sub would have dug into the bed at the far end of its lane.
        s.waterY = riverSurfaceAt(hp[0], hp[2], s.waterY);
        s.demo.setSeaLevel(s.waterY);

        const float tx = s.target == 0 ? s.ax : s.bx;
        const float tz = s.target == 0 ? s.az : s.bz;
        const float dx = tx - hp[0], dz = tz - hp[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < 16.0f) s.target ^= 1;

        const float bearing = std::atan2(dz, dx);
        const float heading = s.haveHeading ? s.heading : bearing;
        const float err = wrapPi(bearing - heading);
        float yawRate = 0.0f;
        if (s.haveHeading && dt > 1e-4f)
            yawRate = wrapPi(s.heading - s.headingPrev) / dt;

        x3::phys::VehicleInput in;
        // A submarine turns like a submarine: lazier gains than the speedboats,
        // and a patrol crawl rather than their full send.
        in.steer = std::clamp(0.9f * err - 0.35f * yawRate, -1.0f, 1.0f);
        in.throttle = 0.30f * (0.4f + 0.6f * std::max(0.0f, std::cos(err)));
        // DEPTH HOLD: a PD on depth error driving `dive`. Fully submerged the
        // hull is ~2.1x buoyant (BoatDemo masses it to float half out), so the
        // equilibrium command is a firm, steady push DOWN — the bias term —
        // and the PD only trims around it. Without the bias the controller
        // would fight its way to the surface every time it centred.
        const float wantY = s.waterY - s.depth;
        const float eY    = hp[1] - wantY;                       // + = too high
        const float vY    = m_phys ? m_phys->getBodyVelocity(s.demo.hull()).y : 0.0f;
        in.dive = std::clamp(-0.90f - 0.55f * eY - 0.30f * vY, -1.0f, 0.25f);
        s.demo.setInput(in);
        s.demo.preStep(dt);
    }
}

void RiverLife::postPhysics(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& phys, x3::audio::IAudioSystem* audio,
                            const x3::phys::Vec3& focus) {
    (void)device;
    if (!m_built) return;

    for (Boat& b : m_boats) {
        if (!b.ok) continue;
        b.demo.postStep(dt);

        float hp[3]; b.demo.hullPos(hp);
        float q[4]; phys.getBodyRotation(b.demo.hull(), q);
        // Hull world forward = q * (0,0,-1), flattened.
        float fwd[3], up[3];
        vehcam::hullAxes(q, fwd, up);
        float fx = fwd[0], fz = fwd[2];
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (fl > 1e-4f) { fx /= fl; fz /= fl; }
        b.headingPrev = b.haveHeading ? b.heading : std::atan2(fz, fx);
        b.heading = std::atan2(fz, fx);
        b.haveHeading = true;

        // ---- DRIVER pose-follow: standing at the helm, facing the bow. -----
        if (b.driver) {
            // Feet on the deck (hull top), a step forward of midships.
            const float deckY = hp[1] + 0.6f;
            const float px = hp[0] + fx * 0.55f;
            const float pz = hp[2] + fz * 0.55f;
            // Facing (dirX,dirZ) -> yaw = atan2(-dirX, -dirZ) (monster.cpp's
            // -Z-forward convention).
            const float dyaw = std::atan2(-fx, -fz);
            b.driver->setPropPose(x3::phys::Vec3{ px, deckY, pz }, dyaw);
            b.driver->setPropMotion(0.0f, 0.0f);
            b.driver->update(dt, scene, phys, b.driver->pos());
        }

        // ---- SPEEDBOAT thrust boost. BoatDemo's stock propThrust equals its
        // own linear drag at ~1.4 m/s — a putt-putt. Rather than fork the
        // proven demo hull, shove it from the host side through the same
        // public applyImpulse seam Jake's car-push uses: throttle-scaled,
        // along the flattened hull forward, cut off at planing speed (the
        // drag model above that is untouched, so it still settles honestly).
        const float speed = b.demo.controller()
                          ? std::fabs(b.demo.controller()->forwardSpeed()) : 0.0f;
        if (b.throttle > 0.05f && speed < 10.5f && hp[1] < b.waterY + 0.4f) {
            const float kBoostN = 110000.0f;   // ~10 m/s against the 2.5/s drag
            phys.applyImpulse(b.demo.hull(),
                x3::phys::Vec3{ fx * kBoostN * b.throttle * dt, 0.0f,
                                fz * kBoostN * b.throttle * dt });
        }

        // ---- WAKE: stern foam + bow spray while under way. ------------------
        if (speed > 1.6f) {
            b.wakeAcc += dt * std::min(44.0f, 14.0f + speed * 3.0f);
            while (b.wakeAcc >= 1.0f) {
                b.wakeAcc -= 1.0f;
                m_rng = m_rng * 1664525u + 1013904223u;
                const float r0 = (float)((m_rng >> 8) & 0xFFFF) / 65535.0f - 0.5f;
                m_rng = m_rng * 1664525u + 1013904223u;
                const float r1 = (float)((m_rng >> 8) & 0xFFFF) / 65535.0f - 0.5f;
                // Stern foam: behind the transom, spreading with the wake vee.
                Puff& p = m_puffs[m_puffNext];
                m_puffNext = (m_puffNext + 1) % (uint32_t)m_puffs.size();
                p.x = hp[0] - fx * 3.1f + (-fz) * r0 * 1.2f;
                p.z = hp[2] - fz * 3.1f + ( fx) * r0 * 1.2f;
                p.surfY = b.waterY;       // THIS hull's local river surface
                p.y = p.surfY + 0.15f;    // crest level: foam rides ON the chop,
                                          // not half-drowned in the troughs
                p.vx = -fx * (0.8f + speed * 0.10f) + (-fz) * r0 * 2.2f;
                p.vz = -fz * (0.8f + speed * 0.10f) + ( fx) * r0 * 2.2f;
                p.vy = 0.25f + 0.3f * std::fabs(r1);
                p.age = 0.0f; p.life = 2.2f + 0.6f * std::fabs(r0);
                p.size0 = 0.45f + 0.18f * std::fabs(r1);
                p.spray = false;
                // Bow spray: every other puff, off the chine, additive sparkle.
                if (((m_puffNext) & 1u) == 0u) {
                    Puff& s = m_puffs[m_puffNext];
                    m_puffNext = (m_puffNext + 1) % (uint32_t)m_puffs.size();
                    const float sideSign = (r1 > 0.0f) ? 1.0f : -1.0f;
                    s.x = hp[0] + fx * 2.6f + (-fz) * sideSign * 1.4f;
                    s.z = hp[2] + fz * 2.6f + ( fx) * sideSign * 1.4f;
                    s.surfY = b.waterY;
                    s.y = s.surfY + 0.20f;
                    s.vx = fx * speed * 0.25f + (-fz) * sideSign * (1.5f + r0);
                    s.vz = fz * speed * 0.25f + ( fx) * sideSign * (1.5f + r0);
                    s.vy = 1.6f + std::fabs(r0) * 1.2f;
                    s.age = 0.0f; s.life = 0.65f;
                    s.size0 = 0.16f;
                    s.spray = true;
                }
            }
        }

        // ---- OUTBOARD: emitter rides the hull; pitch/vol follow the load. ---
        if (audio && b.loop.valid()) {
            audio->setLoopPosition(b.loop, hp[0], hp[1], hp[2]);
            const float load = std::min(1.0f, speed / 9.0f);
            audio->setLoopParams(b.loop, 0.4f + 0.5f * load,
                                 b.pitch * (0.88f + 0.30f * load));
        }
    }

    // ---- Advance the wake pool (drag + buoyant settle + expiry). -----------
    for (Puff& p : m_puffs) {
        if (p.age >= p.life) continue;
        p.age += dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        const float drag = std::max(0.0f, 1.0f - 2.2f * dt);
        p.vx *= drag; p.vz *= drag;
        if (p.spray) p.vy -= 6.0f * dt;               // spray falls back
        else {
            p.vy *= drag;                             // foam settles onto the surface
            if (p.y > p.surfY + 0.08f) p.y = std::max(p.surfY + 0.06f, p.y - 0.4f * dt);
        }
    }

    // ---- FISH: the schools live around the focus (camera/player). ----------
    m_fish.update(dt, scene, focus);
}

// One composed speedboat: white planing hull, raked bow, smoked windscreen,
// red gunwale stripes, black outboard — every part a tinted unit cube on the
// hull's live rotation. Local frame: -Z bow, +Z stern (CONVENTIONS §3).
void RiverLife::drawBoatSkin(x3::rhi::IRenderDevice& device,
                             const x3::rhi::FrameContext& frame,
                             const float hp[3], const float q[4]) {
    float R[9]; quatToBasis(q, R);
    const float white[4] = { 1, 1, 1, 1 };
    auto part = [&](float lx, float ly, float lz, float sx, float sy, float sz,
                    float pitch, x3::rhi::TextureHandle tex) {
        float world[16];
        partWorld(R, hp, lx, ly, lz, sx, sy, sz, pitch, world);
        device.drawMesh(frame, m_boatCube, tex, white, world);
    };
    part(0.0f, 0.05f,  0.45f, 2.70f, 1.00f, 4.90f, 0.0f,  m_texHull);   // main hull
    part(0.0f, 0.18f, -2.45f, 2.10f, 0.80f, 2.00f, 0.20f, m_texHull);   // raked bow
    part(-1.32f, 0.48f, 0.30f, 0.14f, 0.22f, 4.60f, 0.0f, m_texTrim);   // port stripe
    part( 1.32f, 0.48f, 0.30f, 0.14f, 0.22f, 4.60f, 0.0f, m_texTrim);   // stbd stripe
    part(0.0f, 0.92f, -0.70f, 1.90f, 0.60f, 0.14f, 0.38f, m_texGlass);  // windscreen
    part(0.0f, 0.70f,  3.10f, 0.50f, 0.90f, 0.55f, 0.0f,  m_texMotor);  // outboard
}

// ---------------------------------------------------------------------------
// THE PATROL SUB's composed skin. Same doctrine as the speedboat above (no
// submarine GLB exists that fits this world — the armory's only one is a rigged
// Victorian steampunk character with walk/shoot clips), one step further: a
// round pressure hull instead of boxes, because a sub read through green river
// water is almost entirely SILHOUETTE and a boxy silhouette reads as a crate.
//
// Hull forward is -Z (CONVENTIONS.md), so the Y-aligned cylinder primitives are
// stood along Z with pitch = pi/2. Overall ~6.6 m long, 1.2 m diameter — a
// MIDGET patrol boat, the only kind that fits an 18-foot river; a fleet sub
// would be aground bow and stern.
// ---------------------------------------------------------------------------
void RiverLife::drawSubSkin(x3::rhi::IRenderDevice& device,
                            const x3::rhi::FrameContext& frame,
                            const float hp[3], const float q[4]) {
    float R[9]; quatToBasis(q, R);
    const float white[4] = { 1, 1, 1, 1 };
    const float kHalfPi = 1.57079633f;
    auto part = [&](x3::rhi::MeshHandle mesh, x3::rhi::TextureHandle tex,
                    float lx, float ly, float lz, float sx, float sy, float sz,
                    float pitch) {
        float world[16];
        partWorld(R, hp, lx, ly, lz, sx, sy, sz, pitch, world);
        device.drawMesh(frame, mesh, tex, white, world);
    };
    // Pressure hull + rounded nose and tail (the cylinder is unit-radius/unit
    // half-height, the sphere unit-radius, so the scales below ARE the metres).
    part(m_subTube, m_texSubHull,  0.0f, 0.0f,  0.0f,  0.58f, 2.90f, 0.58f, kHalfPi);
    part(m_subBall, m_texSubHull,  0.0f, 0.0f, -2.90f, 0.58f, 0.58f, 0.85f, 0.0f);
    part(m_subBall, m_texSubHull,  0.0f, 0.0f,  2.90f, 0.52f, 0.52f, 0.75f, 0.0f);
    // Conning tower (sail) + its dive planes.
    part(m_boatCube, m_texSubDark, 0.0f, 0.62f, -0.30f, 0.46f, 0.68f, 1.45f, 0.0f);
    part(m_boatCube, m_texSubDark, 0.0f, 1.05f, -0.30f, 1.55f, 0.09f, 0.42f, 0.0f);
    // Stern planes + rudder, just forward of the screw.
    part(m_boatCube, m_texSubDark, 0.0f, 0.0f,  2.55f, 2.05f, 0.09f, 0.52f, 0.0f);
    part(m_boatCube, m_texSubDark, 0.0f, 0.0f,  2.55f, 0.09f, 1.35f, 0.52f, 0.0f);
    // The screw: a short shrouded disc on the tail cone.
    part(m_subTube, m_texSubDark,  0.0f, 0.0f,  3.62f, 0.34f, 0.07f, 0.34f, kHalfPi);
    // ONE amber running light on the sail — a dark hull under dark water needs
    // a single point the eye can find (rule 5: texture-gated emissive over a
    // near-black body, never a flat bright quad).
    part(m_boatCube, m_texSubLamp, 0.0f, 1.16f, -0.30f, 0.14f, 0.09f, 0.14f, 0.0f);
}

void RiverLife::render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       const Scene& scene) {
    if (!m_built) return;
    for (Boat& b : m_boats) {
        if (!b.ok) continue;
        if (m_phys && m_boatCube.valid()) {
            float hp[3]; b.demo.hullPos(hp);
            float q[4];  m_phys->getBodyRotation(b.demo.hull(), q);
            drawBoatSkin(device, frame, hp, q);
        } else {
            b.demo.render(frame);        // graybox fallback (art unavailable)
        }
        if (b.driver) b.driver->drawMonster(device, frame, scene);
    }
    // Wake particles: foam is ALPHA (translucent whitewater), spray ADDITIVE
    // (sunlit droplets feeding bloom) — the submitParticles blend contract.
    m_foamOut.clear(); m_sprayOut.clear();
    for (const Puff& p : m_puffs) {
        if (p.age >= p.life) continue;
        const float t = p.age / p.life;
        x3::rhi::IRenderDevice::ParticleInstance pi;
        pi.pos[0] = p.x; pi.pos[1] = p.y; pi.pos[2] = p.z;
        if (p.spray) {
            pi.size = p.size0 * (1.0f + 1.2f * t);
            const float a = (1.0f - t);
            pi.color[0] = 0.55f * a; pi.color[1] = 0.62f * a; pi.color[2] = 0.66f * a;
            pi.color[3] = 1.0f;
            m_sprayOut.push_back(pi);
        } else {
            pi.size = p.size0 * (1.0f + 2.6f * t);
            pi.color[0] = 0.88f; pi.color[1] = 0.93f; pi.color[2] = 0.95f;
            pi.color[3] = 0.80f * (1.0f - t) * (1.0f - t);
            m_foamOut.push_back(pi);
        }
    }
    if (!m_foamOut.empty())
        device.submitParticles(m_foamOut.data(), (uint32_t)m_foamOut.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Alpha);
    if (!m_sprayOut.empty())
        device.submitParticles(m_sprayOut.data(), (uint32_t)m_sprayOut.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Additive);
}

void RiverLife::shutdown(x3::audio::IAudioSystem* audio) {
    for (uint32_t i = 0; i < (uint32_t)m_boats.size(); ++i) {
        if (!m_boats[i].ok) continue;
        float hp[3]; m_boats[i].demo.hullPos(hp);
        x3::logInfo("[river-life] boat " + std::to_string(i) + " final (" +
                    std::to_string(hp[0]) + ", " + std::to_string(hp[1]) + ", " +
                    std::to_string(hp[2]) + ") speed " +
                    std::to_string(boatSpeed(i)) + " m/s");
    }
    for (Boat& b : m_boats) {
        if (audio && b.loop.valid()) audio->stopLoop(b.loop);
        b.demo.shutdown();
    }
    m_boats.clear();
    m_built = false;
}

void RiverLife::boatPos(uint32_t i, float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (i < m_boats.size() && m_boats[i].ok) m_boats[i].demo.hullPos(out);
}

float RiverLife::boatSpeed(uint32_t i) const {
    if (i >= m_boats.size() || !m_boats[i].ok || !m_boats[i].demo.controller())
        return 0.0f;
    return m_boats[i].demo.controller()->forwardSpeed();
}

} // namespace x3::game
