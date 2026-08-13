// LEVELDOC WORLD — data-driven loader for the editor's LevelDoc JSON. See header.
#include "leveldoc_world.h"

#include "asset_root.h"
#include "headless_device.h"
#include "mesh_prims.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace x3::game {

namespace {

// Column-major model matrix from pos + the full 11.0 rotation (mirrors
// editor_host::brushMatrix — one convention, editor::rotYPR, for the whole round
// trip; pitch = roll = 0 reproduces the old yaw-only matrix bit-identically).
void objMatrix(const float pos[3], float yaw, float pitch, float roll,
               float m[16], float scale = 1.0f) {
    float R[9];
    x3::editor::rotYPR(yaw, pitch, roll, R);
    m[0]=R[0]*scale;  m[1]=R[1]*scale;  m[2]=R[2]*scale;   m[3]=0;
    m[4]=R[3]*scale;  m[5]=R[4]*scale;  m[6]=R[5]*scale;   m[7]=0;
    m[8]=R[6]*scale;  m[9]=R[7]*scale;  m[10]=R[8]*scale;  m[11]=0;
    m[12]=pos[0]; m[13]=pos[1]; m[14]=pos[2]; m[15]=1;
}

// File write time as a comparable integer (0 if the file is missing).
int64_t fileMtime(const std::string& path) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return (int64_t)t.time_since_epoch().count();
}

} // namespace

// ---------------------------------------------------------------------------
// Material resolve — bakes the curated procedural surface once per MatTex kind
// (session cache shared across reloads). Mirrors EditorHost::resolveMaterial so
// a brush re-skinned in the editor loads back with the SAME surface.
// ---------------------------------------------------------------------------
uint32_t LevelDocWorld::resolveMaterial(const std::string& id,
                                        x3::rhi::IRenderDevice& device, float outTint[3]) {
    int mi = x3::editor::editorMaterialFind(id);
    if (mi < 0) mi = 0;                                  // unknown id -> grid default
    const x3::editor::BlockoutMaterial& mat = x3::editor::editorMaterials()[mi];
    if (outTint) { outTint[0]=mat.tint[0]; outTint[1]=mat.tint[1]; outTint[2]=mat.tint[2]; }
    const uint8_t k = (uint8_t)mat.tex;
    if (k < 8 && m_matTex[k] != 0) return m_matTex[k];   // already baked this session
    std::vector<uint8_t> px; uint32_t n = 256;
    using x3::editor::MatTex;
    switch (mat.tex) {
        case MatTex::Grid:       n = 1000; px = x3::prims::makeBlockoutGridRGBA(n, 10); break;
        case MatTex::Panel:      px = x3::prims::makeSciFiPanelRGBA(n, 2); break;
        case MatTex::CleanPanel: px = x3::prims::makeCleanPanelRGBA(n, 4); break;
        case MatTex::Floor:      px = x3::prims::makeFloorGrateRGBA(n, 2); break;
        case MatTex::Ceiling:    px = x3::prims::makeCeilingPanelRGBA(n, 3); break;
        case MatTex::Solid:      n = 4; px = x3::prims::makeSolidRGBA(n, 255, 255, 255); break;
        default:                 n = 4; px = x3::prims::makeSolidRGBA(n, 255, 255, 255); break;
    }
    x3::rhi::TextureHandle h = device.createTexture(px.data(), n, n, /*srgb*/true);
    if (k < 8) m_matTex[k] = h.id;
    return h.id;
}

// ---------------------------------------------------------------------------
// GLB model session cache (mirrors EditorHost::loadModelCached).
// ---------------------------------------------------------------------------
const LevelDocWorld::LoadedModel* LevelDocWorld::loadModelCached(
        const std::string& relPath, x3::rhi::IRenderDevice& device) {
    auto it = m_modelCache.find(relPath);
    if (it != m_modelCache.end()) return &it->second;
    if (!m_modelLoader) {
        m_modelAssets.reset(x3::asset::createAssetSource());
        const std::string dir = convertedGlbRoot();
        m_modelDirMounted = m_modelAssets->mountDir(dir, 0);
        if (!m_modelDirMounted)
            x3::logWarn("[leveldoc] model mount failed: " + dir);
        m_modelLoader.reset(x3::asset::createModelLoader(&device, m_modelAssets.get()));
    }
    LoadedModel lm;
    if (m_modelLoader && m_modelDirMounted) {
        lm.model = m_modelLoader->load(relPath);
        if (lm.model.ok) {
            lm.drawables = x3::asset::makeDrawables(lm.model);
            lm.ok = !lm.drawables.empty();
        }
    }
    if (lm.ok) x3::logInfo("[leveldoc] model loaded: " + relPath + " (" +
                           std::to_string(lm.drawables.size()) + " drawables)");
    else       x3::logWarn("[leveldoc] model missing/failed (graybox marker): " + relPath);
    auto res = m_modelCache.emplace(relPath, std::move(lm));
    return &res.first->second;
}

// ---------------------------------------------------------------------------
// Scene slot claim: REUSE a freed loader-owned slot first (Scene::recycle bumps
// the generation), else append. Keeps the scene from growing across reloads.
// ---------------------------------------------------------------------------
uint32_t LevelDocWorld::claimSlot(Scene& scene, const Entity& e) {
    if (!m_freeSlots.empty()) {
        const uint32_t idx = m_freeSlots.back();
        m_freeSlots.pop_back();
        scene.recycle(idx, e);
        m_liveSlots.push_back(idx);
        return idx;
    }
    const uint32_t idx = scene.add(e);
    m_liveSlots.push_back(idx);
    return idx;
}

