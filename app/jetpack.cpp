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
constexpr float kTankSide   = 0.120f;  // tank centreline off the spine, sideways
// Tank base 0.42 below the spine point, not 0.30: at 0.30 the 0.54 m drums
// topped out level with the back of his NECK (eyes-on, 21_jetpack_pack.png) —
// a pack rides between the shoulder blades and the waist, not on the collar.
constexpr float kTankBotY   = -0.42f;  // tank BASE below the spine point
constexpr float kTankSclXZ  = 0.019f;  // 10.9 m drum -> 0.21 m tank diameter
constexpr float kTankSclY   = 0.045f;  // 12.0 m drum -> 0.54 m tank height
// PAIRED WITH kTankBotY (NO_SLOP rule 4): the nozzles hang off the BOTTOM of
// the tanks and the FX anchor is the nozzle's mouth, so moving the tanks moves
// both of these or the plume fires out of his spine. kNozzleY == kTankBotY +
// 0.02 (just inside the drum base), kMouthY == kNozzleY - kNozzleLen.
constexpr float kNozzleY    = -0.40f;  // nozzle mount under the tank base
constexpr float kNozzleScl  = 0.20f;   // 0.66 m vent -> 0.13 m nozzle mouth
constexpr float kNozzleLen  = 0.16f;   // vent depth 1.0 m -> 0.16 m bell
constexpr float kMouthY     = -0.56f;  // FX anchor: the nozzle mouth

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
            d.clearcoat, d.clearcoatRough,
            /*selfLight=*/0.0f, /*metallicScale=*/1.0f,
            /*foliage=*/0.0f, d.metallicFactor, d.roughnessFactor);
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

    // Spawn rate. EYES-ON FIX (the first X3_SHOT_JETPACK capture): at 220/s the
    // plume read as four detached white beads twenty metres behind his boots,
    // not as thrust. Two separate causes, both fixed here:
    //   (1) DENSITY. A 300 mph plume is stretched over ~18 m of trail. 220/s
    //       over a 0.26 s life is ~57 puffs spread across that — beads. The
    //       rate is now speed-aware: the faster the wearer, the longer the
    //       trail, the more puffs it takes to stay CONTINUOUS.
    //   (2) INHERITANCE — see the eject block below.
    if (thrust > 0.02f) {
        const float wearerSpd = std::sqrt(vel[0]*vel[0] + vel[1]*vel[1] + vel[2]*vel[2]);
        const float stretch   = 1.0f + wearerSpd / 34.0f;   // 1x at rest, ~5x at 300 mph
        m_spawnAcc += dt * (90.0f + 320.0f * thrust) * stretch;
        // SUB-FRAME BIRTH TIMES — the second eyes-on fix. Full inheritance
        // alone turned the beads into a STRING of beads: every puff spawned in
        // one frame is born at the same point with the same velocity, so it
        // travels as one comoving clump, and the pack lays down exactly one
        // clump per frame — at 134 m/s that is a blob every 2.2 m, which is
        // precisely the dotted line the capture showed. Each puff is now given
        // a birth time spread across the frame: it is back-dated along the
        // pack's own path and pre-aged, so the frame's worth of gas comes out
        // as a continuous ribbon instead of a pellet.
        const int nSpawn = (int)m_spawnAcc;
        int spawnIdx = 0;
        while (m_spawnAcc >= 1.0f && m_puffs.size() < 2600) {
            m_spawnAcc -= 1.0f;
            // a0 = seconds this puff has ALREADY existed by the end of the
            // frame (idx 0 is the oldest, born at the top of the frame).
            const float a0 = (nSpawn > 0)
                ? dt * (1.0f - (float)spawnIdx / (float)nSpawn) : 0.0f;
            ++spawnIdx;
            m_seed = m_seed * 1664525u + 1013904223u;
            const uint32_t h = m_seed;
            const int side = (h >> 30) & 1;
            const float jx = (((h >> 3)  & 255) / 255.0f - 0.5f) * 0.05f;
            const float jy = (((h >> 11) & 255) / 255.0f - 0.5f) * 0.05f;
            const float jz = (((h >> 19) & 255) / 255.0f - 0.5f) * 0.05f;
            const bool haze = ((h >> 27) & 7) < 4;   // half haze: it IS the visible trail
            Puff p{};
            p.x = m_nozzle[side][0] + jx;
            p.y = m_nozzle[side][1] + jy;
            p.z = m_nozzle[side][2] + jz;
            // EJECT + INHERITANCE. Gas leaves the nozzle fast RELATIVE TO THE
            // PACK, and at the instant it leaves it is still carrying the
            // pack's full forward momentum — the air then drags it back. The
            // first cut inherited only 0.35 of the wearer's velocity, which at
            // 134 m/s means every puff was born already moving 87 m/s BACKWARD
            // relative to him: it left the nozzle and was 22 m astern a
            // quarter-second later, so the plume never touched his boots. Full
            // NEAR-full inheritance + the drag in the integrator below is both
            // the honest physics and the thing that makes the cone start AT the
            // nozzle. Not 1.0: at exactly 1.0 the gas is perfectly comoving and
            // only drag ever separates it, which is what let the per-frame
            // clumps survive as beads. kInherit leaves ~12% of airspeed as
            // relative slip — a few metres of trail over a puff's life, dense
            // and attached, instead of either a 22 m dotted line (0.35) or a
            // rigid clump (1.0).
            constexpr float kInherit = 0.88f;
            const float eject = haze ? 5.0f : 13.0f;
            p.vx = m_plumeDir[0] * eject + vel[0] * kInherit + jx * 8.0f;
            p.vy = m_plumeDir[1] * eject + vel[1] * kInherit + jy * 8.0f;
            p.vz = m_plumeDir[2] * eject + vel[2] * kInherit + jz * 8.0f;
            // Back-date to its sub-frame birth: the pack was at (nozzle -
            // vel*a0) when this puff left, and the puff has flown p.v for a0.
            p.x += (p.vx - vel[0]) * a0;
            p.y += (p.vy - vel[1]) * a0;
            p.z += (p.vz - vel[2]) * a0;
            p.age = a0;
            // TWO POPULATIONS, and they do different jobs. Three captures
            // were spent trying to make ONE population read as thrust, and it
            // never will: a 300 mph plume is 4-5 m long, and any puff big
            // enough to be seen at that length resolves as its own ball from a
            // 3 m chase camera (that is the dotted line in every earlier
            // shot). So:
            //   CORE — short-lived, small, BRIGHT: a tight flame that sits ON
            //     the nozzles and never gets long enough to bead.
            //   HAZE — long-lived, LARGE and very faint: neighbouring puffs are
            //     laid down 0.27 m apart and are 1.8 m across by the end, so
            //     they overlap many-deep and integrate into a smooth contrail
            //     instead of a row of dots. Faint is what makes overlap read as
            //     density rather than as a white wall.
            // Core life 0.10 s, not 0.18: at 0.18 the flame was still long
            // enough to string 3 m of individually-resolvable white dots down
            // behind his boots (they read as sparks, not thrust). At 0.10 s the
            // core cannot travel far enough to separate — it is a compact
            // flame AT the nozzles — and the length of the plume is the haze's
            // job, which is the population built to do it.
            p.life  = haze ? 1.20f : 0.10f;
            p.size0 = haze ? 0.25f : 0.06f;
            p.size1 = haze ? 1.80f : 0.15f;
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
        // AIR DRAG — the other half of the inheritance fix above. The puff is
        // born with the wearer's full velocity and the air bleeds it, so the
        // plume STARTS at the nozzle and is progressively left behind instead
        // of being born already astern. Exponential, so it is dt-correct.
        const float keep = std::exp(-3.2f * dt);
        p.vx *= keep; p.vy *= keep; p.vz *= keep;
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
            pi.color[3] = 0.42f * heat * (0.35f + 0.65f * thrust);
            addOut.push_back(pi);
        } else {
            pi.color[0] = 0.72f; pi.color[1] = 0.76f; pi.color[2] = 0.82f;
            pi.color[3] = 0.035f * (1.0f - t) * (1.0f - t);
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
