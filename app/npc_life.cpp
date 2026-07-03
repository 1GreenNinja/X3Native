// NPC LIFE implementation — the LIVING CITY layer. See app/npc_life.h for the design.
// Game/slice code only — engine/ stays pure.

#include "npc_life.h"
#include "hackables.h"
#include "mesh_prims.h"
#include "terrain.h"          // worldFreewaySampleArc / worldFreewayHalfWidth / length
#include "headless_device.h"  // self-test device
#include "timeline.h"         // self-test: karma rules
#include "alert.h"            // self-test: robbery alarm heat

#include "engine/core/x3_log.h"
#include "engine/llm/ILlmSystem.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

// ===========================================================================
// Names + labels
// ===========================================================================
const char* archetypeName(Archetype a) {
    switch (a) {
        case Archetype::HotDogVendor:  return "HOT-DOG VENDOR";
        case Archetype::BankRobber:    return "DAY LABORER";      // the scan reads innocuous
        case Archetype::Electrician:   return "GRID ELECTRICIAN";
        case Archetype::Programmer:    return "NETRUNNER";
        case Archetype::Baker:         return "CAKE-MAKER";
        case Archetype::Gardener:      return "MEDIAN GARDENER";
        case Archetype::Courier:       return "FREEWAY COURIER";
        case Archetype::StreetCop:     return "STREET PATROL";
        case Archetype::Preacher:      return "STREET PROPHET";
        case Archetype::OffShiftDrone: return "RATION-LINE WORKER";
        case Archetype::Fixer:         return "BLACK-MARKET FIXER";
        case Archetype::Kid:           return "STREET KID";
        default: return "CIVILIAN";
    }
}

const char* npcActivityName(NpcActivity s) {
    switch (s) {
        case NpcActivity::AtHome:    return "AtHome";
        case NpcActivity::ToWork:    return "ToWork";
        case NpcActivity::AtWork:    return "AtWork";
        case NpcActivity::ToLeisure: return "ToLeisure";
        case NpcActivity::AtLeisure: return "AtLeisure";
        case NpcActivity::ToHome:    return "ToHome";
        default: return "?";
    }
}

const char* robberyPhaseName(RobberyPhase p) {
    switch (p) {
        case RobberyPhase::Idle:    return "Idle";
        case RobberyPhase::Casing:  return "Casing";
        case RobberyPhase::Strike:  return "Strike";
        case RobberyPhase::Alarm:   return "Alarm";
        case RobberyPhase::Flee:    return "Flee";
        case RobberyPhase::Escaped: return "Escaped";
        case RobberyPhase::Caught:  return "Caught";
        default: return "?";
    }
}

// ===========================================================================
// The persona table — the roster's SOUL (D:\GameDev\NPC_LIFE_ROSTER.md). The voice
// register + telling details are LAW; keep each archetype's identity distinct.
// ===========================================================================
namespace {

// Index MUST match the Archetype enum order.
const Persona kPersonas[kArchetypeCount] = {
    // HotDogVendor
    { Archetype::HotDogVendor, "HOT-DOG VENDOR",
      { "Undercuts the syndicate stall two blocks down. They've noticed.",
        "Nineteen years on this corner. Same scorched apron.",
        "Knows every cop's order by heart. Feeds two for free." }, 3,
      { "You look like a two-dog day, friend.",
        "Mustard's free. The advice costs extra.",
        "Careful - they're hot off the roller." }, 3,
      18, 0, { 0.80f, 0.55f, 0.25f }, 1.00f, /*static*/true,  false, false },

    // BankRobber
    { Archetype::BankRobber, "DAY LABORER",
      { "Heart rate's up. Keeps checking the guard rotation.",
        "That coffee went cold an hour ago. He hasn't touched it.",
        "Counting exits, not minutes." }, 3,
      { "...just getting a coffee. That's all this is.",
        "You need something from me?",
        "I'm not looking for any trouble." }, 3,
      5, 0, { 0.34f, 0.34f, 0.40f }, 1.00f, false, false, false },

    // Electrician
    { Archetype::Electrician, "GRID ELECTRICIAN",
      { "Owes his apprentice three weeks' pay. Feels it.",
        "Knows exactly which junction boxes are live.",
        "Third double shift this week. Fingers won't uncurl." }, 3,
      { "Don't touch that panel unless you like the taste of copper.",
        "This grid's held together with spit and swearing.",
        "Mind the cables - half of 'em bite." }, 3,
      22, 0, { 0.75f, 0.62f, 0.20f }, 1.05f, false, false, false },

    // Programmer
    { Archetype::Programmer, "NETRUNNER",
      { "Searches: 'how to tell if you're being watched.' He is.",
        "Hasn't slept in a bed in six days. The deck runs hot.",
        "Three deadlines, one working rig, no exits." }, 3,
      { "You're standing in my light.",
        "I don't talk to people who blink that much.",
        "...what. What do you want." }, 3,
      60, 0, { 0.30f, 0.36f, 0.56f }, 1.00f, /*static*/true, false, false },

    // Baker  (VULNERABLE — hacking her costs karma)
    { Archetype::Baker, "CAKE-MAKER",
      { "Making a wedding cake for a couple who broke up yesterday. Doesn't know yet.",
        "The first batch always burns. She lets it.",
        "Flour in her hair since four this morning." }, 3,
      { "First batch is always the honest one.",
        "Here - this one's a little crooked, take it, it's free.",
        "You look like you skipped breakfast, love." }, 3,
      12, -3, { 0.86f, 0.72f, 0.60f }, 0.97f, false, false, false },

    // Gardener
    { Archetype::Gardener, "MEDIAN GARDENER",
      { "The only one out here who plants things that outlive the lease.",
        "Talks to the planters. They listen better than people.",
        "Saving seeds for a garden the city hasn't approved." }, 3,
      { "City eats everything but the roots.",
        "Give it water and time. Same as anything worth keeping.",
        "Mind the seedlings there." }, 3,
      10, 0, { 0.30f, 0.55f, 0.30f }, 1.02f, false, false, false },

    // Courier  (rides the freeway)
    { Archetype::Courier, "FREEWAY COURIER",
      { "Carrying a package she was paid too much not to open.",
        "Ran three red grids to make this drop on time.",
        "Knows the freeway blindfolded. Has, once." }, 3,
      { "Green means go, chrome. You in or in the way?",
        "Package waits for no one.",
        "Move, or get moved." }, 3,
      20, 0, { 0.82f, 0.24f, 0.30f }, 1.00f, false, /*freeway*/true, false },

    // StreetCop  (patrols + responds to the alarm)
    { Archetype::StreetCop, "STREET PATROL",
      { "Three days from a pension he doesn't believe he'll collect.",
        "Hasn't drawn his weapon in a year. Quietly proud of it.",
        "Knows this block better than his own kitchen." }, 3,
      { "Move along. Nothing here worth your face on a camera.",
        "Keep it civil and we both go home tonight.",
        "Eyes forward, hands where I can see 'em." }, 3,
      15, 0, { 0.20f, 0.30f, 0.56f }, 1.05f, false, false, /*patrol*/true },

    // Preacher  (static ranter — a Level 4.5 canon wink)
    { Archetype::Preacher, "STREET PROPHET",
      { "Knows the tower has more floors than it admits. Nobody listens.",
        "Slept under the overpass and dreamed the real number.",
        "Counts the window-bands every night. They don't add up." }, 3,
      { "They count the floors WRONG, you hear me?!",
        "The building LIES and you all just WALK past it!",
        "Repent the arithmetic! REPENT it!" }, 3,
      0, 0, { 0.60f, 0.55f, 0.45f }, 1.03f, /*static*/true, false, false },

    // OffShiftDrone  (VULNERABLE — hacking the tired mass costs karma)
    { Archetype::OffShiftDrone, "RATION-LINE WORKER",
      { "Been awake twenty-six hours. Still smiles at the vendor.",
        "Sends every credit home to a name he won't say.",
        "Twelfth straight shift. Counting to just one more." }, 3,
      { "...long shift. You have one of those?",
        "Just want to get home.",
        "Line was three hours today. Three." }, 3,
      8, -4, { 0.45f, 0.45f, 0.50f }, 1.00f, false, false, false },

    // Fixer
    { Archetype::Fixer, "BLACK-MARKET FIXER",
      { "Sold the same map to four different desperate people this week.",
        "Everything on him is someone else's, once.",
        "Prices go up the moment you look like you need it." }, 3,
      { "Everything's for sale, friend. Even the asking.",
        "You didn't hear it from me. You didn't hear anything.",
        "Credits first. Then we're friends." }, 3,
      40, 0, { 0.40f, 0.30f, 0.46f }, 1.00f, /*static*/true, false, false },

    // Kid  (DON'T hack — the game watches; big karma hit)
    { Archetype::Kid, "STREET KID",
      { "Lifts credits to buy medicine for someone who won't say who.",
        "Fast hands, faster feet. Faster reasons.",
        "Keeps a folded photo he won't show anyone." }, 3,
      { "Didn't take nothin'. You didn't see nothin'.",
        "You're not a cop. ...are you?",
        "Gimme a credit and I'll disappear." }, 3,
      6, -8, { 0.70f, 0.40f, 0.30f }, 0.70f, false, false, false },
};

} // namespace

