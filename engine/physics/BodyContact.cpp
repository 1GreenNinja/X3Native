// BodyContact.cpp — see BodyContact.h. Pure positional math; the only engine
// dependency is IPhysicsWorld's rayCast in the discovery helper.
#include "BodyContact.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace x3 { namespace phys {

namespace {

inline Vec3  add(Vec3 a, Vec3 b)      { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3  sub(Vec3 a, Vec3 b)      { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3  mul(Vec3 a, float s)     { return { a.x * s, a.y * s, a.z * s }; }
inline float dot(Vec3 a, Vec3 b)      { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len(Vec3 a)              { return std::sqrt(dot(a, a)); }
inline Vec3  norm(Vec3 a)             { float l = len(a); return (l > 1e-8f) ? mul(a, 1.0f / l) : Vec3{ 0, 1, 0 }; }

// Signed penetration of a bone sphere into a surface: > 0 means the sphere
// intrudes past the plane by that many meters. Finite surfaces only exist
// within their tangent extents (grown by the sphere radius so a sphere resting
// on the slab EDGE still contacts). Bones far below the plane (fully tunneled,
// center deeper than one diameter) are ignored — a contact pass corrects
// resting penetration, it does not teleport bodies up through furniture.
float penetration(const ContactBone& b, const ContactSurface& s) {
    const Vec3  rel = sub(b.pos, s.point);
    const float d   = dot(rel, s.normal);          // center height above plane
    if (d < -2.0f * b.radius) return 0.0f;         // tunneled: not ours to fix
    if (s.halfU > 0.0f) {
        if (std::fabs(dot(rel, s.uAxis)) > s.halfU + b.radius) return 0.0f;
        if (std::fabs(dot(rel, s.vAxis)) > s.halfV + b.radius) return 0.0f;
    }
    return b.radius - d;
}

} // namespace

BodySolveStats solveBodyContact(ContactBone* bones, uint32_t boneCount,
                                const ContactSurface* surfaces, uint32_t surfaceCount,
                                const BodySolveParams& params) {
    BodySolveStats stats{};
    if (!bones || boneCount == 0) return stats;

    // Rest lengths captured from the INPUT pose — the artist's pose is truth;
    // the solver's job is contact compliance, not re-posing.
    std::vector<float> rest(boneCount, 0.0f);
    for (uint32_t i = 0; i < boneCount; ++i)
        if (bones[i].parent >= 0 && (uint32_t)bones[i].parent < boneCount)
            rest[i] = len(sub(bones[i].pos, bones[bones[i].parent].pos));

    for (int it = 0; it < params.iterations; ++it) {
        // ---- Contact projection ----
        for (uint32_t i = 0; i < boneCount; ++i) {
            for (uint32_t si = 0; si < surfaceCount; ++si) {
                const ContactSurface& s = surfaces[si];
                const float pen = penetration(bones[i], s);
                const float allowed = s.soft ? s.indentBudget * bones[i].massFrac : 0.0f;
                if (pen > allowed + params.epsilon) {
                    bones[i].pos = add(bones[i].pos, mul(s.normal, pen - allowed));
                    ++stats.contactsResolved;
                }
            }
        }
        // ---- Distance constraints (2 damped sweeps: root-out then leaf-in) ----
        for (int sweep = 0; sweep < 2; ++sweep) {
            const bool fwd = (sweep == 0);
            for (uint32_t k = 0; k < boneCount; ++k) {
                const uint32_t i = fwd ? k : (boneCount - 1 - k);
                const int p = bones[i].parent;
                if (p < 0 || (uint32_t)p >= boneCount || rest[i] <= 1e-6f) continue;
                const Vec3  delta = sub(bones[i].pos, bones[(uint32_t)p].pos);
                const float l     = len(delta);
                if (l < 1e-8f) continue;
                const float err  = (l - rest[i]) * 0.5f * params.damping;
                const Vec3  corr = mul(delta, err / l);
                bones[i].pos            = sub(bones[i].pos, corr);
                bones[(uint32_t)p].pos  = add(bones[(uint32_t)p].pos, corr);
            }
        }
    }

    // Residuals for the caller/gate.
    for (uint32_t i = 0; i < boneCount; ++i)
        for (uint32_t si = 0; si < surfaceCount; ++si) {
            const float pen = penetration(bones[i], surfaces[si]);
            if (pen <= 0.0f) continue;
            if (surfaces[si].soft) { if (pen > stats.maxSoftPenetration)  stats.maxSoftPenetration  = pen; }
            else                   { if (pen > stats.maxRigidPenetration) stats.maxRigidPenetration = pen; }
        }
    return stats;
}

uint32_t discoverRigidSurfaces(IPhysicsWorld& world,
                               const ContactBone* bones, uint32_t boneCount,
                               ContactSurface* out, uint32_t maxOut,
                               float castDist, float mergeDist) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < boneCount && n < maxOut; ++i) {
        // Cast from just above the sphere straight down: finds the support
        // surface under a resting/penetrating bone.
        const Vec3 origin{ bones[i].pos.x, bones[i].pos.y + bones[i].radius, bones[i].pos.z };
        const RayHit hit = world.rayCast(origin, Vec3{ 0.0f, -1.0f, 0.0f }, castDist, Layer::Static);
        if (!hit.hit) continue;
        bool dup = false;
        for (uint32_t k = 0; k < n; ++k) {
            if (dot(out[k].normal, hit.normal) > 0.999f &&
                std::fabs(dot(sub(hit.point, out[k].point), out[k].normal)) < mergeDist) {
                dup = true; break;
            }
        }
        if (dup) continue;
        ContactSurface s{};
        s.point  = hit.point;
        s.normal = norm(hit.normal);
        s.soft   = false;             // physics carries no softness tag
        out[n++] = s;
    }
    return n;
}

uint32_t bakeSoftIndentation(const ContactBone* bones, uint32_t boneCount,
                             const ContactSurface& surf,
                             float* verts, uint32_t vertCount, uint32_t strideFloats,
                             int normalOffset,
                             const uint32_t* indices, uint32_t indexCount,
                             const IndentParams& params) {
    if (!bones || !verts || vertCount == 0 || strideFloats < 3) return 0;
    const float R = (params.falloffRadius > 1e-4f) ? params.falloffRadius : 0.22f;

    uint32_t displaced = 0;
    for (uint32_t v = 0; v < vertCount; ++v) {
        float* p = verts + (size_t)v * strideFloats;
        const Vec3 vp{ p[0], p[1], p[2] };
        // Deepest dent any bone presses at this vertex (max, not sum).
        float depth = 0.0f;
        for (uint32_t i = 0; i < boneCount; ++i) {
            // Contact center = the bone projected onto the surface plane.
            const Vec3  rel  = sub(bones[i].pos, surf.point);
            const float h    = dot(rel, surf.normal);
            const Vec3  onPl = sub(bones[i].pos, mul(surf.normal, h));
            // Only bones actually pressing (sphere reaches the plane).
            if (h > bones[i].radius + 1e-3f) continue;
            const Vec3  dv  = sub(vp, onPl);
            const Vec3  tangential = sub(dv, mul(surf.normal, dot(dv, surf.normal)));
            const float dist = len(tangential);
            if (dist >= R) continue;
            // Wendland-style smooth kernel: (1 - (d/R)^2)^2 — C1, compact.
            const float x = dist / R;
            const float w = (1.0f - x * x) * (1.0f - x * x);
            const float d = surf.indentBudget * bones[i].massFrac * w;
            if (d > depth) depth = d;
        }
        if (depth > 1e-5f) {
            p[0] -= surf.normal.x * depth;
            p[1] -= surf.normal.y * depth;
            p[2] -= surf.normal.z * depth;
            ++displaced;
        }
    }

    // Re-derive smooth vertex normals from the indexed triangles.
    if (indices && indexCount >= 3 && normalOffset >= 3 &&
        (uint32_t)normalOffset + 3 <= strideFloats) {
        for (uint32_t v = 0; v < vertCount; ++v) {
            float* nrm = verts + (size_t)v * strideFloats + normalOffset;
            nrm[0] = nrm[1] = nrm[2] = 0.0f;
        }
        for (uint32_t t = 0; t + 2 < indexCount; t += 3) {
            const uint32_t ia = indices[t], ib = indices[t + 1], ic = indices[t + 2];
            if (ia >= vertCount || ib >= vertCount || ic >= vertCount) continue;
            const float* pa = verts + (size_t)ia * strideFloats;
            const float* pb = verts + (size_t)ib * strideFloats;
            const float* pc = verts + (size_t)ic * strideFloats;
            const Vec3 e1{ pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
            const Vec3 e2{ pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
            const Vec3 fn{ e1.y * e2.z - e1.z * e2.y,
                           e1.z * e2.x - e1.x * e2.z,
                           e1.x * e2.y - e1.y * e2.x };   // area-weighted
            for (uint32_t vi : { ia, ib, ic }) {
                float* nrm = verts + (size_t)vi * strideFloats + normalOffset;
                nrm[0] += fn.x; nrm[1] += fn.y; nrm[2] += fn.z;
            }
        }
        for (uint32_t v = 0; v < vertCount; ++v) {
            float* nrm = verts + (size_t)v * strideFloats + normalOffset;
            const Vec3 nn = norm(Vec3{ nrm[0], nrm[1], nrm[2] });
            nrm[0] = nn.x; nrm[1] = nn.y; nrm[2] = nn.z;
        }
    }
    return displaced;
}

// ===========================================================================
// Headless self-test (--test-bodycontact)
// ===========================================================================
namespace {
int g_pass = 0, g_total = 0;
void check(bool ok, const char* name) {
    ++g_total;
    if (ok) { ++g_pass; std::printf("  PASS %s\n", name); }
    else    {           std::printf("  FAIL %s\n", name); }
}

// An 8-bone supine chain (head -> heels) lying along +X, sunk into the plane.
std::vector<ContactBone> makeChain(float y) {
    std::vector<ContactBone> b(8);
    for (int i = 0; i < 8; ++i) {
        b[(size_t)i].parent   = i - 1;                     // simple chain
        b[(size_t)i].pos      = { 0.22f * (float)i, y, 0.0f };
        b[(size_t)i].radius   = 0.05f;
        b[(size_t)i].massFrac = (i == 3) ? 0.25f : 0.10f;  // pelvis carries more
    }
    return b;
}
} // namespace

bool runBodyContactSelfTest() {
    g_pass = g_total = 0;
    const ContactSurface rigid{};   // y=0 plane, +Y, infinite, rigid

    // T1 — rigid rest: sunken chain resolves ON the plane, joints intact.
    {
        auto b = makeChain(-0.03f);
        std::vector<float> rest(b.size(), 0.22f);
        const BodySolveStats st = solveBodyContact(b.data(), (uint32_t)b.size(), &rigid, 1);
        bool onTop = true, linked = true;
        for (size_t i = 0; i < b.size(); ++i) {
            if (b[i].pos.y < b[i].radius - 2e-3f) onTop = false;
            if (i > 0) {
                const float dx = b[i].pos.x - b[i - 1].pos.x,
                            dy = b[i].pos.y - b[i - 1].pos.y,
                            dz = b[i].pos.z - b[i - 1].pos.z;
                const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (std::fabs(l - rest[i]) > 0.02f * rest[i]) linked = false;
            }
        }
        check(onTop && st.maxRigidPenetration <= 2e-3f, "T1 rigid: body rests ON the surface (no clipping)");
        check(linked, "T1b rigid: bone chain stays connected (lengths within 2%)");
    }

    // T2 — soft settle: penetration > 0 but <= budget * massFrac.
    {
        ContactSurface soft{};
        soft.soft = true; soft.indentBudget = 0.06f;
        auto b = makeChain(-0.03f);
        solveBodyContact(b.data(), (uint32_t)b.size(), &soft, 1);
        bool settled = true;
        for (auto& bn : b) {
            const float pen = bn.radius - bn.pos.y;
            if (pen < -2e-3f || pen > soft.indentBudget * bn.massFrac + 2e-3f) settled = false;
        }
        check(settled, "T2 soft: body settles IN (0 < depth <= budget*massFrac)");
    }

    // T3 — indentation bake: dent inside falloff only, depth scales with mass.
    {
        // 33x17 grid over 2.0 x 1.0 m at y=0; stride 8 (pos,normal,uv).
        const uint32_t NX = 33, NZ = 17, stride = 8;
        std::vector<float> verts((size_t)NX * NZ * stride, 0.0f);
        std::vector<uint32_t> idx;
        for (uint32_t z = 0; z < NZ; ++z)
            for (uint32_t x = 0; x < NX; ++x) {
                float* p = verts.data() + ((size_t)z * NX + x) * stride;
                p[0] = 2.0f * (float)x / (float)(NX - 1) - 1.0f;
                p[1] = 0.0f;
                p[2] = 1.0f * (float)z / (float)(NZ - 1) - 0.5f;
                p[4] = 1.0f;   // normal +Y
            }
        for (uint32_t z = 0; z + 1 < NZ; ++z)
            for (uint32_t x = 0; x + 1 < NX; ++x) {
                const uint32_t a = z * NX + x, bq = a + 1, c = a + NX, d = c + 1;
                idx.insert(idx.end(), { a, c, bq,  bq, c, d });
            }
        ContactSurface soft{};
        soft.soft = true; soft.indentBudget = 0.06f;
        ContactBone bone{};
        bone.pos = { 0.0f, 0.03f, 0.0f }; bone.radius = 0.05f; bone.massFrac = 0.5f;
        IndentParams ip{}; ip.falloffRadius = 0.25f;

        auto vHalf = verts;
        const uint32_t nHalf = bakeSoftIndentation(&bone, 1, soft, vHalf.data(),
                                                   NX * NZ, stride, 3,
                                                   idx.data(), (uint32_t)idx.size(), ip);
        float deepHalf = 0.0f; bool inside = true;
        for (uint32_t v = 0; v < NX * NZ; ++v) {
            const float* p0 = verts.data() + (size_t)v * stride;
            const float* p1 = vHalf.data() + (size_t)v * stride;
            const float d = p0[1] - p1[1];
            if (d > deepHalf) deepHalf = d;
            const float r = std::sqrt(p0[0] * p0[0] + p0[2] * p0[2]);
            if (d > 1e-5f && r >= ip.falloffRadius) inside = false;
        }
        bone.massFrac = 1.0f;
        auto vFull = verts;
        bakeSoftIndentation(&bone, 1, soft, vFull.data(), NX * NZ, stride, 3,
                            idx.data(), (uint32_t)idx.size(), ip);
        float deepFull = 0.0f;
        for (uint32_t v = 0; v < NX * NZ; ++v)
            deepFull = std::fmax(deepFull, verts[(size_t)v * stride + 1] - vFull[(size_t)v * stride + 1]);

        check(nHalf > 0 && inside, "T3 indent: displaced verts exist, all inside the falloff radius");
        check(deepHalf > 0.02f && deepHalf < 0.04f && deepFull > deepHalf * 1.5f,
              "T3b indent: depth ~= budget*massFrac and scales with mass");
        // Normals tilted inside the dent (no longer straight +Y at the rim).
        bool tilted = false;
        for (uint32_t v = 0; v < NX * NZ; ++v)
            if (vHalf[(size_t)v * stride + 4] < 0.999f &&
                vHalf[(size_t)v * stride + 1] < -1e-4f) { tilted = true; break; }
        check(tilted, "T3c indent: normals re-derived (dent walls tilt)");
    }

    // T4 — finite extent: a bone off the slab edge is NOT resolved by it.
    {
        ContactSurface slab{};
        slab.halfU = 0.5f; slab.halfV = 0.5f;
        ContactBone b{};
        b.pos = { 2.0f, -0.03f, 0.0f }; b.radius = 0.05f; b.massFrac = 0.2f;
        solveBodyContact(&b, 1, &slab, 1);
        check(b.pos.y < 0.0f, "T4 finite surface: bones beyond the slab edge are untouched");
    }

    // T5 — determinism: identical inputs -> bit-identical outputs.
    {
        auto b1 = makeChain(-0.03f);
        auto b2 = makeChain(-0.03f);
        solveBodyContact(b1.data(), (uint32_t)b1.size(), &rigid, 1);
        solveBodyContact(b2.data(), (uint32_t)b2.size(), &rigid, 1);
        check(std::memcmp(b1.data(), b2.data(), b1.size() * sizeof(ContactBone)) == 0,
              "T5 determinism: same inputs, identical outputs");
    }

    std::printf("body-contact: %d/%d passed\n", g_pass, g_total);
    return g_pass == g_total;
}

}} // namespace x3::phys
