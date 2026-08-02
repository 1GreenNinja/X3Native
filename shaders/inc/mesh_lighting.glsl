#ifndef X3_MESH_LIGHTING_GLSL
#define X3_MESH_LIGHTING_GLSL
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
#endif  // X3_MESH_LIGHTING_GLSL
