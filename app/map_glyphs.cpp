// map_glyphs.cpp — SDF-rasterized icon sheet for the world map. Design in header.
#include "map_glyphs.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace x3::game {

namespace {

struct V { float x, y; };
inline V  sub(V a, V b) { return { a.x - b.x, a.y - b.y }; }
inline float dot(V a, V b) { return a.x * b.x + a.y * b.y; }
inline float len(V a) { return std::sqrt(dot(a, a)); }

inline float sdCircle(V p, float r) { return len(p) - r; }
inline float sdBox(V p, V b) {
    const V q{ std::fabs(p.x) - b.x, std::fabs(p.y) - b.y };
    const V m{ std::max(q.x, 0.0f), std::max(q.y, 0.0f) };
    return len(m) + std::min(std::max(q.x, q.y), 0.0f);
}
inline float sdRoundBox(V p, V b, float r) { return sdBox(p, { b.x - r, b.y - r }) - r; }
inline float sdSegment(V p, V a, V b, float r) {
    const V pa = sub(p, a), ba = sub(b, a);
    const float h = std::clamp(dot(pa, ba) / std::max(1e-6f, dot(ba, ba)), 0.0f, 1.0f);
    return len(V{ pa.x - ba.x * h, pa.y - ba.y * h }) - r;
}
// Signed distance to an arbitrary (possibly concave) polygon — iq's formulation.
float sdPolygon(V p, const V* v, int n) {
    float d = dot(sub(p, v[0]), sub(p, v[0]));
    float s = 1.0f;
    for (int i = 0, j = n - 1; i < n; j = i, ++i) {
        const V e = sub(v[j], v[i]);
        const V w = sub(p, v[i]);
        const float t = std::clamp(dot(w, e) / std::max(1e-6f, dot(e, e)), 0.0f, 1.0f);
        const V b{ w.x - e.x * t, w.y - e.y * t };
        d = std::min(d, dot(b, b));
        const bool c0 = p.y >= v[i].y, c1 = p.y < v[j].y, c2 = e.x * w.y > e.y * w.x;
        if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) s = -s;
    }
    return s * std::sqrt(d);
}
inline float sdTri(V p, V a, V b, V c) { const V v[3] = { a, b, c }; return sdPolygon(p, v, 3); }
inline float opU(float a, float b) { return std::min(a, b); }
inline float opS(float a, float b) { return std::max(a, -b); }        // a minus b
inline float opI(float a, float b) { return std::max(a, b); }
inline float opRing(float d, float t) { return std::fabs(d) - t; }
inline V off(V p, float dx, float dy) { return { p.x - dx, p.y - dy }; }

