// map_atlas.cpp — the GTA-style world-map rasterizer. Design notes in the header.
#include "map_atlas.h"
#include "terrain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <thread>

namespace x3::game {

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Fan `fn(y0, y1)` across the hardware threads in contiguous row bands. The
// bake is embarrassingly parallel per row for every pass except the road
// distance field, which is ALSO row-banded (each band rasterises every segment
// that overlaps it), so no pass ever writes a texel another thread reads.
uint32_t parallelRows(uint32_t rows, const std::function<void(uint32_t, uint32_t)>& fn) {
    uint32_t n = std::thread::hardware_concurrency();
    n = std::clamp<uint32_t>(n == 0 ? 4 : n, 1, 16);
    if (rows < 64) n = 1;
    if (n == 1) { fn(0, rows); return 1; }
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (uint32_t t = 0; t < n; ++t) {
        const uint32_t y0 = rows * t / n, y1 = rows * (t + 1) / n;
        if (y1 <= y0) continue;
        pool.emplace_back([&fn, y0, y1] { fn(y0, y1); });
    }
    for (std::thread& th : pool) th.join();
    return n;
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float smoothstep(float e0, float e1, float x) {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
inline void mix3(float* c, const uint8_t* to, float a) {
    c[0] += ((float)to[0] - c[0]) * a;
    c[1] += ((float)to[1] - c[1]) * a;
    c[2] += ((float)to[2] - c[2]) * a;
}
inline void mix3f(float* c, const float* to, float a) {
    c[0] += (to[0] - c[0]) * a;
    c[1] += (to[1] - c[1]) * a;
    c[2] += (to[2] - c[2]) * a;
}
inline void lerp3(float* out, const uint8_t* a, const uint8_t* b, float t) {
    out[0] = (float)a[0] + ((float)b[0] - (float)a[0]) * t;
    out[1] = (float)a[1] + ((float)b[1] - (float)a[1]) * t;
    out[2] = (float)a[2] + ((float)b[2] - (float)a[2]) * t;
}

// The float working canvas: RGB in sRGB byte range, alpha 0..1. Converted to
// RGBA8 at the end so every layer composites with full precision.
struct Canvas {
    uint32_t res = 0;
    std::vector<float> rgb;    // res*res*3
    std::vector<float> a;      // res*res
    float* px(uint32_t x, uint32_t y) { return &rgb[((size_t)y * res + x) * 3]; }
};

// ---------------------------------------------------------------------------
// 1+2. Terrain + water.
// ---------------------------------------------------------------------------
struct HeightGrid {
    uint32_t step = 1;          // texels per grid cell
    uint32_t gw = 0, gh = 0;    // nodes (with a one-cell apron each side)
    float    mpp = 1.0f;
    std::vector<float> h, depth, gx, gz;   // per node
    // Node (i,j) sits at texel coordinate ((i-1)*step, (j-1)*step).
    inline float at(const std::vector<float>& v, int i, int j) const {
        i = std::clamp(i, 0, (int)gw - 1); j = std::clamp(j, 0, (int)gh - 1);
        return v[(size_t)j * gw + i];
    }
    // Bilinear at texel-space (tx, ty).
    inline void sample(float tx, float ty, float& oh, float& od, float& ogx, float& ogz) const {
        const float u = tx / (float)step + 1.0f, v = ty / (float)step + 1.0f;
        const int i = (int)std::floor(u), j = (int)std::floor(v);
        const float fu = u - (float)i, fv = v - (float)j;
        auto bl = [&](const std::vector<float>& arr) {
            const float a00 = at(arr, i, j),     a10 = at(arr, i + 1, j);
            const float a01 = at(arr, i, j + 1), a11 = at(arr, i + 1, j + 1);
            return (a00 * (1 - fu) + a10 * fu) * (1 - fv) + (a01 * (1 - fu) + a11 * fu) * fv;
        };
        oh = bl(h); od = bl(depth); ogx = bl(gx); ogz = bl(gz);
    }
};

constexpr float kDryDepth = -6.0f;   // pseudo depth for dry nodes (see header: sand band)

void sampleHeightGrid(const MapBakeRequest& rq, HeightGrid& g, MapBakeStats* st) {
    const auto t0 = Clock::now();
    g.mpp = (rq.wx1 - rq.wx0) / (float)rq.res;
    // ~2 m spacing or 1 texel, whichever is coarser: the field has no structure
    // under 2 m, and a 2048^2 street-zoom tile would otherwise cost 4M queries.
    g.step = std::clamp<uint32_t>((uint32_t)std::lround(2.0f / g.mpp), 1u, 16u);
    const uint32_t cells = (rq.res + g.step - 1) / g.step;
    g.gw = cells + 3; g.gh = cells + 3;
    const size_t n = (size_t)g.gw * g.gh;
    g.h.assign(n, 0.0f); g.depth.assign(n, kDryDepth); g.gx.assign(n, 0.0f); g.gz.assign(n, 0.0f);
    const uint32_t threads = parallelRows(g.gh, [&](uint32_t j0, uint32_t j1) {
        for (uint32_t j = j0; j < j1; ++j) {
            const float wz = rq.wz0 + ((float)((int)j - 1) * (float)g.step) * g.mpp;
            for (uint32_t i = 0; i < g.gw; ++i) {
                const float wx = rq.wx0 + ((float)((int)i - 1) * (float)g.step) * g.mpp;
                const float hh = terrainHeightAtWorld(wx, wz);
                const float wl = worldWaterLevelAt(wx, wz);
                const size_t k = (size_t)j * g.gw + i;
                g.h[k] = hh;
                g.depth[k] = (wl > kWorldWaterDry) ? std::max(wl - hh, 0.02f) : kDryDepth;
            }
        }
    });
    // Coarse bands (>= 4 m node spacing) get one 3x3 box pass over the heights:
    // the field's metre-scale roughness sampled once per 5-10 m node is pure
    // aliasing that the hillshade turned into a grey speckle over every hill.
    // Fine bands keep the raw samples (there the nodes resolve the surface).
    if ((float)g.step * g.mpp >= 4.0f) {
        std::vector<float> src = g.h;
        for (uint32_t j = 0; j < g.gh; ++j)
            for (uint32_t i = 0; i < g.gw; ++i) {
                float acc = 0.0f;
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di) acc += g.at(src, (int)i + di, (int)j + dj);
                g.h[(size_t)j * g.gw + i] = acc / 9.0f;
            }
    }
    // Slope from central differences on the grid (node spacing step*mpp metres).
    const float inv2d = 1.0f / (2.0f * (float)g.step * g.mpp);
    for (uint32_t j = 0; j < g.gh; ++j)
        for (uint32_t i = 0; i < g.gw; ++i) {
            const size_t k = (size_t)j * g.gw + i;
            g.gx[k] = (g.at(g.h, (int)i + 1, (int)j) - g.at(g.h, (int)i - 1, (int)j)) * inv2d;
            g.gz[k] = (g.at(g.h, (int)i, (int)j + 1) - g.at(g.h, (int)i, (int)j - 1)) * inv2d;
        }
    if (st) { st->heightMs = msSince(t0); st->heightSamples = (uint32_t)n; st->threads = threads; }
}

void hypsometric(float h, float* out) {
    using namespace mappal;
    if (h <= 0.0f)         { out[0] = kLandLow[0]; out[1] = kLandLow[1]; out[2] = kLandLow[2]; return; }
    if (h < 40.0f)         { lerp3(out, kLandLow,  kLandMid,  h / 40.0f); return; }
    if (h < 150.0f)        { lerp3(out, kLandMid,  kLandHigh, (h - 40.0f) / 110.0f); return; }
    if (h < 420.0f)        { lerp3(out, kLandHigh, kLandPeak, (h - 150.0f) / 270.0f); return; }
    out[0] = kLandPeak[0]; out[1] = kLandPeak[1]; out[2] = kLandPeak[2];
}

// Coarse grid of the envelope distance over the tile (see header: TERRAIN
// FADE). Cells are the tile width / 192 but never under 8 m; one apron node
// each side so the bilinear lift never clamps inside the tile.
struct EnvGrid {
    uint32_t n = 0;          // nodes per side
    float    cell = 1.0f;    // m per cell
    float    x0 = 0, z0 = 0; // world of node (0,0)
    std::vector<float> d;
    bool empty() const { return n == 0; }
    float at(int i, int j) const {
        i = std::clamp(i, 0, (int)n - 1); j = std::clamp(j, 0, (int)n - 1);
        return d[(size_t)j * n + i];
    }
    float sample(float wx, float wz) const {
        const float u = (wx - x0) / cell, v = (wz - z0) / cell;
        const int i = (int)std::floor(u), j = (int)std::floor(v);
        const float fu = u - (float)i, fv = v - (float)j;
        return (at(i, j) * (1 - fu) + at(i + 1, j) * fu) * (1 - fv) + (at(i, j + 1) * (1 - fu) + at(i + 1, j + 1) * fu) * fv;
    }
};

void buildEnvGrid(const MapBakeRequest& rq, const MapFeatureSet& f, EnvGrid& eg) {
    if (f.envelope.empty()) return;
    const float w = rq.wx1 - rq.wx0;
    eg.cell = std::max(8.0f, w / 192.0f);
    eg.n = (uint32_t)std::ceil(w / eg.cell) + 3;
    eg.x0 = rq.wx0 - eg.cell; eg.z0 = rq.wz0 - eg.cell;
    eg.d.assign((size_t)eg.n * eg.n, 0.0f);
    parallelRows(eg.n, [&](uint32_t j0, uint32_t j1) {
        for (uint32_t j = j0; j < j1; ++j)
            for (uint32_t i = 0; i < eg.n; ++i)
                eg.d[(size_t)j * eg.n + i] = mapEnvelopeDistance(f, eg.x0 + (float)i * eg.cell, eg.z0 + (float)j * eg.cell);
    });
}

void paintTerrain(const MapBakeRequest& rq, const MapFeatureSet& f, const HeightGrid& g, Canvas& cv,
                  std::vector<float>& waterFrac, MapBakeStats* st) {
    const auto t0 = Clock::now();
    const uint32_t res = rq.res;
    const float mpp = (rq.wx1 - rq.wx0) / (float)res;
    EnvGrid eg; buildEnvGrid(rq, f, eg);
    waterFrac.assign((size_t)res * res, 0.0f);
    // Light from the north-west, 40 deg up: north = +Z (= screen up), west = -X.
    const float L[3] = { -0.58f, 0.66f, 0.48f };
    const float slopeGain = 2.2f;   // relief exaggeration — the field is gentle near the city
    parallelRows(res, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < res; ++x) {
                float h, d, gx, gz;
                g.sample((float)x + 0.5f, (float)y + 0.5f, h, d, gx, gz);
                float* c = cv.px(x, y);
                // --- land tint x hillshade
                float land[3]; hypsometric(h, land);
                float nx = -gx * slopeGain, ny = 1.0f, nz = -gz * slopeGain;
                const float nl = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
                nx *= nl; ny *= nl; nz *= nl;
                const float shade = std::max(0.0f, nx * L[0] + ny * L[1] + nz * L[2]);
                const float mul = 0.50f + 0.76f * shade;   // flat ground -> ~1.0
                c[0] = land[0] * mul; c[1] = land[1] * mul; c[2] = land[2] * mul;
                // --- wilderness fade (header: TERRAIN FADE). Water texels are
                // handled below and overwrite this, so the sea disc is untouched.
                if (!eg.empty()) {
                    const float ed = eg.sample(rq.wx0 + ((float)x + 0.5f) * mpp, rq.wz0 + ((float)y + 0.5f) * mpp);
                    if (ed > 0.0f) {
                        const float t = smoothstep(0.0f, kEnvelopeMarginM, ed);
                        const float hint = 1.0f + (mul - 1.0f) * mappal::kWildReliefHint;
                        const float far = 1.0f - mappal::kWildFarDarken * smoothstep(kEnvelopeMarginM, kEnvelopeFarM, ed);
                        for (int i = 0; i < 3; ++i)
                            c[i] = (c[i] + ((float)mappal::kWild[i] * hint - c[i]) * t) * far;
                    }
                }
                // --- beach: dry ground within ~4 m of a water surface, not a cliff
                const float slope = std::sqrt(gx * gx + gz * gz);
                if (d <= 0.0f && d > -4.0f && slope < 0.45f) {
                    const float sandT = (1.0f + d / 4.0f) * (1.0f - clamp01(slope / 0.45f) * 0.5f);
                    mix3(c, mappal::kSand, clamp01(sandT) * 0.9f);
                }
                // --- water coverage: 2x2 sub-samples of the interpolated depth so the
                // shoreline is anti-aliased instead of stair-stepped
                float wf = 0.0f;
                if (d > -2.0f) {
                    for (int sy = 0; sy < 2; ++sy)
                        for (int sx = 0; sx < 2; ++sx) {
                            float sh, sd, sgx, sgz;
                            g.sample((float)x + 0.25f + 0.5f * sx, (float)y + 0.25f + 0.5f * sy, sh, sd, sgx, sgz);
                            if (sd > 0.0f) wf += 0.25f;
                        }
                }
                if (wf > 0.0f) {
                    float wcol[3];
                    lerp3(wcol, mappal::kWaterShallow, mappal::kWaterDeep, clamp01(d / 14.0f));
                    mix3f(c, wcol, wf);
                }
                waterFrac[(size_t)y * res + x] = wf;
                cv.a[(size_t)y * res + x] = 1.0f;
            }
        }
    });
    // Shoreline: a 1-texel lighter line on the WATER side of every land/water edge.
    parallelRows(res, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < res; ++x) {
                const size_t k = (size_t)y * res + x;
                if (waterFrac[k] < 0.5f) continue;
                bool edge = false;
                if (x > 0       && waterFrac[k - 1]   < 0.5f) edge = true;
                if (x + 1 < res && waterFrac[k + 1]   < 0.5f) edge = true;
                if (y > 0       && waterFrac[k - res] < 0.5f) edge = true;
                if (y + 1 < res && waterFrac[k + res] < 0.5f) edge = true;
                if (edge) mix3(cv.px(x, y), mappal::kShore, 0.85f);
            }
        }
    });
    if (st) st->terrainMs = msSince(t0);
}

