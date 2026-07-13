// THE WATER ZAP — see app/waterzap.h.
#include "waterzap.h"

#include "headless_device.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::game {

WaterZapEntry findWaterEntry(const x3::phys::Vec3& origin, const x3::phys::Vec3& dir,
                             float range, const WaterZapQueryFn& water, float step) {
    WaterZapEntry e{};
    if (!water || range <= 0.0f) return e;
    if (step < 0.05f) step = 0.05f;

    // Already IN the water? Then the pool the shooter floats in is the pool that
    // goes live — the entry is the surface directly over him.
    const float w0 = water(origin.x, origin.z);
    if (w0 > kFishDryTest && origin.y <= w0) {
        e.hit = true; e.fromInWater = true;
        e.x = origin.x; e.z = origin.z; e.surfaceY = w0; e.y = w0;
        return e;
    }

    // March the ray until it is at/below a water surface.
    const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl < 1e-6f) return e;
    const float ux = dir.x / dl, uy = dir.y / dl, uz = dir.z / dl;
    float prevX = origin.x, prevY = origin.y, prevZ = origin.z;
    for (float t = step; t <= range + 1e-4f; t += step) {
        const float px = origin.x + ux * t;
        const float py = origin.y + uy * t;
        const float pz = origin.z + uz * t;
        const float w = water(px, pz);
        if (w > kFishDryTest && py <= w) {
            // Refine the crossing between prev (above/dry) and p (in water) so the
            // arcs spider out from where the bolt actually punched the surface.
            float ax = prevX, ay = prevY, az = prevZ;   // last known "not in water"
            float bx = px, by = py, bz = pz;
            for (int k = 0; k < 8; ++k) {
                const float mx = 0.5f * (ax + bx), my = 0.5f * (ay + by), mz = 0.5f * (az + bz);
                const float mw = water(mx, mz);
                if (mw > kFishDryTest && my <= mw) { bx = mx; by = my; bz = mz; }
                else                               { ax = mx; ay = my; az = mz; }
            }
            const float surf = water(bx, bz);
            e.hit = true;
            e.x = bx; e.z = bz;
            e.surfaceY = (surf > kFishDryTest) ? surf : by;
            e.y = e.surfaceY;
            return e;
        }
        prevX = px; prevY = py; prevZ = pz;
    }
    return e;
}

bool inZappedWater(const x3::phys::Vec3& feet, float cx, float cz, float radius,
                   const WaterZapQueryFn& water) {
    if (!water) return false;
    const float w = water(feet.x, feet.z);
    if (w < kFishDryTest) return false;          // dry ground
    if (feet.y >= w - 0.05f) return false;       // standing ON the bank, not IN it
    const float dx = feet.x - cx, dz = feet.z - cz;
    return (dx * dx + dz * dz) <= radius * radius;
}

int zapPlayer(Player& player, const x3::phys::Vec3& feet, float cx, float cz,
              const WaterZapQueryFn& water, float radius) {
    if (!player.isAlive()) return 0;
    if (!inZappedWater(feet, cx, cz, radius, water)) return 0;
    // TIM'S NUMBER: HALF OF MAX HEALTH. Once (the WaterZapper latch owns "once").
    const int dmg = player.maxHp() / 2;
    if (!player.takeDamage(dmg)) return 0;       // iframe / god swallowed it
    x3::logInfo("waterzap: the player is IN the water — " + std::to_string(dmg)
                + " damage (half of " + std::to_string(player.maxHp()) + ")");
    return dmg;
}

uint32_t zapMonsters(MonsterManager& mm, Scene& scene, x3::phys::IPhysicsWorld& physics,
                     float cx, float cz, const WaterZapQueryFn& water,
                     float radius, int damage) {
    uint32_t hit = 0;
    for (uint32_t i = 0; i < mm.count(); ++i) {
        MonsterSystem& m = mm.at(i);
        if (!m.alive()) continue;
        if (!inZappedWater(m.pos(), cx, cz, radius, water)) continue;
        m.takeMeleeDamage(damage, scene, physics, x3::DamageType::Energy);
        ++hit;
    }
    return hit;
}

