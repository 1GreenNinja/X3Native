#include "sealife.h"

#include "asset_root.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

float wrapPi(float a) {
    while (a > kPi)  a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

// Slew `cur` toward `want` at `rate` rad/s, the short way round.
float slew(float cur, float want, float rate, float dt) {
    const float d = wrapPi(want - cur);
    const float m = rate * dt;
    if (d >  m) return cur + m;
    if (d < -m) return cur - m;
    return want;
}

// THE SPECIES TABLE. Every number here is a design decision, so it lives in one
// readable block rather than scattered through the state machine.
//
//                                                                                     detect stalk commit charge  bite      bite  veer   depth      hp   pred surf
//  key           rig                len  cruise charge  turn                          rad    rad  delay  range   range dmg  cool  time   min  max
const SeaSpeciesDef kSpecies[(int)SeaSpecies::Count] = {
    // GREAT WHITE — the hunter. 5 m. Two or three bites kill a 100 HP player.
    // He commits after ~3.5 s of stalking, which is long enough to be TERRIFYING and
    // short enough not to be boring. surfacer = his dorsal cuts the surface: THE TELL.
    { "GreatWhite", "shark.glb",     5.0f,  2.3f,  9.5f,  1.35f,  40.0f, 17.0f, 3.5f, 22.0f, 3.0f, 40, 2.2f, 2.6f,  0.5f,  6.0f, 220, true,  true  },
    // BLUE SHARK — deeper, leaner, more patient. Stalks nearly twice as long before
    // committing and hits for half as much: a threat, not an execution.
    { "BlueShark",  "blueshark.glb", 3.2f,  2.0f,  7.2f,  1.55f,  32.0f, 22.0f, 6.5f, 17.0f, 2.3f, 22, 2.6f, 3.0f,  7.0f, 40.0f, 140, true,  false },
    // GIANT SQUID — the abyss. It does not hunt. cruiseSpeed is a drift; chargeSpeed
    // equals it, and commitDelay is huge, so it never really runs the attack line —
    // but contact with 10 m of squid still hurts. Depth band sits it at the undersea
    // base, far below kSeaZapDepth: the zap CANNOT reach it.
    { "GiantSquid", "squid.glb",    10.0f,  0.65f, 0.9f,  0.45f,  30.0f, 12.0f, 9.0f, 14.0f, 5.5f, 18, 3.2f, 4.0f, 38.0f, 66.0f, 400, true,  false },
};

} // namespace

const SeaSpeciesDef& seaSpeciesDef(SeaSpecies s) {
    const int i = (int)s;
    return kSpecies[(i >= 0 && i < (int)SeaSpecies::Count) ? i : 0];
}

const char* seaStateName(SeaState s) {
    switch (s) {
        case SeaState::Patrol:  return "PATROL";
        case SeaState::Stalk:   return "STALK";
        case SeaState::Charge:  return "CHARGE";
        case SeaState::Bite:    return "BITE";
        case SeaState::VeerOff: return "VEER";
        case SeaState::Dead:    return "DEAD";
    }
    return "?";
}

uint32_t SeaLifeSystem::rng() {
    m_rngState = m_rngState * 1664525u + 1013904223u;
    return m_rngState;
}
float SeaLifeSystem::frand() { return (float)(rng() >> 8) / 16777216.0f; }

float SeaLifeSystem::waterAt(float x, float z) const {
    return m_water ? m_water(x, z) : kFishDryTest - 1.0f;
}
float SeaLifeSystem::bedAt(float x, float z, float surface) const {
    return m_bed ? m_bed(x, z) : (surface - 60.0f);
}

// ---------------------------------------------------------------------------
void SeaLifeSystem::spawn(const SeaCreatureDesc& d, Scene& scene,
                          x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics) {
    const SeaSpeciesDef& sd = seaSpeciesDef(d.species);

    const float surf = waterAt(d.homeX, d.homeZ);
    if (surf <= kFishDryTest) {
        x3::logInfo(std::string("sealife: ") + sd.key + " home is DRY at ("
                    + std::to_string((int)d.homeX) + "," + std::to_string((int)d.homeZ)
                    + ") — skipped");
        return;
    }

    SeaCreature c;
    c.species = d.species;
    c.homeX = d.homeX; c.homeZ = d.homeZ; c.roam = d.roam;
    c.patrolAng = frand() * 2.0f * kPi;
    c.yaw = d.heading;
    c.health = sd.health;
    c.x = d.homeX + std::cos(c.patrolAng) * d.roam;
    c.z = d.homeZ + std::sin(c.patrolAng) * d.roam;

    const float s2 = waterAt(c.x, c.z);
    const float useSurf = (s2 > kFishDryTest) ? s2 : surf;
    const float bed = bedAt(c.x, c.z, useSurf);
    // sit it in its depth band, but never through the seabed
    c.depthPhase = frand() * 2.0f * kPi;
    float depth = sd.depthMin + frand() * std::max(0.0f, sd.depthMax - sd.depthMin);
    c.wantDepth = depth;
    c.y = useSurf - depth;
    c.y = std::max(c.y, bed + sd.length * 0.35f);
    c.y = std::min(c.y, useSurf - sd.depthMin);
    c.speed = sd.cruiseSpeed;

    // The INERT prop that owns the skinned GLB (app/crowd_skin.cpp's trick).
    MonsterSystem::Tuning t;
    t.type       = MonsterType::Guard;
    t.chaseSpeed = 0.0f;      // THIS system owns motion
    t.damage     = 0;         // THIS system owns damage
    t.ranged     = false;
    t.noBody     = true;      // pure visual, no hitbox
    t.modelFile  = sd.rig;
    t.modelDirOverride = riggedGlbRoot();
    t.standUpZtoY = false;    // rigged_glb sources are Y-up
    t.modelScale  = sd.length;   // the GLB is normalized to 1 m long

    c.sys = std::make_unique<MonsterSystem>();
    c.sys->buildMonsterTuned(scene, device, physics, riggedGlbRoot(),
                             x3::phys::Vec3{ c.x, c.y, c.z }, t);

    if (!c.sys->usingRealModel() || !c.sys->skinnable()) {
        // Never ship an invisible damage source. Log loudly and drop it.
        x3::logInfo(std::string("sealife: ") + sd.key + " rig '" + sd.rig
                    + "' FAILED to load/skin — creature dropped");
        scene.get(c.sys->entity()).visible = false;
        c.sys.reset();
        return;
    }
    c.skinned = true;
    scene.get(c.sys->entity()).roomId = m_cfg.roomId;
    c.sys->setCalmLoop("cruise");     // the baked travelling-sine loop
    m_creatures.push_back(std::move(c));
}

