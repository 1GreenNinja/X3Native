// Souped-up strata / disco elevator (core). See app/elevator.h + the design
// summary docs/design/X3_WORLD_BLUEPRINT.md §2.2 + the motion contract
// specs/ELEVATOR.spec.md.
//
// Clean-room: ported from Tim's OWN Babylon module
// Q3Engine/src/features/x3-elevator.js (Tim's IP — NOT id Tech / RBDOOM /
// Quake), built ONLY against X3Native's IPhysicsWorld + Scene + IRenderDevice +
// IAudioSystem interfaces, mirroring DoorSystem's moved-static-body technique.
#include "elevator.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"
#include "engine/audio/IAudioSystem.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ===========================================================================
// CONFIG — earth-strata layers (ported 1:1 from x3-elevator.js CFG.STRATA).
// Facility-relative Y bands; the bottom three glow (Crystal Veins / Magma /
// Alien Substrate). Atmospheric geology — see blueprint §2.2: the band runs to
// -400 m even though the real shaft bottoms higher.
// ===========================================================================
static const std::array<StrataLayer, 9> kStrata = {{
    {  80.0f,  200.0f, "Sky & Concrete",  {0.40f, 0.50f, 0.60f}, false, {0,0,0} },
    {  20.0f,   80.0f, "Foundation Stone",{0.35f, 0.30f, 0.25f}, false, {0,0,0} },
    { -20.0f,   20.0f, "Limestone",       {0.55f, 0.52f, 0.45f}, false, {0,0,0} },
    { -80.0f,  -20.0f, "Granite",         {0.30f, 0.28f, 0.32f}, false, {0,0,0} },
    {-140.0f,  -80.0f, "Basalt",          {0.20f, 0.18f, 0.15f}, false, {0,0,0} },
    {-200.0f, -140.0f, "Obsidian",        {0.10f, 0.08f, 0.12f}, false, {0,0,0} },
    {-260.0f, -200.0f, "Crystal Veins",   {0.12f, 0.08f, 0.18f}, true,  {0.30f, 0.10f, 0.60f} },
    {-320.0f, -260.0f, "Magma Zone",      {0.25f, 0.06f, 0.02f}, true,  {0.80f, 0.20f, 0.05f} },
    {-400.0f, -320.0f, "Alien Substrate", {0.08f, 0.04f, 0.12f}, true,  {0.20f, 0.04f, 0.40f} },
}};

const std::array<StrataLayer, 9>& ElevatorSystem::strata() { return kStrata; }

// The 1127 disco code (ported from CFG.DISCO_CODE).
static constexpr const char* kDiscoCode = "1127";
static constexpr float kDiscoSlow = 0.25f;   // 1/4-speed glide (CFG.DISCO_SLOW)

// Play a one-shot at the cab center (spatialized) if the handle + audio resolve.
void ElevatorSystem::playOneShot(x3::audio::SoundHandle s, float vol, float pitch) {
    if (!m_audio || !s.valid()) return;
    m_audio->playSound3D(s, m_pos.x, m_pos.y + m_halfY + 1.2f, m_pos.z, vol, pitch);
}

const char* ElevatorSystem::stateName(ElevState s) {
    switch (s) {
        case ElevState::Idle:          return "IDLE";
        case ElevState::Accelerating:  return "ACCELERATING";
        case ElevState::Cruising:      return "CRUISING";
        case ElevState::Decelerating:  return "DECELERATING";
        case ElevState::Arriving:      return "ARRIVING";
        case ElevState::DoorsOpening:  return "DOORS_OPENING";
        case ElevState::DoorsOpen:     return "DOORS_OPEN";
        case ElevState::DoorsClosing:  return "DOORS_CLOSING";
        case ElevState::EmergencyStop: return "EMERGENCY_STOP";
        case ElevState::Freefall:      return "FREEFALL";
    }
    return "?";
}

bool ElevatorSystem::moving() const {
    // Anything that isn't sitting still with the doors settled counts as "moving"
    // for the legacy bool (preserves the core test's semantics: the cab is busy).
    return m_state != ElevState::Idle && m_state != ElevState::DoorsOpen;
}

const char* ElevatorSystem::currentStratum() const {
    const float y = m_pos.y;
    for (const StrataLayer& s : kStrata)
        if (y >= s.yMin && y <= s.yMax) return s.name;
    return "Unknown";
}