// ---------------------------------------------------------------------------
// Brush spawn — the SAME engine objects the editor host builds (prims mesh with
// the brush's surface material, plus a static Jolt body: AABB box for Box, a
// static mesh for Ramp so the slope is walkable).
// ---------------------------------------------------------------------------
void LevelDocWorld::spawnBrush(const x3::editor::BlockoutBrush& b, Scene& scene,
                               x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics) {
    // Box / Ramp / Cylinder / Stairs — one shared type table (prims::brushTypeOf), so an
    // authored level always spawns in-game as exactly what the editor drew.
    const auto type = x3::prims::brushTypeOf(b.type);
    x3::prims::PrimMesh pm = x3::prims::buildBrushMesh(type, b.size);

    Entity e;
    e.mesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                               pm.index.data(), (uint32_t)pm.index.size());
    float matTint[3] = { 1, 1, 1 };
    e.tex = x3::rhi::TextureHandle{ resolveMaterial(b.material, device, matTint) };
    e.baseColor[0] = b.tint[0]*matTint[0]; e.baseColor[1] = b.tint[1]*matTint[1];
    e.baseColor[2] = b.tint[2]*matTint[2]; e.baseColor[3] = 1.0f;
    e.tag = (uint32_t)Tag::Static;
    objMatrix(b.pos, b.yaw, b.pitch, b.roll, e.transform);
    Built rec; rec.slot = claimSlot(scene, e); rec.ownsMesh = true;
    m_builtObjects.push_back(rec);
    ++m_brushEntities;

    if (b.collide) {
        using x3::editor::rot3Enabled;
        const bool rotated = rot3Enabled() &&
            (std::fabs(b.yaw) > 1e-6f || std::fabs(b.pitch) > 1e-6f ||
             std::fabs(b.roll) > 1e-6f);
        x3::phys::Vec3 pos{ b.pos[0], b.pos[1], b.pos[2] };
        x3::phys::BodyId bid{};
        if (type == x3::prims::BrushType::Box) {
            x3::phys::Vec3 he{ b.size[0]*0.5f, b.size[1]*0.5f, b.size[2]*0.5f };
            bid = physics.addBox(he, pos, 0.0f, x3::phys::Layer::Static);
            // 11.0: orient the box body to the brush (mirrors EditorHost::spawnBrush;
            // X3_EDITOR_ROT3=0 restores the old axis-aligned bodies exactly).
            if (bid.id && rotated) {
                float q[4];
                x3::editor::yprToQuat(b.yaw, b.pitch, b.roll, q);
                physics.setBodyRotation(bid, q);
            }
        } else {
            // Ramp / Cylinder / Stairs: a static MESH of the brush's own triangles (an AABB
            // would make the slope, the round wall and the treads all read as one block).
            // 11.0: rotate the local verts by the brush orientation before translating.
            std::vector<float> cv = pm.cverts;   // local -> world
            float R[9];
            if (rotated) x3::editor::rotYPR(b.yaw, b.pitch, b.roll, R);
            for (size_t i = 0; i + 2 < cv.size(); i += 3) {
                float x = cv[i+0], y = cv[i+1], z = cv[i+2];
                if (rotated) {
                    const float wx = R[0]*x + R[3]*y + R[6]*z;
                    const float wy = R[1]*x + R[4]*y + R[7]*z;
                    const float wz = R[2]*x + R[5]*y + R[8]*z;
                    x = wx; y = wy; z = wz;
                }
                cv[i+0] = x + b.pos[0]; cv[i+1] = y + b.pos[1]; cv[i+2] = z + b.pos[2];
            }
            bid = physics.addStaticMesh(cv.data(), (uint32_t)(cv.size()/3),
                                        pm.cindex.data(), (uint32_t)pm.cindex.size());
        }
        if (bid.id) { m_bodies.push_back(bid.id); ++m_bodyCount; }
    }
}

