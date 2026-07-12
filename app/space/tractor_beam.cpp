// Tractor-beam VFX -- see app/space/tractor_beam.h.
//
// ART-DIRECTION RETUNE (owner, 2026-07-11): "Tractor beam is too cartoony. Think of
// how this looks in real TV shows. It's a faint, blue beam, shimmering and pulsing
// with power, but not a solid rendered object." The old look (a solid striped cyan
// cone drawn opaque through drawMeshEmissive) is replaced by a FAINT, TRANSLUCENT,
// blue-white beam the scene reads straight through, alive with subtle motion.
//
// Implementation notes:
//   - TRANSLUCENCY: the two cones + the two glow orbs are drawn through
//     drawMeshPBR(..., alphaBlend=true). The device sets the per-object BLEND bit,
//     partitions the draw into the transparent batch (AFTER the opaques -- the
//     S6/glass ordering law, no host ordering needed), and mesh.frag honors the
//     authored baseColorFactor.a LITERALLY when it lands in the NEAR-CLEAR
//     canopy-glass band (0, 0.07): outA = a + a faint fresnel edge term. The outer
//     haze rides ~0.05 (deep in the near-clear band); the inner core ~0.10 (the
//     legacy mostly-see-through mapping -- fine for a firmer core). Result: the
//     scene is visible THROUGH the beam.
//   - STRUCTURE: two NESTED unit cones authored along +Z (apex/emitter z=0, base/
//     capture z=1), each re-oriented + stretched onto the live from->to axis per
//     frame. The HAZE cone is `hazeScale`x wider than the CORE cone. Both windings
//     are emitted so the surface survives back-face cull from any camera angle.
//   - GLOW: brightness rides emissiveTex (texture-gated -- the durable ACES recipe)
//     at MODEST strength over a near-black baseColor rgb, so the beam GLOWS faintly
//     instead of flat-filling to a lit-plastic slab. Emissive color is faint
//     blue-white (never saturated cyan candy).
//   - TEXTURES: (core) a LOW-CONTRAST longitudinal falloff -- brightest at the
//     emitter, feathering to near-nothing at the captured end -- with 2-3 faint
//     bands + a whisper of noise. (haze) a TILEABLE-in-V band field (the power
//     pulses) + noise; roughly uniform average so a continuous V-pan reads seamless.
//   - MOTION: (a) PULSE -- the haze cone's UV V is panned along the beam each frame
//     (updateMesh; the REPEAT sampler wraps a raw unbounded V with no seam) so the
//     bands flow emitter->capture. (b) BREATHE -- the per-draw emissive strength is
//     modulated +/-18% on a slow ~0.7 Hz sine + a tiny ~6 Hz +/-5% flicker; the
//     core breathes slightly out of phase with the haze. `intensity` (0..1) is the
//     lock-on ramp scaling overall brightness.

#include "tractor_beam.h"

#include "engine/core/x3_log.h"
#include "../headless_device.h"   // HeadlessRenderDevice for the self-test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::space {

