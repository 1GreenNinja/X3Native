#pragma once
// LEVELDOC WORLD — the DATA-DRIVEN level loader for the EDITOR's LevelDoc JSON.
//
// This is the Level Architect payoff: the engine builds a PLAYABLE world from the
// exact JSON the native editor (app/editor, --editor / F8) SAVES — brushes (Box/
// Ramp + per-brush surface materials + Jolt collision), placed GLB props, point
// lights, trigger zones (with an optional per-trigger SCRIPT hook id, carried in
// the format so the Lua trigger chain composes later), and the player start —
// instead of hardcoded C++ geometry.
//
// RELATION TO level_loader.* (the CANONICAL loader): that system consumes the
// owner's LevelArchitect v2.project.json (a rooms[]+doors[] FACILITY graph with a
// doorway resolver + portal PVS). THIS system consumes the EDITOR's LevelDoc (a
// flat brushes[]+entities[] authoring doc). They are different abstractions on
// purpose; the LevelDoc is what --test-editor round-trips and what the in-engine
// editor edits, so the edit -> save -> reload loop closes HERE. Both loaders share
// the same engine objects (Scene entities + prims meshes + Jolt bodies).
//
// OWNERSHIP + HOT RELOAD: every object this loader builds is TRACKED (scene slot,
// whether the mesh is loader-owned or model-cache-owned, the Jolt body id). A
// reload tears down ONLY those objects (meshes destroyed, bodies removed, scene
// slots recycled for the rebuild — the scene never grows across reloads), then
// rebuilds from the new JSON in place; the player body is never touched so the
// position survives. Hot reload = an mtime poll (pollHotReload) or the console
// command `level_reload` (wired in main.cpp). Material textures + loaded GLBs are
// SESSION caches (kept across reloads, freed in shutdown) so a reload is cheap.
//
// Game/slice code only — engine/ stays pure (IRenderDevice / IPhysicsWorld /
// IModelLoader / Scene only).

