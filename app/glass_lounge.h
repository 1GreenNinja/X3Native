#pragma once
// GLASS LOUNGE + SIT-AT-CHAIR interaction (Tim's vision). A small social set-piece
// in the open area behind the B1 detention cell: a clear plate-GLASS table (a thin
// see-through glass slab top on 4 opaque metal legs) ringed by simple opaque chairs
// (a seat box + a back box). The player can walk up to a chair, face it, press E to
// SIT (the camera lerps to a lowered seated pose oriented at the table, movement
// locks while free mouse-look stays), and press E again (or any movement key) to
// STAND back up.
//
// Clean-room: Scene/Entity + IRenderDevice + IPhysicsWorld + mesh_prims box builder
// only (the same building blocks as level1.cpp / holo_terminal.cpp). No purchased
// C# / id Tech source consulted.
//
// Two cleanly separated pieces:
//   * GlassLounge — builds the table + a data-driven list of chairs into the Scene
//     (room-tagged to B1 so the per-floor occlusion cull keeps them, frustum-cullable
//     via their mesh AABB). The tabletop is the GLASS entity (Entity.transparent +
//     a CLEAR GlassMaterial), matching the holo-terminal glass pattern.
//   * SitController — a tiny, deviceless STATE MACHINE (Standing <-> Sitting with a
//     smooth lerp) the host drives from the E-interact path + the camera readback.
//     Headless-testable (--test-sit) with no GLFW/Vulkan.
//
// The chairs are a small POSITION list (kept data-driven) so more can be added by
// appending one ChairSlot — see GlassLounge::build().
#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// One authored chair around the lounge table. `pos` is the seat CENTER on the floor
// (world; pos.y == the floor the chair stands on). `yaw` is the facing the seated
// player is oriented to (radians; the device forward convention fwd =
// (cos y, 0, sin y) at zero pitch — yaw points the camera AT the table). The seat
// entity ids are filled by build() (the chair is collision-LESS so the player can
// stand right at it and the seated pose isn't shoved by the seat capsule).
struct ChairSlot {
    x3::phys::Vec3 pos{};        // seat center, on the floor (world)
    float          yaw = 0.0f;   // facing toward the table (radians)
    uint32_t       seatEntity = kNoLink;
    uint32_t       backEntity = kNoLink;
};

// Eye height (m above the floor) of the SEATED pose — lower than the standing eye
// (~1.6 m). A person sitting in a chair has eyes ~1.1 m up.
inline constexpr float kSeatedEyeHeight = 1.1f;
// How far in front of the seat center (along the chair's facing) the seated eye sits
// — the player slides onto the seat, eyes a touch back from the chair center.
inline constexpr float kSeatedEyeForward = 0.0f;
// Reach (m, XZ) within which a chair offers the "[E] Sit" prompt + accepts the sit.
inline constexpr float kSitReach = 1.2f;
// Minimum facing alignment (dot of the player's look-forward with the chair->table
// direction) to offer the sit prompt: the player must be roughly facing the chair.
inline constexpr float kSitFacingDot = 0.30f;   // ~72 deg cone
// Seconds for the sit/stand camera lerp (a smooth ease, not a snap).
inline constexpr float kSitLerpTime = 0.35f;

// The lounge: a glass table + a ring of chairs, built once into the B1 plate area.
class GlassLounge {
public:
    // Build the table + chairs into `scene` (render via `device`, collision via
    // `physics`). `tableCenter` is the table center on the floor (world; .y == the
    // floor Y). `floorRoomId` is the L1Floor index (== (uint32_t)L1Floor::B1) tagged
    // onto every lounge entity so the per-floor cull keeps them with the floor.
    // Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics,
               const x3::phys::Vec3& tableCenter, uint32_t floorRoomId);

    bool built() const { return m_built; }

    // The chair list (data-driven; the host queries nearestChair for the interaction).
    uint32_t          chairCount() const { return (uint32_t)m_chairs.size(); }
    const ChairSlot&  chair(uint32_t i) const { return m_chairs[i]; }

    // Index of the chair the player at `eye` is closest to AND roughly facing within
    // kSitReach, or -1 if none qualifies. `lookYaw` is the player's look yaw (radians).
    // Used by the host to decide whether to show the "[E] Sit" prompt + which chair to
    // seat at. Pure query (no state).
    int nearestSittableChair(const x3::phys::Vec3& eye, float lookYaw) const;

    // The world EYE pose a seated player occupies at chair `i`: position (seat center
    // + the seated eye height, nudged forward by kSeatedEyeForward) and facing yaw.
    // The host lerps the camera to this while seated. Caller must pass a valid index.
    x3::phys::Vec3 seatedEye(uint32_t i) const;
    float          seatedYaw(uint32_t i) const { return m_chairs[i].yaw; }