void SeaLifeSystem::build(const SeaConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device,
                          x3::phys::IPhysicsWorld& physics) {
    m_cfg = cfg;
    m_rngState = cfg.seed ? cfg.seed : 0x5EA1Fu;
    m_creatures.clear();
    m_creatures.reserve(cfg.creatures.size());
    for (const auto& d : cfg.creatures) spawn(d, scene, device, physics);
    m_built = true;
    x3::logInfo("sealife: " + std::to_string(m_creatures.size()) + " big animals in the sea");
}

// ---------------------------------------------------------------------------
// THE HUNT. One creature's mind.
// ---------------------------------------------------------------------------
void SeaLifeSystem::think(SeaCreature& c, float dt, const x3::phys::Vec3& playerFeet,
                          bool playerInWater, Player* player) {
    const SeaSpeciesDef& d = seaSpeciesDef(c.species);
    c.stateT += dt;
    if (c.biteCool > 0.0f) c.biteCool -= dt;

    const float dx = playerFeet.x - c.x;
    const float dz = playerFeet.z - c.z;
    const float dy = playerFeet.y - c.y;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    // A DRY PLAYER IS NEVER PREY. Standing on the bank, you are simply not in the
    // shark's world — he goes back to his circuits. (Self-test S6.)
    const bool huntable = playerInWater && d.predator && player != nullptr
                          && player->isAlive();

    auto go = [&](SeaState s) { c.state = s; c.stateT = 0.0f; };

    switch (c.state) {
        case SeaState::Patrol: {
            c.speed = d.cruiseSpeed;
            // A surfacer breathes the surface: a slow rise/sink through its band, so
            // the dorsal CUTS, disappears, and cuts again. A shark pinned at one depth
            // either never shows a fin or never stops showing one; both are wrong.
            if (c.holdDepth) {
                // staged: the caller owns wantDepth
            } else if (d.surfacer) {
                const float mid = 0.5f * (d.depthMin + d.depthMax);
                const float amp = 0.5f * (d.depthMax - d.depthMin);
                c.wantDepth = mid + amp * std::sin(m_time * 0.30f + c.depthPhase);
            } else {
                c.wantDepth = 0.5f * (d.depthMin + d.depthMax);
            }
            c.patrolAng += (d.cruiseSpeed / std::max(4.0f, c.roam)) * dt;
            const float tx = c.homeX + std::cos(c.patrolAng) * c.roam;
            const float tz = c.homeZ + std::sin(c.patrolAng) * c.roam;
            const float want = std::atan2(-(tx - c.x), -(tz - c.z));  // model faces -Z
            c.yaw = slew(c.yaw, want, d.turnRate, dt);
            if (huntable && dist < d.detectRadius) {
                x3::logInfo(std::string("sealife: ") + d.key + " has the scent");
                go(SeaState::Stalk);
            }
        } break;

        case SeaState::Stalk: {
            // Circle him at stalkRadius, closing slowly. THIS is the dread — the
            // shark does not charge on sight, he takes his time.
            if (!huntable || dist > d.detectRadius * 1.35f) { go(SeaState::Patrol); break; }
            c.speed = d.cruiseSpeed * 1.25f;
            const float toP = std::atan2(-dx, -dz);
            // orbit: aim tangentially, biased inward as the stalk matures
            const float closing = std::min(1.0f, c.stateT / std::max(0.5f, d.commitDelay));
            const float tangent = toP + (kPi * 0.5f) * (1.0f - closing * 0.85f);
            c.yaw = slew(c.yaw, tangent, d.turnRate, dt);
            // rise/sink to HIS level — he is the prey, not a depth you happen to share
            const float surfP = waterAt(playerFeet.x, playerFeet.z);
            if (surfP > kFishDryTest && !c.holdDepth)
                c.wantDepth = std::clamp(surfP - playerFeet.y, d.depthMin, d.depthMax);
            if (c.stateT >= d.commitDelay && dist <= d.chargeRange) {
                x3::logInfo(std::string("sealife: ") + d.key + " COMMITS");
                c.bitThisPass = false;      // a fresh run
                go(SeaState::Charge);
            }
        } break;

        case SeaState::Charge: {
            if (!huntable) { go(SeaState::Patrol); break; }
            c.speed = d.chargeSpeed;
            const float surfC = waterAt(playerFeet.x, playerFeet.z);
            if (surfC > kFishDryTest && !c.holdDepth)
                c.wantDepth = std::clamp(surfC - playerFeet.y, d.depthMin, d.depthMax);
            const float want = std::atan2(-dx, -dz);
            c.yaw = slew(c.yaw, want, d.turnRate * 2.2f, dt);
            if (dist <= d.biteRange) { go(SeaState::Bite); break; }
            // missed / overshot -> peel off and come back around
            if (c.stateT > 3.2f) go(SeaState::VeerOff);
        } break;

        case SeaState::Bite: {
            c.speed = d.chargeSpeed * 0.55f;
            // ONCE PER PASS. The latch — not a per-frame drain. (Self-test S5.)
            if (!c.bitThisPass && c.biteCool <= 0.0f && huntable && dist <= d.biteRange * 1.25f) {
                if (player->takeDamage(d.biteDamage)) {
                    c.bitThisPass = true;
                    c.biteCool = d.biteCooldown;
                    x3::logInfo(std::string("sealife: ") + d.key + " BITES for "
                                + std::to_string(d.biteDamage));
                }
            }
            if (c.stateT > 0.45f) go(SeaState::VeerOff);
        } break;

        case SeaState::VeerOff: {
            c.speed = d.chargeSpeed * 0.8f;
            const float away = std::atan2(dx, dz);   // opposite of "toward"
            c.yaw = slew(c.yaw, away, d.turnRate * 1.4f, dt);
            if (c.stateT >= d.veerTime) {
                c.bitThisPass = false;               // next approach may bite again
                go(huntable ? SeaState::Stalk : SeaState::Patrol);
            }
        } break;

        case SeaState::Dead: break;
    }

    // Clip selection: the sharks run the fast loop on the attack line.
    if (c.sys && c.skinned) {
        const bool hot = (c.state == SeaState::Charge || c.state == SeaState::Bite);
        c.sys->setCalmLoop(hot ? "charge" : "cruise");
    }
}