// ---------------------------------------------------------------------------
// Entity spawn: lights -> PointLight list; triggers -> TriggerSystem zones (with
// the script hook recorded); model entities -> GLB drawable instances (or a
// graybox marker if the GLB is absent); everything else -> a small tinted
// graybox marker so an authored doc always visualizes.
// ---------------------------------------------------------------------------
void LevelDocWorld::spawnEntity(uint32_t entityIdx, const x3::editor::EditorEntity& e,
                                Scene& scene, x3::rhi::IRenderDevice& device,
                                x3::phys::IPhysicsWorld& physics) {
    (void)physics;
    if (e.type == "light") {
        // tint = the light's linear RGB; scale = its BRIGHTNESS dial (the device
        // expects color pre-multiplied by intensity — HDR-friendly) and stretches
        // the range with it so a brighter light also reaches further.
        x3::rhi::PointLight pl;
        pl.pos[0] = e.pos[0]; pl.pos[1] = e.pos[1]; pl.pos[2] = e.pos[2];
        const float intensity = std::max(0.25f, e.scale) * 3.0f;
        pl.color[0] = e.tint[0] * intensity;
        pl.color[1] = e.tint[1] * intensity;
        pl.color[2] = e.tint[2] * intensity;
        pl.range = std::min(40.0f, std::max(3.0f, 4.0f + e.scale * 5.0f));
        m_lights.push_back(pl);
        return;
    }
    if (e.type == "trigger") {
        // Zone extents: explicit size[] when authored, else `scale` as a uniform
        // full extent (min 0.5 m so a default trigger is enterable).
        float ex = e.size[0] > 0.0f ? e.size[0] : std::max(0.5f, e.scale);
        float ey = e.size[1] > 0.0f ? e.size[1] : std::max(0.5f, e.scale);
        float ez = e.size[2] > 0.0f ? e.size[2] : std::max(0.5f, e.scale);
        x3::phys::Vec3 mn{ e.pos[0]-ex*0.5f, e.pos[1]-ey*0.5f, e.pos[2]-ez*0.5f };
        x3::phys::Vec3 mx{ e.pos[0]+ex*0.5f, e.pos[1]+ey*0.5f, e.pos[2]+ez*0.5f };
        m_triggers.add(mn, mx, /*id*/entityIdx, /*enabled*/true);
        if (!e.script.empty()) m_triggerScripts.emplace_back(entityIdx, e.script);
        return;
    }
    if (e.type == "portal" && x3::editor::portalsEnabled()) {
        // 11.0 PORTAL (see the LEVELDOC PORTAL CONTRACT in editor.h): spawn a
        // NON-COLLIDING oriented marker slab (W x H x thickness) so a hand- or
        // generator-placed portal is visible in-game exactly where and how it
        // faces. Consumers (the tunnel system) read pos/yaw/pitch/roll/size and
        // the `script` link id from the doc — this marker implies nothing else.
        // X3_EDITOR_PORTAL=0 falls through to the generic marker (prior behaviour).
        const float w = e.size[0] > 0 ? e.size[0] : 3.0f;
        const float h = e.size[1] > 0 ? e.size[1] : 3.0f;
        const float t = e.size[2] > 0 ? e.size[2] : 0.4f;
        const float slab[3] = { w, h, t };
        x3::prims::PrimMesh pm =
            x3::prims::buildBrushMesh(x3::prims::BrushType::Box, slab);
        Entity se;
        se.mesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                    pm.index.data(), (uint32_t)pm.index.size());
        se.baseColor[0] = e.tint[0]; se.baseColor[1] = e.tint[1];
        se.baseColor[2] = e.tint[2]; se.baseColor[3] = 1.0f;
        for (int i = 0; i < 4; ++i) se.emissive[i] = e.emissive[i];
        se.tag = (uint32_t)Tag::Prop;
        objMatrix(e.pos, e.yaw, e.pitch, e.roll, se.transform);
        Built rec; rec.slot = claimSlot(scene, se); rec.ownsMesh = true;
        m_builtObjects.push_back(rec);
        ++m_propEntities;
        return;
    }
    if (!e.model.empty()) {
        const LoadedModel* lm = loadModelCached(e.model, device);
        if (lm && lm->ok) {
            float obj[16];
            objMatrix(e.pos, e.yaw, e.pitch, e.roll, obj, e.scale);
            for (const auto& d : lm->drawables) {
                if (!d.meshId) continue;
                Entity se;
                se.mesh = x3::rhi::MeshHandle{ d.meshId };
                se.tex  = x3::rhi::TextureHandle{ d.baseColorTexId };
                for (int i = 0; i < 4; ++i) se.baseColor[i] = d.baseColorFactor[i];
                // Canon SELF-LIT glow: forward the leveldoc entity's optional
                // emissive { r,g,b,strength } onto every drawable so a fusion
                // core / solar cell face / neon strip glows independent of light
                // (default {0,0,0,0} == no glow, unchanged for existing props).
                for (int i = 0; i < 4; ++i) se.emissive[i] = e.emissive[i];
                se.tag = (uint32_t)Tag::Prop;
                x3::asset::mulMat4(obj, d.nodeTransform, se.transform);
                Built rec; rec.slot = claimSlot(scene, se);
                rec.ownsMesh = false;            // mesh belongs to the model cache
                m_builtObjects.push_back(rec);
                ++m_propEntities;
            }
            return;
        }
        // fall through -> graybox marker stand-in for the missing GLB
    }
    // Generic marker (prop/enemy/item/npc/spawn/unknown): a tinted graybox box.
    x3::prims::PrimMesh pm = x3::prims::makeBox(0.4f*e.scale, 0.45f*e.scale, 0.4f*e.scale,
                                                0.0f, 0.0f, 0.0f, 1.0f);
    Entity se;
    se.mesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                pm.index.data(), (uint32_t)pm.index.size());
    se.baseColor[0] = e.tint[0]; se.baseColor[1] = e.tint[1];
    se.baseColor[2] = e.tint[2]; se.baseColor[3] = 1.0f;
    se.tag = (uint32_t)Tag::Prop;
    objMatrix(e.pos, e.yaw, e.pitch, e.roll, se.transform);
    Built rec; rec.slot = claimSlot(scene, se); rec.ownsMesh = true;
    m_builtObjects.push_back(rec);
    ++m_propEntities;
}

// ---------------------------------------------------------------------------
void LevelDocWorld::buildAll(Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics) {
    for (const auto& b : m_doc.brushes) spawnBrush(b, scene, device, physics);
    for (uint32_t i = 0; i < (uint32_t)m_doc.entities.size(); ++i)
        spawnEntity(i, m_doc.entities[i], scene, device, physics);
    m_built = true;
    x3::logInfo("[leveldoc] built '" + m_doc.name + "': " +
                std::to_string(m_brushEntities) + " brush entities, " +
                std::to_string(m_propEntities) + " prop entities, " +
                std::to_string(m_bodyCount) + " bodies, " +
                std::to_string(m_lights.size()) + " lights, " +
                std::to_string(m_triggers.count()) + " triggers");
}

// ---------------------------------------------------------------------------
// Teardown — destroys ONLY the doc-built objects this loader spawned: loader-
// owned meshes are destroyed, every owned scene slot is hidden + queued for
// recycle, every owned Jolt body is removed. Session caches (material textures,
// GLB models) survive (freed in shutdown).
// ---------------------------------------------------------------------------
void LevelDocWorld::teardownBuilt(Scene& scene, x3::rhi::IRenderDevice& device,
                                  x3::phys::IPhysicsWorld& physics) {
    for (const Built& rec : m_builtObjects) {
        if (rec.slot >= scene.size()) continue;
        Entity& e = scene.get(rec.slot);
        if (rec.ownsMesh && e.mesh.valid()) device.destroyMesh(e.mesh);
        e.mesh = x3::rhi::MeshHandle{};
        e.visible = false;
        m_freeSlots.push_back(rec.slot);
    }
    m_builtObjects.clear();
    m_liveSlots.clear();
    for (uint32_t bid : m_bodies) physics.removeBody(x3::phys::BodyId{ bid });
    m_bodies.clear();
    m_brushEntities = m_propEntities = m_bodyCount = 0;
    m_lights.clear();
    m_triggers = TriggerSystem{};
    m_triggerScripts.clear();
    m_built = false;
}

