// ============================================================================
// labzero_sim.h -- ESCAPE FROM LAB ZERO: headless simulation core (S0)
// C++20, STANDARD LIBRARY ONLY (Sim Purity Rule, LABZERO_PORT_RFC.md).
// ============================================================================
// This is the C# spec, executable. Behavior parity with the verified C# build
// of 2026-07-31 (controls v2, jetpack flight mode, +-90 aim w/ magnetism,
// coyote/buffer feel, AI-lite Biped/Drone, contact damage, score).
//
// Units: px, px/s, px/s^2. Fixed timestep 1/60 s. All timers integer steps.
// Determinism: one seeded mt19937 per sim; no statics, no globals, no clock.
// The host renders FROM this state and never mutates it.
// ============================================================================
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace labzero {

// ============================================================================
// CONSTANTS -- values are the C# GameConstants, verbatim
// ============================================================================
struct K {
    static constexpr float FIXED_TIMESTEP = 1.0f / 60.0f;
    static constexpr int   MAX_PHYSICS_STEPS = 5;

    static constexpr float GRAVITY = 3600.0f;
    static constexpr float TERMINAL_VELOCITY = 900.0f;
    static constexpr float JUMP_STRENGTH = -900.0f;

    static constexpr int   PLAYER_W = 63;
    static constexpr int   PLAYER_H = 88;
    static constexpr int   PLAYER_MAX_HEALTH = 2500;   // yes, 2500 -- spec law
    static constexpr float PLAYER_SPEED = 300.0f;
    static constexpr float RUN_MULT = 1.7f;

    static constexpr int   COYOTE_STEPS = 6;
    static constexpr int   BUFFER_STEPS = 5;
    static constexpr int   SHOOT_COOLDOWN_STEPS = 10;
    static constexpr int   INVULN_STEPS = 45;

    // Aim (controls v2: arrows aim ALWAYS; +-90 with cardinal magnetism)
    static constexpr float AIM_SPEED = 1.8f;                 // rad/s
    static constexpr float AIM_LIMIT = 1.57079632679f;       // pi/2 exactly
    static constexpr float AIM_MAGNET_ZONE = 0.12f;
    static constexpr float AIM_MAGNET_PULL = 0.45f;          // per step
    static constexpr float AIM_EASE_BACK = 0.85f;            // per step

    // Jetpack flight mode (J/Space toggles airborne; W/S fly; hover default)
    static constexpr float JET_THRUST = 5400.0f;
    static constexpr float JET_MAX_RISE = 520.0f;
    static constexpr float JET_FUEL_MAX = 100.0f;
    static constexpr float JET_DRAIN = 40.0f;
    static constexpr float JET_REGEN = 26.0f;
    static constexpr float JET_HOVER_DRAIN = 12.0f;
    static constexpr float JET_DIVE_DRAIN = 6.0f;
    static constexpr float JET_DIVE_ACCEL = 4200.0f;
    static constexpr float JET_MAX_DIVE = 620.0f;
    static constexpr float JET_REARM = 15.0f;
    static constexpr float JET_HOVER_DAMP = 9.0f;            // 1/s toward stillness

    static constexpr float PROJECTILE_SPEED = 800.0f;
    static constexpr int   PROJECTILE_DAMAGE = 10;

    static constexpr int   ENEMY_RESPAWN_STEPS = 90;
    static constexpr float ENEMY_SPEED_SCALE = 60.0f;        // stat -> px/s
};

// ============================================================================
// INPUT -- the host maps GLFW (or anything) into this. Controls v2 semantics:
// jumpHeld is (Space || J) merged UPSTREAM; fireHeld is (F || Numpad0 || Alt).
// ============================================================================
struct LzInput {
    bool left = false, right = false;
    bool jumpHeld = false;          // Space or J (merged by host)
    bool fireHeld = false;          // F / Numpad0 / Alt (merged by host)
    bool arrowAimUp = false;        // Up arrow ONLY (aim, always)
    bool arrowAimDown = false;      // Down arrow ONLY
    bool flyUp = false;             // W ONLY (flight stick / on-foot aim-up)
    bool flyDown = false;           // S ONLY
    bool run = false;               // Shift
};

// ============================================================================
// WORLD PIECES
// ============================================================================
struct LzRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool intersects(const LzRect& o) const {
        return x < o.x + o.w && x + w > o.x && y < o.y + o.h && y + h > o.y;
    }
};

