#pragma once
// DEALERSHIP — "where you go to buy new cars, see cars spinning slowly under
// great lighting in a showroom." ONE WORLD content (canonlevel, `city` streamed
// region). Game/slice code only — engine/ stays pure.
//
// DESIGN
//   * SITE. One constexpr DealershipSite (kDealershipSite) is the single source
//     of truth for WHERE the dealership stands and WHICH road it fronts. The
//     building is authored in a LOCAL frame (front glass at local -X, long axis
//     along local Z) and placed by (cx, cz, yawDeg), so re-pointing it is a
//     three-number edit. Today: center (200, 360) on the New District flat pad
//     (terrain.cpp kPads: (200,500) r190, y=15 — the site is ~140 m from the
//     pad center so the whole footprint + forecourt is on the FLAT part), glass
//     front facing -X onto the District -> Spire connector road (city.cpp
//     `addRoadSegmented(170,410 -> 170,150, hw 4)`), 28 m south of the district's
//     built edge (Industrial Blvd z=410 hw 7, warehouses at z=420 half-depth 12).
//     The road reference is MIRRORED into the site struct (roadX/roadZ0/roadZ1/
//     roadHalfW) because City exposes no road table on trunk — the self-test
//     asserts the forecourt meets it, so a city.cpp change that moves the road
//     fails the test instead of silently orphaning the forecourt.
//   * GEOMETRY. Clean brush-like boxes (mesh_prims makeBox/makeCylinder), not
//     procedural noise: a 30 x 18 m hall, 6 m ceiling. White clean panels
//     (prims::makeCleanPanelRGBA) on the back/side walls + ceiling; a DARK
//     POLISHED reflector floor (the --screenshot-car slab dial: rough .08,
//     metal .5, near-black albedo) so SSR/RT reflections sweep the paint; a
//     floor-to-ceiling TINTED GLASS front (drawMeshGlass, alphaBlend => the blend
//     partition, GlassMaterial opacity ~.3) split by a 2.5 x 3 m doorway with the
//     glass header above it; three recessed ceiling STRIPS of cool-white HDR
//     emissive (strength 6 — anything > 1 blooms) running the length of the
//     hall; a dark fascia lip with a cyan signage band; a sales desk against the
//     back wall; a concrete FORECOURT slab from the front glass to the road verge.
//   * DOORS (owner review, round 2: "glass swinging doors"). The doorway carries
//     a PAIR of tinted glass leaves in slim charcoal frames, each hinged at its
//     OUTER jamb (local z = -/+kDoorHalfW) and 1.25 m wide, so they meet at the
//     center when closed. They swing BOTH WAYS: when anyone (the player's feet
//     or an NPC) comes within kDealershipDoorTrigger of the doorway center the
//     pair opens AWAY from that approacher (into the hall for someone on the
//     forecourt, onto the forecourt for someone leaving), holds while the
//     doorway is occupied, and closes kDealershipDoorHoldS after it clears. The
//     motion is a NORMALIZED CURSOR u in [0,1] integrated as +-dt/duration (the
//     DoorSystem doctrine — time enters in exactly one place) and the angle is
//     kDealershipDoorSwingDeg x doorEase(u) (door.h's trapezoid profile, a pure
//     function of u), so 60 Hz and 30 Hz land on the same angle after the same
//     elapsed time (asserted) and a reversal mid-swing is continuous. Each leaf
//     is ONE Jolt static box teleported (setBodyPosition + setBodyRotation) on
//     every frame the cursor moves — a static body re-seated per frame is the
//     same trick DoorSystem uses, and a closed pair really blocks the doorway
//     (asserted by a ray through it). The door WAVs (assets/audio/doors/
//     door_open/door_close) play as 3D one-shots at the doorway when the pair
//     starts opening / seats closed, through the host's IAudioSystem (setAudio;
//     none => silent, headless-safe). DoorSystem itself is slide-only (SlideUp /
//     SlideSplit, Scene-entity based) so it is REUSED for its ease + doctrine,
//     not threaded through.
//   * SITE DRESSING (owner review, round 2: "trees + a curb line"). A KERB LINE
//     of light-grey prisms 0.30 m wide and 0.15 m proud of the slab runs along
//     the forecourt's ROAD edge and both SIDE edges, broken by two 6 m DROPPED-
//     KERB aprons (0.04 m lips, centred on the gaps between the forecourt slots)
//     where cars enter and leave; two square kerb PLANTERS flank the entrance.
//     TREES reuse the road-trees path exactly: the nature GLBs
//     (nature/PoplarTree001.glb, nature/OakBigTree01.glb — real bark + MASK leaf
//     textures) loaded through EnvArtSystem with the far-LOD "billboard" node
//     skipped and setFoliage(1) so the leaf cards get the vegetation wrap
//     instead of rendering grey. Placement is a fixed LOCAL table: two poplars
//     in the entrance planters, a poplar row down each side of the site, an oak
//     row behind the hall (crowns clear of the roof). Each tree gets the
//     road_trees TRUNK collider only (a static box inscribed in the measured
//     bole, 5 m x scale tall; crowns stay walk-through) — the colliders ride the
//     region hooks like every other body here. EnvArtSystem draws directly
//     (drawMeshPBR from its own loader) so nothing lands in a region ledger. The
//     self-test asserts kerb/tree counts and that no kerb prism or trunk
//     overlaps the hall, a forecourt slot, or the asphalt.
//   * NPCs (owner review, round 2). A SALESPERSON stands behind the desk facing
//     the glass, and kDealershipCustomers CUSTOMERS browse: each walks a short
//     LOCAL waypoint loop on the glass-side walkway / the back aisle, pausing to
//     look at a car (waypoints keep >= 0.9 m off every disc rim). Same machinery
//     as Town's pedestrians — a Player capsule + an AnimatedCharacter on the
//     CityPerson rigs with townPedClipTable() — with ONE difference: the Player
//     capsule's wish velocity is normalized (2.5 m/s is its floor) which is a
//     stride, not a browse, so the customers are moved KINEMATICALLY
//     (setFeetPosition along the segment at kDealershipNpcWalk m/s, then a
//     zero-input Player::update so the capsule stays grounded); the rig measures
//     the feet delta itself and plays the walk blend at that speed. Warm albedo
//     tints (the showroom-civilian palette, AnimatedCharacter::setTint) make the
//     shared rigs read as different people. NPCs tick only while resident, count
//     as door approachers, and never leave the hall footprint (asserted). Their
//     capsules live until physics shutdown (characters are not removeBody-able —
//     the Town limit), so the count is deliberately small. `[E] Talk` at the
//     salesperson (kDealershipTalkReach) cycles three short trim/price lines
//     through the same prompt chip (no new dialog system; prices come from the
//     trim table so they cannot drift from the buy prompt).
//   * TURNTABLES. Four 5 m discs (0.6 m tall) in a row along the hall, each with
//     a display car on top spinning at a DISTINCT 5..8 deg/s, advanced by
//     update(dt) — yaw = rate x elapsed, so two step sizes over the same elapsed
//     time land on the same yaw (asserted by the self-test). Lighting per disc is
//     a warm overhead KEY + a cool low FILL (the showcase recipe) as Forward+
//     point lights; the strips are what the eye sees, the point lights are what
//     lights the paint.
//   * TRIMS. The four display cars are FOUR TRIMS OF THE SAME CTR (assets carry
//     exactly one hero GLB — Vehicles/CTR.glb; the roster's GBX coupe is the
//     player's hero car, not stock). Honest about it: the trim list is data
//     (DealershipCarDef: id / name / price / tint / glb), one GLB loaded once,
//     each disc repaints the clearcoat panels with its trim tint (the world_cars
//     paint rule: repaint where d.clearcoat > 0.01). Graybox box+wheels when the
//     GLB is missing or headless.
//   * DIRECT-DRAW DOCTRINE (world_cars.h). Everything here is drawn by draw()
//     from host-owned GPU resources created once in build() — NOTHING goes
//     through Scene::add(). The streamed-region ownership ledgers capture every
//     Scene::add() in their realize window and destroy the captured meshes at
//     evict; a shared GLB mesh or a mesh we intend to keep must never land in a
//     region ledger. The region hooks (onRegionBuild/onRegionTeardown, forwarded
//     from WorldStreamer::setRegionHooks for "city") therefore only add/remove
//     OUR OWN static Jolt bodies and flip a residency flag that gates draw() +
//     lights + interaction. Stream-out/in cycles are leak-free by construction
//     (bodyCount() is asserted 0 after teardown, equal after rebuild).
//   * BUY. Walk up to a disc: prompt "[E] BUY <NAME> - <price> cr" plus the
//     balance. E with credits >= price deducts EXACTLY the price from the wallet,
//     asks the host to deliver the car (DeliverFn -> WorldCars::addCar of a
//     WorldCarDef parked on the next free FORECOURT slot, unlocked, in the trim
//     paint, region "city" so WorldCars' own region hooks re-park it across
//     stream cycles), and shows "SOLD - <NAME> delivered to the forecourt".
//     Not enough credits: "NO CREDITS" (the perfshop wording), nothing changes.
//     Four forecourt slots; a fifth purchase says "FORECOURT FULL" and refuses
//     (money untouched). The display car never leaves the disc.
//   * SOLD LIST PERSISTED (owner review, round 2). The sold list (trim id, paint,
//     forecourt slot) is serialized to a small JSON sidecar — dealership.json in
//     the working directory, next to vehbuild.json — written after every sale
//     (saveSoldFile) and read once at boot (loadSoldFile, after build() and the
//     deliver hook): each entry is RE-DELIVERED through the same DeliverFn as a
//     purchase, so the bought cars are back on the same slots after quit/reload
//     and a purchase after a load appends to the next free slot. The host's
//     deliver hook parks immediately only while the site is resident; at boot
//     the region is not yet realized, so WorldCars keeps the car pending until
//     "city" builds (its normal path). Round trip asserted by the self-test.
//   * CREDITS live in ONE place: vehparts::VehicleBuild::credits (default 12000),
//     persisted in vehbuild.json (vehparts::defaultBuildSavePath()). The
//     dealership BORROWS a VehicleBuild* (like PerfShop does) and calls an
//     optional save hook after every successful sale; the host owns the object,
//     loads it at boot and saves it in that hook. The self-test injects its own.
//
// Headless-testable: build() with a null device skips all GPU resources (trees,
// rigs, meshes) but keeps the kerb/tree/NPC LOGIC (tables, colliders, capsules,
// doors); the ground query is injected (host: terrainHeightAtWorld; test: a flat
// slab). See runDealershipSelfTest (--test-dealership) and
// runDealershipScreenshots (--screenshot-dealership).

