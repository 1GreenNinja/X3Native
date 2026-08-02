// Cascaded Shadow Maps — cascade fitting math. See Csm.h for the clean-room
// provenance statement (Zhang et al. 2006 practical split + the standard
// stable-CSM literature; no GPL / id Tech / RBDOOM / Unreal source consulted).
#include "Csm.h"

#include <algorithm>
#include <cmath>

namespace x3::csm {

float splitDistance(float zNear, float zFar, int i, int count, float lambda) {
    if (count <= 0) return zFar;
    if (i <= 0)     return zNear;
    if (i >= count) return zFar;
    const float si = (float)i / (float)count;
    // Logarithmic: n * (f/n)^(i/N)  — distributes slices so each covers a
    // constant RATIO of depth, which matches how perspective aliasing grows.
    const float logSplit = zNear * std::pow(zFar / std::max(zNear, 1e-4f), si);
    // Uniform: n + (f-n) * i/N — equal world-space thickness.
    const float uniSplit = zNear + (zFar - zNear) * si;
    // Practical split: the lambda-weighted blend (Zhang et al. 2006, eq. 5).
    return lambda * logSplit + (1.0f - lambda) * uniSplit;
}

glm::mat4 legacyOrthoViewProj(const glm::vec3& center, const glm::vec3& sunDirNorm,
                              float ortho, float depthHalf) {
    // Verbatim from the pre-CSM computeLightViewProj(): same operations, same
    // order, so the produced floats are bit-identical. DO NOT "tidy" this.
    const glm::vec3 eye = center + sunDirNorm * depthHalf;
    const glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 upPick = (std::fabs(glm::dot(sunDirNorm, up)) > 0.99f) ? glm::vec3(0, 0, 1) : up;
    glm::mat4 view = glm::lookAt(eye, center, upPick);
    // Ortho with Vulkan's [0,1] Z (GLM_FORCE_DEPTH_ZERO_TO_ONE), reverse-Y clip.
    glm::mat4 proj = glm::ortho(-ortho, ortho, -ortho, ortho, 0.0f, 2.0f * depthHalf);
    proj[1][1] *= -1.0f;
    return proj * view;
}

glm::mat4 lightRotation(const glm::vec3& sunDirNorm) {
    // Same degenerate-up guard the legacy computeLightViewProj() uses, so a
    // straight-overhead sun does not produce a singular basis.
    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    const glm::vec3 upPick = (std::fabs(glm::dot(sunDirNorm, worldUp)) > 0.99f)
                           ? glm::vec3(0.0f, 0.0f, 1.0f) : worldUp;
    // View FROM the world origin looking along -sunDir (i.e. the direction the
    // light travels). Pure rotation: no translation term, so light space is a
    // rigid, camera-independent re-frame of world space.
    return glm::lookAt(glm::vec3(0.0f), -sunDirNorm, upPick);
}

namespace {

// Bounding SPHERE of the frustum slice [n, f], expressed as (distance along the
// view axis, radius). Closed form, and — the whole point — a function of only
// n, f and the two field-of-view tangents. It contains NO camera orientation
// term, so rotating the camera cannot change the radius. That is what stops
// the ortho box from breathing when the player merely looks around.
//
// Slice corners sit at (+-d*tx, +-d*ty, -d) for d in {n, f}. Put the sphere
// centre on the view axis at -z. Equating the squared distance to a near corner
// and a far corner:
//     n^2*a^2 + (n-z)^2 = f^2*a^2 + (f-z)^2,  with a^2 = tx^2 + ty^2
// =>  z = (f + n) * (a^2 + 1) / 2
void sliceSphere(float n, float f, float tanX, float tanY,
                 float& outDist, float& outRadius) {
    const float a2 = tanX * tanX + tanY * tanY;
    float z = (f + n) * (a2 + 1.0f) * 0.5f;
    if (z >= f) {
        // The centre would fall beyond the far plane: the minimal enclosing
        // sphere is then the one circumscribing the FAR quad alone. (Happens
        // for wide slices / wide FOV; without this the radius over-inflates.)
        z = f;
        outDist = z;
        outRadius = f * std::sqrt(a2);
        return;
    }
    outDist = z;
    outRadius = std::sqrt(f * f * a2 + (f - z) * (f - z));
}

// The 8 corners of the view frustum slice [n, f] in WORLD space.
void sliceCorners(const Params& p, float n, float f, glm::vec3 out[8]) {
    const glm::vec3 fwd = glm::normalize(p.camFwd);
    glm::vec3 up = glm::normalize(p.camUp);
    up = up - fwd * glm::dot(up, fwd);              // re-orthonormalize (setCameraBasis does the same)
    const float ul = glm::length(up);
    up = (ul > 1e-6f) ? up / ul : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, up));

    const float tanY = std::tan(glm::radians(p.fovYDeg) * 0.5f);
    const float tanX = tanY * p.aspect;
    int k = 0;
    for (int di = 0; di < 2; ++di) {
        const float d = (di == 0) ? n : f;
        const glm::vec3 c = p.camPos + fwd * d;
        for (int yi = -1; yi <= 1; yi += 2)
            for (int xi = -1; xi <= 1; xi += 2)
                out[k++] = c + right * (d * tanX * (float)xi) + up * (d * tanY * (float)yi);
    }
}

