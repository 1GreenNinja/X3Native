// ABOVE-GROUND SALVARI CLIFFS FINALE (Act 1 §2d) — `--world cliffs`. See cliffs.h.
//
// Clean-room: built from Scene + TerrainStreamer + the engine interfaces
// (IRenderDevice / IPhysicsWorld / IJobSystem / IModelLoader / IAssetSource) only.
// No purchased C# / id Tech / RBDOOM source consulted. The GLB-actor load + draw
// path mirrors app/rescue.cpp; the terrain + ocean path mirrors the `--world ocean`
// screenshot setup in app/main.cpp; both reuse only PUBLIC, already-shipped APIs.
#include "cliffs.h"

#include "mesh_prims.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// Facing law (CONVENTIONS.md / monster.cpp headingToFace): to point a model's
// local -Z along planar (dirX,dirZ), yaw = atan2(-dirX,-dirZ).
float headingToFace(float dirX, float dirZ) {
    if (dirX * dirX + dirZ * dirZ < 1e-12f) return 0.0f;
    return std::atan2(-dirX, -dirZ);
}

// Column-major T(pos) * Ry(yaw) * S(scale).
void composeTRS(float m[16], const float pos[3], float yaw, float scale) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    // Ry columns: x' = ( c, 0, -s ), z' = ( s, 0, c ), y' = ( 0,1,0 ), each * scale.
    m[0]  = c * scale; m[1]  = 0.0f;  m[2]  = -s * scale; m[3]  = 0.0f;
    m[4]  = 0.0f;      m[5]  = scale; m[6]  = 0.0f;       m[7]  = 0.0f;
    m[8]  = s * scale; m[9]  = 0.0f;  m[10] = c * scale;  m[11] = 0.0f;
    m[12] = pos[0];    m[13] = pos[1];m[14] = pos[2];     m[15] = 1.0f;
}

} // namespace

// ===========================================================================
// CliffsActor
// ===========================================================================
void CliffsActor::worldMatrix(float out[16]) const {
    composeTRS(out, pos, yaw, scale);
}

void CliffsActor::draw(x3::rhi::IRenderDevice& device,
                       const x3::rhi::FrameContext& frame) const {
    float world[16];
    worldMatrix(world);

    if (!usingReal) {
        // Procedural fallback box (headless / load-failure): one draw at the world
        // matrix so the actor still exists + reads in the frame.
        if (!fallbackMesh.valid()) return;
        if (emissive[3] > 0.0f)
            device.drawMeshEmissive(frame, fallbackMesh, fallbackTex, tint, emissive, world);
        else
            device.drawMesh(frame, fallbackMesh, fallbackTex, tint, world);
        return;
    }

    for (const auto& d : drawables) {
        float color[4] = {
            d.baseColorFactor[0] * tint[0],
            d.baseColorFactor[1] * tint[1],
            d.baseColorFactor[2] * tint[2],
            d.baseColorFactor[3] * tint[3],
        };
        float fin[16];
        x3::asset::mulMat4(world, d.nodeTransform, fin);
        if (emissive[3] > 0.0f)
            device.drawMeshEmissive(frame, x3::rhi::MeshHandle{ d.meshId },
                                    x3::rhi::TextureHandle{ d.baseColorTexId },
                                    color, emissive, fin);
        else
            device.drawMesh(frame, x3::rhi::MeshHandle{ d.meshId },
                            x3::rhi::TextureHandle{ d.baseColorTexId }, color, fin);
    }
}

void CliffsActor::destroy(x3::rhi::IRenderDevice& device) {
    if (usingReal && loader) loader->unload(model);
    if (fallbackMesh.valid()) device.destroyMesh(fallbackMesh);
    if (fallbackTex.valid())  device.destroyTexture(fallbackTex);
    drawables.clear();
    loader.reset();
    assets.reset();
}

