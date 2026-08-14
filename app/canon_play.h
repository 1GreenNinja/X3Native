#pragma once
// CANON FLOOR-1 GAMEPLAY (--world canonlevel). Game/slice code only — engine/ stays pure.
//
// The data-driven canonical Floor 1 (level_loader.*) builds the lit geometry + doors +
// per-room flood-fill cull but spawns ZERO characters ("AREA CLEAR."). This system wires
// the EXISTING gameplay systems (MonsterManager / RescueSystem / WeaponSystem) onto the
// REAL canon room centers so --world canonlevel is PLAYABLE:
//
//   * a SIDEARM pickup in Jake's Cell (the spawn) so you leave the cell armed;
//   * ANIMATED enemies (GPU-skinned marcus_webb / alien_crawler / chief_martinez set via
//     tuningFor(DominionTrooper/Verthani/BlueSynth)) down the Main Hall + a few side cells;
//   * the Martinez BOSS in the Boss Arena;
//   * the 3 rescue girls (Aria/Keisha/Emily) in the Medical Bay + adjacent wards, each
//     guarded by 1-2 attackers — kill the attackers to interrupt the alien-DNA infection
//     and save her before the timer; saved -> grateful companion, expired -> transforms to
//     a boss (the existing RescueSystem victim->boss lifecycle).
//
// EVERY spawned entity is ROOM-TAGGED (its Scene::Entity::roomId set to the canon room it
// occupies) so the portal flood-fill cull + per-room lights include it, and so the model
// draw can be gated to the visible set (perf: only nearby characters are drawn/skinned).
// enemiesRemaining() folds every group + the boss so the HUD "AREA CLEAR" reflects reality.
//
// MP-friendly / headless-testable style (mirrors Level1Game / RescueSystem): build() once
// against a parsed CanonFloor, tick() each frame with the player as the damage sink, an
// interact hook for rescue, room-gated draw, HUD accessors. --test-canonplay drives it on
// a HeadlessDevice (no window / Vulkan) and asserts the spawn anchoring + room tagging +
// the distinct per-girl dialog table.

#include "scene.h"
#include "monster.h"
#include "rescue.h"
#include "weapon.h"
#include "level_loader.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <array>
#include <cstdint>
#include <functional>   // [W9-3 RPG] CanonItemSinkFn
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

struct StairNavChain;   // feat/stair-nav: the stairwell's waypoint chain (stairwell.h)

// ---- Per-girl dialog (staging/girls_dialog.json). 4 lifecycle states, distinct lines
// per girl. Loaded at build (or a tiny baked fallback if the JSON is absent) so each girl
// speaks her OWN lines by state instead of the shared makeRescueDialog() table. ----
enum class GirlDialogState : uint32_t {
    CaptiveFrantic   = 0,   // mid-attack, terror, calling Jake
    RescuedGrateful  = 1,   // saved before the infection took — relief + thanks
    CompanionAmorous = 2,   // now following Jake — warm / flirty / devoted
    InfectedLost     = 3,   // NOT saved in time — transformed to a boss
    Count            = 4
};

// One girl's lines for all four states (each state holds 1..N lines).
struct GirlDialog {
    std::string name;
    std::array<std::vector<std::string>, (size_t)GirlDialogState::Count> states;
    bool empty() const {
        for (const auto& s : states) if (!s.empty()) return false;
        return true;
    }
};

// The per-girl dialog table (Aria/Keisha/Emily + any others the JSON carries). Loaded from
// the staged JSON; on absence a small baked-in table keeps distinct per-girl voices so the
// feature (and --test-canonplay) works on a clean checkout.
class GirlsDialog {
public:
    // Load from `jsonPath` (staging/girls_dialog.json). On any failure, bakes the minimal
    // distinct table. Always leaves the table non-empty. Returns true iff the JSON parsed.
    bool load(std::string_view jsonPath);

    // True iff loaded from the JSON (vs the baked fallback).
    bool fromJson() const { return m_fromJson; }

    uint32_t count() const { return (uint32_t)m_girls.size(); }
    const GirlDialog* find(std::string_view name) const;

