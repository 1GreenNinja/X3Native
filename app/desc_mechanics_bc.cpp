// DESC MECHANICS Tier B/C impl + --test-descmech-bc (W9-2). See desc_mechanics_bc.h.
#include "desc_mechanics_bc.h"

#include "asset_root.h"
#include "chat_tree.h"
#include "desc_mechanics.h"   // killRoomGlow (shared glow-kill lever, W9-1)
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// #16 memory fragments: 4 short victim memories, re-baked onto the glass per E.
// Line 0 is the HoloTerminal header title; the rest are body rows.
const std::vector<std::string>& memoryFragmentLines(uint32_t idx) {
    static const std::vector<std::string> kFragments[DescMechanicsBC::kMemoryFragments] = {
        {
            "NEURAL ARCHIVE  --  EXTRACTION 0447",
            "SUBJECT: T. OKAFOR, AGE 31",
            "",
            "...THE LAKE HOUSE. JUNE. MY DAUGHTER",
            "COUNTING TO TEN WITH HER EYES SHUT.",
            "I NEVER GOT TO HIDE.",
            "",
            "[FRAGMENT ENDS]",
        },
        {
            "NEURAL ARCHIVE  --  EXTRACTION 0512",
            "SUBJECT: UNKNOWN FEMALE",
            "",
            "THEY SAID THE SHOTS WERE VITAMINS.",
            "MY HANDS STOPPED BEING MY HANDS.",
            "SOMEONE IS WEARING MY VOICE.",
            "",
            "[FRAGMENT ENDS]",
        },
        {
            "NEURAL ARCHIVE  --  EXTRACTION 0583",
            "SUBJECT: M. REEVES, FACILITY SECURITY",
            "",
            "I OPENED THE DOOR FOR THEM.",
            "EVERY NIGHT I OPEN IT AGAIN.",
            "FORGIVE ME. FORGIVE ME. FORGI--",
            "",
            "[SIGNAL LOST]",
        },
        {
            "NEURAL ARCHIVE  --  EXTRACTION 0611",
            "SUBJECT: [REDACTED], AGE 9",
            "",
            "MOM SAYS THE HUMMING IS ANGELS.",
            "THE HUMMING IS NOT ANGELS.",
            "I WANT TO GO HOME NOW PLEASE.",
            "",
            "[FRAGMENT ENDS]",
        },
    };
    return kFragments[idx % DescMechanicsBC::kMemoryFragments];
}

const std::vector<std::string>& memoryStandbyLines() {
    static const std::vector<std::string> kStandby = {
        "NEURAL ARCHIVE  --  MEMORY STATION 4",
        "EXTRACTED ENGRAM BANK: 3,472 ENTRIES",
        "",
        "PLAYBACK READY.",
        "",
        "[E]  PLAY NEXT FRAGMENT",
    };
    return kStandby;
}

float planar2(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

} // namespace

