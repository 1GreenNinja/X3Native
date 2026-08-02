#pragma once
// ============================================================================
// VERTEX COMPRESSION — the packed mesh vertex formats and their codecs.
//
// CLEAN-ROOM, original work. Written from the Vulkan 1.3 / IEEE-754 specs only
// (the A2B10G10R10_SNORM_PACK32 bit layout and SNORM conversion rule, and the
// binary16 encoding). No GPL / id Tech / RBDOOM / Unreal source consulted.
// See docs/CLEANROOM_PROCESS.md.
//
// ---------------------------------------------------------------------------
// THE PROBLEM
// ---------------------------------------------------------------------------
// rhi::MeshVertex is 32 B (float3 pos, float3 normal, float2 uv) and every
// vertex is re-fetched by the depth pre-pass, EVERY CSM cascade (four of them
// since Lane 3), the opaque colour pass, the velocity pass and each IBL probe
// face. Narrowing the vertex narrows all of them at once.
//
// ---------------------------------------------------------------------------
// THE FORMATS  (DeviceDesc::vertexFormat / --vtxfmt)
// ---------------------------------------------------------------------------
//   0  LEGACY      32 B   pos R32G32B32_SFLOAT @0 | nrm R32G32B32_SFLOAT @12
//                         | uv R32G32_SFLOAT @24            <- today, bit-exact
//   1  NORMAL10    24 B   pos R32G32B32_SFLOAT @0
//                         | nrm A2B10G10R10_SNORM_PACK32 @12
//                         | uv R32G32_SFLOAT @16            <- -25%, LOSSLESS UV
//   2  NORMAL10UV16 20 B  pos R32G32B32_SFLOAT @0
//                         | nrm A2B10G10R10_SNORM_PACK32 @12
//                         | uv R16G16_SFLOAT @16            <- -37.5%
//
// POSITION STAYS float3. Quantising it needs a per-mesh scale/bias, which means
// a per-mesh uniform the vertex shader must read — that is a change to the draw
// path and to shaders other lanes own, and it is also what breaks world-scale
// meshes (a 4 km terrain tile in 16-bit is 6 cm of wobble). Not worth it here.
//
// ---------------------------------------------------------------------------
// WHY THERE IS NO SHADER-SIDE UNPACK CODE
// ---------------------------------------------------------------------------
// These are real Vulkan VERTEX BUFFER formats, so the fixed-function vertex
// fetch does the conversion in hardware and the shader still declares plain
// `in vec3 inNormal` / `in vec2 inUV`. mesh.vert, depth.vert, shadow.vert,
// velocity.vert, the two cutout verts and mesh_probe.vert are therefore
// UNCHANGED — which also means this lane does not touch shaders/inc/* that
// Lanes 1/2/3 own. Hand-rolled unpack in GLSL would be strictly worse: extra
// ALU per vertex to redo work the fetch unit does for free.
//
// The cost of that choice is that the format is a DEVICE-WIDE decision (vertex
// input state is baked into every PSO), not per-mesh. Hence a startup switch
// rather than a runtime cvar. See VulkanRenderDevice::createMeshLodChain and
// createMesh for the upload-side packing.
//
// ---------------------------------------------------------------------------
// PRECISION (measured by --test-geolod)
// ---------------------------------------------------------------------------
// * normal: 10-bit SNORM per axis -> worst-case angular error ~0.1 deg. Well
//   under the shading difference a normal map introduces.
// * uv (format 2 only): binary16. Exact enough for the 0..1 range (relative
//   error <= 2^-11), but a TILED uv of 64.0 has a step of 0.0625 — a visible
//   texture crawl on large tiled surfaces. That is precisely why format 1
//   exists and why format 2 is not the default.
// ============================================================================

#include "IRenderDevice.h"

#include <cstdint>
#include <cstring>