    // The first line for a girl in a state (the host shows it as a subtitle). Returns an
    // empty string if the girl / state is unknown. (A subtitle line is enough per spec.)
    std::string line(std::string_view name, GirlDialogState state) const;

    // True iff at least two girls have DISTINCT captive_frantic line sets (the per-girl
    // voice proof for --test-canonplay).
    bool linesAreDistinct() const;

    const std::vector<GirlDialog>& girls() const { return m_girls; }

private:
    void bakeFallback();
    std::vector<GirlDialog> m_girls;
    bool m_fromJson = false;
};

// Resolve the staged girls-dialog JSON path (repo staging/ dir, relative to the exe / cwd).
std::string canonGirlsDialogPath();

// ---------------------------------------------------------------------------
// R-5 (PB fold): UPPER-FLOOR item pickups. The upper floors scatter lightweight
// room-tagged pickup props (ammo/health/weapon caches, keycards, the nano-booster,
// lore terminals) so fighting UP the tower has an economy. Rendered as small
// tinted box props (the same primitive the Floor-1 Security keycard uses);
// grabbed on proximity in tick(). Re-homed from playable-build eb334e3.
// ---------------------------------------------------------------------------
enum class CanonItemKind : uint32_t {
    Ammo        = 0,   // ammo crate (amber)
    Health      = 1,   // medkit (red)
    Weapon      = 2,   // a heavier weapon cache (orange)
    Keycard     = 3,   // access keycard (cyan)
    NanoBooster = 4,   // the nano-booster buff (green-cyan)
    LoreTerminal= 5,   // a readable lore/data terminal (blue)
    Count       = 6
};
const char* canonItemKindName(CanonItemKind k);

// One placed pickup: a Tag::Prop entity (tinted box) at a room-tagged spot.
struct CanonItem {
    CanonItemKind kind   = CanonItemKind::Ammo;
    uint32_t      room   = kNoRoom;   // canon room it sits in (room-tagged)
    uint32_t      entity = kNoLink;   // its Scene Tag::Prop entity
    x3::phys::Vec3 pos{};             // world position
    bool          taken  = false;     // collected (hidden) once grabbed
};

// ---------------------------------------------------------------------------
// CanonPlay — the canon Floor-1 gameplay controller.
// ---------------------------------------------------------------------------
class CanonPlay {
public:
    // Build the gameplay onto the parsed+resolved `floor`: the sidearm pickup in Jake's
    // Cell, the Main-Hall + side-cell enemy squad, the Martinez boss in the Boss Arena, and
    // the 3 rescue girls + their attackers in the Medical Bay / adjacent wards. Every
    // spawned entity is room-tagged. `modelDir` is the rigged-GLB dir (the animated set).
    // The per-girl dialog is loaded from `girlsDialogPath` (staging JSON; baked fallback on
    // absence). Call once after buildCanonFloor().
    // `deferUpperFloors`: queue the F2-F7 squad enemies instead of spawning them
    // inside the build (the ~2 s tail of the boot-regression hunt, task #4) — the
    // host then drains the queue via tickUpperSpawns() over the first frames.
    // Items/pickups always place synchronously (cheap prim boxes). Tests and
    // screenshot captures keep the default (false): full content at build return.
    void build(const CanonFloor& floor, Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
               std::string_view girlsDialogPath, bool deferUpperFloors = false);

    // Deferred upper-floor spawns: pop up to `maxSpawns` queued squad enemies
    // (one Monster each). Returns the number of jobs still queued (0 = drained).
    // The player boots in the F1 cell, so floors 2-7 populate invisibly over the
    // first ~seconds of play — one spawn per frame keeps frames smooth.
    uint32_t tickUpperSpawns(const CanonFloor& floor, Scene& scene,
                             x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics,
                             uint32_t maxSpawns = 1);

