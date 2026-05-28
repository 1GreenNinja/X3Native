#pragma once
// HOLO-TERMINAL KIOSK SYSTEM (placeable terminals). The Spire now has N
// holographic kiosks scattered around B1; each is a clear-glass panel with a
// glowing UI texture (see app/holo_terminal.*) PLUS per-instance metadata: a
// unique id, a display label, an optional access code, a TerminalCommand tag
// that drives a real-world effect when the player submits the code, and a roomId
// for the per-floor cull. Mirrors the data-driven `glass_lounge` pattern.
//
// Interaction (host-driven, same input gate as the cell terminal):
//     idle  --[E near a kiosk]-->  using
//     using --[type chars/digits]-->  typing (input field grows)
//     using --[Enter]-->  EXECUTE (command dispatch) --> idle
//     using --[Esc / E again]-->  idle (no-op)
// While `using`, movement+firing are LOCKED (the host respects the same gate as
// the cell terminal: termMode + codeMode + uiMenu + sit suppress gameplay input).
//
// CLEAN-ROOM: built atop the existing HoloTerminal (per-instance render+input
// state) + Scene/Entity + DoorSystem + IRenderDevice. No third-party source
// consulted.

#include "holo_terminal.h"
#include "scene.h"
#include "door.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// What a terminal DOES when its code is accepted (or just "use" if no code).
// Each command reads a small per-instance param block (see TerminalParams).
enum class TerminalCommand : uint32_t {
    None        = 0,   // no effect (a pure information / lore terminal uses ShowLore instead)
    UnlockCell  = 1,   // open the cell trapdoor (the legacy code-1278 terminal)
    OpenDoor    = 2,   // unlock + open a specific door (DoorSystem index in params.doorIdx)
    ToggleLights = 3,  // cycle nearby point lights' intensity: full -> dim -> off -> full
    TriggerAlarm = 4,  // ~3s screen tint flash + ambient pulse, then auto-reset
    ShowLore    = 5,   // dump params.lore lines onto the terminal's readout (re-bakes glass)
    SpawnCrate  = 6,   // pop a glowing crate-prop in the world (a small treasure)
};

// Per-instance parameters carried alongside the command. Only the fields relevant
// to the command are read; the rest stay default. Designed to be appended in
// future without breaking call sites.
struct TerminalParams {
    // OpenDoor: the index into the host DoorSystem this command unlocks+opens.
    uint32_t doorIdx = kNoLink;
    // ToggleLights: anchor + radius (XZ, world). The system finds the K nearest
    // PointLights inside this radius and cycles them through full/dim/off.
    x3::phys::Vec3 lightAnchor{};
    float          lightRadius = 6.0f;
    uint32_t       lightMaxAffect = 3;
    // ShowLore: the lines to dump onto the terminal's readout (replaces lines 1+).
    // Line 0 is taken from the terminal's `label` so the header still reads.
    std::vector<std::string> lore;
    // SpawnCrate: world position for the spawned prop (filled in command dispatch).
    x3::phys::Vec3 cratePos{};
};