#include "scene.h"
#include "trigger.h"
#include "editor/editor.h"               // LevelDoc (the on-disk format) + materials

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace x3::game {

class LevelDocWorld {
public:
    // Parse the LevelDoc JSON at `path` and build it into the live world. Returns
    // false (and builds nothing) on file/parse failure so the caller can fall back.
    // Remembers the path + mtime for hot reload.
    bool loadFromFile(const std::string& path, Scene& scene,
                      x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    // Build directly from an in-memory doc (the round-trip self-test path). Does
    // NOT arm file hot-reload (no path).
    bool buildFromDoc(const x3::editor::LevelDoc& doc, Scene& scene,
                      x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    // Force a reload from the remembered file NOW (console `level_reload`). Tears
    // down the doc-built objects (and ONLY those) and rebuilds from the new JSON.
    // Returns false if the file is missing/unparseable (the OLD world stays up).
    bool reloadNow(Scene& scene, x3::rhi::IRenderDevice& device,
                   x3::phys::IPhysicsWorld& physics);

    // Mtime hot-reload poll: cheap stat at most every `kPollInterval` seconds
    // (pass the caller's running clock in `nowSeconds`). Returns true iff a
    // changed file was detected AND the reload applied.
    bool pollHotReload(double nowSeconds, Scene& scene,
                       x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    // Tear down everything this loader built + owns (doc objects AND the session
    // caches: material textures, loaded GLB models). Safe to call twice.
    void shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics);

    // Per-frame: test the player position against the doc's trigger zones. Returns
    // the entity indices of any triggers that fired THIS call (each fires once).
    // Each fire is logged with its script hook id ("x3 trigger_enter" — the Lua
    // binding composes here later).
    std::vector<uint32_t> updateTriggers(const x3::phys::Vec3& playerPos);

    // ---- Queries (host + self-test) ----------------------------------------
    bool built() const { return m_built; }
    const x3::editor::LevelDoc& doc() const { return m_doc; }
    void playerStart(float out[3]) const {
        out[0] = m_doc.playerStart[0]; out[1] = m_doc.playerStart[1]; out[2] = m_doc.playerStart[2];
    }
    // Doc-built point lights (entities[] with type "light"). The host feeds these
    // to device.setPointLights each frame (capped; see selectLights).
    const std::vector<x3::rhi::PointLight>& lights() const { return m_lights; }
    // Append up to `maxLights` doc lights nearest `eye` into `out` (not cleared).
    uint32_t selectLights(float eyeX, float eyeY, float eyeZ,
                          std::vector<x3::rhi::PointLight>& out, uint32_t maxLights = 16) const;

    // Counts for the self-test / diagnostics.
    uint32_t brushEntityCount() const { return m_brushEntities; }   // live brush scene entities
    uint32_t propEntityCount()  const { return m_propEntities; }    // live prop/marker scene entities
    uint32_t bodyCount()        const { return m_bodyCount; }       // live Jolt bodies owned
    uint32_t lightCount()       const { return (uint32_t)m_lights.size(); }
    uint32_t triggerCount()     const { return m_triggers.count(); }
    // The script hook id of trigger entity index `entityIdx` ("" if none).
    std::string triggerScript(uint32_t entityIdx) const;
    // Scene slot ids currently owned (live) — the self-test asserts transforms.
    const std::vector<uint32_t>& ownedSlots() const { return m_liveSlots; }
    // How many scene slots this loader has EVER claimed (free + live). The
    // self-test asserts this does not grow across a same-size reload (slot reuse).
    uint32_t totalSlotsClaimed() const { return (uint32_t)(m_liveSlots.size() + m_freeSlots.size()); }

private:
    // Spawn helpers (mirror EditorHost::spawnBrush so editor + loader can never
    // drift on what a brush MEANS).
    void spawnBrush(const x3::editor::BlockoutBrush& b, Scene& scene,
                    x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);
    void spawnEntity(uint32_t entityIdx, const x3::editor::EditorEntity& e, Scene& scene,
                     x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);
    void buildAll(Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics);
    void teardownBuilt(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics);

    // Claim a scene slot: reuse a previously-owned freed slot (recycle) else add.
    uint32_t claimSlot(Scene& scene, const Entity& e);

    // Resolve a brush material id -> cached texture id + tint (bakes the procedural
    // texture on first use; session cache). Mirrors EditorHost::resolveMaterial.
    uint32_t resolveMaterial(const std::string& id, x3::rhi::IRenderDevice& device,
                             float outTint[3]);

    // Load + cache a GLB by relPath (session cache; unloaded in shutdown).
    struct LoadedModel {
        x3::asset::Model                      model;
        std::vector<x3::asset::ModelDrawable> drawables;
        bool ok = false;
    };
    const LoadedModel* loadModelCached(const std::string& relPath,
                                       x3::rhi::IRenderDevice& device);

    // One built scene object record (the ownership ledger).
    struct Built {
        uint32_t slot     = kNoLink;   // scene entity index
        bool     ownsMesh = false;     // true => destroyMesh on teardown (brush/marker
                                       // meshes; GLB drawable meshes belong to the cache)
    };

    x3::editor::LevelDoc m_doc;
    bool        m_built = false;
    std::string m_path;                          // hot-reload source ("" = in-memory)
    int64_t     m_mtime = 0;                     // last seen write time (file ticks)
    double      m_nextPoll = 0.0;                // next allowed stat (seconds)

    std::vector<Built>    m_builtObjects;        // everything spawned this build
    std::vector<uint32_t> m_liveSlots;           // live owned scene slots (diagnostics)
    std::vector<uint32_t> m_freeSlots;           // owned slots freed by teardown (recycled)
    std::vector<uint32_t> m_bodies;              // live Jolt body ids owned
    uint32_t m_brushEntities = 0, m_propEntities = 0, m_bodyCount = 0;

    std::vector<x3::rhi::PointLight> m_lights;   // doc lights (rebuilt each load)
    TriggerSystem                    m_triggers; // doc trigger zones (rebuilt each load)
    std::vector<std::pair<uint32_t, std::string>> m_triggerScripts; // entityIdx -> script

    // Session caches (kept across reloads; freed in shutdown).
    uint32_t m_matTex[8] = { 0,0,0,0,0,0,0,0 };  // per-MatTex baked texture ids
    std::unique_ptr<x3::asset::IAssetSource> m_modelAssets;
    std::unique_ptr<x3::asset::IModelLoader> m_modelLoader;
    std::unordered_map<std::string, LoadedModel> m_modelCache;
    bool m_modelDirMounted = false;
};

// Author the small SAMPLE LevelDoc the loader proofs use (--screenshot-loader and,
// when the default doc file is absent, `--world fromdoc`'s seed): a graybox room
// (floor + walls), a ramp, a hazard pillar, two warm lights, a scripted trigger,
// and a player start. Pure data — no device.
x3::editor::LevelDoc makeSampleLevelDoc();

// The default LevelDoc path `--world fromdoc` boots (== the editor's File>Save
// target, so the edit -> save -> hot-reload loop closes on it out of the box).
const char* defaultLevelDocPath();

// Headless self-test (--test-loader): author a doc in memory (brushes + props +
// lights + a scripted trigger) -> SAVE -> LOAD through the real loader -> assert
// the built world matches (entity counts, transforms, material tints, collision
// body count, trigger script); then MODIFY + reload -> assert the delta applied,
// the old objects are gone, scene slots were recycled (no growth), and the
// create/destroy ledgers (meshes, textures, bodies) balance to ZERO after
// shutdown (the no-leak gate). Logs PASS/FAIL L#; returns true iff all pass.
// No window / Vulkan.
bool runLevelDocLoaderSelfTest();

} // namespace x3::game
