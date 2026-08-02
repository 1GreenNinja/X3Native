#pragma once
// ============================================================================
// labzero_rail — the gameplay SPLINE for the Lab Zero 3D side-scroller rail
// (LABZERO_3D_ADDENDUM.md P0 §3).
//
// A side-scroller in a real 3D world is "one world, one character, one
// constraint". This header is the constraint: a Catmull-Rom curve through N
// nodes, parameterised by ARC LENGTH so a constant `s` rate is a constant
// world-space speed (a raw spline parameter is not — it bunches on curves and
// the character would speed up and slow down for no reason the player can see).
//
// The rail carries no physics and no rendering: the host steers the Jolt
// character toward it every fixed step (tangent for input, lateral correction
// for the glue) and hangs the camera off it. Weight w = 1 is fully railed;
// P2's cave seam blends w -> 0 and the same rail keeps tracking nearest-point
// so re-attachment on exit is seamless.
//
// Pure std-lib + phys::Vec3 — no GLFW, no device, no Jolt. Headless-testable.
//
// AXES (CLAUDE.md law): right-handed, Y-up, -Z forward, 1 unit = 1 metre.
// `side()` is tangent x up, so it points to the character's RIGHT along +X-ish
// travel — the camera hangs on the +side face and the world reads left-to-right.
// ============================================================================

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/physics/IPhysicsWorld.h"

namespace x3 { namespace game { namespace labzero {

// One control point. Y is advisory: when a ground function is bound the rail
// rides the terrain height at (x,z) instead, so the rail can never float above
// or sink under a streamed heightfield (X3_WORLD_RULES rule 4, contact surface).
struct RailNode { float x = 0.0f, y = 0.0f, z = 0.0f; };

class Rail {
public:
    // Ground feed: world (x,z) -> surface Y. Bind the terrain height query and
    // the rail hugs the mountain; leave it unbound and the node Y values are
    // used verbatim (that is what the headless tests do — no terrain needed).
    using GroundFn = std::function<float(float x, float z)>;
    void setGroundFn(GroundFn fn) { m_ground = std::move(fn); rebuild(); }

    // Height (metres) the rail floats ABOVE the ground sample. The character's
    // capsule feet sit on the terrain; the rail is the path those feet follow,
    // so this stays 0 for the character and is only lifted for the camera rig.
    void setGroundOffset(float m) { m_groundOffset = m; rebuild(); }

    // Replace the node list. Needs >= 2 nodes; fewer is a no-op (a rail with one
    // point has no tangent, and silently producing a zero tangent would send the
    // character nowhere with no error — better to refuse).
    void setNodes(const RailNode* nodes, uint32_t count) {
        if (!nodes || count < 2) return;
        m_nodes.assign(nodes, nodes + count);
        rebuild();
    }

    // A straight run of `lengthM` metres from `start` along the unit heading
    // (hx,hz) — the P0 shape (two nodes). Curving comes free later by calling
    // setNodes() with more.
    void setStraight(const x3::phys::Vec3& start, float hx, float hz, float lengthM) {
        const float inv = 1.0f / std::max(1e-4f, std::sqrt(hx * hx + hz * hz));
        hx *= inv; hz *= inv;
        RailNode n[2];
        n[0] = { start.x,                  start.y, start.z };
        n[1] = { start.x + hx * lengthM,   start.y, start.z + hz * lengthM };
        setNodes(n, 2);
    }

    bool     valid()  const { return m_nodes.size() >= 2 && m_arc.size() >= 2; }
    uint32_t nodeCount() const { return (uint32_t)m_nodes.size(); }
    // Total arc length in metres.
    float    length() const { return m_arc.empty() ? 0.0f : m_arc.back(); }

    // ---- Sampling (all take arc length `s` in metres, clamped to the rail) ---

    x3::phys::Vec3 point(float s) const {
        if (!valid()) return x3::phys::Vec3{ 0, 0, 0 };
        return sampleAt(paramForArc(clampS(s)));
    }

    // Unit tangent (direction of travel at +s). Never zero on a valid rail.
    x3::phys::Vec3 tangent(float s) const {
        if (!valid()) return x3::phys::Vec3{ 1, 0, 0 };
        const float h = 0.05f;                       // 5 cm central difference
        const float s0 = clampS(s - h), s1 = clampS(s + h);
        x3::phys::Vec3 a = point(s0), b = point(s1);
        return normalize(x3::phys::Vec3{ b.x - a.x, b.y - a.y, b.z - a.z },
                         x3::phys::Vec3{ 1, 0, 0 });
    }

    // Unit side axis = tangent x up, flattened to the ground plane. This is the
    // axis the camera offsets along and the axis the rail-plane aim pitches
    // about (P1). Horizontal by construction so a sloped rail never rolls the
    // side-on camera.
    x3::phys::Vec3 side(float s) const {
        const x3::phys::Vec3 t = tangent(s);
        // t x (0,1,0) = (t.z*1 - t.y*0, t.x*0 - t.z*0, t.y*0 - t.x*1) = (t.z, 0, -t.x)
        return normalize(x3::phys::Vec3{ t.z, 0.0f, -t.x }, x3::phys::Vec3{ 0, 0, -1 });
    }

