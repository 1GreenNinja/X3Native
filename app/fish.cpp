// FISH — see app/fish.h. REAL Rodin species (pose-baked swim, PBR) with the
// lofted countershaded procedural fish kept as the never-fail fallback.
#include "fish.h"

#include "mesh_prims.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// THE SPECIES TABLE. Sizes are real: a rudd/bream/perch is a hand-sized river
// fish; a pike is a metre-long ambush predator, and it is ALONE.
// ---------------------------------------------------------------------------
static const FishSpeciesDesc kSpecies[(uint32_t)FishSpecies::Count] = {
    // name      glb                    size  jit   beatHz speed solitary
    { "rudd",   "Fish_Rudd.glb",        0.26f, 0.22f, 2.8f, 1.05f, false },
    { "bream",  "Fish_Bream.glb",       0.30f, 0.20f, 2.4f, 0.95f, false },
    { "perch",  "Fish_Perch.glb",       0.24f, 0.20f, 3.0f, 1.15f, false },
    // THE PIKE: big, slow, solitary. It holds station in the reach and barely
    // moves — which is exactly why you notice it.
    { "pike",   "Fish_Pike.glb",        0.92f, 0.12f, 1.3f, 0.34f, true  },
};

const FishSpeciesDesc& fishSpecies(FishSpecies s) {
    const uint32_t i = (uint32_t)s;
    return kSpecies[i < (uint32_t)FishSpecies::Count ? i : 0];
}

