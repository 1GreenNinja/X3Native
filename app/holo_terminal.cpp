// Holographic terminal — see app/holo_terminal.h.
//
// Clean-room: Scene/Entity + IRenderDevice + mesh_prims only. The screen is a thin
// translucent (baseColor alpha) emissive (HDR glow -> bloom) quad; the text is
// drawn by the host over it (worldToScreen + drawHudText).
#include "holo_terminal.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"
#include "engine/rhi/font_robotomono.h"   // kRobotoMonoTTF — baked into the on-glass text

// stb_truetype: DECLARATIONS ONLY here. The implementation
// (STB_TRUETYPE_IMPLEMENTATION) is compiled once inside the engine TU
// (VulkanRenderDevice.cpp); these extern symbols link against it. We use it on the
// CPU to rasterize the readout strings directly into the hologram's RGBA pixels so
// the text lives ON the glass surface (tilts in 3D with the panel), exactly like a
// Babylon DynamicTexture — instead of a flat worldToScreen overlay that swings to
// face the camera.
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {
// ---------------------------------------------------------------------------
// PROCEDURAL HOLOGRAM UI TEXTURE (RGBA8). The screen is drawn as a strongly
// EMISSIVE quad whose COLOR comes from this texture, so the panel reads as a
// projected sci-fi SECURITY CONSOLE — a crisp cyan/white LINE-ART HUD printed
// on a dark translucent-blue glass plate (matching the reference photos), not a
// flat MS-Paint slab. Layout (drawn procedurally into the buffer below):
//   * a deep-blue glass BASE with a top->bottom backlit gradient, fine cyan
//     SCANLINES + a faint data-grid + soft vignette UNDER the line-art (so it
//     still reads as a shimmering hologram),
//   * a TOP HEADER bar (rule + hexagon emblem; the live title text is rendered
//     ON-GLASS by the host's drawHoloReadout over a clear header strip),
//   * a BRACKET-CORNERED frame around the whole UI with one chamfered corner,
//   * a LEFT column of small icon squares + rows of fine "data text" tick-marks,
//   * a CENTER schematic — a rounded-rect node with branching lines + labels and
//     a downward chevron below it,
//   * a RIGHT column with three warning-triangle icons, 2-3 label+value-bar data
//     fields, and a solid bright indicator square,
//   * a BOTTOM dotted/coded data strip across the width.
// The main mesh pass is opaque, so translucency is FAKED by darkening the texture
// (dark base + bright emissive line-art glows over the dark scene = glowing glass).
// Tileable is irrelevant (one quad, UV 0..1). Output srgb=true (color texture).
// ---------------------------------------------------------------------------

// Float RGB framebuffer the line-art is composited into. Lines ADD light (so the
// cyan strokes bloom over the dark glass). One channel triple per pixel.
struct Canvas {
    uint32_t n;
    std::vector<float> r, g, b;
    explicit Canvas(uint32_t nn) : n(nn), r((size_t)nn*nn,0), g((size_t)nn*nn,0), b((size_t)nn*nn,0) {}
    inline void add(int x, int y, float rr, float gg, float bb, float a) {
        if (x < 0 || y < 0 || x >= (int)n || y >= (int)n) return;
        const size_t i = (size_t)y * n + x;
        r[i] += rr * a; g[i] += gg * a; b[i] += bb * a;
    }
};

// Additive 1px-soft point (a 2x2 footprint with sub-pixel coverage) so diagonal /
// thin strokes don't alias into dotted lines — cheap anti-aliasing for the HUD.
inline void plot(Canvas& c, float fx, float fy, float r, float g, float b, float a) {
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    c.add(x0,   y0,   r,g,b, a*(1-tx)*(1-ty));
    c.add(x0+1, y0,   r,g,b, a*(tx)*(1-ty));
    c.add(x0,   y0+1, r,g,b, a*(1-tx)*(ty));
    c.add(x0+1, y0+1, r,g,b, a*(tx)*(ty));
}

// Anti-aliased line of given thickness (in px) from (x0,y0) to (x1,y1).
inline void line(Canvas& c, float x0, float y0, float x1, float y1,
                 float r, float g, float b, float a, float thick = 1.6f) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx*dx + dy*dy);
    if (len < 0.001f) { plot(c, x0, y0, r,g,b, a); return; }
    const float nx = dx / len, ny = dy / len;       // step direction
    const float px = -ny, py = nx;                   // perpendicular (for thickness)
    const int steps = (int)(len + 1.0f);
    const int half = (int)std::ceil(thick * 0.5f);
    for (int s = 0; s <= steps; ++s) {
        const float cxp = x0 + nx * s, cyp = y0 + ny * s;
        for (int t = -half; t <= half; ++t) {
            const float d = std::fabs((float)t);
            const float cov = 1.0f - std::max(0.0f, (d - thick*0.5f + 0.5f)); // soft edge
            if (cov <= 0.0f) continue;
            plot(c, cxp + px * t, cyp + py * t, r,g,b, a * (cov > 1 ? 1 : cov));
        }
    }
}

// Axis-aligned rectangle OUTLINE (thin stroke).
inline void rectFrame(Canvas& c, float x0, float y0, float x1, float y1,
                      float r, float g, float b, float a, float thick = 1.6f) {
    line(c, x0,y0, x1,y0, r,g,b,a,thick);
    line(c, x1,y0, x1,y1, r,g,b,a,thick);
    line(c, x1,y1, x0,y1, r,g,b,a,thick);
    line(c, x0,y1, x0,y0, r,g,b,a,thick);
}

// FILLED rectangle (solid additive block — value bars / indicators).
inline void rectFill(Canvas& c, float x0, float y0, float x1, float y1,
                     float r, float g, float b, float a) {
    const int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
    for (int y = iy0; y <= iy1; ++y)
        for (int x = ix0; x <= ix1; ++x)
            c.add(x, y, r,g,b, a);
}

// Rounded-rect OUTLINE (a "node" box) — straight edges + quarter-arc corners.
inline void roundRectFrame(Canvas& c, float x0, float y0, float x1, float y1, float rad,
                           float r, float g, float b, float a, float thick = 1.6f) {
    line(c, x0+rad,y0, x1-rad,y0, r,g,b,a,thick);   // top
    line(c, x0+rad,y1, x1-rad,y1, r,g,b,a,thick);   // bottom
    line(c, x0,y0+rad, x0,y1-rad, r,g,b,a,thick);   // left
    line(c, x1,y0+rad, x1,y1-rad, r,g,b,a,thick);   // right
    struct Cn { float cx, cy, a0; } cn[4] = {
        { x1-rad, y0+rad, -1.57079633f }, { x1-rad, y1-rad, 0.0f },
        { x0+rad, y1-rad, 1.57079633f }, { x0+rad, y0+rad, 3.14159265f },
    };
    for (auto& k : cn) {
        const int seg = 8;
        float pxp = k.cx + std::cos(k.a0) * rad, pyp = k.cy + std::sin(k.a0) * rad;
        for (int s = 1; s <= seg; ++s) {
            const float aa = k.a0 + 1.57079633f * ((float)s/seg);
            const float nxp = k.cx + std::cos(aa) * rad, nyp = k.cy + std::sin(aa) * rad;
            line(c, pxp,pyp, nxp,nyp, r,g,b,a,thick);
            pxp = nxp; pyp = nyp;
        }
    }
}

