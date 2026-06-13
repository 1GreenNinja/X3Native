// INTERACTIVE WORLD MAP — tiles baked from real geometry + GTA-feel pan/zoom +
// POI discovery + waypoint + fast travel. See world_map.h.
// Clean-room, original work: X3Native's own Scene/level_loader/story_ops/ui +
// the public IRenderDevice interface only.
#include "world_map.h"

#include "world_stream.h"     // self-test: the streaming-aware fast-travel path
#include "headless_device.h"  // self-test device

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace x3::game {

// ===========================================================================
// MapCamera — pan/zoom math.
// ===========================================================================
void MapCamera::jumpTo(float wx, float wz, float s) {
    s = std::clamp(s, minScale, maxScale);
    cx = tCx = wx; cz = tCz = wz;
    scale = tScale = s;
    anchorActive = false;
}

void MapCamera::zoomAt(float pxX, float pxY, float wheelSteps) {
    if (wheelSteps == 0.0f) return;
    const float ns = std::clamp(tScale * std::pow(kWheelStepMul, wheelSteps),
                                minScale, maxScale);
    if (ns == tScale) return;
    // Anchor the world point CURRENTLY under the cursor; the lerp in update()
    // holds it fixed under that pixel for the whole animation (the GTA feel:
    // zoom centers on where you point).
    pxToWorld(pxX, pxY, anchorWx, anchorWz);
    anchorPx = pxX; anchorPy = pxY;
    anchorActive = true;
    tScale = ns;
    tCx = anchorWx - (anchorPx - vw * 0.5f) / ns;
    tCz = anchorWz - (anchorPy - vh * 0.5f) / ns;
}

void MapCamera::panPixels(float dxPx, float dyPx) {
    // Drag pan is IMMEDIATE (the map sticks to the cursor); targets follow.
    cx -= dxPx / scale; cz -= dyPx / scale;
    tCx = cx; tCz = cz;
    anchorActive = false;
}

void MapCamera::panWorld(float dxM, float dzM) {
    tCx += dxM; tCz += dzM;
    anchorActive = false;
}

void MapCamera::update(float dt) {
    if (dt <= 0.0f) return;
    // Scale lerps in LOG space (each octave of zoom takes equal time — no
    // "slow far / fast near" asymmetry).
    const float az = 1.0f - std::exp(-kZoomLerpRate * dt);
    const float ls = std::log(scale), lt = std::log(tScale);
    scale = std::exp(ls + (lt - ls) * az);
    if (std::fabs(std::log(tScale) - std::log(scale)) < 1e-4f) scale = tScale;

    if (anchorActive) {
        // Hold the anchored world point exactly under the anchor pixel through
        // the zoom (the invariant the self-test asserts), at EVERY intermediate
        // scale — not just at convergence.
        cx = anchorWx - (anchorPx - vw * 0.5f) / scale;
        cz = anchorWz - (anchorPy - vh * 0.5f) / scale;
        tCx = anchorWx - (anchorPx - vw * 0.5f) / tScale;
        tCz = anchorWz - (anchorPy - vh * 0.5f) / tScale;
        if (scale == tScale) anchorActive = false;
    } else {
        const float ap = 1.0f - std::exp(-kPanLerpRate * dt);
        cx += (tCx - cx) * ap;
        cz += (tCz - cz) * ap;
        if (std::fabs(tCx - cx) < 1e-3f) cx = tCx;
        if (std::fabs(tCz - cz) < 1e-3f) cz = tCz;
    }
}

void MapCamera::worldToPx(float wx, float wz, float& pxX, float& pxY) const {
    pxX = vw * 0.5f + (wx - cx) * scale;
    pxY = vh * 0.5f + (wz - cz) * scale;
}

void MapCamera::pxToWorld(float pxX, float pxY, float& wx, float& wz) const {
    wx = cx + (pxX - vw * 0.5f) / scale;
    wz = cz + (pxY - vh * 0.5f) / scale;
}

bool MapCamera::settled(float scaleEps, float panEpsM) const {
    return std::fabs(tScale - scale) <= scaleEps &&
           std::fabs(tCx - cx) <= panEpsM && std::fabs(tCz - cz) <= panEpsM;
}

// ===========================================================================
// POI table (assets/world/map_pois.json — x3.mappois/1).
// ===========================================================================
namespace {
float jnumf(const JValue* v, float d) { return (v && v->isNum()) ? (float)v->num : d; }
bool  jboolf(const JValue* v, bool d) { return (v && v->t == JValue::T::Bool) ? v->b : d; }
} // namespace

bool MapPoiTable::load(const std::string& path, std::vector<std::string>& errors) {
    pois.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) { errors.push_back(path + ": cannot open"); return false; }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JParser parser(src);
    JValue root = parser.parseValue();
    if (!parser.ok || !root.isObj()) { errors.push_back(path + ": JSON parse failed"); return false; }
    const std::string fmt = root.find("format") ? root.find("format")->asStr() : "";
    if (fmt != "x3.mappois/1") {
        errors.push_back(path + ": format is `" + fmt + "` (want x3.mappois/1)");
        return false;
    }
    const JValue* arr = root.find("pois");
    if (!arr || !arr->isArr()) { errors.push_back(path + ": missing `pois` array"); return false; }
    bool ok = true;
    for (const JValue& jp : *arr->arr) {
        if (!jp.isObj()) { errors.push_back(path + ": poi is not an object"); ok = false; continue; }
        MapPoi p;
        p.id     = jp.find("id")     ? jp.find("id")->asStr()     : "";
        p.name   = jp.find("name")   ? jp.find("name")->asStr()   : p.id;
        p.type   = jp.find("type")   ? jp.find("type")->asStr()   : "landmark";
        p.region = jp.find("region") ? jp.find("region")->asStr() : "";
        if (const JValue* a = jp.find("pos"); a && a->isArr() && a->arr->size() == 3) {
            p.x = jnumf(&(*a->arr)[0], 0.0f);
            p.y = jnumf(&(*a->arr)[1], 0.0f);
            p.z = jnumf(&(*a->arr)[2], 0.0f);
        }
        p.floor          = (int)jnumf(jp.find("floor"), 0.0f);
        p.discoverRadius = jnumf(jp.find("discover_radius"), 8.0f);
        p.fastTravel     = jboolf(jp.find("fast_travel"), true);
        if (p.id.empty()) { errors.push_back(path + ": poi missing `id`"); ok = false; continue; }
        if (indexOf(p.id) >= 0) { errors.push_back(path + ": duplicate poi id `" + p.id + "`"); ok = false; continue; }
        pois.push_back(std::move(p));
    }
    if (!ok) pois.clear();
    return ok;
}

int MapPoiTable::indexOf(std::string_view id) const {
    for (size_t i = 0; i < pois.size(); ++i)
        if (pois[i].id == id) return (int)i;
    return -1;
}

std::string worldMapPoisJsonPath() {
    static const char* kCandidates[] = {
        "assets/world/map_pois.json",
        "../assets/world/map_pois.json",
        "../../assets/world/map_pois.json",
        R"(D:\GameDev\X3Native-frustumcull\assets\world\map_pois.json)",
        R"(C:\GameDev\X3Native-engine\assets\world\map_pois.json)",
    };
    for (const char* c : kCandidates)
        if (std::filesystem::exists(c)) return c;
    return kCandidates[0];
}