Result computeImpl(const Params& p, bool stable) {
    Result r{};
    r.count = std::max(1, std::min(p.count, kMaxCascades));

    const glm::vec3 sunDir = glm::normalize(p.sunDir);
    const glm::mat4 lightRot = lightRotation(sunDir);

    const float tanY = std::tan(glm::radians(p.fovYDeg) * 0.5f);
    const float tanX = tanY * p.aspect;
    const float zNear = std::max(p.zNear, 1e-3f);
    const float zFar  = std::max(p.zFar, zNear + 1e-3f);
    const float dim   = (float)std::max(1u, p.shadowDim);

    const glm::vec3 fwd = glm::normalize(p.camFwd);

    for (int i = 0; i < r.count; ++i) {
        const float sn = splitDistance(zNear, zFar, i,     r.count, p.lambda);
        const float sf = splitDistance(zNear, zFar, i + 1, r.count, p.lambda);

        Cascade& cd = r.c[i];
        cd.splitNear = sn;
        cd.splitFar  = sf;

        float centreDist = 0.0f, radius = 0.0f;
        glm::vec3 centreWS{ 0.0f };

        if (stable) {
            // ---- ROTATION-INVARIANT EXTENT ---------------------------------
            // Radius from the analytic slice sphere: a function of (n, f, fov,
            // aspect) only. The sphere CENTRE moves with the camera, but its
            // SIZE never does.
            sliceSphere(sn, sf, tanX, tanY, centreDist, radius);
            centreWS = p.camPos + fwd * centreDist;
        } else {
            // ---- NEGATIVE CONTROL: light-space AABB of the corners ---------
            // The extent now depends on how the slice is ORIENTED relative to
            // the sun, so it changes as the camera rotates -> shimmer.
            glm::vec3 corners[8];
            sliceCorners(p, sn, sf, corners);
            glm::vec3 mn( 1e30f), mx(-1e30f);
            for (int k = 0; k < 8; ++k) {
                const glm::vec3 ls = glm::vec3(lightRot * glm::vec4(corners[k], 1.0f));
                mn = glm::min(mn, ls); mx = glm::max(mx, ls);
            }
            radius = 0.5f * std::max(mx.x - mn.x, mx.y - mn.y);
            const glm::vec3 ctrLS = (mn + mx) * 0.5f;
            centreWS = glm::vec3(glm::inverse(lightRot) * glm::vec4(ctrLS, 1.0f));
            centreDist = sf;
        }

        cd.radius = radius;

        // World meters per shadow texel at this cascade's extent. This is the
        // quantum the light-space origin is rounded to.
        const float texel = (2.0f * radius) / dim;
        cd.texelWorld = texel;

        // Centre in light space (a pure rotation of the world position).
        glm::vec3 cLS = glm::vec3(lightRot * glm::vec4(centreWS, 1.0f));

        float halfExtent = radius;
        if (stable) {
            // ---- STABLE TEXEL SNAPPING -------------------------------------
            // Round the light-space origin DOWN to the texel lattice. Because
            // `lightRot` is anchored at the world origin, that lattice is fixed
            // in the world: sub-texel camera motion leaves the box exactly where
            // it was, so every shadow texel keeps covering the same world patch
            // and edges stop crawling.
            cLS.x = std::floor(cLS.x / texel) * texel;
            cLS.y = std::floor(cLS.y / texel) * texel;
            // Snapping displaces the box centre by up to one texel, so pad the
            // half-extent by one texel to keep the slice sphere fully inside.
            halfExtent = radius + texel;
        }
        cd.halfExtent = halfExtent;
        cd.snappedX = cLS.x;
        cd.snappedY = cLS.y;

        // Depth range along the sun direction. In light space the camera looks
        // down -Z, so a point at light-space z sits at view distance -z.
        // [cLS.z - radius, cLS.z + radius] brackets the slice; extend the near
        // side by kCasterMargin so off-screen casters still reach the map.
        const float zn = -(cLS.z + radius) - kCasterMargin;
        const float zf = -(cLS.z - radius);

        glm::mat4 proj = glm::ortho(cLS.x - halfExtent, cLS.x + halfExtent,
                                    cLS.y - halfExtent, cLS.y + halfExtent,
                                    zn, zf);
        // Vulkan reverse-Y clip. CAREFUL: the legacy `proj[1][1] *= -1` alone is
        // only correct for a SYMMETRIC box, where the Y translation term is zero.
        // A cascade box is deliberately OFF-CENTRE (that is what texel snapping
        // produces), so the translation term must be negated too — otherwise the
        // mirror happens about y=0 in light space instead of about the box centre
        // and the cascade samples the wrong half of the world.
        proj[1][1] *= -1.0f;
        proj[3][1] *= -1.0f;
        cd.viewProj = proj * lightRot;

        // ---- PER-CASCADE BIAS ------------------------------------------------
        // A single constant bias cannot serve a 4x range of texel sizes: tuned
        // for cascade 0 it peter-pans cascade 3; tuned for cascade 3 it acnes
        // cascade 0. Both terms are therefore expressed in TEXELS and converted:
        //   depthBias  — in light-clip depth units, so it scales with the depth
        //                range (zf - zn) this cascade actually spans.
        //   normalBias — a world-space push along the surface normal of ~1.5
        //                texels, which is the right unit: the error a depth
        //                comparison must absorb is a texel's worth of surface
        //                slope, and a texel is `texel` meters wide here.
        const float depthRange = std::max(zf - zn, 1e-3f);
        cd.depthBias  = (2.0f * texel) / depthRange;
        cd.normalBias = 1.5f * texel;

        r.splitFar[i] = sf;
    }
    // Lanes past the last real cascade: a sentinel so the shader's selection
    // loop always settles on cascade (count-1) rather than running off the end.
    for (int i = r.count; i < kMaxCascades; ++i) r.splitFar[i] = 1e30f;
    return r;
}

} // namespace

Result compute(const Params& p)      { return computeImpl(p, /*stable=*/true);  }
Result computeNaive(const Params& p) { return computeImpl(p, /*stable=*/false); }

} // namespace x3::csm
