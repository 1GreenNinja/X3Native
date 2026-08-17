// THE JETPACK — pack visuals + thrust FX. See jetpack.h for the contract and
// the armory provenance (composed from the two textured kit props because the
// 914-package catalog ships no dedicated jetpack).

#include "jetpack.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace x3::game {

namespace {

// ---------------------------------------------------------------------------
// PACK LAYOUT, in spine-bone space (metres). mixamorigSpine2 sits mid-chest;
// its bone axes were VERIFIED BY RENDER (X3_WORLD_RULES rule 0, the
// X3_SHOT_JETPACK captures): +Y runs up the spine, +Z points out the
// character's BACK (post tools/jake_bake.py bake). The pack hangs on +Z.
// ---------------------------------------------------------------------------
constexpr float kPlateOut   = 0.10f;   // backplate face off the spine
constexpr float kTankOut    = 0.17f;   // tank centreline off the spine
constexpr float kTankSide   = 0.105f;  // tank centreline off the spine, sideways
constexpr float kTankBotY   = -0.30f;  // tank BASE below the spine point
constexpr float kTankSclXZ  = 0.019f;  // 10.9 m drum -> 0.21 m tank diameter
constexpr float kTankSclY   = 0.045f;  // 12.0 m drum -> 0.54 m tank height
constexpr float kNozzleY    = -0.31f;  // nozzle mount under the tank base
constexpr float kNozzleScl  = 0.20f;   // 0.66 m vent -> 0.13 m nozzle mouth
constexpr float kNozzleLen  = 0.16f;   // vent depth 1.0 m -> 0.16 m bell
constexpr float kMouthY     = -0.50f;  // FX anchor: the nozzle mouth

// Column-major helpers (the character_anim/thirdperson matrix convention).
void matIdentity(float m[16]) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b (column-major 4x4) — local copy of asset::mulMat4's math so the
// pack does not pull the loader header for one multiply.
void matMul(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] +
                             a[1 * 4 + row] * b[c * 4 + 1] +
                             a[2 * 4 + row] * b[c * 4 + 2] +
                             a[3 * 4 + row] * b[c * 4 + 3];
    std::memcpy(out, r, sizeof(r));
}

// T * Rx(deg) * S, column-major. Rx is all the pack needs (nozzles point
// their +Z depth straight down = -90 about X).
void matTRS(float tx, float ty, float tz, float rxDeg,
            float sx, float sy, float sz, float out[16]) {
    const float a = rxDeg * 0.01745329252f;
    const float c = std::cos(a), s = std::sin(a);
    // col0
    out[0] = sx;  out[1] = 0.0f;      out[2] = 0.0f;     out[3] = 0.0f;
    // col1 (rotated about X)
    out[4] = 0.0f; out[5] = c * sy;   out[6] = s * sy;   out[7] = 0.0f;
    // col2
    out[8] = 0.0f; out[9] = -s * sz;  out[10] = c * sz;  out[11] = 0.0f;
    // col3
    out[12] = tx; out[13] = ty; out[14] = tz; out[15] = 1.0f;
}

void xformPoint(const float m[16], float x, float y, float z, float out[3]) {
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

void xformDir(const float m[16], float x, float y, float z, float out[3]) {
    out[0] = m[0] * x + m[4] * y + m[8]  * z;
    out[1] = m[1] * x + m[5] * y + m[9]  * z;
    out[2] = m[2] * x + m[6] * y + m[10] * z;
    const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (l > 1e-6f) { out[0] /= l; out[1] /= l; out[2] /= l; }
}

} // namespace

// ---------------------------------------------------------------------------
// Loading (the character_anim::load recipe: mount, load, makeDrawables).
// ---------------------------------------------------------------------------
bool JetpackRig::loadPiece(x3::rhi::IRenderDevice& device, const std::string& dir,
                           const std::string& file, Piece& out) {
    out.src.reset(x3::asset::createAssetSource());
    if (!out.src || !out.src->mountDir(dir, 0)) {
        x3::logWarn("[jetpack] mountDir failed: " + dir);
        return false;
    }
    out.loader.reset(x3::asset::createModelLoader(&device, out.src.get()));
    out.model = out.loader->load(file);
    if (!out.model.ok) {
        x3::logWarn("[jetpack] " + file + " failed to load");
        return false;
    }
    out.draw = x3::asset::makeDrawables(out.model);
    out.ok = !out.draw.empty();
    return out.ok;
}

