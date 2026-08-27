// Button -> door interaction (S4). See app/door.h.
//
// Clean-room: built from the IPhysicsWorld + Scene interfaces only.
#include "door.h"
#include "headless_device.h"
#include "level.h"
#include "mesh_prims.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Door geometry constants (must match the S2 doorway gap in level.cpp).
//   Doorway: +Z wall at z = kRoomHalf (8.0), gap centered at X=0, half-width
//   kDoorHalf (0.6 -> 1.2 m wide), wall thickness kWallT (0.2 -> half 0.1),
//   passage clear up to the lintel bottom at y = 2.1.
// ---------------------------------------------------------------------------
namespace {
constexpr float kDoorEdgeZ      = 8.0f;   // +Z wall plane (== level.cpp edge)
constexpr float kDoorHalfX      = 0.6f;   // == kDoorHalf (fills the 1.2 m opening)
constexpr float kDoorHalfZ      = 0.1f;   // == kWallT*0.5 (matches wall thickness)
constexpr float kPassageTop     = 2.1f;   // == lintel bottom (clear opening height)
constexpr float kDoorHalfY      = kPassageTop * 0.5f;          // 1.05
constexpr float kDoorCenterY    = kDoorHalfY;                  // bottom sits at y=0
// Slide UP by the full door height so the bottom rises to the lintel: the
// passage (y in [0, 2.1]) is then clear.
constexpr float kSlideUp        = kPassageTop;                 // 2.1 m
constexpr float kOpenDuration   = 1.0f;                        // seconds

// ---- Shared SM_Door_A GLB visual ------------------------------------------
// Loose-GLB root + relative path of the real sci-fi door mesh (same kit as
// env_art.cpp). The door mesh is drawn OVER the (now invisible) collision box.
// Root resolved via assetRoot() (assets-LFS pass): repo-relative assets/ first,
// G:/GameModels fallback. Lazy-resolved once (the exe path is stable).
inline const std::string& kDoorGlbDir() {
    static const std::string d = convertedGlbRoot();
    return d;
}
const char* kDoorGlbRel   = "ModularSciFi_Interior/SM_Door_A.glb";
const char* kFrameGlbRel  = "ModularSciFi_Interior/SM_DoorFrame_A.glb";

// Probed WORLD-space AABB of SM_Door_A AFTER the GLB node TRS is applied (the
// space makeDrawables() bakes into nodeTransform — measured with python parsing
// the glb POSITION accessor min/max through the node hierarchy):
//   min (-4.875, 0.054, -0.112)  max (-2.525, 3.554, 0.112)
//   size 2.350 (X wide) x 3.500 (Y tall) x 0.224 (Z thick), centered at
//   X=-3.700, Y=1.804, Z=0.0. The slab faces along Z (thin in Z, wide in X).
struct GlbAabb { float minx,miny,minz, maxx,maxy,maxz; };
constexpr GlbAabb kDoorAabb { -4.875f, 0.054f, -0.112f, -2.525f, 3.554f, 0.112f };
inline float gcx(const GlbAabb& a){ return (a.minx+a.maxx)*0.5f; }
inline float gcz(const GlbAabb& a){ return (a.minz+a.maxz)*0.5f; }
constexpr float kDoorGlbW = kDoorAabb.maxx - kDoorAabb.minx;  // 2.35 natural width (X)
constexpr float kDoorGlbH = kDoorAabb.maxy - kDoorAabb.miny;  // 3.50 natural height (Y)

// Probed WORLD-space AABB of SM_DoorFrame_A after its node TRS (the same table
// cell_dressing.cpp / env_art.cpp measured — kept identical so all three passes
// seat the same asset the same way): a 6.25 m x 4.446 m x 0.554 m showroom frame.
constexpr GlbAabb kFrameAabb { -6.250f, -0.043f, -0.277f, 0.0f, 4.403f, 0.277f };
constexpr float kFrameGlbW = kFrameAabb.maxx - kFrameAabb.minx;   // 6.250
constexpr float kFrameGlbT = kFrameAabb.maxz - kFrameAabb.minz;   // 0.554
// The frame's probed AABB base is a thin buried SILL; its VISIBLE jamb starts
// ~0.69 GLB-units higher (cell_dressing measured this by eye as a 0.40 m drop at
// its 0.58 scale — 0.40 / 0.58 = 0.69 — and its frames sit correctly on the deck).
// Anchoring THAT plane on the floor is what stops the ring reading as floating.
constexpr float kFrameSillGlb = 0.69f;
constexpr float kFrameGlbH    = kFrameAabb.maxy - (kFrameAabb.miny + kFrameSillGlb);  // 3.756

// ---- PER-FLOOR STYLE TABLE (door.h DoorStyle) ------------------------------
// Hues follow the SAME canonical zone-colour ladder the rest of the tower already
// speaks (docs/design/TEXTURE_DESIGN_STRATEGY §1.2; app_run.cpp's per-floor light
// tints and fog, room_dressing.cpp's Zone recipes): F3 Genetics GREEN, F4
// Cybernetics CYAN, F5 Drone AMBER, F6 Alien biolume TEAL, F7 Executive BRASS.
// Doors were the one surface NOT carrying it. `leaf` multiplies an already
// value-normalized albedo, so every channel is <= 1 and the surface-library VALUE
// band (0.40 ceiling) still holds — this is a HUE pass, not a brightness cheat.
constexpr DoorStyle kDoorStyles[kDoorFloorCount] = {
    // name                leaf (albedo x)           sign (emissive)           frame
    { "B1 Detention",     {0.72f,0.74f,0.78f}, {1.00f,0.24f,0.14f}, {0.55f,0.57f,0.62f} }, // cold steel + RED detention tell
    { "F1 Atrium",        {0.92f,0.92f,0.94f}, {0.82f,0.90f,1.00f}, {0.78f,0.80f,0.84f} }, // clean lobby white
    { "F2 Medical",       {0.90f,0.95f,0.92f}, {0.28f,1.00f,0.70f}, {0.80f,0.86f,0.83f} }, // clinical white-green
    { "F3 Genetics",      {0.68f,0.86f,0.70f}, {0.36f,1.00f,0.42f}, {0.52f,0.68f,0.54f} }, // vat GREEN
    { "F4 Cybernetics",   {0.62f,0.76f,0.88f}, {0.30f,0.85f,1.00f}, {0.42f,0.52f,0.62f} }, // cold CYAN over dark steel
    { "F5 Drone Mfg",     {0.88f,0.72f,0.42f}, {1.00f,0.66f,0.12f}, {0.62f,0.50f,0.30f} }, // industrial AMBER caution
    { "F6 Alien Tech",    {0.52f,0.78f,0.74f}, {0.24f,1.00f,0.86f}, {0.34f,0.52f,0.50f} }, // biolume TEAL
    { "F7 Executive",     {0.86f,0.80f,0.62f}, {1.00f,0.86f,0.48f}, {0.70f,0.62f,0.42f} }, // BRASS luxury
};

// Canonical Spire floor base elevations (X3_WORLD_BLUEPRINT §2.1; the table
// app/level1.h L1Floor documents): B1=0 F1=5 F2=10 F3=20 F4=30 F5=65 F6=78 F7=91.
// DUPLICATED here rather than #including level1.h so door.cpp stays independent of
// the graybox level module — a door built in ANY world still resolves a style.
constexpr float kFloorBaseY[kDoorFloorCount] = { 0.0f, 5.0f, 10.0f, 20.0f, 30.0f, 65.0f, 78.0f, 91.0f };

// A LOCKED door burns its signage RED whatever floor it is on (see door.h): the
// live status readout for the existing keycard/keypad gate.
constexpr float kLockedSign[3] = { 1.00f, 0.16f, 0.10f };

// ---------------------------------------------------------------------------
// DOOR MODEL REGISTRY (doors-pass; see door.h). AABBs PROBED via a tools-side
// glTF node walk (accessor min/max through each node's TRS chain) — the same
// discipline as kDoorAabb above. The sound columns all reference the ONE door
// sound family the library ships (assets/audio/doors + interact) — the closest
// family for every model; a per-model WAV drop is a data edit here.
// ---------------------------------------------------------------------------
constexpr DoorModelDef kDoorModels[] = {
    // 0 — door_a: the SM_Door_A default. Drawn by the dedicated legacy path
    // (m_doorModel + frame + backing slab); this row exists so every door has a
    // registry identity + sound row. AABB = kDoorAabb.
    { "door_a", "ModularSciFi_Interior/SM_Door_A.glb", 0, DoorMotion::SlideUp,
      { -4.875f, 0.054f, -0.112f, -2.525f, 3.554f, 0.112f },
      nullptr, nullptr, {0,0,0,0,0,0}, {0,0,0,0,0,0}, false, 0.0f,
      "doors/door_open.wav", "doors/door_close.wav", "doors/door_locked.wav",
      "interact/servo_loop.wav", "interact/door_thunk.wav" },

    // 1 — slider: meshy sliding_door_art. door_frame (static) + door_panel_L/R
    // parting HORIZONTALLY (its authored L/R translation clips). Probed after
    // node TRS (pure translations). Width axis = X (preYaw 0).
    { "slider", "props_articulated/sliding_door_art.glb", 1, DoorMotion::SlideSplit,
      { -0.951f, -0.787f, -0.475f, 0.949f, 0.785f, 0.473f },
      "door_panel_L", "door_panel_R",
      { -0.832f, -0.787f, -0.356f, 0.321f, 0.123f, 0.340f },
      { -0.122f, -0.787f, -0.358f, 0.837f, 0.111f, 0.342f },
      false, 0.0f,
      "doors/door_open.wav", "doors/door_close.wav", "doors/door_locked.wav",
      "interact/servo_loop.wav", "interact/door_thunk.wav" },

    // 2 — bulkhead: SciFiKit3 Wall_Door_Simple_01's Door_Left/Door_Right leaves.
    // The kit's converted node TRS for these panels is BROKEN (probed: panels
    // land at z -3.6..-0.2 while their own wall spans z 0..6, rotated flat), so
    // the leaves seat from MESH-LOCAL bounds (rawPanelSpace) and the fused
    // Wall/Fence nodes are skipped entirely (canon walls already exist; LAW 2
    // forbids a doubled wall). Mesh-local: X +-0.5 = thickness, Y 0..3.416 = up,
    // Z = the width run (Left -1.333..0, Right 0..1.333) -> preYaw 90 deg maps
    // the Z run onto the standard X run.
    { "bulkhead", "SciFiKit3/Wall_Door_Simple_01.glb", 0, DoorMotion::SlideSplit,
      { -0.5f, 0.0f, -1.333f, 0.5f, 3.416f, 1.333f },
      "Door_Left", "Door_Right",
      { -0.5f, 0.0f, -1.333f, 0.5f, 3.416f, 0.0f },
      { -0.5f, 0.0f,  0.0f,   0.5f, 3.416f, 1.333f },
      true, 1.5707963f,
      "doors/door_open.wav", "doors/door_close.wav", "doors/door_locked.wav",
      "interact/servo_loop.wav", "interact/door_thunk.wav" },
};
constexpr uint32_t kDoorModelCount = (uint32_t)(sizeof(kDoorModels) / sizeof(kDoorModels[0]));

// Registry-model VALUE normalization (surface-library band; door_a's measured
// 0.42 lives in loadDoorMesh). meshy PBR ships near-honest albedos; the SciFi
// kits ship hot ones (every sibling measured so) — eyeball-pending values, both
// hue-preserving scales like the leaf/frame normalizations above.
constexpr float kModelValueScale[kDoorModelCount] = { 1.0f, 0.90f, 0.55f };
} // namespace

const DoorModelDef* doorModelDefs(uint32_t& count) { count = kDoorModelCount; return kDoorModels; }

uint32_t doorModelIndex(const char* key) {
    if (!key || !*key) return 0;
    for (uint32_t i = 0; i < kDoorModelCount; ++i)
        if (std::string_view(kDoorModels[i].key) == key) return i;
    return 0;   // unknown key -> the door_a default (never a build failure)
}

// ---------------------------------------------------------------------------
// Motion profile + per-floor style (see door.h for the derivation + the dt rule).
// ---------------------------------------------------------------------------
float doorEase(float u, float rampFrac) {
    if (!(u > 0.0f)) return 0.0f;          // also catches NaN
    if (u >= 1.0f)   return 1.0f;
    const float r = std::min(std::max(rampFrac, 0.0f), 0.5f);
    if (r <= 1e-4f)  return u;             // degenerate ramp: plain linear
    const float V = 1.0f / (1.0f - r);     // cruise velocity (profile area == 1)
    if (u <= r)          return V * u * u / (2.0f * r);
    if (u >= 1.0f - r) { const float w = 1.0f - u; return 1.0f - V * w * w / (2.0f * r); }
    return V * (r * 0.5f + (u - r));
}

uint32_t doorFloorForY(float worldY) {
    uint32_t best = 0;
    for (uint32_t i = 0; i < kDoorFloorCount; ++i)
        if (worldY >= kFloorBaseY[i] - 1.5f) best = i;   // 1.5 m slack: doorway Y is floor-ish
    return best;                                          // below B1 (deep zone/club) -> B1
}

const DoorStyle& doorStyleFor(uint32_t floorIdx) {
    return kDoorStyles[floorIdx < kDoorFloorCount ? floorIdx : kDoorFloorCount - 1];
}