// ===========================================================================
// BUILD — the cab platform (unchanged core; identical to the prior behavior so
// --test-elevator E1-E6 stay green).
// ===========================================================================
bool ElevatorSystem::build(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics,
                           float shaftX, float shaftZ,
                           float cabHalfX, float cabHalfY, float cabHalfZ,
                           const std::vector<float>& stopsCenterY, int startStop) {
    if (stopsCenterY.empty()) {
        x3::logError("[elevator] build: no stops");
        return false;
    }
    m_halfX = cabHalfX; m_halfY = cabHalfY; m_halfZ = cabHalfZ;
    m_stopsY = stopsCenterY;
    std::sort(m_stopsY.begin(), m_stopsY.end());     // low -> high
    m_target = std::clamp(startStop, 0, (int)m_stopsY.size() - 1);
    m_curStop = m_target;
    m_pos = x3::phys::Vec3{ shaftX, m_stopsY[m_target], shaftZ };
    m_state = ElevState::Idle;

    // Render mesh authored centered at the body origin (the Entity transform
    // translation drives world placement as the cab moves), like a door slab.
    x3::prims::PrimMesh geo = x3::prims::makeBox(m_halfX, m_halfY, m_halfZ, 0, 0, 0, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    // Industrial cab platform: brushed metal.
    e.baseColor[0] = 0.40f; e.baseColor[1] = 0.42f; e.baseColor[2] = 0.46f; e.baseColor[3] = 1.0f;
    // Static body (mass 0): blocks/standable while still, repositioned via
    // setBodyPosition while moving. Half-extents == render extents.
    e.body = physics.addBox(x3::phys::Vec3{ m_halfX, m_halfY, m_halfZ }, m_pos, 0.0f,
                            x3::phys::Layer::Static);
    e.transform[12] = m_pos.x;
    e.transform[13] = m_pos.y;
    e.transform[14] = m_pos.z;
    m_entity = scene.add(e);
    m_body = scene.get(m_entity).body;
    m_built = true;

    x3::logInfo("[elevator] built: " + std::to_string(m_stopsY.size()) + " stops at (" +
                std::to_string(shaftX) + ", " + std::to_string(shaftZ) + "), start stop " +
                std::to_string(m_target));
    return true;
}

// ===========================================================================
// CALL VERBS
// ===========================================================================
void ElevatorSystem::callTo(int stopIndex) {
    if (!m_built) return;
    if (m_fsm) {
        // FSM: ignore a fresh call while already busy travelling, but DO accept a
        // call while idle or with the doors open (closes the doors first).
        if (m_state != ElevState::Idle && m_state != ElevState::DoorsOpen) return;
        int t = std::clamp(stopIndex, 0, (int)m_stopsY.size() - 1);
        if (t == m_target && m_state == ElevState::Idle) return;
        startTravelTo(t);
        return;
    }
    // Legacy core.
    if (m_state != ElevState::Idle) return;
    int t = std::clamp(stopIndex, 0, (int)m_stopsY.size() - 1);
    if (t == m_target) return;
    m_target = t;
    m_state = ElevState::Accelerating;   // legacy "Moving"
    x3::logInfo("[elevator] called to stop " + std::to_string(m_target));
}

void ElevatorSystem::callNext() {
    if (!m_built || m_stopsY.size() < 2) return;
    if (m_fsm) {
        if (m_state != ElevState::Idle && m_state != ElevState::DoorsOpen) return;
        startTravelTo((m_target + 1) % (int)m_stopsY.size());
        return;
    }
    if (m_state != ElevState::Idle) return;
    m_target = (m_target + 1) % (int)m_stopsY.size();
    m_state = ElevState::Accelerating;   // legacy "Moving"
    x3::logInfo("[elevator] called to stop " + std::to_string(m_target));
}

bool ElevatorSystem::playerRiding(const x3::phys::Vec3& feet) const {
    if (!m_built) return false;
    const float dx = feet.x - m_pos.x;
    const float dz = feet.z - m_pos.z;
    if (std::fabs(dx) > m_halfX + 0.35f) return false;
    if (std::fabs(dz) > m_halfZ + 0.35f) return false;
    // Y window: feet near/just above the cab top (generous for ride detection).
    const float top = m_pos.y + m_halfY;
    return feet.y >= top - 0.5f && feet.y <= top + 2.5f;
}

// ===========================================================================
// UPDATE — dispatch to the legacy linear move or the souped-up FSM.
// ===========================================================================
float ElevatorSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (!m_built || dt <= 0.0f) return 0.0f;
    return m_fsm ? fsmUpdate(dt, scene, physics) : legacyUpdate(dt, scene, physics);
}

float ElevatorSystem::legacyUpdate(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (m_state == ElevState::Idle) return 0.0f;

    const float target = m_stopsY[m_target];
    const float dy = target - m_pos.y;
    const float step = m_speed * dt;
    float moved;
    if (std::fabs(dy) <= step) {            // arrive: snap to the stop
        moved = dy;
        m_pos.y = target;
        m_state = ElevState::Idle;
        m_curStop = m_target;
        x3::logInfo("[elevator] arrived at stop " + std::to_string(m_target));
    } else {
        moved = (dy > 0.0f) ? step : -step;
        m_pos.y += moved;
    }
    syncBodyAndTransform(scene, physics);
    return moved;
}

void ElevatorSystem::syncBodyAndTransform(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    physics.setBodyPosition(m_body, x3::phys::Vec3{ m_pos.x + m_shakeX, m_pos.y, m_pos.z });
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& e = scene.get(m_entity);
        e.transform[12] = m_pos.x + m_shakeX;   // sync render transform to the body
        e.transform[13] = m_pos.y;
        e.transform[14] = m_pos.z;
    }
}

// ===========================================================================
// SOUPED-UP FSM
// ===========================================================================
void ElevatorSystem::enableFsm(bool on) {
    m_fsm = on;
    if (on) {
        // Make the FSM converge from a clean idle state.
        m_state = ElevState::Idle;
        m_fsmSpeed = 0.0f;
        m_stateTime = 0.0f;
        m_doorPct = 1.0f;
        if (m_clubStopY == kUninit) m_clubStopY = kDefaultClubFloorY + m_halfY;
    }
}

void ElevatorSystem::setClubStopY(float centerY) { m_clubStopY = centerY; }

std::string ElevatorSystem::floorLabel(int stopIndex) const {
    if (stopIndex >= 0 && stopIndex < (int)m_floorLabels.size() &&
        !m_floorLabels[stopIndex].empty())
        return m_floorLabels[stopIndex];
    return "S" + std::to_string(stopIndex);
}

void ElevatorSystem::startTravelTo(int stopIndex) {
    m_target = std::clamp(stopIndex, 0, (int)m_stopsY.size() - 1);
    // Begin by closing the doors (DOORS_CLOSING -> ACCELERATING when shut). If the
    // doors are already shut, jump straight to acceleration.
    if (m_doorPct > 0.0f) {
        m_state = ElevState::DoorsClosing;
    } else {
        m_state = ElevState::Accelerating;
    }
    m_stateTime = 0.0f;
    // Disco slow-glide if descending in disco mode (ported from callElevator()).
    if (m_disco && m_stopsY[m_target] < m_pos.y) m_discoSlow = true;
    x3::logInfo(std::string("[elevator] FSM call to stop ") + std::to_string(m_target) +
                (m_discoSlow ? " (DISCO SLOW)" : ""));
}

