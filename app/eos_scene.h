#pragma once
// ============================================================================
// EOS SCENE — grey-box loader for the "eos-scene-1" native snapshot format
// (the Empires of Shadow native-client spike; contract:
// epochs-rts/docs/design/NATIVE-SCENE-FORMAT.md, charter NATIVE-CLIENT-SPIKE.md).
//
// A scene is two files in one directory:
//   manifest.json — UTF-8 JSON: string tables, the section table, the canonical
//                   30 s benchmark camera path (121 keyframes, LINEAR).
//   scene.bin     — one flat little-endian blob of raw typed arrays, located by
//                   the manifest's byte-exact section table.
//
// COORDINATES: the source world is LEFT-handed, +Y up, 1 world unit per tile,
// world x = tx, world z = ty. This engine is RIGHT-handed (CLAUDE.md §AXES), so
// Z is negated EXACTLY ONCE at import — every source (x, y, z) becomes engine
// (x, y, -z), camera keyframes included. The spec's a-b-c / b-d-c triangle
// split is emitted in the SAME index order over the Z-negated vertices, which
// IS the required winding flip: the mirrored triangles come out CCW-from-above
// (+Y normals), matching VK_FRONT_FACE_COUNTER_CLOCKWISE (verified by cross
// product and by eyeballing the captures — terrain visible from above, not
// inside-out).
//
// GREY-BOX ON PURPOSE (charter: "NO art-parity ambition"): terrain is meshed
// from the reliefHeights fine grid with a per-vertex terrain-tint * baked-AO
// texture; water is the engine's Gerstner plane at manifest waterLevel; trees
// are one cheap trunk-cylinder + canopy-cone mesh instanced per node
// (deterministic per-instance yaw/scale/tint jitter so 25k trees don't read as
// a rubber-stamp grid); other resource nodes are small tinted cubes; buildings
// are boxes on their (pre-flattened) footprints, foundations are slabs; units
// are capsules tinted per player; grove giants are the big cones.
//
// Game/slice code only — engine/ stays pure (same seam as valley/cliffs/city).
// ============================================================================

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

class EosSceneWorld {
public:
    // Parse manifest.json + read scene.bin from `dir`. Returns false (with a
    // logged reason) on any contract violation: wrong version, bin size
    // mismatch, missing/mis-typed/mis-sized section, bad camera path.
    bool load(const std::string& dir);

    // Create the GPU meshes/texture and precompute every instance transform.
    // Call between the device's beginUploadBatch()/endUploadBatch() for the
    // single-submit boot path. load() must have succeeded.
    void build(x3::rhi::IRenderDevice& device);

    // Submit the whole scene for this frame (terrain chunks + all instances).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // Destroy the GPU resources build() created. Safe to call more than once.
    void shutdown(x3::rhi::IRenderDevice& device);

    // Triangle-exact terrain height at SOURCE-space (x, z) — the spec's
    // interpolation rule on the b-c diagonal split (unit/camera Y placement).
    float heightAt(float x, float z) const;

    // The canonical benchmark orbit at time t (seconds, wrapped to the loop).
    // Outputs ENGINE-space eye position + unit forward (Z already negated).
    void cameraAt(float t, float pos[3], float fwd[3]) const;
    float cameraDuration() const { return m_camDuration; }

    float waterLevel() const { return m_waterLevel; }
    int   mapW() const { return m_W; }
    int   mapH() const { return m_H; }
    // Section element counts, for the boot log / host report.
    uint32_t nodeCount() const     { return (uint32_t)m_nodeKind.size(); }
    uint32_t treeCount() const     { return m_treeCount; }
    uint32_t groveCount() const    { return (uint32_t)m_groveSeed.size(); }
    uint32_t buildingCount() const { return (uint32_t)m_buildingDef.size(); }
    uint32_t unitCount() const     { return (uint32_t)m_unitType.size(); }
    uint32_t drawInstanceCount() const { return (uint32_t)m_instances.size() + (uint32_t)m_terrainMeshes.size(); }
    int      seed() const          { return m_seed; }
    const std::string& mapScript() const { return m_mapScript; }

private:
    // One submitted instance: a shared mesh + a per-instance tint + transform.
    struct Inst {
        x3::rhi::MeshHandle mesh;
        float tint[4];
        float model[16];
    };

    void buildTerrain(x3::rhi::IRenderDevice& device);
    void buildInstances(x3::rhi::IRenderDevice& device);

    // ---- manifest ----
    int         m_seed = 0;
    std::string m_mapScript;
    int   m_W = 0, m_H = 0;          // map tiles
    int   m_R = 1;                   // reliefRes (fine verts per tile edge)
    int   m_GW1 = 0, m_GH1 = 0;      // fine-grid vertex dims (W*R+1, H*R+1)
    float m_waterLevel = -0.16f;
    std::vector<std::string> m_terrainTypes;
    std::vector<int>         m_waterTerrainIds;
    std::vector<std::string> m_nodeKinds;
    std::vector<std::string> m_buildingDefs;
    std::vector<std::string> m_unitDefs;

    // ---- camera path (source-space keyframes; converted in cameraAt) ----
    struct CamKey { float t; float pos[3]; float target[3]; };
    std::vector<CamKey> m_camKeys;
    float m_camDuration = 30.0f;

    // ---- section payloads (copied out of scene.bin) ----
    std::vector<uint8_t>  m_terrain;        // W*H tile ids
    std::vector<float>    m_heights;        // GW1*GH1 fine-grid heights
    std::vector<float>    m_ao;             // GW1*GH1 baked valley AO (0.70..1.0)
    std::vector<float>    m_nodePos;        // nodes*2 (x,z)
    std::vector<uint8_t>  m_nodeKind;       // nodes
    std::vector<float>    m_grovePos;       // groves*2
    std::vector<uint32_t> m_groveSeed;      // groves
    std::vector<uint16_t> m_buildingRect;   // buildings*4 (tx,ty,w,h)
    std::vector<uint16_t> m_buildingDef;    // buildings
    std::vector<uint8_t>  m_buildingPlayer; // buildings
    std::vector<uint8_t>  m_buildingFlags;  // buildings (bit0 = complete)
    std::vector<float>    m_unitPos;        // units*2
    std::vector<float>    m_unitFacing;     // units (loaded per contract; a capsule
                                            // has no visible facing — not applied)
    std::vector<uint16_t> m_unitType;       // units
    std::vector<uint8_t>  m_unitPlayer;     // units
    std::vector<uint8_t>  m_unitFlags;      // units (bit0 = benchmark spawn)

    // ---- GPU state ----
    bool m_built = false;
    x3::rhi::TextureHandle m_terrainTex{};             // per-vertex tint * AO
    std::vector<x3::rhi::MeshHandle> m_terrainMeshes;  // world-baked chunks
    x3::rhi::MeshHandle m_treeMesh{};                  // trunk + canopy, one mesh
    x3::rhi::MeshHandle m_giantMesh{};                 // grove giant (big cone)
    x3::rhi::MeshHandle m_cubeMesh{};                  // unit cube, base at y=0
    x3::rhi::MeshHandle m_capsuleMesh{};               // unit capsule, feet at y=0
    std::vector<Inst>   m_instances;                   // everything non-terrain
    uint32_t m_treeCount = 0;
};

} // namespace x3::game