// Bracket-corner HUD frame: instead of a full rectangle, draw right-angle corner
// brackets (L-shapes) at each corner + short edge ticks. One corner (top-right) is
// CHAMFERED (clipped at 45 deg) for the sci-fi "cut corner" look in the reference.
inline void bracketFrame(Canvas& c, float x0, float y0, float x1, float y1,
                         float arm, float chamf, float r, float g, float b, float a, float thick) {
    // top-left
    line(c, x0,y0, x0+arm,y0, r,g,b,a,thick);
    line(c, x0,y0, x0,y0+arm, r,g,b,a,thick);
    // bottom-left
    line(c, x0,y1, x0+arm,y1, r,g,b,a,thick);
    line(c, x0,y1, x0,y1-arm, r,g,b,a,thick);
    // bottom-right
    line(c, x1,y1, x1-arm,y1, r,g,b,a,thick);
    line(c, x1,y1, x1,y1-arm, r,g,b,a,thick);
    // top-right CHAMFERED corner: the two arms stop short of the corner and a 45-deg
    // cut joins them (the clipped/beveled corner from the reference).
    line(c, x1-chamf,y0, x1-arm-chamf,y0, r,g,b,a,thick);  // short top arm
    line(c, x1,y0+chamf, x1,y0+arm+chamf, r,g,b,a,thick);  // short right arm
    line(c, x1-chamf,y0, x1,y0+chamf, r,g,b,a,thick);      // the 45-deg chamfer cut
    // faint connecting edges so the frame reads as a continuous border (dim).
    line(c, x0+arm,y0, x1-arm-chamf,y0, r,g,b,a*0.30f,thick*0.7f); // top
    line(c, x0+arm,y1, x1-arm,y1, r,g,b,a*0.30f,thick*0.7f);       // bottom
    line(c, x0,y0+arm, x0,y1-arm, r,g,b,a*0.30f,thick*0.7f);       // left
    line(c, x1,y0+arm+chamf, x1,y1-arm, r,g,b,a*0.30f,thick*0.7f); // right
}

// Hexagon OUTLINE (the small header emblem).
inline void hexagon(Canvas& c, float cx, float cy, float rad,
                    float r, float g, float b, float a, float thick) {
    float pxp = 0, pyp = 0;
    for (int s = 0; s <= 6; ++s) {
        const float aa = 0.5235988f + 1.04719755f * s;   // 30deg start, 60deg steps
        const float nxp = cx + std::cos(aa) * rad, nyp = cy + std::sin(aa) * rad;
        if (s > 0) line(c, pxp,pyp, nxp,nyp, r,g,b,a,thick);
        pxp = nxp; pyp = nyp;
    }
}

// Warning triangle (△ with a "!" inside) — outline + a vertical bar + a dot.
inline void warnTriangle(Canvas& c, float cx, float topY, float halfW, float h,
                        float r, float g, float b, float a, float thick) {
    const float apexX = cx, apexY = topY;
    const float blX = cx - halfW, brX = cx + halfW, baseY = topY + h;
    line(c, apexX,apexY, blX,baseY, r,g,b,a,thick);
    line(c, apexX,apexY, brX,baseY, r,g,b,a,thick);
    line(c, blX,baseY, brX,baseY, r,g,b,a,thick);
    // exclamation bar + dot
    const float exTop = apexY + h*0.34f, exBot = apexY + h*0.66f;
    line(c, cx, exTop, cx, exBot, r,g,b,a, thick);
    rectFill(c, cx-thick*0.5f, apexY + h*0.76f, cx+thick*0.5f, apexY + h*0.82f, r,g,b,a);
}

// Downward chevron (▽ open arrow).
inline void chevronDown(Canvas& c, float cx, float topY, float halfW, float h,
                       float r, float g, float b, float a, float thick) {
    line(c, cx-halfW, topY, cx, topY+h, r,g,b,a,thick);
    line(c, cx+halfW, topY, cx, topY+h, r,g,b,a,thick);
}

// ---------------------------------------------------------------------------
// ON-GLASS TEXT — stb_truetype CPU rasterizer (the Babylon DynamicTexture move).
// A shared font handle (Roboto Mono, embedded) initialized lazily, and a routine
// that ADDS each glyph's coverage as glowing cyan/white ink into the Canvas at a
// given top-left pixel pos + pixel height. Because the text is baked into the
// hologram texture, it sits on the glass plate and tilts in 3D with the panel.
// ---------------------------------------------------------------------------
struct GlassFont {
    stbtt_fontinfo info{};
    bool ready = false;
    GlassFont() {
        const unsigned char* ttf = x3::rhi::kRobotoMonoTTF;
        const int off = stbtt_GetFontOffsetForIndex(ttf, 0);
        if (off >= 0 && stbtt_InitFont(&info, ttf, off)) ready = true;
    }
};
const GlassFont& glassFont() { static GlassFont f; return f; }

// Pixel advance width of `s` at pixel height `px` (for fitting / centering).
float textWidthPx(const std::string& s, float px) {
    const GlassFont& f = glassFont();
    if (!f.ready) return px * 0.6f * (float)s.size();   // mono fallback estimate
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    float w = 0.0f;
    for (size_t i = 0; i < s.size(); ++i) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, (unsigned char)s[i], &adv, &lsb);
        w += adv * scale;
        if (i + 1 < s.size())
            w += stbtt_GetCodepointKernAdvance(&f.info, (unsigned char)s[i], (unsigned char)s[i+1]) * scale;
    }
    return w;
}

// Rasterize `s` at pixel height `px` with its top-left at (penX, topY). Each glyph's
// 8-bit coverage is ADDED as glowing ink (so it blooms over the dark glass like the
// rest of the line-art). Returns the advance width in pixels.
float drawTextGlass(Canvas& c, const std::string& s, float penX, float topY, float px,
                    float r, float g, float b, float a) {
    const GlassFont& f = glassFont();
    if (!f.ready) return 0.0f;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    int asc = 0, desc = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&f.info, &asc, &desc, &lineGap);
    const float baseline = topY + asc * scale;          // cell top -> baseline
    float pen = penX;
    for (size_t i = 0; i < s.size(); ++i) {
        const int ch = (unsigned char)s[i];
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, ch, &adv, &lsb);
        if (ch != ' ') {
            int gw = 0, gh = 0, gxo = 0, gyo = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(&f.info, scale, scale, ch, &gw, &gh, &gxo, &gyo);
            if (bmp) {
                const float gx0 = pen + lsb * scale;     // glyph left edge on the pen line
                for (int yy = 0; yy < gh; ++yy) {
                    for (int xx = 0; xx < gw; ++xx) {
                        const float cov = bmp[yy * gw + xx] / 255.0f;
                        if (cov <= 0.003f) continue;
                        c.add((int)std::lround(gx0 + gxo + xx),
                              (int)std::lround(baseline + gyo + yy),
                              r, g, b, a * cov);
                    }
                }
                stbtt_FreeBitmap(bmp, nullptr);
            }
        }
        pen += adv * scale;
        if (i + 1 < s.size())
            pen += stbtt_GetCodepointKernAdvance(&f.info, ch, (unsigned char)s[i+1]) * scale;
    }
    return pen - penX;
}

