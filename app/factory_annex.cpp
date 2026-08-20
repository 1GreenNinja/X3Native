// THE CONFECTION ANNEX — shell + bore + trigger map (Phase 2, T5). See
// factory_annex.h for the shape and the plan pointer. CLEAN-ROOM original
// content; no Dahl/Wonka names or trademarks anywhere.
//
// Authoring bookkeeping mirrors app/rifthub.cpp EXACTLY:
//   * every device mesh handle build() creates goes into ONE vector
//     (m_meshes) and shutdown() frees it uniformly; SHARED handles (one mesh
//     instanced by many entities — the bore rib torus) are pushed ONCE;
//   * animated groups are CONTIGUOUS Scene entity spans recorded at authoring
//     time; tick() pokes emissive[3]/transforms in place — NO per-frame heap;
//   * the curated PBR sets come from the SurfaceLibrary, which owns its
//     textures (destroyAll in shutdown); a set that fails to load falls back
//     to the flat-tinted look, so the headless self-test never breaks.

#include "factory_annex.h"

#include "asset_root.h"
#include "headless_device.h"   // the self-test's leak-counting device
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// ---- Palette (plan design decision 7: candy-bright against dark iron) ------
// Phase-5 note: these are MULTIPLIERS over real albedos now, not flat colors.
// The fa_iron_wall corrugated albedo is a mid-brown scan, so the aubergine
// walls carry their hue in the tint at ~0.5 value (the old 0.16 flat-tint
// crushed the texture to black — the "slop" verdict); the deck rides the
// bright mw_metal_panels_a set pulled down to a believable floor value.
constexpr float kIronTint[3]   = { 0.52f, 0.44f, 0.62f };  // aubergine over corrugated iron
constexpr float kFloorTint[3]  = { 0.38f, 0.34f, 0.44f };  // deck panels, dusk-violet
constexpr float kBrassTint[3]  = { 0.69f, 0.55f, 0.34f };  // brass #b08d57
constexpr float kBrassGlow[3]  = { 1.00f, 0.78f, 0.42f };  // warm brass emissive
constexpr float kGlassTint[3]  = { 0.72f, 0.88f, 0.94f };  // cool curtain glass
constexpr float kBoreIron[3]   = { 0.46f, 0.40f, 0.54f };  // bore panels (darker aubergine)

// Shell dims not already public on the class.
constexpr float kSlabHalfT   = 0.25f;   // floor/roof slab half-thickness
constexpr float kWallHalfT   = 0.15f;   // iron wall half-thickness
constexpr float kOpenHalf    = 2.2f;    // cab pass-through opening half (cab 1.6 + clear)
constexpr float kLandHalf    = 6.0f;    // shaft-side landing pad half-extent
constexpr float kBoreLift    = 0.7f;    // bore centerline above floor-B baseY (clears
                                        // the cab top + a standing rider in the 4 m tube)
constexpr uint32_t kBoreSides = 8;      // octagonal bore
constexpr float kRibPitch     = 3.0f;   // brass rib every 3 m

// Room table (ORIGINAL names only — the IP-clean law).
struct RoomSpec { const char* name; float accent[3]; };
constexpr RoomSpec kRoomSpec[FactoryAnnex::kFloorCount] = {
    { "MIXTURE ATRIUM",  { 1.00f, 0.35f, 0.55f } },   // A — raspberry
    { "INVENTION WORKS", { 0.40f, 1.00f, 0.60f } },   // B — mint
    { "FIZZ GALLERY",    { 1.00f, 0.72f, 0.25f } },   // C — amber
    { "SORTING HALL",    { 1.00f, 0.84f, 0.30f } },   // D — gold
    { "TUBE JUNCTION",   { 0.35f, 0.90f, 1.00f } },   // E — cyan
};

// Column-major basis+translation transform (the rifthub makeXform, verbatim
// convention — see valley.cpp placeTilted for the column layout).
inline void makeXform(float m[16],
                      const float xA[3], const float yA[3], const float zA[3],
                      float wx, float wy, float wz) {
    m[0] = xA[0]; m[1] = xA[1]; m[2]  = xA[2]; m[3]  = 0.0f;
    m[4] = yA[0]; m[5] = yA[1]; m[6]  = yA[2]; m[7]  = 0.0f;
    m[8] = zA[0]; m[9] = zA[1]; m[10] = zA[2]; m[11] = 0.0f;
    m[12] = wx;   m[13] = wy;   m[14] = wz;    m[15] = 1.0f;
}

// Axis-aligned box dressed with a curated PBR set (albedo+normal+MR), tinted;
// falls back to the flat tint when the set didn't load. Mesh centered at the
// world position directly (no transform gymnastics — the shell is axis-aligned).
// EVERY handle goes into `meshes` (the ONE uniform vector).
uint32_t addSurfBox(Scene& scene, x3::rhi::IRenderDevice& device,
                    std::vector<x3::rhi::MeshHandle>& meshes,
                    float cx, float cy, float cz, float hx, float hy, float hz,
                    const SurfaceSet* sf, const float tint[3],
                    float emStrength = 0.0f, float uvScale = 0.25f) {
    x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, uvScale);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    meshes.push_back(e.mesh);
    if (sf && sf->ok) { e.tex = sf->albedo; e.normalTex = sf->normal; e.mrTex = sf->mr; }
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = 1.0f;
    if (emStrength > 0.0f) {
        e.emissive[0] = tint[0]; e.emissive[1] = tint[1]; e.emissive[2] = tint[2];
        e.emissive[3] = emStrength;
    }
    e.tag = (uint32_t)Tag::Static;
    return scene.add(e);
}

// Flat emissive box (glow trim). Dark body + tinted glow so a dim emitter never
// reads as saturated plastic (the rifthub round-2 realism law).
uint32_t addEmissiveBox(Scene& scene, x3::rhi::IRenderDevice& device,
                        std::vector<x3::rhi::MeshHandle>& meshes,
                        float cx, float cy, float cz, float hx, float hy, float hz,
                        const float glow[3], float emStrength) {
    x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    meshes.push_back(e.mesh);
    e.baseColor[0] = glow[0] * 0.25f; e.baseColor[1] = glow[1] * 0.25f;
    e.baseColor[2] = glow[2] * 0.25f; e.baseColor[3] = 1.0f;
    e.emissive[0] = glow[0]; e.emissive[1] = glow[1]; e.emissive[2] = glow[2];
    e.emissive[3] = emStrength;
    e.tag = (uint32_t)Tag::Prop;
    return scene.add(e);
}

// One GLASS CURTAIN pane (the see-through wall the lateral approach looks
// through). Real glass pipeline: transparent entity + GlassMaterial, near-clear
// (X3_WORLD_RULES material law: glass alpha well under 0.07 reads as glass).
uint32_t addGlassPane(Scene& scene, x3::rhi::IRenderDevice& device,
                      std::vector<x3::rhi::MeshHandle>& meshes,
                      float cx, float cy, float cz, float hx, float hy, float hz) {
    x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    meshes.push_back(e.mesh);
    e.baseColor[0] = kGlassTint[0]; e.baseColor[1] = kGlassTint[1];
    e.baseColor[2] = kGlassTint[2]; e.baseColor[3] = 1.0f;
    e.transparent = true;
    e.glass.opacity    = 0.055f;   // near-clear — the rooms must READ through it
    e.glass.refraction = 0.012f;
    e.glass.roughness  = 0.04f;
    e.glass.specular   = 0.65f;
    e.glass.tint[0] = kGlassTint[0]; e.glass.tint[1] = kGlassTint[1];
    e.glass.tint[2] = kGlassTint[2];
    e.tag = (uint32_t)Tag::Static;
    return scene.add(e);
}

// A floor/roof slab WITH the 4.4 m central cab pass-through opening: four
// axis-aligned segments (two full-width Z strips + two X side blocks) whose
// union is the 40x40 slab minus the opening. Render + physics both.
void addSlabWithOpening(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics,
                        std::vector<x3::rhi::MeshHandle>& meshes,
                        float cx, float topY, float cz, float half,
                        const SurfaceSet* sf, const float tint[3]) {
    const float cy = topY - kSlabHalfT;
    struct Seg { float ox, oz, hx, hz; };
    const float stripH = (half - kOpenHalf) * 0.5f;        // Z strip half-depth
    const float stripO = kOpenHalf + stripH;               // Z strip center offset
    const Seg segs[4] = {
        { 0.0f,  stripO, half,  stripH },                  // +Z strip (full width)
        { 0.0f, -stripO, half,  stripH },                  // -Z strip (full width)
        {  stripO, 0.0f, stripH, kOpenHalf },              // +X side block
        { -stripO, 0.0f, stripH, kOpenHalf },              // -X side block
    };
    for (const Seg& s : segs) {
        addSurfBox(scene, device, meshes, cx + s.ox, cy, cz + s.oz,
                   s.hx, kSlabHalfT, s.hz, sf, tint);
        physics.addBox(x3::phys::Vec3{ s.hx, kSlabHalfT, s.hz },
                       x3::phys::Vec3{ cx + s.ox, cy, cz + s.oz },
                       0.0f, x3::phys::Layer::Static);
    }
}

