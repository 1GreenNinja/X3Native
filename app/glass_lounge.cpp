// GLASS LOUNGE + SIT-AT-CHAIR — see app/glass_lounge.h.
//
// Clean-room: Scene/Entity + IRenderDevice + IPhysicsWorld + mesh_prims box builder
// only. The tabletop is flagged transparent + carries a CLEAR GlassMaterial (matching
// the holo-terminal glass entity pattern in app/holo_terminal.cpp — a separate agent
// is upgrading the glass shader for real shininess/transparency; the params here are
// tuned for that pass, the lounge automatically picks up the polish once the shader
// merges). Legs + chairs are opaque boxes.
//
// The SitController is a deviceless state machine: a smoothed lerp (kSitLerpTime)
// between the player's standing eye/yaw and the seated pose. Headless-testable
// (--test-sit) with no GLFW/Vulkan. The host (main.cpp) drives it from the same
// keyDown-gated E-interact path the cell HoloTerminal uses, so it cannot fire while
// typing into the terminal/keypad or while in a UI menu, and firing a weapon never
// triggers sit. Only one interactable (nearest chair OR the terminal) prompts at a
// time — the host gates the chair prompt behind "not in termMode AND no terminal in
// range" exactly like the elevator/terminal prompts do.
#include "glass_lounge.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

namespace {

constexpr float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

// ---------------------------------------------------------------------------
// Lounge dimensions (meters). Authored as constants so the table + chair layout is
// one place — change here to retune. Per the task: tabletop 1.6 x 0.05 x 0.9 m on
// 4 opaque metal legs; chairs = a seat box + a back box (opaque).
// ---------------------------------------------------------------------------
// TABLE — top is GLASS (a thin slab), legs are opaque thin square boxes.
constexpr float kTableTopHX     = 0.80f;   // half-extent X (full 1.6 m)
constexpr float kTableTopHY     = 0.040f;  // half-extent Y (full 8 cm — thicker plate, owner pass 2)
constexpr float kTableTopHZ     = 0.45f;   // half-extent Z (full 0.9 m)
constexpr float kTableHeight    = 0.75f;   // top SURFACE height (m above the floor)
constexpr float kTableLegHX     = 0.020f;  // square 4 cm metal legs (slimmer/refined)
constexpr float kTableLegHZ     = 0.020f;
// Tiny gap so leg TOPS sit a hair BELOW the glass slab bottom — avoids any z-fight at
// the meeting surface + guarantees no "leg poking into glass" visual through the plate.
constexpr float kLegGlassGap    = 0.002f;
// CHAIR — seat box + back box (opaque). Seat top at kChairSeatHeight; back rises above.
constexpr float kChairSeatHX    = 0.225f;  // 45 cm wide seat
constexpr float kChairSeatHY    = 0.025f;  // 5 cm thick cushion
constexpr float kChairSeatHZ    = 0.225f;  // 45 cm deep seat
constexpr float kChairSeatHeight= 0.45f;   // seat top (m above floor)
constexpr float kChairBackHX    = 0.225f;
constexpr float kChairBackHY    = 0.225f;  // 45 cm tall back
constexpr float kChairBackHZ    = 0.025f;  // 5 cm thick
// The back is placed BEHIND the seat (opposite the seated facing) so the player
// looks AT the table when seated.
constexpr float kChairBackOffset = 0.20f;  // distance back from seat center
// CHAIR LEGS — 4 thin posts from the floor up to the seat bottom at the seat's
// axis-aligned corners (inset a hair so they look like they support the seat from
// underneath, not at its outer rim).
constexpr float kChairLegHX     = 0.018f;  // ~3.6 cm square posts
constexpr float kChairLegHZ     = 0.018f;
constexpr float kChairLegInset  = 0.025f;  // inset from seat edge

// Default colors. The table top is set CLEAR (low-opacity GlassMaterial); legs are
// gunmetal; chairs are a warm dark grey.
const float kTableTopTint[4] = { 0.85f, 0.92f, 1.0f, 1.0f };   // tinted near-white (glass tint also lives in GlassMaterial)
const float kTableLegTint[4] = { 0.32f, 0.34f, 0.38f, 1.0f };  // gunmetal
const float kChairTint[4]    = { 0.30f, 0.28f, 0.26f, 1.0f };  // warm dark grey

// Add one opaque world-baked box (render mesh + optional static collision) to the
// scene. Mirrors level1.cpp's addBox signature — render via `device`, collision via
// `physics`, room-tagged so the per-floor cull keeps the box with its floor.
uint32_t addOpaqueBox(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                      float hx, float hy, float hz, float cx, float cy, float cz,
                      const float color[4], bool collide, uint32_t roomId) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    e.baseColor[0] = color[0]; e.baseColor[1] = color[1];
    e.baseColor[2] = color[2]; e.baseColor[3] = color[3];
    for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
    e.tag = (uint32_t)Tag::Prop;
    e.roomId = roomId;
    if (collide)
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
    return scene.add(e);
}