    // Advance one frame: enemies attack the player (the damage sink) on cooldown, the boss
    // runs its phase machine, the girls' attackers fight, and the rescue timers/companions
    // tick. `eye`/`playerPos` is the camera world position. `player` may be null (headless
    // geometry tick). `attackFx` spawns the per-attack beam (host wires CombatFx).
    void tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
              const x3::phys::Vec3& eye, IDamageSink* player, const AttackFxFn& attackFx);

    // Fire one shot along `dir` from `eye` (only when armed): damages the first live
    // hostile across every group (Main Hall / cells / girl-attackers / Martinez / rescue
    // bosses). Returns the FireResult so the host spawns FX. No-op when not armed.
    FireResult onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                      Scene& scene, x3::phys::IPhysicsWorld& physics,
                      int damage = kDamagePerShot,
                      x3::DamageType type = x3::DamageType::Kinetic);

    // Try to rescue the nearest captive girl in range of `playerPos` (E-interact). Returns
    // true iff a girl was rescued this call (the host logs / surfaces her grateful bark).
    bool tryRescue(const x3::phys::Vec3& playerPos, float reach = kRescueReach);

    // Draw the pickup + all live characters, GATED by the room cull: a monster/girl is
    // drawn only if its room is in the scene's current visible set (perf — only nearby
    // characters are drawn). The boss + rescue bosses draw when their room is visible.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const Scene& scene) const;

    // Draw the held weapon viewmodel (no-op unless armed). Forwards to the WeaponSystem.
    void drawViewmodel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       float ex, float ey, float ez, float yaw, float pitch,
                       float yawOff, float pitchOff, float rollOff,
                       float fwd, float right, float down) const;

    // ---- Cue / death-FX fan-out (footsteps, impacts, gib bursts). Wire after build. ----
    void setCueSink(const GameCueFn& sink);
    void setDeathFxSink(const DeathFxFn& sink);
    // Lightning battery-cell pickups grant charge through this sink (wire to
    // Arsenal::grantCharge). Called with the charge amount when a cell is collected.
    void setChargeSink(std::function<void(int)> sink) { m_chargeSink = std::move(sink); }

    // ---- STAIR PURSUIT (feat/stair-nav) ------------------------------------
    // Wire the stairwell's nav chain (stairwellNavChain — borrowed, host-owned).
    // Once set, tick() routes awake, live, grounded SQUAD hostiles (main hall /
    // cell guards / attackers / upper squads — NEVER the boss ladder or rescue
    // bosses, whose arenas are story anchors) up/down the stairwell when the
    // player is on a different floor and the enemy is within the seek radius of
    // its floor's stair entry. Level 4.5 is unreachable by construction: the
    // chain has no exit there, and stairNavRoute refuses non-exit floors.
    void setStairNav(const StairNavChain* chain) { m_stairNav = chain; }

    // ---- [W9-3 RPG] item sink: pickups deposit into the BACKPACK ------------
    // When wired, a walked-over upper-floor pickup is offered to the sink INSTEAD
    // of the silent auto-collect: return true = accepted (prop hides, item is in
    // the bag / applied), false = refused (bag full — the pickup STAYS in the
    // world). Unwired (headless tests) keeps the legacy collect-and-log behavior.
    using CanonItemSinkFn = std::function<bool(const CanonItem&)>;
    void setItemSink(const CanonItemSinkFn& sink) { m_itemSink = sink; }

    // Tear down any live death-ragdoll bodies across every enemy group + the boss. Call
    // BEFORE the physics world is shut down (mirrors MonsterManager::shutdown) so no Jolt
    // ragdoll body is destroyed after the world is gone (an access violation otherwise).
    // Idempotent / safe to call any time.
    void shutdown();

    // ---- Queries (HUD + the self-test) ------------------------------------
    bool built() const { return m_built; }
    bool armed() const { return m_weapon.hasWeapon(); }
    void cheatArm(Scene& scene) { m_weapon.forceArm(scene); }   // IDKFA/IDFA

    // Total LIVE hostiles across every group (corridor squad + cell guards + the per-girl
    // attackers + Martinez + any rescue-boss transforms). Drives the HUD "ENEMIES" counter
    // so it never reads "AREA CLEAR" while something is alive.
    int enemiesRemaining() const;

    // ---- OPENING-FLOW SPAWN GATING (the 101-enemy-horde fix) ----------------
    // Every boot spawn starts DORMANT (idling/patrolling its beat, blind to the
    // player). tick() wakes monsters by REGION/PROGRESSION: nothing wakes while
    // Jake is still inside his cell (the wake-as-a-captive beat is quiet, alert
    // stays CALM); once he leaves the cell, spawns wake by proximity (same-floor
    // radius / a tight 3D radius) and STAY awake. Taking damage always wakes a
    // monster (MonsterSystem::fire). Alert reinforcements spawn awake (they hunt).
    //
    // LIVE hostiles that are AWAKE (non-dormant): the honest HUD "ENEMIES" count —
    // the LOCAL threats, not the whole spire.
    int enemiesAwake() const;
    // True once the player has left Jake's Cell (through the door OR down the
    // trapdoor). Drives the opening objective beat ("find a way out of the cell").
    bool leftCell() const { return m_leftCell; }

    bool martinezSpawned() const { return m_martinezSpawned; }
    bool martinezAlive()   const { return m_martinezSpawned && m_martinez.alive(); }

    RescueSystem&       rescue()       { return m_rescue; }
    const RescueSystem& rescue() const { return m_rescue; }
    const WeaponSystem& weapon() const { return m_weapon; }
    const GirlsDialog&  dialog() const { return m_dialog; }

    // ---- W5-3: SARAH + THE ENDGAME SPINE -----------------------------------
    // Sarah is a single standalone RescueVictim OWNED here (not in the F2
    // RescueSystem — VictimId is a fixed 3-girl enum and rescue.{h,cpp} are
    // another workstream's files this wave). Her captive timer NEVER runs
    // (tick is fed hubReached=false), so she cannot expire: the endgame gate is
    // the CLONE, not a clock. Lifecycle: Captive (containment field, gated on
    // cloneDefeated) -> Companion (follows Jake) -> Extracted (reached the
    // Helipad = the WIN state, latched for the host).
    bool sarahPresent()   const { return m_sarahBuilt; }
    const RescueVictim* sarah() const { return m_sarahBuilt ? &m_sarah : nullptr; }
    // True once the F7 "Jake's Clone" ladder boss is dead (latched — stays true
    // even after the corpse despawns). False until the ladder spawned him.
    bool cloneDefeated() const;
    // E-rescue on Sarah: refuses while the clone lives (the containment field is
    // keyed to his bio-signature — the story gate), else flips her to Companion.
    bool trySarahRescue(const x3::phys::Vec3& playerPos, float reach = kRescueReach);
    // Latch: true exactly once, the frame she reaches the Helipad (the host runs
    // the win sequence off this edge).
    bool sarahExtractedThisFrame() { const bool f = m_sarahWinFrame; m_sarahWinFrame = false; return f; }
    bool sarahExtracted() const { return m_sarahBuilt && m_sarah.extracted(); }
    uint32_t sarahRoom()   const { return m_sarahRoom; }
    uint32_t helipadRoom() const { return m_helipadRoom; }
    // Test hook (--test-goldenpath): kill the F7 clone through the REAL fire path
    // (a point-blank lethal shot at his body) so the endgame gate opens without
    // simulating a full boss fight. Cheat-family method (like cheatArm). Returns
    // true when he is confirmed dead.
    bool testKillClone(Scene& scene, x3::phys::IPhysicsWorld& physics);

    // ---- W9-1: DESC-MECHANICS HOOKS (docs/DESC_MECHANICS_TODO.md Tier A) ----
    // Coolant sabotage (F4 "Coolant System" console): The Collective takes x1.5
    // damage from now on (applied at damage application, so it works whether the
    // flag lands before or after his spawn). Idempotent; false if he is absent
    // (single-floor load). Keyed on StoryFlags `f4.coolant_sabotaged` by the host.
    bool applyCoolantSabotage();
    bool coolantSabotaged() const { return m_coolantSabotaged; }
    // EMP discharge (F4-crafted device): stun every SYNTHETIC-species enemy
    // (EnemyType::BlueSynth — the drone/synth bestiary row) within `radius` of
    // `center` for `secs`. Regular groups only (the boss ladder is story-owned).
    // Returns how many were stunned.
    uint32_t empStun(const x3::phys::Vec3& center, float radius = 12.0f,
                     float secs = 6.0f);
    // Master hack (F5 "Central Control Hub"): permanently power down every
    // BlueSynth squad enemy on floor `floorNum` (per roomFloorNum, resolved via
    // floor.roomAt on each body). Killing them afterwards still counts. Returns
    // how many powered down. Keyed on StoryFlags `f5.hacked` by the host.
    uint32_t setDroneSpeciesDocile(const CanonFloor& floor, int floorNum);
    // Ladder-boss lookup by display-name substring (test/diagnostic accessor;
    // e.g. "The Collective"). nullptr when absent. Non-owning.
    MonsterSystem* findLadderBoss(std::string_view showNameSub);

    // The canon room id a girl is in (Medical Bay / a ward), by victim index. kNoRoom if
    // unknown. Used by the host to surface her per-girl subtitle from the right state.
    uint32_t girlRoom(uint32_t victimIndex) const {
        return victimIndex < m_girlRooms.size() ? m_girlRooms[victimIndex] : kNoRoom;
    }

    // ---- HUD radar / nameplate feed (read-only enumeration) ----
    // `awake`: opening-flow gating state — a DORMANT spawn still enumerates (the
    // alert system's observers = the facility's ears, and the P6 marks==remaining
    // invariant holds), but the host's radar skips it (an undetected threat is not
    // a red blip; the counter reads awake-only, and the two must agree).
    struct EnemyMark { x3::phys::Vec3 pos; const char* label; bool awake = true; };
    uint32_t liveEnemyMarks(EnemyMark* out, uint32_t cap) const;
    uint32_t liveCompanionPositions(x3::phys::Vec3* out, uint32_t cap) const;

    // ---- ALERT / WANTED-SYSTEM FEED (the canon arming of app/alert.h) -------
    // True iff ANY live hostile across every group currently holds LOS to the
    // player (the AlertSystem's `playerSeen` observation — mirrors Level1Game's
    // corridor/checkpoint scan).
    bool anyHostileLineOfSight() const;
    // Visit every DEAD hostile's position (the corpse census the host feeds to
    // AlertSystem::registerCorpse each frame — the system dedupes internally).
    void forEachCorpse(const std::function<void(const x3::phys::Vec3&)>& fn) const;

    // ---- AREA-OF-EFFECT HOOK (the WATER ZAP, app/waterzap.h) ---------------
    // Visit every hostile MonsterManager (Main Hall / cell guards / girl
    // attackers / floor bosses / upper squads / the rescue bosses) so a host
    // AoE (the lightning gun electrifying the water) can damage whatever is
    // standing in its radius. Read/write access: the visitor applies damage.
    void forEachHostileManager(const std::function<void(MonsterManager&)>& fn);
    // Alert REINFORCEMENTS (SEARCH / KILL SQUAD effects): queue `count` extra
    // guards into the SAME deferred-spawn queue the F2-F7 boot squads use, so
    // the host's tickUpperSpawns(.., 1) budget (one spawn per frame) holds.
    // Spawn room = the doored NEIGHBOUR of the player's room nearest the player
    // (guards arrive "through the door"), falling back to the player's own room,
    // then the Main Hall. Enemies are room-tagged like every other canon spawn
    // and use the shared tunings (DominionTrooper; Illuminated for a kill squad).
    // Returns the number queued (0 when no spawn room resolves).
    uint32_t queueAlertReinforcements(const CanonFloor& floor,
                                      const x3::phys::Vec3& nearPos,
                                      int count, bool killSquad);

    // ---- Spawn bookkeeping (the self-test asserts these) ------------------
    // The room the sidearm pickup was placed in (Jake's Cell). kNoRoom if unbuilt.
    uint32_t pickupRoom() const { return m_pickupRoom; }
    // The room Martinez spawned in (the Boss Arena). kNoRoom if absent.
    uint32_t bossRoom() const { return m_bossRoom; }
    // Count of room-tagged hostile spawns (every Main-Hall / cell / attacker spawn).
    uint32_t taggedHostileCount() const { return m_taggedHostiles; }
    // The Main-Hall enemy count + the cell-guard count (the corridor squad split).
    uint32_t mainHallCount()  const { return m_mainHall.count(); }
    uint32_t cellGuardCount() const { return m_cellGuards.count(); }
    // Per-girl attacker count (Medical Bay interrupt-rescue enemies).
    uint32_t attackerCount() const { return m_attackers.count(); }
    // Borrow a ward attacker (the assault-tableau self-test asserts his FACING and
    // that damage releases his scripted pose, against the real built level).
    MonsterSystem&       attackerAt(uint32_t i)       { return m_attackers.at(i); }
    const MonsterSystem& attackerAt(uint32_t i) const { return m_attackers.at(i); }
    // Placed lightning battery-cell count (the self-test asserts >0 + room-tagging).
    uint32_t batteryCount() const { return (uint32_t)m_batteries.size(); }

    // R-5 (PB fold): upper-floor population + pickups (regular enemies on floors
    // 2-7 by themed room name; item props with proximity grab).
    uint32_t upperEnemyCount() const { return m_upperEnemies.count(); }
    uint32_t upperItemCount()  const { return (uint32_t)m_upperItems.size(); }
    uint32_t upperItemsTaken() const {
        uint32_t n = 0; for (const auto& it : m_upperItems) if (it.taken) ++n; return n;
    }
    const std::vector<CanonItem>& upperItems() const { return m_upperItems; }