void ElevatorSystem::emergencyStop() {
    if (!m_fsm) return;
    m_state = ElevState::EmergencyStop;
    m_stateTime = 0.0f;
    m_fsmSpeed = 0.0f;
    playOneShot(m_snd.buzz, 0.9f, 0.7f);   // alarm klaxon
    x3::logInfo("[elevator] EMERGENCY STOP");
}

void ElevatorSystem::freefall() {
    if (!m_fsm) return;
    m_state = ElevState::Freefall;
    m_stateTime = 0.0f;
    m_fsmSpeed = 0.0f;
    x3::logInfo("[elevator] FREEFALL");
}

float ElevatorSystem::fsmUpdate(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    m_stateTime += dt;
    const float startY = m_pos.y;

    const float targetY = m_stopsY[m_target];
    const float dist = std::fabs(targetY - m_pos.y);
    const float dir  = (targetY > m_pos.y) ? 1.0f : -1.0f;
    // In disco slow mode the EFFECTIVE motion dt is scaled down while travelling
    // (ported from eDt in _updateElevator) — a dreamy 1/4-speed glide.
    const float eDt = (m_discoSlow && m_fsmSpeed > 0.5f) ? dt * kDiscoSlow : dt;

    switch (m_state) {
        case ElevState::Idle:
            m_fsmSpeed = 0.0f;
            m_shakeX = 0.0f;
            break;

        case ElevState::DoorsClosing:
            if (!m_doorWasClosing) {              // state-entry edge: doors begin to seal
                playOneShot(m_snd.doorClose, 0.7f, 1.0f);
                m_doorWasClosing = true;
            }
            m_doorPct = std::max(0.0f, m_doorPct - dt / m_tune.doorSpeed);
            if (m_doorPct <= 0.0f) {
                m_doorPct = 0.0f;
                m_state = ElevState::Accelerating;
                m_stateTime = 0.0f;
            }
            break;

        case ElevState::Accelerating:
            m_fsmSpeed = std::min(m_fsmSpeed + m_tune.accel * eDt, m_tune.maxSpeed);
            m_pos.y += dir * m_fsmSpeed * eDt;
            m_totalDist += m_fsmSpeed * eDt;
            if (m_fsmSpeed >= m_tune.maxSpeed) { m_state = ElevState::Cruising; m_stateTime = 0.0f; }
            if (dist < m_tune.decelDist)       { m_state = ElevState::Decelerating; m_stateTime = 0.0f; }
            break;

        case ElevState::Cruising:
            m_fsmSpeed = m_tune.maxSpeed * (m_discoSlow ? kDiscoSlow : 1.0f);
            m_pos.y += dir * m_fsmSpeed * eDt;
            m_totalDist += m_fsmSpeed * eDt;
            if (dist < m_tune.decelDist) { m_state = ElevState::Decelerating; m_stateTime = 0.0f; }
            break;

        case ElevState::Decelerating:
            m_fsmSpeed = std::max(m_fsmSpeed - m_tune.decel * eDt, 0.5f);
            m_pos.y += dir * m_fsmSpeed * eDt;
            m_totalDist += m_fsmSpeed * eDt;
            if (dist < 0.08f) {
                m_pos.y = targetY;
                m_fsmSpeed = 0.0f;
                m_state = ElevState::Arriving;
                m_stateTime = 0.0f;
            }
            break;

        case ElevState::Arriving:
            // Latch the arrival: update the current floor, clear disco-slow, ding,
            // then open the doors (ported from STATE.ARRIVING).
            m_fsmSpeed = 0.0f;
            m_curStop = m_target;
            m_discoSlow = false;
            playOneShot(m_snd.ding, 0.85f, 1.0f);   // arrival chime
            m_lastDingStop = m_curStop;
            m_state = ElevState::DoorsOpening;
            m_stateTime = 0.0f;
            x3::logInfo("[elevator] arrived at stop " + std::to_string(m_target) +
                        " (" + currentStratum() + ")");
            break;

        case ElevState::DoorsOpening:
            if (!m_doorWasOpening) {              // state-entry edge: doors begin to retract
                playOneShot(m_snd.doorOpen, 0.7f, 1.0f);
                m_doorWasOpening = true;
            }
            m_doorPct = std::min(1.0f, m_doorPct + dt / m_tune.doorSpeed);
            if (m_doorPct >= 1.0f) {
                m_doorPct = 1.0f;
                m_state = ElevState::DoorsOpen;
                m_stateTime = 0.0f;
            }
            break;

        case ElevState::DoorsOpen:
            // Hold open, then settle to idle (the host re-opens on a fresh call).
            if (m_stateTime > m_tune.doorHold) {
                m_state = ElevState::Idle;
                m_stateTime = 0.0f;
            }
            break;

        case ElevState::EmergencyStop: {
            // Halt + decaying lateral shake (ported from STATE.EMERGENCY_STOP).
            m_fsmSpeed = 0.0f;
            const float decay = std::max(0.0f, 1.0f - m_stateTime / 3.0f);
            m_shakeX = std::sin(m_stateTime * 30.0f) * 0.05f * decay;
            if (m_stateTime > 3.0f) {
                m_shakeX = 0.0f;
                m_state = ElevState::Idle;
                m_stateTime = 0.0f;
            }
            break;
        }

        case ElevState::Freefall:
            // Dramatic drop (ported from STATE.FREEFALL): accelerate down hard.
            m_fsmSpeed = std::min(m_fsmSpeed + 20.0f * dt, 40.0f);
            m_pos.y -= m_fsmSpeed * dt;
            m_totalDist += m_fsmSpeed * dt;
            break;
    }

    // Floor-passing dings while moving (procedural-audio hook).
    if (m_fsmSpeed > 1.0f) {
        int near = -1; float nd = 1.0e30f;
        for (int i = 0; i < (int)m_stopsY.size(); ++i) {
            float d = std::fabs(m_pos.y - m_stopsY[i]);
            if (d < nd) { nd = d; near = i; }
        }
        if (near != m_lastDingStop && nd < 1.5f) {
            m_lastDingStop = near;
            // Quieter, higher-pitched blip as a floor slides past (vs the arrival ding).
            playOneShot(m_snd.ding, 0.25f, 1.5f);
        }
    }

    // Door SFX state-entry edges reset once we leave that door state, so the next
    // open/close fires its one-shot again.
    if (m_state != ElevState::DoorsClosing) m_doorWasClosing = false;
    if (m_state != ElevState::DoorsOpening) m_doorWasOpening = false;

    updateMotorAudio(dt);

    // Disco light/strata/glass cue (animates the registered point lights + the
    // strata/disco-ball emissive). m_discoTime advances only in disco mode.
    const float t = m_discoTime;
    if (m_disco) { m_discoTime += dt; applyDiscoCue(dt, t); }

    syncBodyAndTransform(scene, physics);
    layoutVisuals(scene);
    return m_pos.y - startY;
}