#include "world_cars.h"     // WorldCarDef (the delivered forecourt car)
#include "vehparts.h"       // VehicleBuild (the wallet)

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/audio/IAudioSystem.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

class EnvArtSystem;        // env_art.h (trees) — held by pointer, header stays light
class Player;              // player.h (NPC capsules)
class AnimatedCharacter;   // character_anim.h (NPC rigs)

// Where the dealership stands. Local frame: origin at the hall's floor center,
// FRONT (glass + doorway + forecourt) at local -X, long axis along local Z.
// World = RotY(yawDeg) * local + (cx, cz). yawDeg 0 => the front faces world -X.
struct DealershipSite {
    float cx = 200.0f, cz = 360.0f;   // hall floor center (world XZ)
    float yawDeg = 0.0f;              // facing (0 => glass front looks down -X)
    float halfDepth = 9.0f;           // local X half-extent (18 m deep)
    float halfWidth = 15.0f;          // local Z half-extent (30 m wide)
    float ceilingH  = 6.0f;           // floor top -> ceiling underside
    float forecourtDepth = 16.5f;     // slab from the glass toward the road (m)
    // The fronted road, mirrored from city.cpp (District -> Spire connector).
    float roadX = 170.0f, roadZ0 = 150.0f, roadZ1 = 410.0f, roadHalfW = 4.0f;
    // Region that owns the site's static bodies (WorldStreamer region id).
    const char* region = "city";
};
constexpr DealershipSite kDealershipSite{};