// One placed kiosk. The `terminal` carries the rendered glass + input state;
// the metadata in this struct is what makes each kiosk DIFFERENT from the next.
struct TerminalInstance {
    std::string id;                    // unique id (e.g. "cell_lock", "lights_central")
    std::string label;                 // short display name (used in the [E] prompt + header)
    std::string code;                  // empty == "press E + Enter" with no code needed
    TerminalCommand command = TerminalCommand::None;
    TerminalParams  params;
    x3::phys::Vec3  pos{};             // world panel center
    float           yaw = 0.0f;        // panel facing (radians; the device fwd convention)
    float           width = 1.4f;
    float           height = 0.9f;
    float           ceilingY = 0.0f;   // 0 == auto (pos.y + 1.7)
    uint32_t        roomId = kNoRoom;  // per-floor cull tag (== (uint32_t)L1Floor::B1 for kiosks on B1)
    // The renderer + input state. The system OWNS a HoloTerminal for every kiosk
    // it places in the world (via add()). The legacy cell-escape terminal lives
    // in SecretRoom and is registered into this system by reference (addExternal)
    // so the kiosk registry still counts it + the input/code path is unified; in
    // that case the unique_ptr stays empty and `external` is the live pointer.
    std::unique_ptr<HoloTerminal> owned;
    HoloTerminal*  external = nullptr;
    HoloTerminal&       terminal()       { return external ? *external : *owned; }
    const HoloTerminal& terminal() const { return external ? *external : *owned; }
    // Lore overlay timeout: when > 0 the terminal is displaying ShowLore output;
    // counts down and resets to the original boot readout when it hits 0. While
    // active the terminal stays interactable but a re-fire of ShowLore just
    // re-arms the timer.
    float           loreTimer = 0.0f;
};

// Live effect state — alarm flash etc. Polled by the host so it can render the
// screen-tint overlay + drive SkyParams during the brief alarm pulse.
struct TerminalAlarmState {
    bool  active = false;
    float timer = 0.0f;      // seconds remaining
    float total = 3.0f;      // total duration (for the host's eased ramp)
};

// Reach (m, XZ) within which a kiosk offers the [E] prompt + accepts the press.
inline constexpr float kKioskReach = 2.5f;

class HoloTerminalSystem {
public:
    // Append a kiosk. The HoloTerminal is allocated + `build()` is called with
    // the instance's pos/yaw/size/roomId. Subsequent calls to setLines/addLine on
    // the returned reference update the readout. Returns the index of the new
    // instance (stable for the system's lifetime).
    uint32_t add(Scene& scene, x3::rhi::IRenderDevice& device,
                 const std::string& id, const std::string& label,
                 const std::string& code, TerminalCommand cmd,
                 const TerminalParams& params,
                 const x3::phys::Vec3& pos, float yaw,
                 float width, float height, float ceilingY, uint32_t roomId,
                 const std::vector<std::string>& bootLines = {});

    // Append a kiosk WITHOUT building geometry (headless test only). Used by the
    // self-test to populate the registry + exercise the state machine without
    // creating GPU resources. The terminal pointer is still valid; build() is
    // never called so geometry queries return defaults.
    uint32_t addHeadless(const std::string& id, const std::string& label,
                         const std::string& code, TerminalCommand cmd,
                         const TerminalParams& params,
                         const x3::phys::Vec3& pos, float yaw,
                         uint32_t roomId);

    // Register an EXTERNAL HoloTerminal already built by another system (e.g.
    // SecretRoom's cell terminal). The system records the metadata + a non-owning
    // pointer so the kiosk participates in nearest-in-reach + the unified input
    // path + the command dispatch, without taking ownership. `pos`/`yaw`/`label`
    // should mirror the external's build pose. The external's lifetime must
    // outlive the system.
    uint32_t addExternal(HoloTerminal& external,
                         const std::string& id, const std::string& label,
                         const std::string& code, TerminalCommand cmd,
                         const TerminalParams& params,
                         const x3::phys::Vec3& pos, float yaw, uint32_t roomId);

    // Count / lookup.
    uint32_t count() const { return (uint32_t)m_terminals.size(); }
    const TerminalInstance& at(uint32_t i) const { return m_terminals[i]; }
    TerminalInstance&       at(uint32_t i)       { return m_terminals[i]; }
    int findById(const std::string& id) const;   // -1 if missing

    // Find the nearest kiosk within `kKioskReach` of `eye` (XZ). Returns -1 if
    // none qualifies. Pure query — no state change.
    int nearestInReach(const x3::phys::Vec3& eye) const;

    // Activate the kiosk at `index` for input. Sets the underlying terminal
    // active so typed chars accumulate. Idempotent.
    void enter(uint32_t index);

    // Deactivate the currently-active kiosk (Esc / E again). Clears the input
    // line. Idempotent.
    void leave();

