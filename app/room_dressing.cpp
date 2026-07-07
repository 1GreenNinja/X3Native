// WAVE-3 ROOM-DRESSING RECIPES — see room_dressing.h. Dressing math follows the
// cell_dressing laws: 0.14 m inset vs graybox planes (0.2 m slabs centered), yaw-only
// prop placement, instance emissive[3] SCALES material emissive, contact shadows
// ground props, one key light per room, one accent hue per zone (ART_BIBLE §2/§3).
#include "room_dressing.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kInset  = 0.14f;   // wall/ceiling panel inset vs the graybox plane
constexpr float kFloorLift = 0.012f;

// ---- Zones (recipe ids). 0 = no recipe (room left graybox). -------------------------
enum Zone : uint8_t { ZNone = 0, ZHall, ZCorridor, ZWard, ZSecurity, ZLab, ZBoss,
                      ZLobby, ZStorage,
                      // W3-2 — the tower floors (ART_BIBLE §3 zone palettes):
                      ZMedical,    // F2 Medical Bay — clinical sickly-green
                      ZGenetics,   // F3 Genetics Lab — green pushed harder
                      ZCyber,      // F4 Cybernetics — dark steel + cyan instruments
                      ZDroneBay,   // F5 Drone Station — industrial hangar + amber caution
                      ZSalvari,    // F6 Salvari — darkest, alien, biolume green
                      ZExec,       // F7 Executive — clean dark luxury + brass (new bible entry)
                      ZCave,       // W5-1 F4.5 Nexus Chamber — fog/atmosphere ONLY (canon_45
                                   // hand-dresses the cavern; no panel recipe)
                      ZCount };

// ---- Converted-kit props (paths + probed AABBs, from the cell_dressing tables). -----
const char* kRelConsole = "ModularSciFi_Interior/SM_Console.glb";
const char* kRelPipes   = "ModularSciFi_Interior/SM_Pipes_A.glb";
const char* kRelCrateS  = "SciFi_Warehouse_Kit/Crate Short.glb";
const char* kRelCrateL  = "SciFi_Warehouse_Kit/Crate Long.glb";
const char* kRelBarrel  = "SciFi_Warehouse_Kit/Barrel.glb";
const char* kRelPallet  = "SciFi_Warehouse_Kit/Pallet.glb";
const char* kRelBin     = "SciFi_Warehouse_Kit/Garbage Bin.glb";
const char* kRelCot     = "Detention/SM_Hospital_Bed.glb";

struct Aabb { float minx, miny, minz, maxx, maxy, maxz; };
constexpr Aabb kConsAabb   { -0.47f,  0.00f, -0.31f,  0.31f, 1.55f, 0.29f };
constexpr Aabb kPipesAabb  { -0.123f,-0.011f,  0.00f,  0.539f,0.164f,3.000f };
constexpr Aabb kCrateSAabb { -0.668f, 0.000f,-0.000f,  0.000f,0.600f,0.671f };
constexpr Aabb kCrateLAabb { -0.640f, 0.000f, 0.000f,  0.000f,0.600f,1.274f };
constexpr Aabb kBarrelAabb { -0.441f, 0.000f,-0.456f,  0.440f,1.225f,0.425f };
constexpr Aabb kPalletAabb { -1.559f, 0.000f,-0.005f,  0.003f,0.198f,1.519f };
constexpr Aabb kBinAabb    { -0.277f, 0.001f,-0.277f,  0.277f,1.061f,0.277f };
constexpr Aabb kCotAabb    { -1.200f, 0.100f,-0.600f,  1.100f,1.200f,0.600f };
inline float acx(const Aabb& a) { return (a.minx + a.maxx) * 0.5f; }
inline float acz(const Aabb& a) { return (a.minz + a.maxz) * 0.5f; }

// ---- Per-zone recipe: surfaces, palette, fog (ART_BIBLE §3 zone table). --------------
struct Recipe {
    const char* wall;   float wallTile;
    const char* floor;  float floorTile;
    const char* ceil;   float ceilTile;
    // key light (the ONE statement), fill (<= half), accent (the ONE hue)
    float keyR, keyG, keyB, keyRange;
    float accR, accG, accB, accRange;
    x3::rhi::IRenderDevice::FogParams fog;
};

x3::rhi::IRenderDevice::FogParams fogOf(float r, float g, float b, float d,
                                        float start, float cap) {
    x3::rhi::IRenderDevice::FogParams f;
    f.enabled = true; f.color[0] = r; f.color[1] = g; f.color[2] = b;
    f.density = d; f.start = start; f.maxOpacity = cap;
    return f;
}