// ===========================================================================
// Headless self-test (--test-waterzap)
// ===========================================================================
namespace {

int t_pass = 0, t_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++t_pass; x3::logInfo(std::string("[waterzap-test] PASS ") + name); }
    else      { ++t_fail; x3::logError(std::string("[waterzap-test] FAIL ") + name); }
}

class CountingDevice final : public HeadlessRenderDevice {
public:
    uint32_t meshCreates = 0;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                   const uint32_t* idx, uint32_t ni) override {
        ++meshCreates;
        return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
    }
};

// Synthetic water: a straight CHANNEL along +X (|z| <= 20), surface at Y = 0,
// bed at Y = -4. Everything else is dry land at Y = 1 (the bank).
constexpr float kTestSurface = 0.0f;
constexpr float kTestBed     = -4.0f;
float testWater(float, float z) {
    return (std::fabs(z) <= 20.0f) ? kTestSurface : (kFishDryTest * 2.0f);
}
float testBed(float, float z) {
    return (std::fabs(z) <= 20.0f) ? kTestBed : 1.0f;
}

FishConfig testFishConfig() {
    FishConfig fc;
    fc.activeRadius = 400.0f;   // the whole test channel is live
    FishSchoolDesc a;   // school A: at the origin (INSIDE the zap radius)
    a.centerX = 0.0f; a.centerZ = 0.0f; a.count = 10; a.spread = 3.0f;
    a.heading = 0.0f; a.speed = 0.6f;
    FishSchoolDesc b = a;   // school B: 60 m downstream (WELL OUTSIDE the radius)
    b.centerX = 60.0f; b.count = 8;
    fc.schools = { a, b };
    return fc;
}

} // namespace