// Signed distance for glyph g at p (y UP, [-1,1]). Negative = inside.
float glyphSdf(MapGlyph g, V p) {
    switch (g) {
    case MapGlyph::Plate:       return sdRoundBox(p, { 0.80f, 0.80f }, 0.24f);
    case MapGlyph::PlateRing:   return opRing(sdRoundBox(p, { 0.80f, 0.80f }, 0.24f), 0.09f);
    case MapGlyph::Disc:        return sdCircle(p, 0.72f);
    case MapGlyph::Ring:        return opRing(sdCircle(p, 0.66f), 0.13f);
    case MapGlyph::Diamond:     return (std::fabs(p.x) + std::fabs(p.y) - 0.82f) * 0.7071f;
    case MapGlyph::DiamondRing: return opRing((std::fabs(p.x) + std::fabs(p.y) - 0.80f) * 0.7071f, 0.11f);
    case MapGlyph::Pin: {
        const float head = sdCircle(off(p, 0, 0.22f), 0.50f);
        const float tip  = sdTri(p, { -0.42f, 0.05f }, { 0.42f, 0.05f }, { 0.0f, -0.88f });
        return opS(opU(head, tip), sdCircle(off(p, 0, 0.22f), 0.20f));
    }
    case MapGlyph::Star: {
        V v[10];
        for (int i = 0; i < 10; ++i) {
            const float a = 1.5707963f + (float)i * 0.6283185f;   // start at the top
            const float r = (i & 1) ? 0.40f : 0.90f;
            v[i] = { std::cos(a) * r, std::sin(a) * r };
        }
        return sdPolygon(p, v, 10);
    }
    case MapGlyph::Tower: {
        float d = sdBox(off(p, 0.12f, -0.12f), { 0.30f, 0.64f });
        d = opU(d, sdBox(off(p, 0.12f, 0.66f), { 0.05f, 0.24f }));       // antenna
        d = opU(d, sdBox(off(p, -0.50f, -0.42f), { 0.22f, 0.34f }));     // low block
        return d;
    }
    case MapGlyph::Base: {
        const float slab = sdBox(off(p, 0, -0.50f), { 0.85f, 0.14f });
        const float dome = opI(sdCircle(off(p, 0, -0.36f), 0.56f), -(p.y + 0.36f));
        return opU(slab, dome);
    }
    case MapGlyph::Car: {
        float d = sdRoundBox(off(p, 0, -0.15f), { 0.86f, 0.26f }, 0.14f);
        d = opU(d, sdRoundBox(off(p, 0.02f, 0.16f), { 0.46f, 0.26f }, 0.16f));
        d = opU(d, sdCircle(off(p, -0.48f, -0.46f), 0.20f));
        d = opU(d, sdCircle(off(p, 0.48f, -0.46f), 0.20f));
        return d;
    }
    case MapGlyph::Wrench: {
        float d = sdSegment(p, { -0.62f, -0.62f }, { 0.22f, 0.22f }, 0.15f);
        float head = opRing(sdCircle(off(p, 0.42f, 0.42f), 0.36f), 0.15f);
        // Jaw notch: a box along the diagonal, opening toward the top-right.
        const V q = off(p, 0.42f, 0.42f);
        const V r{ (q.x + q.y) * 0.7071f, (q.y - q.x) * 0.7071f };
        head = opS(head, sdBox(off(r, 0.45f, 0.0f), { 0.35f, 0.13f }));
        return opU(d, head);
    }
    case MapGlyph::Mountain: {
        const float a = sdTri(p, { -0.98f, -0.62f }, { 0.02f, -0.62f }, { -0.48f, 0.42f });
        const float b = sdTri(p, { -0.40f, -0.62f }, { 0.98f, -0.62f }, { 0.28f, 0.78f });
        return opU(a, b);
    }
    case MapGlyph::Interchange: {
        float d = sdBox(p, { 0.92f, 0.13f });
        d = opU(d, sdBox(p, { 0.13f, 0.92f }));
        d = opU(d, opRing(sdCircle(p, 0.50f), 0.13f));
        return d;
    }
    case MapGlyph::Portal: {
        const float outer = opU(opI(sdCircle(off(p, 0, 0.08f), 0.74f), -(p.y - 0.08f)),
                                sdBox(off(p, 0, -0.38f), { 0.74f, 0.46f }));
        const float inner = opU(opI(sdCircle(off(p, 0, 0.08f), 0.44f), -(p.y - 0.08f)),
                                sdBox(off(p, 0, -0.48f), { 0.44f, 0.56f }));
        return opS(outer, inner);
    }
    case MapGlyph::Flag: {
        const float pole = sdBox(off(p, -0.58f, 0.0f), { 0.08f, 0.88f });
        const float pen  = sdTri(p, { -0.50f, 0.86f }, { 0.74f, 0.46f }, { -0.50f, 0.06f });
        return opU(pole, pen);
    }
    case MapGlyph::Arrow: {
        const V v[4] = { { 0.0f, 0.92f }, { 0.74f, -0.82f }, { 0.0f, -0.40f }, { -0.74f, -0.82f } };
        return sdPolygon(p, v, 4);
    }
    case MapGlyph::Cross:       return opU(sdBox(p, { 0.86f, 0.10f }), sdBox(p, { 0.10f, 0.86f }));
    case MapGlyph::Door: {
        float d = opS(sdBox(p, { 0.56f, 0.82f }), sdBox(off(p, 0, -0.06f), { 0.40f, 0.66f }));
        return opU(d, sdCircle(off(p, 0.22f, -0.12f), 0.09f));
    }
    case MapGlyph::Elevator: {
        float d = opRing(sdBox(p, { 0.66f, 0.82f }), 0.08f);
        d = opU(d, sdTri(p, { -0.30f, 0.14f }, { 0.30f, 0.14f }, { 0.0f, 0.56f }));
        d = opU(d, sdTri(p, { -0.30f, -0.14f }, { 0.30f, -0.14f }, { 0.0f, -0.56f }));
        return d;
    }
    case MapGlyph::Skull: {
        float d = opU(sdCircle(off(p, 0, 0.16f), 0.62f), sdBox(off(p, 0, -0.46f), { 0.36f, 0.26f }));
        d = opS(d, sdCircle(off(p, -0.25f, 0.22f), 0.16f));
        d = opS(d, sdCircle(off(p, 0.25f, 0.22f), 0.16f));
        d = opS(d, sdBox(off(p, -0.13f, -0.58f), { 0.04f, 0.16f }));
        d = opS(d, sdBox(off(p, 0.13f, -0.58f), { 0.04f, 0.16f }));
        return d;
    }
    case MapGlyph::Lock: {
        float d = sdRoundBox(off(p, 0, -0.32f), { 0.62f, 0.46f }, 0.10f);
        const float shackle = opI(opRing(sdCircle(off(p, 0, 0.26f), 0.38f), 0.10f), -(p.y - 0.20f));
        d = opU(d, shackle);
        return opS(d, sdCircle(off(p, 0, -0.28f), 0.11f));
    }
    case MapGlyph::Cell: {
        float d = opRing(sdBox(p, { 0.66f, 0.66f }), 0.08f);
        d = opU(d, sdBox(off(p, -0.30f, 0), { 0.06f, 0.62f }));
        d = opU(d, sdBox(p, { 0.06f, 0.62f }));
        d = opU(d, sdBox(off(p, 0.30f, 0), { 0.06f, 0.62f }));
        return d;
    }
    case MapGlyph::Secret:      return opS(sdCircle(p, 0.72f), sdCircle(p, 0.30f));
    case MapGlyph::Club: {
        float d = sdCircle(off(p, -0.32f, -0.44f), 0.32f);
        d = opU(d, sdBox(off(p, -0.06f, 0.12f), { 0.07f, 0.60f }));
        d = opU(d, sdBox(off(p, 0.26f, 0.62f), { 0.38f, 0.10f }));
        return d;
    }
    case MapGlyph::Hall: {
        const float body = sdBox(off(p, 0, -0.18f), { 0.86f, 0.46f });
        const float roof = sdTri(p, { -0.98f, 0.28f }, { 0.98f, 0.28f }, { 0.0f, 0.86f });
        return opU(body, roof);
    }
    case MapGlyph::Rose: {
        const V ns[4] = { { 0.0f, 0.98f }, { 0.15f, 0.0f }, { 0.0f, -0.70f }, { -0.15f, 0.0f } };
        const V ew[4] = { { 0.70f, 0.0f }, { 0.0f, 0.15f }, { -0.70f, 0.0f }, { 0.0f, -0.15f } };
        float d = opU(sdPolygon(p, ns, 4), sdPolygon(p, ew, 4));
        return opU(d, opRing(sdCircle(p, 0.56f), 0.035f));
    }
    default: return 1.0f;
    }
}