constexpr uint32_t kDealershipDiscs = 4;        // turntables
constexpr uint32_t kDealershipForecourtSlots = 4;
constexpr float    kDealershipDiscRadius = 2.5f; // 5 m diameter
constexpr float    kDealershipDiscHeight = 0.6f;
constexpr float    kDealershipReach = 2.0f;      // buy reach past the disc rim (m)
constexpr float    kDealershipStatusS = 4.0f;    // status line hold (s)
constexpr float    kDealershipLightRange = 90.0f;// selectLights eye gate (m)
// Doors (see DESIGN / DOORS).
constexpr float    kDealershipDoorSwingDeg = 90.0f;  // fully open
constexpr float    kDealershipDoorTrigger  = 2.5f;   // approach radius from the doorway center (m)
constexpr float    kDealershipDoorOpenS    = 1.1f;   // closed -> open
constexpr float    kDealershipDoorCloseS   = 1.4f;   // open -> closed
constexpr float    kDealershipDoorHoldS    = 0.8f;   // stays open after the doorway clears
// Site dressing.
constexpr float    kDealershipKerbH  = 0.15f;        // kerb proud of the slab (m)
constexpr float    kDealershipKerbW  = 0.30f;
constexpr float    kDealershipApronH = 0.04f;        // dropped-kerb lip
// NPCs.
constexpr uint32_t kDealershipCustomers = 4;
constexpr float    kDealershipNpcWalk   = 0.9f;      // browse pace (m/s)
constexpr float    kDealershipTalkReach = 3.0f;      // the NpcDialog reach

