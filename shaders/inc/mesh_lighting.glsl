#ifndef X3_MESH_LIGHTING_GLSL
#define X3_MESH_LIGHTING_GLSL
// ===========================================================================
// FORWARD POINT LIGHTING — attenuation + the per-fragment light ITERATOR.
// Owner: LANE 2 (clustered lighting). Shared by mesh.frag and glass.frag.
//
// Two paths, selected per frame by the r_clusterlights cvar:
//
//   LEGACY  (cam.clusterCfg.x == 0) — the original fixed 64-entry UBO array,
//     looped in full for every fragment. UNCHANGED, deliberately: the existing
//     md5 / screenshot gates are pinned to its exact output, so this branch must
//     stay bit-for-bit what it always was. It is the fallback and the reference.
//
//   CLUSTERED (cam.clusterCfg.x != 0) — a froxel grid over the view frustum
//     (GX x GY tiles x GZ exponential depth slices; dimensions come from the UBO,
//     single source of truth in engine/rhi/ClusterLights.h). The host assigns each
//     light to the froxels its sphere of influence overlaps and writes fixed-stride
//     per-froxel index lists into an SSBO. A fragment resolves its OWN froxel from
//     gl_FragCoord + its view depth and iterates ONLY that froxel's list, out of a
//     light array that now holds up to 1024 lights instead of 64.
//
// The rest of the shader never branches on this: it calls x3LightCount() and then
// x3Light(i), which return the right thing either way. That is what keeps the
// dielectric / PBR / clearcoat / debug loops from having to be written twice.
//
// CLEAN-ROOM: Olsson & Assarsson, "Clustered Deferred and Forward Shading"
// (HPG 2012) + standard published froxel-grid write-ups. The slicing law and the
// froxel indexing are re-derived in engine/rhi/ClusterLights.h. No GPL / id Tech /
// RBDOOM / Unreal source consulted.
// ===========================================================================

// Smooth windowed point-light attenuation: bright near the source, smoothly
// reaching exactly 0 at `range` (no hard cutoff seam, no unbounded 1/d^2 spike).
//   window = clamp(1 - (d/range)^4, 0, 1)^2   (UE4-style range falloff)
//   falloff = window / (d^2 + 1)              (bounded inverse-square-ish core)
float pointAtten(float dist, float range) {
    float t = dist / max(range, 0.0001);
    float w = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    w *= w;
    return w / (dist * dist + 1.0);
}

// ---------------------------------------------------------------------------
// Cluster resources. Declared here (not in the orchestrator) so glass.frag picks
// them up from the same include and the two shaders can never disagree about the
// binding numbers. Both live on the per-frame set 1, next to the object SSBO and
// the camera UBO.
//
//   binding 3 : the scene light array, up to kMaxSceneLights (1024) entries.
//   binding 4 : the froxel light lists. FIXED STRIDE — froxel c owns
//               [c*maxPerCluster, (c+1)*maxPerCluster). counts[] rides in the
//               same buffer, ahead of the lists, so there is one binding not two:
//                   [0 .. clusterCount)                     = per-froxel counts
//                   [clusterCount .. clusterCount*(1+stride)) = the lists
// When r_clusterlights is 0 these buffers still exist (layout-valid, sized 1) and
// are NEVER READ — the legacy branch below cannot touch them.
// ---------------------------------------------------------------------------
struct ClusterLight {
    vec4 posRange;   // xyz = world position, w = range (metres)
    vec4 colorPad;   // rgb = linear color * intensity, a = unused
};
layout(std430, set = 1, binding = 3) readonly buffer SceneLights {
    ClusterLight lights[];
} lightBuf;
layout(std430, set = 1, binding = 4) readonly buffer ClusterLists {
    uint data[];     // [counts | fixed-stride index lists], see above
} clusterBuf;

