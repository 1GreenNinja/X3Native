#pragma once
// SEALIFE — THE OCEAN LIVES. "Sharks are good!" -> "Put the rest in the ocean!" (Tim)
//
// The BIG animals of the sea, kept deliberately separate from app/fish.h:
//     fish.h    = SHOALS   — dozens of small fish, procedural mesh, boids, belly-up.
//     sealife.h = ANIMALS  — a handful of large, RIGGED, skinned creatures that
//                            hunt you, or ignore you, or simply loom.
// Bloating fish.cpp with a predator state machine would have wrecked both.
//
// THE CAST (what actually shipped, and why — see the asset audit at the bottom):
//   * GREAT WHITE  — THE HUNTER. The point of the whole feature. Patrols the
//       estuary mouth and the shallows the player swims out into, DETECTS him in
//       the water, STALKS (circles, closing — this is the dread), CHARGES, BITES
//       for 40 (two or three bites kill), VEERS OFF, and comes back around.
//       THE TELL: his dorsal fin CUTS THE SURFACE when he is shallow — visible from
//       the bank and while swimming — and it drags a FOAM WAKE: the spreading V +
//       fin churn (see THE WAKE below; unblocked by the 24371e2 water-depth fix).
//   * BLUE SHARK   — the deep-water second predator: same kit, longer stalk, rarer
//       commit, less damage. Deeper, so you meet him when you go down.
//   * GIANT SQUID  — THE ABYSS. Down at the undersea base in the dark, huge, slow,
//       drifting, arms trailing. It does not hunt; it does not need to. It only has
//       to be there when your torch finds it. A faint bioluminescent tint keeps it
//       readable in black water. Contact hurts (a slow crush), but that is a detail.
//
// EVERYTHING DIES TO THE ZAP (app/waterzap.h). The lightning gun electrifies the
// water: every creature inside kWaterZapRadius that is within kSeaZapDepth of the
// SURFACE takes kSeaZapDamage of Energy — a shark dies outright — and the PLAYER
// pays HALF HIS MAX HEALTH for it. The shark hunts you; you fry the water; it costs
// you half your life. That loop is the best thing in this feature.
//   The DEPTH CAP is deliberate and load-bearing: electrification is a SURFACE
//   phenomenon, so it cannot reach the squid at -60 m. You cannot cheese the abyss
//   with the zap. Swim down there and deal with it.
//
// SIMULACRA, like the fish and the crowds: kinematic, no physics bodies (noBody),
// range-gated, dt-scaled, zero per-frame allocation. Each creature is hosted by an
// INERT MonsterSystem (chaseSpeed 0, damage 0, noBody) purely to own the skinned-GLB
// lifetime + the joint palette — the exact trick app/crowd_skin.cpp uses. THIS system
// owns all motion and all damage; the MonsterSystem AI never runs.
//
// THE RIGS (tools/sealife_bake.py): each creature is a Rodin sculpt decimated to a
// game budget with its FINS AND TENTACLES PROTECTED (a naive decimator eats thin flat
// surfaces first — i.e. the entire silhouette), re-skinned onto a 6-bone spine, and
// baked with seamless sin-driven loops:
//     "Cruise" — the travelling-sine tail beat (all species)
//     "Charge" — faster, higher amplitude (the sharks' attack run)
// A creature that translates without flexing is a floating prop, so the clip is not
// decoration — it is the difference between an animal and a statue.
//
// ---- ASSET AUDIT (2026-07-12), because three of the five models Tim named are NOT
// what their filenames claim, and shipping them would have been a lie:
//     GreatWhiteSharkGameReady  GOOD  — a true great white (snout, teeth, gills,
//                                       crescent tail). Its PBR set is NOT loose next
//                                       to the OBJ; it is packed inside the .usdz.
//     sea_giant_squid           GOOD  — a real squid: finned mantle + 10 arms.
//     sea_hammerhead            WRONG — NO cephalofoil. Total width is 8% of length;
//                                       a hammer spans ~25%. It is a lean, long-snouted
//                                       shark. Shipped honestly as the BLUE SHARK, which
//                                       fills exactly the gameplay slot Tim asked for
//                                       ("deeper water, less aggressive"). Only the
//                                       species name changed.
//     sea_manta_ray             WRONG — not a ray at all: a shark-ish body with a tall
//                                       dorsal fin and scythe fins. No disc, no wings,
//                                       no whip tail. NOT SHIPPED — a "manta" with a
//                                       dorsal fin reads as a THREAT, which destroys the
//                                       one thing the manta was for (harmless wonder).
//     sea_humpback_whale        WRONG — a BUST. Head + one flipper, closed off at the
//                                       back. No body, no fluke. It cannot swim.
//                                       NOT SHIPPED.
// Nothing better exists anywhere on this machine (D:\Assets ~200 packs, G:\Assets 210
// packs, G:\GameModels: no whale, no manta, no ray, no dolphin). The MANTA and the
// WHALE are therefore BLOCKED ON ART, not on code: SeaSpecies has their slots and the
// behaviours are specified below, so both drop in the day a real model exists.