const Persona& persona(Archetype a) {
    uint32_t i = (uint32_t)a;
    if (i >= kArchetypeCount) i = (uint32_t)Archetype::OffShiftDrone;
    return kPersonas[i];
}

// ===========================================================================
// Meshes (shared, procedural — the crowd/ecology stance)
// ===========================================================================
namespace {

void appendBox(x3::prims::PrimMesh& m, float hx, float hy, float hz,
               float cx, float cy, float cz) {
    x3::prims::PrimMesh b = x3::prims::makeBox(hx, hy, hz, cx, cy, cz);
    const uint32_t base = (uint32_t)m.verts.size();
    m.verts.insert(m.verts.end(), b.verts.begin(), b.verts.end());
    for (uint32_t i : b.index) m.index.push_back(base + i);
}

x3::prims::PrimMesh makeHumanoid() {
    x3::prims::PrimMesh m;
    appendBox(m, 0.21f, 0.33f, 0.12f,  0.00f, 1.04f, 0.00f);   // torso
    appendBox(m, 0.11f, 0.12f, 0.11f,  0.00f, 1.55f, 0.00f);   // head
    appendBox(m, 0.085f,0.39f, 0.085f,-0.115f,0.39f, 0.00f);   // legs
    appendBox(m, 0.085f,0.39f, 0.085f, 0.115f,0.39f, 0.00f);
    appendBox(m, 0.065f,0.30f, 0.065f,-0.285f,1.02f, 0.00f);   // arms
    appendBox(m, 0.065f,0.30f, 0.065f, 0.285f,1.02f, 0.00f);
    return m;
}

x3::prims::PrimMesh makeCart() {
    x3::prims::PrimMesh m;
    appendBox(m, 0.75f, 0.45f, 0.45f, 0.0f, 0.55f, 0.0f);      // cart body
    appendBox(m, 0.10f, 0.55f, 0.10f,-0.6f, 1.25f, 0.35f);     // umbrella pole
    appendBox(m, 0.85f, 0.05f, 0.55f,-0.6f, 1.80f, 0.35f);     // canopy
    appendBox(m, 0.18f, 0.18f, 0.05f, 0.55f, 0.55f, 0.46f);    // little sign
    return m;
}

x3::prims::PrimMesh makeBike() {
    x3::prims::PrimMesh m;
    appendBox(m, 0.85f, 0.14f, 0.16f, 0.0f, 0.55f, 0.0f);      // frame
    appendBox(m, 0.24f, 0.24f, 0.06f, 0.70f, 0.30f, 0.0f);     // front wheel
    appendBox(m, 0.24f, 0.24f, 0.06f,-0.70f, 0.30f, 0.0f);     // rear wheel
    appendBox(m, 0.14f, 0.28f, 0.18f,-0.10f, 0.95f, 0.0f);     // rider torso
    appendBox(m, 0.09f, 0.09f, 0.09f,-0.10f, 1.28f, 0.0f);     // rider head
    return m;
}

x3::prims::PrimMesh makeCar() {
    x3::prims::PrimMesh m;
    appendBox(m, 1.9f, 0.35f, 0.85f, 0.0f, 0.45f, 0.0f);       // body
    appendBox(m, 1.0f, 0.30f, 0.75f,-0.1f, 1.05f, 0.0f);       // cabin
    return m;
}

x3::prims::PrimMesh makeMarker() {
    // A small floating lozenge above the head (the scan holo marker).
    return x3::prims::makeBox(0.12f, 0.20f, 0.03f, 0.0f, 0.0f, 0.0f);
}

// First/last name pools (procedural scan-card names).
const char* kFirst[] = { "MARA","DEX","LIN","RAV","JUNO","KAI","VESNA","OMAR","TESS","BRIX",
                         "SELA","NIKO","ADA","POKA","YUSEF","REN","MILA","CID","ONYX","LENA" };
const char* kLast[]  = { "VOSS","OKORO","CHEZ","KOLE","AMARI","STRAND","BELL","QURESHI","NOX",
                         " VALE","DRAKE","ILES","MOSS","REYES","KANE","SOL","VIK","HARO" };
constexpr uint32_t kFirstN = (uint32_t)(sizeof(kFirst)/sizeof(kFirst[0]));
constexpr uint32_t kLastN  = (uint32_t)(sizeof(kLast)/sizeof(kLast[0]));

inline float dist2XZ(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z; return dx*dx + dz*dz;
}

// The spawn plan: how many of each archetype line the drag. (Freeway couriers are ADDED
// on top via cfg.freewayMovers.)
struct SpawnRow { Archetype arch; int count; };
const SpawnRow kSpawnPlan[] = {
    { Archetype::HotDogVendor, 1 }, { Archetype::BankRobber, 1 },
    { Archetype::Electrician,  3 }, { Archetype::Programmer, 2 },
    { Archetype::Baker,        1 }, { Archetype::Gardener,   2 },
    { Archetype::Courier,      2 }, { Archetype::StreetCop,  3 },
    { Archetype::Preacher,     1 }, { Archetype::OffShiftDrone, 4 },
    { Archetype::Fixer,        1 }, { Archetype::Kid,        2 },
};
constexpr uint32_t kSpawnRows = (uint32_t)(sizeof(kSpawnPlan)/sizeof(kSpawnPlan[0]));

} // namespace

