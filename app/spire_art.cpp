// EFLZ Spire per-floor THEMED art overlay (spire_art pass). See app/spire_art.h.
//
// Sibling of env_art.cpp: env_art paints the base ModularSciFi look uniformly
// across the whole Spire; spire_art layers per-floor THEMED dressing ON TOP — the
// "Modular Abandoned Hospital" kit on F3 (Genetics Lab) for clinical-horror
// atmosphere. Placements come from mined-from-Unity manifests under
// tools/manifests/<floor>_*.x3lvl.json (each entry already in X3Native's Y-up
// right-handed frame: pos[xyz] + rotQuat[xyzw] + scale[xyz]). build() anchors each
// placement at the floor's base Y (level1Rooms()[floor].y0), composes a world TRS,
// loads the matching converted GLB (cached by name), and registers a point-light
// fixture for each surgical lamp (emissive driven HARD for the HDR bloom — the
// clinical-horror look). draw() issues the per-drawable model * nodeTransform draws
// AFTER env_art::draw() so the themed dressing layers on top.
//
// TEXTURE HANDLING (stretch goal — fallback chosen): the converted Hospital GLBs
// carry materials but NO embedded textures (Unity packs ship textures separately as
// loose TX_*_ALB/NRM/RMA.png). Engine-side albedo binding from those loose PNGs
// would require wiring a PNG decoder + the IRenderDevice::createTexture path into
// the app layer (stb_image lives only inside engine/ internals, not exposed here),
// which is scope-creep for a stretch goal. We take the documented FALLBACK: rely on
// the GLB materials' baseColorFactor (flat-shaded but correctly tinted). The
// clinical-horror read still works via the emissive surgical lamps + HDR pipeline.
// Texture hit rate: 0/55 (no engine-side albedo binding attempted; baseColorFactor
// carries the look). A future pass can lift the createTexture path into a shared
// helper and match TX_<X>_ALB.png by GLB stem.
//
// Per-asset / per-manifest FALLBACK mirrors env_art: a missing GLB or a
// malformed/missing manifest degrades to "no coverage" for that floor (the env_art
// base stays visible there) and NEVER crashes.
//
// Clean-room: built from IModelLoader + IAssetSource + IRenderDevice + Scene
// interfaces only. No purchased C# / id Tech / RBDOOM source consulted.
#include "spire_art.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace x3::game {

namespace {

// ---- The Hospital kit lives in this subdir under the mounted converted_glb dir. -
const char* kHospitalSubdir = "Modular_Abandoned_Hospital";

// Compose a column-major 4x4 world TRS from a manifest placement: position (world),
// a rotation quaternion (x,y,z,w), and a per-axis scale. Matches glTF/glm column-
// major convention so it feeds mulMat4 / drawMesh directly. The manifest already
// expresses everything in X3Native's Y-up right-handed frame, so no axis swap.
void composeTRS(float m[16],
                float px, float py, float pz,
                float qx, float qy, float qz, float qw,
                float sx, float sy, float sz) {
    // Normalize the quaternion defensively (mined data may carry tiny drift).
    float n = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
    if (n > 1e-8f) { qx/=n; qy/=n; qz/=n; qw/=n; } else { qx=qy=qz=0.0f; qw=1.0f; }
    const float xx=qx*qx, yy=qy*qy, zz=qz*qz;
    const float xy=qx*qy, xz=qx*qz, yz=qy*qz;
    const float wx=qw*qx, wy=qw*qy, wz=qw*qz;
    // Rotation basis columns (R), each then scaled by the matching axis scale.
    // col0 = R * (sx,0,0), col1 = R * (0,sy,0), col2 = R * (0,0,sz).
    m[0]  = (1.0f - 2.0f*(yy+zz)) * sx;
    m[1]  = (2.0f*(xy+wz))        * sx;
    m[2]  = (2.0f*(xz-wy))        * sx;
    m[3]  = 0.0f;
    m[4]  = (2.0f*(xy-wz))        * sy;
    m[5]  = (1.0f - 2.0f*(xx+zz)) * sy;
    m[6]  = (2.0f*(yz+wx))        * sy;
    m[7]  = 0.0f;
    m[8]  = (2.0f*(xz+wy))        * sz;
    m[9]  = (2.0f*(yz-wx))        * sz;
    m[10] = (1.0f - 2.0f*(xx+yy)) * sz;
    m[11] = 0.0f;
    m[12] = px; m[13] = py; m[14] = pz; m[15] = 1.0f;
}

// ---- One parsed manifest placement. ----
struct Placement {
    std::string name;          // GLB file stem (maps to <subdir>/<name>.glb)
    float pos[3]   = {0,0,0};
    float rot[4]   = {0,0,0,1}; // x,y,z,w
    float scale[3] = {1,1,1};
    bool  ok = false;
};

// ---- Minimal hand-rolled JSON parser for THIS manifest shape ONLY. --------------
// The manifest is a constrained, regular format: a top-level ARRAY of flat OBJECTs,
// each with string fields ("prefab","name") and number-array fields ("pos","rotQuat",
// "scale"). We do not need a general JSON library — we scan for the keys we care
// about per object. Robust to whitespace/newlines and the leading-`-` minus signs in
// the mined floats. Any parse hiccup yields ok=false placements (skipped), so a
// malformed manifest degrades to "no coverage", never a crash. ----
struct JsonScanner {
    const std::string& s;
    size_t i = 0;
    explicit JsonScanner(const std::string& str) : s(str) {}