// Integrate position + hold the depth band (kinematic, dt-scaled).
void SeaLifeSystem::swim(SeaCreature& c, float dt) {
    const SeaSpeciesDef& d = seaSpeciesDef(c.species);

    if (c.dead) {
        // Belly-up: roll over, rise to the surface, drift, despawn.
        c.deadT += dt;
        const float surf = waterAt(c.x, c.z);
        if (surf > kFishDryTest) {
            const float want = surf - d.length * 0.10f;
            c.y += (want - c.y) * std::min(1.0f, 0.55f * dt);
        }
        c.x += std::sin(c.yaw) * 0.35f * dt;
        c.z += std::cos(c.yaw) * 0.35f * dt;
        c.pitch = 0.0f;
        return;
    }

    const float fx = -std::sin(c.yaw);      // model faces -Z at yaw 0
    const float fz = -std::cos(c.yaw);
    c.x += fx * c.speed * dt;
    c.z += fz * c.speed * dt;

    // Depth: hold the species band under the LOCAL surface, never through the bed.
    const float surf = waterAt(c.x, c.z);
    if (surf <= kFishDryTest) {
        // wandered onto land — turn hard back toward home
        const float home = std::atan2(-(c.homeX - c.x), -(c.homeZ - c.z));
        c.yaw = slew(c.yaw, home, d.turnRate * 3.0f, dt);
        return;
    }
    const float bed = bedAt(c.x, c.z, surf);
    // aim for the depth the mind asked for, always inside the species band
    const float wantDepth = std::clamp(c.wantDepth, d.depthMin, d.depthMax);
    float wantY = surf - wantDepth;
    const float floorY = bed + d.length * 0.30f;
    if (wantY < floorY) wantY = std::min(floorY, surf - d.depthMin);

    const float dy = wantY - c.y;
    const float rise = std::max(0.6f, c.speed * 0.55f);
    const float step = std::clamp(dy, -rise * dt, rise * dt);
    c.y += step;
    // pitch follows the climb/dive so he does not swim flat while changing depth
    const float wantPitch = std::clamp(dy * 0.28f, -0.42f, 0.42f);
    c.pitch += (wantPitch - c.pitch) * std::min(1.0f, 3.0f * dt);
}

// ---------------------------------------------------------------------------
// THE WAKE (surface foam). See the design block in sealife.h: a per-creature
// ring of surface samples drawn as soft alpha billboards through the engine's
// particle pass — the scene-entity attempt died in the glass depth pre-pass.
// ---------------------------------------------------------------------------
float SeaLifeSystem::wakeGate(const SeaCreature& c) const {
    if (c.dead || c.gone) return 0.0f;
    const float surf = waterAt(c.x, c.z);
    if (surf <= kFishDryTest) return 0.0f;
    // Depth of the BODY TOP (his back), not his centre: the mark on the surface
    // is made by the part of him nearest to it.
    const float topDepth = (surf - c.y) - seaSpeciesDef(c.species).length * kWakeTopFrac;
    return std::clamp((kWakeZeroTop - topDepth) / (kWakeZeroTop - kWakeFullTop),
                      0.0f, 1.0f);
}

void SeaLifeSystem::updateWake(SeaCreature& c, float dt) {
    // Age the whole trail first — dead or diving, old foam keeps dissipating.
    for (auto& p : c.wake)
        if (p.age < kWakeLife) p.age += dt;
    if (c.dead) return;

    const float s = wakeGate(c);
    if (s <= 0.0f) { c.wakeDist = 0.0f; return; }   // deep: no surface mark

    c.wakeDist += c.speed * dt;
    if (c.wakeDist < kWakeSampleDist) return;
    c.wakeDist -= kWakeSampleDist;

    const float surf = waterAt(c.x, c.z);
    if (surf <= kFishDryTest) return;

    SeaCreature::WakePoint& p = c.wake[c.wakeHead];
    c.wakeHead = (c.wakeHead + 1) % kWakePoints;
    const float fx = -std::sin(c.yaw), fz = -std::cos(c.yaw);   // model faces -Z
    p.x = c.x; p.z = c.z;
    p.surfY = surf;
    p.perpX = -fz; p.perpZ = fx;    // unit perpendicular in the surface plane
    p.speed = c.speed;
    p.strength = s;
    p.age = 0.0f;
}

