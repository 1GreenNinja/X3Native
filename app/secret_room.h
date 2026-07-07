#pragma once
// CODE-LOCKED TRAPDOOR -> SECRET ROOM (Tim's vision). A cohesive Level-1 feature
// that wires the existing building blocks together:
//   * HoloTerminal (app/holo_terminal.*) in Jake's cell — enter the override code.
//   * A FLOOR-HATCH door (app/door.*, DoorSpec.floorHatch) cut into the cell floor,
//     starting LOCKED. The terminal's submit sink unlocks + opens it on the code.
//   * A SECRET ROOM built BELOW the cell (negative Y) — reachable by dropping
//     through the open hatch. Stocked per the vision: many emissive sci-fi SCREENS
//     on the walls, a glowing TREASURE cache, a WEAPON pickup, HEALTH packs, and a
//     special NANO-BOOSTER pickup (a flagged tech/bio upgrade).
//
// Game/slice code only — engine/ stays pure. This is additive: it owns its own
// HoloTerminal + a floor-hatch door registered in the host's DoorSystem + a
// WeaponSystem for the secret weapon, and a small set of pickup props. The host
// (Level1Game) builds it in build() and ticks it in tick(); the headless
// --test-secretroom drives the same logic.
#include "scene.h"
#include "holo_terminal.h"
#include "door.h"
#include "weapon.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// The override code that opens the cell trapdoor (the lore code, matching the
// detention-terminal readout). Entering exactly this on the HoloTerminal unlocks
// + opens the floor hatch.
inline constexpr const char* kSecretRoomCode = "1278";   // (1127 is Door C / Club 1127 — keep this distinct)

// The hatch/terminal SFX kit (mirrors ElevatorSounds — the elevator Glow-Up
// treatment applied to the trapdoor). All loads are graceful: an invalid handle
// simply keeps that one cue silent, never a crash.
struct SecretRoomSounds {
    x3::audio::SoundHandle buzz;    // wrong override code (terminal reject)
    x3::audio::SoundHandle chime;   // access granted — fires ONCE on the unlock edge
    x3::audio::SoundHandle servo;   // looped heavy servo while the panels slide
    x3::audio::SoundHandle thunk;   // panels seat at end of travel (open OR closed)
};

// One collectible in the secret room. A pickup prop with a kind, an emissive look,
// and a collected latch (the player walking within radius collects it). HEALTH and
// NANO carry a payload the host applies (heal amount / the nano upgrade flag).
struct SecretPickup {
    enum class Kind : uint32_t { Treasure = 0, Health = 1, NanoBooster = 2 };
    Kind           kind = Kind::Treasure;
    uint32_t       entity = kNoLink;     // the visible prop entity in the Scene
    x3::phys::Vec3 pos{};                // world center
    int            healAmount = 0;       // HEALTH: HP restored on pickup
    bool           collected = false;    // latched once the player walks into it
};

