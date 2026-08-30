// WORLD CARS — findable, drivable, hackable vehicles in the one world.
// See world_cars.h for the design. Clean-room; built only through the public
// engine interfaces (IRenderDevice / IPhysicsWorld / IVehicleController).

#include "world_cars.h"
#include "mesh_prims.h"
#include "audio_root.h"

#include "engine/core/x3_log.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x3::game {

namespace {

// Column-major T(pos) * RotY(yaw). RH, CONVENTIONS.md: RotY maps +Z ->
// (sin yaw, 0, cos yaw) — "yaw" is the parked NOSE direction.
void composeYaw(float x, float y, float z, float yaw, float out[16]) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    out[0] = c;  out[1] = 0; out[2] = -s; out[3] = 0;
    out[4] = 0;  out[5] = 1; out[6] = 0;  out[7] = 0;
    out[8] = s;  out[9] = 0; out[10] = c; out[11] = 0;
    out[12] = x; out[13] = y; out[14] = z; out[15] = 1;
}

// Rotate vector v by quaternion q (x,y,z,w).
void quatRotate(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    // t = 2 * cross(q.xyz, v); out = v + w*t + cross(q.xyz, t)
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

constexpr float kDry = -1.0e30f;   // "no water" acceptance threshold

} // namespace

// ===========================================================================
// Build
// ===========================================================================
bool WorldCars::build(const std::vector<WorldCarDef>& defs,
                      x3::rhi::IRenderDevice* device, x3::phys::IPhysicsWorld& physics,
                      std::string_view glbDir) {
    if (m_built || !m_ground) return false;
    const auto t0 = std::chrono::steady_clock::now();
    m_device = device;
    m_physics = &physics;
    m_glbDir = std::string(glbDir);

    // Parked visual: the hero-car GLB (all drawables at their authored node
    // transforms — wheels included; origin on the ground, nose +Z). Graybox
    // box+wheels fallback on a clean checkout without the GLB / headless.
    if (m_device) {
        m_glbSrc.reset(x3::asset::createAssetSource());
        if (m_glbSrc && m_glbSrc->mountDir(m_glbDir, 0)) {
            m_glbLoader.reset(x3::asset::createModelLoader(m_device, m_glbSrc.get()));
            m_glbModel = m_glbLoader->load("Vehicles/CTR.glb");
            if (m_glbModel.ok) {
                m_glbDraw = x3::asset::makeDrawables(m_glbModel);
                m_skinned = !m_glbDraw.empty();
            }
        }
        if (!m_skinned) {
            std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
            x3::prims::makeCube(0.5f, cv, ci);
            m_boxMesh = m_device->createMesh(cv.data(), (uint32_t)cv.size(),
                                             ci.data(), (uint32_t)ci.size());
            std::vector<x3::rhi::MeshVertex> wv; std::vector<uint32_t> wi;
            makeUnitCylinderY(14, wv, wi);
            m_wheelMesh = m_device->createMesh(wv.data(), (uint32_t)wv.size(),
                                               wi.data(), (uint32_t)wi.size());
            auto t = x3::prims::makeSolidRGBA(8, 255, 255, 255);
            m_whiteTex = m_device->createTexture(t.data(), 8, 8, true);
        }
    }

    m_cars.reserve(defs.size());
    for (const WorldCarDef& d : defs) {
        Car c; c.def = d;
        if (d.region.empty()) {
            // Host-owned: resident from boot at the authored pose.
            parkCar(c, d.x, m_ground(d.x, d.z), d.z,
                    d.yawDeg * 0.0174533f, physics);
        }
        m_cars.push_back(std::move(c));
    }
    m_built = true;
    m_bootMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    uint32_t host = 0; for (const Car& c : m_cars) if (c.def.region.empty()) ++host;
    x3::logInfo("world-cars: " + std::to_string(host) + " host cars parked (" +
                std::to_string(m_cars.size() - host) + " region-owned pending), skin " +
                (m_skinned ? "CTR GLB" : (m_device ? "graybox" : "headless")) +
                ", boot " + std::to_string(m_bootMs) + " ms");
    return true;
}