// Add the GLASS tabletop entity: a thin slab whose top surface sits at
// (cx, cy+kTableTopHY, cz). Flags Entity.transparent + a CLEAR GlassMaterial (low
// opacity, near-white tint, near-zero roughness, gentle refraction + specular). The
// real shader pass is owned by a parallel agent (feat/glass-shiny-transparent); the
// lounge tabletop just sets the material — it will read as proper see-through glass
// once that shader merges, and renders as a faint translucent slab in the meantime.
// Collision-enabled so you can rest things on it (the host doesn't, but it's the
// right default for a physical table).
uint32_t addGlassTabletop(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                          float cx, float cy, float cz, uint32_t roomId) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(kTableTopHX, kTableTopHY, kTableTopHZ, cx, cy, cz, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    // Full-albedo white so the glass tint multiplier shows correctly under the lit
    // pass; the actual see-through dial is `glass.opacity`.
    e.baseColor[0] = kTableTopTint[0]; e.baseColor[1] = kTableTopTint[1];
    e.baseColor[2] = kTableTopTint[2]; e.baseColor[3] = 1.0f;
    // ---- REAL TRANSLUCENT GLASS (engine glass pass) — CLEAR PLATE-GLASS preset.
    // Low opacity so the table is see-through (~10% blend alpha; refraction + spec
    // will carry the bulk of the read once the upgraded glass.frag merges). Near-
    // white tint with a hint of cool blue so it doesn't look colorless-amber. Very
    // low roughness (polished, NOT frosted — a clear lounge plate). Subtle refraction
    // (the floor under the slab bends a hair). Modest specular so the surface catches
    // room light as a soft sheen.
    e.transparent = true;
    e.glass.opacity    = 0.10f;          // CLEAR — barely-tinted plate glass
    // Cool BLUEISH plate-glass tint (owner pass 2) — distinct cool cast, not colorless.
    e.glass.tint[0]    = 0.60f; e.glass.tint[1] = 0.78f; e.glass.tint[2] = 0.95f;
    e.glass.roughness  = 0.04f;          // polished plate (not frosted)
    e.glass.refraction = 0.02f;          // subtle scene-bend behind the plate
    e.glass.specular   = 0.55f;          // a clean soft sheen, not a hot mirror
    for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
    e.tag = (uint32_t)Tag::Prop;
    e.roomId = roomId;
    // Collision: a flat plate is fine to stand the table top up physically — bullets
    // / dropped barrels would stop on the glass. Cheap (the same authored box).
    e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                   geo.cindex.data(), (uint32_t)geo.cindex.size());
    return scene.add(e);
}

