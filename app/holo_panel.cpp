// HoloPanel — THE holo-glass platform. See app/holo_panel.h.
//
// Clean-room: Scene/Entity + IRenderDevice + mesh_prims + stb_truetype only.
#include "holo_panel.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"
#include "engine/rhi/font_robotomono.h"

// stb_truetype: DECLARATIONS ONLY (the implementation is compiled once inside the
// engine TU). We rasterize on the CPU straight into the screen texture, so the text
// lives ON the glass and tilts in 3D with the panel — a Babylon DynamicTexture, not
// a camera-facing 2D overlay.
#include <stb_truetype.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {
namespace holo {

namespace {
constexpr float kPi = 3.14159265f;

struct GlassFont {
    stbtt_fontinfo info{};
    bool ready = false;
    GlassFont() {
        const unsigned char* ttf = x3::rhi::kRobotoMonoTTF;
        const int off = stbtt_GetFontOffsetForIndex(ttf, 0);
        if (off >= 0 && stbtt_InitFont(&info, ttf, off)) ready = true;
    }
};
const GlassFont& gFont() { static GlassFont f; return f; }
} // namespace

bool fontReady() { return gFont().ready; }

// ---------------------------------------------------------------------------
// STATUS INK — the keyword read that makes a console look like a console.
// ---------------------------------------------------------------------------
Ink statusInk(const std::string& s) {
    std::string U; U.reserve(s.size());
    for (char ch : s) U += (char)std::toupper((unsigned char)ch);
    auto has = [&](const char* kw) { return U.find(kw) != std::string::npos; };

    // POSITIVE OVERRIDES FIRST — and this ordering is load-bearing, not style.
    // These are the good-news words that CONTAIN a bad-news word as a substring:
    // "UNLOCK" contains "LOCK", "UNLOCKED" contains "LOCKED", "STABLE" contains a
    // fragment of "UNSTABLE" in the other direction. Run the negative list first and
    // "ENTER OVERRIDE CODE TO UNLOCK CELL" reads as an ALERT, which is precisely
    // backwards. Longest/most-specific meaning wins.
    if (has("UNLOCK") || has("GRANTED") || has("NO FAULT") || has("NO ERROR"))
        return kGreen;

    // ORANGE — warning / alert / failure / anything the player must not ignore.
    if (has("FAIL") || has("WARN") || has("ALERT") || has("CRIT") || has("DANGER") ||
        has("BREACH") || has("AUGMENT") || has("REJECT") || has("UNSTABLE") ||
        has("LOCK") || has("DENIED") || has("ERROR") || has("DESTROY"))
        return kOrange;

    // GREEN — good news: it is up, it is safe, it worked.
    if (has("OK") || has("ONLINE") || has("SECURE") || has("ACTIVE") || has("STABLE") ||
        has("OPEN") || has("READY") || has("NOMINAL") || has("PASS") || has("CLEAR"))
        return kGreen;

    return kBlue;   // everything else: headers, data, structure.
}

// ---------------------------------------------------------------------------
// Line-art primitives (additive, anti-aliased).
// ---------------------------------------------------------------------------
void plot(Canvas& c, float fx, float fy, Ink k, float a) {
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    c.addInk(x0,   y0,   k, a * (1 - tx) * (1 - ty));
    c.addInk(x0+1, y0,   k, a * (tx)     * (1 - ty));
    c.addInk(x0,   y0+1, k, a * (1 - tx) * (ty));
    c.addInk(x0+1, y0+1, k, a * (tx)     * (ty));
}

void line(Canvas& c, float x0, float y0, float x1, float y1, Ink k, float a, float thick) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx*dx + dy*dy);
    if (len < 0.001f) { plot(c, x0, y0, k, a); return; }
    const float nx = dx / len, ny = dy / len;
    const float px = -ny, py = nx;
    const int steps = (int)(len + 1.0f);
    const int half = (int)std::ceil(thick * 0.5f);
    for (int s = 0; s <= steps; ++s) {
        const float cxp = x0 + nx * s, cyp = y0 + ny * s;
        for (int t = -half; t <= half; ++t) {
            const float d = std::fabs((float)t);
            const float cov = 1.0f - std::max(0.0f, (d - thick * 0.5f + 0.5f));
            if (cov <= 0.0f) continue;
            plot(c, cxp + px * t, cyp + py * t, k, a * (cov > 1 ? 1 : cov));
        }
    }
}

void rectFrame(Canvas& c, float x0, float y0, float x1, float y1, Ink k, float a, float thick) {
    line(c, x0,y0, x1,y0, k,a,thick);
    line(c, x1,y0, x1,y1, k,a,thick);
    line(c, x1,y1, x0,y1, k,a,thick);
    line(c, x0,y1, x0,y0, k,a,thick);
}

void rectFill(Canvas& c, float x0, float y0, float x1, float y1, Ink k, float a) {
    const int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
    for (int y = iy0; y <= iy1; ++y)
        for (int x = ix0; x <= ix1; ++x)
            c.addInk(x, y, k, a);
}

void roundRectFrame(Canvas& c, float x0, float y0, float x1, float y1, float rad,
                    Ink k, float a, float thick) {
    line(c, x0+rad,y0, x1-rad,y0, k,a,thick);
    line(c, x0+rad,y1, x1-rad,y1, k,a,thick);
    line(c, x0,y0+rad, x0,y1-rad, k,a,thick);
    line(c, x1,y0+rad, x1,y1-rad, k,a,thick);
    struct Cn { float cx, cy, a0; } cn[4] = {
        { x1-rad, y0+rad, -1.57079633f }, { x1-rad, y1-rad, 0.0f },
        { x0+rad, y1-rad, 1.57079633f },  { x0+rad, y0+rad, 3.14159265f },
    };
    for (auto& kk : cn) {
        const int seg = 8;
        float pxp = kk.cx + std::cos(kk.a0) * rad, pyp = kk.cy + std::sin(kk.a0) * rad;
        for (int s = 1; s <= seg; ++s) {
            const float aa = kk.a0 + 1.57079633f * ((float)s / seg);
            const float nxp = kk.cx + std::cos(aa) * rad, nyp = kk.cy + std::sin(aa) * rad;
            line(c, pxp,pyp, nxp,nyp, k,a,thick);
            pxp = nxp; pyp = nyp;
        }
    }
}