#include "fish.h"      // FishWaterFn / FishBedFn / kFishDryTest
#include "monster.h"   // MonsterSystem — the inert skinned-GLB prop host
#include "player.h"
#include "scene.h"

#include "engine/physics/IPhysicsWorld.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Tuning the self-test asserts.
// ---------------------------------------------------------------------------
constexpr float kSeaZapDepth  = 10.0f;   // the zap only reaches this far DOWN (m)
constexpr int   kSeaZapDamage = 500;     // Energy per zap — lethal to any shark
constexpr float kSeaFinDepth  = 1.10f;   // dorsal breaks the surface above this depth

// THE WAKE (shipped — take 2). A foam trail behind the surfaced fin was first built
// as a Scene Entity and CUT: the sea surface was GLASS replayed in the DEPTH
// PRE-PASS, so a quad hugging the surface was swallowed, and no scene-entity blend
// path could light it — it rendered as a BLACK SLICK on the night sea. The note
// here used to end "foam wants a real FX/particle path, not a scene Entity", and
// once 24371e2 took the water off the opaque depth range that is EXACTLY what
// shipped: the wake rides the engine's PARTICLE-pass primitives (the CombatFx
// path — ALPHA-blended so it cannot bloom into a white slab, UNLIT-tinted so it
// cannot go black at night, depth-TESTED read-only so banks and terrain occlude
// it, drawn AFTER the water so it sits ON the surface).
//   * THE V     — two arms of foam trailing the fin along his ACTUAL path (a
//                 per-creature ring of surface samples; each remembers where the
//                 fin crossed and which way he was heading). The arms spread at
//                 the Kelvin half-angle (tan 19.47° — the real physics number is
//                 also the cheapest one to pick) and grow/fade as they age.
//                 Drawn as DECALS — oriented quads laid FLAT on the surface plane
//                 (normal +Y), NOT camera-facing billboards: a billboard trail
//                 viewed from a low bank smears VERTICALLY at grazing angles
//                 (foam standing half a metre out of the sea), while a decal
//                 hugs the plane from every angle.
//   * THE CHURN — a handful of froth blobs boiling at the fin cut itself,
//                 jittered deterministically off the sim clock. These ARE
//                 billboards: churned froth genuinely has height at the cut.
//                 Immediate-mode: nothing simulated, nothing allocated.
//   * THE GATE  — full foam while the body's TOP is within kWakeFullTop of the
//                 surface, NOTHING once it is deeper than kWakeZeroTop: a deep
//                 shark leaves no surface mark (self-test S11), and a dead one
//                 stops sampling while his last trail dissipates.
// Budgeted like the god rays: <= ~80 billboards per surfaced shark, ONE
// submitParticles call per frame, zero per-frame allocation (fixed rings + a
// member scratch — S10 still holds).
constexpr int   kWakePoints       = 24;     // path-history ring slots per creature
constexpr float kWakeSampleDist   = 0.9f;   // metres of travel between surface samples
constexpr float kWakeLife         = 6.0f;   // seconds one wake point lives
constexpr float kWakeSpreadTan    = 0.353f; // tan(19.47 deg) — the Kelvin wake half-angle
constexpr float kWakeSpreadMax    = 3.2f;   // arm half-width cap (m)
constexpr float kWakeTopFrac      = 0.11f;  // body TOP above centre, as a fraction of
                                            // length (the 5 m great white's back rides
                                            // ~0.55 m above his origin — the fin-shot math)