// ---------------------------------------------------------------------------
// 3. District tint discs.
// ---------------------------------------------------------------------------
void paintDistricts(const MapBakeRequest& rq, const MapFeatureSet& f, Canvas& cv, MapBakeStats* st) {
    if (f.districts.empty()) return;
    const auto t0 = Clock::now();
    const uint32_t res = rq.res;
    const float mpp = (rq.wx1 - rq.wx0) / (float)res;
    struct D { float cx, cz, r; float tint[3]; };
    std::vector<D> ds;
    for (const MapDistrict& d : f.districts) {
        // Clip to the tile with a margin; skip districts that cannot touch it.
        if (d.cx + d.r < rq.wx0 || d.cx - d.r > rq.wx1 || d.cz + d.r < rq.wz0 || d.cz - d.r > rq.wz1) continue;
        D o{ d.cx, d.cz, d.r, {} };
        // Lift the authored tint toward white: a zone wash, not a paint spill.
        for (int i = 0; i < 3; ++i) o.tint[i] = (d.rgb[i] * 0.55f + 0.45f) * 255.0f;
        ds.push_back(o);
    }
    if (ds.empty()) return;
    parallelRows(res, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            const float wz = rq.wz0 + ((float)y + 0.5f) * mpp;
            for (uint32_t x = 0; x < res; ++x) {
                const float wx = rq.wx0 + ((float)x + 0.5f) * mpp;
                for (const D& d : ds) {
                    const float dx = wx - d.cx, dz = wz - d.cz;
                    const float dist = std::sqrt(dx * dx + dz * dz);
                    if (dist >= d.r) continue;
                    const float a = 0.26f * smoothstep(1.0f, 0.55f, dist / d.r);
                    mix3f(cv.px(x, y), d.tint, a);
                }
            }
        }
    });
    if (st) st->districtMs = msSince(t0);
}

