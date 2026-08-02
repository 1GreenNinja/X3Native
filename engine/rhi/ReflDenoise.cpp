// Reflection buffer DENOISE — edge-aware a-trous, device-independent reference.
// CLEAN-ROOM: see the provenance block in ReflDenoise.h. No GPL / id Tech /
// RBDOOM / Unreal source consulted.
//
// shaders/refl_denoise.comp is a transcription of filterIteration() below. If
// one changes, change the other; --test-refldenoise guards the PROPERTIES (edge
// preservation, energy, identity-when-off) but cannot see the GPU, so the
// screenshots are the gate on the shader itself.

#include "engine/rhi/ReflDenoise.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace x3::refldn {
namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The shared body of the real filter and the negative control. `edgeStops`
// selects whether the depth + normal weights participate; everything else —
// tap pattern, spatial kernel, PREMULTIPLIED confidence accumulation — is
// identical, so any assertion the control fails is failing specifically because
// the edge stops are gone and not because two different filters were compared.
void iterateImpl(const Pixel* src, const Aux* aux, int w, int h,
                 int step, const Params& p, Pixel* dst, bool edgeStops) {
    if (w <= 0 || h <= 0 || step < 1) return;
    const float normalPow = std::max(p.normalPow, 0.0f);
    const float depthSigma = std::max(p.depthSigma, 1e-4f);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int   ci = y * w + x;
            const Pixel c  = src[ci];
            const Aux   a0 = aux[ci];

            // SKY / no-surface centre: refl.comp wrote nothing meaningful here,
            // so there is nothing to denoise. Pass it through untouched rather
            // than pulling neighbouring reflections into the sky.
            if (!(a0.dist > 0.0f)) { dst[ci] = c; continue; }

            // Accumulators. sumW is the pure geometric/spatial weight (it
            // normalises CONFIDENCE); sumCW is weight*confidence (it normalises
            // COLOUR). Splitting them is the premultiplied reconstruction that
            // stops zero-confidence neighbours — whose colour is BLACK — from
            // darkening a confident pixel. See the header for why that binary
            // confidence field is the measured blotch.
            float sumW  = 0.0f;
            float sumCW = 0.0f;
            float sumRGB[3] = { 0.0f, 0.0f, 0.0f };

            for (int ky = -2; ky <= 2; ++ky) {
                for (int kx = -2; kx <= 2; ++kx) {
                    const int tx = clampi(x + kx * step, 0, w - 1);
                    const int ty = clampi(y + ky * step, 0, h - 1);
                    const int ti = ty * w + tx;
                    const Aux at = aux[ti];
                    if (!(at.dist > 0.0f)) continue;      // sky / invalid tap

                    float wt = kAtrousRow[kx + 2] * kAtrousRow[ky + 2];

                    if (edgeStops) {
                        // DEPTH stop. Tolerance is relative to view distance so
                        // far geometry (coarser depth, larger texel footprint)
                        // is not over-rejected — the ssgi_temporal.frag rule.
                        // This is the term that refuses to filter across the
                        // car's lower silhouette onto the floor.
                        const float tol  = depthSigma * a0.dist;
                        const float dd   = std::fabs(at.dist - a0.dist);
                        wt *= std::exp(-dd / tol);

                        // NORMAL stop. Rejects creases and silhouettes that
                        // happen to sit at a similar depth (a wing mirror
                        // against the door behind it), which the depth term
                        // alone cannot see.
                        const float nd = a0.n[0] * at.n[0] + a0.n[1] * at.n[1] + a0.n[2] * at.n[2];
                        wt *= std::pow(std::max(nd, 0.0f), normalPow);
                    }

                    if (!(wt > 0.0f)) continue;
                    const float cw = wt * std::max(src[ti].conf, 0.0f);
                    sumW  += wt;
                    sumCW += cw;
                    sumRGB[0] += cw * src[ti].rgb[0];
                    sumRGB[1] += cw * src[ti].rgb[1];
                    sumRGB[2] += cw * src[ti].rgb[2];
                }
            }

            Pixel o;
            if (sumW > 0.0f) {
                o.conf = sumCW / sumW;
                if (sumCW > 1e-8f) {
                    o.rgb[0] = sumRGB[0] / sumCW;
                    o.rgb[1] = sumRGB[1] / sumCW;
                    o.rgb[2] = sumRGB[2] / sumCW;
                } else {
                    // No confident neighbour anywhere in the footprint: keep the
                    // centre's colour so a zero-confidence region does not
                    // invent one. Its confidence is (correctly) ~0, so
                    // mesh.frag's blend weight discards it regardless.
                    o.rgb[0] = c.rgb[0]; o.rgb[1] = c.rgb[1]; o.rgb[2] = c.rgb[2];
                }
            } else {
                o = c;
            }
            dst[ci] = o;
        }
    }
}

} // namespace

void filterIteration(const Pixel* src, const Aux* aux, int w, int h,
                     int step, const Params& p, Pixel* dst) {
    iterateImpl(src, aux, w, h, step, p, dst, /*edgeStops=*/true);
}

void filterIterationNoEdgeStops(const Pixel* src, const Aux* aux, int w, int h,
                                int step, const Params& p, Pixel* dst) {
    iterateImpl(src, aux, w, h, step, p, dst, /*edgeStops=*/false);
}

void filter(const Pixel* src, const Aux* aux, int w, int h, const Params& p,
            Pixel* dst, Pixel* scratch, bool edgeStops) {
    const size_t n = (size_t)std::max(w, 0) * (size_t)std::max(h, 0);
    if (n == 0) return;
    // iterations <= 0 IS the r_refldenoise 0 contract: a verbatim copy, so the
    // consumer sees byte-identical texels and the render is bit-exact.
    if (p.iterations <= 0) { std::memcpy(dst, src, n * sizeof(Pixel)); return; }

    // Ping-pong so the LAST write always lands in `dst`, for any iteration
    // count. The renderer uses the identical parity trick (dstIdx = (N-1-i)&1)
    // so mesh.frag's descriptor can bind ONE fixed image with no per-frame
    // descriptor churn.
    const bool odd = (p.iterations & 1) != 0;
    Pixel* pingA = odd ? dst : scratch;   // first write target
    Pixel* pingB = odd ? scratch : dst;

    const Pixel* in = src;
    Pixel* out = pingA;
    int step = 1;
    for (int i = 0; i < p.iterations; ++i) {
        iterateImpl(in, aux, w, h, step, p, out, edgeStops);
        in = out;
        out = (out == pingA) ? pingB : pingA;
        step *= 2;
    }
}

} // namespace x3::refldn