    // The index of the currently-active kiosk, or -1 if none.
    int active() const { return m_active; }
    bool isUsing() const { return m_active >= 0; }

    // Push a typed char into the active kiosk's input line. No-op if no kiosk is
    // active. Lowercase a-z is upper-cased to match the on-glass font; digits +
    // a few punctuation chars pass through. Anything else is dropped.
    void pushChar(char c);
    void backspace();

    // Submit the active kiosk's input. If the kiosk's `code` is empty, ALWAYS
    // executes (the kiosk acts as a no-code "press to fire" button). Otherwise
    // the typed string must EXACTLY equal `code` to dispatch. Returns true iff
    // the command was dispatched (a real-world effect kicked off).
    bool submit(Scene& scene, DoorSystem& doors,
                x3::rhi::IRenderDevice& device);

    // Headless submit: same logic minus geometry side-effects (used by
    // --test-terminals). `doors` may be a fresh empty DoorSystem; UnlockCell /
    // OpenDoor commands no-op gracefully when doorIdx is out of range.
    bool submitHeadless(uint32_t index, const std::string& input,
                        DoorSystem* doors);

    // Per-frame tick: drives every terminal's cursor blink + texture re-bake +
    // lore-timeout reset + the alarm decay. Safe with no `device` (headless).
    void update(float dt);

    // ---- Effect state queries (host reads to render screen overlays). ----
    const TerminalAlarmState& alarm() const { return m_alarm; }

    // ---- Wiring slots. The host plugs callbacks in to perform side effects
    // owned by other systems (the cell-lock hatch, the level point lights).
    using UnlockCellFn = std::function<bool()>;
    using LightsToggleFn = std::function<void(const x3::phys::Vec3& at, float radius,
                                              uint32_t maxAffect)>;
    using SpawnCrateFn = std::function<void(const x3::phys::Vec3& at)>;
    void setUnlockCellSink(UnlockCellFn fn) { m_unlockCell = std::move(fn); }
    void setLightsToggleSink(LightsToggleFn fn) { m_lightsToggle = std::move(fn); }
    void setSpawnCrateSink(SpawnCrateFn fn) { m_spawnCrate = std::move(fn); }

private:
    bool dispatch(uint32_t index, Scene& scene, DoorSystem& doors,
                  x3::rhi::IRenderDevice& device);
    bool dispatchHeadless(uint32_t index, DoorSystem* doors);
    void seedBootReadout(TerminalInstance& t, const std::vector<std::string>& boot);

    std::vector<TerminalInstance> m_terminals;
    int                           m_active = -1;
    TerminalAlarmState            m_alarm;

    UnlockCellFn      m_unlockCell;
    LightsToggleFn    m_lightsToggle;
    SpawnCrateFn      m_spawnCrate;
    // Counter for SpawnCrate so each invocation gets its own scene entity slot
    // (keeps repeated triggers from clobbering each other on the same kiosk).
    uint32_t          m_crateSerial = 0;
};

// Headless self-test (--test-terminals). Asserts:
//   T0 the registry holds >= 5 instances after build (counts a synthetic 6-instance
//      seed mirroring the in-app B1 layout);
//   T1 the cell-lock terminal unlocks on "1278" (and only on "1278");
//   T2 OpenDoor dispatches when the typed code matches (state observable on the
//      DoorSystem);
//   T3 ToggleLights / TriggerAlarm / ShowLore each fire their effect on submit
//      (alarm timer arms, lore lines replace the readout, lights sink fires);
//   T4 interaction state machine: idle -> using -> typing -> execute -> idle (the
//      active() index changes through each phase, input clears on submit);
//   T5 the system rejects input while idle (no terminal active) — pushChar is a
//      no-op so nothing leaks into a kiosk by mistake.
// Returns true iff all pass.
bool runHoloTerminalSystemSelfTest();

} // namespace x3::game