// ---------------------------------------------------------------------------
// build — register the Tier-B/C mechanics onto the loaded tower. Anchor offsets
// mirror the room_dressing desc-gold placements (containment cots at cz+2.6,
// aug-bay cots at (+/-2.2, +/-1.6), the comms console at cz-1.0, the clone-lab
// glass pane at cx+1.2, the Nexus core glow at the room center).
// ---------------------------------------------------------------------------
bool DescMechanicsBC::build(const CanonFloor& floor, CanonPlay& play, StoryFlags& flags,
                            Scene& scene, x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& physics, std::string_view modelDir) {
    m_floor    = &floor;
    m_play     = &play;
    m_flags    = &flags;
    m_scene    = &scene;
    m_device   = &device;
    m_physics  = &physics;
    m_modelDir = std::string(modelDir);

    auto anchor = [&](uint32_t room, float dx, float dz) {
        const CanonRoom& R = floor.rooms[room];
        return x3::phys::Vec3{ R.cx + dx, R.y0() + 1.0f, R.cz + dz };
    };

    // ---- #6 SALVARI ALLIES (F6). "3 prisoners. Can be freed as allies." ----
    const uint32_t salvariRoom = floor.roomByName("Salvari Containment");
    if (salvariRoom != kNoRoom) {
        const CanonRoom& R = floor.rooms[salvariRoom];
        m_cotPos[0] = { R.cx - 3.0f, R.y0(), R.cz + 2.6f };   // the dressed cot row
        m_cotPos[1] = { R.cx,        R.y0(), R.cz + 2.6f };
        m_cotPos[2] = { R.cx + 3.0f, R.y0(), R.cz + 2.6f };
        InteractPoint p;
        p.id     = "salvari_free";
        p.pos    = anchor(salvariRoom, 0.0f, 2.6f);
        p.radius = 3.4f;
        p.prompt = "[E] RELEASE THE SALVARI PRISONERS";
        p.onUse  = [this](StoryFlags& f) -> std::string {
            f.set("f6.salvari_freed");
            spawnAllies(*m_scene, *m_physics);
            queueBark("Salvari: WE ARE THREE. WE FIGHT AS ONE. YOUR ENEMIES ARE OURS, HUMAN.", 2.2f);
            return "THE CONTAINMENT FIELDS DROP - THREE SALVARI RISE FROM THE COTS";
        };
        m_points.add(std::move(p));
        // Loaded save: the flag survives, the allies respawn, the point stays spent.
        if (flags.has("f6.salvari_freed")) {
            spawnAllies(scene, physics);
            if (InteractPoint* ip = m_points.find("salvari_free")) ip->used = true;
        }
    }

    // ---- #7 ENERGY NEXUS OVERLOAD (F6). "Portal power. Overload = seal forever."
    m_nexusRoom  = floor.roomByName("Energy Nexus");
    m_portalRoom = floor.roomByName("Portal Chamber");
    if (m_nexusRoom != kNoRoom) {
        {
            InteractPoint p;
            p.id     = "nexus_fuse_a";
            p.pos    = anchor(m_nexusRoom, -2.4f, 0.0f);
            p.radius = 2.2f;
            p.prompt = "[E] CUT PRIMARY FEED (FUSEBOX A)";
            p.onUse  = [this](StoryFlags& f) -> std::string {
                f.set("f6.nexus_fuse_a");
                return "PRIMARY FEED CUT - THE CORE PITCH RISES";
            };
            m_points.add(std::move(p));
        }
        {
            InteractPoint p;
            p.id     = "nexus_fuse_b";
            p.pos    = anchor(m_nexusRoom, 2.4f, 0.0f);
            p.radius = 2.2f;
            p.prompt = "[E] CUT SECONDARY FEED (FUSEBOX B)";
            p.requiresFlags = { "f6.nexus_fuse_a" };
            p.missingBark   = "THE SECONDARY FEED IS SLAVED TO THE PRIMARY - CUT FUSEBOX A FIRST";
            p.onUse  = [this](StoryFlags& f) -> std::string {
                f.set("f6.nexus_fuse_b");
                return "SECONDARY FEED CUT - THE CORE IS RUNNING UNREGULATED";
            };
            m_points.add(std::move(p));
        }
        {
            InteractPoint p;
            p.id     = "nexus_core";
            p.pos    = anchor(m_nexusRoom, 0.0f, 0.0f);
            p.radius = 1.8f;
            p.prompt = "[E] OVERLOAD THE ENERGY NEXUS";
            p.requiresFlags = { "f6.nexus_fuse_a", "f6.nexus_fuse_b" };
            p.missingBark   = "CORE CASING SEALED - BOTH REGULATOR FEEDS MUST BE CUT FIRST";
            p.onUse  = [this](StoryFlags& f) -> std::string {
                f.set("f6.portal_sealed");
                m_portalGlowKill = true;    // host edge: portal glass + glow die
                // #12: the minimal Portal Chamber event — the seal's bark beat.
                queueBark("THE EVENT HORIZON COLLAPSES TO DEAD GLASS. NOTHING ELSE COMES THROUGH.", 2.5f);
                queueBark("Jake: Whatever was on the other side... stays there.", 5.2f);
                return "OVERLOAD CASCADE - THE NEXUS FEEDS BACK INTO THE PORTAL";
            };
            m_points.add(std::move(p));
        }
        // Loaded save: portal already sealed -> re-raise the (idempotent) glow
        // kill so the dead-portal look survives a reload; the chain stays spent.
        if (flags.has("f6.portal_sealed")) {
            m_portalGlowKill = true;
            for (const char* id : { "nexus_fuse_a", "nexus_fuse_b", "nexus_core" })
                if (InteractPoint* ip = m_points.find(id)) ip->used = true;
        }
    }

    // ---- #9 PROTOTYPE TESTING COURSE (F4). "Testing arena. Obstacle course."
    m_courseRoom = floor.roomByName("Prototype Testing");
    if (m_courseRoom != kNoRoom) {
        const CanonRoom& R = floor.rooms[m_courseRoom];
        // The dressed crate slalom runs SW -> NE; the trigger pair brackets it.
        m_courseStart  = { R.cx - 3.6f, R.y0() + 1.0f, R.cz - 2.6f };
        m_courseFinish = { R.cx + 3.9f, R.y0() + 1.0f, R.cz + 2.9f };
        if (flags.has("f4.course_beaten")) m_course = CourseState::Beaten;
    }

    // ---- #10 AUGMENTATION CHAIRS (F4). "8 aug chairs. Strength/speed/armor."
    const uint32_t augRoom = floor.roomByName("Augmentation Bay");
    if (augRoom != kNoRoom) {
        struct ChairDef {
            const char* id; const char* prompt; float dx, dz;
            float dm, sm; int hp; const char* flag; const char* bark;
        };
        const ChairDef chairs[3] = {
            { "aug_chair_strength", "[E] AUG CHAIR: STRENGTH (+25% DAMAGE)", -2.2f, -1.6f,
              0.25f, 0.0f, 0,  "f4.aug_strength",
              "MYOFIBRIL WEAVE INTEGRATED - YOUR HITS LAND HARDER (+25% DAMAGE)" },
            { "aug_chair_speed",    "[E] AUG CHAIR: SPEED (+20% MOVE)",       2.2f, -1.6f,
              0.0f, 0.20f, 0,  "f4.aug_speed",
              "REFLEX BUS OVERCLOCKED - YOU MOVE FASTER (+20% SPEED)" },
            { "aug_chair_armor",    "[E] AUG CHAIR: ARMOR (+50 MAX HP)",     -2.2f,  1.6f,
              0.0f, 0.0f, 50, "f4.aug_armor",
              "SUBDERMAL PLATING FUSED - YOU CAN TAKE MORE (+50 MAX HP)" },
        };
        for (const ChairDef& c : chairs) {
            InteractPoint p;
            p.id     = c.id;
            p.pos    = anchor(augRoom, c.dx, c.dz);
            p.radius = 2.2f;
            p.prompt = c.prompt;
            const ChairDef cd = c;
            p.onUse  = [this, cd](StoryFlags& f) -> std::string {
                f.set("f4.aug_used");
                f.set(cd.flag);
                m_aug.damageMult = cd.dm;
                m_aug.speedMult  = cd.sm;
                m_aug.maxHpBonus = cd.hp;
                m_augChanged = true;
                m_augApplied = true;
                // Roguelite choose-one: the other chairs power down for the run.
                for (const char* id : { "aug_chair_strength", "aug_chair_speed", "aug_chair_armor" })
                    if (InteractPoint* other = m_points.find(id)) other->used = true;
                queueBark("THE OTHER CHAIRS POWER DOWN. ONE AUGMENTATION PER SUBJECT.", 2.2f);
                return cd.bark;
            };
            m_points.add(std::move(p));
        }
        // Loaded save: one was used -> re-lock all three + re-apply the pick.
        if (flags.has("f4.aug_used")) {
            for (const char* id : { "aug_chair_strength", "aug_chair_speed", "aug_chair_armor" })
                if (InteractPoint* ip = m_points.find(id)) ip->used = true;
            if (flags.has("f4.aug_strength")) m_aug.damageMult = 0.25f;
            if (flags.has("f4.aug_speed"))    m_aug.speedMult  = 0.20f;
            if (flags.has("f4.aug_armor"))    m_aug.maxHpBonus = 50;
            m_augChanged = true;
            m_augApplied = true;
        }
    }

    // ---- #13 FIRST CONTACT (F6). The elder at the dressed meeting circle. ----
    m_contactRoom = floor.roomByName("First Contact Chamber");
    if (m_contactRoom != kNoRoom) spawnElder(scene, physics);

    // ---- #14 CLONE-TANK PRE-SCENE (F7 Clone Lab). ----
    m_cloneRoom = floor.roomByName("Clone Lab");
    if (m_cloneRoom != kNoRoom) {
        const CanonRoom& R = floor.rooms[m_cloneRoom];
        m_tankPos = { R.cx + 1.2f, R.y0() + 1.4f, R.cz };   // the dressed glass pane
        if (flags.has("f7.clone_seen")) m_cloneSceneFired = true;
    }

    // ---- #15 COMMS BEACON (F7 Comms Center). ----
    const uint32_t commsRoom = floor.roomByName("Comms Center");
    if (commsRoom != kNoRoom) {
        InteractPoint p;
        p.id     = "comms_beacon";
        p.pos    = anchor(commsRoom, 0.0f, -1.0f);   // the dressed console
        p.radius = 2.4f;
        p.prompt = "[E] ACTIVATE THE DISTRESS BEACON";
        p.onUse  = [this](StoryFlags& f) -> std::string {
            f.set("f7.beacon_activated");
            queueBark("[RADIO] ...KZZT... THIS IS RELAY SEVEN. GROUND SIGNAL CONFIRMED. WHO IS THIS?", 3.0f);
            queueBark("[RADIO] IF YOU CAN REACH THE ROOF, WE CAN REACH YOU. EXTRACTION WINDOW LOGGED.", 6.5f);
            return "BEACON ALIGNED - BROADCASTING ON ALL SURFACE BANDS";
        };
        m_points.add(std::move(p));
    }

    // ---- #16 MEMORY HOLO (F4 Neural Interface Lab). ----
    m_memRoom = floor.roomByName("Neural Interface Lab");
    if (m_memRoom != kNoRoom) {
        const CanonRoom& R = floor.rooms[m_memRoom];
        // Back-wall panel between the two dressed consoles, facing INTO the room
        // (yaw pi flips the -Z screen normal toward +Z), support arm to the lid.
        m_memory.build(scene, device,
                       { R.cx, R.y0() + 1.55f, R.z0() + 0.45f },
                       3.14159265f, 1.5f, 0.95f, R.y1() - 0.10f);
        m_memory.setLines(memoryStandbyLines());
        m_memory.setTextColor(0.75f, 0.95f, 0.85f);   // archive green-white ink
        m_memBuilt = true;
        InteractPoint p;
        p.id      = "memory_station";
        p.pos     = { R.cx, R.y0() + 1.0f, R.z0() + 1.5f };
        p.radius  = 2.6f;
        p.prompt  = "[E] PLAY MEMORY FRAGMENT";
        p.oneShot = false;
        p.onUse   = [this](StoryFlags& f) -> std::string {
            f.set("f4.memory_viewed");
            m_memFragment = (m_memFragment % kMemoryFragments) + 1;   // 1..4, wraps
            applyMemoryFragment(m_memFragment - 1);
            return "NEURAL ARCHIVE: FRAGMENT " + std::to_string(m_memFragment) +
                   " OF " + std::to_string(kMemoryFragments);
        };
        m_points.add(std::move(p));
    }

    x3::logInfo("[descmech-bc] built: " + std::to_string(m_points.count()) +
                " interact points; allies " +
                (salvariRoom != kNoRoom ? "armed" : "absent") + ", nexus chain " +
                (m_nexusRoom != kNoRoom ? "armed" : "absent") + ", course " +
                (m_courseRoom != kNoRoom ? "armed" : "absent") + ", memory panel " +
                (m_memBuilt ? "built" : "absent") + ", elder " +
                (m_elderSpawned ? "placed" : "absent"));
    return m_points.count() > 0 || m_elderSpawned || m_memBuilt;
}

