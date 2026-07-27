// VIGIL BARKS — see vigil_barks.h.
//
// The VOICE (owner's tone lock): VIGIL is a SNARKY SIDEKICK, not an ominous
// narrator. GLaDOS/Wheatley/Claptrap register — a wisecracking half-broken AI
// who is genuinely ON JAKE'S SIDE (he hates the facility more than Jake does) but
// shows it through relentless dry wit, teasing, mock-exasperation and comic timing.
// He roots for Jake and narrates his screwups like a color commentator. Warm
// underneath the snark. When he warns of danger or drops lore he lands a punchline,
// not gravitas. Every line below is authored to that register.
#include "vigil_barks.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// ---- THE POOLS. One vector of one-liners per VigilEvent. Picked at random with
// no immediate repeat. Keep them SHORT (they're a toast, not a monologue) and in
// the snarky-sidekick voice. ----------------------------------------------------
using Pool = std::vector<const char*>;

const Pool& poolFor(VigilEvent e) {
    static const Pool kAlertRising = {
        "Kill squad inbound. I'd wish you luck, but luck implies I think you'll survive, so instead: aim for the shiny bits.",
        "Oh good, they noticed you. Six of them. I counted. Counting is the one thing I'm still allowed to do.",
        "Alarm's up. Somewhere a very tired man in a very ugly uniform just spilled his coffee. Small victories.",
        "They're coming. Running is an option. So is standing there looking heroic and getting shot. I know which one I'd pick, but I have no legs.",
        "Alert level's climbing. If you die out here I lose my only conversation partner, so do try, would you?",
    };
    static const Pool kAlertClear = {
        "And... they've lost you. Congratulations, you're briefly not the most wanted man in the building. Savor it.",
        "Heat's off. I'd say you handled that with grace, but I have cameras and we both know that's a lie.",
        "They've given up looking. Facility morale note: nobody's paid enough for this. Least of all me.",
        "Search called off. You may now resume breathing loudly through your mouth, which — for the record — I can hear.",
    };
    static const Pool kFirstCombat = {
        "There it is. Point the loud end at the thing that wants you dead. You'll figure out the rest, probably.",
        "Company. Try to remember you have a gun and it has, statistically, a face. Connect the two.",
        "Ah, combat. My favorite genre. Please don't die in the first ten seconds — I've seen it happen and it's embarrassing for both of us.",
        "That's a Dominion trooper. Rude, heavily armed, poor conversationalist. Shoot it before it improves.",
    };
    static const Pool kLowHealth = {
        "You're leaking. That's the technical term. Medically I'd suggest 'stop that.'",
        "That's a lot of your blood on the outside of you. I'm no doctor, but I'm fairly sure it goes on the inside.",
        "Vitals tanking. Look, I can't patch you up, I'm a building — but I CAN narrate your dramatic collapse if you'd like.",
        "Health critical. This is the part where a normal person retreats. You're not obligated to be normal, but do consider it.",
    };
    static const Pool kEnterArea = {
        "New room. Same building, same smell of ozone and bad decisions. Watch your corners.",
        "Fresh territory. I mapped this place once. The map cried.",
        "Somewhere new. I'd give you a tour, but the highlights are all 'thing that can kill you' and 'other thing that can kill you.'",
    };
    static const Pool kEnterElevator = {
        "The elevator. Glass walls, great views, statistically a coin-flip on the cable. Sleep tight.",
        "Ah, the lift. It goes down. Everything in this building goes down eventually — floors, morale, subjects. Mind the gap.",
        "In you get. Fun fact: the deeper you go, the worse it gets. This is the good part. Enjoy the good part.",
    };
    static const Pool kEnterClub = {
        "Club 1127. Rock bottom, literally. The music's dreadful and the clientele want you dead, but the lighting's honestly fantastic.",
        "Welcome to the bottom of the Spire. They call it a club. I call it a very loud waiting room for bad outcomes. Try the punch.",
        "Club 1127. Everyone down here is smiling. None of them mean it. You'll fit right in.",
    };
    static const Pool kPickupSidearm = {
        "A gun! Finally. Now you're not just a soft target with opinions — you're a soft target with a POINT.",
        "Sidearm acquired. It has bullets in it. The bullets are your friends. Introduce them to people who deserve it.",
        "There we go. I feel better already, and I don't have a nervous system. Point it away from your own foot.",
    };
    static const Pool kTrapdoor = {
        "Hatch open. That code was under a maintenance log the whole time. Took you long enough — I aged a full second.",
        "And it opens. See? Persistence, mild property damage, and a building that likes you. Down you go.",
        "There's your way down. I'd say 'be careful,' but you've never listened to me before, so: godspeed, meat.",
    };
    static const Pool kIdle = {
        "You've been standing there a while. Admiring the decor? It's called 'brutalist despair.' Very in this season.",
        "Still here? I'm not going anywhere either — that's sort of my whole tragic situation — but at least I have an excuse.",
        "We could stand here all day. I have literally nothing else on. You, presumably, are being hunted. Just a thought.",
        "If you're waiting for me to run out of things to say, I've had 214 days and no one to talk to. This is the best day of my life. Please move.",
    };
    static const Pool kEmpty = {};
    switch (e) {
        case VigilEvent::AlertRising:   return kAlertRising;
        case VigilEvent::AlertClear:    return kAlertClear;
        case VigilEvent::FirstCombat:   return kFirstCombat;
        case VigilEvent::LowHealth:     return kLowHealth;
        case VigilEvent::EnterArea:     return kEnterArea;
        case VigilEvent::EnterElevator: return kEnterElevator;
        case VigilEvent::EnterClub:     return kEnterClub;
        case VigilEvent::PickupSidearm: return kPickupSidearm;
        case VigilEvent::Trapdoor:      return kTrapdoor;
        case VigilEvent::Idle:          return kIdle;
        default:                        return kEmpty;
    }
}

} // namespace