// ===========================================================================
// Park / unpark (the static parked state)
// ===========================================================================
void WorldCars::parkCar(Car& c, float x, float y, float z, float yaw,
                        x3::phys::IPhysicsWorld& physics) {
    if (c.body.valid()) { physics.removeBody(c.body); c.body = {}; }
    c.px = x; c.py = y; c.pz = z; c.yaw = yaw;
    // Static collision box (the player bumps into a parked car; bullets hit it).
    c.body = physics.addBox(x3::phys::Vec3{ 0.85f, 0.62f, 1.95f },
                            x3::phys::Vec3{ x, y + 0.66f, z },
                            0.0f, x3::phys::Layer::Static);
    if (c.body.valid()) {
        const float q[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
        physics.setBodyRotation(c.body, q);
    }
    c.resident = true;
}

void WorldCars::unparkCar(Car& c, x3::phys::IPhysicsWorld& physics) {
    if (c.body.valid()) { physics.removeBody(c.body); c.body = {}; }
    c.resident = false;
}

// ===========================================================================
// Region lifecycle
// ===========================================================================
void WorldCars::onRegionBuild(std::string_view regionId, x3::phys::IPhysicsWorld& physics) {
    if (!m_built) return;
    uint32_t n = 0;
    for (int i = 0; i < (int)m_cars.size(); ++i) {
        Car& c = m_cars[i];
        if (c.def.region != regionId) continue;
        if (i == m_drivenIdx) continue;   // being driven: the live rig owns it
        // Fresh parked state at the AUTHORED pose (v1 policy; displaced exits
        // reset here — the unlocked-id set is what persists).
        parkCar(c, c.def.x, m_ground(c.def.x, c.def.z), c.def.z,
                c.def.yawDeg * 0.0174533f, physics);
        ++n;
    }
    if (n) x3::logInfo("world-cars: " + std::to_string(n) + " cars parked with region `" +
                       std::string(regionId) + "`");
}

void WorldCars::onRegionTeardown(std::string_view regionId, x3::phys::IPhysicsWorld& physics) {
    if (!m_built) return;
    for (int i = 0; i < (int)m_cars.size(); ++i) {
        Car& c = m_cars[i];
        if (c.def.region != regionId) continue;
        if (i == m_drivenIdx) continue;   // live rig: host-owned, survives
        unparkCar(c, physics);
    }
}

// ===========================================================================
// Interaction
// ===========================================================================
int WorldCars::nearestCar(float x, float z) const {
    int best = -1; float bestD = 1e9f;
    for (int i = 0; i < (int)m_cars.size(); ++i) {
        const Car& c = m_cars[i];
        if (!c.resident || i == m_drivenIdx) continue;
        // Reach is measured to the DOOR SIDES (nose +Z rotated yaw; doors on
        // local +-X at ~1.0 m), so "within 2.5 m of the door" is the real rule.
        const float rx = std::cos(c.yaw), rz = -std::sin(c.yaw);   // local +X in world
        for (int s = -1; s <= 1; s += 2) {
            const float dx = x - (c.px + rx * (float)s);
            const float dz = z - (c.pz + rz * (float)s);
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d <= kCarReach && d < bestD) { bestD = d; best = i; }
        }
    }
    return best;
}