// ===========================================================================
// CliffsArea
// ===========================================================================
CliffsActor CliffsArea::loadActor(x3::rhi::IRenderDevice& device, std::string_view file,
                                  const float pos[3], float yaw, float scale,
                                  const float tint[4]) {
    CliffsActor a;
    std::memcpy(a.pos, pos, sizeof(a.pos));
    a.yaw = yaw; a.scale = scale;
    std::memcpy(a.tint, tint, sizeof(a.tint));

    // Load via a mounted loose-dir asset source (same path as rescue.cpp /
    // MonsterSystem). The rigged GLBs are Y-up, so no Z->Y stand-up. On any failure
    // a procedural box stands in so the actor still exists (and --smoketest passes
    // on the headless device, where no real GLB upload happens).
    //
    // The Salvari ship + princess + trooper GLBs are NOT in the repo's LFS asset
    // subset (assets/rigged_glb only carries the curated Level-1 cast), so mount
    // BOTH the portable repo root (higher priority) AND the external library
    // G:/GameModels/rigged_glb (lower priority). PakAssetSource searches mounts in
    // priority order, so a file present in either dir resolves — the repo build
    // stays portable and these set-piece assets load off G: when present.
    a.assets.reset(x3::asset::createAssetSource());
    const std::string repoDir = riggedGlbRoot();
    const std::string extDir  = "G:/GameModels/rigged_glb";
    bool mountedRepo = a.assets->mountDir(repoDir, 10);
    bool mountedExt  = a.assets->mountDir(extDir, 0);
    if (mountedRepo || mountedExt) {
        a.loader.reset(x3::asset::createModelLoader(&device, a.assets.get()));
        a.model = a.loader->load(std::string(file));
        if (a.model.ok)
            a.drawables = x3::asset::makeDrawables(a.model);
    } else {
        x3::logWarn("[cliffs] mountDir failed (repo + ext): " + repoDir + " | " + extDir);
    }

    if (!a.drawables.empty()) {
        a.usingReal = true;
        x3::logInfo("[cliffs] loaded " + std::string(file) + " — " +
                    std::to_string(a.drawables.size()) + " primitive(s) @ (" +
                    std::to_string(pos[0]) + ", " + std::to_string(pos[1]) + ", " +
                    std::to_string(pos[2]) + ")");
    } else {
        // Fallback humanoid-ish box so the actor still renders + the area is valid.
        a.usingReal = false;
        x3::prims::PrimMesh geo = x3::prims::makeBox(0.4f, 0.9f, 0.4f, 0.0f, 0.9f, 0.0f, 1.0f);
        a.fallbackMesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                           geo.index.data(), (uint32_t)geo.index.size());
        auto px = x3::prims::makeSolidRGBA(8, 200, 210, 230);
        a.fallbackTex = device.createTexture(px.data(), 8, 8, true);
        x3::logWarn("[cliffs] " + std::string(file) + " load failed; using fallback box");
    }
    return a;
}

