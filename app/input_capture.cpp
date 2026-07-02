#include "input_capture.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/core/x3_log.h"

namespace x3::game {

void InputCaptureManager::applyCursorMode(bool showCursor) {
    if (m_cursorInit && m_cursorShown == showCursor) return;   // already correct, no-op
    m_cursorShown = showCursor;
    m_cursorInit  = true;
    if (m_window) {
        glfwSetInputMode(m_window, GLFW_CURSOR,
                          showCursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
    }
}

bool InputCaptureManager::acquire(const std::string& tag, bool showCursor) {
    if (m_owner == tag) {
        // Re-affirm by the current owner: not a transition, just make sure
        // the cursor mode matches (cheap, no log spam).
        applyCursorMode(showCursor);
        return true;
    }
    if (!m_owner.empty()) {
        // FORCED TRANSFER (see header: overlap policy). Always visible in the
        // log, never silent — but INFO, not WARN: with the host's end-of-frame
        // priority reconcile a transfer is also the NORMAL path for stacked
        // surfaces (opening the console over the pause menu hands capture
        // menu -> console and back), not only an anomaly.
        x3::logInfo("[input] capture handoff: " + m_owner + " -> " + tag);
        m_owner = tag;
        applyCursorMode(showCursor);
        return true;
    }
    m_owner = tag;
    applyCursorMode(showCursor);
    x3::logInfo("[input] captured-by " + tag);
    return true;
}

void InputCaptureManager::release(const std::string& tag) {
    if (m_owner != tag) return;   // stale release from a non-owner: ignored, not logged
    m_owner.clear();
    applyCursorMode(false);       // default: no owner -> gameplay FPS look (cursor hidden)
    x3::logInfo("[input] released-by " + tag);
}

void InputCaptureManager::forceRelease(const char* reason) {
    if (m_owner.empty()) return;
    x3::logInfo(std::string("[input] force-released owner=") + m_owner +
                " reason=" + (reason ? reason : "?"));
    m_owner.clear();
    applyCursorMode(false);
}

bool InputCaptureManager::watchdogTick(const std::function<bool(const std::string& tag)>& isStillValid) {
    if (m_owner.empty()) return false;
    const bool valid = isStillValid && isStillValid(m_owner);
    if (valid) return false;
    x3::logError("[input] WATCHDOG reclaimed capture from " + m_owner);
    m_owner.clear();
    applyCursorMode(false);
    return true;
}

void InputCaptureManager::resetForTest() {
    m_owner.clear();
    m_cursorShown = false;
    m_cursorInit  = false;
}

// ===========================================================================
// Headless self-test (--test-inputcapture). No window (m_window stays
// nullptr throughout -> every GLFW call inside applyCursorMode() is a guarded
// no-op), so this exercises exactly the same ownership/policy code the live
// game runs, deterministically and without Vulkan.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[inputcapture-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[inputcapture-test] FAIL ") + name); }
}
}