uint32_t SeaLifeSystem::buildWakeFoam(int only, uint32_t& decalCount,
                                      uint32_t& churnCount) const {
    uint32_t nd = 0;   // trail decals  -> m_wakeTrailScratch
    uint32_t np = 0;   // churn blobs   -> m_wakeChurnScratch
    // Foam tint: unlit grey-white with a cold cast. ALPHA-blended (never additive),
    // per DECISIONS.md "VALUE, NOT LUMENS" — bright against dark water without ever
    // feeding bloom, so it cannot become a flat white slab.
    constexpr float kR = 0.70f, kG = 0.74f, kB = 0.78f;
    for (uint32_t ci = 0; ci < m_creatures.size(); ++ci) {
        if (only >= 0 && (int)ci != only) continue;
        const SeaCreature& c = m_creatures[ci];
        if (c.gone || !c.active) continue;

        // ---- THE V: two spreading arms + the churned centreline they peel from.
        // DECALS, laid flat on the surface plane (normal +Y): a billboard trail
        // smears vertically when seen from a low bank; a decal hugs the water.
        uint32_t pi = 0;
        for (const auto& p : c.wake) {
            const uint32_t pIdx = pi++;
            if (p.age >= kWakeLife) continue;
            const float f = p.strength * (1.0f - p.age / kWakeLife);
            if (f <= 0.01f) continue;
            if (nd + 3 > kWakeMaxInstances) break;
            // The arms leave the path at the Kelvin half-angle: a point dropped
            // t seconds ago sits offset (t * speed * tan 19.47°) off the line.
            const float off = 0.18f
                + std::min(kWakeSpreadMax, p.age * p.speed * kWakeSpreadTan);
            const float y        = p.surfY + kWakeLift;
            const float armSize  = 0.30f + p.age * 0.20f;   // patches GROW...
            const float armAlpha = 0.42f * f;               // ...as they thin out
            // deterministic per-point spin so the round marks don't tile
            const float spin = (float)((pIdx * 2654435761u + ci * 7919u) & 1023u)
                               * (2.0f * kPi / 1024.0f);
            for (int side = -1; side <= 1; side += 2) {
                auto& q = m_wakeTrailScratch[nd++];
                q.center[0] = p.x + p.perpX * off * (float)side;
                q.center[1] = y;
                q.center[2] = p.z + p.perpZ * off * (float)side;
                q.halfSize  = armSize;
                q.normal[0] = 0.0f; q.normal[1] = 1.0f; q.normal[2] = 0.0f;
                q.angle     = spin * (float)side;
                q.color[0] = kR; q.color[1] = kG; q.color[2] = kB;
                q.color[3] = armAlpha;
            }
            auto& mid = m_wakeTrailScratch[nd++];   // the centreline: wider, fainter
            mid.center[0] = p.x; mid.center[1] = y; mid.center[2] = p.z;
            mid.halfSize  = 0.46f + p.age * 0.26f;
            mid.normal[0] = 0.0f; mid.normal[1] = 1.0f; mid.normal[2] = 0.0f;
            mid.angle     = spin;
            mid.color[0] = kR; mid.color[1] = kG; mid.color[2] = kB;
            mid.color[3] = 0.26f * f;
        }

        // ---- THE CHURN: froth boiling where the fin cuts, right now. Immediate-
        // mode billboard blobs orbiting the cut on deterministic per-blob phases —
        // animated by m_time, owning no state at all. Billboards on purpose:
        // churned froth genuinely stands out of the water at the cut.
        const float s = wakeGate(c);
        if (s > 0.0f) {
            const float surf = waterAt(c.x, c.z);
            if (surf > kFishDryTest) {
                const float y = surf + kWakeLift;
                for (int k = 0; k < kWakeChurnBlobs; ++k) {
                    if (np >= kWakeMaxInstances) break;
                    const float ph = (float)(((ci * 7919u + (uint32_t)k * 2654435761u) >> 8)
                                             & 1023u) * (2.0f * kPi / 1024.0f);
                    const float wob = m_time * (1.3f + 0.45f * std::sin(ph * 3.0f));
                    const float r   = 0.10f + 0.34f
                        * (0.5f + 0.5f * std::sin(ph * 5.0f + wob * 0.7f));
                    const float ang = ph + wob;
                    auto& q = m_wakeChurnScratch[np++];
                    q.pos[0] = c.x + std::cos(ang) * r;
                    q.pos[1] = y;
                    q.pos[2] = c.z + std::sin(ang) * r;
                    q.size = 0.16f + 0.10f * (0.5f + 0.5f * std::sin(m_time * 3.1f + ph * 7.0f));
                    q.color[0] = kR; q.color[1] = kG; q.color[2] = kB;
                    q.color[3] = s * (0.30f + 0.25f * (0.5f + 0.5f * std::sin(m_time * 2.3f + ph)));
                }
            }
        }
    }
    decalCount = nd;
    churnCount = np;
    return nd + np;
}

float SeaLifeSystem::wakeStrength(uint32_t i) const {
    return (i < m_creatures.size()) ? wakeGate(m_creatures[i]) : 0.0f;
}
uint32_t SeaLifeSystem::wakeQuadCount() const {
    uint32_t nd = 0, np = 0;
    return buildWakeFoam(-1, nd, np);
}
uint32_t SeaLifeSystem::wakeQuadCount(uint32_t i) const {
    if (i >= m_creatures.size()) return 0;
    uint32_t nd = 0, np = 0;
    return buildWakeFoam((int)i, nd, np);
}