namespace {

constexpr float kTau = 6.28318530717959f;

// Tileable hash noise in [0,1) over an n-cell field (wraps at n so a V-pan seams).
static float hashNoise(uint32_t x, uint32_t y, uint32_t n) {
    uint32_t h = (x % n) * 374761393u + (y % n) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}

// ---- A thin cone tube authored along +Z ------------------------------------
// apex (emitter) at z=0 radius r0, base (capture) at z=1 radius r1. `slices` panes
// around, `rings` axial segments. Both windings so it survives BACK_BIT cull from
// any angle. UV: u = angle [0,1] around; v = z [0,1] (0 emitter -> 1 capture).
struct ConeMesh {
    std::vector<rhi::MeshVertex> verts;
    std::vector<uint32_t>        index;
};

static ConeMesh buildCone(float r0, float r1, uint32_t slices, uint32_t rings) {
    ConeMesh m;
    slices = std::max<uint32_t>(slices, 3u);
    rings  = std::max<uint32_t>(rings, 1u);
    m.verts.reserve((size_t)slices * rings * 4);
    m.index.reserve((size_t)slices * rings * 12);

    for (uint32_t r = 0; r < rings; ++r) {
        float v0 = (float)r / (float)rings;
        float v1 = (float)(r + 1) / (float)rings;
        float rad0 = r0 + (r1 - r0) * v0;
        float rad1 = r0 + (r1 - r0) * v1;
        for (uint32_t f = 0; f < slices; ++f) {
            float a0 = (float)f / (float)slices * kTau;
            float a1 = (float)(f + 1) / (float)slices * kTau;
            float ca0 = std::cos(a0), sa0 = std::sin(a0);
            float ca1 = std::cos(a1), sa1 = std::sin(a1);
            float p00[3] = { ca0 * rad0, sa0 * rad0, v0 };
            float p10[3] = { ca1 * rad0, sa1 * rad0, v0 };
            float p11[3] = { ca1 * rad1, sa1 * rad1, v1 };
            float p01[3] = { ca0 * rad1, sa0 * rad1, v1 };
            float am = 0.5f * (a0 + a1);
            float nx = std::cos(am), ny = std::sin(am), nz = 0.0f;
            float u0 = (float)f / (float)slices;
            float u1 = (float)(f + 1) / (float)slices;
            auto push = [&](const float* p, float uu, float vv) {
                rhi::MeshVertex vx{};
                vx.pos[0] = p[0]; vx.pos[1] = p[1]; vx.pos[2] = p[2];
                vx.normal[0] = nx; vx.normal[1] = ny; vx.normal[2] = nz;
                vx.uv[0] = uu; vx.uv[1] = vv;
                m.verts.push_back(vx);
            };
            uint32_t base = (uint32_t)m.verts.size();
            push(p00, u0, v0);
            push(p10, u1, v0);
            push(p11, u1, v1);
            push(p01, u0, v1);
            // SINGLE winding: the transparent pipeline is cull-NONE, so one winding
            // already reads from inside AND outside. Emitting both would double the
            // alpha stack at the silhouette -> a hard tube-mouth rim (the very "solid
            // rendered object" the retune kills). One winding keeps the edge soft.
            m.index.push_back(base + 0); m.index.push_back(base + 1); m.index.push_back(base + 2);
            m.index.push_back(base + 0); m.index.push_back(base + 2); m.index.push_back(base + 3);
        }
    }
    return m;
}

// ---- A small unit-radius UV sphere (the emitter / impact glow orbs) ---------
// Both windings so the translucent shell reads from any angle.
static ConeMesh buildSphere(uint32_t stacks, uint32_t slices) {
    ConeMesh m;
    stacks = std::max<uint32_t>(stacks, 3u);
    slices = std::max<uint32_t>(slices, 3u);
    for (uint32_t st = 0; st < stacks; ++st) {
        float t0 = (float)st / (float)stacks;
        float t1 = (float)(st + 1) / (float)stacks;
        float ph0 = t0 * 3.14159265358979f, ph1 = t1 * 3.14159265358979f;
        for (uint32_t sl = 0; sl < slices; ++sl) {
            float a0 = (float)sl / (float)slices * kTau;
            float a1 = (float)(sl + 1) / (float)slices * kTau;
            auto sph = [](float phi, float az, float* o) {
                float sp = std::sin(phi), cp = std::cos(phi);
                o[0] = sp * std::cos(az); o[1] = cp; o[2] = sp * std::sin(az);
            };
            float p00[3], p10[3], p11[3], p01[3];
            sph(ph0, a0, p00); sph(ph0, a1, p10); sph(ph1, a1, p11); sph(ph1, a0, p01);
            auto push = [&](const float* p, float uu, float vv) {
                rhi::MeshVertex vx{};
                vx.pos[0] = p[0]; vx.pos[1] = p[1]; vx.pos[2] = p[2];
                vx.normal[0] = p[0]; vx.normal[1] = p[1]; vx.normal[2] = p[2];
                vx.uv[0] = uu; vx.uv[1] = vv;
                m.verts.push_back(vx);
            };
            uint32_t base = (uint32_t)m.verts.size();
            push(p00, (float)sl/slices, t0);
            push(p10, (float)(sl+1)/slices, t0);
            push(p11, (float)(sl+1)/slices, t1);
            push(p01, (float)sl/slices, t1);
            // Single winding (cull-NONE transparent pipeline) -> soft glow shell.
            m.index.push_back(base+0); m.index.push_back(base+1); m.index.push_back(base+2);
            m.index.push_back(base+0); m.index.push_back(base+2); m.index.push_back(base+3);
        }
    }
    return m;
}

// ---- CORE energy field: LONGITUDINAL FALLOFF -------------------------------
// Brightness at sAlong (0 emitter -> 1 capture): BRIGHTEST at the emitter,
// feathering to near-nothing at the captured end (the TV "power converging from the
// ship" read). Low contrast, with 2-3 faint bands so a soft pulse structure exists.
// Returns a scalar luminance in ~[0.1, 1.0]. Shared by the bake + the CPU test ref.
static float coreLumAlong(float sAlong, const TractorBeam::Tuning& t) {
    float s = std::clamp(sAlong, 0.0f, 1.0f);
    // Falloff: BRIGHTEST at the emitter, feathering toward the capture end -- but
    // held to a GLOW FLOOR (~0.4), never toward black. The beam is alpha-BLENDED
    // (src-over): a near-black far end would SUBTRACT from the brighter space bg (a
    // dark tinted cone). Staying above the background keeps it purely a faint glow.
    float falloff = 0.40f + 0.60f * std::pow(1.0f - s, 1.2f);
    // Faint bands (power pulses) -- LOW amplitude so there are no candy stripes.
    float bands = 0.90f + 0.10f * std::sin(s * kTau * t.ringDensity);
    return std::clamp(falloff * bands, 0.0f, 1.5f);
}

static std::vector<uint8_t> bakeCoreRGBA(uint32_t w, uint32_t h, const TractorBeam::Tuning& t) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        float s = (y + 0.5f) / (float)h;
        float lum = coreLumAlong(s, t);
        for (uint32_t x = 0; x < w; ++x) {
            float u = (x + 0.5f) / (float)w;
            // Whisper of noise + a faint azimuthal ripple -> shimmer, not stripes.
            float noise   = 0.94f + 0.06f * hashNoise(x, y, w);
            float ripple  = 0.96f + 0.04f * std::sin(u * kTau * 2.0f + s * 4.0f);
            float l = lum * noise * ripple;
            // Faint BLUE-WHITE: coreColor on the bright core, edgeColor as the wash.
            float r = t.edgeColor[0] * 0.35f + t.coreColor[0] * l;
            float g = t.edgeColor[1] * 0.35f + t.coreColor[1] * l;
            float b = t.edgeColor[2] * 0.35f + t.coreColor[2] * l;
            auto u8 = [](float c) { c = std::clamp(c, 0.0f, 1.0f);
                                    return (uint8_t)std::lround(c * 255.0f); };
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = u8(r); p[1] = u8(g); p[2] = u8(b); p[3] = 255;
        }
    }
    return px;
}