void ElevatorSystem::updateMotorAudio(float dt) {
    // Motor/cable hum: a SUSTAINED loop voice whose pitch + volume track cab speed.
    // The frequency sweep 40 -> 120 Hz (CFG.MOTOR_*) maps onto a playback-rate (and
    // hence pitch) ramp on the looped WAV, so the hum audibly winds up under load and
    // settles at rest. Started lazily the first time the cab moves; stopped when it
    // comes to rest so an idle car is quiet.
    const float ratio = std::clamp(m_fsmSpeed / std::max(0.001f, m_tune.maxSpeed), 0.0f, 1.0f);
    const float targetHz = m_tune.motorIdleHz + ratio * (m_tune.motorMoveHz - m_tune.motorIdleHz);
    m_motorHz += (targetHz - m_motorHz) * std::min(1.0f, dt * 5.0f);

    if (!m_audio || !m_snd.motor.valid()) return;

    const bool wantHum = m_fsmSpeed > 0.05f;   // only while the cab is actually moving
    // Pitch the hum from the tracked frequency (idle Hz -> rate 0.6, full -> 1.4).
    const float rate = 0.6f + (m_motorHz - m_tune.motorIdleHz) /
                       std::max(1.0f, m_tune.motorMoveHz - m_tune.motorIdleHz) * 0.8f;
    const float vol  = 0.18f + 0.42f * ratio;  // louder under load

    if (wantHum) {
        if (!m_motorLoop.valid())
            m_motorLoop = m_audio->startLoop(m_snd.motor, vol, std::max(0.25f, rate));
        else
            m_audio->setLoopParams(m_motorLoop, vol, std::max(0.25f, rate));
    } else if (m_motorLoop.valid()) {
        m_audio->stopLoop(m_motorLoop);
        m_motorLoop = x3::audio::LoopHandle{};
    }
}

// ===========================================================================
// KEYPAD — terminal code entry. "1127" = DISCO toggle (+ descend to the club).
// ===========================================================================
bool ElevatorSystem::keypadDigit(int digit) {
    if (!m_fsm) return false;
    if (digit < 0 || digit > 9) return false;
    m_codeBuf += (char)('0' + digit);
    playOneShot(m_snd.keyClick, 0.5f, 1.0f + 0.04f * (float)digit);   // key-click blip

    if (m_codeBuf.size() >= 4) {
        const bool ok = (m_codeBuf.substr(m_codeBuf.size() - 4) == kDiscoCode);
        m_codeBuf.clear();
        if (ok) {
            m_disco = !m_disco;
            if (m_disco) {
                m_discoTime = 0.0f;
                playOneShot(m_snd.ding, 1.0f, 1.25f);   // bright "access granted" chime
                x3::logInfo("[elevator] DISCO MODE activated — code 1127 accepted; "
                            "descending to Club 1127 at Y=" + std::to_string(m_clubStopY));
                // Descend to the Club 1127 stop. Make sure the club stop is among
                // the reachable stops (append it if the host didn't add it), then
                // drive the cab there. The Club 1127 lane builds the room at Y=-200.
                m_descendToClub = true;
                int clubIdx = -1;
                for (int i = 0; i < (int)m_stopsY.size(); ++i)
                    if (std::fabs(m_stopsY[i] - m_clubStopY) < 0.5f) { clubIdx = i; break; }
                if (clubIdx < 0) {
                    m_stopsY.insert(m_stopsY.begin(), m_clubStopY);  // becomes the low stop
                    std::sort(m_stopsY.begin(), m_stopsY.end());
                    for (int i = 0; i < (int)m_stopsY.size(); ++i)
                        if (std::fabs(m_stopsY[i] - m_clubStopY) < 0.5f) { clubIdx = i; break; }
                    // Re-resolve the current/target stop indices after the insert.
                }
                startTravelTo(clubIdx);
            } else {
                m_disco = false;
                m_discoSlow = false;
                x3::logInfo("[elevator] DISCO MODE off");
            }
            return ok;
        } else {
            playOneShot(m_snd.buzz, 0.6f, 0.8f);   // wrong-code buzzer
            x3::logInfo("[elevator] keypad: wrong code");
        }
    }
    return false;
}

// ===========================================================================
// VISUALS — graybox in-car kit (glass / strata / OLEDs / mirror / terminal /
// ceiling light / disco ball) as child Scene entities offset around the cab.
// ===========================================================================
namespace {
// Add a tinted (optionally emissive) box entity centered at the cab origin; the
// per-frame layout offsets it. Returns the entity id (kNoLink on a bad mesh).
uint32_t addKit(Scene& scene, x3::rhi::IRenderDevice& device,
                float hx, float hy, float hz,
                const float color[4], const float emissive[4]) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, 0, 0, 0, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = (uint32_t)Tag::Prop;
    e.body.id = 0;                  // purely visual — no physics body
    return scene.add(e);
}
} // namespace