// ---------------------------------------------------------------------------
bool LevelDocWorld::buildFromDoc(const x3::editor::LevelDoc& doc, Scene& scene,
                                 x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& physics) {
    if (m_built) teardownBuilt(scene, device, physics);
    m_doc = doc;
    buildAll(scene, device, physics);
    return true;
}

bool LevelDocWorld::loadFromFile(const std::string& path, Scene& scene,
                                 x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& physics) {
    x3::editor::LevelDoc tmp;
    if (!tmp.loadJson(path)) {
        x3::logWarn("[leveldoc] load failed: " + path);
        return false;
    }
    if (m_built) teardownBuilt(scene, device, physics);
    m_doc = tmp;
    m_path = path;
    m_mtime = fileMtime(path);
    buildAll(scene, device, physics);
    return true;
}

bool LevelDocWorld::reloadNow(Scene& scene, x3::rhi::IRenderDevice& device,
                              x3::phys::IPhysicsWorld& physics) {
    if (m_path.empty()) return false;
    x3::editor::LevelDoc tmp;
    if (!tmp.loadJson(m_path)) {
        // Unparseable (possibly a mid-write save) — keep the OLD world up; the
        // next mtime change retries.
        x3::logWarn("[leveldoc] reload parse failed; keeping the current world: " + m_path);
        m_mtime = fileMtime(m_path);
        return false;
    }
    teardownBuilt(scene, device, physics);
    m_doc = tmp;
    m_mtime = fileMtime(m_path);
    buildAll(scene, device, physics);
    x3::logInfo("[leveldoc] HOT-RELOADED " + m_path);
    return true;
}

bool LevelDocWorld::pollHotReload(double nowSeconds, Scene& scene,
                                  x3::rhi::IRenderDevice& device,
                                  x3::phys::IPhysicsWorld& physics) {
    constexpr double kPollInterval = 0.5;   // seconds between stats
    if (m_path.empty() || !m_built) return false;
    if (nowSeconds < m_nextPoll) return false;
    m_nextPoll = nowSeconds + kPollInterval;
    const int64_t t = fileMtime(m_path);
    if (t == 0 || t == m_mtime) return false;
    return reloadNow(scene, device, physics);
}

void LevelDocWorld::shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics) {
    if (m_built) teardownBuilt(scene, device, physics);
    for (uint32_t& t : m_matTex) {
        if (t) device.destroyTexture(x3::rhi::TextureHandle{ t });
        t = 0;
    }
    if (m_modelLoader) {
        for (auto& kv : m_modelCache)
            if (kv.second.model.ok) m_modelLoader->unload(kv.second.model);
    }
    m_modelCache.clear();
    m_modelLoader.reset();
    m_modelAssets.reset();
    m_freeSlots.clear();
    m_path.clear();
}

// ---------------------------------------------------------------------------
std::vector<uint32_t> LevelDocWorld::updateTriggers(const x3::phys::Vec3& playerPos) {
    std::vector<uint32_t> fired = m_triggers.update(playerPos);
    for (uint32_t id : fired) {
        const std::string s = triggerScript(id);
        x3::logInfo("[leveldoc] trigger_enter id=" + std::to_string(id) +
                    (s.empty() ? std::string(" (no script)")
                               : " script='" + s + "'"));
    }
    return fired;
}

std::string LevelDocWorld::triggerScript(uint32_t entityIdx) const {
    for (const auto& p : m_triggerScripts)
        if (p.first == entityIdx) return p.second;
    return {};
}

