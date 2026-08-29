// ===========================================================================
// motion_rig.cpp — the motion-domain verification rig. See motion_rig.h for
// what it is for and why the metric is shaped the way it is.
// ===========================================================================
#include "motion_rig.h"

#include "surface_library.h"      // decodePngRGBA8
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace x3::motionrig {

namespace {

inline double luma(const uint8_t* p) {
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
}

// Clamp a Region to whole pixels, leaving a one-pixel margin so the central
// gradient stencil below never reads outside the frame.
struct PixRect { int x0, y0, x1, y1; };
PixRect toPixels(const Region& r, int w, int h) {
    PixRect o;
    o.x0 = std::clamp((int)std::floor(r.x0 * (float)w), 1, w - 2);
    o.y0 = std::clamp((int)std::floor(r.y0 * (float)h), 1, h - 2);
    o.x1 = std::clamp((int)std::ceil (r.x1 * (float)w), o.x0 + 1, w - 1);
    o.y1 = std::clamp((int)std::ceil (r.y1 * (float)h), o.y0 + 1, h - 1);
    return o;
}

} // namespace

// ---------------------------------------------------------------------------
RgbaImage loadImage(const std::string& path) {
    RgbaImage img;
    img.px = x3::game::decodePngRGBA8(path, img.w, img.h);
    if (!img.valid()) { img.w = img.h = 0; img.px.clear(); }
    return img;
}

// ---------------------------------------------------------------------------
// PART 1 — the capture.
// ---------------------------------------------------------------------------
std::vector<std::string> captureRails(x3::rhi::IRenderDevice& device,
                                      const RailsConfig& cfg,
                                      RailsDrawFn draw, void* drawCtx) {
    std::vector<std::string> out;
    if (cfg.frames <= 0) return out;

    namespace fs = std::filesystem;
    std::error_code ec;
    if (!cfg.dir.empty()) fs::create_directories(cfg.dir, ec);

    // Two device-state resets, both using EXISTING documented mechanisms, so two
    // successive series start from an identical renderer state and the
    // determinism check can be bit-exact rather than "close enough":
    //
    //  (1) TAA HISTORY. setPostFX() invalidates the history on an off->on
    //      transition (VulkanRenderDevice.cpp: `if (p.taa && !m_post.taa)
    //      m_taaHistoryValid = false;`). Toggling through off does that without
    //      needing a resize or a new engine API.
    //  (2) TAA JITTER PHASE. A free-running frame counter with an 8-frame Halton
    //      cycle, which is why RailsConfig documents (settle + frames) as a
    //      multiple of 8: the counter lands on the same phase at the start of
    //      every series. The rig cannot reset that counter, so it aligns to it.
    //
    // Auto-exposure needs no reset: it snaps on every headless frame by design.
    // The motion-blur tap dither needs none either: its phase is forced to 0 in
    // headless, for exactly this reason.

    const int total = cfg.settle + cfg.frames;
    out.reserve((size_t)cfg.frames);

    for (int i = 0; i < total; ++i) {
        // Path time. Settle frames walk BACKWARDS from t0 so the camera is
        // already in steady motion (and therefore producing real velocity) by
        // the time frame 0 is captured -- a camera that starts from rest at the
        // first captured frame would have a spurious zero-velocity frame.
        const float t = cfg.t0 + (float)(i - cfg.settle) * cfg.dt;
        const int   capIdx = i - cfg.settle;

        device.setCamera(cfg.camX + cfg.camSpeed * t, cfg.camY, cfg.camZ,
                         cfg.yaw, cfg.pitch, cfg.fov);

        char path[512] = {0};
        if (capIdx >= 0) {
            std::snprintf(path, sizeof(path), "%s/%s_%03d.png",
                          cfg.dir.c_str(), cfg.prefix.c_str(), capIdx);
            device.armCapture(path);
        }

        auto frame = device.beginFrame();
        if (frame.valid && draw) draw(drawCtx, device, frame, t);
        device.endFrame(frame);

        if (capIdx >= 0) {
            if (!device.captureFrame(path)) {
                x3::logError(std::string("[motionrig] capture failed: ") + path);
                return {};
            }
            out.emplace_back(path);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// PART 2 — the measurement.
// ---------------------------------------------------------------------------
SeriesMetrics measureFrames(const std::vector<RgbaImage>& frames,
                            MotionAxis axis, Region roi) {
    SeriesMetrics m;
    if (frames.empty() || !frames[0].valid()) return m;
    const int w = frames[0].w, h = frames[0].h;
    for (const auto& f : frames)
        if (!f.valid() || f.w != w || f.h != h) return m;

    const PixRect r = toPixels(roi, w, h);
    const bool horizontal = (axis == MotionAxis::Horizontal);

    double sumAlong = 0.0, sumAcross = 0.0;
    size_t nGrad = 0;
    double sumSqTime = 0.0;
    size_t nTime = 0;

    for (size_t fi = 0; fi < frames.size(); ++fi) {
        const RgbaImage& img = frames[fi];
        const RgbaImage* prev = (fi > 0) ? &frames[fi - 1] : nullptr;

        for (int y = r.y0; y < r.y1; ++y) {
            for (int x = r.x0; x < r.x1; ++x) {
                const size_t c  = ((size_t)y * w + x) * 4;
                const size_t xp = ((size_t)y * w + (x + 1)) * 4;
                const size_t xm = ((size_t)y * w + (x - 1)) * 4;
                const size_t yp = ((size_t)(y + 1) * w + x) * 4;
                const size_t ym = ((size_t)(y - 1) * w + x) * 4;

                // Central differences, accumulated as ENERGY (squared), not as
                // absolute value.
                //
                // THIS CHOICE IS LOAD-BEARING. Mean |gradient| is total variation,
                // and total variation is CONSERVED when you blur a step edge: an
                // edge of height H spread over N pixels gives N pixels of gradient
                // H/N, which sums to the same H. A mean-|gradient| metric would
                // therefore be nearly blind to blur on hard edges -- a guard that
                // does not guard. Gradient ENERGY falls as H^2/N over the same
                // spreading, so it responds monotonically to blur length, and it
                // collapses outright once the blur exceeds the feature size.
                const double gx = luma(&img.px[xp]) - luma(&img.px[xm]);
                const double gy = luma(&img.px[yp]) - luma(&img.px[ym]);
                sumAlong  += horizontal ? gx * gx : gy * gy;
                sumAcross += horizontal ? gy * gy : gx * gx;
                ++nGrad;

                if (prev) {
                    const double d = luma(&img.px[c]) - luma(&prev->px[c]);
                    sumSqTime += d * d;
                    ++nTime;
                }
            }
        }
    }

    if (nGrad == 0) return m;
    m.frames      = (int)frames.size();
    // RMS, so both numbers stay in luminance units and the ratio is meaningful.
    m.gradAlong   = std::sqrt(sumAlong  / (double)nGrad);
    m.gradAcross  = std::sqrt(sumAcross / (double)nGrad);
    m.anisotropy  = (m.gradAcross > 1e-9) ? (m.gradAlong / m.gradAcross) : 0.0;
    m.temporalRms = (nTime > 0) ? std::sqrt(sumSqTime / (double)nTime) : 0.0;
    m.valid       = true;
    return m;
}

SeriesMetrics measureSeries(const std::vector<std::string>& pngs,
                            MotionAxis axis, Region roi) {
    std::vector<RgbaImage> frames;
    frames.reserve(pngs.size());
    for (const auto& p : pngs) {
        RgbaImage img = loadImage(p);
        if (!img.valid()) {
            x3::logError("[motionrig] could not decode " + p);
            return {};
        }
        frames.emplace_back(std::move(img));
    }
    return measureFrames(frames, axis, roi);
}

double directionalBlurIndex(const SeriesMetrics& test, const SeriesMetrics& ref) {
    if (!test.valid || !ref.valid) return 0.0;
    if (!(ref.anisotropy > 1e-6))  return 0.0;
    return 1.0 - (test.anisotropy / ref.anisotropy);
}

// ---------------------------------------------------------------------------
// PART 3 — helpers.
// ---------------------------------------------------------------------------
ImageDelta compareImages(const std::string& a, const std::string& b) {
    ImageDelta d;
    RgbaImage ia = loadImage(a), ib = loadImage(b);
    if (!ia.valid() || !ib.valid() || ia.w != ib.w || ia.h != ib.h) return d;

    double sumAbs = 0.0, sumSq = 0.0;
    int    maxAbs = 0;
    const size_t n = (size_t)ia.w * ia.h;
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const int dv = (int)ia.px[i * 4 + c] - (int)ib.px[i * 4 + c];
            const int av = dv < 0 ? -dv : dv;
            sumAbs += av; sumSq += (double)dv * dv;
            if (av > maxAbs) maxAbs = av;
        }
    }
    const double mse = sumSq / (double)(n * 3);
    d.meanAbs = sumAbs / (double)(n * 3);
    d.maxAbs  = maxAbs;
    // Same formulation as the tree's existing dB receipts (geolod_shot.cpp).
    d.psnrDb  = (mse > 0.0) ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
    d.valid   = true;
    return d;
}

bool seriesBitIdentical(const std::vector<std::string>& a,
                        const std::vector<std::string>& b) {
    if (a.empty() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        RgbaImage ia = loadImage(a[i]), ib = loadImage(b[i]);
        if (!ia.valid() || !ib.valid()) return false;
        if (ia.w != ib.w || ia.h != ib.h) return false;
        if (ia.px != ib.px) return false;
    }
    return true;
}

RgbaImage applyDirectionalBlur(const RgbaImage& src, MotionAxis axis, int radiusPx) {
    RgbaImage dst = src;
    if (!src.valid() || radiusPx <= 0) return dst;
    const bool horizontal = (axis == MotionAxis::Horizontal);
    const int w = src.w, h = src.h;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int acc[3] = { 0, 0, 0 };
            int n = 0;
            for (int k = -radiusPx; k <= radiusPx; ++k) {
                const int sx = horizontal ? std::clamp(x + k, 0, w - 1) : x;
                const int sy = horizontal ? y : std::clamp(y + k, 0, h - 1);
                const size_t si = ((size_t)sy * w + sx) * 4;
                acc[0] += src.px[si]; acc[1] += src.px[si + 1]; acc[2] += src.px[si + 2];
                ++n;
            }
            const size_t di = ((size_t)y * w + x) * 4;
            dst.px[di]     = (uint8_t)(acc[0] / n);
            dst.px[di + 1] = (uint8_t)(acc[1] / n);
            dst.px[di + 2] = (uint8_t)(acc[2] / n);
        }
    }
    return dst;
}

} // namespace x3::motionrig