struct Platform { LzRect r; };

enum class EnemyKind : uint8_t { Biped, Drone };

struct Enemy {
    EnemyKind kind = EnemyKind::Biped;
    float x = 0, y = 0;
    float vy = 0;
    int   w = 40, h = 60;
    int   hp = 60;
    float speed = 120.0f;           // px/s
    bool  onGround = false;
    float hoverPhase = 0.0f;
    bool  alive = true;
    int   respawnTimer = 0;
    LzRect bounds() const { return {x, y, (float)w, (float)h}; }
};

struct Projectile {
    float x = 0, y = 0, vx = 0, vy = 0;
    int lifetime = 240;
    bool alive = true;
    LzRect bounds() const { return {x - 4, y - 2, 8, 4}; }
};

// ============================================================================
// PLAYER -- the feel layer, faithful to Player.cs
// ============================================================================
struct Player {
    float x = 0, y = 0;             // y = TOP; feet at y + H
    float vx = 0, vy = 0;
    bool  onGround = false;
    bool  facingRight = true;

    int   health = K::PLAYER_MAX_HEALTH;
    int   invulnSteps = 0;
    int   shootCooldown = 0;

    float aimAngle = 0.0f;          // radians; -pi/2 = straight up

    // jump feel
    int coyote = 0;
    int jumpBuffer = 0;
    bool prevJumpHeld = false;

    // jetpack flight mode
    bool  hasJetpack = false;
    bool  jetActive = false;
    float jetFuel = 0.0f;
    bool  jetThrusting = false;     // producing thrust/hover this step

    float prevY = 0.0f;             // swept platform test

    LzRect bounds() const { return {x, y, (float)K::PLAYER_W, (float)K::PLAYER_H}; }
};

// ============================================================================
// THE SIM
// ============================================================================
class LabZeroSim {
public:
    Player player;
    std::vector<Enemy> enemies;
    std::vector<Projectile> projectiles;
    std::vector<Platform> platforms;

    long  score = 0;
    long  stepCount = 0;
    float floorY = 0;               // the line feet stand on
    float worldW = 0;

    explicit LabZeroSim(float worldWidth = 1280.0f, float worldHeight = 720.0f,
                        uint32_t seed = 0xC0FFEE)
        : rng(seed)
    {
        worldW = worldWidth;
        floorY = std::floor(worldHeight * 0.85f) + K::PLAYER_H;   // RFC SS2.3
        player.x = 100.0f;
        player.y = floorY - K::PLAYER_H;
        player.prevY = player.y;
        player.onGround = true;

        spawnEnemy(EnemyKind::Biped, 700.0f);
        spawnEnemy(EnemyKind::Biped, 950.0f);
        spawnEnemy(EnemyKind::Drone, 820.0f);
    }

    // ---- one fixed step; dt is ALWAYS K::FIXED_TIMESTEP -------------------
    void step(const LzInput& in) {
        const float dt = K::FIXED_TIMESTEP;
        ++stepCount;

        stepTimers();
        stepHorizontal(in, dt);
        stepAiming(in, dt);
        stepJumpAndToggle(in);
        stepJetpack(in, dt);
        stepVertical(dt);
        stepPlatforms();
        stepShooting(in);
        stepEnemies(dt);
        stepProjectiles(dt);
        stepContacts();
    }

    // ---- FNV-1a state hash: the determinism fingerprint -------------------
    uint64_t stateHash() const {
        // FIELD-WISE hashing: raw sizeof() memcpy would include struct PADDING
        // bytes, which are not deterministic across objects. Found by T1.
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const void* p, size_t n) {
            const uint8_t* b = (const uint8_t*)p;
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        };
        auto mf = [&](float f)   { mix(&f, sizeof f); };
        auto mi = [&](int32_t i) { mix(&i, sizeof i); };
        auto mb = [&](bool b2)   { uint8_t v = b2 ? 1 : 0; mix(&v, 1); };