// The secret-room system: builds the trapdoor + the room + its contents, owns the
// in-cell HoloTerminal and the secret weapon pickup, and runs per-frame pickup
// collection. Built once on top of an already-built Level-1 cell.
class SecretRoom {
public:
    // Build the whole feature into `scene` (render via `device`, collision via
    // `physics`). `cellCenter` is Jake's cell center (world; the cell floor is at
    // cellCenter.y). `doors` is the host's DoorSystem (the hatch is registered
    // there so the existing door update/animation drives it). `modelDir` is the
    // loose-GLB dir for the secret weapon pickup. Wires the terminal submit sink to
    // unlock+open the hatch on the correct code. Call once.
    // The three trailing params RELOCATE the feature for the data-driven canon cell
    // (--world canonlevel): hatchCx/hatchCz place the floor-hatch opening in world XZ
    // (NaN = the legacy level1.h kCellHatch* spot, so existing callers are
    // byte-identical), and cellCeilY pins the terminal's ceiling support pipe to the
    // REAL room ceiling (<= cellCenter.y means "legacy": cellCenter.y + 2.8).
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, DoorSystem& doors,
               const x3::phys::Vec3& cellCenter, std::string_view modelDir,
               float hatchCx = std::numeric_limits<float>::quiet_NaN(),
               float hatchCz = std::numeric_limits<float>::quiet_NaN(),
               float cellCeilY = 0.0f);

    // Advance one frame: tick the terminal cursor blink, and run pickup collection
    // against the player position (collecting health/nano/treasure within radius).
    // Returns events via the queries below (latched flags). `heal` may be null (a
    // headless geometry test); when set, HEALTH packs call heal(amount) once.
    void tick(float dt, Scene& scene, const x3::phys::Vec3& playerPos,
              const std::function<void(int)>& heal = {});

    // ---- HoloTerminal access (the host routes input / draws text over it). ----
    HoloTerminal&       terminal()       { return m_terminal; }
    const HoloTerminal& terminal() const { return m_terminal; }

    // ---- Audio (the elevator treatment): wire once after boot audio exists.
    // tick() drives the servo loop + thunk off the hatch door-state edges (so EVERY
    // open path sounds — terminal sink, Lua script, console) and the chime off the
    // status-lens unlock edge; the submit sink fires the wrong-code buzz. ----
    void setSounds(x3::audio::IAudioSystem* audio, const SecretRoomSounds& s) {
        m_audio = audio; m_snd = s;
    }

    // ---- Hatch queries (HUD + the self-test). ----
    uint32_t  hatchDoorIndex() const { return m_hatchIdx; }       // index into the host DoorSystem
    bool      hatchBuilt() const { return m_hatchIdx != kNoLink; }
    // Submit a code to the terminal sink directly (the host can also pump chars +
    // submit() through the terminal). Returns true iff the code was accepted (the
    // hatch unlocked + began opening). Pure convenience for the test + console.
    bool submitCode(const std::string& code, DoorSystem& doors);

    // ---- Secret-room queries (HUD + self-test). ----
    bool      built() const { return m_built; }
    x3::phys::Vec3 roomCenter() const { return m_roomCenter; }
    float     roomFloorY() const { return m_roomFloorY; }
    uint32_t  pickupCount() const { return (uint32_t)m_pickups.size(); }
    const SecretPickup& pickup(uint32_t i) const { return m_pickups[i]; }
    uint32_t  screenCount() const { return m_screenCount; }    // emissive wall displays
    const WeaponSystem& secretWeapon() const { return m_weapon; }
    WeaponSystem&       secretWeapon()       { return m_weapon; }

    // The NANO-BOOSTER upgrade: set true the frame the player collects the nano pickup.
    // The host reads this to apply the tech/bio upgrade (see the build() TODO — a
    // deep upgrade system would consume this flag; today it is a clear, queryable boost
    // flag + an optional +maxHP-style effect routed through the heal callback).
    bool nanoBoosterActive() const { return m_nanoActive; }

    // Treasure / health collected counts (HUD).
    uint32_t treasureCollected() const { return m_treasureGot; }
    uint32_t healthCollected() const   { return m_healthGot; }

    // Draw the secret weapon pickup (bob/spin) — host calls in the draw block.
    void drawExtras(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    const Scene& scene) const { m_weapon.drawPickup(device, frame, scene); }

private:
    // Append one emissive "screen"/display quad to the wall. Returns the entity id.
    uint32_t addScreen(Scene& scene, x3::rhi::IRenderDevice& device,
                       const x3::phys::Vec3& center, float yaw,
                       float w, float h, float r, float g, float b, float strength);
    // Append one collectible prop (emissive box) and register it in m_pickups.
    uint32_t addPickup(Scene& scene, x3::rhi::IRenderDevice& device,
                       SecretPickup::Kind kind, const x3::phys::Vec3& pos,
                       int healAmount, float r, float g, float b, float strength,
                       float halfSize);

    HoloTerminal   m_terminal;
    WeaponSystem   m_weapon;            // the secret weapon pickup
    DoorSystem*    m_doorsPtr = nullptr; // the host DoorSystem holding the hatch
    uint32_t       m_hatchIdx = kNoLink; // hatch index in the host DoorSystem
    // ---- Hatch audio state (see setSounds). ----
    x3::audio::IAudioSystem* m_audio = nullptr;
    SecretRoomSounds m_snd{};
    x3::audio::LoopHandle m_servoVoice{};   // live while the panels slide
    bool           m_hatchWasMoving = false; // Opening/Closing edge tracker
    x3::phys::Vec3 m_roomCenter{};
    float          m_roomFloorY = 0.0f;
    std::vector<SecretPickup> m_pickups;
    uint32_t       m_screenCount = 0;
    bool           m_nanoActive  = false;
    uint32_t       m_treasureGot = 0;
    uint32_t       m_healthGot   = 0;
    bool           m_built = false;
    // AAA hatch read (R6): the status-light entity on the hatch rim — RED while the
    // hatch is locked, flipped GREEN by tick() the frame it unlocks/opens.
    uint32_t       m_statusEnt = kNoLink;
    bool           m_statusGreen = false;
};

// Radius (m) within which walking into a secret-room pickup collects it (XZ plane).
inline constexpr float kSecretPickupRadius = 1.1f;

// Headless self-test (--test-secretroom). Builds the cell + the secret room on a
// HeadlessDevice + Jolt world and asserts S1-S6:
//   S1 the hatch starts LOCKED + Closed;
//   S2 a WRONG code does NOT open it (stays locked + closed);
//   S3 the CORRECT code (via the terminal submit sink) unlocks + opens it;
//   S4 the secret room geometry exists BELOW the cell (a floor at negative Y);
//   S5 the room is stocked: >=1 weapon, >=1 health, >=1 treasure, >=1 nano, many screens;
//   S6 a body dropped through the OPEN hatch reaches the secret-room floor.
// Logs PASS/FAIL S#, returns true iff all pass. No window/Vulkan.
bool runSecretRoomSelfTest();

} // namespace x3::game