// Indexed by Zone. Texture sets are AD-3's curated survivors (matlib-verified).
const Recipe& recipeFor(uint8_t z) {
    static const Recipe kRecipes[ZCount] = {
        /*ZNone*/     {},
        /*ZHall*/     { "mw_metal_trim_b", 2.8f, "sr_rubberfloor", 2.2f, "mw_metal_panels_a", 3.0f,
                        1.55f, 1.70f, 1.90f, 6.0f,   0.14f, 0.75f, 0.85f, 2.6f,
                        fogOf(0.030f, 0.040f, 0.046f, 0.0045f, 1.5f, 0.65f) },
        /*ZCorridor*/ { "mw_concrete_panels_a", 2.6f, "sr_rubberfloor", 2.2f, "mw_metal_panels_a", 3.0f,
                        1.30f, 1.42f, 1.58f, 5.0f,   0.14f, 0.75f, 0.85f, 2.4f,
                        fogOf(0.030f, 0.040f, 0.046f, 0.0045f, 1.5f, 0.65f) },
        /*ZWard*/     { "hh_wall_01a", 3.0f, "hh_floor_01a", 2.4f, "hh_ceiling_01a", 2.8f,
                        2.20f, 1.70f, 1.05f, 3.6f,   1.50f, 0.95f, 0.25f, 2.2f,
                        fogOf(0.045f, 0.040f, 0.034f, 0.0035f, 1.2f, 0.60f) },
        /*ZSecurity*/ { "mw_concrete_panels_a", 2.4f, "mw_metal_grate", 2.0f, "mw_metal_panels_a", 3.0f,
                        1.90f, 1.90f, 2.00f, 3.2f,   1.40f, 0.07f, 0.05f, 2.2f,
                        fogOf(0.020f, 0.022f, 0.026f, 0.0030f, 1.2f, 0.55f) },
        /*ZLab*/      { "mw_plaster_painted", 2.6f, "sr_rubberfloor", 2.2f, "hh_ceiling_01a", 2.8f,
                        2.30f, 2.50f, 2.30f, 6.5f,   0.25f, 1.10f, 0.35f, 2.6f,
                        fogOf(0.038f, 0.046f, 0.040f, 0.0030f, 1.5f, 0.55f) },
        /*ZBoss*/     { "sr_concrete_01", 2.8f, "sr_concrete_a", 2.6f, "mw_metal_panels_a", 3.2f,
                        2.30f, 1.80f, 1.10f, 6.0f,   1.50f, 0.95f, 0.25f, 2.6f,
                        fogOf(0.045f, 0.040f, 0.034f, 0.0040f, 1.4f, 0.62f) },
        /*ZLobby*/    { "mw_metal_trim_a", 2.8f, "sr_rubberfloor", 2.2f, "mw_metal_panels_a", 3.0f,
                        1.50f, 1.62f, 1.78f, 4.6f,   0.14f, 0.75f, 0.85f, 2.4f,
                        fogOf(0.030f, 0.040f, 0.046f, 0.0040f, 1.5f, 0.60f) },
        /*ZStorage*/  { "mw_concrete_panels_b", 2.6f, "sr_concrete_a", 2.4f, "mw_metal_panels_a", 3.0f,
                        1.35f, 1.15f, 0.85f, 4.2f,   1.50f, 0.95f, 0.25f, 2.2f,
                        fogOf(0.045f, 0.040f, 0.034f, 0.0035f, 1.2f, 0.60f) },
        // ---- W3-2 tower floors. Sets include AD-3's four previously-unused curated
        // survivors (cc_porous_cement, mw_thermal_padding, sr_metal_b, mw_metal_grate). ----
        /*ZMedical*/  { "hh_wall_01a", 3.0f, "hh_floor_01a", 2.4f, "hh_ceiling_01a", 2.8f,
                        2.10f, 2.30f, 2.10f, 5.5f,   0.30f, 1.05f, 0.35f, 2.6f,
                        fogOf(0.040f, 0.048f, 0.040f, 0.0032f, 1.4f, 0.55f) },
        /*ZGenetics*/ { "mw_plaster_painted", 2.6f, "hh_floor_01a", 2.4f, "hh_ceiling_01a", 2.8f,
                        1.90f, 2.30f, 1.95f, 5.5f,   0.20f, 1.20f, 0.30f, 2.8f,
                        fogOf(0.034f, 0.052f, 0.036f, 0.0042f, 1.4f, 0.60f) },
        /*ZCyber*/    { "sr_metal_b", 2.6f, "mw_metal_grate", 2.0f, "mw_metal_panels_a", 3.0f,
                        1.45f, 1.60f, 1.85f, 5.0f,   0.16f, 0.85f, 1.05f, 2.6f,
                        fogOf(0.024f, 0.032f, 0.040f, 0.0038f, 1.4f, 0.60f) },
        /*ZDroneBay*/ { "mw_thermal_padding", 2.8f, "mw_metal_grate", 2.2f, "mw_metal_panels_a", 3.2f,
                        1.75f, 1.65f, 1.45f, 6.5f,   1.55f, 0.95f, 0.25f, 2.8f,
                        fogOf(0.035f, 0.035f, 0.032f, 0.0035f, 1.5f, 0.60f) },
        /*ZSalvari*/  { "sr_concrete_01", 2.8f, "sr_concrete_a", 2.6f, "sr_concrete_01", 3.2f,
                        1.10f, 0.92f, 0.62f, 4.5f,   0.25f, 1.10f, 0.45f, 2.8f,
                        fogOf(0.018f, 0.026f, 0.021f, 0.0060f, 1.2f, 0.72f) },
        /*ZExec*/     { "cc_porous_cement", 3.2f, "sr_concrete_a", 2.6f, "mw_metal_panels_a", 3.2f,
                        2.00f, 1.80f, 1.50f, 5.5f,   1.60f, 1.15f, 0.45f, 2.6f,
                        fogOf(0.040f, 0.036f, 0.030f, 0.0025f, 1.6f, 0.50f) },
        // W5-1: the Nexus Chamber — no surfaces/lights (canon_45 owns the look);
        // the fog IS the recipe: near-black, heavy, silhouettes-over-detail.
        /*ZCave*/     { nullptr, 0, nullptr, 0, nullptr, 0,
                        0, 0, 0, 0,   0, 0, 0, 0,
                        fogOf(0.010f, 0.014f, 0.010f, 0.0140f, 0.8f, 0.88f) },
    };
    return kRecipes[z < ZCount ? z : ZNone];
}