    // Arc length of the closest point on the rail to `p`. Coarse scan over the
    // arc table then a local refine — the character moves a few cm per step so
    // this is exact enough to keep the lateral correction honest, and it is what
    // makes re-attachment after a free-3D excursion (P2) seamless.
    float nearestS(const x3::phys::Vec3& p) const {
        if (!valid()) return 0.0f;
        float bestS = 0.0f, bestD2 = 1e30f;
        const uint32_t n = (uint32_t)m_samples.size();
        for (uint32_t i = 0; i < n; ++i) {
            const float d2 = dist2Planar(m_samples[i], p);
            if (d2 < bestD2) { bestD2 = d2; bestS = m_arc[i]; }
        }
        // Refine within +/- one sample spacing.
        const float step = (n > 1) ? (length() / (float)(n - 1)) : 0.0f;
        for (int it = 0; it < 6 && step > 0.0f; ++it) {
            const float h = step / (float)(2 << it);
            const float sa = clampS(bestS - h), sb = clampS(bestS + h);
            const float da = dist2Planar(point(sa), p), db = dist2Planar(point(sb), p);
            if (da < bestD2)      { bestD2 = da; bestS = sa; }
            else if (db < bestD2) { bestD2 = db; bestS = sb; }
        }
        return bestS;
    }

    // Planar (XZ) offset of `p` from the rail at `s`, expressed along side().
    // Positive = p is on the +side of the rail. The host feeds this straight
    // into the lateral correction; the sign is what makes the correction pull
    // the character back rather than push it away.
    float lateralOffset(float s, const x3::phys::Vec3& p) const {
        const x3::phys::Vec3 r = point(s), sd = side(s);
        return (p.x - r.x) * sd.x + (p.z - r.z) * sd.z;
    }

private:
    std::vector<RailNode>       m_nodes;
    std::vector<x3::phys::Vec3> m_samples;   // densely sampled curve points
    std::vector<float>          m_arc;       // cumulative arc length per sample
    GroundFn                    m_ground;
    float                       m_groundOffset = 0.0f;

    // Samples per node span. 64 over a 400 m two-node rail is ~6 m spacing for
    // the coarse scan, refined analytically after — cheap and plenty.
    static constexpr uint32_t kSamplesPerSpan = 64;

    float clampS(float s) const {
        const float L = length();
        return s < 0.0f ? 0.0f : (s > L ? L : s);
    }

    static float dist2Planar(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
        const float dx = a.x - b.x, dz = a.z - b.z;
        return dx * dx + dz * dz;
    }

    static x3::phys::Vec3 normalize(const x3::phys::Vec3& v, const x3::phys::Vec3& fallback) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (l < 1e-6f) return fallback;
        return x3::phys::Vec3{ v.x / l, v.y / l, v.z / l };
    }

    // Clamped Catmull-Rom: endpoints are duplicated so a 2-node rail is exactly
    // the straight segment between them (no overshoot, no phantom curvature).
    const RailNode& nodeAt(int i) const {
        const int last = (int)m_nodes.size() - 1;
        return m_nodes[(size_t)(i < 0 ? 0 : (i > last ? last : i))];
    }

    // Curve sample at spline parameter u in [0, nodeCount-1].
    x3::phys::Vec3 sampleAt(float u) const {
        const int last = (int)m_nodes.size() - 1;
        int i = (int)std::floor(u);
        if (i < 0) i = 0;
        if (i > last - 1) i = last - 1;
        const float t = u - (float)i;
        const RailNode& p0 = nodeAt(i - 1);
        const RailNode& p1 = nodeAt(i);
        const RailNode& p2 = nodeAt(i + 1);
        const RailNode& p3 = nodeAt(i + 2);
        const float t2 = t * t, t3 = t2 * t;
        auto cr = [&](float a, float b, float c, float d) {
            return 0.5f * ((2.0f * b) + (-a + c) * t +
                           (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
                           (-a + 3.0f * b - 3.0f * c + d) * t3);
        };
        x3::phys::Vec3 out{ cr(p0.x, p1.x, p2.x, p3.x),
                            cr(p0.y, p1.y, p2.y, p3.y),
                            cr(p0.z, p1.z, p2.z, p3.z) };
        if (m_ground) out.y = m_ground(out.x, out.z) + m_groundOffset;
        return out;
    }

    // Spline parameter whose arc length is `s` (linear search over the table —
    // monotonic, so a lerp between bracketing samples is exact to sample scale).
    float paramForArc(float s) const {
        const uint32_t n = (uint32_t)m_arc.size();
        if (n < 2) return 0.0f;
        if (s <= 0.0f) return 0.0f;
        if (s >= m_arc.back()) return (float)(m_nodes.size() - 1);
        uint32_t lo = 0, hi = n - 1;
        while (hi - lo > 1) {
            const uint32_t mid = (lo + hi) / 2;
            if (m_arc[mid] <= s) lo = mid; else hi = mid;
        }
        const float seg = m_arc[hi] - m_arc[lo];
        const float f = (seg > 1e-6f) ? (s - m_arc[lo]) / seg : 0.0f;
        const float uStep = (float)(m_nodes.size() - 1) / (float)(n - 1);
        return ((float)lo + f) * uStep;
    }

    void rebuild() {
        m_samples.clear();
        m_arc.clear();
        if (m_nodes.size() < 2) return;
        const uint32_t spans = (uint32_t)m_nodes.size() - 1;
        const uint32_t n = spans * kSamplesPerSpan + 1;
        m_samples.reserve(n);
        m_arc.reserve(n);
        float acc = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            const float u = (float)spans * (float)i / (float)(n - 1);
            const x3::phys::Vec3 p = sampleAt(u);
            if (i > 0) {
                const x3::phys::Vec3& q = m_samples.back();
                const float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
                acc += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            m_samples.push_back(p);
            m_arc.push_back(acc);
        }
    }
};

}}} // namespace x3::game::labzero