private:
    // R-5 (PB fold): spawn a themed squad in a named upper-floor room (deterministic
    // zig scatter, depth-scaled hp/speed) / place one pickup prop / populate 2-7.
    uint32_t spawnUpperEnemies(const CanonFloor& floor, Scene& scene,
                               x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                               const char* roomName, const EnemyType* mix, uint32_t mixCount,
                               int hpBonus, float speedBonus);
    bool placeUpperItem(const CanonFloor& floor, Scene& scene, x3::rhi::IRenderDevice& device,
                        const char* roomName, CanonItemKind kind, float dx, float dz);
    void buildUpperFloors(const CanonFloor& floor, Scene& scene,
                          x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                          bool deferred);
    // One queued F2-F7 squad enemy (deferred boot spawn): room + slot-in-squad
    // (the deterministic zig scatter needs i-of-n) + depth scaling.
    struct UpperSpawnJob {
        std::string room;
        EnemyType   type;
        uint32_t    idx = 0, squadSize = 1;
        int         hpBonus = 0;
        float       speedBonus = 0.0f;
        // Alert reinforcements resolve their room by INDEX (room names are not
        // unique across floors); kNoRoom = resolve `room` by name (boot squads).
        uint32_t    roomIdx = kNoRoom;
    };
    void spawnOneUpper(const CanonFloor& floor, Scene& scene,
                       x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                       const UpperSpawnJob& job);
    // Tag a freshly-spawned monster's Scene entity with `room` (so the cull + lights include
    // it). Returns the room id (for bookkeeping). Idempotent / bounds-checked.
    uint32_t tagRoom(Scene& scene, const MonsterSystem& m, uint32_t room);

    // Draw a manager's monsters, gated to the scene's visible room set (perf cull).
    void drawManagerCulled(const MonsterManager& mm, x3::rhi::IRenderDevice& device,
                           const x3::rhi::FrameContext& frame, const Scene& scene) const;

    // ---- CHARACTER MUTUAL EXCLUSION (Tim 2026-07-26) ----------------------
    // Gather every LIVE character into `bodies` (SepBody discs) + a parallel `owners`
    // array: a movable monster's MonsterSystem* (written back after separation) or
    // nullptr for a captive/companion/Sarah ANCHOR. Covers every hostile group +
    // Martinez + the rescue captives + Sarah.
    void gatherCharacters(std::vector<SepBody>& bodies,
                          std::vector<MonsterSystem*>& owners);
    // Runtime enforcement (called at the END of tick): resolve all character overlaps
    // so no two characters share a volume, then write the corrected positions back to
    // the movable monsters (captives are anchors — never moved onto a monster).
    void separateCharacters(x3::phys::IPhysicsWorld& physics);
    // Spawn-placement guard: return a spot near `want` that does NOT overlap any
    // already-placed character (outward spiral step-search); `radius` is the
    // spawnee's collision radius. Prevents a monster spawning merged into a captive.
    x3::phys::Vec3 clearSpawnPos(const x3::phys::Vec3& want, float radius);
    // True iff `b` is a non-damageable friendly body (a captive/companion/Sarah). The
    // weapon aim-ray steps PAST these so a captive never eats an enemy's shot (backup
    // to mutual exclusion — a captive can still stand between the player and a foe).
    bool isFriendlyBody(x3::phys::BodyId b) const;

    WeaponSystem   m_weapon;       // sidearm pickup in Jake's Cell
    MonsterManager m_mainHall;     // animated squad down the Main Hall
    MonsterManager m_cellGuards;   // a few enemies in side cells
    MonsterManager m_attackers;    // the per-girl Medical-Bay attackers (interrupt rescue)
    MonsterManager m_floorBosses;  // W4-1: the F2-F7 authored boss ladder (one per arena)
    MonsterManager m_upperEnemies; // R-5 (PB fold): regular squads on floors 2-7
    std::vector<CanonItem> m_upperItems;   // R-5: the upper-floor pickups
    std::vector<UpperSpawnJob> m_upperQueue;   // task #4: deferred boot spawns
    size_t m_upperQueueNext = 0;               // drained via tickUpperSpawns()
    MonsterSystem  m_martinez;     // the Boss Arena boss
    RescueSystem   m_rescue;       // the 3 girls (Aria/Keisha/Emily) + their boss transforms
    GirlsDialog    m_dialog;       // per-girl 4-state lines
    // W5-3: Sarah (standalone victim, see the public block) + the endgame state.
    RescueVictim m_sarah;
    bool     m_sarahBuilt   = false;
    bool     m_sarahWinFrame = false;
    uint32_t m_sarahRoom    = kNoRoom;
    uint32_t m_helipadRoom  = kNoRoom;
    int      m_cloneIdx     = -1;      // m_floorBosses index of "Jake's Clone"
    // W9-1 desc-mechanics: ladder bookkeeping + the coolant-sabotage latch.
    std::vector<std::string> m_ladderNames;   // show name per m_floorBosses index
    int      m_collectiveIdx = -1;     // m_floorBosses index of "The Collective"
    bool     m_coolantSabotaged = false;
    mutable bool m_cloneDeadLatch = false;
    float    m_heliX0 = 0, m_heliX1 = 0, m_heliZ0 = 0, m_heliZ1 = 0;   // helipad XZ rect

    std::vector<uint32_t> m_girlRooms;   // canon room per victim index

    GameCueFn  m_cueSink;
    DeathFxFn  m_deathFx;
    CanonItemSinkFn m_itemSink;   // [W9-3 RPG] backpack deposit (empty = legacy)
    std::string m_modelDir;

    // ---- STAIR PURSUIT state (feat/stair-nav, see setStairNav) --------------
    const StairNavChain* m_stairNav = nullptr;   // borrowed (host owns)
    float m_stairRouteTimer = 0.0f;              // routing cadence countdown
    void routeStairPursuers(Scene& scene, const x3::phys::Vec3& eye);

    // ---- Lightning battery-cell pickups (floating, spinning, translucent faceted
    // crystals — the Lab2 crystal language; grant charge to the Lightning Gun). ----
    struct Battery {
        x3::phys::Vec3 pos{};
        uint32_t       entity    = kNoLink;
        bool           collected = false;
        float          phase     = 0.0f;   // spin/bob phase offset so they don't sync
    };
    std::vector<Battery>     m_batteries;
    x3::rhi::MeshHandle      m_crystalMesh;      // shared faceted-crystal mesh
    float                    m_batteryAnimT = 0.0f;
    std::function<void(int)> m_chargeSink;       // -> Arsenal::grantCharge
    static constexpr int     kBatteryCharge = 60;  // charge granted per cell
    // Build one crystal battery pickup at `pos`, room-tagged, into the scene.
    void addBattery(Scene& scene, x3::rhi::IRenderDevice& device,
                    const x3::phys::Vec3& pos, uint32_t room);

    // ---- Opening-flow spawn gating state (see the public block) -------------
    // Jake's Cell XZ rect + floor Y, cached at build (tick has no floor ref).
    bool  m_cellValid = false;
    bool  m_leftCell  = false;
    float m_cellX0 = 0, m_cellX1 = 0, m_cellZ0 = 0, m_cellZ1 = 0, m_cellFloorY = 0;
    // Mark every live boot spawn dormant / wake spawns near the player.
    void setAllDormant();
    void wakeNearbySpawns(const x3::phys::Vec3& eye);

    uint32_t m_pickupRoom    = kNoRoom;
    uint32_t m_bossRoom      = kNoRoom;
    uint32_t m_taggedHostiles = 0;
    bool     m_martinezSpawned = false;
    bool     m_built           = false;
    // W4-1: one-shot Martinez entrance beat (arena bounds cached at build — tick()
    // has no floor reference to query rooms from).
    bool     m_bossIntroFired = false;
    float    m_arenaX0 = 0, m_arenaX1 = 0, m_arenaZ0 = 0, m_arenaZ1 = 0;
    x3::phys::Vec3 m_arenaCtr{};
};