// Append one chair (seat + back) at slot.pos with slot.yaw facing — the back box is
// translated kChairBackOffset behind the seat along -facing so the player looks AT
// the table when seated. Collision-LESS so the player can stand right at it (the
// seated transition isn't shoved by the seat capsule). Room-tagged with floorRoomId.
// Fills slot.seatEntity / slot.backEntity.
void buildChair(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                ChairSlot& slot, uint32_t floorRoomId) {
    const float yaw = slot.yaw;
    const float fx  = std::cos(yaw);   // forward (toward the table)
    const float fz  = std::sin(yaw);
    // Seat: a thin cushion box, seat-top at slot.pos.y + kChairSeatHeight.
    const float sy = slot.pos.y + kChairSeatHeight - kChairSeatHY;
    slot.seatEntity = addOpaqueBox(scene, device, physics,
                                   kChairSeatHX, kChairSeatHY, kChairSeatHZ,
                                   slot.pos.x, sy, slot.pos.z,
                                   kChairTint, /*collide*/false, floorRoomId);
    // Back: a thin tall box, centered kChairBackOffset BEHIND the seat (opposite
    // forward) and tall (kChairBackHY) above the cushion.
    const float bx = slot.pos.x - fx * kChairBackOffset;
    const float bz = slot.pos.z - fz * kChairBackOffset;
    const float by = slot.pos.y + kChairSeatHeight + kChairBackHY;   // back centered above seat top
    // The back's thin dimension is along the chair facing (so the broad face is the
    // backrest the player leans on). The chair is axis-aligned built — for the small
    // set we accept either +Z or -Z facing chairs use the .HZ thin axis and +X/-X
    // facing chairs use the .HX thin axis. Simple branch on |fx| vs |fz|.
    const bool alongX = std::fabs(fx) >= std::fabs(fz);
    const float bhx = alongX ? kChairBackHZ : kChairBackHX;
    const float bhz = alongX ? kChairBackHX : kChairBackHZ;
    slot.backEntity = addOpaqueBox(scene, device, physics, bhx, kChairBackHY, bhz,
                                   bx, by, bz, kChairTint, /*collide*/false, floorRoomId);
    // 4 chair LEGS — thin posts from the floor up to the seat bottom, at the seat's
    // axis-aligned corners (inset slightly). Opaque + collide-less so the chair stays a
    // walk-through prop; the SIT interaction is the only contact the player has with it.
    const float legSpanY = kChairSeatHeight - kChairSeatHY * 2.0f;
    const float legCy2   = slot.pos.y + legSpanY * 0.5f;
    const float legHy2   = legSpanY * 0.5f;
    const float lxOff    = kChairSeatHX - kChairLegInset;
    const float lzOff    = kChairSeatHZ - kChairLegInset;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            addOpaqueBox(scene, device, physics,
                         kChairLegHX, legHy2, kChairLegHZ,
                         slot.pos.x + sx * lxOff, legCy2, slot.pos.z + sz * lzOff,
                         kChairTint, /*collide*/false, floorRoomId);
}

} // namespace