// `lines` = the readout (line 0 is the header title, 1+ are the left-column data
// rows). `inputLine` (if non-empty) is the live "> code_" prompt, baked in amber
// below the data rows. Passing them in lets the text be rasterized ON the glass.
std::vector<uint8_t> makeHologramRGBA(uint32_t n,
                                      const std::vector<std::string>& lines,
                                      const std::string& inputLine,
                                      const float* inkOverride = nullptr) {
    const float fn = (float)n;

    // ---- 1) GLASS BASE (under the line-art): deep-blue gradient + scanlines +
    // grid + vignette, EXACTLY as before so it still reads as a hologram. ----
    Canvas c(n);
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / fn, v = (y + 0.5f) / fn;
            float br = 0.05f, bg = 0.16f, bb = 0.34f;
            const float grad = 1.0f - v * 0.55f;            // brighter toward the top
            br *= grad; bg *= grad; bb *= grad;
            const float scan = 0.78f + 0.22f * ((y % 4u) < 2u ? 1.0f : 0.55f);
            br *= scan; bg *= scan; bb *= scan;
            if ((x % 28u) < 1u || (y % 28u) < 1u) { bg += 0.10f; bb += 0.16f; } // data-grid
            const float cx = (u - 0.5f), cy = (v - 0.5f);
            const float rad = std::sqrt(cx*cx + cy*cy) * 1.42f;
            const float vig = 1.0f - 0.45f * rad * rad;
            br *= vig; bg *= vig; bb *= vig;
            const size_t i = (size_t)y * n + x;
            c.r[i] = br; c.g[i] = bg; c.b[i] = bb;
        }
    }

    // ---- 2) LINE-ART HUD (additive cyan/white). All coordinates in pixels, scaled
    // off n so it stays crisp at 1024^2. Layout follows the reference photos.
    // Strokes are pushed HOT (cyan well above the glass base) so the printed HUD
    // survives the flat additive emissive flood + reads under scene lighting. ----
    auto P = [&](float f){ return f * fn; };                // fraction -> pixels
    const float CY_R = 0.55f, CY_G = 1.30f, CY_B = 1.45f;   // hot cyan stroke (HDR-ish)
    const float WT_R = 1.30f, WT_G = 1.55f, WT_B = 1.65f;   // hot cyan-white
    const float th  = std::max(1.4f, fn / 512.0f);          // base stroke thickness
    const float thh = th * 1.5f;                            // heavier strokes

    // Whole-UI bracket frame with a chamfered top-right corner.
    bracketFrame(c, P(0.045f), P(0.05f), P(0.955f), P(0.95f),
                 P(0.075f), P(0.055f), CY_R,CY_G,CY_B, 0.95f, thh);

    // --- TOP HEADER: rule + hexagon emblem at the left end. The TITLE TEXT itself is
    // rendered on-glass by the host (drawHoloReadout) over this clear strip. ---
    const float hdrY = P(0.135f);
    hexagon(c, P(0.085f), P(0.092f), P(0.030f), WT_R,WT_G,WT_B, 1.0f, thh);
    line(c, P(0.085f)-P(0.012f), P(0.092f), P(0.085f)+P(0.012f), P(0.092f), WT_R,WT_G,WT_B,0.9f, th); // emblem tick
    line(c, P(0.130f), hdrY, P(0.92f), hdrY, WT_R,WT_G,WT_B, 1.0f, thh);   // header rule
    line(c, P(0.130f), hdrY+P(0.010f), P(0.55f), hdrY+P(0.010f), CY_R,CY_G,CY_B, 0.5f, th); // sub-rule

    // --- LEFT COLUMN: icon squares + rows of fine "data text" tick blocks. ---
    const float lx0 = P(0.075f), lx1 = P(0.345f);
    // three little icon squares
    for (int i = 0; i < 3; ++i) {
        const float ix = lx0 + i * P(0.055f);
        rectFrame(c, ix, P(0.20f), ix + P(0.035f), P(0.235f), CY_R,CY_G,CY_B, 0.9f, th);
        if (i == 1) line(c, ix, P(0.20f), ix+P(0.035f), P(0.235f), CY_R,CY_G,CY_B,0.7f,th); // diag detail
    }
    // paragraphs of short tick-marks (read as fine data text from a distance)
    {
        float ty = P(0.275f);
        const float rowH = P(0.026f);
        for (int row = 0; row < 14; ++row) {
            // each row is a run of short blocks of varied length (like words)
            float cxr = lx0;
            const int words = 3 + (row * 7 + 2) % 4;
            for (int w = 0; w < words; ++w) {
                const float wlen = P(0.018f) + P(0.010f) * ((row*5 + w*3) % 4);
                if (cxr + wlen > lx1) break;
                const float bright = (row % 5 == 0 && w == 0) ? 0.85f : 0.45f; // first word of some rows brighter
                line(c, cxr, ty, cxr + wlen, ty, CY_R,CY_G,CY_B, bright, th);
                cxr += wlen + P(0.012f);
            }
            ty += rowH;
            if (ty > P(0.86f)) break;
        }
    }
    // a faint vertical divider after the left column
    line(c, lx1 + P(0.015f), P(0.19f), lx1 + P(0.015f), P(0.87f), CY_R,CY_G,CY_B, 0.25f, th);

    // --- CENTER SCHEMATIC: a rounded-rect node + branching horizontal lines with
    // tiny end-nodes + labels, and a downward chevron below it. Kept LEFT of the
    // right column (branches stop before x=0.66) so the two zones don't collide. ---
    const float ncx = P(0.500f), ncy = P(0.41f);
    const float nhw = P(0.072f), nhh = P(0.058f);
    roundRectFrame(c, ncx-nhw, ncy-nhh, ncx+nhw, ncy+nhh, P(0.020f), WT_R,WT_G,WT_B, 0.95f, thh);
    // a couple of inner detail lines (the node "label rows")
    line(c, ncx-nhw+P(0.012f), ncy-P(0.018f), ncx+nhw-P(0.012f), ncy-P(0.018f), CY_R,CY_G,CY_B,0.6f,th);
    line(c, ncx-nhw+P(0.012f), ncy+P(0.010f), ncx+nhw-P(0.025f), ncy+P(0.010f), CY_R,CY_G,CY_B,0.5f,th);
    // three branching lines off the right side with small node squares + label ticks
    const float bx0 = ncx + nhw;
    for (int i = 0; i < 3; ++i) {
        const float by = ncy - P(0.032f) + i * P(0.032f);
        const float bxEnd = P(0.625f);
        line(c, bx0, ncy, bx0 + P(0.016f), by, CY_R,CY_G,CY_B, 0.7f, th);  // angled spur
        line(c, bx0 + P(0.016f), by, bxEnd, by, CY_R,CY_G,CY_B, 0.7f, th); // horizontal run
        rectFrame(c, bxEnd, by-P(0.009f), bxEnd+P(0.018f), by+P(0.009f), CY_R,CY_G,CY_B,0.8f,th); // end node
    }
    // a branch DOWN to the chevron
    line(c, ncx, ncy+nhh, ncx, ncy+nhh+P(0.030f), CY_R,CY_G,CY_B, 0.7f, th);
    chevronDown(c, ncx, ncy+nhh+P(0.030f), P(0.028f), P(0.026f), WT_R,WT_G,WT_B, 0.95f, thh);

    // --- RIGHT COLUMN: three warning triangles, data fields (label + value bar),
    // and a solid indicator square. ---
    const float rx0 = P(0.70f), rx1 = P(0.92f);
    for (int i = 0; i < 3; ++i) {
        const float tx = rx0 + i * P(0.075f);
        warnTriangle(c, tx + P(0.030f), P(0.205f), P(0.030f), P(0.052f), WT_R,WT_G,WT_B, 0.95f, th);
    }
    // 3 data fields: a label tick + a bright value bar
    {
        float fy = P(0.33f);
        const float fieldH = P(0.06f);
        const float barLens[3] = { 0.62f, 0.40f, 0.85f };
        for (int i = 0; i < 3; ++i) {
            line(c, rx0, fy, rx0 + P(0.06f), fy, CY_R,CY_G,CY_B, 0.6f, th);          // label tick
            // bar track (dim) + bright fill
            const float barY = fy + P(0.014f);
            rectFrame(c, rx0, barY, rx1, barY + P(0.018f), CY_R,CY_G,CY_B, 0.4f, th);
            const float fillX = rx0 + (rx1 - rx0) * barLens[i];
            rectFill(c, rx0+th, barY+th, fillX, barY + P(0.018f) - th, WT_R,WT_G,WT_B, 0.85f);
            fy += fieldH;
        }
        // solid bright indicator square at the bottom of the right column
        rectFill(c, rx1 - P(0.030f), fy + P(0.005f), rx1, fy + P(0.035f), WT_R,WT_G,WT_B, 1.0f);
        line(c, rx0, fy + P(0.020f), rx1 - P(0.045f), fy + P(0.020f), CY_R,CY_G,CY_B, 0.5f, th);
    }

    // --- BOTTOM dotted/coded data strip across the width. ---
    {
        const float by = P(0.875f);
        line(c, P(0.075f), by - P(0.014f), P(0.92f), by - P(0.014f), CY_R,CY_G,CY_B, 0.6f, th);
        float dx = P(0.075f);
        int k = 0;
        while (dx < P(0.92f)) {
            const float dlen = (k % 3 == 0) ? P(0.020f) : ((k % 3 == 1) ? P(0.008f) : P(0.013f));
            const float bright = (k % 4 == 0) ? 0.95f : 0.55f;
            line(c, dx, by, dx + dlen, by, CY_R,CY_G,CY_B, bright, thh);
            dx += dlen + P(0.009f);
            ++k;
        }
    }

    // ---- 2b) ON-GLASS READOUT TEXT (rasterized with stb_truetype). This is the
    // text that used to be a worldToScreen overlay; baking it into the texture makes
    // it sit ON the glass + tilt with the panel. Positions mirror the old overlay:
    //   * line 0 = HEADER TITLE, wide + bright across the top header strip,
    //   * lines 1+ = LEFT-column data rows (clipped to the left ~52% so the center
    //     schematic + right column line-art stay readable),
    //   * inputLine = amber "> code_" prompt below the last data row.
    if (glassFont().ready && !lines.empty()) {
        const float HOT = 1.0f;                       // ink alpha (additive glow ink)
        // --- HEADER TITLE: fit to the header strip width, bright cyan-white. ---
        {
            const float titleBudget = P(0.86f - 0.13f);   // header rule span ~ x[0.13..0.92]
            float tpx = P(0.052f);                          // start height (~53 px @1024)
            const float tw = textWidthPx(lines[0], tpx);
            if (tw > P(0.79f) && tw > 1.0f) tpx *= P(0.79f) / tw;   // shrink to span
            (void)titleBudget;
            drawTextGlass(c, lines[0], P(0.130f), P(0.060f), tpx, WT_R, WT_G, WT_B, HOT);
        }
        // --- BODY rows (1+) down the left column. Shrink to the left zone width. ---
        const float lx0b = P(0.075f);
        const float zoneW = P(0.345f) - lx0b;          // left data-column width
        float bpx = P(0.033f);                          // ~34 px @1024
        for (size_t li = 1; li < lines.size(); ++li) {
            const float w = textWidthPx(lines[li], bpx);
            if (w > zoneW && w > 1.0f) bpx *= zoneW / w;
        }
        if (!inputLine.empty()) {
            // The input renders at bpx*1.18 below — keep IT inside the zone too
            // (freeform questions are much longer than a 4-digit code).
            const float w = textWidthPx(inputLine, bpx * 1.18f);
            if (w > zoneW && w > 1.0f) bpx *= zoneW / w;
        }
        if (bpx < P(0.018f)) bpx = P(0.018f);
        const float rowH = bpx * 1.30f;
        float ty = P(0.205f);                           // below the header strip
        for (size_t li = 1; li < lines.size(); ++li) {
            // first data row a touch brighter (the "SUBJECT" line), rest steady cyan.
            // W4-2: when the host set an ink override (VIGIL presence = orange), body
            // rows bake with m_textColor; the default path is byte-identical to before.
            const float aRow = (li == 1) ? HOT : 0.92f;
            const float inkR = inkOverride ? inkOverride[0] * 1.15f : CY_R * 1.15f;
            const float inkG = inkOverride ? inkOverride[1]          : CY_G;
            const float inkB = inkOverride ? inkOverride[2]          : CY_B;
            drawTextGlass(c, lines[li], lx0b, ty, bpx, inkR, inkG, inkB, aRow);
            ty += rowH;
        }
        // --- LIVE INPUT LINE in amber, just below the last data row. ---
        if (!inputLine.empty()) {
            const float ipx = bpx * 1.18f;
            drawTextGlass(c, inputLine, lx0b, ty + rowH * 0.25f, ipx,
                          1.30f, 0.78f, 0.22f, HOT);    // hot amber prompt
        }
    }

    // ---- 3) ROUNDED-CORNER + edge FADE applied to the composited result, so the
    // silhouette reads as a rounded translucent panel (corners die toward black). ----
    const float r2corner = 0.30f;
    std::vector<uint8_t> px((size_t)n * n * 4);
    auto to8 = [](float c0) -> uint8_t {
        int v = (int)(c0 * 255.0f + 0.5f);
        return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    };
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / fn, v = (y + 0.5f) / fn;
            const float cx = (u - 0.5f), cy = (v - 0.5f);
            const float ax = std::fabs(cx) * 2.0f, ay = std::fabs(cy) * 2.0f;
            float fade = 1.0f;
            const float inx = 1.0f - r2corner, iny = 1.0f - r2corner;
            if (ax > inx && ay > iny) {
                const float dx = (ax - inx) / r2corner, dy = (ay - iny) / r2corner;
                const float cd = std::sqrt(dx*dx + dy*dy);
                fade = 1.0f - cd; if (fade < 0.0f) fade = 0.0f;
            }
            const float edge = 1.0f - 0.6f * std::max(0.0f, std::max(ax, ay) - 0.92f) / 0.08f;
            fade *= (edge < 0.0f ? 0.0f : edge);
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

