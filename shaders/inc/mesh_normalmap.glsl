#ifndef X3_MESH_NORMALMAP_GLSL
#define X3_MESH_NORMALMAP_GLSL
// Perturb the geometry normal by a tangent-space normal map via a derivative TBN
// (no vertex tangents needed). idx = bindless normal-map index.
vec3 perturbNormal(vec3 N, vec3 wp, vec2 uv, uint idx) {
    vec3 t = texture(textures[nonuniformEXT(idx)], uv).xyz * 2.0 - 1.0;
    vec3 dp1 = dFdx(wp), dp2 = dFdy(wp);
    vec2 du1 = dFdx(uv), du2 = dFdy(uv);
    vec3 T = normalize(dp1 * du2.y - dp2 * du1.y);
    vec3 B = -normalize(cross(N, T));
    return normalize(mat3(T, B, N) * t);
}
#endif  // X3_MESH_NORMALMAP_GLSL
