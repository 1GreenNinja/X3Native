// Club 1127 + the flooded cave/tunnel network. See app/club1127.h.
//
// Clean-room: built from the Scene + IRenderDevice + IPhysicsWorld + MonsterSystem
// interfaces only (the same public seams app/env_art.cpp + app/door.cpp use). No
// purchased C# / id Tech source consulted.
//
// ---- Reaching this area ----------------------------------------------------
//   (a) STANDALONE TEST: `--world club` (dispatched in app/main.cpp next to
//       `--world terrain`). Walk it (WASD / mouse / Space / F noclip); add
//       `--screenshot <path>` to capture the showcase vantage headlessly.
//   (b) CODE-1127 ENTRY FROM THE SPIRE (canon): the Spire's keypad already accepts
//       code 1127 (see app/level1_game.cpp tryDoorCode + main.cpp's codeMode). The
//       hook point is there: when 1127 is accepted at the SECRET club keypad
//       (rather than Door C), the host would teleport the player into a club
//       instance built by Club1127World::build() instead of opening a door. That
//       cross-level transition is intentionally NOT wired here to keep this change
//       low-conflict with level1.cpp; the `--world club` flag is enough to
//       build/verify the area. (Search main.cpp for "code-1127 hook point".)
//
// ---- Layout (Y-up, +X right, -Z forward; see docs/CONVENTIONS.md) ----------
//   CLUB (around the origin, floor at y=0):
//     * A ~26 x 22 m room, NOT a plain box: a sunken central DANCE FLOOR (glowing
//       neon-grid tiles) ringed by a raised walkway, with two upper CATWALKS /
//       BALCONIES along the +Z / -Z walls reached conceptually from the sides.
//     * A BAR counter along the -X wall with BartenderDanny behind it; RexBouncer
//       stands by the entrance landing. Neon strips + a hanging light rig over the
//       dance floor; magenta/cyan/violet point lights give the club its glow.
//     * A glassy mezzanine railing (translucent-looking emissive strips) cribbed
//       from the Showroom/ModularSciFi feel.
//   CAVE MOUTH + TUNNEL (off the club's +X wall):
//     * A jagged CAVE MOUTH breaks the +X wall; a sloped, arched TUNNEL descends
//       (~5 m down over ~20 m) toward the caverns — approximated with stacked,
//       jittered rock boxes that vary in size/height/angle so it reads organic.
//   CAVERNS (below + beyond, floor sloping down to ~y=-6):
//     * POWER-CORE cavern: an irregular chamber with a glowing core pillar +
//       teal crystal clusters (each crystal carries its own small point light,
//       LevelArchitect-style).
//     * FLOODED section: a shallow "water" slab (dark teal, emissive sheen) with a
//       GreatWhiteShark + a manta ray + a hammerhead lurking; lore-cache nooks in
//       the walls (glowing data-cache boxes).
//     * HIDDEN BOSS ARENA: a wider, taller cavern at the far end with the
//       BossTheSiren as the hidden boss, lit red.
// ---------------------------------------------------------------------------
#include "club1127.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

// Small deterministic LCG so the "organic" jitter is reproducible run-to-run
// (same caves every launch -> stable screenshots). Seeded once in build().
struct Rng {
    uint32_t s = 0x1127C0DEu;
    float next01() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFFFF) / 16777215.0f; }
    float range(float a, float b) { return a + (b - a) * next01(); }
};

// Common tints (linear-ish; the device tonemaps). Club neon is bright + saturated.
const float kFloorClub[4]  = { 0.10f, 0.10f, 0.14f, 1.0f }; // dark club floor
const float kWallClub[4]   = { 0.13f, 0.12f, 0.18f, 1.0f }; // moody club wall
const float kBarTop[4]     = { 0.20f, 0.16f, 0.10f, 1.0f }; // warm bar wood/metal
const float kRock[4]       = { 0.16f, 0.13f, 0.11f, 1.0f }; // cave rock (LevelArchitect caveMat)
const float kRockFloor[4]  = { 0.13f, 0.11f, 0.09f, 1.0f }; // cave floor
// (The flooded section now uses the engine's REAL animated water — driven per
//  frame via setWaterParams in Club1127World::tickWater() — not a static slab,
//  so the old kWater slab tint + kEmitWaterSheen glow are gone.)