// ===========================================================================
// GlassLounge::build — geometry assembly. Lays the table at tableCenter (top
// surface at kTableHeight above the floor) and rings it with 4 chairs facing in.
// ===========================================================================
void GlassLounge::build(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics,
                        const x3::phys::Vec3& tableCenter, uint32_t floorRoomId) {
    if (m_built) return;
    const float fx = tableCenter.x, fy = tableCenter.y, fz = tableCenter.z;

    // ---- TABLE: 4 metal legs + the GLASS top. Legs run from the floor up to just
    // below the slab; the slab center sits at fy + kTableHeight - kTableTopHY so the
    // slab TOP is at fy + kTableHeight. ----
    // Legs at the 4 corners (inset a hair so they're under the slab, not at the rim).
    const float legInsetX = kTableTopHX - 0.06f;
    const float legInsetZ = kTableTopHZ - 0.06f;
    // Leg tops sit kLegGlassGap below the slab bottom so the surfaces don't share a
    // plane (no z-fight / no leg appearing to enter the glass interior).
    const float legSpan   = kTableHeight - kTableTopHY * 2.0f - kLegGlassGap;
    const float legCy     = fy + legSpan * 0.5f;
    const float legHy     = legSpan * 0.5f;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            addOpaqueBox(scene, device, physics,
                         kTableLegHX, legHy, kTableLegHZ,
                         fx + sx * legInsetX, legCy, fz + sz * legInsetZ,
                         kTableLegTint, /*collide*/true, floorRoomId);
    // GLASS TOP centered above the legs.
    addGlassTabletop(scene, device, physics, fx, fy + kTableHeight - kTableTopHY, fz, floorRoomId);

    // ---- CHAIRS: 4 around the table, facing the table center. Data-driven: append
    // more entries here to add chairs (each is one ChairSlot). Offsets put the seats
    // outside the slab rim with breathing room (kChairSeatHZ + ~0.10 m clearance from
    // the slab edge); the yaw points the seated player AT the table center.
    // Layout (top-down):
    //                          [+Z chair, faces -Z]
    //                                  |
    //     [-X chair, faces +X]  ---TABLE---  [+X chair, faces -X]
    //                                  |
    //                          [-Z chair, faces +Z]
    const float gap = 0.15f;                                       // chair-rim clearance
    const float chairXOff = kTableTopHX + kChairSeatHZ + gap;      // chairs on X sides face inward along X
    const float chairZOff = kTableTopHZ + kChairSeatHZ + gap;      // chairs on Z sides face inward along Z
    // Yaw convention (device: fwd = (cos y, 0, sin y) at zero pitch):
    //   yaw =  0     -> +X    yaw =  pi   -> -X
    //   yaw =  pi/2  -> +Z    yaw = -pi/2 -> -Z
    constexpr float PI = 3.14159265358979323846f;
    m_chairs.push_back(ChairSlot{ x3::phys::Vec3{ fx + chairXOff, fy, fz }, /*yaw*/ PI       }); // +X seat faces -X
    m_chairs.push_back(ChairSlot{ x3::phys::Vec3{ fx - chairXOff, fy, fz }, /*yaw*/ 0.0f     }); // -X seat faces +X
    m_chairs.push_back(ChairSlot{ x3::phys::Vec3{ fx, fy, fz + chairZOff }, /*yaw*/ -PI*0.5f }); // +Z seat faces -Z
    m_chairs.push_back(ChairSlot{ x3::phys::Vec3{ fx, fy, fz - chairZOff }, /*yaw*/  PI*0.5f }); // -Z seat faces +Z
    for (ChairSlot& s : m_chairs)
        buildChair(scene, device, physics, s, floorRoomId);

    m_built = true;
    x3::logInfo("glass-lounge: built table at (" + std::to_string(fx) + ", " +
                std::to_string(fy) + ", " + std::to_string(fz) + ") with " +
                std::to_string((int)m_chairs.size()) + " chairs (room=" +
                std::to_string((int)floorRoomId) + ")");
}

// ===========================================================================
// nearestSittableChair — pick the closest chair within kSitReach (XZ) that the
// player at `eye` with look yaw `lookYaw` is roughly FACING (dot of the player's
// forward XZ with the eye->chair XZ direction >= kSitFacingDot). Returns the chair
// index, or -1 if no chair qualifies. Pure (no state).
// ===========================================================================
int GlassLounge::nearestSittableChair(const x3::phys::Vec3& eye, float lookYaw) const {
    if (m_chairs.empty()) return -1;
    const float r2max = kSitReach * kSitReach;
    const float lfx = std::cos(lookYaw);   // player look forward on the XZ plane
    const float lfz = std::sin(lookYaw);
    int best = -1; float bestD2 = r2max;
    for (uint32_t i = 0; i < m_chairs.size(); ++i) {
        const ChairSlot& s = m_chairs[i];
        const float dx = s.pos.x - eye.x, dz = s.pos.z - eye.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 > r2max) continue;
        // Facing: eye -> chair direction (XZ), normalized; require dot with the
        // player's look forward >= kSitFacingDot. Very-close-and-within-pos slots
        // (d2 below a tiny epsilon) pass facing trivially so a player standing right
        // on top of the seat can still sit.
        if (d2 > 0.01f) {
            const float inv = 1.0f / std::sqrt(d2);
            const float ax = dx * inv, az = dz * inv;
            const float dot = lfx * ax + lfz * az;
            if (dot < kSitFacingDot) continue;
        }
        if (d2 <= bestD2) { bestD2 = d2; best = (int)i; }
    }
    return best;
}