void SeaLifeSystem::writeTransform(SeaCreature& c, Scene& scene) {
    if (!c.sys || !c.skinned) return;
    const uint32_t ent = c.sys->entity();
    if (ent == kNoLink) return;
    const SeaSpeciesDef& d = seaSpeciesDef(c.species);
    const float s = d.length;

    // Model faces -Z at yaw 0 (the engine's monster convention, and what
    // tools/sealife_bake.py bakes to). Compose yaw about Y, then pitch about the
    // local right axis, then roll (PI when dead: belly-up).
    const float cy = std::cos(c.yaw), sy = std::sin(c.yaw);
    const float cp = std::cos(c.pitch), sp = std::sin(c.pitch);
    const float roll = c.dead ? kPi : 0.0f;
    const float cr = std::cos(roll), sr = std::sin(roll);

    // right (+X), up (+Y), back (+Z) of the model, in world space
    float rx = cy,        ry = 0.0f, rz = -sy;
    float ux = sy * sp,   uy = cp,   uz = cy * sp;
    float bx = sy * cp,   by = -sp,  bz = cy * cp;
    // roll about the model's own forward (-Z) axis
    const float r2x = rx * cr + ux * sr, r2y = ry * cr + uy * sr, r2z = rz * cr + uz * sr;
    const float u2x = ux * cr - rx * sr, u2y = uy * cr - ry * sr, u2z = uz * cr - rz * sr;

    float* t = scene.get(ent).transform;
    t[0]  = r2x * s; t[1]  = r2y * s; t[2]  = r2z * s; t[3]  = 0.0f;
    t[4]  = u2x * s; t[5]  = u2y * s; t[6]  = u2z * s; t[7]  = 0.0f;
    t[8]  = bx  * s; t[9]  = by  * s; t[10] = bz  * s; t[11] = 0.0f;
    t[12] = c.x;     t[13] = c.y;     t[14] = c.z;     t[15] = 1.0f;
}

void SeaLifeSystem::update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics,
                           const x3::phys::Vec3& playerFeet, Player* player) {
    if (!m_built || dt <= 0.0f) return;
    (void)device;
    m_time += dt;

    // Is the player IN the water? His FEET decide it — a wading player's eye is above
    // the surface while he is very much in the water. (Feet are passed in, so this
    // never depends on the Player's physics capsule having been spawned.)
    bool playerInWater = false;
    if (player) {
        const float w = waterAt(playerFeet.x, playerFeet.z);
        playerInWater = (w > kFishDryTest) && (playerFeet.y < w);
    }

    for (auto& c : m_creatures) {
        if (c.gone) continue;

        const float ddx = playerFeet.x - c.x, ddz = playerFeet.z - c.z;
        const bool wasActive = c.active;
        c.active = (ddx * ddx + ddz * ddz) <= (m_cfg.activeRadius * m_cfg.activeRadius);
        if (c.active != wasActive && c.sys && c.skinned)
            scene.get(c.sys->entity()).visible = c.active;
        if (!c.active) continue;                     // range gate: no sim, no draw

        if (c.dead) {
            swim(c, dt);
            updateWake(c, dt);                       // the last trail dissipates
            if (c.deadT > 30.0f) {                   // corpse despawns
                c.gone = true;
                if (c.sys && c.skinned) scene.get(c.sys->entity()).visible = false;
                continue;
            }
        } else {
            think(c, dt, playerFeet, playerInWater, player);
            swim(c, dt);
            updateWake(c, dt);                       // THE WAKE follows the fin

            // He scatters the shoals he swims through.
            if (m_fish && m_fish->built()) {
                const SeaSpeciesDef& d = seaSpeciesDef(c.species);
                (void)d;
            }

        }

        // Drive the skinned prop: feed it the pose, then STAMP our own basis over
        // the top (the monster only bakes yaw; we also want pitch, roll and scale).
        if (c.sys && c.skinned) {
            c.sys->setPropPose(x3::phys::Vec3{ c.x, c.y, c.z }, c.yaw);
            c.sys->setPropMotion(0.0f, 0.0f);        // keep the calm loop playing
            c.sys->update(dt, scene, physics, c.sys->pos());   // playerPos = self: AI never runs
            writeTransform(c, scene);
        }
    }

}

void SeaLifeSystem::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                         const Scene& scene) const {
    if (!m_built) return;
    if (!scene.roomVisible(m_cfg.roomId)) return;
    for (const auto& c : m_creatures) {
        if (c.gone || !c.active || !c.skinned || !c.sys) continue;
        c.sys->drawMonster(device, frame, scene);
    }
    // THE WAKE: foam riding the surface — the trail as DECALS laid flat on the
    // water plane, the fin churn as one alpha billboard batch. Both draw in the
    // particle pass: after the water, depth-tested read-only (banks occlude
    // foam), soft-edged, never additive. Empty when everyone is deep: zero
    // submit, zero GPU cost. (Self-test S11.)
    uint32_t nd = 0, np = 0;
    buildWakeFoam(-1, nd, np);
    static bool s_wakeLogged = false;    // one boot-style line the first time foam flows
    if (!s_wakeLogged && (nd + np) > 0) {
        s_wakeLogged = true;
        x3::logInfo("sealife wake: first foam submit — " + std::to_string(nd)
                    + " trail decals + " + std::to_string(np) + " churn blobs");
    }
    if (nd > 0) device.submitDecals(m_wakeTrailScratch, nd);
    if (np > 0)
        device.submitParticles(m_wakeChurnScratch, np,
                               x3::rhi::IRenderDevice::ParticleBlend::Alpha);
}

// THE ZAP. A surface phenomenon: it cannot reach the abyss.
uint32_t SeaLifeSystem::killWithin(float cx, float cz, float radius) {
    uint32_t killed = 0;
    for (auto& c : m_creatures) {
        if (c.dead || c.gone) continue;
        const float dx = c.x - cx, dz = c.z - cz;
        if (dx * dx + dz * dz > radius * radius) continue;

        const float surf = waterAt(c.x, c.z);
        if (surf <= kFishDryTest) continue;
        const float depth = surf - c.y;
        if (depth > kSeaZapDepth) continue;   // TOO DEEP — the squid survives, by design

        c.health -= kSeaZapDamage;
        if (c.health <= 0) {
            c.dead = true;
            c.deadT = 0.0f;
            c.state = SeaState::Dead;
            c.speed = 0.0f;
            ++killed;
            x3::logInfo(std::string("sealife: ") + seaSpeciesDef(c.species).key
                        + " KILLED by the water zap");
        }
    }
    return killed;
}