void CliffsArea::build(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, x3::jobs::IJobSystem* jobs) {
    // --- Canonical streamed world config (the single source of truth) so a
    //     host-side placeOnTerrain()/heightAt() agrees with what is streamed. ---
    const TerrainConfig& cfg = worldTerrainConfig();

    // --- Ocean sea level: set part-way up the height range (heightScale ~55 m) so
    //     the valleys around the chosen cliff-top FLOOD — i.e. there is visible water
    //     at the cliff base — while the pad still sits WELL above the waterline. The
    //     cliff-edge scan below then prefers a high pad whose surroundings dip below
    //     this level (real shoreline in frame). ---
    m_seaLevel = 22.0f;

    // --- Pick a CLIFF-EDGE vantage: scan a wide grid for a high point (well above
    //     the sea, so the pad is a clear cliff-top) that ALSO has a nearby BELOW-SEA
    //     sample — i.e. a real cliff with water at its base. Score each candidate by
    //     its own height + the depth of the deepest neighbor a short way off, and
    //     remember the direction toward that drop so the camera looks out over the
    //     ocean. terrainHeightAtWorld() is pure + valid before any tile exists. ---
    float bestX = 40.0f, bestZ = -10.0f, bestScore = -1e9f;
    float bestDropX = -0.707f, bestDropZ = -0.707f;
    const float kProbe = 130.0f;   // how far off the pad to look for the drop
    for (float cz = -560.0f; cz <= 560.0f; cz += 32.0f) {
        for (float cx = -560.0f; cx <= 560.0f; cx += 32.0f) {
            const float h = terrainHeightAtWorld(cx, cz);
            if (h < m_seaLevel + 16.0f) continue;     // need a genuine cliff-top
            // Find the deepest neighbor a probe-radius out (the drop toward the sea),
            // sampling a couple of radii so a below-sea valley is picked up.
            float deepest = h; float dDirX = 0.0f, dDirZ = 0.0f;
            for (int k = 0; k < 12; ++k) {
                const float ang = (float)k * 0.5235987756f;   // 12 directions (30 deg)
                const float cs = std::cos(ang), sn = std::sin(ang);
                for (float pr = kProbe * 0.6f; pr <= kProbe; pr += kProbe * 0.4f) {
                    const float nh = terrainHeightAtWorld(cx + pr * cs, cz + pr * sn);
                    if (nh < deepest) { deepest = nh; dDirX = cs; dDirZ = sn; }
                }
            }
            // Strongly reward a drop that goes BELOW the sea (visible shoreline),
            // plus the pad's own height + the raw drop.
            const float drop = h - deepest;
            const float belowSeaBonus = (deepest < m_seaLevel) ? (m_seaLevel - deepest) * 4.0f : -40.0f;
            const float score = h * 0.5f + drop * 1.5f + belowSeaBonus;
            if (score > bestScore) {
                bestScore = score; bestX = cx; bestZ = cz;
                bestDropX = dDirX; bestDropZ = dDirZ;
            }
        }
    }
    { const float l = std::sqrt(bestDropX*bestDropX + bestDropZ*bestDropZ);
      const float inv = (l > 1e-4f) ? 1.0f/l : 0.0f;
      m_dropDir[0] = bestDropX * inv; m_dropDir[1] = bestDropZ * inv; }
    // Anchor the pad ON the surface at the chosen cliff-edge high point.
    float padOnSurf[3];
    placeOnTerrain(bestX, bestZ, padOnSurf);   // {x, surfaceY, z}
    m_padCenter[0] = padOnSurf[0];
    m_padCenter[1] = padOnSurf[1];
    m_padCenter[2] = padOnSurf[2];
    // Diagnostic: the deepest terrain a probe-radius out along the drop dir (where
    // the sea should show) — confirms the cliff actually descends below sea level.
    const float dropProbeH = terrainHeightAtWorld(
        m_padCenter[0] + m_dropDir[0] * kProbe, m_padCenter[2] + m_dropDir[1] * kProbe);
    x3::logInfo("[cliffs] pad vantage @ (" + std::to_string(m_padCenter[0]) + ", " +
                std::to_string(m_padCenter[1]) + ", " + std::to_string(m_padCenter[2]) +
                ")  seaLevel=" + std::to_string(m_seaLevel) +
                "  dropDir=(" + std::to_string(m_dropDir[0]) + "," + std::to_string(m_dropDir[1]) +
                ")  probeH@" + std::to_string((int)kProbe) + "m=" + std::to_string(dropProbeH));

    // --- Snowy-exterior sky + sun (cool, bright). The streamed terrain's own
    //     height-blended material paints SNOW on the high cliffs automatically. ---
    {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.85f; sp.sunDir[2] = 0.40f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.98f; sp.sunColor[2] = 0.96f; // crisp white-blue
        sp.sunIntensity = 1.15f; sp.haze = 0.6f; sp.exposure = 1.0f;
        device.setSkyParams(sp);
    }

    // --- Ocean at the cliff base. Tasteful cold Gerstner defaults; per-frame the
    //     wave clock advances in update(). Sea level is well under the pad. ---
    m_water = x3::rhi::IRenderDevice::WaterParams{};
    m_water.enabled = true;
    m_water.seaLevel = m_seaLevel;
    m_water.amplitude = 0.55f; m_water.steepness = 0.55f;
    m_water.waveLength = 18.0f; m_water.speed = 0.9f;
    m_water.deepColor[0] = 0.012f; m_water.deepColor[1] = 0.05f;  m_water.deepColor[2] = 0.11f;
    m_water.shallowColor[0] = 0.08f; m_water.shallowColor[1] = 0.28f; m_water.shallowColor[2] = 0.36f;
    m_water.sunDir[0] = 0.35f; m_water.sunDir[1] = 0.85f; m_water.sunDir[2] = 0.40f;
    m_water.specular = 16.0f; m_water.fresnel = 0.02f;
    device.setWaterParams(m_water);

    // --- Bring up the streamed terrain ring centered on the pad. Radius 8 tiles
    //     (= 256 m) so a generous expanse of cliffs is resident. jobs may be null
    //     (synchronous generation — the headless self-test path). ---
    m_streamer.init(scene, device, physics, jobs, cfg, m_padCenter[0], m_padCenter[2], /*radius=*/8);
    m_streamer.setUploadBudget(64);   // fill the ring fast for the single capture

    // --- The landing pad: a flat, wide box planted ON the terrain so the ship has
    //     solid level ground to set down on (the natural terrain there is sloped).
    //     The box is sunk so its TOP sits ~0.4 m proud of the surface; static
    //     collision so it reads solid. center = surface + (top offset) - padHY. ---
    const float padHX = 7.0f, padHY = 0.6f, padHZ = 7.0f;
    const float padTopAbove = 0.4f;                     // pad top this far over the surface
    const float padCY = m_padCenter[1] + padTopAbove - padHY;  // box center Y
    m_padTopY = padCY + padHY;                          // walkable top of the pad
    x3::prims::PrimMesh pad = x3::prims::makeBox(padHX, padHY, padHZ,
                                                 m_padCenter[0], padCY, m_padCenter[2], 0.5f);
    m_padMesh = device.createMesh(pad.verts.data(), (uint32_t)pad.verts.size(),
                                  pad.index.data(), (uint32_t)pad.index.size());
    m_padBody = physics.addStaticMesh(pad.cverts.data(), (uint32_t)(pad.cverts.size() / 3),
                                      pad.cindex.data(), (uint32_t)pad.cindex.size());
    // A cold metallic-grey pad texture (alien landing platform).
    auto padPx = x3::prims::makeCheckerRGBA(64, 8, 150, 158, 172, 64, 70, 86);
    m_padTex = device.createTexture(padPx.data(), 64, 64, true);

    // --- The Salvari ship: SET DOWN ON the pad (its base resting on the pad top).
    //     SpaceShip.glb is ~2.9 x 1.8 x 4.4 m at scale 1 (its base sits at local
    //     y=0), so scale it up to a ~14 m set-piece that dominates the 14 m pad.
    //     Yaw it to angle the hull toward the camera vantage. ---
    {
        const float shipScale = 4.0f;
        float shipPos[3] = { m_padCenter[0], m_padTopY, m_padCenter[2] };
        const float shipTint[4] = { 0.9f, 0.95f, 1.05f, 1.0f };   // cool alien hull
        const float shipYaw = headingToFace(-1.0f, 0.4f);          // angled to the vantage
        CliffsActor ship = loadActor(device, "SpaceShip.glb", shipPos, shipYaw, shipScale, shipTint);
        // A faint engine/hull glow so the ship reads as alien tech against the snow.
        ship.emissive[0] = 0.10f; ship.emissive[1] = 0.30f; ship.emissive[2] = 0.55f; ship.emissive[3] = 0.6f;
        m_actors.push_back(std::move(ship));
    }

    // --- Salvari forces: K'thara (the princess/commander) + two troopers, each
    //     ANCHORED to the terrain surface via placeOnTerrain. They stand on the
    //     CAMERA-FACING (uphill) side of the pad — between the camera and the ship —
    //     so they read clearly in front of the hull, facing the ship. Offsets are
    //     in a frame aligned to the camera: `back` = away from the drop (toward the
    //     camera), `side` = perpendicular. ---
    const float backX = -m_dropDir[0], backZ = -m_dropDir[1];      // toward the camera
    const float sideX = -m_dropDir[1], sideZ =  m_dropDir[0];      // perpendicular (XZ)
    struct Spec { const char* file; float back, side; float scale; float r,g,b; };
    const Spec forces[3] = {
        { "SalvariPrincess.glb",          11.0f,  1.5f, 1.0f, 1.05f, 0.95f, 1.10f }, // K'thara — bioluminescent
        { "EnemyOccupationTrooper777.glb",10.0f, -4.5f, 1.0f, 0.85f, 0.90f, 1.00f }, // trooper
        { "EnemyOccupationTrooper777.glb",13.0f,  5.0f, 1.0f, 0.85f, 0.90f, 1.00f }, // trooper
    };
    for (const Spec& sp : forces) {
        const float wx = m_padCenter[0] + backX * sp.back + sideX * sp.side;
        const float wz = m_padCenter[2] + backZ * sp.back + sideZ * sp.side;
        float onSurf[3];
        placeOnTerrain(wx, wz, onSurf);            // {x, surfaceY, z} — ON the terrain
        // Face the pad center (the ship).
        const float yaw = headingToFace(m_padCenter[0] - wx, m_padCenter[2] - wz);
        const float tint[4] = { sp.r, sp.g, sp.b, 1.0f };
        CliffsActor act = loadActor(device, sp.file, onSurf, yaw, sp.scale, tint);
        // K'thara gets a soft bioluminescent glow (the Salvari are bioluminescent).
        if (std::string(sp.file) == "SalvariPrincess.glb") {
            act.emissive[0] = 0.20f; act.emissive[1] = 0.55f; act.emissive[2] = 0.65f; act.emissive[3] = 0.5f;
        }
        m_actors.push_back(std::move(act));
    }

    // --- Point-light fills so the ship + Salvari read against the bright snow (the
    //     directional sun alone leaves the camera-facing sides flat). Warm key on
    //     the ship + a cool rim on the Salvari, plus an overhead fill on the pad. ---
    {
        x3::rhi::PointLight pl[4];
        // Warm key in front of the ship (camera side).
        pl[0].pos[0] = m_padCenter[0] - 6.0f; pl[0].pos[1] = m_padTopY + 5.0f; pl[0].pos[2] = m_padCenter[2] + 6.0f;
        pl[0].range = 26.0f; pl[0].color[0] = 6.0f; pl[0].color[1] = 5.4f; pl[0].color[2] = 4.6f;
        // Cool rim behind the Salvari.
        pl[1].pos[0] = m_padCenter[0] + 14.0f; pl[1].pos[1] = m_padTopY + 4.0f; pl[1].pos[2] = m_padCenter[2] - 6.0f;
        pl[1].range = 22.0f; pl[1].color[0] = 3.4f; pl[1].color[1] = 4.4f; pl[1].color[2] = 6.0f;
        // Overhead fill on the pad/ship.
        pl[2].pos[0] = m_padCenter[0]; pl[2].pos[1] = m_padTopY + 9.0f; pl[2].pos[2] = m_padCenter[2];
        pl[2].range = 28.0f; pl[2].color[0] = 4.2f; pl[2].color[1] = 4.4f; pl[2].color[2] = 4.8f;
        // Glow under the ship (engine wash on the pad).
        pl[3].pos[0] = m_padCenter[0]; pl[3].pos[1] = m_padTopY + 1.0f; pl[3].pos[2] = m_padCenter[2];
        pl[3].range = 14.0f; pl[3].color[0] = 1.2f; pl[3].color[1] = 3.0f; pl[3].color[2] = 5.5f;
        device.setPointLights(pl, 4);
    }

    physics.optimizeBroadphase();
    x3::logInfo("[cliffs] built — ship + " + std::to_string(m_actors.size() - 1) +
                " Salvari, ocean@" + std::to_string(m_seaLevel) +
                ", " + std::to_string(m_streamer.residentCount()) + " tile(s) resident at init");
}