constexpr float kWakeFullTop      = 0.5f;   // body top within this of the surface: FULL wake
constexpr float kWakeZeroTop      = 2.0f;   // body top deeper than this: NO wake at all
// Foam rides this high off the surface plane. NOT a taste number: the estuary
// surface still swallows coplanar transparents exactly as the original wake
// post-mortem measured ("lifting it +0.30 m makes it draw") — foam at +0.07 m
// is eaten to nothing while +0.35 m draws clean (bisected empirically 7/20 with
// alpha-1.0 probe decals). 0.35 clears the kill zone; at gameplay distances the
// offset is imperceptible and the trail reads as sitting ON the water.
constexpr float kWakeLift         = 0.35f;
constexpr int   kWakeChurnBlobs   = 7;      // froth blobs boiling at the fin cut
constexpr int   kWakeMaxInstances = 512;    // global per-frame billboard cap (scratch size)

enum class SeaSpecies : uint8_t {
    GreatWhite = 0,
    BlueShark,
    GiantSquid,
    // --- BLOCKED ON ART (see the audit above). Behaviours are specified; the day a
    // real model lands, add the def + a spawn and they work.
    //   MantaRay  — harmless WONDER: glides mid-water on slow wing beats, banks,
    //               passes near the player without ever threatening. 1-2 of them.
    //   Humpback  — harmless AWE: far offshore, huge, slow, surfaces to BLOW
    //               (spout FX + a distant call). A "look at that", not a mechanic.
    Count
};

enum class SeaState : uint8_t {
    Patrol = 0,   // slow cruise circuits of the home ring
    Stalk,        // player detected: circle at range, closing slowly (THE DREAD)
    Charge,       // committed: Charge clip, fast, straight at him
    Bite,         // in contact: damage lands ONCE per pass
    VeerOff,      // peel away, then come back around
    Dead          // zapped: rolls belly-up, floats, despawns
};
const char* seaStateName(SeaState s);

// Per-species constants. All distances in metres, speeds m/s, times seconds.
struct SeaSpeciesDef {
    const char* key;
    const char* rig;          // GLB in assets/rigged_glb (baked by tools/sealife_bake.py)
    float length;             // real size; the GLB is normalized to 1 m, so this IS the scale
    float cruiseSpeed;
    float chargeSpeed;
    float turnRate;           // rad/s
    float detectRadius;       // player in the water inside this -> STALK
    float stalkRadius;        // the circling stand-off distance
    float commitDelay;        // seconds of stalking before he commits to a CHARGE
    float chargeRange;        // will only commit from inside this
    float biteRange;          // contact distance
    int   biteDamage;
    float biteCooldown;       // min seconds between bites landing
    float veerTime;           // seconds spent peeling away after a pass
    float depthMin, depthMax; // preferred depth band BELOW the surface
    int   health;
    bool  predator;           // false => NEVER damages anyone (manta/whale slots)
    bool  surfacer;           // true => cruises shallow enough for THE FIN to cut
};
const SeaSpeciesDef& seaSpeciesDef(SeaSpecies s);

// One authored creature.
struct SeaCreatureDesc {
    SeaSpecies species = SeaSpecies::GreatWhite;
    float homeX = 0.0f, homeZ = 0.0f;   // centre of its patrol ring (must be over water)
    float roam  = 55.0f;                // patrol ring radius (m)
    float heading = 0.0f;               // initial heading (rad)
};

struct SeaConfig {
    std::vector<SeaCreatureDesc> creatures;
    uint32_t roomId       = kNoRoom;    // PVS tag (host stamps kStreamedExteriorRoom)
    float    activeRadius = 300.0f;     // simulated/drawn within this of the player
    uint32_t seed         = 0x5EA1Fu;
};