uint32_t SeaLifeSystem::aliveCount() const {
    uint32_t n = 0;
    for (const auto& c : m_creatures) if (!c.dead && !c.gone) ++n;
    return n;
}
uint32_t SeaLifeSystem::activeCount() const {
    uint32_t n = 0;
    for (const auto& c : m_creatures) if (c.active && !c.gone) ++n;
    return n;
}
bool SeaLifeSystem::finUp(uint32_t i) const {
    if (i >= m_creatures.size()) return false;
    const SeaCreature& c = m_creatures[i];
    if (c.dead || !seaSpeciesDef(c.species).surfacer) return false;
    const float surf = waterAt(c.x, c.z);
    if (surf <= kFishDryTest) return false;
    return (surf - c.y) <= kSeaFinDepth;
}
int SeaLifeSystem::findSpecies(SeaSpecies s) const {
    for (uint32_t i = 0; i < m_creatures.size(); ++i)
        if (m_creatures[i].species == s && !m_creatures[i].gone) return (int)i;
    return -1;
}

// ===========================================================================
// SELF-TEST (--test-sealife). Headless: no window, no Vulkan.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void scheck(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  PASS  ") + what); }
    else    { ++g_fail; x3::logInfo(std::string("  FAIL  ") + what); }
}

// A flat synthetic sea: surface at y=0 everywhere inside a big disc, bed at -80.
float testWater(float x, float z) {
    const float r = std::sqrt(x * x + z * z);
    return (r < 400.0f) ? 0.0f : (kFishDryTest - 1.0f);
}
float testBed(float, float) { return -80.0f; }

// Counts GPU resource creates so we can prove the sim allocates NOTHING per frame.
class SeaCountingDevice final : public HeadlessRenderDevice {
public:
    uint32_t meshCreates = 0, texCreates = 0;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                   const uint32_t* idx, uint32_t ni) override {
        ++meshCreates;
        return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
    }
    x3::rhi::TextureHandle createTexture(const void* px, uint32_t w, uint32_t h,
                                         bool mips) override {
        ++texCreates;
        return HeadlessRenderDevice::createTexture(px, w, h, mips);
    }
};

} // namespace

