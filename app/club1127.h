#pragma once
// Club 1127 — "THE DEEP". Tim's real ~4,000 sq ft underground Miami nightclub,
// ported forward to native C++/Vulkan from his OWN Babylon game module
// (Q3Engine/src/world/x3-club1127.js — Tim's IP, NOT id Tech). The hidden club at
// the bottom of the EFLZ Spire, reached via the elevator's DISCO descent (keypad
// code 1127) down to Y=-200 (docs/design/X3_WORLD_BLUEPRINT.md §2.3).
//
// Game/slice code only — engine/ stays pure. A self-contained area, kept
// LOW-CONFLICT: it does NOT touch level1.cpp / elevator.* / the Spire. The host
// reaches it via the `--world club` CLI flag (mirrors `--world terrain`); the
// elevator's in-game 1127 disco descent wires the trip down + drops the player at
// spawn() (this module just builds the room at Y=-200 + exposes the build/draw API).
//
// What it builds (porting x3-club1127.js 1:1, real Club 1127 measurements):
//   * MAIN SHELL — 50x100x30 ft room (15.24 x 30.48 x 9.14 m), floor at Y=-200.
//   * ENGINE ROOM + LOUNGE — 2-story room on the north (-Z) side: a ground floor,
//     a second-story lounge floor at 15 ft with a railing, and a 12-STEP STAIR up
//     the west wall; glass swing doors + an inset alcove door into the main club.
//   * SUSPENDED DJ BOOTH — elevated booth (turntables, mixer console, 2 OLED
//     screens, a secured KEYPAD DOOR), hung on support brackets over the floor.
//   * THE ORB — a 2 m mirror ball on a cable at the ceiling, 4 rotating colored
//     spotlights + 4 ring lights orbiting it.
//   * AERIAL BAR — neon-lit suspended bar beside the DJ booth, polished counter +
//     safety railings + a magenta neon underglow.
//   * GROUND BAR — counter along the west wall + 7 stools + a bar light.
//   * DANCE FLOOR — full-club purple/dark checkerboard of glowing tiles.
//   * PA RIG — 4x SVS PB16-Ultra subs (corners), 16x JBL JRX200 (8 stacked pairs)
//     + 8 amps on the walls, 4x JBL 18" subs flanking the floor, 16x JBL surrounds.
//   * 28 BLACKLIGHTS — 4 ft UV tubes on the walls at 10 ft intervals, pulsing.
//   * TV MULTIPLEX — 6 screens (80/85/75/65/55/55") on a POE network.
//   * VIP / COUCHES — black couches + end table (SE corner) + a VIP couch (SW).
//
// Construction mirrors app/env_art.cpp + app/door.cpp + the prior club:
//   * Geometry: x3::prims::makeBox -> device->createMesh (render) + physics->
//     addStaticMesh (collision), registered as Scene entities (Tag::Static). The
//     Scene draws/syncs them like any other static geometry. Cylinders/spheres in
//     the JS (turntables, stools, the ORB, blacklight tubes, cables) are
//     approximated with boxes (the engine's primitive) — graybox-faithful.
//   * Characters (a DJ behind the booth + a bouncer at the landing): loaded + drawn
//     + animated via MonsterSystem with damage 0 / chaseSpeed 0 (inert "props" that
//     still skin + play their idle clip). A failed GLB load falls back to a box, so
//     the area never breaks.
//   * Lights: the world hands the host a vector<PointLight> (neon magenta/cyan/
//     violet + UV + the orbiting ring/spot lights + warm bar fills) to push via
//     setPointLights, plus emissive fixture/tile/blacklight meshes for bloom.
//   * Animation: the ORB spins, the spotlights orbit, the blacklights pulse — all
//     ported from updateClub1127(); the lights ride the device's per-frame set.
//
// Performance (per the JS + §2.3): the entire club lives BELOW Y=-200, far from
// any other world, so it is naturally distance-culled; emissive materials are
// authored once (frozen) and the geometry is static. The ~80 m cull distance is
// recorded for the host. Box counts are kept modest (merged-equivalent: one mesh
// per fixture-class instance, low tessellation).

#include "scene.h"
#include "monster.h"
#include "surface_library.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// Build result / handle for the Club 1127 area. Owns the inert character systems
// (so their loaded GLB GPU handles stay alive for the app lifetime) and exposes
// the spawn point + the neon point-light set the host must apply, plus a small
// fixture census the headless `--test-club` self-test asserts against.
class Club1127World {
public:
    // The club's world Y (floor of the main room). Canon: Y = -200 (§2.3).
    static constexpr float kClubY = -200.0f;

    // Real Club 1127 main-room footprint (Tim's measurements; meters).
    //   CW = 50 ft depth (east-west, X)    = 15.24 m
    //   CL = 100 ft length (north-south, Z) = 30.48 m
    //   CH = 30 ft ceiling                  =  9.14 m
    static constexpr float kCW = 15.24f;   // X span (50 ft)
    static constexpr float kCL = 30.48f;   // Z span (100 ft)
    static constexpr float kCH = 9.14f;    // ceiling height (30 ft)
    static constexpr float kCullDist = 80.0f;   // §2.3 distance cull (m)