std::string poiFoundFlag(const std::string& poiId)      { return "poi." + poiId + ".found"; }
std::string regionSeenFlag(const std::string& regionId) { return "region." + regionId + ".seen"; }

bool poiDiscovered(const StoryFlags& flags, const MapPoi& poi) {
    return flags.has(poiFoundFlag(poi.id));
}

FastTravelGate fastTravelGate(const MapPoi& poi, const StoryFlags& flags,
                              bool missionBlocksTravel) {
    if (!poi.fastTravel)               return FastTravelGate::NotAnAnchor;
    if (!poiDiscovered(flags, poi))    return FastTravelGate::Undiscovered;
    if (flags.has("alert.active"))     return FastTravelGate::Alert;     // the alert hook
    if (missionBlocksTravel)           return FastTravelGate::Mission;
    return FastTravelGate::Ok;
}

const char* fastTravelGateText(FastTravelGate g) {
    switch (g) {
        case FastTravelGate::Ok:           return "READY";
        case FastTravelGate::NotAnAnchor:  return "NO TRANSIT ANCHOR HERE";
        case FastTravelGate::Undiscovered: return "LOCATION NOT DISCOVERED";
        case FastTravelGate::Alert:        return "CANNOT TRAVEL WHILE HUNTED";
        case FastTravelGate::Mission:      return "MISSION IN PROGRESS - TRAVEL LOCKED";
    }
    return "";
}

// ===========================================================================
// Tile bake — CPU top-down rasterizer.
// ===========================================================================
namespace {

struct PixCanvas {
    uint8_t* px; uint32_t res;
    float wx0, wz0, invW, invH;   // world rect -> [0,res) mapping
    int xToPx(float wx) const { return (int)((wx - wx0) * invW); }
    int zToPy(float wz) const { return (int)((wz - wz0) * invH); }
    void put(int x, int y, const float c[4]) {
        if (x < 0 || y < 0 || x >= (int)res || y >= (int)res) return;
        uint8_t* d = px + ((size_t)y * res + x) * 4;
        const float a = std::clamp(c[3], 0.0f, 1.0f);
        const float ia = 1.0f - a;
        d[0] = (uint8_t)std::clamp(c[0] * 255.0f * a + d[0] * ia, 0.0f, 255.0f);
        d[1] = (uint8_t)std::clamp(c[1] * 255.0f * a + d[1] * ia, 0.0f, 255.0f);
        d[2] = (uint8_t)std::clamp(c[2] * 255.0f * a + d[2] * ia, 0.0f, 255.0f);
        d[3] = (uint8_t)std::clamp(a * 255.0f + d[3] * ia, 0.0f, 255.0f);
    }
    // Filled world-rect (alpha-blended onto the canvas).
    void fillWorld(float x0, float z0, float x1, float z1, const float c[4]) {
        int px0 = xToPx(std::min(x0, x1)), px1 = xToPx(std::max(x0, x1));
        int py0 = zToPy(std::min(z0, z1)), py1 = zToPy(std::max(z0, z1));
        if (px1 == px0) px1 = px0 + 1;   // never vanish below one pixel
        if (py1 == py0) py1 = py0 + 1;
        for (int y = py0; y < py1; ++y)
            for (int x = px0; x < px1; ++x) put(x, y, c);
    }
    // World-rect OUTLINE, `pxW` pixels thick (drawn inward).
    void rectWorld(float x0, float z0, float x1, float z1, int pxW, const float c[4]) {
        int px0 = xToPx(std::min(x0, x1)), px1 = xToPx(std::max(x0, x1));
        int py0 = zToPy(std::min(z0, z1)), py1 = zToPy(std::max(z0, z1));
        for (int t = 0; t < pxW; ++t) {
            for (int x = px0; x <= px1; ++x) { put(x, py0 + t, c); put(x, py1 - t, c); }
            for (int y = py0; y <= py1; ++y) { put(px0 + t, y, c); put(px1 - t, y, c); }
        }
    }
};

} // namespace

void bakeFloorTilePixels(const CanonFloor& floor, std::vector<uint8_t>& outRgba,
                         uint32_t res, float wx0, float wz0, float wx1, float wz1) {
    outRgba.assign((size_t)res * res * 4, 0);   // transparent background
    if (!floor.valid() || wx1 <= wx0 || wz1 <= wz0) return;
    PixCanvas cv{ outRgba.data(), res, wx0, wz0,
                  res / (wx1 - wx0), res / (wz1 - wz0) };

    // Median room Y of the floor: rooms deeper below it band darker (the
    // F1 cave system / hidden sub-level read as "deeper" at a glance).
    std::vector<float> ys; ys.reserve(floor.rooms.size());
    for (const CanonRoom& r : floor.rooms) ys.push_back(r.cy);
    std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
    const float medY = ys.empty() ? 0.0f : ys[ys.size() / 2];

    // Pass 1: room floor fills (depth-banded blueprint teal).
    for (const CanonRoom& r : floor.rooms) {
        const float depth = std::clamp((medY - r.cy) / 40.0f, 0.0f, 1.0f);
        float fill[4] = {
            0.055f + 0.035f * (1.0f - depth),
            0.150f - 0.085f * depth,
            0.195f - 0.090f * depth,
            0.92f,
        };
        // Secret-ish rooms tint faintly violet so they read as "off the books".
        if (r.name.find("Hidden") != std::string::npos ||
            r.name.find("Cave")   != std::string::npos) {
            fill[0] += 0.05f; fill[2] += 0.06f;
        }
        cv.fillWorld(r.x0(), r.z0(), r.x1(), r.z1(), fill);
    }
    // Pass 2: wall perimeters (bright cyan, 2 px).
    const float wall[4] = { 0.34f, 0.84f, 1.00f, 1.0f };
    for (const CanonRoom& r : floor.rooms)
        cv.rectWorld(r.x0(), r.z0(), r.x1(), r.z1(), 2, wall);
    // Pass 3: doorway openings cut THROUGH the shared walls (floor-toned gap).
    const float gap[4] = { 0.10f, 0.22f, 0.27f, 1.0f };
    for (const CanonDoorway& d : floor.doorways) {
        const float half = 0.9f;   // ~1.8 m opening
        if (d.axis == 0) cv.fillWorld(d.cx - 0.45f, d.cz - half, d.cx + 0.45f, d.cz + half, gap);
        else             cv.fillWorld(d.cx - half, d.cz - 0.45f, d.cx + half, d.cz + 0.45f, gap);
    }
}