void ElevatorSystem::buildVisuals(Scene& scene, x3::rhi::IRenderDevice& device) {
    if (!m_built || m_visualsBuilt) return;

    // Car interior dims approximated from the JS CFG (3.8 x 5.2 x 3.8 m car). The
    // physics platform half-extents are smaller; the visuals are decorative.
    const float carW = 3.8f, carH = 5.2f, carD = 3.8f, T = 0.12f;
    const float noEm[4] = {0,0,0,0};

    // Glass observation wall (left, -X): translucent blue-grey graybox slab.
    { const float c[4] = {0.15f, 0.20f, 0.28f, 0.35f};
      m_eGlass = addKit(scene, device, T*0.5f, carH*0.40f, carD*0.42f, c, noEm); }

    // Earth-strata scroll plane behind the glass (further -X): emissive so it
    // reads as a lit display. Tint + glow are re-driven per frame from the
    // current stratum in layoutVisuals().
    { const float c[4] = {0.30f, 0.28f, 0.32f, 1.0f};
      const float em[4] = {0.30f, 0.28f, 0.32f, 0.6f};
      m_eStrata = addKit(scene, device, 0.02f, carH*0.45f, carD*0.40f, c, em); }

    // Back-wall mirror (-Z): bright, near-white metallic graybox plane.
    { const float c[4] = {0.92f, 0.92f, 0.95f, 1.0f};
      m_eMirror = addKit(scene, device, carW*0.32f, carH*0.30f, 0.02f, c, noEm); }

    // Twin OLED viewscreens: left = geo survey, right = floor directory. Emissive
    // cyan/violet graybox panels.
    { const float c[4] = {0.0f, 0.05f, 0.10f, 1.0f};
      const float em[4] = {0.0f, 0.40f, 0.80f, 1.2f};
      m_eOledL = addKit(scene, device, 0.30f, 0.19f, 0.02f, c, em); }
    { const float c[4] = {0.05f, 0.0f, 0.08f, 1.0f};
      const float em[4] = {0.30f, 0.10f, 0.60f, 1.0f};
      m_eOledR = addKit(scene, device, 0.30f, 0.19f, 0.02f, c, em); }

    // Blue access terminal + keypad (right wall, +X): emissive blue graybox.
    { const float c[4] = {0.04f, 0.08f, 0.15f, 1.0f};
      const float em[4] = {0.0f, 0.30f, 0.90f, 1.0f};
      m_eTerm = addKit(scene, device, 0.05f, 0.30f, 0.18f, c, em); }

    // Ceiling disco ball (hidden until disco — emissive 0 until then).
    { const float c[4] = {0.90f, 0.90f, 0.90f, 1.0f};
      m_eDiscoBall = addKit(scene, device, 0.30f, 0.30f, 0.30f, c, noEm); }

    // Ceiling light fixture: emissive cool-white strip.
    { const float c[4] = {0.67f, 0.80f, 0.93f, 1.0f};
      const float em[4] = {0.40f, 0.60f, 0.80f, 1.0f};
      m_eCeil = addKit(scene, device, carW*0.30f, 0.03f, 0.15f, c, em); }

    // Twin sliding DOOR panels on the front (+X) wall — two tall brushed-metal
    // slabs that part along Z as m_doorPct rises. Half-width per panel = a quarter
    // of the car so the pair closes flush at the center. A faint emissive seam reads
    // in the dark. Animated in layoutVisuals() from m_doorPct.
    { const float c[4] = {0.34f, 0.36f, 0.40f, 1.0f};
      const float em[4] = {0.05f, 0.08f, 0.10f, 0.4f};
      m_eDoorL = addKit(scene, device, 0.06f, carH*0.42f, carD*0.24f, c, em);
      m_eDoorR = addKit(scene, device, 0.06f, carH*0.42f, carD*0.24f, c, em); }

    // Floor INDICATOR strip above the doors: an emissive bar that the host/layout
    // tints green when the doors are open (safe to step out) and amber while
    // travelling. Gives the rider an at-a-glance status cue.
    { const float c[4] = {0.05f, 0.06f, 0.06f, 1.0f};
      const float em[4] = {0.0f, 0.9f, 0.3f, 1.4f};
      m_eIndicator = addKit(scene, device, 0.04f, 0.10f, carD*0.30f, c, em); }

    // Point lights: [0] = ceiling interior fill, [1..4] = disco spots (off until
    // disco mode). The host pushes these via setPointLights; the disco cue animates
    // them in update().
    m_lights.clear();
    x3::rhi::PointLight ceil;
    ceil.color[0] = 0.60f; ceil.color[1] = 0.73f; ceil.color[2] = 0.87f;
    ceil.range = 12.0f;
    m_lights.push_back(ceil);
    const float spot[4][3] = {
        {1.0f, 0.2f, 0.4f}, {0.2f, 0.4f, 1.0f}, {0.2f, 1.0f, 0.4f}, {1.0f, 0.8f, 0.2f}
    };
    for (int i = 0; i < 4; ++i) {
        x3::rhi::PointLight l;
        l.color[0] = spot[i][0]; l.color[1] = spot[i][1]; l.color[2] = spot[i][2];
        l.range = 6.0f;
        m_lights.push_back(l);   // intensity baked into color; starts at 0 (disco off)
        for (int c = 0; c < 3; ++c) m_lights.back().color[c] = 0.0f;
    }

    m_visualsBuilt = true;
    layoutVisuals(scene);
    x3::logInfo("[elevator] visuals built: glass + strata + twin OLEDs + mirror + "
                "blue terminal/keypad + ceiling light + disco ball");
}