// ===========================================================================
// RNG + names
// ===========================================================================
uint32_t NpcLife::rng(uint32_t& s) const {
    s = s * 1664525u + 1013904223u;
    return s;
}

std::string NpcLife::makeName(uint32_t& s) const {
    std::string f = kFirst[rng(s) % kFirstN];
    std::string l = kLast[rng(s) % kLastN];
    // Trim a stray leading space in the pool (kept aligned for the source column).
    while (!l.empty() && l.front() == ' ') l.erase(l.begin());
    return f + " " + l;
}

// ===========================================================================
// Build
// ===========================================================================
void NpcLife::build(const NpcLifeConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device,
                    HackableRegistry* hax, x3::llm::ILlmSystem* llm) {
    if (m_built) return;
    m_cfg = cfg;
    m_hax = hax;
    m_llm = llm;
    m_t   = cfg.startFraction - std::floor(cfg.startFraction);

    const float cx = cfg.centerX, cz = cfg.centerZ, g = cfg.groundY;
    m_bankPos     = { cx + 40.0f, g + 0.2f, cz + 11.0f };   // the bank front (alarm origin)
    m_freewayExit = { cx + 96.0f, g + 0.2f, cz + 1.0f };    // east edge -> toward the freeway

    // Shared meshes (one create each — the leak canary asserts this).
    { auto pm = makeHumanoid(); m_bodyMesh = device.createMesh(pm.verts.data(),(uint32_t)pm.verts.size(),pm.index.data(),(uint32_t)pm.index.size()); }
    { auto pm = makeCart();     m_cartMesh = device.createMesh(pm.verts.data(),(uint32_t)pm.verts.size(),pm.index.data(),(uint32_t)pm.index.size()); }
    { auto pm = makeBike();     m_bikeMesh = device.createMesh(pm.verts.data(),(uint32_t)pm.verts.size(),pm.index.data(),(uint32_t)pm.index.size()); }
    x3::rhi::MeshHandle carMesh, markerMesh;
    { auto pm = makeCar();      carMesh    = device.createMesh(pm.verts.data(),(uint32_t)pm.verts.size(),pm.index.data(),(uint32_t)pm.index.size()); }
    { auto pm = makeMarker();   markerMesh = device.createMesh(pm.verts.data(),(uint32_t)pm.verts.size(),pm.index.data(),(uint32_t)pm.index.size()); }

    uint32_t s = cfg.seed ? cfg.seed : 1u;

    // ---- Resolve a schedule + initial position per archetype instance. -----
    auto placeSchedule = [&](NpcAgent& a, Archetype arch, int idx) {
        NpcSchedule sc;
        const float jit = (float)((int)(rng(s) % 1000) - 500) * 0.012f;   // -6..+6 m
        switch (arch) {
        case Archetype::HotDogVendor:
            sc = { cx-10+jit, cz+9, cx-10, cz+9, cx-10, cz+9, 0.10f,0.95f, 0.10f,0.95f }; break;
        case Archetype::BankRobber:
            // home == work == the cafe by the bank: he spawns AT his casing post and stays
            // there until the heist timer/trigger fires (then the robbery FSM owns him).
            sc = { cx+30, cz+9, cx+30, cz+9, cx+30, cz+9, 0.05f,0.95f, 0.05f,0.95f }; break;
        case Archetype::Electrician: {
            const float wx = cx - 40.0f + idx*40.0f;
            sc = { cx-75, cz+4+jit, wx, (idx%2? cz-9:cz+9), cx+30, cz-9, 0.30f,0.66f, 0.66f,0.86f }; break; }
        case Archetype::Programmer:
            sc = { cx-38-idx*4, cz+9, cx-38-idx*4, cz+9, cx-38-idx*4, cz+9, 0.10f,0.95f, 0.10f,0.95f }; break;
        case Archetype::Baker:
            sc = { cx-78, cz-4, cx-60, cz+9, cx-60, cz+9, 0.02f,0.42f, 0.02f,0.42f }; break;   // gone by afternoon
        case Archetype::Gardener:
            sc = { cx-72, cz+5+jit, cx - idx*24.0f, cz, cx-20, cz, 0.26f,0.72f, 0.72f,0.88f }; break;
        case Archetype::Courier:
            sc = { cx+80, cz+jit, cx-15+idx*30.0f, cz+9, cx+45, cz-9, 0.28f,0.74f, 0.74f,0.90f }; break;
        case Archetype::StreetCop:
            sc = { cx-58, cz, cx+58, cz, cx, cz, 0.00f,1.00f, 0.00f,1.00f }; break;   // ping-pong all day
        case Archetype::Preacher:
            sc = { cx+25, cz-9, cx+25, cz-9, cx+25, cz-9, 0.00f,1.00f, 0.00f,1.00f }; break;
        case Archetype::OffShiftDrone:
            sc = { (idx%2? cx+82 : cx-82), cz + (idx%2? -4:4), cx+15, cz+9, cx+8, cz-9, 0.30f,0.66f, 0.66f,0.74f }; break;
        case Archetype::Fixer:
            sc = { cx+48, cz-14, cx+48, cz-14, cx+48, cz-14, 0.00f,1.00f, 0.00f,1.00f }; break;   // alley mouth
        case Archetype::Kid:
            sc = { cx-20+idx*40.0f, cz+9, cx+idx*20.0f, cz-9, cx, cz+9, 0.10f,0.90f, 0.10f,0.90f }; break;
        default: sc = { cx, cz, cx, cz, cx, cz, 0.3f,0.66f, 0.66f,0.86f }; break;
        }
        a.sched = sc;
        // Start at home (or the post for static archetypes), a touch of jitter.
        const Persona& p = persona(arch);
        a.pos.x = (p.isStatic ? sc.workX : sc.homeX) + (float)((int)(rng(s)%400)-200)*0.006f;
        a.pos.z = (p.isStatic ? sc.workZ : sc.homeZ) + (float)((int)(rng(s)%400)-200)*0.006f;
        a.pos.y = g;
        a.target = a.pos;
        a.yaw    = (float)(rng(s)%6283)*0.001f;
    };

    auto spawnWalker = [&](Archetype arch, int idx) {
        const Persona& p = persona(arch);
        NpcAgent a;
        a.arch  = arch;
        a.seed  = rng(s);
        a.speed = (arch==Archetype::Kid ? 2.4f
                 : arch==Archetype::Gardener ? 0.7f
                 : arch==Archetype::StreetCop ? 1.35f
                 : arch==Archetype::Courier ? 1.6f : 1.1f);
        a.detailIdx = (int)(rng(s) % p.detailCount);
        a.name = makeName(s);
        placeSchedule(a, arch, idx);

        // Body entity.
        Entity e;
        e.mesh = m_bodyMesh;
        const float jitc = 0.85f + (float)(rng(s)%1000)*0.0003f;
        e.baseColor[0]=p.tint[0]*jitc; e.baseColor[1]=p.tint[1]*jitc; e.baseColor[2]=p.tint[2]*jitc; e.baseColor[3]=1.0f;
        e.emissive[0]=p.tint[0]; e.emissive[1]=p.tint[1]; e.emissive[2]=p.tint[2]; e.emissive[3]=0.12f;  // faint neon rim
        a.entity = scene.add(e);

        // Vendor gets a cart prop beside the body.
        if (arch==Archetype::HotDogVendor) {
            Entity c; c.mesh = m_cartMesh;
            c.baseColor[0]=0.55f; c.baseColor[1]=0.30f; c.baseColor[2]=0.12f; c.baseColor[3]=1.0f;
            c.emissive[0]=1.2f; c.emissive[1]=0.5f; c.emissive[2]=0.1f; c.emissive[3]=0.6f;   // lit shopfront glow
            c.transform[12]=a.pos.x+0.9f; c.transform[13]=g; c.transform[14]=a.pos.z;
            a.propEntity = scene.add(c);
        }

        m_agents.push_back(a);
        const uint32_t ai = (uint32_t)m_agents.size()-1;

        // Register the scan-card hackable (name + occupation + telling detail + karma rule).
        if (hax && cfg.registerScans) {
            const float mk[3] = { 0.25f, 0.9f, 1.0f };
            Entity mkE; mkE.mesh = markerMesh;
            mkE.baseColor[0]=mk[0]; mkE.baseColor[1]=mk[1]; mkE.baseColor[2]=mk[2]; mkE.baseColor[3]=1.0f;
            mkE.emissive[0]=mk[0]; mkE.emissive[1]=mk[1]; mkE.emissive[2]=mk[2]; mkE.emissive[3]=0.5f;
            mkE.transform[12]=a.pos.x; mkE.transform[13]=g+2.05f; mkE.transform[14]=a.pos.z;
            const uint32_t mkEnt = scene.add(mkE);
            HackableObject n;
            n.type = HackableType::Npc;
            n.pos  = { a.pos.x, g+1.6f, a.pos.z };
            n.entity = mkEnt;
            n.label = a.name;
            n.occupation = archetypeName(arch);
            n.detail = p.detail[a.detailIdx];
            n.credits = p.skimCredits;
            n.karmaSet = true; n.karmaValue = p.hackKarma;   // the moral texture
            m_agents[ai].hackReg = hax->add(n);

            // Optional LLM flavor: ask the model for a fresh telling detail per instance,
            // cached (async, non-blocking). The hand-authored line above is the fallback
            // and stays until/unless a generation completes (see update()).
            if (m_llm && m_llm->modelLoaded()) {
                std::string persona = std::string("You are ") + archetypeName(arch) +
                    " in a neon dystopian city. In ONE terse sentence, state a single "
                    "telling, humanizing secret about yourself (a Watch-Dogs profiler line). "
                    "No preamble.";
                x3::llm::ChatId ch = m_llm->startChat(persona);
                if (ch != x3::llm::kInvalidChat && m_llm->submit(ch, "Your telling detail:")) {
                    m_agents[ai].llmChat = ch;
                    m_agents[ai].llmPending = true;
                }
            }
        }
    };

    // ---- Spawn the authored drag mix. ----
    for (uint32_t r = 0; r < kSpawnRows; ++r)
        for (int i = 0; i < kSpawnPlan[r].count; ++i)
            spawnWalker(kSpawnPlan[r].arch, i);

    // ---- Find the robber; start it casing. ----
    for (uint32_t i = 0; i < m_agents.size(); ++i)
        if (m_agents[i].arch == Archetype::BankRobber) { m_robber = (int)i; break; }
    m_robPhase = (m_robber >= 0 && cfg.robberyAtFraction >= 0.0f) ? RobberyPhase::Casing
               : (m_robber >= 0 ? RobberyPhase::Casing : RobberyPhase::Idle);

    // ---- FREEWAY TRAFFIC: couriers on bikes + a few cars riding the ribbon. ----
    const float halfW = std::max(2.0f, worldFreewayHalfWidth());
    for (int i = 0; i < cfg.freewayMovers; ++i) {
        NpcAgent a;
        a.arch = Archetype::Courier;
        a.onFreeway = true;
        a.seed = rng(s);
        a.arcForward = (i % 2 == 0);
        a.arc = (float)i / (float)std::max(1, cfg.freewayMovers);   // staggered along the road
        a.arcSpeed = 0.030f + (float)(rng(s)%1000)*0.00004f;        // 0.030..0.070 /s
        const bool car = (i % 3 == 2);
        // Opposite lanes by direction; cars ride the outer lane.
        a.arcLane = (a.arcForward ? 1.0f : -1.0f) * halfW * (car ? 0.55f : 0.30f);
        a.name = makeName(s);
        a.detailIdx = (int)(rng(s) % persona(Archetype::Courier).detailCount);
        Entity e;
        e.mesh = car ? carMesh : m_bikeMesh;
        if (car) { e.baseColor[0]=0.25f; e.baseColor[1]=0.28f; e.baseColor[2]=0.55f; }
        else     { e.baseColor[0]=0.82f; e.baseColor[1]=0.24f; e.baseColor[2]=0.30f; }
        e.baseColor[3]=1.0f;
        e.emissive[0]=1.0f; e.emissive[1]=0.3f; e.emissive[2]=0.2f; e.emissive[3]=0.5f;   // tail/headlight glow
        a.entity = scene.add(e);
        m_agents.push_back(a);
    }

    m_built = true;
    x3::logInfo("npc_life: built " + std::to_string(m_agents.size()) + " living NPCs (" +
                std::to_string(cfg.freewayMovers) + " on the freeway) + scan-cards");
}