// A FLAT panel quad (front face only, +Z normal) with cut/rounded corners: a
// center-fan over a ring of perimeter points that follow rounded corners, so the
// SILHOUETTE reads as a rounded rectangle (not a hard MS-Paint box). UVs map the
// panel rect to 0..1 so the hologram texture lands square on it. Half-extents hw,hh.
x3::prims::PrimMesh makeRoundedPanel(float hw, float hh, float corner) {
    x3::prims::PrimMesh m;
    const float r = std::min(corner, std::min(hw, hh) * 0.9f);
    // Perimeter points (CCW), rounding each corner with a small arc.
    std::vector<float> ring;  // x,y pairs
    auto pt = [&](float x, float y){ ring.push_back(x); ring.push_back(y); };
    const int seg = 4;        // arc segments per corner
    // corner centers
    struct C { float cx, cy, a0; } corners[4] = {
        {  hw - r,  hh - r, 0.0f             },   // top-right
        { -hw + r,  hh - r, 3.14159265f*0.5f },   // top-left
        { -hw + r, -hh + r, 3.14159265f      },   // bottom-left
        {  hw - r, -hh + r, 3.14159265f*1.5f },   // bottom-right
    };
    for (int c = 0; c < 4; ++c) {
        for (int s = 0; s <= seg; ++s) {
            const float a = corners[c].a0 + (3.14159265f * 0.5f) * ((float)s / (float)seg);
            pt(corners[c].cx + std::cos(a) * r, corners[c].cy + std::sin(a) * r);
        }
    }
    const uint32_t rn = (uint32_t)(ring.size() / 2);
    auto push = [&](float x, float y){
        // UV: panel rect [-hw,hw]x[-hh,hh] -> [0,1]; v flips so row 0 = top.
        const float uu = (x + hw) / (2.0f * hw);
        const float vv = 1.0f - (y + hh) / (2.0f * hh);
        m.verts.push_back({{x, y, 0.0f}, {0.0f, 0.0f, 1.0f}, {uu, vv}});
    };
    push(0.0f, 0.0f);                 // center vertex (index 0)
    for (uint32_t i = 0; i < rn; ++i) push(ring[i*2], ring[i*2+1]);
    for (uint32_t i = 0; i < rn; ++i) {
        const uint32_t a = 1 + i, b = 1 + ((i + 1) % rn);
        // CCW so it faces +Z (matches VK_FRONT_FACE_COUNTER_CLOCKWISE).
        m.index.push_back(0); m.index.push_back(a); m.index.push_back(b);
    }
    // DOUBLE-SIDED: the terminal is mounted with yaw that points the +Z front face
    // away from the player. The mesh pipeline uses VK_CULL_MODE_NONE so a single fan
    // would still draw, but a back fan with its OWN -Z normal makes the lit term read
    // correctly (the diffuse N.L stays positive) from the player side too. Same UVs so
    // the line-art HUD reads upright no matter which side faces the viewer. ----
    const uint32_t base = (uint32_t)m.verts.size();
    const float backZ = -0.001f;     // a hair behind in LOCAL -Z (avoids coplanar z-fight)
    auto pushBack = [&](float x, float y){
        // U is MIRRORED for the back fan: this fan's visible side is the -Z LOCAL
        // face, which after the terminal's yaw faces the player. Viewing the same
        // texture from behind flips it horizontally, so pre-flip U here to cancel
        // that out — otherwise the readout text renders backwards (right-to-left).
        const float uu = 1.0f - (x + hw) / (2.0f * hw);
        const float vv = 1.0f - (y + hh) / (2.0f * hh);
        m.verts.push_back({{x, y, backZ}, {0.0f, 0.0f, -1.0f}, {uu, vv}});
    };
    pushBack(0.0f, 0.0f);             // back center (index base)
    for (uint32_t i = 0; i < rn; ++i) pushBack(ring[i*2], ring[i*2+1]);
    for (uint32_t i = 0; i < rn; ++i) {
        const uint32_t a = base + 1 + i, b = base + 1 + ((i + 1) % rn);
        // Reversed winding (b,a) so this fan's visible side is the -Z LOCAL face,
        // which after the terminal's yaw rotation faces the player.
        m.index.push_back(base); m.index.push_back(b); m.index.push_back(a);
    }
    return m;
}

