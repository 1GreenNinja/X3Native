#ifndef X3_MESH_MATERIAL_GLSL
#define X3_MESH_MATERIAL_GLSL
// ---- PBR helpers (Cook-Torrance GGX). Only the metallic-roughness branch in main()
// calls these; plain meshes (vMrTexIndex == 0) never do, so their shading is unchanged.
const float PI = 3.14159265359;
float D_GGX(float NoH, float a) { float a2 = a * a; float d = (NoH * NoH) * (a2 - 1.0) + 1.0; return a2 / max(PI * d * d, 1e-7); }
float V_SmithGGX(float NoV, float NoL, float a) {
    float k = a * 0.5;
    float gv = NoL * (NoV * (1.0 - k) + k);
    float gl = NoV * (NoL * (1.0 - k) + k);
    return 0.5 / max(gv + gl, 1e-5);
}
vec3 F_Schlick(float u, vec3 f0) { return f0 + (1.0 - f0) * pow(clamp(1.0 - u, 0.0, 1.0), 5.0); }
// Roughness-aware Fresnel (Sebastien Lagarde): rough surfaces keep less grazing
// reflectance than the mirror Schlick term, so IBL specular doesn't over-rim.
vec3 F_SchlickRoughness(float u, vec3 f0, float rough) {
    return f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(clamp(1.0 - u, 0.0, 1.0), 5.0);
}
#endif  // X3_MESH_MATERIAL_GLSL