void rasterCell(MapGlyph g, uint8_t* rgba, uint32_t stride, uint32_t size) {
    // 6 px apron at 64 px scales with size so the shape fills ~81% of the cell.
    const float apron = 6.0f * (float)size / 64.0f;
    const float half = (float)size * 0.5f, inner = half - apron;
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            float cov = 0.0f;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float fx = (float)x + ((float)sx + 0.5f) / 4.0f;
                    const float fy = (float)y + ((float)sy + 0.5f) / 4.0f;
                    const V p{ (fx - half) / inner, -(fy - half) / inner };   // y up
                    if (glyphSdf(g, p) < 0.0f) cov += 1.0f / 16.0f;
                }
            uint8_t* o = rgba + ((size_t)y * stride + x) * 4;
            o[0] = o[1] = o[2] = 255;
            o[3] = (uint8_t)std::lround(cov * 255.0f);
        }
}

} // namespace

float mapGlyphCoverage(MapGlyph g, float px, float py) {
    return glyphSdf(g, V{ px, py }) < 0.0f ? 1.0f : 0.0f;
}

void rasterizeMapGlyphSheet(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    w = kMapGlyphCols * kMapGlyphCell;
    h = kMapGlyphRows * kMapGlyphCell;
    rgba.assign((size_t)w * h * 4, 0);
    for (uint32_t gi = 0; gi < (uint32_t)MapGlyph::Count; ++gi) {
        const uint32_t cx = (gi % kMapGlyphCols) * kMapGlyphCell;
        const uint32_t cy = (gi / kMapGlyphCols) * kMapGlyphCell;
        rasterCell((MapGlyph)gi, rgba.data() + ((size_t)cy * w + cx) * 4, w, kMapGlyphCell);
    }
}

void rasterizeMapGlyph(MapGlyph g, uint32_t size, std::vector<uint8_t>& rgba) {
    rgba.assign((size_t)size * size * 4, 0);
    rasterCell(g, rgba.data(), size, size);
}

void mapGlyphUv(MapGlyph g, float& u0, float& v0, float& u1, float& v1) {
    const uint32_t gi = (uint32_t)g;
    const float cw = 1.0f / (float)kMapGlyphCols, ch = 1.0f / (float)kMapGlyphRows;
    u0 = (float)(gi % kMapGlyphCols) * cw; u1 = u0 + cw;
    v0 = (float)(gi / kMapGlyphCols) * ch; v1 = v0 + ch;
}

} // namespace x3::game