// ---------------------------------------------------------------------------
// 4. Footprints — exact rect coverage per texel.
// ---------------------------------------------------------------------------
inline float overlap1(float a0, float a1, float b0, float b1) {
    return std::max(0.0f, std::min(a1, b1) - std::max(a0, b0));
}

void paintFootprints(const MapBakeRequest& rq, const MapFeatureSet& f, Canvas& cv,
                     const std::vector<float>& waterFrac, MapBakeStats* st) {
    if (f.footprints.empty()) return;
    const auto t0 = Clock::now();
    const uint32_t res = rq.res;
    const float mpp = (rq.wx1 - rq.wx0) / (float)res, tpm = 1.0f / mpp;
    struct R { float x0, y0, x1, y1; float fill[3], edge[3]; bool edged; uint8_t order; };
    std::vector<R> rs;
    rs.reserve(f.footprints.size());
    for (const MapFootprint& fp : f.footprints) {
        if (fp.x1 <= rq.wx0 || fp.x0 >= rq.wx1 || fp.z1 <= rq.wz0 || fp.z0 >= rq.wz1) continue;
        R r;
        r.x0 = (fp.x0 - rq.wx0) * tpm; r.x1 = (fp.x1 - rq.wx0) * tpm;
        r.y0 = (fp.z0 - rq.wz0) * tpm; r.y1 = (fp.z1 - rq.wz0) * tpm;
        const float ex = r.x1 - r.x0, ey = r.y1 - r.y0;
        if (ex < rq.minFeatureTexels && ey < rq.minFeatureTexels) continue;
        const uint8_t* fillc = mappal::kBldgLow;
        r.edged = true; r.order = 2;
        switch (fp.kind) {
        case MapFootKind::Paved:    fillc = mappal::kPaved; r.edged = false; r.order = 0; break;
        case MapFootKind::Walk:     fillc = mappal::kWalk;  r.edged = false; r.order = 1; break;
        case MapFootKind::Landmark: fillc = mappal::kLandmark; r.order = 3; break;
        case MapFootKind::Building:
            fillc = fp.h < 6.0f ? mappal::kBldgLow : fp.h < 14.0f ? mappal::kBldgMid
                  : fp.h < 30.0f ? mappal::kBldgTall : mappal::kBldgTower;
            break;
        }
        for (int i = 0; i < 3; ++i) {
            r.fill[i] = (float)fillc[i];
            r.edge[i] = (fp.kind == MapFootKind::Landmark) ? (float)mappal::kLandmarkEdge[i] : r.fill[i] * 0.70f;
        }
        // Tall on top of short: store height in the order key's fractional slot
        // via a stable sort below (order first, then h).
        rs.push_back(r);
        rs.back().order = (uint8_t)(r.order * 60 + std::min(59, (int)(fp.h * 1.5f)));
    }
    if (rs.empty()) return;
    std::stable_sort(rs.begin(), rs.end(), [](const R& a, const R& b) { return a.order < b.order; });
    parallelRows(res, [&](uint32_t y0, uint32_t y1) {
        for (const R& r : rs) {
            const int ty0 = std::max((int)y0, (int)std::floor(r.y0));
            const int ty1 = std::min((int)y1 - 1, (int)std::ceil(r.y1));
            if (ty0 > ty1) continue;
            const int tx0 = std::max(0, (int)std::floor(r.x0));
            const int tx1 = std::min((int)res - 1, (int)std::ceil(r.x1));
            const bool edged = r.edged && (r.x1 - r.x0) > 3.0f && (r.y1 - r.y0) > 3.0f;
            for (int y = ty0; y <= ty1; ++y) {
                const float oy = overlap1(r.y0, r.y1, (float)y, (float)y + 1);
                if (oy <= 0.0f) continue;
                const float iy = edged ? overlap1(r.y0 + 1.0f, r.y1 - 1.0f, (float)y, (float)y + 1) : oy;
                for (int x = tx0; x <= tx1; ++x) {
                    const float ox = overlap1(r.x0, r.x1, (float)x, (float)x + 1);
                    if (ox <= 0.0f) continue;
                    const float cov = ox * oy;
                    float* c = cv.px((uint32_t)x, (uint32_t)y);
                    mix3f(c, r.fill, cov);
                    if (edged) {
                        const float ix = overlap1(r.x0 + 1.0f, r.x1 - 1.0f, (float)x, (float)x + 1);
                        const float ecov = cov - ix * iy;
                        if (ecov > 0.001f) mix3f(c, r.edge, ecov);
                    }
                }
            }
        }
    });
    (void)waterFrac;
    if (st) st->footMs = msSince(t0);
}