private:
    std::vector<ChairSlot> m_chairs;
    bool                   m_built = false;
};

// ---------------------------------------------------------------------------
// SIT STATE MACHINE — deviceless, headless-testable. The host feeds it the player's
// standing eye/yaw each frame plus edge events (sit request / stand request), and
// reads back the CURRENT camera eye/yaw to issue to setCamera + whether movement is
// locked. The transition is a smooth lerp over kSitLerpTime (so the camera eases into
// / out of the seat rather than snapping).
// ---------------------------------------------------------------------------
class SitController {
public:
    enum class State : uint32_t { Standing = 0, Sitting = 1 };

    State state() const { return m_state; }
    bool  seated() const { return m_state == State::Sitting; }
    int   chairIndex() const { return m_chair; }      // the chair being sat in, or -1
    // True while the camera is mid-lerp (between standing and seated). The host can
    // suppress re-prompting / re-firing while a transition is animating.
    bool  transitioning() const { return m_t > 0.0f && m_t < 1.0f; }

    // Begin sitting at chair `chairIndex`, lerping FROM the current standing eye/yaw
    // TO the seated pose. Ignored if already sitting. `seatEye`/`seatYaw` are the
    // target seated pose (from GlassLounge::seatedEye/seatedYaw).
    void requestSit(int chairIndex, const x3::phys::Vec3& standEye, float standYaw,
                    const x3::phys::Vec3& seatEye, float seatYaw);

    // Begin standing up, lerping FROM the seated pose BACK toward the live standing
    // eye/yaw. Ignored if already standing. The host keeps feeding the live standing
    // pose via update() so the camera lands exactly where the body is.
    void requestStand();

    // Advance the lerp by `dt`. `liveStandEye`/`liveStandYaw` are the player's CURRENT
    // standing eye/yaw (the camera readback) — used as the lerp endpoint when standing
    // up AND as the held value while fully standing. Returns nothing; query camEye()/
    // camYaw()/movementLocked() afterward.
    void update(float dt, const x3::phys::Vec3& liveStandEye, float liveStandYaw);

    // The camera EYE the host should issue this frame (lerped while transitioning,
    // the seated pose while fully seated, the live standing eye while fully standing).
    x3::phys::Vec3 camEye() const { return m_camEye; }
    float          camYaw() const { return m_camYaw; }

    // True iff player MOVEMENT should be locked this frame (seated OR sitting-down
    // mid-lerp). Standing up unlocks immediately so a tapped movement key walks away.
    bool movementLocked() const { return m_state == State::Sitting; }

private:
    State          m_state = State::Standing;
    int            m_chair = -1;
    float          m_t = 1.0f;        // lerp progress 0..1 (1 == settled)
    // Lerp endpoints (eye + yaw). m_fromYaw/m_toYaw are unwrapped so the yaw lerp
    // takes the short way around.
    x3::phys::Vec3 m_fromEye{}, m_toEye{};
    float          m_fromYaw = 0.0f, m_toYaw = 0.0f;
    // Held seated target (so standing-up re-uses the seated start exactly).
    x3::phys::Vec3 m_seatEye{};
    float          m_seatYaw = 0.0f;
    // The camera output for the current frame.
    x3::phys::Vec3 m_camEye{};
    float          m_camYaw = 0.0f;
};

// Headless self-test (--test-sit). Drives the SitController state machine through
// enter -> seated -> stand with no GLFW/Vulkan and asserts:
//   X0 starts Standing, movement unlocked, camera == the live standing pose;
//   X1 requestSit enters Sitting, locks movement, and the lerp eases the eye toward
//      the seated pose (DOWN in Y) over kSitLerpTime;
//   X2 once settled the camera == the seated pose (lower eye, table-facing yaw);
//   X3 a second sit request while seated is ignored (no double-enter);
//   X4 requestStand returns to Standing, UNLOCKS movement, and the camera lerps back
//      to the live standing pose;
//   X5 GlassLounge::nearestSittableChair picks a chair only when in reach AND facing.
// Logs PASS/FAIL X#, returns true iff all pass.
bool runSitSelfTest();

} // namespace x3::game