// Headless self-test (--test-canonplay). Loads Floor 1, builds the canon floor + CanonPlay
// on a HeadlessDevice + Jolt world, and asserts: the sidearm spawns in Jake's Cell; N
// animated enemies + the cell guards spawn in the Main Hall / cells (room-tagged); Martinez
// spawns in the Boss Arena; the 3 girls + their attackers spawn in the Medical Bay (room-
// tagged); enemiesRemaining() counts them all; and the per-girl dialog table has DISTINCT
// lines per girl across the 4 states. Logs PASS/FAIL P#, returns true iff all pass. No
// window / Vulkan. Skips cleanly (PASS) if the canonical JSON is absent on this machine.
bool runCanonPlaySelfTest();

// --test-goldenpath (W5-3): the ENDGAME SPINE check — the critical path from the cell
// to the win, asserted at the state level (headless, no window): the full tower loads,
// every spine room exists (wards / lobbies / Clone arena / Sarah's cell / Helipad),
// Sarah spawns captive, the clone GATE holds (rescue refused while he lives), killing
// him opens it, the rescue flips her to Companion, and reaching the Helipad extracts
// her = the WIN latch. Also proves sarah.json loads + her first_meeting tree starts.
// This is the Gate-C foundation. Logs PASS/FAIL G#, returns true iff all pass.
bool runGoldenPathSelfTest();