void bracketFrame(Canvas& c, float x0, float y0, float x1, float y1,
                  float arm, float chamf, Ink k, float a, float thick) {
    line(c, x0,y0, x0+arm,y0, k,a,thick);
    line(c, x0,y0, x0,y0+arm, k,a,thick);
    line(c, x0,y1, x0+arm,y1, k,a,thick);
    line(c, x0,y1, x0,y1-arm, k,a,thick);
    line(c, x1,y1, x1-arm,y1, k,a,thick);
    line(c, x1,y1, x1,y1-arm, k,a,thick);
    // top-right CHAMFERED corner (the sci-fi cut corner).
    line(c, x1-chamf,y0, x1-arm-chamf,y0, k,a,thick);
    line(c, x1,y0+chamf, x1,y0+arm+chamf, k,a,thick);
    line(c, x1-chamf,y0, x1,y0+chamf, k,a,thick);
    // faint connecting edges (the frame reads continuous).
    line(c, x0+arm,y0, x1-arm-chamf,y0, k, a*0.30f, thick*0.7f);
    line(c, x0+arm,y1, x1-arm,y1,       k, a*0.30f, thick*0.7f);
    line(c, x0,y0+arm, x0,y1-arm,       k, a*0.30f, thick*0.7f);
    line(c, x1,y0+arm+chamf, x1,y1-arm, k, a*0.30f, thick*0.7f);
}

void hexagon(Canvas& c, float cx, float cy, float rad, Ink k, float a, float thick) {
    float pxp = 0, pyp = 0;
    for (int s = 0; s <= 6; ++s) {
        const float aa = 0.5235988f + 1.04719755f * s;
        const float nxp = cx + std::cos(aa) * rad, nyp = cy + std::sin(aa) * rad;
        if (s > 0) line(c, pxp,pyp, nxp,nyp, k,a,thick);
        pxp = nxp; pyp = nyp;
    }
}

void warnTriangle(Canvas& c, float cx, float topY, float halfW, float h,
                  Ink k, float a, float thick) {
    const float blX = cx - halfW, brX = cx + halfW, baseY = topY + h;
    line(c, cx,topY, blX,baseY, k,a,thick);
    line(c, cx,topY, brX,baseY, k,a,thick);
    line(c, blX,baseY, brX,baseY, k,a,thick);
    line(c, cx, topY + h*0.34f, cx, topY + h*0.66f, k,a,thick);
    rectFill(c, cx-thick*0.5f, topY + h*0.76f, cx+thick*0.5f, topY + h*0.82f, k, a);
}

void chevronDown(Canvas& c, float cx, float topY, float halfW, float h,
                 Ink k, float a, float thick) {
    line(c, cx-halfW, topY, cx, topY+h, k,a,thick);
    line(c, cx+halfW, topY, cx, topY+h, k,a,thick);
}

void chevronUp(Canvas& c, float cx, float botY, float halfW, float h,
               Ink k, float a, float thick) {
    line(c, cx-halfW, botY, cx, botY-h, k,a,thick);
    line(c, cx+halfW, botY, cx, botY-h, k,a,thick);
}

// ---------------------------------------------------------------------------
// Text. See the header for why xScale exists (non-square panels stretch the bake).
// ---------------------------------------------------------------------------
float textWidth(const std::string& s, float px, float xScale) {
    const GlassFont& f = gFont();
    if (!f.ready) return px * 0.6f * (float)s.size() * xScale;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    float w = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, (unsigned char)s[i], &adv, &lsb);
        w += adv * scale;
        if (i + 1 < s.size())
            w += stbtt_GetCodepointKernAdvance(&f.info, (unsigned char)s[i],
                                               (unsigned char)s[i+1]) * scale;
    }
    return w * xScale;
}

float drawText(Canvas& c, const std::string& s, float penX, float topY, float px,
               Ink k, float a, float xScale) {
    const GlassFont& f = gFont();
    if (!f.ready) return 0.0f;
    const float scaleY = stbtt_ScaleForPixelHeight(&f.info, px);
    const float scaleX = scaleY * xScale;
    int asc = 0, desc = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&f.info, &asc, &desc, &lineGap);
    const float baseline = topY + asc * scaleY;
    float pen = penX;
    for (size_t i = 0; i < s.size(); ++i) {
        const int ch = (unsigned char)s[i];
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, ch, &adv, &lsb);
        if (ch != ' ') {
            int gw = 0, gh = 0, gxo = 0, gyo = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(&f.info, scaleX, scaleY, ch,
                                                          &gw, &gh, &gxo, &gyo);
            if (bmp) {
                const float gx0 = pen + lsb * scaleX;
                for (int yy = 0; yy < gh; ++yy) {
                    for (int xx = 0; xx < gw; ++xx) {
                        const float cov = bmp[yy * gw + xx] / 255.0f;
                        if (cov <= 0.003f) continue;
                        c.addInk((int)std::lround(gx0 + gxo + xx),
                                 (int)std::lround(baseline + gyo + yy), k, a * cov);
                    }
                }
                stbtt_FreeBitmap(bmp, nullptr);
            }
        }
        pen += adv * scaleX;
        if (i + 1 < s.size())
            pen += stbtt_GetCodepointKernAdvance(&f.info, ch, (unsigned char)s[i+1]) * scaleX;
    }
    return pen - penX;
}