namespace x3::rhi {

enum : uint32_t {
    kVtxFmtLegacy       = 0,   // 32 B — today's layout, bit-exact
    kVtxFmtNormal10     = 1,   // 24 B — packed normal, full-precision UV
    kVtxFmtNormal10Uv16 = 2,   // 20 B — packed normal + half UV
    kVtxFmtCount        = 3,
};

inline uint32_t vertexStrideFor(uint32_t fmt) {
    switch (fmt) {
        case kVtxFmtNormal10:     return 24;
        case kVtxFmtNormal10Uv16: return 20;
        default:                  return 32;
    }
}

// ---- normal <-> A2B10G10R10_SNORM_PACK32 ----------------------------------
// Vulkan packs this as a single uint32: bits 0..9 = R, 10..19 = G, 20..29 = B,
// 30..31 = A. Each 10-bit field is a two's-complement SNORM: the value v in
// [-1,1] maps to round(v * 511), and the decode is max(raw / 511, -1).
inline uint32_t packNormal1010102(const float n[3]) {
    auto q = [](float v) -> uint32_t {
        if (v < -1.0f) v = -1.0f;
        if (v >  1.0f) v =  1.0f;
        int32_t i = (int32_t)(v * 511.0f + (v >= 0.0f ? 0.5f : -0.5f));
        if (i >  511) i =  511;
        if (i < -511) i = -511;
        return (uint32_t)(i & 0x3FF);
    };
    return q(n[0]) | (q(n[1]) << 10) | (q(n[2]) << 20);   // A left 0
}

inline void unpackNormal1010102(uint32_t p, float out[3]) {
    auto d = [](uint32_t raw) -> float {
        int32_t i = (int32_t)(raw & 0x3FF);
        if (i & 0x200) i -= 1024;                 // sign-extend 10 bits
        const float v = (float)i / 511.0f;
        return (v < -1.0f) ? -1.0f : v;
    };
    out[0] = d(p);
    out[1] = d(p >> 10);
    out[2] = d(p >> 20);
}

// ---- float <-> binary16 ----------------------------------------------------
// Round-to-nearest-even is not required here (the GPU only ever DECODES these),
// so this uses round-half-away-from-zero on the mantissa, which keeps the
// implementation short and the maximum error at half an ulp either way.
inline uint16_t packHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;

    if (((x >> 23) & 0xFF) == 0xFF)                       // Inf / NaN
        return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0u));
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);   // overflow -> Inf
    if (exp <= 0) {                                       // subnormal / zero
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exp);
        const uint32_t half  = (mant + (1u << (shift - 1))) >> shift;
        return (uint16_t)(sign | half);
    }
    const uint32_t half = (mant + 0x1000u) >> 13;         // round mantissa
    if (half & 0x400u) return (uint16_t)(sign | (uint32_t)((exp + 1) << 10));
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (half & 0x3FFu));
}

inline float unpackHalf(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
    const uint32_t mant = (uint32_t)(h & 0x3FFu);
    uint32_t x;
    if (exp == 0) {
        if (mant == 0) { x = sign; }
        else {                                            // subnormal
            int32_t e = -1;
            uint32_t m = mant;
            do { ++e; m <<= 1; } while ((m & 0x400u) == 0);
            x = sign | (uint32_t)((127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 0x1F) {
        x = sign | 0x7F800000u | (mant << 13);
    } else {
        x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

// ---- MeshVertex <-> packed -------------------------------------------------
// `dst` must have vertexStrideFor(fmt) bytes available.
inline void packVertex(const MeshVertex& v, uint32_t fmt, void* dst) {
    uint8_t* p = (uint8_t*)dst;
    std::memcpy(p, v.pos, 12);
    if (fmt == kVtxFmtLegacy) {
        std::memcpy(p + 12, v.normal, 12);
        std::memcpy(p + 24, v.uv, 8);
        return;
    }
    const uint32_t n = packNormal1010102(v.normal);
    std::memcpy(p + 12, &n, 4);
    if (fmt == kVtxFmtNormal10Uv16) {
        const uint16_t uv[2] = { packHalf(v.uv[0]), packHalf(v.uv[1]) };
        std::memcpy(p + 16, uv, 4);
    } else {
        std::memcpy(p + 16, v.uv, 8);
    }
}

inline void unpackVertex(const void* src, uint32_t fmt, MeshVertex& out) {
    const uint8_t* p = (const uint8_t*)src;
    std::memcpy(out.pos, p, 12);
    if (fmt == kVtxFmtLegacy) {
        std::memcpy(out.normal, p + 12, 12);
        std::memcpy(out.uv, p + 24, 8);
        return;
    }
    uint32_t n;
    std::memcpy(&n, p + 12, 4);
    unpackNormal1010102(n, out.normal);
    if (fmt == kVtxFmtNormal10Uv16) {
        uint16_t uv[2];
        std::memcpy(uv, p + 16, 4);
        out.uv[0] = unpackHalf(uv[0]);
        out.uv[1] = unpackHalf(uv[1]);
    } else {
        std::memcpy(out.uv, p + 16, 8);
    }
}

// Pack a whole array. Returns the byte size written.
inline size_t packVertices(const MeshVertex* src, uint32_t count, uint32_t fmt, void* dst) {
    const uint32_t stride = vertexStrideFor(fmt);
    uint8_t* p = (uint8_t*)dst;
    for (uint32_t i = 0; i < count; ++i) packVertex(src[i], fmt, p + (size_t)i * stride);
    return (size_t)count * stride;
}

} // namespace x3::rhi
