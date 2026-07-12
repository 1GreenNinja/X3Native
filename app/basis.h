#pragma once
// ===========================================================================
// basis.h — THE ONE WAY to build a model basis from a direction. (KNOWN_BUGS R3)
//
// THE BUG THIS FILE EXISTS TO KILL
// --------------------------------
// A model matrix's upper 3x3 must be a ROTATION. A rotation has determinant +1.
// A hand-rolled basis [right, up, outward] where
//
//     right = (-outward.z, 0, outward.x)          <- the copy-pasted idiom
//
// has determinant **-1**. That is a REFLECTION, not a rotation. It mirrors the
// mesh: triangle winding reverses, so VK_FRONT_FACE_COUNTER_CLOCKWISE + back-face
// culling throws away the model's OUTER shell and rasterizes its INNER shell. You
// are then looking at the INSIDE of the object, whose normals point AWAY from every
// light in the room.
//
// It is nearly invisible in a screenshot, which is why it survived for months:
//   * the SILHOUETTE is perfect (a mirrored mesh has the same outline),
//   * albedo and normal-map relief look right (texture lookups don't care about winding),
//   * the specular lobe is coherent,
//   * and there is ZERO DIFFUSE at any albedo under any light.
// Every symptom points at the ART. It is the RENDERER. (See THE PATTERN in
// docs/KNOWN_BUGS.md: the renderer is guilty far more often than the art.)
//
// THE SMOKING-GUN TEST: a cube carrying the object's EXACT material renders
// blown-out white in the same room while a 120-intensity probe light 3 m from the
// object leaves it black. No material bug and no lighting bug can produce that pair.
// Only a mirror can.
//
// THE LAW
// -------
//   * NEVER hand-roll a basis from a direction vector. Call basisFromOutward().
//   * If you must hand-roll one, assert det3() > 0 in a test. --test-basis scans
//     every entity of every world and goes RED on a negative determinant.
//
// Convention (docs/CONVENTIONS.md): right-handed, +X right, +Y up, -Z forward,
// 1 unit = 1 m. Model matrices are column-major (m[0..2] = local X in world, etc).
// ===========================================================================
#include <cmath>

namespace x3::game {

// ---- Determinant of the upper-left 3x3 of a column-major 4x4 -----------------
// > 0 = rotation (possibly with positive scale). < 0 = MIRROR: the mesh is
// inside-out and cannot be lit. ~0 = degenerate/collapsed basis (invisible).
inline float det3(const float m[16]) {
    return m[0] * (m[5] * m[10] - m[6] * m[9])
         - m[4] * (m[1] * m[10] - m[2] * m[9])
         + m[8] * (m[1] * m[6]  - m[2] * m[5]);
}

// Determinant of a basis given as three column vectors (X, Y, Z).
inline float det3(const float X[3], const float Y[3], const float Z[3]) {
    return X[0] * (Y[1] * Z[2] - Y[2] * Z[1])
         - Y[0] * (X[1] * Z[2] - X[2] * Z[1])
         + Z[0] * (X[1] * Y[2] - X[2] * Y[1]);
}

// A basis is MIRRORED when its determinant is negative. Exactly zero (a collapsed
// / deliberately-invisible transform) is not a mirror — it draws nothing.
inline bool isMirroredBasis(const float m[16], float eps = 1e-6f) {
    return det3(m) < -eps;
}

// ---------------------------------------------------------------------------
// basisFromOutward — a GUARANTEED right-handed orthonormal basis whose local +Z
// is `outward`. THIS IS THE ONLY SANCTIONED WAY to orient a model off a normal /
// outward / forward / tangent vector.
//
//   locZ = normalize(outward)                 (the axis you care about)
//   locX = normalize(worldUp x locZ)          (NOT (-z, 0, x) -- that is the mirror)
//   locY = locZ x locX                        (already unit, exactly perpendicular)
//
// det[locX, locY, locZ] = locX . ((locZ x locX) x locZ) = locX . locX = +1. Always.
// Near-vertical `outward` (a floor/ceiling normal) falls back to a +X reference so
// the cross product never collapses.
// ---------------------------------------------------------------------------
inline void basisFromOutward(const float outward[3],
                             float locX[3], float locY[3], float locZ[3]) {
    float zx = outward[0], zy = outward[1], zz = outward[2];
    float zl = std::sqrt(zx * zx + zy * zy + zz * zz);
    if (!(zl > 1e-6f)) { zx = 0.0f; zy = 0.0f; zz = 1.0f; zl = 1.0f; }   // degenerate -> +Z
    zx /= zl; zy /= zl; zz /= zl;

    // Reference "up" — swapped for +X when outward is (near) vertical.
    float ux = 0.0f, uy = 1.0f, uz = 0.0f;
    if (std::fabs(zy) > 0.999f) { ux = 1.0f; uy = 0.0f; uz = 0.0f; }

    // locX = up x locZ
    float xx = uy * zz - uz * zy;
    float xy = uz * zx - ux * zz;
    float xz = ux * zy - uy * zx;
    float xl = std::sqrt(xx * xx + xy * xy + xz * xz);
    if (!(xl > 1e-6f)) { xx = 1.0f; xy = 0.0f; xz = 0.0f; xl = 1.0f; }
    xx /= xl; xy /= xl; xz /= xl;

    // locY = locZ x locX (unit by construction; both operands are unit + orthogonal)
    const float yx = zy * xz - zz * xy;
    const float yy = zz * xx - zx * xz;
    const float yz = zx * xy - zy * xx;

    locX[0] = xx; locX[1] = xy; locX[2] = xz;
    locY[0] = yx; locY[1] = yy; locY[2] = yz;
    locZ[0] = zx; locZ[1] = zy; locZ[2] = zz;
}

// Column-major model matrix from an explicit basis + translation. (No scale — pass
// pre-scaled columns if you need one; a NEGATIVE scale is a mirror, so don't.)
inline void makeBasisXform(float m[16],
                           const float locX[3], const float locY[3], const float locZ[3],
                           float tx, float ty, float tz) {
    m[0] = locX[0]; m[1] = locX[1]; m[2]  = locX[2]; m[3]  = 0.0f;
    m[4] = locY[0]; m[5] = locY[1]; m[6]  = locY[2]; m[7]  = 0.0f;
    m[8] = locZ[0]; m[9] = locZ[1]; m[10] = locZ[2]; m[11] = 0.0f;
    m[12] = tx;     m[13] = ty;     m[14] = tz;      m[15] = 1.0f;
}

// --test-basis (app/basis_test.cpp): the TOTAL invariant. Builds the worlds on a
// headless device and asserts det3 > 0 on every entity, with negative controls.
bool runBasisSelfTest();

} // namespace x3::game