// ---------------------------------------------------------------------------
// Bases.
// ---------------------------------------------------------------------------
void blackGlassBase(Canvas& c) {
    const uint32_t n = c.n;
    const float fn = (float)n;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / fn, v = (y + 0.5f) / fn;
            float br = 0.004f, bg = 0.010f, bb = 0.020f;   // near-black, faintest cool cast
            const float grad = 1.0f - v * 0.35f;
            br *= grad; bg *= grad; bb *= grad;
            const float scan = 0.70f + 0.30f * ((y % 4u) < 2u ? 1.0f : 0.55f);
            br *= scan; bg *= scan; bb *= scan;
            if ((x % 28u) < 1u || (y % 28u) < 1u) { bg += 0.012f; bb += 0.020f; }  // data-grid
            const float cx = (u - 0.5f), cy = (v - 0.5f);
            const float rad = std::sqrt(cx*cx + cy*cy) * 1.42f;
            const float vig = 1.0f - 0.45f * rad * rad;
            br *= vig; bg *= vig; bb *= vig;
            const size_t i = (size_t)y * n + x;
            c.r[i] = br; c.g[i] = bg; c.b[i] = bb;
        }
    }
}

void warmBase(Canvas& c) {
    const uint32_t n = c.n;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float scan = 0.80f + 0.20f * ((y % 4u) < 2u ? 1.0f : 0.6f);
            const size_t i = (size_t)y * n + x;
            c.r[i] = 0.030f * scan; c.g[i] = 0.017f * scan; c.b[i] = 0.005f * scan;
        }
    }
}

// ---------------------------------------------------------------------------
// THE QUIET BAND — the text-vs-graphics fix.
// ---------------------------------------------------------------------------
void quietBand(Canvas& c, float x0, float y0, float x1, float y1,
               float keep, float feather) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    const int ix0 = std::max(0, (int)std::floor(x0 - feather));
    const int iy0 = std::max(0, (int)std::floor(y0 - feather));
    const int ix1 = std::min((int)c.n - 1, (int)std::ceil(x1 + feather));
    const int iy1 = std::min((int)c.n - 1, (int)std::ceil(y1 + feather));
    const float f = std::max(0.001f, feather);
    for (int y = iy0; y <= iy1; ++y) {
        for (int x = ix0; x <= ix1; ++x) {
            // Signed distance INSIDE the rect (0 at the border, grows inward), clamped
            // to the feather -> a soft-edged well rather than a hard rectangular cut.
            const float dx = std::min((float)x - x0, x1 - (float)x);
            const float dy = std::min((float)y - y0, y1 - (float)y);
            const float d  = std::min(dx, dy);
            if (d <= -f) continue;                       // fully outside the feather
            const float t = std::clamp((d + f) / (2.0f * f), 0.0f, 1.0f);  // 0 out -> 1 in
            const float mul = 1.0f + (keep - 1.0f) * t;  // 1 outside -> `keep` inside
            const size_t i = (size_t)y * c.n + x;
            c.r[i] *= mul; c.g[i] *= mul; c.b[i] *= mul;
        }
    }
}

// ---------------------------------------------------------------------------
// Probes.
// ---------------------------------------------------------------------------
namespace {
inline void probeRect(const Canvas& c, float x0, float y0, float x1, float y1,
                      int& ix0, int& iy0, int& ix1, int& iy1) {
    ix0 = std::max(0, (int)x0); iy0 = std::max(0, (int)y0);
    ix1 = std::min((int)c.n, (int)x1); iy1 = std::min((int)c.n, (int)y1);
}
} // namespace

float inkFraction(const Canvas& c, float x0, float y0, float x1, float y1, float lumaThresh) {
    int ix0, iy0, ix1, iy1; probeRect(c, x0, y0, x1, y1, ix0, iy0, ix1, iy1);
    uint64_t lit = 0, total = 0;
    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            const size_t i = (size_t)y * c.n + x;
            const float lum = 0.2126f * c.r[i] + 0.7152f * c.g[i] + 0.0722f * c.b[i];
            if (lum > lumaThresh) ++lit;
            ++total;
        }
    }
    return total ? (float)lit / (float)total : 0.0f;
}

namespace {
// A pixel "counts" as a colour only if it is LIT (above the substrate) and that
// channel genuinely dominates. Blue demands b > g by a real margin — which is exactly
// what makes CYAN (g ~= b) fail the blue test. That is the point.
template <typename F>
float channelFraction(const Canvas& c, float x0, float y0, float x1, float y1, F pred) {
    int ix0, iy0, ix1, iy1; probeRect(c, x0, y0, x1, y1, ix0, iy0, ix1, iy1);
    uint64_t hit = 0, total = 0;
    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            const size_t i = (size_t)y * c.n + x;
            const float r = c.r[i], g = c.g[i], b = c.b[i];
            const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            ++total;
            if (lum > 0.30f && pred(r, g, b)) ++hit;
        }
    }
    return total ? (float)hit / (float)total : 0.0f;
}
} // namespace

float blueFraction(const Canvas& c, float x0, float y0, float x1, float y1) {
    return channelFraction(c, x0, y0, x1, y1, [](float r, float g, float b) {
        return b > g * 1.30f && b > r * 1.60f;      // BLUE, not cyan (cyan has g ~= b)
    });
}
float greenFraction(const Canvas& c, float x0, float y0, float x1, float y1) {
    return channelFraction(c, x0, y0, x1, y1, [](float r, float g, float b) {
        return g > b * 1.40f && g > r * 1.40f;
    });
}
float orangeFraction(const Canvas& c, float x0, float y0, float x1, float y1) {
    return channelFraction(c, x0, y0, x1, y1, [](float r, float g, float b) {
        return r > b * 2.0f && r > g * 1.30f;
    });
}