// ===========================================================================
// Per-agent transform
// ===========================================================================
void NpcLife::writeTransform(NpcAgent& a, Scene& scene) const {
    if (a.entity == kNoLink || a.entity >= scene.size()) return;
    const Persona& p = persona(a.arch);
    const float sc = a.onFreeway ? 1.0f : p.scale;
    const float cyf = std::cos(a.yaw), snf = std::sin(a.yaw);
    Entity& e = scene.get(a.entity);
    float* t = e.transform;
    t[0]=cyf*sc; t[1]=0; t[2]=-snf*sc; t[3]=0;
    t[4]=0;      t[5]=sc; t[6]=0;      t[7]=0;
    t[8]=snf*sc; t[9]=0; t[10]=cyf*sc; t[11]=0;
    t[12]=a.pos.x; t[13]=a.pos.y; t[14]=a.pos.z; t[15]=1;
    // Keep the scan marker floating over the head (so the highlight + look-target read).
    if (a.hackReg != kNoLink && m_hax) {
        const uint32_t mk = m_hax->at(a.hackReg).entity;
        if (mk != kNoLink && mk < scene.size()) {
            Entity& me = scene.get(mk);
            me.transform[12]=a.pos.x; me.transform[13]=a.pos.y+2.05f; me.transform[14]=a.pos.z;
        }
        // Keep the hackable's world pos in sync so look-target/nearby track a moving NPC.
        m_hax->at(a.hackReg).pos = { a.pos.x, a.pos.y+1.6f, a.pos.z };
    }
    if (a.propEntity != kNoLink && a.propEntity < scene.size()) {
        Entity& pe = scene.get(a.propEntity);
        pe.transform[12]=a.pos.x+0.9f; pe.transform[14]=a.pos.z;   // cart follows if the vendor drifts
    }
}