// Emissive helpers: { r, g, b, strength }. strength > 1 => bright HDR bloom source.
const float kEmitOff[4]    = { 0.0f, 0.0f, 0.0f, 0.0f };
const float kEmitMagenta[4]= { 1.00f, 0.10f, 0.65f, 5.0f };
const float kEmitCyan[4]   = { 0.10f, 0.85f, 1.00f, 5.0f };
const float kEmitViolet[4] = { 0.55f, 0.20f, 1.00f, 4.5f };
const float kEmitNeonGrid[4]={ 0.20f, 0.70f, 1.00f, 3.0f }; // dance-floor tiles
const float kEmitCrystal[4]= { 0.15f, 0.95f, 0.85f, 4.0f }; // teal cave crystal
const float kEmitCore[4]   = { 0.40f, 0.85f, 1.00f, 7.0f }; // power core (very bright)
const float kEmitLore[4]   = { 0.95f, 0.80f, 0.20f, 4.0f }; // lore-cache amber
const float kEmitBoss[4]   = { 1.00f, 0.18f, 0.12f, 5.0f }; // boss-arena red

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
                               float uvScale) {
    // Render + collision geometry authored in WORLD space (centered at cx,cy,cz),
    // so the Entity transform stays identity (static geometry — exactly like
    // buildTestLevel/env-art). The Scene draws it; addStaticMesh gives collision.
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, uvScale);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = (uint32_t)Tag::Static;
    if (collide) {
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
    }
    // Authored transform is identity (geometry already world-placed).
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
    // Face roughly toward the club center / a fixed point so the prop has a
    // sensible heading; chaseSpeed 0 means it never actually pursues it.
    m_charFace.push_back(x3::phys::Vec3{ 0.0f, pos.y, 0.0f });
}