uint32_t bakeEntityTilePixels(const Scene& scene, x3::rhi::IRenderDevice& device,
                              const std::vector<uint32_t>& entities,
                              std::vector<uint8_t>& outRgba, uint32_t res,
                              float wx0, float wz0, float wx1, float wz1,
                              float yMin, float yMax) {
    outRgba.assign((size_t)res * res * 4, 0);
    if (entities.empty() || wx1 <= wx0 || wz1 <= wz0) return 0;
    PixCanvas cv{ outRgba.data(), res, wx0, wz0,
                  res / (wx1 - wx0), res / (wz1 - wz0) };

    struct Item { float x0, z0, x1, z1, topY; bool ground; };
    std::vector<Item> items; items.reserve(entities.size());
    const float tileArea = (wx1 - wx0) * (wz1 - wz0);

    for (uint32_t id : entities) {
        if (id >= scene.size()) continue;
        const Entity& e = scene.get(id);
        if (!e.mesh.valid() || !e.visible) continue;
        float bmin[3], bmax[3];
        if (!device.meshBounds(e.mesh, bmin, bmax)) continue;
        // World AABB = |M3x3| * localExtent around M * localCenter (column-major).
        const float lc[3] = { (bmin[0]+bmax[0])*0.5f, (bmin[1]+bmax[1])*0.5f, (bmin[2]+bmax[2])*0.5f };
        const float le[3] = { (bmax[0]-bmin[0])*0.5f, (bmax[1]-bmin[1])*0.5f, (bmax[2]-bmin[2])*0.5f };
        const float* m = e.transform;
        float wc[3], we[3];
        for (int a = 0; a < 3; ++a) {
            wc[a] = m[12 + a] + m[0 + a] * lc[0] + m[4 + a] * lc[1] + m[8 + a] * lc[2];
            we[a] = std::fabs(m[0 + a]) * le[0] + std::fabs(m[4 + a]) * le[1] + std::fabs(m[8 + a]) * le[2];
        }
        const float top = wc[1] + we[1], bot = wc[1] - we[1];
        if (top < yMin || bot > yMax) continue;     // outside the y slice
        Item it{ wc[0] - we[0], wc[2] - we[2], wc[0] + we[0], wc[2] + we[2], top, false };
        const float area = (it.x1 - it.x0) * (it.z1 - it.z0);
        if (area > tileArea * 0.5f) it.ground = true;   // ground slab / sky shell
        items.push_back(it);
    }
    // Painter's order: ground first, then by top Y ascending (tall = on top).
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.ground != b.ground) return a.ground;
        return a.topY < b.topY;
    });
    const float bandLo = yMin, bandH = std::max(8.0f, yMax - yMin);
    uint32_t painted = 0;
    for (const Item& it : items) {
        if (it.ground) {
            const float g[4] = { 0.045f, 0.075f, 0.095f, 0.85f };
            cv.fillWorld(std::max(it.x0, wx0), std::max(it.z0, wz0),
                         std::min(it.x1, wx1), std::min(it.z1, wz1), g);
            ++painted;
            continue;
        }
        const float t = std::clamp((it.topY - bandLo) / bandH, 0.0f, 1.0f);
        const float fill[4] = {
            0.06f + (0.32f - 0.06f) * t,
            0.13f + (0.80f - 0.13f) * t,
            0.16f + (0.95f - 0.16f) * t,
            0.95f,
        };
        const float rim[4] = { fill[0] * 0.4f, fill[1] * 0.4f, fill[2] * 0.4f, 0.9f };
        cv.fillWorld(it.x0, it.z0, it.x1, it.z1, fill);
        cv.rectWorld(it.x0, it.z0, it.x1, it.z1, 1, rim);   // cheap-AO rim
        ++painted;
    }
    return painted;
}

// ===========================================================================
// WorldMapSystem.
// ===========================================================================
bool WorldMapSystem::init(const std::string& poisPath, const std::string& spireLevelDocPath) {
    std::vector<std::string> errs;
    if (!m_pois.load(poisPath, errs))
        for (const std::string& e : errs) x3::logWarn("[worldmap] " + e);
    m_spireDocPath = spireLevelDocPath;
    m_floors.clear();
    if (!m_spireDocPath.empty()) {
        float rx0 = 1e9f, rz0 = 1e9f, rx1 = -1e9f, rz1 = -1e9f;
        for (int f = 1; f <= 16; ++f) {
            CanonFloor cf = loadCanonFloor(m_spireDocPath, f);
            if (!cf.valid()) { if (f == 1) break; else continue; }
            SpireFloor sf;
            sf.num = f;
            // Median room Y — the floor-slice selector key.
            std::vector<float> ys; ys.reserve(cf.rooms.size());
            for (const CanonRoom& r : cf.rooms) ys.push_back(r.cy);
            std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
            sf.medianY = ys[ys.size() / 2];
            for (const CanonRoom& r : cf.rooms) {
                rx0 = std::min(rx0, r.x0()); rx1 = std::max(rx1, r.x1());
                rz0 = std::min(rz0, r.z0()); rz1 = std::max(rz1, r.z1());
            }
            sf.floor = std::move(cf);
            m_floors.push_back(std::move(sf));
        }
        if (!m_floors.empty()) {
            // One shared (square-ish, padded) rect for ALL floors so the floor
            // selector swaps slices without the view jumping.
            const float pad = 4.0f;
            m_spireRect[0] = rx0 - pad; m_spireRect[1] = rz0 - pad;
            m_spireRect[2] = rx1 + pad; m_spireRect[3] = rz1 + pad;
            m_selFloor = m_floors.front().num;
            x3::logInfo("[worldmap] " + std::to_string(m_floors.size()) +
                        " Spire floors parsed from `" + m_spireDocPath + "`");
        }
    }
    x3::logInfo("[worldmap] " + std::to_string(m_pois.pois.size()) + " POIs from `" +
                poisPath + "`");
    return !m_pois.empty() || !m_floors.empty();
}

void WorldMapSystem::shutdown(x3::rhi::IRenderDevice& device) {
    for (SpireFloor& sf : m_floors)
        if (sf.tile.baked && sf.tile.tex.valid()) { device.destroyTexture(sf.tile.tex); sf.tile = MapTile{}; }
    for (RegionTileEntry& rt : m_regionTiles)
        if (rt.tile.baked && rt.tile.tex.valid()) { device.destroyTexture(rt.tile.tex); rt.tile = MapTile{}; }
    m_regionTiles.clear();
}

void WorldMapSystem::discoveryTick(StoryFlags& flags, float px, float py, float pz) {
    for (const MapPoi& p : m_pois.pois) {
        const std::string flag = poiFoundFlag(p.id);
        if (flags.has(flag)) continue;
        const float dx = px - p.x, dz = pz - p.z;
        if (dx * dx + dz * dz > p.discoverRadius * p.discoverRadius) continue;
        if (std::fabs(py - p.y) > 6.0f) continue;
        flags.set(flag);
        if (!p.region.empty()) flags.set(regionSeenFlag(p.region));
        x3::logInfo("[worldmap] DISCOVERED `" + p.name + "` (" + flag + ")");
    }
}

void WorldMapSystem::setWaypoint(float x, float z, int floor) {
    m_waypoint.active = true; m_waypoint.x = x; m_waypoint.z = z; m_waypoint.floor = floor;
}

const MapPoi* WorldMapSystem::travelTarget() const {
    if (m_travelPoi < 0 || m_travelPoi >= (int)m_pois.pois.size()) return nullptr;
    return &m_pois.pois[m_travelPoi];
}

FastTravelGate WorldMapSystem::travelGate(int poiIndex, const StoryFlags& flags,
                                          bool missionBlocksTravel) const {
    if (poiIndex < 0 || poiIndex >= (int)m_pois.pois.size())
        return FastTravelGate::NotAnAnchor;
    return fastTravelGate(m_pois.pois[poiIndex], flags, missionBlocksTravel);
}

int WorldMapSystem::spireFloorIndex(int floorNum) const {
    for (size_t i = 0; i < m_floors.size(); ++i)
        if (m_floors[i].num == floorNum) return (int)i;
    return -1;
}

void WorldMapSystem::selectFloor(int floorNum) {
    if (spireFloorIndex(floorNum) >= 0) m_selFloor = floorNum;
}

