// ship_interior_art — see header. Kit piece placements are computed against the
// MEASURED converted-GLB AABBs (integration-feast fold):
//   Interior_Wall_Simple_02   0.5 x 4.0 x 6.0   (thickness X, height Y, length Z)
//   Interior_Wall_Computer_01 1.9 x 4.0 x 6.5
//   Interior_Wall_Pipes_01    0.9 x 5.2 x 8.5
//   Interior_Wall_Bed_01      2.0 x 4.9 x 7.3
//   Interior_Wall_Case_01     1.4 x 5.3 x 7.0
//   Interior_Kitchen_01       3.5 x 3.5 x 1.5
//   Wall_Door_Simple_01       3.3 x 5.0 x 7.3
//   Floor_01                  6 x 1 x 6 (slab, top at y=0)
//   Console_Small_01          1.0 x 1.2 x 1.4 ; Chair_01 1.25 x 1.5
//   Panel_Screen_01 2.6 x 1.8 ; Articulated_TV_Screen_01 3.0 x 2.0
//   Barrel_01 1.0 x 1.3 ; Big_Oxygen_Tank_01 10.9 x 12 (scaled to prop size)
//   Air_Grid_01 4.2 x 1.1 (ceiling vent)
// Rooms (makeSmallCockpit): cockpit 6x3x6 @ origin, corridor 3x3x5 aft (z 3..8),
// door {0,1.1,3} 1.4x2.2, helm {0,0,-2.4}, nav {1.8,0,-1.6}, windows: forward
// {0,1.6,-3} 3x1.4 + port {-3,1.6,0} 2x1 (portals nudge 0.22 m into the room —
// cladding sits 0.10 m inside the graybox so the portal stays IN FRONT of it).

#include "ship_interior_art.h"
#include "../asset_root.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <cstring>

namespace x3::space {

namespace {

// Column-major TRS: yaw about +Y, uniform scale, translate.
void composeYawTS(float yaw, float s, float tx, float ty, float tz, float out[16]) {
    const float c = std::cos(yaw) * s, sn = std::sin(yaw) * s;
    out[0]=c;    out[1]=0; out[2]=-sn;  out[3]=0;
    out[4]=0;    out[5]=s; out[6]=0;    out[7]=0;
    out[8]=sn;   out[9]=0; out[10]=c;   out[11]=0;
    out[12]=tx;  out[13]=ty; out[14]=tz; out[15]=1;
}

void mul4(const float a[16], const float b[16], float o[16]) {
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += a[k*4+row] * b[col*4+k];
            o[col*4+row] = v;
        }
}

} // namespace