void ElevatorSystem::layoutVisuals(Scene& scene) {
    if (!m_visualsBuilt) return;
    const float carW = 3.8f, carH = 5.2f, carD = 3.8f, T = 0.12f;
    const float cx = m_pos.x + m_shakeX, cz = m_pos.z;
    // The cab top is the car floor; the car interior rises +carH/2 above center.
    const float floorY = m_pos.y + m_halfY;
    const float midY = floorY + carH * 0.5f;

    auto place = [&](uint32_t id, float ox, float oy, float oz) {
        if (id == kNoLink || id >= scene.size()) return;
        Entity& e = scene.get(id);
        e.transform[12] = cx + ox;
        e.transform[13] = midY + oy;
        e.transform[14] = cz + oz;
    };
    place(m_eGlass,     -carW * 0.5f + T,        0.0f,           0.0f);
    place(m_eStrata,    -carW * 0.5f - 0.30f,    0.0f,           0.0f);
    place(m_eMirror,     0.0f,                   0.15f,         -carD * 0.5f + T);
    place(m_eOledL,     -0.9f,                   carH * 0.5f - 1.2f, carD * 0.5f - 0.18f);
    place(m_eOledR,      carW * 0.5f - 0.18f,    0.0f,          -carD * 0.25f);
    place(m_eTerm,       carW * 0.5f - 0.18f,   -0.30f,          carD * 0.25f);
    place(m_eDiscoBall,  0.0f,                   carH * 0.5f - 0.5f, 0.0f);
    place(m_eCeil,       0.0f,                   carH * 0.5f - 0.1f, 0.0f);

    // Sliding doors on the +X wall: closed (doorPct=0) the two panels meet at z=0;
    // open (doorPct=1) they retract to +/- a quarter-depth. Each panel is a quarter
    // wide, so the slide distance is carD*0.25.
    {
        const float slide = m_doorPct * carD * 0.25f;
        const float doorX = carW * 0.5f - 0.06f;
        const float doorY = -0.4f;     // doors sit slightly below car mid (head clearance)
        place(m_eDoorL, doorX, doorY, -carD * 0.25f * 0.5f - slide);
        place(m_eDoorR, doorX, doorY,  carD * 0.25f * 0.5f + slide);
    }
    // Indicator strip above the doors.
    place(m_eIndicator, carW * 0.5f - 0.04f, carH * 0.42f, 0.0f);
    // Tint the indicator: green when doors fully open, amber while moving/closing.
    if (m_eIndicator != kNoLink && m_eIndicator < scene.size()) {
        Entity& e = scene.get(m_eIndicator);
        const bool open = (m_state == ElevState::DoorsOpen) || (m_doorPct > 0.95f &&
                          m_state == ElevState::Idle);
        if (open) { e.emissive[0] = 0.0f; e.emissive[1] = 0.9f; e.emissive[2] = 0.3f; }
        else      { e.emissive[0] = 1.0f; e.emissive[1] = 0.55f; e.emissive[2] = 0.0f; }
        e.emissive[3] = 1.4f;
    }

    // Position the point lights at the cab interior (ceiling), spots ringed.
    if (!m_lights.empty()) {
        m_lights[0].pos[0] = cx; m_lights[0].pos[1] = midY + carH * 0.5f - 0.5f; m_lights[0].pos[2] = cz;
        for (int i = 1; i < (int)m_lights.size(); ++i) {
            const float a = (float)(i - 1) / 4.0f * 6.2831853f;
            m_lights[i].pos[0] = cx + std::cos(a) * 1.2f;
            m_lights[i].pos[1] = midY + 1.0f;
            m_lights[i].pos[2] = cz + std::sin(a) * 1.2f;
        }
    }

    // Disco-ball emissive: glows when disco mode is on, dark otherwise.
    if (m_eDiscoBall != kNoLink && m_eDiscoBall < scene.size()) {
        Entity& e = scene.get(m_eDiscoBall);
        const float g = m_disco ? 0.9f : 0.0f;
        e.emissive[0] = 0.8f * g; e.emissive[1] = 0.8f * g; e.emissive[2] = 0.9f * g;
        e.emissive[3] = m_disco ? 1.2f : 0.0f;
    }

    // Drive the strata-plane tint/glow from the cab's current stratum.
    if (m_eStrata != kNoLink && m_eStrata < scene.size()) {
        Entity& e = scene.get(m_eStrata);
        for (const StrataLayer& s : kStrata) {
            if (m_pos.y >= s.yMin && m_pos.y <= s.yMax) {
                for (int c = 0; c < 3; ++c) e.baseColor[c] = s.rgb[c];
                if (s.glow) {
                    for (int c = 0; c < 3; ++c) e.emissive[c] = s.glowRgb[c];
                    e.emissive[3] = 1.4f;
                } else {
                    for (int c = 0; c < 3; ++c) e.emissive[c] = s.rgb[c];
                    e.emissive[3] = 0.5f;
                }
                break;
            }
        }
    }
}

void ElevatorSystem::applyDiscoCue(float /*dt*/, float t) {
    if (!m_visualsBuilt) return;
    // Disco-ball emissive + the spinning, pulsing colored spots (the disco cue).
    // The ceiling light dims to a dim purple while the 4 spots cycle hue/intensity.
    // (The disco-ball entity emissive is driven in layoutVisuals() from m_disco.)
    if (!m_lights.empty()) {
        m_lights[0].color[0] = 0.20f; m_lights[0].color[1] = 0.05f; m_lights[0].color[2] = 0.40f;
        const float baseSpot[4][3] = {
            {1.0f, 0.2f, 0.4f}, {0.2f, 0.4f, 1.0f}, {0.2f, 1.0f, 0.4f}, {1.0f, 0.8f, 0.2f}
        };
        for (int i = 1; i < (int)m_lights.size(); ++i) {
            const float pulse = 0.6f + 0.4f * std::sin(t * 3.0f + (float)i * 1.5f);
            for (int c = 0; c < 3; ++c) m_lights[i].color[c] = baseSpot[i - 1][c] * pulse;
        }
    }
}

} // namespace x3::game