// --test-opening (opening-flow fix): the WAKE-IN-CELL contract, asserted headless.
// Loads the tower + builds CanonPlay exactly like the host, then asserts: the spawn
// room resolves to Jake's Cell and the spawn point is inside it on the floor; the
// player is UNARMED at wake and the sidearm pickup is out of auto-arm reach of the
// spawn point (a first tick at the spawn must NOT arm); every boot spawn is DORMANT
// (enemiesAwake()==0 while enemiesRemaining() counts the full roster); no hostile
// holds LOS at wake so a fed AlertSystem stays 0 CALM across simulated seconds; and
// stepping OUT of the cell latches leftCell + wakes only the LOCAL hall spawns
// (0 < awake << total). Logs PASS/FAIL O#, returns true iff all pass. Skips (PASS)
// when the canonical JSON is absent.
bool runOpeningFlowSelfTest();

// --test-stairnav (feat/stair-nav): asserts the stairwell nav chain matches the
// built geometry (monotone-Y spine inside the shaft, one exit per REAL floor),
// the STRUCTURAL 4.5 seal (no exit/waypoint at the 4.5/master height, unserved
// routes refused, negative control on a doctored chain), and a live MonsterSystem
// commuting F1 -> F3 over the BUILT tower + stairwell within a bounded headless
// sim, with a per-step ground-clearance raycast proving feet stay on the treads.
// Logs PASS/FAIL S#, returns true iff all pass. Skips (PASS) when the canonical
// JSON is absent.
bool runStairNavSelfTest();

} // namespace x3::game