// ---------------------------------------------------------------------------
// Composite out.
// ---------------------------------------------------------------------------
std::vector<uint8_t> finish(const Canvas& c) {
    const uint32_t n = c.n;
    const float fn = (float)n;
    const float rc = 0.30f;
    std::vector<uint8_t> px((size_t)n * n * 4);
    auto to8 = [](float v) -> uint8_t {
        int i = (int)(v * 255.0f + 0.5f);
        return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
    };
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / fn, v = (y + 0.5f) / fn;
            const float ax = std::fabs(u - 0.5f) * 2.0f, ay = std::fabs(v - 0.5f) * 2.0f;
            float fade = 1.0f;
            const float in = 1.0f - rc;
            if (ax > in && ay > in) {
                const float dx = (ax - in) / rc, dy = (ay - in) / rc;
                const float cd = std::sqrt(dx*dx + dy*dy);
                fade = std::max(0.0f, 1.0f - cd);
            }
            const float edge = 1.0f - 0.6f * std::max(0.0f, std::max(ax, ay) - 0.92f) / 0.08f;
            fade *= std::max(0.0f, edge);
            const size_t i = (size_t)y * n + x;
            uint8_t* p = &px[i * 4];
            p[0] = to8(c.r[i] * fade);
            p[1] = to8(c.g[i] * fade);
            p[2] = to8(c.b[i] * fade);
            p[3] = 255;
        }
    }
    return px;
}

} // namespace holo

// ===========================================================================
// THE SHIPPED CONTENT BAKERS.
// Each one obeys the readability law: line-art to the MARGINS, a QUIET BAND under
// the type, text drawn LAST and HOT. Decoration never prints through the readout.
// ===========================================================================
using namespace holo;

std::vector<uint8_t> bakeFloorSelect(uint32_t n, const std::vector<std::string>& floors,
                                     int sel, const std::string& caption, float aspect) {
    Canvas c(n);
    blackGlassBase(c);
    const float fn = (float)n;
    auto P = [&](float f) { return f * fn; };
    const float xs = (aspect > 0.001f) ? 1.0f / aspect : 1.0f;   // cancel the panel stretch
    const float th = std::max(1.4f, fn / 512.0f);

    // --- Line-art, kept to the MARGINS. ---
    rectFrame(c, P(0.05f), P(0.05f), P(0.95f), P(0.95f), kBlue, 0.55f, th * 1.4f);
    chevronUp(c,   P(0.50f), P(0.115f), P(0.026f), P(0.030f), kBlueHi, 0.85f, th * 1.3f);
    chevronDown(c, P(0.50f), P(0.885f), P(0.026f), P(0.030f), kBlueHi, 0.85f, th * 1.3f);

    // --- The floor list. Each row gets its OWN quiet band, then the label on top. ---
    const size_t count = std::max<size_t>(1, floors.size());
    const float listTop = P(0.16f), listBot = P(0.78f);
    const float rowH = (listBot - listTop) / (float)count;
    float ty = listTop;
    for (size_t i = 0; i < floors.size(); ++i) {
        const bool onSel = ((int)i == sel);
        const float y0 = ty, y1 = ty + rowH * 0.86f;
        quietBand(c, P(0.09f), y0, P(0.91f), y1, 0.10f, P(0.006f));
        const Ink k = onSel ? kGreen : kBlue;        // CURRENT FLOOR = GREEN
        if (onSel) {
            rectFrame(c, P(0.09f), y0, P(0.91f), y1, kGreen, 0.75f, th);
            rectFill(c, P(0.105f), y0 + rowH * 0.28f, P(0.135f), y0 + rowH * 0.56f, kGreen, 1.0f);
        }
        float tpx = std::min(rowH * 0.66f, P(0.10f));
        const float w = textWidth(floors[i], tpx, xs);
        if (w > P(0.72f) && w > 1.0f) tpx *= P(0.72f) / w;
        drawText(c, floors[i], P(0.165f), y0 + rowH * 0.10f, tpx, k, onSel ? 1.0f : 0.85f, xs);
        ty += rowH;
    }

    // --- State caption (IDLE / DOORS OPEN / DESCENDING) on its own quiet strip. ---
    if (!caption.empty()) {
        quietBand(c, P(0.08f), P(0.80f), P(0.92f), P(0.875f), 0.08f, P(0.006f));
        float cpx = P(0.058f);
        const float w = textWidth(caption, cpx, xs);
        if (w > P(0.80f) && w > 1.0f) cpx *= P(0.80f) / w;
        drawText(c, caption, P(0.10f), P(0.805f), cpx, statusInk(caption), 1.0f, xs);
    }
    return finish(c);
}

std::vector<uint8_t> bakeKeypad(uint32_t n, const std::string& entered, bool locked,
                                float aspect) {
    // NOTE: the keypad has REAL, physically-modelled keys. This is the READOUT above
    // them — not a virtual digit grid, which would double the buttons.
    Canvas c(n);
    blackGlassBase(c);
    const float fn = (float)n;
    auto P = [&](float f) { return f * fn; };
    const float xs = (aspect > 0.001f) ? 1.0f / aspect : 1.0f;
    const float th = std::max(1.4f, fn / 512.0f) * 1.5f;   // bolder: a keypad reads SMALL

    rectFrame(c, P(0.04f), P(0.06f), P(0.96f), P(0.94f), kBlue, 0.45f, th);

    // Header.
    quietBand(c, P(0.06f), P(0.10f), P(0.94f), P(0.36f), 0.10f, P(0.010f));
    float hpx = P(0.20f);
    {
        const float w = textWidth("ENTER CODE", hpx, xs);
        if (w > P(0.84f) && w > 1.0f) hpx *= P(0.84f) / w;
    }
    drawText(c, "ENTER CODE", P(0.08f), P(0.12f), hpx, kBlueHi, 1.0f, xs);

    // The entered code, in amber — the line the player is actually reading.
    quietBand(c, P(0.06f), P(0.38f), P(0.94f), P(0.64f), 0.08f, P(0.010f));
    const std::string shown = entered.empty() ? std::string("____") : entered;
    float epx = P(0.22f);
    {
        const float w = textWidth("> " + shown + "_", epx, xs);
        if (w > P(0.84f) && w > 1.0f) epx *= P(0.84f) / w;
    }
    drawText(c, "> " + shown + "_", P(0.08f), P(0.40f), epx, kAmber, 1.0f, xs);

    // Lock status — GREEN granted / ORANGE locked. The whole point of the screen.
    quietBand(c, P(0.06f), P(0.66f), P(0.94f), P(0.90f), 0.08f, P(0.010f));
    const std::string st = locked ? "LOCKED" : "ACCESS GRANTED";
    float spx = P(0.19f);
    {
        const float w = textWidth(st, spx, xs);
        if (w > P(0.84f) && w > 1.0f) spx *= P(0.84f) / w;
    }
    drawText(c, st, P(0.08f), P(0.68f), spx, locked ? kOrange : kGreen, 1.0f, xs);
    return finish(c);
}