        mf(player.x); mf(player.y); mf(player.vx); mf(player.vy);
        mb(player.onGround); mb(player.facingRight);
        mi(player.health); mi(player.invulnSteps); mi(player.shootCooldown);
        mf(player.aimAngle); mi(player.coyote); mi(player.jumpBuffer);
        mb(player.prevJumpHeld); mb(player.hasJetpack); mb(player.jetActive);
        mf(player.jetFuel); mb(player.jetThrusting);
        for (const auto& e : enemies) {
            mi((int)e.kind); mf(e.x); mf(e.y); mf(e.vy);
            mi(e.hp); mb(e.alive); mi(e.respawnTimer); mf(e.hoverPhase);
        }
        for (const auto& p : projectiles) {
            mf(p.x); mf(p.y); mf(p.vx); mf(p.vy); mi(p.lifetime);
        }
        mi((int32_t)score);
        return h;
    }

private:
    std::mt19937 rng;

    void spawnEnemy(EnemyKind k, float ex) {
        Enemy e; e.kind = k; e.x = ex;
        if (k == EnemyKind::Drone) {
            e.w = 34; e.h = 34; e.hp = 40; e.speed = 180.0f;
            e.y = floorY - 260.0f;
        } else {
            e.w = 40; e.h = 60; e.hp = 60; e.speed = 120.0f;
            e.y = floorY - e.h;
            e.onGround = true;
        }
        e.hoverPhase = std::uniform_real_distribution<float>(0.f, 6.28f)(rng);
        enemies.push_back(e);
    }

    void stepTimers() {
        if (player.shootCooldown > 0) --player.shootCooldown;
        if (player.invulnSteps > 0) --player.invulnSteps;
    }

    void stepHorizontal(const LzInput& in, float dt) {
        float speed = K::PLAYER_SPEED * (in.run ? K::RUN_MULT : 1.0f);
        player.vx = 0.0f;
        if (in.left)  { player.vx = -speed; player.facingRight = false; }
        if (in.right) { player.vx = speed;  player.facingRight = true; }
        player.x += player.vx * dt;
        player.x = std::clamp(player.x, 0.0f, worldW - K::PLAYER_W);
    }

    void stepAiming(const LzInput& in, float dt) {
        // Controls v2 twin-stick law: arrows aim ALWAYS. W/S also aim, but
        // only when flight isn't claiming them. AIMING IS NEVER DISABLED.
        bool flightOwnsWS = player.jetActive && !player.onGround;
        bool up   = in.arrowAimUp   || (!flightOwnsWS && in.flyUp);
        bool down = in.arrowAimDown || (!flightOwnsWS && in.flyDown);

        if (up) {
            player.aimAngle -= K::AIM_SPEED * dt;
            if (player.aimAngle < -(K::AIM_LIMIT - K::AIM_MAGNET_ZONE))
                player.aimAngle += (-K::AIM_LIMIT - player.aimAngle) * K::AIM_MAGNET_PULL;
            if (player.aimAngle < -K::AIM_LIMIT) player.aimAngle = -K::AIM_LIMIT;
            if (K::AIM_LIMIT + player.aimAngle < 0.001f) player.aimAngle = -K::AIM_LIMIT;
        } else if (down) {
            player.aimAngle += K::AIM_SPEED * dt;
            if (player.aimAngle > (K::AIM_LIMIT - K::AIM_MAGNET_ZONE))
                player.aimAngle += (K::AIM_LIMIT - player.aimAngle) * K::AIM_MAGNET_PULL;
            if (player.aimAngle > K::AIM_LIMIT) player.aimAngle = K::AIM_LIMIT;
            if (K::AIM_LIMIT - player.aimAngle < 0.001f) player.aimAngle = K::AIM_LIMIT;
        } else {
            player.aimAngle *= K::AIM_EASE_BACK;
            if (std::fabs(player.aimAngle) < 0.01f) player.aimAngle = 0.0f;
        }
    }

    void stepJumpAndToggle(const LzInput& in) {
        // Coyote: refreshed on ground, decays airborne (order matters -- this
        // is BEFORE the press is read, matching Player.cs exactly)
        if (player.onGround) player.coyote = K::COYOTE_STEPS;
        else if (player.coyote > 0) --player.coyote;

        bool edge = in.jumpHeld && !player.prevJumpHeld;
        player.prevJumpHeld = in.jumpHeld;

        // Jetpack toggle intercepts the AIRBORNE jump edge (grounded always jumps)
        if (edge && !player.onGround && player.hasJetpack &&
            (player.jetActive || player.jetFuel >= K::JET_REARM)) {
            player.jetActive = !player.jetActive;
            player.jumpBuffer = 0;
            return;
        }

        if (edge) player.jumpBuffer = K::BUFFER_STEPS;
        else if (player.jumpBuffer > 0) --player.jumpBuffer;

        if (player.jumpBuffer <= 0) return;
        if (player.coyote > 0) {
            player.vy = K::JUMP_STRENGTH;
            player.onGround = false;
            player.coyote = 0;
            player.jumpBuffer = 0;
        }
    }

    void stepJetpack(const LzInput& in, float dt) {
        player.jetThrusting = false;
        if (!player.hasJetpack) return;

        if (player.jetActive && player.jetFuel <= 0.0f)
            player.jetActive = false;                 // tank dry: fall

        if (player.jetActive) {
            if (player.onGround) {
                if (in.flyUp && player.jetFuel > 0.0f) {
                    player.vy = -K::JET_MAX_RISE * 0.6f;   // takeoff
                    player.onGround = false;
                    player.jetThrusting = true;
                }
            } else {
                player.jetThrusting = true;
                if (in.flyUp) {
                    player.vy -= K::JET_THRUST * dt;
                    player.vy = std::max(player.vy, -K::JET_MAX_RISE);
                    player.jetFuel -= K::JET_DRAIN * dt;
                } else if (in.flyDown) {
                    player.vy += K::JET_DIVE_ACCEL * dt;
                    player.vy = std::min(player.vy, K::JET_MAX_DIVE);
                    player.jetFuel -= K::JET_DIVE_DRAIN * dt;
                } else {
                    // HOVER: damp toward stillness FIRST, then pre-cancel the
                    // gravity stepVertical will add. The reverse order damps
                    // the canceled velocity and re-adds gravity un-damped:
                    // equilibrium +60 px/s -- a permanent sink. Found by T17;
                    // the same bug existed in the C# build and is now fixed there.
                    player.vy += (0.0f - player.vy) * std::min(1.0f, K::JET_HOVER_DAMP * dt);
                    player.vy -= K::GRAVITY * dt;
                    player.jetFuel -= K::JET_HOVER_DRAIN * dt;
                }
                if (player.jetFuel < 0.0f) player.jetFuel = 0.0f;
            }
        }

        if (player.onGround && !player.jetThrusting && player.jetFuel < K::JET_FUEL_MAX)
            player.jetFuel = std::min(K::JET_FUEL_MAX, player.jetFuel + K::JET_REGEN * dt);
    }

    void stepVertical(float dt) {
        player.prevY = player.y;
        if (!player.onGround) {
            player.vy += K::GRAVITY * dt;             // semi-implicit Euler
            player.vy = std::min(player.vy, K::TERMINAL_VELOCITY);
        }
        player.y += player.vy * dt;

        float groundY = floorY - K::PLAYER_H;
        if (player.y >= groundY) {
            player.y = groundY;
            player.vy = 0.0f;
            player.onGround = true;
        } else if (player.onGround && !hasSupport()) {
            // Grounded flag persists ONLY while something holds the feet:
            // the floor line or a platform top. (The naive floor-only check
            // oscillated on platforms -- found by T3.)
            player.onGround = false;
        }
        if (player.y < 0.0f) { player.y = 0.0f; if (player.vy < 0) player.vy = 0.0f; }
    }

    void stepPlatforms() {
        // Swept landing, matching the engine: previous-bottom vs platform top
        for (const auto& p : platforms) {
            LzRect pb = player.bounds();
            if (!pb.intersects(p.r)) continue;
            float prevBottom = player.prevY + K::PLAYER_H;
            if (player.vy > 0 && prevBottom <= p.r.y + 0.001f) {
                player.y = p.r.y - K::PLAYER_H;
                player.vy = 0.0f;
                player.onGround = true;
            } else if (player.vy < 0 && player.prevY >= p.r.y + p.r.h - 0.001f) {
                player.vy = 0.0f;
            }
        }
    }

    bool hasSupport() const {
        float feet = player.y + K::PLAYER_H;
        if (std::fabs(feet - floorY) < 0.6f) return true;
        for (const auto& p : platforms)
            if (std::fabs(feet - p.r.y) < 0.6f &&
                player.x + K::PLAYER_W > p.r.x && player.x < p.r.x + p.r.w)
                return true;
        return false;
    }

    void stepShooting(const LzInput& in) {
        if (!in.fireHeld || player.shootCooldown > 0) return;
        player.shootCooldown = K::SHOOT_COOLDOWN_STEPS;

        float dir = player.facingRight ? 1.0f : -1.0f;
        Projectile pr;
        pr.x = player.facingRight ? player.x + K::PLAYER_W : player.x;
        pr.y = player.y + K::PLAYER_H * 0.5f;
        pr.vx = std::cos(player.aimAngle) * K::PROJECTILE_SPEED * dir;
        pr.vy = std::sin(player.aimAngle) * K::PROJECTILE_SPEED;
        projectiles.push_back(pr);
    }

    void stepEnemies(float dt) {
        for (auto& e : enemies) {
            if (!e.alive) {
                if (--e.respawnTimer <= 0) {
                    float rx = std::uniform_real_distribution<float>(200.f, worldW - 100.f)(rng);
                    EnemyKind k = (std::uniform_int_distribution<int>(0, 1)(rng) == 0)
                                  ? EnemyKind::Biped : EnemyKind::Drone;
                    Enemy fresh; fresh.kind = k; fresh.x = rx;
                    if (k == EnemyKind::Drone) { fresh.w=34; fresh.h=34; fresh.hp=40;
                        fresh.speed=180.f; fresh.y=floorY-260.f; }
                    else { fresh.w=40; fresh.h=60; fresh.hp=60; fresh.speed=120.f;
                        fresh.y=floorY-fresh.h; fresh.onGround=true; }
                    fresh.hoverPhase = std::uniform_real_distribution<float>(0.f,6.28f)(rng);
                    e = fresh;
                }
                continue;
            }
            float dx = (player.x + K::PLAYER_W*0.5f) - (e.x + e.w*0.5f);
            if (e.kind == EnemyKind::Drone) {
                e.hoverPhase += dt * 3.2f;
                float dy = (player.y + K::PLAYER_H*0.5f) - (e.y + e.h*0.5f);
                e.x += (dx > 0 ? 1.f : -1.f) * e.speed * dt;
                e.y += (dy > 0 ? 1.f : -1.f) * e.speed * 0.6f * dt
                     + std::sin(e.hoverPhase) * 22.0f * dt;
            } else {
                e.x += (dx > 0 ? 1.f : -1.f) * e.speed * dt;
                e.vy += K::GRAVITY * dt;
                e.vy = std::min(e.vy, K::TERMINAL_VELOCITY);
                e.y += e.vy * dt;
                float gy = floorY - e.h;
                if (e.y >= gy) { e.y = gy; e.vy = 0.f; e.onGround = true; }
            }
            e.x = std::clamp(e.x, 0.0f, worldW - e.w);
        }
    }

    void stepProjectiles(float dt) {
        for (auto& pr : projectiles) {
            if (!pr.alive) continue;
            pr.x += pr.vx * dt;
            pr.y += pr.vy * dt;
            if (--pr.lifetime <= 0 || pr.x < -50 || pr.x > worldW + 50 ||
                pr.y < -50 || pr.y > floorY + 50)
                pr.alive = false;
        }
        for (auto& pr : projectiles) {
            if (!pr.alive) continue;
            for (auto& e : enemies) {
                if (!e.alive) continue;
                if (pr.bounds().intersects(e.bounds())) {
                    e.hp -= K::PROJECTILE_DAMAGE;
                    pr.alive = false;
                    if (e.hp <= 0) {
                        e.alive = false;
                        e.respawnTimer = K::ENEMY_RESPAWN_STEPS;
                        score += 100;
                    }
                    break;
                }
            }
        }
        projectiles.erase(std::remove_if(projectiles.begin(), projectiles.end(),
            [](const Projectile& p){ return !p.alive; }), projectiles.end());
    }

    void stepContacts() {
        if (player.invulnSteps > 0) return;
        for (const auto& e : enemies) {
            if (!e.alive) continue;
            if (player.bounds().intersects(e.bounds())) {
                player.health = std::max(0, player.health - 10);
                player.invulnSteps = K::INVULN_STEPS;
                break;                                 // exactly one hit
            }
        }
    }
};

} // namespace labzero