// ---------------------------------------------------------------------------
// 5. Roads — distance field per class.
// ---------------------------------------------------------------------------
struct RoadField {
    std::vector<float>   dist;    // texel distance to the nearest centreline
    std::vector<float>   along;   // metres along that centreline (dash phase)
    std::vector<uint8_t> flags;   // bit0 tunnel, bit1 bridge
    std::vector<float>   hw;      // that road's drawn half-width (texels)
    std::vector<float>   mh;      // that road's median half-width (texels, 0 = none)
};

struct Seg {
    float ax, ay, bx, by;   // texel space
    float s0, len;          // metres along the road at A, segment length (m)
    float hw;               // drawn half-width (texels)
    float mh;               // median half-width (texels, 0 = single roadway)
    uint8_t flags;
};

void paintRoads(const MapBakeRequest& rq, const MapFeatureSet& f, Canvas& cv, MapBakeStats* st) {
    if (f.roads.empty()) return;
    const auto t0 = Clock::now();
    const uint32_t res = rq.res;
    const float mpp = (rq.wx1 - rq.wx0) / (float)res, tpm = 1.0f / mpp;
    RoadField fld;
    const size_t n = (size_t)res * res;
    fld.dist.resize(n); fld.along.resize(n); fld.flags.resize(n); fld.hw.resize(n); fld.mh.resize(n);

    for (int ci = 0; ci < (int)MapRoadClass::Count; ++ci) {
        const MapRoadClass cls = (MapRoadClass)ci;
        std::vector<Seg> segs;
        float maxReach = 0.0f;
        bool median = false;
        for (const MapRoad& r : f.roads) {
            if (r.cls != cls) continue;
            const size_t cnt = std::min(r.x.size(), r.z.size());
            if (cnt < 2) continue;
            const float hw = mapRoadHalfWidthTexels(cls, r.halfW, mpp);
            if (hw * 2.0f < rq.minFeatureTexels) continue;
            median = median || r.median;
            // The median gap only opens once it is at least a texel wide;
            // below that the stripe path (mh = 0) keeps the overview honest.
            const float mhTex = r.medianHalfW * tpm;
            const float mh = (r.medianHalfW > 0.0f && mhTex >= 0.75f) ? mhTex : 0.0f;
            float s = 0.0f;
            for (size_t i = 0; i + 1 < cnt; ++i) {
                Seg sg;
                sg.ax = (r.x[i] - rq.wx0) * tpm;     sg.ay = (r.z[i] - rq.wz0) * tpm;
                sg.bx = (r.x[i + 1] - rq.wx0) * tpm; sg.by = (r.z[i + 1] - rq.wz0) * tpm;
                const float dxm = r.x[i + 1] - r.x[i], dzm = r.z[i + 1] - r.z[i];
                sg.len = std::sqrt(dxm * dxm + dzm * dzm);
                sg.s0 = s; s += sg.len;
                sg.hw = hw;
                sg.mh = mh;
                sg.flags = 0;
                if (i < r.tunnel.size() && r.tunnel[i]) sg.flags |= 1;
                if (i < r.bridge.size() && r.bridge[i]) sg.flags |= 2;
                // A wide route ends square, not in a 100 m half-disc that
                // reads as a roundabout (interior joins still round so bends
                // stay seamless). Only the polyline's two ends carry the flag.
                if (mh > 0.0f || hw * mpp >= 15.0f) {
                    if (i == 0)       sg.flags |= 4;
                    if (i + 2 == cnt) sg.flags |= 8;
                }
                // Cull segments entirely outside the tile (+ reach).
                const float reach = hw * 1.4f + 3.0f;
                const float sx0 = std::min(sg.ax, sg.bx) - reach, sx1 = std::max(sg.ax, sg.bx) + reach;
                const float sy0 = std::min(sg.ay, sg.by) - reach, sy1 = std::max(sg.ay, sg.by) + reach;
                if (sx1 < 0 || sy1 < 0 || sx0 > (float)res || sy0 > (float)res) continue;
                maxReach = std::max(maxReach, reach);
                segs.push_back(sg);
            }
        }
        if (segs.empty()) continue;

        std::fill(fld.dist.begin(), fld.dist.end(), std::numeric_limits<float>::infinity());
        parallelRows(res, [&](uint32_t y0, uint32_t y1) {
            for (const Seg& sg : segs) {
                const float reach = sg.hw * 1.4f + 3.0f;
                const int ty0 = std::max((int)y0, (int)std::floor(std::min(sg.ay, sg.by) - reach));
                const int ty1 = std::min((int)y1 - 1, (int)std::ceil(std::max(sg.ay, sg.by) + reach));
                if (ty0 > ty1) continue;
                const int tx0 = std::max(0, (int)std::floor(std::min(sg.ax, sg.bx) - reach));
                const int tx1 = std::min((int)res - 1, (int)std::ceil(std::max(sg.ax, sg.bx) + reach));
                const float ex = sg.bx - sg.ax, ey = sg.by - sg.ay;
                const float l2 = std::max(1e-6f, ex * ex + ey * ey);
                for (int y = ty0; y <= ty1; ++y) {
                    const float py = (float)y + 0.5f;
                    for (int x = tx0; x <= tx1; ++x) {
                        const float px = (float)x + 0.5f;
                        float t = ((px - sg.ax) * ex + (py - sg.ay) * ey) / l2;
                        if (((sg.flags & 4) && t < 0.0f) || ((sg.flags & 8) && t > 1.0f)) continue;   // square end
                        t = clamp01(t);
                        const float qx = sg.ax + ex * t - px, qy = sg.ay + ey * t - py;
                        const float d = std::sqrt(qx * qx + qy * qy);
                        if (d > reach) continue;
                        const size_t k = (size_t)y * res + x;
                        // Prefer the wider road at equal distance, and let a
                        // segment's interior win over a neighbour's end cap so the
                        // dash phase runs continuously through joins.
                        if (d < fld.dist[k] - 0.01f || (d < fld.dist[k] + 0.01f && sg.hw > fld.hw[k])) {
                            fld.dist[k] = d; fld.along[k] = sg.s0 + sg.len * t;
                            fld.flags[k] = sg.flags; fld.hw[k] = sg.hw; fld.mh[k] = sg.mh;
                        }
                    }
                }
            }
        });

        // Composite this class.
        const bool fwy = (cls == MapRoadClass::Freeway);
        const uint8_t* fillc = fwy ? mappal::kFwyFill : mappal::kRoadFill;
        const uint8_t* casec = fwy ? mappal::kFwyCase : mappal::kRoadCase;
        // Dash period in metres — long enough to read as dashes at street zoom,
        // never under 14 texels so overview bakes do not dither.
        const float dashPeriod = std::max(28.0f, 14.0f * mpp);
        parallelRows(res, [&](uint32_t y0, uint32_t y1) {
            for (uint32_t y = y0; y < y1; ++y)
                for (uint32_t x = 0; x < res; ++x) {
                    const size_t k = (size_t)y * res + x;
                    const float d = fld.dist[k];
                    if (!(d < 1e9f)) continue;
                    const float hw = fld.hw[k];
                    const float cw = std::clamp(hw * 0.30f, 0.8f, 2.4f);
                    if (d > hw + cw + 1.0f) continue;
                    const bool tunnel = (fld.flags[k] & 1) != 0;
                    const bool bridge = (fld.flags[k] & 2) != 0;
                    const float mh = fld.mh[k];
                    float covFill = clamp01(hw - d + 0.5f);
                    float covCase = clamp01(hw + cw - d + 0.5f);
                    // Twin roadways: no pavement inside the median gap; the
                    // gap gets its own inner casings so it reads as two roads.
                    float covGap = 0.0f, covInner = 0.0f;
                    if (mh > 0.0f) {
                        covGap   = clamp01(mh - d + 0.5f);
                        covFill *= 1.0f - covGap;
                        const float icw = std::max(0.6f, cw * 0.6f);
                        covInner = clamp01(icw - std::fabs(d - mh) + 0.5f);
                    }
                    float* c = cv.px(x, y);
                    bool dashOn = true;
                    if (tunnel) {
                        const float ph = std::fmod(fld.along[k], dashPeriod);
                        dashOn = ph < dashPeriod * 0.55f;
                        // A bored reach: the casing thins to a dashed outline and the
                        // fill only paints on the dashes — the terrain shows through.
                        covCase *= dashOn ? 0.55f : 0.25f;
                        covFill *= dashOn ? 0.80f : 0.0f;
                    }
                    mix3(c, bridge ? mappal::kBridgeCase : casec, covCase);
                    mix3(c, fillc, covFill);
                    if (mh > 0.0f) {
                        // The median: graded ground tone, then the two inner
                        // casing lines (tunnel reaches dash them like the outer).
                        mix3(c, mappal::kMedian, covGap * (tunnel ? (dashOn ? 0.6f : 0.0f) : 0.85f));
                        mix3(c, casec, covInner * (tunnel ? (dashOn ? 0.5f : 0.2f) : 0.9f));
                    } else if (median && fwy && dashOn) {
                        const float mh = std::max(0.55f, std::min(1.4f * tpm, hw * 0.10f));
                        const float covMed = clamp01(mh - d + 0.5f) * (tunnel ? 0.6f : 1.0f);
                        if (covMed > 0.0f) mix3(c, mappal::kMedian, covMed);
                    }
                }
        });
    }
    if (st) st->roadMs = msSince(t0);
}

} // namespace