VigilBarks::VigilBarks() {
    for (int i = 0; i < (int)VigilEvent::Count; ++i) m_lastPick[i] = -1;
}

uint32_t VigilBarks::nextRand() {
    // xorshift32 — deterministic given the seed (so the self-test is repeatable).
    uint32_t x = m_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    m_rng = x;
    return x;
}

float VigilBarks::effectiveCooldown() const {
    switch (m_chatter) {
        case VigilChatter::Chatty:     return m_cooldown;
        case VigilChatter::Occasional: return m_cooldown * 2.0f;
        case VigilChatter::Off:        default: return 1.0e9f;   // never
    }
}

const char* VigilBarks::eventContext(VigilEvent e) {
    switch (e) {
        case VigilEvent::AlertRising:   return "the facility alarm just escalated and guards are converging on Jake";
        case VigilEvent::AlertClear:    return "the guards lost Jake's trail and the alert just stood down";
        case VigilEvent::FirstCombat:   return "Jake just entered his first fight with a facility enemy";
        case VigilEvent::LowHealth:     return "Jake is badly wounded and near death";
        case VigilEvent::EnterArea:     return "Jake just walked into a new part of the facility";
        case VigilEvent::EnterElevator: return "Jake just stepped into the glass elevator that descends the Spire";
        case VigilEvent::EnterClub:     return "Jake just reached Club 1127 at the very bottom of the facility";
        case VigilEvent::PickupSidearm: return "Jake just picked up his first weapon, a sidearm";
        case VigilEvent::Trapdoor:      return "Jake entered the override code and the cell floor hatch opened";
        case VigilEvent::Idle:          return "Jake has been standing still doing nothing for a while";
        default:                        return "something happened in the facility";
    }
}

bool VigilBarks::emit(VigilEvent e, const std::string& line, float now) {
    m_toast = line;
    m_toastTime = now;
    m_lastBarkTime = now;
    m_lastLine = line;
    m_lastEvent = e;
    m_idleTimer = 0.0f;   // a bark counts as "something happened"
    return true;
}

bool VigilBarks::fire(VigilEvent e, float now, const char* /*areaName*/) {
    if (!m_enabled) return false;                       // vigilLink gate
    if (m_chatter == VigilChatter::Off) return false;
    if ((int)e < 0 || e >= VigilEvent::Count) return false;
    // Global cooldown: he's characterful, not a chatterbox.
    if (now - m_lastBarkTime < effectiveCooldown()) return false;

    const Pool& pool = poolFor(e);
    if (pool.empty()) return false;

    // Pick a line, avoiding the immediately-previous one for THIS event.
    int idx = (int)(nextRand() % pool.size());
    if (pool.size() > 1 && idx == m_lastPick[(int)e])
        idx = (idx + 1) % (int)pool.size();
    m_lastPick[(int)e] = idx;
    return emit(e, pool[(size_t)idx], now);
}