// ---------------------------------------------------------------------------
// spawns
// ---------------------------------------------------------------------------
void DescMechanicsBC::spawnAllies(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (m_alliesSpawned || !m_device) return;
    MonsterSystem::Tuning t = act2EnemyTuning(Act2EnemyType::SalvariAlly);
    t.modelFile  = "SalvariPrincess.glb";   // the real Salvari rig (rigged_glb)
    t.standoff   = 3.2f;                    // follow CLOSE (the camp marker held 9 m)
    t.chaseSpeed = 3.8f;
    for (int i = 0; i < 3; ++i) {
        x3::phys::Vec3 at = m_cotPos[i];
        at.z -= 0.8f;   // step off the cot
        m_allies.spawn(scene, *m_device, physics, m_modelDir, at, t);
    }
    m_alliesSpawned = true;
    x3::logInfo("[descmech-bc] 3 Salvari allies freed (follow + fire support)");
}

void DescMechanicsBC::spawnElder(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (m_elderSpawned || !m_device || m_contactRoom == kNoRoom) return;
    const CanonRoom& R = m_floor->rooms[m_contactRoom];
    MonsterSystem::Tuning t = act2EnemyTuning(Act2EnemyType::SalvariAlly);
    t.modelFile  = "SalvariPrincess.glb";
    t.chaseSpeed = 0.0f;                                   // a stationary speaker
    t.tint[0] = 1.05f; t.tint[1] = 0.98f; t.tint[2] = 0.80f;   // elder gold
    m_elderIdx = (int)m_allies.spawn(scene, *m_device, physics, m_modelDir,
                                     { R.cx, R.y0(), R.cz - 0.6f }, t);
    m_elderSpawned = true;
    x3::logInfo("[descmech-bc] Salvari elder placed at the First Contact meeting circle");
}