// ===========================================================================
// Schedule / activity / routing
// ===========================================================================
NpcActivity NpcLife::desiredActivity(const NpcAgent& a) const {
    const NpcSchedule& s = a.sched;
    auto inWin = [&](float a0, float a1){ return m_t >= a0 && m_t < a1; };
    if (inWin(s.workStart, s.workEnd))       return NpcActivity::AtWork;
    if (inWin(s.leisureStart, s.leisureEnd)) return NpcActivity::AtLeisure;
    return NpcActivity::AtHome;
}

void NpcLife::retarget(NpcAgent& a) {
    // Two-leg Manhattan route so the NPC WALKS THE STREET GRID: first slide along the
    // current sidewalk to the destination's X, then cross to the destination Z.
    a.via = { a.target.x, a.pos.z, 0.0f };
    a.viaActive = (std::fabs(a.target.x - a.pos.x) > 3.0f) &&
                  (std::fabs(a.target.z - a.pos.z) > 3.0f);
}

void NpcLife::updateWalker(NpcAgent& a, float dt) {
    const Persona& p = persona(a.arch);
    const float arriveEps2 = 0.36f;   // 0.6 m
    x3::phys::Vec3 dest = a.target;
    bool fsmOwned = false;

    if ((int)(&a - m_agents.data()) == m_robber && m_robPhase != RobberyPhase::Idle &&
        m_robPhase != RobberyPhase::Casing) {
        // The robbery FSM owns the robber's destination during Strike/Alarm/Flee.
        fsmOwned = true;
        dest = a.target;   // already set by updateRobbery
    } else if (p.patrols) {
        if (a.converge && !a.spoofed) {
            dest = m_bankPos;                       // converge on the alarm
        } else {
            // Ping-pong the beat between the two posts (west <-> east).
            const x3::phys::Vec3 west{ a.sched.homeX, a.pos.y, a.sched.homeZ };
            const x3::phys::Vec3 east{ a.sched.workX, a.pos.y, a.sched.workZ };
            const x3::phys::Vec3 cur = (dist2XZ(a.target, east) < dist2XZ(a.target, west)) ? east : west;
            dest = (dist2XZ(a.pos, cur) < arriveEps2)
                 ? ((dist2XZ(cur, east) < 1.0f) ? west : east) : cur;
        }
    } else if (a.arch == Archetype::Kid) {
        // Darts: on arrival pick a fresh spot along a random sidewalk.
        if (dist2XZ(a.pos, a.target) < arriveEps2) {
            uint32_t rs = a.seed ^ 0x1234u; rng(rs); a.seed = rs;
            dest.x = m_cfg.centerX + (float)((int)(rng(rs)%1600)-800)*0.06f;
            dest.z = m_cfg.centerZ + ((rng(rs)&1) ? 9.0f : -9.0f);
            dest.y = a.pos.y;
        }
    } else if (p.isStatic) {
        dest = { a.sched.workX, a.pos.y, a.sched.workZ };   // hold the post
    } else {
        // Schedule-driven daily loop.
        const NpcActivity want = desiredActivity(a);
        switch (want) {
            case NpcActivity::AtWork:    dest = { a.sched.workX,    a.pos.y, a.sched.workZ };    break;
            case NpcActivity::AtLeisure: dest = { a.sched.leisureX, a.pos.y, a.sched.leisureZ }; break;
            default:                     dest = { a.sched.homeX,    a.pos.y, a.sched.homeZ };    break;
        }
    }

    // Retarget (+ new street route) only when the destination actually changed.
    if (!fsmOwned && dist2XZ(dest, a.target) > 0.25f) { a.target = dest; retarget(a); }
    else if (fsmOwned) { a.viaActive = false; }

    // Seek the current leg.
    x3::phys::Vec3 goal = a.viaActive ? a.via : a.target;
    const float dx = goal.x - a.pos.x, dz = goal.z - a.pos.z;
    const float len = std::sqrt(dx*dx + dz*dz);
    if (len > 0.05f) {
        const float step = std::min(a.speed * dt, len);
        a.pos.x += dx/len * step;
        a.pos.z += dz/len * step;
        float want = std::atan2(-dx, -dz);   // -Z forward convention
        float d = want - a.yaw;
        while (d >  3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        const float mx = 10.0f * dt;
        a.yaw += std::clamp(d, -mx, mx);
    }
    if (a.viaActive && dist2XZ(a.pos, a.via) < arriveEps2) a.viaActive = false;

    // Activity read-out (for the HUD / schedule test).
    if (!p.patrols && a.arch != Archetype::Kid && !fsmOwned) {
        const NpcActivity want = desiredActivity(a);
        const bool arrived = dist2XZ(a.pos, a.target) < arriveEps2;
        if (arrived) a.activity = want;
        else a.activity = (want==NpcActivity::AtWork ? NpcActivity::ToWork
                         : want==NpcActivity::AtLeisure ? NpcActivity::ToLeisure
                         : NpcActivity::ToHome);
    }
}

// ===========================================================================
// Freeway traffic
// ===========================================================================
void NpcLife::updateFreeway(NpcAgent& a, float dt) {
    a.arc += (a.arcForward ? 1.0f : -1.0f) * a.arcSpeed * dt;
    // Despawn/recycle at the bounds (loop the ribbon — no leak).
    if (a.arc > 1.0f)  a.arc -= 1.0f;
    if (a.arc < 0.0f)  a.arc += 1.0f;

    float c[3], tg[2];
    if (worldFreewaySampleArc(a.arc, c, tg)) {
        // across = up x tangent = (t.z, 0, -t.x); offset into the lane.
        const float ax = tg[1], az = -tg[0];
        a.pos.x = c[0] + ax * a.arcLane;
        a.pos.y = c[1] + 0.4f;
        a.pos.z = c[2] + az * a.arcLane;
        float fx = (a.arcForward ? tg[0] : -tg[0]);
        float fz = (a.arcForward ? tg[1] : -tg[1]);
        a.yaw = std::atan2(-fx, -fz);
    } else {
        // No graded ribbon available (headless with no corridor) — park off to the side.
        a.pos = { m_cfg.centerX + 110.0f, m_cfg.groundY, m_cfg.centerZ + 30.0f };
    }
}

// ===========================================================================
// The robbery set-piece
// ===========================================================================
void NpcLife::triggerRobbery() {
    if (m_robber < 0) return;
    if (m_robPhase == RobberyPhase::Strike || m_robPhase == RobberyPhase::Alarm ||
        m_robPhase == RobberyPhase::Flee) return;   // already in motion
    m_robPhase = RobberyPhase::Strike;
    NpcAgent& r = m_agents[m_robber];
    r.target = m_bankPos;
    r.viaActive = false;
    r.speed = 2.2f;
}

void NpcLife::updateRobbery(float dt) {
    if (m_robber < 0) return;
    NpcAgent& r = m_agents[m_robber];

    // Auto-trigger when the day clock crosses the strike time (Casing -> Strike).
    if (m_robPhase == RobberyPhase::Casing && m_cfg.robberyAtFraction >= 0.0f &&
        m_t >= m_cfg.robberyAtFraction && m_t < m_cfg.robberyAtFraction + 0.25f) {
        triggerRobbery();
    }

    switch (m_robPhase) {
    case RobberyPhase::Strike:
        r.target = m_bankPos;
        if (dist2XZ(r.pos, m_bankPos) < 4.0f) {
            m_robPhase = RobberyPhase::Alarm;
            m_robTimer = 1.2f;
            if (!m_alarmFired) {
                m_alarmFired = true;
                if (m_alarm) m_alarm(m_bankPos, 60);   // trip the REAL AlertSystem
            }
            // Every un-spoofed cop drops the beat and converges on the bank.
            for (NpcAgent& c : m_agents)
                if (persona(c.arch).patrols && !c.spoofed) { c.converge = true; c.speed = 2.0f; }
        }
        break;
    case RobberyPhase::Alarm:
        m_robTimer -= dt;
        if (m_robTimer <= 0.0f) {
            m_robPhase = RobberyPhase::Flee;
            r.target = m_freewayExit;
            r.speed = 2.6f;
        }
        break;
    case RobberyPhase::Flee: {
        r.target = m_freewayExit;
        // A converging, un-spoofed cop within reach collars him.
        for (const NpcAgent& c : m_agents) {
            if (!persona(c.arch).patrols || c.spoofed || !c.converge) continue;
            if (dist2XZ(c.pos, r.pos) < 2.6f) { m_robPhase = RobberyPhase::Caught; r.speed = 0.0f; break; }
        }
        if (m_robPhase == RobberyPhase::Flee && dist2XZ(r.pos, m_freewayExit) < 6.0f)
            m_robPhase = RobberyPhase::Escaped;
        break;
    }
    default: break;
    }
}

void NpcLife::notifyHacked(uint32_t hackRegIndex) {
    for (NpcAgent& a : m_agents) {
        if (a.hackReg != hackRegIndex) continue;
        if (persona(a.arch).patrols) {
            // Spoof the cop's radio — he loses the scent and drifts back to his beat.
            a.spoofed  = true;
            a.converge = false;
            a.speed    = 1.35f;
        } else if (a.arch == Archetype::BankRobber) {
            a.planKnown = true;   // pre-warned: the player read his plan
        }
        return;
    }
}

// ===========================================================================
// Update
// ===========================================================================
void NpcLife::setDayFraction(float t) { m_t = t - std::floor(t); }

void NpcLife::update(float dt, Scene& scene) {
    if (!m_built) return;
    m_t += dt / std::max(1.0f, m_cfg.dayLengthSeconds);
    m_t -= std::floor(m_t);

    updateRobbery(dt);

    for (NpcAgent& a : m_agents) {
        if (a.onFreeway) updateFreeway(a, dt);
        else             updateWalker(a, dt);
        writeTransform(a, scene);
    }

    // Drain any completed LLM detail generations (non-blocking; bounded per frame).
    if (m_llm) {
        int budget = 2;
        for (NpcAgent& a : m_agents) {
            if (!a.llmPending || budget <= 0) continue;
            --budget;
            x3::llm::PollResult pr = m_llm->poll(a.llmChat);
            if (pr.done) {
                a.llmPending = false;
                std::string line = pr.newTokens;
                // Trim whitespace/newlines; keep it a single terse sentence.
                while (!line.empty() && (line.back()=='\n'||line.back()=='\r'||line.back()==' ')) line.pop_back();
                if (!pr.failed && line.size() > 6 && a.hackReg != kNoLink) {
                    m_hax->at(a.hackReg).detail = line;   // swap the fallback for the live line
                }
                m_llm->endChat(a.llmChat);
                a.llmChat = 0;
            }
        }
    }
}

// ===========================================================================
// Queries
// ===========================================================================
uint32_t NpcLife::countArchetype(Archetype a) const {
    uint32_t n = 0; for (const NpcAgent& g : m_agents) if (g.arch == a && !g.onFreeway) ++n; return n;
}
uint32_t NpcLife::countActivity(NpcActivity s) const {
    uint32_t n = 0; for (const NpcAgent& g : m_agents) if (!g.onFreeway && g.activity == s) ++n; return n;
}
uint32_t NpcLife::convergingCops() const {
    uint32_t n = 0; for (const NpcAgent& g : m_agents) if (persona(g.arch).patrols && g.converge && !g.spoofed) ++n; return n;
}
uint32_t NpcLife::freewayCount() const {
    uint32_t n = 0; for (const NpcAgent& g : m_agents) if (g.onFreeway) ++n; return n;
}
uint32_t NpcLife::copPositions(x3::phys::Vec3* out, uint32_t max) const {
    uint32_t n = 0;
    for (const NpcAgent& g : m_agents) {
        if (n >= max) break;
        if (persona(g.arch).patrols) out[n++] = g.pos;
    }
    return n;
}

// ===========================================================================
// Headless self-test (--test-npclife)
// ===========================================================================
namespace {

int n_pass = 0, n_fail = 0;
void ncheck(bool cond, const char* name) {
    if (cond) { ++n_pass; x3::logInfo(std::string("[npclife-test] PASS ") + name); }
    else      { ++n_fail; x3::logError(std::string("[npclife-test] FAIL ") + name); }
}

class NpcCountingDevice final : public HeadlessRenderDevice {
public:
    uint32_t meshCreates = 0;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                   const uint32_t* idx, uint32_t ni) override {
        ++meshCreates;
        return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
    }
};

} // namespace