// (The old FLAT makeRoundedRim ribbon — a quad strip between two coplanar contours,
// which read as a SQUARE-section frame — was replaced by the ROUND-section pipe
// x3::prims::makeRoundedRectTube, swept along the same rounded-rect path. See build().)
} // namespace

void HoloTerminal::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::Vec3 pos, float yaw, float width, float height,
                         float ceilingY) {
    m_pos = pos; m_width = width; m_height = height;
    m_scene = &scene;
    m_device = &device;
    const float cs = std::cos(yaw), sn = std::sin(yaw);

    // Place a box child: half-extents (hx,hy,hz) at a LOCAL offset (ox,oy,oz) from
    // pos, yaw-rotated into world; translucent `alpha`, emissive {r,g,b,strength}.
    auto addBox = [&](float hx, float hy, float hz, float ox, float oy, float oz,
                      float r, float g, float b, float alpha, float er, float eg, float eb, float es) {
        x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0]=r; e.baseColor[1]=g; e.baseColor[2]=b; e.baseColor[3]=alpha;
        e.emissive[0]=er; e.emissive[1]=eg; e.emissive[2]=eb; e.emissive[3]=es;
        e.tag = (uint32_t)Tag::Prop;
        // world offset = R_y(yaw) * (ox,oy,oz)
        const float wx = cs*ox + sn*oz, wz = -sn*ox + cs*oz;
        e.transform[0]=cs; e.transform[2]=-sn; e.transform[8]=sn; e.transform[10]=cs;
        e.transform[12]=pos.x+wx; e.transform[13]=pos.y+oy; e.transform[14]=pos.z+wz;
        return scene.add(e);
    };

    // Place a custom-mesh child at a LOCAL +Z offset (oz) from pos (yaw-rotated),
    // with a translucent baseColor + soft emissive (for the glass rim). When
    // `asGlass` is set the entity is flagged for the engine's REAL translucent-glass
    // pass (Entity.transparent): `alpha` becomes the glass opacity and (r,g,b) the
    // glass tint — see-through glass instead of the old "fake by darkening" alpha.
    auto addMesh = [&](const x3::prims::PrimMesh& geo, float oz,
                       float r, float g, float b, float alpha,
                       float er, float eg, float eb, float es, bool asGlass = false) {
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0]=r; e.baseColor[1]=g; e.baseColor[2]=b; e.baseColor[3]=alpha;
        e.emissive[0]=er; e.emissive[1]=eg; e.emissive[2]=eb; e.emissive[3]=es;
        if (asGlass) {
            e.transparent = true;
            e.glass.opacity = alpha;            // see-through dial (was the fake alpha)
            e.glass.tint[0]=r; e.glass.tint[1]=g; e.glass.tint[2]=b;
            e.glass.roughness  = 0.0f;           // CLEAR — not frosted (owner playtest: crisp text + shimmer, no frost)
            e.glass.refraction = 0.02f;          // subtle scene-bend behind the plate
            e.glass.specular   = 0.6f;           // the light shimmer (kept)
        }
        e.tag = (uint32_t)Tag::Prop;
        const float wx = sn*oz, wz = cs*oz;
        e.transform[0]=cs; e.transform[2]=-sn; e.transform[8]=sn; e.transform[10]=cs;
        e.transform[12]=pos.x+wx; e.transform[13]=pos.y; e.transform[14]=pos.z+wz;
        return scene.add(e);
    };

    const float hw = width * 0.5f, hh = height * 0.5f;

    // Seed the boot readout BEFORE baking the texture so the static lines are
    // rasterized into the glass on the very first frame (the dynamic input line is
    // appended/re-baked later via regenTexture()). The Awakening terminal (EFLZ §3).
    // Line 0 is the HEADER TITLE; lines 1+ are the left-column data rows.
    m_lines = {
        "SECURITY CELL 07  --  STATUS: SECURE",
        "SUBJECT: JAKE",
        "STATUS: AUGMENTED",
        "MUSCULOSKELETAL OUTPUT: +400%",
        "RESTRAINT INTEGRITY: FAILING",
        "MAINTENANCE: AUTO-DIAG OK",
        "",
        "ENTER OVERRIDE CODE TO UNLOCK CELL:",
    };

    // Build the procedural hologram UI texture once (shared by the screen quad). The
    // readout TEXT is rasterized INTO it (stb_truetype) so it sits ON the glass.
    // 1024^2 so the fine cyan line-art (brackets, schematic, tick-row data text,
    // warning triangles, dotted strip) AND the baked text stay crisp on the glass.
    {
        m_texN = 1024;
        m_textOnGlass = glassFont().ready;   // text baked in -> host skips its overlay
        std::vector<uint8_t> holo = makeHologramRGBA(m_texN, m_lines, /*inputLine*/"");
        m_holoTex = device.createTexture(holo.data(), m_texN, m_texN, /*srgb*/true);
        m_lastInputShown = false;
        m_texDirty = false;
    }

    // ---- ROUND-SECTION GLASS TRIM (replaces the old FLAT rim ribbon — which read as
    // a SQUARE-section frame — with a real ROUNDED/CYLINDRICAL pipe). A circular
    // cross-section is swept along the panel's rounded-rect contour (makeRoundedRectTube)
    // so the trim is an actual round tube of glass with rounded corners. Two concentric
    // pipes give the polished-edge read: an inner clear-glass bead hugging the screen,
    // and a slightly larger, dimmer outer pipe glowing faintly into the dark. The whole
    // trim runs through the engine's REAL see-through glass pass (asGlass). ----
    {
        const float corner = std::min(hw, hh) * 0.30f;     // match the panel's rounding
        // Inner glass bead: a slim round pipe hugging the screen edge, faint cyan glass
        // tint + soft sheen so it reads as a polished lit round edge (not a flat line).
        x3::prims::PrimMesh innerRim =
            x3::prims::makeRoundedRectTube(hw + 0.010f, hh + 0.010f, corner, /*tubeR*/0.016f,
                                           /*pathSeg*/6, /*tubeSeg*/14);
        m_decor.push_back(addMesh(innerRim, 0.0f,
                                  0.55f, 0.85f, 1.0f, 0.34f,    // translucent cyan-white glass
                                  0.18f, 0.55f, 0.85f, 0.9f,    // GENTLE sheen (not a neon outline)
                                  /*asGlass=*/true));           // REAL see-through glass pipe
        // Outer pipe: a hair larger + dimmer, fatter round section, so the trim fades
        // softly into the surround like a glass tube catching ambient light.
        x3::prims::PrimMesh outerRim =
            x3::prims::makeRoundedRectTube(hw + 0.034f, hh + 0.034f, corner + 0.020f, /*tubeR*/0.022f,
                                           /*pathSeg*/6, /*tubeSeg*/14);
        m_decor.push_back(addMesh(outerRim, 0.0f,
                                  0.10f, 0.28f, 0.45f, 0.20f,   // very translucent dark-cyan glass
                                  0.06f, 0.22f, 0.38f, 0.55f,   // dim outer glow
                                  /*asGlass=*/true));           // REAL see-through glass pipe
        // Faint backer BEHIND the glass so the rim + line-art have a touch of contrast
        // WITHOUT reading as an opaque slab (that was bug #1: an OPAQUE dark backplate
        // filled the silhouette, so the see-through glass screen looked solid). Make it
        // REAL low-opacity glass too, so the cell behind still shows through the screen.
        x3::prims::PrimMesh backPanel = makeRoundedPanel(hw + 0.03f, hh + 0.03f, corner + 0.02f);
        m_decor.push_back(addMesh(backPanel, +0.024f,
                                  0.02f, 0.05f, 0.10f, 0.18f,   // barely-there dark-blue tint
                                  0.03f, 0.12f, 0.22f, 0.30f,
                                  /*asGlass=*/true));           // see-through, NOT an opaque slab
    }

    // ---- Glass ARM from the ceiling down to the top of the screen, carrying
    // emissive traces (fiber-optic cyan + copper amber) visible inside the glass. ----
    const float ceil = (ceilingY > 0.0f) ? ceilingY : pos.y + 1.7f;
    const float armTopY = ceil;
    const float armBotY = pos.y + hh + 0.05f;
    const float armH = (armTopY - armBotY) * 0.5f;
    if (armH > 0.05f) {
        const float armMidY = (armTopY + armBotY) * 0.5f - pos.y;  // local oy
        const float armBackZ = +0.06f;                              // BEHIND the glass (into the wall, away from player)
        // Slim glass spar (faint blue, slight emissive sheen) — not a fat gray bar.
        m_decor.push_back(addBox(0.035f, armH, 0.035f, 0, armMidY, armBackZ,
                                 0.10f,0.20f,0.30f, 1.0f,  0.12f,0.35f,0.55f, 0.6f));
        // Fiber-optic trace (cyan) + copper trace (amber) threaded inside the glass.
        m_decor.push_back(addBox(0.007f, armH, 0.007f, -0.014f, armMidY, armBackZ - 0.002f,
                                 0,0,0,1,  0.2f,0.9f,1.0f, 2.8f));   // cyan fiber
        m_decor.push_back(addBox(0.007f, armH, 0.007f,  0.014f, armMidY, armBackZ - 0.002f,
                                 0,0,0,1,  1.0f,0.55f,0.2f, 2.2f));  // copper trace
    }

    // ---- The HOLOGRAM screen: a rounded-corner glass quad, dark base color so the
    // EMISSIVE hologram texture + glow carry the look (reads as projected translucent
    // UI, not a solid slab). The texture supplies scanlines/grid/header/vignette +
    // rounded-corner fade; emissive makes it glow + feed bloom. update() pulses the
    // emissive strength + scrolls a scanline overlay for the live shimmer. ----
    x3::prims::PrimMesh geo = makeRoundedPanel(hw, hh, std::min(hw, hh) * 0.30f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    e.tex = m_holoTex;                                  // procedural hologram UI
    // REAL translucent GLASS (engine glass pass) — replaces the old "fake by leaving
    // it opaque + carrying the look in the emissive" hack. The plate is now actually
    // see-through: the cell behind subtly shows through while the holo UI texture +
    // emissive glow carry the projected-display read. baseColor stays full-albedo so
    // the bright cyan line-art survives the lit term; glass.opacity is the see-through
    // dial (mid so the printed HUD still reads strongly against what's behind).
    e.baseColor[0] = 1.0f; e.baseColor[1] = 1.0f; e.baseColor[2] = 1.0f; e.baseColor[3] = 1.0f;
    e.transparent = true;
    // Tasteful tinted, lightly-frosted, emissive display glass (spec §2 preset). With
    // M2-M4 live: the cell behind bends a touch through it (refraction), the surface
    // catches a cool-blue fresnel sheen + glints (M3) and reads slightly milky (M4
    // frost). Mid opacity keeps the printed holo UI dominant while the glass character
    // shows. Tuned against the M2-M4 shader, not the M1 fake-translucency hack.
    e.glass.opacity    = 0.55f;          // mid — UI reads, glass still see-through
    e.glass.tint[0]    = 0.78f; e.glass.tint[1] = 0.90f; e.glass.tint[2] = 1.0f; // cool-blue tint
    e.glass.roughness  = 0.14f;          // lightly frosted display glass (M4)
    e.glass.refraction = 0.025f;         // gentle bend of the cell behind (M2)
    e.glass.specular   = 0.55f;          // fresnel sheen + glints (M3)
    // Moderate emissive: gives the glass a hologram glow / bloom WITHOUT flooding out
    // the textured line-art (which rides the full-albedo lit term). Pulsed in update().
    m_emBase[0] = 0.10f; m_emBase[1] = 0.34f; m_emBase[2] = 0.52f; m_emBase[3] = 0.45f;
    e.emissive[0]=m_emBase[0]; e.emissive[1]=m_emBase[1]; e.emissive[2]=m_emBase[2]; e.emissive[3]=m_emBase[3];
    e.tag = (uint32_t)Tag::Prop;
    e.transform[0]=cs;  e.transform[2]=-sn;
    e.transform[8]=sn;  e.transform[10]=cs;
    e.transform[12]=pos.x; e.transform[13]=pos.y; e.transform[14]=pos.z;
    m_entity = scene.add(e);

    // ---- A second, slightly-in-front SCANLINE overlay quad: a thin bright cyan
    // emissive sheet whose emissive STRENGTH is animated in update() to scroll a
    // shimmer band down the glass (the moving "projector refresh" line). Same rounded
    // shape, nudged toward the viewer so it composites over the base. ----
    x3::prims::PrimMesh sgeo = makeRoundedPanel(hw*0.98f, hh*0.98f, std::min(hw, hh) * 0.28f);
    Entity se;
    se.mesh = device.createMesh(sgeo.verts.data(), (uint32_t)sgeo.verts.size(),
                                sgeo.index.data(), (uint32_t)sgeo.index.size());
    se.tex = m_holoTex;
    se.baseColor[0]=0.0f; se.baseColor[1]=0.0f; se.baseColor[2]=0.0f; se.baseColor[3]=1.0f;
    se.emissive[0]=0.3f; se.emissive[1]=0.9f; se.emissive[2]=1.0f; se.emissive[3]=0.0f;  // pulsed in update()
    // Glass too: drawn in the SAME transparent pass as the base plate (both
    // depth-write OFF) so the in-front sweep quad composites OVER the glass panel
    // instead of occluding it. Low opacity -> a faint additive-looking shimmer band;
    // its near-black albedo means it contributes essentially only its emissive sweep.
    se.transparent = true;
    se.glass.opacity    = 0.22f;
    se.glass.tint[0]    = 0.30f; se.glass.tint[1] = 0.90f; se.glass.tint[2] = 1.0f;
    se.glass.roughness  = 0.15f;
    se.glass.refraction = 0.0f;
    se.glass.specular   = 0.4f;
    se.tag = (uint32_t)Tag::Prop;
    const float foZ = 0.014f;                            // toward the viewer (front face +Z local)
    const float fwx = cs*0.0f + sn*foZ, fwz = -sn*0.0f + cs*foZ;
    se.transform[0]=cs; se.transform[2]=-sn; se.transform[8]=sn; se.transform[10]=cs;
    se.transform[12]=pos.x+fwx; se.transform[13]=pos.y; se.transform[14]=pos.z+fwz;
    m_scanEntity = scene.add(se);

    // (Boot readout seeded above, before the texture bake, so the on-glass text is
    // rasterized into the first hologram frame.)
    x3::logInfo("[holoterm] built cell-01 terminal (translucent emissive) at (" +
                std::to_string((int)pos.x) + "," + std::to_string((int)pos.y) + "," +
                std::to_string((int)pos.z) + ")");
}