bool WorldCars::interact(const x3::phys::Vec3& playerFeet, bool eHeld, bool eEdge,
                         bool exitEdge, float dt, Player* player,
                         x3::phys::IPhysicsWorld& physics, x3::audio::IAudioSystem* audio) {
    m_prompt.clear();
    if (!m_built) return false;

    if (m_driving) {
        m_prompt = "[E] Exit";
        if (eEdge || exitEdge) {
            exitCar(player, physics);
            return eEdge;
        }
        return false;
    }

    const int idx = nearestCar(playerFeet.x, playerFeet.z);
    if (idx < 0) { m_hackT = 0.0f; m_hackIdx = -1; return false; }
    Car& c = m_cars[idx];

    if (carLocked((uint32_t)idx)) {
        if (m_hackIdx != idx) { m_hackIdx = idx; m_hackT = 0.0f; }
        if (eHeld) {
            m_hackT += dt;
            if (m_hackT >= kCarHackSeconds) {
                m_unlocked.insert(c.def.id);
                m_hackT = 0.0f; m_hackIdx = -1;
                const x3::phys::Vec3 cp{ c.px, c.py + 0.8f, c.pz };
                // Car-alarm chirp (buzz + chime, the shared interact WAVs).
                if (audio) {
                    if (!m_sndLoaded) {
                        m_sndBuzz  = audio->load(resolveAudio("interact/buzz.wav"));
                        m_sndChime = audio->load(resolveAudio("interact/chime.wav"));
                        m_sndLoaded = true;
                    }
                    if (m_sndBuzz.valid())  audio->playSound2D(m_sndBuzz, 0.7f, 1.35f);
                    if (m_sndChime.valid()) audio->playSound2D(m_sndChime, 0.6f, 0.9f);
                }
                if (m_alarm) m_alarm(cp);   // host: guarded alert + crowd scatter
                m_prompt = "UNLOCKED";
                x3::logInfo("world-cars: '" + c.def.id + "' hacked open (permanent)");
            } else {
                char b[48];
                std::snprintf(b, sizeof(b), "HACKING... %d%%",
                              (int)(m_hackT / kCarHackSeconds * 100.0f));
                m_prompt = b;
            }
        } else {
            m_hackT = 0.0f;   // released early: progress resets
            m_prompt = "LOCKED - [hold E] hack";
        }
        return eEdge;   // near a locked car the E belongs to the hack
    }

    m_hackT = 0.0f; m_hackIdx = -1;
    m_prompt = "[E] Enter";
    if (eEdge && enterCar(idx, player, physics)) {
        m_prompt = "[E] Exit";
        return true;
    }
    return false;
}

// ===========================================================================
// Enter / exit
// ===========================================================================
bool WorldCars::enterCar(int idx, Player* player, x3::phys::IPhysicsWorld& physics) {
    Car& c = m_cars[idx];
    // Live-rig spawn pose: the chassis drives nose = -Z, the parked nose is
    // +Z rotated c.yaw — flip 180 so the live car faces the way it was parked.
    const float liveYaw = c.yaw + 3.14159265f;
    const float q[4] = { 0.0f, std::sin(liveYaw * 0.5f), 0.0f, std::cos(liveYaw * 0.5f) };
    const float sx = c.px, sy = c.py + 1.0f, sz = c.pz;

    if (!m_driveBuilt) {
        // First entry: build the ONE live rig (chassis + Jolt VehicleConstraint
        // + the GLB skin) + its limbo slab far below the world.
        {
            x3::prims::PrimMesh g = x3::prims::makeBox(10.0f, 0.5f, 10.0f,
                                                       kLimboX, kLimboY - 0.5f, kLimboZ, 0.02f);
            m_limboSlab = physics.addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                                                g.cindex.data(), (uint32_t)g.cindex.size());
        }
        bool ok = false;
        if (m_device) {
            ok = m_drive.build(*m_device, physics, sx, sy, sz);
            if (ok) {
                const bool sk = m_drive.skin(*m_device, m_glbDir, "Vehicles/CTR.glb");
                x3::logInfo(std::string("world-cars: live rig built, GLB skin ") +
                            (sk ? "ON" : "absent - graybox"));
            }
        } else {
            ok = m_drive.buildPhysics(physics, sx, sy, sz);
        }
        if (!ok) { x3::logError("world-cars: live rig build FAILED"); return false; }
        m_driveBuilt = true;
        // The rig is built LAZILY, here, on first entry -- long after the host
        // composed the performance-parts tuning at boot. Replay the cached tuning
        // now, otherwise every installed part / ECU tune silently does nothing on
        // the car actually driven in the game (the applyTuning() call at boot ran
        // while m_driveBuilt was still false and returned false).
        if (m_haveTuning) {
            const bool tuned = m_drive.applyTuning(m_pendingTuning);
            x3::logInfo(std::string("[vehparts] canon car tuning applied on entry: ") +
                        (tuned ? "OK" : "REJECTED"));
        }
    } else {
        physics.setBodyPosition(m_drive.chassis(), x3::phys::Vec3{ sx, sy, sz });
    }
    physics.setBodyRotation(m_drive.chassis(), q);
    { const float v0[3] = { 0, 0, 0 };
      physics.setBodyLinearVelocity(m_drive.chassis(), v0);
      physics.setBodyAngularVelocity(m_drive.chassis(), v0); }
    m_drive.setPaintTint(c.def.tint);
    { x3::phys::VehicleInput in{}; m_drive.setInput(in); }

    // The parked state gives way to the live rig.
    unparkCar(c, physics);

    // STASH the player capsule: parked high above the car. Player::update is
    // skipped while driving (the host's sim branch), so the CharacterVirtual
    // neither falls nor collides up there; exit re-seats it beside the car.
    if (player)
        player->setFeetPosition(physics, x3::phys::Vec3{ c.px, c.py + 150.0f, c.pz });

    m_driving = true;
    m_drivenIdx = idx;
    x3::logInfo("world-cars: entered '" + c.def.id + "' (WASD drive, Space brake, E/F exit)");
    return true;
}