// One trim on a turntable. `glb` is relative to convertedGlbRoot().
struct DealershipCarDef {
    const char* id;
    const char* name;
    int         price;       // credits
    float       tint[3];     // clearcoat repaint
    float       rateDegS;    // turntable spin rate (deg/s)
    const char* glb;
};

// One kerb prism, LOCAL frame; cy is relative to the floor top (the slab top is
// 0.04 m below the floor top). Also the collider.
struct DealershipKerb { float cx, cy, cz, hx, hy, hz; };
// One planted tree, LOCAL frame. `scale` is the uniform GLB scale (poplar 21.5 m,
// oak 35.3 m tall at 1); trunk half-width / crown radius derive from it.
struct DealershipTree { float lx, lz, scale, yawDeg; bool oak; };

// The sold-list sidecar (see DESIGN / SOLD LIST PERSISTED).
inline const char* defaultDealershipSavePath() { return "dealership.json"; }

class Dealership {
public:
    using GroundFn  = std::function<float(float x, float z)>;
    using DeliverFn = std::function<bool(const WorldCarDef& def)>;
    using SaveFn    = std::function<void()>;

    Dealership();
    ~Dealership();
    Dealership(const Dealership&) = delete;
    Dealership& operator=(const Dealership&) = delete;

    // Build the site. `device` may be null (headless: bodies + logic only).
    // Ground query REQUIRED before build (floor Y = ground at the site center +
    // a small lip). GPU resources are created here and live until shutdown();
    // static bodies wait for onRegionBuild(site.region). NPC capsules are
    // spawned here (they need the physics world, not the region).
    bool build(x3::rhi::IRenderDevice* device, x3::phys::IPhysicsWorld& physics,
               std::string_view glbDir, const DealershipSite& site = kDealershipSite);
    bool built() const { return m_built; }

