#pragma once
// GOD RAYS (W10 underwater beauty) — shafts of sunlight falling from the water
// surface, the companion piece to the mesh.frag caustics (setCaustics).
//
// Each shaft is the street-light-cone trick turned on the water: a CROSSED
// PAIR of tapered quads (both windings, so it reads from every angle) hanging
// from just under the surface, drawn through the glass pass's ADDITIVE GLOW
// mode (GlassMaterial::additive — rides the BLEND tail post-24371e2, so it
// never writes depth and never punches a hole in anything). The falloff is
// baked into a 2D gradient (bright at the surface, dissolved to nothing by
// ~75% of the drop; soft across the width so a quad edge never reads as a
// plank edge), the soft silhouette comes from the shader's view-angle rim
// fade, and the whole shaft leans down-sun so the light falls the way the
// sun actually enters the water.
//
// PLACEMENT is deterministic (one LCG, the fish/crowd doctrine): a handful
// along the river reach nearest the facility (the same worldRiverNodes spline
// the fish schools seed on) + a loose cluster in the estuary/sea shallows —
// only where the player can actually be underwater. Shaft length is capped by
// the real water depth (bed query), so no ray ever pokes through the bed.
//
// LIFE: update() gives each shaft a slow alpha BREATHING and a slow surface
// DRIFT (dt-scaled, per-shaft phase), so the light feels alive without ever
// strobing. Brightness scales with the host's sun elevation/intensity at
// build (sunScale): the rays die with the sun — dusk water keeps no shafts.
//
// COST: ~16 shafts x 8 tris of additive glass + one 64x64 gradient. The
// entities are PVS-gated on the streamed-exterior room like the fish.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::game {

class GodRays {
public:
    struct Config {
        uint32_t roomId = kNoRoom;       // PVS room (kStreamedExteriorRoom)
        // Facility/tower center — selects the river reach (the fish doctrine:
        // shafts fall over the same nearest-node window the schools seed on).
        float nearX = 0.0f, nearZ = 0.0f;
        // Direction TOWARD the sun (the sky's sunDir) — shafts lean down-sun.
        float sunDirX = 0.55f, sunDirY = 0.16f, sunDirZ = -0.35f;
        // Brightness master from sun elevation * intensity (0 at dusk -> the
        // rays vanish with the sun; ~1+ under the --day staging sky).
        float sunScale = 1.0f;
    };

    // Build the shafts (canon world boot, after the terrain/water exist).
    void build(const Config& cfg, Scene& scene, x3::rhi::IRenderDevice& device);

    // Per-frame: slow drift + alpha breathing (dt-scaled; cheap).
    void update(float dt, Scene& scene);

    bool     built() const { return m_built; }
    uint32_t shaftCount() const { return (uint32_t)m_shafts.size(); }

private:
    struct Shaft {
        SceneHandle ent;
        float x = 0, z = 0;          // authored surface anchor (drift center)
        float topY = 0;              // just under the water surface
        float yaw = 0;               // faces the down-sun azimuth
        float baseStrength = 1.0f;   // emissive strength before breathing
        float phase = 0;             // per-shaft breathing/drift phase
    };
    void addShaft(Scene& scene, x3::rhi::IRenderDevice& device,
                  float x, float z, uint32_t seed);

    std::vector<Shaft>     m_shafts;
    x3::rhi::TextureHandle m_grad;      // 2D falloff bake (axial * width)
    float                  m_time = 0.0f;
    float                  m_sunScale = 1.0f;
    uint32_t               m_roomId = kNoRoom;
    float                  m_leanX = 0.0f, m_leanZ = 0.0f;  // down-sun lean per meter of drop
    bool                   m_built = false;
};

} // namespace x3::game