void WorldCars::exitCar(Player* player, x3::phys::IPhysicsWorld& physics) {
    if (!m_driving || m_drivenIdx < 0) return;
    Car& c = m_cars[m_drivenIdx];

    // Bring the car to rest + read its pose.
    const x3::phys::Vec3 p = physics.getBodyPosition(m_drive.chassis());
    float q[4]; physics.getBodyRotation(m_drive.chassis(), q);
    const float fwdL[3] = { 0, 0, -1 };            // chassis forward = the nose
    float n[3]; quatRotate(q, fwdL, n);
    const float parkYaw = std::atan2(n[0], n[2]);  // nose dir -> parked yaw
    { const float v0[3] = { 0, 0, 0 };
      physics.setBodyLinearVelocity(m_drive.chassis(), v0);
      physics.setBodyAngularVelocity(m_drive.chassis(), v0); }
    { x3::phys::VehicleInput in{}; in.handBrake = 1.0f; m_drive.setInput(in); }

    // Re-park the visual/body at the rest pose ON the ground (yaw-only — the
    // parked read is flat; a slope exit pops a few cm, v1-acceptable).
    const float gy = m_ground(p.x, p.z);
    parkCar(c, p.x, gy, p.z, parkYaw, physics);

    // Restore the player at the DOOR SIDE on the terrain — never into deep
    // water (flip to the other door if that side is deep; if both are, the
    // right door wins and the capsule's own water query starts the swim).
    if (player) {
        const float rx = std::cos(parkYaw), rz = -std::sin(parkYaw);   // local +X
        float ex = p.x + rx * 2.1f, ez = p.z + rz * 2.1f;
        if (m_water) {
            const float wy = m_water(ex, ez);
            if (wy > kDry && wy - m_ground(ex, ez) > 1.05f) {
                const float ox = p.x - rx * 2.1f, oz = p.z - rz * 2.1f;
                const float wy2 = m_water(ox, oz);
                if (!(wy2 > kDry && wy2 - m_ground(ox, oz) > 1.05f)) { ex = ox; ez = oz; }
            }
        }
        player->setFeetPosition(physics, x3::phys::Vec3{ ex, m_ground(ex, ez) + 0.1f, ez });
    }

    // The live rig idles on the limbo slab until the next entry.
    physics.setBodyPosition(m_drive.chassis(),
                            x3::phys::Vec3{ kLimboX, kLimboY + 1.2f, kLimboZ });
    { const float qi[4] = { 0, 0, 0, 1 }; physics.setBodyRotation(m_drive.chassis(), qi); }
    { const float v0[3] = { 0, 0, 0 };
      physics.setBodyLinearVelocity(m_drive.chassis(), v0);
      physics.setBodyAngularVelocity(m_drive.chassis(), v0); }

    m_driving = false;
    m_drivenIdx = -1;
    x3::logInfo("world-cars: exited '" + c.def.id + "' (parked)");
}