    void setGroundQuery(GroundFn fn) { m_ground = std::move(fn); }
    // The wallet (borrowed, host-owned) + the persist hook fired after a sale.
    void setWallet(vehparts::VehicleBuild* wallet, SaveFn onChanged = {}) {
        m_wallet = wallet; m_save = std::move(onChanged);
    }
    // Delivery: the host parks the bought car (WorldCars::addCar). Must return
    // true when the car exists in the world; false refuses the sale (no charge).
    void setDeliverHook(DeliverFn fn) { m_deliver = std::move(fn); }
    // Door sounds: loads assets/audio/doors/door_open + door_close through the
    // audio root (resolveAudio). Null => silent. Honors X3_MUTE engine-side.
    void setAudio(x3::audio::IAudioSystem* audio);

    // ---- Region lifecycle (forward from WorldStreamer::setRegionHooks) ----
    void onRegionBuild(std::string_view regionId, x3::phys::IPhysicsWorld& physics);
    void onRegionTeardown(std::string_view regionId, x3::phys::IPhysicsWorld& physics);
    bool resident() const { return m_resident; }

    // ---- Per-frame ---------------------------------------------------------
    // Turntables + status timer + doors + NPCs. `playerFeet` (nullable) is the
    // on-foot player's position — a door approacher; pass null while driving.
    void update(float dt, const x3::phys::Vec3* playerFeet = nullptr);
    // Prompt/buy/talk. eEdge = E rising edge this frame. Returns true iff the
    // press was consumed (a sale, or a salesperson line). Feet = the on-foot
    // player position.
    bool interact(const x3::phys::Vec3& feet, bool eEdge);
    // HUD line: the buy prompt while in reach, "[E] Talk" at the salesperson,
    // the status line for a few seconds after a press, else empty.
    const std::string& prompt() const { return m_line; }
    // Point lights (pre-multiplied color) when the eye is within range; inserts
    // at the FRONT of `out` (the host's budget trim keeps the front).
    uint32_t selectLights(float ex, float ey, float ez,
                          std::vector<x3::rhi::PointLight>& out) const;
    // Direct-draw the hall, kerbs, trees, NPCs, doors, glass, discs and display
    // cars. Call inside beginFrame/endFrame; the host gates on the outdoor PVS.
    // No-op unless resident.
    void draw(const x3::rhi::FrameContext& frame) const;

    // Idempotent. Removes bodies + GPU resources BEFORE physics/device die.
    void shutdown(x3::phys::IPhysicsWorld& physics);

    // ---- Sold list persistence ---------------------------------------------
    std::string soldJson() const;
    // Replace the sold list from JSON, RE-DELIVERING each car through the
    // deliver hook (slot order). Entries whose delivery fails are dropped.
    // Returns false only on a malformed document (list left empty).
    bool restoreSold(const std::string& json);
    bool saveSoldFile(const std::string& path) const;
    // Read + restore; remembers `path` so every later sale rewrites it. A
    // missing file is an empty list (true).
    bool loadSoldFile(const std::string& path);