std::vector<uint8_t> bakePlacard(uint32_t n, const std::vector<std::string>& lines,
                                 float aspect) {
    Canvas c(n);
    warmBase(c);
    const float fn = (float)n;
    auto P = [&](float f) { return f * fn; };
    const float xs = (aspect > 0.001f) ? 1.0f / aspect : 1.0f;
    const float th = std::max(1.4f, fn / 512.0f);

    rectFrame(c, P(0.06f), P(0.06f), P(0.94f), P(0.94f), kOrange, 0.65f, th * 1.4f);
    line(c, P(0.09f), P(0.245f), P(0.91f), P(0.245f), kOrange, 0.60f, th);

    if (!lines.empty()) {
        quietBand(c, P(0.08f), P(0.085f), P(0.92f), P(0.235f), 0.10f, P(0.012f));
        float tpx = P(0.105f);
        const float tw = textWidth(lines[0], tpx, xs);
        if (tw > P(0.78f) && tw > 1.0f) tpx *= P(0.78f) / tw;
        drawText(c, lines[0], P(0.11f), P(0.095f), tpx, kOrange, 1.0f, xs);
    }
    if (lines.size() > 1) {
        const float bodyTop = P(0.30f), bodyBot = P(0.90f);
        quietBand(c, P(0.08f), bodyTop - P(0.012f), P(0.92f), bodyBot, 0.10f, P(0.012f));
        const size_t rows = lines.size() - 1;
        float bpx = std::min(P(0.085f), (bodyBot - bodyTop) / (rows * 1.35f));
        for (size_t i = 1; i < lines.size(); ++i) {
            const float w = textWidth(lines[i], bpx, xs);
            if (w > P(0.78f) && w > 1.0f) bpx *= P(0.78f) / w;
        }
        float ty = bodyTop;
        for (size_t i = 1; i < lines.size(); ++i) {
            // Placards carry warnings — honour the status read (RESTRICTED => orange).
            const Ink k = statusInk(lines[i]);
            drawText(c, lines[i], P(0.11f), ty, bpx,
                     (k.g > 1.0f && k.r < 1.0f) ? kGreen : kOrange, 0.95f, xs);
            ty += bpx * 1.35f;
        }
    }
    return finish(c);
}

