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
//       the bank and while swimming. (The foam wake behind it did not survive contact
//       with the water's glass depth pre-pass; see kSeaFinDepth below.)
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

// NO WAKE (yet). A foam trail behind the surfaced fin was built and CUT: as a Scene
// Entity it renders as a BLACK SLICK on the night sea. Two causes, both real:
//   * the sea surface is GLASS and replays in the DEPTH PRE-PASS (the same quirk that
//     hides live fish seen from the bank), so a quad hugging the surface is swallowed;
//     lifting it +0.30 m makes it draw, but
//   * an unlit quad has nothing to light it out there, and neither the alphaBlend path
//     nor the additive/glass path (copied from the zap's water-flash disc) got emissive
//     foam out of it — it stays black, which is WORSE than no wake at all.
// The dorsal breaking the surface is the tell and it reads on its own. Foam wants a
// real FX/particle path, not a scene Entity; it is NOT shipped rather than shipped broken.

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

private:
    void  spawn(const SeaCreatureDesc& d, Scene& scene, x3::rhi::IRenderDevice& device,
                x3::phys::IPhysicsWorld& physics);
    void  think(SeaCreature& c, float dt, const x3::phys::Vec3& playerFeet,
                bool playerInWater, Player* player);
    void  swim(SeaCreature& c, float dt);
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
// Prints "sealife: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runSealifeSelfTest();

} // namespace x3::game