    // A census of the fixtures the test asserts. Populated by build().
    struct Stats {
        int   entities      = 0;   // total Scene entities authored by the club
        int   blacklights   = 0;   // 4 ft UV tubes (target: 28)
        int   tvScreens     = 0;   // POE multiplex displays (target: 6)
        int   stairSteps    = 0;   // engine-room stair steps (target: 12)
        int   barStools     = 0;   // ground-bar stools (target: 7)
        int   svsSubs       = 0;   // SVS PB16-Ultra corner subs (target: 4)
        int   jblLineArray  = 0;   // JBL JRX200 cabinets (target: 16 = 8 pairs)
        int   jbl18Subs     = 0;   // JBL PRO 18" subs (target: 4)
        int   surrounds     = 0;   // JBL N26/S38 surrounds (target: 16)
        int   couches       = 0;   // couch/VIP seating pieces (target: >=3)
        bool  hasDjBooth    = false;
        bool  hasDjTurntables = false;
        bool  hasDjScreens  = false;   // 2 OLED screens on the booth
        bool  hasKeypadDoor = false;   // DJ booth keypad door
        bool  hasOrb        = false;   // the 2 m mirror ball
        bool  hasAerialBar  = false;
        bool  hasGroundBar  = false;
        bool  hasLoungeFloor = false;  // 2nd-story engine-room lounge floor
        bool  hasDanceFloor = false;   // checkerboard tiles
        // Authored main-room floor center Y (asserted ~ -200) + footprint bounds.
        float floorY        = 0.0f;
        float roomMinX = 0.0f, roomMaxX = 0.0f;   // X extent of the main shell
        float roomMinZ = 0.0f, roomMaxZ = 0.0f;   // Z extent of the main shell
        float ceilingY      = 0.0f;               // authored ceiling slab center Y
    };

    // Build the whole club into `scene` / `physics`, uploading meshes through
    // `device`, at world Y = kClubY (-200). `modelDir` is the rigged-GLB root used
    // for the DJ / bouncer character props. Call once. Idempotent: a second call is
    // a no-op (returns the cached Stats).
    const Stats& build(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, std::string_view modelDir);

    // Advance the club one frame: spin the ORB, orbit the spotlights, pulse the
    // blacklight emissive, and tick the inert character idle clips. Re-uploads the
    // animated point-light set to the device (the orbiting spot/ring lights move),
    // so this must be called every frame before beginFrame() in the live path.
    void update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                x3::phys::IPhysicsWorld& physics);

    // Draw the loaded GLB characters (DJ, bouncer). Call alongside scene.render()
    // each frame, like Level1Game::drawWorldExtras.
    void drawCharacters(x3::rhi::IRenderDevice& device,
                        const x3::rhi::FrameContext& frame, const Scene& scene) const;

    // Player spawn (feet position) — at the elevator-entrance landing on the east
    // wall, facing into the club toward the dance floor + DJ booth. The host poses
    // the camera/character here (the elevator's disco descent drops the player in).
    x3::phys::Vec3 spawn() const { return m_spawn; }

    // A good fixed showcase camera pose for the headless screenshot: an elevated
    // vantage over the dance floor that frames the DJ booth + the ORB + the bars.
    // Fills the 5 floats (x,y,z,yaw,pitch).
    void showcaseCamera(float out[5]) const;

    // The neon/UV/orbiting point lights the host should apply with
    // IRenderDevice::setPointLights. STATIC ones are set once after build; the
    // ORBITING spot/ring lights move each frame, so update() re-pushes the whole
    // set — the host can also just push pointLights() once and call update().
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

    // The fixture census (valid after build()).
    const Stats& stats() const { return m_stats; }

    // The OLED screen entities (2 DJ + 6 TV multiplex + 1 back-bar band + 1 lounge).
    // Exposed so the self-test can assert they are real textured glass carrying the
    // per-texel emissive path — "the panel exists" and "the panel SHOWS SOMETHING"
    // are not the same check (the rifthub blank-screen lesson, c44da59).
    const std::vector<uint32_t>& oledEntities() const { return m_oledEnts; }

    // Recommended cull distance (m): hide the club beyond this from the player.
    float cullDistance() const { return kCullDist; }

    bool built() const { return m_built; }