// Floor A's slab (Task 7): the standard 40x40-with-cab-opening MINUS the
// confection river channel — a full-depth slot at local x [kRiverX0, kRiverX1]
// running the whole Z extent. Five axis-aligned segments, render + physics
// (the slot is REALLY open; the river bed/banks below are Floor A content,
// factory_rooms.cpp). East region carries the cab opening exactly like
// addSlabWithOpening; west of the channel is one bank strip.
void addSlabFloorA(Scene& scene, x3::rhi::IRenderDevice& device,
                   x3::phys::IPhysicsWorld& physics,
                   std::vector<x3::rhi::MeshHandle>& meshes,
                   float cx, float topY, float cz, float half,
                   const SurfaceSet* sf, const float tint[3]) {
    const float cy  = topY - kSlabHalfT;
    const float rx0 = FactoryAnnex::kRiverX0, rx1 = FactoryAnnex::kRiverX1;
    struct Seg { float x0, z0, x1, z1; };
    const Seg segs[5] = {
        { -half,     -half,      rx0,        half      },   // west bank strip
        { rx1,        kOpenHalf, half,       half      },   // east: +Z strip
        { rx1,       -half,      half,      -kOpenHalf },   // east: -Z strip
        { kOpenHalf, -kOpenHalf, half,       kOpenHalf },   // east: +X side block
        { rx1,       -kOpenHalf, -kOpenHalf, kOpenHalf },   // east: -X side block
    };
    for (const Seg& s : segs) {
        const float scx = cx + (s.x0 + s.x1) * 0.5f, scz = cz + (s.z0 + s.z1) * 0.5f;
        const float shx = (s.x1 - s.x0) * 0.5f,      shz = (s.z1 - s.z0) * 0.5f;
        addSurfBox(scene, device, meshes, scx, cy, scz, shx, kSlabHalfT, shz, sf, tint);
        physics.addBox(x3::phys::Vec3{ shx, kSlabHalfT, shz },
                       x3::phys::Vec3{ scx, cy, scz },
                       0.0f, x3::phys::Layer::Static);
    }
}

// Floors B/C/D (Task 10): the standard slab-with-cab-opening PLUS the Chute of
// Dubious Quality's drop-shaft hole at local (kChuteX, kChuteZ) — the +Z strip
// (which contains the chute column) splits into four boxes around the hole.
// Seven segments total; render + physics both.
void addSlabWithChuteHole(Scene& scene, x3::rhi::IRenderDevice& device,
                          x3::phys::IPhysicsWorld& physics,
                          std::vector<x3::rhi::MeshHandle>& meshes,
                          float cx, float topY, float cz, float half,
                          const SurfaceSet* sf, const float tint[3]) {
    const float cy = topY - kSlabHalfT;
    const float hx0 = FactoryAnnex::kChuteX - FactoryAnnex::kChuteHoleHalf;   // 6.9
    const float hx1 = FactoryAnnex::kChuteX + FactoryAnnex::kChuteHoleHalf;   // 9.1
    const float hz0 = FactoryAnnex::kChuteZ - FactoryAnnex::kChuteHoleHalf;
    const float hz1 = FactoryAnnex::kChuteZ + FactoryAnnex::kChuteHoleHalf;
    struct Seg { float x0, z0, x1, z1; };
    const Seg segs[7] = {
        { -half,     -half,      half,      -kOpenHalf },   // -Z strip (full width)
        {  kOpenHalf, -kOpenHalf, half,      kOpenHalf },   // +X side block
        { -half,     -kOpenHalf, -kOpenHalf, kOpenHalf },   // -X side block
        { -half,      kOpenHalf,  hx0,       half      },   // +Z strip: west of hole
        {  hx1,       kOpenHalf,  half,      half      },   // +Z strip: east of hole
        {  hx0,       kOpenHalf,  hx1,       hz0       },   // +Z strip: south of hole
        {  hx0,       hz1,        hx1,       half      },   // +Z strip: north of hole
    };
    for (const Seg& s : segs) {
        const float scx = cx + (s.x0 + s.x1) * 0.5f, scz = cz + (s.z0 + s.z1) * 0.5f;
        const float shx = (s.x1 - s.x0) * 0.5f,      shz = (s.z1 - s.z0) * 0.5f;
        addSurfBox(scene, device, meshes, scx, cy, scz, shx, kSlabHalfT, shz, sf, tint);
        physics.addBox(x3::phys::Vec3{ shx, kSlabHalfT, shz },
                       x3::phys::Vec3{ scx, cy, scz },
                       0.0f, x3::phys::Layer::Static);
    }
}

} // namespace

const float FactoryAnnex::kFloorBaseY[FactoryAnnex::kFloorCount] =
    { 2.0f, 15.0f, 28.0f, 41.0f, 54.0f };

// ---- Per-room content hooks (factory_rooms.cpp; EMPTY in Phase 2) ----------
// Declared here (file-scope contract with factory_rooms.cpp) so the annex core
// never needs touching when Phase 3 fills the rooms.
void buildRoomMixture  (FactoryRoomCtx& ctx, AnnexRoom& room);
void buildRoomInvention(FactoryRoomCtx& ctx, AnnexRoom& room);
void buildRoomFizz     (FactoryRoomCtx& ctx, AnnexRoom& room);
void buildRoomSorting  (FactoryRoomCtx& ctx, AnnexRoom& room);
void buildRoomTube     (FactoryRoomCtx& ctx, AnnexRoom& room);
void tickRoomMixture   (Scene& scene, AnnexRoom& room, float t);
void tickRoomInvention (Scene& scene, AnnexRoom& room, float t);
void tickRoomFizz      (Scene& scene, AnnexRoom& room, float t);
void tickRoomSorting   (Scene& scene, AnnexRoom& room, float t);
void tickRoomTube      (Scene& scene, AnnexRoom& room, float t);
void factoryGlbCacheReset();   // Phase 5: drop the previous build's drawable cache
// Phase 5: the load-if-present hero hook (factory_rooms.cpp); the self-test
// probes both of its branches directly (F16b/F16c).
int heroHook(FactoryRoomCtx& ctx, const char* relPath, const char* what,
             float x, float y, float z, float yaw, float scale,
             bool isProp, std::vector<float>* outXf);

FactoryAnnex::ElevatorGraph FactoryAnnex::makeElevatorGraph(float shaftX, float shaftZ) {
    ElevatorGraph g;
    const float ax = shaftX + kAnnexXOff;
    // Cab TOP flush with each walking floor => stop center = baseY - kCabHalfY.
    auto stopY = [](float baseY) { return baseY - kCabHalfY; };
    // Spire-side stops (visible on the panel from the start).
    g.stops.push_back({ { shaftX, stopY(kFloorBaseY[0]), shaftZ }, "F1", false });
    g.stops.push_back({ { shaftX, stopY(kFloorBaseY[1]), shaftZ }, "F3", false });
    // Annex stops A1..A5 — ALL hidden until keypad 4790 (unlockHidden). NOTE
    // (Phase-1 handoff): hidden stops CAN serve as route WAYPOINTS while
    // locked; the lock gates destination selection only — exactly what the
    // annex chain wants.
    const char* aNames[kFloorCount] = { "A1", "A2", "A3", "A4", "A5" };
    for (uint32_t i = 0; i < kFloorCount; ++i)
        g.stops.push_back({ { ax, stopY(kFloorBaseY[i]), shaftZ }, aNames[i], true });
    // Rails: F1<->F3 vertical; F3<->A2 the LATERAL bore leg (both at the floor-B
    // height, so the leg is flat); A1..A5 the annex vertical chain.
    g.rails = { { g.f1, g.f3 }, { g.f3, g.a2 },
                { g.a1, g.a2 }, { g.a2, 4 }, { 4, 5 }, { 5, g.a5 } };
    // Panel labels (Phase-1 handoff: Stop::label is stored but floorLabel()
    // reads ONLY m_floorLabels — the host must call setFloorLabels with these).
    g.labels = { "F1", "F3", "A1", "A2", "A3", "A4", "A5" };
    return g;
}