// ---- HAZE energy field: TILEABLE band pattern (scrolled) -------------------
// A low-contrast blue-white field that TILES seamlessly in V (all bands are
// integer-cycle sines; noise wraps mod h) so a continuous per-frame V-pan reads as
// power flowing down the beam with no seam. Roughly uniform average brightness --
// the emitter->capture falloff is carried by the CORE field + the emitter orb.
static std::vector<uint8_t> bakeHazeRGBA(uint32_t w, uint32_t h, const TractorBeam::Tuning& t) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    int nb = std::max(1, (int)std::lround(t.ringDensity));   // integer band count -> tiles
    for (uint32_t y = 0; y < h; ++y) {
        float v = (y + 0.5f) / (float)h;
        // Two integer-cycle bands, low amplitude -> soft flowing pulses, tileable.
        float bands = 0.60f
                    + 0.24f * (0.5f + 0.5f * std::sin(v * kTau * (float)nb))
                    + 0.16f * (0.5f + 0.5f * std::sin(v * kTau * (float)(nb + 1) + 1.3f));
        for (uint32_t x = 0; x < w; ++x) {
            float u = (x + 0.5f) / (float)w;
            float noise  = 0.92f + 0.08f * hashNoise(x, y, h);
            float ripple = 0.95f + 0.05f * std::sin(u * kTau * 2.0f);
            float l = bands * noise * ripple;
            float r = t.edgeColor[0] * 0.55f + t.coreColor[0] * l * 0.5f;
            float g = t.edgeColor[1] * 0.55f + t.coreColor[1] * l * 0.5f;
            float b = t.edgeColor[2] * 0.55f + t.coreColor[2] * l * 0.5f;
            auto u8 = [](float c) { c = std::clamp(c, 0.0f, 1.0f);
                                    return (uint8_t)std::lround(c * 255.0f); };
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = u8(r); p[1] = u8(g); p[2] = u8(b); p[3] = 255;
        }
    }
    return px;
}