bool JetpackRig::load(x3::rhi::IRenderDevice& device,
                      const std::string& convertedGlbRoot) {
    if (m_loaded) return true;
    const bool tank = loadPiece(device, convertedGlbRoot + "/SciFiKit3",
                                "Big_Oxygen_Tank_01.glb", m_tank);
    const bool vent = loadPiece(device, convertedGlbRoot + "/SciFi_Warehouse_Kit",
                                "Duct Vent.glb", m_vent);
    // BOTH or nothing: a pack with missing pieces is a stand-in (NO_SLOP 3).
    m_loaded = tank && vent;
    x3::logInfo(std::string("[jetpack] pack ") +
                (m_loaded ? "loaded (tanks + housing, textured kit pieces)"
                          : "NOT loaded — flying without a visible pack is held"));
    return m_loaded;
}

void JetpackRig::drawPiece(const x3::rhi::FrameContext& frame,
                           x3::rhi::IRenderDevice& device, const Piece& piece,
                           const float world[16]) {
    for (const x3::asset::ModelDrawable& d : piece.draw) {
        const float bc[4]   = { d.baseColorFactor[0], d.baseColorFactor[1],
                                d.baseColorFactor[2], d.baseColorFactor[3] };
        const float emis[3] = { d.emissiveFactor[0], d.emissiveFactor[1],
                                d.emissiveFactor[2] };
        device.drawMeshPBR(frame,
            x3::rhi::MeshHandle{ d.meshId },
            x3::rhi::TextureHandle{ d.baseColorTexId },
            x3::rhi::TextureHandle{ d.normalTexId },
            x3::rhi::TextureHandle{ d.mrTexId },
            bc, emis, world, d.alphaMask, d.alphaBlend,
            x3::rhi::TextureHandle{ d.emissiveTexId },
            x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
            d.clearcoat, d.clearcoatRough);
    }
}

void JetpackRig::draw(const x3::rhi::FrameContext& frame,
                      x3::rhi::IRenderDevice& device,
                      const float spineWorld[16]) {
    if (!m_loaded) return;
    float local[16], world[16];

    // Backplate: the vent box flattened against the back, flanges out.
    matTRS(0.0f, -0.16f, kPlateOut, 0.0f, 0.52f, 0.50f, 0.10f, local);
    matMul(spineWorld, local, world);
    drawPiece(frame, device, m_vent, world);

    // Twin tanks (origin at the drum BASE — rule 4 keeps this easy).
    for (int side = 0; side < 2; ++side) {
        const float sx = (side == 0) ? -kTankSide : kTankSide;
        matTRS(sx, kTankBotY, kTankOut, 0.0f,
               kTankSclXZ, kTankSclY, kTankSclXZ, local);
        matMul(spineWorld, local, world);
        drawPiece(frame, device, m_tank, world);
    }

    // Nozzles: the vent again, small, depth rotated straight DOWN.
    for (int side = 0; side < 2; ++side) {
        const float sx = (side == 0) ? -kTankSide : kTankSide;
        matTRS(sx, kNozzleY, kTankOut, -90.0f,
               kNozzleScl, kNozzleScl, kNozzleLen, local);
        matMul(spineWorld, local, world);
        drawPiece(frame, device, m_vent, world);
        xformPoint(spineWorld, sx, kMouthY, kTankOut, m_nozzle[side]);
    }
    xformDir(spineWorld, 0.0f, -1.0f, 0.0f, m_plumeDir);
    m_haveNozzles = true;
}

