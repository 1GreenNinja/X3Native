#pragma once
// ============================================================================
// labzero_rail — the LAB ZERO gameplay rail (LABZERO_3D_ADDENDUM.md P0.3).
//
// A Catmull-Rom spline sampled into an arc-length table: position + tangent
// lookup by arc-length parameter s (metres from the rail start). P0 ships TWO
// nodes (a straight 400 m run); curving comes free later by adding nodes —
// the lookup API does not change.
//
// std-only by design (mirrors the labzero sim-purity rule): no engine includes,
// so the rail math is trivially testable headlessly.
// No GPL / id Tech / RBDOOM source consulted.
// ============================================================================

#include <cmath>
#include <cstdint>
#include <vector>

namespace x3::game {

struct RailNode { float x, y, z; };

class LabZeroRail {
public:
    // Build the arc-length table from the node list. 2+ nodes required.
    // `samplesPerSeg` controls table resolution (32 ≈ centimetre accuracy on
    // a 400 m run — the camera smoothing eats any residual).
    void init(const std::vector<RailNode>& nodes, uint32_t samplesPerSeg = 32) {
        m_pts.clear(); m_cum.clear();
        if (nodes.size() < 2) return;
        const uint32_t segs = (uint32_t)nodes.size() - 1;
        for (uint32_t i = 0; i < segs; ++i) {
            const RailNode& p0 = nodes[i == 0 ? 0 : i - 1];
            const RailNode& p1 = nodes[i];
            const RailNode& p2 = nodes[i + 1];
            const RailNode& p3 = nodes[(i + 2 < nodes.size()) ? i + 2 : i + 1];
            const uint32_t last = (i == segs - 1) ? samplesPerSeg : samplesPerSeg - 1;
            for (uint32_t k = 0; k <= last; ++k) {
                const float t = (float)k / (float)samplesPerSeg;
                m_pts.push_back(catmullRom(p0, p1, p2, p3, t));
            }
        }
        m_cum.resize(m_pts.size(), 0.0f);
        for (size_t i = 1; i < m_pts.size(); ++i)
            m_cum[i] = m_cum[i - 1] + dist(m_pts[i - 1], m_pts[i]);
    }

    float length() const { return m_cum.empty() ? 0.0f : m_cum.back(); }

    // Position on the rail at arc-length s (clamped to [0, length]).
    RailNode pos(float s) const {
        if (m_pts.size() < 2) return RailNode{0, 0, 0};
        size_t i; float f; locate(s, i, f);
        return lerp(m_pts[i], m_pts[i + 1], f);
    }

    // Unit tangent at arc-length s (finite difference over the table).
    RailNode tangent(float s) const {
        if (m_pts.size() < 2) return RailNode{1, 0, 0};
        size_t i; float f; locate(s, i, f);
        RailNode d{ m_pts[i + 1].x - m_pts[i].x,
                    m_pts[i + 1].y - m_pts[i].y,
                    m_pts[i + 1].z - m_pts[i].z };
        const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (l < 1e-6f) return RailNode{1, 0, 0};
        return RailNode{ d.x / l, d.y / l, d.z / l };
    }

    // Arc-length parameter of the table point nearest to (x,z) — a LOCAL search
    // seeded at sHint (character tracking never jumps the rail), clamped to a
    // +-window so a hairpin elsewhere on the spline can never steal the lock.
    float closestParam(float x, float z, float sHint, float window = 25.0f) const {
        if (m_pts.size() < 2) return 0.0f;
        float bestS = sHint, bestD = 1e30f;
        const float s0 = sHint - window, s1 = sHint + window;
        for (size_t i = 0; i < m_pts.size(); ++i) {
            if (m_cum[i] < s0 || m_cum[i] > s1) continue;
            const float dx = m_pts[i].x - x, dz = m_pts[i].z - z;
            const float d = dx * dx + dz * dz;
            if (d < bestD) { bestD = d; bestS = m_cum[i]; }
        }
        return bestS;
    }

private:
    static RailNode catmullRom(const RailNode& p0, const RailNode& p1,
                               const RailNode& p2, const RailNode& p3, float t) {
        const float t2 = t * t, t3 = t2 * t;
        auto cr = [&](float a, float b, float c, float d) {
            return 0.5f * ((2.0f * b) + (-a + c) * t +
                           (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
                           (-a + 3.0f * b - 3.0f * c + d) * t3);
        };
        return RailNode{ cr(p0.x, p1.x, p2.x, p3.x),
                         cr(p0.y, p1.y, p2.y, p3.y),
                         cr(p0.z, p1.z, p2.z, p3.z) };
    }
    static float dist(const RailNode& a, const RailNode& b) {
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    static RailNode lerp(const RailNode& a, const RailNode& b, float f) {
        return RailNode{ a.x + (b.x - a.x) * f,
                         a.y + (b.y - a.y) * f,
                         a.z + (b.z - a.z) * f };
    }
    // Table segment containing arc-length s: index i and fraction f in [i, i+1].
    void locate(float s, size_t& i, float& f) const {
        if (s <= 0.0f) { i = 0; f = 0.0f; return; }
        if (s >= m_cum.back()) { i = m_pts.size() - 2; f = 1.0f; return; }
        size_t lo = 0, hi = m_cum.size() - 1;
        while (lo + 1 < hi) {
            const size_t mid = (lo + hi) / 2;
            if (m_cum[mid] <= s) lo = mid; else hi = mid;
        }
        i = lo;
        const float span = m_cum[lo + 1] - m_cum[lo];
        f = (span > 1e-6f) ? (s - m_cum[lo]) / span : 0.0f;
    }

    std::vector<RailNode> m_pts;   // sampled polyline
    std::vector<float>    m_cum;   // cumulative arc length per sample
};

} // namespace x3::game