// Compose a column-major model that maps the unit cone's +Z onto (to-from),
// stretched to the beam length, with the cross-section radii scaled by `radScale`.
// Returns false when the beam is degenerate (near-zero length). basis is stable
// even when the axis is (anti)parallel to world-up.
static bool beamModel(const float from[3], const float to[3], float radScale, float out[16]) {
    float ax = to[0] - from[0], ay = to[1] - from[1], az = to[2] - from[2];
    float len = std::sqrt(ax*ax + ay*ay + az*az);
    if (!(len > 1e-4f)) return false;
    float zx = ax/len, zy = ay/len, zz = az/len;
    float upx = 0.0f, upy = 1.0f, upz = 0.0f;
    if (std::fabs(zy) > 0.999f) { upx = 1.0f; upy = 0.0f; upz = 0.0f; }
    float xx = upy*zz - upz*zy, xy = upz*zx - upx*zz, xz = upx*zy - upy*zx;
    float xl = std::sqrt(xx*xx + xy*xy + xz*xz);
    if (!(xl > 1e-5f)) { xx = 1.0f; xy = 0.0f; xz = 0.0f; xl = 1.0f; }
    xx/=xl; xy/=xl; xz/=xl;
    float yx = zy*xz - zz*xy, yy = zz*xx - zx*xz, yz = zx*xy - zy*xx;
    out[0]=xx*radScale; out[1]=xy*radScale; out[2]=xz*radScale; out[3]=0.0f;
    out[4]=yx*radScale; out[5]=yy*radScale; out[6]=yz*radScale; out[7]=0.0f;
    out[8]=zx*len;      out[9]=zy*len;      out[10]=zz*len;     out[11]=0.0f;
    out[12]=from[0];    out[13]=from[1];    out[14]=from[2];    out[15]=1.0f;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
TractorBeam::Tuning clampTuning(const TractorBeam::Tuning& in) {
    TractorBeam::Tuning t = in;
    if (!(t.emitterRadius >= 0.0f)) t.emitterRadius = 0.0f;
    if (!(t.captureRadius  > 0.0f)) t.captureRadius = 0.1f;
    if (!(t.ringDensity   >= 1.0f)) t.ringDensity   = 1.0f;
    if (!(t.flowSpeed     >= 0.0f)) t.flowSpeed     = 0.0f;
    if (!(t.hazeScale     >= 1.0f)) t.hazeScale     = 1.0f;
    t.coreAlpha = std::clamp(t.coreAlpha, 0.005f, 0.95f);
    t.hazeAlpha = std::clamp(t.hazeAlpha, 0.005f, 0.95f);
    return t;
}

// CPU reference for the test: luminance of the baked CORE energy at sAlong.
float TractorBeam::sampleEnergyBrightness(float sAlong, const Tuning& t) {
    Tuning c = clampTuning(t);
    return coreLumAlong(sAlong, c);
}

// ---------------------------------------------------------------------------
void TractorBeam::init(rhi::IRenderDevice& dev, const Tuning& t) {
    if (m_initialized) return;
    const Tuning c = clampTuning(t);

    // Two nested unit cones (z in [0,1]); re-oriented onto the from->to axis each
    // frame. The haze is built at the CORE radii and widened per-frame by hazeScale
    // in the model matrix (so both share the authored taper). 40 slices keep the
    // silhouette smooth; the core carries more rings for the falloff, the haze fewer
    // (its UVs are re-uploaded every frame -- keep that cheap).
    // The thin CORE scrolls (its UVs are re-uploaded each frame) so it carries a
    // moderate ring count; the wide HAZE is static, so it can be smooth + cheap.
    ConeMesh core = buildCone(c.emitterRadius, c.captureRadius, /*slices=*/36, /*rings=*/40);
    m_meshCore = dev.createMesh(core.verts.data(), (uint32_t)core.verts.size(),
                                core.index.data(), (uint32_t)core.index.size());
    m_coreBaseVerts = core.verts;          // keep the un-panned UVs for the flow pan
    m_coreScratch   = core.verts;

    ConeMesh haze = buildCone(c.emitterRadius, c.captureRadius, /*slices=*/36, /*rings=*/40);
    m_meshHaze = dev.createMesh(haze.verts.data(), (uint32_t)haze.verts.size(),
                                haze.index.data(), (uint32_t)haze.index.size());

    ConeMesh orb = buildSphere(/*stacks=*/12, /*slices=*/16);
    m_meshOrb = dev.createMesh(orb.verts.data(), (uint32_t)orb.verts.size(),
                               orb.index.data(), (uint32_t)orb.index.size());

    // Linear storage (srgb=false): the emissive draw multiplies these texels by the
    // HDR emissive term. Core carries the falloff detail (higher V res).
    std::vector<uint8_t> coreTex = bakeCoreRGBA(/*w=*/128, /*h=*/512, c);
    m_texGrad = dev.createTexture(coreTex.data(), 128, 512, /*srgb=*/false);
    std::vector<uint8_t> hazeTex = bakeHazeRGBA(/*w=*/128, /*h=*/512, c);
    m_texBands = dev.createTexture(hazeTex.data(), 128, 512, /*srgb=*/false);

    m_alphaBlended = true;
    m_initialized  = m_meshCore.valid() && m_meshHaze.valid() && m_meshOrb.valid()
                  && m_texGrad.valid() && m_texBands.valid();
    m_lastTuning   = c;
    if (!m_initialized) x3::logError("TractorBeam::init: mesh or texture creation failed");
}

void TractorBeam::shutdown(rhi::IRenderDevice& dev) {
    if (m_meshCore.valid()) { dev.destroyMesh(m_meshCore); m_meshCore = rhi::MeshHandle{}; }
    if (m_meshHaze.valid()) { dev.destroyMesh(m_meshHaze); m_meshHaze = rhi::MeshHandle{}; }
    if (m_meshOrb.valid())  { dev.destroyMesh(m_meshOrb);  m_meshOrb  = rhi::MeshHandle{}; }
    if (m_texGrad.valid())  { dev.destroyTexture(m_texGrad);  m_texGrad  = rhi::TextureHandle{}; }
    if (m_texBands.valid()) { dev.destroyTexture(m_texBands); m_texBands = rhi::TextureHandle{}; }
    m_coreBaseVerts.clear();
    m_coreScratch.clear();
    m_initialized = false;
    m_lastStrength = 0.0f;
    m_lastHazeAlpha = 0.0f;
    m_lastCoreAlpha = 0.0f;
    m_lastDrawn = false;
}

// ---------------------------------------------------------------------------
void TractorBeam::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                         const float* viewProj16, const float from[3], const float to[3],
                         float intensity, float timeSec, const Tuning& t) {
    (void)viewProj16;
    m_lastDrawn = false;
    m_lastStrength = 0.0f;
    m_lastHazeAlpha = 0.0f;
    m_lastCoreAlpha = 0.0f;
    if (!m_initialized || !from || !to) return;

    const Tuning c = clampTuning(t);
    m_lastTuning = c;
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    m_lastIntensity = intensity;

    // Model matrices: core at authored radii, haze widened by hazeScale. beamModel
    // also serves as the degenerate guard (from==to -> skip everything, no NaN).
    float coreM[16], hazeM[16];
    if (!beamModel(from, to, 1.0f, coreM))         return;
    if (!beamModel(from, to, c.hazeScale, hazeM))  return;

    // BREATHE: a slow ~0.7 Hz sine (+/-18%) + a tiny ~6 Hz flicker (+/-5%) -> shimmer
    // without strobe. The core breathes slightly OUT OF PHASE with the haze so the
    // two layers never pulse as one flat mass.
    const float slowHaze = 1.0f + 0.18f * std::sin(timeSec * 0.7f * kTau);
    const float slowCore = 1.0f + 0.18f * std::sin(timeSec * 0.7f * kTau + 2.1f);
    const float flicker  = 1.0f + 0.05f * std::sin(timeSec * 6.0f * kTau);

    // Lock-on ramp: a faint targeting glow at intensity 0 that climbs to full lock.
    // Brightness rides the texture-gated emissive (MODEST -- a faint TV beam, not a
    // bloom bomb). Kept well under a flat-white slab.
    const float kBase = 1.6f;
    const float ramp  = 0.20f + 0.80f * intensity;
    const float coreStrength = kBase * ramp * slowCore * flicker;
    const float hazeStrength = kBase * 0.55f * ramp * slowHaze * flicker;
    m_lastStrength = coreStrength;

    // Near-black baseColor so the LIT term stays dark (all brightness is emissive).
    // .a is the translucency dial: haze deep in the near-clear canopy band, core a
    // touch firmer.
    const float hazeBase[4] = { 0.02f, 0.025f, 0.035f, c.hazeAlpha };
    const float coreBase[4] = { 0.02f, 0.025f, 0.035f, c.coreAlpha };
    const float coreEmis[4] = { c.coreColor[0], c.coreColor[1], c.coreColor[2], coreStrength };
    const float hazeEmis[4] = { c.coreColor[0], c.coreColor[1], c.coreColor[2], hazeStrength };
    m_lastHazeAlpha = c.hazeAlpha;
    m_lastCoreAlpha = c.coreAlpha;

    const rhi::TextureHandle noTex{};

    // ---- (1) WIDE OUTER HAZE cone: STATIC UV, longitudinal FALLOFF texture. ------
    // Brightest at the emitter, feathering to near-nothing at the capture end, so the
    // wide far mouth DIMS OUT instead of ringing a hard tube-edge (the "solid object"
    // read the retune kills). The gradient is tied to the geometry -> no scroll.
    dev.drawMeshPBR(fr, m_meshHaze, m_texGrad, noTex, noTex, hazeBase, hazeEmis, hazeM,
                    /*alphaMask*/false, /*alphaBlend*/true, /*emissiveTex*/m_texGrad);

    // ---- (2) THIN INNER CORE cone: PULSE the flow by panning its UV V each frame. -
    // The tileable band field scrolls emitter->capture (power flowing in). Raw
    // (unbounded) V offset; the REPEAT sampler wraps it with no seam. Re-upload the
    // panned copy (updateMesh -- the ship_windows portal pattern). The core is thin,
    // so its far mouth is small and reads as a bright thread, not a tube rim.
    const float vPan = timeSec * c.flowSpeed;   // units/sec toward the capture end
    if (m_coreScratch.size() == m_coreBaseVerts.size()) {
        for (size_t i = 0; i < m_coreBaseVerts.size(); ++i) {
            m_coreScratch[i] = m_coreBaseVerts[i];
            m_coreScratch[i].uv[1] = m_coreBaseVerts[i].uv[1] + vPan;
        }
        dev.updateMesh(m_meshCore, m_coreScratch.data(), (uint32_t)m_coreScratch.size());
    }
    dev.drawMeshPBR(fr, m_meshCore, m_texBands, noTex, noTex, coreBase, coreEmis, coreM,
                    /*alphaMask*/false, /*alphaBlend*/true, /*emissiveTex*/m_texBands);

    // ---- (3) EMITTER glow orb (bright) + (4) IMPACT orb (faint) -----------------
    // Small translucent glow shells that anchor the beam ends on TV. Same near-clear
    // recipe. The emitter orb is the brighter source; the impact orb is a soft
    // shimmer patch where the beam grabs the ship.
    auto drawOrb = [&](const float at[3], float radius, float strength, float alpha) {
        const float m[16] = {
            radius,0,0,0,  0,radius,0,0,  0,0,radius,0,  at[0],at[1],at[2],1.0f };
        const float base[4] = { 0.02f, 0.025f, 0.035f, alpha };
        const float emis[4] = { c.coreColor[0], c.coreColor[1], c.coreColor[2], strength };
        dev.drawMeshPBR(fr, m_meshOrb, m_texGrad, noTex, noTex, base, emis, m,
                        /*alphaMask*/false, /*alphaBlend*/true, /*emissiveTex*/m_texGrad);
    };
    const float emitR = std::max(0.55f, c.emitterRadius * 2.0f);
    const float impR  = std::max(0.8f, c.captureRadius * 0.7f);
    drawOrb(from, emitR, kBase * 0.9f * ramp * slowCore * flicker, 0.05f);
    drawOrb(to,   impR,  kBase * 0.5f * ramp * slowHaze * flicker, 0.035f);

    m_lastDrawn = true;
}

