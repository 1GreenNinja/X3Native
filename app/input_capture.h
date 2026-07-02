#pragma once
// ===========================================================================
// InputCaptureManager — P0 fix (fix/input-capture-lockup): the SINGLE owner of
// GLFW's gameplay cursor-mode state, and the SINGLE source of truth for "what
// currently holds input capture".
//
// BACKGROUND: Tim's playtest locked up — mouse/camera stopped responding while
// the game kept rendering — the classic "input capture leak" (something
// acquired exclusive input and never released it, or the GLFW cursor mode got
// desynced from the engine's internal capture bookkeeping). The audit found
// SEVEN independent capturers (console, pause/settings menu, the world map,
// the cell HoloTerminal, the door/elevator keypad, NPC chat-tree dialog, and
// rescued-NPC dialog), each tracked by its own hand-rolled bool, hand-added to
// a handful of DIFFERENT per-frame gating expressions (the cursor-mode call,
// the mouse-look-freeze check, the movement gate, the melee/fire gate, the
// Esc-routing chain). Those lists had already drifted out of sync with each
// other (chat-tree dialog gated melee/fire but NOT the mouse-look freeze or
// Esc; rescued-NPC dialog wasn't wired into ANY of them) — exactly the bug
// class that produces a stuck/partial capture. See docs/design or the PR
// description for the full file:line audit.
//
// THIS FILE is the fix: one small registry that owns the actual
// glfwSetInputMode(GLFW_CURSOR, ...) call and answers "is anything captured /
// who / is it still legitimate" for every gating site in app_run.cpp. Every
// capturer acquire()/release()s a named tag instead of touching GLFW or a
// local bool directly, so the desync bug class becomes structurally
// impossible: there is exactly one glfwSetInputMode call site in the whole
// engine for gameplay capture, and it lives inside this class.
// ===========================================================================
#include <functional>
#include <string>

struct GLFWwindow;

namespace x3::game {

class InputCaptureManager {
public:
    // Bind the GLFWwindow this manager drives. Safe to call with nullptr
    // (headless / self-test): all GLFW calls become no-ops, but the ownership
    // bookkeeping (acquire/release/owner/watchdog) still works, so the whole
    // policy is unit-testable without a window (--test-inputcapture).
    void init(GLFWwindow* window) { m_window = window; }

    // Take capture for `tag`. `showCursor` selects GLFW_CURSOR_NORMAL (true,
    // e.g. console/menu/world-map — the player needs to see + click a
    // pointer) vs GLFW_CURSOR_DISABLED (false, e.g. terminal/keypad/dialog —
    // gameplay look is frozen but the OS cursor stays hidden/captured, no
    // pointer needed for a keyboard-only interaction).
    //
    // OVERLAP POLICY (exercised by --test-inputcapture's "overlap" case,
    // documented here because the task requires the policy to be explicit):
    // acquiring while a DIFFERENT tag already holds capture is a FORCED
    // TRANSFER — the incoming acquire always wins. The previous owner is
    // dropped and the handoff logged ("capture handoff: X -> Y", one
    // greppable line). A silent no-op/rejection was rejected as a policy:
    // that is exactly how the old ad-hoc-flag system could wedge — a second
    // capturer's request would just vanish, and the FIRST owner (possibly
    // already stale/orphaned) would keep the game captured with no recourse.
    // A forced transfer + a log line means an overlap is always visible in
    // the log, never silently invisible. (It is also the NORMAL path for
    // stacked surfaces under the host's end-of-frame priority reconcile —
    // e.g. opening the console over the pause menu — hence INFO, not WARN.)
    // (Same-tag re-acquire, e.g. a capturer re-affirming every frame, is a
    // cheap no-op — no log spam, just makes sure the cursor mode matches.)
    bool acquire(const std::string& tag, bool showCursor);

    // Release `tag`'s capture. Only actually releases if `tag` IS the
    // current owner — a stale/late release from a PREVIOUS (already
    // superseded) owner must not be able to clobber a NEWER owner's capture.
    // This is the concrete fix for the "NPC despawns mid-dialog, its stale
    // release call fires after something else already grabbed capture"
    // class of bug. No-op (and not logged) if `tag` doesn't currently own
    // capture.
    void release(const std::string& tag);

    // Force-release whatever is held, regardless of who holds it, and
    // restore the default (uncaptured -> gameplay FPS look) cursor mode.
    // Used by the ESC failsafe (always called once per Esc edge, regardless
    // of which subsystem's own Esc branch fired) and by the watchdog. A
    // no-op (not logged) if nothing is currently held. `reason` is a short
    // human tag for the log line (e.g. "esc-failsafe").
    void forceRelease(const char* reason);

    bool               hasOwner() const { return !m_owner.empty(); }
    const std::string& owner()    const { return m_owner; }
    bool               ownedBy(const std::string& tag) const { return m_owner == tag; }

    // Per-frame watchdog: if there is a current owner, ask the caller-
    // supplied predicate whether that owner is STILL a legitimately active
    // capturer (whatever "valid" means for that tag — e.g. the dialog's NPC
    // still exists and is in range, the terminal is still built()+active(),
    // the cutscene is still playing). If the predicate returns false (or the
    // tag is unrecognized by the predicate), the owner is orphaned: force-
    // release it and log exactly one clear, greppable reclaim line. No-op if
    // there is no owner. Returns true iff it reclaimed this call.
    bool watchdogTick(const std::function<bool(const std::string& tag)>& isStillValid);

    // Test-only: reset to the "nothing captured, cursor-mode unknown" state
    // without logging or touching GLFW, so --test-inputcapture cases start
    // from a clean slate. Not used by the live game.
    void resetForTest();

private:
    void applyCursorMode(bool showCursor);

    GLFWwindow* m_window = nullptr;
    std::string m_owner;              // "" == nobody holds capture
    bool        m_cursorShown = false; // mirrors the LAST glfwSetInputMode issued
    bool        m_cursorInit  = false; // have we issued at least one call yet?
};

// ===========================================================================
// Headless self-test (--test-inputcapture). No window/Vulkan (GLFWwindow* is
// nullptr throughout — the manager's GLFW calls are guarded no-ops, so the
// ownership/policy logic runs exactly as it does live). Asserts:
//   * basic acquire/release per tag (every known tag name)
//   * overlap (forced-transfer policy: second acquire always wins, first
//     owner is released first)
//   * despawn-mid-dialog (a stale release from a superseded owner must NOT
//     clobber the newer owner's capture)
//   * ESC force-release (forceRelease drops any owner + restores "no owner")
//   * watchdog reclaim (an orphaned owner is auto-released + logs the
//     "WATCHDOG reclaimed" line; a still-valid owner is left alone)
// Prints "[inputcapture-test] N passed, M failed"; returns true iff all pass.
// ===========================================================================
bool runInputCaptureSelfTest();

} // namespace x3::game
