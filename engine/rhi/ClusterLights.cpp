// ===========================================================================
// ClusterLights.cpp — froxel-grid light assignment. See ClusterLights.h for the
// technique, the clean-room provenance and the overflow policy.
//
// CPU OR COMPUTE? This is CPU, single-threaded, and deliberately so for v1:
//
//   1. DETERMINISM IS A HARD REQUIREMENT HERE. This repo gates on md5-identical
//      screenshots (--test-primlight, --screenshot-city, the shots/ set). A
//      parallel scatter or a GPU atomic-append pass produces per-froxel lists in
//      a race-dependent ORDER, which changes float accumulation order in the
//      shader's light loop, which changes the low bits of the image, which breaks
//      every image gate in the tree. A serial ascending-index scatter is exactly
//      reproducible.
//   2. IT IS NOT THE BOTTLENECK. The loop is O(lights x froxels-actually-touched),
//      not O(lights x 3456): each light's screen-space extent narrows the tile
//      range and its depth extent narrows the slice range before a single
//      sphere-AABB test runs. Measured cost is reported in the lane writeup.
//   3. THE RHI LAYER HAS NO IJobSystem HANDLE. VulkanRenderDevice does not take
//      one and nothing in engine/rhi does; wiring one in is a layering change
//      that buys nothing until (2) says otherwise.
//   4. A COMPUTE PASS WOULD HAVE TO EARN ITS SURFACE AREA. The engine enforces
//      "no pipeline may be created after frame 1" (boot precompile), so a cluster
//      compute pass costs a shader, a pipeline, a descriptor set, two barriers
//      and a boot-time precompile slot — for a pass that is not yet measurable.
//
// If the light count ever grows past what this comfortably absorbs, the fixed-
// stride layout is already the shape a compute pass wants: swap the serial
// append for a two-phase count/fill so the ORDER stays ascending-by-light-index
// and the image gates keep holding.
// ===========================================================================

#include "ClusterLights.h"
#include "IRenderDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace x3 { namespace rhi {