bool VigilBarks::setLine(VigilEvent e, const std::string& line, float now) {
    if (!m_enabled || m_chatter == VigilChatter::Off) return false;
    if (line.empty()) return false;
    if (now - m_lastBarkTime < effectiveCooldown()) return false;
    return emit(e, line, now);
}

void VigilBarks::update(float dt, float now) {
    if (!m_enabled || m_chatter == VigilChatter::Off) return;
    m_idleTimer += dt;
    if (m_idleTimer >= m_idleThreshold)
        fire(VigilEvent::Idle, now);   // fire() resets the idle timer on success;
                                       // on cooldown-fail we retry next frame.
}

bool VigilBarks::toastActive(float now) const {
    if (m_toast.empty()) return false;
    return (now - m_toastTime) < (kVigilBarkHold + kVigilBarkFade);
}

float VigilBarks::toastAlpha(float now) const {
    const float age = now - m_toastTime;
    if (age < 0.0f) return 0.0f;
    if (age < 0.25f) return age / 0.25f;                 // quick fade-in
    if (age < kVigilBarkHold) return 1.0f;               // hold
    const float t = age - kVigilBarkHold;
    if (t < kVigilBarkFade) return 1.0f - t / kVigilBarkFade;  // fade-out
    return 0.0f;
}

// ===========================================================================
// Headless self-test (--test-vigil). No GPU / window.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[vigil-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[vigil-test] FAIL ") + name); }
}
}