// Classify a canon room into a recipe zone by name/type (case-sensitive canonical data).
uint8_t classify(const CanonRoom& r, const CanonBeats& bt, uint32_t roomId) {
    if (roomId == bt.jakeCell) return ZNone;          // frozen hand-calibrated reference
    if (r.cy < -50.0f)         return ZNone;          // Cave / Hidden Sub-Level (organic zone, later)
    auto has = [&](const char* s) { return r.name.find(s) != std::string::npos; };

    // ---- W3-2: TOWER FLOORS route by ELEVATION BAND (the data ships absolute
    // elevations: F2 y~10, F3 ~20, F4 ~30, the F4.5 tiers 33..64, F5 ~65, F6 ~78,
    // F7 ~91). Structural kinds keep the shared recipes; everything else takes the
    // floor's zone. The F4.5 Cave-Chamber tiers stay ZNone (the organic monster
    // zone is its own future pass, like the deep caves).
    if (r.cy > 5.0f) {
        // W5-1: the Nexus Chamber (4.5 tiers + the open-ceiling Access room) takes the
        // CAVE zone — no panels (canon_45 hand-dresses), but the heavy near-black fog
        // rides the zone-atmosphere path like every other zone.
        if (r.type == "Cave Chamber" || r.platform || r.openCeiling) return ZCave;
        if (has("Elevator"))                         return ZLobby;
        // Long tower corridors (18-24 m) need the HALL treatment (light RHYTHM +
        // trim walls) — a single mid key leaves them black tunnels (R2 eye round).
        if (has("Hall") || has("Corridor"))
            return (std::max(r.w, r.d) >= 14.0f) ? ZHall : ZCorridor;
        if (has("Boss") || r.type == "Boss Arena")   return ZBoss;
        if (r.type == "Holding Cell")                return ZWard;   // Quarantine / Sarah's cell
        if (has("Security") || has("Guard") || has("Armory") || has("Weapons Locker"))
                                                     return ZSecurity;
        if (r.type == "Storage" || has("Storage") || has("Coolant") || has("Power Junction")
            || has("Maintenance") || has("Recharge") || has("Cold Room")) return ZStorage;
        const float y = r.cy;
        if (y < 18.0f)  return ZMedical;             // F2 wards / theaters / pharmacy
        if (y < 28.0f)  return ZGenetics;            // F3
        if (y < 33.0f)  return ZCyber;               // F4
        if (y < 76.0f)  return ZDroneBay;            // F5
        if (y < 88.0f)  return ZSalvari;             // F6
        return ZExec;                                // F7 + roof rooms
    }

    // ---- Floor 1 (unchanged from W3-1). ----
    if (has("Main Hall"))                            return ZHall;
    if (has("Hall") || has("Corridor"))              return ZCorridor;
    if (roomId == bt.security || has("Security") || has("Armory")) return ZSecurity;
    if (roomId == bt.research || has("Research") || has("Lab") || has("Genetics")
        || roomId == bt.medical || has("Medical"))   return ZLab;
    if (roomId == bt.bossArena || has("Boss"))       return ZBoss;
    if (has("Elevator"))                             return ZLobby;
    if (has("Storage") || has("Supply") || has("Mess") || has("Cafeteria")) return ZStorage;
    if (r.type == "Cell" || has("WL-") || has("WR-") || has("EL-") || has("ER-")
        || has("Ward"))                              return ZWard;
    return ZNone;
}

// ---- Face/segment math for opening-aware wall tiling. --------------------------------
// Faces: 0 = x0 plane (panel faces +X), 1 = x1 (-X), 2 = z0 (+Z), 3 = z1 (-Z).
struct Cut { float lo, hi; };

void collectCuts(const CanonFloor& floor, uint32_t room, const CanonRoom& r,
                 std::vector<Cut> cuts[4]) {
    const float m = 0.35f;   // clearance beyond the cut for jambs/frames
    for (const CanonDoorway& d : floor.doorways) {
        if (d.a != room && d.b != room) continue;
        if (d.kind == DoorwayKind::CrossLevel) continue;   // vertical tube, not a wall cut
        if (d.axis == 0) {   // wall plane X=const -> cut on face 0 or 1, span along Z
            const int f = (std::fabs(d.cx - r.x0()) <= std::fabs(d.cx - r.x1())) ? 0 : 1;
            cuts[f].push_back({ d.cz - d.cutHalf - m, d.cz + d.cutHalf + m });
        } else {             // wall plane Z=const -> cut on face 2 or 3, span along X
            const int f = (std::fabs(d.cz - r.z0()) <= std::fabs(d.cz - r.z1())) ? 2 : 3;
            cuts[f].push_back({ d.cx - d.cutHalf - m, d.cx + d.cutHalf + m });
        }
    }
}

// Subtract cut intervals from [lo..hi]; append surviving segments >= minLen.
void segments(float lo, float hi, std::vector<Cut>& cuts, float minLen,
              std::vector<Cut>& out) {
    std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.lo < b.lo; });
    float cur = lo;
    for (const Cut& c : cuts) {
        if (c.lo > cur && (std::min(c.lo, hi) - cur) >= minLen)
            out.push_back({ cur, std::min(c.lo, hi) });
        cur = std::max(cur, c.hi);
        if (cur >= hi) return;
    }
    if (hi - cur >= minLen) out.push_back({ cur, hi });
}