void FactoryAnnex::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
                         float shaftX, float shaftZ) {
    if (m_built) return;
    m_shaftX = shaftX; m_shaftZ = shaftZ;
    m_annexX = shaftX + kAnnexXOff;
    m_annexZ = shaftZ;
    const float ax = m_annexX, az = m_annexZ;
    m_entFirst = scene.size();

    // ===== Curated PBR surface sets (owned by the library; destroyAll in
    // shutdown; a set that fails to load leaves ok=false and the boxes fall
    // back to their flat tints — the headless self-test path never breaks).
    // Phase 5 swaps the shell onto the fa_* factory sets (tools/
    // tex_curate_factory.py): corrugated riveted iron for the walls/bore,
    // smudged worn metal (brass-tinted) for every trim run.
    m_surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& sIron  = m_surf.get(device, "fa_iron_wall");        // corrugated iron walls
    const SurfaceSet& sDeck  = m_surf.get(device, "mw_metal_panels_a");   // floor decks (panel relief)
    const SurfaceSet& sTrim  = m_surf.get(device, "fa_brass_worn");       // brass trim body

    // ===== Phase-5 GLB services: ONE loader for the Glimvale dressing + the
    // StarForge hero hooks (barrels.cpp's Barrel.glb pattern, annex-wide). A
    // failed mount leaves loader null — every hook then takes its procedural
    // fallback branch and logs it (the annex never REQUIRES an asset).
    m_assets.reset(x3::asset::createAssetSource());
    if (m_assets->mountDir(convertedGlbRoot(), 0)) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
    } else {
        x3::logWarn("[factory] mountDir failed (" + convertedGlbRoot() +
                    ") — all hero hooks + dressing fall back to procedural");
    }
    m_art = FactoryArtHooks{};
    m_art.loader = m_loader.get();
    m_art.models = &m_models;
    factoryGlbCacheReset();

    // ===== Rooms (names + accents; content spans stay EMPTY until Phase 3).
    m_rooms.clear();
    m_rooms.reserve(kFloorCount);
    for (uint32_t i = 0; i < kFloorCount; ++i) {
        AnnexRoom r;
        r.name  = kRoomSpec[i].name;
        r.baseY = kFloorBaseY[i];
        r.centerX = ax; r.centerZ = az;
        r.accent[0] = kRoomSpec[i].accent[0];
        r.accent[1] = kRoomSpec[i].accent[1];
        r.accent[2] = kRoomSpec[i].accent[2];
        m_rooms.push_back(r);
    }

    // ===== Floor slabs (A..E) + the roof — each with the 4.4 m central cab
    // opening (the annex vertical chain passes through the room centers; the
    // parked cab fills the opening flush with the walking floor). The ROOF is
    // the plane the Burst shatters through (kRoofY = 65 — setBurst's roofY).
    // Floor A carries the confection river slot (Task 7); the rest are the
    // standard slab-with-cab-opening.
    addSlabFloorA(scene, device, physics, m_meshes,
                  ax, kFloorBaseY[0], az, kFloorHalf, &sDeck, kFloorTint);
    // B/C/D carry the chute drop-shaft hole (Task 10: the hatch on D opens
    // into a glass shaft that falls THROUGH B and C to the padded room on A).
    // Phase 5: the FIZZ GALLERY (C) floor is the checkered tile — authored as
    // a THIN OVERLAY over the deck slab, not a slab swap: a swapped slab put
    // the red checker on its UNDERSIDE, i.e. on the Invention Works' ceiling
    // (caught on the first Phase-5 floor-B capture).
    for (uint32_t i = 1; i <= 3; ++i)
        addSlabWithChuteHole(scene, device, physics, m_meshes,
                             ax, kFloorBaseY[i], az, kFloorHalf, &sDeck, kFloorTint);
    {
        const SurfaceSet& sCheck = m_surf.get(device, "fa_tile_checker");
        constexpr float kCheckTint[3] = { 1.0f, 0.96f, 0.92f };
        const float topY = kFloorBaseY[2] + 0.012f;
        const float hx0 = kChuteX - kChuteHoleHalf, hx1 = kChuteX + kChuteHoleHalf;
        const float hz0 = kChuteZ - kChuteHoleHalf, hz1 = kChuteZ + kChuteHoleHalf;
        const float half = kFloorHalf;
        struct Seg { float x0, z0, x1, z1; };
        const Seg segs[7] = {
            { -half,     -half,      half,      -kOpenHalf },
            {  kOpenHalf, -kOpenHalf, half,      kOpenHalf },
            { -half,     -kOpenHalf, -kOpenHalf, kOpenHalf },
            { -half,      kOpenHalf,  hx0,       half      },
            {  hx1,       kOpenHalf,  half,      half      },
            {  hx0,       kOpenHalf,  hx1,       hz0       },
            {  hx0,       hz1,        hx1,       half      },
        };
        for (const Seg& s : segs)
            addSurfBox(scene, device, m_meshes,
                       ax + (s.x0 + s.x1) * 0.5f, topY - 0.006f, az + (s.z0 + s.z1) * 0.5f,
                       (s.x1 - s.x0) * 0.5f, 0.006f, (s.z1 - s.z0) * 0.5f,
                       &sCheck, kCheckTint);
    }
    addSlabWithOpening(scene, device, physics, m_meshes,
                       ax, kFloorBaseY[4], az, kFloorHalf, &sDeck, kFloorTint);
    addSlabWithOpening(scene, device, physics, m_meshes,
                       ax, kRoofY + 2.0f * kSlabHalfT, az, kFloorHalf,
                       &sIron, kIronTint);   // roof slab: top at 65.5, bottom at 65

    // ===== Iron walls: +X / +Z / -Z, each ONE full-height panel (floor A base
    // to the roof) — aubergine iron over the metal-plate set. Physics matches.
    {
        const float y0 = kFloorBaseY[0];                    // 2
        const float y1 = kRoofY;                            // 65
        const float wallHalfH = (y1 - y0) * 0.5f;           // 31.5
        const float wallCy    = (y0 + y1) * 0.5f;           // 33.5
        struct Wall { float cx, cz, hx, hz; };
        const Wall walls[3] = {
            { ax + kFloorHalf - kWallHalfT, az, kWallHalfT, kFloorHalf },   // +X (far)
            { ax, az + kFloorHalf - kWallHalfT, kFloorHalf, kWallHalfT },   // +Z
            { ax, az - kFloorHalf + kWallHalfT, kFloorHalf, kWallHalfT },   // -Z
        };
        for (const Wall& w : walls) {
            addSurfBox(scene, device, m_meshes, w.cx, wallCy, w.cz,
                       w.hx, wallHalfH, w.hz, &sIron, kIronTint, 0.0f, 0.12f);
            physics.addBox(x3::phys::Vec3{ w.hx, wallHalfH, w.hz },
                           x3::phys::Vec3{ w.cx, wallCy, w.cz },
                           0.0f, x3::phys::Layer::Static);
        }
    }

    // ===== GLASS CURTAIN — the shaft-facing (-X) wall. Per-floor panes so the
    // lateral bore approach looks INTO the rooms; floor B's curtain leaves a
    // 4.4 m central gap where the bore seals in (the cab enters there). Glass
    // is physical (blocks) except the floor-B gap.
    {
        const float gx = ax - kFloorHalf + 0.06f;   // pane plane, just inside the edge
        for (uint32_t i = 0; i < kFloorCount; ++i) {
            const float cy = kFloorBaseY[i] + kClearH * 0.5f;
            const float hy = kClearH * 0.5f;
            if (i == 1) {
                // Floor B: two panes flanking the bore gap.
                const float paneH = (kFloorHalf - kOpenHalf) * 0.5f;   // 8.9
                const float paneO = kOpenHalf + paneH;                 // 11.1
                for (int s = -1; s <= 1; s += 2) {
                    addGlassPane(scene, device, m_meshes,
                                 gx, cy, az + s * paneO, 0.06f, hy, paneH);
                    physics.addBox(x3::phys::Vec3{ 0.06f, hy, paneH },
                                   x3::phys::Vec3{ gx, cy, az + s * paneO },
                                   0.0f, x3::phys::Layer::Static);
                }
            } else {
                addGlassPane(scene, device, m_meshes, gx, cy, az, 0.06f, hy, kFloorHalf);
                physics.addBox(x3::phys::Vec3{ 0.06f, hy, kFloorHalf },
                               x3::phys::Vec3{ gx, cy, az },
                               0.0f, x3::phys::Layer::Static);
            }
        }
    }

    // ===== Brass trim: full-height columns every 8 m on the three iron walls
    // (PBR brass body, no glow) + the glass curtain's five MULLIONS, which DO
    // carry a soft warm glow and are the m_trimGlow span tick() breathes.
    {
        const float y0 = kFloorBaseY[0], y1 = kRoofY;
        const float colHalfH = (y1 - y0) * 0.5f, colCy = (y0 + y1) * 0.5f;
        const float offs[5] = { -16.0f, -8.0f, 0.0f, 8.0f, 16.0f };
        for (float o : offs)   // +X wall columns
            addSurfBox(scene, device, m_meshes, ax + kFloorHalf - 0.45f, colCy, az + o,
                       0.12f, colHalfH, 0.12f, &sTrim, kBrassTint);
        for (float o : offs) { // +Z / -Z wall columns
            addSurfBox(scene, device, m_meshes, ax + o, colCy, az + kFloorHalf - 0.45f,
                       0.12f, colHalfH, 0.12f, &sTrim, kBrassTint);
            addSurfBox(scene, device, m_meshes, ax + o, colCy, az - kFloorHalf + 0.45f,
                       0.12f, colHalfH, 0.12f, &sTrim, kBrassTint);
        }
        // Glass-curtain mullions (contiguous glow span).
        m_trimGlowFirst = scene.size();
        for (float o : offs) {
            // The center mullion would bisect the floor-B bore gap — nudge it up:
            // it spans only floors C..E there. Simpler + honest: skip the gap by
            // splitting the center mullion into an upper segment.
            if (o == 0.0f) {
                const float segY0 = kFloorBaseY[2];   // 28 (above the bore)
                addEmissiveBox(scene, device, m_meshes,
                               ax - kFloorHalf + 0.20f, (segY0 + y1) * 0.5f, az,
                               0.10f, (y1 - segY0) * 0.5f, 0.10f, kBrassGlow, 0.6f);
            } else {
                addEmissiveBox(scene, device, m_meshes,
                               ax - kFloorHalf + 0.20f, colCy, az + o,
                               0.10f, colHalfH, 0.10f, kBrassGlow, 0.6f);
            }
        }
        m_trimGlowCount = scene.size() - m_trimGlowFirst;
    }

    // ===== Per-floor ACCENT COVES: one emissive strip low on the far (+X)
    // wall per room, in the room's accent color — from the glass curtain the
    // five floors read as five differently-coloured wonder rooms even before
    // Phase 3 furnishes them. SHELL-owned span (the room glow spans stay
    // empty until Phase 3 — the self-test asserts that).
    {
        m_accentFirst = scene.size();
        for (uint32_t i = 0; i < kFloorCount; ++i) {
            const float glow[3] = { kRoomSpec[i].accent[0], kRoomSpec[i].accent[1],
                                    kRoomSpec[i].accent[2] };
            addEmissiveBox(scene, device, m_meshes,
                           ax + kFloorHalf - 0.55f, kFloorBaseY[i] + 0.9f, az,
                           0.06f, 0.14f, kFloorHalf - 2.0f, glow, 1.5f);
        }
        m_accentCount = scene.size() - m_accentFirst;
    }

    // ===== THE BORE — 4 m octagonal corridor from the shaft to the annex glass
    // wall at the floor-B level, brass-ribbed every 3 m. The cab traverses its
    // axis (the lateral rail y = 15 - cabHalfY); the centerline sits kBoreLift
    // above the floor-B base so the tube clears the cab top + a standing rider.
    {
        const float cy    = kFloorBaseY[1] + kBoreLift;         // centerline y
        const float x0    = shaftX + 1.8f;                      // just past the shaft
        const float x1    = ax - kFloorHalf;                    // the annex wall plane
        const float len   = x1 - x0;
        const float bx    = (x0 + x1) * 0.5f;
        const float panelHalfW = kBoreRadius * std::tan(3.1415926f / kBoreSides) + 0.06f;
        // 8 wall panels around the X axis (basis: local +X = world +X, local
        // +Y = radial, local +Z = tangent; det +1).
        for (uint32_t s = 0; s < kBoreSides; ++s) {
            const float a  = ((float)s + 0.5f) * (6.2831853f / kBoreSides);
            const float ry = std::cos(a), rz = std::sin(a);
            const float xA[3] = { 1, 0, 0 };
            const float yA[3] = { 0, ry, rz };
            const float zA[3] = { 0, -rz, ry };
            x3::prims::PrimMesh pm = x3::prims::makeBox(len * 0.5f, 0.12f, panelHalfW,
                                                        0, 0, 0, 0.15f);
            Entity e;
            e.mesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                       pm.index.data(), (uint32_t)pm.index.size());
            m_meshes.push_back(e.mesh);
            if (sIron.ok) { e.tex = sIron.albedo; e.normalTex = sIron.normal; e.mrTex = sIron.mr; }
            e.baseColor[0] = kBoreIron[0]; e.baseColor[1] = kBoreIron[1];
            e.baseColor[2] = kBoreIron[2]; e.baseColor[3] = 1.0f;
            // A small warm emissive floor so the tube interior reads as a lit
            // service bore, not a void (X3_WORLD_RULES: flat emissive well
            // under the ACES clip; this is a floor, not a light show).
            e.emissive[0] = 0.55f; e.emissive[1] = 0.46f; e.emissive[2] = 0.55f;
            e.emissive[3] = 0.10f;
            e.tag = (uint32_t)Tag::Static;
            makeXform(e.transform, xA, yA, zA,
                      bx, cy + (kBoreRadius + 0.12f) * ry, az + (kBoreRadius + 0.12f) * rz);
            scene.add(e);
        }
        // Physics: the four CARDINAL panels as axis-aligned catch boxes (floor /
        // ceiling / sides) so a rider who steps out of the cab mid-bore lands on
        // tube, not void. The diagonals stay visual-only.
        physics.addBox(x3::phys::Vec3{ len * 0.5f, 0.12f, kBoreRadius },
                       x3::phys::Vec3{ bx, cy - kBoreRadius - 0.12f, az },
                       0.0f, x3::phys::Layer::Static);
        physics.addBox(x3::phys::Vec3{ len * 0.5f, 0.12f, kBoreRadius },
                       x3::phys::Vec3{ bx, cy + kBoreRadius + 0.12f, az },
                       0.0f, x3::phys::Layer::Static);
        physics.addBox(x3::phys::Vec3{ len * 0.5f, kBoreRadius, 0.12f },
                       x3::phys::Vec3{ bx, cy, az - kBoreRadius - 0.12f },
                       0.0f, x3::phys::Layer::Static);
        physics.addBox(x3::phys::Vec3{ len * 0.5f, kBoreRadius, 0.12f },
                       x3::phys::Vec3{ bx, cy, az + kBoreRadius + 0.12f },
                       0.0f, x3::phys::Layer::Static);

        // Brass RIBS every 3 m: ONE shared torus mesh (pushed ONCE into
        // m_meshes), instanced per rib; the rib entities are the contiguous
        // m_ribGlow span tick() pulses (the powered-bore cue). Centerline
        // radius sits INSIDE the tube shell (panels run r 2.0-2.24) so each
        // rib reads as a full glowing ring from the cab, not a poked-through
        // cusp — verified against the first bore-ride capture.
        x3::prims::PrimMesh torus = x3::prims::makeTorus(kBoreRadius - 0.12f, 0.08f, 24, 10);
        x3::rhi::MeshHandle ribMesh = device.createMesh(
            torus.verts.data(), (uint32_t)torus.verts.size(),
            torus.index.data(), (uint32_t)torus.index.size());
        m_meshes.push_back(ribMesh);   // shared handle: ONCE
        m_ribGlowFirst = scene.size();
        // Torus is authored in its local XY plane (axis +Z); aim the axis down
        // world +X: xAxis=(0,0,-1), yAxis=(0,1,0), zAxis=(1,0,0) — det +1.
        const float xA[3] = { 0, 0, -1 }, yA[3] = { 0, 1, 0 }, zA[3] = { 1, 0, 0 };
        for (float rx = x0 + 1.5f; rx < x1 - 0.5f; rx += kRibPitch) {
            Entity e;
            e.mesh = ribMesh;
            if (sTrim.ok) { e.tex = sTrim.albedo; e.normalTex = sTrim.normal; e.mrTex = sTrim.mr; }
            e.baseColor[0] = kBrassTint[0]; e.baseColor[1] = kBrassTint[1];
            e.baseColor[2] = kBrassTint[2]; e.baseColor[3] = 1.0f;
            e.emissive[0] = kBrassGlow[0]; e.emissive[1] = kBrassGlow[1];
            e.emissive[2] = kBrassGlow[2]; e.emissive[3] = 0.42f;
            e.tag = (uint32_t)Tag::Prop;
            makeXform(e.transform, xA, yA, zA, rx, cy, az);
            scene.add(e);
        }
        m_ribGlowCount = scene.size() - m_ribGlowFirst;
    }

    // ===== Shaft-side landings (F1 / F3): 12x12 pads around the shaft with the
    // same 4.4 m cab opening, so the player has somewhere to stand and call the
    // cab. (The Spire proper is NOT built here — this world is the annex.)
    addSlabWithOpening(scene, device, physics, m_meshes,
                       shaftX, kFloorBaseY[0], shaftZ, kLandHalf, &sDeck, kFloorTint);
    addSlabWithOpening(scene, device, physics, m_meshes,
                       shaftX, kFloorBaseY[1], shaftZ, kLandHalf, &sDeck, kFloorTint);

    // ===== Per-room content hooks (factory_rooms.cpp — Phase 3 fills them).
    {
        FactoryRoomCtx ctx{ scene, device, physics, m_meshes, m_surf, ax, az,
                            &m_river, &m_art };
        buildRoomMixture  (ctx, m_rooms[0]);
        buildRoomInvention(ctx, m_rooms[1]);
        buildRoomFizz     (ctx, m_rooms[2]);
        buildRoomSorting  (ctx, m_rooms[3]);
        buildRoomTube     (ctx, m_rooms[4]);
    }

    // ===== The 10 trigger AABBs (ids 300-313; TriggerSystem latches each once).
    {
        using FT = FactoryTrigger;
        auto add = [&](FT id, float x0, float y0, float z0,
                       float x1, float y1, float z1) {
            triggers.add(x3::phys::Vec3{ x0, y0, z0 }, x3::phys::Vec3{ x1, y1, z1 },
                         (uint32_t)id);
        };
        const float boreCy = kFloorBaseY[1] + kBoreLift;
        // 300 bore entry: the shaft-side tube mouth.
        add(FT::BoreEntry, shaftX + 1.0f, boreCy - 2.5f, az - 2.2f,
                           shaftX + 6.0f, boreCy + 2.5f, az + 2.2f);
        // 301-305 room entries: a 6x3x6 volume on each floor beside the cab
        // landing (stepping off the cab into the room crosses it).
        const FT roomIds[kFloorCount] = { FT::RoomMixture, FT::RoomInvention,
                                          FT::RoomFizz, FT::RoomSorting, FT::RoomTube };
        for (uint32_t i = 0; i < kFloorCount; ++i)
            add(roomIds[i], ax - 3.0f, kFloorBaseY[i], az - 3.0f,
                            ax + 3.0f, kFloorBaseY[i] + 3.0f, az + 3.0f);
        // 310 sorter chute (Floor D): the 2x2 hatch footprint.
        add(FT::SorterChute, ax + 7.0f, kFloorBaseY[3], az + 7.0f,
                             ax + 9.0f, kFloorBaseY[3] + 2.5f, az + 9.0f);
        // 311 fizz low-grav (Floor C): the central zone. +-7.5 (not the plan's
        // +-8): the chute drop-shaft column at (8,8) must NOT clip the zone —
        // a player falling the shaft through floor C is not in the gallery.
        add(FT::FizzLowGrav, ax - 7.5f, kFloorBaseY[2], az - 7.5f,
                             ax + 7.5f, kFloorBaseY[2] + 3.5f, az + 7.5f);
        // 312 burst-arm dais (Floor E): under the roof opening, ringing the cab.
        add(FT::BurstArm, ax - 4.0f, kFloorBaseY[4], az - 4.0f,
                          ax + 4.0f, kFloorBaseY[4] + 2.5f, az + 4.0f);
        // 313 tube-ride boarding point (Floor E, tube-fan corner).
        add(FT::TubeRide, ax - 14.0f, kFloorBaseY[4], az - 14.0f,
                          ax - 10.0f, kFloorBaseY[4] + 3.0f, az - 10.0f);
    }

    // ===== The annex point-light rig (static; hosts merge it with the
    // elevator's lights into ONE setPointLights push). One warm accent-tinted
    // light per floor at the room ceiling + a brass pair along the bore, so
    // the rooms READ through the glass curtain and the ride is lit.
    {
        auto addLight = [&](float x, float y, float z,
                            float r, float g, float b, float range) {
            x3::rhi::PointLight pl{};
            pl.pos[0] = x; pl.pos[1] = y; pl.pos[2] = z;
            pl.color[0] = r; pl.color[1] = g; pl.color[2] = b;
            pl.range = range;
            m_lights.push_back(pl);
        };
        for (uint32_t i = 0; i < kFloorCount; ++i) {
            const float* a = kRoomSpec[i].accent;
            // Warm white pulled toward the room accent, mid-height so the DECK
            // catches it (a 20 m room lit only from the ceiling reads black at
            // the floor — verified on the first bore-ride capture).
            addLight(ax, kFloorBaseY[i] + 6.0f, az,
                     7.0f + 2.5f * a[0], 6.3f + 2.5f * a[1], 5.8f + 2.5f * a[2],
                     36.0f);
            // Phase 5: three FILLS per floor. One key at the center left
            // every 20 m room's corners pitch black — the Glimvale dressing,
            // the padded room and the crate corners lived entirely in ambient
            // (the floor-A art-pass capture showed the bank garden as black
            // blobs). Soft warm fills, deliberately dimmer than the key.
            addLight(ax - 14.0f, kFloorBaseY[i] + 5.5f, az + 9.0f,
                     2.8f, 2.2f, 2.0f, 21.0f);
            addLight(ax + 13.0f, kFloorBaseY[i] + 5.5f, az - 11.0f,
                     2.8f, 2.2f, 2.0f, 21.0f);
            addLight(ax - 9.0f, kFloorBaseY[i] + 6.0f, az - 9.0f,
                     2.2f, 1.75f, 1.6f, 18.0f);
        }
        // Floor A extras: the north-east work corner (padded room + crates)
        // and the west bank garden both face AWAY from the key — a third fill
        // each so the confection river's banks read at dusk.
        addLight(ax + 11.0f, kFloorBaseY[0] + 5.0f, az + 11.0f, 2.3f, 1.8f, 1.5f, 15.0f);
        addLight(ax - 14.0f, kFloorBaseY[0] + 5.5f, az - 9.0f, 2.0f, 1.5f, 1.5f, 15.0f);
        const float boreCy = kFloorBaseY[1] + kBoreLift;
        addLight(shaftX + 12.0f, boreCy, az, 2.4f, 1.9f, 1.1f, 14.0f);   // brass bore
        addLight(shaftX + 30.0f, boreCy, az, 2.4f, 1.9f, 1.1f, 14.0f);
    }

    m_entCount = scene.size() - m_entFirst;
    m_built = true;
    char msg[256];
    std::snprintf(msg, sizeof(msg),
        "[factory] annex shell built: %u rooms, %u entities, %u meshes, "
        "%u triggers (300-313), center x=%.0f | hero hooks %u real / %u fallback, "
        "%u dressing entities",
        (uint32_t)m_rooms.size(), m_entCount, (uint32_t)m_meshes.size(),
        kFactoryTrigCount, ax, m_art.heroPresent, m_art.heroFallback,
        m_art.dressEntities);
    x3::logInfo(msg);
}