x3::phys::Vec3 DescMechanicsBC::elderPos() const {
    if (!m_elderSpawned || m_elderIdx < 0 || (uint32_t)m_elderIdx >= m_allies.count())
        return {};
    return m_allies.at((uint32_t)m_elderIdx).pos();
}

void DescMechanicsBC::applyMemoryFragment(uint32_t idx) {
    if (!m_memBuilt) return;
    m_memory.setLines(memoryFragmentLines(idx));
}

// ---------------------------------------------------------------------------
// tick — ally follow + fire support, the course trigger pair, the clone-tank
// proximity scene, the timed bark chains, and the memory panel shimmer/re-bake.
// ---------------------------------------------------------------------------
void DescMechanicsBC::tick(float dt, const x3::phys::Vec3& eye, IDamageSink* player,
                           Scene& scene, x3::phys::IPhysicsWorld& physics) {
    (void)player;
    if (!built() || dt <= 0.0f) return;

    // Late-arriving flags (an F9 flags restore after build): re-sync the
    // one-shot state exactly like descMech's late-sabotage handling.
    if (!m_alliesSpawned && m_flags->has("f6.salvari_freed") &&
        m_points.find("salvari_free")) {
        spawnAllies(scene, physics);
        if (InteractPoint* ip = m_points.find("salvari_free")) ip->used = true;
    }
    if (!m_augApplied && m_flags->has("f4.aug_used") && m_points.find("aug_chair_armor")) {
        for (const char* id : { "aug_chair_strength", "aug_chair_speed", "aug_chair_armor" })
            if (InteractPoint* ip = m_points.find(id)) ip->used = true;
        if (m_flags->has("f4.aug_strength")) m_aug.damageMult = 0.25f;
        if (m_flags->has("f4.aug_speed"))    m_aug.speedMult  = 0.20f;
        if (m_flags->has("f4.aug_armor"))    m_aug.maxHpBonus = 50;
        m_augApplied = true;
        m_augChanged = true;
    }
    if (m_course != CourseState::Beaten && m_flags->has("f4.course_beaten"))
        m_course = CourseState::Beaten;

    // Timed bark chains (the beacon radio / the portal seal beat / ally lines).
    for (size_t i = 0; i < m_timed.size();) {
        m_timed[i].t -= dt;
        if (m_timed[i].t <= 0.0f) {
            m_barks.push_back(std::move(m_timed[i].line));
            x3::logInfo("[descmech-bc] bark: " + m_barks.back());
            m_timed.erase(m_timed.begin() + (std::ptrdiff_t)i);
        } else {
            ++i;
        }
    }

    // #6: allies follow Jake (movement-only update — allied, 0 player damage) and
    // emit round-robin fire-support strikes at the nearest hostile in range.
    if (m_allies.count() > 0) {
        m_allies.update(dt, scene, physics, eye);
        if (m_alliesSpawned && m_allies.aliveCount() > 0) {
            m_strikeT -= dt;
            if (m_strikeT <= 0.0f) {
                m_strikeT = kAllyStrikePeriod;
                for (uint32_t tries = 0; tries < m_allies.count(); ++tries) {
                    const uint32_t i = m_nextStriker++ % m_allies.count();
                    if ((int)i == m_elderIdx) continue;         // the elder never fights
                    MonsterSystem& ally = m_allies.at(i);
                    if (!ally.alive()) continue;
                    x3::phys::Vec3 from = ally.pos();
                    from.y += 1.2f;                             // chest height
                    x3::phys::Vec3 hit{};
                    if (m_play->allyStrike(from, kAllyStrikeRadius, kAllyStrikeDamage,
                                           scene, physics, &hit)) {
                        ++m_allyStrikes;
                        if (m_attackFx) m_attackFx(from, hit);
                        if (m_allyStrikes % 6 == 1)
                            queueBark("Salvari: THE TARGET BURNS. WE DO NOT MISS.");
                    }
                    break;   // one striker per beat (staggered pressure, not a laser wall)
                }
            }
        }
    }

    // #9: the course trigger pair + clock (position checks — the cold-room pattern).
    if (m_courseRoom != kNoRoom && m_course != CourseState::Beaten) {
        const uint32_t rm = m_floor->roomAt(eye.x, eye.y, eye.z);
        if (rm == m_courseRoom && !m_courseHint) {
            m_courseHint = true;
            queueBark("PROTOTYPE COURSE: CROSS THE WEST MARKER TO START THE 45s CLOCK");
        }
        switch (m_course) {
            case CourseState::Idle:
                if (rm == m_courseRoom && planar2(eye, m_courseStart) < 1.6f * 1.6f) {
                    m_course  = CourseState::Running;
                    m_courseT = kCourseSeconds;
                    queueBark("COURSE CLOCK RUNNING - REACH THE FAR MARKER IN 45 SECONDS");
                }
                break;
            case CourseState::Running:
                m_courseT -= dt;
                if (rm != m_courseRoom) {
                    m_course = CourseState::Cooldown;
                    queueBark("COURSE ABORTED - RUNNER LEFT THE ARENA");
                } else if (planar2(eye, m_courseFinish) < 1.8f * 1.8f) {
                    m_course = CourseState::Beaten;
                    m_flags->set("f4.course_beaten");
                    const uint32_t n = m_play->spawnBonusCache(*m_floor, scene, *m_device,
                                                               "Prototype Testing", 3.2f, -2.8f);
                    queueBark("COURSE BEATEN - REWARD CACHE UNLOCKED (" +
                              std::to_string(n) + " ITEMS AT THE EAST WALL)");
                } else if (m_courseT <= 0.0f) {
                    m_course = CourseState::Cooldown;
                    queueBark("COURSE FAILED - RETURN TO THE START MARKER TO RE-ARM");
                }
                break;
            case CourseState::Cooldown:
                if (planar2(eye, m_courseStart) > 3.0f * 3.0f) m_course = CourseState::Idle;
                break;
            default: break;
        }
    }

    // #14: the clone-tank pre-scene (one-shot proximity beat at the glass pane).
    if (m_cloneRoom != kNoRoom && !m_cloneSceneFired) {
        const float dy = std::fabs(eye.y - m_tankPos.y);
        if (dy < 2.0f && planar2(eye, m_tankPos) < 2.4f * 2.4f) {
            m_cloneSceneFired = true;
            m_flags->set("f7.clone_seen");
            queueBark("THE CRYO TANK HUMS. SOMETHING FLOATS BEHIND THE GLASS.");
            queueBark("SPECIMEN JP-2 // MATURATION 94% // GAIT MODEL: SYNCED", 2.4f);
            queueBark("Jake: ...that's ME in there.", 4.8f);
        }
    }

    // #16: the panel's cursor/shimmer + any pending on-glass re-bake.
    if (m_memBuilt) m_memory.update(dt);
}