uint32_t LevelDocWorld::selectLights(float eyeX, float eyeY, float eyeZ,
                                     std::vector<x3::rhi::PointLight>& out,
                                     uint32_t maxLights) const {
    if (m_lights.empty() || maxLights == 0) return 0;
    std::vector<uint32_t> order(m_lights.size());
    for (uint32_t i = 0; i < (uint32_t)order.size(); ++i) order[i] = i;
    auto d2 = [&](uint32_t i) {
        const auto& l = m_lights[i];
        const float dx = l.pos[0]-eyeX, dy = l.pos[1]-eyeY, dz = l.pos[2]-eyeZ;
        return dx*dx + dy*dy + dz*dz;
    };
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b){ return d2(a) < d2(b); });
    uint32_t n = 0;
    for (uint32_t i : order) {
        if (n >= maxLights) break;
        out.push_back(m_lights[i]);
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// The sample doc (loader proofs + the `--world fromdoc` seed when no file exists):
// a small graybox ROOM — deck floor, three paneled walls, a ramp up to a hazard
// pillar ledge, two warm lights, a scripted trigger at the ramp foot.
// ---------------------------------------------------------------------------
x3::editor::LevelDoc makeSampleLevelDoc() {
    using x3::editor::LevelDoc;
    using x3::editor::BlockoutBrush;
    using x3::editor::EditorEntity;
    LevelDoc doc;
    doc.name = "loader_sample_room";
    doc.biome = "facility";
    doc.playerStart[0] = 0.0f; doc.playerStart[1] = 0.6f; doc.playerStart[2] = 4.0f;
    auto brush = [&](const char* name, uint32_t type, float px, float py, float pz,
                     float sx, float sy, float sz, const char* mat) {
        BlockoutBrush b; b.name = name; b.type = type;
        b.pos[0]=px; b.pos[1]=py; b.pos[2]=pz;
        b.size[0]=sx; b.size[1]=sy; b.size[2]=sz;
        b.material = mat; b.collide = true;
        doc.brushes.push_back(b);
    };
    brush("floor",   0,  0.0f, -0.25f,  0.0f, 14.0f, 0.5f, 14.0f, "floor");
    brush("wall_n",  0,  0.0f,  1.75f, -7.0f, 14.0f, 4.0f,  0.4f, "wall");
    brush("wall_w",  0, -7.0f,  1.75f,  0.0f,  0.4f, 4.0f, 14.0f, "wall_blue");
    brush("wall_e",  0,  7.0f,  1.75f,  0.0f,  0.4f, 4.0f, 14.0f, "wall");
    brush("ledge",   0,  4.0f,  0.75f, -4.5f,  5.0f, 1.5f,  4.0f, "clean");
    brush("ramp",    1,  0.0f,  0.75f, -4.5f,  3.0f, 1.5f,  4.0f, "concrete");
    brush("pillar",  0,  4.5f,  2.5f,  -5.0f,  1.0f, 2.0f,  1.0f, "hazard");
    { EditorEntity e; e.name = "key_light";  e.type = "light"; e.pos[0]=0;  e.pos[1]=3.2f; e.pos[2]=0;
      e.tint[0]=1.0f; e.tint[1]=0.92f; e.tint[2]=0.78f; e.scale = 2.2f; doc.entities.push_back(e); }
    { EditorEntity e; e.name = "ledge_light"; e.type = "light"; e.pos[0]=4.0f; e.pos[1]=3.4f; e.pos[2]=-4.5f;
      e.tint[0]=0.65f; e.tint[1]=0.85f; e.tint[2]=1.0f; e.scale = 1.6f; doc.entities.push_back(e); }
    { EditorEntity e; e.name = "ramp_foot"; e.type = "trigger"; e.pos[0]=0; e.pos[1]=0.8f; e.pos[2]=-1.5f;
      e.size[0]=3.0f; e.size[1]=2.0f; e.size[2]=1.5f; e.script = "ramp_reached"; doc.entities.push_back(e); }
    { EditorEntity e; e.name = "crate_marker"; e.type = "prop"; e.pos[0]=-3.5f; e.pos[1]=0.45f; e.pos[2]=-2.0f;
      e.tint[0]=0.8f; e.tint[1]=0.55f; e.tint[2]=0.2f; doc.entities.push_back(e); }
    return doc;
}

const char* defaultLevelDocPath() { return "build/proof/architect_level.json"; }

// ===========================================================================
// Headless self-test (--test-loader). L0-L10. No window / Vulkan.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[loader-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[loader-test] FAIL ") + name); }
}
bool feq(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }

// Counting device: ledger of every mesh/texture create + destroy so the test can
// assert the reload + shutdown paths balance to zero (the no-leak gate).
class CountingDevice : public HeadlessRenderDevice {
public:
    uint32_t meshCreated = 0, meshDestroyed = 0, texCreated = 0, texDestroyed = 0;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t vc,
                                   const uint32_t* idx, uint32_t ic) override {
        ++meshCreated;
        return HeadlessRenderDevice::createMesh(v, vc, idx, ic);
    }
    void destroyMesh(x3::rhi::MeshHandle h) override {
        if (h.valid()) ++meshDestroyed;
        HeadlessRenderDevice::destroyMesh(h);
    }
    x3::rhi::TextureHandle createTexture(const void* px, uint32_t w, uint32_t h,
                                         bool srgb) override {
        ++texCreated;
        return HeadlessRenderDevice::createTexture(px, w, h, srgb);
    }
    void destroyTexture(x3::rhi::TextureHandle h) override {
        if (h.valid()) ++texDestroyed;
        HeadlessRenderDevice::destroyTexture(h);
    }
    int liveMeshes() const { return (int)meshCreated - (int)meshDestroyed; }
    int liveTextures() const { return (int)texCreated - (int)texDestroyed; }
};

} // namespace