void FactoryAnnex::tick(float dt, Scene& scene) {
    if (!m_built) return;
    m_time += dt;
    const float t = m_time;
    // Bore ribs: the powered-bore breathing pulse (per-rib phase offsets).
    // Phase-5 EMISSIVE RESTRAINT: warm accents hold <= 0.45 steady (the art
    // direction's glow law) — the ribs are brass PBR first, glow second.
    for (uint32_t i = 0; i < m_ribGlowCount; ++i) {
        Entity& e = scene.get(m_ribGlowFirst + i);
        e.emissive[3] = 0.32f + 0.12f * std::sin(t * 1.4f + (float)i * 0.45f);
    }
    // Glass-curtain mullions: a slower, subtler breath (warm cap 0.45).
    for (uint32_t i = 0; i < m_trimGlowCount; ++i) {
        Entity& e = scene.get(m_trimGlowFirst + i);
        e.emissive[3] = 0.32f + 0.10f * std::sin(t * 0.6f + (float)i * 1.1f);
    }
    // Per-floor accent coves: a gentle candy-shop breathe, phase per floor.
    // Warm accents (raspberry/amber/gold) hold ~0.4; cool (mint/cyan) may
    // breathe to ~0.9 (the cool cap) — the cove is a STRIP, not a lightbox.
    {
        constexpr float coveBase[kFloorCount] = { 0.38f, 0.72f, 0.38f, 0.38f, 0.72f };
        constexpr float coveAmp[kFloorCount]  = { 0.07f, 0.16f, 0.07f, 0.07f, 0.16f };
        for (uint32_t i = 0; i < m_accentCount && i < kFloorCount; ++i) {
            Entity& e = scene.get(m_accentFirst + i);
            e.emissive[3] = coveBase[i] + coveAmp[i] * std::sin(t * 0.5f + (float)i * 1.3f);
        }
    }
    // The Chute of Dubious Quality (Task 10): once trigger 310 latches, the
    // Sorting Hall's clock runs — tickRoomSorting slides the hatch open after
    // the 1.5 s stand-on beat.
    if (m_chute && m_rooms.size() > 3) m_rooms[3].stateA += dt;
    // Room content animation (Phase 3): per-room hooks + a gentle accent pulse
    // on whatever glow span a room registers.
    tickRoomMixture  (scene, m_rooms[0], t);
    tickRoomInvention(scene, m_rooms[1], t);
    tickRoomFizz     (scene, m_rooms[2], t);
    tickRoomSorting  (scene, m_rooms[3], t);
    tickRoomTube     (scene, m_rooms[4], t);
}