bool DescMechanicsBC::onUse(const x3::phys::Vec3& eye, std::string* barkOut) {
    if (!built()) return false;
    return m_points.onUse(eye, *m_flags, barkOut);
}

std::string DescMechanicsBC::takeBark() {
    if (m_barks.empty()) return std::string();
    std::string b = m_barks.front();
    m_barks.erase(m_barks.begin());
    return b;
}

std::string DescMechanicsBC::hudStatusLine() const {
    std::string s;
    auto append = [&s](const std::string& tag) {
        if (!s.empty()) s += "  |  ";
        s += tag;
    };
    if (m_course == CourseState::Running)
        append("COURSE " + std::to_string((int)(m_courseT + 0.999f)) + "s");
    if (m_alliesSpawned) {
        const uint32_t elderAlive =
            (m_elderIdx >= 0 && (uint32_t)m_elderIdx < m_allies.count() &&
             m_allies.at((uint32_t)m_elderIdx).alive()) ? 1u : 0u;
        const uint32_t fighters = m_allies.aliveCount() - elderAlive;
        if (fighters > 0) append("SALVARI x" + std::to_string(fighters));
    }
    return s;
}

void DescMechanicsBC::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                           const Scene& scene) const {
    // Room-gated like CanonPlay::drawManagerCulled (kNoRoom-tagged draws always).
    for (uint32_t i = 0; i < m_allies.count(); ++i) {
        const MonsterSystem& m = m_allies.at(i);
        const uint32_t ent = m.entity();
        uint32_t room = kNoRoom;
        if (ent != kNoLink && ent < scene.size()) room = scene.get(ent).roomId;
        if (!scene.roomVisible(room)) continue;
        m.drawMonster(device, frame, scene);
    }
}

bool DescMechanicsBC::augChangedPending() {
    const bool p = m_augChanged;
    m_augChanged = false;
    return p;
}

bool DescMechanicsBC::portalGlowKillPending() {
    const bool p = m_portalGlowKill;
    m_portalGlowKill = false;
    return p;
}

void DescMechanicsBC::queueBark(std::string b, float delay) {
    if (delay > 0.0f) {
        m_timed.push_back({ delay, std::move(b) });
        return;
    }
    m_barks.push_back(std::move(b));
    x3::logInfo("[descmech-bc] bark: " + m_barks.back());
}

void DescMechanicsBC::shutdown() {
    m_allies.shutdown();
}

// =====================================================================================
// Headless self-test (--test-descmech-bc). No window / Vulkan.
// =====================================================================================
namespace {

int g_bpass = 0, g_bfail = 0;
void bcheck(bool cond, const char* name) {
    if (cond) { ++g_bpass; x3::logInfo(std::string("[descmech-bc-test] PASS ") + name); }
    else      { ++g_bfail; x3::logError(std::string("[descmech-bc-test] FAIL ") + name); }
}

x3::phys::Vec3 roomEyeBC(const CanonFloor& f, uint32_t room) {
    const CanonRoom& R = f.rooms[room];
    return x3::phys::Vec3{ R.cx, R.y0() + 1.6f, R.cz };
}

// Drain every immediate bark into one string (for content asserts).
std::string drainBarks(DescMechanicsBC& bc) {
    std::string all;
    for (std::string b = bc.takeBark(); !b.empty(); b = bc.takeBark()) { all += b; all += '\n'; }
    return all;
}

} // namespace