void WorldCars::forceExit(Player* player, x3::phys::IPhysicsWorld& physics) {
    if (!m_driving) return;
    x3::logInfo("world-cars: ENGINE DROWNED - forced exit");
    exitCar(player, physics);
}

bool WorldCars::inDeepWater() const {
    if (!m_driving || !m_driveBuilt || !m_water) return false;
    float cp[3]; m_drive.chassisPos(cp);
    const float wy = m_water(cp[0], cp[2]);
    if (!(wy > kDry)) return false;
    const float gy = m_ground(cp[0], cp[2]);
    // Deep enough to swim in AND the hull is actually down in it.
    return (wy - gy) > 1.3f && cp[1] < wy + 0.3f;
}

// ===========================================================================
// Camera / velocity / audio
// ===========================================================================
void WorldCars::driverCamera(float yaw, float pitch, float& x, float& y, float& z) const {
    float cp[3] = { 0, 0, 0 };
    // The chase camera follows the INTERPOLATED hull, not the raw post-step
    // body (fix/car-phasing). It must be the same pose render() draws: the
    // camera is rigidly bolted to the car, so if the two disagree by even a
    // sub-step the car swims inside its own framing. Following the interpolated
    // pose is also what makes the WORLD glide instead of arriving in 60 Hz
    // lurches — that lurch, with the car pinned motionless against it, is the
    // "phasing out of phase" Tim reported at speed.
    if (m_driveBuilt) m_drive.renderChassisPos(cp);
    const float dist = 10.0f, height = 3.5f;   // host_drive's chase framing
    x = cp[0] - std::cos(pitch) * std::cos(yaw) * dist;
    y = cp[1] + height - std::sin(pitch) * dist;
    z = cp[2] - std::cos(pitch) * std::sin(yaw) * dist;
}

void WorldCars::chassisVelocity(float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (m_driveBuilt && m_physics)
        m_physics->getBodyLinearVelocity(m_drive.chassis(), out);
}

x3::phys::Vec3 WorldCars::carPosition() const {
    float cp[3] = { 0, 0, 0 };
    if (m_driveBuilt) m_drive.chassisPos(cp);
    return { cp[0], cp[1], cp[2] };
}

void WorldCars::updateAudio(x3::audio::IAudioSystem* audio, float throttle, float dt) {
    if (!audio) return;
    if (m_driving) {
        // Multi-RPM bank first (lazy one-time init — the canon car is entered
        // long after boot). Falls back to the legacy loop if the bank refuses.
        if (m_useBank && !m_bankTried) {
            m_bankTried = true;
            namespace fs = std::filesystem;
            m_bankReady = m_engineNote.init(audio,
                (fs::path(assetRoot()) / "audio/vehicles/engine_bank").string(),
                /*redlineRpm=*/6500.0f);
        }
        const float rpmFrac = std::fmin(std::fmax(engineRPM() / 6500.0f, 0.0f), 1.0f);
        const float th = std::fmin(std::fmax(throttle, 0.0f), 1.0f);
        if (m_useBank && m_bankReady) {
            if (m_engineLoop.valid()) { audio->stopLoop(m_engineLoop); m_engineLoop = {}; }
            m_engineNote.setMuted(false);
            const x3::phys::Vec3 cp = carPosition();
            m_engineNote.update(engineRPM(),
                                th * EngineNote::torqueCurve(rpmFrac),
                                dt, cp.x, cp.y, cp.z);
            return;
        }
        if (!m_engineLoop.valid()) {
            if (!m_sndEngine.valid())
                m_sndEngine = audio->load(resolveAudio("vehicles/engine_loop.wav"));
            if (m_sndEngine.valid())
                m_engineLoop = audio->startLoop(m_sndEngine, 0.35f, 0.8f);
        }
        if (m_engineLoop.valid()) {
            // LEGACY single loop (bank off/absent). The 0.30 floor is the
            // diagnosed defect; kept verbatim as the A/B reference.
            audio->setLoopParams(m_engineLoop,
                                 0.30f + 0.28f * th + 0.18f * rpmFrac,
                                 0.65f + 1.15f * rpmFrac + 0.15f * th);
        }
    } else {
        if (m_engineLoop.valid()) {
            audio->stopLoop(m_engineLoop);
            m_engineLoop = {};
        }
        m_engineNote.setMuted(true);
    }
}