    // ---- Queries (tests / HUD) ---------------------------------------------
    const DealershipSite& site() const { return m_site; }
    uint32_t turntableCount() const { return kDealershipDiscs; }
    uint32_t carCount() const;                   // trims on display
    const DealershipCarDef& trim(uint32_t i) const;
    float discYaw(uint32_t i) const { return m_discYaw[i]; }       // radians, unwrapped
    void  discCenter(uint32_t i, float out[3]) const;               // world, disc TOP
    void  forecourtSlot(uint32_t i, float& x, float& z, float& yawDeg) const;
    uint32_t bodyCount() const { return (uint32_t)m_bodies.size(); }
    uint32_t soldCount() const { return (uint32_t)m_sold.size(); }
    int   soldTrim(uint32_t slot) const { return m_sold[slot].trim; }
    int  nearestDisc(float x, float z) const;    // -1 if none in reach
    const std::string& status() const { return m_status; }
    // Local -> world (XZ) for the site frame (tests sample footprint corners).
    void toWorld(float lx, float lz, float& wx, float& wz) const;
    void toLocal(float wx, float wz, float& lx, float& lz) const;
    float floorTopY() const { return m_floorY; }
    // Doors: 0 closed .. kDealershipDoorSwingDeg open; dir +1 = swung into the
    // hall (+X), -1 = onto the forecourt (-X), 0 = closed.
    float doorAngleDeg() const;
    int   doorSwingDir() const { return m_doorDir; }
    bool  doorsClosed() const { return m_doorU <= 0.0f; }
    bool  doorsOpen()   const { return m_doorU >= 1.0f; }
    void  doorwayCenter(float& wx, float& wz) const;
    // Site dressing tables (LOCAL frame).
    uint32_t kerbCount() const { return (uint32_t)m_kerbs.size(); }
    const DealershipKerb& kerb(uint32_t i) const { return m_kerbs[i]; }
    uint32_t treeCount() const { return (uint32_t)m_trees.size(); }
    const DealershipTree& tree(uint32_t i) const { return m_trees[i]; }
    static float treeTrunkHalfW(const DealershipTree& t);
    static float treeCrownRadius(const DealershipTree& t);
    // NPCs: index 0 is the salesperson, 1.. the customers.
    uint32_t npcCount() const { return (uint32_t)m_npcs.size(); }
    x3::phys::Vec3 npcFeet(uint32_t i) const;
    bool npcWalking(uint32_t i) const;

private:
    struct Box { float cx, cy, cz, hx, hy, hz; };   // LOCAL frame, y relative to floor top
    struct Sold { int trim; float tint[3]; };       // index == forecourt slot
    struct Waypoint { float lx, lz, dwell; int faceDisc; };
    struct Npc {
        std::unique_ptr<Player>            body;
        std::unique_ptr<AnimatedCharacter> rig;
        std::vector<Waypoint>              path;    // empty => stands (the salesperson)
        uint32_t wp = 0;
        float    dwell = 0.0f;
        float    faceYaw = 0.0f;                    // device convention (cos, ., sin)
        bool     walking = false;
        float    tint[3] = { 1.0f, 1.0f, 1.0f };
    };
    x3::phys::BodyId addStaticBox(const Box& b, x3::phys::IPhysicsWorld& physics);
    void layoutSite();                              // kerb + tree tables (LOCAL)
    float treeBaseY(const DealershipTree& t) const; // world Y of the trunk base
    float localDirYaw(float lx, float lz) const;    // LOCAL direction -> world rig yaw
    void spawnNpcs(x3::phys::IPhysicsWorld& physics);
    void updateDoors(float dt, const x3::phys::Vec3* playerFeet);
    void updateNpcs(float dt);
    void leafPose(uint32_t leaf, float& wx, float& wz, float& worldYaw) const;
    void seatLeafBodies();
    bool deliverSold(int trimIdx, const float tint[3], WorldCarDef* out = nullptr);
    void drawCar(const x3::rhi::FrameContext& frame, const DealershipCarDef& t,
                 float x, float y, float z, float yaw) const;
    void drawBox(const x3::rhi::FrameContext& frame, x3::rhi::MeshHandle mesh,
                 x3::rhi::TextureHandle bc, x3::rhi::TextureHandle mr,
                 const float color[4], const float emis[4]) const;

    bool m_built = false, m_resident = false;
    DealershipSite m_site{};
    float m_floorY = 0.0f;                 // world Y of the floor top
    float m_cosY = 1.0f, m_sinY = 0.0f;    // site yaw
    GroundFn  m_ground;
    DeliverFn m_deliver;
    SaveFn    m_save;
    vehparts::VehicleBuild* m_wallet = nullptr;
    std::string m_soldPath;                // loadSoldFile() remembers it for saves

