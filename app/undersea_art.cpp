// EFLZ Act-4 undersea-base art overlay. See app/undersea_art.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice + Scene
// interfaces + the OceanBasePlan only. No third-party engine source consulted.
#include "undersea_art.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// Converted Abyssal Station GLB (modules-only; OceanBase supplies the seafloor),
// relative to the mounted converted_glb dir.
const char* kRelStation = "Undersea/abyssal_station.glb";

// Probed glTF-space (engine Y-up) AABB of abyssal_station.glb, in meters
// (assemble_undersea_base.py exports it base-seated at Y=0). Derived from the
// Blender probe x[-14,17.5] y[-8.5,11.75] z[0,13.277] via the glTF->Blender
// import map (blender.z = gltf.y up, blender.y = -gltf.z):
//   gltf X[-14.0, 17.5]   Y[0.0, 13.277]   Z[-11.75, 8.5]
// We seat the station's base-center anchor onto the base top deck.
constexpr float kStAnchorX = 1.75f;    // (-14.0 + 17.5)/2  centre X
constexpr float kStAnchorY = 0.0f;     // min Y (base) -> sits on the deck
constexpr float kStAnchorZ = -1.625f;  // (-11.75 + 8.5)/2  centre Z
// Up-scale so the ~31x20 m kit reads as a real complex on the r=80 disc.
constexpr float kStationScale = 2.6f;

// Column-major TRS: yaw about +Y (rad) + uniform scale s, mapping the local point
// (px,py,pz) to world (wx,wy,wz). i.e. world = T(w)*R_y(yaw)*S(s)*T(-p). Mirrors
// env_art.cpp's placeYaw (see docs/CONVENTIONS.md §1/§3 facing convention).
void placeYaw(float m[16], float yaw, float s,
              float px, float py, float pz,
              float wx, float wy, float wz) {
    const float c = std::cos(yaw), sn = std::sin(yaw);
    m[0]=c*s;  m[1]=0;  m[2]=-sn*s; m[3]=0;
    m[4]=0;    m[5]=s;  m[6]=0;     m[7]=0;
    m[8]=sn*s; m[9]=0;  m[10]=c*s;  m[11]=0;
    const float rpx = (c*px + sn*pz) * s;
    const float rpy = (py) * s;
    const float rpz = (-sn*px + c*pz) * s;
    m[12]=wx - rpx; m[13]=wy - rpy; m[14]=wz - rpz; m[15]=1.0f;
}

} // namespace

uint32_t UnderseaArtSystem::loadAsset(const std::string& relPath) {
    for (uint32_t i=0;i<m_assetPaths.size();++i)
        if (m_assetPaths[i]==relPath) return i;

    UnderseaAsset a;
    a.model = m_loader->load(relPath);
    if (a.model.ok) {
        a.drawables = x3::asset::makeDrawables(a.model);
        a.ok = !a.drawables.empty();
    }
    if (a.ok)
        x3::logInfo("[undersea-art] loaded " + relPath + " — " +
                    std::to_string(a.drawables.size()) + " drawable prim(s)");
    else
        x3::logWarn("[undersea-art] FAILED to load " + relPath + " (graybox fallback kept)");

    uint32_t idx = (uint32_t)m_assetTable.size();
    m_assetTable.push_back(std::move(a));
    m_assetPaths.push_back(relPath);
    return idx;
}

void UnderseaArtSystem::addInstanceEmissive(uint32_t a, const float transform[16],
                                            const float emissive[4]) {
    if (a >= m_assetTable.size() || !m_assetTable[a].ok) return; // skip failed assets
    UnderseaInstance e; e.asset = a;
    for (int i=0;i<16;++i) e.transform[i]=transform[i];
    if (emissive) for (int i=0;i<4;++i) e.emissive[i]=emissive[i];
    m_instances.push_back(e);
}

