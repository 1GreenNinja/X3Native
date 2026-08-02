#pragma once
// ============================================================================
// Mesh decimation — quadric error metric (QEM) edge collapse, SUBSET placement.
//
// CLEAN-ROOM, original work. Written from the published technique only:
//   * Garland & Heckbert, "Surface Simplification Using Quadric Error Metrics"
//     (SIGGRAPH 1997) — the fundamental quadric Kp = pp^T of a face plane, the
//     per-vertex quadric as the sum of incident face quadrics, and the edge
//     collapse cost v^T Q v.
//   * Garland & Heckbert's follow-up note on boundary preservation (a heavily
//     weighted plane perpendicular to the face through each boundary edge).
//   * The standard normal-flip guard described in the simplification literature
//     (reject a collapse that inverts any surviving incident face).
// No GPL / id Tech / RBDOOM / Unreal / meshoptimizer source was consulted.
// See docs/CLEANROOM_PROCESS.md.
//
// WHY SUBSET PLACEMENT (collapse to an EXISTING endpoint, never to the optimal
// v = Q^-1 b):
//   1. The output index buffer then references ONLY vertices that already exist
//      in the input, so every LOD in a chain can SHARE ONE VERTEX BUFFER. That
//      is the entire memory argument for LOD chains, and it falls out for free.
//   2. No attribute interpolation. Normals, UVs and (later) packed vertex data
//      survive byte-identically instead of being averaged across a UV seam.
//   3. The geometric error is then a genuine VERTEX DISPLACEMENT in metres,
//      which is exactly the quantity the screen-space-error LOD selector needs
//      (app/mesh_lod.h). Optimal placement would give a quadric value whose
//      units are squared-distance-to-planes, a much shakier thing to project.
// The cost of subset placement is a modestly higher error for the same triangle
// budget than optimal placement; for discrete LOD at a 1-2 px error budget that
// is not the binding constraint.
//
// WHY IT WELDS FIRST: an authored mesh splits vertices at every UV seam and hard
// normal crease, so the raw index buffer is not edge-connected across those
// seams and a naive collapse would refuse to remove anything. Decimation runs on
// POSITION-WELDED topology; the surviving representative index is mapped back to
// a real input vertex at the end.
//
// RHI-free and deterministic, so --test-geolod exercises it with no GPU.
// ============================================================================

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// Result of one decimation step.
struct DecimateResult {
    std::vector<uint32_t> indices;   // indexes the ORIGINAL vertex array (subset placement)
    float    maxError    = 0.0f;     // model-space max vertex displacement, metres
    uint32_t collapses   = 0;        // edge collapses performed
    uint32_t triangles   = 0;        // triangles in `indices` (indices.size()/3)
};

// Decimate `idx` (a triangle list over `verts`) toward `targetRatio` of its
// current triangle count (0 < targetRatio < 1). `priorError` is the accumulated
// displacement the INPUT already carries relative to the original LOD0 mesh
// (pass 0 for the first step); the returned maxError includes it, so chaining
// steps yields a monotonically non-decreasing error per level.
//
// Never returns fewer than `minTriangles` triangles. On any degenerate input
// (no verts / no indices / ratio out of range) it returns the input unchanged
// with maxError == priorError.
DecimateResult decimateMesh(const x3::rhi::MeshVertex* verts, uint32_t vcount,
                            const uint32_t* idx, uint32_t icount,
                            float targetRatio, float priorError = 0.0f,
                            uint32_t minTriangles = 4);

// Model-space bounding sphere of `verts` (AABB centre, max radius) — the same
// formulation the renderer bakes into Mesh::boundsCenter/boundsRadius, mirrored
// here so LOD chain building needs no device round-trip.
void meshBoundingSphere(const x3::rhi::MeshVertex* verts, uint32_t vcount,
                        float outCenter[3], float& outRadius);

} // namespace x3::game
