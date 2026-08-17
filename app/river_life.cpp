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
// / -1 upstream), returning the interpolated point `arc` metres along it.
// Stops early (clamping) at the polyline end or when the river's own water
// level has fallen/risen more than `maxDrop` from `refY` — the rendered plane
// is FLAT at the bridge's waterY, so a lane must not run to where the real
// river has left that level (a boat there would float visibly above/below
// the drawn surface).
void pointAlongReach(const WorldRiverNode* rn, uint32_t n, uint32_t start,
                     int step, float arc, float refY, float maxDrop,
                     float& outX, float& outZ) {
    float x = rn[start].x, z = rn[start].z;
    uint32_t i = start;
    float left = arc;
    while (left > 0.0f) {
        const int j = (int)i + step;
        if (j < 0 || j >= (int)n) break;
        if (std::fabs(rn[j].waterY - refY) > maxDrop) break;
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
        m_fish.setWaterQuery([](float x, float z) { return worldWaterLevelAt(x, z); });
        m_fish.setBedQuery([](float x, float z) { return terrainHeightAtWorld(x, z); });
        m_fish.setModelDir(riggedGlbRoot());
        m_fish.build(fc, scene, device);
        x3::logInfo("[river-life] FISH: " + std::to_string(m_fish.fishCount()) +
                    " fish in " + std::to_string(m_fish.schoolCount()) +
                    " schools on the bridge reach");
    }

    // ==== TWO SPEEDBOATS — patrol lanes, one per side of the bridge =========
    // Sea level = plan.waterY: the SAME height the rendered Gerstner plane
    // uses, so the hulls sit on the visible water by construction. Lanes stay
    // where the river's own level is within 0.9 m of that plane (the flat-
    // plane honesty bound) and clear of the piers (lane ends start 45 m out).
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
        // Level bound 1.5 m: the reach descends ~1.2 m per chain node here, and
        // the 5.5 m (18 ft) channel leaves a ~3.4 m draft margin under the flat
        // plane even at the lane's far end — the bound is about the HULL
        // sitting visibly ON the drawn surface, not about grounding.
        pointAlongReach(nodes, rn, nearest, step,  45.0f, m_waterY, 1.5f, b.ax, b.az);
        pointAlongReach(nodes, rn, nearest, step, 190.0f, m_waterY, 1.5f, b.bx, b.bz);
        const float laneLen = std::sqrt((b.bx - b.ax) * (b.bx - b.ax) +
                                        (b.bz - b.az) * (b.bz - b.az));
        if (laneLen < 30.0f) {
            x3::logWarn("[river-life] side " + std::to_string(side) +
                        ": lane too short (" + std::to_string(laneLen) + " m) — skipped");
            continue;
        }
        const float sx = (b.ax + b.bx) * 0.5f, sz = (b.az + b.bz) * 0.5f;
        b.ok = b.demo.build(device, phys, sx, m_waterY + 0.35f, sz, m_waterY,
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

    m_built = true;
    x3::logInfo("[river-life] built: " + std::to_string(m_boats.size()) +
                " speedboat(s) on the reach, water Y " + std::to_string(m_waterY));
    return true;
}

void RiverLife::prePhysics(float dt) {
    if (!m_built) return;
    for (Boat& b : m_boats) {
        if (!b.ok || !b.demo.controller()) continue;
        float hp[3]; b.demo.hullPos(hp);
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
        b.demo.setInput(in);
        b.demo.preStep(dt);
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

        // ---- WAKE: stern foam + bow spray while under way. ------------------
        const float speed = b.demo.controller()
                          ? std::fabs(b.demo.controller()->forwardSpeed()) : 0.0f;
        if (speed > 1.6f) {
            b.wakeAcc += dt * std::min(30.0f, 8.0f + speed * 2.2f);
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
                p.y = m_waterY + 0.06f;
                p.vx = -fx * (0.8f + speed * 0.10f) + (-fz) * r0 * 2.2f;
                p.vz = -fz * (0.8f + speed * 0.10f) + ( fx) * r0 * 2.2f;
                p.vy = 0.25f + 0.3f * std::fabs(r1);
                p.age = 0.0f; p.life = 1.5f + 0.5f * std::fabs(r0);
                p.size0 = 0.30f + 0.12f * std::fabs(r1);
                p.spray = false;
                // Bow spray: every other puff, off the chine, additive sparkle.
                if (((m_puffNext) & 1u) == 0u) {
                    Puff& s = m_puffs[m_puffNext];
                    m_puffNext = (m_puffNext + 1) % (uint32_t)m_puffs.size();
                    const float sideSign = (r1 > 0.0f) ? 1.0f : -1.0f;
                    s.x = hp[0] + fx * 2.6f + (-fz) * sideSign * 1.4f;
                    s.z = hp[2] + fz * 2.6f + ( fx) * sideSign * 1.4f;
                    s.y = m_waterY + 0.12f;
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
            if (p.y > m_waterY + 0.08f) p.y = std::max(m_waterY + 0.06f, p.y - 0.4f * dt);
        }
    }

    // ---- FISH: the schools live around the focus (camera/player). ----------
    m_fish.update(dt, scene, focus);
}

void RiverLife::render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       const Scene& scene) {
    if (!m_built) return;
    for (Boat& b : m_boats) {
        if (!b.ok) continue;
        b.demo.render(frame);
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
            pi.color[0] = 0.78f; pi.color[1] = 0.84f; pi.color[2] = 0.86f;
            pi.color[3] = 0.42f * (1.0f - t) * (1.0f - t);
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
