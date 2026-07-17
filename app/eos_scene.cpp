// EOS SCENE — "eos-scene-1" loader + grey-box world builder. See eos_scene.h.
//
// Implements the C loader from NATIVE-SCENE-FORMAT.md §"C pseudocode loader"
// against the engine's public seams only (IRenderDevice + the vendored jmini
// JSON DOM — no new dependency). Every deviation from the spec is a logged
// load() failure, not a silent fallback: the benchmark lies if the geometry is
// not the browser's geometry.
#include "eos_scene.h"
#include "json_mini.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace x3::game {

namespace {

constexpr float kTwoPi = 6.2831853f;

// Deterministic per-instance jitter hash (position-keyed, run-to-run stable).
inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline float hash01(uint32_t x) { return (float)(hash32(x) & 0xFFFFFFu) / 16777215.0f; }

// Column-major TRS with a yaw about +Y (engine right-handed) + uniform scale.
inline void trsYaw(float m[16], float x, float y, float z, float yaw, float s) {
    const float c = std::cos(yaw), sn = std::sin(yaw);
    m[0] = c * s;  m[1] = 0; m[2] = -sn * s; m[3] = 0;
    m[4] = 0;      m[5] = s; m[6] = 0;       m[7] = 0;
    m[8] = sn * s; m[9] = 0; m[10] = c * s;  m[11] = 0;
    m[12] = x;     m[13] = y; m[14] = z;     m[15] = 1;
}
// Column-major translate + per-axis scale (boxes/slabs).
inline void trScale(float m[16], float x, float y, float z, float sx, float sy, float sz) {
    m[0] = sx; m[1] = 0; m[2] = 0; m[3] = 0;
    m[4] = 0; m[5] = sy; m[6] = 0; m[7] = 0;
    m[8] = 0; m[9] = 0; m[10] = sz; m[11] = 0;
    m[12] = x; m[13] = y; m[14] = z; m[15] = 1;
}

// ---- grey-box palettes (sRGB-ish 0..1 tints; the device tonemaps) ----------

// Terrain-type tint by NAME (the manifest's terrainTypes index IS the id; the
// name lookup keeps this correct if the table grows/reorders). Unknown names
// get a loud magenta so a table drift is visible in the first capture.
struct Tint3 { float r, g, b; };
Tint3 terrainTint(const std::string& name) {
    if (name == "grass")    return { 0.34f, 0.49f, 0.24f };
    if (name == "dirt")     return { 0.52f, 0.41f, 0.27f };
    if (name == "sand")     return { 0.80f, 0.72f, 0.51f };
    if (name == "rock")     return { 0.47f, 0.47f, 0.49f };
    if (name == "snow")     return { 0.92f, 0.93f, 0.95f };
    if (name == "forest")   return { 0.26f, 0.40f, 0.20f };
    if (name == "shallows") return { 0.27f, 0.51f, 0.59f };
    if (name == "water")    return { 0.16f, 0.27f, 0.43f };
    if (name == "cliff")    return { 0.37f, 0.35f, 0.36f };
    if (name == "road")     return { 0.59f, 0.55f, 0.47f };
    return { 1.0f, 0.0f, 1.0f };
}

// Resource-node tint by kind name (non-tree nodes render as small cubes).
Tint3 nodeTint(const std::string& kind) {
    if (kind == "berry")  return { 0.42f, 0.20f, 0.38f };
    if (kind == "gold")   return { 0.93f, 0.76f, 0.22f };
    if (kind == "stone")  return { 0.56f, 0.56f, 0.58f };
    if (kind == "sheep")  return { 0.90f, 0.88f, 0.82f };
    if (kind == "boar")   return { 0.40f, 0.29f, 0.21f };
    if (kind == "fish")   return { 0.36f, 0.55f, 0.72f };
    if (kind == "lemon")  return { 0.88f, 0.83f, 0.28f };
    if (kind == "copper") return { 0.72f, 0.44f, 0.26f };
    if (kind == "iron")   return { 0.44f, 0.46f, 0.51f };
    if (kind == "lithium")return { 0.78f, 0.86f, 0.82f };
    return { 0.50f, 0.48f, 0.46f };   // the rest of the ore ladder: neutral ore grey
}

// Player tint (buildings + units). 0 = gaia.
Tint3 playerTint(uint8_t p) {
    switch (p) {
        case 0:  return { 0.62f, 0.56f, 0.44f };   // gaia: neutral tan (wildlife/hamlets)
        case 1:  return { 0.22f, 0.38f, 0.86f };   // blue
        case 2:  return { 0.85f, 0.22f, 0.19f };   // red
        case 3:  return { 0.22f, 0.66f, 0.30f };   // green
        case 4:  return { 0.90f, 0.78f, 0.22f };   // yellow
        case 5:  return { 0.60f, 0.28f, 0.72f };   // purple
        case 6:  return { 0.24f, 0.70f, 0.72f };   // cyan
        case 7:  return { 0.90f, 0.52f, 0.20f };   // orange
        default: return { 0.85f, 0.50f, 0.65f };   // pink+
    }
}

// Building box height (wu) by def name — grey-box massing, not art.
float buildingHeight(const std::string& def) {
    if (def == "town-center")    return 3.5f;
    if (def == "princess-tower") return 8.0f;
    if (def == "mill")           return 2.6f;
    if (def == "tavern")         return 2.2f;
    if (def == "stable")         return 1.7f;
    return 1.9f;                                    // house & default
}

// Unit capsule scale by def name (a leviathan is not villager-sized).
float unitScale(const std::string& def) {
    if (def == "leviathan")                                return 3.0f;
    if (def == "desert-dragon" || def == "volcano-dragon") return 2.5f;
    if (def == "giant-spider")                             return 1.8f;
    if (def == "gargoyle")                                 return 1.5f;
    if (def == "wild-centaur")                             return 1.35f;
    if (def == "knight")                                   return 1.15f;
    if (def == "wild-hawk" || def == "vampire-bat" ||
        def == "fairy" || def == "forest-imp")             return 0.6f;
    return 1.0f;
}

// ---- tiny extra primitive: an open cone (rim at y0, apex at y1) ------------
// Appended into a PrimMesh at a vertex offset. Flat per-face normals; no bottom
// cap (the orbit never sees a canopy from below). CCW-from-outside winding.
void appendCone(x3::prims::PrimMesh& m, float r, float y0, float y1, uint32_t seg) {
    const float slope = r / std::max(0.05f, y1 - y0);
    for (uint32_t i = 0; i < seg; ++i) {
        const float a0 = kTwoPi * (float)i / (float)seg;
        const float a1 = kTwoPi * (float)(i + 1) / (float)seg;
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        // Face normal: mid-angle radial tilted up by the slope.
        const float am = (a0 + a1) * 0.5f;
        float nx = std::cos(am), ny = slope, nz = std::sin(am);
        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        nx /= nl; ny /= nl; nz /= nl;
        const uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{c0 * r, y0, s0 * r}, {nx, ny, nz}, {0, 0}});
        m.verts.push_back({{c1 * r, y0, s1 * r}, {nx, ny, nz}, {1, 0}});
        m.verts.push_back({{0.0f,   y1, 0.0f  }, {nx, ny, nz}, {0.5f, 1}});
        // CCW seen from outside: rim0 -> apex -> rim1 (engine +X x +Y = +Z).
        m.index.insert(m.index.end(), {base, base + 2, base + 1});
    }
}

