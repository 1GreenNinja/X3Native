#version 450
// GPU-compute debris draw — fragment (Subsystem K, tier T2).
//
// CLEAN-ROOM, original work. Built from the K spec + the Vulkan spec + public
// real-time references. No id Tech / RBDOOM source consulted.
//
// A cheap single-directional-light shade so the tumbling debris reads with form
// (the pool can be tens of thousands of cubes; this stays trivially cheap). Output
// goes into the linear HDR scene target, like the other transparent-pass draws.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Same sun direction the rest of the renderer lights from (toward the sun).
    vec3 L = normalize(vec3(0.4, 1.0, 0.3));
    float ndl = max(dot(normalize(vNormal), L), 0.0);
    float ambient = 0.25;
    vec3 lit = vColor.rgb * (ambient + (1.0 - ambient) * ndl);
    outColor = vec4(lit, vColor.a);
}
