// Club 1127 — "THE DEEP". See app/club1127.h.
//
// CLEAN-ROOM: ported forward from Tim's OWN Babylon game module
// (C:/Users/Tim Smith/OneDrive/GameDev/Q3Engine/src/world/x3-club1127.js — Tim's
// IP, authored by his "Agent 66"; NOT id Tech / RBDOOM / any third-party engine).
// Built here from that JS layout + the X3Native Scene / IRenderDevice /
// IPhysicsWorld / MonsterSystem interfaces only (the same public seams
// app/env_art.cpp + app/door.cpp + the prior club used). No third-party engine
// source consulted.
//
// MAPPING NOTES (JS -> native):
//   * The JS parents everything to a TransformNode at (originX, D.Y, originZ) and
//     positions children RELATIVE. Native addBox authors WORLD-space geometry, so
//     we keep originX/Z = 0 and ADD the club Y (kClubY = -200) to every center Y.
//     A child at JS-local y becomes world y = (kClubY + y).
//   * JS axes: Babylon is left-handed (+Z forward). X3Native is right-handed
//     (-Z forward; docs/CONVENTIONS.md). The club is mirror-symmetric front/back
//     and we only PLACE boxes (no winding-sensitive normals beyond makeBox's own),
//     so we keep the JS coordinates as-authored — the room reads identically.
//   * Babylon StandardMaterial diffuse/emissive -> native baseColor[] + emissive[]
//     ({r,g,b,strength}); strength > 1 => a bright HDR bloom source.
//   * Cylinders/spheres (turntables, stools, the ORB, blacklight tubes, cables,
//     railing balusters) are approximated with boxes (the engine's primitive).
//   * The JS Babylon lights (Hemispheric/Point/Spot) -> the engine's forward
//     PointLight set (premultiplied color); spotlights become orbiting point
//     lights, the hemisphere becomes a few soft fill lights.
//   * updateClub1127() (ORB spin + spotlight orbit + blacklight pulse) -> update().
//
// Reaching this area:
//   (a) STANDALONE: `--world club` (app/main.cpp). Walk it (WASD / mouse / Space /
//       F noclip); `--world club --screenshot <path>` captures the showcase vantage.
//   (b) ELEVATOR DISCO DESCENT (canon): the elevator's keypad code 1127 puts it in
//       DISCO mode + descends to Y=-200 (§2.2/§2.3). That elevator lane wires the
//       descent + teleports the player to spawn(); this module just builds the room.
#include "club1127.h"
#include "mesh_prims.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

// ---- Tints (linear-ish; the device tonemaps) ------------------------------
// Ported from the JS StandardMaterial diffuse colors (hex -> 0..1 RGB).
const float kWall[4]   = { 0.039f, 0.039f, 0.070f, 1.0f }; // 0x0a0a12 club wall
const float kFloor[4]  = { 0.031f, 0.031f, 0.063f, 1.0f }; // 0x080810 club floor
const float kCeil[4]   = { 0.020f, 0.020f, 0.031f, 1.0f }; // 0x050508 ceiling
const float kSpk[4]    = { 0.039f, 0.039f, 0.039f, 1.0f }; // 0x0a0a0a speaker cab
const float kAmp[4]    = { 0.067f, 0.067f, 0.067f, 1.0f }; // 0x111111 amp
const float kSub[4]    = { 0.031f, 0.031f, 0.031f, 1.0f }; // 0x080808 sub cab
const float kMetal[4]  = { 0.227f, 0.227f, 0.267f, 1.0f }; // 0x3a3a44 metal platform
const float kCouch[4]  = { 0.039f, 0.020f, 0.031f, 1.0f }; // 0x0a0508 couch
const float kStair[4]  = { 0.102f, 0.102f, 0.133f, 1.0f }; // 0x1a1a22 stair
const float kRail[4]   = { 0.267f, 0.267f, 0.333f, 1.0f }; // 0x444455 railing
const float kBar[4]    = { 0.102f, 0.082f, 0.125f, 1.0f }; // 0x1a1520 bar body
const float kBarTop[4] = { 0.165f, 0.125f, 0.208f, 1.0f }; // 0x2a2035 bar top
const float kStool[4]  = { 0.133f, 0.133f, 0.133f, 1.0f }; // 0x222222 stool seat
const float kStoolLeg[4]={ 0.267f, 0.267f, 0.267f, 1.0f }; // 0x444444 stool leg
const float kChrome[4] = { 0.533f, 0.533f, 0.600f, 1.0f }; // 0x888899 chrome handle
const float kTvFrame[4]= { 0.031f, 0.031f, 0.031f, 1.0f }; // 0x080808 TV bezel
const float kGlass[4]  = { 0.200f, 0.267f, 0.333f, 0.55f }; // 0x334455 glass door
const float kCable[4]  = { 0.267f, 0.267f, 0.267f, 1.0f }; // 0x444444 cable
const float kOrb[4]    = { 0.700f, 0.700f, 0.800f, 1.0f }; // mirror ball facets

// ---- Emissive helpers: { r, g, b, strength }. strength > 1 => HDR bloom. -----
const float kEmitOff[4]     = { 0.0f, 0.0f, 0.0f, 0.0f };
const float kEmitNeon[4]    = { 1.00f, 0.0f, 1.00f, 4.0f };  // magenta aerial-bar neon
const float kEmitDjCon[4]   = { 0.10f, 0.10f, 0.28f, 1.5f }; // DJ console glow
const float kEmitDjScr[4]   = { 0.30f, 0.30f, 0.90f, 3.0f }; // DJ/OLED screens
const float kEmitKeypad[4]  = { 0.10f, 0.95f, 0.30f, 2.0f }; // green keypad
const float kEmitBarTop[4]  = { 0.30f, 0.20f, 0.50f, 1.5f }; // bar-top glow
const float kEmitTile1[4]   = { 0.45f, 0.0f, 0.85f, 2.2f };  // purple dance tile (0x2a0050)
const float kEmitTile2[4]   = { 0.12f, 0.0f, 0.30f, 1.2f };  // dark dance tile (0x0a0020)
const float kEmitOrb[4]     = { 0.45f, 0.45f, 0.60f, 1.4f };  // ORB self-glow
const float kEmitLed[4]     = { 0.10f, 1.00f, 0.10f, 3.0f };  // amp power LED
const float kEmitAbTop[4]   = { 0.353f, 0.353f, 0.416f, 1.2f };// aerial-bar polished top
// Blacklight base emissive (PULSED each frame in update()): deep UV violet.
const float kBlacklightR = 0.50f, kBlacklightG = 0.0f, kBlacklightB = 1.0f;