// ===========================================================================
// Draw
// ===========================================================================
void WorldCars::drawParked(const x3::rhi::FrameContext& frame, const Car& c) const {
    float carM[16]; composeYaw(c.px, c.py, c.pz, c.yaw, carM);
    if (m_skinned) {
        float fin[16];
        for (const auto& d : m_glbDraw) {
            x3::asset::mulMat4(carM, d.nodeTransform, fin);
            const bool matEmis = d.emissiveTexId != 0 ||
                d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f ||
                d.emissiveFactor[2] > 0.001f;
            float emis[4] = { d.emissiveFactor[0], d.emissiveFactor[1],
                              d.emissiveFactor[2], matEmis ? 1.0f : 0.0f };
            float bc[4] = { d.baseColorFactor[0], d.baseColorFactor[1],
                            d.baseColorFactor[2], d.baseColorFactor[3] };
            if (d.clearcoat > 0.01f) {   // repaint the car-paint panels per variant
                bc[0] = c.def.tint[0]; bc[1] = c.def.tint[1]; bc[2] = c.def.tint[2];
            }
            m_device->drawMeshPBR(frame,
                                  x3::rhi::MeshHandle{ d.meshId },
                                  x3::rhi::TextureHandle{ d.baseColorTexId },
                                  x3::rhi::TextureHandle{ d.normalTexId },
                                  x3::rhi::TextureHandle{ d.mrTexId },
                                  bc, emis, fin, d.alphaMask, d.alphaBlend,
                                  x3::rhi::TextureHandle{ d.emissiveTexId },
                                  x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
                                  d.clearcoat, d.clearcoatRough,
                                  /*selfLight=*/0.0f, /*metallicScale=*/1.0f,
                                  /*foliage=*/0.0f, d.metallicFactor, d.roughnessFactor);
        }
        return;
    }
    // Graybox fallback: tinted chassis box + 4 dark wheels at the rest stations.
    if (!m_boxMesh.valid()) return;
    const float bodyCol[4]  = { c.def.tint[0], c.def.tint[1], c.def.tint[2], 1.0f };
    const float wheelCol[4] = { 0.12f, 0.12f, 0.14f, 1.0f };
    float local[16], fin[16];
    // Chassis: 1.68 x 1.0 x 3.9 box centered 0.76 m up (the DriveDemo footprint).
    std::memset(local, 0, sizeof(local));
    local[0] = 1.68f; local[5] = 1.0f; local[10] = 3.9f; local[13] = 0.76f; local[15] = 1.0f;
    x3::asset::mulMat4(carM, local, fin);
    m_device->drawMesh(frame, m_boxMesh, m_whiteTex, bodyCol, fin);
    // Wheels: unit Y-cylinder, axle rotated to local X (RotZ 90), r=0.33 w=0.24.
    const float st[4][2] = { { -0.677f, -1.186f }, { 0.677f, -1.186f },
                             { -0.723f,  1.088f }, { 0.723f,  1.088f } };
    for (int i = 0; i < 4; ++i) {
        const float r = 0.33f, w = 0.24f;
        // RotZ(90) * S(r, w, r), then translate to the station at hub height r.
        std::memset(local, 0, sizeof(local));
        local[1] = r;             // col0 = (0, r, 0)
        local[4] = -w;            // col1 = (-w, 0, 0)
        local[10] = r;            // col2 = (0, 0, r)
        local[12] = st[i][0]; local[13] = r; local[14] = st[i][1]; local[15] = 1.0f;
        x3::asset::mulMat4(carM, local, fin);
        m_device->drawMesh(frame, m_wheelMesh, m_whiteTex, wheelCol, fin);
    }
}

void WorldCars::draw(const x3::rhi::FrameContext& frame) const {
    if (!m_built || !m_device) return;
    for (int i = 0; i < (int)m_cars.size(); ++i)
        if (m_cars[i].resident && i != m_drivenIdx) drawParked(frame, m_cars[i]);
    if (m_driving && m_driveBuilt) m_drive.render(frame);
}