int WorldMapSystem::floorForY(float y) const {
    int best = 0; float bestD = 1e30f;
    for (const SpireFloor& sf : m_floors) {
        const float d = std::fabs(y - sf.medianY);
        if (d < bestD) { bestD = d; best = sf.num; }
    }
    return best;
}

const MapTile* WorldMapSystem::ensureSpireTile(x3::rhi::IRenderDevice& device, int floorNum) {
    const int i = spireFloorIndex(floorNum);
    if (i < 0) return nullptr;
    SpireFloor& sf = m_floors[i];
    if (sf.tile.baked) return &sf.tile;
    const uint32_t res = 1024;
    std::vector<uint8_t> px;
    bakeFloorTilePixels(sf.floor, px, res,
                        m_spireRect[0], m_spireRect[1], m_spireRect[2], m_spireRect[3]);
    sf.tile.tex = device.createTexture(px.data(), res, res, /*srgb=*/false);
    sf.tile.wx0 = m_spireRect[0]; sf.tile.wz0 = m_spireRect[1];
    sf.tile.wx1 = m_spireRect[2]; sf.tile.wz1 = m_spireRect[3];
    sf.tile.res = res;
    sf.tile.baked = sf.tile.tex.valid();
    return sf.tile.baked ? &sf.tile : nullptr;
}

const MapTile* WorldMapSystem::ensureRegionTile(x3::rhi::IRenderDevice& device, const Scene& scene,
                                                const std::string& regionId,
                                                const std::vector<uint32_t>& entities,
                                                float wx0, float wz0, float wx1, float wz1,
                                                float yMin, float yMax) {
    RegionTileEntry* entry = nullptr;
    for (RegionTileEntry& rt : m_regionTiles)
        if (rt.id == regionId) { entry = &rt; break; }
    if (entry && entry->tile.baked) return &entry->tile;
    if (entities.empty()) return nullptr;
    if (!entry) { m_regionTiles.push_back(RegionTileEntry{ regionId, MapTile{} }); entry = &m_regionTiles.back(); }
    const uint32_t res = 1024;
    std::vector<uint8_t> px;
    const uint32_t painted =
        bakeEntityTilePixels(scene, device, entities, px, res, wx0, wz0, wx1, wz1, yMin, yMax);
    if (painted == 0) return nullptr;   // nothing readable yet — retry next open
    entry->tile.tex = device.createTexture(px.data(), res, res, /*srgb=*/false);
    entry->tile.wx0 = wx0; entry->tile.wz0 = wz0; entry->tile.wx1 = wx1; entry->tile.wz1 = wz1;
    entry->tile.res = res;
    entry->tile.baked = entry->tile.tex.valid();
    return entry->tile.baked ? &entry->tile : nullptr;
}

const MapTile* WorldMapSystem::regionTile(const std::string& regionId) const {
    for (const RegionTileEntry& rt : m_regionTiles)
        if (rt.id == regionId && rt.tile.baked) return &rt.tile;
    return nullptr;
}

void WorldMapSystem::invalidateSpireTiles(x3::rhi::IRenderDevice& device) {
    for (SpireFloor& sf : m_floors)
        if (sf.tile.baked && sf.tile.tex.valid()) { device.destroyTexture(sf.tile.tex); sf.tile = MapTile{}; }
    // Re-parse the doc on the next ensure (hot-reload: the map IS the world).
    if (!m_spireDocPath.empty()) {
        const std::string doc = m_spireDocPath;
        const MapPoiTable saved = m_pois;
        m_floors.clear();
        init(worldMapPoisJsonPath(), doc);
        if (m_pois.empty()) m_pois = saved;
    }
}

void WorldMapSystem::invalidateRegionTile(x3::rhi::IRenderDevice& device, const std::string& regionId) {
    for (RegionTileEntry& rt : m_regionTiles)
        if (rt.id == regionId && rt.tile.baked) {
            if (rt.tile.tex.valid()) device.destroyTexture(rt.tile.tex);
            rt.tile = MapTile{};
        }
}

void WorldMapSystem::open(float playerX, float playerY, float playerZ, float vpW, float vpH) {
    m_open = true;
    m_confirmPoi = -1;
    m_cam.setViewport(vpW, vpH);
    // Open at "region" zoom centered on the player; auto-select the player's floor.
    m_cam.jumpTo(playerX, playerZ, 6.0f);
    const int f = floorForY(playerY);
    if (f > 0) m_selFloor = f;
    m_dragging = false; m_dragMoved = 0.0f;
}

// ---------------------------------------------------------------------------
// The map screen.
// ---------------------------------------------------------------------------
void WorldMapSystem::drawPoiIcon(x3::ui::UiContext& ui, const MapPoi& poi, float px, float py,
                                 bool hovered, float t) const {
    // Icon: a small box + a type glyph. Colors per class.
    float c[4] = { 0.55f, 0.85f, 1.00f, 0.95f };
    const char* glyph = "*";
    if      (poi.type == "cell")     { glyph = "C"; }
    else if (poi.type == "hall")     { glyph = "H"; }
    else if (poi.type == "security") { glyph = "S"; c[0]=1.0f; c[1]=0.55f; c[2]=0.35f; }
    else if (poi.type == "armory")   { glyph = "A"; c[0]=1.0f; c[1]=0.80f; c[2]=0.30f; }
    else if (poi.type == "secret")   { glyph = "?"; c[0]=0.80f; c[1]=0.55f; c[2]=1.00f; }
    else if (poi.type == "boss")     { glyph = "!"; c[0]=1.0f; c[1]=0.30f; c[2]=0.30f; }
    else if (poi.type == "elevator") { glyph = "E"; c[0]=0.45f; c[1]=1.0f; c[2]=0.65f; }
    else if (poi.type == "door")     { glyph = "D"; c[0]=0.70f; c[1]=0.78f; c[2]=0.88f; }
    else if (poi.type == "club")     { glyph = "J"; c[0]=1.0f; c[1]=0.40f; c[2]=0.85f; }
    else if (poi.type == "city")     { glyph = "T"; c[0]=0.95f; c[1]=0.85f; c[2]=0.45f; }
    else if (poi.type == "base")     { glyph = "B"; c[0]=0.40f; c[1]=0.85f; c[2]=0.95f; }
    else if (poi.type == "landmark") { glyph = "L"; c[0]=0.75f; c[1]=0.85f; c[2]=0.80f; }
    const float s = hovered ? 14.0f : 11.0f;
    const float bg[4] = { 0.02f, 0.05f, 0.08f, hovered ? 0.95f : 0.8f };
    ui.quad(px - s * 0.5f, py - s * 0.5f, s, s, bg);
    const float rim[4] = { c[0], c[1], c[2], hovered ? 1.0f : 0.85f };
    ui.quad(px - s * 0.5f, py - s * 0.5f, s, 1.5f, rim);
    ui.quad(px - s * 0.5f, py + s * 0.5f - 1.5f, s, 1.5f, rim);
    ui.quad(px - s * 0.5f, py - s * 0.5f, 1.5f, s, rim);
    ui.quad(px + s * 0.5f - 1.5f, py - s * 0.5f, 1.5f, s, rim);
    ui.textCentered(glyph, px, py - s * 0.36f, s * 0.75f, rim, x3::ui::UiContext::FontRole::HudMono);
    if (hovered) {
        const float lbl[4] = { 0.92f, 0.97f, 1.0f, 0.95f };
        ui.text(poi.name.c_str(), px + s, py - 7.0f, 14.0f, lbl);
    }
    (void)t;
}