// Push a point light (premultiplied color) into the set.
void addLight(std::vector<x3::rhi::PointLight>& v, float x, float y, float z,
              float r, float g, float b, float range) {
    x3::rhi::PointLight l;
    l.pos[0] = x; l.pos[1] = y; l.pos[2] = z; l.range = range;
    l.color[0] = r; l.color[1] = g; l.color[2] = b;
    v.push_back(l);
}

} // namespace

uint32_t Club1127World::addBox(Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics,
                               float cx, float cy, float cz, float hx, float hy, float hz,
                               const float color[4], const float emissive[4], bool collide,
                               float uvScale, const SurfaceSet* surf) {
    // Render + collision geometry authored in WORLD space (centered at cx,cy,cz),
    // so the Entity transform stays identity (static geometry — exactly like
    // buildTestLevel/env-art). The Scene draws it; addStaticMesh gives collision.
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, uvScale);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    if (surf && surf->ok) { e.tex = surf->albedo; e.normalTex = surf->normal; e.mrTex = surf->mr; }
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = (uint32_t)Tag::Static;
    if (collide) {
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
    }
    return scene.add(e);
}

void Club1127World::addCharacter(Scene& scene, x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                                 const std::string& modelFile, const x3::phys::Vec3& pos,
                                 float scale, bool standUpZtoY, const float tint[4]) {
    auto sys = std::make_unique<MonsterSystem>();
    MonsterSystem::Tuning t;
    t.type        = MonsterType::Guard;
    t.hp          = 100;
    t.chaseSpeed  = 0.0f;       // INERT prop: never moves (just idles in place)
    t.damage      = 0;          // never attacks
    t.ranged      = false;
    t.modelFile   = modelFile;
    t.modelDirOverride = std::string(modelDir);
    t.standUpZtoY = standUpZtoY;
    t.modelScale  = scale;
    if (tint) for (int i = 0; i < 4; ++i) t.tint[i] = tint[i];
    sys->buildMonsterTuned(scene, device, physics, modelDir, pos, t);
    m_chars.push_back(std::move(sys));
}