uint32_t DoorSystem::add(const Door& d) {
    uint32_t i = (uint32_t)m_doors.size();
    m_doors.push_back(d);
    return i;
}

Door* DoorSystem::findByEntity(uint32_t entityId) {
    for (Door& d : m_doors)
        if (d.entity == entityId || d.entity2 == entityId) return &d;
    return nullptr;
}

const Door* DoorSystem::findByEntity(uint32_t entityId) const {
    for (const Door& d : m_doors)
        if (d.entity == entityId || d.entity2 == entityId) return &d;
    return nullptr;
}

// W2-A2: 3D door-sound emission at the door's body position. Silent when the host
// never wired audio (headless tests) or a WAV failed to load (clean machines).
void DoorSystem::playDoorSound(const Door& d, x3::audio::SoundHandle h, float vol) const {
    if (!m_audio || !h.valid()) return;
    m_audio->playSound3D(h, d.closedPos.x, d.closedPos.y, d.closedPos.z, vol, 1.0f);
}

// Per-model sound lookup (doors-pass): the door's registry row's handle when
// wireModelSounds() loaded one, else the global setAudio/setMotorAudio handle.
// which: 0=open 1=close 2=locked 3=servo 4=thunk.
x3::audio::SoundHandle DoorSystem::modelSnd(const Door& d, int which) const {
    if (d.modelIdx < (uint32_t)m_slots.size()) {
        const ModelSlot& ms = m_slots[d.modelIdx];
        const x3::audio::SoundHandle h =
            which == 0 ? ms.sOpen : which == 1 ? ms.sClose :
            which == 2 ? ms.sLocked : which == 3 ? ms.sServo : ms.sThunk;
        if (h.valid()) return h;
    }
    return which == 0 ? m_sndOpen : which == 1 ? m_sndClose :
           which == 2 ? m_sndLocked : which == 3 ? m_sndServo : m_sndThunk;
}

void DoorSystem::wireModelSounds(x3::audio::IAudioSystem* audio,
                                 const std::function<std::string(std::string_view)>& resolve) {
    if (!audio || !resolve) return;
    if (m_slots.size() < kDoorModelCount) m_slots.resize(kDoorModelCount);
    for (uint32_t i = 0; i < kDoorModelCount; ++i) {
        const DoorModelDef& def = kDoorModels[i];
        ModelSlot& ms = m_slots[i];
        ms.sOpen   = audio->load(resolve(def.sndOpen));
        ms.sClose  = audio->load(resolve(def.sndClose));
        ms.sLocked = audio->load(resolve(def.sndLocked));
        ms.sServo  = audio->load(resolve(def.sndServo));
        ms.sThunk  = audio->load(resolve(def.sndThunk));
    }
}

// ---- SERVO VOICE (door-mesh-swap audio). The ONLY two places a motor voice is
// created or destroyed. Both are idempotent, and both are called exclusively from
// DoorSystem::update() — the single function that can move a door OR settle it —
// so "a live loop" and "a moving slab" are the same condition by construction.
void DoorSystem::startMotor(Door& d) const {
    const x3::audio::SoundHandle servo = modelSnd(d, 3);
    if (!m_audio || !servo.valid()) return;
    if (d.motorLoop.valid()) return;          // already humming (mid-slide reversal)
    // 3D so the hum attenuates + pans against the listener for as long as it runs.
    // Closing runs a touch lower — the same servo winding down under gravity.
    const float pitch = (d.state == DoorState::Closing) ? 0.92f : 1.0f;
    d.motorLoop = m_audio->startLoop3D(servo, d.closedPos.x, d.closedPos.y,
                                       d.closedPos.z, 0.55f, pitch);
}

void DoorSystem::stopMotor(Door& d) const {
    if (!d.motorLoop.valid()) return;
    if (m_audio) m_audio->stopLoop(d.motorLoop);
    d.motorLoop = x3::audio::LoopHandle{};    // cleared even with audio gone: no stale handle
}

void DoorSystem::stopAllMotors() {
    for (Door& d : m_doors) stopMotor(d);
}

uint32_t DoorSystem::liveMotorCount() const {
    uint32_t n = 0;
    for (const Door& d : m_doors) if (d.motorLoop.valid()) ++n;
    return n;
}

// The legacy fire-and-forget open/close cue. Fired ONLY when no servo loop is
// wired: doors/door_open.wav is 2.19 s and door_close.wav 1.38 s against a ~1.0 s
// slide, so as a motion voice it ran on after the slab had stopped. With a servo
// loop present the motion is voiced by that loop (started/stopped in update()) and
// this stays silent; without one it is still better than a silent door.
void DoorSystem::playMotionCue(const Door& d, bool opening, float vol) const {
    if (modelSnd(d, 3).valid()) return;   // the loop owns the motion; no over-running one-shot
    playDoorSound(d, opening ? modelSnd(d, 0) : modelSnd(d, 1), vol);
}

bool DoorSystem::startOpening(Door& d) const {
    if (d.locked) { playDoorSound(d, modelSnd(d, 2), 0.55f); return false; }  // §6.4 + denied buzz
    if (d.state != DoorState::Closed) return false;
    d.state = DoorState::Opening;
    d.t = 0.0f;
    playMotionCue(d, true, 0.8f);
    return true;
}

bool DoorSystem::toggle(Door& d) const {
    switch (d.state) {
        case DoorState::Closed:
            if (d.locked) { playDoorSound(d, modelSnd(d, 2), 0.55f); return false; }  // §6.4
            d.state = DoorState::Opening;         // t is already 0
            playMotionCue(d, true, 0.8f);
            return true;
        case DoorState::Open:
            d.state = DoorState::Closing;         // t is already == duration
            playMotionCue(d, false, 0.8f);
            return true;
        case DoorState::Opening:
            d.state = DoorState::Closing;         // reverse mid-slide (keep t)
            playMotionCue(d, false, 0.6f);
            return true;
        case DoorState::Closing:
            // LOCK-BYPASS FIX (doors-pass): a door sealed mid-close (closeAndLock)
            // could be re-opened by E during the travel — the one path around the
            // lock. A locked, Closing door now refuses the reversal (denied buzz)
            // and finishes seating.
            if (d.locked) { playDoorSound(d, modelSnd(d, 2), 0.55f); return false; }
            d.state = DoorState::Opening;         // reverse mid-slide (keep t)
            playMotionCue(d, true, 0.6f);
            return true;
    }
    return false;
}

bool DoorSystem::tryDoorCode(const x3::phys::Vec3& eye, int code, float range) {
    const float r2 = range * range;
    int best = -1; float bestD2 = r2;
    for (uint32_t i = 0; i < (uint32_t)m_doors.size(); ++i) {
        const Door& d = m_doors[i];
        if (!d.locked || d.code == 0) continue;       // only locked, coded doors
        const float dx = eye.x - d.closedPos.x, dz = eye.z - d.closedPos.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = (int)i; }
    }
    if (best < 0) return false;
    Door& d = m_doors[(uint32_t)best];
    if (d.code != code) return false;                 // wrong code: stays locked
    unlock(d);                                         // clears locked (incl. the keycard gate)
    return startOpening(d);
}

Door* DoorSystem::nearestLockedDoor(const x3::phys::Vec3& eye, float range) {
    const float r2 = range * range;
    int best = -1; float bestD2 = r2;
    for (uint32_t i = 0; i < (uint32_t)m_doors.size(); ++i) {
        const Door& d = m_doors[i];
        if (!d.locked) continue;
        const float dx = eye.x - d.closedPos.x, dz = eye.z - d.closedPos.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = (int)i; }
    }
    return best < 0 ? nullptr : &m_doors[(uint32_t)best];
}

void DoorSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (dt <= 0.0f) return;
    for (Door& d : m_doors) {
        // Move the shared progress cursor and settle to the terminal states.
        if (d.state == DoorState::Opening) {
            d.t += dt;
            if (d.t >= d.duration) { d.t = d.duration; d.state = DoorState::Open; }
        } else if (d.state == DoorState::Closing) {
            d.t -= dt;
            if (d.t <= 0.0f) { d.t = 0.0f; d.state = DoorState::Closed; }
        } else {
            // Closed / Open: nothing to animate. Belt-and-braces — a door that
            // reached a terminal state by ANY path (a host poking `state`, a level
            // teardown) still surrenders its servo voice here, so a hum can never
            // survive a stopped slab.
            if (d.motorLoop.valid()) stopMotor(d);
            continue;
        }
        // ---- MOTOR AUDIO: the voice's lifetime IS the motion's lifetime. We are
        // inside the "this door moved this frame" branch, and the two cursor tests
        // above are the ONLY way a door leaves Opening/Closing, so the start and
        // the stop below bracket the motion exactly — at any duration, any dt.
        const bool stillMoving = (d.state == DoorState::Opening || d.state == DoorState::Closing);
        if (stillMoving) {
            startMotor(d);                        // idempotent: no restart mid-slide
        } else {
            stopMotor(d);                         // the frame the slab SEATS
            playDoorSound(d, modelSnd(d, 4), 0.65f);  // short transient, fires once
        }
        // Normalized cursor -> EASED displacement (accel / cruise / decel; door.h).
        // dt entered only through the `d.t +/- dt` integration above, so this is
        // frame-rate independent by construction (repo HARD RULE: never per-frame
        // motion). drawMeshes() runs the identical call on the identical cursor.
        const float uRaw = d.duration > 0.0f ? (d.t / d.duration) : 1.0f;
        const float u    = doorEase(uRaw);
        // Sweep closed -> open and drive the body.
        x3::phys::Vec3 p{
            d.closedPos.x + (d.openPos.x - d.closedPos.x) * u,
            d.closedPos.y + (d.openPos.y - d.closedPos.y) * u,
            d.closedPos.z + (d.openPos.z - d.closedPos.z) * u,
        };
        physics.setBodyPosition(d.body, p);
        // Refresh the Entity transform translation so the render mesh follows.
        // (Scene::update would also do this, but we update here so the door
        // tracks even if the caller skips a scene sync this frame.)
        if (d.entity != kNoLink && d.entity < scene.size()) {
            Entity& e = scene.get(d.entity);
            e.transform[12] = p.x;
            e.transform[13] = p.y;
            e.transform[14] = p.z;
            e.transform[15] = 1.0f;
        }
        // SECOND PANEL (two-panel floor hatch): slide it the opposite way on the
        // SAME cursor so the two halves part from / close to the centre together.
        if (d.body2.valid()) {
            x3::phys::Vec3 p2{
                d.closedPos2.x + (d.openPos2.x - d.closedPos2.x) * u,
                d.closedPos2.y + (d.openPos2.y - d.closedPos2.y) * u,
                d.closedPos2.z + (d.openPos2.z - d.closedPos2.z) * u,
            };
            physics.setBodyPosition(d.body2, p2);
            if (d.entity2 != kNoLink && d.entity2 < scene.size()) {
                Entity& e2 = scene.get(d.entity2);
                e2.transform[12] = p2.x;
                e2.transform[13] = p2.y;
                e2.transform[14] = p2.z;
                e2.transform[15] = 1.0f;
            }
        }
    }
}

void DoorSystem::loadDoorMesh(x3::rhi::IRenderDevice& device, std::string_view convertedGlbDir) {
    if (m_meshOk || m_loader) return;   // already loaded (or already tried)

    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(convertedGlbDir, 0)) {
        x3::logWarn("[door] mountDir failed: " + std::string(convertedGlbDir) +
                    " — keeping graybox door box");
        return;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
    m_doorModel = m_loader->load(kDoorGlbRel);
    if (m_doorModel.ok) {
        m_doorDrawables = x3::asset::makeDrawables(m_doorModel);
        m_meshOk = !m_doorDrawables.empty();
        // B5 — SM_Door_A SHIPS A NEAR-WHITE ALBEDO, and DoorSystem owns this mesh, so the
        // defect is GAME-WIDE. MEASURED off T_Door_A_Dif (not guessed): the door BODY is
        // 42% of the texels at mean sRGB 227 -> a LINEAR ALBEDO of 0.768. Fresh snow is
        // ~0.85. A painted institutional door is ~0.30. A 0.77-albedo door reflects 77% of
        // every photon that reaches it, so it scorches hot against any honest rig while the
        // ~0.25-albedo walls beside it sit correctly dark — the same over-unity crutch as
        // the 1.08 cot and the black rifthub tube (docs/KNOWN_BUGS.md, "VALUE, NOT LUMENS").
        //
        // Renormalize with glTF's OWN albedo multiplier (baseColor = factor x texture, in
        // LINEAR space) rather than rewriting the GLB: the .glb is Git-LFS tracked and the
        // asset itself is not corrupt — our USE of it was never normalized. 0.768 x 0.42 =
        // 0.32 linear (~0.60 sRGB), the exact value that fixed the rifthub tube.
        //
        // NOTE this is a VALUE fix, not a hue fix. It is NOT what made the door read pink —
        // that was a misplaced red light (see cell_dressing.cpp). Proof: scaling the albedo
        // moved the door's brightness but its R-G held at +57. Fix the light for hue, the
        // albedo for value; do not confuse the two.
        constexpr float kDoorAlbedoScale = 0.42f;
        for (auto& dr : m_doorDrawables)
            for (int k = 0; k < 3; ++k) dr.baseColorFactor[k] *= kDoorAlbedoScale;
    }
    if (m_meshOk)
        x3::logInfo("[door] loaded " + std::string(kDoorGlbRel) + " — " +
                    std::to_string(m_doorDrawables.size()) + " drawable prim(s)");
    else
        x3::logWarn("[door] FAILED to load " + std::string(kDoorGlbRel) +
                    " (graybox door box kept)");

    // ---- SM_DoorFrame_A: the real frame that SEATS the leaf in its opening (LAW 1
    // "one wall, one hole, one frame seated in that hole"; LAW 4 "prefer a frame
    // around every opening"). Same asset cell_dressing/env_art already use, loaded
    // through the same source so it costs one extra model, not a second kit mount.
    // A frame failure is non-fatal: the leaf still draws, the doorway just loses
    // its trim.
    m_frameModel = m_loader->load(kFrameGlbRel);
    if (m_frameModel.ok) {
        m_frameDrawables = x3::asset::makeDrawables(m_frameModel);
        m_frameOk = !m_frameDrawables.empty();
    }
    x3::logInfo(std::string("[door] frame ") + kFrameGlbRel + (m_frameOk ? " loaded" : " NOT loaded (doorways untrimmed)"));

    // LAW 1 seal: build the shared unit-cube backing slab (see door.h m_fillMesh).
    if (m_meshOk && !m_fillMesh.valid()) {
        x3::prims::PrimMesh geo = x3::prims::makeBox(0.5f, 0.5f, 0.5f, 0, 0, 0, 1.0f);
        m_fillMesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                       geo.index.data(), (uint32_t)geo.index.size());
        // Matte dielectric MR texel (glTF MR: G=rough 0.85, B=metal 0). Reusing the
        // LEAF's MR map here was measured blown-white: its metal zones sampled with
        // unit-cube UVs made the slab a mirror aimed at the room probe (L5's WHITE
        // case). A door back-plate is painted steel, not chrome.
        const uint8_t mr[4] = { 0, 217, 0, 255 };
        m_fillMr = device.createTexture(mr, 1, 1, false);
    }
}