bool runInputCaptureSelfTest() {
    g_pass = g_fail = 0;
    InputCaptureManager cap;
    cap.init(nullptr);   // headless: no window, GLFW calls are no-ops

    // ---- C0: basic acquire/release per known tag. ----
    static const char* kTags[] = {
        "console", "menu", "worldmap", "terminal", "keypad", "dialog",
        "npc_dialog", "cutscene", "elevator"
    };
    bool allTagsOk = true;
    for (const char* t : kTags) {
        cap.resetForTest();
        const bool acquired = cap.acquire(t, /*showCursor*/ false);
        const bool ownedRight = acquired && cap.hasOwner() && cap.ownedBy(t) && cap.owner() == t;
        cap.release(t);
        const bool releasedRight = !cap.hasOwner() && cap.owner().empty();
        allTagsOk = allTagsOk && ownedRight && releasedRight;
    }
    check(allTagsOk, "C0 basic acquire/release round-trips cleanly for every known tag");

    // ---- C1: overlap -- a second acquire while one is held is a FORCED
    //      TRANSFER (documented policy): the new tag always wins, the old
    //      owner is gone (not left silently "still there"). ----
    cap.resetForTest();
    cap.acquire("terminal", false);
    const bool secondAcquireOk = cap.acquire("dialog", false);
    check(secondAcquireOk && cap.ownedBy("dialog") && !cap.ownedBy("terminal"),
          "C1 overlap: second acquire forcibly transfers ownership (documented policy)");

    // ---- C2: despawn-mid-dialog -- simulate an NPC despawning while a
    //      dialog holds capture: something else (e.g. the player opening the
    //      world map) has ALREADY taken over by the time the dialog system's
    //      own (now-stale) release() call finally runs. The stale release
    //      must be a no-op -- it must NOT release the NEW owner's capture.
    //      This is exactly "release keyed off a handle that's gone stale". --
    cap.resetForTest();
    cap.acquire("dialog", false);        // talking to the NPC
    cap.acquire("worldmap", true);       // NPC despawns; player opens the map before
                                          // the dialog system notices and calls release
    cap.release("dialog");               // the dialog's stale/late release call
    check(cap.hasOwner() && cap.ownedBy("worldmap"),
          "C2 despawn-mid-dialog: a stale release from a superseded owner does not leak/clobber");

    // ---- C3: ESC force-release -- regardless of who holds capture (or
    //      whether anyone does), forceRelease() always leaves "no owner" +
    //      restores the default cursor mode. ----
    cap.resetForTest();
    cap.acquire("keypad", false);
    cap.forceRelease("esc-failsafe");
    const bool c3a = !cap.hasOwner();
    cap.forceRelease("esc-failsafe");   // idempotent: no owner -> still no owner, no crash
    const bool c3b = !cap.hasOwner();
    check(c3a && c3b, "C3 ESC force-release always restores 'no owner', idempotently");

    // ---- C4: watchdog reclaim -- an orphaned owner (predicate says it's no
    //      longer legitimately active, e.g. the terminal's built()/active()
    //      went false without termMode being told) is auto-released by the
    //      per-frame watchdog. A STILL-VALID owner must be left alone. ----
    cap.resetForTest();
    cap.acquire("terminal", false);
    bool reclaimedWhenOrphaned = false;
    {
        InputCaptureManager cap2; cap2.init(nullptr);
        cap2.acquire("terminal", false);
        const bool reclaimed = cap2.watchdogTick([](const std::string& tag) {
            return tag != "terminal";   // "terminal" is orphaned; anything else is fine
        });
        reclaimedWhenOrphaned = reclaimed && !cap2.hasOwner();
    }
    bool leftAloneWhenValid = false;
    {
        InputCaptureManager cap3; cap3.init(nullptr);
        cap3.acquire("dialog", false);
        const bool reclaimed = cap3.watchdogTick([](const std::string&) { return true; });
        leftAloneWhenValid = !reclaimed && cap3.hasOwner() && cap3.ownedBy("dialog");
    }
    bool noopWhenNoOwner = false;
    {
        InputCaptureManager cap4; cap4.init(nullptr);
        const bool reclaimed = cap4.watchdogTick([](const std::string&) { return false; });
        noopWhenNoOwner = !reclaimed && !cap4.hasOwner();
    }
    check(reclaimedWhenOrphaned && leftAloneWhenValid && noopWhenNoOwner,
          "C4 watchdog reclaims an orphaned owner, leaves a valid one alone, no-ops with no owner");

    // ---- C5: same-tag re-acquire is a cheap no-op transition (not a
    //      transfer) -- a capturer re-affirming every frame must not spam a
    //      transfer/log storm or lose ownership of itself. ----
    cap.resetForTest();
    cap.acquire("console", true);
    const bool reAcquireOk = cap.acquire("console", true) && cap.ownedBy("console");
    check(reAcquireOk, "C5 same-tag re-acquire stays the owner (not treated as an overlap)");

    x3::logInfo(std::string("[inputcapture-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