void UnderseaArtSystem::build(x3::rhi::IRenderDevice& device,
                              std::string_view convertedGlbDir,
                              const OceanBasePlan& plan) {
    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(convertedGlbDir, 0)) {
        x3::logWarn("[undersea-art] mountDir failed: " + std::string(convertedGlbDir) +
                    " — keeping full graybox");
        return;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));

    const uint32_t station = loadAsset(kRelStation);
    if (!m_assetTable[station].ok) {
        x3::logWarn("[undersea-art] station GLB unavailable — OceanBase graybox kept");
        return;
    }

    // ---- Seat the station's base-centre anchor on the base top deck centre. ----
    float m[16];
    placeYaw(m, /*yaw*/0.0f, kStationScale,
             kStAnchorX, kStAnchorY, kStAnchorZ,
             plan.cx, plan.baseDeckY, plan.cz);
    // The engine has no IBL, so the matte hull's albedo is capped dark and reads
    // below the murk. A TASTEFUL cool emissive lifts the whole hull above the deep
    // (the matte sun+ambient diffuse detail still varies underneath, so it isn't a
    // flat blob) — reads as a lit/bioluminescent deep-sea station + feeds bloom.
    // (Rich gunmetal-with-IBL awaits the engine IBL pass — see note below.)
    const float kStationEmis[4] = { 0.09f, 0.14f, 0.21f, 2.0f };
    addInstanceEmissive(station, m, kStationEmis);

    // ---- Cool point-light fixtures around the station so the PBR hull is lit in
    // the deep (the env_art Light_A pattern) + feeds the bloom chain. Cyan-white,
    // premultiplied by intensity; range reaches across the structure. ----
    // Cool-WHITE (only a slight blue lean) so the gunmetal PBR reads instead of
    // tinting the whole hull blue. Placed OUTSIDE + ABOVE the ~82 m / ~34 m-tall
    // station so they light the exterior (not buried inside the hull).
    // KNOWN ISSUE (under investigation w/ Integrator): in a standalone --world
    // loop this converted GLB renders via drawMeshPBR as a near-black silhouette
    // even at the origin with sun + these point lights set — ruled out: world
    // coords, metallic level, missing normals. The whole standalone scene reads
    // under-lit, so it's a sun/exposure/PBR-path interaction the full Level1 scene
    // render handles but a bare --world loop does not. The lit read is pending that.
    // Point lights fall off as ~1/d^2 (mesh.frag pointAtten), so distant lamps do
    // nothing — these sit CLOSE (within ~18 m) and among the structure so they
    // actually rim-light nearby modules + add variation over the emissive lift.
    const float kIntensity = 7.0f;
    const float kColR = 0.78f * kIntensity;
    const float kColG = 0.90f * kIntensity;
    const float kColB = 1.00f * kIntensity;
    const float deckY = plan.baseDeckY;
    struct LP { float dx, dy, dz, range; };
    const LP lps[] = {
        {  18.0f, 12.0f,  16.0f, 34.0f },   // close fills around the complex
        { -18.0f, 12.0f,  16.0f, 34.0f },
        {  18.0f, 12.0f, -16.0f, 34.0f },
        { -18.0f, 12.0f, -16.0f, 34.0f },
        {   0.0f, 22.0f,   0.0f, 40.0f },   // close key over the habitat
    };
    for (const LP& l : lps) {
        x3::rhi::PointLight pl;
        pl.pos[0] = plan.cx + l.dx; pl.pos[1] = deckY + l.dy; pl.pos[2] = plan.cz + l.dz;
        pl.range  = l.range;
        pl.color[0] = kColR; pl.color[1] = kColG; pl.color[2] = kColB;
        m_lightFixtures.push_back(pl);
    }

    x3::logInfo("[undersea-art] built: " + std::to_string(assetsLoaded()) + " asset(s), " +
                std::to_string(m_instances.size()) + " instance(s), " +
                std::to_string(m_lightFixtures.size()) + " point light(s) @ base (" +
                std::to_string((int)plan.cx) + "," + std::to_string((int)plan.cz) + ")");
}

void UnderseaArtSystem::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    for (const UnderseaInstance& inst : m_instances) {
        const UnderseaAsset& a = m_assetTable[inst.asset];
        for (const auto& d : a.drawables) {
            float fin[16];
            x3::asset::mulMat4(inst.transform, d.nodeTransform, fin);
            device.drawMeshPBR(frame,
                               x3::rhi::MeshHandle{ d.meshId },
                               x3::rhi::TextureHandle{ d.baseColorTexId },
                               x3::rhi::TextureHandle{ d.normalTexId },
                               x3::rhi::TextureHandle{ d.mrTexId },
                               d.baseColorFactor,
                               inst.emissive,
                               fin);
        }
    }
}

uint32_t UnderseaArtSystem::assetsLoaded() const {
    uint32_t n=0; for (const auto& a : m_assetTable) if (a.ok) ++n; return n;
}

// ===========================================================================
// Headless self-test (--test-undersea-art).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[undersea-art-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[undersea-art-test] FAIL " + name); }
}
using HeadlessDevice = x3::game::HeadlessRenderDevice;

} // namespace

bool runUnderseaArtSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;

    // Build the graybox undersea zone first (gives the OceanBasePlan to overlay onto).
    OceanBase ob;
    ob.build(scene, device, *physics);
    check(ob.built(), "U0 OceanBase graybox built (overlay target)");

    UnderseaArtSystem art;
    art.build(device, convertedGlbRoot(), ob.plan());

    check(art.assetsLoaded() > 0,
          "U1 abyssal station GLB loaded (got " + std::to_string(art.assetsLoaded()) + ")");
    check(art.instanceCount() > 0,
          "U2 station instance placed (got " + std::to_string(art.instanceCount()) + ")");
    check(!art.lightFixtures().empty(),
          "U3 cool point-light fixtures registered (got " +
          std::to_string(art.lightFixtures().size()) + ")");

    physics->shutdown();
    x3::logInfo(std::string("undersea-art: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