void CliffsArea::update(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics, float dt,
                        float focusX, float focusZ) {
    m_streamer.update(scene, device, physics, focusX, focusZ);
    m_waveTime += dt;
    m_water.time = m_waveTime;
    device.setWaterParams(m_water);
}

void CliffsArea::render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const Scene& scene) const {
    // 1) the streamed terrain tiles are Scene entities — draw the ground.
    scene.render(device, frame);
    // 2) the landing pad.
    if (m_padMesh.valid()) {
        const float idP[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const float white[4] = {1,1,1,1};
        device.drawMesh(frame, m_padMesh, m_padTex, white, idP);
    }
    // 3) the ship + the Salvari forces.
    for (const auto& a : m_actors) a.draw(device, frame);
}

void CliffsArea::shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                          x3::phys::IPhysicsWorld& physics) {
    for (auto& a : m_actors) a.destroy(device);
    m_actors.clear();
    if (m_padMesh.valid()) device.destroyMesh(m_padMesh);
    if (m_padTex.valid())  device.destroyTexture(m_padTex);
    m_padMesh = x3::rhi::MeshHandle{};
    m_padTex  = x3::rhi::TextureHandle{};
    m_streamer.shutdown(scene, device, physics);
}

void CliffsArea::suggestCamera(float eye[3], float& yaw, float& pitch) const {
    // The pad sits on a cliff-top; m_dropDir points toward the nearby descent to the
    // sea. Stand on the UPHILL side (opposite the drop) + a bit to the side and look
    // out ACROSS the ship along the drop direction — that sightline runs the ship +
    // the Salvari, over the cliff edge, and down to the ocean, in one cinematic 3/4
    // frame.
    const float px = m_padCenter[0], pz = m_padCenter[2];
    const float dropX = m_dropDir[0], dropZ = m_dropDir[1];
    const float side[2] = { -dropZ, dropX };       // perpendicular in XZ
    eye[0] = px - dropX * 30.0f + side[0] * 12.0f;
    eye[1] = m_padTopY + 12.0f;
    eye[2] = pz - dropZ * 30.0f + side[1] * 12.0f;
    // Aim toward a point past the cliff edge, dropped toward the sea, so the ocean
    // enters the lower frame while the ship/Salvari hold the foreground.
    const float tx = px + dropX * 40.0f;
    const float ty = (m_padTopY + m_seaLevel) * 0.5f;   // between pad top + sea
    const float tz = pz + dropZ * 40.0f;
    const float dx = tx - eye[0], dy = ty - eye[1], dz = tz - eye[2];
    yaw = std::atan2(dz, dx);                       // setCamera forward uses (cos yaw, .., sin yaw)
    const float horiz = std::sqrt(dx * dx + dz * dz);
    pitch = std::atan2(dy, horiz);
}

