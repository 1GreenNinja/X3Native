// Quadric-error-metric edge-collapse decimation. See mesh_decimate.h for the
// design, the provenance, and why placement is restricted to the two endpoints.
#include "mesh_decimate.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>

namespace x3::game {
namespace {

// ---- A symmetric 4x4 quadric, stored as its 10 unique entries ---------------
// Q = [a b c d; b e f g; c f h i; d g i j] with the plane p = (nx,ny,nz,d):
// Kp = p p^T. Evaluating at v = (x,y,z,1) gives the squared distance sum.
struct Quadric {
    double a = 0, b = 0, c = 0, d = 0;
    double e = 0, f = 0, g = 0;
    double h = 0, i = 0;
    double j = 0;

    void addPlane(double nx, double ny, double nz, double pd, double w) {
        a += w * nx * nx; b += w * nx * ny; c += w * nx * nz; d += w * nx * pd;
        e += w * ny * ny; f += w * ny * nz; g += w * ny * pd;
        h += w * nz * nz; i += w * nz * pd;
        j += w * pd * pd;
    }
    void add(const Quadric& o) {
        a += o.a; b += o.b; c += o.c; d += o.d;
        e += o.e; f += o.f; g += o.g;
        h += o.h; i += o.i; j += o.j;
    }
    double eval(double x, double y, double z) const {
        return a * x * x + 2 * b * x * y + 2 * c * x * z + 2 * d * x
             + e * y * y + 2 * f * y * z + 2 * g * y
             + h * z * z + 2 * i * z
             + j;
    }
};

struct V3 { double x = 0, y = 0, z = 0; };

inline V3 sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 cross(const V3& a, const V3& b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double len(const V3& a) { return std::sqrt(dot(a, a)); }

// Undirected welded-vertex edge key, order-independent.
inline uint64_t edgeKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return ((uint64_t)a << 32) | (uint64_t)b;
}

// Priority-queue entry. Lazy invalidation: an entry is stale if either endpoint
// has been collapsed away, or if the recorded stamp no longer matches the
// endpoints' current stamps (their quadrics moved since the cost was computed).
struct PqItem {
    double   cost;
    uint32_t a, b;      // welded ids; `b` collapses INTO `a`
    uint64_t stamp;
    bool operator<(const PqItem& o) const { return cost > o.cost; }  // min-heap
};

} // namespace

void meshBoundingSphere(const x3::rhi::MeshVertex* verts, uint32_t vcount,
                        float outCenter[3], float& outRadius) {
    outCenter[0] = outCenter[1] = outCenter[2] = 0.0f;
    outRadius = 0.0f;
    if (!verts || vcount == 0) return;
    float mn[3] = { verts[0].pos[0], verts[0].pos[1], verts[0].pos[2] };
    float mx[3] = { mn[0], mn[1], mn[2] };
    for (uint32_t v = 1; v < vcount; ++v)
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], verts[v].pos[k]);
            mx[k] = std::max(mx[k], verts[v].pos[k]);
        }
    for (int k = 0; k < 3; ++k) outCenter[k] = 0.5f * (mn[k] + mx[k]);
    for (uint32_t v = 0; v < vcount; ++v) {
        const float dx = verts[v].pos[0] - outCenter[0];
        const float dy = verts[v].pos[1] - outCenter[1];
        const float dz = verts[v].pos[2] - outCenter[2];
        outRadius = std::max(outRadius, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
}

DecimateResult decimateMesh(const x3::rhi::MeshVertex* verts, uint32_t vcount,
                            const uint32_t* idx, uint32_t icount,
                            float targetRatio, float priorError,
                            uint32_t minTriangles) {
    DecimateResult out;
    out.maxError = priorError;
    if (!verts || !idx || vcount == 0 || icount < 3) return out;
    out.indices.assign(idx, idx + icount);
    out.triangles = icount / 3;
    if (!(targetRatio > 0.0f) || targetRatio >= 1.0f) return out;

    // ---- 1. Weld by position -------------------------------------------------
    // Authored meshes split vertices at UV seams / hard creases; decimation must
    // see the surface as connected or nothing collapses. Quantise to 1e-4 m
    // (0.1 mm) — finer than any authored weld tolerance in this repo's assets.
    const double kWeldQ = 1.0e4;
    std::unordered_map<uint64_t, uint32_t> weldMap;
    weldMap.reserve(vcount * 2);
    std::vector<uint32_t> wid(vcount, 0);      // input vertex -> welded id
    std::vector<uint32_t> rep;                 // welded id -> a representative input vertex
    std::vector<V3>       wpos;                // welded id -> position
    rep.reserve(vcount);
    wpos.reserve(vcount);
    for (uint32_t v = 0; v < vcount; ++v) {
        const int64_t qx = (int64_t)std::llround((double)verts[v].pos[0] * kWeldQ);
        const int64_t qy = (int64_t)std::llround((double)verts[v].pos[1] * kWeldQ);
        const int64_t qz = (int64_t)std::llround((double)verts[v].pos[2] * kWeldQ);
        // 21 bits per axis is ample for +-1000 m at 0.1 mm; fold to 64 bits.
        const uint64_t key = ((uint64_t)(uint32_t)(qx * 73856093) << 32) ^
                             ((uint64_t)(uint32_t)(qy * 19349663) << 16) ^
                              (uint64_t)(uint32_t)(qz * 83492791);
        auto it = weldMap.find(key);
        if (it == weldMap.end()) {
            const uint32_t w = (uint32_t)rep.size();
            weldMap.emplace(key, w);
            rep.push_back(v);
            wpos.push_back({ (double)verts[v].pos[0], (double)verts[v].pos[1], (double)verts[v].pos[2] });
            wid[v] = w;
        } else {
            wid[v] = it->second;
        }
    }
    const uint32_t wcount = (uint32_t)rep.size();
    if (wcount < 4) return out;

    // ---- 2. Faces over welded ids -------------------------------------------
    struct Face { uint32_t w[3]; uint32_t src[3]; bool dead = false; };
    std::vector<Face> faces;
    faces.reserve(icount / 3);
    for (uint32_t t = 0; t + 2 < icount; t += 3) {
        Face f{};
        for (int k = 0; k < 3; ++k) { f.src[k] = idx[t + k]; f.w[k] = wid[idx[t + k]]; }
        if (f.w[0] == f.w[1] || f.w[1] == f.w[2] || f.w[0] == f.w[2]) continue;  // degenerate
        faces.push_back(f);
    }
    if (faces.size() < (size_t)minTriangles) return out;

    const uint32_t targetTris =
        std::max<uint32_t>(minTriangles, (uint32_t)((double)faces.size() * (double)targetRatio));

    // ---- 3. Vertex -> incident face adjacency --------------------------------
    std::vector<std::vector<uint32_t>> adj(wcount);
    for (uint32_t fi = 0; fi < (uint32_t)faces.size(); ++fi)
        for (int k = 0; k < 3; ++k) adj[faces[fi].w[k]].push_back(fi);

    // ---- 4. Per-vertex quadrics from area-weighted face planes ---------------
    std::vector<Quadric> Q(wcount);
    // Total FACE-plane weight accumulated into each vertex's quadric. Needed to
    // turn the quadric value (a weighted sum of SQUARED distances to planes)
    // back into a distance: dev = sqrt(v^T Q v / totalWeight) is the RMS
    // deviation of `v` from the surface those planes describe, in metres.
    // Boundary planes are deliberately excluded from the weight: their weight is
    // an artificial 1000x penalty meant to block collapses, not to describe the
    // surface, and including it would dilute the deviation toward zero.
    std::vector<double> Qw(wcount, 0.0);
    std::unordered_map<uint64_t, uint32_t> edgeFaceCount;
    edgeFaceCount.reserve(faces.size() * 3);
    for (const Face& f : faces) {
        const V3& p0 = wpos[f.w[0]];
        const V3& p1 = wpos[f.w[1]];
        const V3& p2 = wpos[f.w[2]];
        V3 n = cross(sub(p1, p0), sub(p2, p0));
        const double twoArea = len(n);
        if (twoArea <= 1e-18) continue;
        n.x /= twoArea; n.y /= twoArea; n.z /= twoArea;
        const double pd = -dot(n, p0);
        for (int k = 0; k < 3; ++k) { Q[f.w[k]].addPlane(n.x, n.y, n.z, pd, twoArea); Qw[f.w[k]] += twoArea; }
        for (int k = 0; k < 3; ++k) ++edgeFaceCount[edgeKey(f.w[k], f.w[(k + 1) % 3])];
    }

    // Boundary preservation (Garland & Heckbert's note): for every edge used by
    // exactly one face, add a heavily weighted plane through the edge and
    // PERPENDICULAR to that face, which pins the silhouette/open border.
    for (const auto& kv : edgeFaceCount) {
        if (kv.second != 1) continue;
        const uint32_t a = (uint32_t)(kv.first >> 32);
        const uint32_t b = (uint32_t)(kv.first & 0xFFFFFFFFu);
        // Find the one face carrying this edge to get its normal.
        V3 fn{};
        for (uint32_t fi : adj[a]) {
            const Face& f = faces[fi];
            bool hasB = (f.w[0] == b || f.w[1] == b || f.w[2] == b);
            if (!hasB) continue;
            V3 n = cross(sub(wpos[f.w[1]], wpos[f.w[0]]), sub(wpos[f.w[2]], wpos[f.w[0]]));
            const double l = len(n);
            if (l > 1e-18) { fn = { n.x / l, n.y / l, n.z / l }; }
            break;
        }
        V3 e = sub(wpos[b], wpos[a]);
        const double el = len(e);
        if (el <= 1e-12) continue;
        e.x /= el; e.y /= el; e.z /= el;
        V3 n = cross(e, fn);                       // in the face plane, normal to the edge
        const double nl = len(n);
        if (nl <= 1e-12) continue;
        n.x /= nl; n.y /= nl; n.z /= nl;
        const double pd = -dot(n, wpos[a]);
        const double w = 1000.0 * el;              // heavy: boundaries must survive
        Q[a].addPlane(n.x, n.y, n.z, pd, w);
        Q[b].addPlane(n.x, n.y, n.z, pd, w);
    }

    // ---- 5. Collapse loop ----------------------------------------------------
    std::vector<uint8_t>  dead(wcount, 0);
    std::vector<uint64_t> stamp(wcount, 0);       // bumped whenever a vertex's Q changes
    // Per-vertex SURFACE DEVIATION in metres, seeded with whatever the input
    // level already carried. Because Garland-Heckbert quadrics ACCUMULATE (the
    // merged vertex inherits every plane of both endpoints), the deviation at a
    // vertex is measured against the ORIGINAL surface, not against the previous
    // decimation step — so this is a running MAX, not a running sum. Summing is
    // what makes a naive implementation report a 19% error on a decimated unit
    // sphere whose real deviation is a fraction of a percent.
    std::vector<double>   dev(wcount, (double)priorError);
    std::vector<uint32_t> remap(wcount);          // welded id -> surviving welded id
    for (uint32_t w = 0; w < wcount; ++w) remap[w] = w;

    // Cost of collapsing `b` into `a` under subset placement: evaluate the merged
    // quadric at a's position. The caller tries both orders and keeps the cheaper.
    auto costAt = [&](uint32_t keep, uint32_t drop) {
        Quadric q = Q[keep]; q.add(Q[drop]);
        const V3& p = wpos[keep];
        return std::max(0.0, q.eval(p.x, p.y, p.z));
    };

    std::priority_queue<PqItem> pq;
    auto pushEdge = [&](uint32_t a, uint32_t b) {
        if (a == b || dead[a] || dead[b]) return;
        const double ca = costAt(a, b);            // b -> a
        const double cb = costAt(b, a);            // a -> b
        PqItem it{};
        if (ca <= cb) { it.cost = ca; it.a = a; it.b = b; }
        else          { it.cost = cb; it.a = b; it.b = a; }
        // Stamp MUST be built from the (possibly swapped) it.a/it.b, because the
        // pop-side staleness check recomputes it from those same two slots.
        it.stamp = stamp[it.a] ^ (stamp[it.b] << 1);
        pq.push(it);
    };
    for (const auto& kv : edgeFaceCount) {
        const uint32_t a = (uint32_t)(kv.first >> 32);
        const uint32_t b = (uint32_t)(kv.first & 0xFFFFFFFFu);
        pushEdge(a, b);
    }

    uint32_t liveTris = (uint32_t)faces.size();
    double   maxDev   = (double)priorError;

    while (liveTris > targetTris && !pq.empty()) {
        const PqItem it = pq.top();
        pq.pop();
        const uint32_t keep = it.a, drop = it.b;
        if (dead[keep] || dead[drop]) continue;
        if ((stamp[keep] ^ (stamp[drop] << 1)) != it.stamp) continue;   // stale cost

        // ---- normal-flip guard + LOCAL MAX DEVIATION ----
        // Every face incident to `drop` that does NOT contain `keep` survives with
        // `drop` moved onto `keep`. Reject the collapse if any of them inverts.
        // The same sweep measures how far `keep` sits off each of those faces'
        // ORIGINAL planes, which is the deviation this collapse actually
        // introduces at its worst point. That MAX is what a silhouette shows;
        // the quadric's area-weighted RMS (computed below) systematically
        // under-reports it on thin lattice geometry — a strut one triangle wide
        // contributes almost no area, so averaging hides exactly the feature
        // whose disappearance a viewer notices.
        bool flips = false;
        double stepMaxDev = 0.0;
        for (uint32_t fi : adj[drop]) {
            Face& f = faces[fi];
            if (f.dead) continue;
            if (f.w[0] == keep || f.w[1] == keep || f.w[2] == keep) continue;   // will die
            V3 p[3];
            for (int k = 0; k < 3; ++k) p[k] = wpos[f.w[k] == drop ? keep : f.w[k]];
            V3 nOld = cross(sub(wpos[f.w[1]], wpos[f.w[0]]), sub(wpos[f.w[2]], wpos[f.w[0]]));
            V3 nNew = cross(sub(p[1], p[0]), sub(p[2], p[0]));
            const double lo = len(nOld), ln = len(nNew);
            if (ln <= 1e-16) { flips = true; break; }                  // degenerated to a sliver
            if (lo > 1e-16 && dot(nOld, nNew) / (lo * ln) < 0.0) { flips = true; break; }
            if (lo > 1e-16) {
                const V3 un{ nOld.x / lo, nOld.y / lo, nOld.z / lo };
                stepMaxDev = std::max(stepMaxDev,
                                      std::abs(dot(un, sub(wpos[keep], wpos[f.w[0]]))));
            }
        }
        if (flips) continue;

        // ---- perform the collapse ----
        // The quadric value at the surviving position, normalised by the plane
        // weight it was built from, IS the surface deviation this collapse
        // introduces (metres). Note this is a DEVIATION FROM THE SURFACE, not
        // the distance the vertex travelled: sliding a vertex along a smooth
        // sphere moves it a whole edge length but barely leaves the surface, and
        // it is the latter that shows up on screen.
        const double wsum = Qw[keep] + Qw[drop];
        const double rmsDev = (wsum > 1e-12) ? std::sqrt(std::max(0.0, it.cost) / wsum) : 0.0;
        // Take the LARGER of the two estimates. The quadric RMS carries the whole
        // accumulated history (Garland-Heckbert quadrics inherit every plane of
        // both endpoints, so it measures deviation from the ORIGINAL surface, not
        // just the previous level); the local max catches the thin-feature case
        // the RMS averages away. Neither alone is enough.
        dev[keep] = std::max({ dev[keep], dev[drop], rmsDev, stepMaxDev });
        maxDev    = std::max(maxDev, dev[keep]);

        for (uint32_t fi : adj[drop]) {
            Face& f = faces[fi];
            if (f.dead) continue;
            const bool touchesKeep = (f.w[0] == keep || f.w[1] == keep || f.w[2] == keep);
            if (touchesKeep) { f.dead = true; --liveTris; continue; }
            for (int k = 0; k < 3; ++k)
                if (f.w[k] == drop) { f.w[k] = keep; f.src[k] = rep[keep]; }
            adj[keep].push_back(fi);
        }
        Q[keep].add(Q[drop]);
        Qw[keep] += Qw[drop];
        dead[drop] = 1;
        remap[drop] = keep;
        ++stamp[keep];
        ++out.collapses;

        // adj[keep] only ever grows; compact out dead faces occasionally so the
        // re-cost sweep stays proportional to the LIVE fan, not the history.
        if (adj[keep].size() > 64) {
            auto& v = adj[keep];
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](uint32_t fi) { return faces[fi].dead; }),
                    v.end());
        }

        // Re-cost every edge still incident to `keep`.
        for (uint32_t fi : adj[keep]) {
            const Face& f = faces[fi];
            if (f.dead) continue;
            for (int k = 0; k < 3; ++k) {
                const uint32_t o = f.w[k];
                if (o != keep && !dead[o]) pushEdge(keep, o);
            }
        }
    }

    // ---- 6. Emit ------------------------------------------------------------
    // Faces reference welded ids; map each back to a REAL input vertex so the
    // result indexes the original vertex buffer (subset placement).
    out.indices.clear();
    out.indices.reserve((size_t)liveTris * 3);
    for (const Face& f : faces) {
        if (f.dead) continue;
        if (f.w[0] == f.w[1] || f.w[1] == f.w[2] || f.w[0] == f.w[2]) continue;
        for (int k = 0; k < 3; ++k) out.indices.push_back(f.src[k]);
    }
    out.triangles = (uint32_t)(out.indices.size() / 3);
    out.maxError  = (float)maxDev;
    return out;
}

} // namespace x3::game