// Column-major yaw-about-Y / pitch-about-X rotations composed into a TR transform.
void makeTR(float t[16], float yawY, float pitchX, float px, float py, float pz) {
    const float cy = std::cos(yawY),  sy = std::sin(yawY);
    const float cx = std::cos(pitchX), sx = std::sin(pitchX);
    // R = Ry * Rx (columns = images of basis vectors)
    t[0] = cy;        t[1] = 0.0f;  t[2]  = -sy;       t[3]  = 0;
    t[4] = sy * sx;   t[5] = cx;    t[6]  = cy * sx;   t[7]  = 0;
    t[8] = sy * cx;   t[9] = -sx;   t[10] = cy * cx;   t[11] = 0;
    t[12] = px;       t[13] = py;   t[14] = pz;        t[15] = 1;
}

} // namespace

// ---- Mesh helpers --------------------------------------------------------------------

x3::rhi::MeshHandle RoomDressing::quadMesh(x3::rhi::IRenderDevice& device,
                                           float w, float h, float tileMeters) {
    // Dedupe by quantized dims (0.25 m grid) + tile size.
    const uint64_t key = (uint64_t)std::lround(w * 4.0f) << 40 |
                         (uint64_t)std::lround(h * 4.0f) << 16 |
                         (uint64_t)std::lround(tileMeters * 8.0f);
    for (auto& kv : m_quadCache) if (kv.first == key) return kv.second;
    // Centered XY quad, normal +Z, UVs repeat every tileMeters. Double-sided winding
    // so the orient math can never backface-cull a wall out of existence.
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float tu = w / tileMeters, tv = h / tileMeters;
    x3::rhi::MeshVertex v[4] = {};
    const float px[4] = { -hw,  hw,  hw, -hw };
    const float py[4] = { -hh, -hh,  hh,  hh };
    const float uu[4] = { 0, tu, tu, 0 };
    const float vv[4] = { tv, tv, 0, 0 };
    for (int i = 0; i < 4; ++i) {
        v[i].pos[0] = px[i]; v[i].pos[1] = py[i]; v[i].pos[2] = 0.0f;
        v[i].normal[2] = 1.0f; v[i].uv[0] = uu[i]; v[i].uv[1] = vv[i];
    }
    const uint32_t idx[12] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 };
    x3::rhi::MeshHandle mh = device.createMesh(v, 4, idx, 12);
    m_quadCache.push_back({ key, mh });
    return mh;
}

uint32_t RoomDressing::loadAsset(const std::string& rel) {
    for (uint32_t i = 0; i < m_assetPaths.size(); ++i)
        if (m_assetPaths[i] == rel) return i;
    Asset a;
    a.model = m_loader->load(rel);
    if (a.model.ok) {
        a.drawables = x3::asset::makeDrawables(a.model);
        a.ok = !a.drawables.empty();
    }
    if (!a.ok) x3::logWarn("[room-dress] failed to load " + rel + " (prop skipped)");
    m_assetTable.push_back(std::move(a));
    m_assetPaths.push_back(rel);
    return (uint32_t)m_assetTable.size() - 1;
}

void RoomDressing::placeProp(uint32_t room, uint32_t asset, float yaw, float s,
                             float ax, float ay, float az,
                             float wx, float wy, float wz,
                             const float emissive[4], const float tint[4]) {
    if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
    const float c = std::cos(yaw), sn = std::sin(yaw);
    PropInst e; e.room = room; e.asset = asset;
    e.transform[0] = c * s;  e.transform[2] = -sn * s;
    e.transform[5] = s;
    e.transform[8] = sn * s; e.transform[10] = c * s;
    e.transform[15] = 1.0f;
    const float rpx = (c * ax + sn * az) * s;
    const float rpy = ay * s;
    const float rpz = (-sn * ax + c * az) * s;
    e.transform[12] = wx - rpx; e.transform[13] = wy - rpy; e.transform[14] = wz - rpz;
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    if (tint)     for (int i = 0; i < 4; ++i) e.tint[i]     = tint[i];
    m_props.push_back(e);
}

// ---- Build ----------------------------------------------------------------------------