void HoloTerminal::setActive(bool on) {
    if (m_active == on) return;
    m_active = on;
    m_texDirty = true;   // show/hide the live input line ON the glass
}

void HoloTerminal::pushChar(char c) {
    if (!m_active) return;
    // Printable ASCII only; cap the length.
    if (c < 32 || c > 126) return;
    if (m_input.size() >= kMaxInput) return;
    m_input += c;
    m_texDirty = true;   // re-bake the on-glass input line on change (not every frame)
}

void HoloTerminal::backspace() {
    if (!m_active || m_input.empty()) return;
    m_input.pop_back();
    m_texDirty = true;
}

bool HoloTerminal::submit() {
    if (!m_active) return false;
    const std::string v = m_input;
    bool accept = m_submit ? m_submit(v) : true;
    if (accept) {
        if (!v.empty()) m_lines.push_back("> " + v + "   [ACCEPTED]");
    } else {
        m_lines.push_back("> " + v + "   [REJECTED]");
    }
    m_input.clear();
    m_texDirty = true;   // readout grew + input cleared -> re-bake the glass
    return accept;
}

// Re-rasterize the readout (static lines + live input line) into the hologram
// texture and re-upload it, then point both glass quads at the new handle. The old
// texture is destroyed. Called from update() only when m_texDirty.
void HoloTerminal::regenTexture() {
    if (!m_device || m_entity == kNoLink) { m_texDirty = false; return; }
    // The live input line is baked on-glass while active (the typed override code).
    // A static '_' caret marks the field (the blink stays on the host's tiny fallback
    // only — we don't re-bake 2x/sec just to flash a cursor).
    std::string inputLine;
    if (m_active) inputLine = "> " + m_input + "_";
    m_lastInputShown = m_active;

    std::vector<uint8_t> holo = makeHologramRGBA(m_texN, m_lines, inputLine,
                                                 m_inkOverride ? m_textColor : nullptr);
    x3::rhi::TextureHandle fresh = m_device->createTexture(holo.data(), m_texN, m_texN, /*srgb*/true);
    if (!fresh.valid()) { m_texDirty = false; return; }   // keep the old tex on failure

    x3::rhi::TextureHandle old = m_holoTex;
    m_holoTex = fresh;
    if (m_scene) {
        if (m_entity     != kNoLink && m_entity     < m_scene->size()) m_scene->get(m_entity).tex     = m_holoTex;
        if (m_scanEntity != kNoLink && m_scanEntity < m_scene->size()) m_scene->get(m_scanEntity).tex = m_holoTex;
    }
    if (old.valid()) m_device->destroyTexture(old);
    m_texDirty = false;
}