void FactoryAnnex::onTrigger(uint32_t triggerId) {
    if (!m_built) return;
    using FT = FactoryTrigger;
    switch ((FT)triggerId) {
    case FT::BoreEntry:
        if (!m_boreEntered) {
            m_boreEntered = true;
            x3::logInfo("[factory] bore entry — the lateral corridor to the Annex");
        }
        return;
    case FT::RoomMixture: case FT::RoomInvention: case FT::RoomFizz:
    case FT::RoomSorting: case FT::RoomTube: {
        const uint32_t idx = triggerId - (uint32_t)FT::RoomMixture;
        if (idx < m_rooms.size() && !m_rooms[idx].visited) {
            m_rooms[idx].visited = true;
            x3::logInfo(std::string("[factory] entered ") + m_rooms[idx].name);
        }
        return;
    }
    case FT::SorterChute:
        if (!m_chute) { m_chute = true; x3::logInfo("[factory] the Chute of Dubious Quality (Phase 3 opens it)"); }
        return;
    case FT::FizzLowGrav:
        if (!m_lowGrav) { m_lowGrav = true; x3::logInfo("[factory] FIZZ zone — low gravity (host scales jump x1.8)"); }
        return;
    case FT::BurstArm:
        if (!m_burstDais) { m_burstDais = true; x3::logInfo("[factory] the golden dais — the roof is not the limit (9999)"); }
        return;
    case FT::TubeRide:
        if (!m_tubeRide) { m_tubeRide = true; x3::logInfo("[factory] tube junction boarding point (Phase 3 rides it)"); }
        return;
    default: return;
    }
}

