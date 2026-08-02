#ifndef X3_MESH_BRDF_GLSL
#define X3_MESH_BRDF_GLSL
// ============================================================================
// LIGHT-UNIT CONVENTION (engine-wide; the fix for "GLB meshes are unlit").
//
// This engine has TWO direct-lighting paths in this shader, chosen per object by
// "does it carry an MR map": the DIELECTRIC path (no MR — every procedural prim:
// floors, walls, level geometry) and this PBR path (any MR map — which is EVERY
// GLB, because the model loader synthesizes a 1x1 MR from the glTF factors).
//
// The dielectric path evaluates a plain, UNNORMALIZED Lambert: albedo * N.L * C.
// It does NOT divide by PI. Every light rig in the game (point intensities,
// ranges, sun color) was authored and eyeballed against THAT convention — i.e.
// the light's `color * intensity` is authored as a PI-scaled irradiance, not as
// radiance. The PBR path used the textbook normalized Lambert (albedo/PI), so
// under the SAME light a GLB shaded 1/PI = 0.318x of the prim standing next to
// it (metallic 0), and ~0.03x once metallic climbed to 0.8 — the "GLB meshes are
// effectively unlit" bug. Measured: a white probe cube at one world position,
// prim vs GLB, is BYTE-IDENTICAL on the same path (the loader/normals are
// correct) and drops by exactly this factor when it crosses to the PBR path.
//
// So the PBR path adopts the SAME convention as the dielectric path: no 1/PI,
// and the caller passes the same diffuse weight the dielectric path uses for
// that light (sun 0.75, point lights 1.0). Energy still conserves — (1-F) hands
// the Fresnel share to the specular lobe, which the dielectric path simply
// throws away. Result: a metallic=0 GLB and a prim with the same albedo now
// shade within ~4% of each other under identical lights, and metals get a real
// (rather than PI-crushed) diffuse+spec response.
// ============================================================================
const float kSunDiffuseW   = 0.75;   // matches the dielectric path's sun weight
const float kPointDiffuseW = 1.0;    // matches the dielectric path's point-light weight

// One light's outgoing radiance factor (Lambert diffuse + GGX spec) * NoL.
// `dw` = the path-parity diffuse weight above (NOT 1/PI — see the convention note).
vec3 brdf(vec3 N, vec3 V, float NoV, vec3 L, vec3 F0, vec3 diff, float a, float dw) {
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NoH = max(dot(N, H), 0.0), VoH = max(dot(V, H), 0.0);
    vec3 F = F_Schlick(VoH, F0);
    vec3 spec = D_GGX(NoH, a) * V_SmithGGX(NoV, NoL, a) * F;
    return ((vec3(1.0) - F) * diff * dw + spec) * NoL;
}
#endif  // X3_MESH_BRDF_GLSL