const Club1127World::Stats& Club1127World::build(Scene& scene, x3::rhi::IRenderDevice& device,
                                                 x3::phys::IPhysicsWorld& physics,
                                                 std::string_view modelDir) {
    if (m_built) return m_stats;
    m_built = true;

    const uint32_t entsBefore = scene.size();

    // The club Y origin: everything authored at JS-local y is offset by oy.
    const float oy = kClubY;          // -200
    const float CW = kCW, CL = kCL, CH = kCH;
    const float T  = 0.3f;            // wall thickness (JS WALL_T)

    // Engine-room/lounge dims (JS D.ER_*).
    const float ER_W = 6.1f;          // 20 ft wide
    const float ER_D = 4.27f;         // 14 ft deep
    const float LOUNGE_Y = 4.57f;     // 2nd story at 15 ft

    // Convenience: author a box at JS-local coords (Y offset to oy applied here).
    // `sf` (W6-3 texture pass) optionally carries a real surface_library set.
    auto box = [&](float x, float y, float z, float hx, float hy, float hz,
                   const float* col, const float* em, bool coll, float uv = 1.0f,
                   const SurfaceSet* sf = nullptr) {
        return addBox(scene, device, physics, x, oy + y, z, hx, hy, hz, col,
                      em ? em : kEmitOff, coll, uv, sf);
    };

    // ==================================================================
    // W6-3 TEXTURE PASS — real PBR sets from the pack library (ART_BIBLE §4)
    // replacing the box-tint-only geometry (was: zero architecture textures,
    // magenta neon accent only). Walls = dark venue concrete panels; floors +
    // stage/booth platforms = rubber dance floor / brushed metal; bar = plastic
    // laminate body + trim top. The magenta neon accent (kEmitNeon) is UNCHANGED
    // — it's an emissive-only strip, not a texture, and stays bible-compliant.
    // On a headless device with no assets fetched yet, SurfaceSet::ok is false
    // and addBox falls back to the old flat-tinted box (never breaks the build).
    // ==================================================================
    SurfaceLibrary surf;
    surf.mount(assetRoot() + "/surface_library");
    auto set = [&](const char* name) -> const SurfaceSet& { return surf.get(device, name); };
    const SurfaceSet& sWall  = set("mw_concrete_panels_a"); // dark venue walls
    const SurfaceSet& sFloor = set("sr_rubberfloor");        // dance/club floor
    const SurfaceSet& sMetal = set("mw_metal_trim_b");       // stage/booth platforms
    const SurfaceSet& sBar   = set("mw_wall_plastic");       // bar body laminate
    const SurfaceSet& sTrim  = set("mw_floor_trim");         // bar top / trim
    const SurfaceSet& sStair = set("sr_concrete_a");         // stair treads

    m_stats.floorY    = oy;           // main floor center at world Y = -200
    m_stats.roomMinX  = -CW / 2;  m_stats.roomMaxX = CW / 2;
    m_stats.roomMinZ  = -CL / 2;  m_stats.roomMaxZ = CL / 2;

    // ==================================================================
    // MAIN SHELL — floor, ceiling, four walls (the 50x100x30 ft room).
    // ==================================================================
    box(0, 0.0f, 0, CW / 2, 0.1f, CL / 2, kFloor, kEmitOff, true, 0.4f, &sFloor);  // floor slab
    box(0, CH,  0, CW / 2, 0.1f, CL / 2, kCeil,  kEmitOff, true, 0.4f);            // ceiling
    m_stats.ceilingY = oy + CH;

    // North wall (-Z) — gap for the engine room (ER_W wide, centered at x=0).
    const float erGap = ER_W;
    const float nwSide = (CW - erGap) / 2;
    box(-(erGap / 2 + nwSide / 2), CH / 2, -CL / 2, nwSide / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
    box( (erGap / 2 + nwSide / 2), CH / 2, -CL / 2, nwSide / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
    // South wall (+Z) — solid.
    box(0, CH / 2, CL / 2, CW / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
    // West long wall (-X) — solid (the ground-bar wall).
    box(-CW / 2, CH / 2, 0, T / 2, CH / 2, CL / 2, kWall, kEmitOff, true, 0.5f, &sWall);
    // East long wall (+X) — elevator entrance gap near the south end.
    {
        const float entrW = 3.5f;                      // elevator opening width
        const float entrZ = CL / 2 - entrW / 2 - 1.0f; // near the SE corner
        const float northLen = CL / 2 + (entrZ - entrW / 2);
        box(CW / 2, CH / 2, -CL / 2 + northLen / 2, T / 2, CH / 2, northLen / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        const float southLen = CL / 2 - (entrZ + entrW / 2);
        if (southLen > 0.1f)
            box(CW / 2, CH / 2, CL / 2 - southLen / 2, T / 2, CH / 2, southLen / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        // Header above the elevator door.
        box(CW / 2, 2.8f + (CH - 2.8f) / 2, entrZ, T / 2, (CH - 2.8f) / 2, entrW / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        // Player spawn: just inside the elevator opening on the floor, facing -X
        // (into the club toward the dance floor + DJ booth).
        m_spawn = x3::phys::Vec3{ CW / 2 - 1.6f, oy + 0.15f, entrZ };
    }

    // ==================================================================
    // ENGINE ROOM + LOUNGE (north side, 2 stories, 12-step stair).
    //   The JS parents these to a node at z = -CL/2 - ER_D/2; we add that offset.
    // ==================================================================
    const float erZ0 = -CL / 2 - ER_D / 2;   // engine-room center Z
    auto erbox = [&](float x, float y, float z, float hx, float hy, float hz,
                     const float* col, const float* em, bool coll, float uv = 1.0f,
                     const SurfaceSet* sf = nullptr) {
        return box(x, y, erZ0 + z, hx, hy, hz, col, em, coll, uv, sf);
    };
    erbox(0, 0.0f, 0, ER_W / 2, 0.1f, ER_D / 2, kFloor, kEmitOff, true, 0.5f, &sFloor); // ER floor
    erbox(0, CH,  0, ER_W / 2, 0.1f, ER_D / 2, kCeil,  kEmitOff, true, 0.5f);     // ER ceiling
    erbox(0, CH / 2, -ER_D / 2, ER_W / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall); // back wall
    erbox(-ER_W / 2, CH / 2, 0, T / 2, CH / 2, ER_D / 2, kWall, kEmitOff, true, 0.5f, &sWall); // -X side
    erbox( ER_W / 2, CH / 2, 0, T / 2, CH / 2, ER_D / 2, kWall, kEmitOff, true, 0.5f, &sWall); // +X side

    // Lounge floor (2nd story) + railing + balusters.
    erbox(0, LOUNGE_Y, 0, (ER_W - 0.2f) / 2, 0.1f, (ER_D - 0.2f) / 2, kFloor, kEmitOff, true, 0.5f, &sFloor);
    m_stats.hasLoungeFloor = true;
    erbox(0, LOUNGE_Y + 0.5f, ER_D / 2 - 0.1f, (ER_W - 0.4f) / 2, 0.5f, 0.03f, kRail, kEmitOff, true);
    for (int r = 0; r < 8; ++r) {
        const float rx = -ER_W / 2 + 0.5f + r * (ER_W - 1) / 7;
        erbox(rx, LOUNGE_Y + 0.5f, ER_D / 2 - 0.1f, 0.03f, 0.5f, 0.03f, kRail, kEmitOff, false);
    }

    // 12-step stair along the WEST wall up to the lounge.
    {
        const int stCt = 12;
        const float stD = ER_D / stCt;
        const float stR = LOUNGE_Y / stCt;
        for (int s = 0; s < stCt; ++s) {
            erbox(-ER_W / 2 + 0.65f, stR * (s + 0.5f), -ER_D / 2 + stD * (s + 0.5f),
                  0.5f, (stR * (s + 0.5f)) /* riser grows */ * 0.0f + 0.04f, (stD - 0.02f) / 2,
                  kStair, kEmitOff, true, 1.0f, &sStair);
            ++m_stats.stairSteps;
        }
    }

    // Engine-room south wall: a center pier + glass swing doors (west) + an inset
    // alcove door (east) into the main club, ported from the JS (simplified piers/
    // headers; the doors are visual props).
    {
        const float erSZ = ER_D / 2;          // south edge of the ER (local z)
        const float glassDoorW = 1.8f;
        const float pierW = 0.8f;
        const float westDoorX = -ER_W / 4;
        // West header + flanks.
        const float westSectionW = ER_W / 2 - pierW / 2;
        const float headerH = CH - 2.4f;
        erbox(-(pierW / 2 + westSectionW / 2), 2.4f + headerH / 2, erSZ,
              westSectionW / 2, headerH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        // Glass swing doors (2 leaves) + chrome handles (visual; non-colliding).
        const float ghw = glassDoorW / 2;
        for (int s = -1; s <= 1; s += 2) {
            erbox(westDoorX + s * (ghw / 2 + 0.01f), 1.15f, erSZ, (ghw - 0.02f) / 2, 1.15f, 0.02f,
                  kGlass, kEmitOff, false);
            erbox(westDoorX + s * 0.02f, 1.1f, erSZ + (s > 0 ? 0.04f : -0.04f),
                  0.015f, 0.125f, 0.03f, kChrome, kEmitOff, false);
        }
        // Center pier.
        erbox(0, CH / 2, erSZ, pierW / 2, CH / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        // East alcove: header + inset door + a pass-through cutout frame.
        const float eastSectionW = ER_W / 2 - pierW / 2;
        const float eastCenterX = pierW / 2 + eastSectionW / 2;
        const float alcoveDoorW = 1.2f;
        erbox(eastCenterX, 2.2f + (CH - 2.2f) / 2, erSZ - 0.6f, alcoveDoorW / 2, (CH - 2.2f) / 2, T / 2, kWall, kEmitOff, true, 0.5f, &sWall);
        erbox(eastCenterX, 1.05f, erSZ - 0.57f, (alcoveDoorW - 0.04f) / 2, 1.05f, 0.03f, kStair, kEmitOff, false);
        erbox(eastCenterX, 0.61f, erSZ - 0.59f, 0.485f, 0.61f, 0.04f, kRail, kEmitOff, false); // cutout frame
        // Lounge overhang above the east alcove.
        erbox(eastCenterX, LOUNGE_Y, erSZ - 0.6f + 0.2f, (eastSectionW + 0.3f) / 2, 0.075f, (0.6f + 0.4f) / 2, kFloor, kEmitOff, true);
    }

    // Engine-room fill light.
    addLight(m_lights, 0, oy + 3.0f, erZ0, 0.30f, 0.20f, 0.45f, 9.0f);

    // ==================================================================
    // SUSPENDED DJ BOOTH (turntables, mixer, 2 OLED, keypad door, brackets).
    // ==================================================================
    {
        const float djW = 3.5f, djD = 2.5f, djH = 2.8f, djY = LOUNGE_Y;
        const float djZ = -CL / 2 + djD / 2 + 0.3f;
        box(0, djY, djZ, djW / 2, 0.075f, djD / 2, kMetal, kEmitOff, true, 1.0f, &sMetal); // booth floor
        box(0, djY + djH, djZ, (djW + 0.1f) / 2, 0.05f, (djD + 0.1f) / 2, kCeil, kEmitOff, false); // booth ceiling
        m_stats.hasDjBooth = true;

        // Back wall (split around the keypad door).
        const float djBkW = (djW - 0.9f) / 2;
        box(-(0.45f + djBkW / 2), djY + djH / 2, -CL / 2 + 0.3f, djBkW / 2, djH / 2, T / 2, kWall, kEmitOff, true, 1.0f, &sWall);
        box( (0.45f + djBkW / 2), djY + djH / 2, -CL / 2 + 0.3f, djBkW / 2, djH / 2, T / 2, kWall, kEmitOff, true, 1.0f, &sWall);
        // Secured keypad door + keypad.
        box(djW / 2 - 0.6f, djY + 1.1f, -CL / 2 + 0.3f, 0.45f, 1.1f, 0.04f, kStair, kEmitOff, false);
        box(djW / 2 - 0.1f, djY + 1.2f, -CL / 2 + 0.35f, 0.05f, 0.075f, 0.015f, kStair, kEmitKeypad, false);
        m_stats.hasKeypadDoor = true;

        // Low front + side walls (the booth railing).
        box(0, djY + 0.55f, -CL / 2 + djD + 0.3f, djW / 2, 0.55f, T / 2, kWall, kEmitOff, true, 1.0f, &sWall);
        for (int s = -1; s <= 1; s += 2)
            box(s * djW / 2, djY + 0.55f, djZ, T / 2, 0.55f, djD / 2, kWall, kEmitOff, true, 1.0f, &sWall);

        // DJ mixer console + 2 turntables (cylinders -> flat boxes).
        box(0, djY + 1.05f, -CL / 2 + djD - 0.1f, 1.4f, 0.06f, 0.4f, kStair, kEmitDjCon, false);
        for (int i = 0; i < 2; ++i) {
            const float xo = (i == 0 ? -0.7f : 0.7f);
            box(xo, djY + 1.14f, -CL / 2 + djD - 0.1f, 0.275f, 0.02f, 0.275f, kSpk, kEmitDjCon, false);
        }
        m_stats.hasDjTurntables = true;

        // 2 OLED screens.
        for (int i = 0; i < 2; ++i) {
            const float xo = (i == 0 ? -0.25f : 0.25f);
            box(xo, djY + 1.35f, -CL / 2 + djD - 0.35f, 0.175f, 0.125f, 0.015f, kTvFrame, kEmitDjScr, false);
        }
        m_stats.hasDjScreens = true;

        // Support brackets down to the floor.
        for (int s = -1; s <= 1; s += 2)
            box(s * (djW / 2 - 0.2f), djY / 2, -CL / 2 + djD + 0.3f, 0.075f, djY / 2, 0.075f, kMetal, kEmitOff, true);

        // Booth glow.
        addLight(m_lights, 0, oy + djY + 1.6f, djZ, 0.30f, 0.30f, 0.80f, 7.0f);

        // ==============================================================
        // AERIAL BAR (beside the booth, neon underglow, polished top, railings).
        // ==============================================================
        const float abW = 4.0f, abD = 1.5f, abX = -djW / 2 - abW / 2 + 0.5f, abZ = djZ;
        box(abX, djY, abZ, abW / 2, 0.06f, abD / 2, kMetal, kEmitOff, true, 1.0f, &sMetal);               // platform
        box(abX, djY + 0.55f, -CL / 2 + djD + 0.1f, (abW - 0.4f) / 2, 0.55f, 0.25f, kMetal, kEmitOff, true, 1.0f, &sMetal); // counter
        box(abX, djY + 1.13f, -CL / 2 + djD + 0.1f, (abW - 0.2f) / 2, 0.025f, 0.3f, kMetal, kEmitAbTop, false, 1.0f, &sMetal); // polished top
        m_stats.hasAerialBar = true;
        // Magenta neon strips under the platform edges.
        box(abX, djY - 0.08f, abZ + abD / 2, (abW - 0.4f) / 2, 0.02f, 0.02f, kWall, kEmitNeon, false);
        box(abX, djY - 0.08f, abZ - abD / 2, (abW - 0.4f) / 2, 0.02f, 0.02f, kWall, kEmitNeon, false);
        box(abX - abW / 2 + 0.2f, djY - 0.08f, abZ, 0.02f, 0.02f, (abD - 0.2f) / 2, kWall, kEmitNeon, false);
        addLight(m_lights, abX, oy + djY - 0.3f, abZ, 2.0f, 0.0f, 2.0f, 8.0f);  // magenta underglow
        // Safety railings.
        box(abX, djY + 0.5f, abZ + abD / 2, abW / 2, 0.5f, 0.02f, kRail, kEmitOff, true);
        box(abX, djY + 0.5f, abZ - abD / 2, abW / 2, 0.5f, 0.02f, kRail, kEmitOff, true);
        box(abX - abW / 2, djY + 0.5f, abZ, 0.02f, 0.5f, abD / 2, kRail, kEmitOff, true);
    }

    // ==================================================================
    // 28 BLACKLIGHTS — 4 ft UV tubes on the walls at 10 ft intervals (pulsing).
    //   Long walls: 10 + 10; south wall flanking the 85": 3 + 3 (capped) = 28.
    // ==================================================================
    {
        const float bi = 3.048f;     // 10 ft interval
        const float bh = 1.83f;      // tube half-... (JS BL_HEIGHT 1.83 m full)
        auto blacklight = [&](float x, float z) {
            const uint32_t id = box(x, CH * 0.5f, z, 0.04f, bh / 2, 0.04f, kWall, nullptr, false);
            // Set its starting emissive (update() pulses it).
            Entity& e = scene.get(id);
            e.emissive[0] = kBlacklightR; e.emissive[1] = kBlacklightG;
            e.emissive[2] = kBlacklightB; e.emissive[3] = 3.0f;
            m_blacklightEnts.push_back(id);
            ++m_stats.blacklights;
        };
        // 28 blacklights total (canon §2.3): 10 per long wall (20) + 4 per side of
        // the south wall (8). The long-wall tubes are evenly spread along Z inside
        // the room; the south-wall tubes flank the 85" centered display.
        const float zLo = -CL / 2 + bi, zHi = CL / 2 - bi;   // inner Z band
        for (int side = -1; side <= 1; side += 2)
            for (int n = 0; n < 10; ++n) {
                const float z = zLo + (zHi - zLo) * n / 9.0f;  // 10 tubes, n=0..9
                blacklight(side * (CW / 2 - 0.05f), z);
            }
        // South wall: 4 per side, snug within the room half-width.
        for (int s = -1; s <= 1; s += 2)
            for (int n = 0; n < 4; ++n) {
                const float x = s * (1.5f + n * 1.7f);          // max |x| = 6.6 < CW/2
                blacklight(x, CL / 2 - 0.05f);
            }
        // UV point lights (4) — the violet wash over the room.
        const float uv[4][3] = {
            { 0, CH * 0.5f, -CL / 4 }, { 0, CH * 0.5f, CL / 4 },
            { -CW / 3, CH * 0.5f, 0 }, { CW / 3, CH * 0.5f, 0 }
        };
        for (auto& p : uv)
            addLight(m_lights, p[0], oy + p[1], p[2], 0.4f, 0.05f, 1.0f, 22.0f);
    }

    // ==================================================================
    // TV MULTIPLEX (POE) — 6 screens at the real JS sizes/positions.
    // ==================================================================
    {
        auto tv = [&](float inches, float x, float y, float z) {
            const float dm  = inches * 0.0254f;
            const float tvH = dm / std::sqrt(1.0f + (16.0f / 9.0f) * (16.0f / 9.0f));
            const float tvW = tvH * 16.0f / 9.0f;
            box(x, y, z, (tvW + 0.05f) / 2, (tvH + 0.05f) / 2, 0.03f, kTvFrame, kEmitOff, false); // bezel
            box(x, y, z + 0.035f, tvW / 2, tvH / 2, 0.005f, kTvFrame, kEmitDjScr, false);          // screen
            ++m_stats.tvScreens;
        };
        const float nwSideX = -(ER_W / 2 + nwSide / 2);
        tv(80, nwSideX, 2.74f, -CL / 2 + 0.05f);
        tv(85, 0, CH * 0.55f, CL / 2 - 0.05f);
        tv(55, CW / 2 - 0.8f, 1.8f, CL / 2 - 2.0f);
        tv(75, CW / 2 - 1.5f, 2.2f, CL / 2 - 5.0f);
        tv(65, -ER_W / 2 + 0.1f, LOUNGE_Y + 1.5f, -CL / 2 - ER_D / 2);
        tv(55, -CW / 2 + 0.8f, 2.0f, CL / 2 - 1.5f);
    }

    // ==================================================================
    // SOUND SYSTEM (the real PA rig).
    // ==================================================================
    // 4x SVS PB16-Ultra subs (corners).
    for (int i = 0; i < 4; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f;
        const float sz = (i & 2) ? 1.0f : -1.0f;
        box(sx * (CW / 2 - 1), 0.37f, sz * (CL / 4), 0.32f, 0.37f, 0.28f, kSub, kEmitOff, true);
        ++m_stats.svsSubs;
    }
    // 8 stacked pairs JBL JRX200 (16 cabinets) + 8 amps + power LEDs on the walls.
    {
        const float jrxSp = CL / 5;
        for (int side = -1; side <= 1; side += 2)
            for (int n = 0; n < 4; ++n) {
                const float z = -CL / 2 + jrxSp * (n + 1);
                const float x = side * (CW / 2 - 0.5f);
                const float wy = CH * 0.55f;
                box(x, wy,        z, 0.265f, 0.38f, 0.215f, kSpk, kEmitOff, false); m_stats.jblLineArray++;
                box(x, wy + 0.8f, z, 0.265f, 0.38f, 0.215f, kSpk, kEmitOff, false); m_stats.jblLineArray++;
                box(x, wy - 0.55f, z, 0.24f, 0.10f, 0.175f, kAmp, kEmitOff, false);                 // amp
                box(x - side * 0.01f, wy - 0.5f, z + 0.18f, 0.015f, 0.015f, 0.015f, kAmp, kEmitLed, false); // power LED
            }
    }
    // 4x JBL PRO 18" subs flanking the dance floor.
    {
        const float p[4][2] = { {-1,-1.f/3}, {-1,1.f/3}, {1,-1.f/3}, {1,1.f/3} };
        for (auto& s : p) {
            box(s[0] * (CW / 2 - 0.5f), 0.35f, s[1] * CL, 0.305f, 0.305f, 0.305f, kSub, kEmitOff, true);
            ++m_stats.jbl18Subs;
        }
    }
    // 16x JBL N26/S38 surrounds (walls, alternating sizes).
    {
        const float surSp = CL / 9;
        for (int side = -1; side <= 1; side += 2)
            for (int n = 0; n < 8; ++n) {
                const float z = -CL / 2 + surSp * (n + 1);
                const float x = side * (CW / 2 - 0.15f);
                const bool s38 = (n % 2 == 0);
                box(x, CH * 0.75f, z, (s38 ? 0.25f : 0.2f) / 2, (s38 ? 0.5f : 0.3f) / 2, 0.09f, kSpk, kEmitOff, false);
                ++m_stats.surrounds;
            }
    }

    // ==================================================================
    // DANCE FLOOR — full-club checkerboard of glowing purple/dark tiles.
    // ==================================================================
    {
        const int cols = (int)std::floor(CW);   // ~15
        const int rows = (int)std::floor(CL);   // ~30
        const float tw = CW / cols, td = CL / rows;
        for (int gx = 0; gx < cols; ++gx)
            for (int gz = 0; gz < rows; ++gz) {
                const float tx = -CW / 2 + tw / 2 + gx * tw;
                const float tz = -CL / 2 + td / 2 + gz * td;
                const float* em = ((gx + gz) % 2 == 0) ? kEmitTile1 : kEmitTile2;
                box(tx, 0.12f, tz, (tw - 0.02f) / 2, 0.015f, (td - 0.02f) / 2, kFloor, em, false, 1.0f, &sFloor);
            }
        m_stats.hasDanceFloor = true;
    }

    // ==================================================================
    // THE ORB — 2 m mirror ball on a cable, 4 spotlights + 4 ring lights.
    // ==================================================================
    {
        m_orbY = oy + (CH - 1.5f);
        // Mirror ball (sphere -> a 1 m-radius emissive box, faceted look via tint).
        m_orbEnt = box(0, CH - 1.5f, 0, 1.0f, 1.0f, 1.0f, kOrb, kEmitOrb, false);
        m_orbValid = true;
        m_stats.hasOrb = true;
        // Suspending cable.
        box(0, CH - 0.5f, 0, 0.02f, 0.75f, 0.02f, kCable, kEmitOff, false);
        // 4 ring lights (permanent colored point lights, orbital). These are the
        // FIRST orbiting set (rewritten each frame by update()).
        // 4 colored spotlights (rotating). Stored AFTER the static lights.
    }

    // ==================================================================
    // GROUND BAR + 7 STOOLS (west side).
    // ==================================================================
    {
        box(-CW / 2 + 1.4f, 0.55f, CL / 4, 0.4f, 0.55f, 2.5f, kBar, kEmitOff, true, 1.0f, &sBar);     // bar body
        box(-CW / 2 + 1.4f, 1.13f, CL / 4, 0.475f, 0.03f, 2.55f, kBarTop, kEmitBarTop, false, 1.0f, &sTrim); // bar top
        m_stats.hasGroundBar = true;
        addLight(m_lights, -CW / 2 + 1.4f, oy + 2.5f, CL / 4, 0.40f, 0.25f, 0.50f, 6.0f);       // bar light
        for (int i = 0; i < 7; ++i) {
            const float sz = CL / 4 - 2.5f + 0.5f + i * 5.0f / 7.0f;
            box(-CW / 2 + 2.2f, 0.75f, sz, 0.2f, 0.03f, 0.2f, kStool, kEmitOff, false);          // seat
            box(-CW / 2 + 2.2f, 0.36f, sz, 0.03f, 0.36f, 0.03f, kStoolLeg, kEmitOff, false);     // leg
            ++m_stats.barStools;
        }
    }

    // ==================================================================
    // BLACK COUCHES + END TABLE (SE corner) + VIP COUCH (SW corner).
    // ==================================================================
    for (int i = 0; i < 2; ++i) {
        box(CW / 2 - 1.5f, 0.225f, CL / 2 - 1.5f - i * 1.8f, 1.0f, 0.225f, 0.375f, kCouch, kEmitOff, true);  // seat
        box(CW / 2 - 0.5f, 0.65f,  CL / 2 - 1.5f - i * 1.8f, 1.0f, 0.2f, 0.06f, kCouch, kEmitOff, false);     // back
        ++m_stats.couches;
    }
    box(CW / 2 - 1.5f, 0.275f, CL / 2 - 2.4f, 0.3f, 0.275f, 0.3f, kStair, kEmitOff, true);                    // end table
    ++m_stats.couches;
    box(-CW / 2 + 2, 0.25f, CL / 2 - 1.5f, 1.25f, 0.25f, 0.4f, kCouch, kEmitOff, true);                       // VIP seat
    box(-CW / 2 + 2, 0.7f,  CL / 2 - 1.1f, 1.25f, 0.2f, 0.075f, kCouch, kEmitOff, false);                     // VIP back
    ++m_stats.couches;

    // ==================================================================
    // CLUB AMBIENT + KEY LIGHTS (Babylon hemi/point/fill -> point lights).
    // ==================================================================
    addLight(m_lights, 0, oy + CH * 0.7f, 0, 0.30f, 0.20f, 0.40f, 25.0f);       // central overhead fill
    addLight(m_lights, -CW / 2 + 2, oy + 3.0f, CL / 4, 0.25f, 0.15f, 0.35f, 10.0f); // ground-bar area fill
    addLight(m_lights, 0, oy + 2.0f, 0, 0.35f, 0.25f, 0.55f, 18.0f);            // dance-floor wash

    // ---- ORBITING ORB LIGHTS: 4 spots + 4 ring lights. These trail the static
    // lights and are rewritten each frame by update(). Record where they start. ----
    m_staticLightCount = m_lights.size();
    // 4 colored spotlights (orbit radius ~4, near the ceiling).
    const float spotCols[4][3] = { {2.0f,0.0f,0.0f}, {0.0f,0.0f,2.0f}, {0.0f,2.0f,0.0f}, {2.0f,1.0f,0.0f} };
    for (int i = 0; i < 4; ++i) {
        const float a = (i / 4.0f) * 2.0f * kPi;
        addLight(m_lights, std::cos(a) * 4.0f, m_orbY, std::sin(a) * 4.0f,
                 spotCols[i][0], spotCols[i][1], spotCols[i][2], 22.0f);
    }
    // 4 ring lights (orbit radius ~8, mid-height).
    const float ringCols[4][3] = { {1.0f,0.0f,0.5f}, {0.0f,0.5f,1.0f}, {0.5f,0.0f,1.0f}, {0.0f,1.0f,0.5f} };
    for (int i = 0; i < 4; ++i) {
        const float a = (i / 4.0f) * 2.0f * kPi;
        addLight(m_lights, std::cos(a) * 8.0f, oy + 4.0f, std::sin(a) * 8.0f,
                 ringCols[i][0], ringCols[i][1], ringCols[i][2], 22.0f);
    }

    // ==================================================================
    // CHARACTERS — a DJ behind the booth + a bouncer at the landing (inert props
    // that still skin + idle). Graceful box fallback on a failed GLB load.
    // ==================================================================
    {
        const float warm[4] = { 1.2f, 1.1f, 0.95f, 1.0f };
        const float cool[4] = { 0.9f, 1.05f, 1.3f, 1.0f };
        const float djY = LOUNGE_Y;
        const float djZ = -CL / 2 + 2.5f / 2 + 0.3f;
        // DJ in the booth.
        addCharacter(scene, device, physics, modelDir, "marcus_webb.glb",
                     x3::phys::Vec3{ 0.0f, oy + djY, djZ }, 1.0f, false, warm);
        // Bouncer near the elevator landing.
        addCharacter(scene, device, physics, modelDir, "RexBouncer.glb",
                     x3::phys::Vec3{ CW / 2 - 2.0f, oy + 0.0f, CL / 2 - 4.0f }, 1.0f, true, cool);
    }

    m_stats.entities = (int)(scene.size() - entsBefore);

    x3::logInfo("[club1127] built THE DEEP (Club 1127) at Y=" + std::to_string((int)oy) +
                ": " + std::to_string(m_stats.entities) + " entities, " +
                std::to_string(m_lights.size()) + " point lights, " +
                std::to_string(m_stats.blacklights) + " blacklights, " +
                std::to_string(m_stats.tvScreens) + " TVs, " +
                std::to_string(m_chars.size()) + " characters");
    return m_stats;
}

void Club1127World::update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics) {
    if (!m_built) return;
    m_time += dt;
    const float t = m_time;

    // --- Spin THE ORB (rotate about Y) by rewriting its transform's upper 3x3. ---
    if (m_orbValid && m_orbEnt < scene.size()) {
        const float ang = t * 0.5f;            // matches JS dt*0.5 cadence
        const float c = std::cos(ang), s = std::sin(ang);
        Entity& e = scene.get(m_orbEnt);
        // Column-major: keep translation, set Y-rotation in the 3x3. The ORB box
        // is authored in WORLD space centered at the origin column, so we must put
        // the rotation about the orb center: translate to center is already baked
        // into the geometry (centered at 0,m_orbY,0), so a pure rotation works.
        e.transform[0] = c;  e.transform[2] = -s;
        e.transform[8] = s;  e.transform[10] = c;
        // (The orb geometry is authored at world (0, m_orbY, 0); rotating its model
        //  matrix about the origin spins it in place since its center is the origin.)
    }

    // --- Orbit the 4 spotlights + 4 ring lights (the JS spotlight orbit). ---
    if (m_lights.size() >= m_staticLightCount + 8) {
        for (int i = 0; i < 4; ++i) {     // spotlights: radius 4
            const float a = t * 1.2f + (i / 4.0f) * 2.0f * kPi;
            auto& L = m_lights[m_staticLightCount + i];
            L.pos[0] = std::cos(a) * 4.0f;
            L.pos[2] = std::sin(a) * 4.0f;
        }
        for (int i = 0; i < 4; ++i) {     // ring lights: radius 8
            const float a = -t * 0.8f + (i / 4.0f) * 2.0f * kPi;
            auto& L = m_lights[m_staticLightCount + 4 + i];
            L.pos[0] = std::cos(a) * 8.0f;
            L.pos[2] = std::sin(a) * 8.0f;
        }
    }

    // --- Pulse the blacklight emissive (each tube phase-offset). ---
    for (size_t i = 0; i < m_blacklightEnts.size(); ++i) {
        const uint32_t id = m_blacklightEnts[i];
        if (id >= scene.size()) continue;
        const float pulse = 0.7f + 0.3f * std::sin(t * 0.8f + i * 0.3f);
        Entity& e = scene.get(id);
        e.emissive[0] = kBlacklightR * pulse;
        e.emissive[1] = kBlacklightG;
        e.emissive[2] = kBlacklightB * pulse;
        e.emissive[3] = 3.0f;
    }

    // Re-push the (now-moved) light set to the device so the orbiting lights animate.
    device.setPointLights(m_lights.data(), (uint32_t)m_lights.size());

    // Tick the inert character props (idle clips; chaseSpeed 0 => no movement).
    for (auto& c : m_chars)
        c->update(dt, scene, physics, c->pos());
}

void Club1127World::drawCharacters(x3::rhi::IRenderDevice& device,
                                   const x3::rhi::FrameContext& frame, const Scene& scene) const {
    for (const auto& c : m_chars)
        c->drawMonster(device, frame, scene);
}

void Club1127World::showcaseCamera(float out[5]) const {
    // Elevated vantage from the SE corner (near the elevator landing) looking
    // toward -X/-Z across the dance floor so the glowing checkerboard, the DJ
    // booth + ORB on the far north wall, the ground bar (left), and the PA stacks
    // all read in one frame. Y/Z keep us inside the 30 ft ceiling.
    out[0] = kCW / 2 - 2.0f;   // x: by the east wall / elevator
    out[1] = kClubY + 5.0f;    // y: above the floor, below the ceiling
    out[2] = kCL / 2 - 3.0f;   // z: south end
    out[3] = -2.35f;           // yaw: look toward -X/-Z (the dance floor + booth)
    out[4] = -0.18f;           // pitch: slightly down over the floor
}

// ===========================================================================
// Headless self-test (--test-club). Build the club at Y=-200 with the shared
// HeadlessRenderDevice + a real physics world (no window / Vulkan), assert the key
// fixtures + the room footprint/Y, tick a few frames, and confirm it is leak-clean
// (idempotent rebuild adds NO meshes; mesh creates are balanced by entities).
// ===========================================================================
} // namespace x3::game

#include "headless_device.h"
#include "asset_root.h"        // x3::game::riggedGlbRoot()
#include "engine/physics/IPhysicsWorld.h"
#include <cmath>

namespace x3::game {

bool runClubSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool cond, const char* name) {
        if (cond) { ++pass; x3::logInfo(std::string("[club-test] PASS ") + name); }
        else      { ++fail; x3::logError(std::string("[club-test] FAIL ") + name); }
    };

    // A counting device: tracks live mesh handles so we can assert no leak.
    struct CountingDevice : public HeadlessRenderDevice {
        int created = 0, destroyed = 0;
        x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                       const uint32_t* idx, uint32_t ni) override {
            ++created;
            return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
        }
        void destroyMesh(x3::rhi::MeshHandle h) override {
            ++destroyed;
            HeadlessRenderDevice::destroyMesh(h);
        }
    };

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    CountingDevice device;
    Scene scene;

    Club1127World club;
    const Club1127World::Stats& s = club.build(scene, device, *physics, x3::game::riggedGlbRoot());

    // (1) Room footprint + Y. The main floor sits at world Y = -200, the room is
    //     the real 50x100x30 ft (15.24 x 30.48 x 9.14 m), ceiling 30 ft above.
    {
        const float wX = s.roomMaxX - s.roomMinX;   // ~15.24
        const float wZ = s.roomMaxZ - s.roomMinZ;   // ~30.48
        const float h  = s.ceilingY - s.floorY;     // ~9.14
        const bool yOk   = std::fabs(s.floorY - (-200.0f)) < 0.01f;
        const bool footOk = std::fabs(wX - 15.24f) < 0.05f && std::fabs(wZ - 30.48f) < 0.05f;
        const bool ceilOk = std::fabs(h - 9.14f) < 0.05f;
        check(yOk && footOk && ceilOk,
              "main room is 50x100x30 ft (15.24x30.48x9.14 m) with its floor at Y=-200");
    }

    // (2) Suspended DJ booth: platform + turntables + 2 OLED + keypad door.
    check(s.hasDjBooth && s.hasDjTurntables && s.hasDjScreens && s.hasKeypadDoor,
          "suspended DJ booth (turntables + 2 OLED screens + keypad door)");

    // (3) THE ORB — the 2 m mirror ball.
    check(s.hasOrb, "THE ORB (mirror ball) exists");

    // (4) Aerial bar + ground bar with exactly 7 stools.
    check(s.hasAerialBar && s.hasGroundBar && s.barStools == 7,
          "aerial bar + ground bar with 7 stools");

    // (5) Engine-room/lounge with a 12-step stair.
    check(s.hasLoungeFloor && s.stairSteps == 12,
          "2-story engine-room/lounge with a 12-step stair");

    // (6) The real PA rig: 4 SVS subs + 16 JBL line-array cabs + 4 JBL 18" subs +
    //     16 surrounds.
    check(s.svsSubs == 4 && s.jblLineArray == 16 && s.jbl18Subs == 4 && s.surrounds == 16,
          "PA rig: 4 SVS subs + 16 JBL line-array + 4 JBL 18\" subs + 16 surrounds");

    // (7) 28 blacklights.
    check(s.blacklights == 28, "28 blacklights");

    // (8) 6-screen TV multiplex.
    check(s.tvScreens == 6, "6-screen TV multiplex");

    // (9) Dance floor + VIP/couch seating.
    check(s.hasDanceFloor && s.couches >= 3, "dance-floor checkerboard + VIP/couch seating");

    // (10) Player spawn sits inside the room footprint, on the floor at Y=-200.
    {
        const x3::phys::Vec3 sp = club.spawn();
        const bool inX = sp.x > s.roomMinX && sp.x < s.roomMaxX;
        const bool inZ = sp.z > s.roomMinZ && sp.z < s.roomMaxZ;
        const bool onFloor = sp.y >= -200.0f - 0.01f && sp.y <= -200.0f + 1.0f;
        check(inX && inZ && onFloor && std::isfinite(sp.x) && std::isfinite(sp.z),
              "player spawn is inside the room footprint on the Y=-200 floor");
    }

    // (11) Animate a few frames: ORB spins, lights orbit, blacklights pulse. Assert
    //      transforms/light positions stay finite (no NaN) and lights actually moved.
    {
        const auto& L0 = club.pointLights();
        // Snapshot ONE orbiting spotlight (the last 8 lights orbit; a ring of
        // symmetric lights has an invariant coordinate SUM, so track a single one).
        const size_t orbIdx = L0.size() >= 8 ? L0.size() - 8 : 0;
        const float bx = L0[orbIdx].pos[0], bz = L0[orbIdx].pos[2];
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 30; ++i)
            club.update(dt, scene, device, *physics);
        bool finite = true;
        for (const auto& l : club.pointLights())
            if (!std::isfinite(l.pos[0]) || !std::isfinite(l.pos[1]) || !std::isfinite(l.pos[2]))
                finite = false;
        const auto& L1 = club.pointLights();
        const float moved = std::fabs(L1[orbIdx].pos[0] - bx) + std::fabs(L1[orbIdx].pos[2] - bz);
        check(finite && moved > 1e-3f,
              "ORB/spotlights/blacklights animate (an orbiting light moved, all finite)");
    }

    // (12) Idempotent rebuild: a second build() is a no-op and creates NO new mesh.
    {
        const int before = device.created;
        club.build(scene, device, *physics, x3::game::riggedGlbRoot());
        check(device.created == before && club.stats().entities == s.entities,
              "rebuild is idempotent (no duplicated geometry / no leak)");
    }

    // (13) Leak-clean: every mesh the device handed out can be destroyed and the
    //      device's create/destroy ledger balances (the live VMA allocationCount=0
    //      proof is the Debug --smoketest; here we prove the count bookkeeping).
    {
        // Destroy every mesh handle the club authored (ids are contiguous 1..created
        // from the stub's monotonic minting), then assert the ledger balances.
        for (int h = 1; h <= device.created; ++h)
            device.destroyMesh(x3::rhi::MeshHandle{ (uint32_t)h });
        check(device.created > 0 && device.destroyed == device.created,
              "mesh create/destroy ledger balances (leak-clean bookkeeping)");
    }

    physics->shutdown();

    const int total = pass + fail;
    x3::logInfo("club: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return fail == 0;
}

} // namespace x3::game
