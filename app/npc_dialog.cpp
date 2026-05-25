// NPC TALK / DIALOG self-test (--test-npctalk). See app/npc_dialog.h.
//
// Clean-room: built from the dialog state machine + RescueSystem + the engine
// interfaces only. No window / Vulkan. Mirrors runRescueSelfTest's structure.
#include "npc_dialog.h"
#include "rescue.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {

int g_tpass = 0, g_tfail = 0;
void tcheck(bool cond, const char* name) {
    if (cond) { ++g_tpass; x3::logInfo(std::string("[npctalk-test] PASS ") + name); }
    else      { ++g_tfail; x3::logError(std::string("[npctalk-test] FAIL ") + name); }
}

using HeadlessDevice = x3::game::HeadlessRenderDevice;

// Planar distance helper (the host's in-range fact uses the same XZ measure).
float horizDist(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

bool runNpcTalkSelfTest() {
    g_tpass = g_tfail = 0;

    // ---- Part A: the dialog STATE MACHINE in isolation (no rescue system). ----
    // A fixed captive at the origin; the host's "inRange" fact is the only input
    // besides the E edge, so we model it exactly: in-range iff within kTalkReach.
    {
        const x3::phys::Vec3 captive{ 0.0f, 0.4f, 0.0f };
        NpcDialog dlg;
        int completedCount = 0;
        auto onComplete = [&]() -> bool { ++completedCount; return true; };

        // T1: OUT-OF-RANGE E does nothing (no exchange starts).
        {
            const x3::phys::Vec3 farPlayer{ 50.0f, 0.4f, 0.0f };
            const bool inRange = horizDist(farPlayer, captive) <= kTalkReach;
            bool rescued = dlg.interact(inRange, "Aria", captive, onComplete);
            tcheck(!rescued && !dlg.active() && completedCount == 0,
                   "T1 out-of-range E starts nothing");
        }

        // T2: IN-RANGE E STARTS the exchange (shows line 0, not yet complete).
        const x3::phys::Vec3 nearPlayer{ 1.0f, 0.4f, 0.0f };
        {
            const bool inRange = horizDist(nearPlayer, captive) <= kTalkReach;
            bool rescued = dlg.interact(inRange, "Aria", captive, onComplete);
            tcheck(!rescued && dlg.active() && dlg.lineIndex() == 0 &&
                   dlg.lineCount() >= 3 && dlg.lineCount() <= 5 &&
                   dlg.partner() == "Aria",
                   "T2 in-range E starts the exchange (line 0, 3-5 lines)");
        }

        // T3: each subsequent E advances exactly one line, up to the last; no
        // completion fires while lines remain.
        {
            const bool inRange = true;   // player stays in range
            bool advancedCleanly = true;
            const uint32_t last = dlg.lineCount() - 1;   // index of the final line
            // Each E up to (and including) reaching the last line must ADVANCE and
            // must NOT complete (lines still remain).
            for (uint32_t expect = 1; expect <= last; ++expect) {
                bool rescued = dlg.interact(inRange, "Aria", captive, onComplete);
                if (rescued || !dlg.active() || dlg.lineIndex() != expect) advancedCleanly = false;
            }
            // Now showing the LAST line; one more E completes (rescues) + ends.
            bool finalRescue = dlg.interact(inRange, "Aria", captive, onComplete);
            if (!finalRescue || dlg.active()) advancedCleanly = false;
            tcheck(advancedCleanly && completedCount == 1,
                   "T3 E advances to the end then completes once");
        }

        // T4: after completion the dialog is idle again; the onComplete fired once.
        tcheck(!dlg.active() && completedCount == 1,
               "T4 dialog idle after completion (callback fired exactly once)");

        // T5: walking OUT of range mid-exchange cancels the box (no stranded UI,
        // no accidental rescue).
        {
            NpcDialog d2;
            int comp2 = 0;
            auto oc2 = [&]() -> bool { ++comp2; return true; };
            d2.interact(true, "Keisha", captive, oc2);          // start
            d2.interact(true, "Keisha", captive, oc2);          // advance to line 1
            tcheck(d2.active() && d2.lineIndex() == 1, "T5a exchange mid-way");
            bool rescued = d2.interact(false, "Keisha", captive, oc2);  // out of range
            tcheck(!rescued && !d2.active() && comp2 == 0,
                   "T5 out-of-range mid-exchange cancels (no rescue)");
        }
    }

    // ---- Part B: END-TO-END with a real RescueSystem — completing the dialog
    // wires onComplete -> RescueSystem::tryRescue, which must flip the captive to a
    // Companion (the existing follow AI). This proves the full talk->companion flow.
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
        physics->init();
        HeadlessDevice device;
        Scene scene;
        RescueSystem rescue;

        const x3::phys::Vec3 wA{  0.0f, 0.4f, 0.0f };
        const x3::phys::Vec3 wB{ 20.0f, 0.4f, 0.0f };
        const x3::phys::Vec3 wC{ 40.0f, 0.4f, 0.0f };
        rescue.build(scene, device, *physics, riggedGlbRoot(), wA, wB, wC, /*timer*/300.0f);
        rescue.activate();   // clocks running (irrelevant on a 300 s timer for this test)

        NpcDialog dlg;
        // The host's per-frame wiring, reproduced: find the nearest LIVE captive,
        // compute the in-range fact, and on the E-edge poke dlg.interact with a
        // rescue callback bound to the rescue system.
        const x3::phys::Vec3 player{ wA.x + 1.0f, 0.4f, wA.z };   // standing by Aria
        auto nearestCaptive = [&](std::string& who, x3::phys::Vec3& pos) -> bool {
            float best = kTalkReach * kTalkReach; bool found = false;
            for (uint32_t i = 0; i < rescue.victimCount(); ++i) {
                const RescueVictim& v = rescue.victim(i);
                if (!v.captive()) continue;
                const float dx = player.x - v.pos().x, dz = player.z - v.pos().z;
                const float d2 = dx * dx + dz * dz;
                if (d2 <= best) { best = d2; who = v.name(); pos = v.pos(); found = true; }
            }
            return found;
        };
        auto doRescue = [&]() -> bool { return rescue.tryRescue(player, kRescueReach); };

        // Drive the FULL exchange with E presses until the rescue lands.
        bool rescuedSignaled = false;
        for (int i = 0; i < 16 && !rescuedSignaled; ++i) {
            std::string who; x3::phys::Vec3 cpos{};
            const bool inRange = nearestCaptive(who, cpos);
            rescuedSignaled = dlg.interact(inRange, who, cpos, doRescue);
        }
        const bool ariaCompanion = rescue.victim(0).companion();
        tcheck(rescuedSignaled && ariaCompanion && rescue.rescuedCount() == 1,
               "T6 end-to-end talk completes -> captive becomes a Companion");

        // T7: with the only in-range captive now a companion, a fresh exchange near
        // her ward starts NOTHING (no live captive in range) — the talk gate is off.
        {
            NpcDialog d3;
            std::string who; x3::phys::Vec3 cpos{};
            const bool inRange = nearestCaptive(who, cpos);   // Aria is a companion now
            bool started = d3.interact(inRange, who, cpos, doRescue) || d3.active();
            tcheck(!inRange && !started, "T7 no live captive in range -> no exchange");
        }

        physics->shutdown();
    }

    x3::logInfo("[npctalk-test] " + std::to_string(g_tpass) + " passed, " +
                std::to_string(g_tfail) + " failed");
    return g_tfail == 0;
}

} // namespace x3::game