// ===========================================================================
// THE FIXTURE.
// ===========================================================================
namespace {
// A rounded-corner panel quad. DOUBLE-SIDED, and the back fan's U is MIRRORED.
//
// This is load-bearing, not a nicety: panels are mounted at whatever yaw the room
// needs, and the CANON CELL TERMINAL is built at yaw=PI — its LOCAL +Z front face
// points AWAY from the player. The mesh pipeline runs VK_CULL_MODE_NONE, so a
// front-only fan still draws from behind, but you are then looking at the texture
// through its own back, which flips it horizontally: the readout renders BACKWARDS.
// The back fan carries its own -Z normal (so the diffuse N.L stays positive and the
// panel reads correctly lit from the player's side) and a pre-flipped U that cancels
// the mirroring. Result: the panel is legible from EITHER side, at ANY yaw — which is
// exactly what a platform has to guarantee, because the next variant will face the
// other way and nobody will remember this comment.
x3::prims::PrimMesh roundedPanel(float hw, float hh, float corner) {
    x3::prims::PrimMesh m;
    const float r = std::min(corner, std::min(hw, hh) * 0.9f);
    std::vector<float> ring;
    auto pt = [&](float x, float y) { ring.push_back(x); ring.push_back(y); };
    const int seg = 6;
    struct C { float cx, cy, a0; } cs[4] = {
        { hw-r, hh-r, 0.0f }, { -hw+r, hh-r, kPi*0.5f },
        { -hw+r, -hh+r, kPi }, { hw-r, -hh+r, kPi*1.5f }
    };
    for (int k = 0; k < 4; ++k)
        for (int s = 0; s <= seg; ++s) {
            const float a = cs[k].a0 + (kPi * 0.5f) * ((float)s / seg);
            pt(cs[k].cx + std::cos(a) * r, cs[k].cy + std::sin(a) * r);
        }
    const uint32_t rn = (uint32_t)(ring.size() / 2);

    // FRONT fan (+Z normal); UV maps the panel rect to 0..1 (row 0 = top).
    auto push = [&](float x, float y) {
        const float uu = (x + hw) / (2.0f * hw), vv = 1.0f - (y + hh) / (2.0f * hh);
        m.verts.push_back({ {x, y, 0.0f}, {0.0f, 0.0f, 1.0f}, {uu, vv} });
    };
    push(0.0f, 0.0f);
    for (uint32_t i = 0; i < rn; ++i) push(ring[i*2], ring[i*2+1]);
    for (uint32_t i = 0; i < rn; ++i)                       // CCW => faces +Z
        m.index.insert(m.index.end(), { 0u, 1u + i, 1u + ((i + 1) % rn) });

    // BACK fan (-Z normal), U MIRRORED, nudged a hair behind to avoid coplanar z-fight.
    const uint32_t base = (uint32_t)m.verts.size();
    auto pushBack = [&](float x, float y) {
        const float uu = 1.0f - (x + hw) / (2.0f * hw);     // pre-flip: cancels the mirror
        const float vv = 1.0f - (y + hh) / (2.0f * hh);
        m.verts.push_back({ {x, y, -0.001f}, {0.0f, 0.0f, -1.0f}, {uu, vv} });
    };
    pushBack(0.0f, 0.0f);
    for (uint32_t i = 0; i < rn; ++i) pushBack(ring[i*2], ring[i*2+1]);
    for (uint32_t i = 0; i < rn; ++i)                       // reversed winding => faces -Z
        m.index.insert(m.index.end(),
                       { base, base + 1u + ((i + 1) % rn), base + 1u + i });
    return m;
}

x3::prims::PrimMesh cylinderY(float r, float halfH, uint32_t seg = 20) {
    x3::prims::PrimMesh m;
    seg = std::max(6u, seg);
    const float tp = 6.2831853f;
    for (uint32_t j = 0; j <= seg; ++j) {
        const float a = tp * ((float)j / seg), ca = std::cos(a), sa = std::sin(a);
        m.verts.push_back({ {ca*r, -halfH, sa*r}, {ca,0,sa}, {(float)j/seg, 0} });
        m.verts.push_back({ {ca*r,  halfH, sa*r}, {ca,0,sa}, {(float)j/seg, 1} });
    }
    for (uint32_t j = 0; j < seg; ++j) {
        const uint32_t b0 = j*2, t0 = j*2+1, b1 = (j+1)*2, t1 = (j+1)*2+1;
        m.index.insert(m.index.end(), { b0,b1,t0, t0,b1,t1 });
    }
    auto cap = [&](float y, float ny) {
        const uint32_t ctr = (uint32_t)m.verts.size();
        m.verts.push_back({ {0,y,0}, {0,ny,0}, {0.5f,0.5f} });
        const uint32_t st = (uint32_t)m.verts.size();
        for (uint32_t j = 0; j <= seg; ++j) {
            const float a = tp * ((float)j / seg);
            m.verts.push_back({ {std::cos(a)*r, y, std::sin(a)*r}, {0,ny,0}, {0,0} });
        }
        for (uint32_t j = 0; j < seg; ++j) {
            if (ny > 0) m.index.insert(m.index.end(), { ctr, st+j, st+j+1 });
            else        m.index.insert(m.index.end(), { ctr, st+j+1, st+j });
        }
    };
    cap(halfH, 1.0f); cap(-halfH, -1.0f);
    return m;
}
} // namespace