    void skipWs() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    bool eof() const { return i >= s.size(); }
    char peek() const { return i < s.size() ? s[i] : '\0'; }

    // Read a JSON string literal (assumes current char is the opening quote).
    bool readString(std::string& out) {
        if (peek() != '"') return false;
        ++i; out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '\\') { if (i < s.size()) out.push_back(s[i++]); continue; }
            if (c == '"') return true;
            out.push_back(c);
        }
        return false;
    }

    // Read up to `n` floats from a "[ a, b, c ]" array into out[]. Returns count read.
    int readNumberArray(float* out, int n) {
        skipWs();
        if (peek() != '[') return 0;
        ++i;
        int k = 0;
        while (i < s.size()) {
            skipWs();
            if (peek() == ']') { ++i; break; }
            // Parse a number (handles leading '-', decimals, exponents).
            size_t start = i;
            while (i < s.size()) {
                char c = s[i];
                if (c == ',' || c == ']' || std::isspace((unsigned char)c)) break;
                ++i;
            }
            if (i > start) {
                if (k < n) out[k] = std::strtof(s.substr(start, i - start).c_str(), nullptr);
                ++k;
            }
            skipWs();
            if (peek() == ',') ++i;
        }
        return k;
    }
};

// Parse the whole manifest text into a list of placements. Returns false if the text
// is not a JSON array (the caller then treats the floor as uncovered).
bool parseManifest(const std::string& text, std::vector<Placement>& out) {
    JsonScanner sc(text);
    sc.skipWs();
    if (sc.peek() != '[') return false;
    ++sc.i;
    while (!sc.eof()) {
        sc.skipWs();
        if (sc.peek() == ']') { ++sc.i; break; }
        if (sc.peek() == ',') { ++sc.i; continue; }
        if (sc.peek() != '{') { ++sc.i; continue; }  // skip stray tokens defensively
        ++sc.i;  // enter object
        Placement p;
        // Scan key:value pairs until the matching '}'.
        while (!sc.eof()) {
            sc.skipWs();
            if (sc.peek() == '}') { ++sc.i; break; }
            if (sc.peek() == ',') { ++sc.i; continue; }
            if (sc.peek() != '"') { ++sc.i; continue; }
            std::string key;
            if (!sc.readString(key)) break;
            sc.skipWs();
            if (sc.peek() == ':') ++sc.i;
            sc.skipWs();
            if (key == "name") {
                std::string v;
                if (sc.readString(v)) p.name = v;
            } else if (key == "prefab") {
                std::string v; sc.readString(v);  // consumed, unused (we key on name)
            } else if (key == "pos") {
                sc.readNumberArray(p.pos, 3);
            } else if (key == "rotQuat") {
                sc.readNumberArray(p.rot, 4);
            } else if (key == "scale") {
                sc.readNumberArray(p.scale, 3);
            } else {
                // Unknown key: skip its value (string, array, or bare token).
                sc.skipWs();
                if (sc.peek() == '"') { std::string v; sc.readString(v); }
                else if (sc.peek() == '[') { float tmp[8]; sc.readNumberArray(tmp, 8); }
                else { while (!sc.eof() && sc.peek() != ',' && sc.peek() != '}') ++sc.i; }
            }
        }
        p.ok = !p.name.empty();
        out.push_back(std::move(p));
    }
    return true;
}

// True if the GLB stem reads as a surgical lamp (an HDR emissive light fixture).
bool isSurgicalLamp(const std::string& name) {
    return name.find("Surgical_Lamp") != std::string::npos
        || name.find("Fluoresent_Light") != std::string::npos;  // (sic — pack spelling)
}

} // namespace