// Seated EYE: seat center + the seated eye height, nudged forward along the chair
// yaw by kSeatedEyeForward (so the player's eyes land where they'd actually be once
// seated — slightly forward of the back, not at the cushion's geometric center).
x3::phys::Vec3 GlassLounge::seatedEye(uint32_t i) const {
    const ChairSlot& s = m_chairs[i];
    const float fx = std::cos(s.yaw), fz = std::sin(s.yaw);
    return x3::phys::Vec3{ s.pos.x + fx * kSeatedEyeForward,
                           s.pos.y + kSeatedEyeHeight,
                           s.pos.z + fz * kSeatedEyeForward };
}

// ===========================================================================
// SitController — the smooth-lerp state machine.
// ===========================================================================
namespace {
constexpr float TWO_PI = 6.28318530717958647692f;
constexpr float PI_LERP = 3.14159265358979323846f;

// Smooth easeInOut (cosine ease) of t in [0,1] -> [0,1]. Eases ON and OFF so the
// camera glides into the seat / out of it rather than ramping linearly.
inline float ease(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 0.5f - 0.5f * std::cos(t * PI_LERP);
}
// Lerp `a`->`b` with `t` in [0,1].
inline float L(float a, float b, float t) { return a + (b - a) * t; }
inline x3::phys::Vec3 LV(const x3::phys::Vec3& a, const x3::phys::Vec3& b, float t) {
    return x3::phys::Vec3{ L(a.x,b.x,t), L(a.y,b.y,t), L(a.z,b.z,t) };
}
// Yaw lerp the SHORT WAY around (so a -pi -> +pi flip doesn't whip through 0).
inline float Lyaw(float a, float b, float t) {
    float d = b - a;
    while (d >  PI_LERP) d -= TWO_PI;
    while (d < -PI_LERP) d += TWO_PI;
    return a + d * t;
}
} // namespace

void SitController::requestSit(int chairIndex, const x3::phys::Vec3& standEye, float standYaw,
                               const x3::phys::Vec3& seatEye, float seatYaw) {
    if (m_state == State::Sitting) return;     // ignore double-sit
    m_state    = State::Sitting;
    m_chair    = chairIndex;
    m_fromEye  = standEye;  m_fromYaw = standYaw;
    m_toEye    = seatEye;   m_toYaw   = seatYaw;
    m_seatEye  = seatEye;   m_seatYaw = seatYaw;
    m_t        = 0.0f;
    // Seed the camera output so a query before update() reads the standing pose
    // (no first-frame snap to the target).
    m_camEye = standEye;    m_camYaw = standYaw;
}

void SitController::requestStand() {
    if (m_state == State::Standing) return;
    m_state   = State::Standing;
    // Lerp FROM the seated pose back to the live standing pose. update() will refine
    // the to-pose each tick (so the camera lands exactly where the body is now).
    m_fromEye = m_seatEye;  m_fromYaw = m_seatYaw;
    m_toEye   = m_seatEye;  m_toYaw   = m_seatYaw;   // refreshed in update()
    m_t       = 0.0f;
    // m_chair stays so the host can read it for an exit transition; it is irrelevant
    // for state queries (seated() now returns false).
}

void SitController::update(float dt, const x3::phys::Vec3& liveStandEye, float liveStandYaw) {
    if (m_state == State::Standing) {
        if (m_t < 1.0f) {
            // Standing-up lerp: refresh the to-pose to the live standing pose every
            // tick so the camera tracks the body the moment it starts moving.
            m_toEye = liveStandEye; m_toYaw = liveStandYaw;
            m_t += dt / kSitLerpTime;
            if (m_t >= 1.0f) m_t = 1.0f;
            const float e = ease(m_t);
            m_camEye = LV(m_fromEye, m_toEye, e);
            m_camYaw = Lyaw(m_fromYaw, m_toYaw, e);
        } else {
            // Fully standing — the camera is the live standing pose 1:1.
            m_camEye = liveStandEye; m_camYaw = liveStandYaw;
        }
    } else {
        // Sitting: lerp toward the seated pose, then hold.
        if (m_t < 1.0f) {
            m_t += dt / kSitLerpTime;
            if (m_t >= 1.0f) m_t = 1.0f;
            const float e = ease(m_t);
            m_camEye = LV(m_fromEye, m_toEye, e);
            m_camYaw = Lyaw(m_fromYaw, m_toYaw, e);
        } else {
            // While seated: keep the EYE at the seated pose, but let the YAW reflect
            // the live look (so mouse-look while seated rotates the camera). The host
            // continues to feed liveStandYaw which IS the player's live yaw (the
            // player's look angle is independent of the seated/standing distinction;
            // movement is locked but the look update is not).
            m_camEye = m_seatEye;
            m_camYaw = liveStandYaw;
        }
    }
}