bool runNpcLifeSelfTest() {
    n_pass = n_fail = 0;
    const float dt = 1.0f/60.0f;

    // ---- N1: the archetype mix spawns; ONE shared body mesh (+ cart/bike/car/marker). ----
    {
        NpcCountingDevice device; Scene scene; HackableRegistry hax; NpcLife life;
        NpcLifeConfig cfg; cfg.groundY = 0.0f;
        life.build(cfg, scene, device, &hax);
        bool allArch = true;
        for (uint32_t r = 0; r < kSpawnRows; ++r)
            if (life.countArchetype(kSpawnPlan[r].arch) != (uint32_t)kSpawnPlan[r].count) allArch = false;
        // 5 shared meshes total (body/cart/bike/car/marker) — NOT one-per-agent.
        ncheck(allArch && device.meshCreates == 5 && life.agentCount() > 20 &&
               hax.countType(HackableType::Npc) > 20,
               "N1 archetype mix + shared meshes + scan-cards registered");
    }

    // ---- N2: schedules drive the state machine (work-hours -> AtWork/ToWork; night -> home). ----
    {
        HeadlessRenderDevice device; Scene scene; NpcLife life;
        NpcLifeConfig cfg; cfg.registerScans = false; cfg.robberyAtFraction = -1.0f;
        life.build(cfg, scene, device);
        // Mid-work (noon-ish): drones head to / are at work.
        life.setDayFraction(0.45f);
        for (int f=0; f<60*40; ++f) life.update(dt, scene);
        uint32_t atWork = life.countActivity(NpcActivity::AtWork) + life.countActivity(NpcActivity::ToWork);
        // Deep night: the tired mass has gone home.
        life.setDayFraction(0.95f);
        for (int f=0; f<60*80; ++f) life.update(dt, scene);
        uint32_t atHome = life.countActivity(NpcActivity::AtHome) + life.countActivity(NpcActivity::ToHome);
        ncheck(atWork >= 3 && atHome >= 3, "N2 time-of-day schedule state machine (work->home)");
    }

    // ---- N3: street routing converges walkers onto real destinations (no drifting off). ----
    {
        HeadlessRenderDevice device; Scene scene; NpcLife life;
        NpcLifeConfig cfg; cfg.registerScans = false; cfg.robberyAtFraction = -1.0f;
        life.build(cfg, scene, device);
        life.setDayFraction(0.45f);
        for (int f=0; f<60*60; ++f) life.update(dt, scene);
        // A day-worker should be within a sane radius of the drag (not wandered to infinity).
        bool bounded = true;
        for (uint32_t i=0;i<life.agentCount();++i){
            const NpcAgent& a = life.agent(i);
            if (a.onFreeway) continue;
            const float dx=a.pos.x-cfg.centerX, dz=a.pos.z-cfg.centerZ;
            if (dx*dx+dz*dz > 200.0f*200.0f) bounded=false;
        }
        ncheck(bounded, "N3 walkers stay on the district grid, seeking real posts");
    }

    // ---- N4: the robbery fires the alarm sink + converges cops + the robber flees. ----
    {
        HeadlessRenderDevice device; Scene scene; NpcLife life; AlertSystem alert;
        alert.configure(defaultAlertConfig());
        NpcLifeConfig cfg; cfg.registerScans = false; cfg.robberyAtFraction = -1.0f;   // manual trigger
        life.build(cfg, scene, device);
        bool alarmCalled = false; x3::phys::Vec3 alarmPos{};
        // A bank alarm is an always-noticed broadcast (like a triggered tamper alarm), so it
        // raises heat regardless of which way a patrol happens to be facing.
        life.setAlarmSink([&](const x3::phys::Vec3& p, int heat){ alarmCalled=true; alarmPos=p; alert.reportTerminalHack(p); });
        const float startX = life.agent((uint32_t)life.robberIndex()).pos.x;   // casing at the cafe
        life.triggerRobbery();
        // Drive the whole set-piece; feed the cop positions to the AlertSystem as observers.
        bool sawConverge = false;
        for (int frames = 0; frames < 60*40; ++frames) {
            life.update(dt, scene);
            x3::phys::Vec3 obs[8]; uint32_t nobs = life.copPositions(obs, 8);
            alert.update(dt, nobs ? obs[0] : x3::phys::Vec3{}, obs, nobs, false);
            if (life.convergingCops() >= 1) sawConverge = true;
            if (life.robberyPhase()==RobberyPhase::Escaped) break;
        }
        const float endX = life.agent((uint32_t)life.robberIndex()).pos.x;
        ncheck(alarmCalled, "N4a robbery tripped the alarm sink");
        ncheck(sawConverge, "N4b street cops converged on the bank");
        ncheck(alert.heat() > 0.0f, "N4c the alarm raised REAL AlertSystem heat");
        ncheck(life.robberyPhase()==RobberyPhase::Escaped && endX > startX + 20.0f,
               "N4d the robber fled toward the freeway (+X) and escaped");
    }

    // ---- N4e: hacking a converging cop SPOOFS him (misdirect the heat). ----
    {
        HeadlessRenderDevice device; Scene scene; NpcLife life;
        NpcLifeConfig cfg; cfg.registerScans = true; cfg.robberyAtFraction = -1.0f;
        HackableRegistry hax; life.build(cfg, scene, device, &hax);
        life.triggerRobbery();
        // Let the robber reach the bank so the cops are actually converging (~5-8 s).
        for (int f=0; f<60*10 && life.convergingCops()==0; ++f) life.update(dt, scene);
        const uint32_t before = life.convergingCops();
        // Find a converging cop's hackable index and hack it.
        int hacked = -1;
        for (uint32_t i=0;i<life.agentCount();++i){
            const NpcAgent& a = life.agent(i);
            if (persona(a.arch).patrols && a.converge && a.hackReg!=kNoLink){ life.notifyHacked(a.hackReg); hacked=(int)i; break; }
        }
        for (int f=0; f<60; ++f) life.update(dt, scene);
        ncheck(before >= 1 && hacked >= 0 && life.convergingCops() == before-1,
               "N4e hacking a cop spoofs his radio (one fewer converging)");
    }

    // ---- N5: the KARMA RULES — vulnerable NPCs cost karma; fixer/robber neutral. ----
    {
        HeadlessRenderDevice device; Scene scene; NpcLife life; HackableRegistry hax; TimelineState tl;
        NpcLifeConfig cfg; life.build(cfg, scene, device, &hax);
        HackSinks sinks; sinks.onKarma = [&](int d){ tl.adjustKarma(d); };
        hax.setSinks(sinks);
        auto karmaOf = [&](Archetype want)->int{
            for (uint32_t i=0;i<life.agentCount();++i){
                const NpcAgent& a = life.agent(i);
                if (a.arch==want && a.hackReg!=kNoLink) return computeHackEffect(hax.at(a.hackReg)).karma;
            }
            return 999;
        };
        const int kidK   = karmaOf(Archetype::Kid);
        const int droneK = karmaOf(Archetype::OffShiftDrone);
        const int bakerK = karmaOf(Archetype::Baker);
        const int fixerK = karmaOf(Archetype::Fixer);
        const int robK   = karmaOf(Archetype::BankRobber);
        ncheck(kidK < 0 && droneK < 0 && bakerK < 0 && kidK <= droneK,
               "N5a hacking the vulnerable (kid/drone/baker) costs karma (kid worst)");
        ncheck(fixerK == 0 && robK == 0, "N5b the fixer + robber are karma-neutral");
        // Apply a kid hack through the REAL registry+timeline and confirm karma dropped.
        int before = tl.axes().karma;
        for (uint32_t i=0;i<life.agentCount();++i){
            const NpcAgent& a = life.agent(i);
            if (a.arch==Archetype::Kid && a.hackReg!=kNoLink){ hax.hack(a.hackReg); break; }
        }
        ncheck(tl.axes().karma < before, "N5c a kid hack moved REAL TimelineState karma down");
    }

    // ---- N6: freeway movers ride the ribbon + recycle at the bounds (no leak). ----
    {
        HeadlessRenderDevice device; Scene scene; NpcLife life;
        NpcLifeConfig cfg; cfg.registerScans = false; cfg.freewayMovers = 6;
        life.build(cfg, scene, device);
        const uint32_t fw0 = life.freewayCount();
        // Capture an arc; tick long enough to force a recycle wrap.
        for (int f=0; f<60*120; ++f) life.update(dt, scene);
        bool inBounds = true;
        for (uint32_t i=0;i<life.agentCount();++i){
            const NpcAgent& a = life.agent(i);
            if (a.onFreeway && (a.arc < 0.0f || a.arc > 1.0f)) inBounds = false;
        }
        ncheck(fw0 == 6 && life.freewayCount() == 6 && inBounds,
               "N6 freeway traffic rides + recycles within bounds (constant count)");
    }

    // ---- N7: leak canary — long ticking creates no new meshes/entities. ----
    {
        NpcCountingDevice device; Scene scene; HackableRegistry hax; NpcLife life;
        NpcLifeConfig cfg; life.build(cfg, scene, device, &hax);
        const uint32_t meshesAfterBuild = device.meshCreates;
        const uint32_t entsAfterBuild = scene.size();
        const uint32_t agentsAfterBuild = life.agentCount();
        life.triggerRobbery();
        for (int f=0; f<60*90; ++f) {
            life.update(dt, scene);
            if (f % 900 == 0) life.setDayFraction((float)(f%1000)/1000.0f);
        }
        ncheck(device.meshCreates == meshesAfterBuild && scene.size() == entsAfterBuild &&
               life.agentCount() == agentsAfterBuild,
               "N7 no mesh/entity/agent leak across long ticking");
    }

    x3::logInfo("npclife: " + std::to_string(n_pass) + "/" +
                std::to_string(n_pass + n_fail) + " passed");
    return n_fail == 0;
}

} // namespace x3::game