// ===========================================================================
// Headless self-test (--test-elevatorfsm). Self-contained: uses the shared
// HeadlessRenderDevice + a fresh Jolt world; no window/Vulkan. Leak-clean.
// ===========================================================================
#include "headless_device.h"

namespace x3::game {

namespace {
int    s_pass = 0, s_fail = 0;
void   fcheck(bool cond, const char* name) {
    if (cond) { ++s_pass; x3::logInfo(std::string("  [PASS] ") + name); }
    else      { ++s_fail; x3::logError(std::string("  [FAIL] ") + name); }
}
constexpr float kDt = 1.0f / 60.0f;
} // namespace

bool runElevatorFsmSelfTest() {
    s_pass = s_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessRenderDevice device;
    Scene scene;

    // A tall shaft: stop 0 at cab-center y=0.15 (room floor), stop 1 at +60 m up
    // (long enough that the cab actually reaches MAX_SPEED == CRUISING before the
    // decel window). Cab half-extents match the main wiring.
    const float cabHY = 0.15f;
    const float groundY = cabHY;            // cab CENTER at the room floor
    const float topY    = cabHY + 60.0f;    // cab CENTER 60 m up
    ElevatorSystem elev;
    bool built = elev.build(scene, device, *physics, 0.0f, 0.0f,
                            1.4f, cabHY, 1.4f, std::vector<float>{ groundY, topY }, 0);
    elev.enableFsm(true);
    elev.buildVisuals(scene, device);

    // ---- F1: builds, FSM enabled, idle at the ground stop, club stop = -200.
    fcheck(built && elev.built() && elev.fsmEnabled() &&
           elev.state() == ElevState::Idle &&
           std::fabs(elev.cabCenter().y - groundY) < 1e-3f,
           "F1 builds + FSM enabled, idle at ground stop");
    // Club descent target is Y=-200 floor + cab half-height (so the cab top sits
    // flush with the club floor at -200).
    fcheck(std::fabs(elev.clubStopY() - (ElevatorSystem::kDefaultClubFloorY + cabHY)) < 1e-3f,
           "F1b club stop Y = -200 (+ cab half-height)");

    // ---- F2: strata = 9 layers, named, with the bottom three glowing.
    {
        const auto& st = ElevatorSystem::strata();
        bool glows = st[6].glow && st[7].glow && st[8].glow && !st[0].glow;
        fcheck(st.size() == 9 &&
               std::string(st[0].name) == "Sky & Concrete" &&
               std::string(st[8].name) == "Alien Substrate" && glows,
               "F2 9 earth-strata layers, named, bottom-3 glow");
    }

    // ---- F3: a NORMAL ride up — drive the FSM and record the state sequence.
    // Expect IDLE -> (DOORS_CLOSING) -> ACCELERATING -> CRUISING -> DECELERATING
    // -> ARRIVING -> DOORS_OPENING -> DOORS_OPEN -> IDLE, the cab reaches topY, and
    // the speed ramps up to (clamps at) MAX_SPEED then ramps back down.
    {
        elev.callTo(1);   // call to the top stop
        bool sawClosing = false, sawAccel = false, sawCruise = false, sawDecel = false,
             sawArriving = false, sawOpening = false, sawOpen = false, backIdle = false;
        bool clampedMax = false, rampedDown = false;
        bool cruiseBeforeDecel = false;
        float maxSeen = 0.0f;
        ElevState prev = elev.state();
        float feetY = elev.cabTopY() + 0.05f;   // a simulated rider on the cab top
        float carried = 0.0f;

        for (int i = 0; i < 4000; ++i) {
            float edy = elev.update(kDt, scene, *physics);
            if (elev.playerRiding(x3::phys::Vec3{0.0f, feetY, 0.0f})) {
                feetY += edy; carried += edy;
            }
            ElevState s = elev.state();
            switch (s) {
                case ElevState::DoorsClosing: sawClosing = true; break;
                case ElevState::Accelerating: sawAccel = true; break;
                case ElevState::Cruising:     sawCruise = true; if (!sawDecel) cruiseBeforeDecel = true; break;
                case ElevState::Decelerating: sawDecel = true; break;
                case ElevState::Arriving:     sawArriving = true; break;
                case ElevState::DoorsOpening: sawOpening = true; break;
                case ElevState::DoorsOpen:    sawOpen = true; break;
                default: break;
            }
            // Track the FSM speed via the carried delta magnitude per frame.
            float frameSpeed = std::fabs(edy) / kDt;
            if (frameSpeed > maxSeen) maxSeen = frameSpeed;
            // Speed clamps at MAX_SPEED (within a frame's accel step tolerance).
            if (frameSpeed > elev.tuning().maxSpeed - 0.5f &&
                frameSpeed <= elev.tuning().maxSpeed + 0.5f) clampedMax = true;
            // Detect ramp-down: a slower frame after the max while decelerating.
            if (s == ElevState::Decelerating && frameSpeed < maxSeen - 1.0f) rampedDown = true;
            prev = s;
            if (s == ElevState::Idle && (sawArriving || sawOpen)) { backIdle = true; break; }
        }
        (void)prev;

        bool reached = std::fabs(elev.cabCenter().y - topY) < 0.05f;
        bool seq = sawClosing && sawAccel && sawCruise && sawDecel && sawArriving &&
                   sawOpening && sawOpen && backIdle;
        fcheck(seq, "F3 ride state sequence IDLE->CLOSING->ACCEL->CRUISE->DECEL->ARRIVING->DOORS->IDLE");
        fcheck(reached, "F3b cab reaches the target floor Y (60 m up)");
        fcheck(cruiseBeforeDecel && clampedMax && maxSeen <= elev.tuning().maxSpeed + 0.5f,
               "F3c speed ramps up + CLAMPS at MAX_SPEED (~14) before decel");
        fcheck(rampedDown, "F3d speed ramps DOWN during DECELERATING");
        fcheck(std::fabs(carried - (topY - groundY)) < 0.25f,
               "F3e rider carried up by the full ride height");
    }

    // ---- F4: the 1127 keypad code triggers DISCO + a descent to Y=-200.
    {
        ElevatorSystem e2;
        e2.build(scene, device, *physics, 10.0f, 10.0f, 1.4f, cabHY, 1.4f,
                 std::vector<float>{ groundY, topY }, 1);   // start at the TOP stop
        e2.enableFsm(true);
        // Wrong code first: 9-9-9-9 must NOT enable disco.
        e2.keypadDigit(9); e2.keypadDigit(9); e2.keypadDigit(9);
        bool notYet = !e2.disco();
        bool wrong = e2.keypadDigit(9);
        fcheck(notYet && !wrong && !e2.disco(), "F4 wrong code (9999) does NOT enable disco");

        // Right code: 1-1-2-7 enables disco; the 4th digit returns true.
        e2.keypadDigit(1); e2.keypadDigit(1); e2.keypadDigit(2);
        bool completed = e2.keypadDigit(7);
        bool descending = (e2.state() == ElevState::DoorsClosing ||
                           e2.state() == ElevState::Accelerating ||
                           e2.state() == ElevState::Cruising ||
                           e2.state() == ElevState::Decelerating);
        fcheck(completed && e2.disco() && descending,
               "F4b code 1127 enables DISCO + starts a descent");
        fcheck(std::fabs(e2.clubStopY() - (ElevatorSystem::kDefaultClubFloorY + cabHY)) < 1e-3f,
               "F4c disco descent target is Club 1127 at Y=-200");

        // Drive it down and assert it actually reaches the club stop.
        for (int i = 0; i < 20000 && e2.state() != ElevState::DoorsOpen &&
                        e2.state() != ElevState::Idle; ++i)
            e2.update(kDt, scene, *physics);
        fcheck(std::fabs(e2.cabCenter().y - (ElevatorSystem::kDefaultClubFloorY + cabHY)) < 0.1f,
               "F4d cab descends all the way to the Club 1127 stop (Y=-200)");
    }

    // ---- F5: FREEFALL is reachable + drops the cab.
    {
        ElevatorSystem e3;
        e3.build(scene, device, *physics, 20.0f, 20.0f, 1.4f, cabHY, 1.4f,
                 std::vector<float>{ groundY, topY }, 1);
        e3.enableFsm(true);
        float before = e3.cabCenter().y;
        e3.freefall();
        bool inState = e3.state() == ElevState::Freefall;
        for (int i = 0; i < 30; ++i) e3.update(kDt, scene, *physics);
        bool dropped = e3.cabCenter().y < before - 1.0f;
        fcheck(inState && dropped, "F5 FREEFALL reachable + drops the cab");
    }

    // ---- F6: EMERGENCY_STOP is reachable, halts, shakes, then recovers to IDLE.
    {
        ElevatorSystem e4;
        e4.build(scene, device, *physics, 30.0f, 30.0f, 1.4f, cabHY, 1.4f,
                 std::vector<float>{ groundY, topY }, 0);
        e4.enableFsm(true);
        e4.callTo(1);
        for (int i = 0; i < 20; ++i) e4.update(kDt, scene, *physics);  // get it moving
        e4.emergencyStop();
        bool inState = e4.state() == ElevState::EmergencyStop;
        // Run past the 3 s shake window -> recovers to IDLE.
        for (int i = 0; i < 250; ++i) e4.update(kDt, scene, *physics);
        bool recovered = e4.state() == ElevState::Idle;
        fcheck(inState && recovered, "F6 EMERGENCY_STOP reachable, halts + recovers to IDLE");
    }

    // ---- F7: state-name table is complete (all 10 states named, distinct).
    {
        const ElevState all[] = {
            ElevState::Idle, ElevState::Accelerating, ElevState::Cruising,
            ElevState::Decelerating, ElevState::Arriving, ElevState::DoorsOpening,
            ElevState::DoorsOpen, ElevState::DoorsClosing, ElevState::EmergencyStop,
            ElevState::Freefall };
        bool ok = true;
        for (const ElevState s : all) {
            const char* n = ElevatorSystem::stateName(s);
            if (!n || n[0] == '?' || n[0] == '\0') ok = false;
        }
        fcheck(ok, "F7 all 10 FSM states have names");
    }

    // ---- F8: the legacy core path is UNTOUCHED (FSM off => linear snap arrive),
    // so --test-elevator stays green. Build a fresh non-FSM cab and ride it.
    {
        ElevatorSystem e5;
        e5.build(scene, device, *physics, 40.0f, 40.0f, 1.4f, cabHY, 1.4f,
                 std::vector<float>{ groundY, cabHY + 6.0f }, 0);
        e5.setSpeed(8.0f);
        e5.callNext();
        bool startedMoving = e5.moving() && e5.targetStop() == 1;
        for (int i = 0; i < 600 && e5.moving(); ++i) e5.update(kDt, scene, *physics);
        bool arrived = !e5.moving() && std::fabs(e5.cabCenter().y - (cabHY + 6.0f)) < 1e-3f;
        fcheck(startedMoving && arrived, "F8 legacy non-FSM linear lift still arrives (core unchanged)");
    }

    physics->shutdown();
    x3::logInfo("elevatorfsm: " + std::to_string(s_pass) + "/" +
                std::to_string(s_pass + s_fail) + " passed");
    return s_fail == 0;
}

} // namespace x3::game