void HoloPanel::build(Scene& scene, x3::rhi::IRenderDevice& device, const HoloPanelParams& p) {
    m_scene = &scene; m_device = &device; m_pos = p.pos;
    m_texN = p.texN; m_shimmer = p.shimmerIntensity;

    const float cs = std::cos(p.yaw), sn = std::sin(p.yaw);
    const float hw = p.width * 0.5f, hh = p.height * 0.5f;
    const float corner = (p.cornerRadius > 0.0f) ? p.cornerRadius : std::min(hw, hh) * 0.30f;

    // ---- ROUND 9: addMetal DID NOT MAKE METAL. --------------------------------
    // Tim: the rifthub consoles are "white plastic tablets on antennas". They were.
    // This lambda set baseColor and NOTHING ELSE — no mrTex — so every frame and
    // mount entity fell to Scene's drawMeshEmissive path: a flat DIELECTRIC, zero
    // metalness, no specular lobe worth the name. A dielectric at albedo 0.66-0.76
    // is not "shiny metallic round pipe"; it is the literal definition of WHITE
    // PLASTIC. The lambda was NAMED addMetal, so for ~10 re-fixes nobody re-checked
    // the one thing the name was asserting. (Same class as KNOWN_BUGS B5 / SM_Door_A:
    // a near-white albedo that the old 0.42 ambient wash was hiding. Honest light
    // exposed it.) VALUE, NOT LUMENS: fix the material, do not add a light.
    //
    // Now: a real glTF MR map + real machined-steel base values. On a ROUND pipe a
    // high metalness + low-ish roughness puts a tight specular streak down the crest
    // — that streak is the ONLY thing that makes a cylinder read as a pipe, and it
    // is exactly what catches the membrane's blue. The diffuse goes dark (metal has
    // almost none), so the frame stops swallowing the black glass it surrounds.
    {
        const uint8_t polishPx[4] = { 0,  66, 235, 255 };   // rough .26, metal .92 — polished steel
        const uint8_t gunPx[4]    = { 0, 107, 217, 255 };   // rough .42, metal .85 — machined gunmetal
        m_mrPolish = device.createTexture(polishPx, 1, 1, false);
        m_mrGun    = device.createTexture(gunPx,    1, 1, false);
    }

    // Opaque METAL helper (frame + mount). HONEST LIGHTING: no emissive on structure.
    auto addMetal = [&](const x3::prims::PrimMesh& g, float ox, float oy, float oz,
                        float cr, float cg, float cb,
                        x3::rhi::TextureHandle mr) {
        Entity e;
        e.mesh = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                   g.index.data(), (uint32_t)g.index.size());
        m_meshes.push_back(e.mesh);
        e.mrTex = mr;   // <- the whole fix: this is what routes it to the PBR metal path
        e.baseColor[0] = cr; e.baseColor[1] = cg; e.baseColor[2] = cb; e.baseColor[3] = 1.0f;
        e.tag = (uint32_t)Tag::Prop;
        e.roomId = p.roomId;
        const float wx = cs*ox + sn*oz, wz = -sn*ox + cs*oz;
        e.transform[0] = cs; e.transform[2] = -sn;
        e.transform[8] = sn; e.transform[10] = cs;
        e.transform[12] = p.pos.x + wx;
        e.transform[13] = p.pos.y + oy;
        e.transform[14] = p.pos.z + wz;
        m_decor.push_back(scene.add(e));
    };

    // ---- (1) THE BLACK-GLASS SCREEN. -----------------------------------------
    if (!p.contentBake) { x3::logError("[holopanel] no contentBake supplied"); return; }
    {
        std::vector<uint8_t> rgba = p.contentBake(m_texN);
        m_screenTex = device.createTexture(rgba.data(), m_texN, m_texN, /*srgb*/true);

        x3::prims::PrimMesh geo = roundedPanel(hw, hh, corner);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        m_meshes.push_back(e.mesh);
        e.tex = m_screenTex;
        e.baseColor[0] = 1.0f; e.baseColor[1] = 1.0f; e.baseColor[2] = 1.0f; e.baseColor[3] = 1.0f;
        e.transparent = true;
        // BLACK GLASS, SHINY, CLEAR TEXT — the canonical recipe:
        //   opacity HIGH  -> a black SLAB, not a blue window (the substrate is the texture).
        //   tint NEUTRAL  -> the ink keeps its authored colour (blue/green/orange).
        //   roughness ~0  -> POLISHED: it catches the room. That is the "shiny".
        //   refraction 0  -> a screen must not bend the scene behind it.
        //   emissiveMap 1 -> THE KEY: glow is modulated PER TEXEL, so the panel lights up
        //                    exactly where the readout is and the black stays black.
        e.glass.opacity     = p.paneOpacity;
        e.glass.tint[0] = 1.0f; e.glass.tint[1] = 1.0f; e.glass.tint[2] = 1.0f;
        e.glass.roughness   = p.paneRoughness;
        e.glass.refraction  = 0.0f;
        e.glass.specular    = p.paneSpecular;
        e.glass.emissiveMap = 1.0f;
        m_emBase[0] = 1.0f; m_emBase[1] = 1.0f; m_emBase[2] = 1.0f;
        m_emBase[3] = p.emissiveStrength;
        for (int k = 0; k < 4; ++k) e.emissive[k] = m_emBase[k];
        e.tag = (uint32_t)Tag::Prop;
        e.roomId = p.roomId;
        e.transform[0] = cs; e.transform[2] = -sn;
        e.transform[8] = sn; e.transform[10] = cs;
        e.transform[12] = p.pos.x; e.transform[13] = p.pos.y; e.transform[14] = p.pos.z;
        m_pane = scene.add(e);
    }
    // ---- NOTHING MAY BE ADDED IN FRONT OF THE PANE (local +Z). Read law #1 in the
    // header before you even think about a scanline overlay or a "protective" pane.

    // ---- (2) FRAME — OPAQUE METAL, coplanar with the glass edge. --------------
    if (p.frame == HoloFrame::Pipe) {
        // The SHINY METALLIC ROUND-PIPE picture frame: an inner polished-steel bead
        // hugging the glass, and a fatter gunmetal collar behind it for real depth.
        // The base values ARE the metals' F0 reflectance (steel ~0.56, chrome ~0.60) —
        // for a metal, baseColor is the specular tint, NOT a coat of paint. Paired with
        // metallic ~0.9 the diffuse is ~nil, so these read as dark, glinting steel that
        // picks the blue out of the room. That is a pipe. The old 0.66-0.76 DIELECTRIC
        // was a bright matte solid: the white tablet.
        x3::prims::PrimMesh innerRim =
            x3::prims::makeRoundedRectTube(hw + 0.010f, hh + 0.010f, corner, 0.016f, 6, 14);
        addMetal(innerRim, 0, 0, 0, 0.62f, 0.65f, 0.70f, m_mrPolish);   // polished bead
        x3::prims::PrimMesh outerRim =
            x3::prims::makeRoundedRectTube(hw + 0.034f, hh + 0.034f, corner + 0.020f, 0.022f, 6, 14);
        addMetal(outerRim, 0, 0, 0, 0.40f, 0.42f, 0.46f, m_mrGun);      // gunmetal collar
    } else if (p.frame == HoloFrame::Bezel) {
        x3::prims::PrimMesh bez =
            x3::prims::makeRoundedRectTube(hw + 0.012f, hh + 0.012f, corner, 0.010f, 6, 10);
        addMetal(bez, 0, 0, 0, 0.10f, 0.11f, 0.13f, m_mrGun);
    }

    // ---- (3) MOUNT — it HANGS, it does not float. -----------------------------
    if (p.mount == HoloMount::CeilingPipe) {
        const float ceilY = (p.ceilingY > 0.0f) ? p.ceilingY : p.pos.y + 1.7f;
        const float top = p.pos.y + hh + 0.030f;
        const float H = ceilY - top;
        if (H > 0.05f) {
            const float sr = 0.034f;   // ROUND 9: 0.026 read as an antenna. A pipe has heft.
            const float mid = (ceilY + top) * 0.5f - p.pos.y;
            // The single support pipe, top-centre, up to the ceiling. DARK gunmetal and
            // NO self-glow: a tall vertical prim catches the point rig side-on, so a
            // bright one reads as a neon rod. Structure holds weight; it does not light.
            // ROUND 9: the support pipe is now real GUNMETAL and slightly fatter
            // (0.026 -> 0.034 m). At 26 mm, matte and pale, it read as a radio ANTENNA
            // stuck in a tablet. A pipe that carries a hanging fixture is a structural
            // member: it wants some heft and a specular crest so the eye reads a tube.
            addMetal(cylinderY(sr, H * 0.5f, 20), 0, mid, 0, 0.38f, 0.40f, 0.44f, m_mrGun);
            addMetal(cylinderY(sr + 0.016f, 0.028f, 20), 0, (top - p.pos.y) + 0.028f, 0,
                     0.58f, 0.61f, 0.66f, m_mrPolish);                       // lower collar
            addMetal(cylinderY(sr + 0.026f, 0.022f, 20), 0, (ceilY - p.pos.y) - 0.022f, 0,
                     0.58f, 0.61f, 0.66f, m_mrPolish);                       // ceiling collar
        }
    } else if (p.mount == HoloMount::WallFlush) {
        // The back-box goes BEHIND the pane (local -Z). In front, it would depth-eat
        // the screen — that is law #1, and it is the whole bug.
        x3::prims::PrimMesh box = x3::prims::makeBox(hw * 0.90f, hh * 0.90f, 0.03f, 0, 0, 0, 1.0f);
        addMetal(box, 0, 0, -0.05f, 0.10f, 0.11f, 0.13f, m_mrGun);
    } else if (p.mount == HoloMount::FreeStand) {
        const float floorY = (p.floorY > 0.0f) ? p.floorY : p.pos.y - hh - 0.6f;
        const float bottom = p.pos.y - hh - 0.03f;
        const float H = bottom - floorY;
        if (H > 0.05f) {
            const float sr = 0.030f;
            const float mid = (bottom + floorY) * 0.5f - p.pos.y;
            addMetal(cylinderY(sr, H * 0.5f, 20), 0, mid, 0, 0.38f, 0.40f, 0.44f, m_mrGun);
            addMetal(cylinderY(sr + 0.020f, 0.026f, 20), 0, (bottom - p.pos.y) - 0.026f, 0,
                     0.58f, 0.61f, 0.66f, m_mrPolish);
            addMetal(cylinderY(0.14f, 0.020f, 28), 0, (floorY - p.pos.y) + 0.020f, 0,
                     0.44f, 0.46f, 0.50f, m_mrGun);                          // base disc
        }
    }
    // HoloMount::None — the caller's housing already carries it (keypad, elevator cab).

    // ---- (4) Glow-light suggestion, in FRONT along the panel normal. ----------
    if (p.glowLight) {
        const float nx = sn, nz = cs;
        m_hasGlow = true;
        m_glowPos[0] = p.pos.x + nx * 0.55f;
        m_glowPos[1] = p.pos.y;
        m_glowPos[2] = p.pos.z + nz * 0.55f;
        m_glowColor[0] = p.glowColor[0];
        m_glowColor[1] = p.glowColor[1];
        m_glowColor[2] = p.glowColor[2];
        m_glowRange = p.glowRange;
    }
}