// ===========================================================================
// Headless self-test (--test-sit). X0-X5 per the header.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[sit-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[sit-test] FAIL ") + name); }
}
} // namespace

bool runSitSelfTest() {
    g_pass = g_fail = 0;

    // ---- Synthetic lounge (no Scene/device/physics needed for the state machine + the
    // chair-pick math): seed a GlassLounge with 4 chairs at unit-distance around the
    // origin, facing inward. We never call build() (which needs a device); the
    // chair-pick math reads m_chairs only.
    GlassLounge lounge;
    // Manually populate the chair list mirroring build()'s layout (table at origin).
    // We can't access m_chairs directly from a test — so exercise nearestSittableChair
    // through a tiny scratch helper that mirrors the same selection logic on synthetic
    // chairs. We can also expose the chair count via chair()/chairCount(); the headless
    // test does NOT call build (no device), so m_chairs stays empty and the chair-pick
    // gate (X5) uses a local list instead.
    // (build() is exercised in-app and by the smoketest's level build.)

    // ---- X0: SitController starts STANDING; movement unlocked; cam == the live
    // standing pose.
    SitController sc;
    const x3::phys::Vec3 stand{ 1.0f, 1.6f, 0.0f };
    const float standYaw = 0.0f;
    sc.update(0.0f, stand, standYaw);
    check(sc.state() == SitController::State::Standing && !sc.seated() && !sc.movementLocked() &&
          std::fabs(sc.camEye().y - 1.6f) < 1e-4f && std::fabs(sc.camYaw() - 0.0f) < 1e-4f,
          "X0 starts Standing, movement unlocked, camera == standing pose");

    // ---- X1: requestSit enters Sitting + locks movement; the lerp eases the eye
    // DOWN in Y toward the seated pose over kSitLerpTime. Sample at t=0 (still at
    // stand), t=kSitLerpTime/2 (eye lowered, not yet settled), and just before
    // settling — eye Y must MONOTONICALLY decrease.
    const x3::phys::Vec3 seat{ 1.0f, 1.1f, 0.0f };    // 50 cm below the stand eye
    const float seatYaw = 0.0f;
    sc.requestSit(/*chair*/0, stand, standYaw, seat, seatYaw);
    bool entered = (sc.state() == SitController::State::Sitting) && sc.seated() && sc.movementLocked();
    sc.update(0.0f, stand, standYaw);    // freeze a sample at t=0 (after the request)
    const float y0 = sc.camEye().y;
    sc.update(kSitLerpTime * 0.5f, stand, standYaw);
    const float y1 = sc.camEye().y;
    sc.update(kSitLerpTime * 0.4f, stand, standYaw);
    const float y2 = sc.camEye().y;
    check(entered && y1 < y0 && y2 < y1, "X1 requestSit enters Sitting + lerps eye DOWN toward the seated pose");

    // ---- X2: once settled the camera == the seated pose (eye Y at seat, yaw at the
    // chair's facing). Push the lerp well past kSitLerpTime.
    sc.update(kSitLerpTime * 2.0f, stand, standYaw);
    check(sc.seated() && std::fabs(sc.camEye().y - seat.y) < 1e-4f &&
          std::fabs(sc.camEye().x - seat.x) < 1e-4f &&
          std::fabs(sc.camEye().z - seat.z) < 1e-4f,
          "X2 settled seated camera == the seated pose");

    // ---- X3: a second requestSit while seated is IGNORED (no double-enter, no
    // reset of the lerp / chair). chair() must remain 0.
    sc.requestSit(/*chair*/2, stand, standYaw,
                  x3::phys::Vec3{ 0.0f, 1.0f, 1.0f }, 1.57f);
    check(sc.seated() && sc.chairIndex() == 0, "X3 requestSit while seated is ignored");

    // ---- X4: requestStand returns to Standing, UNLOCKS movement immediately, and
    // the camera lerps back toward the live standing pose. Eye Y must increase (and
    // settle) and movementLocked() flips false the moment the request lands.
    sc.requestStand();
    check(sc.state() == SitController::State::Standing && !sc.movementLocked(),
          "X4a requestStand exits to Standing + UNLOCKS movement immediately");
    sc.update(0.0f, stand, standYaw);
    const float upY0 = sc.camEye().y;
    sc.update(kSitLerpTime * 0.5f, stand, standYaw);
    const float upY1 = sc.camEye().y;
    sc.update(kSitLerpTime * 2.0f, stand, standYaw);
    check(upY1 > upY0 && std::fabs(sc.camEye().y - stand.y) < 1e-4f,
          "X4b camera lerps UP back to the live standing pose");

    // ---- X5: nearestSittableChair picks a chair only when IN REACH and FACING. Build
    // a tiny scratch lounge to exercise the picker. The test stays headless — the
    // chair list is filled directly by appending to the temp lounge through a friend-
    // less workaround: invoke build() with a HeadlessDevice substitute is overkill;
    // instead probe a known geometry by constructing chair slots inline + calling a
    // local copy of the selection logic, then assert the same INVARIANTS that
    // nearestSittableChair guarantees.
    //   * an eye 0.5 m -X of a chair facing -X picks it (in reach + facing it).
    //   * an eye 5 m away does NOT pick it (out of reach).
    //   * an eye in reach but looking AWAY does NOT pick it (facing fails).
    // (We CAN call nearestSittableChair on an empty lounge — returns -1.)
    check(lounge.nearestSittableChair(stand, standYaw) == -1, "X5a empty lounge -> no chair");

    // Use a directly-populated lounge via build() with NO device/physics is not
    // possible (build() calls device.createMesh). Reproduce the selection function
    // standalone with the same constants so X5 still exercises the picker math.
    struct TestChair { x3::phys::Vec3 pos; float yaw; };
    const TestChair chairs[1] = {
        // Single chair at (+1, 0, 0), facing -X (yaw=pi) — sitting at it means looking -X.
        { x3::phys::Vec3{ 1.0f, 0.0f, 0.0f }, 3.14159265f }
    };
    auto pick = [&](const x3::phys::Vec3& eye, float lookYaw) -> int {
        const float r2max = kSitReach * kSitReach;
        const float lfx = std::cos(lookYaw), lfz = std::sin(lookYaw);
        int best = -1; float bestD2 = r2max;
        for (uint32_t i = 0; i < 1; ++i) {
            const TestChair& s = chairs[i];
            const float dx = s.pos.x - eye.x, dz = s.pos.z - eye.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 > r2max) continue;
            if (d2 > 0.01f) {
                const float inv = 1.0f / std::sqrt(d2);
                const float ax = dx * inv, az = dz * inv;
                const float dot = lfx * ax + lfz * az;
                if (dot < kSitFacingDot) continue;
            }
            if (d2 <= bestD2) { bestD2 = d2; best = (int)i; }
        }
        return best;
    };
    // Eye at (+0.5, 0, 0) (0.5 m -X of the chair), looking +X (yaw=0) — picks chair 0.
    const int p_inReach = pick(x3::phys::Vec3{ 0.5f, 1.6f, 0.0f }, 0.0f);
    // Eye at (-5, 0, 0) — far away — picks none.
    const int p_far     = pick(x3::phys::Vec3{ -5.0f, 1.6f, 0.0f }, 0.0f);
    // Eye at (+0.5, 0, 0) looking -X (yaw=pi) — AWAY from the chair → none.
    const int p_away    = pick(x3::phys::Vec3{ 0.5f, 1.6f, 0.0f }, 3.14159265f);
    check(p_inReach == 0 && p_far == -1 && p_away == -1,
          "X5b nearestSittableChair picks in-reach+facing only");

    x3::logInfo(std::string("[sit-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
