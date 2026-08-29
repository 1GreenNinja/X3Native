#version 450

// MOTION BLUR — STAGE 2 of 3: NEIGHBOUR MAX.  (CLEAN-ROOM, original.)
//
// For each tile, take the largest velocity found in its (2*kReach+1)^2 tile
// neighbourhood.  This is the GATHER form of the dilation: instead of
// rasterizing every fast tile as a quad stretched along its motion so it
// physically reaches into the tiles it will bleed into (the scatter form), each
// tile searches outward.  Gather is the GPU-friendlier of the two and is exact
// for the reach we allow.
//
// THE INVARIANT that makes it exact, and the one thing to not break:
//     maxBlurPixels <= kReach * kTile
// A pixel is only ever told about a fast tile within kReach tiles, so a blur
// longer than kReach*kTile pixels could be produced by a tile this pixel never
// looked at, and the silhouette artefact comes back.  x3::rhi::motionBlurMaxRadius()
// clamps the r_mb_maxblur cvar to exactly this bound CPU-side; the two constants
// below must match engine/rhi/MotionBlur.h.
//
// No game-engine source was consulted or copied; see PROVENANCE.md.

const int kTile  = 20;   // must equal x3::rhi::kMotionBlurTile
const int kReach = 2;    // must equal x3::rhi::kMotionBlurReach

layout(set = 0, binding = 0) uniform sampler2D tileTex;   // RG16F tile-max, pixels

layout(location = 0) out vec2 outNeighborMax;

void main() {
    const ivec2 gridSize = textureSize(tileTex, 0);
    const ivec2 t        = ivec2(gl_FragCoord.xy);

    vec2  best    = vec2(0.0);
    float bestLen = -1.0;

    for (int dy = -kReach; dy <= kReach; ++dy) {
        for (int dx = -kReach; dx <= kReach; ++dx) {
            ivec2 s = clamp(t + ivec2(dx, dy), ivec2(0), gridSize - 1);
            vec2  v = texelFetch(tileTex, s, 0).rg;
            float l = length(v);
            if (l > bestLen) { bestLen = l; best = v; }
        }
    }

    outNeighborMax = best;
}
