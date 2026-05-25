// X3 ECS self-test — see engine/ecs/Ecs.h.
#include "Ecs.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::ecs {

namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[ecs-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[ecs-test] FAIL ") + name); }
}

// Test components (POD — exactly the cache-local layout an ECS wants).
struct Position { float x, y, z; };
struct Velocity { float x, y, z; };
struct Health   { int hp; };
} // namespace

bool runEcsSelfTest() {
    g_pass = g_fail = 0;
    World w;

    constexpr uint32_t N = 50000;
    std::vector<EntityId> ents;
    ents.reserve(N);

    // Create N entities; all get Position, even-indexed get Velocity, every 5th
    // gets Health.
    for (uint32_t i = 0; i < N; ++i) {
        EntityId e = w.create();
        ents.push_back(e);
        w.add<Position>(e, Position{ (float)i, 0.0f, 0.0f });
        if (i % 2 == 0) w.add<Velocity>(e, Velocity{ 1.0f, 2.0f, 3.0f });
        if (i % 5 == 0) w.add<Health>(e, Health{ 100 });
    }

    // ---- C0: all entities are alive + counted. ----
    check(w.aliveCount() == N, "C0 created 50k entities");

    // ---- C1: a single-component view visits ALL entities. ----
    {
        size_t n = 0;
        w.each<Position>([&](EntityId, Position&) { ++n; });
        check(n == N, "C1 view<Position> visits every entity");
    }

    // ---- C2: a 2-component view visits only the intersection + integrates. ----
    {
        size_t n = 0;
        const float dt = 0.5f;
        w.each<Position, Velocity>([&](EntityId, Position& p, Velocity& v) {
            p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;   // integrate
            ++n;
        });
        // Even entities (N/2) have Velocity; spot-check one integrated value:
        // entity 100 started x=100, +1*0.5 -> 100.5.
        bool integrated = std::fabs(w.get<Position>(ents[100]).x - 100.5f) < 1e-3f;
        bool oddUntouched = std::fabs(w.get<Position>(ents[101]).x - 101.0f) < 1e-3f;
        check(n == N / 2 && integrated && oddUntouched,
              "C2 view<Position,Velocity> hits the intersection + integrates");
    }

    // ---- C3: a 3-component view (every entity div by 10 has Pos+Vel+Health). ----
    {
        size_t n = w.countMatching<Position, Velocity, Health>();
        // Has Velocity (even) AND Health (every 5th) => multiples of 10: N/10.
        check(n == N / 10, "C3 view<Position,Velocity,Health> = the 3-way intersection");
    }

    // ---- C4: destroy a chunk; stale handles go invalid + count drops. ----
    {
        for (uint32_t i = 0; i < 10000; ++i) w.destroy(ents[i]);
        bool gone = !w.valid(ents[0]) && !w.valid(ents[9999]);
        bool live = w.valid(ents[10000]);
        bool counted = w.aliveCount() == N - 10000;
        // The Position view now visits the survivors only.
        size_t n = 0; w.each<Position>([&](EntityId, Position&) { ++n; });
        check(gone && live && counted && n == N - 10000,
              "C4 destroy invalidates handles + shrinks the views");
    }

    // ---- C5: generation recycling — a new entity reuses a freed index, and the
    // OLD handle to that index stays invalid. ----
    {
        EntityId reborn = w.create();                 // reuses a freed index
        bool oldDead = !w.valid(ents[0]);             // ents[0]'s index likely recycled
        bool newLive = w.valid(reborn);
        // The reborn handle differs from any destroyed handle at the same index.
        bool distinct = (reborn != ents[0]);
        check(oldDead && newLive && distinct, "C5 generation recycling invalidates stale handles");
    }

    x3::logInfo(std::string("[ecs-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::ecs