uint32_t SpireArtSystem::loadAsset(const std::string& relPath) {
    // Cache: one upload per unique kit piece.
    for (uint32_t i = 0; i < m_assetPaths.size(); ++i)
        if (m_assetPaths[i] == relPath) return i;

    SpireArtAsset a;
    if (m_loader) {
        a.model = m_loader->load(relPath);
        if (a.model.ok) {
            a.drawables = x3::asset::makeDrawables(a.model);
            a.ok = !a.drawables.empty();
        }
    }
    if (a.ok)
        x3::logInfo("[spire-art] loaded " + relPath + " — " +
                    std::to_string(a.drawables.size()) + " drawable prim(s)");
    else
        x3::logWarn("[spire-art] FAILED to load " + relPath + " (env_art base kept)");

    uint32_t idx = (uint32_t)m_assetTable.size();
    m_assetTable.push_back(std::move(a));
    m_assetPaths.push_back(relPath);
    return idx;
}

void SpireArtSystem::addInstance(uint32_t a, L1Floor f, const float transform[16]) {
    if (a >= m_assetTable.size() || !m_assetTable[a].ok) return;  // skip failed assets
    SpireArtInstance e; e.asset = a; e.floor = f;
    for (int i = 0; i < 16; ++i) e.transform[i] = transform[i];
    m_instances.push_back(e);
}