bool runWaterZapSelfTest() {
    t_pass = t_fail = 0;
    const float dt = 1.0f / 60.0f;
    WaterZapQueryFn water = [](float x, float z) { return testWater(x, z); };

    // ---- Z1: a shot from the BANK into the channel meets the water -----------
    {
        const x3::phys::Vec3 eye{ 0.0f, 3.0f, -30.0f };     // on the bank, above it
        const x3::phys::Vec3 dir{ 0.0f, -0.45f, 1.0f };     // angled down into the channel
        const WaterZapEntry e = findWaterEntry(eye, dir, 60.0f, water);
        check(e.hit && !e.fromInWater &&
              std::fabs(e.surfaceY - kTestSurface) < 1e-3f &&
              std::fabs(e.y - kTestSurface) < 1e-3f &&
              std::fabs(e.z) <= 20.0f,
              "Z1 shot from the bank MEETS the water (entry on the surface)");
    }

    // ---- Z2: a SUBMERGED shooter zaps the pool he is floating in -------------
    {
        const x3::phys::Vec3 eye{ 5.0f, -0.6f, 2.0f };      // under the surface
        const x3::phys::Vec3 dir{ 1.0f, 0.0f, 0.0f };
        const WaterZapEntry e = findWaterEntry(eye, dir, 30.0f, water);
        check(e.hit && e.fromInWater &&
              std::fabs(e.x - 5.0f) < 1e-3f && std::fabs(e.z - 2.0f) < 1e-3f &&
              std::fabs(e.surfaceY - kTestSurface) < 1e-3f,
              "Z2 fired from IN the water -> the pool he floats in goes live");
    }

    // ---- Z3: a shot that never meets water produces NO zap -------------------
    {
        const x3::phys::Vec3 eye{ 0.0f, 3.0f, -30.0f };
        const x3::phys::Vec3 up{ 0.0f, 1.0f, 0.2f };        // fired at the sky
        const WaterZapEntry a = findWaterEntry(eye, up, 60.0f, water);
        // ...and one fired along the bank, parallel to the channel but never over it.
        const x3::phys::Vec3 along{ 1.0f, -0.1f, 0.0f };
        const WaterZapEntry b = findWaterEntry(eye, along, 30.0f, water);
        check(!a.hit && !b.hit, "Z3 a shot that never meets water -> NO zap");
    }

    // ---- Z4: fish INSIDE the radius die, fish OUTSIDE survive ----------------
    {
        CountingDevice device;
        Scene scene;
        FishSystem fish;
        fish.setWaterQuery(water);
        fish.setBedQuery([](float x, float z) { return testBed(x, z); });
        const FishConfig fc = testFishConfig();
        fish.build(fc, scene, device);
        const uint32_t total = fish.fishCount();
        // Settle the schools for 3 s with the player far away (no flee).
        const x3::phys::Vec3 far{ 0.0f, 0.0f, 300.0f };
        for (int i = 0; i < 180; ++i) fish.update(dt, scene, far);

        uint32_t inR = 0, outR = 0;
        for (uint32_t i = 0; i < total; ++i) {
            const Fish& f = fish.fish(i);
            const float dx = f.x, dz = f.z;
            if (std::sqrt(dx * dx + dz * dz) <= kWaterZapRadius) ++inR; else ++outR;
        }
        const uint32_t killed = fish.killWithin(0.0f, 0.0f, kWaterZapRadius);
        bool allInDead = true, allOutAlive = true;
        for (uint32_t i = 0; i < total; ++i) {
            const Fish& f = fish.fish(i);
            const float d = std::sqrt(f.x * f.x + f.z * f.z);
            if (d <= kWaterZapRadius && !f.dead) allInDead = false;
            if (d > kWaterZapRadius && f.dead)   allOutAlive = false;
        }
        check(total == 18 && device.meshCreates == 3 && inR >= 8 && outR >= 8 &&
              killed == inR && allInDead && allOutAlive &&
              fish.aliveCount() == outR,
              "Z4 fish INSIDE the radius die, fish OUTSIDE survive (3 shared lofted meshes)");

        // ---- Z8a: the dead float BELLY-UP to the surface, then despawn -------
        for (int i = 0; i < 240; ++i) fish.update(dt, scene, far);   // 4 s
        bool risen = true;
        for (uint32_t i = 0; i < total; ++i) {
            const Fish& f = fish.fish(i);
            if (f.dead && f.y < kTestSurface - 0.05f) risen = false;   // still deep
        }
        const uint32_t floatingNow = fish.deadCount();
        // Run past deadLinger: every corpse despawns (hidden), none linger.
        for (int i = 0; i < (int)((fc.deadLinger + 1.0f) * 60.0f); ++i)
            fish.update(dt, scene, far);
        check(risen && floatingNow == killed && fish.deadCount() == 0 &&
              device.meshCreates == 3,
              "Z8a dead fish float belly-up to the surface, then despawn (no leak)");
    }

    // ---- Z5/Z6/Z7: THE PLAYER'S OWN PUNISHMENT ------------------------------
    {
        Player player;
        const int maxHp = player.maxHp();
        WaterZapper zap;
        // He is SWIMMING in the channel at the entry point.
        const x3::phys::Vec3 swimFeet{ 2.0f, kTestSurface - 1.4f, 1.0f };
        const float cx = 0.0f, cz = 0.0f;

        // Frame 1 of a HELD trigger: the zap lands.
        int dmg1 = 0;
        if (zap.canZap()) { dmg1 = zapPlayer(player, swimFeet, cx, cz, water); zap.noteZap(); }
        const int hpAfter1 = player.hp();

        // 60 more frames of the SAME held trigger: the latch refuses every one.
        for (int i = 0; i < 60; ++i) {
            zap.tick(dt);
            player.updateHealth(dt);
            if (zap.canZap()) { zapPlayer(player, swimFeet, cx, cz, water); zap.noteZap(); }
        }
        check(dmg1 == maxHp / 2 && hpAfter1 == maxHp - maxHp / 2 &&
              player.hp() == maxHp - maxHp / 2 && zap.zapCount() == 1,
              "Z5 player IN the water loses EXACTLY half max health, ONCE (held trigger does not drain)");

        // Release + let the cooldown expire -> a SECOND pull zaps again.
        zap.triggerReleased();
        for (int i = 0; i < (int)(kWaterZapCooldown * 60.0f) + 4; ++i) {
            zap.tick(dt); player.updateHealth(dt);
        }
        int dmg2 = 0;
        if (zap.canZap()) { dmg2 = zapPlayer(player, swimFeet, cx, cz, water); zap.noteZap(); }
        check(dmg2 == maxHp / 2 && zap.zapCount() == 2 && player.hp() == 0 && player.dead(),
              "Z6 release + cooldown -> the second zap lands (and half again kills him)");

        // Z7: a player on the BANK takes ZERO (feet above the water / out of it).
        Player dry;
        const x3::phys::Vec3 bankFeet{ 2.0f, 1.0f, -25.0f };   // on the bank, dry ground
        const int dmg3 = zapPlayer(dry, bankFeet, cx, cz, water);
        // ...and one standing in the channel but 40 m from the entry: out of radius.
        const x3::phys::Vec3 farWet{ 40.0f, kTestSurface - 1.4f, 0.0f };
        const int dmg4 = zapPlayer(dry, farWet, cx, cz, water);
        check(dmg3 == 0 && dmg4 == 0 && dry.hp() == dry.maxHp(),
              "Z7 a player on the BANK (or outside the radius) takes ZERO");
    }

    // ---- Z8b: the fish sim is DETERMINISTIC --------------------------------
    {
        CountingDevice d1, d2;
        Scene s1, s2;
        FishSystem f1, f2;
        for (FishSystem* f : { &f1, &f2 }) {
            f->setWaterQuery(water);
            f->setBedQuery([](float x, float z) { return testBed(x, z); });
        }
        f1.build(testFishConfig(), s1, d1);
        f2.build(testFishConfig(), s2, d2);
        const x3::phys::Vec3 player{ 4.0f, -1.0f, 0.0f };   // in the water, spooking them
        for (int i = 0; i < 600; ++i) {   // 10 s incl. flee bursts
            f1.update(dt, s1, player);
            f2.update(dt, s2, player);
        }
        bool same = f1.fishCount() == f2.fishCount();
        float worst = 0.0f;
        for (uint32_t i = 0; same && i < f1.fishCount(); ++i) {
            const Fish& a = f1.fish(i);
            const Fish& b = f2.fish(i);
            worst = std::max(worst, std::fabs(a.x - b.x));
            worst = std::max(worst, std::fabs(a.y - b.y));
            worst = std::max(worst, std::fabs(a.z - b.z));
        }
        // The player is in the school: it must have SCATTERED (someone fled).
        bool scattered = false;
        for (uint32_t i = 0; i < f1.fishCount(); ++i) {
            const Fish& f = f1.fish(i);
            const float dx = f.x - player.x, dz = f.z - player.z;
            if (std::sqrt(dx * dx + dz * dz) > f1.config().fleeRadius) scattered = true;
        }
        check(same && worst == 0.0f && scattered,
              "Z8b the fish school sim is DETERMINISTIC (and the player parts the school)");
    }

    // ---- Z9: THE SPECIES TABLE — shoals shoal, and the PIKE IS ALONE --------
    // Also the GRACEFUL-FALLBACK contract: this test never calls setModelDir(), so
    // no GLB is reachable and EVERY fish must degrade to the procedural loft (3
    // shared meshes, 3 entities each) rather than crash or render nothing.
    {
        CountingDevice device;
        Scene scene;
        FishSystem fish;
        fish.setWaterQuery(water);
        fish.setBedQuery([](float x, float z) { return testBed(x, z); });

        FishConfig fc;
        fc.activeRadius = 400.0f;
        FishSchoolDesc shoal;                       // a proper rudd shoal
        shoal.centerX = 0.0f; shoal.centerZ = 0.0f;
        shoal.species = FishSpecies::Rudd;
        shoal.count = 12; shoal.spread = 3.0f;
        shoal.speed = fishSpecies(FishSpecies::Rudd).speed;
        FishSchoolDesc gang = shoal;                // a small loose perch gang
        gang.centerX = 30.0f;
        gang.species = FishSpecies::Perch;
        gang.count = 5; gang.spread = 6.0f;
        gang.speed = fishSpecies(FishSpecies::Perch).speed;
        FishSchoolDesc loner = shoal;               // THE PIKE. One.
        loner.centerX = 60.0f;
        loner.species = FishSpecies::Pike;
        loner.count = 1; loner.spread = 2.0f;
        loner.speed = fishSpecies(FishSpecies::Pike).speed;
        fc.schools = { shoal, gang, loner };
        fish.build(fc, scene, device);

        // The species landed on the right fish.
        uint32_t nRudd = 0, nPerch = 0, nPike = 0;
        for (uint32_t i = 0; i < fish.fishCount(); ++i) {
            switch (fish.fish(i).species) {
                case FishSpecies::Rudd:  ++nRudd;  break;
                case FishSpecies::Perch: ++nPerch; break;
                case FishSpecies::Pike:  ++nPike;  break;
                default: break;
            }
        }
        // A PREDATOR DOES NOT SHOAL: exactly one pike, its school flagged solitary.
        bool pikeAlone = (nPike == 1);
        for (uint32_t i = 0; i < fish.schoolCount(); ++i) {
            const FishSchool& sc = fish.school(i);
            if ((sc.species == FishSpecies::Pike) != sc.solitary) pikeAlone = false;
        }
        // ...and it is the BIG one: the pike's authored length dwarfs the shoal's.
        const bool pikeBigger =
            fishSpecies(FishSpecies::Pike).size >
            3.0f * fishSpecies(FishSpecies::Rudd).size * 0.5f &&
            fishSpecies(FishSpecies::Pike).speed <
            fishSpecies(FishSpecies::Rudd).speed;

        // THE FALLBACK CONTRACT: no model dir => every fish is the loft, and the
        // loft is still exactly 3 shared meshes + 1 skin for the whole world.
        const bool fellBack = (fish.glbFishCount() == 0) &&
                              !fish.speciesLoaded(FishSpecies::Pike) &&
                              (device.meshCreates == 3) &&
                              (fish.drawCount() == 3 * fish.fishCount());

        // And it still SWIMS and DIES: tick it, zap the shoal, tick it again.
        const x3::phys::Vec3 away{ 500.0f, 0.0f, 500.0f };   // nobody spooks them
        for (int i = 0; i < 120; ++i) fish.update(dt, scene, away);
        const uint32_t killed = fish.killWithin(0.0f, 0.0f, kWaterZapRadius);
        for (int i = 0; i < 120; ++i) fish.update(dt, scene, away);
        const bool zapWorks = (killed == nRudd) && (fish.deadCount() == killed) &&
                              (nPike == 1 && !fish.fish(fish.fishCount() - 1).dead);

        check(nRudd == 12 && nPerch == 5 && pikeAlone && pikeBigger && fellBack &&
              zapWorks,
              "Z9 species table: rudd/bream SHOAL, perch GANG, the PIKE IS ALONE "
              "(and a missing GLB degrades to the procedural loft, never a statue)");
    }

    x3::logInfo("waterzap: " + std::to_string(t_pass) + "/" +
                std::to_string(t_pass + t_fail) + " passed");
    return t_fail == 0;
}

} // namespace x3::game
