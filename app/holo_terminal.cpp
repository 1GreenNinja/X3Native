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
#include <cctype>
#include <cstdlib>
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
    auto P = [&](float f){ return f * fn; };                // fraction -> pixels

    // ====================================================================
    // PROFESSIONAL TERMINAL LAYOUT (medical/military UI discipline). OWNER PASS
    // (2026-07-11): "the text is hard to read because graphics interfere with it."
    // The panel bakes into a rounded quad whose corners FADE to black (the wanted
    // rounded-glass look), so EVERY element lives inside a CORNER-SAFE GRID —
    // nothing important enters the corner discs (fixes 'STATUS: SECURE' clipping
    // at the top-right corner). Three non-overlapping columns —
    //   LEFT   = text-only readout (NO line-art behind text + a darker backing plate),
    //   CENTER = a self-contained schematic (no stroke exits the cell),
    //   RIGHT  = warning icons + value bars on a fixed right column —
    // a HEADER strip (emblem + title + ONE rule) and a dim BOTTOM data strip.
    // Brightness hierarchy: TEXT > schematic > decoration.
    // ====================================================================
    struct Rect { float x0, y0, x1, y1; };
    const Rect zHeader { 0.160f, 0.050f, 0.840f, 0.190f };   // emblem + title + rule
    const Rect zLeft   { 0.070f, 0.210f, 0.430f, 0.835f };   // text-only readout
    const Rect zCenter { 0.455f, 0.225f, 0.665f, 0.720f };   // schematic cell
    const Rect zRight  { 0.700f, 0.205f, 0.890f, 0.835f };   // bars / warnings
    const Rect zBottom { 0.160f, 0.850f, 0.840f, 0.905f };   // dim data strip
    auto inRect = [](const Rect& q, float u, float v){
        return u >= q.x0 && u <= q.x1 && v >= q.y0 && v <= q.y1;
    };

    // ---- 1) BASE FIELD: near-black glass with a faint scanline. TEXT zones (header,
    // left readout, bottom strip) get a DARKER backing plate (no grid) so glowing ink
    // always reads at high contrast; the schematic/right zones keep a faint grid so
    // they read as a live display. The bezel margin stays clear so the glass shows the
    // room at the very edge. ----
    const float paneM = 0.055f;
    Canvas c(n);
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / fn, v = (y + 0.5f) / fn;
            const size_t i = (size_t)y * n + x;
            const bool inPane = (u > paneM && u < 1.0f - paneM &&
                                 v > paneM && v < 1.0f - paneM);
            if (!inPane) { c.r[i]=0.004f; c.g[i]=0.009f; c.b[i]=0.017f; continue; } // bezel
            // Text backing plate: near-black, no grid — the "darker plate behind the
            // text block" so ink sits on a clean field (owner: text must read at a glance).
            if (inRect(zLeft,u,v) || inRect(zHeader,u,v) || inRect(zBottom,u,v)) {
                c.r[i]=0.006f; c.g[i]=0.012f; c.b[i]=0.024f; continue;
            }
            float br = 0.012f, bg = 0.024f, bb = 0.050f;
            const float scan = 0.85f + 0.15f * ((y % 4u) < 2u ? 1.0f : 0.5f);
            br *= scan; bg *= scan; bb *= scan;
            // faint grid ONLY in the schematic + right zones (never behind text)
            if ((inRect(zCenter,u,v) || inRect(zRight,u,v)) &&
                ((x % 28u) < 1u || (y % 28u) < 1u)) { bg += 0.010f; bb += 0.018f; }
            c.r[i]=br; c.g[i]=bg; c.b[i]=bb;
        }
    }

    // ---- Palette + brightness hierarchy. Decoration is dimmed to ~42% of text so the
    // eye ranks TEXT > schematic > decoration (owner: "dim decorative strokes"). ----
    const float CY_R = 0.26f, CY_G = 0.56f, CY_B = 1.60f;   // blue line-art (HDR)
    const float WT_R = 0.60f, WT_G = 0.92f, WT_B = 1.75f;   // bright blue-white
    const float th  = std::max(1.4f, fn / 512.0f);          // base stroke thickness
    const float thh = th * 1.5f;                            // heavier strokes
    const float kDeco  = 0.42f;    // frames / ticks / dotted strip / dividers (decoration)
    const float kSchem = 0.62f;    // schematic strokes + nodes (mid tier)

    // ---- 2) OUTER FRAME: corner brackets with the chamfered top-right corner, drawn
    // DIM. Inset to the corner-safe box so the brackets themselves never clip. ----
    bracketFrame(c, P(0.052f), P(0.055f), P(0.948f), P(0.945f),
                 P(0.070f), P(0.050f), CY_R,CY_G,CY_B, kDeco, thh);

    // ---- HEADER: hexagon emblem + ONE rule under it. Title text baked on-glass below. ----
    hexagon(c, P(zHeader.x0 + 0.020f), P(0.108f), P(0.026f), WT_R,WT_G,WT_B, kSchem, thh);
    line(c, P(zHeader.x0), P(0.170f), P(zHeader.x1), P(0.170f), WT_R,WT_G,WT_B, 0.7f, thh);

    // ---- LEFT COLUMN is TEXT-ONLY: no line-art here (readout baked below) so nothing
    // crosses the text rows. One dim vertical divider marks the column edge. ----
    line(c, P(zLeft.x1 + 0.012f), P(zLeft.y0), P(zLeft.x1 + 0.012f), P(zLeft.y1),
         CY_R,CY_G,CY_B, 0.22f, th);

    // ---- CENTER SCHEMATIC — a node + branches, ALL inside zCenter (no stroke exits the
    // cell into the text zones). ----
    {
        const float ncx = P(0.560f), ncy = P(0.400f);
        const float nhw = P(0.055f), nhh = P(0.050f);
        roundRectFrame(c, ncx-nhw, ncy-nhh, ncx+nhw, ncy+nhh, P(0.018f),
                       WT_R,WT_G,WT_B, kSchem, thh);
        line(c, ncx-nhw+P(0.010f), ncy-P(0.016f), ncx+nhw-P(0.010f), ncy-P(0.016f),
             CY_R,CY_G,CY_B, kSchem*0.8f, th);
        line(c, ncx-nhw+P(0.010f), ncy+P(0.010f), ncx+nhw-P(0.020f), ncy+P(0.010f),
             CY_R,CY_G,CY_B, kSchem*0.7f, th);
        const float bx0 = ncx + nhw, bxEnd = P(zCenter.x1 - 0.012f);
        for (int i = 0; i < 3; ++i) {
            const float by = ncy - P(0.030f) + i * P(0.030f);
            line(c, bx0, ncy, bx0 + P(0.014f), by, CY_R,CY_G,CY_B, kSchem, th);
            line(c, bx0 + P(0.014f), by, bxEnd - P(0.016f), by, CY_R,CY_G,CY_B, kSchem, th);
            rectFrame(c, bxEnd - P(0.016f), by-P(0.008f), bxEnd, by+P(0.008f),
                      CY_R,CY_G,CY_B, kSchem, th);
        }
        line(c, ncx, ncy+nhh, ncx, ncy+nhh+P(0.026f), CY_R,CY_G,CY_B, kSchem, th);
        chevronDown(c, ncx, ncy+nhh+P(0.026f), P(0.026f), P(0.024f), WT_R,WT_G,WT_B, kSchem, thh);
    }

    // ---- RIGHT COLUMN — warning triangles + labelled value bars on a fixed column. ----
    {
        const float rx0 = P(zRight.x0), rx1 = P(zRight.x1);
        for (int i = 0; i < 3; ++i) {
            const float tx = rx0 + i * P(0.066f);
            warnTriangle(c, tx + P(0.028f), P(0.225f), P(0.028f), P(0.048f),
                         WT_R,WT_G,WT_B, kSchem, th);
        }
        float fy = P(0.345f);
        const float fieldH = P(0.060f);
        const float barLens[3] = { 0.62f, 0.40f, 0.85f };
        for (int i = 0; i < 3; ++i) {
            line(c, rx0, fy, rx0 + P(0.055f), fy, CY_R,CY_G,CY_B, kDeco, th);   // label tick
            const float barY = fy + P(0.014f);
            rectFrame(c, rx0, barY, rx1, barY + P(0.018f), CY_R,CY_G,CY_B, kDeco, th);
            const float fillX = rx0 + (rx1 - rx0) * barLens[i];
            rectFill(c, rx0+th, barY+th, fillX, barY + P(0.018f) - th, WT_R,WT_G,WT_B, kSchem);
            fy += fieldH;
        }
        rectFill(c, rx1 - P(0.028f), fy + P(0.004f), rx1, fy + P(0.032f), WT_R,WT_G,WT_B, kSchem);
    }

    // ---- BOTTOM data strip — dim dotted/coded run, corner-safe span (ends well inside
    // the corners so it never clips). ----
    {
        const float by = P(0.878f);
        float dx = P(zBottom.x0);
        int k = 0;
        while (dx < P(zBottom.x1)) {
            const float dlen = (k % 3 == 0) ? P(0.020f) : ((k % 3 == 1) ? P(0.008f) : P(0.013f));
            line(c, dx, by, dx + dlen, by, CY_R,CY_G,CY_B, kDeco, thh);
            dx += dlen + P(0.010f);
            ++k;
        }
    }

    // ---- 2b) ON-GLASS READOUT TEXT (rasterized with stb_truetype so it sits ON the
    // glass + tilts with the panel). All text is corner-safe (header fits inside
    // x[0.16..0.84]; body rows live in the mid-band of the left column). ----
    if (glassFont().ready && !lines.empty()) {
        const float HOT = 1.0f;                       // ink alpha (additive glow ink)
        // --- HEADER TITLE: fit inside the header strip, RIGHT of the emblem, so the
        // whole title (incl. 'STATUS: SECURE') stays clear of the top-right corner. ---
        {
            const float tX = P(zHeader.x0 + 0.055f);        // start right of the emblem
            const float tSpan = P(zHeader.x1) - tX;         // available width to the safe edge
            float tpx = P(0.050f);
            const float tw = textWidthPx(lines[0], tpx);
            if (tw > tSpan && tw > 1.0f) tpx *= tSpan / tw; // shrink to span (never clips)
            drawTextGlass(c, lines[0], tX, P(0.078f), tpx, WT_R, WT_G, WT_B, HOT);
        }
        // --- BODY rows (1+) down the left text-only column. Fit to the column width. ---
        const float lx0b = P(zLeft.x0 + 0.012f);
        const float zoneW = P(zLeft.x1) - lx0b;
        float bpx = P(0.033f);
        for (size_t li = 1; li < lines.size(); ++li) {
            const float w = textWidthPx(lines[li], bpx);
            if (w > zoneW && w > 1.0f) bpx *= zoneW / w;
        }
        if (!inputLine.empty()) {
            const float w = textWidthPx(inputLine, bpx * 1.15f);
            if (w > zoneW && w > 1.0f) bpx *= zoneW / w;
        }
        if (bpx < P(0.017f)) bpx = P(0.017f);
        const float rowH = bpx * 1.34f;
        float ty = P(zLeft.y0 + 0.006f);
        // STATUS-COLOR console: GREEN = ok/secure, ORANGE = warning/alert, BLUE = data.
        // Ink is pushed a touch brighter than before so every row reads over the darker
        // backing (owner: "raise ink brightness slightly").
        auto statusColor = [](const std::string& s, float& r, float& g, float& b) {
            std::string U; U.reserve(s.size());
            for (char ch : s) U += (char)std::toupper((unsigned char)ch);
            auto has = [&](const char* kw){ return U.find(kw) != std::string::npos; };
            if (has("FAIL") || has("WARN") || has("ALERT") || has("CRIT") || has("DANGER") ||
                has("BREACH") || has("AUGMENT") || has("REJECT") || has("UNSTABLE") || has("LOCK")) {
                r = 1.75f; g = 0.78f; b = 0.16f;            // ORANGE (warning / alert)
            } else if (has(" OK") || has("ONLINE") || has("SECURE") || has("READY") ||
                       has("NOMINAL") || has("ACTIVE") || has("GRANT") || has("ACCEPT") ||
                       has("STABLE") || has("CLEAR")) {
                r = 0.30f; g = 1.70f; b = 0.58f;            // GREEN (ok / status good)
            } else {
                r = 0.40f; g = 0.78f; b = 1.80f;            // BLUE (default data)
            }
        };
        for (size_t li = 1; li < lines.size(); ++li) {
            if (lines[li].empty()) { ty += rowH * 0.5f; continue; }   // blank = half-row spacer
            float rr, gg, bb; statusColor(lines[li], rr, gg, bb);
            // W4-2 ink override (VIGIL presence = orange): body rows take the host ink.
            if (inkOverride) { rr = inkOverride[0]; gg = inkOverride[1]; bb = inkOverride[2]; }
            drawTextGlass(c, lines[li], lx0b, ty, bpx, rr, gg, bb, HOT);
            ty += rowH;
        }
        // --- LIVE INPUT LINE in amber, in its OWN clear strip (a dim underline rule
        // sets it apart from the data rows). ---
        if (!inputLine.empty()) {
            const float ipx = bpx * 1.15f;
            const float iy  = ty + rowH * 0.35f;
            line(c, lx0b, iy - P(0.006f), P(zLeft.x1), iy - P(0.006f),
                 1.20f, 0.72f, 0.20f, kDeco, th);         // dim amber divider above the prompt
            drawTextGlass(c, inputLine, lx0b, iy, ipx, 1.35f, 0.80f, 0.24f, HOT);
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

// A straight ROUND PIPE along the Y axis (the ceiling support strut + collars): a
// capped cylinder of radius `r`, from y=-halfH to y=+halfH, centered on origin.
// Smooth side normals; flat end caps. Used for the metallic ceiling-mount pipe.
x3::prims::PrimMesh makeCylinderY(float r, float halfH, uint32_t seg = 20) {
    x3::prims::PrimMesh m;
    seg = std::max(6u, seg);
    const float kTwoPi = 6.2831853f;
    const uint32_t ring = seg + 1;
    // Side wall: two rings (bottom, top) swept around.
    for (uint32_t j = 0; j <= seg; ++j) {
        const float a = kTwoPi * ((float)j / (float)seg);
        const float ca = std::cos(a), sa = std::sin(a);
        const float u = (float)j / (float)seg;
        m.verts.push_back({ { ca*r, -halfH, sa*r }, { ca, 0.0f, sa }, { u, 0.0f } });
        m.verts.push_back({ { ca*r,  halfH, sa*r }, { ca, 0.0f, sa }, { u, 1.0f } });
    }
    for (uint32_t j = 0; j < seg; ++j) {
        const uint32_t b0 = j*2, t0 = j*2+1, b1 = (j+1)*2, t1 = (j+1)*2+1;
        m.index.insert(m.index.end(), { b0, b1, t0, t0, b1, t1 });
    }
    // End caps (top + bottom fans).
    auto cap = [&](float y, float ny) {
        const uint32_t c = (uint32_t)m.verts.size();
        m.verts.push_back({ {0.0f, y, 0.0f}, {0.0f, ny, 0.0f}, {0.5f, 0.5f} });
        const uint32_t start = (uint32_t)m.verts.size();
        for (uint32_t j = 0; j <= seg; ++j) {
            const float a = kTwoPi * ((float)j / (float)seg);
            m.verts.push_back({ { std::cos(a)*r, y, std::sin(a)*r }, {0.0f, ny, 0.0f}, {0,0} });
        }
        for (uint32_t j = 0; j < seg; ++j) {
            if (ny > 0.0f) m.index.insert(m.index.end(), { c, start+j, start+j+1 });
            else           m.index.insert(m.index.end(), { c, start+j+1, start+j });
        }
    };
    cap( halfH,  1.0f);
    cap(-halfH, -1.0f);
    (void)ring;
    return m;
}
} // namespace

// Shared with HoloPanel (the platform): bake the flagship console hologram — the
// rich multi-color status HUD + readout text — into an RGBA8 n×n buffer, so the
// terminal SKIN of a HoloPanel is a single call.
std::vector<uint8_t> bakeConsoleHologram(uint32_t n, const std::vector<std::string>& lines,
                                         const std::string& input) {
    return makeHologramRGBA(n, lines, input);
}

// DARK-GLASS ROUNDED MEDICAL-VITALS MONITOR (F2 rescue-room wall screen). Reuses the
// black-glass line-art toolkit above (dark pane + additive glowing strokes + on-glass
// text + rounded-corner fade) so it matches Jake's-cell terminal LANGUAGE — a live
// dark-glass readout, not a flat teal quad. Content: NAME header + STABLE tag, a green
// ECG heart-rate trace, and green/cyan vitals rows.
std::vector<uint8_t> bakeMedicalMonitor(uint32_t n, const std::string& name) {
    const float fn = (float)n;
    auto P = [&](float f){ return f * fn; };

    // ---- 1) Near-black glass pane (barely-blue) + faint scanline/grid, dark bezel
    // margin — identical read to the flagship terminal's dark screen field. ----
    const float paneM = 0.055f;
    Canvas c(n);
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / fn, v = (y + 0.5f) / fn;
            const size_t i = (size_t)y * n + x;
            const bool inPane = (u > paneM && u < 1.0f - paneM &&
                                 v > paneM && v < 1.0f - paneM);
            if (inPane) {
                float br = 0.010f, bg = 0.020f, bb = 0.040f;
                const float scan = 0.85f + 0.15f * ((y % 4u) < 2u ? 1.0f : 0.5f);
                br *= scan; bg *= scan; bb *= scan;
                if ((x % 30u) < 1u || (y % 30u) < 1u) { bg += 0.010f; bb += 0.018f; }
                c.r[i] = br; c.g[i] = bg; c.b[i] = bb;
            } else {
                c.r[i] = 0.004f; c.g[i] = 0.008f; c.b[i] = 0.015f;
            }
        }
    }

    // GLOWING palette (HDR so the strokes bloom over the dark pane).
    const float GN_R = 0.22f, GN_G = 1.55f, GN_B = 0.48f;   // vitals GREEN (ECG / HR)
    const float BL_R = 0.30f, BL_G = 0.66f, BL_B = 1.70f;   // data BLUE/cyan
    const float WT_R = 0.62f, WT_G = 0.94f, WT_B = 1.72f;   // bright blue-white (name)
    const float th  = std::max(1.4f, fn / 512.0f);
    const float thh = th * 1.6f;

    // ---- 2) Rounded bezel: a rounded-rect frame just inside the pane edge, so the
    // silhouette reads as a rounded monitor (paired with the corner fade in the pack). ----
    roundRectFrame(c, P(paneM + 0.015f), P(paneM + 0.015f),
                   P(1.0f - paneM - 0.015f), P(1.0f - paneM - 0.015f),
                   P(0.06f), BL_R, BL_G, BL_B, 0.55f, thh);
    roundRectFrame(c, P(paneM + 0.028f), P(paneM + 0.028f),
                   P(1.0f - paneM - 0.028f), P(1.0f - paneM - 0.028f),
                   P(0.05f), BL_R, BL_G, BL_B, 0.22f, th);

    // ---- 3) HEADER: subject NAME + STABLE status + rule. ----
    {
        std::string hdr = name;
        for (auto& ch : hdr) ch = (char)std::toupper((unsigned char)ch);
        const float npx = P(0.070f);
        drawTextGlass(c, hdr, P(0.10f), P(0.095f), npx, WT_R, WT_G, WT_B, 1.0f);
        // "SUBJECT" subtitle
        drawTextGlass(c, "VITALS MONITOR", P(0.10f), P(0.185f), P(0.030f),
                      BL_R, BL_G, BL_B, 0.75f);
        // STABLE tag (green) at the right, with a filled status dot.
        const std::string tag = "STABLE";
        const float tpx = P(0.040f);
        const float tw = textWidthPx(tag, tpx);
        const float tx = P(1.0f - paneM - 0.05f) - tw;
        rectFill(c, tx - P(0.045f), P(0.11f), tx - P(0.045f) + P(0.020f), P(0.13f),
                 GN_R, GN_G, GN_B, 0.95f);
        drawTextGlass(c, tag, tx, P(0.098f), tpx, GN_R, GN_G, GN_B, 1.0f);
        line(c, P(0.10f), P(0.235f), P(1.0f - paneM - 0.05f), P(0.235f),
             WT_R, WT_G, WT_B, 0.9f, thh);
    }

    // ---- 4) ECG heart-rate trace (green). A repeating PQRST beat swept across a
    // band; drawn as a connected additive polyline so it blooms like a real monitor. ----
    {
        const float bx0 = P(0.10f), bx1 = P(1.0f - paneM - 0.05f);
        const float vy  = P(0.375f);            // band centerline
        const float bh  = P(0.11f);             // beat amplitude scale
        // PQRST as a function of beat phase p in [0,1) -> vertical offset (fraction of bh).
        auto ecg = [](float p) -> float {
            auto bump = [](float x, float c0, float w, float h) {
                const float d = (x - c0) / w;
                return (std::fabs(d) < 1.0f) ? h * (0.5f + 0.5f * std::cos(d * 3.14159265f)) : 0.0f;
            };
            float y = 0.0f;
            y += bump(p, 0.13f, 0.05f,  0.16f);   // P wave
            y -= bump(p, 0.32f, 0.018f, 0.14f);   // Q
            y += bump(p, 0.36f, 0.016f, 1.00f);   // R spike
            y -= bump(p, 0.40f, 0.020f, 0.30f);   // S
            y += bump(p, 0.60f, 0.07f,  0.26f);   // T wave
            return y;
        };
        const int steps = (int)(bx1 - bx0);
        const float beats = 3.4f;               // beats across the band width
        float pxprev = bx0, pyprev = vy;
        for (int s = 0; s <= steps; ++s) {
            const float x = bx0 + (float)s;
            const float t = (float)s / (float)steps;
            const float p = std::fmod(t * beats, 1.0f);
            const float yv = vy - ecg(p) * bh;
            if (s > 0) line(c, pxprev, pyprev, x, yv, GN_R, GN_G, GN_B, 0.98f, thh);
            pxprev = x; pyprev = yv;
        }
        // faint baseline
        line(c, bx0, vy + P(0.005f), bx1, vy + P(0.005f), GN_R, GN_G, GN_B, 0.16f, th);
        // "72 BPM" big readout at the right end of the trace band.
        drawTextGlass(c, "72", bx1 - P(0.20f), P(0.30f), P(0.070f), GN_R, GN_G, GN_B, 1.0f);
        drawTextGlass(c, "BPM", bx1 - P(0.20f), P(0.375f), P(0.028f), GN_R, GN_G, GN_B, 0.85f);
    }

    // ---- 5) VITALS ROWS (label + value). Green = pulse/HR, cyan = the rest. ----
    {
        struct Row { const char* label; const char* value; bool green; };
        const Row rows[] = {
            { "HR",   "72 bpm",      true  },
            { "BP",   "118 / 76",    false },
            { "SpO2", "98 %",        false },
            { "TEMP", "36.4 C",      false },
            { "RESP", "16 /min",     false },
        };
        const float lx = P(0.11f);
        const float vx = P(0.42f);
        float ty = P(0.50f);
        const float rowH = P(0.083f);
        const float lpx = P(0.036f);
        const float vpx = P(0.050f);
        for (const Row& rw : rows) {
            const float lr = rw.green ? GN_R : BL_R;
            const float lg = rw.green ? GN_G : BL_G;
            const float lb = rw.green ? GN_B : BL_B;
            drawTextGlass(c, rw.label, lx, ty + P(0.012f), lpx, BL_R, BL_G, BL_B, 0.85f);
            drawTextGlass(c, rw.value, vx, ty, vpx, lr, lg, lb, 1.0f);
            line(c, lx, ty + rowH - P(0.020f), P(1.0f - paneM - 0.05f), ty + rowH - P(0.020f),
                 BL_R, BL_G, BL_B, 0.12f, th);
            ty += rowH;
        }
    }

    // ---- 6) Rounded-corner + edge fade (same silhouette treatment as the terminal). ----
    const float r2corner = 0.28f;
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

    // SHINY CHROME metallic-roughness map (glTF packing: G = roughness, B = metallic).
    // metallic 1.0 (B=255), low roughness (~0.13 -> G=33) so the pipe frame + ceiling
    // strut read as polished steel catching sharp point-light highlights + reflections.
    auto chromeMR = x3::prims::makeSolidRGBA(4, 0, 33, 255);
    x3::rhi::TextureHandle chromeTex = device.createTexture(chromeMR.data(), 4, 4, /*srgb*/false);
    // ANTI-GLARE MATTE BLACK-GLASS metallic-roughness map for the screen PANE: metallic 0
    // (dielectric, B=0), HIGH roughness (~0.60 -> G=153). OWNER PASS (2026-07-11):
    // "Anti-glare screen too" — the old glossy G=26 (~0.10 rough) threw HOT specular
    // ORBS from the room point-lights that bloomed over the text. A matte roughness
    // spreads those into at most a broad faint sheen (never a defined orb), so the
    // dark-glass character now comes from the near-black albedo + chrome frame + glowing
    // ink, NOT from reflections. Medical-display finish.
    auto glossMR = x3::prims::makeSolidRGBA(4, 0, 153, 0);
    x3::rhi::TextureHandle glossTex = device.createTexture(glossMR.data(), 4, 4, /*srgb*/false);

    // Add a METALLIC round-pipe part (mrTex -> Cook-Torrance PBR path) at a LOCAL
    // offset (ox,oy,oz) from pos, yaw-rotated into world. baseColor = brushed-steel
    // albedo; the mrTex makes it metallic + polished. Optional faint emissive rim.
    auto addMetal = [&](const x3::prims::PrimMesh& geo, float ox, float oy, float oz,
                        float r = 0.78f, float g = 0.80f, float b = 0.85f) {
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.mrTex = chromeTex;                          // -> drawMeshPBR (metallic + reflections)
        e.baseColor[0]=r; e.baseColor[1]=g; e.baseColor[2]=b; e.baseColor[3]=1.0f;
        e.tag = (uint32_t)Tag::Prop;
        const float wx = cs*ox + sn*oz, wz = -sn*ox + cs*oz;
        e.transform[0]=cs; e.transform[2]=-sn; e.transform[8]=sn; e.transform[10]=cs;
        e.transform[12]=pos.x+wx; e.transform[13]=pos.y+oy; e.transform[14]=pos.z+wz;
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

    // ---- SHINY METALLIC ROUND-PIPE FRAME (Tim's redesign): a polished chrome/steel
    // round pipe (torus-section tube) bent into a rounded-rectangle PICTURE FRAME that
    // wraps the entire outer edge of the black-glass screen. High metallic + low
    // roughness (chromeTex) so it catches the point-light highlight + reflections and
    // reads as real steel tube holding the glass. A hair larger than the screen so it
    // sits proud of the glass edge like a bezel pipe. ----
    const float frameCorner = std::min(hw, hh) * 0.30f;
    {
        x3::prims::PrimMesh frame =
            x3::prims::makeRoundedRectTube(hw + 0.030f, hh + 0.030f, frameCorner + 0.02f,
                                           /*tubeR*/0.028f, /*pathSeg*/8, /*tubeSeg*/18);
        m_decor.push_back(addMetal(frame, 0.0f, 0.0f, 0.0f, 0.80f, 0.82f, 0.86f));
    }

    // ---- A SINGLE SUPPORT PIPE to the CEILING (Tim: the terminal HANGS from it, not
    // floating). One matching metallic round pipe rises from the TOP-CENTER of the
    // frame straight up to the ceiling, with a COLLAR/JOINT where it meets the frame
    // and where it meets the ceiling — so it reads as physically holding the weight. ----
    const float ceil = (ceilingY > 0.0f) ? ceilingY : pos.y + 1.7f;
    const float frameTopY   = pos.y + hh + 0.030f;   // world Y of the frame's top edge
    const float strutBotY   = frameTopY;
    const float strutTopY   = ceil;
    const float strutH      = strutTopY - strutBotY;
    if (strutH > 0.05f) {
        const float strutR    = 0.026f;
        const float strutMidY = (strutTopY + strutBotY) * 0.5f - pos.y;   // local oy
        // The support pipe (vertical chrome cylinder), top-center, in the panel plane.
        x3::prims::PrimMesh strut = makeCylinderY(strutR, strutH * 0.5f, 20);
        m_decor.push_back(addMetal(strut, 0.0f, strutMidY, 0.0f, 0.80f, 0.82f, 0.86f));
        // COLLAR where the strut meets the FRAME (a short fatter ring/cylinder joint).
        x3::prims::PrimMesh collarLo = makeCylinderY(strutR + 0.016f, 0.028f, 20);
        m_decor.push_back(addMetal(collarLo, 0.0f, (frameTopY - pos.y) + 0.028f, 0.0f,
                                   0.72f, 0.74f, 0.78f));
        // COLLAR / ceiling mount plate where the strut meets the CEILING.
        x3::prims::PrimMesh collarHi = makeCylinderY(strutR + 0.026f, 0.022f, 20);
        m_decor.push_back(addMetal(collarHi, 0.0f, (ceil - pos.y) - 0.022f, 0.0f,
                                   0.72f, 0.74f, 0.78f));
    }

    // ---- (1) THE TEXT-BACKING PANE — an OPAQUE near-black rounded quad whose EMISSIVE
    // is driven by the baked hologram UI texture (drawMeshPBR emissive-map path). The
    // pane itself reads BLACK; the console TEXT + line-art glow as pure self-lit ink in
    // STATUS COLORS (blue headers / green OK / orange alerts — baked per-line into the
    // texture) so they read crisp + high-contrast. This sits a hair BEHIND the black
    // glass slab, so you read the glowing text THROUGH the dark glass. ----
    x3::prims::PrimMesh geo = makeRoundedPanel(hw, hh, frameCorner);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    e.tex          = m_holoTex;   // near-black albedo
    e.emissiveTex  = m_holoTex;   // EMISSIVE map: the glowing multi-color text/HUD gate
    e.mrTex        = glossTex;    // GLOSSY BLACK GLASS: dielectric fresnel + specular highlight
    e.baseColor[0] = 0.03f; e.baseColor[1] = 0.035f; e.baseColor[2] = 0.05f; e.baseColor[3] = 1.0f;
    // Neutral-WHITE emissive multiplier so the texel's own STATUS COLORS survive
    // (green/orange are not tinted away); .a is the glow strength (bloom source), lifted
    // to punch THROUGH the dark glass slab in front. update() pulses .a for live shimmer.
    m_emBase[0] = 1.00f; m_emBase[1] = 1.00f; m_emBase[2] = 1.00f; m_emBase[3] = 2.30f;
    e.emissive[0]=m_emBase[0]; e.emissive[1]=m_emBase[1]; e.emissive[2]=m_emBase[2]; e.emissive[3]=m_emBase[3];
    e.tag = (uint32_t)Tag::Prop;
    e.transform[0]=cs;  e.transform[2]=-sn;
    e.transform[8]=sn;  e.transform[10]=cs;
    e.transform[12]=pos.x; e.transform[13]=pos.y; e.transform[14]=pos.z;
    m_entity = scene.add(e);

    // ---- (2) THE BLACK GLASS SLAB (Tim's redesign): a full-size rounded translucent
    // NEAR-BLACK glass plate mounted a hair IN FRONT of the text pane. It is the screen
    // SURFACE — dark glass you read the glowing text through, with a fresnel sheen + the
    // shader's animated M3 GLINT sweep, and the room faintly visible in its reflection.
    // Near-black tint keeps it reading as BLACK glass; low opacity + high emissive text
    // behind means the status text still punches through. ----
    x3::prims::PrimMesh sgeo = makeRoundedPanel(hw, hh, frameCorner);
    Entity se;
    se.mesh = device.createMesh(sgeo.verts.data(), (uint32_t)sgeo.verts.size(),
                                sgeo.index.data(), (uint32_t)sgeo.index.size());
    se.baseColor[0]=0.03f; se.baseColor[1]=0.035f; se.baseColor[2]=0.05f; se.baseColor[3]=1.0f;
    se.emissive[0]=0.05f; se.emissive[1]=0.08f; se.emissive[2]=0.13f; se.emissive[3]=0.04f;  // faint sheen, pulsed in update()
    se.transparent = true;
    se.glass.opacity    = 0.10f;         // low opacity: the glowing text punches THROUGH; the near-black tint keeps it reading as a black-glass surface
    se.glass.tint[0]    = 0.55f; se.glass.tint[1] = 0.62f; se.glass.tint[2] = 0.75f; // cool smoked tint (TEST: lighter so text survives)
    se.glass.roughness  = 0.55f;         // ANTI-GLARE matte (owner): broad soft sheen, no hot orbs
    se.glass.refraction = 0.014f;        // gentle bend of the cell behind (M2)
    se.glass.specular   = 0.05f;         // near-zero specular so room lights never bloom as orbs on the text
    se.tag = (uint32_t)Tag::Prop;
    // Nudge the glass toward the viewer along the panel normal (+Z world) so it
    // composites OVER the text pane.
    const float foZ = 0.014f;
    se.transform[0]=cs; se.transform[2]=-sn; se.transform[8]=sn; se.transform[10]=cs;
    se.transform[12]=pos.x; se.transform[13]=pos.y; se.transform[14]=pos.z + foZ;
    // The GLOSSY PANE itself now carries the black-glass read (dielectric fresnel +
    // specular via glossTex), so the separate front slab is OFF by default — its
    // screen-space refraction path blacks out the text where the scene-copy is
    // unpopulated. Opt-in (HOLO_GLASS_SLAB) for in-engine scenes that DO populate it.
    if (std::getenv("HOLO_GLASS_SLAB")) m_scanEntity = scene.add(se);

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
        // The dark screen pane samples the baked UI as BOTH its albedo AND its
        // emissive map — re-point both at the fresh handle. The outer glass plate
        // carries no UI texture (it's clear glass), so it's left untouched.
        if (m_entity != kNoLink && m_entity < m_scene->size()) {
            Entity& pane = m_scene->get(m_entity);
            pane.tex         = m_holoTex;
            pane.emissiveTex = m_holoTex;
        }
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
        scan.emissive[3] = 0.06f + 0.08f * band;               // SUBTLE cool sheen pulse on the clear glass (the M3 fresnel glint carries the sweep)
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

    // ---- H5: CORNER-SAFE LAYOUT (owner UI pass) — the baked texture must FADE its
    // rounded corners to black (so nothing clips there) while keeping bright content in
    // the safe interior. This proves the corner-inset layout headlessly (no engine). ----
    {
        const uint32_t bn = 128;
        std::vector<std::string> demo = {
            "SECURITY CELL 07  --  STATUS: SECURE",
            "SUBJECT: JAKE", "STATUS: AUGMENTED", "RESTRAINT INTEGRITY: FAILING",
        };
        std::vector<uint8_t> tex = bakeConsoleHologram(bn, demo, "");
        auto lum = [&](uint32_t x, uint32_t y) -> int {
            const uint8_t* p = &tex[((size_t)y * bn + x) * 4];
            return (int)p[0] + p[1] + p[2];
        };
        // The four extreme corners must be black (the corner fade zeroes them).
        const bool cornersDark = lum(0,0) <= 6 && lum(bn-1,0) <= 6 &&
                                 lum(0,bn-1) <= 6 && lum(bn-1,bn-1) <= 6;
        // The safe interior must carry real content (line-art / text ink).
        int bright = 0;
        for (uint32_t y = bn/4; y < bn*3/4; ++y)
            for (uint32_t x = bn/4; x < bn*3/4; ++x)
                if (lum(x,y) > 60) ++bright;
        check(cornersDark && bright > 40,
              "H5 rounded corners fade to black + safe interior carries content");
    }

    x3::logInfo(std::string("[holoterm-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