void SpireArtSystem::addInstanceEmissive(uint32_t a, L1Floor f,
                                         const float transform[16],
                                         const float emissive[4]) {
    if (a >= m_assetTable.size() || !m_assetTable[a].ok) return;  // skip failed assets
    SpireArtInstance e; e.asset = a; e.floor = f;
    for (int i = 0; i < 16; ++i) e.transform[i] = transform[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    m_instances.push_back(e);
}

bool SpireArtSystem::floorFromManifestName(std::string_view fileName, L1Floor& out) {
    // Filenames look like "f3_overview.x3lvl.json"; key off the leading "<floor>_".
    // Lowercase the prefix so "F3_" / "f3_" both match.
    std::string fn(fileName);
    for (char& c : fn) c = (char)std::tolower((unsigned char)c);
    struct Map { const char* prefix; L1Floor f; };
    static const Map kMap[] = {
        { "b1_", L1Floor::B1 }, { "f1_", L1Floor::F1 }, { "f2_", L1Floor::F2 },
        { "f3_", L1Floor::F3 }, { "f4_", L1Floor::F4 }, { "f5_", L1Floor::F5 },
        { "f6_", L1Floor::F6 }, { "f7_", L1Floor::F7 },
    };
    for (const Map& m : kMap) {
        if (fn.rfind(m.prefix, 0) == 0) { out = m.f; return true; }
    }
    return false;
}

bool SpireArtSystem::loadManifestForFloor(L1Floor floor,
                                          std::string_view manifestPath,
                                          std::string_view glbSubdir,
                                          const Level1Layout& layout) {
    // Read the manifest file off disk (it lives under the manifests dir, NOT the
    // mounted asset VFS — it is authoring metadata, not a shipped asset).
    std::ifstream in(std::filesystem::path(manifestPath), std::ios::binary);
    if (!in) {
        x3::logWarn("[spire-art] manifest open failed: " + std::string(manifestPath));
        return false;
    }
    std::ostringstream ss; ss << in.rdbuf();
    const std::string text = ss.str();

    std::vector<Placement> placements;
    if (!parseManifest(text, placements) || placements.empty()) {
        x3::logWarn("[spire-art] manifest parse yielded no placements: " +
                    std::string(manifestPath));
        return false;
    }

    // Anchor everything at the floor's base Y (the Spire is a non-uniform vertical
    // stack; the manifest authored each placement relative to a y0 plate). F3 = 20 m.
    const float baseY = layout.floorBaseY[(uint32_t)floor];

    // HDR surgical-lamp emissive: cold clinical-white driven HARD so the lamps read
    // as bright HDR sources feeding the bloom chain (THE clinical-horror look).
    const float kLampEmis[4] = { 0.85f, 0.92f, 1.00f, 7.0f };
    // Point-light tint premultiplied by intensity (linear; mesh.frag accumulates).
    const float kLampInt = 3.0f;
    const float kLampCol[3] = { 0.85f*kLampInt, 0.92f*kLampInt, 1.00f*kLampInt };

    uint32_t placed = 0;
    bool anyAssetOk = false;
    float m[16];
    for (const Placement& p : placements) {
        if (!p.ok) continue;
        const std::string rel = std::string(glbSubdir) + "/" + p.name + ".glb";
        const uint32_t a = loadAsset(rel);
        if (a >= m_assetTable.size() || !m_assetTable[a].ok) continue;  // missing GLB -> skip
        anyAssetOk = true;

        composeTRS(m,
                   p.pos[0], baseY + p.pos[1], p.pos[2],
                   p.rot[0], p.rot[1], p.rot[2], p.rot[3],
                   p.scale[0], p.scale[1], p.scale[2]);

        if (isSurgicalLamp(p.name)) {
            addInstanceEmissive(a, floor, m, kLampEmis);
            // Register a forward point light at the lamp's world position so the
            // surgical bay is actually LIT (cold clinical pool), not just a glowing
            // fixture mesh. The lamp head hangs at world (px, baseY+py, pz).
            x3::rhi::PointLight pl;
            pl.pos[0] = p.pos[0];
            pl.pos[1] = baseY + p.pos[1];
            pl.pos[2] = p.pos[2];
            pl.range  = 8.0f;
            pl.color[0] = kLampCol[0]; pl.color[1] = kLampCol[1]; pl.color[2] = kLampCol[2];
            m_lightFixtures.push_back(pl);
        } else {
            addInstance(a, floor, m);
        }
        ++placed;
    }

    x3::logInfo("[spire-art] floor manifest " + std::string(manifestPath) + ": " +
                std::to_string(placed) + " instance(s) placed (baseY=" +
                std::to_string(baseY) + ")");
    return anyAssetOk && placed > 0;
}

SpireArtMask SpireArtSystem::build(x3::rhi::IRenderDevice& device,
                                   std::string_view convertedGlbDir,
                                   std::string_view manifestsDir,
                                   const Level1Layout& layout) {
    SpireArtMask mask;  // all false until a floor's overlay loads real GLBs

    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(convertedGlbDir, 0)) {
        x3::logWarn("[spire-art] mountDir failed: " + std::string(convertedGlbDir) +
                    " — keeping env_art base on all floors");
        return mask;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));

    // Scan the manifests dir for "<floor>_*.x3lvl.json" files; load each onto its
    // floor. A missing dir / no manifests -> all-false mask (env_art base stays).
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir(manifestsDir);
    if (!fs::is_directory(dir, ec)) {
        x3::logWarn("[spire-art] manifests dir missing: " + std::string(manifestsDir) +
                    " — keeping env_art base on all floors");
        return mask;
    }

    // Deterministic order (filesystem iteration order is unspecified).
    std::vector<fs::path> files;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!ent.is_regular_file()) continue;
        const std::string fn = ent.path().filename().string();
        // Match "*.x3lvl.json".
        if (fn.size() < 11) continue;
        if (fn.find(".x3lvl.json") == std::string::npos) continue;
        files.push_back(ent.path());
    }
    std::sort(files.begin(), files.end());

    for (const fs::path& f : files) {
        const std::string fn = f.filename().string();
        L1Floor floor;
        if (!floorFromManifestName(fn, floor)) {
            x3::logWarn("[spire-art] unknown floor for manifest '" + fn + "' — skipped");
            continue;
        }
        const bool covered = loadManifestForFloor(floor, f.string(),
                                                  kHospitalSubdir, layout);
        if (covered) mask.floorCovered[(uint32_t)floor] = true;
    }

    x3::logInfo("[spire-art] built: " + std::to_string(assetsLoaded()) +
                " asset(s) loaded, " + std::to_string(m_instances.size()) +
                " instance(s), " + std::to_string(m_lightFixtures.size()) +
                " lamp light(s); F3 covered=" +
                std::to_string((int)mask.floorCovered[(uint32_t)L1Floor::F3]));
    return mask;
}

void SpireArtSystem::draw(x3::rhi::IRenderDevice& device,
                          const x3::rhi::FrameContext& frame) const {
    for (const SpireArtInstance& inst : m_instances) {
        const SpireArtAsset& a = m_assetTable[inst.asset];
        for (const auto& d : a.drawables) {
            float fin[16];
            x3::asset::mulMat4(inst.transform, d.nodeTransform, fin);
            // HDR pipeline: emissive instances (surgical lamps) glow + feed bloom.
            device.drawMeshEmissive(frame,
                                    x3::rhi::MeshHandle{ d.meshId },
                                    x3::rhi::TextureHandle{ d.baseColorTexId },
                                    d.baseColorFactor,
                                    inst.emissive,
                                    fin);
        }
    }
}

uint32_t SpireArtSystem::assetsLoaded() const {
    uint32_t n = 0; for (const auto& a : m_assetTable) if (a.ok) ++n; return n;
}

uint32_t SpireArtSystem::instanceCountOnFloor(L1Floor f) const {
    uint32_t n = 0;
    for (const auto& inst : m_instances) if (inst.floor == f) ++n;
    return n;
}

} // namespace x3::game