bool runSealifeSelfTest() {
    g_pass = 0; g_fail = 0;
    x3::logInfo("sealife self-test");

    // ---- S1: the species table is coherent.
    {
        const SeaSpeciesDef& gw = seaSpeciesDef(SeaSpecies::GreatWhite);
        const SeaSpeciesDef& bs = seaSpeciesDef(SeaSpecies::BlueShark);
        scheck(gw.predator && gw.surfacer, "S1 great white is a predator and a surfacer (THE FIN)");
        scheck(gw.chargeSpeed > gw.cruiseSpeed * 2.0f, "S1 the charge is much faster than the cruise");
        scheck(gw.biteDamage >= 35 && gw.biteDamage <= 45, "S1 a bite is 35-45 (2-3 bites kill)");
        scheck(bs.commitDelay > gw.commitDelay, "S1 the blue shark stalks LONGER before committing");
        scheck(bs.biteDamage < gw.biteDamage, "S1 the blue shark hits for less than the great white");
        const SeaSpeciesDef& sq = seaSpeciesDef(SeaSpecies::GiantSquid);
        scheck(sq.depthMin > kSeaZapDepth, "S1 the squid lives BELOW the zap's reach (the abyss is safe)");
    }

    SeaCountingDevice device;
    auto physics = x3::phys::createPhysicsWorld();

    // NOTE: we do NOT call Player::setFeetPosition here. It early-outs unless the
    // player has been SPAWNED with a physics body, which silently left the test player
    // at the origin (feet.y == 0 == the surface => "not in the water") and made the
    // shark ignore him. The feet are passed to update() explicitly instead — exactly
    // as zapPlayer() takes them — so the sim never depends on a physics capsule.

    auto makeCfg = [](SeaSpecies sp, float hx, float hz, float roam) {
        SeaConfig cfg;
        SeaCreatureDesc d;
        d.species = sp; d.homeX = hx; d.homeZ = hz; d.roam = roam;
        cfg.creatures.push_back(d);
        cfg.activeRadius = 1000.0f;    // never range-gate in the test
        cfg.seed = 0xC0FFEEu;
        return cfg;
    };

    // ---- S2: PATROL stays home and holds its depth band.
    {
        Scene scene;
        SeaLifeSystem sea;
        sea.setWaterQuery(testWater);
        sea.setBedQuery(testBed);
        sea.build(makeCfg(SeaSpecies::GreatWhite, 0, 0, 40.0f), scene, device, *physics);
        if (sea.count() == 0) {
            scheck(false, "S2 the great white spawned (rig missing? run tools/sealife_bake.ps1)");
        } else {
            const SeaSpeciesDef& d = seaSpeciesDef(SeaSpecies::GreatWhite);
            bool inRing = true, inBand = true, patrolled = true;
            for (int i = 0; i < 1200; ++i) {
                sea.update(1.0f / 60.0f, scene, device, *physics,
                           x3::phys::Vec3{ 900.0f, 0.0f, 900.0f }, nullptr);  // player far away
                const SeaCreature& c = sea.creature(0);
                const float r = std::sqrt(c.x * c.x + c.z * c.z);
                if (r > 40.0f * 2.2f) inRing = false;
                const float depth = 0.0f - c.y;
                if (depth < d.depthMin - 0.6f || depth > d.depthMax + 0.6f) inBand = false;
                if (c.state != SeaState::Patrol) patrolled = false;
            }
            scheck(inRing, "S2 patrol stays near its home ring");
            scheck(inBand, "S2 patrol holds its depth band");
            scheck(patrolled, "S2 with no player in the water he never leaves PATROL");
        }
    }

    // ---- S3/S4/S5: the hunt. detect -> stalk -> charge -> bite, ONCE per pass.
    {
        Scene scene;
        SeaLifeSystem sea;
        sea.setWaterQuery(testWater);
        sea.setBedQuery(testBed);
        sea.build(makeCfg(SeaSpecies::GreatWhite, 0, 0, 30.0f), scene, device, *physics);
        if (sea.count() == 0) {
            scheck(false, "S3 the great white spawned");
        } else {
            Player player;
            player.resetHealth();
            // IN the water (feet below the surface at y=0), right next to him.
            const x3::phys::Vec3 pp{ 0.0f, -6.0f, 0.0f };   // IN the water (surface y=0)

            constexpr float dt = 1.0f / 60.0f;
            bool sawStalk = false, sawCharge = false, sawBite = false;
            const int hp0 = player.hp();
            for (int i = 0; i < 3000 && !sawBite; ++i) {
                sea.update(dt, scene, device, *physics, pp, &player);
                player.updateHealth(dt);          // let the iframe window decay
                const SeaState s = sea.creature(0).state;
                if (s == SeaState::Stalk)  sawStalk = true;
                if (s == SeaState::Charge) sawCharge = true;
                if (s == SeaState::Bite)   sawBite = true;
            }
            scheck(sawStalk,  "S3 a swimmer in range drives PATROL -> STALK (no charge on sight)");
            scheck(sawCharge, "S4 the stalk COMMITS to a CHARGE");
            scheck(sawBite,   "S4 the charge reaches the BITE");
            // ENTERING the Bite state and ACTING in it are different frames — the
            // damage lands on the next tick. Give it room, then hold him in contact.
            for (int i = 0; i < 40; ++i) {
                sea.update(dt, scene, device, *physics, pp, &player);
                player.updateHealth(dt);
            }
            const int bitten = hp0 - player.hp();
            const int expect = seaSpeciesDef(SeaSpecies::GreatWhite).biteDamage;
            scheck(bitten == expect,
                   "S5 ONE pass = ONE bite's damage (not a per-frame drain)");
            // Pin him in contact for 600 more frames with the iframe window EXPIRING
            // every time: only the per-pass latch can stop a second bite, and a fresh
            // one may only land after a full VEER -> STALK -> CHARGE cycle.
            const int hp1 = player.hp();
            for (int i = 0; i < 600; ++i) {
                sea.update(dt, scene, device, *physics, pp, &player);
                player.updateHealth(dt);
            }
            const int drained = hp1 - player.hp();
            scheck(drained <= expect,
                   "S5 600 frames of contact cannot drain more than one further bite");
        }
    }

    // ---- S6: a DRY player is never attacked.
    {
        Scene scene;
        SeaLifeSystem sea;
        sea.setWaterQuery(testWater);
        sea.setBedQuery(testBed);
        sea.build(makeCfg(SeaSpecies::GreatWhite, 0, 0, 30.0f), scene, device, *physics);
        if (sea.count() == 0) { scheck(false, "S6 the great white spawned"); }
        else {
            Player player;
            player.resetHealth();
            const x3::phys::Vec3 pp{ 0.0f, 2.0f, 0.0f };    // ON the bank: feet ABOVE the surface
            bool everHunted = false;
            for (int i = 0; i < 2000; ++i) {
                sea.update(1.0f / 60.0f, scene, device, *physics, pp, &player);
                if (sea.creature(0).state != SeaState::Patrol) everHunted = true;
            }
            scheck(!everHunted, "S6 a player OUT of the water is never hunted");
            scheck(player.hp() == player.maxHp(), "S6 a player OUT of the water is never bitten");
        }
    }

    // ---- S7: THE ZAP.
    {
        Scene scene;
        SeaLifeSystem sea;
        sea.setWaterQuery(testWater);
        sea.setBedQuery(testBed);
        SeaConfig cfg;
        SeaCreatureDesc a; a.species = SeaSpecies::GreatWhite; a.homeX = 0;   a.homeZ = 0;   a.roam = 3.0f;
        SeaCreatureDesc b; b.species = SeaSpecies::GreatWhite; b.homeX = 120; b.homeZ = 0;   b.roam = 3.0f;
        SeaCreatureDesc s; s.species = SeaSpecies::GiantSquid; s.homeX = 4;   s.homeZ = 0;   s.roam = 3.0f;
        cfg.creatures = { a, b, s };
        cfg.activeRadius = 1000.0f;
        sea.build(cfg, scene, device, *physics);
        if (sea.count() < 3) {
            scheck(false, "S7 all three creatures spawned (rigs present?)");
        } else {
            // settle so the squid sinks into its deep band
            for (int i = 0; i < 600; ++i)
                sea.update(1.0f / 60.0f, scene, device, *physics,
                           x3::phys::Vec3{ 900, 0, 900 }, nullptr);
            const uint32_t before = sea.aliveCount();
            const uint32_t killed = sea.killWithin(0.0f, 0.0f, 12.0f);   // kWaterZapRadius
            scheck(killed == 1, "S7 the zap kills the shark inside the radius");
            scheck(sea.aliveCount() == before - 1, "S7 exactly one creature died");
            scheck(!sea.creature(1).dead, "S7 the shark OUTSIDE the radius survives");
            scheck(!sea.creature(2).dead, "S7 the DEEP squid survives — the abyss is out of reach");
        }
    }

    // ---- S8: a harmless species never damages anyone. (No harmless model shipped,
    //          so this asserts the CONTRACT the manta/whale will rely on.)
    {
        bool anyHarmlessDamages = false;
        for (int i = 0; i < (int)SeaSpecies::Count; ++i) {
            const SeaSpeciesDef& d = seaSpeciesDef((SeaSpecies)i);
            if (!d.predator && d.biteDamage > 0) anyHarmlessDamages = true;
        }
        scheck(!anyHarmlessDamages, "S8 no non-predator carries bite damage");
    }

    // ---- S9: determinism.
    {
        auto run = [&](std::vector<float>& out) {
            Scene scene;
            SeaLifeSystem sea;
            sea.setWaterQuery(testWater);
            sea.setBedQuery(testBed);
            SeaConfig cfg;
            SeaCreatureDesc d; d.species = SeaSpecies::GreatWhite; d.roam = 45.0f;
            SeaCreatureDesc e; e.species = SeaSpecies::BlueShark;  e.homeX = 60; e.roam = 30.0f;
            cfg.creatures = { d, e };
            cfg.activeRadius = 1000.0f;
            cfg.seed = 0xABCDEFu;
            sea.build(cfg, scene, device, *physics);
            for (int i = 0; i < 900; ++i)
                sea.update(1.0f / 60.0f, scene, device, *physics,
                           x3::phys::Vec3{ 500, 0, 500 }, nullptr);
            for (uint32_t i = 0; i < sea.count(); ++i) {
                const SeaCreature& c = sea.creature(i);
                out.push_back(c.x); out.push_back(c.y); out.push_back(c.z); out.push_back(c.yaw);
            }
        };
        std::vector<float> a, b;
        run(a); run(b);
        bool same = (a.size() == b.size()) && !a.empty();
        for (size_t i = 0; same && i < a.size(); ++i)
            if (std::fabs(a[i] - b[i]) > 1e-5f) same = false;
        scheck(same, "S9 the sim is DETERMINISTIC (identical ticks -> identical state)");
    }

    // ---- S10: the sim allocates NOTHING per frame (build may; update may not).
    {
        Scene scene;
        SeaLifeSystem sea;
        sea.setWaterQuery(testWater);
        sea.setBedQuery(testBed);
        sea.build(makeCfg(SeaSpecies::GreatWhite, 0, 0, 30.0f), scene, device, *physics);
        const uint32_t m0 = device.meshCreates, t0 = device.texCreates;
        for (int i = 0; i < 600; ++i)
            sea.update(1.0f / 60.0f, scene, device, *physics,
                       x3::phys::Vec3{ 0, -6, 0 }, nullptr);
        scheck(device.meshCreates == m0 && device.texCreates == t0,
               "S10 600 frames of update create ZERO meshes/textures (no per-frame alloc)");
    }

    // ---- S11: THE WAKE — foam exists iff a shark rides the surface.
    {
        Scene scene;
        SeaLifeSystem sea;
        sea.setWaterQuery(testWater);
        sea.setBedQuery(testBed);
        sea.build(makeCfg(SeaSpecies::GreatWhite, 0, 0, 30.0f), scene, device, *physics);
        if (sea.count() == 0) {
            scheck(false, "S11 the great white spawned");
        } else {
            // An observer inside activeRadius but DRY and playerless: he patrols,
            // and the wake sim runs (an inactive creature is neither simmed nor drawn).
            const x3::phys::Vec3 obs{ 150.0f, 5.0f, 0.0f };
            // Stage him AT the surface — the fin-shot trick: pin wantDepth.
            SeaCreature& c = sea.creatureMut(0);
            c.holdDepth = true;
            c.wantDepth = 0.55f;
            for (int i = 0; i < 900; ++i)
                sea.update(1.0f / 60.0f, scene, device, *physics, obs, nullptr);
            scheck(sea.finUp(0), "S11 staged shallow: THE FIN cuts the surface");
            scheck(sea.wakeStrength(0) > 0.9f, "S11 the wake gate is FULL at the surface");
            scheck(sea.wakeQuadCount() > 0, "S11 wake foam EXISTS while he rides the surface");
            // Send him deep: the gate closes and the old trail dissipates fully
            // (descent ~4 s at the rise rate + kWakeLife of fade + margin = 13 s).
            c.wantDepth = 5.5f;
            for (int i = 0; i < 780; ++i)
                sea.update(1.0f / 60.0f, scene, device, *physics, obs, nullptr);
            scheck(sea.wakeStrength(0) <= 0.0f, "S11 the wake gate is CLOSED deep");
            scheck(sea.wakeQuadCount() == 0, "S11 a DEEP shark leaves NO surface foam");
        }
        // Negative control: the squid lives in the abyss — never one blob of foam.
        Scene scene2;
        SeaLifeSystem sq;
        sq.setWaterQuery(testWater);
        sq.setBedQuery(testBed);
        sq.build(makeCfg(SeaSpecies::GiantSquid, 0, 0, 20.0f), scene2, device, *physics);
        if (sq.count() == 0) {
            scheck(false, "S11 the squid spawned (negative control)");
        } else {
            bool everFoam = false;
            for (int i = 0; i < 900; ++i) {
                sq.update(1.0f / 60.0f, scene2, device, *physics,
                          x3::phys::Vec3{ 150.0f, 5.0f, 0.0f }, nullptr);
                if (sq.wakeQuadCount() != 0) everFoam = true;
            }
            scheck(!everFoam, "S11 negative control: the deep-water squid NEVER foams");
        }
    }

    x3::logInfo("sealife: " + std::to_string(g_pass) + "/" + std::to_string(g_pass + g_fail)
                + " passed");
    return g_fail == 0;
}

} // namespace x3::game