void HoloPanel::reposition(x3::phys::Vec3 newPos) {
    if (!m_scene || m_pane == kNoLink) return;
    const float dx = newPos.x - m_pos.x, dy = newPos.y - m_pos.y, dz = newPos.z - m_pos.z;
    if (dx == 0.0f && dy == 0.0f && dz == 0.0f) return;
    auto shift = [&](uint32_t id) {
        if (id == kNoLink || id >= m_scene->size()) return;
        Entity& e = m_scene->get(id);
        e.transform[12] += dx; e.transform[13] += dy; e.transform[14] += dz;
    };
    shift(m_pane);
    for (uint32_t id : m_decor) shift(id);
    m_glowPos[0] += dx; m_glowPos[1] += dy; m_glowPos[2] += dz;
    m_pos = newPos;
}

void HoloPanel::setContent(const std::vector<uint8_t>& rgba) {
    if (!m_device || m_pane == kNoLink) return;
    x3::rhi::TextureHandle fresh = m_device->createTexture(rgba.data(), m_texN, m_texN, true);
    if (!fresh.valid()) return;
    x3::rhi::TextureHandle old = m_screenTex;
    m_screenTex = fresh;
    if (m_scene && m_pane < m_scene->size()) m_scene->get(m_pane).tex = m_screenTex;
    if (old.valid()) m_device->destroyTexture(old);
}

void HoloPanel::update(float dt) {
    m_clock += dt;
    if (!m_scene || m_pane == kNoLink || m_pane >= m_scene->size()) return;
    // The shimmer rides the READOUT INK itself (emissiveMap is on): the text breathes.
    // It is NOT a flat sheet of blue laid over the screen — that was the old second pane.
    const float pulse = 0.94f + 0.06f * m_shimmer * std::sin(m_clock * 1.7f);
    const float flick = 0.985f + 0.015f * std::sin(m_clock * 13.0f);
    Entity& e = m_scene->get(m_pane);
    e.emissive[0] = m_emBase[0]; e.emissive[1] = m_emBase[1]; e.emissive[2] = m_emBase[2];
    e.emissive[3] = m_emBase[3] * pulse * flick;
}

bool HoloPanel::screenHasContent() const {
    if (!m_scene || m_pane == kNoLink || m_pane >= m_scene->size()) return false;
    if (!m_screenTex.valid()) return false;
    const Entity& e = m_scene->get(m_pane);
    if (!e.tex.valid() || e.tex.id != m_screenTex.id) return false;   // actually BOUND
    return true;
}

void HoloPanel::shutdown(x3::rhi::IRenderDevice& device) {
    for (auto h : m_meshes) if (h.valid()) device.destroyMesh(h);
    m_meshes.clear();
    if (m_screenTex.valid()) { device.destroyTexture(m_screenTex); m_screenTex = {}; }
    // The frame/mount MR texels (round 9). The rifthub stands EIGHT of these and its
    // smoketest gates on allocationCount == 0 — a leak here fails the gate.
    if (m_mrPolish.valid()) { device.destroyTexture(m_mrPolish); m_mrPolish = {}; }
    if (m_mrGun.valid())    { device.destroyTexture(m_mrGun);    m_mrGun    = {}; }
    m_decor.clear();
    m_pane = kNoLink;
    m_scene = nullptr;
    m_device = nullptr;
}

} // namespace x3::game