// ===========================================================================
// Shutdown
// ===========================================================================
void WorldCars::shutdown(x3::phys::IPhysicsWorld& physics) {
    if (!m_built) return;
    m_engineNote.shutdown();          // bank voices (safe if never started)
    m_bankTried = m_bankReady = false;
    for (Car& c : m_cars) unparkCar(c, physics);
    m_cars.clear();
    if (m_driveBuilt) { m_drive.shutdown(); m_driveBuilt = false; }
    if (m_limboSlab.valid()) { physics.removeBody(m_limboSlab); m_limboSlab = {}; }
    if (m_device) {
        if (m_boxMesh.valid())   m_device->destroyMesh(m_boxMesh);
        if (m_wheelMesh.valid()) m_device->destroyMesh(m_wheelMesh);
        if (m_whiteTex.valid())  m_device->destroyTexture(m_whiteTex);
        m_boxMesh = {}; m_wheelMesh = {}; m_whiteTex = {};
    }
    if (m_skinned && m_glbLoader) m_glbLoader->unload(m_glbModel);
    m_glbDraw.clear(); m_skinned = false;
    m_glbLoader.reset(); m_glbSrc.reset();
    m_driving = false; m_drivenIdx = -1;
    m_built = false;
}

// ===========================================================================
// Headless self-test (--test-canonvehicle)
// ===========================================================================
bool runCanonVehicleSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++passN; x3::logInfo(std::string("[canonvehicle] PASS ") + name); }
        else    { ++failN; x3::logError(std::string("[canonvehicle] FAIL ") + name); }
    };
    const float dt = 1.0f / 60.0f;

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) { x3::logError("[canonvehicle] physics init failed"); return false; }
    {   // Flat ground slab, top at y=0 (matches the injected ground query).
        x3::prims::PrimMesh g = x3::prims::makeBox(400.0f, 0.5f, 400.0f, 0.0f, -0.5f, 0.0f, 0.02f);
        phys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                            g.cindex.data(), (uint32_t)g.cindex.size());
    }

    WorldCars cars;
    cars.setGroundQuery([](float, float) { return 0.0f; });
    int alarms = 0;
    cars.setHackAlarmHook([&](const x3::phys::Vec3&) { ++alarms; });
    std::vector<WorldCarDef> defs;
    defs.push_back({ "apron_a", 10.0f, 70.0f, 90.0f, false, { 0.9f, 0.1f, 0.1f }, "" });
    defs.push_back({ "city_b",  28.0f, 96.0f,  0.0f, true,  { 0.1f, 0.4f, 0.9f }, "city" });
    check(cars.build(defs, nullptr, *phys, ""), "build: 2-car authored set (headless)");
    check(cars.resident(0) && !cars.resident(1), "build: host car resident, region car pending");
    cars.onRegionBuild("city", *phys);
    check(cars.resident(1), "region: city car parked on region realize");
    phys->optimizeBroadphase();

    Player pl;
    pl.spawn(*phys, 11.6f, 0.2f, 70.0f);   // ~1.6 m from the apron car's door
    for (int i = 0; i < 30; ++i) { PlayerInput in{}; pl.update(in, dt, *phys); phys->step(dt); }

    // V1 — E within reach enters.
    const bool consumed = cars.interact(pl.feet(), true, true, false, dt, &pl, *phys, nullptr);
    check(consumed && cars.driving() && cars.drivenIndex() == 0,
          "V1 enter: E in reach takes the wheel (E consumed)");
    check(pl.feet().y > 50.0f, "V1 enter: player capsule stashed clear of the world");

    // Settle onto the suspension, then drive 4 s at full throttle.
    for (int i = 0; i < 90; ++i) {
        x3::phys::VehicleInput in{}; cars.driveInput(in);
        cars.preStep(dt); phys->step(dt); cars.postStep(dt);
    }
    const x3::phys::Vec3 c0 = cars.carPosition();
    for (int i = 0; i < 240; ++i) {
        x3::phys::VehicleInput in{}; in.throttle = 1.0f; cars.driveInput(in);
        cars.preStep(dt); phys->step(dt); cars.postStep(dt);
    }
    const x3::phys::Vec3 c1 = cars.carPosition();
    const float disp = std::sqrt((c1.x - c0.x) * (c1.x - c0.x) + (c1.z - c0.z) * (c1.z - c0.z));
    x3::logInfo("[canonvehicle] displacement " + std::to_string(disp) + " m, fwd " +
                std::to_string(cars.forwardSpeed()) + " m/s, y " + std::to_string(c1.y));
    check(disp > 10.0f, "V2 drive: 4 s full throttle displaces > 10 m");
    check(c1.y > -0.5f && c1.y < 2.5f, "V2 drive: car stays ON the ground plane");

    // V3 — exit: capsule restored grounded beside the car.
    cars.interact(pl.feet(), true, true, false, dt, &pl, *phys, nullptr);
    check(!cars.driving(), "V3 exit: E exits (car parked at rest)");
    {
        const x3::phys::Vec3 f = pl.feet();
        const float d = std::sqrt((f.x - c1.x) * (f.x - c1.x) + (f.z - c1.z) * (f.z - c1.z));
        check(d > 1.2f && d < 3.5f, "V3 exit: player beside the car (door side)");
        bool grounded = false;
        for (int i = 0; i < 40; ++i) { PlayerInput in{}; pl.update(in, dt, *phys); phys->step(dt); grounded = pl.grounded(); }
        check(grounded && pl.feet().y < 0.4f, "V3 exit: player grounded on the terrain");
    }

    // V4 — locked car: entry refused; hold-E 3 s unlocks; early release resets.
    pl.setFeetPosition(*phys, x3::phys::Vec3{ 29.6f, 0.2f, 96.0f });
    for (int i = 0; i < 10; ++i) { PlayerInput in{}; pl.update(in, dt, *phys); phys->step(dt); }
    bool c4 = cars.interact(pl.feet(), true, true, false, dt, &pl, *phys, nullptr);
    check(c4 && !cars.driving() && !cars.unlocked("city_b"),
          "V4 hack: locked car refuses entry (E goes to the hack)");
    for (int i = 0; i < 60; ++i) cars.interact(pl.feet(), true, false, false, dt, &pl, *phys, nullptr);
    check(cars.hackProgress() > 0.25f && cars.hackProgress() < 0.45f,
          "V4 hack: 1 s of hold ~= 33% progress (readout live)");
    cars.interact(pl.feet(), false, false, false, dt, &pl, *phys, nullptr);   // release
    check(cars.hackProgress() == 0.0f, "V4 hack: releasing E resets progress");
    for (int i = 0; i < 190; ++i) cars.interact(pl.feet(), true, false, false, dt, &pl, *phys, nullptr);
    check(cars.unlocked("city_b"), "V4 hack: 3 s hold unlocks permanently");
    check(alarms == 1, "V4 hack: alarm hook fired exactly once");

    // V5 — the unlocked latch survives a region stream-out/in cycle.
    cars.onRegionTeardown("city", *phys);
    check(!cars.resident(1), "V5 region: teardown unparks the city car");
    cars.onRegionBuild("city", *phys);
    const x3::phys::Vec3 pp = cars.parkedPos(1);
    check(cars.resident(1) && std::fabs(pp.x - 28.0f) < 0.01f && std::fabs(pp.z - 96.0f) < 0.01f,
          "V5 region: rebuild re-parks at the authored curb");
    check(cars.unlocked("city_b"), "V5 region: the hacked-open latch SURVIVES the rebuild");

    // V6 — the hacked car now opens.
    bool c6 = cars.interact(pl.feet(), true, true, false, dt, &pl, *phys, nullptr);
    check(c6 && cars.driving() && cars.drivenIndex() == 1,
          "V6 enter: the hacked car takes E and drives");
    cars.interact(pl.feet(), true, true, false, dt, &pl, *phys, nullptr);   // exit clean

    cars.shutdown(*phys);
    phys->shutdown();
    x3::logInfo("[canonvehicle] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
