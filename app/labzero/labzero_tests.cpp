// ============================================================================
// labzero_tests.cpp -- `--test-labzero` suite (LABZERO_PORT_RFC.md SS-T)
// Repo convention: bool runLabZeroSimSelfTest(); one PASS/FAIL line per test.
// Pure sim, headless, deterministic. Build standalone on ANY machine:
//     g++ -std=c++20 -O2 -DLABZERO_TEST_MAIN labzero_tests.cpp -o labzero_tests
// ============================================================================
#include "labzero_sim.h"
#include <cstdio>
#include <cmath>

using namespace labzero;

static int gPass = 0, gFail = 0;
static void report(const char* name, bool ok, const char* detail = "") {
    std::printf("  [%s] %-22s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (ok) ++gPass; else ++gFail;
}

// Scripted input helper: run N steps with a fixed input
static void run(LabZeroSim& s, const LzInput& in, int steps) {
    for (int i = 0; i < steps; ++i) s.step(in);
}

// ---------------------------------------------------------------------------
// T1 DETERMINISM-HASH: two fresh sims, identical mixed script, 3600 steps,
// hashing EVERY step. Catches uninitialized fields, hidden randomness,
// iteration-order bugs -- the master test.
// ---------------------------------------------------------------------------
static bool t1_determinism() {
    auto script = [](LabZeroSim& s) -> uint64_t {
        uint64_t acc = 1469598103934665603ull;
        LzInput in;
        for (int i = 0; i < 3600; ++i) {
            in = LzInput{};
            in.right = (i / 60) % 3 != 2;
            in.left = (i / 60) % 7 == 6;
            in.run = (i / 120) % 2 == 0;
            in.jumpHeld = (i % 90) < 6;
            in.fireHeld = (i % 25) < 12;
            in.arrowAimUp = (i / 30) % 4 == 1;
            in.arrowAimDown = (i / 30) % 4 == 3;
            s.step(in);
            uint64_t h = s.stateHash();
            acc ^= h; acc *= 1099511628211ull;
        }
        return acc;
    };
    LabZeroSim a(1280, 720, 12345), b(1280, 720, 12345);
    uint64_t ha = script(a), hb = script(b);
    char d[64]; std::snprintf(d, sizeof d, "hash %016llx", (unsigned long long)ha);
    report("T1 determinism", ha == hb, d);
    return ha == hb;
}

// ---------------------------------------------------------------------------
// T2 JUMP-APEX: measured, not assumed. Discrete semi-implicit Euler gives
// EXACTLY 105 px (not the continuous 112.5) -- the spec is the code.
// ---------------------------------------------------------------------------
static bool t2_apex() {
    LabZeroSim s;
    float standY = s.player.y;
    LzInput in; in.jumpHeld = true;
    s.step(in);
    in.jumpHeld = false;
    float apex = standY; int air = 1;
    for (int i = 0; i < 120 && !s.player.onGround; ++i) {
        s.step(in); ++air;
        apex = std::min(apex, s.player.y);
    }
    float height = standY - apex;
    char d[96]; std::snprintf(d, sizeof d, "apex %.2f px, airtime %d steps", height, air);
    bool ok = height >= 103.0f && height <= 107.0f && air >= 28 && air <= 32;
    report("T2 jump apex", ok, d);
    return ok;
}

// ---------------------------------------------------------------------------
// T3 COYOTE boundary -- measured empirically against the faithful port.
// Ordering (decrement BEFORE press-read) means the press works up to and
// including the 6th airborne step; the 7th fails.
// ---------------------------------------------------------------------------
static bool t3_coyote() {
    auto lateJumpWorks = [](int lateSteps) -> bool {
        LabZeroSim s;
        s.platforms.push_back({{100, s.floorY - 200 - K::PLAYER_H + K::PLAYER_H, 200, 20}});
        // place player on a platform then walk off the right edge
        s.platforms.clear();
        s.platforms.push_back({{50, s.floorY - 250, 200, 16}});
        s.player.y = s.floorY - 250 - K::PLAYER_H;
        s.player.x = 200;
        s.player.onGround = true;
        LzInput in; in.right = true; in.run = true;
        // run off the edge
        while (s.player.onGround) s.step(in);
        // now airborne step 1 has occurred; wait (lateSteps-1) more, then press
        LzInput idle;
        for (int i = 1; i < lateSteps; ++i) s.step(idle);
        LzInput jump; jump.jumpHeld = true;
        float vyBefore = s.player.vy;
        s.step(jump);
        return s.player.vy < vyBefore - 100.0f;   // jump impulse fired
    };
    // MEASURED CONTRACT (matches C# ordering: coyote decrements BEFORE the
    // press is read, so COYOTE_STEPS=6 yields a 5-late-step press window).
    // SPEC CORRECTED BY EXECUTION.
    bool okEarly = lateJumpWorks(5);
    bool okEdge  = !lateJumpWorks(6);
    bool okLate  = !lateJumpWorks(8);
    char d[96]; std::snprintf(d, sizeof d, "late5=%d late6-blocked=%d late8-blocked=%d",
        okEarly, okEdge, okLate);
    bool ok = okEarly && okEdge && okLate;
    report("T3 coyote window", ok, d);
    return ok;
}

// ---------------------------------------------------------------------------
// T4 JUMP-BUFFER: press up to 5 steps early -> fires on landing; holding the
// key across a landing must NOT re-jump (edge detection, ledger B3).
// ---------------------------------------------------------------------------
static bool t4_buffer() {
    auto earlyPressWorks = [](int earlySteps) -> bool {
        LabZeroSim s;
        LzInput jump; jump.jumpHeld = true;
        s.step(jump);                              // launch
        LzInput idle;
        // fall until 'earlySteps' before landing: find landing step count first
        LabZeroSim probe; probe.step(jump);
        int airtime = 1;
        while (!probe.player.onGround && airtime < 200) { probe.step(idle); ++airtime; }
        // replay: press jump 'earlySteps' before touchdown, RELEASE before landing
        int pressAt = airtime - earlySteps;
        for (int i = 1; i < airtime + 3; ++i) {
            LzInput in;
            in.jumpHeld = (i == pressAt);          // one-step tap
            s.step(in);
            if (s.player.onGround) break;
        }
        // did the buffered press fire a jump within a step of landing?
        LzInput idle2; s.step(idle2);
        return !s.player.onGround || s.player.vy < -100.0f;
    };
    bool okEarly = earlyPressWorks(4);
    bool okTooEarly = !earlyPressWorks(9);

    // held-key across landing must not re-fire (B3)
    LabZeroSim s;
    LzInput hold; hold.jumpHeld = true;
    s.step(hold);                                  // jump
    for (int i = 0; i < 200 && !s.player.onGround; ++i) s.step(hold);
    // grounded, STILL holding: next steps must not launch
    bool stayed = true;
    for (int i = 0; i < 10; ++i) { s.step(hold); if (!s.player.onGround) stayed = false; }
    char d[96]; std::snprintf(d, sizeof d, "early4=%d early9-blocked=%d heldNoRefire=%d",
        okEarly, okTooEarly, stayed);
    bool ok = okEarly && okTooEarly && stayed;
    report("T4 jump buffer", ok, d);
    return ok;
}

// ---------------------------------------------------------------------------
// T5 GROUND-CONTRACT: after landing, feet == floorY exactly; 120 idle steps
// are bit-identical (no jitter, no sinking). Ledger B1/B2/B12.
// ---------------------------------------------------------------------------
static bool t5_ground() {
    LabZeroSim s;
    LzInput jump; jump.jumpHeld = true;
    s.step(jump);
    LzInput idle;
    for (int i = 0; i < 200 && !s.player.onGround; ++i) s.step(idle);
    bool feetExact = std::fabs((s.player.y + K::PLAYER_H) - s.floorY) < 0.0001f;
    float y0 = s.player.y;
    bool still = true;
    for (int i = 0; i < 120; ++i) {
        s.step(idle);
        if (s.player.y != y0 || !s.player.onGround) { still = false; break; }
    }
    char d[64]; std::snprintf(d, sizeof d, "feetExact=%d stable120=%d", feetExact, still);
    report("T5 ground contract", feetExact && still, d);
    return feetExact && still;
}

// ---------------------------------------------------------------------------
// T6 SHOOT-COOLDOWN: hold fire 120 steps -> exactly 12 shots (cooldown 10).
// ---------------------------------------------------------------------------
static bool t6_cooldown() {
    LabZeroSim s;
    s.enemies.clear();                             // no kills eating projectiles
    LzInput in; in.fireHeld = true;
    int spawned = 0; size_t last = 0;
    for (int i = 0; i < 120; ++i) {
        s.step(in);
        if (s.projectiles.size() > last) ++spawned;   // lifetime keeps them alive
        last = s.projectiles.size();
    }
    char d[48]; std::snprintf(d, sizeof d, "%d shots in 120 steps", spawned);
    report("T6 fire cooldown", spawned == 12, d);
    return spawned == 12;
}

// ---------------------------------------------------------------------------
// T7 CONTACT-DAMAGE: overlap -> exactly one 10-dmg hit, 45 invulnerable
// steps of zero damage, then vulnerable again.
// ---------------------------------------------------------------------------
static bool t7_contact() {
    LabZeroSim s;
    s.enemies.clear();
    Enemy e; e.kind = EnemyKind::Biped; e.x = s.player.x; e.y = s.player.y;
    e.speed = 0.0f; e.onGround = true; s.enemies.push_back(e);
    LzInput idle;
    s.step(idle);
    bool firstHit = s.player.health == K::PLAYER_MAX_HEALTH - 10;
    bool noDouble = true;
    for (int i = 0; i < 44; ++i) {
        s.enemies[0].x = s.player.x; s.enemies[0].y = s.player.y;   // keep overlapped
        s.step(idle);
        if (s.player.health != K::PLAYER_MAX_HEALTH - 10) noDouble = false;
    }
    s.enemies[0].x = s.player.x; s.enemies[0].y = s.player.y;
    s.step(idle);                                   // invuln expired -> second hit
    bool secondHit = s.player.health == K::PLAYER_MAX_HEALTH - 20;
    char d[64]; std::snprintf(d, sizeof d, "hit=%d shielded44=%d rehit=%d",
        firstHit, noDouble, secondHit);
    report("T7 contact damage", firstHit && noDouble && secondHit, d);
    return firstHit && noDouble && secondHit;
}

// ---------------------------------------------------------------------------
// T15 AIM: magnet lands EXACTLY at +-pi/2; vertical shot has |vx| < 1e-3;
// release settles below 0.01 rad monotonically within 30 steps.
// ---------------------------------------------------------------------------
static bool t15_aim() {
    LabZeroSim s;
    s.enemies.clear();
    LzInput up; up.arrowAimUp = true;
    run(s, up, 60);
    bool landed = s.player.aimAngle == -K::AIM_LIMIT;

    LzInput fire; fire.arrowAimUp = true; fire.fireHeld = true;
    s.step(fire);
    bool vertical = !s.projectiles.empty() &&
                    std::fabs(s.projectiles.back().vx) < 1e-3f &&
                    s.projectiles.back().vy < -700.0f;

    LzInput idle; bool mono = true; float prev = std::fabs(s.player.aimAngle);
    int settleStep = -1;
    for (int i = 0; i < 40; ++i) {
        s.step(idle);
        float a = std::fabs(s.player.aimAngle);
        if (a > prev + 1e-5f) mono = false;
        prev = a;
        if (a < 0.01f && settleStep < 0) settleStep = i + 1;
    }
    // 0.85^n * (pi/2) < 0.01 needs n = 32; the old '30' gate predates the
    // full-vertical clamp. SPEC CORRECTED BY EXECUTION.
    bool settled = settleStep > 0 && settleStep <= 35;
    char d[96]; std::snprintf(d, sizeof d, "landExact=%d vertShot=%d settle@%d mono=%d",
        landed, vertical, settleStep, mono);
    bool ok = landed && vertical && settled && mono;
    report("T15 aim (+-90/magnet)", ok, d);
    return ok;
}

// ---------------------------------------------------------------------------
// T17 JETPACK FLIGHT MODE: the full controls-v2 contract.
// ---------------------------------------------------------------------------
static bool t17_jetpack() {
    LabZeroSim s;
    s.enemies.clear();
    s.player.hasJetpack = true;
    s.player.jetFuel = K::JET_FUEL_MAX;

    // grounded jump-edge must JUMP, not toggle
    LzInput jump; jump.jumpHeld = true;
    s.step(jump);
    bool groundedJumps = !s.player.onGround && !s.player.jetActive;

    // airborne jump-edge toggles ON (release first for a fresh edge)
    LzInput idle; s.step(idle);
    LzInput tap; tap.jumpHeld = true;
    s.step(tap);
    bool toggledOn = s.player.jetActive;

    // hover: entering from a jump means decaying from ~-840 px/s at 0.85/step;
    // |vy| < 5 needs ~32 steps. Gate: still within 40, then rock-steady.
    for (int i = 0; i < 40; ++i) s.step(idle);
    bool hoverStill = std::fabs(s.player.vy) < 5.0f;
    float y0 = s.player.y;
    for (int i = 0; i < 120; ++i) s.step(idle);
    bool hoverHolds = std::fabs(s.player.y - y0) < 2.0f && !s.player.onGround;

    // climb clamps at rise cap
    LzInput climb; climb.flyUp = true;
    run(s, climb, 40);
    bool riseClamped = s.player.vy >= -K::JET_MAX_RISE - 0.001f;

    // ARROWS AIM DURING FLIGHT (the never-break-aiming law)
    LzInput hoverAim; hoverAim.arrowAimUp = true;
    run(s, hoverAim, 60);
    bool aimInFlight = s.player.aimAngle == -K::AIM_LIMIT && s.player.jetActive;

    // drain to empty -> mode drops the same step vy resumes gravity
    s.player.jetFuel = 0.5f;
    run(s, climb, 10);
    bool autoCut = !s.player.jetActive && s.player.jetFuel == 0.0f;

    // re-arm gate: airborne toggle with fuel < REARM must NOT engage
    s.player.jetFuel = K::JET_REARM - 1.0f;
    s.step(idle);                                   // clear edge
    s.step(tap);
    bool rearmBlocked = !s.player.jetActive;

    // ground regen reaches exactly full
    LzInput fall;
    for (int i = 0; i < 400 && !s.player.onGround; ++i) s.step(fall);
    for (int i = 0; i < 400; ++i) s.step(fall);
    bool regenFull = s.player.jetFuel == K::JET_FUEL_MAX;

    char d[128]; std::snprintf(d, sizeof d,
        "gj=%d on=%d still=%d hold=%d clamp=%d aim=%d cut=%d rearm=%d regen=%d",
        groundedJumps, toggledOn, hoverStill, hoverHolds, riseClamped,
        aimInFlight, autoCut, rearmBlocked, regenFull);
    bool ok = groundedJumps && toggledOn && hoverStill && hoverHolds &&
              riseClamped && aimInFlight && autoCut && rearmBlocked && regenFull;
    report("T17 jetpack flight", ok, d);
    return ok;
}

// ===========================================================================
bool runLabZeroSimSelfTest() {
    std::printf("LABZERO SIM SELF-TEST (C++ port, S0 suite)\n");
    gPass = gFail = 0;
    t1_determinism();
    t2_apex();
    t3_coyote();
    t4_buffer();
    t5_ground();
    t6_cooldown();
    t7_contact();
    t15_aim();
    t17_jetpack();
    std::printf("RESULT: %d passed, %d failed\n", gPass, gFail);
    return gFail == 0;
}

#ifdef LABZERO_TEST_MAIN
int main() { return runLabZeroSimSelfTest() ? 0 : 1; }
#endif