namespace {

constexpr float kPi = 3.14159265f;

// ---------------------------------------------------------------------------
// THE FISH MESH — a lofted body, not a stack of boxes.
//
// The spine runs +X (snout) to -X (tail). `profileH(t)` is the body HALF-HEIGHT
// along the normalized spine t in [0,1] (0 = snout tip, 1 = tail root): pointed
// at the snout, deepest at ~30% back, pinched at the tail root. The body is
// laterally compressed — half-width = kWidthRatio * half-height — which is what
// gives a fish its knife-edge from above and its slab profile from the side.
//
// The fish is cut into THREE pieces at the hinge joints so it can S-FLEX: HEAD
// [0, t1], MID [t1, t2], TAIL [t2, 1] + the forked caudal fin. Each piece's mesh
// is built in ITS OWN local space (origin = its front joint) so the host hinges
// them with plain transforms — no skinning, no per-frame vertex work.
// ---------------------------------------------------------------------------
constexpr float kNoseX      =  0.60f;   // spine x at t=0
constexpr float kRootX      = -0.34f;   // spine x at t=1 (tail root)
constexpr float kWidthRatio =  0.42f;   // half-width / half-height (compressed)
constexpr float kJointT1    =  0.40f;   // head|mid hinge
constexpr float kJointT2    =  0.70f;   // mid|tail hinge
constexpr int   kRingSegs   =  10;      // vertices around a ring

inline float spineX(float t) { return kNoseX + (kRootX - kNoseX) * t; }

// Body half-height profile (local m): control points, smoothstep-blended. Tuned
// so the deepest point sits ~30% back and the tail root pinches to a stalk.
float profileH(float t) {
    static const float kT[] = { 0.00f, 0.07f, 0.18f, 0.30f, 0.50f, 0.72f, 0.88f, 1.00f };
    static const float kH[] = { 0.008f, 0.042f, 0.078f, 0.092f, 0.078f, 0.048f, 0.028f, 0.018f };
    const int n = 8;
    if (t <= kT[0]) return kH[0];
    for (int i = 1; i < n; ++i) {
        if (t <= kT[i]) {
            const float u  = (t - kT[i - 1]) / (kT[i] - kT[i - 1]);
            const float su = u * u * (3.0f - 2.0f * u);
            return kH[i - 1] + (kH[i] - kH[i - 1]) * su;
        }
    }
    return kH[n - 1];
}

// UV convention: u = along the body, v = UP-NESS (1 = the back ridge, 0 = the
// belly line). The skin is a vertical gradient — dark olive-steel at v=1, pale
// silver at v=0 — so COUNTERSHADING comes free on every vertex of every fish
// from one 8x64 texture. It is also what makes a belly-up corpse read.
void pushRing(x3::prims::PrimMesh& m, float t, float xOffset, float u) {
    const float h = profileH(t);
    const float w = h * kWidthRatio;
    const float x = spineX(t) - xOffset;
    for (int s = 0; s < kRingSegs; ++s) {
        const float a  = (float)s / (float)kRingSegs * 2.0f * kPi;   // 0 = +Y (back)
        const float cy = std::cos(a), sz = std::sin(a);
        x3::rhi::MeshVertex v{};
        v.pos[0] = x;  v.pos[1] = h * cy;  v.pos[2] = w * sz;
        const float ny = cy / std::max(h, 1e-4f), nz = sz / std::max(w, 1e-4f);
        const float nl = std::sqrt(ny * ny + nz * nz);
        v.normal[0] = 0.0f; v.normal[1] = ny / nl; v.normal[2] = nz / nl;
        v.uv[0] = u;
        v.uv[1] = 0.5f - 0.5f * cy;    // 0 = back ridge, 1 = belly line
        m.verts.push_back(v);
    }
}

void bridgeRings(x3::prims::PrimMesh& m, uint32_t a, uint32_t b) {
    for (int s = 0; s < kRingSegs; ++s) {
        const uint32_t s0 = (uint32_t)s, s1 = (uint32_t)((s + 1) % kRingSegs);
        m.index.insert(m.index.end(), { a + s0, b + s0, b + s1 });
        m.index.insert(m.index.end(), { a + s0, b + s1, a + s1 });
    }
}

// A flat FIN blade in the fish's XY plane (double-sided), with a fixed `v` so it
// samples the dark (back) or pale (belly) end of the gradient.
void pushBlade(x3::prims::PrimMesh& m,
               float x0, float y0, float x1, float y1,
               float x2, float y2, float x3, float y3, float v) {
    const uint32_t base = (uint32_t)m.verts.size();
    const float px[4] = { x0, x1, x2, x3 };
    const float py[4] = { y0, y1, y2, y3 };
    for (int i = 0; i < 4; ++i) {
        x3::rhi::MeshVertex mv{};
        mv.pos[0] = px[i]; mv.pos[1] = py[i]; mv.pos[2] = 0.0f;
        mv.normal[0] = 0.0f; mv.normal[1] = 0.0f; mv.normal[2] = 1.0f;
        mv.uv[0] = (float)(i & 1); mv.uv[1] = v;
        m.verts.push_back(mv);
    }
    m.index.insert(m.index.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    m.index.insert(m.index.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
}

// A triangular FIN in the fish's XY plane (double-sided). Fins are fans, not
// strips: the quad version read as sticks crossing the body.
void pushTri(x3::prims::PrimMesh& m,
             float x0, float y0, float x1, float y1, float x2, float y2, float v) {
    const uint32_t base = (uint32_t)m.verts.size();
    const float px[3] = { x0, x1, x2 };
    const float py[3] = { y0, y1, y2 };
    for (int i = 0; i < 3; ++i) {
        x3::rhi::MeshVertex mv{};
        mv.pos[0] = px[i]; mv.pos[1] = py[i]; mv.pos[2] = 0.0f;
        mv.normal[0] = 0.0f; mv.normal[1] = 0.0f; mv.normal[2] = 1.0f;
        mv.uv[0] = (float)(i & 1); mv.uv[1] = v;
        m.verts.push_back(mv);
    }
    m.index.insert(m.index.end(), { base, base + 1, base + 2 });
    m.index.insert(m.index.end(), { base, base + 2, base + 1 });   // double-sided
}

// Loft one body piece over t in [tA, tB]; local origin at spineX(tA).
x3::prims::PrimMesh loftPiece(float tA, float tB, int rings) {
    x3::prims::PrimMesh m;
    const float x0 = spineX(tA);
    uint32_t prev = 0;
    for (int i = 0; i <= rings; ++i) {
        const float t = tA + (tB - tA) * ((float)i / (float)rings);
        const uint32_t base = (uint32_t)m.verts.size();
        pushRing(m, t, x0, (float)i / (float)rings);
        if (i > 0) bridgeRings(m, prev, base);
        prev = base;
    }
    return m;
}

// HEAD [0, t1]: the loft + a pointed snout cap + the pectoral pair.
x3::prims::PrimMesh makeHeadMesh() {
    x3::prims::PrimMesh m = loftPiece(0.0f, kJointT1, 5);
    {   // snout: fan the first ring to a point ahead of it
        const uint32_t tip = (uint32_t)m.verts.size();
        x3::rhi::MeshVertex v{};
        v.pos[0] = 0.035f; v.pos[1] = 0.0f; v.pos[2] = 0.0f;
        v.normal[0] = 1.0f;
        v.uv[0] = 0.0f; v.uv[1] = 0.5f;
        m.verts.push_back(v);
        for (int s = 0; s < kRingSegs; ++s) {
            const uint32_t s0 = (uint32_t)s, s1 = (uint32_t)((s + 1) % kRingSegs);
            m.index.insert(m.index.end(), { tip, s1, s0 });
        }
    }
    // PECTORAL: a SMALL swept fan low on the flank (the first pass gave the fish
    // wings — it read as a paper aeroplane).
    const float px = spineX(0.30f) - spineX(0.0f);
    pushTri(m, px, -0.022f,  px - 0.062f, -0.055f,  px - 0.030f, -0.012f, 0.86f);
    return m;
}

// MID [t1, t2]: the loft + the dorsal blade standing on the back ridge.
x3::prims::PrimMesh makeMidMesh() {
    x3::prims::PrimMesh m = loftPiece(kJointT1, kJointT2, 4);
    const float x0 = spineX(kJointT1);
    const float a = spineX(0.44f) - x0, b = spineX(0.66f) - x0;
    // DORSAL: a low swept-back fan standing on the ridge (dark end of the skin).
    pushTri(m, a, 0.062f,  (a + b) * 0.5f - 0.012f, 0.140f,  b, 0.045f, 0.04f);
    return m;
}

// TAIL [t2, 1] + the FORKED CAUDAL FIN — the read that says "fish".
x3::prims::PrimMesh makeTailMesh() {
    x3::prims::PrimMesh m = loftPiece(kJointT2, 1.0f, 4);
    const float x0   = spineX(kJointT2);
    const float root = spineX(1.0f) - x0;      // tail root, local
    const float L    = 0.21f;                  // caudal length
    const float H    = 0.135f;                 // lobe height
    const float notch = root - L * 0.45f;      // the fork's inner notch
    // THE FORKED CAUDAL: two fans from the root out to a notched trailing edge —
    // upper lobe (dark) and lower lobe (pale). This is the read that says "fish".
    pushTri(m, root, 0.004f,  root - L,  H,  notch, 0.0f, 0.12f);
    pushTri(m, root, -0.004f, root - L, -H,  notch, 0.0f, 0.86f);
    // ANAL fin: a small fan under the peduncle (pale side).
    pushTri(m, root + 0.075f, -0.020f,  root + 0.020f, -0.062f,  root + 0.020f, -0.014f, 0.94f);
    return m;
}

// THE SKIN: one 8x64 vertical gradient — dark olive-steel BACK (v=1) through a
// mid flank to a pale silver BELLY (v=0), plus a bright lateral line at the
// seam (the flash a banking fish throws).
std::vector<uint8_t> makeFishSkin(int W, int H) {
    std::vector<uint8_t> px((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) {
        // v runs 0 (BACK ridge) -> 1 (BELLY line) with the mesh's uv.y above.
        const float v = 1.0f - (float)y / (float)(H - 1);   // row 0 -> v=1 (belly)
        const float backC[3]  = { 0.16f, 0.19f, 0.15f };
        const float midC[3]   = { 0.42f, 0.46f, 0.44f };
        const float bellyC[3] = { 0.88f, 0.90f, 0.92f };
        float c[3];
        if (v > 0.5f) {
            const float u = (v - 0.5f) / 0.5f, su = u * u * (3.0f - 2.0f * u);
            for (int k = 0; k < 3; ++k) c[k] = midC[k] + (backC[k] - midC[k]) * su;
        } else {
            const float u = v / 0.5f, su = u * u * (3.0f - 2.0f * u);
            for (int k = 0; k < 3; ++k) c[k] = bellyC[k] + (midC[k] - bellyC[k]) * su;
        }
        const float d = v - 0.52f;
        const float band = std::exp(-(d * d) / (2.0f * 0.02f * 0.02f));
        for (int k = 0; k < 3; ++k) c[k] = std::min(1.0f, c[k] + 0.35f * band);
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &px[((size_t)y * W + x) * 4];
            p[0] = (uint8_t)std::lround(255.0f * c[0]);
            p[1] = (uint8_t)std::lround(255.0f * c[1]);
            p[2] = (uint8_t)std::lround(255.0f * c[2]);
            p[3] = 255;
        }
    }
    return px;
}

inline float slew(float a, float target, float maxStep) {
    float d = target - a;
    while (d >  kPi) d -= 2.0f * kPi;
    while (d < -kPi) d += 2.0f * kPi;
    if (d >  maxStep) d =  maxStep;
    if (d < -maxStep) d = -maxStep;
    return a + d;
}

} // namespace

uint32_t FishSystem::rng() {
    m_rngState = m_rngState * 1664525u + 1013904223u;
    return m_rngState;
}
float FishSystem::frand() { return (float)(rng() % 10000u) * 0.0001f; }

float FishSystem::waterAt(float x, float z) const {
    return m_water ? m_water(x, z) : kFishDryTest * 2.0f;
}

float FishSystem::bedAt(float x, float z, float surface) const {
    return m_bed ? m_bed(x, z) : (surface - 30.0f);
}

// THE POSE for this fish right now. A GLB fish's swim is a MESH SWAP: the baker
// froze one full tail beat into kFishCruise meshes (and a bigger-sweep flee beat
// into kFishFast), so all the runtime does is index them by the fish's own beat
// phase. Deterministic; no vertex work; no skinning.
uint32_t FishSystem::poseIndex(const Fish& f) const {
    if (f.dead) return kFishDeadPose;                 // the slack, limp body
    const FishSpeciesDesc& sp = fishSpecies(f.species);
    const bool bolting = f.fleeT > 0.0f;
    const uint32_t n    = bolting ? kFishFast : kFishCruise;
    const uint32_t base = bolting ? kFishCruise : 0u;
    // The flee burst beats faster as well as harder (the old 1.9x, kept).
    const float beat = sp.beatHz * (bolting ? 1.9f : 1.0f);
    const float turns = m_time * beat + f.phase * (1.0f / (2.0f * kPi));
    float frac = turns - std::floor(turns);           // 0..1 through the beat
    if (!(frac >= 0.0f && frac < 1.0f)) frac = 0.0f;  // NaN guard
    uint32_t k = (uint32_t)(frac * (float)n);
    if (k >= n) k = n - 1;
    return base + k;
}

// Chain the three pieces: HEAD carries the fish transform; MID hinges at joint 1
// by midW; TAIL hinges at joint 2 IN THE MID'S FRAME by tailW (so the sweeps
// compound down the spine — that is the travelling wave).
//
// A GLB fish is the EASY case: one entity, one basis, and the flex is already in
// the mesh — we just swap in the pose. Only the procedural fallback needs the
// hinge chain.
void FishSystem::writeTransform(Fish& f, Scene& scene) {
    if (f.glb) {
        if (f.entHead == kNoLink) return;
        const FishSpeciesDesc& sp = fishSpecies(f.species);
        const float s  = sp.size * f.size;            // the mesh is UNIT-LENGTH
        const float cy = std::cos(f.yaw), sy = std::sin(f.yaw);
        const float cr = std::cos(f.roll), sr = std::sin(f.roll);
        Entity& e = scene.get(f.entHead);
        float* t = e.transform;
        const float c0[3] = {  cy,       0.0f,  -sy      };   // forward (+X = snout)
        const float c1[3] = {  sy * sr,  cr,     cy * sr };   // up (+Y)
        const float c2[3] = {  sy * cr, -sr,     cy * cr };   // right (+Z)
        t[0] = c0[0] * s; t[1] = c0[1] * s; t[2]  = c0[2] * s; t[3]  = 0.0f;
        t[4] = c1[0] * s; t[5] = c1[1] * s; t[6]  = c1[2] * s; t[7]  = 0.0f;
        t[8] = c2[0] * s; t[9] = c2[1] * s; t[10] = c2[2] * s; t[11] = 0.0f;
        t[12] = f.x; t[13] = f.y; t[14] = f.z; t[15] = 1.0f;
        // THE SWIM.
        const SpeciesArt& art = m_art[(uint32_t)f.species];
        const uint32_t p = poseIndex(f);
        if (p < art.poses.size()) e.mesh = art.poses[p];
        return;
    }

    const float s  = m_cfg.size * f.size;
    const float cy = std::cos(f.yaw), sy = std::sin(f.yaw);
    const float cr = std::cos(f.roll), sr = std::sin(f.roll);
    const float c0[3] = {  cy,       0.0f,  -sy      };   // forward (+X)
    const float c1[3] = {  sy * sr,  cr,     cy * sr };   // up (+Y)
    const float c2[3] = {  sy * cr, -sr,     cy * cr };   // right (+Z)

    auto put = [&](uint32_t ent, const float a0[3], const float a1[3], const float a2[3],
                   float px, float py, float pz) {
        if (ent == kNoLink) return;
        float* t = scene.get(ent).transform;
        t[0] = a0[0] * s; t[1] = a0[1] * s; t[2]  = a0[2] * s; t[3]  = 0.0f;
        t[4] = a1[0] * s; t[5] = a1[1] * s; t[6]  = a1[2] * s; t[7]  = 0.0f;
        t[8] = a2[0] * s; t[9] = a2[1] * s; t[10] = a2[2] * s; t[11] = 0.0f;
        t[12] = px; t[13] = py; t[14] = pz; t[15] = 1.0f;
    };
    // Hinge a basis about its own UP axis: X' = X c - Z s, Z' = X s + Z c.
    auto hinge = [](const float a0[3], const float a2[3], float ang,
                    float o0[3], float o2[3]) {
        const float c = std::cos(ang), sn = std::sin(ang);
        for (int k = 0; k < 3; ++k) {
            o0[k] = a0[k] * c  - a2[k] * sn;
            o2[k] = a0[k] * sn + a2[k] * c;
        }
    };

    put(f.entHead, c0, c1, c2, f.x, f.y, f.z);

    const float dHeadMid = spineX(kJointT1) - spineX(0.0f);   // (negative: aft)
    float m0[3], m2[3];
    hinge(c0, c2, f.midW, m0, m2);
    const float mx = f.x + c0[0] * dHeadMid * s;
    const float my = f.y + c0[1] * dHeadMid * s;
    const float mz = f.z + c0[2] * dHeadMid * s;
    put(f.entMid, m0, c1, m2, mx, my, mz);

    const float dMidTail = spineX(kJointT2) - spineX(kJointT1);
    float t0[3], t2[3];
    hinge(m0, m2, f.tailW, t0, t2);
    const float tx = mx + m0[0] * dMidTail * s;
    const float ty = my + m0[1] * dMidTail * s;
    const float tz = mz + m0[2] * dMidTail * s;
    put(f.entTail, t0, c1, t2, tx, ty, tz);
}

void FishSystem::setVisible(Fish& f, Scene& scene, bool vis) {
    const uint32_t ents[3] = { f.entHead, f.entMid, f.entTail };
    for (uint32_t e : ents) if (e != kNoLink) scene.get(e).visible = vis;
}

// ---------------------------------------------------------------------------
// SPECIES ART: load one pose-baked GLB (tools/fish_bake.py).
//
// The GLB carries kFishPoses mesh nodes — the SAME fish frozen at each phase of
// the swim. ModelDrawable has no name, so the baker encodes the pose index in the
// node's TRANSLATION X; we round it back to an int and then ignore nodeTransform
// (each pose mesh is authored in the fish's own local space, unit-length, +X =
// snout). Anything unexpected (wrong pose count, a gap in the indices, a missing
// mesh) => the species is NOT ok and every fish of it falls back to the loft.
// ---------------------------------------------------------------------------
void FishSystem::loadSpecies(FishSpecies s, x3::rhi::IRenderDevice& device) {
    SpeciesArt& art = m_art[(uint32_t)s];
    const FishSpeciesDesc& sp = fishSpecies(s);
    if (m_modelDir.empty() || !sp.glb || !*sp.glb) return;   // procedural by request

    if (!m_loader) {
        m_assets.reset(x3::asset::createAssetSource());
        if (!m_assets || !m_assets->mountDir(m_modelDir, 0)) {
            x3::logWarn(std::string("fish: cannot mount ") + m_modelDir
                        + " — the fish fall back to the procedural loft");
            m_assets.reset();
            return;
        }
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
    }
    if (!m_loader) return;

    x3::asset::Model model = m_loader->load(sp.glb);
    if (!model.ok) {
        x3::logWarn(std::string("fish: ") + sp.glb
                    + " did not load — " + sp.name
                    + " falls back to the procedural loft");
        return;
    }
    const std::vector<x3::asset::ModelDrawable> dr = x3::asset::makeDrawables(model);
    if (dr.size() != kFishPoses) {
        x3::logWarn(std::string("fish: ") + sp.glb + " has "
                    + std::to_string(dr.size()) + " mesh nodes, expected "
                    + std::to_string(kFishPoses)
                    + " (re-run tools/fish_bake.py) — falling back to the loft");
        m_loader->unload(model);
        return;
    }

    art.poses.assign(kFishPoses, x3::rhi::MeshHandle{});
    uint32_t bound = 0;
    for (const x3::asset::ModelDrawable& d : dr) {
        const int idx = (int)std::lround(d.nodeTransform[12]);   // the pose index
        if (idx < 0 || idx >= (int)kFishPoses || !d.meshId) continue;
        if (art.poses[(size_t)idx].valid()) continue;            // duplicate index
        art.poses[(size_t)idx] = x3::rhi::MeshHandle{ d.meshId };
        ++bound;
        if (!art.albedo.valid()) {
            art.albedo = x3::rhi::TextureHandle{ d.baseColorTexId };
            art.normal = x3::rhi::TextureHandle{ d.normalTexId };
            art.mr     = x3::rhi::TextureHandle{ d.mrTexId };
        }
    }
    if (bound != kFishPoses) {
        x3::logWarn(std::string("fish: ") + sp.glb + " bound only "
                    + std::to_string(bound) + "/" + std::to_string(kFishPoses)
                    + " poses — falling back to the procedural loft");
        art.poses.clear();
        m_loader->unload(model);
        return;
    }

    uint32_t idx = 0;
    for (const x3::asset::MeshPrimitive& p : model.primitives) idx += p.indexCount;
    art.tris = idx / 3u / kFishPoses;

    // The Model OWNS the mesh/texture handles, so it must outlive every Entity
    // that points at them: it is held by THIS system (m_models) for the system's
    // whole life — host-owned and persistent, the parked-cars doctrine (a shared
    // mesh must never land in a region ledger). It is deliberately NOT a
    // function-local static: --test-waterzap Z8b builds two FishSystems to compare
    // them, and shared art between instances would break that.
    m_models.push_back(std::move(model));

    art.ok = true;
    x3::logInfo(std::string("fish: ") + sp.name + " <- " + sp.glb + "  "
                + std::to_string(kFishPoses) + " swim poses, "
                + std::to_string(art.tris) + " tris/pose, PBR "
                + (art.normal.valid() ? "albedo+normal" : "albedo")
                + (art.mr.valid() ? "+MR" : "")
                + ", length " + std::to_string(sp.size) + " m");
}

// The PROCEDURAL FALLBACK art (art pass 2): 3 lofted meshes + 1 countershading
// gradient. Built ONCE, and only when some species actually needs it.
void FishSystem::buildProceduralArt(x3::rhi::IRenderDevice& device) {
    if (m_procBuilt) return;
    auto up = [&](const x3::prims::PrimMesh& m) {
        return device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                 m.index.data(), (uint32_t)m.index.size());
    };
    const x3::prims::PrimMesh mh = makeHeadMesh();
    const x3::prims::PrimMesh mm = makeMidMesh();
    const x3::prims::PrimMesh mt = makeTailMesh();
    m_procTris[0] = (uint32_t)mh.index.size() / 3u;
    m_procTris[1] = (uint32_t)mm.index.size() / 3u;
    m_procTris[2] = (uint32_t)mt.index.size() / 3u;
    m_meshHead = up(mh);
    m_meshMid  = up(mm);
    m_meshTail = up(mt);
    const int W = 8, H = 64;
    const std::vector<uint8_t> px = makeFishSkin(W, H);
    m_skin = device.createTexture(px.data(), W, H, false);
    m_procBuilt = true;
}

void FishSystem::build(const FishConfig& cfg, Scene& scene,
                       x3::rhi::IRenderDevice& device) {
    if (m_built) return;
    m_cfg = cfg;
    m_rngState = cfg.seed ? cfg.seed : 0xF15Fu;

    // Load the real art for every species this deployment actually uses.
    bool used[(uint32_t)FishSpecies::Count] = {};
    for (const FishSchoolDesc& sd : m_cfg.schools) used[(uint32_t)sd.species] = true;
    for (uint32_t i = 0; i < (uint32_t)FishSpecies::Count; ++i)
        if (used[i]) loadSpecies((FishSpecies)i, device);

    // Any species that did NOT get real art needs the loft.
    bool needProc = false;
    for (uint32_t i = 0; i < (uint32_t)FishSpecies::Count; ++i)
        if (used[i] && !m_art[i].ok) needProc = true;
    if (needProc) buildProceduralArt(device);

    uint32_t skipped = 0, glbFish = 0, procFish = 0;
    for (const FishSchoolDesc& sd : m_cfg.schools) {
        const float surf = waterAt(sd.centerX, sd.centerZ);
        if (surf < kFishDryTest) {   // a school never spawns on land
            ++skipped;
            x3::logWarn("fish: school at (" + std::to_string(sd.centerX) + ", "
                        + std::to_string(sd.centerZ) + ") is DRY — skipped");
            continue;
        }
        const FishSpeciesDesc& sp = fishSpecies(sd.species);
        const SpeciesArt& art = m_art[(uint32_t)sd.species];
        FishSchool sc;
        sc.cx = sd.centerX; sc.cz = sd.centerZ; sc.heading = sd.heading;
        sc.speed = sd.speed;
        sc.species  = sd.species;
        sc.solitary = sp.solitary;
        const uint32_t si = (uint32_t)m_schools.size();
        m_schools.push_back(sc);

        for (uint32_t k = 0; k < sd.count; ++k) {
            Fish f;
            f.school  = si;
            f.species = sd.species;
            f.glb     = art.ok;
            const float ang = (float)k / (float)(sd.count ? sd.count : 1) * 2.0f * kPi
                            + frand() * 0.6f;
            const float rad = sd.spread * (0.35f + 0.65f * frand());
            f.slotX = std::cos(ang) * rad;
            f.slotZ = std::sin(ang) * rad;
            f.slotD = frand() * 1.4f;               // 0..1.4 m of extra depth
            f.size  = 1.0f + (frand() * 2.0f - 1.0f) * sp.sizeJitter;
            f.phase = frand() * 6.2831853f;
            f.speed = sd.speed * (0.9f + frand() * 0.3f);
            const float ch = std::cos(sc.heading), sh = std::sin(sc.heading);
            f.x = sc.cx + f.slotX * ch - f.slotZ * sh;
            f.z = sc.cz + f.slotX * sh + f.slotZ * ch;
            const float ws = waterAt(f.x, f.z);
            const float surfHere = (ws < kFishDryTest) ? surf : ws;
            const float bed = bedAt(f.x, f.z, surfHere);
            f.y = surfHere - m_cfg.depthBelowSurf - f.slotD;
            if (f.y < bed + m_cfg.depthMin) f.y = bed + m_cfg.depthMin;
            f.yaw = sc.heading;

            Entity e;
            e.roomId  = m_cfg.roomId;
            e.visible = false;
            const float j = 0.94f + frand() * 0.12f;   // a whisper of variation
            if (art.ok) {
                // REAL FISH: the scanned albedo/normal/MR carry the whole look, so
                // the tint stays ~white (a school tint would STAIN a real fish).
                // mrTex valid => Scene::render routes it through drawMeshPBR, and
                // the normal map gets its scales.
                e.tex       = art.albedo;
                e.normalTex = art.normal;
                e.mrTex     = art.mr;
                e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = j;
                e.baseColor[3] = 1.0f;
                e.mesh = art.poses[0];
                // THE UNDERWATER FILL — and it is the fish's OWN colour.
                // The submerged volume of the river is nearly unlit: the first
                // proof shot came back with a perfectly-framed pike rendered as a
                // BLACK SILHOUETTE (the viewmodel was black too — it is the water,
                // not the fish). The old procedural fish only survived that gloom
                // because it carried a flat grey-blue emissive whisper — but a flat
                // emissive on a REAL fish washes the scanned art into a glowing
                // lozenge and buries the normal map.
                // So the fill is the ALBEDO ITSELF, routed through emissiveTex:
                // the PBR path MULTIPLIES the emissive term by that map, so the
                // pike's olive back, its pale spot-rows and its red caudal all lift
                // out of the dark in their own hues, and a black texel stays black.
                // A fish is not a lamp — this is the scattered light a real fish
                // returns in green water, floored so it is never a hole in the frame.
                e.emissiveTex  = art.albedo;
                e.emissive[0] = e.emissive[1] = e.emissive[2] = 1.0f;
                e.emissive[3] = 0.35f;
                f.entHead = scene.add(e);
                ++glbFish;
            } else {
                // THE LOFT (fallback): the skin carries the countershading; the
                // school tint and a per-fish jitter MULTIPLY it.
                e.tex = m_skin;
                e.baseColor[0] = sd.tint[0] * j;
                e.baseColor[1] = sd.tint[1] * j;
                e.baseColor[2] = sd.tint[2] * j;
                e.baseColor[3] = 1.0f;
                e.emissive[0] = 0.40f; e.emissive[1] = 0.48f; e.emissive[2] = 0.52f;
                e.emissive[3] = 0.18f;
                Entity eh = e; eh.mesh = m_meshHead;
                Entity em = e; em.mesh = m_meshMid;
                Entity et = e; et.mesh = m_meshTail;
                f.entHead = scene.add(eh);
                f.entMid  = scene.add(em);
                f.entTail = scene.add(et);
                ++procFish;
            }

            writeTransform(f, scene);
            m_fish.push_back(f);
        }
    }
    m_built = true;
    x3::logInfo("fish: built " + std::to_string(m_fish.size()) + " fish in "
                + std::to_string(m_schools.size()) + " schools — "
                + std::to_string(glbFish) + " REAL (pose-baked GLB), "
                + std::to_string(procFish) + " procedural loft, "
                + std::to_string(skipped) + " dry schools skipped; "
                + std::to_string(triCount()) + " tris across "
                + std::to_string(drawCount()) + " draws");
}

void FishSystem::update(float dt, Scene& scene, const x3::phys::Vec3& playerPos) {
    if (!m_built || dt <= 0.0f) return;
    m_time += dt;

    // ---- School centers: drift along the water, turning away from dry land ----
    for (FishSchool& sc : m_schools) {
        const float dx = playerPos.x - sc.cx, dz = playerPos.z - sc.cz;
        sc.active = (dx * dx + dz * dz) <= (m_cfg.activeRadius * m_cfg.activeRadius);
        if (!sc.active) continue;
        const float look = 8.0f;
        bool wet = waterAt(sc.cx + std::cos(sc.heading) * look,
                           sc.cz + std::sin(sc.heading) * look) > kFishDryTest;
        if (!wet) {
            for (int k = 1; k <= 10 && !wet; ++k) {
                const float step = (float)k * 0.32f;
                for (int sgn = -1; sgn <= 1 && !wet; sgn += 2) {
                    const float h = sc.heading + (float)sgn * step;
                    if (waterAt(sc.cx + std::cos(h) * look,
                                sc.cz + std::sin(h) * look) > kFishDryTest) {
                        sc.heading = h; wet = true;
                    }
                }
            }
            if (!wet) sc.heading += kPi;   // dead end: about-face
        }
        const float sp = sc.speed;
        sc.cx += std::cos(sc.heading) * sp * dt;
        sc.cz += std::sin(sc.heading) * sp * dt;
        if (waterAt(sc.cx, sc.cz) < kFishDryTest) {
            sc.cx -= std::cos(sc.heading) * sp * dt;
            sc.cz -= std::sin(sc.heading) * sp * dt;
            sc.heading += kPi;
        }
    }

    // ---- Fish ----
    const uint32_t n = (uint32_t)m_fish.size();
    for (uint32_t i = 0; i < n; ++i) {
        Fish& f = m_fish[i];
        const FishSchool& sc = m_schools[f.school];
        if (f.gone) continue;
        if (!sc.active && !f.dead) { setVisible(f, scene, false); continue; }
        setVisible(f, scene, true);

        const float surf = waterAt(f.x, f.z);

        // ---- DEAD: belly-UP in the surface plane, drifting, lolling ----------
        if (f.dead) {
            f.deadT += dt;
            if (f.deadT >= m_cfg.deadLinger) {
                f.gone = true;
                setVisible(f, scene, false);
                continue;
            }
            if (surf > kFishDryTest) {
                // The corpse rests IN the plane: rolled 180, the PALE BELLY faces
                // the sky and breaks the surface; the dark back hangs below it.
                const float top = surf + 0.02f;
                if (f.y < top) f.y = std::min(top, f.y + m_cfg.deadRise * dt);
                const float nx = f.x + std::cos(sc.heading) * m_cfg.deadDrift * dt;
                const float nz = f.z + std::sin(sc.heading) * m_cfg.deadDrift * dt;
                if (waterAt(nx, nz) > kFishDryTest) { f.x = nx; f.z = nz; }
            }
            f.roll  = slew(f.roll, kPi, 3.5f * dt);      // roll over: belly to the sky
            f.yaw  += 0.22f * dt;                        // lolling spin on the current
            f.midW  = slew(f.midW,  0.10f, 0.8f * dt);   // the swim goes slack
            f.tailW = slew(f.tailW, 0.26f, 0.8f * dt);
            writeTransform(f, scene);
            continue;
        }

        // ---- ALIVE: flee / cohesion / separation / alignment ------------------
        const float pdx = f.x - playerPos.x, pdy = f.y - playerPos.y, pdz = f.z - playerPos.z;
        const float pd2 = pdx * pdx + pdy * pdy + pdz * pdz;
        if (pd2 < m_cfg.fleeRadius * m_cfg.fleeRadius) f.fleeT = m_cfg.fleeTime;
        else if (f.fleeT > 0.0f) f.fleeT -= dt;

        float dirX = 0.0f, dirZ = 0.0f, speed = f.speed;
        if (f.fleeT > 0.0f) {
            const float len = std::sqrt(pdx * pdx + pdz * pdz);
            if (len > 1e-3f) { dirX = pdx / len; dirZ = pdz / len; }
            else { dirX = std::cos(f.yaw); dirZ = std::sin(f.yaw); }
            speed = m_cfg.fleeSpeed;
        } else {
            const float ch = std::cos(sc.heading), sh = std::sin(sc.heading);
            const float tx = sc.cx + f.slotX * ch - f.slotZ * sh;
            const float tz = sc.cz + f.slotX * sh + f.slotZ * ch;
            float ax = tx - f.x, az = tz - f.z;
            const float ad = std::sqrt(ax * ax + az * az);
            if (ad > 1e-3f) { ax /= ad; az /= ad; }
            const float w = ad > 1.5f ? 0.75f : 0.25f;
            dirX = ax * w + ch * (1.0f - w);
            dirZ = az * w + sh * (1.0f - w);
        }
        // SEPARATION (same school only). A LONER has no schoolmates to avoid —
        // predators do not shoal, and this loop is what a shoal IS.
        for (uint32_t j = 0; j < n && !sc.solitary; ++j) {
            if (j == i) continue;
            const Fish& o = m_fish[j];
            if (o.school != f.school || o.dead || o.gone) continue;
            const float sx = f.x - o.x, sz = f.z - o.z;
            const float d2 = sx * sx + sz * sz;
            if (d2 < m_cfg.separation * m_cfg.separation && d2 > 1e-6f) {
                const float d = std::sqrt(d2);
                dirX += (sx / d) * 0.9f;
                dirZ += (sz / d) * 0.9f;
            }
        }
        const float dl = std::sqrt(dirX * dirX + dirZ * dirZ);
        if (dl > 1e-3f) { dirX /= dl; dirZ /= dl; }
        else { dirX = std::cos(f.yaw); dirZ = std::sin(f.yaw); }

        // Advance — but a fish NEVER beaches: a step onto dry ground is refused.
        const float nx = f.x + dirX * speed * dt;
        const float nz = f.z + dirZ * speed * dt;
        if (waterAt(nx, nz) > kFishDryTest) { f.x = nx; f.z = nz; }

        // Depth: bounded between the bed and just under the surface.
        const float s2 = waterAt(f.x, f.z);
        if (s2 > kFishDryTest) {
            const float bed = bedAt(f.x, f.z, s2);
            float ty = s2 - m_cfg.depthBelowSurf - f.slotD;
            ty += std::sin(m_time * 0.7f + f.phase) * 0.12f;
            const float lo = bed + m_cfg.depthMin;
            const float hi = s2 - m_cfg.depthBelowSurf * 0.5f;
            if (ty < lo) ty = lo;
            if (ty > hi) ty = hi;
            f.y += (ty - f.y) * std::min(1.0f, 2.5f * dt);
        }

        // FACING + THE SWIM. A travelling sine down the spine: the mid sweeps a
        // little, the tail sweeps more and one beatLag BEHIND it — that phase
        // offset IS the S-flex (equal phases would wag the fish like a plank).
        // The body also BANKS into its turns (roll from the measured yaw rate).
        const float want = std::atan2(dirZ, dirX);
        const float prevYaw = f.yaw;
        // A PREDATOR TURNS SLOWLY. The pike is deliberate — half the shoal's yaw
        // rate — which, with its low speed, is what makes it read as a big fish
        // holding station rather than a big minnow.
        const float turn = m_cfg.turnRate * (sc.solitary ? 0.5f : 1.0f);
        f.yaw = slew(f.yaw, want, turn * dt);
        float yawRate = f.yaw - prevYaw;
        while (yawRate >  kPi) yawRate -= 2.0f * kPi;
        while (yawRate < -kPi) yawRate += 2.0f * kPi;
        yawRate = (dt > 1e-5f) ? yawRate / dt : 0.0f;
        float bank = -yawRate * m_cfg.bankPerTurn;
        if (bank >  m_cfg.bankMax) bank =  m_cfg.bankMax;
        if (bank < -m_cfg.bankMax) bank = -m_cfg.bankMax;
        f.roll += (bank - f.roll) * std::min(1.0f, 6.0f * dt);

        // The PROCEDURAL flex (a GLB fish's flex is already baked into its pose
        // mesh — writeTransform just picks it). The beat is the SPECIES' beat, so
        // a fallback pike still cruises with slow, heavy tail beats and a rudd
        // still flickers.
        const bool  bolting = f.fleeT > 0.0f;
        const float beat = fishSpecies(f.species).beatHz * (bolting ? 1.9f : 1.0f);
        const float gain = bolting ? 1.5f : 1.0f;
        const float ph   = m_time * beat * 6.2831853f + f.phase;
        f.midW  = std::sin(ph) * m_cfg.midAmp * gain;
        f.tailW = std::sin(ph - m_cfg.beatLag) * m_cfg.tailAmp * gain;
        writeTransform(f, scene);
    }
}

uint32_t FishSystem::killWithin(float cx, float cz, float radius) {
    if (!m_built) return 0;
    uint32_t killed = 0;
    const float r2 = radius * radius;
    for (Fish& f : m_fish) {
        if (f.dead || f.gone) continue;
        const float dx = f.x - cx, dz = f.z - cz;
        if (dx * dx + dz * dz <= r2) { f.dead = true; f.deadT = 0.0f; f.fleeT = 0.0f; ++killed; }
    }
    if (killed)
        x3::logInfo("fish: THE ZAP killed " + std::to_string(killed) + " fish");
    return killed;
}

uint32_t FishSystem::aliveCount() const {
    uint32_t n = 0;
    for (const Fish& f : m_fish) if (!f.dead) ++n;
    return n;
}
uint32_t FishSystem::deadCount() const {
    uint32_t n = 0;
    for (const Fish& f : m_fish) if (f.dead && !f.gone) ++n;
    return n;
}
uint32_t FishSystem::activeCount() const {
    uint32_t n = 0;
    for (const Fish& f : m_fish) if (m_schools[f.school].active && !f.gone) ++n;
    return n;
}

bool FishSystem::speciesLoaded(FishSpecies s) const {
    const uint32_t i = (uint32_t)s;
    return i < (uint32_t)FishSpecies::Count && m_art[i].ok;
}
uint32_t FishSystem::glbFishCount() const {
    uint32_t n = 0;
    for (const Fish& f : m_fish) if (f.glb && !f.gone) ++n;
    return n;
}
// What the fish actually cost the renderer. A GLB fish is ONE draw of ONE
// pose-baked mesh; a fallback fish is three lofted pieces.
uint32_t FishSystem::triCount() const {
    uint32_t n = 0;
    for (const Fish& f : m_fish) {
        if (f.gone) continue;
        if (f.glb) n += m_art[(uint32_t)f.species].tris;
        else       n += m_procTris[0] + m_procTris[1] + m_procTris[2];
    }
    return n;
}
uint32_t FishSystem::drawCount() const {
    uint32_t n = 0;
    for (const Fish& f : m_fish) {
        if (f.gone) continue;
        n += f.glb ? 1u : 3u;
    }
    return n;
}

} // namespace x3::game