uint32_t ShipInteriorArt::build(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                                const ShipManifest& manifest) {
    m_device = &device;
    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(x3::game::convertedGlbRoot(), 0)) {
        x3::logWarn("[shipart] mountDir failed: " + x3::game::convertedGlbRoot());
        return 0;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));

    // Shared 1x1 satin MR: assigning it forces the Scene's PBR route so kit
    // normal/emissive maps are honored (the intro-cockpit recipe).
    const uint8_t mrPx[4] = { 255, 150, 40, 255 };
    m_mrShared = device.createTexture(mrPx, 1, 1, /*srgb*/false);

    // Load a piece + place ALL its drawables at yaw/scale/pos. Returns entity
    // count placed (0 == GLB missing: graybox fallback, log once).
    auto place = [&](const char* glb, float yaw, float s,
                     float tx, float ty, float tz) -> uint32_t {
        x3::asset::Model model = m_loader->load(std::string("SciFiKit3/") + glb);
        if (!model.ok) {
            x3::logWarn(std::string("[shipart] missing piece (graybox fallback): ") + glb);
            return 0;
        }
        const auto draws = x3::asset::makeDrawables(model);
        float T[16]; composeYawTS(yaw, s, tx, ty, tz, T);
        uint32_t placed = 0;
        for (const auto& d : draws) {
            if (!d.meshId) continue;
            x3::game::Entity e;
            e.mesh = x3::rhi::MeshHandle{ d.meshId };
            e.tex  = x3::rhi::TextureHandle{ d.baseColorTexId };
            e.normalTex = x3::rhi::TextureHandle{ d.normalTexId };
            e.mrTex = d.mrTexId ? x3::rhi::TextureHandle{ d.mrTexId } : m_mrShared;
            for (int i = 0; i < 4; ++i) e.baseColor[i] = d.baseColorFactor[i];
            if (d.emissiveTexId) {
                e.emissiveTex = x3::rhi::TextureHandle{ d.emissiveTexId };
                e.emissive[0] = 1.0f; e.emissive[1] = 1.0f; e.emissive[2] = 1.0f;
                e.emissive[3] = 1.1f;   // kit screens/strips glow (HDR-tuned, not pre-HDR 2x)
            }
            mul4(T, d.nodeTransform, e.transform);
            e.tag = (uint32_t)x3::game::Tag::Prop;
            scene.add(e);
            ++placed;
        }
        m_models.push_back(std::move(model));
        m_entities += placed;
        return placed;
    };

    // Track the console screen entity for interactivity: remember the NEXT scene
    // id before placing, then record the station fixture.
    auto placeStation = [&](const Station& st) {
        const float yaw = st.yaw;
        // Console faces the station yaw; chair behind the helm.
        const uint32_t before = scene.size();
        place("Console_Small_01.glb", yaw, 1.0f, st.pos[0], st.pos[1] + 0.06f, st.pos[2]);
        ArtStation a;
        a.kind = st.kind;
        a.pos[0] = st.pos[0]; a.pos[1] = st.pos[1]; a.pos[2] = st.pos[2];
        a.screenEntity = (before < scene.size()) ? before : UINT32_MAX;
        m_stations.push_back(a);
    };

    // ================= COCKPIT (6 x 3 x 6 @ origin) =================
    // Deck: one 6x6 slab (top at y=0 in the GLB, min corner at (-6,0)); scale
    // 1.0 places its top at +0.06 for a thin raised deck over the graybox floor.
    place("Floor_01.glb", 0.0f, 1.0f, +3.0f, 0.06f, -3.0f);
    // Aft cockpit wall cladding (z=+3, door in the middle): the doorframe piece.
    place("Wall_Door_Simple_01.glb", 1.5708f, 0.60f, 0.0f, 0.02f, 2.85f);
    // Starboard wall (x=+3): computer wall (length Z fits the 6 m wall at 0.9).
    place("Interior_Wall_Computer_01.glb", 0.0f, 0.75f, 2.65f, 0.02f, 0.0f);
    // Port wall (x=-3) carries the side WINDOW at z=0: clad the two flanks with
    // short pipe-wall segments so the portal aperture stays open.
    place("Interior_Wall_Pipes_01.glb", 0.0f, 0.42f, -2.78f, 0.02f, -2.1f);
    place("Interior_Wall_Pipes_01.glb", 0.0f, 0.42f, -2.78f, 0.02f, +2.1f);
    // (Forward wall z=-3 deliberately UNCLAD: the S6 portal dominates it.)
    // Ceiling vent + a status screen over the computer wall.
    place("Air_Grid_01.glb", 0.0f, 0.6f, 0.0f, 2.92f, 0.0f);
    place("Panel_Screen_01.glb", -1.5708f, 0.7f, 2.55f, 2.05f, -1.6f);
    // Helm chair (the pilot sits here; console placed by placeStation below).
    place("Chair_01.glb", 3.14159f, 0.9f, 0.0f, 0.06f, -1.5f);

    // Stations (helm + nav consoles with interactable screens).
    for (const auto& st : manifest.stations) placeStation(st);

    // ================= CORRIDOR (3 x 3 x 5, z 3..8) =================
    // Deck: one 6x6 slab scaled 0.5 covers 3x3; two tiles cover z 3..8 (overlap ok).
    place("Floor_01.glb", 0.0f, 0.5f, +1.5f, 0.06f, 3.0f);
    place("Floor_01.glb", 0.0f, 0.5f, +1.5f, 0.06f, 5.5f);
    // Port side (x=-1.5): crew BUNK wall.
    place("Interior_Wall_Bed_01.glb", 0.0f, 0.58f, -1.28f, 0.02f, 5.2f);
    // Starboard side (x=+1.5): LOCKER (case) wall.
    place("Interior_Wall_Case_01.glb", 3.14159f, 0.56f, 1.30f, 0.02f, 5.2f);
    // Aft end (z=8): the GALLEY (kitchen) module against the aft wall (tightened
    // 0.72 -> 0.60 + recentered after the walkable check — the corridor is 3 m
    // wide and the first pass crowded the walkway).
    place("Interior_Kitchen_01.glb", -1.5708f, 0.60f, -0.9f, 0.02f, 7.85f);
    // Props: a small oxygen bottle + barrel tucked into the aft-starboard corner.
    place("Big_Oxygen_Tank_01.glb", 0.0f, 0.11f, 1.15f, 0.06f, 7.65f);
    place("Barrel_01.glb", 0.0f, 0.6f, 1.15f, 0.06f, 6.9f);
    // Corridor ceiling vent + a TV screen over the bunks.
    place("Air_Grid_01.glb", 0.0f, 0.5f, 0.0f, 2.92f, 5.5f);
    place("Articulated_TV_Screen_01.glb", 1.5708f, 0.36f, -1.22f, 2.15f, 5.6f);

    x3::logInfo("[shipart] placed " + std::to_string(m_entities) + " art entit(ies), "
                + std::to_string(m_stations.size()) + " interactable station(s)");
    return m_entities;
}

void ShipInteriorArt::shutdown() {
    if (m_loader) for (auto& m : m_models) if (m.ok) m_loader->unload(m);
    m_models.clear();
    if (m_device && m_mrShared.valid()) m_device->destroyTexture(m_mrShared);
    m_stations.clear();
    m_entities = 0;
}

} // namespace x3::space