bool runLevelDocLoaderSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("running LevelDoc data-driven loader self-test (L0-L10)...");

    // ---- The authored doc: 4 brushes (one hazard material, one no-collide ramp),
    // 1 graybox item marker, 1 missing-GLB model entity (fallback marker), 2 lights,
    // 1 scripted trigger. ----
    x3::editor::LevelDoc doc;
    doc.name = "loader_test"; doc.biome = "facility";
    doc.playerStart[0] = 1.0f; doc.playerStart[1] = 0.5f; doc.playerStart[2] = 2.0f;
    auto brush = [&](const char* n, uint32_t type, float px, float py, float pz,
                     float sx, float sy, float sz, const char* mat, bool collide) {
        x3::editor::BlockoutBrush b; b.name = n; b.type = type;
        b.pos[0]=px; b.pos[1]=py; b.pos[2]=pz;
        b.size[0]=sx; b.size[1]=sy; b.size[2]=sz;
        b.material = mat; b.collide = collide;
        b.tint[0] = b.tint[1] = b.tint[2] = 1.0f;   // neutral so L2's material-tint
                                                    // assert sees the PURE mat tint
        doc.brushes.push_back(b);
    };
    brush("floor",  0, 0, -0.25f, 0, 10, 0.5f, 10, "floor",  true);
    brush("wall",   0, 0,  1.5f, -5, 10, 3,    0.4f, "wall", true);
    brush("pillar", 0, 3,  1.0f,  3, 1,  2,    1,   "hazard", true);
    brush("ramp",   1, -2, 0.75f, 0, 3,  1.5f, 4,   "",      false);
    // 11.0: a THREE-AXIS-rotated, colliding, GENERATOR-stamped box (index 4) —
    // the rotation + provenance round-trip subject for L7.
    brush("tilt",   0, 0,  3.0f,  0, 2,  1,    1,   "",      true);
    doc.brushes[4].yaw = 0.52359879f;    // 30 deg
    doc.brushes[4].pitch = 0.78539816f;  // 45 deg
    doc.brushes[4].roll = -0.26179939f;  // -15 deg
    doc.brushes[4].gen = "test:tilt:0";
    { x3::editor::EditorEntity e; e.name="item"; e.type="item";
      e.pos[0]=2; e.pos[1]=0.5f; e.pos[2]=1; e.yaw=0.5f;
      e.tint[0]=1.0f; e.tint[1]=0.82f; e.tint[2]=0.3f; doc.entities.push_back(e); }
    { x3::editor::EditorEntity e; e.name="ghost"; e.type="model";
      e.model="__no_such_dir__/missing.glb"; e.pos[0]=-3; e.pos[2]=2; doc.entities.push_back(e); }
    { x3::editor::EditorEntity e; e.name="lampA"; e.type="light";
      e.pos[0]=0; e.pos[1]=2.5f; e.pos[2]=0; e.tint[0]=1; e.tint[1]=0.9f; e.tint[2]=0.7f;
      e.scale=2.0f; doc.entities.push_back(e); }
    { x3::editor::EditorEntity e; e.name="lampB"; e.type="light";
      e.pos[0]=4; e.pos[1]=2.5f; e.pos[2]=4; doc.entities.push_back(e); }
    { x3::editor::EditorEntity e; e.name="tz"; e.type="trigger";
      e.pos[0]=3; e.pos[1]=1; e.pos[2]=3; e.size[0]=2; e.size[1]=2; e.size[2]=2;
      e.script="secret_door"; doc.entities.push_back(e); }
    // 11.0: a hand-orientable PORTAL with a link id + generator provenance —
    // the L8 subject (spawns an oriented non-colliding marker slab).
    { x3::editor::EditorEntity e; e.name="gate_a"; e.type="portal";
      e.pos[0]=5; e.pos[1]=2; e.pos[2]=1;
      e.yaw=0.9f; e.pitch=0.25f; e.roll=-0.1f;
      e.size[0]=3; e.size[1]=3; e.size[2]=0.4f;
      e.tint[0]=0.3f; e.tint[1]=0.9f; e.tint[2]=1.0f;
      e.script="tunnel_a_west"; e.gen="tunnel:portal:a_west";
      doc.entities.push_back(e); }

    // ---- L0: the EXTENDED format (script + size) survives the JSON round-trip. ----
    {
        x3::editor::LevelDoc rt;
        bool parsed = rt.fromJson(doc.toJson());
        bool same = parsed && rt.entities.size() == doc.entities.size() &&
                    rt.brushes.size() == doc.brushes.size();
        const x3::editor::EditorEntity* tz = nullptr;
        for (const auto& e : rt.entities) if (e.name == "tz") tz = &e;
        bool trig = tz && tz->type == "trigger" && tz->script == "secret_door" &&
                    feq(tz->size[0], 2.0f) && feq(tz->size[1], 2.0f) && feq(tz->size[2], 2.0f);
        check(same && trig, "L0 extended LevelDoc (script + trigger size) JSON round-trip");
    }

    // ---- Save to a real file, then LOAD through the real loader. ----
    const std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".")
                             + "/x3_loader_test.json";
    CountingDevice device;
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    Scene scene;
    LevelDocWorld world;

    // ---- L1: file save -> loadFromFile builds the world (counts + playerStart). ----
    {
        bool saved = doc.saveJson(path);
        bool loaded = saved && world.loadFromFile(path, scene, device, *physics);
        float ps[3] = { 0, 0, 0 };
        if (loaded) world.playerStart(ps);
        // 5 brush entities; 3 prop entities (item marker + missing-GLB fallback
        // marker + portal marker slab); 4 bodies (the ramp is collide=false and the
        // PORTAL adds NO body); 2 lights; 1 trigger.
        check(loaded && world.built() &&
              world.brushEntityCount() == 5 && world.propEntityCount() == 3 &&
              world.bodyCount() == 4 && world.lightCount() == 2 &&
              world.triggerCount() == 1 &&
              feq(ps[0], 1.0f) && feq(ps[1], 0.5f) && feq(ps[2], 2.0f),
              "L1 save -> loadFromFile builds 5 brushes / 3 props / 4 bodies / 2 lights / 1 trigger");
    }

    // ---- L2: transforms + material tint applied to the built scene entities. ----
    {
        // Slot order: brushes spawn first in doc order -> slot[2] is "pillar".
        bool ok = world.ownedSlots().size() >= 4;
        if (ok) {
            const Entity& pillar = scene.get(world.ownedSlots()[2]);
            // Transform translation == authored pos.
            ok = feq(pillar.transform[12], 3.0f) && feq(pillar.transform[13], 1.0f) &&
                 feq(pillar.transform[14], 3.0f);
            // Hazard material tint (0.82, 0.20, 0.18) multiplied into baseColor.
            ok = ok && feq(pillar.baseColor[0], 0.82f, 0.02f) &&
                 feq(pillar.baseColor[1], 0.20f, 0.02f) && pillar.visible && pillar.mesh.valid();
        }
        check(ok, "L2 brush transform translation + hazard material tint applied");
    }

    // ---- L3: the trigger fires ONCE on entry, carries its script hook. ----
    {
        auto none  = world.updateTriggers(x3::phys::Vec3{ 0, 1, 0 });     // outside
        auto fired = world.updateTriggers(x3::phys::Vec3{ 3, 1, 3 });     // inside
        auto again = world.updateTriggers(x3::phys::Vec3{ 3, 1, 3 });     // latched
        bool ok = none.empty() && fired.size() == 1 && again.empty() &&
                  world.triggerScript(fired.empty() ? 0u : fired[0]) == "secret_door";
        check(ok, "L3 trigger zone fires once with script hook 'secret_door'");
    }

    // ---- L4: MODIFY the doc + reload -> the delta applies in place. ----
    const uint32_t slotsBefore  = world.totalSlotsClaimed();
    const int meshLiveBefore    = device.liveMeshes();
    {
        x3::editor::LevelDoc mod = doc;
        mod.brushes.erase(mod.brushes.begin() + 1);          // drop "wall" (a collide body)
        mod.brushes[1].pos[0] = 6.0f;                        // move "pillar" x 3 -> 6
        { x3::editor::EditorEntity e; e.name="lampC"; e.type="light";
          e.pos[0]=-4; e.pos[1]=2; e.pos[2]=-4; mod.entities.push_back(e); }
        bool saved = mod.saveJson(path);
        bool reloaded = saved && world.reloadNow(scene, device, *physics);
        // New shape: 4 brushes / 3 bodies / 3 lights; pillar (now slot order [1]) at x=6.
        bool counts = reloaded && world.brushEntityCount() == 4 &&
                      world.bodyCount() == 3 && world.lightCount() == 3 &&
                      world.triggerCount() == 1;
        bool moved = counts && world.ownedSlots().size() >= 2 &&
                     feq(scene.get(world.ownedSlots()[1]).transform[12], 6.0f);
        check(counts && moved, "L4 modify + reload applies the delta (counts + moved transform)");
    }

    // ---- L5: reload REUSED the scene slots (no growth) + the mesh ledger shrank
    // by exactly the dropped brush (old objects gone, nothing duplicated). ----
    {
        const bool slotsReused = world.totalSlotsClaimed() == slotsBefore;
        // One fewer owned mesh is alive (before: 5 brushes + 2 markers + 1 portal
        // slab = 8 owned meshes; after the wall drop: 7).
        const bool ledger = device.liveMeshes() == meshLiveBefore - 1;
        check(slotsReused && ledger,
              "L5 reload recycles scene slots (no growth) + mesh ledger shrinks by the delta");
    }

    // ---- L6: mtime hot-reload poll picks up an on-disk change. ----
    {
        x3::editor::LevelDoc mod2 = doc;                     // back to the 5-brush shape
        bool saved = mod2.saveJson(path);
        // Poll at t=10 (past any interval); mtime changed -> reload applies.
        bool hot = saved && world.pollHotReload(10.0, scene, device, *physics);
        // Immediately polling again finds no change.
        bool quiet = !world.pollHotReload(11.0, scene, device, *physics);
        check(hot && quiet && world.brushEntityCount() == 5 && world.bodyCount() == 4,
              "L6 mtime poll hot-reloads the changed file once");
    }

    // ---- L7 (11.0): the ROTATED brush survived file save -> load BIT-exactly and
    // its built scene transform carries the FULL three-axis rotation. The negative
    // control: the yaw-only matrix must NOT match — this check fails outright if
    // the loader (or the JSON) drops pitch/roll. ----
    {
        const x3::editor::LevelDoc& d = world.doc();
        bool docExact = d.brushes.size() == 5 &&
                        d.brushes[4].yaw   == 0.52359879f &&
                        d.brushes[4].pitch == 0.78539816f &&
                        d.brushes[4].roll  == -0.26179939f &&
                        d.brushes[4].gen   == "test:tilt:0" &&
                        d.brushes[4].genEdited == false;
        bool xfOk = false, notYawOnly = false;
        if (world.ownedSlots().size() >= 5) {
            const Entity& tilt = scene.get(world.ownedSlots()[4]);
            float R[9];
            x3::editor::rotYPR(0.52359879f, 0.78539816f, -0.26179939f, R);
            xfOk = true;
            const int map[9] = { 0,1,2, 4,5,6, 8,9,10 };   // 4x4 col-major -> 3x3
            for (int k = 0; k < 9; ++k)
                xfOk = xfOk && feq(tilt.transform[map[k]], R[k], 1e-6f);
            xfOk = xfOk && feq(tilt.transform[12], 0.0f) &&
                   feq(tilt.transform[13], 3.0f) && feq(tilt.transform[14], 0.0f);
            // Negative control: a yaw-only build of the same brush differs.
            float Ry[9];
            x3::editor::rotYPR(0.52359879f, 0.0f, 0.0f, Ry);
            for (int k = 0; k < 9; ++k)
                if (std::fabs(Ry[k] - tilt.transform[map[k]]) > 0.01f) notYawOnly = true;
        }
        check(docExact && xfOk && notYawOnly,
              "L7 rotated brush: bit-exact doc round-trip + full 3-axis scene transform");
    }

    // ---- L8 (11.0): the PORTAL spawned as an ORIENTED marker slab (no body), and
    // its link id + provenance survived the file round trip. ----
    {
        const x3::editor::LevelDoc& d = world.doc();
        const x3::editor::EditorEntity* gate = nullptr;
        for (const auto& e : d.entities) if (e.name == "gate_a") gate = &e;
        bool docOk = gate && gate->type == "portal" &&
                     gate->yaw == 0.9f && gate->pitch == 0.25f && gate->roll == -0.1f &&
                     gate->script == "tunnel_a_west" &&
                     gate->gen == "tunnel:portal:a_west" && !gate->genEdited;
        // Spawn order: 5 brushes (slots [0..4]), item marker [5], ghost fallback
        // [6], portal slab [7]. Lights/triggers claim no slots.
        bool xfOk = false;
        if (world.ownedSlots().size() == 8) {
            const Entity& slab = scene.get(world.ownedSlots()[7]);
            float R[9];
            x3::editor::rotYPR(0.9f, 0.25f, -0.1f, R);
            xfOk = feq(slab.transform[12], 5.0f) && feq(slab.transform[13], 2.0f) &&
                   feq(slab.transform[14], 1.0f);
            const int map[9] = { 0,1,2, 4,5,6, 8,9,10 };
            for (int k = 0; k < 9; ++k)
                xfOk = xfOk && feq(slab.transform[map[k]], R[k], 1e-6f);
        }
        // The portal must add NO collision body (bodies are the 4 colliding brushes).
        check(docOk && xfOk && world.bodyCount() == 4 && world.propEntityCount() == 3,
              "L8 portal: oriented marker slab, link id + provenance kept, NO body");
    }

    // ---- L9 (11.0 handoff): a GENERATED doc — authored in a loop the way a
    // generator would emit it, every object gen-stamped and three-axis-rotated —
    // survives file save -> load with NOTHING dropped (field-for-field, floats
    // bit-exact), builds, and tears down leak-free. The comparator itself is
    // negative-controlled: a single perturbed float must make it fail. ----
    {
        x3::editor::LevelDoc gdoc;
        gdoc.name = "gen_ring"; gdoc.biome = "facility";
        for (int i = 0; i < 12; ++i) {
            const float a = (float)i * 0.5235988f;           // 30-deg arc steps
            x3::editor::BlockoutBrush b;
            b.name = "seg_" + std::to_string(i);
            b.type = 0u;
            b.pos[0] = 20.0f * std::cos(a); b.pos[1] = 0.5f + 0.3f * (float)i;
            b.pos[2] = 20.0f * std::sin(a);
            b.size[0] = 4; b.size[1] = 0.5f; b.size[2] = 3;
            b.yaw = a; b.pitch = 0.08f * (float)(i % 3); b.roll = -0.05f * (float)(i % 2);
            b.gen = "gen:ring:seg:" + std::to_string(i);
            gdoc.brushes.push_back(b);
        }
        for (int i = 0; i < 3; ++i) {
            x3::editor::EditorEntity e;
            e.name = "gen_gate_" + std::to_string(i); e.type = "portal";
            e.pos[0] = (float)(i * 7); e.pos[1] = 2; e.pos[2] = -10;
            e.yaw = 0.3f * (float)i; e.pitch = 0.1f; e.roll = 0.0f;
            e.size[0] = 3; e.size[1] = 3; e.size[2] = 0.4f;
            e.script = "gen_link_" + std::to_string(i);
            e.gen = "gen:ring:gate:" + std::to_string(i);
            gdoc.entities.push_back(e);
        }
        const std::string gpath = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".")
                                  + "/x3_loader_gen_test.json";
        auto docEquals = [](const x3::editor::LevelDoc& a, const x3::editor::LevelDoc& b) {
            if (a.name != b.name || a.brushes.size() != b.brushes.size() ||
                a.entities.size() != b.entities.size()) return false;
            for (size_t i = 0; i < a.brushes.size(); ++i) {
                const auto& x = a.brushes[i]; const auto& y = b.brushes[i];
                if (x.name != y.name || x.type != y.type || x.material != y.material ||
                    x.collide != y.collide || x.gen != y.gen || x.genEdited != y.genEdited)
                    return false;
                for (int k = 0; k < 3; ++k)
                    if (x.pos[k] != y.pos[k] || x.size[k] != y.size[k] ||
                        x.tint[k] != y.tint[k]) return false;
                if (x.yaw != y.yaw || x.pitch != y.pitch || x.roll != y.roll) return false;
            }
            for (size_t i = 0; i < a.entities.size(); ++i) {
                const auto& x = a.entities[i]; const auto& y = b.entities[i];
                if (x.name != y.name || x.type != y.type || x.model != y.model ||
                    x.script != y.script || x.gen != y.gen || x.genEdited != y.genEdited)
                    return false;
                for (int k = 0; k < 3; ++k)
                    if (x.pos[k] != y.pos[k] || x.size[k] != y.size[k] ||
                        x.tint[k] != y.tint[k]) return false;
                if (x.yaw != y.yaw || x.pitch != y.pitch || x.roll != y.roll ||
                    x.scale != y.scale) return false;
            }
            return true;
        };
        x3::editor::LevelDoc rt;
        bool lossless = gdoc.saveJson(gpath) && rt.loadJson(gpath) && docEquals(gdoc, rt);
        // NEGATIVE CONTROL: perturb one float by one grid cell — must NOT compare equal.
        x3::editor::LevelDoc bad = rt;
        bad.brushes[7].pitch += 0.5f;
        bool comparatorBites = !docEquals(gdoc, bad);
        // Build it, then tear it down: the device ledgers must return exactly to
        // their pre-build level (the generated-doc no-leak gate).
        const int meshesBefore = device.liveMeshes();
        LevelDocWorld gworld;
        bool built = gworld.loadFromFile(gpath, scene, device, *physics) &&
                     gworld.brushEntityCount() == 12 && gworld.propEntityCount() == 3 &&
                     gworld.bodyCount() == 12;
        gworld.shutdown(scene, device, *physics);
        bool balanced = device.liveMeshes() == meshesBefore;
        check(lossless && comparatorBites && built && balanced,
              "L9 GENERATED doc: lossless save->load (bit-exact), builds, tears down leak-free");
    }

    // ---- L10: shutdown balances every ledger to zero (the no-leak gate). ----
    {
        world.shutdown(scene, device, *physics);
        const bool meshes   = device.liveMeshes() == 0;
        const bool textures = device.liveTextures() == 0;
        const bool bodies   = world.bodyCount() == 0;
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "[loader-test] ledgers after shutdown: meshes %u/%u textures %u/%u",
            device.meshCreated, device.meshDestroyed, device.texCreated, device.texDestroyed);
        x3::logInfo(msg);
        check(meshes && textures && bodies && !world.built(),
              "L10 shutdown: mesh/texture/body ledgers balance to ZERO (no leaks)");
    }

    physics->shutdown();
    x3::logInfo(std::string("[loader-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