// ===========================================================================
float mapRoadHalfWidthTexels(MapRoadClass cls, float halfWidthM, float metresPerTexel) {
    float floorTex = 0.75f;
    switch (cls) {
    case MapRoadClass::Street:   floorTex = 0.75f; break;
    case MapRoadClass::Arterial: floorTex = 1.00f; break;
    case MapRoadClass::Ramp:     floorTex = 0.90f; break;
    case MapRoadClass::Freeway:  floorTex = 1.60f; break;
    default: break;
    }
    return std::max(halfWidthM / std::max(1e-4f, metresPerTexel), floorTex);
}

namespace {
bool near3(const uint8_t* p, const uint8_t* ref, int tol) {
    return std::abs((int)p[0] - ref[0]) <= tol && std::abs((int)p[1] - ref[1]) <= tol &&
           std::abs((int)p[2] - ref[2]) <= tol;
}
}
float mapEnvelopeDistance(const MapFeatureSet& f, float wx, float wz) {
    if (f.envelope.empty()) return 0.0f;
    float best = 3.4e38f;
    for (const MapEnvDisc& e : f.envelope) {
        const float d = std::sqrt((wx - e.cx) * (wx - e.cx) + (wz - e.cz) * (wz - e.cz)) - e.r;
        if (d < best) best = d;
        if (best <= 0.0f) return 0.0f;
    }
    return std::max(0.0f, best);
}