// ---------------------------------------------------------------------------
// Thrust FX. Additive core plume (glow floor — only ever ADDS light,
// X3_WORLD_RULES rule 5) + a thin alpha haze tail. Deterministic LCG jitter
// (the precip_fx discipline — no rand()).
// ---------------------------------------------------------------------------
void JetpackRig::submitThrustFx(x3::rhi::IRenderDevice& device, float dt,
                                float thrust, const float vel[3]) {
    if (!m_haveNozzles) return;
    thrust = std::clamp(thrust, 0.0f, 1.0f);

    // Spawn. ~220 puffs/s at full thrust across both nozzles, 70/30 core/haze.
    if (thrust > 0.02f) {
        m_spawnAcc += dt * (40.0f + 180.0f * thrust);
        while (m_spawnAcc >= 1.0f && m_puffs.size() < 480) {
            m_spawnAcc -= 1.0f;
            m_seed = m_seed * 1664525u + 1013904223u;
            const uint32_t h = m_seed;
            const int side = (h >> 30) & 1;
            const float jx = (((h >> 3)  & 255) / 255.0f - 0.5f) * 0.05f;
            const float jy = (((h >> 11) & 255) / 255.0f - 0.5f) * 0.05f;
            const float jz = (((h >> 19) & 255) / 255.0f - 0.5f) * 0.05f;
            const bool haze = ((h >> 27) & 7) < 2;
            Puff p{};
            p.x = m_nozzle[side][0] + jx;
            p.y = m_nozzle[side][1] + jy;
            p.z = m_nozzle[side][2] + jz;
            // Plume: driven out the nozzle, inheriting a share of the wearer's
            // velocity so the trail lies back honestly at speed.
            const float eject = haze ? 4.5f : 9.0f;
            p.vx = m_plumeDir[0] * eject + vel[0] * 0.35f + jx * 8.0f;
            p.vy = m_plumeDir[1] * eject + vel[1] * 0.35f + jy * 8.0f;
            p.vz = m_plumeDir[2] * eject + vel[2] * 0.35f + jz * 8.0f;
            p.age = 0.0f;
            p.life  = haze ? 0.70f : 0.26f;
            p.size0 = haze ? 0.16f : 0.09f;
            p.size1 = haze ? 0.85f : 0.34f;
            p.kind  = haze ? 1 : 0;
            m_puffs.push_back(p);
        }
    } else {
        m_spawnAcc = 0.0f;
    }

    // Age + integrate + emit.
    static std::vector<x3::rhi::IRenderDevice::ParticleInstance> addOut, alphaOut;
    addOut.clear(); alphaOut.clear();
    size_t w = 0;
    for (size_t i = 0; i < m_puffs.size(); ++i) {
        Puff p = m_puffs[i];
        p.age += dt;
        if (p.age >= p.life) continue;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        p.vy += 1.6f * dt;                      // hot gas: a little buoyancy
        m_puffs[w++] = p;

        const float t = p.age / p.life;         // 0 -> 1
        x3::rhi::IRenderDevice::ParticleInstance pi;
        pi.pos[0] = p.x; pi.pos[1] = p.y; pi.pos[2] = p.z;
        pi.size = p.size0 + (p.size1 - p.size0) * t;
        if (p.kind == 0) {
            // Hot core: white heart cooling to electric blue down the plume.
            const float heat = 1.0f - t;
            pi.color[0] = 0.55f + 0.75f * heat;
            pi.color[1] = 0.70f + 0.60f * heat;
            pi.color[2] = 1.05f + 0.25f * heat;
            pi.color[3] = 0.50f * heat * (0.35f + 0.65f * thrust);
            addOut.push_back(pi);
        } else {
            pi.color[0] = 0.72f; pi.color[1] = 0.76f; pi.color[2] = 0.82f;
            pi.color[3] = 0.10f * (1.0f - t) * (1.0f - t);
            alphaOut.push_back(pi);
        }
    }
    m_puffs.resize(w);

    if (!addOut.empty())
        device.submitParticles(addOut.data(), (uint32_t)addOut.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Additive);
    if (!alphaOut.empty())
        device.submitParticles(alphaOut.data(), (uint32_t)alphaOut.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Alpha);
}

} // namespace x3::game