// Trunk cylinder (open tube, base y0..top y1) — same convention as appendCone.
void appendTube(x3::prims::PrimMesh& m, float r, float y0, float y1, uint32_t seg) {
    for (uint32_t i = 0; i < seg; ++i) {
        const float a0 = kTwoPi * (float)i / (float)seg;
        const float a1 = kTwoPi * (float)(i + 1) / (float)seg;
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        const uint32_t base = (uint32_t)m.verts.size();
        m.verts.push_back({{c0 * r, y0, s0 * r}, {c0, 0, s0}, {0, 0}});
        m.verts.push_back({{c1 * r, y0, s1 * r}, {c1, 0, s1}, {1, 0}});
        m.verts.push_back({{c1 * r, y1, s1 * r}, {c1, 0, s1}, {1, 1}});
        m.verts.push_back({{c0 * r, y1, s0 * r}, {c0, 0, s0}, {0, 1}});
        m.index.insert(m.index.end(), {base, base + 3, base + 2, base, base + 2, base + 1});
    }
}

// UV-sphere section appended at a center/radius (capsule caps).
void appendSphere(x3::prims::PrimMesh& m, float cx, float cy, float cz, float r,
                  uint32_t stacks, uint32_t slices) {
    const uint32_t base = (uint32_t)m.verts.size();
    for (uint32_t i = 0; i <= stacks; ++i) {
        const float v = (float)i / (float)stacks;
        const float phi = v * 3.14159265f;
        for (uint32_t j = 0; j <= slices; ++j) {
            const float u = (float)j / (float)slices;
            const float th = u * kTwoPi;
            const float x = std::sin(phi) * std::cos(th);
            const float y = std::cos(phi);
            const float z = std::sin(phi) * std::sin(th);
            m.verts.push_back({{cx + x * r, cy + y * r, cz + z * r}, {x, y, z}, {u, v}});
        }
    }
    const uint32_t cols = slices + 1;
    for (uint32_t i = 0; i < stacks; ++i)
        for (uint32_t j = 0; j < slices; ++j) {
            const uint32_t a = base + i * cols + j, b = a + cols;
            m.index.insert(m.index.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
}

} // namespace

// ===========================================================================
// load — manifest.json + scene.bin, byte-exact per the section table.
// ===========================================================================
bool EosSceneWorld::load(const std::string& dir) {
    using jmini::JVal;
    using jmini::JReader;

    const std::string manifestPath = dir + "/manifest.json";
    const std::string manifestText = jmini::readFile(manifestPath);
    if (manifestText.empty()) {
        x3::logError("[eos-scene] manifest missing/unreadable: " + manifestPath);
        return false;
    }
    JReader rd(manifestText);
    const JVal M = rd.parse();
    if (!rd.ok || M.t != JVal::Obj) {
        x3::logError("[eos-scene] manifest JSON parse failed: " + manifestPath);
        return false;
    }

    // ---- version + globals ----
    if (M.sval("version") != "eos-scene-1") {
        x3::logError("[eos-scene] unsupported version '" + M.sval("version") +
                     "' (want eos-scene-1)");
        return false;
    }
    m_seed      = M.inum("seed", 0);
    m_mapScript = M.sval("mapScript");
    if (const JVal* ms = M.get("mapSize")) { m_W = ms->inum("w", 0); m_H = ms->inum("h", 0); }
    m_R = M.inum("reliefRes", 1);
    m_waterLevel = M.fnum("waterLevel", -0.16f);
    if (M.inum("worldUnitsPerTile", 1) != 1) {
        x3::logError("[eos-scene] worldUnitsPerTile != 1 — scale contract changed, refusing");
        return false;
    }
    if (m_W <= 0 || m_H <= 0 || m_R <= 0) {
        x3::logError("[eos-scene] bad mapSize/reliefRes");
        return false;
    }
    m_GW1 = m_W * m_R + 1;
    m_GH1 = m_H * m_R + 1;
    if (const JVal* rg = M.get("reliefGrid")) {
        if (rg->inum("w", 0) != m_GW1 || rg->inum("h", 0) != m_GH1) {
            x3::logError("[eos-scene] reliefGrid dims disagree with W*R+1 x H*R+1");
            return false;
        }
    }

    // ---- string tables ----
    auto readStrings = [&](const char* key, std::vector<std::string>& out) {
        out.clear();
        if (const JVal* a = M.get(key); a && a->t == JVal::Arr)
            for (const JVal& v : a->arr) out.push_back(v.str);
    };
    readStrings("terrainTypes", m_terrainTypes);
    readStrings("nodeKinds",    m_nodeKinds);
    readStrings("buildingDefs", m_buildingDefs);
    readStrings("unitDefs",     m_unitDefs);
    m_waterTerrainIds.clear();
    if (const JVal* a = M.get("waterTerrainIds"); a && a->t == JVal::Arr)
        for (const JVal& v : a->arr) m_waterTerrainIds.push_back((int)v.num);

    // ---- camera path (121 keyframes, linear, looping) ----
    m_camKeys.clear();
    if (const JVal* cam = M.get("camera")) {
        m_camDuration = cam->fnum("durationSeconds", 30.0f);
        if (cam->sval("interpolation") != "linear") {
            x3::logError("[eos-scene] camera interpolation != linear — contract changed");
            return false;
        }
        if (const JVal* kfs = cam->get("keyframes"); kfs && kfs->t == JVal::Arr) {
            for (const JVal& k : kfs->arr) {
                CamKey ck{};
                ck.t = k.fnum("t", 0.0f);
                const JVal* p = k.get("pos");
                const JVal* tg = k.get("target");
                if (!p || p->t != JVal::Arr || p->arr.size() != 3 ||
                    !tg || tg->t != JVal::Arr || tg->arr.size() != 3) {
                    x3::logError("[eos-scene] malformed camera keyframe");
                    return false;
                }
                for (int i = 0; i < 3; ++i) { ck.pos[i] = (float)p->arr[i].num;
                                              ck.target[i] = (float)tg->arr[i].num; }
                m_camKeys.push_back(ck);
            }
        }
    }
    if (m_camKeys.size() < 2) {
        x3::logError("[eos-scene] camera path missing/too short (" +
                     std::to_string(m_camKeys.size()) + " keyframes)");
        return false;
    }

    // ---- scene.bin ----
    const std::string binPath = dir + "/" + M.sval("binFile", "scene.bin");
    std::ifstream bf(binPath, std::ios::binary);
    if (!bf) { x3::logError("[eos-scene] scene.bin missing: " + binPath); return false; }
    std::vector<uint8_t> bin((std::istreambuf_iterator<char>(bf)),
                              std::istreambuf_iterator<char>());
    const long long binBytes = (long long)M.fnum("binBytes", -1.0f);
    // (fnum is float — 1.6 MB is exactly representable; re-read via num for safety)
    long long binBytesExact = -1;
    if (const JVal* bb = M.get("binBytes"); bb && bb->t == JVal::Num)
        binBytesExact = (long long)bb->num;
    (void)binBytes;
    if (binBytesExact < 0 || (long long)bin.size() != binBytesExact) {
        x3::logError("[eos-scene] scene.bin size " + std::to_string(bin.size()) +
                     " != manifest binBytes " + std::to_string(binBytesExact));
        return false;
    }

    // Section lookup: name -> {dtype, count, offset, byteLength}, validated.
    const JVal* sections = M.get("sections");
    if (!sections || sections->t != JVal::Obj) {
        x3::logError("[eos-scene] manifest has no sections table");
        return false;
    }
    auto elemSize = [](const std::string& dt) -> size_t {
        if (dt == "u8") return 1; if (dt == "u16") return 2;
        if (dt == "u32" || dt == "f32") return 4; return 0;
    };
    // Copy a section into a typed vector, checking dtype/bounds/size.
    bool ok = true;
    auto section = [&](const char* name, const char* wantDtype, void* dst,
                       size_t dstElem, size_t* outCount) -> bool {
        const JVal* s = sections->get(name);
        if (!s) { x3::logError(std::string("[eos-scene] section missing: ") + name); return false; }
        const std::string dt = s->sval("dtype");
        const size_t count = (size_t)s->fnum("count", 0);
        const size_t off = (size_t)s->fnum("offset", 0);
        const size_t bytes = (size_t)s->fnum("byteLength", 0);
        if (dt != wantDtype || elemSize(dt) != dstElem || bytes != count * dstElem ||
            off + bytes > bin.size()) {
            x3::logError(std::string("[eos-scene] section bad dtype/bounds: ") + name);
            return false;
        }
        if (outCount) *outCount = count;
        std::memcpy(dst, bin.data() + off, bytes);
        return true;
    };
    auto sectionCount = [&](const char* name) -> size_t {
        const JVal* s = sections->get(name);
        return s ? (size_t)s->fnum("count", 0) : 0;
    };

    m_terrain.resize(sectionCount("terrain"));
    m_heights.resize(sectionCount("reliefHeights"));
    m_ao.resize(sectionCount("reliefAO"));
    m_nodePos.resize(sectionCount("nodePos"));
    m_nodeKind.resize(sectionCount("nodeKind"));
    m_grovePos.resize(sectionCount("grovePos"));
    m_groveSeed.resize(sectionCount("groveSeed"));
    m_buildingRect.resize(sectionCount("buildingRect"));
    m_buildingDef.resize(sectionCount("buildingDef"));
    m_buildingPlayer.resize(sectionCount("buildingPlayer"));
    m_buildingFlags.resize(sectionCount("buildingFlags"));
    m_unitPos.resize(sectionCount("unitPos"));
    m_unitFacing.resize(sectionCount("unitFacing"));
    m_unitType.resize(sectionCount("unitType"));
    m_unitPlayer.resize(sectionCount("unitPlayer"));
    m_unitFlags.resize(sectionCount("unitFlags"));

    ok = ok && section("terrain",        "u8",  m_terrain.data(),        1, nullptr);
    ok = ok && section("reliefHeights",  "f32", m_heights.data(),        4, nullptr);
    ok = ok && section("reliefAO",       "f32", m_ao.data(),             4, nullptr);
    ok = ok && section("nodePos",        "f32", m_nodePos.data(),        4, nullptr);
    ok = ok && section("nodeKind",       "u8",  m_nodeKind.data(),       1, nullptr);
    ok = ok && section("grovePos",       "f32", m_grovePos.data(),       4, nullptr);
    ok = ok && section("groveSeed",      "u32", m_groveSeed.data(),      4, nullptr);
    ok = ok && section("buildingRect",   "u16", m_buildingRect.data(),   2, nullptr);
    ok = ok && section("buildingDef",    "u16", m_buildingDef.data(),    2, nullptr);
    ok = ok && section("buildingPlayer", "u8",  m_buildingPlayer.data(), 1, nullptr);
    ok = ok && section("buildingFlags",  "u8",  m_buildingFlags.data(),  1, nullptr);
    ok = ok && section("unitPos",        "f32", m_unitPos.data(),        4, nullptr);
    ok = ok && section("unitFacing",     "f32", m_unitFacing.data(),     4, nullptr);
    ok = ok && section("unitType",       "u16", m_unitType.data(),       2, nullptr);
    ok = ok && section("unitPlayer",     "u8",  m_unitPlayer.data(),     1, nullptr);
    ok = ok && section("unitFlags",      "u8",  m_unitFlags.data(),      1, nullptr);
    if (!ok) return false;

    // Cross-field sanity: the grids and the parallel arrays must agree.
    if (m_terrain.size() != (size_t)m_W * m_H ||
        m_heights.size() != (size_t)m_GW1 * m_GH1 ||
        m_ao.size() != m_heights.size() ||
        m_nodePos.size() != m_nodeKind.size() * 2 ||
        m_grovePos.size() != m_groveSeed.size() * 2 ||
        m_buildingRect.size() != m_buildingDef.size() * 4 ||
        m_buildingPlayer.size() != m_buildingDef.size() ||
        m_buildingFlags.size() != m_buildingDef.size() ||
        m_unitPos.size() != m_unitType.size() * 2 ||
        m_unitFacing.size() != m_unitType.size() ||
        m_unitPlayer.size() != m_unitType.size() ||
        m_unitFlags.size() != m_unitType.size()) {
        x3::logError("[eos-scene] section counts are inconsistent with each other");
        return false;
    }

    x3::logInfo("[eos-scene] loaded " + m_mapScript + " seed " + std::to_string(m_seed) +
                ": " + std::to_string(m_W) + "x" + std::to_string(m_H) + " tiles, relief " +
                std::to_string(m_GW1) + "x" + std::to_string(m_GH1) +
                ", nodes " + std::to_string(m_nodeKind.size()) +
                ", groves " + std::to_string(m_groveSeed.size()) +
                ", buildings " + std::to_string(m_buildingDef.size()) +
                ", units " + std::to_string(m_unitType.size()) +
                ", camera " + std::to_string(m_camKeys.size()) + " keyframes / " +
                std::to_string(m_camDuration) + " s");
    return true;
}

// ===========================================================================
// heightAt — the spec's triangle-exact interpolation on the b-c split
// (SOURCE space; callers negate Z for the engine afterward).
// ===========================================================================
float EosSceneWorld::heightAt(float x, float z) const {
    const float eps = 1e-4f;
    const int GW = m_GW1 - 1, GH = m_GH1 - 1;
    float cx = std::clamp(x * (float)m_R, 0.0f, (float)GW - eps);
    float cz = std::clamp(z * (float)m_R, 0.0f, (float)GH - eps);
    const int ix = (int)cx, iz = (int)cz;
    const float fx = cx - (float)ix, fz = cz - (float)iz;
    auto h = [&](int i, int j) { return m_heights[(size_t)j * m_GW1 + i]; };
    if (fx + fz <= 1.0f)
        return h(ix, iz) + (h(ix + 1, iz) - h(ix, iz)) * fx
                         + (h(ix, iz + 1) - h(ix, iz)) * fz;
    return h(ix + 1, iz + 1) + (h(ix, iz + 1) - h(ix + 1, iz + 1)) * (1.0f - fx)
                             + (h(ix + 1, iz) - h(ix + 1, iz + 1)) * (1.0f - fz);
}

// ===========================================================================
// cameraAt — linear interpolation over the canonical keyframes, looping;
// output converted to engine space (Z negated) as position + unit forward.
// ===========================================================================
void EosSceneWorld::cameraAt(float t, float pos[3], float fwd[3]) const {
    const float dur = m_camDuration > 0.0f ? m_camDuration : 30.0f;
    float tt = std::fmod(t, dur);
    if (tt < 0.0f) tt += dur;
    // Keyframes are uniformly spaced (t = 0, 0.25, ... dur) but walk by time
    // anyway so a non-uniform path would still replay correctly.
    size_t i = 0;
    while (i + 2 < m_camKeys.size() && m_camKeys[i + 1].t <= tt) ++i;
    const CamKey& a = m_camKeys[i];
    const CamKey& b = m_camKeys[i + 1];
    const float span = std::max(1e-6f, b.t - a.t);
    const float f = std::clamp((tt - a.t) / span, 0.0f, 1.0f);
    float p[3], tg[3];
    for (int k = 0; k < 3; ++k) {
        p[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * f;
        tg[k] = a.target[k] + (b.target[k] - a.target[k]) * f;
    }
    // LEFT-handed source -> RIGHT-handed engine: negate Z once.
    pos[0] = p[0]; pos[1] = p[1]; pos[2] = -p[2];
    float fx = tg[0] - p[0], fy = tg[1] - p[1], fz = -(tg[2] - p[2]);
    const float l = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (l > 1e-6f) { fx /= l; fy /= l; fz /= l; }
    fwd[0] = fx; fwd[1] = fy; fwd[2] = fz;
}

// ===========================================================================
// build — GPU meshes + instance transforms.
// ===========================================================================
void EosSceneWorld::build(x3::rhi::IRenderDevice& device) {
    if (m_built) return;
    m_built = true;
    buildTerrain(device);
    buildInstances(device);
    x3::logInfo("[eos-scene] built: " + std::to_string(m_terrainMeshes.size()) +
                " terrain chunks + " + std::to_string(m_instances.size()) +
                " instances (" + std::to_string(m_treeCount) + " trees)");
}

void EosSceneWorld::buildTerrain(x3::rhi::IRenderDevice& device) {
    const int GW = m_GW1 - 1, GH = m_GH1 - 1;
    const float invR = 1.0f / (float)m_R;

    // ---- per-vertex tint texture: terrain-type color of the vertex's tile,
    // multiplied by the baked valley AO. One texel per fine-grid vertex; the
    // terrain UVs sample texel centers, so bilinear filtering gives a soft
    // cross-fade at tile borders (grey-box friendly, no splat shader).
    {
        std::vector<uint8_t> px((size_t)m_GW1 * m_GH1 * 4);
        for (int fz = 0; fz < m_GH1; ++fz) {
            for (int fx = 0; fx < m_GW1; ++fx) {
                const int tx = std::min(fx / m_R, m_W - 1);
                const int ty = std::min(fz / m_R, m_H - 1);
                const uint8_t id = m_terrain[(size_t)ty * m_W + tx];
                const Tint3 c = (id < m_terrainTypes.size())
                                    ? terrainTint(m_terrainTypes[id])
                                    : Tint3{1, 0, 1};
                const float ao = m_ao[(size_t)fz * m_GW1 + fx];
                uint8_t* p = &px[((size_t)fz * m_GW1 + fx) * 4];
                p[0] = (uint8_t)std::clamp((int)(c.r * ao * 255.0f + 0.5f), 0, 255);
                p[1] = (uint8_t)std::clamp((int)(c.g * ao * 255.0f + 0.5f), 0, 255);
                p[2] = (uint8_t)std::clamp((int)(c.b * ao * 255.0f + 0.5f), 0, 255);
                p[3] = 255;
            }
        }
        m_terrainTex = device.createTexture(px.data(), (uint32_t)m_GW1, (uint32_t)m_GH1,
                                            /*srgb=*/true);
    }

    // ---- chunked terrain mesh: world-baked vertices, identity transforms, so
    // the device's per-instance frustum/HZB cull can drop off-screen chunks.
    const int kChunk = 64;   // fine cells per chunk edge
    std::vector<x3::rhi::MeshVertex> verts;
    std::vector<uint32_t> idx;
    auto H = [&](int i, int j) { return m_heights[(size_t)j * m_GW1 + i]; };
    for (int cz0 = 0; cz0 < GH; cz0 += kChunk) {
        for (int cx0 = 0; cx0 < GW; cx0 += kChunk) {
            const int cw = std::min(kChunk, GW - cx0);
            const int ch = std::min(kChunk, GH - cz0);
            verts.clear(); idx.clear();
            verts.reserve((size_t)(cw + 1) * (ch + 1));
            idx.reserve((size_t)cw * ch * 6);
            for (int z = 0; z <= ch; ++z) {
                for (int x = 0; x <= cw; ++x) {
                    const int fx = cx0 + x, fz = cz0 + z;
                    // Central-difference normal in source space; engine normal
                    // negates the Z component (same mirror as the position).
                    const float step = invR;
                    const float hl = H(std::max(fx - 1, 0), fz);
                    const float hr = H(std::min(fx + 1, GW), fz);
                    const float hd = H(fx, std::max(fz - 1, 0));
                    const float hu = H(fx, std::min(fz + 1, GH));
                    float nx = (hl - hr) / (2.0f * step);
                    float nz = (hd - hu) / (2.0f * step);
                    float ny = 1.0f;
                    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                    nx /= nl; ny /= nl; nz /= nl;
                    x3::rhi::MeshVertex v;
                    v.pos[0] = (float)fx * invR;
                    v.pos[1] = H(fx, fz);
                    v.pos[2] = -(float)fz * invR;          // the ONE Z negation
                    v.normal[0] = nx; v.normal[1] = ny; v.normal[2] = -nz;
                    v.uv[0] = ((float)fx + 0.5f) / (float)m_GW1;
                    v.uv[1] = ((float)fz + 0.5f) / (float)m_GH1;
                    verts.push_back(v);
                }
            }
            auto vid = [&](int x, int z) { return (uint32_t)(z * (cw + 1) + x); };
            for (int z = 0; z < ch; ++z) {
                for (int x = 0; x < cw; ++x) {
                    // Spec split: a-b-c / b-d-c on the b-c diagonal. Emitted in
                    // the SAME index order over Z-negated vertices == the
                    // required winding flip (CCW from +Y in engine space).
                    const uint32_t a = vid(x, z), b = vid(x + 1, z);
                    const uint32_t c = vid(x, z + 1), d = vid(x + 1, z + 1);
                    idx.insert(idx.end(), {a, b, c, b, d, c});
                }
            }
            m_terrainMeshes.push_back(device.createMesh(
                verts.data(), (uint32_t)verts.size(), idx.data(), (uint32_t)idx.size()));
        }
    }
}

void EosSceneWorld::buildInstances(x3::rhi::IRenderDevice& device) {
    using x3::prims::PrimMesh;

    // ---- shared grey-box meshes ----
    // TREE: trunk tube + canopy cone, ~2.2 wu tall, authored feet-at-origin.
    {
        PrimMesh t;
        appendTube(t, 0.10f, 0.0f, 0.9f, 6);
        appendCone(t, 0.55f, 0.6f, 2.2f, 8);
        m_treeMesh = device.createMesh(t.verts.data(), (uint32_t)t.verts.size(),
                                       t.index.data(), (uint32_t)t.index.size());
    }
    // GROVE GIANT: the Hometree-scale landmark — a big trunk + tall cone.
    {
        PrimMesh g;
        appendTube(g, 0.55f, 0.0f, 4.0f, 8);
        appendCone(g, 3.6f, 2.5f, 12.0f, 10);
        m_giantMesh = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                        g.index.data(), (uint32_t)g.index.size());
    }
    // CUBE: unit cube, base at y=0 (buildings/foundations/resource nodes scale it).
    {
        PrimMesh c = x3::prims::makeBox(0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.0f);
        m_cubeMesh = device.createMesh(c.verts.data(), (uint32_t)c.verts.size(),
                                       c.index.data(), (uint32_t)c.index.size());
    }
    // CAPSULE: r=0.18 body, feet at y=0, ~0.95 wu tall.
    {
        PrimMesh cap;
        appendTube(cap, 0.18f, 0.18f, 0.77f, 10);
        appendSphere(cap, 0.0f, 0.77f, 0.0f, 0.18f, 4, 10);
        appendSphere(cap, 0.0f, 0.18f, 0.0f, 0.18f, 4, 10);
        m_capsuleMesh = device.createMesh(cap.verts.data(), (uint32_t)cap.verts.size(),
                                          cap.index.data(), (uint32_t)cap.index.size());
    }

    m_instances.clear();
    m_instances.reserve(m_nodeKind.size() + m_groveSeed.size() +
                        m_buildingDef.size() + m_unitType.size());

    // Which nodeKind index is "tree" (instance-render these; the point of the test).
    int treeKind = -1;
    for (size_t i = 0; i < m_nodeKinds.size(); ++i)
        if (m_nodeKinds[i] == "tree") { treeKind = (int)i; break; }

    // ---- resource nodes ----
    for (size_t i = 0; i < m_nodeKind.size(); ++i) {
        const float sx = m_nodePos[2 * i], sz = m_nodePos[2 * i + 1];
        const uint8_t kind = m_nodeKind[i];
        const float y = heightAt(sx, sz);
        Inst in{};
        const uint32_t hseed = hash32((uint32_t)(sx * 8.0f) * 73856093u ^
                                      (uint32_t)(sz * 8.0f) * 19349663u);
        if ((int)kind == treeKind) {
            ++m_treeCount;
            in.mesh = m_treeMesh;
            // Deterministic yaw + scale + tint jitter: 25k trees must read as a
            // forest, not a rubber stamp.
            const float yaw = hash01(hseed) * kTwoPi;
            const float s = 0.82f + 0.36f * hash01(hseed ^ 0x9E3779B9u);
            trsYaw(in.model, sx, y, -sz, yaw, s);
            const float g = 0.85f + 0.30f * hash01(hseed ^ 0x85EBCA6Bu);
            in.tint[0] = 0.16f * g; in.tint[1] = 0.34f * g; in.tint[2] = 0.15f * g;
            in.tint[3] = 1.0f;
        } else {
            in.mesh = m_cubeMesh;
            const std::string& kn = (kind < m_nodeKinds.size()) ? m_nodeKinds[kind]
                                                                : std::string("?");
            const Tint3 c = nodeTint(kn);
            // Fish bob at the water plane; everything else sits on the ground.
            const bool fish = (kn == "fish");
            const float base = fish ? m_waterLevel : y;
            const float s = fish ? 0.45f : (0.45f + 0.20f * hash01(hseed));
            trScale(in.model, sx, base, -sz, s, fish ? 0.15f : s * 0.9f, s);
            in.tint[0] = c.r; in.tint[1] = c.g; in.tint[2] = c.b; in.tint[3] = 1.0f;
        }
        m_instances.push_back(in);
    }

    // ---- grove giants ----
    for (size_t i = 0; i < m_groveSeed.size(); ++i) {
        const float sx = m_grovePos[2 * i], sz = m_grovePos[2 * i + 1];
        const float y = heightAt(sx, sz);
        Inst in{};
        in.mesh = m_giantMesh;
        const uint32_t hs = hash32(m_groveSeed[i]);
        const float yaw = hash01(hs) * kTwoPi;
        const float s = 0.9f + 0.4f * hash01(hs ^ 0x9E3779B9u);
        trsYaw(in.model, sx, y, -sz, yaw, s);
        in.tint[0] = 0.13f; in.tint[1] = 0.30f; in.tint[2] = 0.14f; in.tint[3] = 1.0f;
        m_instances.push_back(in);
    }

    // ---- buildings: boxes on their (pre-flattened) footprints ----
    for (size_t i = 0; i < m_buildingDef.size(); ++i) {
        const float tx = (float)m_buildingRect[4 * i + 0];
        const float ty = (float)m_buildingRect[4 * i + 1];
        const float w  = (float)m_buildingRect[4 * i + 2];
        const float h  = (float)m_buildingRect[4 * i + 3];
        const float cx = tx + w * 0.5f, cz = ty + h * 0.5f;
        // The ground under the footprint is already flattened in reliefHeights;
        // sample the centre for the base Y (spec note).
        const float y = heightAt(cx, cz);
        const uint16_t def = m_buildingDef[i];
        const std::string& dn = (def < m_buildingDefs.size()) ? m_buildingDefs[def]
                                                              : std::string("?");
        const bool complete = (m_buildingFlags[i] & 1u) != 0;
        const float bh = complete ? buildingHeight(dn) : 0.18f;   // foundation slab
        Inst in{};
        in.mesh = m_cubeMesh;
        trScale(in.model, cx, y, -cz, w * 0.92f, bh, h * 0.92f);
        const Tint3 c = playerTint(m_buildingPlayer[i]);
        const float f = complete ? 1.0f : 0.55f;
        in.tint[0] = c.r * f; in.tint[1] = c.g * f; in.tint[2] = c.b * f; in.tint[3] = 1.0f;
        m_instances.push_back(in);
    }

    // ---- units: capsules tinted per player ----
    for (size_t i = 0; i < m_unitType.size(); ++i) {
        const float sx = m_unitPos[2 * i], sz = m_unitPos[2 * i + 1];
        const float y = heightAt(sx, sz);
        const uint16_t ut = m_unitType[i];
        const std::string& un = (ut < m_unitDefs.size()) ? m_unitDefs[ut]
                                                         : std::string("?");
        Inst in{};
        in.mesh = m_capsuleMesh;
        // unitFacing is parsed and kept (contract), but a capsule has no visible
        // facing — the transform is yaw-free on purpose (grey-box honesty note).
        trsYaw(in.model, sx, y, -sz, 0.0f, unitScale(un));
        const Tint3 c = playerTint(m_unitPlayer[i]);
        in.tint[0] = c.r; in.tint[1] = c.g; in.tint[2] = c.b; in.tint[3] = 1.0f;
        m_instances.push_back(in);
    }
}

void EosSceneWorld::draw(x3::rhi::IRenderDevice& device,
                         const x3::rhi::FrameContext& frame) const {
    static const float kWhite[4] = { 1, 1, 1, 1 };
    static const float kIdentity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    for (const auto& mh : m_terrainMeshes)
        device.drawMesh(frame, mh, m_terrainTex, kWhite, kIdentity);
    for (const auto& in : m_instances)
        device.drawMesh(frame, in.mesh, {}, in.tint, in.model);
}

void EosSceneWorld::shutdown(x3::rhi::IRenderDevice& device) {
    for (auto& mh : m_terrainMeshes) if (mh.valid()) device.destroyMesh(mh);
    m_terrainMeshes.clear();
    auto kill = [&](x3::rhi::MeshHandle& m) { if (m.valid()) { device.destroyMesh(m); m = {}; } };
    kill(m_treeMesh); kill(m_giantMesh); kill(m_cubeMesh); kill(m_capsuleMesh);
    if (m_terrainTex.valid()) { device.destroyTexture(m_terrainTex); m_terrainTex = {}; }
    m_instances.clear();
    m_built = false;
}

} // namespace x3::game