namespace {

inline float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
inline void norm3(float v[3]) {
    const float l = std::sqrt(dot3(v, v));
    if (l > 1e-20f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

} // namespace

ClusterView makeClusterView(const float eye[3], const float fwd[3], const float up[3],
                            float fovYDegrees, float aspect, float zNear, float zFar) {
    ClusterView v{};
    v.camPos[0] = eye[0]; v.camPos[1] = eye[1]; v.camPos[2] = eye[2];

    float f[3] = { fwd[0], fwd[1], fwd[2] };
    norm3(f);
    // glm::lookAt's basis, exactly: s = normalize(cross(f, up)), u = cross(s, f).
    float s[3];
    cross3(f, up, s);
    norm3(s);
    float u[3];
    cross3(s, f, u);

    v.camFwd[0] = f[0];   v.camFwd[1] = f[1];   v.camFwd[2] = f[2];
    v.camRight[0] = s[0]; v.camRight[1] = s[1]; v.camRight[2] = s[2];
    v.camUp[0] = u[0];    v.camUp[1] = u[1];    v.camUp[2] = u[2];

    const float kPi = 3.14159265358979323846f;
    v.tanHalfFovY = std::tan(fovYDegrees * (kPi / 180.0f) * 0.5f);
    v.tanHalfFovX = v.tanHalfFovY * std::max(aspect, 1e-4f);

    // zNear must be strictly positive — the whole slicing law is log(z / zNear).
    v.zNear = std::max(zNear, 1e-3f);
    v.zFar  = std::max(zFar, v.zNear * 1.0001f);

    const float logRatio = std::log(v.zFar / v.zNear);
    v.sliceScale = (float)kClusterGridZ / logRatio;
    v.sliceBias  = -((float)kClusterGridZ * std::log(v.zNear) / logRatio);

    // Lookup tables — see ClusterLights.h. Built ONCE per frame here so the
    // assignment inner loop has no pow() and no divide in it.
    const float ratio = v.zFar / v.zNear;
    for (uint32_t k = 0; k <= kClusterGridZ; ++k)
        v.sliceZ[k] = (k == 0) ? v.zNear
                    : (k == kClusterGridZ) ? v.zFar
                    : v.zNear * std::pow(ratio, (float)k / (float)kClusterGridZ);
    for (uint32_t i = 0; i <= kClusterGridX; ++i)
        v.edgeX[i] = (2.0f * (float)i / (float)kClusterGridX - 1.0f) * v.tanHalfFovX;
    for (uint32_t j = 0; j <= kClusterGridY; ++j)
        v.edgeY[j] = (1.0f - 2.0f * (float)j / (float)kClusterGridY) * v.tanHalfFovY;
    return v;
}

uint32_t clusterSliceForViewZ(const ClusterView& v, float viewZ) {
    // Anything at or in front of the near plane collapses into slice 0; log() of a
    // non-positive depth is meaningless and a fragment must always resolve to a
    // real froxel.
    if (!(viewZ > v.zNear)) return 0;
    const float s = std::log(viewZ) * v.sliceScale + v.sliceBias;
    if (!(s > 0.0f)) return 0;
    const uint32_t k = (uint32_t)s;
    return k >= kClusterGridZ ? kClusterGridZ - 1 : k;
}

float clusterSliceNearZ(const ClusterView& v, uint32_t k) {
    return v.sliceZ[k >= kClusterGridZ ? kClusterGridZ : k];
}

void clusterFroxelBoundsView(const ClusterView& v, uint32_t ix, uint32_t iy, uint32_t iz,
                             float outMin[3], float outMax[3]) {
    const float zn = v.sliceZ[iz];
    const float zf = v.sliceZ[iz + 1];

    // Screen fraction -> NDC. u = 0 is the LEFT edge, and v = 0 is the TOP edge
    // (gl_FragCoord.y grows downward in Vulkan; the device's proj[1][1] *= -1
    // makes view-space +Y land at gl_FragCoord.y == 0). So:
    //     ndcX =  2u - 1
    //     ndcY =  1 - 2v          (+1 at the top, i.e. view +Y)
    // Both are baked into edgeX / edgeY at makeClusterView time.
    const float xL = v.edgeX[ix];         // view x per unit depth, left edge
    const float xR = v.edgeX[ix + 1];
    const float yT = v.edgeY[iy];         // upper edge (larger)
    const float yB = v.edgeY[iy + 1];     // lower edge

    // The froxel is a truncated pyramid: its view-space x/y bounds scale linearly
    // with depth, so the extremes over [zn, zf] sit at one of the two depths. The
    // per-unit-depth edges can be either sign (left of centre is negative), so take
    // the min/max over all four products rather than assuming which end wins.
    const float xs[4] = { xL * zn, xL * zf, xR * zn, xR * zf };
    const float ys[4] = { yT * zn, yT * zf, yB * zn, yB * zf };

    outMin[0] = *std::min_element(xs, xs + 4);
    outMax[0] = *std::max_element(xs, xs + 4);
    outMin[1] = *std::min_element(ys, ys + 4);
    outMax[1] = *std::max_element(ys, ys + 4);
    outMin[2] = zn;
    outMax[2] = zf;
}

ClusterBuildResult buildClusterLightLists(const ClusterView& v,
                                          const PointLight* lights, uint32_t count,
                                          uint32_t* outCounts, uint32_t* outIndices) {
    ClusterBuildResult r{};
    if (!outCounts || !outIndices) return r;

    std::memset(outCounts, 0, sizeof(uint32_t) * kClusterCount);
    if (!lights || count == 0) return r;

    const uint32_t n = std::min(count, kMaxSceneLights);
    r.lightsConsidered = n;

    // Tracks which froxels have already been counted in clustersOverflowed, so a
    // froxel that drops twenty lights is still reported as ONE overflowing froxel.
    // (A bitset over 3456 entries; cheap enough to just clear per call.)
    static thread_local uint8_t s_overflowed[kClusterCount];
    std::memset(s_overflowed, 0, sizeof(s_overflowed));

    for (uint32_t li = 0; li < n; ++li) {
        const PointLight& L = lights[li];

        // --- View-space centre + padded radius -----------------------------
        const float d[3] = { L.pos[0] - v.camPos[0],
                             L.pos[1] - v.camPos[1],
                             L.pos[2] - v.camPos[2] };
        const float cx = dot3(d, v.camRight);
        const float cy = dot3(d, v.camUp);
        const float cz = dot3(d, v.camFwd);
        const float R  = std::max(L.range, 0.0f) * (1.0f + kClusterRadiusPadRel) + kClusterRadiusPad;
        if (!(R > 0.0f)) { ++r.lightsCulled; continue; }

        // --- Depth-slice range ---------------------------------------------
        // Reject outright when the sphere is entirely behind the near plane or
        // entirely past the far plane: those lights cost nothing at all.
        const float zLo = cz - R;
        const float zHi = cz + R;
        if (zHi <= v.zNear || zLo >= v.zFar) { ++r.lightsCulled; continue; }

        // Widen by one slice on each side. A froxel one slice NEARER than the
        // sphere's near depth still has its FAR face exactly at the sphere's
        // tangency point, so at equality it does intersect — and the reference
        // definition (sphere vs froxel AABB, `<=`) includes it. One extra slice
        // costs a handful of rejected AABB tests and removes the knife edge.
        uint32_t k0 = clusterSliceForViewZ(v, std::max(zLo, v.zNear));
        uint32_t k1 = clusterSliceForViewZ(v, std::min(zHi, v.zFar));
        if (k0 > 0) --k0;
        if (k1 + 1 < kClusterGridZ) ++k1;

        // --- Screen-space tile range ---------------------------------------
        // THE REFERENCE this narrowing must never lose against is "sphere vs
        // froxel AABB". A froxel's AABB is the box around a TRUNCATED PYRAMID, so
        // which of its faces is nearest the sphere depends on the tile's side of
        // the view axis AND on the slice depth — the inner edge can be at the near
        // face or at the far face. Inverting the projection to get a tile range
        // therefore does NOT work, and the two attempts that tried it both lost
        // outer froxels (caught by A4 and A9d).
        //
        // So do not invert it: SCAN the tile edges directly. Over the slice span
        // k0..k1 the froxel x-bounds live inside [edge*zA, edge*zB] with
        // zA = the span's near depth and zB its far depth, and a tile is a
        // candidate exactly when that interval overlaps [cx-R, cx+R]. This is the
        // projection of the union AABB onto the axis — a guaranteed superset of
        // the per-froxel test, computed from the SAME edge*z products so there is
        // no rounding disagreement. 16 + 9 comparisons per light; free.
        const float zA = v.sliceZ[k0];
        const float zB = v.sliceZ[k1 + 1];
        const float sxLo = cx - R, sxHi = cx + R;
        const float syLo = cy - R, syHi = cy + R;

        int i0 = -1, i1 = -1, j0 = -1, j1 = -1;
        for (uint32_t i = 0; i < kClusterGridX; ++i) {
            const float lo = std::min(v.edgeX[i] * zA, v.edgeX[i] * zB);
            const float hi = std::max(v.edgeX[i + 1] * zA, v.edgeX[i + 1] * zB);
            if (hi >= sxLo && lo <= sxHi) { if (i0 < 0) i0 = (int)i; i1 = (int)i; }
        }
        for (uint32_t j = 0; j < kClusterGridY; ++j) {
            // edgeY DESCENDS (j = 0 is the top of the screen), so the lower bound
            // comes from edgeY[j+1] and the upper from edgeY[j].
            const float lo = std::min(v.edgeY[j + 1] * zA, v.edgeY[j + 1] * zB);
            const float hi = std::max(v.edgeY[j] * zA, v.edgeY[j] * zB);
            if (hi >= syLo && lo <= syHi) { if (j0 < 0) j0 = (int)j; j1 = (int)j; }
        }
        if (i0 < 0 || j0 < 0) { ++r.lightsCulled; continue; }

        // --- Exact sphere-vs-froxel-AABB over the narrowed range ------------
        // Hoisted by axis: the z bounds are constant across a slice and the y
        // bounds across a row, so the per-axis distance is accumulated OUTWARD-IN
        // and a whole slice or a whole row can be rejected before its inner loop
        // runs. The arithmetic is the SAME expressions clusterFroxelBoundsView
        // uses (same edge*z products, same min/max order), so this stays bit-for-
        // bit the brute-force reference the self-test compares against.
        const float R2 = R * R;
        bool anyHit = false;
        for (uint32_t iz = k0; iz <= k1; ++iz) {
            const float zn = v.sliceZ[iz], zf = v.sliceZ[iz + 1];
            const float ez = cz < zn ? (zn - cz) : cz > zf ? (cz - zf) : 0.0f;
            const float d2z = ez * ez;
            if (d2z > R2) continue;                       // whole slice is out of reach

            for (uint32_t iy = (uint32_t)j0; iy <= (uint32_t)j1; ++iy) {
                const float yT = v.edgeY[iy], yB = v.edgeY[iy + 1];
                const float ys[4] = { yT * zn, yT * zf, yB * zn, yB * zf };
                const float mnY = *std::min_element(ys, ys + 4);
                const float mxY = *std::max_element(ys, ys + 4);
                const float ey = cy < mnY ? (mnY - cy) : cy > mxY ? (cy - mxY) : 0.0f;
                const float d2zy = d2z + ey * ey;
                if (d2zy > R2) continue;                  // whole row is out of reach

                for (uint32_t ix = (uint32_t)i0; ix <= (uint32_t)i1; ++ix) {
                    const float xL = v.edgeX[ix], xR = v.edgeX[ix + 1];
                    const float xs[4] = { xL * zn, xL * zf, xR * zn, xR * zf };
                    const float mnX = *std::min_element(xs, xs + 4);
                    const float mxX = *std::max_element(xs, xs + 4);
                    const float ex = cx < mnX ? (mnX - cx) : cx > mxX ? (cx - mxX) : 0.0f;
                    if (d2zy + ex * ex > R2) continue;

                    const uint32_t ci = clusterIndex(ix, iy, iz);
                    uint32_t& cnt = outCounts[ci];
                    if (cnt >= kMaxLightsPerCluster) {
                        // OVERFLOW: the froxel is full. Drop this light, COUNT it.
                        ++r.overflows;
                        if (!s_overflowed[ci]) { s_overflowed[ci] = 1; ++r.clustersOverflowed; }
                        anyHit = true;   // it DID overlap; it just did not fit
                        continue;
                    }
                    outIndices[(size_t)ci * kMaxLightsPerCluster + cnt] = li;
                    ++cnt;
                    ++r.assignments;
                    if (cnt > r.maxClusterLoad) r.maxClusterLoad = cnt;
                    anyHit = true;
                }
            }
        }
        if (anyHit) ++r.lightsVisible; else ++r.lightsCulled;
    }
    return r;
}

}} // namespace x3::rhi
