#pragma once
// WATCH-DOGS-2 ENVIRONMENTAL HACKING — the "NetHack" hackable-object layer for the
// neon city district. Game/slice code only; engine/ stays pure.
//
// A HackableRegistry holds the world's hackable objects (security cameras, junction
// boxes, ATMs, vehicles, traffic signals, and NPCs), each tagged with a type, a world
// position and its marker entity. It drives the three WD2 beats:
//   1. THE NETHACK HIGHLIGHT — a hold-key toggle (setHighlight) that reveals every
//      nearby hackable with a HoloPanel-style marker through walls. nearby() feeds the
//      host's marker/label draw; lookTarget() picks the one the player is aiming at so
//      the host can raise the "[E] HACK — <effect>" prompt.
//   2. THE HACK — hack(i) runs the object's effect: it fills a HackResult and fires the
//      host-wired sinks. EVERY hack routes through story_ops/alert: it raises HEAT (the
//      AlertSystem, via onHeat) and moves KARMA (TimelineState, via onKarma), plus a
//      per-type effect (lights-out, vehicle pop, credit skim, the NPC scan card).
//   3. THE CONFIRM — onResult hands the HoloPanel the scan card / effect confirmation.
//
// PURITY: the registry itself touches no Scene/physics/render/timeline state — the host
// wires the sinks (HackSinks) to the REAL systems and applies what it reads back. That
// keeps the whole machine headlessly testable (--test-hacking) and reusable by any world.

#include "scene.h"                          // kNoLink

#include "engine/physics/IPhysicsWorld.h"   // x3::phys::Vec3

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace x3::game {

// The hackable-object taxonomy (WD2's scannable street tech).
enum class HackableType : uint32_t {
    Camera = 0,      // see through it / mark hostiles
    JunctionBox,     // kill nearby lights (stealth)
    ATM,             // skim credits (money)
    Vehicle,         // unlock / pop / alarm
    TrafficSignal,   // spoof the signal (distraction)
    Npc,             // the scan card (name/occupation/detail) + skim
    Count
};
constexpr uint32_t kHackableTypeCount = (uint32_t)HackableType::Count;

// Human-readable type name ("CAMERA" / "JUNCTION" / ...). Clamped.
const char* hackableTypeName(HackableType t);
// The prompt verb shown after "[E] HACK — " ("SEE THROUGH" / "KILL LIGHTS" / ...).
const char* hackableEffectVerb(HackableType t);

// One placed hackable object (the world entity is its marker/outline anchor).
struct HackableObject {
    HackableType   type   = HackableType::Camera;
    x3::phys::Vec3 pos{};
    uint32_t       entity = kNoLink;   // marker entity id (host highlights this)
    bool           hacked = false;     // one-shot effects latch here
    // ---- Per-type payload ----
    int         credits = 0;           // ATM / NPC: skimmable amount
    std::string label;                 // NPC scan-card name (or object label)
    std::string occupation;            // NPC scan-card occupation
    std::string detail;                // NPC scan-card one detail
};

// What a hack produced — the host applies the numbers + shows the text.
struct HackResult {
    bool         ok        = false;
    HackableType type      = HackableType::Camera;
    uint32_t     object    = kNoLink;
    int          heat      = 0;        // heat this hack contributes (informational)
    int          karma     = 0;        // karma delta applied
    int          credits   = 0;        // credits skimmed (ATM/NPC)
    std::string  effect;               // one-line confirm ("GRID CUT - LIGHTS OUT")
    // NPC scan card (empty for non-NPC hacks).
    std::string  scanName;
    std::string  scanOccupation;
    std::string  scanDetail;
};

// Deterministic effect model: type (+ per-object payload) -> the numbers/text. Exposed
// so the host and the self-test agree on exactly what a hack does. Pure — no sinks.
HackResult computeHackEffect(const HackableObject& o);

// The host wires these to the REAL systems (kept off the registry so it stays pure).
struct HackSinks {
    // Raise heat at `pos` by ~`heat` (host: AlertSystem::reportTerminalHack + log).
    std::function<void(const x3::phys::Vec3& pos, int heat)> onHeat;
    // Move karma by `delta` (host: TimelineState::adjustKarma).
    std::function<void(int delta)>                           onKarma;
    // Junction box hacked: kill lights near the marker entity (host: emissive off).
    std::function<void(uint32_t entity)>                     onLightsOut;
    // Vehicle hacked: pop/alarm the vehicle at the marker entity.
    std::function<void(uint32_t entity)>                     onVehicle;
    // The result is ready — show the HoloPanel scan card / effect confirm.
    std::function<void(const HackResult&)>                   onResult;
};

// The hackable-object registry + the NetHack highlight + the hack dispatch.
class HackableRegistry {
public:
    // Register an object; returns its stable index.
    uint32_t add(const HackableObject& o);
    uint32_t count() const { return (uint32_t)m_objs.size(); }
    const HackableObject& at(uint32_t i) const { return m_objs[i]; }
    HackableObject&       at(uint32_t i)       { return m_objs[i]; }

    void setSinks(const HackSinks& s) { m_sinks = s; }

    // ---- The NetHack highlight (hold-key toggle) ----
    void setHighlight(bool on) { m_highlight = on; }
    bool highlight() const { return m_highlight; }

    // Indices of objects within `radius` of `pos` (marker/label draw). Appends.
    void nearby(const x3::phys::Vec3& pos, float radius, std::vector<uint32_t>& out) const;

    // The hackable the player is aiming at: within `maxDist` and with the eye->object
    // direction within `maxCosAngle` of `fwd` (a normalized forward). Returns the
    // NEAREST such object, or kNoLink if none. `fwd` need not be unit (normalized here).
    uint32_t lookTarget(const x3::phys::Vec3& eye, const x3::phys::Vec3& fwd,
                        float maxDist, float maxCosAngle) const;

    // Run the hack on object `i`: computes the effect, fires the sinks (heat + karma +
    // per-type + result), latches one-shot state, returns the result. A no-op result
    // (ok=false) if `i` is out of range or the object was already hacked (repeatable
    // types — Camera/TrafficSignal — never latch, so they re-fire).
    HackResult hack(uint32_t i);

    // ---- Census (host HUD / self-test) ----
    uint32_t hackedCount() const;
    uint32_t countType(HackableType t) const;

private:
    std::vector<HackableObject> m_objs;
    HackSinks m_sinks;
    bool      m_highlight = false;
};

// Headless self-test (--test-hacking). Asserts: (X0) a mixed registry populates with
// every type; (X1) the highlight toggles; (X2) nearby()/lookTarget() select correctly;
// (X3) a hack fires its per-type effect + raises HEAT (a real AlertSystem) + moves KARMA
// (a real TimelineState); (X4) per-type dispatch (lights-out/vehicle/skim/scan-card all
// route); (X5) one-shot latch (ATM re-hack skims nothing) while repeatable types re-fire;
// (X6) no entity/marker leaks across a rebuild. Prints "hacking: X/Y passed"; returns
// true iff all pass. No window/Vulkan.
bool runHackingSelfTest();

} // namespace x3::game