// ---------------------------------------------------------------------------
// Which froxel is this fragment in?
//
//   tile   : straight from gl_FragCoord (pixel -> [0,1) screen fraction -> tile).
//            gl_FragCoord.y grows DOWNWARD, and the device's proj[1][1] *= -1
//            puts view-space +Y at gl_FragCoord.y == 0, so row 0 is the TOP of
//            the screen. ClusterLights.cpp's froxel bounds use the same
//            convention (v = 0 is the top edge) — if these two ever disagree the
//            lighting shifts vertically, which is what the self-test's
//            round-trip assertion exists to catch.
//   slice  : the exponential depth law, identical to clusterSliceForViewZ():
//            floor(log(viewZ) * sliceScale + sliceBias), clamped.
//   viewZ  : LINEAR view depth = dot(P - eye, camFwd). Computed from the camera
//            basis rather than a view matrix so the shader needs one extra vec4,
//            not a whole mat4.
// ---------------------------------------------------------------------------
uint x3ClusterOfFragment() {
    uint gx = uint(cam.clusterGrid.x);
    uint gy = uint(cam.clusterGrid.y);
    uint gz = uint(cam.clusterGrid.z);

    // Screen fraction -> tile. clusterCfg.zw hold 1/screenW, 1/screenH.
    vec2 sf = gl_FragCoord.xy * cam.clusterCfg.zw;
    uint tx = min(uint(clamp(sf.x, 0.0, 0.999999) * float(gx)), gx - 1u);
    uint ty = min(uint(clamp(sf.y, 0.0, 0.999999) * float(gy)), gy - 1u);

    float viewZ = dot(vWorldPos - cam.camPos.xyz, cam.camFwd.xyz);
    uint tz = 0u;
    if (viewZ > cam.camFwd.w) {              // camFwd.w = zNear
        float s = log(viewZ) * cam.clusterSlice.x + cam.clusterSlice.y;
        tz = (s > 0.0) ? min(uint(s), gz - 1u) : 0u;
    }
    return (tz * gy + ty) * gx + tx;
}

// ---------------------------------------------------------------------------
// THE ITERATOR. Every light loop in mesh.frag / glass.frag goes through these
// three calls, so the clustered and legacy paths share one body:
//
//     int n = x3LightCount();
//     for (int i = 0; i < n; ++i) {
//         ClusterLight L = x3Light(i);
//         ... L.posRange, L.colorPad ...
//     }
//
// LEGACY: n = min(activeCount, 64) and x3Light(i) reads cam.lights[i] — the exact
// same array elements, in the exact same order, as the original loop. Same values,
// same accumulation order, therefore the same bits.
// ---------------------------------------------------------------------------
int x3LightCount() {
    if (cam.clusterCfg.x < 0.5) {
        // Legacy: the UBO array, capped at kMaxPointLights.
        return min(int(cam.ambientCount.w), kMaxPointLights);
    }
    uint c = x3ClusterOfFragment();
    return int(clusterBuf.data[c]);
}

ClusterLight x3Light(int i) {
    if (cam.clusterCfg.x < 0.5) {
        ClusterLight L;
        L.posRange = cam.lights[i].posRange;
        L.colorPad = cam.lights[i].colorPad;
        return L;
    }
    uint c      = x3ClusterOfFragment();
    uint stride = uint(cam.clusterGrid.w);          // kMaxLightsPerCluster
    uint nClust = uint(cam.clusterSlice.z);         // kClusterCount
    uint idx    = clusterBuf.data[nClust + c * stride + uint(i)];
    return lightBuf.lights[idx];
}

// ---------------------------------------------------------------------------
// r_clusterlights DEBUG VIEW (r_debugview 6): paint the fragment by how many
// lights its froxel holds — black = 0, blue -> green -> red as the list fills,
// white once it is AT the cap (i.e. this froxel is where lights are being
// dropped). The one-frame answer to "is my scene overflowing anywhere?".
// ---------------------------------------------------------------------------
vec3 x3ClusterHeatmap() {
    if (cam.clusterCfg.x < 0.5) return vec3(0.0);
    uint c = x3ClusterOfFragment();
    float n = float(clusterBuf.data[c]);
    float cap = max(cam.clusterGrid.w, 1.0);
    if (n <= 0.0) return vec3(0.0);
    if (n >= cap) return vec3(1.0);
    float t = n / cap;
    // blue -> green -> red ramp
    return t < 0.5 ? mix(vec3(0.0, 0.1, 1.0), vec3(0.0, 1.0, 0.2), t * 2.0)
                   : mix(vec3(0.0, 1.0, 0.2), vec3(1.0, 0.15, 0.0), (t - 0.5) * 2.0);
}
#endif  // X3_MESH_LIGHTING_GLSL