void FactoryAnnex::shutdown(x3::rhi::IRenderDevice& device) {
    if (!m_built) return;
    // ONE vector, freed uniformly (shared handles were pushed exactly once).
    for (auto h : m_meshes) if (h.valid()) device.destroyMesh(h);
    m_meshes.clear();
    m_surf.destroyAll(device);
    // Phase-5 GLB models: unload each (mesh buffers freed; the loader's
    // process-wide texture cache keeps its own steady-state refs — see F15's
    // double build/shutdown balance check).
    if (m_loader) for (auto& m : m_models) m_loader->unload(m);
    m_models.clear();
    m_loader.reset();
    m_assets.reset();
    m_art = FactoryArtHooks{};
    m_rooms.clear();
    m_ribGlowFirst = m_ribGlowCount = 0;
    m_trimGlowFirst = m_trimGlowCount = 0;
    m_accentFirst = m_accentCount = 0;
    m_lights.clear();
    m_river = x3::rhi::IRenderDevice::WaterParams{};
    m_entFirst = m_entCount = 0;
    m_lowGrav = m_chute = m_burstDais = m_tubeRide = m_boreEntered = false;
    m_time = 0.0f;
    m_built = false;
}

void FactoryAnnex::applyAtmosphere(x3::rhi::IRenderDevice& device) const {
    // Dark-iron interior with warm dust in the air; the brass trim + (Phase 3)
    // room emissives carry the wonder. A dim warm dome gives the brass and the
    // glass curtain something to reflect (the rifthub R10 lesson: metal is lit
    // by being shiny AT something) and doubles as the sky the Burst apex sees.
    x3::rhi::IRenderDevice::FogParams fog;
    fog.enabled  = true;
    fog.color[0] = 0.030f; fog.color[1] = 0.022f; fog.color[2] = 0.034f;   // aubergine haze
    fog.density  = 0.012f;
    fog.start    = 4.0f;
    fog.maxOpacity = 0.65f;
    device.setFog(fog);
    x3::rhi::IRenderDevice::SkyParams sp{};
    sp.enabled = true;
    sp.sunDir[0] = -0.25f; sp.sunDir[1] = 0.72f; sp.sunDir[2] = 0.35f;
    sp.sunColor[0] = 1.00f; sp.sunColor[1] = 0.86f; sp.sunColor[2] = 0.62f;
    sp.sunIntensity = 0.55f;                        // low amber evening sun
    sp.haze = 0.75f; sp.exposure = 1.0f;
    sp.zenith[0] = 0.10f; sp.zenith[1] = 0.08f; sp.zenith[2] = 0.16f;
    sp.horizon[0] = 0.55f; sp.horizon[1] = 0.34f; sp.horizon[2] = 0.24f;   // toffee dusk
    device.setSkyParams(sp);
    device.setAmbient(0.072f, 0.062f, 0.082f);      // low aubergine ambient (Phase-5 lift:
                                                    // real albedos are darker than the old
                                                    // flat tints; blacks need a floor)
    device.setIblProbe(false);
    device.setIblIntensity(0.08f);
    device.setIblSpecular(0.9f);
    device.setExposure(1.0f);
}

// ===========================================================================
// Headless self-test (--test-factory). Lives HERE per the plan (T6 step 2).
// ===========================================================================
namespace {
int fa_pass = 0, fa_fail = 0;
bool faCheck(bool ok, const char* what) {
    if (ok) { ++fa_pass; x3::logInfo(std::string("  PASS ") + what); }
    else    { ++fa_fail; x3::logError(std::string("  FAIL ") + what); }
    return ok;
}
// Leak-counting headless device: the VMA-leak analogue a no-op device can
// prove — every mesh/texture created must be destroyed by shutdown().
class CountingDevice final : public HeadlessRenderDevice {
public:
    int meshLive = 0, texLive = 0;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t vc,
                                   const uint32_t* i, uint32_t ic) override {
        ++meshLive; return HeadlessRenderDevice::createMesh(v, vc, i, ic);
    }
    void destroyMesh(x3::rhi::MeshHandle h) override {
        --meshLive; HeadlessRenderDevice::destroyMesh(h);
    }
    x3::rhi::TextureHandle createTexture(const void* px, uint32_t w, uint32_t h,
                                         bool mips) override {
        ++texLive; return HeadlessRenderDevice::createTexture(px, w, h, mips);
    }
    void destroyTexture(x3::rhi::TextureHandle h) override {
        --texLive; HeadlessRenderDevice::destroyTexture(h);
    }
};
} // namespace