// One big animal.
struct SeaCreature {
    SeaSpecies species = SeaSpecies::GreatWhite;
    SeaState   state   = SeaState::Patrol;
    float x = 0, y = 0, z = 0;
    float yaw = 0.0f;            // world XZ heading (rad) — model faces -Z at yaw 0
    float pitch = 0.0f;          // dive/climb (rad)
    float speed = 0.0f;
    float homeX = 0, homeZ = 0, roam = 55.0f;
    float patrolAng = 0.0f;      // position around the patrol ring
    float stateT = 0.0f;         // seconds in the current state
    float wantDepth = 2.0f;      // depth below the surface this creature is aiming for
    // STAGING (screenshots / cinematics): pin wantDepth so the patrol's rise-sink
    // sine cannot drag the subject under mid-capture. Never set during gameplay.
    bool  holdDepth = false;
    float depthPhase = 0.0f;     // per-creature phase of the slow rise/sink cycle
    float biteCool = 0.0f;
    // THE BITE LANDS ONCE PER PASS. Not once per frame — a shark parked on the
    // player must not drain him at 165 Hz. Set when the bite lands; cleared only
    // when he VEERS OFF and comes back around for a new run.
    bool  bitThisPass = false;
    int   health = 100;
    bool  dead = false;
    float deadT = 0.0f;
    bool  gone = false;          // corpse despawned
    bool  active = false;        // player within activeRadius
    bool  skinned = false;       // the rigged GLB actually loaded

    // THE WAKE: ring of surface samples the foam trail is drawn from. Each point
    // remembers where the fin crossed, the heading's perpendicular (the V arms
    // spread along it), how fast he was going, and how open the wake gate was.
    struct WakePoint {
        float x = 0, z = 0;         // surface crossing (world)
        float surfY = 0;            // the water surface there
        float perpX = 0, perpZ = 0; // unit perpendicular to the heading at sample time
        float speed = 0;            // his speed at sample time (drives the V spread)
        float strength = 0;         // wake gate at sample time [0,1]
        float age = kWakeLife;      // seconds since sampled (>= kWakeLife == free slot)
    };
    WakePoint wake[kWakePoints];
    int   wakeHead = 0;             // next ring slot to write
    float wakeDist = 0.0f;          // metres travelled since the last sample

    std::unique_ptr<MonsterSystem> sys;   // inert prop: owns the skinned GLB
};

class SeaLifeSystem {
public:
    // Wire the feeds BEFORE build() (build seeds creatures at real depths).
    void setWaterQuery(FishWaterFn fn) { m_water = std::move(fn); }
    void setBedQuery(FishBedFn fn)     { m_bed   = std::move(fn); }
    // Optional: the shoals this creature scatters when it swims through them.
    void setFishSystem(FishSystem* f)  { m_fish = f; }

    // Build every creature. A creature whose home is DRY is skipped (logged) — an
    // animal never spawns on land. A creature whose rig fails to load is skipped
    // (logged) rather than shipped as an invisible damage source.
    void build(const SeaConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics);

    // Advance one frame.
    // `playerFeet` is the player's FEET (not his eye): "in the water" is a question
    // about where he is STANDING/FLOATING, and a wading player's eye is above the
    // surface while he is very much in it. Passed explicitly — like zapPlayer() —
    // so the sim never depends on the Player's physics capsule being spawned.
    // `player` may be null (the screenshot/settle path): then nothing can be bitten,
    // but the creatures still swim.
    void update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                x3::phys::IPhysicsWorld& physics,
                const x3::phys::Vec3& playerFeet, Player* player);

    // The skinned props carry an invalid render mesh, so Scene::render skips them:
    // the host MUST call this (PVS-gated inside).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // THE ZAP. Kill every live creature within `radius` on the water plane AND
    // within kSeaZapDepth of the surface. Returns the number killed.
    uint32_t killWithin(float cx, float cz, float radius);

    // ---- Queries (HUD / screenshots / self-test) ----
    bool built() const { return m_built; }
    const SeaConfig& config() const { return m_cfg; }
    uint32_t count() const { return (uint32_t)m_creatures.size(); }
    const SeaCreature& creature(uint32_t i) const { return m_creatures[i]; }
    SeaCreature& creatureMut(uint32_t i) { return m_creatures[i]; }
    uint32_t aliveCount() const;
    uint32_t activeCount() const;
    // Is this creature's dorsal cutting the surface right now? (THE FIN)
    bool finUp(uint32_t i) const;
    // Index of the nearest live creature of a species, or -1.
    int findSpecies(SeaSpecies s) const;
    // ---- THE WAKE queries (self-test S11 / debug) ----
    // The wake gate for one creature: 1 with the body top at the surface, fading
    // to 0 by kWakeZeroTop down. 0 for dead/gone/dry.
    float wakeStrength(uint32_t i) const;
    // How many foam billboards the wake would submit THIS frame (all creatures /
    // one creature). draw() submits exactly this many.
    uint32_t wakeQuadCount() const;
    uint32_t wakeQuadCount(uint32_t i) const;