void HoloTerminal::update(float dt) {
    m_blink += dt;
    if (m_blink >= 0.5f) { m_blink -= 0.5f; m_cursorOn = !m_cursorOn; }

    m_clock += dt;

    // Re-bake the on-glass readout ONLY when it changed (input typed/cleared, lines
    // added, mode toggled) — never every frame. Skipped on the headless self-test path
    // (no device/scene).
    if (m_texDirty) regenTexture();

    // ---- Holographic SHIMMER (only when built into a Scene; the self-test path
    // never calls build() so m_scene stays null and this is skipped). ----
    if (!m_scene || m_entity == kNoLink || m_entity >= m_scene->size()) return;

    // Slow emissive PULSE on the base glass (a gentle breathing glow, ~0.27 Hz) plus
    // a faint higher-frequency flicker (the projector instability). Subtle, bounded.
    const float pulse   = 0.86f + 0.14f * std::sin(m_clock * 1.7f);
    const float flicker = 0.97f + 0.03f * std::sin(m_clock * 13.0f);
    const float k = pulse * flicker;
    Entity& screen = m_scene->get(m_entity);
    screen.emissive[0] = m_emBase[0];
    screen.emissive[1] = m_emBase[1];
    screen.emissive[2] = m_emBase[2];
    screen.emissive[3] = m_emBase[3] * k;

    // Scrolling SCANLINE band: ramp the overlay quad's emissive strength up + down
    // (a moving brightness sweep). Triangle wave over ~2.2 s, peaking modestly so it
    // reads as a refresh sweep, not a strobe.
    if (m_scanEntity != kNoLink && m_scanEntity < m_scene->size()) {
        const float t = std::fmod(m_clock * 0.45f, 1.0f);      // 0..1 sweep phase
        const float band = 0.5f - 0.5f * std::cos(t * 6.2831853f); // smooth 0->1->0
        Entity& scan = m_scene->get(m_scanEntity);
        scan.emissive[3] = 0.20f + 0.55f * band;               // gentle additive sweep
    }
}