void Club1127World::build(Scene& scene, x3::rhi::IRenderDevice& device,
                          x3::phys::IPhysicsWorld& physics, std::string_view modelDir) {
    if (m_built) return;
    m_built = true;
    Rng rng;

    // ===================================================================
    // CLUB ROOM — a stylish multi-level neon space (NOT a plain box).
    //   Outer footprint: X in [-13, 13], Z in [-11, 11]. Ceiling ~7 m.
    //   Sunken dance floor: X in [-6, 6], Z in [-5, 5], floor at y=0; the
    //   surrounding walkway ring sits +0.6 m so the dance floor is a pit.
    // ===================================================================
    const float clubX0 = -13.0f, clubX1 = 13.0f;
    const float clubZ0 = -11.0f, clubZ1 = 11.0f;
    const float clubCeil = 7.0f;
    const float wallT = 0.4f;        // wall half-thickness
    const float ringY = 0.6f;        // raised walkway height (dance floor is sunken)

    // --- Raised walkway ring floor (the room floor the player walks at y=ringY),
    // built as a frame of four slabs around the sunken dance pit. ---
    const float pitX0 = -6.0f, pitX1 = 6.0f, pitZ0 = -5.0f, pitZ1 = 5.0f;
    auto slabRing = [&](float x0, float x1, float z0, float z1) {
        const float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
        addBox(scene, device, physics, cx, ringY - 0.15f, cz,
               (x1 - x0) * 0.5f, 0.15f, (z1 - z0) * 0.5f, kFloorClub, kEmitOff, true, 0.5f);
    };
    slabRing(clubX0, clubX1, pitZ1, clubZ1);   // +Z walkway band
    slabRing(clubX0, clubX1, clubZ0, pitZ0);   // -Z walkway band
    slabRing(clubX0, pitX0, pitZ0, pitZ1);     // -X walkway band
    slabRing(pitX1, clubX1, pitZ0, pitZ1);     // +X walkway band

    // --- Sunken DANCE FLOOR: a base slab at y=0 plus a grid of glowing neon
    // tiles. The grid alternates cyan/violet so it reads as a lit dance floor. ---
    addBox(scene, device, physics, 0.0f, -0.15f, 0.0f,
           (pitX1 - pitX0) * 0.5f, 0.15f, (pitZ1 - pitZ0) * 0.5f, kFloorClub, kEmitOff, true, 1.0f);
    {
        const int nx = 6, nz = 5;
        const float tw = (pitX1 - pitX0) / nx, td = (pitZ1 - pitZ0) / nz;
        for (int ix = 0; ix < nx; ++ix)
            for (int iz = 0; iz < nz; ++iz) {
                const float tx = pitX0 + (ix + 0.5f) * tw;
                const float tz = pitZ0 + (iz + 0.5f) * td;
                const bool on = ((ix ^ iz) & 1);
                const float* em = on ? kEmitCyan : kEmitViolet;
                // Thin glowing inlay tile flush with the dance floor.
                addBox(scene, device, physics, tx, 0.02f, tz,
                       tw * 0.42f, 0.03f, td * 0.42f, kFloorClub, em, false, 1.0f);
            }
    }

    // --- Outer walls (four slabs) + ceiling. The +X wall gets a CAVE-MOUTH gap. ---
    const float wallMidY = clubCeil * 0.5f;
    // -Z and +Z walls (full span along X).
    addBox(scene, device, physics, 0.0f, wallMidY, clubZ0 - wallT, (clubX1 - clubX0) * 0.5f + wallT, wallMidY, wallT, kWallClub, kEmitOff, true, 0.5f);
    addBox(scene, device, physics, 0.0f, wallMidY, clubZ1 + wallT, (clubX1 - clubX0) * 0.5f + wallT, wallMidY, wallT, kWallClub, kEmitOff, true, 0.5f);
    // -X wall (full span along Z) — the bar wall.
    addBox(scene, device, physics, clubX0 - wallT, wallMidY, 0.0f, wallT, wallMidY, (clubZ1 - clubZ0) * 0.5f, kWallClub, kEmitOff, true, 0.5f);
    // +X wall, SPLIT around a 4 m-wide cave-mouth gap centered at z=0 (y up to 4 m).
    //   lower lintel above the gap + two side posts so the wall still encloses.
    addBox(scene, device, physics, clubX1 + wallT, wallMidY, -7.5f, wallT, wallMidY, 3.5f, kWallClub, kEmitOff, true, 0.5f); // -Z post
    addBox(scene, device, physics, clubX1 + wallT, wallMidY,  7.5f, wallT, wallMidY, 3.5f, kWallClub, kEmitOff, true, 0.5f); // +Z post
    addBox(scene, device, physics, clubX1 + wallT, 5.5f, 0.0f, wallT, 1.5f, 4.0f, kWallClub, kEmitOff, true, 0.5f);          // lintel above gap
    // Ceiling lid.
    addBox(scene, device, physics, 0.0f, clubCeil + 0.2f, 0.0f, (clubX1 - clubX0) * 0.5f + wallT, 0.2f, (clubZ1 - clubZ0) * 0.5f + wallT, kWallClub, kEmitOff, true, 0.4f);

    // --- Neon WALL STRIPS: glowing accent lines along the walls (the club glow). ---
    for (float zx = clubZ0 + 2.0f; zx <= clubZ1 - 2.0f; zx += 4.0f) {
        addBox(scene, device, physics, clubX0 + 0.25f, 2.4f, zx, 0.06f, 0.9f, 0.12f, kWallClub, kEmitMagenta, false); // -X wall strips
    }
    for (float xx = clubX0 + 3.0f; xx <= clubX1 - 3.0f; xx += 4.0f) {
        addBox(scene, device, physics, xx, 3.0f, clubZ0 + 0.25f, 0.12f, 0.9f, 0.06f, kWallClub, kEmitCyan, false);    // -Z wall strips
        addBox(scene, device, physics, xx, 3.0f, clubZ1 - 0.25f, 0.12f, 0.9f, 0.06f, kWallClub, kEmitViolet, false);  // +Z wall strips
    }

    // ===================================================================
    // CATWALKS / BALCONIES — raised upper galleries along the +Z and -Z walls,
    // with glowing glass-rail strips (the multi-level read). ~3.2 m up, 2.2 m deep.
    // ===================================================================
    const float catY = 3.2f, catDepth = 2.2f;
    auto catwalk = [&](float zCenter, float railZ, const float* railEmit) {
        addBox(scene, device, physics, 0.0f, catY - 0.1f, zCenter,
               (clubX1 - clubX0) * 0.5f - 1.5f, 0.1f, catDepth * 0.5f, kFloorClub, kEmitOff, true, 0.5f);
        // Glowing glass-look railing strip along the inner edge.
        addBox(scene, device, physics, 0.0f, catY + 0.55f, railZ,
               (clubX1 - clubX0) * 0.5f - 1.5f, 0.5f, 0.05f, kWallClub, railEmit, false);
        // Support posts down to the floor at intervals.
        for (float px = clubX0 + 3.0f; px <= clubX1 - 3.0f; px += 5.0f)
            addBox(scene, device, physics, px, (catY - 0.2f) * 0.5f, zCenter,
                   0.18f, (catY - 0.2f) * 0.5f, 0.18f, kWallClub, kEmitOff, true);
    };
    catwalk(clubZ1 - catDepth * 0.5f, clubZ1 - catDepth - 0.05f, kEmitViolet);
    catwalk(clubZ0 + catDepth * 0.5f, clubZ0 + catDepth + 0.05f, kEmitCyan);

    // Stair ramp up to a catwalk (so it reads as reachable) on the +X+Z corner.
    for (int s = 0; s < 6; ++s) {
        const float sy = ringY + s * (catY - ringY) / 6.0f;
        addBox(scene, device, physics, clubX1 - 1.6f, sy * 0.5f + 0.05f, clubZ1 - catDepth - 1.2f - s * 0.5f,
               1.2f, sy * 0.5f + 0.05f, 0.3f, kFloorClub, kEmitOff, true);
    }

    // ===================================================================
    // BAR — counter along the -X wall + a back-bar shelf with bottle glow.
    // ===================================================================
    const float barZ = -2.0f;
    addBox(scene, device, physics, clubX0 + 2.0f, ringY + 0.55f, barZ, 1.4f, 0.55f, 3.5f, kBarTop, kEmitOff, true, 0.7f); // counter
    addBox(scene, device, physics, clubX0 + 2.0f, ringY + 1.15f, barZ, 1.4f, 0.05f, 3.5f, kBarTop, kEmitMagenta, false);   // glowing counter lip
    addBox(scene, device, physics, clubX0 + 0.9f, ringY + 1.6f, barZ, 0.25f, 1.0f, 3.2f, kWallClub, kEmitCyan, false);     // back-bar bottle glow shelf

    // ===================================================================
    // OVERHEAD LIGHT RIG over the dance floor — a crossed truss of emissive bars
    // that also seed bright point lights (the club's key glow).
    // ===================================================================
    addBox(scene, device, physics, 0.0f, clubCeil - 0.6f, 0.0f, 6.0f, 0.12f, 0.18f, kWallClub, kEmitMagenta, false);
    addBox(scene, device, physics, 0.0f, clubCeil - 0.6f, 0.0f, 0.18f, 0.12f, 5.0f, kWallClub, kEmitCyan, false);

    // ---- Club point lights: saturated neon, several colors, ranges that reach the
    // sunken dance floor (~5-7 m below the ceiling rig). ----
    addLight(m_lights, 0.0f, 5.6f, 0.0f, 5.0f, 0.6f, 3.0f, 16.0f);   // magenta key over dance floor
    addLight(m_lights, -4.0f, 3.0f, -3.0f, 0.6f, 4.5f, 5.5f, 12.0f); // cyan over the bar
    addLight(m_lights, 4.0f, 3.0f, 3.0f, 3.0f, 1.2f, 5.5f, 12.0f);   // violet +X+Z
    addLight(m_lights, -4.0f, 3.0f, 4.0f, 4.5f, 0.8f, 3.0f, 11.0f);  // magenta -X+Z
    addLight(m_lights, 4.0f, 3.0f, -4.0f, 0.8f, 4.0f, 5.0f, 11.0f);  // cyan +X-Z
    addLight(m_lights, 0.0f, catY + 1.2f, clubZ1 - 1.5f, 2.5f, 1.0f, 5.0f, 9.0f); // catwalk fill +Z
    addLight(m_lights, 0.0f, catY + 1.2f, clubZ0 + 1.5f, 1.0f, 4.0f, 4.5f, 9.0f); // catwalk fill -Z
    addLight(m_lights, clubX0 + 2.0f, 2.5f, barZ, 4.0f, 0.7f, 3.0f, 8.0f);        // bar magenta pool

    // ===================================================================
    // CAVE MOUTH + DESCENDING TUNNEL (off the +X wall, gap centered z=0).
    //   The tunnel descends from the club floor (y=0 at x~13) down to y~-5 by
    //   x~34, then opens into the power-core cavern. Built as jittered rock boxes
    //   (floor, two arched walls, ceiling chunks) so it reads organic, not a tube.
    // ===================================================================
    const float tunStartX = clubX1;     // 13
    const float tunEndX   = 34.0f;
    const float tunStartY = 0.0f;
    const float tunEndY   = -5.0f;
    const int   tunSteps  = 14;
    auto tunYatX = [&](float x) {
        const float t = (x - tunStartX) / (tunEndX - tunStartX);
        return tunStartY + (tunEndY - tunStartY) * t;
    };
    for (int i = 0; i < tunSteps; ++i) {
        const float x = tunStartX + (tunEndX - tunStartX) * (i + 0.5f) / tunSteps;
        const float y = tunYatX(x);
        const float seg = (tunEndX - tunStartX) / tunSteps;
        const float jz = rng.range(-0.6f, 0.6f);     // wander the centerline in Z
        const float halfSpan = rng.range(2.4f, 3.2f);
        // Sloped floor slab (tilted toward the descent — approximated flat per seg).
        addBox(scene, device, physics, x, y - 0.2f, jz,
               seg * 0.62f, rng.range(0.18f, 0.30f), halfSpan, kRockFloor, kEmitOff, true, 0.6f);
        // Two rough side walls (arched feel: taller, jittered rock chunks).
        const float wallH = rng.range(2.2f, 3.0f);
        addBox(scene, device, physics, x, y + wallH * 0.5f, jz - halfSpan - 0.4f,
               seg * 0.6f, wallH * 0.5f, rng.range(0.5f, 0.9f), kRock, kEmitOff, true, 0.5f);
        addBox(scene, device, physics, x, y + wallH * 0.5f, jz + halfSpan + 0.4f,
               seg * 0.6f, wallH * 0.5f, rng.range(0.5f, 0.9f), kRock, kEmitOff, true, 0.5f);
        // Low ceiling chunks (irregular drop heights -> arched read).
        addBox(scene, device, physics, x, y + wallH + rng.range(0.0f, 0.5f), jz,
               seg * 0.6f, rng.range(0.3f, 0.6f), halfSpan + 0.3f, kRock, kEmitOff, true, 0.5f);
        // A couple of stalactite/boulder nubs for silhouette.
        if ((i & 1) == 0)
            addBox(scene, device, physics, x + rng.range(-0.4f, 0.4f), y + wallH - 0.3f, jz + rng.range(-1.5f, 1.5f),
                   rng.range(0.15f, 0.4f), rng.range(0.3f, 0.7f), rng.range(0.15f, 0.4f), kRock, kEmitOff, false);
        // Sparse teal cave fill light every few segments.
        if (i % 3 == 0)
            addLight(m_lights, x, y + 1.6f, jz, 0.4f, 1.6f, 1.8f, 7.0f);
    }

    // ===================================================================
    // POWER-CORE CAVERN — an irregular chamber at the tunnel end (x ~34..50,
    // floor ~y=-5..-6), with a glowing core pillar + teal crystal clusters.
    // ===================================================================
    const float coreCx = 42.0f, coreCz = 0.0f, coreFloorY = -6.0f;
    const float coreHX = 8.0f, coreHZ = 9.0f, coreCeilH = 7.5f;
    // Cavern floor (slightly varied chunks so it isn't a flat plate).
    for (int gx = 0; gx < 4; ++gx)
        for (int gz = 0; gz < 4; ++gz) {
            const float fx = coreCx - coreHX + (gx + 0.5f) * (2 * coreHX / 4);
            const float fz = coreCz - coreHZ + (gz + 0.5f) * (2 * coreHZ / 4);
            addBox(scene, device, physics, fx, coreFloorY - 0.2f + rng.range(-0.12f, 0.12f), fz,
                   coreHX / 4 + 0.1f, 0.25f, coreHZ / 4 + 0.1f, kRockFloor, kEmitOff, true, 0.5f);
        }
    // Irregular cavern walls: a ring of jittered rock pillars (organic perimeter).
    {
        const int ring = 16;
        for (int i = 0; i < ring; ++i) {
            const float a = (float)i / ring * 2.0f * kPi;
            const float rr = rng.range(0.85f, 1.05f);
            const float wx = coreCx + std::cos(a) * coreHX * rr;
            const float wz = coreCz + std::sin(a) * coreHZ * rr;
            const float h = rng.range(3.0f, coreCeilH);
            addBox(scene, device, physics, wx, coreFloorY + h * 0.5f, wz,
                   rng.range(0.7f, 1.4f), h * 0.5f, rng.range(0.7f, 1.4f), kRock, kEmitOff, true, 0.5f);
        }
    }
    // Cavern ceiling lid (chunky).
    addBox(scene, device, physics, coreCx, coreFloorY + coreCeilH + 0.4f, coreCz,
           coreHX + 1.0f, 0.5f, coreHZ + 1.0f, kRock, kEmitOff, true, 0.4f);
    // THE POWER CORE: a bright glowing pillar at the cavern center.
    addBox(scene, device, physics, coreCx, coreFloorY + 2.2f, coreCz, 0.6f, 2.2f, 0.6f, kWallClub, kEmitCore, false);
    addBox(scene, device, physics, coreCx, coreFloorY + 0.3f, coreCz, 1.4f, 0.3f, 1.4f, kRock, kEmitCyan, true); // glowing base ring
    addLight(m_lights, coreCx, coreFloorY + 3.0f, coreCz, 1.2f, 3.5f, 4.5f, 18.0f);
    addLight(m_lights, coreCx, coreFloorY + 5.0f, coreCz, 0.6f, 2.0f, 2.6f, 16.0f);
    // Teal crystal clusters (LevelArchitect: jittered emissive shards, each a light).
    for (int c = 0; c < 10; ++c) {
        const float cx2 = coreCx + rng.range(-coreHX * 0.7f, coreHX * 0.7f);
        const float cz2 = coreCz + rng.range(-coreHZ * 0.7f, coreHZ * 0.7f);
        const float ch  = rng.range(0.5f, 1.4f);
        addBox(scene, device, physics, cx2, coreFloorY + ch * 0.5f, cz2,
               rng.range(0.1f, 0.25f), ch * 0.5f, rng.range(0.1f, 0.25f), kRock, kEmitCrystal, false);
        addLight(m_lights, cx2, coreFloorY + ch + 0.2f, cz2, 0.2f, 1.0f, 0.9f, 4.0f);
    }

    // ===================================================================
    // FLOODED SECTION — REAL animated water spanning the core cavern's -Z side,
    // extending toward the boss arena, with lurking sea creatures + lore nooks.
    //
    // No static slab: the surface is the engine's Gerstner-wave water plane driven
    // every frame via setWaterParams (see tickWater() + app/main.cpp's --world
    // ocean). The water plane is camera-centered + depth-tested against the scene,
    // so it fills the flooded cavern floor wherever the rock dips below the flood
    // surface Y and is occluded by the club floor / tunnel above. We only record
    // the surface Y here (the sea level) and underlight the pool for local glow.
    // ===================================================================
    const float waterY = coreFloorY + 0.35f;     // shallow flood surface (sea level)
    m_waterY = waterY;                            // tickWater() drives water at this Y
    addLight(m_lights, coreCx + 6.0f, waterY + 2.0f, coreCz - 2.0f, 0.3f, 1.4f, 1.7f, 12.0f);
    // Lore-cache nooks: glowing amber data-cache boxes set in the wall recesses.
    addBox(scene, device, physics, coreCx + 7.0f, coreFloorY + 1.0f, coreCz - 7.5f, 0.4f, 0.4f, 0.4f, kRock, kEmitLore, true);
    addBox(scene, device, physics, coreCx - 6.5f, coreFloorY + 1.2f, coreCz + 6.5f, 0.4f, 0.4f, 0.4f, kRock, kEmitLore, true);
    addLight(m_lights, coreCx + 7.0f, coreFloorY + 1.6f, coreCz - 7.5f, 1.6f, 1.2f, 0.3f, 5.0f);
    addLight(m_lights, coreCx - 6.5f, coreFloorY + 1.8f, coreCz + 6.5f, 1.6f, 1.2f, 0.3f, 5.0f);

    // ===================================================================
    // HIDDEN BOSS ARENA — a wider, taller cavern beyond the flooded section
    // (x ~50..66), lit red, with the hidden boss (BossTheSiren).
    // ===================================================================
    const float bossCx = 58.0f, bossCz = -1.0f, bossFloorY = -6.5f;
    const float bossHX = 8.0f, bossHZ = 8.0f, bossCeilH = 10.0f;
    // Connecting low cave gap from the core cavern to the boss arena (a short
    // jittered tunnel along +X at floor level).
    for (int i = 0; i < 5; ++i) {
        const float x = 50.0f + i * 1.6f;
        addBox(scene, device, physics, x, bossFloorY - 0.2f, -1.0f + rng.range(-0.4f, 0.4f),
               1.0f, 0.25f, rng.range(2.0f, 2.8f), kRockFloor, kEmitOff, true, 0.5f);
        addBox(scene, device, physics, x, bossFloorY + 2.5f, -1.0f, 1.0f, 0.5f, 3.0f, kRock, kEmitOff, true, 0.5f); // low roof
    }
    // Boss arena floor.
    for (int gx = 0; gx < 4; ++gx)
        for (int gz = 0; gz < 4; ++gz) {
            const float fx = bossCx - bossHX + (gx + 0.5f) * (2 * bossHX / 4);
            const float fz = bossCz - bossHZ + (gz + 0.5f) * (2 * bossHZ / 4);
            addBox(scene, device, physics, fx, bossFloorY - 0.2f, fz,
                   bossHX / 4 + 0.1f, 0.25f, bossHZ / 4 + 0.1f, kRockFloor, kEmitOff, true, 0.5f);
        }
    // Irregular tall walls + ceiling.
    {
        const int ring = 18;
        for (int i = 0; i < ring; ++i) {
            const float a = (float)i / ring * 2.0f * kPi;
            const float rr = rng.range(0.9f, 1.08f);
            const float wx = bossCx + std::cos(a) * bossHX * rr;
            const float wz = bossCz + std::sin(a) * bossHZ * rr;
            const float h = rng.range(4.0f, bossCeilH);
            addBox(scene, device, physics, wx, bossFloorY + h * 0.5f, wz,
                   rng.range(0.8f, 1.5f), h * 0.5f, rng.range(0.8f, 1.5f), kRock, kEmitOff, true, 0.5f);
        }
    }
    addBox(scene, device, physics, bossCx, bossFloorY + bossCeilH + 0.4f, bossCz,
           bossHX + 1.5f, 0.5f, bossHZ + 1.5f, kRock, kEmitOff, true, 0.4f);
    // Red boss-arena glow: a few emissive vents + red point lights.
    addBox(scene, device, physics, bossCx, bossFloorY + 0.2f, bossCz, 2.5f, 0.18f, 2.5f, kRock, kEmitBoss, true);
    addLight(m_lights, bossCx, bossFloorY + 4.0f, bossCz, 4.5f, 0.8f, 0.6f, 20.0f);
    addLight(m_lights, bossCx - 4.0f, bossFloorY + 2.5f, bossCz + 3.0f, 3.0f, 0.5f, 0.4f, 12.0f);
    addLight(m_lights, bossCx + 4.0f, bossFloorY + 2.5f, bossCz - 3.0f, 3.0f, 0.5f, 0.4f, 12.0f);

    // ===================================================================
    // CHARACTERS — rigged GLBs as inert, animating props (graceful box fallback).
    //   Club: BartenderDanny behind the bar, RexBouncer at the entrance landing.
    //   Caves: GreatWhiteShark + manta + hammerhead in/over the flooded section;
    //          BossTheSiren as the hidden boss in the arena.
    // The converted/rigged character GLBs are authored Z-up (lying flat), so
    // standUpZtoY=true rotates them upright (same fix the Level-1 characters use).
    // ===================================================================
    const float warm[4] = { 1.2f, 1.1f, 0.95f, 1.0f };
    const float coolGlow[4] = { 0.9f, 1.1f, 1.4f, 1.0f };
    const float redGlow[4] = { 1.5f, 0.7f, 0.7f, 1.0f };
    const float seaTint[4] = { 0.9f, 1.0f, 1.1f, 1.0f };
    // Bartender behind the -X bar counter, on the raised ring floor.
    addCharacter(scene, device, physics, modelDir, "BartenderDanny.glb",
                 x3::phys::Vec3{ clubX0 + 1.1f, ringY, barZ }, 1.0f, true, warm);
    // Bouncer at the entrance landing (the +Z+X corner where the player spawns).
    addCharacter(scene, device, physics, modelDir, "RexBouncer.glb",
                 x3::phys::Vec3{ 9.0f, ringY, 8.0f }, 1.0f, true, coolGlow);
    // Sea creatures lurking in the flooded section (sit at/just under the water).
    addCharacter(scene, device, physics, modelDir, "GreatWhiteSharkGameReady.glb",
                 x3::phys::Vec3{ coreCx + 6.0f, waterY - 0.4f, coreCz - 2.0f }, 1.0f, true, seaTint);
    addCharacter(scene, device, physics, modelDir, "sea_manta_ray.glb",
                 x3::phys::Vec3{ coreCx + 8.0f, waterY + 0.6f, coreCz - 4.5f }, 1.0f, true, seaTint);
    addCharacter(scene, device, physics, modelDir, "sea_hammerhead.glb",
                 x3::phys::Vec3{ coreCx + 4.0f, waterY - 0.3f, coreCz + 0.5f }, 1.0f, true, seaTint);
    // Hidden boss in the red arena.
    addCharacter(scene, device, physics, modelDir, "BossTheSiren.glb",
                 x3::phys::Vec3{ bossCx, bossFloorY, bossCz }, 1.4f, true, redGlow);

    // ---- Player spawn: at the club entrance landing, on the raised ring floor,
    // facing -X toward the dance floor + bar. ----
    m_spawn = x3::phys::Vec3{ 9.5f, ringY + 0.1f, 8.0f };

    x3::logInfo("[club1127] built Club 1127 + cave/tunnel network: " +
                std::to_string(scene.size()) + " entities, " +
                std::to_string(m_lights.size()) + " point lights, " +
                std::to_string(m_chars.size()) + " characters");
}