// ---------------------------------------------------------------------------
// --test-tractor: capital-ship TRACTOR BEAM VFX self-test (game intro capture
// beat). Headless -- exercises the TractorBeam init/render/shutdown lifecycle
// (leak-clean round-trip), intensity ramp/clamp, degenerate from==to skip, that the
// baked CORE energy field is a longitudinal gradient (EMITTER end brightest -- the
// 2026-07-11 art-direction retune), and that the beam is drawn TRANSLUCENT (the
// alpha-blend / near-clear canopy band) rather than the old solid cone.
// ---------------------------------------------------------------------------
bool runTractorSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
        else   {          x3::logError(std::string("  [FAIL] ") + name); }
    };

    x3::game::HeadlessRenderDevice hdev;
    const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    // T1: init + shutdown lifecycle is leak-clean.
    {
        TractorBeam tb;
        tb.init(hdev);
        check(tb.initialized() && tb.mesh().valid() && tb.hazeMesh().valid()
              && tb.orbMesh().valid() && tb.texture().valid() && tb.hazeTexture().valid(),
              "T1 init() produces valid meshes + textures, initialized()=true");
        tb.shutdown(hdev);
        check(!tb.initialized() && !tb.mesh().valid() && !tb.hazeMesh().valid()
              && !tb.texture().valid(),
              "T1b shutdown() releases handles, initialized()=false");
        tb.init(hdev);
        check(tb.initialized(), "T1c re-init after shutdown succeeds (no leak)");
        rhi::MeshHandle before = tb.mesh();
        tb.init(hdev);
        check(tb.mesh().id == before.id, "T1d double-init() is a no-op");
        tb.shutdown(hdev);
        tb.shutdown(hdev);
        check(!tb.initialized(), "T1e double-shutdown() is safe");
    }
    // T2: render() at intensity 0 / 0.5 / 1.0 runs VUID-safe + intensity scales the
    // beam strength (lock-on ramp) + the beam is drawn TRANSLUCENT.
    {
        TractorBeam tb;
        tb.init(hdev);
        rhi::FrameContext fr = hdev.beginFrame();
        const float from[3] = { 0.0f, 0.0f, 0.0f };
        const float to[3]   = { 12.0f, 2.0f, -4.0f };
        tb.render(hdev, fr, idM, from, to, /*intensity=*/0.0f, /*timeSec=*/0.3f);
        check(tb.lastDrawn() && tb.lastStrength() > 0.0f,
              "T2 render() at intensity 0 still draws a faint targeting glow");
        // TRANSLUCENCY GATE (art-direction retune): both cones ride the alpha-blend
        // path with a near-clear alpha -- the scene reads THROUGH the beam.
        check(tb.alphaBlended()
              && tb.lastHazeAlpha() > 0.0f && tb.lastHazeAlpha() < 0.07f
              && tb.lastCoreAlpha() > 0.0f && tb.lastCoreAlpha() < 0.20f,
              "T2a beam is TRANSLUCENT (alpha-blend, near-clear haze + faint core)");
        check(tb.lastHazeAlpha() < tb.lastCoreAlpha(),
              "T2b outer haze is fainter than the inner core");
        float s0 = tb.lastStrength();
        tb.render(hdev, fr, idM, from, to, /*intensity=*/0.5f, 0.3f);
        float s5 = tb.lastStrength();
        tb.render(hdev, fr, idM, from, to, /*intensity=*/1.0f, 0.3f);
        float s1 = tb.lastStrength();
        check(s1 > s5 && s5 > s0,
              "T2c intensity 0 < 0.5 < 1.0 monotonically raises beam strength");
        tb.render(hdev, fr, idM, from, to, /*intensity=*/5.0f, 0.3f);
        check(tb.lastIntensity() == 1.0f, "T2d intensity > 1 clamps to 1.0");
        tb.render(hdev, fr, idM, from, to, /*intensity=*/-3.0f, 0.3f);
        check(tb.lastIntensity() == 0.0f, "T2e intensity < 0 clamps to 0.0");
        hdev.endFrame(fr);
        tb.shutdown(hdev);
    }
    // T3: degenerate from==to is handled gracefully -- the draw is SKIPPED, no NaN.
    {
        TractorBeam tb;
        tb.init(hdev);
        rhi::FrameContext fr = hdev.beginFrame();
        const float same[3] = { 3.0f, 1.0f, -2.0f };
        tb.render(hdev, fr, idM, same, same, /*intensity=*/1.0f, 0.3f);
        check(!tb.lastDrawn() && tb.lastStrength() == 0.0f,
              "T3 from==to skips the draw (no NaN, no degenerate transform)");
        const float a[3] = { 0.0f, 0.0f, 0.0f };
        const float b[3] = { 1e-6f, 0.0f, 0.0f };
        tb.render(hdev, fr, idM, a, b, 1.0f, 0.3f);
        check(!tb.lastDrawn(), "T3b near-zero-length beam is skipped");
        const float to2[3] = { 0.0f, 8.0f, 0.0f };  // straight UP (up-parallel basis path)
        tb.render(hdev, fr, idM, a, to2, 1.0f, 0.3f);
        check(tb.lastDrawn(), "T3c valid beam after a degenerate one draws (up-parallel basis ok)");
        hdev.endFrame(fr);
        tb.shutdown(hdev);
    }
    // T4: Tuning clamping + the baked CORE energy is the new longitudinal gradient
    // (EMITTER end brighter than the capture end) with band variance along the axis.
    {
        TractorBeam::Tuning bad;
        bad.emitterRadius = -1.0f;
        bad.captureRadius = -3.0f;
        bad.ringDensity   = 0.0f;
        bad.flowSpeed     = -5.0f;
        bad.hazeScale     = 0.2f;
        auto c = clampTuning(bad);
        check(c.emitterRadius >= 0.0f, "T4a emitterRadius clamps to >= 0");
        check(c.captureRadius  > 0.0f, "T4b captureRadius clamps to > 0");
        check(c.ringDensity   >= 1.0f, "T4c ringDensity clamps to >= 1");
        check(c.flowSpeed     >= 0.0f, "T4d flowSpeed clamps to >= 0");
        check(c.hazeScale     >= 1.0f, "T4e hazeScale clamps to >= 1");
        TractorBeam tb;
        tb.init(hdev, bad);
        check(tb.initialized(), "T4f init() survives an out-of-range Tuning");
        tb.shutdown(hdev);

        TractorBeam::Tuning t;
        float bEmit = TractorBeam::sampleEnergyBrightness(0.0f, t);
        float bCap  = TractorBeam::sampleEnergyBrightness(1.0f, t);
        check(bEmit > bCap, "T4g EMITTER end is brighter than the capture end (new falloff)");
        float lo = 1e9f, hi = -1e9f;
        const int N = 512;
        for (int i = 0; i < N; ++i) {
            float s = (i + 0.5f) / (float)N;
            float bb = TractorBeam::sampleEnergyBrightness(s, t);
            lo = std::min(lo, bb); hi = std::max(hi, bb);
        }
        check(hi - lo > 0.05f, "T4h energy field produces brightness variance along the beam");
        float a0 = TractorBeam::sampleEnergyBrightness(0.4f, t);
        float a1 = TractorBeam::sampleEnergyBrightness(0.4f, t);
        check(a0 == a1, "T4i energy sample is deterministic");
    }
    x3::logInfo("tractor: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    std::printf("tractor: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