bool RoomDressing::build(x3::rhi::IRenderDevice& device,
                         std::string_view surfaceLibDir, std::string_view convertedGlbDir,
                         const CanonFloor& floor, const CanonBeats& beats) {
    if (!floor.valid()) return false;
    m_surf.mount(std::string(surfaceLibDir));
    m_assets.reset(x3::asset::createAssetSource());
    if (m_assets->mountDir(convertedGlbDir, 0))
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));

    m_roomZone.assign(floor.rooms.size(), ZNone);
    m_zoneFog.assign(ZCount, recipeFor(ZWard).fog);   // default = detention tint
    for (uint8_t z = 1; z < ZCount; ++z) m_zoneFog[z] = recipeFor(z).fog;

    // Surface sets loaded once up front (name -> stable cache pointer).
    auto setIdx = [&](const char* name) -> uint32_t {
        const SurfaceSet& s = m_surf.get(device, name);
        m_sets.push_back(&s);
        return (uint32_t)m_sets.size() - 1;
    };

    const uint32_t aConsole = m_loader ? loadAsset(kRelConsole) : 0;
    const uint32_t aPipes   = m_loader ? loadAsset(kRelPipes)   : 0;
    const uint32_t aCrateS  = m_loader ? loadAsset(kRelCrateS)  : 0;
    const uint32_t aCrateL  = m_loader ? loadAsset(kRelCrateL)  : 0;
    const uint32_t aBarrel  = m_loader ? loadAsset(kRelBarrel)  : 0;
    const uint32_t aPallet  = m_loader ? loadAsset(kRelPallet)  : 0;
    const uint32_t aBin     = m_loader ? loadAsset(kRelBin)     : 0;
    const uint32_t aCot     = m_loader ? loadAsset(kRelCot)     : 0;

    // Kit tints (cell_dressing palette family).
    const float tCrate[4]  = { 0.66f, 0.60f, 0.52f, 1.0f };
    const float tBarrel[4] = { 0.46f, 0.34f, 0.25f, 1.0f };
    const float tPallet[4] = { 0.48f, 0.38f, 0.26f, 1.0f };
    const float tSteel[4]  = { 0.42f, 0.45f, 0.50f, 1.0f };
    const float tDarkCon[4] = { 0.18f, 0.21f, 0.28f, 1.0f };

    // Shadow-disc mesh (radial fade, drawn through the glass pass) — cell_dressing's.
    x3::rhi::MeshHandle disc{};
    {
        std::vector<x3::rhi::MeshVertex> verts;
        std::vector<uint32_t> idx;
        auto push = [&](float x, float z) {
            x3::rhi::MeshVertex mv{}; mv.pos[0] = x; mv.pos[2] = z;
            mv.normal[1] = 1.0f; mv.uv[0] = 0.5f; verts.push_back(mv);
        };
        push(0, 0);
        const int seg = 20;
        for (int i = 0; i <= seg; ++i) {
            const float t = (float)i / seg * 2.0f * kPi;
            push(std::cos(t), std::sin(t));
        }
        for (int i = 1; i <= seg; ++i) {
            idx.push_back(0); idx.push_back(i); idx.push_back(i + 1);
            idx.push_back(0); idx.push_back(i + 1); idx.push_back(i);
        }
        disc = device.createMesh(verts.data(), (uint32_t)verts.size(),
                                 idx.data(), (uint32_t)idx.size());
    }
    auto shadowBlob = [&](uint32_t room, float x, float y, float z, float rx, float rz,
                          float dark) {
        ProcDraw p; p.room = room; p.mesh = disc; p.glass = true;
        p.color[0] = 0.02f; p.color[1] = 0.02f; p.color[2] = 0.03f; p.color[3] = dark;
        p.transform[0] = rx; p.transform[5] = 1; p.transform[10] = rz; p.transform[15] = 1;
        p.transform[12] = x; p.transform[13] = y + kFloorLift; p.transform[14] = z;
        m_proc.push_back(p);
    };

    uint32_t nPanels = 0, nLightsBefore = 0;
    for (uint32_t ri = 0; ri < floor.rooms.size(); ++ri) {
        const CanonRoom& r = floor.rooms[ri];
        const uint8_t z = classify(r, beats, ri);
        if (z == ZNone) continue;
        m_roomZone[ri] = z;
        const Recipe& rec = recipeFor(z);
        // W5-1: fog-only zones (ZCave — the Nexus Chamber) carry NO surface recipe;
        // canon_45 hand-dresses them. The zone tag above still drives the fog.
        if (!rec.wall) continue;
        const uint32_t wallSet  = setIdx(rec.wall);
        const uint32_t floorSet = setIdx(rec.floor);
        const uint32_t ceilSet  = setIdx(rec.ceil);

        const float fY = r.y0(), cY = r.y1();
        const float wallH = (cY - fY) - 0.05f;
        const float wallCy = (fY + cY) * 0.5f;

        // ---- Walls: opening-aware segments per face (Law 1: never cover a door). ----
        std::vector<Cut> cuts[4];
        collectCuts(floor, ri, r, cuts);
        std::vector<Cut> segs;
        auto addWallPanel = [&](int face, float lo, float hi) {
            const float len = hi - lo;
            Panel p; p.room = ri; p.set = wallSet;
            p.mesh = quadMesh(device, len, wallH, rec.wallTile);
            const float mid = (lo + hi) * 0.5f;
            switch (face) {
                case 0: makeTR(p.transform,  kPi * 0.5f, 0, r.x0() + kInset, wallCy, mid); break;
                case 1: makeTR(p.transform, -kPi * 0.5f, 0, r.x1() - kInset, wallCy, mid); break;
                case 2: makeTR(p.transform,  0.0f,       0, mid, wallCy, r.z0() + kInset); break;
                case 3: makeTR(p.transform,  kPi,        0, mid, wallCy, r.z1() - kInset); break;
            }
            m_panels.push_back(p); ++nPanels;
        };
        for (int f = 0; f < 4; ++f) {
            segs.clear();
            const float lo = (f < 2) ? r.z0() : r.x0();
            const float hi = (f < 2) ? r.z1() : r.x1();
            segments(lo + 0.05f, hi - 0.05f, cuts[f], 0.55f, segs);
            for (const Cut& s : segs) addWallPanel(f, s.lo, s.hi);
        }

        // ---- Floor + ceiling (single panels; floor skipped if a descent tube pierces
        // this room — the graybox segments there stay the truth). ----
        bool hasTube = false;
        for (const CanonDoorway& d : floor.doorways)
            if ((d.a == ri || d.b == ri) && d.kind == DoorwayKind::CrossLevel) hasTube = true;
        if (!hasTube) {
            Panel p; p.room = ri; p.set = floorSet;
            p.mesh = quadMesh(device, r.w - 0.1f, r.d - 0.1f, rec.floorTile);
            makeTR(p.transform, 0, -kPi * 0.5f, r.cx, fY + kFloorLift, r.cz);
            m_panels.push_back(p); ++nPanels;
        }
        {
            Panel p; p.room = ri; p.set = ceilSet;
            p.mesh = quadMesh(device, r.w - 0.1f, r.d - 0.1f, rec.ceilTile);
            makeTR(p.transform, 0, kPi * 0.5f, r.cx, cY - kInset, r.cz);
            m_panels.push_back(p); ++nPanels;
        }

        // ---- Lights: ONE key statement + accent at the first doorway (§2/§3). -------
        nLightsBefore = (uint32_t)m_lights.size();
        auto addLight = [&](float x, float y, float zp, float range,
                            float cr, float cg, float cb) {
            CanonLight cl; cl.room = ri;
            cl.light.pos[0] = x; cl.light.pos[1] = y; cl.light.pos[2] = zp;
            cl.light.range = range;
            cl.light.color[0] = cr; cl.light.color[1] = cg; cl.light.color[2] = cb;
            m_lights.push_back(cl);
        };
        const bool longX = r.w >= r.d;
        if (z == ZHall || z == ZCorridor) {
            // Rhythm of cool keys along the long axis (the corridor's ONE statement).
            const float len = longX ? r.w : r.d;
            const int nKeys = std::max(1, (int)(len / 8.0f));
            for (int i = 0; i < nKeys; ++i) {
                const float t = (i + 0.5f) / nKeys - 0.5f;
                addLight(r.cx + (longX ? t * len : 0), cY - 0.35f,
                         r.cz + (longX ? 0 : t * len),
                         rec.keyRange, rec.keyR, rec.keyG, rec.keyB);
            }
        } else {
            addLight(r.cx, cY - 0.5f, r.cz, rec.keyRange, rec.keyR, rec.keyG, rec.keyB);
            if (r.w * r.d > 40.0f)   // wide room: a dim fill at <= half the key
                addLight(r.cx, fY + 0.6f, r.cz, rec.keyRange * 0.8f,
                         rec.keyR * 0.4f, rec.keyG * 0.4f, rec.keyB * 0.4f);
        }
        // Accent at the first doorway threshold (the zone's ONE hue).
        for (const CanonDoorway& d : floor.doorways) {
            if (d.a != ri && d.b != ri) continue;
            if (d.kind == DoorwayKind::CrossLevel) continue;
            addLight(d.cx, fY + 2.0f, d.cz, rec.accRange, rec.accR, rec.accG, rec.accB);
            break;
        }

        // ---- W5-2: the WARD DOOR TELL — a thin amber light-under-the-door strip at
        // each ward threshold (RESCUE_SETPIECE_DESIGN.md §1.2: the read before the
        // burst-in is sound + light bleeding under the door, never a visual of the
        // act). Painted-glow discipline like the R2 guide strips: warm, low, floor-
        // level, spanning the door mouth along the wall the cut runs in. Amber = the
        // detention accent (§3), so the tell stays inside the zone's one-hue law.
        // F2's named wards classify ZMedical (the elevation rule wins over the name
        // rule), so cover BOTH: any ZWard room, or a Medical room actually named as
        // a ward (Ward A: Keisha / Ward B: Emily / Ward C: Aria / Sarah's cell).
        const bool wardTell = (z == ZWard) ||
            (z == ZMedical && r.name.find("Ward") != std::string::npos);
        if (wardTell) {
            for (const CanonDoorway& d : floor.doorways) {
                if (d.a != ri && d.b != ri) continue;
                if (d.kind == DoorwayKind::CrossLevel || d.junction) continue;
                const float span = d.cutHalf * 2.0f * 0.92f;   // just inside the jambs
                ProcDraw s; s.room = ri;
                // axis 0: wall plane X=const -> the mouth runs along Z (thin in X).
                s.mesh = quadMesh(device, d.axis == 0 ? 0.14f : span,
                                          d.axis == 0 ? span  : 0.14f, 1.0f);
                makeTR(s.transform, 0, -kPi * 0.5f, d.cx, fY + kFloorLift + 0.003f, d.cz);
                s.color[0] = 0.42f; s.color[1] = 0.26f; s.color[2] = 0.06f; s.color[3] = 1.0f;
                s.emissive[0] = 1.0f; s.emissive[1] = 0.58f; s.emissive[2] = 0.12f;
                s.emissive[3] = 0.85f;
                m_proc.push_back(s);
            }
        }

        // ---- Guide strips (hall/corridor leading line, §3.2 wayfinding). ------------
        if (z == ZHall || z == ZCorridor) {
            const float len = (longX ? r.w : r.d) - 1.2f;
            const int nSeg = std::max(1, (int)(len / 3.5f));
            for (int i = 0; i < nSeg; ++i) {
                const float t = (i + 0.5f) / nSeg - 0.5f;
                ProcDraw s; s.room = ri;
                s.mesh = quadMesh(device, longX ? 2.4f : 0.09f, longX ? 0.09f : 2.4f, 1.0f);
                makeTR(s.transform, 0, -kPi * 0.5f,
                       r.cx + (longX ? t * len : 0), fY + kFloorLift + 0.002f,
                       r.cz + (longX ? 0 : t * len));
                // R2: a PAINTED guide line, not a Tron beam — darker teal, glow well
                // under lamp level (§4 instrument law; round-1 read as a light source).
                s.color[0] = 0.04f; s.color[1] = 0.30f; s.color[2] = 0.34f; s.color[3] = 1.0f;
                s.emissive[0] = 0.06f; s.emissive[1] = 0.45f; s.emissive[2] = 0.50f;
                s.emissive[3] = 0.55f;
                m_proc.push_back(s);
            }
        }

        // ---- Hero props on a cut-free face (never block a doorway). -----------------
        if (m_loader) {
            int freeFace = -1;
            for (int f = 0; f < 4; ++f) if (cuts[f].empty()) { freeFace = f; break; }
            // face -> inward normal (nx,nz) + a wall-hug position at the face center
            const float margin = 0.55f;
            float px = r.cx, pz = r.cz, yaw = 0.0f;
            if (freeFace >= 0) {
                switch (freeFace) {
                    case 0: px = r.x0() + margin; pz = r.cz; yaw =  kPi * 0.5f; break;
                    case 1: px = r.x1() - margin; pz = r.cz; yaw = -kPi * 0.5f; break;
                    case 2: px = r.cx; pz = r.z0() + margin; yaw =  kPi;        break;
                    case 3: px = r.cx; pz = r.z1() - margin; yaw =  0.0f;       break;
                }
            }
            const uint32_t seed = ri * 2654435761u;
            const float jitter = ((seed >> 8 & 0xFF) / 255.0f - 0.5f) * 0.8f;
            switch (z) {
                case ZMedical:   // W3-2: the F2 wards (Keisha/Emily/Aria) get real cots
                case ZWard:
                    if (freeFace >= 0) {
                        // Cot long axis ALONG the wall: local X (2.3 m) -> face tangent.
                        const float cotYaw = (freeFace < 2) ? kPi * 0.5f : 0.0f;
                        placeProp(ri, aCot, cotYaw, 1.0f, acx(kCotAabb), kCotAabb.miny,
                                  acz(kCotAabb), px + (freeFace >= 2 ? jitter : 0),
                                  fY, pz + (freeFace < 2 ? jitter : 0), nullptr, nullptr);
                        shadowBlob(ri, px, fY, pz, 0.8f, 1.4f, 0.5f);
                    }
                    placeProp(ri, aBin, 0.0f, 0.9f, acx(kBinAabb), kBinAabb.miny,
                              acz(kBinAabb), r.x1() - 0.5f, fY + 0.02f, r.z1() - 0.5f,
                              nullptr, tSteel);
                    break;
                case ZSecurity: case ZLobby: case ZLab:
                case ZGenetics: case ZCyber: case ZExec:   // W3-2: console = the focal instrument
                    // R2: the task pool is the room's focal statement — keep it even when
                    // every face carries a doorway (small spine rooms); only the console
                    // prop needs the free wall.
                    if (freeFace < 0)
                        addLight(r.cx, fY + 2.1f, r.cz, rec.keyRange * 0.7f,
                                 rec.keyR * 0.9f, rec.keyG * 0.9f, rec.keyB * 0.9f);
                    if (freeFace >= 0) {
                        placeProp(ri, aConsole, yaw + kPi, 1.0f, acx(kConsAabb),
                                  kConsAabb.miny, kConsAabb.maxz, px, fY, pz,
                                  nullptr, tDarkCon);
                        shadowBlob(ri, px, fY, pz, 0.55f, 0.45f, 0.45f);
                        // Task key OVER the console (the room's focal point).
                        addLight(px, fY + 2.1f, pz, rec.keyRange * 0.7f,
                                 rec.keyR * 0.9f, rec.keyG * 0.9f, rec.keyB * 0.9f);
                    }
                    if (z == ZSecurity)
                        placeProp(ri, aCrateS, 0.3f + jitter, 1.0f, acx(kCrateSAabb),
                                  kCrateSAabb.miny, acz(kCrateSAabb),
                                  r.cx + 1.2f, fY + 0.02f, r.cz - 1.0f, nullptr, tCrate);
                    if (z == ZLab)
                        placeProp(ri, aBin, 0.0f, 0.9f, acx(kBinAabb), kBinAabb.miny,
                                  acz(kBinAabb), r.x0() + 0.5f, fY + 0.02f, r.z0() + 0.5f,
                                  nullptr, tSteel);
                    break;
                case ZBoss:
                    placeProp(ri, aBarrel, 0.0f, 1.0f, acx(kBarrelAabb), kBarrelAabb.miny,
                              acz(kBarrelAabb), r.cx - r.w * 0.28f, fY + 0.02f,
                              r.cz - r.d * 0.22f, nullptr, tBarrel);
                    placeProp(ri, aBarrel, 0.9f, 1.0f, acx(kBarrelAabb), kBarrelAabb.miny,
                              acz(kBarrelAabb), r.cx + r.w * 0.30f, fY + 0.02f,
                              r.cz + r.d * 0.18f, nullptr, tBarrel);
                    placeProp(ri, aCrateL, 1.1f + jitter, 1.0f, acx(kCrateLAabb),
                              kCrateLAabb.miny, acz(kCrateLAabb),
                              r.cx + r.w * 0.12f, fY + 0.02f, r.cz - r.d * 0.30f,
                              nullptr, tCrate);
                    shadowBlob(ri, r.cx - r.w * 0.28f, fY, r.cz - r.d * 0.22f, 0.55f, 0.55f, 0.45f);
                    shadowBlob(ri, r.cx + r.w * 0.30f, fY, r.cz + r.d * 0.18f, 0.55f, 0.55f, 0.45f);
                    break;
                case ZDroneBay:   // W3-2: hangar clutter = the storage kit under amber caution
                case ZStorage: {
                    placeProp(ri, aCrateL, jitter * 0.5f, 1.0f, acx(kCrateLAabb),
                              kCrateLAabb.miny, acz(kCrateLAabb), px, fY + 0.02f, pz,
                              nullptr, tCrate);
                    placeProp(ri, aCrateS, jitter * 0.5f, 1.0f, acx(kCrateSAabb),
                              kCrateSAabb.miny, acz(kCrateSAabb), px, fY + 0.62f, pz,
                              nullptr, tCrate);
                    placeProp(ri, aPallet, 0.0f, 1.0f, acx(kPalletAabb), kPalletAabb.miny,
                              acz(kPalletAabb), r.cx, fY + 0.02f, r.cz, nullptr, tPallet);
                    placeProp(ri, aBarrel, 0.4f, 1.0f, acx(kBarrelAabb), kBarrelAabb.miny,
                              acz(kBarrelAabb), r.cx - 1.4f, fY + 0.02f, r.cz + 1.1f,
                              nullptr, tBarrel);
                    shadowBlob(ri, px, fY, pz, 0.8f, 0.8f, 0.5f);
                    break;
                }
                case ZHall: {
                    // Overhead pipe runs along the hall's long axis (industrial read).
                    const float py = cY - 0.20f;
                    const float pipeYaw = longX ? kPi * 0.5f : 0.0f;
                    placeProp(ri, aPipes, pipeYaw, 1.0f, acx(kPipesAabb), kPipesAabb.maxy,
                              1.5f, r.cx, py, r.cz + (longX ? r.d * 0.28f : 0), nullptr, tSteel);
                    placeProp(ri, aPipes, pipeYaw, 1.0f, acx(kPipesAabb), kPipesAabb.maxy,
                              1.5f, r.cx + (longX ? 0 : r.w * 0.28f), py,
                              r.cz + (longX ? -r.d * 0.28f : 0), nullptr, tBarrel);
                    break;
                }
                default: break;
            }
        }
        ++m_roomsDressed;
    }
    (void)nLightsBefore;

    x3::logInfo("[room-dress] " + std::to_string(m_roomsDressed) + " rooms dressed: " +
                std::to_string(nPanels) + " surface panels, " +
                std::to_string(m_props.size()) + " props, " +
                std::to_string(m_lights.size()) + " recipe lights, " +
                std::to_string(m_proc.size()) + " strips/shadows");
    return m_roomsDressed > 0;
}