void Club1127World::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    // Tick each inert character so its idle clip plays (CPU skinning re-upload).
    // chaseSpeed 0 + null target => it never moves; only the animation advances.
    for (auto& c : m_chars)
        c->update(dt, scene, physics, c->pos());
}

void Club1127World::tickWater(float dt, x3::rhi::IRenderDevice& device) {
    if (!m_built) return;
    // Advance the wave animation clock + (re)apply the engine's REAL water at the
    // flood surface Y. Same usage as app/main.cpp's --world ocean per-frame driver:
    // the device CACHES these params + re-applies them each frame (like the sky), so
    // we re-submit every frame with an advanced `time` to keep the surface moving.
    //
    // This is an ENCLOSED flooded cavern (not the open sea), so the waves are gentle:
    // a short wavelength + low amplitude so it reads as a still-ish flooded pool with
    // a live, rippling surface rather than rolling ocean swell. The water plane is
    // camera-centered + depth-tested against the scene, so it fills only where the
    // cavern floor sits below `seaLevel` and is occluded by the rock/club above.
    m_waterTime += dt;

    x3::rhi::IRenderDevice::WaterParams wp{};
    wp.enabled    = true;
    wp.seaLevel   = m_waterY;       // the flood surface Y (set in build())
    wp.time       = m_waterTime;    // advance -> the Gerstner waves animate
    wp.amplitude  = 0.14f;          // gentle chop: a flooded pool, not ocean swell
    wp.steepness  = 0.55f;          // visible horizontal pinch on the small waves
    wp.waveLength = 3.0f;           // short waves to fit the ~14 x 12 m pool
    wp.speed      = 0.9f;           // calm, slow ripple
    // Teal flood-water tint: deep murky teal -> brighter shallow teal (the engine
    // blends shallow->deep by how much water the view ray crosses, so shallow edges
    // near the cavern floor glow brighter than the deep middle).
    wp.deepColor[0]    = 0.015f; wp.deepColor[1]    = 0.10f; wp.deepColor[2]    = 0.13f;
    wp.shallowColor[0] = 0.06f;  wp.shallowColor[1] = 0.34f; wp.shallowColor[2] = 0.40f;
    // No real sun underground; aim the glint UP toward the teal core/pool light so
    // the surface picks up a moving specular sparkle + a Fresnel sky-ish reflection.
    wp.sunDir[0] = 0.15f; wp.sunDir[1] = 1.0f; wp.sunDir[2] = 0.30f;
    wp.specular  = 9.0f;            // sun/light glint strength (HDR -> bloom)
    wp.fresnel   = 0.05f;          // a touch more face-on reflectance for a glassy cave pool
    device.setWaterParams(wp);
}

void Club1127World::drawCharacters(x3::rhi::IRenderDevice& device,
                                   const x3::rhi::FrameContext& frame, const Scene& scene) const {
    for (const auto& c : m_chars)
        c->drawMonster(device, frame, scene);
}

void Club1127World::showcaseCamera(float out[5]) const {
    // Elevated 3/4 vantage from the -X+Z corner looking toward +X across the
    // dance floor, so the glowing dance-floor grid, the catwalks/rails, the bar
    // (with the bartender to the left), the bouncer near the entrance, and the
    // jagged CAVE MOUTH breaking the far +X wall all read in one frame.
    out[0] = -10.5f;  // x (back by the bar wall)
    out[1] = 5.0f;    // y (above the ring floor, below the ceiling rig)
    out[2] = 8.5f;    // z (+Z corner)
    out[3] = -0.45f;  // yaw: look toward +X, angled slightly toward -Z (the cave mouth)
    out[4] = -0.30f;  // pitch: down over the floor toward the cave
}

} // namespace x3::game