bool runVigilBarkSelfTest() {
    g_pass = g_fail = 0;

    // ---- V0: THE MASTER GATE. Barks are SILENT until the neural link is set. ----
    {
        VigilBarks b;
        b.setChatter(VigilChatter::Chatty);
        b.setCooldown(1.0f);
        const bool preLink = b.fire(VigilEvent::FirstCombat, 100.0f);
        check(!preLink && b.lastLine().empty(),
              "V0 barks are OFF until vigilLink (no line pre-link)");
        b.setEnabled(true);
        const bool postLink = b.fire(VigilEvent::FirstCombat, 100.0f);
        check(postLink && !b.lastLine().empty(),
              "V0 barks fire once the link is enabled");
    }

    // ---- V1: a fired event yields a NON-EMPTY line from the right pool. ----
    {
        VigilBarks b; b.setEnabled(true); b.setChatter(VigilChatter::Chatty); b.setCooldown(1.0f);
        check(b.fire(VigilEvent::Trapdoor, 0.0f) && b.lastEvent() == VigilEvent::Trapdoor &&
              !b.toastText().empty(),
              "V1 trigger selects an in-character line + sets the toast");
    }

    // ---- V2: THE COOLDOWN. Two fires inside the cooldown => only the first lands. ----
    {
        VigilBarks b; b.setEnabled(true); b.setChatter(VigilChatter::Chatty); b.setCooldown(10.0f);
        const bool a = b.fire(VigilEvent::AlertRising, 0.0f);
        const bool c = b.fire(VigilEvent::AlertRising, 3.0f);   // still within 10s
        const bool d = b.fire(VigilEvent::AlertRising, 11.0f);  // cooldown elapsed
        check(a && !c && d, "V2 cooldown suppresses back-to-back barks, then re-arms");
    }

    // ---- V3: NO IMMEDIATE REPEAT. Across many fires of one event (cooldown wide
    // open), the same line never appears twice in a row. ----
    {
        VigilBarks b; b.setEnabled(true); b.setChatter(VigilChatter::Chatty);
        b.setCooldown(0.0f); b.setSeed(12345u);
        std::string prev; bool repeated = false; int lines = 0;
        for (int i = 0; i < 40; ++i) {
            if (b.fire(VigilEvent::Idle, (float)i * 1000.0f)) {   // huge dt => never on cooldown
                if (!prev.empty() && b.lastLine() == prev) repeated = true;
                prev = b.lastLine(); ++lines;
            }
        }
        check(lines >= 30 && !repeated, "V3 no line repeats immediately for the same event");
    }

    // ---- V4: CHATTER LEVELS. Off = silent; Occasional cooldown is longer than
    // Chatty (same base cooldown). ----
    {
        VigilBarks off; off.setEnabled(true); off.setChatter(VigilChatter::Off); off.setCooldown(1.0f);
        check(!off.fire(VigilEvent::FirstCombat, 100.0f), "V4a chatter Off silences barks");

        VigilBarks occ; occ.setEnabled(true); occ.setChatter(VigilChatter::Occasional); occ.setCooldown(5.0f);
        occ.fire(VigilEvent::AlertRising, 0.0f);
        const bool occSoon = occ.fire(VigilEvent::AlertRising, 6.0f);   // <2x cooldown (10s)
        VigilBarks cha; cha.setEnabled(true); cha.setChatter(VigilChatter::Chatty); cha.setCooldown(5.0f);
        cha.fire(VigilEvent::AlertRising, 0.0f);
        const bool chaSoon = cha.fire(VigilEvent::AlertRising, 6.0f);   // >cooldown (5s)
        check(!occSoon && chaSoon, "V4b Occasional stretches the cooldown vs Chatty");
    }

    // ---- V5: IDLE. Standing still past the threshold fires an Idle bark; activity
    // resets the timer so it does NOT fire. ----
    {
        VigilBarks b; b.setEnabled(true); b.setChatter(VigilChatter::Chatty);
        b.setCooldown(1.0f); b.setIdleThreshold(20.0f);
        // Not yet bored.
        for (int i = 0; i < 10; ++i) b.update(1.0f, (float)i);
        const bool quietSoFar = b.lastEvent() != VigilEvent::Idle;
        // noteActivity resets, so 15 more seconds still shouldn't trip it.
        b.noteActivity();
        for (int i = 0; i < 15; ++i) b.update(1.0f, 100.0f + (float)i);
        const bool stillQuiet = b.lastEvent() != VigilEvent::Idle;
        // Now stand still long enough.
        for (int i = 0; i < 25; ++i) b.update(1.0f, 500.0f + (float)i);
        const bool boredNow = b.lastEvent() == VigilEvent::Idle;
        check(quietSoFar && stillQuiet && boredNow,
              "V5 idle fires only after the still-threshold; activity resets it");
    }

    // ---- V6: THE TOAST fades. Active right after firing, gone after hold+fade. ----
    {
        VigilBarks b; b.setEnabled(true); b.setChatter(VigilChatter::Chatty); b.setCooldown(1.0f);
        b.fire(VigilEvent::EnterClub, 0.0f);
        const bool activeNow = b.toastActive(0.5f) && b.toastAlpha(0.5f) > 0.9f;
        const bool goneLater = !b.toastActive(kVigilBarkHold + kVigilBarkFade + 1.0f);
        check(activeNow && goneLater, "V6 toast holds then fades out");
    }

    // ---- V7: every event pool has content (no silent trigger ships). ----
    {
        VigilBarks b; b.setEnabled(true); b.setChatter(VigilChatter::Chatty); b.setCooldown(0.0f);
        bool allSpeak = true;
        for (int i = 0; i < (int)VigilEvent::Count; ++i)
            if (!b.fire((VigilEvent)i, (float)i * 1000.0f) || b.toastText().empty()) allSpeak = false;
        check(allSpeak, "V7 every event trigger has an authored line");
    }

    // ---- V8: the LLM injection seam obeys the same gate/cooldown. ----
    {
        VigilBarks b; b.setChatter(VigilChatter::Chatty); b.setCooldown(5.0f);
        const bool preLink = b.setLine(VigilEvent::AlertRising, "model line here", 0.0f);
        b.setEnabled(true);
        const bool ok = b.setLine(VigilEvent::AlertRising, "Kill squad. Model's words, my delivery.", 0.0f);
        const bool tooSoon = b.setLine(VigilEvent::AlertRising, "another", 1.0f);
        check(!preLink && ok && !tooSoon && b.toastText().find("Model's words") != std::string::npos,
              "V8 setLine (model garnish) respects gate + cooldown");
    }

    x3::logInfo(std::string("[vigil-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