// ---- Draw ------------------------------------------------------------------------------

void RoomDressing::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const std::vector<uint32_t>& visibleRooms) const {
    if (m_panels.empty() && m_props.empty() && m_proc.empty()) return;
    // Per-frame visibility bitmap (same PVS set the scene cull uses).
    static thread_local std::vector<uint8_t> vis;
    vis.assign(m_roomZone.size(), 0);
    for (uint32_t v : visibleRooms) if (v < vis.size()) vis[v] = 1;

    for (const Panel& p : m_panels) {
        if (p.room >= vis.size() || !vis[p.room]) continue;
        m_surf.drawPanel(device, frame, *m_sets[p.set], p.mesh, p.transform);
    }
    for (const PropInst& e : m_props) {
        if (e.room >= vis.size() || !vis[e.room]) continue;
        const Asset& a = m_assetTable[e.asset];
        for (const auto& d : a.drawables) {
            float fin[16];
            x3::asset::mulMat4(e.transform, d.nodeTransform, fin);
            const bool matEmis = d.emissiveTexId != 0 || d.emissiveFactor[0] > 0.001f ||
                                 d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
            float emis[4];
            if (matEmis) { emis[0] = d.emissiveFactor[0]; emis[1] = d.emissiveFactor[1];
                           emis[2] = d.emissiveFactor[2]; emis[3] = e.emissive[3]; }
            else         { emis[0] = e.emissive[0]; emis[1] = e.emissive[1];
                           emis[2] = e.emissive[2]; emis[3] = e.emissive[3]; }
            const float bc[4] = { d.baseColorFactor[0] * e.tint[0],
                                  d.baseColorFactor[1] * e.tint[1],
                                  d.baseColorFactor[2] * e.tint[2],
                                  d.baseColorFactor[3] * e.tint[3] };
            device.drawMeshPBR(frame, x3::rhi::MeshHandle{ d.meshId },
                               x3::rhi::TextureHandle{ d.baseColorTexId },
                               x3::rhi::TextureHandle{ d.normalTexId },
                               x3::rhi::TextureHandle{ d.mrTexId },
                               bc, emis, fin, d.alphaMask, d.alphaBlend,
                               x3::rhi::TextureHandle{ d.emissiveTexId },
                               x3::rhi::TextureHandle{ d.detailTexId },
                               d.detailUvScale, d.clearcoat, d.clearcoatRough);
        }
    }
    const x3::rhi::TextureHandle white{ 0 };
    for (const ProcDraw& p : m_proc) {
        if (p.room >= vis.size() || !vis[p.room]) continue;
        if (p.glass) {
            x3::rhi::IRenderDevice::GlassMaterial gm;
            gm.opacity = p.color[3]; gm.refraction = 0.0f;
            gm.roughness = 1.0f; gm.specular = 0.0f;
            gm.tint[0] = p.color[0]; gm.tint[1] = p.color[1]; gm.tint[2] = p.color[2];
            device.drawMeshGlass(frame, p.mesh, white, p.color, p.emissive, gm, p.transform);
        } else {
            device.drawMeshEmissive(frame, p.mesh, white, p.color, p.emissive, p.transform);
        }
    }
}

void RoomDressing::applyZoneAtmosphere(x3::rhi::IRenderDevice& device, uint32_t eyeRoom) {
    int zone = ZWard;   // detention default (matches the cell_dressing base opt-in)
    if (eyeRoom < m_roomZone.size() && m_roomZone[eyeRoom] != ZNone)
        zone = m_roomZone[eyeRoom];
    if (zone == m_lastZone) return;
    m_lastZone = zone;
    device.setFog(m_zoneFog[(size_t)zone]);
}

} // namespace x3::game