// ===========================================================================
// Headless self-test (--test-holoterm). H0-H4.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[holoterm-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[holoterm-test] FAIL ") + name); }
}
}

bool runHoloTerminalSelfTest() {
    g_pass = g_fail = 0;

    HoloTerminal t;
    // No device/scene needed for the input/text logic — drive it directly. (build()
    // is exercised in-app; here we test the state machine.)

    // ---- H0: boot readout is present (not blank). ----
    t.setLines({
        "DETENTION TERMINAL  // CELL 01",
        "ENTER OVERRIDE CODE TO UNLOCK CELL:",
    });
    check(t.lines().size() >= 2 && !t.lines()[0].empty(), "H0 terminal boots with readout (not blank)");

    // ---- H1: typing while INACTIVE is ignored; ACTIVE builds the input line. ----
    t.pushChar('1'); bool ignoredWhenInactive = t.input().empty();
    t.setActive(true);
    t.pushChar('1'); t.pushChar('1'); t.pushChar('2'); t.pushChar('7');
    check(ignoredWhenInactive && t.input() == "1127", "H1 input only accepted when active");

    // ---- H2: backspace edits the input line. ----
    t.backspace();
    check(t.input() == "112", "H2 backspace edits the input");

    // ---- H3: submit calls the sink with the value; ACCEPT clears + logs a line. --
    std::string got; bool sinkCalled = false;
    t.setSubmitSink([&](const std::string& v){ got = v; sinkCalled = true; return v == "1127"; });
    t.pushChar('7');                                  // back to "1127"
    size_t before = t.lines().size();
    bool accepted = t.submit();
    check(sinkCalled && got == "1127" && accepted && t.input().empty() && t.lines().size() == before + 1,
          "H3 submit fires the sink, accepts, clears, logs");

    // ---- H4: a REJECTED code keeps a reject line + clears, and the cursor blinks. -
    t.pushChar('9'); t.pushChar('9'); t.pushChar('9'); t.pushChar('9');
    bool rej = t.submit();
    bool blink0 = t.cursorOn();
    t.update(0.6f);                                    // > 0.5 s -> toggles
    bool blinkToggled = (t.cursorOn() != blink0);
    check(!rej && t.input().empty() && blinkToggled, "H4 reject path + cursor blinks");

    x3::logInfo(std::string("[holoterm-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