private:
    // --- Geometry helper (defined in the .cpp) ---
    // A solid static box (render mesh + Jolt collision + Scene entity), tinted,
    // optionally emissive (for neon strips / tiles / blacklights / fixtures).
    // Center + half extents in world meters (Y already offset to kClubY by the
    // caller). `collide=false` for thin decorative inlays. `surf` (W6-3 texture
    // pass) is an optional surface_library set: when provided + valid, the box
    // carries a real albedo/normal/mr texture (the tint in `color` still
    // multiplies it, so the venue's near-black palette is unchanged — the
    // texture adds relief/roughness detail instead of a flat tinted box).
    // Returns the entity id.
    uint32_t addBox(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics,
                    float cx, float cy, float cz, float hx, float hy, float hz,
                    const float color[4], const float emissive[4], bool collide,
                    float uvScale = 1.0f, const SurfaceSet* surf = nullptr);

    // Inert character prop: a MonsterSystem with damage 0 / chaseSpeed 0 that
    // loads + draws + animates `modelFile`. Appends to m_chars.
    void addCharacter(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                      const std::string& modelFile, const x3::phys::Vec3& pos,
                      float scale, bool standUpZtoY, const float tint[4]);

    bool                                          m_built = false;
    Stats                                         m_stats{};
    x3::phys::Vec3                                m_spawn{};
    std::vector<x3::rhi::PointLight>              m_lights;
    // Count of STATIC lights at the front of m_lights; the orbiting spot/ring
    // lights (the last kOrbit*) are rewritten each frame by update().
    size_t                                        m_staticLightCount = 0;
    // Scene-entity ids of the animated emissive blacklight tubes (their emissive
    // pulses each frame) + the ORB (it spins) so update() can touch just those.
    std::vector<uint32_t>                         m_blacklightEnts;
    uint32_t                                       m_orbEnt = 0xFFFFFFFFu;
    bool                                           m_orbValid = false;
    float                                          m_orbY = 0.0f; // ORB center world-Y
    // MAX-OUT pass (Tim 2026-07-07): animated fixture sets update() drives.
    //   OLED screens: emissive shimmer phases so the wall content reads as LIVE
    //   video, not frozen stills. Sub plates: the speaker cones thump on the
    //   128 BPM beat clock. Bright dance tiles: breathe with the same beat.
    std::vector<uint32_t>                         m_oledEnts;
    std::vector<uint32_t>                         m_subPulseEnts;
    std::vector<uint32_t>                         m_tilePulseEnts;
    std::vector<size_t>                           m_subLightIdx;   // corner-sub pulse lights (into m_lights)
    // DANCERS (Tim: 'work on those dancers') — real skinned GLB characters on the
    // floor, choreographed by update(): a beat-locked bounce + hip sway + a slow
    // personal-space shuffle, phase-offset per dancer, layered over their idle
    // clip via MonsterSystem::setPropPose. Replaces the pastel box crowd.
    struct Dancer {
        size_t charIdx = 0;         // index into m_chars
        float  bx = 0, bz = 0;      // home spot on the floor (world XZ)
        float  baseY = 0;           // feet Y (the dance-floor tile top)
        float  yaw = 0;             // base facing
        float  phase = 0;           // personal groove phase offset
        float  energy = 1.0f;       // 0.6 chill .. 1.4 going off
    };
    std::vector<Dancer>                           m_dancers;
    // Running animation clock (seconds) advanced by update().
    float                                         m_time = 0.0f;
    // Inert character prop systems (own the GLB GPU handles for the app lifetime).
    std::vector<std::unique_ptr<MonsterSystem>>   m_chars;
};

// OLED SCREEN-CONTRAST PROBE (the regression guard for the washed-out-slab bug).
//
// A texture probe is NOT enough here, and that is the whole trap: the EQ frames
// ALWAYS had content baked into them. What made the club's screens read as flat
// milky slabs was the flat EMISSIVE — a uniform add over the pane that lifted the
// black substrate to the same brightness as the lit columns. A test that only
// checked "the texture has pixels" would have passed happily on the broken build.
//
// So this probe measures what actually broke: it runs GLASS.FRAG'S OWN EMISSIVE MATH
//   emisMask = mix(vec3(1), texel, emissiveMap);  additive = emissive.rgb * a * emisMask
// on the DARKEST and the BRIGHTEST texel of a real baked EQ frame, and returns the
// ratio brightest:darkest — the on-screen dynamic range of the panel.
//   * emissiveMap = 0 (the old flat path) -> the mask is 1 everywhere, both texels
//     emit identically, and this returns ~1.0: A SLAB. That is the bug, quantified.
//   * emissiveMap = 1 -> the ratio is the texture's own range (>>1): A DISPLAY.
// The self-test asserts a high ratio for the shipping settings AND asserts the ~1.0
// for emissiveMap=0 as a NEGATIVE CONTROL, which proves the probe can actually fail.
float clubOledEmissiveContrast(int hue, float emissiveMap, const float emissive[4]);

// Headless self-test for `--test-club`: build the club at Y=-200 with a stub
// render/physics device (no window / no Vulkan), assert the key fixtures exist
// (DJ booth + turntables + OLED + keypad door, the ORB, aerial + ground bar with 7
// stools, the 12-step stair, the PA rig, 28 blacklights, 6 TVs, the dance floor,
// the room footprint + Y=-200), tick a few frames, and confirm it is leak-clean.
// Logs "club: X/Y passed" and returns true iff all pass. Lives in club1127.cpp.
bool runClubSelfTest();

} // namespace x3::game