private:
    void  spawn(const SeaCreatureDesc& d, Scene& scene, x3::rhi::IRenderDevice& device,
                x3::phys::IPhysicsWorld& physics);
    void  think(SeaCreature& c, float dt, const x3::phys::Vec3& playerFeet,
                bool playerInWater, Player* player);
    void  swim(SeaCreature& c, float dt);
    // THE WAKE: age the trail ring; while the gate is open, drop a fresh surface
    // sample every kWakeSampleDist of travel. Dead creatures only age (the last
    // trail dissipates behind the corpse).
    void  updateWake(SeaCreature& c, float dt);
    // The wake gate [0,1] for one creature (body-top depth against kWakeFullTop /
    // kWakeZeroTop). 0 for dead/gone/dry water.
    float wakeGate(const SeaCreature& c) const;
    // Fill the member scratch with this frame's foam — the trail (V arms +
    // centreline) as surface DECALS, the fin churn as billboard PARTICLES.
    // `only` restricts to one creature index (-1 = all). Returns counts via the
    // out-params; total foam primitives as the return. No allocation.
    uint32_t buildWakeFoam(int only, uint32_t& decalCount, uint32_t& churnCount) const;
    void  writeTransform(SeaCreature& c, Scene& scene);
    float waterAt(float x, float z) const;
    float bedAt(float x, float z, float surface) const;
    uint32_t rng();
    float frand();

    bool        m_built = false;
    SeaConfig   m_cfg{};
    FishWaterFn m_water;
    FishBedFn   m_bed;
    FishSystem* m_fish = nullptr;
    std::vector<SeaCreature> m_creatures;

    // Per-frame foam scratch (member-owned: draw() allocates NOTHING; mutable so
    // the const draw()/wakeQuadCount() can fill it — the CombatFx submit trick).
    // Trail patches are decals ON the surface plane; churn froth is billboards.
    mutable x3::rhi::IRenderDevice::DecalInstance    m_wakeTrailScratch[kWakeMaxInstances];
    mutable x3::rhi::IRenderDevice::ParticleInstance m_wakeChurnScratch[kWakeMaxInstances];

    float    m_time = 0.0f;
    uint32_t m_rngState = 0x5EA1Fu;
};

// Headless self-test (--test-sealife). Asserts:
//   S1  the species table is sane (a shark charges, a non-predator never does).
//   S2  a great white left alone PATROLS and stays inside its roam ring + depth band.
//   S3  a player swimming into range drives PATROL -> STALK (he does not charge on sight).
//   S4  the stalk COMMITS: STALK -> CHARGE -> BITE within a bounded time.
//   S5  THE BITE LANDS ONCE PER PASS — 600 frames of contact deal ONE bite's damage,
//       not 600 (the per-frame drain bug this latch exists to prevent).
//   S6  a player OUT of the water is NEVER attacked (no state leaves Patrol).
//   S7  the ZAP kills sea life inside the radius near the surface; a creature outside
//       the radius survives; a DEEP creature (the squid) survives — the abyss is out
//       of the zap's reach BY DESIGN.
//   S8  harmless species (predator == false) never damage the player.
//   S9  DETERMINISM: two systems, identical ticks -> identical positions/states.
//  S10  no mesh/texture leaks across build/teardown.
//  S11  THE WAKE: foam billboards EXIST while a shark rides the surface; ZERO once
//       he runs deep (the gate closes and the trail dissipates); and the deep-water
//       species (the squid) NEVER foams — the negative control.
// Prints "sealife: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runSealifeSelfTest();

} // namespace x3::game