    float m_discYaw[kDealershipDiscs] = { 0, 0, 0, 0 };
    std::vector<Sold> m_sold;              // purchase order == forecourt slot
    std::string m_line, m_status;
    float m_statusT = 0.0f;
    int   m_nearIdx = -1;
    bool  m_nearSales = false;
    int   m_talkLine = 0;

    // Doors.
    float m_doorU = 0.0f;                  // normalized cursor, 0 closed .. 1 open
    int   m_doorDir = 0;                   // +1 into the hall, -1 onto the forecourt
    float m_doorHold = 0.0f;               // seconds of hold left after clearance
    x3::phys::BodyId m_leafBody[2]{};
    x3::audio::IAudioSystem* m_audio = nullptr;
    x3::audio::SoundHandle m_sndOpen{}, m_sndClose{};

    // Site dressing + NPCs.
    std::vector<DealershipKerb> m_kerbs;
    std::vector<DealershipTree> m_trees;
    std::unique_ptr<EnvArtSystem> m_treeArt;   // device only
    std::vector<Npc> m_npcs;

    std::vector<x3::phys::BodyId> m_bodies;
    x3::phys::IPhysicsWorld* m_physics = nullptr;

    // GPU (host-lifetime; created in build, freed in shutdown).
    x3::rhi::IRenderDevice* m_device = nullptr;
    std::vector<x3::rhi::MeshHandle>    m_meshes;   // everything we created
    std::vector<x3::rhi::TextureHandle> m_textures;
    x3::rhi::MeshHandle m_floor{}, m_backWall{}, m_sideWallA{}, m_sideWallB{},
                        m_ceiling{}, m_fascia{}, m_sign{}, m_strip{}, m_glassA{},
                        m_glassB{}, m_glassHead{}, m_mullion{}, m_disc{},
                        m_discRim{}, m_desk{}, m_deskTop{}, m_forecourt{},
                        m_lampPost{}, m_lampHead{}, m_frontSill{},
                        m_kerbUnit{}, m_leafGlass{}, m_leafFrame{};
    x3::rhi::TextureHandle m_panelTex{}, m_polishedMr{}, m_matteMr{}, m_metalMr{},
                           m_concreteMr{};
    // The display car GLB (one, shared by all trims) or the graybox fallback.
    std::unique_ptr<x3::asset::IAssetSource> m_glbSrc;
    std::unique_ptr<x3::asset::IModelLoader> m_glbLoader;
    x3::asset::Model m_glbModel;
    std::vector<x3::asset::ModelDrawable> m_glbDraw;
    bool m_skinned = false;
    x3::rhi::MeshHandle    m_boxMesh{}, m_wheelMesh{};
    x3::rhi::TextureHandle m_whiteTex{};
};

// Headless self-test (--test-dealership): siting (flat pad, meets the connector
// road, clear of the district's built edge + the river), 4 discs + 4 cars,
// dt-scaled frame-rate-independent turntables, buy deducts exactly the price and
// delivers one forecourt car, insufficient credits refuse and change nothing,
// region teardown/rebuild leaks no bodies and keeps the sold list; kerb + tree
// tables clear of the hall / slots / asphalt with colliders riding the region;
// doors open on approach, close after clearance, frame-rate independent, block
// the doorway when closed; NPCs stay inside the hall and the salesperson talks;
// the sold list round-trips through JSON + a file. Logs "[dealership] PASS/FAIL
// <name>"; returns true iff all pass. No window/Vulkan.
bool runDealershipSelfTest();

// --screenshot-dealership [outDir]: a standalone night set (exterior from the
// road with the glass glowing, interior wide, interior close on one car, the
// doors mid-swing from the forecourt with the NPCs inside) on a flat slab at
// the canon site. `device` is a live headless device (4x SSAA from main.cpp).
// Returns the process exit code (0 = all stills written).
int runDealershipScreenshots(x3::rhi::IRenderDevice* device, const std::string& outDir);

} // namespace x3::game
