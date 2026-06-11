#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Camera depth pre-pass fragment shader — ALPHA-CUTOUT test only (paired with
// depth_cutout.vert; see its header for why this exists).
//
// CLEAN-ROOM, original. Replicates mesh.frag's alpha-cutout discard EXACTLY
// (same bit-31 MASK gate, same baseColorFactor.a multiply, same 0.5 threshold,
// same screen-space-derivative LOD because the same triangles rasterize at the
// same resolution) so the pre-pass depth contains exactly the texels the color
// pass will keep. Non-cutout instances in a mixed group fall through with no
// texture fetch. No color output — depth-only attachment.

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in uint  vTexIndex;
layout(location = 2) flat in float vAlphaFactor;

void main() {
    if ((vTexIndex & 0x80000000u) != 0u) {        // bit31 = glTF alphaMode MASK
        const uint baseIdx = vTexIndex & 0x3FFFFFFFu;
        float a = texture(textures[nonuniformEXT(baseIdx)], vUV).a * vAlphaFactor;
        if (a < 0.5) discard;
    }
}