// ===========================================================================
// Headless self-test (--test-cliffs). No window / Vulkan.
// ===========================================================================
namespace {

int g_cpass = 0, g_cfail = 0;
void ccheck(bool cond, const char* name) {
    if (cond) { ++g_cpass; x3::logInfo(std::string("[cliffs-test] PASS ") + name); }
    else      { ++g_cfail; x3::logError(std::string("[cliffs-test] FAIL ") + name); }
}

} // namespace

bool runCliffsSelfTest() {
    g_cpass = g_cfail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessRenderDevice device;   // mints fake handles; GLBs fall back to boxes
    Scene scene;
    CliffsArea area;
    // jobs=null => the streamer generates the under-focus neighborhood synchronously,
    // so a tile is resident with no window / job pool.
    area.build(scene, device, *physics, /*jobs*/nullptr);

    // C0: the area built ship + Salvari actors (1 ship + 3 forces = 4).
    ccheck(area.actorCount() == 4, "C0 ship + 3 Salvari actors built");

    // C1: the pad sits ABOVE the sea level (a real cliff-top landing).
    ccheck(area.padCenter()[1] > area.seaLevel() + 2.0f, "C1 pad above sea level");

    // C2: the pad center is anchored ON the real terrain surface (placeOnTerrain
    //     agrees with the height field there, within a small epsilon).
    {
        const float* pc = area.padCenter();
        const float surf = terrainHeightAtWorld(pc[0], pc[2]);
        ccheck(std::abs(surf - pc[1]) < 1e-3f, "C2 pad anchored on terrain surface");
    }

    // C3: terrain ring is resident (the under-focus neighborhood generated).
    ccheck(area.residentTiles() >= 1, "C3 terrain resident");

    // C4: a representative Salvari spot resolves to a surface-anchored position
    //     (placeOnTerrain == heightAt, the placement contract this area relies on).
    {
        const float* pc = area.padCenter();
        const float wx = pc[0] + 11.0f, wz = pc[2] + 2.0f;
        float onSurf[3];
        placeOnTerrain(wx, wz, onSurf);
        ccheck(std::abs(onSurf[1] - terrainHeightAtWorld(wx, wz)) < 1e-3f,
               "C4 Salvari anchored on terrain");
    }

    area.shutdown(scene, device, *physics);
    physics->shutdown();
    x3::logInfo("[cliffs-test] " + std::to_string(g_cpass) + " passed, " +
                std::to_string(g_cfail) + " failed");
    return g_cfail == 0;
}

} // namespace x3::game