bool runFactoryAnnexSelfTest() {
    fa_pass = fa_fail = 0;
    constexpr float dt = 1.0f / 60.0f;

    // ---- F1: the shell builds — 5 rooms, real entity/mesh counts, spans sane.
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    phys->init();
    CountingDevice device;
    Scene scene;
    TriggerSystem triggers;
    FactoryAnnex annex;
    annex.build(scene, device, *phys, triggers, /*shaftX*/0.0f, /*shaftZ*/0.0f);
    faCheck(annex.built() && annex.roomCount() == 5,
            "F1 annex builds with 5 wonder-room floors");
    // Shell floor count: 6 slabs x 4 segments + 3 walls + glass + trim + bore
    // panels + ribs + landings — assert honest lower bounds + span validity.
    faCheck(annex.entityCount() >= 60 && annex.meshCount() >= 50 &&
            annex.entityFirst() + annex.entityCount() == scene.size(),
            "F1b shell entity/mesh counts + span in range");
    {
        // Phase-3 law: the per-room content spans are PINNED as each room
        // lands. Phase 5: the Sorting arms + Tube capsule are load-if-present
        // HERO slots — when the forge delivers, one prop entity becomes N
        // drawable entities, so those two pins flex by the room's recorded
        // hero prim counts (0 == procedural fallback == the Phase-3 numbers).
        const uint32_t armN  = annex.room(3).heroArmPrims  ? annex.room(3).heroArmPrims  : 1u;
        const uint32_t capsN = annex.room(4).heroCapsPrims ? annex.room(4).heroCapsPrims : 1u;
        struct Pin { uint32_t prop, glow; };
        const Pin want[FactoryAnnex::kFloorCount] = {
            { 6, 56 },   // A MIXTURE ATRIUM: 6 stir arms; 48 rim studs + 8 river glow
            { 40, 15 },  // B INVENTION WORKS: 8 movers + 24 slats + 8 gizmos;
                         //   5 studs + 8 organ keys + centrifuge + maybe-panel
            { 49, 4 },   // C FIZZ GALLERY: 40 bubbles + 9 fan blades; 4 collars
            { 12u + 2u * armN + 1u, 56 },   // D SORTING HALL: 12 orbs + 2 arms + hatch;
                         //   4 hatch rims + 52 sign strokes (25 + 27)
            { capsN, 17 },   // E TUBE JUNCTION: the capsule; 12 dais studs + 5 collars
        };
        bool pinsOk = true;
        for (uint32_t i = 0; i < annex.roomCount(); ++i) {
            const AnnexRoom& r = annex.room(i);
            if (r.propEntCount != want[i].prop || r.glowEntCount != want[i].glow) {
                pinsOk = false;
                char pb[128];
                std::snprintf(pb, sizeof(pb),
                    "  F1c room %u spans: prop %u (want %u) glow %u (want %u)",
                    i, r.propEntCount, want[i].prop, r.glowEntCount, want[i].glow);
                x3::logError(pb);
            }
            if (r.propEntCount && r.propEntFirst + r.propEntCount > scene.size()) pinsOk = false;
            if (r.glowEntCount && r.glowEntFirst + r.glowEntCount > scene.size()) pinsOk = false;
        }
        faCheck(pinsOk, "F1c per-room content spans pinned (A landed)");
    }

    // ---- F2: Floor A stir arms MOVE — tick 30 frames, a stir arm's rotation
    // basis must change (pose poke on the prop span; plan T7 step 4).
    {
        const AnnexRoom& rA = annex.room(0);
        const uint32_t arm = rA.propEntFirst;
        const float b0 = scene.get(arm).transform[0], b2 = scene.get(arm).transform[2];
        for (int i = 0; i < 30; ++i) annex.tick(dt, scene);
        const bool moved =
            std::fabs(scene.get(arm).transform[0] - b0) > 1e-4f ||
            std::fabs(scene.get(arm).transform[2] - b2) > 1e-4f;
        faCheck(moved, "F2 stir arm rotates across 30 ticks (span pose poke)");
    }

    // ---- F3: the confection river's water params are registered (the host
    // applies them): enabled, raspberry-tinted, surface BELOW every deck.
    {
        const auto wp = annex.riverWater();
        faCheck(wp.enabled &&
                std::fabs(wp.seaLevel - FactoryAnnex::kRiverSurfY) < 1e-4f &&
                wp.seaLevel < FactoryAnnex::kFloorBaseY[0] &&
                wp.shallowColor[0] > wp.shallowColor[1] &&      // raspberry: R-dominant
                wp.shallowColor[0] > wp.shallowColor[2],
                "F3 river water params registered (raspberry, sunken surface)");
    }

    // ---- F4: the Invention Works' eight machines RING the center (the cab
    // chain owns the middle 4.4 m) and every mover sits inside the floor.
    {
        const AnnexRoom& rB = annex.room(1);
        bool ringOk = rB.propEntCount == 40;
        for (uint32_t i = 0; i < 8 && ringOk; ++i) {
            const Entity& e = scene.get(rB.propEntFirst + i);
            const float dx = e.transform[12] - rB.centerX;
            const float dz = e.transform[14] - rB.centerZ;
            const float r2 = dx * dx + dz * dz;
            if (r2 < 2.2f * 2.2f) ringOk = false;                    // center law
            if (std::fabs(dx) > 20.0f || std::fabs(dz) > 20.0f) ringOk = false;
        }
        faCheck(ringOk, "F4 invention works: 8 machines ring the center, in-floor");
    }

    // ---- F5: the conveyor wraps — a slat scrolls, stays on the belt, and
    // jumps back when it reaches the end (14 m at 1.2 m/s => wrap < 13 s).
    {
        const AnnexRoom& rB = annex.room(1);
        const uint32_t slat = rB.propEntFirst + 8;                   // first slat
        const float az0 = rB.centerZ;
        bool moved = false, wrapped = false, onBelt = true;
        float prevZ = scene.get(slat).transform[14];
        for (int i = 0; i < 60 * 13; ++i) {
            annex.tick(dt, scene);
            const float z = scene.get(slat).transform[14];
            if (std::fabs(z - prevZ) > 1e-5f) moved = true;
            if (z < prevZ - 5.0f) wrapped = true;                    // the wrap jump
            if (std::fabs(z - az0) > 7.1f) onBelt = false;
            prevZ = z;
        }
        faCheck(moved && wrapped && onBelt,
                "F5 conveyor slat scrolls, wraps, and never leaves the belt");
    }

    // ---- F6: Fizz Gallery bubbles RISE and WRAP at the ceiling (rise span
    // 10 m at 1.1 m/s => a wrap inside 10 s; the bubble never leaves its column).
    {
        const AnnexRoom& rC = annex.room(2);
        const uint32_t bub = rC.propEntFirst;                // column 0, bubble 0
        bool rose = false, wrapped = false, inColumn = true;
        float prevY = scene.get(bub).transform[13];
        for (int i = 0; i < 60 * 10; ++i) {
            annex.tick(dt, scene);
            const float y = scene.get(bub).transform[13];
            if (y > prevY + 1e-5f) rose = true;
            if (y < prevY - 5.0f)  wrapped = true;           // the ceiling wrap
            if (y < rC.baseY + 0.4f || y > rC.baseY + 11.0f) inColumn = false;
            prevY = y;
        }
        faCheck(rose && wrapped && inColumn,
                "F6 fizz bubbles rise, wrap at the ceiling, stay in the column");
    }

    // ---- F7: the low-grav zone latches (trigger 311; the HOST scales the
    // jump impulse x1.8 off lowGravActive — wiring lives in host_factory.cpp).
    {
        annex.onTrigger((uint32_t)FactoryTrigger::FizzLowGrav);
        faCheck(annex.lowGravActive(), "F7 fizz low-grav zone latches (311)");
    }

    // ---- F8: the orb ring ORBITS — radius pinned at 8 m from the center,
    // angle advancing (0.5 rad/s), duds riding the same ring.
    {
        const AnnexRoom& rD = annex.room(3);
        const uint32_t orb = rD.propEntFirst;
        const Entity& e0 = scene.get(orb);
        const float a0 = std::atan2(e0.transform[14] - rD.centerZ,
                                    e0.transform[12] - rD.centerX);
        bool radiusOk = true;
        for (int i = 0; i < 60 * 2; ++i) {                  // 2 s => ~1 rad
            annex.tick(dt, scene);
            const float dx = scene.get(orb).transform[12] - rD.centerX;
            const float dz = scene.get(orb).transform[14] - rD.centerZ;
            const float r = std::sqrt(dx * dx + dz * dz);
            if (std::fabs(r - 8.0f) > 0.05f) radiusOk = false;
        }
        const Entity& e1 = scene.get(orb);
        const float a1 = std::atan2(e1.transform[14] - rD.centerZ,
                                    e1.transform[12] - rD.centerX);
        float swept = a1 - a0;
        while (swept < -3.1415926f) swept += 6.2831853f;
        while (swept >  3.1415926f) swept -= 6.2831853f;
        faCheck(radiusOk && std::fabs(swept - 1.0f) < 0.15f,
                "F8 orb ring orbits the center (r=8, 0.5 rad/s)");
    }

    // ---- F9: the Chute of Dubious Quality — trigger 310 + the 1.5 s beat
    // slides the hatch open (visual x moves the full 2.4 m slide).
    {
        const AnnexRoom& rD = annex.room(3);
        const uint32_t armN = rD.heroArmPrims ? rD.heroArmPrims : 1u;
        const uint32_t hatch = rD.propEntFirst + 12u + 2u * armN;
        const float x0 = scene.get(hatch).transform[12];
        annex.onTrigger((uint32_t)FactoryTrigger::SorterChute);
        faCheck(annex.chuteTripped(), "F9 chute trigger (310) latches");
        for (int i = 0; i < 60 * 3; ++i) annex.tick(dt, scene);   // 3 s > 1.5 + 0.8
        const float slid = scene.get(hatch).transform[12] - x0;
        faCheck(slid > 2.3f, "F9b hatch slides open after the 1.5 s stand-on beat");
    }

    // ---- F10: the pneumatic capsule traverses AND pauses — over one full
    // cycle it must show both sustained motion and a >= 1.8 s dock stillness,
    // never leave the tube-fan corner, and bump the docking event counter.
    {
        const AnnexRoom& rE = annex.room(4);
        const uint32_t cap = rE.propEntFirst;
        const uint32_t ev0 = rE.eventCount;
        float px = scene.get(cap).transform[12];
        float py = scene.get(cap).transform[13];
        float stillRun = 0.0f, bestStill = 0.0f;
        bool moved = false, inBounds = true;
        for (int i = 0; i < 60 * 20; ++i) {              // 20 s > one full cycle
            annex.tick(dt, scene);
            const float x = scene.get(cap).transform[12];
            const float y = scene.get(cap).transform[13];
            const float step = std::fabs(x - px) + std::fabs(y - py);
            if (step < 1e-5f) { stillRun += dt; if (stillRun > bestStill) bestStill = stillRun; }
            else              { stillRun = 0.0f; moved = true; }
            const float lx = x - rE.centerX;
            if (lx < -18.5f || lx > -5.5f || y < rE.baseY + 1.0f || y > rE.baseY + 10.5f)
                inBounds = false;
            px = x; py = y;
        }
        faCheck(moved && bestStill >= 1.8f && inBounds,
                "F10 capsule traverses the polyline with 2 s dock pauses");
        faCheck(annex.room(4).eventCount > ev0,
                "F10b capsule docking bumps the host cue counter");
    }

    // ---- F11: all 10 triggers registered with the exact 300-313 id map.
    {
        const uint32_t ids[kFactoryTrigCount] = { 300, 301, 302, 303, 304,
                                                  305, 310, 311, 312, 313 };
        bool all = triggers.count() == kFactoryTrigCount;
        for (uint32_t id : ids) if (!triggers.findById(id)) all = false;
        faCheck(all, "F11 all 10 triggers registered (300-305 + 310-313)");
    }

    // ---- F12: the combined elevator graph reaches A5 from F1 THROUGH the 4790
    // unlock. Locked first (callTo must be a no-op), then the code, then the
    // full multi-leg ride: F1 -> F3 (vertical) -> A2 (lateral bore) -> A5
    // (annex chain), full-stop at every corner by design.
    {
        Scene evScene;   // separate scene: entity ids stay annex-clean above
        HeadlessRenderDevice evDevice;
        ElevatorSystem elev;
        const FactoryAnnex::ElevatorGraph g = FactoryAnnex::makeElevatorGraph(0.0f, 0.0f);
        const bool built = elev.buildEx(evScene, evDevice, *phys,
                                        FactoryAnnex::kCabHalfX, FactoryAnnex::kCabHalfY,
                                        FactoryAnnex::kCabHalfZ, g.stops, g.rails, g.f1);
        elev.enableFsm(true);
        elev.setFloorLabels(g.labels);
        elev.setBurst(g.a5, FactoryAnnex::kRoofY, 105.0f);
        faCheck(built && elev.stopCount() == 7, "F12 combined graph builds (7 stops)");
        elev.callTo(g.a5);
        faCheck(elev.state() == ElevState::Idle && !elev.hiddenUnlocked(),
                "F12b locked annex: callTo(A5) is a no-op before the code");
        elev.keypadDigit(4); elev.keypadDigit(7); elev.keypadDigit(9); elev.keypadDigit(0);
        faCheck(elev.hiddenUnlocked(), "F12c keypad 4790 unlocks the annex rail");
        elev.callTo(g.a5);
        bool arrived = false;
        for (int i = 0; i < 60 * 120 && !arrived; ++i) {   // 120 s sim budget
            elev.update(dt, evScene, *phys);
            arrived = (elev.state() == ElevState::DoorsOpen &&
                       elev.currentStop() == g.a5);
        }
        const x3::phys::Vec3 c = elev.cabCenter();
        faCheck(arrived &&
                std::fabs(c.x - FactoryAnnex::kAnnexXOff) < 0.05f &&
                std::fabs(c.y - (FactoryAnnex::kFloorBaseY[4] - FactoryAnnex::kCabHalfY)) < 0.05f,
                "F12d cab rides F1 -> F3 -> lateral bore -> A5 and opens the doors");
        faCheck(elev.floorLabel(g.a5) == "A5",
                "F12e setFloorLabels wired (Phase-1 handoff: labels are host-fed)");
    }

    // ---- F13: tick() animates the shell — a bore rib's emissive moves.
    {
        // Rib span is annex-internal; probe via a known glow entity: tick twice
        // at different times and require SOME entity emissive[3] to change.
        std::vector<float> before(scene.size());
        for (uint32_t i = 0; i < scene.size(); ++i) before[i] = scene.get(i).emissive[3];
        for (int i = 0; i < 30; ++i) annex.tick(dt, scene);
        bool moved = false;
        for (uint32_t i = 0; i < scene.size(); ++i)
            if (std::fabs(scene.get(i).emissive[3] - before[i]) > 1e-4f) moved = true;
        faCheck(moved, "F13 tick() pulses the bore/trim glow spans");
    }

    // ---- F14: onTrigger latches — room visited + low-grav + chute + dais.
    {
        annex.onTrigger((uint32_t)FactoryTrigger::RoomMixture);
        annex.onTrigger((uint32_t)FactoryTrigger::FizzLowGrav);
        annex.onTrigger((uint32_t)FactoryTrigger::SorterChute);
        annex.onTrigger((uint32_t)FactoryTrigger::BurstArm);
        faCheck(annex.room(0).visited && annex.lowGravActive() &&
                annex.chuteTripped() && annex.burstDaisVisited(),
                "F14 onTrigger latches visited/low-grav/chute/dais");
        annex.onTrigger((uint32_t)FactoryTrigger::RoomMixture);   // idempotent
        faCheck(annex.room(0).visited, "F14b onTrigger is idempotent");
    }

    // ---- F16 (Phase 5): the load-if-present hero hooks. Every one of the 17
    // hook sites (6 vats + 8 machine bodies + 1 capsule + 2 sorter arms) must
    // have taken exactly one branch, and the helper must behave correctly in
    // BOTH branches when probed directly: a missing GLB returns 0, tallies the
    // fallback and authors NOTHING; a present GLB (a committed Glimvale mesh)
    // authors >0 drawable entities and tallies present. The present-branch
    // probe is skipped on an asset-less clone (dressing == 0 means no GLB
    // resolved — the same graceful degradation the hooks themselves have).
    {
        faCheck(annex.heroHooksPresent() + annex.heroHooksFallback() == 17,
                "F16 all 17 hero hook sites took exactly one branch");
        std::vector<x3::rhi::MeshHandle> probeMeshes;
        SurfaceLibrary probeSurf;
        std::vector<x3::asset::Model> probeModels;
        Scene probeScene;
        std::unique_ptr<x3::asset::IAssetSource> src(x3::asset::createAssetSource());
        std::unique_ptr<x3::asset::IModelLoader> ld;
        if (src->mountDir(convertedGlbRoot(), 0))
            ld.reset(x3::asset::createModelLoader(&device, src.get()));
        FactoryArtHooks probeArt;
        probeArt.loader = ld.get();
        probeArt.models = &probeModels;
        FactoryRoomCtx probeCtx{ probeScene, device, *phys, probeMeshes,
                                 probeSurf, 0.0f, 0.0f, nullptr, &probeArt };
        const uint32_t ents0 = probeScene.size();
        const int miss = heroHook(probeCtx, "FactoryProps/__NeverDelivered__.glb",
                                  "F16MissProbe", 0, 0, 0, 0.0f, 1.0f, false, nullptr);
        faCheck(miss == 0 && probeArt.heroFallback == 1 &&
                probeScene.size() == ents0,
                "F16b missing GLB: 0 entities, fallback branch tallied+logged");
        if (annex.dressEntityCount() > 0) {
            const int hit = heroHook(probeCtx, "Glimvale/SM_Stylized_Barrel.glb",
                                     "F16HitProbe", 0, 0, 0, 0.0f, 1.0f, false, nullptr);
            faCheck(hit > 0 && probeArt.heroPresent == 1 &&
                    probeScene.size() == ents0 + (uint32_t)hit,
                    "F16c present GLB: drawable entities, present branch tallied+logged");
        } else {
            x3::logWarn("[factory-test] F16c present-branch probe SKIPPED "
                        "(no converted_glb assets on this clone)");
        }
        if (ld) for (auto& m : probeModels) ld->unload(m);
    }

    // ---- F15: clean shutdown. MESHES: every create balanced by a destroy
    // (the headless VMA-leak analogue). TEXTURES: the model loader keeps a
    // process-wide texture cache that DELIBERATELY retains one ref per unique
    // GLB texture (the boot-time cache), so absolute zero only holds for
    // meshes; for textures the honest assertion is the STEADY STATE — a full
    // second build+shutdown cycle must not grow the live count by even one.
    {
        annex.shutdown(device);
        const int texSteady = device.texLive;
        faCheck(!annex.built() && annex.meshCount() == 0 && device.meshLive == 0,
                "F15 shutdown frees every mesh (create/destroy balance)");
        Scene scene2;
        TriggerSystem trig2;
        annex.build(scene2, device, *phys, trig2, /*shaftX*/0.0f, /*shaftZ*/0.0f);
        annex.shutdown(device);
        faCheck(device.meshLive == 0 && device.texLive == texSteady,
                "F15b rebuild+shutdown holds steady state (no cumulative leak)");
    }

    phys->shutdown();
    char msg[96];
    std::snprintf(msg, sizeof(msg), "factory: %d/%d passed", fa_pass, fa_pass + fa_fail);
    if (fa_fail == 0) x3::logInfo(msg); else x3::logError(msg);
    return fa_fail == 0;
}

} // namespace x3::game