void WorldMapSystem::drawScreen(x3::ui::UiContext& ui, x3::rhi::IRenderDevice& device,
                                const x3::rhi::FrameContext& frame, const ScreenInput& in,
                                StoryFlags& flags, float dt) {
    if (!m_open) return;
    m_pulse += dt;
    const float W = (float)ui.screenW() > 0 ? (float)ui.screenW() : m_cam.vw;
    const float H = (float)ui.screenH() > 0 ? (float)ui.screenH() : m_cam.vh;
    m_cam.setViewport(W, H);

    // -------------------- input --------------------
    const bool modal = (m_confirmPoi >= 0);
    if (!modal) {
        if (in.wheel != 0.0f) m_cam.zoomAt(in.mouseX, in.mouseY, in.wheel);
        // WASD pan: constant SCREEN speed (700 px/s) -> world meters by scale.
        const float panPx = 700.0f * dt / m_cam.scale;
        if (in.keyW) m_cam.panWorld(0.0f, -panPx);
        if (in.keyS) m_cam.panWorld(0.0f,  panPx);
        if (in.keyA) m_cam.panWorld(-panPx, 0.0f);
        if (in.keyD) m_cam.panWorld( panPx, 0.0f);
        // Drag pan vs click (click = press+release with < 5 px of travel).
        if (in.mouseDown) {
            if (!m_dragging) { m_dragging = true; m_dragMoved = 0.0f; }
            else {
                const float dx = in.mouseX - m_dragLastX, dy = in.mouseY - m_dragLastY;
                m_dragMoved += std::fabs(dx) + std::fabs(dy);
                if (m_dragMoved > 5.0f) m_cam.panPixels(dx, dy);
            }
            m_dragLastX = in.mouseX; m_dragLastY = in.mouseY;
        } else if (m_dragging) {
            m_dragging = false;
            if (m_dragMoved <= 5.0f) {
                // CLICK: POI first (discovered only), else waypoint toggle.
                int hit = -1;
                for (size_t i = 0; i < m_pois.pois.size(); ++i) {
                    const MapPoi& p = m_pois.pois[i];
                    if (!poiDiscovered(flags, p)) continue;
                    float px, py; m_cam.worldToPx(p.x, p.z, px, py);
                    if (std::fabs(px - in.mouseX) <= 12.0f && std::fabs(py - in.mouseY) <= 12.0f) {
                        hit = (int)i; break;
                    }
                }
                if (hit >= 0) {
                    m_confirmPoi = hit;
                } else {
                    float wx, wz; m_cam.pxToWorld(in.mouseX, in.mouseY, wx, wz);
                    if (m_waypoint.active) {
                        float wpx, wpy; m_cam.worldToPx(m_waypoint.x, m_waypoint.z, wpx, wpy);
                        const bool nearWp = std::fabs(wpx - in.mouseX) <= 12.0f &&
                                            std::fabs(wpy - in.mouseY) <= 12.0f;
                        if (nearWp) clearWaypoint();
                        else setWaypoint(wx, wz, m_selFloor);
                    } else {
                        setWaypoint(wx, wz, m_selFloor);
                    }
                }
            }
        }
        // ENTER: waypoint at the cursor.
        if (in.enterEdge) {
            float wx, wz; m_cam.pxToWorld(in.mouseX, in.mouseY, wx, wz);
            setWaypoint(wx, wz, m_selFloor);
        }
    }
    m_cam.update(dt);

    // -------------------- draw --------------------
    const float bg[4] = { 0.014f, 0.025f, 0.045f, 0.97f };
    ui.quad(0, 0, W, H, bg);

    // Subtle world grid: step picked so lines land 60..360 px apart.
    {
        const float steps[] = { 1000.0f, 500.0f, 100.0f, 50.0f, 10.0f, 5.0f };
        float step = steps[0];
        for (float s : steps) if (s * m_cam.scale >= 60.0f) step = s;
        const float gc[4] = { 0.30f, 0.55f, 0.70f, 0.07f };
        float wxa, wza, wxb, wzb;
        m_cam.pxToWorld(0, 0, wxa, wza);
        m_cam.pxToWorld(W, H, wxb, wzb);
        int guard = 0;
        for (float gx = std::floor(wxa / step) * step; gx <= wxb && guard < 200; gx += step, ++guard) {
            float px, py; m_cam.worldToPx(gx, wza, px, py);
            ui.quad(px, 0, 1, H, gc);
        }
        guard = 0;
        for (float gz = std::floor(wza / step) * step; gz <= wzb && guard < 200; gz += step, ++guard) {
            float px, py; m_cam.worldToPx(wxa, gz, px, py);
            ui.quad(0, py, W, 1, gc);
        }
    }

    // ---- Tiles: regions (fogged when unseen), then the Spire's selected floor.
    auto drawTile = [&](const MapTile& t, bool seen) {
        float px0, py0, px1, py1;
        m_cam.worldToPx(t.wx0, t.wz0, px0, py0);
        m_cam.worldToPx(t.wx1, t.wz1, px1, py1);
        if (px1 < -64 || py1 < -64 || px0 > W + 64 || py0 > H + 64) return;
        const float lit[4]  = { 1.0f, 1.0f, 1.0f, 0.96f };
        const float fog[4]  = { 0.42f, 0.47f, 0.54f, 0.22f };
        device.drawHudImage(frame, t.tex, px0, py0, px1 - px0, py1 - py0,
                            seen ? lit : fog);
    };
    for (const RegionTileEntry& rt : m_regionTiles)
        if (rt.tile.baked) drawTile(rt.tile, flags.has(regionSeenFlag(rt.id)));
    if (const MapTile* st = ensureSpireTile(device, m_selFloor))
        drawTile(*st, flags.has(regionSeenFlag("spire_f1")));

    // ---- Live objective marker (gold, pulsing).
    if (in.objValid) {
        float px, py; m_cam.worldToPx(in.objX, in.objZ, px, py);
        const float a = 0.6f + 0.4f * std::sin(m_pulse * 5.0f);
        const float gold[4] = { 1.0f, 0.82f, 0.25f, a };
        const float s = 9.0f;
        ui.quad(px - s * 0.5f, py - s * 0.5f, s, s, gold);
        ui.textCentered("OBJ", px, py + s * 0.7f, 12.0f, gold, x3::ui::UiContext::FontRole::HudMono);
    }

    // ---- POIs (discovered only; off-floor Spire POIs ghost at low alpha).
    int hover = -1;
    for (size_t i = 0; i < m_pois.pois.size(); ++i) {
        const MapPoi& p = m_pois.pois[i];
        if (!poiDiscovered(flags, p)) continue;
        float px, py; m_cam.worldToPx(p.x, p.z, px, py);
        if (px < -40 || py < -40 || px > W + 40 || py > H + 40) continue;
        const bool hov = !modal && std::fabs(px - in.mouseX) <= 12.0f &&
                         std::fabs(py - in.mouseY) <= 12.0f;
        if (hov) hover = (int)i;
        if (p.floor > 0 && p.floor != m_selFloor) {
            const float ghost[4] = { 0.5f, 0.6f, 0.7f, 0.3f };
            ui.quad(px - 3, py - 3, 6, 6, ghost);
            continue;
        }
        drawPoiIcon(ui, p, px, py, hov, m_pulse);
        // Zoomed out, the MAJOR anchors keep their names visible (the gazetteer
        // read of the world overview); minor POIs label on hover only.
        if (!hov && m_cam.scale < 1.0f &&
            (p.type == "city" || p.type == "base" || p.type == "landmark" ||
             p.type == "club")) {
            const float lbl[4] = { 0.62f, 0.80f, 0.92f, 0.85f };
            ui.textCentered(p.name.c_str(), px, py + 9.0f, 13.0f, lbl);
        }
    }

    // ---- Companions (green dots).
    for (int i = 0; i < in.compCount && in.compX && in.compZ; ++i) {
        float px, py; m_cam.worldToPx(in.compX[i], in.compZ[i], px, py);
        const float g[4] = { 0.35f, 1.0f, 0.55f, 0.7f + 0.3f * std::sin(m_pulse * 4.0f) };
        ui.quad(px - 3, py - 3, 6, 6, g);
    }

    // ---- Waypoint (magenta cross + ring).
    if (m_waypoint.active) {
        float px, py; m_cam.worldToPx(m_waypoint.x, m_waypoint.z, px, py);
        const float mag[4] = { 1.0f, 0.35f, 0.95f, 0.95f };
        ui.quad(px - 8, py - 1.5f, 16, 3, mag);
        ui.quad(px - 1.5f, py - 8, 3, 16, mag);
        const float ring[4] = { 1.0f, 0.35f, 0.95f, 0.35f + 0.2f * std::sin(m_pulse * 4.0f) };
        const float rs = 14.0f;
        ui.quad(px - rs, py - rs, rs * 2, 1.5f, ring);
        ui.quad(px - rs, py + rs, rs * 2, 1.5f, ring);
        ui.quad(px - rs, py - rs, 1.5f, rs * 2, ring);
        ui.quad(px + rs, py - rs, 1.5f, rs * 2 + 1.5f, ring);
    }

    // ---- Player arrow: dot + heading line + view cone (holo line style).
    {
        float px, py; m_cam.worldToPx(in.playerX, in.playerZ, px, py);
        const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        const float cone[4]  = { 0.6f, 0.9f, 1.0f, 0.22f };
        const float hx = std::cos(in.playerYaw), hz = std::sin(in.playerYaw);
        // View cone: two faint rays at +-0.42 rad, stepped as small quads.
        for (int side = -1; side <= 1; side += 2) {
            const float a = in.playerYaw + 0.42f * (float)side;
            const float dx = std::cos(a), dz = std::sin(a);
            for (int s = 2; s < 16; ++s)
                ui.quad(px + dx * s * 2.4f - 1, py + dz * s * 2.4f - 1, 2, 2, cone);
        }
        // Heading line (bright).
        for (int s = 1; s < 11; ++s)
            ui.quad(px + hx * s * 2.4f - 1, py + hz * s * 2.4f - 1, 2, 2, white);
        ui.quad(px - 4, py - 4, 8, 8, white);
        const float dotRim[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
        ui.quad(px - 2, py - 2, 4, 4, dotRim);
    }

    // ---- Floor selector (Spire) — left edge.
    if (!m_floors.empty()) {
        const float bx = 18.0f, bw = 52.0f, bh = 30.0f;
        float by = H * 0.5f - (m_floors.size() * (bh + 6.0f)) * 0.5f;
        const float hdr[4] = { 0.55f, 0.8f, 0.95f, 0.8f };
        ui.label("SPIRE FLOOR", bx, by - 22.0f, 13.0f, hdr);
        // Draw top floor first (tower order: F7 at the top of the column).
        for (int i = (int)m_floors.size() - 1; i >= 0; --i) {
            const SpireFloor& sf = m_floors[i];
            char lbl[8]; std::snprintf(lbl, sizeof(lbl), "F%d", sf.num);
            if (sf.num == m_selFloor) {
                const float sel[4] = { 0.20f, 0.55f, 0.75f, 0.9f };
                ui.quad(bx - 3, by - 3, bw + 6, bh + 6, sel);
            }
            if (!modal && ui.button(lbl, bx, by, bw, bh)) m_selFloor = sf.num;
            else if (modal) { const float d[4] = {0.1f,0.16f,0.22f,0.8f}; ui.quad(bx, by, bw, bh, d);
                              const float tc[4] = {0.7f,0.8f,0.9f,0.9f};
                              ui.textCentered(lbl, bx + bw * 0.5f, by + 7.0f, 15.0f, tc); }
            by += bh + 6.0f;
        }
    }

    // ---- Header + legend.
    {
        const float hdrBg[4] = { 0.02f, 0.05f, 0.09f, 0.85f };
        ui.quad(0, 0, W, 42, hdrBg);
        const float tc[4] = { 0.80f, 0.95f, 1.0f, 1.0f };
        ui.text("WORLD MAP", 18, 9, 22, tc, x3::ui::UiContext::FontRole::Title);
        if (in.locationName && in.locationName[0]) {
            const float lc[4] = { 0.55f, 0.75f, 0.9f, 0.95f };
            ui.text(in.locationName, 200, 13, 16, lc);
        }
        float cwx, cwz; m_cam.pxToWorld(in.mouseX, in.mouseY, cwx, cwz);
        char coords[64];
        std::snprintf(coords, sizeof(coords), "%.0f, %.0f   x%.2f", cwx, cwz, m_cam.scale);
        const float cc[4] = { 0.45f, 0.6f, 0.7f, 0.9f };
        ui.text(coords, W - 230.0f, 13, 14, cc, x3::ui::UiContext::FontRole::HudMono);

        const float legBg[4] = { 0.02f, 0.05f, 0.09f, 0.85f };
        ui.quad(0, H - 34, W, 34, legBg);
        const float lg[4] = { 0.55f, 0.7f, 0.8f, 0.9f };
        ui.text("[#] YOU   [+] WAYPOINT   [boxed letter] POI (CLICK = TRAVEL)   "
                "CLICK MAP = WAYPOINT   DRAG/WASD = PAN   WHEEL = ZOOM   M = CLOSE",
                18, H - 26, 13, lg);
    }

    // ---- Fast-travel confirm prompt (modal).
    if (m_confirmPoi >= 0 && m_confirmPoi < (int)m_pois.pois.size()) {
        const MapPoi& p = m_pois.pois[m_confirmPoi];
        const FastTravelGate gate = fastTravelGate(p, flags, in.missionBlocksTravel);
        const float pw = 460.0f, ph = 150.0f;
        const float px0 = W * 0.5f - pw * 0.5f, py0 = H * 0.5f - ph * 0.5f;
        const float pbg[4] = { 0.03f, 0.07f, 0.12f, 0.97f };
        ui.panel(px0, py0, pw, ph, pbg);
        const float tc[4] = { 0.85f, 0.96f, 1.0f, 1.0f };
        ui.textCentered(p.name.c_str(), W * 0.5f, py0 + 18, 20, tc, x3::ui::UiContext::FontRole::Title);
        if (gate == FastTravelGate::Ok) {
            const float qc[4] = { 0.6f, 0.8f, 0.9f, 0.95f };
            ui.textCentered("FAST TRAVEL TO THIS LOCATION?", W * 0.5f, py0 + 52, 15, qc);
            const bool yes = ui.button("TRAVEL  [ENTER]", px0 + 40, py0 + 88, 180, 38);
            const bool no  = ui.button("CANCEL  [ESC]",   px0 + pw - 220, py0 + 88, 180, 38);
            if (yes || in.enterEdge) {
                m_travelPoi = m_confirmPoi;
                m_travelRequested = true;
                m_confirmPoi = -1;
            } else if (no || in.escEdge) {
                m_confirmPoi = -1;
            }
        } else {
            const float warn[4] = { 1.0f, 0.5f, 0.35f, 0.95f };
            ui.textCentered(fastTravelGateText(gate), W * 0.5f, py0 + 56, 15, warn);
            const bool ok = ui.button("CLOSE  [ESC]", W * 0.5f - 90, py0 + 92, 180, 38);
            if (ok || in.escEdge || in.enterEdge) m_confirmPoi = -1;
        }
    }
    (void)hover;
}

// ===========================================================================
// Headless self-test (--test-worldmap).
// ===========================================================================
namespace {

bool nearly(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// A tiny synthetic two-room floor (room A | doorway | room B) for the bake test.
CanonFloor makeTestFloor() {
    CanonFloor f;
    f.floorNum = 1; f.name = "test";
    CanonRoom a; a.name = "A"; a.type = "Cell";
    a.cx = -3.0f; a.cy = 0.0f; a.cz = 0.0f; a.w = 6.0f; a.h = 4.0f; a.d = 6.0f;
    CanonRoom b; b.name = "B"; b.type = "Hallway";
    b.cx =  3.0f; b.cy = 0.0f; b.cz = 0.0f; b.w = 6.0f; b.h = 4.0f; b.d = 6.0f;
    f.rooms.push_back(a); f.rooms.push_back(b);
    CanonDoorway d; d.a = 0; d.b = 1; d.kind = DoorwayKind::AdjacentX;
    d.cx = 0.0f; d.cy = 0.0f; d.cz = 0.0f; d.axis = 0;
    f.doorways.push_back(d);
    return f;
}

} // namespace

bool runWorldMapSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* tag, const std::string& detail = "") {
        ++total;
        if (ok) { ++pass; x3::logInfo(std::string("  PASS ") + tag + (detail.empty() ? "" : "  (" + detail + ")")); }
        else      x3::logWarn(std::string("  FAIL ") + tag + (detail.empty() ? "" : "  (" + detail + ")"));
    };

    // ---- M1: POI table loads with the canonical entries. -------------------
    MapPoiTable pois;
    {
        std::vector<std::string> errs;
        const bool ok = pois.load(worldMapPoisJsonPath(), errs);
        for (const std::string& e : errs) x3::logWarn("[worldmap-test] " + e);
        check(ok && pois.pois.size() >= 12, "M1 poi table loads",
              std::to_string(pois.pois.size()) + " POIs");
        check(pois.indexOf("jakes_cell") >= 0 && pois.indexOf("armory") >= 0 &&
              pois.indexOf("elevator_f1") >= 0 && pois.indexOf("club_1127") >= 0 &&
              pois.indexOf("city") >= 0 && pois.indexOf("ocean_base") >= 0,
              "M1b canonical POI ids present");
    }

    // ---- M2: proximity discovery + StoryFlags persistence round-trip. ------
    {
        StoryFlags flags;
        const int ci = pois.indexOf("jakes_cell");
        const MapPoi& cell = pois.pois[ci];
        WorldMapSystem wm;
        wm.init(worldMapPoisJsonPath(), "");
        wm.discoveryTick(flags, cell.x + 500.0f, cell.y, cell.z);   // far: no discover
        check(!poiDiscovered(flags, cell), "M2 far tick does NOT discover");
        wm.discoveryTick(flags, cell.x + 1.0f, cell.y + 0.5f, cell.z - 1.0f);
        check(poiDiscovered(flags, cell), "M2b proximity discovers (poi.<id>.found)");
        check(flags.has(regionSeenFlag(cell.region)), "M2c owning region marked seen");
        // Round-trip through the serialized text (the persistence lane).
        StoryFlags fresh;
        fresh.deserialize(flags.serialize());
        check(poiDiscovered(fresh, cell), "M2d discovery survives serialize/deserialize");
        // Y gate: 500 m above the cell does NOT discover it.
        StoryFlags above;
        wm.discoveryTick(above, cell.x, cell.y + 500.0f, cell.z);
        check(!poiDiscovered(above, cell), "M2e Y-gate (no discovery from 500 m above)");
    }

    // ---- M3: waypoint set/clear. -------------------------------------------
    {
        WorldMapSystem wm;
        wm.init(worldMapPoisJsonPath(), "");
        check(!wm.waypoint().active, "M3 waypoint starts clear");
        wm.setWaypoint(120.0f, -40.0f, 1);
        check(wm.waypoint().active && nearly(wm.waypoint().x, 120.0f, 1e-5f) &&
              nearly(wm.waypoint().z, -40.0f, 1e-5f), "M3b waypoint set");
        wm.clearWaypoint();
        check(!wm.waypoint().active, "M3c waypoint cleared");
    }

    // ---- M4: fast-travel gates. ---------------------------------------------
    {
        StoryFlags flags;
        const MapPoi& city = pois.pois[pois.indexOf("city")];
        check(fastTravelGate(city, flags, false) == FastTravelGate::Undiscovered,
              "M4 undiscovered POI blocked");
        flags.set(poiFoundFlag(city.id));
        check(fastTravelGate(city, flags, false) == FastTravelGate::Ok,
              "M4b discovered POI allowed");
        flags.set("alert.active");
        check(fastTravelGate(city, flags, false) == FastTravelGate::Alert,
              "M4c alert flag blocks (the alert hook)");
        flags.clear("alert.active");
        check(fastTravelGate(city, flags, true) == FastTravelGate::Mission,
              "M4d no_fasttravel mission stage blocks");
        const MapPoi& secret = pois.pois[pois.indexOf("hidden_cache")];
        flags.set(poiFoundFlag(secret.id));
        check(fastTravelGate(secret, flags, false) == FastTravelGate::NotAnAnchor,
              "M4e secret POIs are not travel anchors");
    }

    // ---- M5: zoom/pan math — convergence + the cursor-anchored invariant. --
    {
        MapCamera cam;
        cam.setViewport(1280.0f, 720.0f);
        cam.jumpTo(0.0f, 0.0f, 1.0f);
        // World point under (900, 200) BEFORE the zoom.
        float w0x, w0z; cam.pxToWorld(900.0f, 200.0f, w0x, w0z);
        cam.zoomAt(900.0f, 200.0f, 3.0f);
        bool anchored = true;
        for (int i = 0; i < 240; ++i) {
            cam.update(1.0f / 60.0f);
            float wx, wz; cam.pxToWorld(900.0f, 200.0f, wx, wz);
            if (!nearly(wx, w0x, 0.05f) || !nearly(wz, w0z, 0.05f)) { anchored = false; break; }
        }
        check(anchored, "M5 cursor-anchored zoom invariant (point under cursor fixed)");
        check(nearly(cam.scale, cam.tScale, 1e-3f) && cam.scale > 1.0f,
              "M5b zoom lerp converges", "scale " + std::to_string(cam.scale));
        // Pan target convergence.
        cam.panWorld(75.0f, -30.0f);
        for (int i = 0; i < 240; ++i) cam.update(1.0f / 60.0f);
        check(cam.settled(1e-3f, 0.05f), "M5c pan lerp converges");
        // Clamps hold.
        cam.zoomAt(640.0f, 360.0f, +200.0f);
        for (int i = 0; i < 240; ++i) cam.update(1.0f / 60.0f);
        check(cam.scale <= cam.maxScale + 1e-3f, "M5d zoom clamps at room detail");
        cam.zoomAt(640.0f, 360.0f, -400.0f);
        for (int i = 0; i < 360; ++i) cam.update(1.0f / 60.0f);
        check(cam.scale >= cam.minScale - 1e-5f, "M5e zoom clamps at world overview");
    }

    // ---- M6: floor-slice selection from the REAL canonical LevelDoc. -------
    {
        WorldMapSystem wm;
        const std::string doc = canonProjectJsonPath();
        const bool haveDoc = std::filesystem::exists(doc);
        wm.init(worldMapPoisJsonPath(), haveDoc ? doc : "");
        if (haveDoc && wm.floorCount() >= 7) {
            check(wm.floorForY(0.0f) == 1,   "M6 floorForY(0) = F1");
            check(wm.floorForY(10.0f) == 2,  "M6b floorForY(10) = F2");
            check(wm.floorForY(91.0f) == 7,  "M6c floorForY(91) = F7");
            wm.selectFloor(3);
            check(wm.selectedFloor() == 3, "M6d selectFloor(3)");
            wm.selectFloor(99);
            check(wm.selectedFloor() == 3, "M6e selectFloor(unknown) is a no-op");
        } else {
            x3::logWarn("[worldmap-test] canonical LevelDoc not found — floor checks skipped");
            check(true, "M6 (skipped: no LevelDoc on this machine)");
        }
    }

    // ---- M7: floor tile bake (blueprint pixels from room geometry). --------
    {
        const CanonFloor tf = makeTestFloor();
        std::vector<uint8_t> px;
        bakeFloorTilePixels(tf, px, 128, -8.0f, -8.0f, 8.0f, 8.0f);
        size_t lit = 0;
        for (size_t i = 3; i < px.size(); i += 4) if (px[i] > 0) ++lit;
        check(lit > 1000, "M7 floor bake paints rooms", std::to_string(lit) + " px");
        // Room A's center pixel is filled; a corner outside both rooms is not.
        auto alphaAt = [&](float wx, float wz) {
            const int x = (int)((wx + 8.0f) / 16.0f * 128.0f);
            const int y = (int)((wz + 8.0f) / 16.0f * 128.0f);
            return px[((size_t)y * 128 + x) * 4 + 3];
        };
        check(alphaAt(-3.0f, 0.0f) > 0, "M7b room interior painted");
        check(alphaAt(-7.5f, -7.5f) == 0, "M7c outside rooms transparent");
    }

    // ---- M8: entity tile bake from REAL mesh AABBs (headless meshBounds). --
    {
        HeadlessRenderDevice dev;
        Scene scene;
        // A 4x3x4 m "building" mesh at the origin of its local space...
        x3::rhi::MeshVertex v[8] = {};
        int n = 0;
        for (int xi = 0; xi <= 1; ++xi) for (int yi = 0; yi <= 1; ++yi) for (int zi = 0; zi <= 1; ++zi) {
            v[n].pos[0] = xi ? 2.0f : -2.0f;
            v[n].pos[1] = yi ? 3.0f : 0.0f;
            v[n].pos[2] = zi ? 2.0f : -2.0f;
            ++n;
        }
        uint32_t idx[3] = { 0, 1, 2 };
        Entity e;
        e.mesh = dev.createMesh(v, 8, idx, 3);
        // ...placed at world (10, 0, -6).
        e.transform[12] = 10.0f; e.transform[13] = 0.0f; e.transform[14] = -6.0f;
        std::vector<uint32_t> ents;
        ents.push_back(scene.add(e));
        std::vector<uint8_t> px;
        const uint32_t painted =
            bakeEntityTilePixels(scene, dev, ents, px, 128, 0.0f, -16.0f, 32.0f, 16.0f, -1.0f, 20.0f);
        check(painted == 1, "M8 entity footprint rasterized");
        auto alphaAt = [&](float wx, float wz) {
            const int x = (int)((wx - 0.0f) / 32.0f * 128.0f);
            const int y = (int)((wz + 16.0f) / 32.0f * 128.0f);
            return px[((size_t)y * 128 + x) * 4 + 3];
        };
        check(alphaAt(10.0f, -6.0f) > 0, "M8b world AABB lands at the entity's position");
        check(alphaAt(26.0f, 8.0f) == 0, "M8c empty ground stays transparent");
    }

    // ---- M9: FAST TRAVEL through the streaming-aware path. ------------------
    // Teleport to a distant discovered POI; WorldStreamer's proxy fallback +
    // realize must land the destination region with a sane ledger.
    {
        WorldRegionGraph graph;
        std::vector<std::string> errs;
        const bool gok = graph.load(worldRegionsJsonPath(), errs);
        for (const std::string& e : errs) x3::logWarn("[worldmap-test] " + e);
        if (gok && !graph.empty()) {
            std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
            if (phys->init()) {
                HeadlessRenderDevice dev;
                Scene scene;
                WorldStreamer wsm;
                wsm.init(graph, /*jobs=*/nullptr);   // sync parse — fine headless
                const int spire = wsm.indexOf("spire_f1");
                const int ocean = wsm.indexOf("ocean_base");
                check(spire >= 0 && ocean >= 0, "M9 graph has spire_f1 + ocean_base");
                const WorldRegionDesc& sd = wsm.desc((uint32_t)spire);
                wsm.buildStartRegions(scene, dev, *phys, sd.anchor[0], sd.anchor[1], sd.anchor[2]);
                check(wsm.state((uint32_t)spire) == RegionState::Resident,
                      "M9b boot region resident before travel");

                // FAST TRAVEL: snap the player to the ocean-base POI and tick the
                // streamer from there (exactly what the host does after the
                // teleport — the proxy covers the realize window).
                const MapPoi& dst = pois.pois[pois.indexOf("ocean_base")];
                bool proxiedOrResident = false;
                bool resident = false;
                for (int i = 0; i < 600; ++i) {
                    wsm.update(scene, dev, *phys, dst.x, dst.y, dst.z,
                               0.0f, 0.0f, 0.0f, /*budget*/ 50.0, 0.0);
                    if (wsm.proxyActive((uint32_t)ocean) ||
                        wsm.state((uint32_t)ocean) == RegionState::Resident)
                        proxiedOrResident = true;
                    if (wsm.state((uint32_t)ocean) == RegionState::Resident) { resident = true; break; }
                }
                check(proxiedOrResident, "M9c teleport covered (proxy or resident, no void)");
                check(resident, "M9d destination region realizes after fast travel");
                check(resident && !wsm.proxyActive((uint32_t)ocean),
                      "M9e proxy released once the region landed");
                check(wsm.ownedEntityCount((uint32_t)ocean) > 0,
                      "M9f destination ledger owns entities",
                      std::to_string(wsm.ownedEntityCount((uint32_t)ocean)) + " entities");
                wsm.shutdown(scene, dev, *phys);
                check(wsm.meshesCreated() == wsm.meshesDestroyed() &&
                      wsm.bodiesCreated() == wsm.bodiesDestroyed(),
                      "M9g ledger sane at teardown (created == destroyed)");
                phys->shutdown();
            } else {
                check(false, "M9 physics init failed");
            }
        } else {
            check(false, "M9 region graph failed to load");
        }
    }

    x3::logInfo("worldmap: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