bool mapPixelIsWater(const uint8_t* rgba, int tol) {
    // Anywhere on the shallow..deep ramp (or the shoreline tint) counts.
    if (near3(rgba, mappal::kWaterDeep, tol) || near3(rgba, mappal::kWaterShallow, tol)) return true;
    // Between the two: blue-dominant and desaturated.
    return rgba[2] > rgba[0] + 30 && rgba[2] > rgba[1] + 8 && rgba[2] < 180;
}
bool mapPixelIsRoadFill(const uint8_t* rgba, int tol) {
    return near3(rgba, mappal::kRoadFill, tol) || near3(rgba, mappal::kFwyFill, tol);
}
bool mapPixelIsRoadCasing(const uint8_t* rgba, int tol) {
    return near3(rgba, mappal::kRoadCase, tol) || near3(rgba, mappal::kFwyCase, tol) ||
           near3(rgba, mappal::kBridgeCase, tol);
}

void bakeMapTilePixels(const MapFeatureSet& features, const MapBakeRequest& rq,
                       std::vector<uint8_t>& outRgba, MapBakeStats* stats) {
    const auto t0 = Clock::now();
    const uint32_t res = std::max(8u, rq.res);
    outRgba.assign((size_t)res * res * 4, 0);
    if (rq.wx1 <= rq.wx0 || rq.wz1 <= rq.wz0) return;
    MapBakeRequest r = rq; r.res = res;

    Canvas cv; cv.res = res;
    cv.rgb.assign((size_t)res * res * 3, 0.0f);
    cv.a.assign((size_t)res * res, 1.0f);

    HeightGrid grid;
    sampleHeightGrid(r, grid, stats);
    std::vector<float> waterFrac;
    paintTerrain(r, features, grid, cv, waterFrac, stats);
    paintDistricts(r, features, cv, stats);
    paintFootprints(r, features, cv, waterFrac, stats);
    paintRoads(r, features, cv, stats);

    // Feather + outer ring, then pack.
    const float feather = (float)r.featherTexels;
    parallelRows(res, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = 0; x < res; ++x) {
                const size_t k = (size_t)y * res + x;
                float a = cv.a[k];
                if (feather > 0.0f) {
                    const float e = (float)std::min(std::min(x, res - 1 - x), std::min(y, res - 1 - y));
                    a *= clamp01(e / feather);
                }
                if (x == 0 || y == 0 || x == res - 1 || y == res - 1) a = 0.0f;
                const float* c = &cv.rgb[k * 3];
                uint8_t* o = &outRgba[k * 4];
                o[0] = (uint8_t)std::lround(std::clamp(c[0], 0.0f, 255.0f));
                o[1] = (uint8_t)std::lround(std::clamp(c[1], 0.0f, 255.0f));
                o[2] = (uint8_t)std::lround(std::clamp(c[2], 0.0f, 255.0f));
                o[3] = (uint8_t)std::lround(clamp01(a) * 255.0f);
            }
    });
    if (stats) stats->totalMs = msSince(t0);
}

} // namespace x3::game