// ---------------------------------------------------------------------------
// Registry model slots (doors-pass; see door.h).
// ---------------------------------------------------------------------------
DoorSystem::ModelSlot& DoorSystem::slot(uint32_t idx) {
    if (m_slots.size() < kDoorModelCount) m_slots.resize(kDoorModelCount);
    return m_slots[idx < kDoorModelCount ? idx : 0];
}

bool DoorSystem::hasModelMesh(uint32_t modelIdx) const {
    if (modelIdx == 0) return m_meshOk;
    return modelIdx < (uint32_t)m_slots.size() && m_slots[modelIdx].ok;
}

bool DoorSystem::loadModelMesh(x3::rhi::IRenderDevice& device, uint32_t modelIdx) {
    if (modelIdx >= kDoorModelCount) modelIdx = 0;
    // Row 0 first: door_a's dedicated load also creates the shared loader +
    // mounts the converted-GLB root (idempotent).
    loadDoorMesh(device, kDoorGlbDir());
    if (modelIdx == 0) return m_meshOk;
    ModelSlot& ms = slot(modelIdx);
    if (ms.tried) return ms.ok;
    ms.tried = true;
    if (!m_loader || !m_assets) return false;   // mount failed in loadDoorMesh
    const DoorModelDef& def = kDoorModels[modelIdx];
    if (def.rootKind == 1)
        m_assets->mountDir(assetRoot() + "/meshy", 1);   // idempotent secondary root
    ms.model = m_loader->load(def.glbRel);
    if (!ms.model.ok) {
        x3::logWarn(std::string("[door] registry model '") + def.key +
                    "' FAILED to load (" + def.glbRel + ") — graybox panels kept");
        return false;
    }
    // Partition by glTF node name: the two moving panels vs the static shell.
    // rawPanelSpace models keep ONLY their panels (the other nodes carry the
    // broken TRS / a fused wall the canon level already builds), drawn in
    // MESH-local space (identity nodeTransform).
    std::vector<std::string> names;
    std::vector<x3::asset::ModelDrawable> all = x3::asset::makeDrawablesNamed(ms.model, names);
    auto starts = [](const std::string& s, const char* p) {
        return p && s.rfind(p, 0) == 0;
    };
    constexpr float kIdent[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    for (size_t i = 0; i < all.size(); ++i) {
        x3::asset::ModelDrawable dr = all[i];
        const std::string nm = i < names.size() ? names[i] : std::string();
        const bool isA = starts(nm, def.panelANode);
        const bool isB = !isA && starts(nm, def.panelBNode);
        if (def.rawPanelSpace) {
            if (!isA && !isB) continue;                       // broken-TRS shell: skip
            for (int k = 0; k < 16; ++k) dr.nodeTransform[k] = kIdent[k];
        }
        // Hue-preserving VALUE normalization (band discipline; see table above).
        for (int k = 0; k < 3; ++k) dr.baseColorFactor[k] *= kModelValueScale[modelIdx];
        if (isA)      ms.panelADr.push_back(dr);
        else if (isB) ms.panelBDr.push_back(dr);
        else          ms.fixedDr.push_back(dr);
    }
    ms.ok = !ms.panelADr.empty() && !ms.panelBDr.empty();
    x3::logInfo(std::string("[door] registry model '") + def.key + "' " +
                (ms.ok ? "loaded" : "MISSING its panel nodes (graybox kept)") +
                " (fixed " + std::to_string(ms.fixedDr.size()) +
                ", panelA " + std::to_string(ms.panelADr.size()) +
                ", panelB " + std::to_string(ms.panelBDr.size()) + ")");
    return ms.ok;
}

void DoorSystem::drawMeshes(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    // door_a's shared leaf AND any loaded registry model can draw; with neither,
    // every door keeps its (still-visible) graybox boxes.
    bool anySlotOk = false;
    for (const ModelSlot& s : m_slots) if (s.ok) { anySlotOk = true; break; }
    if (!m_meshOk && !anySlotOk) return;

    // Uniform scale to fit each door's opening height: the GLB slab is 3.50 m tall;
    // scale so it stands at the door's clear-passage height (~2.1 m). The width then
    // becomes ~1.41 m (a touch wider than the 1.2 m opening — reads as a real door
    // straddling the frame, like env_art's 2.0 m door FRAME). Thickness ~0.13 m.
    const float ax  = gcx(kDoorAabb);   // GLB anchor X (slab center, ~-3.70)
    const float az  = gcz(kDoorAabb);   // GLB anchor Z (~0)
    const float ay  = kDoorAabb.miny;   // GLB anchor Y (slab BOTTOM, ~0.054)

    for (const Door& d : m_doors) {
        if (d.floorHatch) continue;   // horizontal hatch draws its own graybox box
        // W2-A2: room-visibility gate — walls cull per-room; without this, slabs in
        // culled rooms drew anyway and floated in void from outside-shell sightlines.
        if (m_visQuery && !m_visQuery(d)) continue;
        // CURRENT world center of this door (the slide animation lerps closed->open
        // by the same factor DoorSystem::update() uses). closedPos/openPos are the
        // body CENTER positions; the slab bottom is center.y - height/2.
        // Slide factor from the shared cursor — works for Opening, Closing AND
        // the terminal Open/Closed states (Open pins to 1, Closed to 0).
        // EASED, exactly like DoorSystem::update(): same doorEase() on the same
        // cursor, so the drawn leaf can never drift from its collision box.
        const float uRaw = (d.state == DoorState::Open)
            ? 1.0f
            : (d.duration > 0.0f ? std::min(std::max(d.t / d.duration, 0.0f), 1.0f) : 0.0f);
        const float u = doorEase(uRaw);
        const float cxw = d.closedPos.x + (d.openPos.x - d.closedPos.x) * u;
        const float cyw = d.closedPos.y + (d.openPos.y - d.closedPos.y) * u;
        const float czw = d.closedPos.z + (d.openPos.z - d.closedPos.z) * u;
        const float bottomY = cyw - d.height * 0.5f;   // floor-level bottom of the slab

        // SPLIT-MODEL doors (registry rows > 0): the STATIC doorway centre is the
        // midpoint of the two panels' closed positions (each panel is off-centre
        // by half the opening); door_a's closedPos IS the doorway centre.
        const bool split = d.modelIdx != 0 && d.body2.valid() && !d.floorHatch;
        const float dwx = split ? (d.closedPos.x + d.closedPos2.x) * 0.5f : d.closedPos.x;
        const float dwz = split ? (d.closedPos.z + d.closedPos2.z) * 0.5f : d.closedPos.z;

        const float s = d.height / kDoorGlbH;          // uniform scale (height fit)
        // Orient the slab to the host wall: GLB is wide in X / thin in Z by default.
        //   AlongZ (axis 0): wall plane x=const, opening runs along Z -> yaw 90deg.
        //   AlongX (axis 1): wall plane z=const, opening runs along X -> yaw 0.
        const float yaw = (d.axis == 0) ? (3.14159265358979f * 0.5f) : 0.0f;
        const float cs = std::cos(yaw), sn = std::sin(yaw);

        // PER-FLOOR VARIANT (door.h DoorStyle): one table lookup, no branching.
        const DoorStyle& st = doorStyleFor(d.floorStyle);

        // ---- SM_DoorFrame_A, seated STATIC in the opening (it does not slide with
        // the leaf). Fitted NON-UNIFORMLY so the trim lands ON the cut instead of
        // overhanging it: the 6.25 x 4.446 showroom frame's aspect (1.41) cannot
        // match a 1.6 x 2.2 doorway (0.73) under a uniform scale, and both existing
        // precedents had to pick a side (env_art height-starved it at 2.0 m wide;
        // cell_dressing height-fit it and ate a 3.6 m overhang). Fitting each axis
        // separately gives a jamb that straddles the opening by a fixed bezel and a
        // header that clears the lintel — LAW 1's "frame overlaps the cut by its
        // bezel", no floating, no guesswork. The frame is a rectilinear trim ring,
        // so per-axis scale reads correctly.
        if (d.withFrame && m_frameOk && !d.floorHatch) {
            const float frBezel = 0.35f;                                   // trim past each jamb
            const float sRun = (d.halfWidth * 2.0f + frBezel * 2.0f) / kFrameGlbW;
            const float sYf  = (d.height + 0.30f) / kFrameGlbH;            // + header over the lintel
            const float sThk = 0.30f / kFrameGlbT;                         // straddles the 0.2 m wall
            // Anchor the frame's VISIBLE jamb base (not its buried sill) on the deck,
            // centred on the STATIC doorway (closedPos), never the sliding leaf.
            const float fax = gcx(kFrameAabb), faz = gcz(kFrameAabb);
            const float fay = kFrameAabb.miny + kFrameSillGlb;
            const float sx0 = dwx, sz0 = dwz;                              // static doorway centre
            const float sy0 = d.closedPos.y - d.height * 0.5f;             // doorway floor
            float fmt[16];
            fmt[0]=cs*sRun; fmt[1]=0;    fmt[2]=-sn*sRun; fmt[3]=0;
            fmt[4]=0;       fmt[5]=sYf;  fmt[6]=0;        fmt[7]=0;
            fmt[8]=sn*sThk; fmt[9]=0;    fmt[10]=cs*sThk; fmt[11]=0;
            fmt[12]=sx0 - (cs*fax*sRun + sn*faz*sThk);
            fmt[13]=sy0 - fay*sYf;
            fmt[14]=sz0 - (-sn*fax*sRun + cs*faz*sThk);
            fmt[15]=1.0f;
            for (const auto& fr : m_frameDrawables) {
                float fin[16];
                x3::asset::mulMat4(fmt, fr.nodeTransform, fin);
                // Same VALUE normalization the leaf takes (the kit ships hot albedos),
                // then the floor's frame hue on top.
                constexpr float kFrameValue = 0.46f;
                const float bc[4] = { fr.baseColorFactor[0] * kFrameValue * st.frame[0],
                                      fr.baseColorFactor[1] * kFrameValue * st.frame[1],
                                      fr.baseColorFactor[2] * kFrameValue * st.frame[2],
                                      fr.baseColorFactor[3] };
                const float emis[4] = { fr.emissiveFactor[0], fr.emissiveFactor[1],
                                        fr.emissiveFactor[2], 1.0f };
                device.drawMeshPBR(frame,
                                   x3::rhi::MeshHandle{ fr.meshId },
                                   x3::rhi::TextureHandle{ fr.baseColorTexId },
                                   x3::rhi::TextureHandle{ fr.normalTexId },
                                   x3::rhi::TextureHandle{ fr.mrTexId },
                                   bc, emis, fin, fr.alphaMask, fr.alphaBlend,
                                   x3::rhi::TextureHandle{ fr.emissiveTexId },
                                   x3::rhi::TextureHandle{}, 1.0f, 0.0f, 0.05f,
                                   0.0f, 1.0f, 0.0f,
                                   fr.metallicFactor, fr.roughnessFactor);
            }
        }

        // ---- SPLIT-MODEL DRAW (registry rows > 0): the static shell at the
        // doorway + each panel offset by ITS collision body's current travel —
        // the delta is computed from the SAME eased cursor update() drives the
        // bodies with, so the drawn panels can never drift from collision. ----
        if (split) {
            const DoorModelDef& def = kDoorModels[d.modelIdx < kDoorModelCount ? d.modelIdx : 0];
            const ModelSlot&    ms  = m_slots[d.modelIdx];
            if (!ms.ok) continue;                      // graybox panels render instead
            // Model-local axes: width runs along local X (preYaw 0) or local Z
            // (preYaw 90 deg maps it onto the run). Per-axis fit (rectilinear
            // panels read correctly under it — the frame-fit precedent).
            const bool  zRun   = def.preYaw > 0.5f;
            const float modelW = zRun ? (def.aabb[5] - def.aabb[2]) : (def.aabb[3] - def.aabb[0]);
            const float modelT = zRun ? (def.aabb[3] - def.aabb[0]) : (def.aabb[5] - def.aabb[2]);
            const float modelH = def.aabb[4] - def.aabb[1];
            const float sRun = (d.halfWidth * 2.0f + 0.30f) / modelW;   // straddles the jambs
            const float sY   = d.height / modelH;
            const float sThk = std::min(sRun, 0.30f / modelT);          // never a metre-thick slab
            const float sx = zRun ? sThk : sRun;
            const float sy = sY;
            const float sz = zRun ? sRun : sThk;
            const float ax2 = (def.aabb[0] + def.aabb[3]) * 0.5f;       // local bottom-centre anchor
            const float ay2 =  def.aabb[1];
            const float az2 = (def.aabb[2] + def.aabb[5]) * 0.5f;
            const float totYaw = yaw + def.preYaw;
            const float c2 = std::cos(totYaw), s2 = std::sin(totYaw);
            const float doorFloorY = d.closedPos.y - d.height * 0.5f;
            float M[16];
            M[0]=c2*sx; M[1]=0;  M[2]=-s2*sx; M[3]=0;
            M[4]=0;     M[5]=sy; M[6]=0;      M[7]=0;
            M[8]=s2*sz; M[9]=0;  M[10]=c2*sz; M[11]=0;
            M[12]=dwx - (c2*ax2*sx + s2*az2*sz);
            M[13]=doorFloorY - ay2*sy;
            M[14]=dwz - (-s2*ax2*sx + c2*az2*sz);
            M[15]=1.0f;
            // Panel travel deltas from the shared cursor (== the bodies' motion).
            const float dA[3] = { (d.openPos.x  - d.closedPos.x)  * u,
                                  (d.openPos.y  - d.closedPos.y)  * u,
                                  (d.openPos.z  - d.closedPos.z)  * u };
            const float dB[3] = { (d.openPos2.x - d.closedPos2.x) * u,
                                  (d.openPos2.y - d.closedPos2.y) * u,
                                  (d.openPos2.z - d.closedPos2.z) * u };
            auto drawSet = [&](const std::vector<x3::asset::ModelDrawable>& set,
                               const float* delta) {
                float Mt[16];
                for (int k = 0; k < 16; ++k) Mt[k] = M[k];
                if (delta) { Mt[12] += delta[0]; Mt[13] += delta[1]; Mt[14] += delta[2]; }
                for (const auto& dr : set) {
                    float fin[16];
                    x3::asset::mulMat4(Mt, dr.nodeTransform, fin);
                    const float emis[4] = { dr.emissiveFactor[0], dr.emissiveFactor[1],
                                            dr.emissiveFactor[2], 1.0f };
                    device.drawMeshPBR(frame,
                                       x3::rhi::MeshHandle{ dr.meshId },
                                       x3::rhi::TextureHandle{ dr.baseColorTexId },
                                       x3::rhi::TextureHandle{ dr.normalTexId },
                                       x3::rhi::TextureHandle{ dr.mrTexId },
                                       dr.baseColorFactor, emis, fin,
                                       dr.alphaMask, dr.alphaBlend,
                                       x3::rhi::TextureHandle{ dr.emissiveTexId },
                                       x3::rhi::TextureHandle{}, 1.0f, 0.0f, 0.05f,
                                       0.0f, 1.0f, 0.0f,
                                       dr.metallicFactor, dr.roughnessFactor);
                }
            };
            drawSet(ms.fixedDr, nullptr);
            drawSet(ms.panelADr, dA);
            drawSet(ms.panelBDr, dB);
            continue;   // door_a leaf / backing slab / signage do not apply
        }
        if (!m_meshOk) continue;   // door_a leaf missing: graybox box renders instead

        // Column-major TRS: world = T(c) * R_y(yaw) * S(s) * T(-anchor), placing the
        // GLB anchor (ax, ay, az) at the world bottom-center (cxw, bottomY, czw).
        float m[16];
        m[0]=cs*s;  m[1]=0;   m[2]=-sn*s; m[3]=0;
        m[4]=0;     m[5]=s;   m[6]=0;     m[7]=0;
        m[8]=sn*s;  m[9]=0;   m[10]=cs*s; m[11]=0;
        const float rpx = (cs*ax + sn*az) * s;
        const float rpy = (ay) * s;
        const float rpz = (-sn*ax + cs*az) * s;
        m[12]=cxw - rpx; m[13]=bottomY - rpy; m[14]=czw - rpz; m[15]=1.0f;

        for (const auto& dr : m_doorDrawables) {
            float fin[16];
            x3::asset::mulMat4(m, dr.nodeTransform, fin);
            // B5 / THE PATTERN: the door used to draw through the 5-arg drawMesh() — the
            // NON-PBR path. makeDrawables() had already resolved its NORMAL and MR maps and
            // this call THREW THEM AWAY. Two consequences, both visible in CELL_4up.png:
            //   * no normal map -> the slab's panel/rivet detail shaded dead flat, and
            //   * the non-PBR branch takes the UNNORMALIZED Lambert (albedo x N.L), so the
            //     door shaded ~PI x BRIGHTER than every GLB standing next to it (R1, 5c35d65).
            // Against the honest cell rig that overshoot is exactly what scorched a near-white
            // slab into salmon. Draw it like every other GLB: same PBR path, same maps.
            // ModelDrawable::emissiveFactor is float[3]; drawMeshPBR takes float[4].
            const float emis[4] = { dr.emissiveFactor[0], dr.emissiveFactor[1],
                                    dr.emissiveFactor[2], 1.0f };
            // QA MAINLEVEL SWEEP D16: the leaf's albedo means ~0.78 LINEAR — double the
            // facility's surface-library VALUE band ceiling (0.40, surface_library.h).
            // Every dressed wall beside it is clamped into that band, so under an honest
            // key (Medical Bay's white rig) the leaves were the ONE blown-white thing in
            // the room (sweep2/F1_Medical_Bay_a). Same hue-preserving VALUE normalization
            // the library applies: scale value only, 0.40 / 0.78 ≈ 0.51.
            constexpr float kLeafValueTint = 0.51f;
            // PER-FLOOR HUE on top of that VALUE normalization (st.leaf channels are
            // all <= 1, so the band ceiling still holds — this shifts hue, it never
            // raises brightness).
            const float bc[4] = { dr.baseColorFactor[0] * kLeafValueTint * st.leaf[0],
                                  dr.baseColorFactor[1] * kLeafValueTint * st.leaf[1],
                                  dr.baseColorFactor[2] * kLeafValueTint * st.leaf[2],
                                  dr.baseColorFactor[3] };
            device.drawMeshPBR(frame,
                               x3::rhi::MeshHandle{ dr.meshId },
                               x3::rhi::TextureHandle{ dr.baseColorTexId },
                               x3::rhi::TextureHandle{ dr.normalTexId },
                               x3::rhi::TextureHandle{ dr.mrTexId },
                               bc,
                               emis,
                               fin,
                               dr.alphaMask, dr.alphaBlend,
                               x3::rhi::TextureHandle{ dr.emissiveTexId },
                               x3::rhi::TextureHandle{}, 1.0f, 0.0f, 0.05f,
                               0.0f, 1.0f, dr.foliage,
                               dr.metallicFactor, dr.roughnessFactor);
        }

        // LAW 1 SEAL (QA mainlevel sweep): the pentagon leaf covers only ~72% of the
        // rectangular wall cut (top-right triangle + ~10 cm side margins are open air),
        // so a Closed door leaked a fog-void sightline into the PVS-culled neighbour.
        // Draw the opaque backing slab: full opening + bezel, sliding with the leaf.
        // It is 0.04 m thin, centred on the same plane, so it hides INSIDE the leaf's
        // 0.13 m body wherever the leaf exists and shows as a dark back-plate through
        // the notch. Dark institutional steel in the honest value band; the leaf's MR
        // map rides along so the PBR path keeps a real roughness response (L4 rule:
        // a missing MR texel silently demotes the draw to the unlit emissive path).
        if (m_fillMesh.valid() && !m_doorDrawables.empty()) {
            const float fw = d.halfWidth * 2.0f + 0.16f;  // cut width + bezel into the jambs
            const float fh = d.height + 0.10f;            // covers the head seam
            const float ft = 0.04f;
            const float sxw = (d.axis == 0) ? ft : fw;    // axis 0: wall plane x=const
            const float szw = (d.axis == 0) ? fw : ft;
            float fm[16] = { sxw, 0, 0, 0,   0, fh, 0, 0,   0, 0, szw, 0,
                             cxw, bottomY - 0.03f + fh * 0.5f, czw, 1.0f };
            const float steel[4] = { 0.16f, 0.17f, 0.19f, 1.0f };
            const float noEmis[4] = { 0, 0, 0, 1 };
            device.drawMeshPBR(frame, m_fillMesh,
                               x3::rhi::TextureHandle{},   // flat dark albedo (factor below)
                               x3::rhi::TextureHandle{},   // no normal map
                               m_fillMr,                   // matte dielectric MR texel
                               steel, noEmis, fm);

            // ---- PER-FLOOR SIGNAGE BAND. A thin emissive strip across the door
            // head in the floor's ladder colour — the cheapest, most readable
            // per-floor tell there is (you see the colour down a dark hall before
            // you can resolve the leaf). It RIDES the leaf (bottomY is the animated
            // slab bottom), so the light travels up with the door.
            //
            // It doubles as the LIVE readout for the existing keycard/keypad gate:
            // `locked` burns RED on every floor and flips to the floor colour the
            // instant unlock()/tryDoorCode() clears the flag. No new lock state —
            // it reads Door::locked, which the card/code path already owns.
            const float* sc = d.locked ? kLockedSign : st.sign;
            const float bandW = d.halfWidth * 2.0f * 0.66f;
            const float bandH = 0.09f;
            const float bandT = 0.02f;
            const float bandY = bottomY + d.height * 0.80f;
            const float bandOff = 0.085f;    // just proud of the 0.13 m leaf, both faces
            const float bsx = (d.axis == 0) ? bandT : bandW;
            const float bsz = (d.axis == 0) ? bandW : bandT;
            // VALUE, NOT LUMENS (docs/KNOWN_BUGS.md): the first pass drove this at
            // 2.2 and the band clipped to WHITE in the capture — the hue, which is
            // the entire point of a per-floor tell, was destroyed by the overdrive.
            // 0.85 keeps it clearly a lit strip while its colour survives tonemap.
            const float bandEmis[4] = { sc[0] * 0.85f, sc[1] * 0.85f, sc[2] * 0.85f, 1.0f };
            const float bandAlb[4]  = { sc[0] * 0.10f, sc[1] * 0.10f, sc[2] * 0.10f, 1.0f };
            for (int side = -1; side <= 1; side += 2) {   // readable from BOTH rooms
                const float ox = (d.axis == 0) ? bandOff * (float)side : 0.0f;
                const float oz = (d.axis == 0) ? 0.0f : bandOff * (float)side;
                float bm[16] = { bsx, 0, 0, 0,  0, bandH, 0, 0,  0, 0, bsz, 0,
                                 cxw + ox, bandY, czw + oz, 1.0f };
                device.drawMeshPBR(frame, m_fillMesh,
                                   x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                   m_fillMr, bandAlb, bandEmis, bm);
            }
        }
    }
}

Door* pickAimedDoor(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, float maxDist,
                    Scene& scene, DoorSystem& doors, x3::phys::IPhysicsWorld& physics) {
    // Ray against static geometry (walls, button, door are all Static-layer).
    x3::phys::RayHit hit = physics.rayCast(eye, dir, maxDist, x3::phys::Layer::Static);
    if (!hit.hit || !hit.body.valid()) return nullptr;

    uint32_t ent = scene.entityForBody(hit.body);
    if (ent == kNoLink || ent >= scene.size()) return nullptr;

    const Entity& e = scene.get(ent);

    // Aim at a wall BUTTON linked to a door...
    if (e.tag == (uint32_t)Tag::Button) {
        if (e.link == kNoLink || e.link >= scene.size()) return nullptr;
        return doors.findByEntity(e.link);
    }
    // ...OR aim directly at the DOOR slab itself (intuitive "open/close the door").
    if (e.tag == (uint32_t)Tag::Door)
        return doors.findByEntity(ent);
    return nullptr;
}

bool tryUse(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, float maxDist,
            Scene& scene, DoorSystem& doors, x3::phys::IPhysicsWorld& physics) {
    Door* door = pickAimedDoor(eye, dir, maxDist, scene, doors, physics);
    return door ? doors.toggle(*door) : false;   // open if closed, close if open
}

uint32_t buildDoorAndButton(Scene& scene, DoorSystem& doors,
                            x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& physics) {
    // ---- Door (fills the doorway gap when closed) ----
    const x3::phys::Vec3 doorClosed{ 0.0f, kDoorCenterY, kDoorEdgeZ };
    const x3::phys::Vec3 doorOpen{ doorClosed.x, doorClosed.y + kSlideUp, doorClosed.z };

    uint32_t doorEntId;
    {
        // Render mesh authored centered at the body origin (NOT world-baked) so
        // the Entity transform translation drives its position as the body moves.
        x3::prims::PrimMesh geo = x3::prims::makeBox(kDoorHalfX, kDoorHalfY, kDoorHalfZ,
                                                     0.0f, 0.0f, 0.0f, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        // Distinct door look: a solid orange-red tint (no texture -> flat color).
        e.baseColor[0] = 0.85f; e.baseColor[1] = 0.30f; e.baseColor[2] = 0.18f; e.baseColor[3] = 1.0f;
        e.tag = (uint32_t)Tag::Door;
        // Static body (mass 0): blocks the character while closed, repositioned
        // via setBodyPosition while opening. Box half-extents == render extents.
        e.body = physics.addBox(x3::phys::Vec3{kDoorHalfX, kDoorHalfY, kDoorHalfZ},
                                doorClosed, 0.0f, x3::phys::Layer::Static);
        // Authored transform translation = closed position (Scene::update / the
        // door system overwrite this each frame from the body).
        e.transform[12] = doorClosed.x;
        e.transform[13] = doorClosed.y;
        e.transform[14] = doorClosed.z;
        doorEntId = scene.add(e);
    }

    Door d;
    d.entity    = doorEntId;
    d.body      = scene.get(doorEntId).body;
    d.closedPos = doorClosed;
    d.openPos   = doorOpen;
    d.duration  = kOpenDuration;
    d.state     = DoorState::Closed;
    doors.add(d);

    // ---- Button (small box on the right wall segment, beside the doorway) ----
    // Right +Z wall segment spans x in [+0.6, +8.0] at z=8.0; mount the button
    // on its inner face (z just inside the room) at a comfy 1.3 m height.
    const float kBtnHalf = 0.12f;
    const x3::phys::Vec3 btnPos{ 1.1f, 1.3f, kDoorEdgeZ - kDoorHalfZ - kBtnHalf };
    uint32_t btnEntId;
    {
        x3::prims::PrimMesh geo = x3::prims::makeBox(kBtnHalf, kBtnHalf, kBtnHalf,
                                                     0.0f, 0.0f, 0.0f, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        // Distinct button look: bright cyan-green.
        e.baseColor[0] = 0.20f; e.baseColor[1] = 0.85f; e.baseColor[2] = 0.55f; e.baseColor[3] = 1.0f;
        e.tag  = (uint32_t)Tag::Button;
        e.link = doorEntId;      // link the button to its door
        // Small static collision box so the use-ray can hit it.
        e.body = physics.addBox(x3::phys::Vec3{kBtnHalf, kBtnHalf, kBtnHalf},
                                btnPos, 0.0f, x3::phys::Layer::Static);
        e.transform[12] = btnPos.x;
        e.transform[13] = btnPos.y;
        e.transform[14] = btnPos.z;
        btnEntId = scene.add(e);
    }

    x3::logInfo("buildDoorAndButton: door entity " + std::to_string(doorEntId) +
                " + button entity " + std::to_string(btnEntId) +
                " (button links to door)");
    return btnEntId;
}

// ---------------------------------------------------------------------------
// Two-panel flush floor hatch (blast-door / iris). See door.h.
// ---------------------------------------------------------------------------
uint32_t buildFloorHatch(Scene& scene, DoorSystem& doors,
                         x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics,
                         const DoorSpec& spec) {
    const float hw = spec.halfWidth;          // half the SQUARE opening (covers full Z)
    const float ht = spec.thickness * 0.5f;   // panel Y half-thickness
    const float c  = spec.doorwayCenter.x;    // opening centre X
    const float cy = spec.doorwayCenter.y;    // floor top Y (panels sit flush BELOW this)
    const float cz = spec.doorwayCenter.z;    // opening centre Z

    // Each panel covers half the opening in X (half-extent hw*0.5) and the full
    // depth in Z (half-extent hw). Top FLUSH with the floor: panel centre at
    // cy - ht (so the top surface is at cy, matching the surrounding floor slab).
    const float panelHalfX = hw * 0.5f;
    const x3::phys::Vec3 panelHalf{ panelHalfX, ht, hw };
    const float panelY = cy - ht;

    // Closed: panel 1 fills the -X half (centre at c - panelHalfX), panel 2 the +X
    // half (centre at c + panelHalfX) — together they cover [c-hw, c+hw] flush.
    const x3::phys::Vec3 closed1{ c - panelHalfX, panelY, cz };
    const x3::phys::Vec3 closed2{ c + panelHalfX, panelY, cz };
    // Open: each panel slides OUTWARD by the full opening width (2*hw) so each
    // clears its half and the whole [c-hw, c+hw] X span is open — drop-through.
    const x3::phys::Vec3 open1{ closed1.x - 2.0f * hw, panelY, cz };
    const x3::phys::Vec3 open2{ closed2.x + 2.0f * hw, panelY, cz };

    // Panel material: the cell floor texture/tint when supplied (flush + textured),
    // else the steel `tint`. Use a per-panel render mesh authored centred at the
    // body origin so the Entity transform drives its slide.
    const bool useFloor = spec.floorTex.valid();
    const float* col = useFloor ? spec.floorTint : spec.tint;

    auto buildPanel = [&](const x3::phys::Vec3& closed) -> uint32_t {
        x3::prims::PrimMesh geo = x3::prims::makeBox(panelHalf.x, panelHalf.y, panelHalf.z, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        if (useFloor) e.tex = spec.floorTex;
        e.baseColor[0]=col[0]; e.baseColor[1]=col[1]; e.baseColor[2]=col[2]; e.baseColor[3]=col[3];
        e.tag = (uint32_t)Tag::Door;
        e.visible = true;                       // the flush panels ARE the visual (no GLB)
        e.body = physics.addBox(panelHalf, closed, 0.0f, x3::phys::Layer::Static);
        e.transform[12]=closed.x; e.transform[13]=closed.y; e.transform[14]=closed.z;
        return scene.add(e);
    };

    const uint32_t ent1 = buildPanel(closed1);
    const uint32_t ent2 = buildPanel(closed2);

    Door d;
    d.entity     = ent1;
    d.body       = scene.get(ent1).body;
    d.closedPos  = closed1;
    d.openPos    = open1;
    d.entity2    = ent2;
    d.body2      = scene.get(ent2).body;
    d.closedPos2 = closed2;
    d.openPos2   = open2;
    d.duration   = spec.duration;
    d.state      = DoorState::Closed;
    d.locked     = spec.locked;
    d.code       = spec.code;
    d.keycard    = spec.keycard;
    d.requireBoth = spec.requireBoth;
    d.axis       = (uint32_t)spec.axis;
    d.halfWidth  = spec.halfWidth;
    d.height     = spec.height;
    d.floorHatch = true;
    d.floorStyle = (spec.floorStyle == kDoorFloorAuto) ? doorFloorForY(spec.doorwayCenter.y)
                                                       : spec.floorStyle;
    d.withFrame  = false;                 // a horizontal hatch has no vertical frame
    const uint32_t doorIdx = doors.add(d);

    x3::logInfo("buildFloorHatch: two-panel hatch idx " + std::to_string(doorIdx) +
                " entities " + std::to_string(ent1) + "+" + std::to_string(ent2) +
                (spec.locked ? " [LOCKED]" : "") + " (flush, parts from centre)");
    return doorIdx;
}

// ---------------------------------------------------------------------------
// Generalized door (+ optional button) at an arbitrary doorway (Level 1).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// SPLIT-MODEL door (doors-pass): a registry model whose authored motion is two
// panels parting HORIZONTALLY from the centre ("slider" / "bulkhead"). Two
// static collision boxes — each covering half the opening — slide APART along
// the wall run on the shared cursor (the floor hatch's body2 machinery, stood
// upright), pocketing into the wall meat. Each panel's travel is the larger of
// the half-opening and its VISUAL leaf width, so both collision and mesh clear
// the cut. Closed blocks, open passes — --test-doors D8 walks a capsule at it.
// ---------------------------------------------------------------------------
static uint32_t buildSplitDoor(Scene& scene, DoorSystem& doors,
                               x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics,
                               const DoorSpec& spec, uint32_t modelIdx) {
    const DoorModelDef& def = kDoorModels[modelIdx];
    const float hw = spec.halfWidth;
    const float hh = spec.height * 0.5f;
    const float ht = spec.thickness * 0.5f;
    const bool  runX = (spec.axis == DoorAxis::AlongX);   // wall plane Z=const -> run along X

    // Load the model mesh (idempotent; also warms door_a + the shared loader).
    const bool meshOk = doors.loadModelMesh(device, modelIdx);

    // Visual leaf widths along the run at the build's fit scale (mirror of the
    // drawMeshes fit): sRun = (opening + straddle) / model width.
    const bool  zRun   = def.preYaw > 0.5f;
    const float modelW = zRun ? (def.aabb[5] - def.aabb[2]) : (def.aabb[3] - def.aabb[0]);
    const float sRun   = (hw * 2.0f + 0.30f) / modelW;
    const float pAW = (zRun ? (def.panelA[5] - def.panelA[2]) : (def.panelA[3] - def.panelA[0])) * sRun;
    const float pBW = (zRun ? (def.panelB[5] - def.panelB[2]) : (def.panelB[3] - def.panelB[0])) * sRun;
    const float travelA = std::max(hw, pAW) + 0.02f;
    const float travelB = std::max(hw, pBW) + 0.02f;

    // Panel collision boxes: each covers HALF the opening (full height/thickness).
    const x3::phys::Vec3 panelHalf = runX ? x3::phys::Vec3{ hw * 0.5f, hh, ht }
                                          : x3::phys::Vec3{ ht, hh, hw * 0.5f };
    const float cx = spec.doorwayCenter.x, cy = spec.doorwayCenter.y + hh, cz = spec.doorwayCenter.z;
    const x3::phys::Vec3 closed1 = runX ? x3::phys::Vec3{ cx - hw * 0.5f, cy, cz }
                                        : x3::phys::Vec3{ cx, cy, cz - hw * 0.5f };
    const x3::phys::Vec3 closed2 = runX ? x3::phys::Vec3{ cx + hw * 0.5f, cy, cz }
                                        : x3::phys::Vec3{ cx, cy, cz + hw * 0.5f };
    const x3::phys::Vec3 open1   = runX ? x3::phys::Vec3{ closed1.x - travelA, cy, cz }
                                        : x3::phys::Vec3{ cx, cy, closed1.z - travelA };
    const x3::phys::Vec3 open2   = runX ? x3::phys::Vec3{ closed2.x + travelB, cy, cz }
                                        : x3::phys::Vec3{ cx, cy, closed2.z + travelB };

    auto buildPanel = [&](const x3::phys::Vec3& closed) -> uint32_t {
        x3::prims::PrimMesh geo = x3::prims::makeBox(panelHalf.x, panelHalf.y, panelHalf.z,
                                                     0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0] = spec.tint[0]; e.baseColor[1] = spec.tint[1];
        e.baseColor[2] = spec.tint[2]; e.baseColor[3] = spec.tint[3];
        e.tag = (uint32_t)Tag::Door;
        e.visible = !meshOk;               // graybox fallback only when the GLB is absent
        e.body = physics.addBox(panelHalf, closed, 0.0f, x3::phys::Layer::Static);
        e.transform[12] = closed.x; e.transform[13] = closed.y; e.transform[14] = closed.z;
        return scene.add(e);
    };
    const uint32_t ent1 = buildPanel(closed1);
    const uint32_t ent2 = buildPanel(closed2);

    Door d;
    d.entity     = ent1;
    d.body       = scene.get(ent1).body;
    d.closedPos  = closed1;
    d.openPos    = open1;
    d.entity2    = ent2;
    d.body2      = scene.get(ent2).body;
    d.closedPos2 = closed2;
    d.openPos2   = open2;
    d.duration   = spec.duration;
    d.state      = DoorState::Closed;
    d.locked     = spec.locked;
    d.code       = spec.code;
    d.keycard    = spec.keycard;
    d.requireBoth = spec.requireBoth;
    d.axis       = (uint32_t)spec.axis;
    d.halfWidth  = spec.halfWidth;
    d.height     = spec.height;
    d.modelIdx   = modelIdx;
    d.floorStyle = (spec.floorStyle == kDoorFloorAuto) ? doorFloorForY(spec.doorwayCenter.y)
                                                       : spec.floorStyle;
    d.withFrame  = spec.withFrame;
    const uint32_t doorIdx = doors.add(d);

    if (spec.withButton) {
        const float kBtnHalf = 0.12f;
        x3::phys::Vec3 btnPos = runX
            ? x3::phys::Vec3{ cx + hw + 0.5f, spec.doorwayCenter.y + 1.3f, cz - ht - kBtnHalf }
            : x3::phys::Vec3{ cx - ht - kBtnHalf, spec.doorwayCenter.y + 1.3f, cz + hw + 0.5f };
        x3::prims::PrimMesh geo = x3::prims::makeBox(kBtnHalf, kBtnHalf, kBtnHalf, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0] = 0.20f; e.baseColor[1] = 0.85f; e.baseColor[2] = 0.55f; e.baseColor[3] = 1.0f;
        e.tag  = (uint32_t)Tag::Button;
        e.link = ent1;
        e.body = physics.addBox(x3::phys::Vec3{ kBtnHalf, kBtnHalf, kBtnHalf },
                                btnPos, 0.0f, x3::phys::Layer::Static);
        e.transform[12] = btnPos.x; e.transform[13] = btnPos.y; e.transform[14] = btnPos.z;
        scene.add(e);
    }

    x3::logInfo(std::string("buildSplitDoor['") + def.key + "']: door idx " +
                std::to_string(doorIdx) + " entities " + std::to_string(ent1) + "+" +
                std::to_string(ent2) + " [" + doorStyleFor(d.floorStyle).name + "]" +
                (spec.locked ? " [LOCKED]" : "") + (meshOk ? "" : " (graybox panels)"));
    return doorIdx;
}

uint32_t buildLevelDoor(Scene& scene, DoorSystem& doors,
                        x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics,
                        const DoorSpec& spec) {
    const float hw = spec.halfWidth;
    const float hh = spec.height * 0.5f;
    const float ht = spec.thickness * 0.5f;
    // Half-extents + slide direction.
    x3::phys::Vec3 half, closedPos, openPos;
    if (spec.floorHatch) {
        // FLOOR HATCH (two-panel blast-door / iris): handled below in its own
        // builder so it can lay TWO flush, floor-textured panels that part from the
        // centre. buildFloorHatch() returns the door index directly.
        return buildFloorHatch(scene, doors, device, physics, spec);
    }
    // REGISTRY MODEL dispatch (doors-pass): a split-motion model routes to the
    // two-panel builder; door_a (and unknown keys) keep the slide-up path below.
    {
        const uint32_t modelIdx = doorModelIndex(spec.model);
        if (kDoorModels[modelIdx].motion == DoorMotion::SlideSplit)
            return buildSplitDoor(scene, doors, device, physics, spec, modelIdx);
    }
    if (spec.axis == DoorAxis::AlongX) {
        // Wall plane is Z = const: door is wide in X (the run), thin in Z. Slides UP.
        half      = x3::phys::Vec3{ hw, hh, ht };
        closedPos = x3::phys::Vec3{ spec.doorwayCenter.x, spec.doorwayCenter.y + hh, spec.doorwayCenter.z };
        openPos   = x3::phys::Vec3{ closedPos.x, closedPos.y + spec.height, closedPos.z };
    } else {
        // Wall plane is X = const: door is thin in X, wide in Z (the run). Slides UP.
        half      = x3::phys::Vec3{ ht, hh, hw };
        closedPos = x3::phys::Vec3{ spec.doorwayCenter.x, spec.doorwayCenter.y + hh, spec.doorwayCenter.z };
        openPos   = x3::phys::Vec3{ closedPos.x, closedPos.y + spec.height, closedPos.z };
    }

    // Load the shared real-door GLB once (idempotent). The visual is the GLB slab
    // drawn by drawMeshes(); the procedural box below stays as collision only.
    doors.loadDoorMesh(device, kDoorGlbDir());

    uint32_t doorEntId;
    {
        // Render mesh authored centered at the body origin (NOT world-baked) so
        // the Entity transform translation drives its position as the body moves.
        // The box is now COLLISION-ONLY (visible=false): the real SM_Door_A GLB is
        // drawn over it by DoorSystem::drawMeshes(). The box still blocks the
        // player while closed and is repositioned each frame as the door slides.
        x3::prims::PrimMesh geo = x3::prims::makeBox(half.x, half.y, half.z, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0] = spec.tint[0]; e.baseColor[1] = spec.tint[1];
        e.baseColor[2] = spec.tint[2]; e.baseColor[3] = spec.tint[3];
        e.tag  = (uint32_t)Tag::Door;
        // Hide the flat-color box when a real (vertical) door mesh is available; if
        // the GLB failed to load, keep the box. A FLOOR HATCH always shows its box —
        // the vertical SM_Door GLB doesn't fit a horizontal hatch (drawMeshes skips it).
        e.visible = spec.floorHatch || !doors.hasDoorMesh();
        e.body = physics.addBox(half, closedPos, 0.0f, x3::phys::Layer::Static);
        e.transform[12] = closedPos.x;
        e.transform[13] = closedPos.y;
        e.transform[14] = closedPos.z;
        doorEntId = scene.add(e);
    }

    Door d;
    d.entity    = doorEntId;
    d.body      = scene.get(doorEntId).body;
    d.closedPos = closedPos;
    d.openPos   = openPos;
    d.duration  = spec.duration;
    d.state     = DoorState::Closed;
    d.locked    = spec.locked;
    d.code      = spec.code;
    d.keycard   = spec.keycard;
    d.requireBoth = spec.requireBoth;
    d.axis      = (uint32_t)spec.axis;
    d.halfWidth = spec.halfWidth;
    d.height    = spec.height;
    d.floorHatch = spec.floorHatch;
    // PER-FLOOR VARIANT: auto-derive the floor from the doorway's world Y unless the
    // caller pinned one (door.h DoorSpec::floorStyle).
    d.floorStyle = (spec.floorStyle == kDoorFloorAuto) ? doorFloorForY(spec.doorwayCenter.y)
                                                       : spec.floorStyle;
    d.withFrame = spec.withFrame;
    uint32_t doorIdx = doors.add(d);

    // Optional linked button, mounted on the wall beside the doorway, on the
    // approach (−) side along the axis of travel so the player can press it from
    // the room they arrive in. Cyan-green like the original button.
    if (spec.withButton) {
        const float kBtnHalf = 0.12f;
        x3::phys::Vec3 btnPos;
        if (spec.axis == DoorAxis::AlongX) {
            // Doorway in a Z=const wall: button to +X of the opening, on the −Z face.
            btnPos = x3::phys::Vec3{ spec.doorwayCenter.x + hw + 0.5f, 1.3f,
                                     spec.doorwayCenter.z - ht - kBtnHalf };
        } else {
            // Doorway in an X=const wall: button to +Z of the opening, on the −X face.
            btnPos = x3::phys::Vec3{ spec.doorwayCenter.x - ht - kBtnHalf, 1.3f,
                                     spec.doorwayCenter.z + hw + 0.5f };
        }
        x3::prims::PrimMesh geo = x3::prims::makeBox(kBtnHalf, kBtnHalf, kBtnHalf, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0] = 0.20f; e.baseColor[1] = 0.85f; e.baseColor[2] = 0.55f; e.baseColor[3] = 1.0f;
        e.tag  = (uint32_t)Tag::Button;
        e.link = doorEntId;
        e.body = physics.addBox(x3::phys::Vec3{ kBtnHalf, kBtnHalf, kBtnHalf },
                                btnPos, 0.0f, x3::phys::Layer::Static);
        e.transform[12] = btnPos.x;
        e.transform[13] = btnPos.y;
        e.transform[14] = btnPos.z;
        scene.add(e);
    }

    x3::logInfo("buildLevelDoor: door idx " + std::to_string(doorIdx) +
                " entity " + std::to_string(doorEntId) +
                " [" + doorStyleFor(d.floorStyle).name + "]" +
                (spec.locked ? " [LOCKED]" : "") +
                (spec.withFrame ? " + frame" : "") +
                (spec.withButton ? " + button" : ""));
    return doorIdx;
}

// ===========================================================================
// Headless self-test (--test-interact). T1-T4.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[interact-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[interact-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

// Headless IRenderDevice: the shared no-op test-double (app/headless_device.h).
// Mints monotonically-increasing valid handles so buildTestLevel()/
// buildDoorAndButton() run unchanged with no Vulkan; all draw/frame/camera
// calls are no-ops.
using HeadlessDevice = x3::game::HeadlessRenderDevice;

// Build "eye -> button" aim from a known eye position toward the button entity's
// world center (so the test does not depend on player look math).
x3::phys::Vec3 sub(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return x3::phys::Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

} // namespace

bool runInteractSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;
    DoorSystem doors;

    // Full S2 graybox room (floor + walls + doorway gap + step), then the door
    // + linked button filling that gap. Mirrors the real app's construction.
    buildTestLevel(scene, device, *physics);
    uint32_t btnEnt = buildDoorAndButton(scene, doors, device, *physics);

    // Locate the door (the single registered door).
    Door& door = doors.at(0);
    const x3::phys::Vec3 btnCenter = physics->getBodyPosition(scene.get(btnEnt).body);

    // ---- T1: aim AT the button within range -> button hit, Closed->Opening ---
    {
        // Eye 2.5 m in front of (toward -Z from) the button, at button height.
        x3::phys::Vec3 eye{ btnCenter.x, btnCenter.y, btnCenter.z - 2.5f };
        x3::phys::Vec3 dir = sub(btnCenter, eye);   // points at the button
        bool started = tryUse(eye, dir, 3.0f, scene, doors, *physics);
        bool opening = door.state == DoorState::Opening;
        check(started && opening, "T1 use-ray at button starts door Opening");
    }

    // Record the closed-door body center for the T2/T4 doorway checks.
    const x3::phys::Vec3 doorClosedCenter = door.closedPos;

    // ---- T2: after ~1 s of stepping, door reaches Open and has moved ---------
    {
        // Step the door system + physics for ~1.1 s (66 frames @ 1/60).
        for (int i = 0; i < 66; ++i) {
            doors.update(kFixedDt, scene, *physics);
            physics->step(kFixedDt);
        }
        bool open = door.state == DoorState::Open;
        x3::phys::Vec3 now = physics->getBodyPosition(door.body);
        float moved = std::sqrt((now.x - doorClosedCenter.x) * (now.x - doorClosedCenter.x) +
                                (now.y - doorClosedCenter.y) * (now.y - doorClosedCenter.y) +
                                (now.z - doorClosedCenter.z) * (now.z - doorClosedCenter.z));
        // Slide offset is the full door height (2.1 m up).
        bool slid = moved > 2.0f;
        // The doorway volume (y in [0, 2.1] at x=0, z=8) is no longer occupied:
        // a ray straight DOWN through the opening from above should now pass the
        // door (the closed door's top was at y=2.1; open door bottom is at 2.1).
        // Check directly: door body center y is now well above the passage.
        bool clear = now.y > kPassageTop;       // center above the clear opening
        check(open && slid && clear, "T2 door reaches Open + body slid clear of doorway");
    }

    // ---- T3: aiming AWAY (or beyond range) does NOT open a (fresh) door ------
    {
        // Fresh world so we start from a Closed door.
        std::unique_ptr<x3::phys::IPhysicsWorld> p2(x3::phys::createPhysicsWorld());
        p2->init();
        HeadlessDevice dev2;
        Scene s2;
        DoorSystem d2;
        buildTestLevel(s2, dev2, *p2);
        uint32_t b2 = buildDoorAndButton(s2, d2, dev2, *p2);
        const x3::phys::Vec3 bc = p2->getBodyPosition(s2.get(b2).body);

        // (a) Aim AWAY from the button (opposite direction).
        x3::phys::Vec3 eye{ bc.x, bc.y, bc.z - 2.5f };
        x3::phys::Vec3 away{ 0.0f, 0.0f, -1.0f };   // points further -Z, away from button
        bool startedAway = tryUse(eye, away, 3.0f, s2, d2, *p2);

        // (b) Aim at the button but BEYOND range (button is 2.5 m away; maxDist 1.0).
        x3::phys::Vec3 toBtn = sub(bc, eye);
        bool startedFar = tryUse(eye, toBtn, 1.0f, s2, d2, *p2);

        bool stillClosed = d2.at(0).state == DoorState::Closed;
        check(!startedAway && !startedFar && stillClosed,
              "T3 aiming away / out of range does not open the door");
        p2->shutdown();
    }

    // ---- T4: closed door blocks the doorway; open door lets a character pass --
    {
        // Closed door: a character walking toward +Z at the doorway is stopped
        // before passing z=8. Open door: it passes through.
        auto runDoorway = [](bool openIt) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            HeadlessDevice dev;
            Scene s;
            DoorSystem dd;
            buildTestLevel(s, dev, *w);
            buildDoorAndButton(s, dd, dev, *w);
            Door& dr = dd.at(0);
            if (openIt) {
                // Drive the door fully open before the character approaches.
                dd.startOpening(dr);   // Closed -> Opening
                for (int i = 0; i < 70; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); }
            }
            // Character starts inside the room, on the doorway centerline, walks +Z.
            x3::phys::BodyId chr = w->createCharacter(0.3f, 1.8f, x3::phys::Vec3{0.0f, 0.05f, 6.0f});
            for (int i = 0; i < 30; ++i) { w->moveCharacter(chr, x3::phys::Vec3{0,0,0}, kFixedDt); w->step(kFixedDt); }
            // Push toward +Z (through the doorway at z=8) for ~4 s.
            for (int i = 0; i < 240; ++i) {
                w->moveCharacter(chr, x3::phys::Vec3{0,0,4.0f}, kFixedDt);
                w->step(kFixedDt);
            }
            float z = w->getBodyPosition(chr).z;
            w->shutdown();
            return z;
        };

        float zClosed = runDoorway(false);  // should be stopped before/at the door
        float zOpen   = runDoorway(true);   // should pass beyond z=8
        bool blocked = zClosed < 8.0f;      // never reached the door plane
        bool passed  = zOpen   > 8.2f;      // walked out through the doorway
        check(blocked && passed, "T4 closed door blocks doorway, open door passes");
    }

    // ---- T5: toggle OPENS then CLOSES a door (E to open AND close) -----------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> p5(x3::phys::createPhysicsWorld());
        p5->init();
        HeadlessDevice dev5;
        Scene s5;
        DoorSystem d5;
        buildTestLevel(s5, dev5, *p5);
        buildDoorAndButton(s5, d5, dev5, *p5);
        Door& dr = d5.at(0);
        // First toggle: Closed -> Opening; step it fully open.
        d5.toggle(dr);
        for (int i = 0; i < 70; ++i) { d5.update(kFixedDt, s5, *p5); p5->step(kFixedDt); }
        bool wasOpen = dr.state == DoorState::Open;
        // Second toggle: Open -> Closing; step it fully closed.
        bool tog = d5.toggle(dr);
        bool closing = dr.state == DoorState::Closing;
        for (int i = 0; i < 70; ++i) { d5.update(kFixedDt, s5, *p5); p5->step(kFixedDt); }
        bool backClosed = dr.state == DoorState::Closed;
        x3::phys::Vec3 now = p5->getBodyPosition(dr.body);
        bool downAgain = now.y < kPassageTop;   // slab dropped back to fill the doorway
        check(wasOpen && tog && closing && backClosed && downAgain,
              "T5 toggle opens then closes the door (E open/close)");
        p5->shutdown();
    }

    // ---- T6: a FLOOR HATCH blocks a body resting on it when CLOSED, and lets it
    // fall through when OPEN (the hatch slides aside). "The door on the floor." ----
    {
        auto runHatch = [](bool openIt) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            HeadlessDevice dev;
            Scene s;
            DoorSystem dd;
            // A floor hatch: 1.0 m half-size square opening at y=0, in open floor.
            DoorSpec spec;
            spec.doorwayCenter = x3::phys::Vec3{ 0.0f, 0.0f, 0.0f };
            spec.halfWidth = 1.0f; spec.thickness = 0.2f; spec.withButton = false;
            spec.floorHatch = true;
            buildLevelDoor(s, dd, dev, *w, spec);
            Door& h = dd.at(0);
            if (openIt) { dd.toggle(h); for (int i = 0; i < 70; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); } }
            // Drop a dynamic box from just above the hatch centre.
            x3::phys::BodyId b = w->addBox(x3::phys::Vec3{0.2f,0.2f,0.2f}, x3::phys::Vec3{0,1.0f,0},
                                           2.0f, x3::phys::Layer::Dynamic);
            for (int i = 0; i < 120; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); }
            float y = w->getBodyPosition(b).y;
            w->shutdown();
            return y;
        };
        float yClosed = runHatch(false);   // rests ON the closed hatch (stays up ~0.3+)
        float yOpen   = runHatch(true);    // falls THROUGH the open opening (goes well below 0)
        bool held = yClosed > 0.0f;
        bool fell = yOpen   < -1.0f;
        check(held && fell, "T6 floor hatch holds when closed, drops you through when open");
    }

    physics->shutdown();

    x3::logInfo(std::string("[interact-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ===========================================================================
// --test-doors: the DOOR-MESH-SWAP polish gate (D1-D6). See door.h.
//
// SCOPE, stated plainly: this proves MECHANICS. It cannot prove that the doors
// LOOK right per floor, that the accel/decel FEELS mechanical, or that the servo
// bed sits at the right level in the mix. Those are owner eyeball/ear items.
// ===========================================================================
namespace {

int h_pass = 0, h_fail = 0;
void dcheck(bool cond, const char* name) {
    if (cond) { ++h_pass; x3::logInfo(std::string("[door-test] PASS ") + name); }
    else      { ++h_fail; x3::logError(std::string("[door-test] FAIL ") + name); }
}

// A counting IAudioSystem test double. It records the servo loop's lifecycle so
// D5 can assert the voice is created exactly once per motion and destroyed on the
// seating frame — the mechanical form of "the sound stops with the motion".
class CountingAudio final : public x3::audio::IAudioSystem {
public:
    int loopsStarted = 0, loopsStopped = 0, oneShots = 0;
    int liveLoops() const { return loopsStarted - loopsStopped; }

    bool init() override { return true; }
    void shutdown() override {}
    x3::audio::SoundHandle load(std::string_view) override { return { ++m_nextSound }; }
    void playSound2D(x3::audio::SoundHandle, float, float) override { ++oneShots; }
    void playSound3D(x3::audio::SoundHandle, float, float, float, float, float) override { ++oneShots; }
    void setListener(float, float, float, float, float) override {}
    void playMusic(std::string_view, bool, float) override {}
    void stopMusic() override {}
    void setMusicVolume(float) override {}
    void setMusicEnabled(bool) override {}
    void setMasterSfxVolume(float) override {}
    x3::audio::LoopHandle startLoop(x3::audio::SoundHandle, float, float) override {
        ++loopsStarted; return { ++m_nextLoop };
    }
    x3::audio::LoopHandle startLoop3D(x3::audio::SoundHandle, float, float, float,
                                      float, float) override {
        ++loopsStarted; return { ++m_nextLoop };
    }
    void stopLoop(x3::audio::LoopHandle h) override { if (h.valid()) ++loopsStopped; }
    void update(float) override {}
private:
    uint32_t m_nextSound = 0, m_nextLoop = 0;
};

// Canonical facility doorway dimensions (level_loader.cpp kDoorHalf / kLintel).
constexpr float kCanonDoorHalf = 0.8f;    // 1.6 m opening
constexpr float kCanonLintel   = 2.2f;    // clear head height

} // namespace

bool runDoorSelfTest() {
    h_pass = h_fail = 0;

    // ---- D1: the ease curve is a legal motion profile -----------------------
    {
        const bool ends = std::fabs(doorEase(0.0f)) < 1e-6f &&
                          std::fabs(doorEase(1.0f) - 1.0f) < 1e-6f;
        // Strictly increasing (monotonic 0 -> 1) across a fine sweep.
        bool mono = true, inRange = true;
        float prev = -1.0f;
        for (int i = 0; i <= 1000; ++i) {
            const float s = doorEase((float)i / 1000.0f);
            if (s < prev - 1e-7f) mono = false;
            if (s < -1e-6f || s > 1.0f + 1e-6f) inRange = false;
            prev = s;
        }
        // Zero slope at BOTH ends (accel out of the jamb, decel into the stop) —
        // the difference between "mechanical" and "snapped".
        const float h = 1e-3f;
        const float v0 = (doorEase(h) - doorEase(0.0f)) / h;
        const float v1 = (doorEase(1.0f) - doorEase(1.0f - h)) / h;
        // ...and a real CRUISE in the middle, faster than either end.
        const float vMid = (doorEase(0.5f + h) - doorEase(0.5f - h)) / (2.0f * h);
        const bool eased = v0 < 0.15f && v1 < 0.15f && vMid > 1.2f;
        // Symmetric: opening and closing feel identical.
        bool sym = true;
        for (int i = 0; i <= 100; ++i) {
            const float u = (float)i / 100.0f;
            if (std::fabs(doorEase(u) + doorEase(1.0f - u) - 1.0f) > 1e-5f) sym = false;
        }
        dcheck(ends && mono && inRange && eased && sym,
               "D1 ease curve monotonic 0->1, zero-slope ends, cruising middle, symmetric");
    }

    // ---- D2: the motion is DELTA-TIME scaled, not per-frame -----------------
    // Integrate the SAME 1.0 s door at 60 Hz and at 165 Hz and compare the eased
    // displacement at matched SIM TIME. A per-frame (dt-blind) profile would run
    // 2.75x further at 165 Hz; a dt-scaled one lands on the same curve.
    {
        auto sweep = [](float dt, float simSeconds) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            HeadlessDevice dev; Scene s; DoorSystem dd;
            DoorSpec spec;
            spec.doorwayCenter = x3::phys::Vec3{ 0, 0, 0 };
            spec.halfWidth = kCanonDoorHalf; spec.height = kCanonLintel;
            spec.duration = 1.0f; spec.withButton = false;
            buildLevelDoor(s, dd, dev, *w, spec);
            Door& d = dd.at(0);
            dd.startOpening(d);
            const int steps = (int)std::lround(simSeconds / dt);
            for (int i = 0; i < steps; ++i) dd.update(dt, s, *w);
            const float y = w->getBodyPosition(d.body).y;
            w->shutdown();
            return y;
        };
        const float y60  = sweep(1.0f / 60.0f,  0.6f);
        const float y165 = sweep(1.0f / 165.0f, 0.6f);
        const float agree = std::fabs(y60 - y165);
        // ...and the total travel time is duration-governed, not step-count-governed.
        const float yEnd60  = sweep(1.0f / 60.0f,  1.2f);
        const float yEnd165 = sweep(1.0f / 165.0f, 1.2f);
        const bool bothSeated = std::fabs(yEnd60 - yEnd165) < 1e-4f;
        dcheck(agree < 0.02f && bothSeated,
               "D2 motion is dt-scaled (60 Hz vs 165 Hz agree mid-sweep and at rest)");
        x3::logInfo("[door-test]   mid-sweep y: 60Hz=" + std::to_string(y60) +
                    " 165Hz=" + std::to_string(y165) + " delta=" + std::to_string(agree));
    }

    // ---- D3: per-floor variants resolve, and resolve DISTINCTLY -------------
    {
        // Every canonical floor base Y maps to its own floor index...
        const float baseY[kDoorFloorCount] = { 0, 5, 10, 20, 30, 65, 78, 91 };
        bool mapped = true;
        for (uint32_t i = 0; i < kDoorFloorCount; ++i)
            if (doorFloorForY(baseY[i]) != i) mapped = false;
        // ...deep zone / club (below B1) still resolves (to B1), never OOB.
        const bool deepOk = doorFloorForY(-220.0f) == 0 && doorFloorForY(-5.0f) == 0;
        // ...an out-of-range index clamps instead of reading past the table.
        const bool clampOk = doorStyleFor(999).name != nullptr;
        // ...and no two floors share a signage colour (a floor must READ distinctly).
        bool distinct = true;
        for (uint32_t i = 0; i < kDoorFloorCount; ++i)
            for (uint32_t j = i + 1; j < kDoorFloorCount; ++j) {
                const DoorStyle& a = doorStyleFor(i);
                const DoorStyle& b = doorStyleFor(j);
                const float dd2 = (a.sign[0]-b.sign[0])*(a.sign[0]-b.sign[0]) +
                                  (a.sign[1]-b.sign[1])*(a.sign[1]-b.sign[1]) +
                                  (a.sign[2]-b.sign[2])*(a.sign[2]-b.sign[2]);
                if (dd2 < 0.02f) distinct = false;
            }
        // ...the leaf tints stay inside the value band (hue pass, not a brightness cheat).
        bool valueSafe = true;
        for (uint32_t i = 0; i < kDoorFloorCount; ++i)
            for (int k = 0; k < 3; ++k)
                if (doorStyleFor(i).leaf[k] > 1.0f || doorStyleFor(i).frame[k] > 1.0f)
                    valueSafe = false;
        dcheck(mapped && deepOk && clampOk && distinct && valueSafe,
               "D3 per-floor variants resolve from world Y, clamp safely, read distinctly");
    }

    // ---- D4: PASSABILITY — the regression that must never come back ---------
    // A previous bug had doors that visually opened while their collision stayed in
    // the opening. Build a REAL canonical doorway (jambs + lintel + a door of
    // level_loader's kDoorHalf/kLintel dims) and walk a STANDING 1.8 m capsule at it.
    {
        auto walk = [](bool openIt) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            HeadlessDevice dev; Scene s; DoorSystem dd;
            // Deck.
            w->addBox(x3::phys::Vec3{ 8.0f, 0.2f, 8.0f }, x3::phys::Vec3{ 0, -0.2f, 0 },
                      0.0f, x3::phys::Layer::Static);
            // A wall on the Z=0 plane with a kDoorHalf-wide, kLintel-tall hole cut in it:
            // two jambs + a header (LAW 1 — one wall, one opening).
            const float jw = (8.0f - kCanonDoorHalf) * 0.5f;
            w->addBox(x3::phys::Vec3{ jw, 1.6f, 0.1f },
                      x3::phys::Vec3{ -(kCanonDoorHalf + jw), 1.6f, 0 }, 0.0f, x3::phys::Layer::Static);
            w->addBox(x3::phys::Vec3{ jw, 1.6f, 0.1f },
                      x3::phys::Vec3{  (kCanonDoorHalf + jw), 1.6f, 0 }, 0.0f, x3::phys::Layer::Static);
            w->addBox(x3::phys::Vec3{ 8.0f, 0.4f, 0.1f },
                      x3::phys::Vec3{ 0, kCanonLintel + 0.4f, 0 }, 0.0f, x3::phys::Layer::Static);
            DoorSpec spec;
            spec.doorwayCenter = x3::phys::Vec3{ 0, 0, 0 };
            spec.axis = DoorAxis::AlongX;
            spec.halfWidth = kCanonDoorHalf; spec.height = kCanonLintel;
            spec.duration = 1.0f; spec.withButton = false;
            buildLevelDoor(s, dd, dev, *w, spec);
            Door& d = dd.at(0);
            if (openIt) {
                dd.startOpening(d);
                for (int i = 0; i < 120; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); }
                if (d.state != DoorState::Open) { w->shutdown(); return -99.0f; }
            }
            // THE STANDING PLAYER: radius 0.3, height 1.8 (the capsule the task names).
            x3::phys::BodyId chr = w->createCharacter(0.3f, 1.8f, x3::phys::Vec3{ 0, 0.05f, -3.0f });
            for (int i = 0; i < 30; ++i) { w->moveCharacter(chr, x3::phys::Vec3{0,0,0}, kFixedDt); w->step(kFixedDt); }
            for (int i = 0; i < 300; ++i) {
                w->moveCharacter(chr, x3::phys::Vec3{ 0, 0, 4.0f }, kFixedDt);
                dd.update(kFixedDt, s, *w);
                w->step(kFixedDt);
            }
            const float z = w->getBodyPosition(chr).z;
            w->shutdown();
            return z;
        };
        const float zClosed = walk(false);
        const float zOpen   = walk(true);
        const bool blocked = zClosed < -0.15f;   // never reached the door plane
        const bool passed  = zOpen   >  1.0f;    // walked clean out the far side
        dcheck(blocked && passed,
               "D4 PASSABILITY: 1.8 m capsule blocked by the closed slab, clears the OPEN doorway");
        x3::logInfo("[door-test]   capsule z: closed=" + std::to_string(zClosed) +
                    " open=" + std::to_string(zOpen));
    }

    // ---- D5: the servo voice starts with the motion and STOPS with it -------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        HeadlessDevice dev; Scene s; DoorSystem dd;
        CountingAudio audio;
        dd.setAudio(&audio, audio.load("open"), audio.load("close"), audio.load("locked"));
        dd.setMotorAudio(audio.load("servo"), audio.load("thunk"));
        DoorSpec spec;
        spec.doorwayCenter = x3::phys::Vec3{ 0, 0, 0 };
        spec.halfWidth = kCanonDoorHalf; spec.height = kCanonLintel;
        spec.duration = 1.0f; spec.withButton = false;
        buildLevelDoor(s, dd, dev, *w, spec);
        Door& d = dd.at(0);

        const int shotsBefore = audio.oneShots;
        dd.toggle(d);                                   // Closed -> Opening
        // With a servo loop wired, NO long open/close one-shot may be fired at all.
        const bool noOverrunShot = (audio.oneShots == shotsBefore);
        // The loop must not exist before the first update tick, and must exist during.
        const bool quietBeforeTick = audio.liveLoops() == 0;
        dd.update(kFixedDt, s, *w);
        const bool humWhileMoving = audio.liveLoops() == 1 && dd.liveMotorCount() == 1;
        // Mid-slide, still exactly ONE voice (no per-frame restart).
        for (int i = 0; i < 30; ++i) dd.update(kFixedDt, s, *w);
        const bool oneVoiceOnly = audio.loopsStarted == 1 && audio.liveLoops() == 1;
        // Run past the end: the voice must be gone the moment the slab seats.
        for (int i = 0; i < 60; ++i) dd.update(kFixedDt, s, *w);
        const bool seated = d.state == DoorState::Open;
        const bool silentAtRest = audio.liveLoops() == 0 && dd.liveMotorCount() == 0;
        // Closing raises a NEW voice and drops it again.
        dd.toggle(d);
        for (int i = 0; i < 90; ++i) dd.update(kFixedDt, s, *w);
        const bool closedClean = d.state == DoorState::Closed &&
                                 audio.loopsStarted == 2 && audio.liveLoops() == 0;
        // A locked door refuses to move -> it must never raise a motor voice.
        d.locked = true;
        dd.toggle(d);
        for (int i = 0; i < 30; ++i) dd.update(kFixedDt, s, *w);
        const bool lockedSilent = audio.liveLoops() == 0 && audio.loopsStarted == 2;
        // stopAllMotors() on a live door is safe + idempotent (teardown path).
        d.locked = false;
        dd.toggle(d);
        dd.update(kFixedDt, s, *w);
        dd.stopAllMotors(); dd.stopAllMotors();
        const bool teardownClean = audio.liveLoops() == 0 &&
                                   audio.loopsStopped == audio.loopsStarted;
        w->shutdown();
        dcheck(noOverrunShot && quietBeforeTick && humWhileMoving && oneVoiceOnly &&
               seated && silentAtRest && closedClean && lockedSilent && teardownClean,
               "D5 servo loop brackets the motion exactly (one voice, none at rest, none when locked)");
    }

    // ---- D6: the per-floor / frame spec plumbing survives the builder -------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        HeadlessDevice dev; Scene s; DoorSystem dd;
        // AUTO: a door at F5's elevation resolves the Drone Manufacturing style.
        DoorSpec a;
        a.doorwayCenter = x3::phys::Vec3{ 0, 65.0f, 0 };
        a.halfWidth = kCanonDoorHalf; a.height = kCanonLintel; a.withButton = false;
        const uint32_t ia = buildLevelDoor(s, dd, dev, *w, a);
        // PINNED: an explicit index wins over the elevation.
        DoorSpec b = a;
        b.doorwayCenter = x3::phys::Vec3{ 10.0f, 65.0f, 0 };
        b.floorStyle = 2;                 // F2 Medical
        b.withFrame  = false;
        const uint32_t ib = buildLevelDoor(s, dd, dev, *w, b);
        // A floor HATCH never carries a vertical frame.
        DoorSpec c;
        c.doorwayCenter = x3::phys::Vec3{ 20.0f, 0.0f, 0 };
        c.halfWidth = 0.9f; c.thickness = 0.2f; c.floorHatch = true; c.withButton = false;
        const uint32_t ic = buildLevelDoor(s, dd, dev, *w, c);
        const bool autoOk   = dd.at(ia).floorStyle == 5 && dd.at(ia).withFrame;
        const bool pinnedOk = dd.at(ib).floorStyle == 2 && !dd.at(ib).withFrame;
        const bool hatchOk  = !dd.at(ic).withFrame && dd.at(ic).floorHatch;
        w->shutdown();
        dcheck(autoOk && pinnedOk && hatchOk,
               "D6 floorStyle auto/pinned + withFrame plumb through buildLevelDoor");
    }

    // ---- D7: THE LOCK API (doors-pass) — lock/unlock/isLocked + closeAndLock -
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        HeadlessDevice dev; Scene s; DoorSystem dd;
        CountingAudio audio;
        dd.setAudio(&audio, audio.load("open"), audio.load("close"), audio.load("locked"));
        DoorSpec spec;
        spec.doorwayCenter = x3::phys::Vec3{ 0, 0, 0 };
        spec.halfWidth = kCanonDoorHalf; spec.height = kCanonLintel;
        spec.duration = 1.0f; spec.withButton = false;
        buildLevelDoor(s, dd, dev, *w, spec);
        Door& d = dd.at(0);
        // Locked closed door refuses to open, and SAYS so (denied buzz fired).
        d.lock();
        const int shots0 = audio.oneShots;
        const bool refusedOpen = !dd.startOpening(d) && !dd.toggle(d) &&
                                 d.state == DoorState::Closed && d.isLocked();
        const bool buzzed = audio.oneShots == shots0 + 2;   // one buzz per refusal
        // unlock() restores normal operation.
        d.unlock();
        const bool opens = dd.startOpening(d);
        for (int i = 0; i < 90; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); }
        const bool nowOpen = d.state == DoorState::Open;
        // closeAndLock() seals an OPEN door: it starts Closing + locks, the E
        // toggle can NOT reverse it mid-travel (the lock-bypass fix), and it
        // finishes seated + locked.
        dd.closeAndLock(d);
        const bool sealing = d.state == DoorState::Closing && d.isLocked();
        dd.update(kFixedDt, s, *w);
        const bool noReverse = !dd.toggle(d) && d.state == DoorState::Closing;
        for (int i = 0; i < 90; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); }
        const bool sealed = d.state == DoorState::Closed && d.isLocked() &&
                            !dd.startOpening(d);
        // ...and a plain unlock re-arms it (lock leaves no residue).
        dd.unlock(d);
        const bool rearmed = dd.startOpening(d);
        w->shutdown();
        dcheck(refusedOpen && buzzed && opens && nowOpen && sealing && noReverse &&
               sealed && rearmed,
               "D7 lock API: locked refuses open (buzz), closeAndLock seals mid-travel, unlock re-arms");
    }

    // ---- D8: MODEL REGISTRY + SPLIT MOTION — every door model is usable ------
    {
        uint32_t nDefs = 0;
        const DoorModelDef* defs = doorModelDefs(nDefs);
        // The registry knows every door model family in the tree, each with a
        // full sound set (the data-driven model->sound mapping).
        bool rows = nDefs >= 3 && defs != nullptr;
        bool sounds = true;
        for (uint32_t i = 0; i < nDefs; ++i)
            if (!defs[i].sndOpen || !defs[i].sndClose || !defs[i].sndLocked ||
                !defs[i].sndServo || !defs[i].sndThunk) sounds = false;
        const bool keys = doorModelIndex("door_a") == 0 && doorModelIndex(nullptr) == 0 &&
                          doorModelIndex("nonsense") == 0 &&
                          doorModelIndex("slider") == 1 && doorModelIndex("bulkhead") == 2;

        // A SPLIT door (authored two-panel motion) blocks the canonical doorway
        // closed and passes a standing capsule open — collision state matches
        // the motion state for the new motion class exactly as D4 proves for
        // slide-up. Graybox collision works even with no GLB on disk.
        auto walkSplit = [](const char* model, bool openIt) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            HeadlessDevice dev; Scene s; DoorSystem dd;
            w->addBox(x3::phys::Vec3{ 8.0f, 0.2f, 8.0f }, x3::phys::Vec3{ 0, -0.2f, 0 },
                      0.0f, x3::phys::Layer::Static);
            DoorSpec spec;
            spec.doorwayCenter = x3::phys::Vec3{ 0, 0, 0 };
            spec.axis = DoorAxis::AlongX;
            spec.halfWidth = kCanonDoorHalf; spec.height = kCanonLintel;
            spec.duration = 1.0f; spec.withButton = false;
            spec.model = model;
            buildLevelDoor(s, dd, dev, *w, spec);
            Door& d = dd.at(0);
            if (!d.body2.valid()) { w->shutdown(); return -777.0f; }   // not split-built
            if (openIt) {
                dd.startOpening(d);
                for (int i = 0; i < 120; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); }
                if (d.state != DoorState::Open) { w->shutdown(); return -99.0f; }
            }
            x3::phys::BodyId chr = w->createCharacter(0.3f, 1.8f, x3::phys::Vec3{ 0, 0.05f, -3.0f });
            for (int i = 0; i < 30; ++i) { w->moveCharacter(chr, x3::phys::Vec3{0,0,0}, kFixedDt); w->step(kFixedDt); }
            for (int i = 0; i < 300; ++i) {
                w->moveCharacter(chr, x3::phys::Vec3{ 0, 0, 4.0f }, kFixedDt);
                dd.update(kFixedDt, s, *w);
                w->step(kFixedDt);
            }
            const float z = w->getBodyPosition(chr).z;
            w->shutdown();
            return z;
        };
        const float zClosed = walkSplit("slider", false);
        const float zOpen   = walkSplit("slider", true);
        const bool splitBlocks = zClosed > -20.0f && zClosed < -0.15f;
        const bool splitPasses = zOpen > 1.0f;

        // The split door speaks through the SAME state-transition audio path
        // (open cue/servo owned by transitions, locked buzz on refusal).
        bool splitSounds = false;
        {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            HeadlessDevice dev; Scene s; DoorSystem dd;
            CountingAudio audio;
            dd.setAudio(&audio, audio.load("open"), audio.load("close"), audio.load("locked"));
            dd.setMotorAudio(audio.load("servo"), audio.load("thunk"));
            DoorSpec spec;
            spec.doorwayCenter = x3::phys::Vec3{ 0, 0, 0 };
            spec.halfWidth = kCanonDoorHalf; spec.height = kCanonLintel;
            spec.duration = 1.0f; spec.withButton = false; spec.model = "bulkhead";
            buildLevelDoor(s, dd, dev, *w, spec);
            Door& d = dd.at(0);
            dd.toggle(d);
            dd.update(kFixedDt, s, *w);
            const bool hum = audio.liveLoops() == 1;                  // servo with the motion
            for (int i = 0; i < 90; ++i) dd.update(kFixedDt, s, *w);
            const bool restSilent = audio.liveLoops() == 0 && d.state == DoorState::Open;
            d.lock(); dd.closeAndLock(d);
            for (int i = 0; i < 90; ++i) dd.update(kFixedDt, s, *w);
            const int shots = audio.oneShots;
            dd.toggle(d);                                              // locked refusal
            const bool buzz = audio.oneShots == shots + 1;
            splitSounds = hum && restSilent && buzz;
            w->shutdown();
        }
        dcheck(rows && sounds && keys && splitBlocks && splitPasses && splitSounds,
               "D8 registry: every model resolves w/ sound set; split door blocks closed, passes open, sounds");
        x3::logInfo("[door-test]   split capsule z: closed=" + std::to_string(zClosed) +
                    " open=" + std::to_string(zOpen));
    }

    x3::logInfo(std::string("[door-test] ") + std::to_string(h_pass) + " passed, " +
                std::to_string(h_fail) + " failed");
    x3::logInfo("[door-test] NOTE: mechanics only. Door LOOK per floor, motion FEEL and "
                "servo mix level are owner eyeball/ear items — headless cannot judge them.");
    return h_fail == 0;
}

} // namespace x3::game
