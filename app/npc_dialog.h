#pragma once
// NPC TALK / DIALOG (rescue-companion exchange). Game/slice code only.
//
// The rescued-NPC interaction (the captive girl): the player walks up to a LIVE
// captive, presses E, and a short spoken exchange plays out on the HUD. She goes
// terrified -> relieved -> grateful -> flirty over 3-5 lines; advancing with E.
// On the FINAL line she is RESCUED — flipped to a friendly COMPANION via the
// existing RescueSystem follow AI — and speaks a one-liner as she falls in behind
// the player.
//
// This is an NC17 game so attraction/flirting is fine; the script is TASTEFUL
// (grateful -> flirty -> companion), never explicit.
//
// MP-friendly / headless-testable style (mirrors RescueSystem): this object OWNS
// the dialog state (idle / which line) and is driven PURELY by data fed in from
// the host each frame — the player position, whether a live captive is in talk
// range, and the E rising-edge. It never reads input or touches rendering itself;
// the host reads its public state to draw the prompt + the dialog box, and the
// host supplies the rescue action via a callback fired on completion. That makes
// the whole flow exercisable by --test-npctalk with no window / Vulkan.

#include "scene.h"   // for x3::phys::Vec3 (pulled in transitively, kept explicit)

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace x3::game {

// Talk reach (meters): the player must be within this of a live captive to start
// (and to keep advancing) the exchange. Matches the rescue reach so "[E] Talk"
// appears exactly where a rescue would succeed.
constexpr float kTalkReach = 3.0f;

// One spoken line: the speaker label + the line text. The captive's lines are
// italicized in-fiction (the host can style by speaker if it wants).
struct DialogLine {
    std::string speaker;   // e.g. "ARIA" or "YOU"
    std::string text;      // e.g. "You're... not one of them."
};

// The default rescue-companion exchange (terrified -> relieved -> grateful ->
// flirty -> companion). Personalized lightly per victim name. 5 lines.
inline std::vector<DialogLine> makeRescueDialog(const std::string& who) {
    const std::string her = who.empty() ? "She" : who;
    return {
        { her,   "*flinching* You're... not one of them." },          // terrified
        { "YOU", "Easy. I'm getting you out of here." },              // reassurance
        { her,   "*breathing out* Thank god. I thought I was done." },// relieved -> grateful
        { her,   "Get me out of here and I'm yours." },               // flirty (tasteful)
        { "YOU", "Stay close. Let's move." },                         // -> companion
    };
}

// The one-liner she speaks the moment she becomes a companion (host may surface as
// a floating bark / log). Kept light + warm.
inline std::string companionBark(const std::string& who) {
    (void)who;
    return "Right behind you. Don't let me regret this.";
}

// NpcDialog: a tiny state machine for ONE active exchange at a time.
//   * Idle           — no exchange running. interact() can start one.
//   * line N         — showing line N of the script; interact() advances.
//   * finishing line — the last interact() that advances PAST the final line
//                      fires onComplete() (the host's rescue action) and returns
//                      to Idle.
//
// The host calls interact() ONLY on an E rising-edge while a live captive is in
// range (it passes that range fact + the victim's name + position in). All other
// frames it just reads active()/currentLine()/anchor() to draw.
class NpcDialog {
public:
    // Is an exchange currently running?
    bool active() const { return m_active; }

    // Which line index is showing (valid only while active()).
    uint32_t lineIndex() const { return m_line; }
    uint32_t lineCount() const { return (uint32_t)m_script.size(); }

    // The currently-shown line (valid only while active()). Returns a stable empty
    // line when not active so callers never deref out of range.
    const DialogLine& currentLine() const {
        static const DialogLine kEmpty{};
        if (!m_active || m_line >= m_script.size()) return kEmpty;
        return m_script[m_line];
    }

    // The name of the captive this exchange is with (for the prompt + box header).
    const std::string& partner() const { return m_partner; }

    // World anchor (the captive's head) the host worldToScreen's the box/prompt to.
    x3::phys::Vec3 anchor() const { return m_anchor; }

    // Begin an exchange with a captive. Idempotent if already active with someone.
    void begin(const std::string& who, const x3::phys::Vec3& captivePos) {
        if (m_active) return;
        m_active  = true;
        m_line    = 0;
        m_partner = who;
        m_anchor  = x3::phys::Vec3{ captivePos.x, captivePos.y + 1.7f, captivePos.z };
        m_script  = makeRescueDialog(who);
    }

    // Cancel without rescuing (e.g. the player walked out of range mid-talk). The
    // captive stays a captive; the host can re-start the exchange later (from line 0).
    void cancel() { m_active = false; m_line = 0; }

    // Keep the box anchored to the captive as she (and the camera) move.
    void setAnchor(const x3::phys::Vec3& captivePos) {
        m_anchor = x3::phys::Vec3{ captivePos.x, captivePos.y + 1.7f, captivePos.z };
    }

    // E rising-edge driver. Behavior:
    //   * Not active + captive in range -> START the exchange (show line 0). false.
    //   * Active + more lines remain    -> advance to the next line. false.
    //   * Active + on the LAST line     -> fire onComplete (rescue), end the
    //                                      exchange. Returns TRUE (the host plays the
    //                                      companion bark / SFX).
    //   * Not active + NOT in range     -> no-op. false.
    //
    // `inRange` is the host's "a live captive is within kTalkReach" fact; `who`/
    // `captivePos` describe that captive. `onComplete` performs the actual rescue
    // (the host wires it to RescueSystem::tryRescue) — it returns true iff the
    // rescue took, which gates the completion (so a failed rescue keeps the box up).
    bool interact(bool inRange, const std::string& who, const x3::phys::Vec3& captivePos,
                  const std::function<bool()>& onComplete) {
        if (!m_active) {
            if (!inRange) return false;
            begin(who, captivePos);
            return false;
        }
        // Active: out of range cancels (don't strand the box on screen).
        if (!inRange) { cancel(); return false; }
        setAnchor(captivePos);
        if (m_line + 1 < m_script.size()) {
            ++m_line;
            return false;
        }
        // On the last line: complete -> rescue.
        bool ok = onComplete ? onComplete() : true;
        if (ok) {
            m_active = false;
            m_line   = 0;
            return true;   // rescued this call
        }
        return false;      // rescue refused; keep showing the final line
    }

private:
    bool                    m_active = false;
    uint32_t                m_line   = 0;
    std::string             m_partner;
    x3::phys::Vec3          m_anchor{};
    std::vector<DialogLine> m_script;
};

// Headless self-test (--test-npctalk). Drives an NpcDialog purely with data (no
// window / Vulkan / RescueSystem) and asserts the talk flow: out-of-range E does
// nothing; in-range E starts the exchange; repeated E advances to the end;
// completing the final line fires the rescue callback exactly once. Then a small
// end-to-end check wires it to a real RescueSystem to confirm the partner becomes
// a Companion. Logs PASS/FAIL T#, returns true iff all pass.
bool runNpcTalkSelfTest();

} // namespace x3::game