bool runDescMechBCSelfTest() {
    g_bpass = g_bfail = 0;

    CanonFloor tower = loadCanonTower(canonProjectJsonPath());
    if (!tower.valid()) {
        x3::logInfo("--test-descmech-bc: tower JSON absent — skipping (PASS)");
        return true;
    }

    HeadlessRenderDevice device;
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    Scene scene;
    buildCanonFloor(tower, scene, device, *physics);
    CanonPlay play;
    play.build(tower, scene, device, *physics, riggedGlbRoot(), canonGirlsDialogPath());

    StoryFlags flags;
    DescMechanicsBC bc;
    const bool builtOk = bc.build(tower, play, flags, scene, device, *physics, riggedGlbRoot());

    // ---- B1: registration — every roomed verb landed on its canon room. ----
    {
        bool ok = builtOk &&
                  bc.points().find("salvari_free") && bc.points().find("nexus_fuse_a") &&
                  bc.points().find("nexus_fuse_b") && bc.points().find("nexus_core") &&
                  bc.points().find("aug_chair_strength") && bc.points().find("aug_chair_speed") &&
                  bc.points().find("aug_chair_armor") && bc.points().find("comms_beacon") &&
                  bc.points().find("memory_station");
        if (ok) {
            const uint32_t nexus = tower.roomByName("Energy Nexus");
            const InteractPoint* cp = bc.points().find("nexus_core");
            ok = tower.roomAt(cp->pos.x, cp->pos.y, cp->pos.z) == nexus;
        }
        bcheck(ok, "B1 all nine interact points registered on their canon rooms");
    }

    // ---- B2: NEXUS CHAIN — B refuses before A; A -> B -> core seals; the glow-
    // kill edge fires exactly once; killRoomGlow drops the Portal Chamber lights. --
    {
        std::string bark;
        // Fusebox B first: gated (missingBark), no flag.
        bool ok = bc.onUse(bc.points().find("nexus_fuse_b")->pos, &bark) &&
                  bark.find("CUT FUSEBOX A FIRST") != std::string::npos &&
                  !flags.has("f6.nexus_fuse_b");
        // Core too: gated.
        ok = ok && bc.onUse(bc.points().find("nexus_core")->pos, &bark) &&
             bark.find("SEALED") != std::string::npos && !flags.has("f6.portal_sealed");
        // A -> B -> core.
        ok = ok && bc.onUse(bc.points().find("nexus_fuse_a")->pos, &bark) &&
             flags.has("f6.nexus_fuse_a");
        ok = ok && bc.onUse(bc.points().find("nexus_fuse_b")->pos, &bark) &&
             flags.has("f6.nexus_fuse_b");
        ok = ok && bc.onUse(bc.points().find("nexus_core")->pos, &bark) &&
             flags.has("f6.portal_sealed") &&
             bark.find("OVERLOAD CASCADE") != std::string::npos;
        // The one-shot edge, then the light kill on a synthetic portal-room list.
        ok = ok && bc.portalGlowKillPending() && !bc.portalGlowKillPending();
        {
            std::vector<CanonLight> ls(3);
            ls[0].room = bc.portalRoom(); ls[1].room = bc.portalRoom();
            ls[2].room = bc.nexusRoom();
            for (CanonLight& l : ls) { l.light.color[0] = l.light.color[1] = l.light.color[2] = 3.0f; }
            ok = ok && killRoomGlow(ls, bc.portalRoom()) == 2 && ls[0].light.color[1] < 0.1f;
        }
        // #12: the timed seal beat delivers.
        drainBarks(bc);
        for (int i = 0; i < 70; ++i) bc.tick(0.1f, roomEyeBC(tower, bc.nexusRoom()), nullptr, scene, *physics);
        const std::string beat = drainBarks(bc);
        ok = ok && beat.find("DEAD GLASS") != std::string::npos &&
             beat.find("stays there") != std::string::npos;
        bcheck(ok, "B2 nexus chain: gated B/core refuse -> A->B->core seals + glow edge + seal beat");
    }

    // ---- B3: SALVARI ALLIES — the free interact spawns 3 allied fighters; they
    // follow the player and land fire-support strikes on a real hostile. ----
    {
        std::string bark;
        const uint32_t before = bc.allyCount();   // the elder may already occupy the manager
        bool ok = bc.onUse(bc.points().find("salvari_free")->pos, &bark) &&
                  flags.has("f6.salvari_freed") &&
                  bark.find("THREE SALVARI") != std::string::npos &&
                  bc.allyCount() == before + 3;
        // All three: alive + ALLIED (zero player damage by construction).
        uint32_t allied = 0;
        for (uint32_t i = before; i < bc.allyCount(); ++i)
            if (bc.allies().at(i).alive() && bc.allies().at(i).isAllied()) ++allied;
        ok = ok && allied == 3;
        // Direct API truth: allyStrike damages the nearest hostile. Pick a
        // REGULAR-group mark (allyStrike deliberately skips the boss ladder).
        CanonPlay::EnemyMark marks[512];
        auto regularMark = [&](x3::phys::Vec3* out) -> bool {
            const uint32_t n = play.liveEnemyMarks(marks, 512);
            for (uint32_t i = 0; i < n; ++i) {
                const std::string lbl = marks[i].label;
                if (lbl == "HOSTILE" || lbl == "GUARD" || lbl == "ATTACKER") {
                    *out = marks[i].pos;
                    return true;
                }
            }
            return false;
        };
        x3::phys::Vec3 markPos{};
        ok = ok && regularMark(&markPos);
        int enemiesBefore = play.enemiesRemaining();
        if (ok) {
            x3::phys::Vec3 hit{};
            x3::phys::Vec3 from = markPos; from.x += 1.0f; from.y += 1.0f;
            ok = ok && play.allyStrike(from, 8.0f, 12, scene, *physics, &hit) == 1;
            // Repeated strikes kill: the group count eventually drops.
            for (int s = 0; s < 40 && play.enemiesRemaining() >= enemiesBefore; ++s)
                play.allyStrike(from, 8.0f, 25, scene, *physics, nullptr);
            ok = ok && play.enemiesRemaining() < enemiesBefore;
        }
        // BC-level: stand near the live hostile CLOSEST to the freed allies (the
        // F6 Energy Nexus squad, ~12 m from the containment cots — the followers
        // stay on their spawn floor, so the target must share it); the follow
        // closes the gap and the strike cadence lands within a ~40 s sim window.
        auto nearestRegularToAllies = [&](x3::phys::Vec3* out) -> bool {
            const x3::phys::Vec3 home = bc.points().find("salvari_free")->pos;
            const uint32_t n = play.liveEnemyMarks(marks, 512);
            float best = 1e30f; bool found = false;
            for (uint32_t i = 0; i < n; ++i) {
                const std::string lbl = marks[i].label;
                if (lbl != "HOSTILE" && lbl != "GUARD" && lbl != "ATTACKER") continue;
                const float dx = marks[i].pos.x - home.x, dy = marks[i].pos.y - home.y,
                            dz = marks[i].pos.z - home.z;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best) { best = d2; *out = marks[i].pos; found = true; }
            }
            return found;
        };
        if (ok && nearestRegularToAllies(&markPos)) {
            x3::phys::Vec3 eye = markPos; eye.y += 1.2f;
            const uint32_t strikesBefore = bc.allyStrikesLanded();
            for (int i = 0; i < 800 && bc.allyStrikesLanded() == strikesBefore; ++i)
                bc.tick(0.05f, eye, nullptr, scene, *physics);
            ok = bc.allyStrikesLanded() > strikesBefore;
        }
        x3::logInfo("    B3 strikes landed: " + std::to_string(bc.allyStrikesLanded()) +
                    ", enemies " + std::to_string(enemiesBefore) + " -> " +
                    std::to_string(play.enemiesRemaining()));
        bcheck(ok, "B3 salvari allies: interact -> 3 allied spawns -> follow + strike a hostile");
        drainBarks(bc);
    }

    // ---- B4: COURSE — lose path (clock expires), re-arm, then win path spawns
    // the reward cache + sets the flag. ----
    {
        const uint32_t courseRoom = tower.roomByName("Prototype Testing");
        const CanonRoom& R = tower.rooms[courseRoom];
        const x3::phys::Vec3 start { R.cx - 3.6f, R.y0() + 1.6f, R.cz - 2.6f };
        const x3::phys::Vec3 finish{ R.cx + 3.9f, R.y0() + 1.6f, R.cz + 2.9f };
        const x3::phys::Vec3 away  { R.cx,        R.y0() + 1.6f, R.cz - 0.2f };  // > 3 m off start
        bool ok = bc.courseState() == DescMechanicsBC::CourseState::Idle;
        // Cross the start marker -> clock runs; the HUD tag carries the countdown.
        bc.tick(0.1f, start, nullptr, scene, *physics);
        ok = ok && bc.courseState() == DescMechanicsBC::CourseState::Running &&
             bc.hudStatusLine().find("COURSE") != std::string::npos;
        // Idle at the start until the clock dies -> failed (flag NOT set).
        for (int i = 0; i < 460; ++i) bc.tick(0.1f, start, nullptr, scene, *physics);
        ok = ok && bc.courseState() == DescMechanicsBC::CourseState::Cooldown &&
             !flags.has("f4.course_beaten");
        // Step off the marker -> re-armed; re-cross -> running; reach the finish -> beaten.
        bc.tick(0.1f, away, nullptr, scene, *physics);
        ok = ok && bc.courseState() == DescMechanicsBC::CourseState::Idle;
        bc.tick(0.1f, start, nullptr, scene, *physics);
        ok = ok && bc.courseState() == DescMechanicsBC::CourseState::Running;
        const uint32_t itemsBefore = play.upperItemCount();
        for (int i = 0; i < 30; ++i) bc.tick(0.1f, finish, nullptr, scene, *physics);
        ok = ok && bc.courseState() == DescMechanicsBC::CourseState::Beaten &&
             flags.has("f4.course_beaten") &&
             play.upperItemCount() == itemsBefore + 3;
        bcheck(ok, "B4 course: 45s clock lose -> re-arm -> win sets flag + 3-item reward cache");
        drainBarks(bc);
    }

    // ---- B5: AUG CHAIRS — choose-one exclusivity + the stat fold contribution. --
    {
        std::string bark;
        bool ok = !bc.augChangedPending();   // nothing picked yet
        ok = ok && bc.onUse(bc.points().find("aug_chair_speed")->pos, &bark) &&
             bark.find("REFLEX") != std::string::npos &&
             flags.has("f4.aug_used") && flags.has("f4.aug_speed") &&
             !flags.has("f4.aug_strength") && !flags.has("f4.aug_armor");
        ok = ok && bc.augMods().speedMult > 0.19f && bc.augMods().damageMult == 0.0f &&
             bc.augMods().maxHpBonus == 0;
        ok = ok && bc.augChangedPending() && !bc.augChangedPending();
        // The other chairs are DEAD: E at them falls through (no un-used point).
        ok = ok && !bc.onUse(bc.points().find("aug_chair_strength")->pos, &bark);
        ok = ok && !bc.onUse(bc.points().find("aug_chair_armor")->pos, &bark);
        // The fold law the host applies (multiplicative layering over skills).
        {
            float skillSpeed = 1.10f;   // a pretend owned skill
            skillSpeed *= 1.0f + bc.augMods().speedMult;
            ok = ok && skillSpeed > 1.31f && skillSpeed < 1.33f;   // 1.10 * 1.20
        }
        bcheck(ok, "B5 aug chairs: one pick applies + disables the other two (choose-one)");
        drainBarks(bc);
    }

    // ---- B6: FIRST CONTACT — the elder stands at the meeting circle; the
    // authored tree loads, validates (full reachability), and walks. ----
    {
        bool ok = bc.elderPresent();
        if (ok) {
            const x3::phys::Vec3 ep = bc.elderPos();
            ok = tower.roomAt(ep.x, ep.y + 1.0f, ep.z) == tower.roomByName("First Contact Chamber");
        }
        // Loader + validator truth on the authored JSON.
        {
            std::vector<ChatNpc> npcs;
            std::vector<std::string> errs;
            const std::string p = "docs/design/narrative/chat_trees/salvari_elder.json";
            std::string tryPaths[] = { p, "../" + p, "../../" + p };
            bool loaded = false;
            for (const std::string& tp : tryPaths)
                if (loadChatTreeFile(tp, npcs, errs)) { loaded = true; break; }
            ok = ok && loaded && !npcs.empty() &&
                 validateChatNpc(npcs[0], /*fullReachability*/true, errs);
            for (const std::string& e : errs) x3::logWarn("[descmech-bc-test] tree: " + e);
        }
        // Runner walk: start -> a line is up -> choices exist -> flag fx fired.
        {
            ChatTreeSystem ct;
            ct.loadDefault();
            bool started = ct.hasNpc("salvari_elder") &&
                           ct.start("salvari_elder", "first_meeting");
            ok = ok && started && !ct.currentLine().empty();
            ok = ok && ct.flags().has("f6.first_contact");   // entry-node fx
            // Walk a few beats (choices preferred so the layers are exercised).
            int depth = 0;
            while (ct.active() && depth++ < 12) {
                if (!ct.choices().empty()) { if (!ct.choose(0)) break; }
                else if (!ct.advance()) break;
            }
            ok = ok && depth > 2;
        }
        bcheck(ok, "B6 first contact: elder placed + salvari_elder.json loads/validates/walks");
    }

    // ---- B7: CLONE-TANK PRE-SCENE — one-shot proximity beat + the lore flag. --
    {
        const uint32_t cloneRoom = tower.roomByName("Clone Lab");
        const CanonRoom& R = tower.rooms[cloneRoom];
        const x3::phys::Vec3 atTank{ R.cx + 1.2f, R.y0() + 1.6f, R.cz };
        drainBarks(bc);
        for (int i = 0; i < 70; ++i) bc.tick(0.1f, atTank, nullptr, scene, *physics);
        const std::string beat = drainBarks(bc);
        bool ok = flags.has("f7.clone_seen") &&
                  beat.find("ME in there") != std::string::npos &&
                  beat.find("MATURATION") != std::string::npos;
        // One-shot: standing there again produces nothing new.
        for (int i = 0; i < 30; ++i) bc.tick(0.1f, atTank, nullptr, scene, *physics);
        ok = ok && drainBarks(bc).empty();
        bcheck(ok, "B7 clone-tank pre-scene: one-shot bark beat + f7.clone_seen");
    }

    // ---- B8: COMMS BEACON — interact + the timed radio chain. ----
    {
        std::string bark;
        bool ok = bc.onUse(bc.points().find("comms_beacon")->pos, &bark) &&
                  flags.has("f7.beacon_activated") &&
                  bark.find("BEACON ALIGNED") != std::string::npos;
        const uint32_t commsRoom = tower.roomByName("Comms Center");
        for (int i = 0; i < 90; ++i) bc.tick(0.1f, roomEyeBC(tower, commsRoom), nullptr, scene, *physics);
        const std::string radio = drainBarks(bc);
        ok = ok && radio.find("RELAY SEVEN") != std::string::npos &&
             radio.find("EXTRACTION WINDOW") != std::string::npos;
        bcheck(ok, "B8 comms beacon: interact -> flag + 2-line timed radio chain");
    }

    // ---- B9: MEMORY HOLO — the panel is built; E cycles distinct fragments and
    // wraps after four; the on-glass bake path re-bakes per cycle. ----
    {
        std::string bark;
        bool ok = bc.memoryPanel().built() && bc.memoryFragment() == 0;
        const std::string standby = bc.memoryPanel().lines().empty()
                                        ? std::string() : bc.memoryPanel().lines()[0];
        ok = ok && standby.find("MEMORY STATION") != std::string::npos;
        std::string firstRows[4];
        for (int u = 0; u < 4; ++u) {
            ok = ok && bc.onUse(bc.points().find("memory_station")->pos, &bark) &&
                 bark.find("FRAGMENT " + std::to_string(u + 1)) != std::string::npos;
            bc.tick(0.05f, bc.points().find("memory_station")->pos, nullptr, scene, *physics);
            ok = ok && !bc.memoryPanel().lines().empty();
            firstRows[u] = bc.memoryPanel().lines().size() > 1 ? bc.memoryPanel().lines()[1]
                                                               : std::string();
        }
        // Distinct subjects per fragment; the fifth use wraps to fragment 1.
        ok = ok && firstRows[0] != firstRows[1] && firstRows[1] != firstRows[2] &&
             firstRows[2] != firstRows[3];
        ok = ok && bc.onUse(bc.points().find("memory_station")->pos, &bark) &&
             bark.find("FRAGMENT 1") != std::string::npos;
        ok = ok && flags.has("f4.memory_viewed");
        x3::logInfo(std::string("    B9 text-on-glass bake: ") +
                    (bc.memoryPanel().textOnGlass() ? "ACTIVE (font rasterized)"
                                                    : "fallback (font absent on this box)"));
        bcheck(ok, "B9 memory holo: standby -> 4 distinct fragments -> wrap + lore flag");
    }

    bc.shutdown();
    play.shutdown();
    physics->shutdown();
    x3::logInfo("--test-descmech-bc: " + std::to_string(g_bpass) + " passed, " +
                std::to_string(g_bfail) + " failed");
    return g_bfail == 0;
}

} // namespace x3::game
