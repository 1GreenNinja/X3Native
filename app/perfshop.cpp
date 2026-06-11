// PERFORMANCE SHOP — see perfshop.h.
//
// Clean-room: Scene/Entity + IRenderDevice + the LevelDoc loader + mesh_prims +
// stb_truetype (engine-linked implementation) only. The terminal glass bake
// follows holo_terminal.cpp's text-on-glass technique (additive float canvas ->
// RGBA8 -> createTexture); the small canvas helpers are re-stated here because
// the originals are file-local to that TU.
#include "perfshop.h"
#include "mesh_prims.h"
#include "terrain.h"          // terrainHeightAtWorld — site picking on the streamed terrain
#include "asset_root.h"

#include "engine/core/x3_log.h"
#include "engine/rhi/font_robotomono.h"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// ===========================================================================
// Additive float canvas (the holo-terminal technique, re-stated).
// ===========================================================================
struct Canvas {
    uint32_t w, h;
    std::vector<float> r, g, b;
    Canvas(uint32_t ww, uint32_t hh)
        : w(ww), h(hh), r((size_t)ww*hh,0), g((size_t)ww*hh,0), b((size_t)ww*hh,0) {}
    inline void add(int x, int y, float rr, float gg, float bb, float a) {
        if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) return;
        const size_t i = (size_t)y * w + x;
        r[i] += rr * a; g[i] += gg * a; b[i] += bb * a;
    }
    inline void set(int x, int y, float rr, float gg, float bb) {
        if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) return;
        const size_t i = (size_t)y * w + x;
        r[i] = rr; g[i] = gg; b[i] = bb;
    }
};

inline void plot(Canvas& c, float fx, float fy, float r, float g, float b, float a) {
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    c.add(x0,   y0,   r,g,b, a*(1-tx)*(1-ty));
    c.add(x0+1, y0,   r,g,b, a*(tx)*(1-ty));
    c.add(x0,   y0+1, r,g,b, a*(1-tx)*(ty));
    c.add(x0+1, y0+1, r,g,b, a*(tx)*(ty));
}

inline void line(Canvas& c, float x0, float y0, float x1, float y1,
                 float r, float g, float b, float a, float thick = 1.6f) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx*dx + dy*dy);
    if (len < 1e-4f) { plot(c, x0, y0, r, g, b, a); return; }
    const float nx = -dy / len, ny = dx / len;
    const int steps = (int)std::ceil(len);
    const float ht = thick * 0.5f;
    for (int s = 0; s <= steps; ++s) {
        const float t = (float)s / (float)steps;
        const float px = x0 + dx * t, py = y0 + dy * t;
        for (float o = -ht; o <= ht; o += 1.0f)
            plot(c, px + nx * o, py + ny * o, r, g, b, a);
    }
}

inline void rectFill(Canvas& c, float x0, float y0, float x1, float y1,
                     float r, float g, float b, float a) {
    for (int y = (int)y0; y <= (int)y1; ++y)
        for (int x = (int)x0; x <= (int)x1; ++x)
            c.add(x, y, r, g, b, a);
}

inline void rectFrame(Canvas& c, float x0, float y0, float x1, float y1,
                      float r, float g, float b, float a, float thick = 1.6f) {
    line(c, x0,y0, x1,y0, r,g,b,a,thick);
    line(c, x1,y0, x1,y1, r,g,b,a,thick);
    line(c, x1,y1, x0,y1, r,g,b,a,thick);
    line(c, x0,y1, x0,y0, r,g,b,a,thick);
}

// ---- stb_truetype text (Roboto Mono, engine-embedded) ----------------------
struct ShopFont {
    stbtt_fontinfo info{};
    bool ready = false;
    ShopFont() {
        const unsigned char* ttf = x3::rhi::kRobotoMonoTTF;
        const int off = stbtt_GetFontOffsetForIndex(ttf, 0);
        if (off >= 0 && stbtt_InitFont(&info, ttf, off)) ready = true;
    }
};
const ShopFont& shopFont() { static ShopFont f; return f; }

float textWidth(const std::string& s, float px) {
    const ShopFont& f = shopFont();
    if (!f.ready) return px * 0.6f * (float)s.size();
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    float w = 0.0f;
    for (char ch : s) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, (unsigned char)ch, &adv, &lsb);
        w += adv * scale;
    }
    return w;
}

float drawText(Canvas& c, const std::string& s, float penX, float topY, float px,
               float r, float g, float b, float a) {
    const ShopFont& f = shopFont();
    if (!f.ready) return 0.0f;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &asc, &desc, &gap);
    const float baseline = topY + asc * scale;
    float pen = penX;
    for (char chs : s) {
        const int ch = (unsigned char)chs;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, ch, &adv, &lsb);
        if (ch != ' ') {
            int gw = 0, gh = 0, gxo = 0, gyo = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(&f.info, scale, scale, ch, &gw, &gh, &gxo, &gyo);
            if (bmp) {
                const float gx0 = pen + lsb * scale;
                for (int yy = 0; yy < gh; ++yy)
                    for (int xx = 0; xx < gw; ++xx) {
                        const float cov = bmp[yy * gw + xx] / 255.0f;
                        if (cov > 0.003f)
                            c.add((int)std::lround(gx0 + gxo + xx),
                                  (int)std::lround(baseline + gyo + yy), r, g, b, a * cov);
                    }
                stbtt_FreeBitmap(bmp, nullptr);
            }
        }
        pen += adv * scale;
    }
    return pen - penX;
}

// Canvas -> RGBA8 (tonemapped clamp; srgb texture).
std::vector<uint8_t> canvasToRGBA(const Canvas& c) {
    std::vector<uint8_t> out((size_t)c.w * c.h * 4);
    for (size_t i = 0; i < (size_t)c.w * c.h; ++i) {
        auto to8 = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
        out[i*4+0] = to8(c.r[i]); out[i*4+1] = to8(c.g[i]); out[i*4+2] = to8(c.b[i]);
        out[i*4+3] = 255;
    }
    return out;
}

// Double-sided unit quad in XY (half-extents 0.5), facing +Z, UV 0..1 (v=0 top).
x3::prims::PrimMesh makeUnitQuad() {
    x3::prims::PrimMesh m;
    auto push = [&](float x, float y, float z, float nz, float u, float v) {
        m.verts.push_back({{x, y, z}, {0, 0, nz}, {u, v}});
    };
    // front (+Z)
    push(-0.5f,  0.5f, 0.0f,  1, 0, 0); push(0.5f,  0.5f, 0.0f,  1, 1, 0);
    push( 0.5f, -0.5f, 0.0f,  1, 1, 1); push(-0.5f, -0.5f, 0.0f, 1, 0, 1);
    m.index.insert(m.index.end(), { 0,3,2, 0,2,1 });
    // back (-Z, same UVs so the art reads from behind too)
    push(-0.5f,  0.5f, -0.002f, -1, 0, 0); push(0.5f,  0.5f, -0.002f, -1, 1, 0);
    push( 0.5f, -0.5f, -0.002f, -1, 1, 1); push(-0.5f, -0.5f, -0.002f, -1, 0, 1);
    m.index.insert(m.index.end(), { 4,6,7, 4,5,6 });
    return m;
}

// ===========================================================================
// The shop LevelDoc (LOCAL frame: floor top y=0, bay opening faces +Z).
// ===========================================================================
x3::editor::LevelDoc makePerfShopLevelDoc() {
    using x3::editor::BlockoutBrush;
    using x3::editor::EditorEntity;
    x3::editor::LevelDoc doc;
    doc.name  = "perfshop";
    doc.biome = "facility";
    doc.playerStart[0] = 0; doc.playerStart[1] = 0.1f; doc.playerStart[2] = 3.0f;

    auto brush = [&](const char* name, float px, float py, float pz,
                     float sx, float sy, float sz, const char* mat,
                     float tr, float tg, float tb, uint32_t type = 0, float yaw = 0.0f) {
        BlockoutBrush b;
        b.name = name; b.type = type;
        b.pos[0]=px; b.pos[1]=py; b.pos[2]=pz;
        b.size[0]=sx; b.size[1]=sy; b.size[2]=sz;
        b.yaw = yaw; b.material = mat;
        b.tint[0]=tr; b.tint[1]=tg; b.tint[2]=tb;
        b.collide = true;
        doc.brushes.push_back(b);
    };
    auto lightAt = [&](const char* name, float px, float py, float pz,
                       float tr, float tg, float tb, float intensity) {
        EditorEntity e;
        e.name = name; e.type = "light";
        e.pos[0]=px; e.pos[1]=py; e.pos[2]=pz;
        e.tint[0]=tr; e.tint[1]=tg; e.tint[2]=tb;
        e.scale = intensity;
        doc.entities.push_back(e);
    };

    // Shell: floor (incl. the exterior apron to z=+9.8), walls, bay header, roof.
    brush("floor",      0, -0.2f,  1.4f, 18.0f, 0.4f, 17.2f, "floor",  0.62f,0.63f,0.66f);
    brush("wall_back",  0,  2.5f, -6.8f, 18.0f, 5.0f,  0.4f, "panel",  0.72f,0.74f,0.78f);
    brush("wall_left", -8.8f, 2.5f, 0,    0.4f, 5.0f, 14.0f, "panel",  0.72f,0.74f,0.78f);
    brush("wall_right", 8.8f, 2.5f, 0,    0.4f, 5.0f, 14.0f, "panel",  0.72f,0.74f,0.78f);
    brush("bay_left",  -6.0f, 2.5f, 6.8f, 6.0f, 5.0f,  0.4f, "panel",  0.72f,0.74f,0.78f);
    brush("bay_right",  6.0f, 2.5f, 6.8f, 6.0f, 5.0f,  0.4f, "panel",  0.72f,0.74f,0.78f);
    brush("bay_header", 0,    4.35f,6.8f, 6.0f, 1.3f,  0.4f, "panel",  0.60f,0.62f,0.68f);
    brush("roof",       0,    5.2f, 0,   18.0f, 0.4f, 14.4f, "ceiling",0.70f,0.72f,0.75f);

    // The LIFT: a low steel pad (drive onto it) + safety side rails.
    brush("lift_pad",   0, 0.06f, -1.5f, 4.6f, 0.12f, 6.2f, "solid", 0.25f,0.27f,0.31f);
    brush("lift_rail_l",-2.55f, 0.35f, -1.5f, 0.25f, 0.7f, 5.8f, "solid", 0.85f,0.45f,0.10f);
    brush("lift_rail_r", 2.55f, 0.35f, -1.5f, 0.25f, 0.7f, 5.8f, "solid", 0.85f,0.45f,0.10f);

    // Set dressing: red toolboxes, parts shelves, a workbench, a tire stack.
    brush("toolbox_a", -6.0f, 0.55f, -6.0f, 1.4f, 1.1f, 0.7f, "solid", 0.75f,0.08f,0.10f);
    brush("toolbox_b", -3.8f, 0.55f, -6.0f, 1.4f, 1.1f, 0.7f, "solid", 0.75f,0.08f,0.10f);
    brush("toolbox_c",  4.6f, 0.55f, -6.0f, 1.4f, 1.1f, 0.7f, "solid", 0.78f,0.12f,0.10f);
    brush("shelf_a",   -8.3f, 1.5f,  2.0f, 0.5f, 3.0f, 3.4f, "panel", 0.55f,0.58f,0.62f);
    brush("shelf_b",   -8.3f, 1.5f, -2.5f, 0.5f, 3.0f, 3.4f, "panel", 0.55f,0.58f,0.62f);
    brush("workbench",  8.2f, 0.5f,  2.5f, 0.8f, 1.0f, 4.0f, "solid", 0.45f,0.36f,0.26f);
    brush("tirestack",  7.9f, 0.55f,-5.6f, 1.1f, 1.1f, 1.1f, "solid", 0.12f,0.12f,0.13f);

    // Lights: warm work lights, a bright lift spot, the magenta sign wash
    // outside, a cyan accent on the terminal corner.
    lightAt("work_fl", -4.5f, 4.4f,  3.0f, 1.0f, 0.93f, 0.80f, 1.6f);
    lightAt("work_fr",  4.5f, 4.4f,  3.0f, 1.0f, 0.93f, 0.80f, 1.6f);
    lightAt("work_bl", -4.5f, 4.4f, -4.0f, 1.0f, 0.93f, 0.80f, 1.6f);
    lightAt("work_br",  4.5f, 4.4f, -4.0f, 1.0f, 0.93f, 0.80f, 1.6f);
    lightAt("lift_spot", 0.0f, 4.5f, -1.5f, 1.0f, 0.97f, 0.90f, 2.2f);
    lightAt("sign_wash", 0.0f, 4.9f,  8.6f, 1.0f, 0.25f, 0.72f, 1.8f);
    lightAt("term_cyan",-2.8f, 3.2f, -5.6f, 0.30f, 0.85f, 1.0f, 1.2f);
    return doc;
}

// The shipped LevelDoc path (the canonical authored asset).
std::string perfShopDocPath() { return assetRoot() + "/levels/perfshop.leveldoc.json"; }

// Tier label "T1".."T4".
std::string tierTag(int t) { return "T" + std::to_string(std::clamp(t, 1, 9)); }

// One-line stat string for a part row (per category semantics).
std::string statLine(const x3::game::vehparts::Part& p) {
    char b[128];
    const std::string& c = p.category;
    if (c == "camshaft")      { std::snprintf(b, sizeof(b), "CURVE PROFILE  +%d RPM", (int)p.redlineBonus); }
    else if (c == "exhaust")  { std::snprintf(b, sizeof(b), "+%.1f%% PWR  NOTE %d", p.powerPct, p.noteId); }
    else if (c == "intake")   { std::snprintf(b, sizeof(b), "+%.1f%% PWR", p.powerPct); }
    else if (c == "intercooler") { std::snprintf(b, sizeof(b), "+%.1f%% PWR  +%.2f BAR SAFE", p.powerPct, p.safeBoostBonus); }
    else if (c == "forced_induction") {
        if (p.fiType == "turbo") std::snprintf(b, sizeof(b), "TURBO +%.0f%% @MAX  SPOOL %.1fs", p.boostPowerPct, p.spoolLagS);
        else                     std::snprintf(b, sizeof(b), "SUPERCHARGER +%.0f%% FLAT", p.boostPowerPct);
    }
    else if (c == "ecu")      { std::snprintf(b, sizeof(b), "MAX %.1f BAR  SAFE %.1f  KNK %.1f", p.maxBoost, p.safeBoost, p.knockLimit); }
    else if (c == "tires")    { std::snprintf(b, sizeof(b), "GRIP x%.2f  [%s]", p.gripScale, p.compound.c_str()); }
    else if (c == "suspension"){ std::snprintf(b, sizeof(b), "%.0fmm DROP  %.1fHz/%.2f", -p.rideHeightDelta*1000.0f, p.suspFreq, p.suspDamp); }
    else if (c == "brakes")   { std::snprintf(b, sizeof(b), "%.0f Nm", p.brakeTorque); }
    else if (c == "weight")   { std::snprintf(b, sizeof(b), "%.0f KG", p.massDelta); }
    else if (c == "nitrous")  { std::snprintf(b, sizeof(b), "x%.2f TQ  %.0fs TANK", p.nitrousMult, p.tankSeconds); }
    else b[0] = '\0';
    return b;
}

} // namespace

// ===========================================================================
// PerfShop
// ===========================================================================
bool PerfShop::build(Scene& scene, x3::rhi::IRenderDevice& device,
                     x3::phys::IPhysicsWorld& physics,
                     vehparts::Catalog* catalog, vehparts::VehicleBuild* buildState,
                     float aheadX, float aheadZ) {
    m_catalog = catalog; m_build = buildState;
    m_scene = &scene; m_device = &device;
    m_status = "WELCOME TO LATE NIGHT SPEED";

    // ---- 1) The LevelDoc (LOCAL frame). Prefer the shipped asset (hand/editor
    // editable); generate + save it when absent so the authoring loop closes. ----
    x3::editor::LevelDoc doc;
    const std::string docPath = perfShopDocPath();
    bool fromFile = doc.loadJson(docPath);
    if (!fromFile) {
        doc = makePerfShopLevelDoc();
        if (doc.saveJson(docPath))
            x3::logInfo("[perfshop] authored LevelDoc written: " + docPath);
    }
    x3::logInfo(std::string("[perfshop] LevelDoc source: ") + (fromFile ? docPath : "generated"));

    // ---- 2) Pick the FLATTEST candidate site around (aheadX, aheadZ): sample
    // the 20x20 m footprint on the terrain, minimize (max-min) height. ----
    float bestX = aheadX, bestZ = aheadZ, bestSpread = 1e9f, bestMax = 0.0f;
    for (int cand = 0; cand < 9; ++cand) {
        const float cx = aheadX + ((cand % 3) - 1) * 26.0f;
        const float cz = aheadZ + ((cand / 3) - 1) * 26.0f;
        float mn = 1e9f, mx = -1e9f;
        for (int gx = -2; gx <= 2; ++gx)
            for (int gz = -2; gz <= 2; ++gz) {
                const float hgt = terrainHeightAtWorld(cx + gx * 5.0f, cz + gz * 5.0f);
                mn = std::min(mn, hgt); mx = std::max(mx, hgt);
            }
        if (mx - mn < bestSpread) { bestSpread = mx - mn; bestX = cx; bestZ = cz; bestMax = mx; }
    }
    const float floorTop = bestMax + 0.12f;
    m_site[0] = bestX; m_site[1] = floorTop; m_site[2] = bestZ;
    {
        char lb[160];
        std::snprintf(lb, sizeof(lb), "[perfshop] site (%.1f, %.1f, %.1f), terrain spread %.2f m",
                      bestX, floorTop, bestZ, bestSpread);
        x3::logInfo(lb);
    }

    // ---- 3) Runtime ENTRY RAMP from the terrain up to the apron lip (rise
    // computed at the actual bay approach; ramp rises along +Z then yaw=pi so
    // it climbs TOWARD the shop for a car arriving from +Z). ----
    {
        const float apronEdgeZ = 9.8f;                  // local floor edge
        const float rampRun    = 3.0f;
        const float tBase = terrainHeightAtWorld(bestX, bestZ + apronEdgeZ + rampRun);
        const float rise  = std::max(0.15f, floorTop - tBase + 0.02f);
        x3::editor::BlockoutBrush ramp;
        ramp.name = "entry_ramp"; ramp.type = 1;        // Ramp
        ramp.pos[0] = 0.0f;
        ramp.pos[1] = (tBase - floorTop) + rise * 0.5f; // local (pre-translate) center
        ramp.pos[2] = apronEdgeZ + rampRun * 0.5f;
        ramp.size[0] = 10.0f; ramp.size[1] = rise; ramp.size[2] = rampRun;
        ramp.yaw = 3.14159265f;                         // rise toward -Z (into the bay)
        ramp.material = "floor";
        ramp.tint[0]=0.58f; ramp.tint[1]=0.59f; ramp.tint[2]=0.62f;
        ramp.collide = true;
        doc.brushes.push_back(ramp);
    }

    // ---- 4) Translate the local doc onto the site + build through the REAL
    // data-driven loader. ----
    for (auto& b : doc.brushes) {
        b.pos[0] += m_site[0]; b.pos[1] += m_site[1]; b.pos[2] += m_site[2];
    }
    for (auto& e : doc.entities) {
        e.pos[0] += m_site[0]; e.pos[1] += m_site[1]; e.pos[2] += m_site[2];
    }
    if (!m_world.buildFromDoc(doc, scene, device, physics)) {
        x3::logError("[perfshop] LevelDocWorld build failed");
        return false;
    }

    m_lift[0] = m_site[0]; m_lift[1] = m_site[1] + 0.12f; m_lift[2] = m_site[2] - 1.5f;

    // ---- 5) Code-built extras the LevelDoc can't express: the neon sign + the
    // terminal glass. ----
    auto addQuad = [&](float w, float h, float lx, float ly, float lz,
                       x3::rhi::TextureHandle tex, const float emissive[4],
                       bool asGlass, float alpha) -> uint32_t {
        x3::prims::PrimMesh q = makeUnitQuad();
        Entity e;
        e.mesh = device.createMesh(q.verts.data(), (uint32_t)q.verts.size(),
                                   q.index.data(), (uint32_t)q.index.size());
        m_extraMeshes.push_back(e.mesh);
        e.tex = tex;
        e.baseColor[0]=1; e.baseColor[1]=1; e.baseColor[2]=1; e.baseColor[3]=alpha;
        for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
        if (asGlass) {
            e.transparent = true;
            e.glass.opacity = alpha;
            e.glass.tint[0]=0.6f; e.glass.tint[1]=0.85f; e.glass.tint[2]=1.0f;
            e.glass.roughness = 0.0f; e.glass.refraction = 0.02f; e.glass.specular = 0.5f;
        }
        e.tag = (uint32_t)Tag::Prop;
        e.transform[0] = w; e.transform[5] = h; e.transform[10] = 1.0f;
        e.transform[12] = m_site[0] + lx;
        e.transform[13] = m_site[1] + ly;
        e.transform[14] = m_site[2] + lz;
        const uint32_t slot = scene.add(e);
        m_extraSlots.push_back(slot);
        return slot;
    };

    // 'LATE NIGHT SPEED' — the neon sign over the bay door (Tim's garage).
    {
        Canvas c(1024, 256);
        // near-black backing so only the tubes glow
        for (uint32_t i = 0; i < c.w * c.h; ++i) { c.r[i]=0.02f; c.g[i]=0.01f; c.b[i]=0.03f; }
        const std::string txt = "LATE NIGHT SPEED";
        // Fit the text to the canvas (Roboto Mono at 120px overflows 1024).
        float px = 120.0f;
        if (textWidth(txt, px) > 940.0f) px *= 940.0f / textWidth(txt, px);
        const float tw = textWidth(txt, px);
        const float x0 = (1024.0f - tw) * 0.5f;
        const float ty = (256.0f - px) * 0.5f;
        // a SOFT halo (2 light passes) + the hot magenta tube core
        for (int o = 1; o <= 2; ++o) {
            drawText(c, txt, x0 - (float)o, ty, px, 0.55f, 0.06f, 0.28f, 0.06f);
            drawText(c, txt, x0 + (float)o, ty, px, 0.55f, 0.06f, 0.28f, 0.06f);
        }
        drawText(c, txt, x0, ty, px, 1.0f, 0.22f, 0.62f, 1.0f);
        // tube border
        rectFrame(c, 14, 14, 1010, 242, 0.85f, 0.16f, 0.50f, 0.7f, 4.0f);
        std::vector<uint8_t> rgba = canvasToRGBA(c);
        x3::rhi::TextureHandle tex = device.createTexture(rgba.data(), 1024, 256, /*srgb*/true);
        m_extraTex.push_back(tex);
        // LOW flat emissive: the engine's per-object emissive is a uniform add
        // (not texture-gated), so anything higher floods the tubes into a slab.
        // The magenta sign_wash point light supplies the neon spill instead.
        const float em[4] = { 1.0f, 0.30f, 0.72f, 0.45f };
        addQuad(5.8f, 1.25f, 0.0f, 4.35f, 7.06f, tex, em, /*glass*/false, 1.0f);  // on the bay header band
    }

    // The garage TERMINAL: dark backplate + the live glass screen on the back
    // wall, facing the lift (the UI texture is baked in bakeTerminal()).
    {
        // backplate (plain dark panel, slight emissive rim feel via tint)
        Canvas c(8, 8);
        for (uint32_t i = 0; i < 64; ++i) { c.r[i]=0.03f; c.g[i]=0.04f; c.b[i]=0.06f; }
        std::vector<uint8_t> rgba = canvasToRGBA(c);
        x3::rhi::TextureHandle bp = device.createTexture(rgba.data(), 8, 8, true);
        m_extraTex.push_back(bp);
        const float em0[4] = { 0.05f, 0.10f, 0.16f, 0.4f };
        addQuad(3.5f, 2.3f, -2.8f, 2.3f, -6.56f, bp, em0, false, 1.0f);
    }
    bakeTerminal(device);   // creates m_screenTex + the glass entity

    // First composition (pure data; the host applies it to the car via recompose()).
    if (m_catalog && m_build) m_composed = vehparts::compose(*m_catalog, *m_build);

    x3::logInfo("[perfshop] built: " + std::to_string(m_world.brushEntityCount()) +
                " brushes, " + std::to_string(m_world.lightCount()) + " lights, lift at (" +
                std::to_string(m_lift[0]) + ", " + std::to_string(m_lift[2]) + ")");
    return true;
}

void PerfShop::shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics) {
    m_world.shutdown(scene, device, physics);
    for (uint32_t slot : m_extraSlots) {
        Entity& e = scene.get(slot);
        e.visible = false; e.mesh = {};
    }
    for (auto& mh : m_extraMeshes) device.destroyMesh(mh);
    for (auto& th : m_extraTex)    device.destroyTexture(th);
    if (m_screenTex.id) device.destroyTexture(m_screenTex);
    m_extraSlots.clear(); m_extraMeshes.clear(); m_extraTex.clear();
    m_screenTex = {}; m_screenSlot = kNoLink;
    m_scene = nullptr; m_device = nullptr;
}

bool PerfShop::onLiftPad(const float carPos[3]) const {
    const float dx = carPos[0] - m_lift[0];
    const float dz = carPos[2] - m_lift[2];
    return std::fabs(dx) <= 2.2f && std::fabs(dz) <= 3.0f;
}

void PerfShop::setShopMode(bool on) {
    if (m_shopMode == on) return;
    m_shopMode = on;
    if (on) {
        m_orbitA = 3.14159265f * 0.5f;   // start the swing from the bay side
        m_status = "WELCOME TO LATE NIGHT SPEED";
        markUiDirty();
    }
}

void PerfShop::orbitCam(float out5[5]) const {
    const float radius = 7.2f, height = 2.9f;
    const float cx = m_lift[0] + std::cos(m_orbitA) * radius;
    const float cy = m_lift[1] + height;
    const float cz = m_lift[2] + std::sin(m_orbitA) * radius;
    const float dx = m_lift[0] - cx, dy = (m_lift[1] + 0.7f) - cy, dz = m_lift[2] - cz;
    const float flat = std::sqrt(dx*dx + dz*dz);
    out5[0] = cx; out5[1] = cy; out5[2] = cz;
    out5[3] = std::atan2(dz, dx);          // yaw (engine forward = cos/sin yaw on XZ)
    out5[4] = std::atan2(dy, flat);        // pitch
}

// ---- UI input --------------------------------------------------------------
void PerfShop::uiUp() {
    if (m_mode != Mode::Parts) return;
    if (m_focus == Focus::Categories) m_catCursor = std::max(0, m_catCursor - 1);
    else                              m_partCursor = std::max(0, m_partCursor - 1);
    markUiDirty();
}

void PerfShop::uiDown() {
    if (m_mode != Mode::Parts || !m_catalog) return;
    if (m_focus == Focus::Categories)
        m_catCursor = std::min((int)m_catalog->categories().size() - 1, m_catCursor + 1);
    else {
        const auto parts = m_catalog->inCategory(m_catalog->categories()[m_catCursor].id);
        m_partCursor = std::min((int)parts.size() - 1, m_partCursor + 1);
    }
    markUiDirty();
}

void PerfShop::uiSelect() {
    if (m_mode != Mode::Parts || !m_catalog || !m_build) return;
    if (m_focus == Focus::Categories) {
        m_focus = Focus::Parts; m_partCursor = 0; markUiDirty();
        return;
    }
    const auto& cat = m_catalog->categories()[m_catCursor];
    const auto parts = m_catalog->inCategory(cat.id);
    if (m_partCursor < 0 || m_partCursor >= (int)parts.size()) return;
    const vehparts::Part* p = parts[m_partCursor];
    const std::string* cur = m_build->installedIn(cat.id);
    char sb[96];
    if (cur && *cur == p->id) {
        // SELL BACK at 70%.
        const int back = (p->price * 7) / 10;
        m_build->removeFrom(cat.id);
        m_build->credits += back;
        std::snprintf(sb, sizeof(sb), "SOLD BACK %s  +%d CR", p->name.c_str(), back);
        m_status = sb;
        applyAndSave(nullptr);   // car applied on next update()
    } else if (m_build->credits < p->price) {
        m_status = "INSUFFICIENT CREDITS";
    } else {
        m_build->credits -= p->price;
        if (cur) {  // trade in the old part of this category at 70%
            if (const vehparts::Part* old = m_catalog->find(*cur))
                m_build->credits += (old->price * 7) / 10;
        }
        m_build->install(cat.id, p->id);
        if (cat.id == "nitrous") m_build->nitrousRemaining = p->tankSeconds;
        std::snprintf(sb, sizeof(sb), "INSTALLED %s  -%d CR", p->name.c_str(), p->price);
        m_status = sb;
        applyAndSave(nullptr);
    }
    markUiDirty();
}

void PerfShop::uiBack() {
    if (m_mode == Mode::Parts && m_focus == Focus::Parts) {
        m_focus = Focus::Categories;
        markUiDirty();
    }
}

void PerfShop::uiTab() {
    m_mode = (m_mode == Mode::Parts) ? Mode::Dyno : Mode::Parts;
    m_focus = Focus::Categories;
    markUiDirty();
}

void PerfShop::adjustTune(int slider, int dir) {
    if (!m_build || !m_catalog) return;
    if (m_composed.ecuMaxBoost <= 0.0f) { m_status = "INSTALL AN ECU TO TUNE"; markUiDirty(); return; }
    vehparts::EcuTune& t = m_build->tune;
    if (slider == 0) t.boost  = std::clamp(t.boost  + 0.1f  * dir, 0.0f,  m_composed.ecuMaxBoost);
    if (slider == 1) t.fuel   = std::clamp(t.fuel   + 0.01f * dir, 0.85f, 1.20f);
    if (slider == 2) t.timing = std::clamp(t.timing + 0.05f * dir, 0.0f,  1.0f);
    applyAndSave(nullptr);
    markUiDirty();
}

void PerfShop::startPull() {
    if (m_mode != Mode::Dyno || m_pullT >= 0.0f) return;
    m_pullT = 0.0f;
    m_pullPopped = false;
    m_status = "PULL RUNNING...";
    markUiDirty();
}

void PerfShop::repairEngine() {
    if (!m_build || !m_build->engineDamaged) return;
    const int cost = std::max(500, m_composed.repairCost);
    if (m_build->credits < cost) { m_status = "REPAIR: INSUFFICIENT CREDITS"; markUiDirty(); return; }
    m_build->credits -= cost;
    m_build->engineDamaged = false;
    char sb[64]; std::snprintf(sb, sizeof(sb), "ENGINE REBUILT  -%d CR", cost);
    m_status = sb;
    applyAndSave(nullptr);
    markUiDirty();
}

void PerfShop::refillNitrous() {
    if (!m_build) return;
    if (m_composed.nitrousMult <= 0.0f) { m_status = "NO NITROUS KIT INSTALLED"; markUiDirty(); return; }
    if (m_build->nitrousRemaining >= m_composed.nitrousTankS - 0.01f) { m_status = "TANK ALREADY FULL"; markUiDirty(); return; }
    const int cost = std::max(50, m_composed.nitrousRefillCost);
    if (m_build->credits < cost) { m_status = "REFILL: INSUFFICIENT CREDITS"; markUiDirty(); return; }
    m_build->credits -= cost;
    m_build->nitrousRemaining = m_composed.nitrousTankS;
    char sb[64]; std::snprintf(sb, sizeof(sb), "NITROUS REFILLED  -%d CR", cost);
    m_status = sb;
    m_needSave = true;
    markUiDirty();
}

void PerfShop::recompose(DriveDemo* car) {
    if (!m_catalog || !m_build) return;
    m_composed = vehparts::compose(*m_catalog, *m_build);
    if (car) car->applyTuning(m_composed.tuning);
}

void PerfShop::applyAndSave(DriveDemo* car) {
    recompose(car);
    m_needSave = true;
    m_carDirty = true;   // (re)apply to the live car on the next update()
}

void PerfShop::update(float dt, DriveDemo* car,
                      x3::audio::IAudioSystem* audio, x3::audio::SoundHandle bangSfx) {
    if (m_shopMode) m_orbitA += dt * 0.22f;     // slow showcase orbit (dt-scaled)

    // Deferred tuning application (UI handlers don't hold the car pointer).
    if (m_carDirty && car) { car->applyTuning(m_composed.tuning); m_carDirty = false; }

    // ---- Dyno pull sweep. ----
    if (m_pullT >= 0.0f) {
        m_pullT += dt / 4.5f;                   // 4.5 s sweep
        // LIMIT POP: an unsafe tune lets go near the top of the sweep.
        if (!m_pullPopped && m_composed.willPop && m_pullT >= 0.78f) {
            m_pullPopped = true;
            if (m_build) m_build->engineDamaged = true;
            applyAndSave(car);
            if (audio && bangSfx.valid()) audio->playSound2D(bangSfx, 1.0f, 0.55f);
            m_status = "LIMIT POP! ENGINE DAMAGED — R TO REPAIR";
            x3::logInfo("[perfshop] DYNO LIMIT POP (knock " + std::to_string(m_composed.knockIndex) +
                        " >= " + std::to_string(m_composed.knockLimit) + ")");
        }
        if (m_pullT >= 1.0f) {
            m_pullT = -1.0f;
            m_havePull = true;
            m_lastPeakTq = m_composed.peakTorque;
            m_lastPeakKw = m_composed.peakPowerKw;
            if (!m_pullPopped) {
                char sb[96];
                std::snprintf(sb, sizeof(sb), "PULL DONE  %.0f Nm / %.0f kW", m_lastPeakTq, m_lastPeakKw);
                m_status = sb;
            }
        }
        // Re-bake the trace at ~12 Hz while sweeping.
        m_bakeCooldown -= dt;
        if (m_bakeCooldown <= 0.0f) { m_texDirty = true; m_bakeCooldown = 1.0f / 12.0f; }
    }

    if (m_texDirty && m_device) bakeTerminal(*m_device);
}

// ===========================================================================
// The terminal glass bake (PARTS / DYNO screens).
// ===========================================================================
void PerfShop::bakeTerminal(x3::rhi::IRenderDevice& device) {
    m_texDirty = false;
    const uint32_t N = m_texN;
    Canvas c(N, N);
    const float fn = (float)N;
    auto P = [&](float f) { return f * fn; };

    // Dark blue glass base: gradient + scanlines + grid + vignette (holo style).
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            const float u = (x + 0.5f) / fn, v = (y + 0.5f) / fn;
            float br = 0.04f, bg = 0.10f, bb = 0.22f;
            const float grad = 1.0f - v * 0.5f;
            br *= grad; bg *= grad; bb *= grad;
            const float scan = 0.80f + 0.20f * ((y % 4u) < 2u ? 1.0f : 0.6f);
            br *= scan; bg *= scan; bb *= scan;
            if ((x % 32u) < 1u || (y % 32u) < 1u) { bg += 0.05f; bb += 0.09f; }
            const float cx = u - 0.5f, cy = v - 0.5f;
            const float vig = 1.0f - 0.4f * (cx*cx + cy*cy) * 2.0f;
            c.set((int)x, (int)y, br * vig, bg * vig, bb * vig);
        }

    const float CY[3] = { 0.55f, 1.30f, 1.45f };   // hot cyan
    const float WT[3] = { 1.30f, 1.55f, 1.65f };   // hot white-cyan
    const float AM[3] = { 1.55f, 1.05f, 0.35f };   // amber
    const float MG[3] = { 1.45f, 0.40f, 1.00f };   // magenta accent
    const float th = std::max(1.3f, fn / 512.0f);

    // Frame + header.
    rectFrame(c, P(0.03f), P(0.03f), P(0.97f), P(0.97f), CY[0],CY[1],CY[2], 0.9f, th*1.4f);
    char hb[96];
    if (m_build) std::snprintf(hb, sizeof(hb), "CREDITS %d", m_build->credits);
    else         std::snprintf(hb, sizeof(hb), "CREDITS ----");
    drawText(c, m_mode == Mode::Parts ? "LATE NIGHT SPEED // PARTS" : "LATE NIGHT SPEED // DYNO",
             P(0.06f), P(0.045f), P(0.038f), WT[0],WT[1],WT[2], 1.0f);
    drawText(c, hb, P(0.94f) - textWidth(hb, P(0.030f)), P(0.050f), P(0.030f), AM[0],AM[1],AM[2], 1.0f);
    line(c, P(0.05f), P(0.105f), P(0.95f), P(0.105f), WT[0],WT[1],WT[2], 0.9f, th*1.3f);

    if (m_mode == Mode::Parts && m_catalog) {
        // ---- LEFT: categories. ----
        const auto& cats = m_catalog->categories();
        float y = P(0.14f);
        const float rowH = P(0.058f);
        for (int i = 0; i < (int)cats.size(); ++i) {
            const bool cur = (i == m_catCursor);
            if (cur) rectFill(c, P(0.045f), y - P(0.006f), P(0.345f), y + rowH - P(0.018f),
                              CY[0]*0.18f, CY[1]*0.18f, CY[2]*0.18f, 1.0f);
            const std::string* inst = m_build ? m_build->installedIn(cats[i].id) : nullptr;
            if (inst) plot(c, P(0.062f), y + rowH*0.36f, AM[0],AM[1],AM[2], 6.0f); // installed dot
            const float* col = cur && m_focus == Focus::Categories ? WT : CY;
            drawText(c, cats[i].label, P(0.085f), y, P(0.030f), col[0],col[1],col[2],
                     cur ? 1.0f : 0.75f);
            y += rowH;
        }
        line(c, P(0.36f), P(0.13f), P(0.36f), P(0.86f), CY[0],CY[1],CY[2], 0.3f, th);

        // ---- RIGHT: parts of the selected category. ----
        if (m_catCursor >= 0 && m_catCursor < (int)cats.size()) {
            const auto parts = m_catalog->inCategory(cats[m_catCursor].id);
            float py = P(0.14f);
            const float prowH = P(0.105f);
            for (int i = 0; i < (int)parts.size(); ++i) {
                const vehparts::Part* p = parts[i];
                const bool cur = (m_focus == Focus::Parts && i == m_partCursor);
                if (cur) rectFill(c, P(0.385f), py - P(0.008f), P(0.955f), py + prowH - P(0.024f),
                                  CY[0]*0.20f, CY[1]*0.20f, CY[2]*0.20f, 1.0f);
                const std::string* inst = m_build ? m_build->installedIn(p->category) : nullptr;
                const bool isInstalled = inst && *inst == p->id;
                const float* col = isInstalled ? AM : (cur ? WT : CY);
                drawText(c, tierTag(p->tier) + "  " + p->name, P(0.40f), py, P(0.030f),
                         col[0],col[1],col[2], cur || isInstalled ? 1.0f : 0.8f);
                char pr[48];
                if (isInstalled) std::snprintf(pr, sizeof(pr), "[INSTALLED]");
                else             std::snprintf(pr, sizeof(pr), "%d CR", p->price);
                drawText(c, pr, P(0.94f) - textWidth(pr, P(0.026f)), py + P(0.002f), P(0.026f),
                         isInstalled ? AM[0] : CY[0], isInstalled ? AM[1] : CY[1],
                         isInstalled ? AM[2] : CY[2], 0.95f);
                drawText(c, statLine(*p), P(0.42f), py + P(0.040f), P(0.023f),
                         CY[0],CY[1],CY[2], 0.55f);
                py += prowH;
            }
        }
        drawText(c, m_focus == Focus::Categories
                     ? "ARROWS pick category  ENTER open  TAB dyno  W drive out"
                     : "ARROWS pick part  ENTER buy/sell  BKSP back  TAB dyno",
                 P(0.06f), P(0.915f), P(0.024f), CY[0],CY[1],CY[2], 0.7f);
    } else {
        // ====================== DYNO ======================
        // ---- Sliders (left column). ----
        const char* names[3] = { "BOOST", "FUEL", "TIMING" };
        float vals[3] = { 0, 1, 0 }, mins[3] = { 0, 0.85f, 0 }, maxs[3] = { 1, 1.20f, 1 };
        float safes[3] = { -1, -1, -1 };
        if (m_build) { vals[0]=m_build->tune.boost; vals[1]=m_build->tune.fuel; vals[2]=m_build->tune.timing; }
        const bool hasEcu = m_composed.ecuMaxBoost > 0.0f;
        if (hasEcu && m_catalog && m_build) {
            maxs[0] = m_composed.ecuMaxBoost;
            if (const std::string* eid = m_build->installedIn("ecu")) {
                if (const vehparts::Part* ecu = m_catalog->find(*eid)) {
                    float icBonus = 0.0f;
                    if (const std::string* icId = m_build->installedIn("intercooler"))
                        if (const vehparts::Part* ic = m_catalog->find(*icId)) icBonus = ic->safeBoostBonus;
                    safes[0] = ecu->safeBoost + icBonus;
                    safes[1] = ecu->safeLean;
                    safes[2] = ecu->safeTiming;
                }
            }
        }
        float sy = P(0.16f);
        for (int i = 0; i < 3; ++i) {
            drawText(c, names[i], P(0.06f), sy, P(0.026f), CY[0],CY[1],CY[2], 0.9f);
            char vb[32];
            if (i == 0) std::snprintf(vb, sizeof(vb), "%.1f bar", vals[i]);
            else if (i == 1) std::snprintf(vb, sizeof(vb), "%.2f", vals[i]);
            else std::snprintf(vb, sizeof(vb), "%.2f adv", vals[i]);
            drawText(c, vb, P(0.155f), sy, P(0.026f), WT[0],WT[1],WT[2], 0.9f);
            // track + fill + safe tick
            const float bx0 = P(0.06f), bx1 = P(0.26f), by = sy + P(0.045f);
            line(c, bx0, by, bx1, by, CY[0],CY[1],CY[2], 0.30f, th*2.2f);
            const float frac = std::clamp((vals[i]-mins[i]) / std::max(1e-4f, maxs[i]-mins[i]), 0.0f, 1.0f);
            const bool over = safes[i] >= 0.0f && vals[i] > safes[i] + 1e-4f;
            line(c, bx0, by, bx0 + (bx1-bx0)*frac, by,
                 over ? 1.8f : CY[0], over ? 0.3f : CY[1], over ? 0.25f : CY[2], 0.95f, th*2.2f);
            if (safes[i] >= mins[i]) {
                const float sf = std::clamp((safes[i]-mins[i]) / std::max(1e-4f, maxs[i]-mins[i]), 0.0f, 1.0f);
                line(c, bx0 + (bx1-bx0)*sf, by - P(0.012f), bx0 + (bx1-bx0)*sf, by + P(0.012f),
                     AM[0],AM[1],AM[2], 1.0f, th*1.4f);
            }
            sy += P(0.085f);
        }
        if (!hasEcu)
            drawText(c, "INSTALL AN ECU TO TUNE", P(0.06f), sy, P(0.024f), AM[0],AM[1],AM[2], 0.9f);

        // ---- Knock gauge. ----
        {
            const float kx0 = P(0.06f), kx1 = P(0.26f), ky = P(0.46f);
            drawText(c, "KNOCK", kx0, ky - P(0.034f), P(0.024f), CY[0],CY[1],CY[2], 0.8f);
            line(c, kx0, ky, kx1, ky, CY[0],CY[1],CY[2], 0.30f, th*2.2f);
            if (hasEcu && m_composed.knockLimit > 0.0f) {
                const float kf = std::clamp(m_composed.knockIndex / m_composed.knockLimit, 0.0f, 1.0f);
                line(c, kx0, ky, kx0 + (kx1-kx0)*kf, ky,
                     0.4f + 1.4f*kf, 1.4f - 1.1f*kf, 0.3f, 0.95f, th*2.2f);
            }
            if (m_composed.willPop)
                drawText(c, "!! WILL POP !!", kx0, ky + P(0.015f), P(0.026f), 1.9f, 0.3f, 0.25f, 1.0f);
        }

        // ---- Status block: damage / nitrous. ----
        {
            float dy = P(0.56f);
            if (m_build && m_build->engineDamaged) {
                char db[64]; std::snprintf(db, sizeof(db), "ENGINE DAMAGED  R repair %d CR",
                                           std::max(500, m_composed.repairCost));
                drawText(c, db, P(0.06f), dy, P(0.024f), 1.9f, 0.35f, 0.3f, 1.0f);
                dy += P(0.05f);
            }
            if (m_composed.nitrousMult > 0.0f && m_build) {
                char nb[64]; std::snprintf(nb, sizeof(nb), "NOS %.1f/%.0fs  N refill %d CR",
                                           m_build->nitrousRemaining, m_composed.nitrousTankS,
                                           std::max(50, m_composed.nitrousRefillCost));
                drawText(c, nb, P(0.06f), dy, P(0.024f), MG[0],MG[1],MG[2], 0.9f);
            }
        }

        // ---- Graph: torque (cyan) + power (amber) vs RPM, drawn to pullT. ----
        const float gx0 = P(0.32f), gx1 = P(0.94f), gy0 = P(0.20f), gy1 = P(0.74f);
        rectFrame(c, gx0, gy0, gx1, gy1, CY[0],CY[1],CY[2], 0.5f, th);
        for (int i = 1; i < 4; ++i) {  // grid
            const float gy = gy0 + (gy1-gy0) * (float)i / 4.0f;
            line(c, gx0, gy, gx1, gy, CY[0],CY[1],CY[2], 0.12f, th);
            const float gx = gx0 + (gx1-gx0) * (float)i / 4.0f;
            line(c, gx, gy0, gx, gy1, CY[0],CY[1],CY[2], 0.12f, th);
        }
        // Y scale: headroom over the current peak torque / power.
        const float tqMax = std::max(100.0f, m_composed.peakTorque * 1.15f);
        const float kwMax = std::max(50.0f,  m_composed.peakPowerKw * 1.15f);
        const float sweepEnd = m_pullT >= 0.0f ? m_pullT : (m_havePull ? 1.0f : 0.0f);
        const int   nSamp = 96;
        float pTqX = 0, pTqY = 0, pKwX = 0, pKwY = 0;
        for (int i = 0; i <= (int)(sweepEnd * nSamp); ++i) {
            const float t = (float)i / (float)nSamp;
            // The damaged-pop kink: after a pop mid-pull the trace falls off.
            float tq = m_composed.torqueAtRpmFrac(t);
            float kw = m_composed.powerKwAtRpmFrac(t);
            const float x = gx0 + (gx1-gx0) * t;
            const float ty = gy1 - (gy1-gy0) * std::clamp(tq / tqMax, 0.0f, 1.0f);
            const float ky = gy1 - (gy1-gy0) * std::clamp(kw / kwMax, 0.0f, 1.0f);
            if (i > 0) {
                line(c, pTqX, pTqY, x, ty, CY[0],CY[1],CY[2], 0.95f, th*1.5f);
                line(c, pKwX, pKwY, x, ky, AM[0],AM[1],AM[2], 0.85f, th*1.3f);
            }
            pTqX = x; pTqY = ty; pKwX = x; pKwY = ky;
        }
        // Sweep needle.
        if (m_pullT >= 0.0f) {
            const float nx = gx0 + (gx1-gx0) * std::clamp(m_pullT, 0.0f, 1.0f);
            line(c, nx, gy0, nx, gy1, WT[0],WT[1],WT[2], 0.8f, th*1.4f);
            char rb[32];
            const float rpm = 1000.0f + (m_composed.maxRpm - 1000.0f) * std::clamp(m_pullT, 0.0f, 1.0f);
            std::snprintf(rb, sizeof(rb), "%.0f RPM", rpm);
            drawText(c, rb, std::min(nx + P(0.01f), gx1 - P(0.12f)), gy0 + P(0.012f), P(0.024f),
                     WT[0],WT[1],WT[2], 0.9f);
        }
        // Legend + peaks.
        drawText(c, "TQ Nm", gx0 + P(0.012f), gy0 - P(0.036f), P(0.022f), CY[0],CY[1],CY[2], 0.85f);
        drawText(c, "PWR kW", gx0 + P(0.10f), gy0 - P(0.036f), P(0.022f), AM[0],AM[1],AM[2], 0.85f);
        char pk[96];
        std::snprintf(pk, sizeof(pk), "PEAK %.0f Nm @%.0f   %.0f kW @%.0f",
                      m_composed.peakTorque, m_composed.peakTorqueRpm,
                      m_composed.peakPowerKw, m_composed.peakPowerRpm);
        drawText(c, pk, gx0, gy1 + P(0.018f), P(0.026f), WT[0],WT[1],WT[2], 0.95f);

        drawText(c, "1/2 boost  3/4 fuel  5/6 timing  SPACE pull  TAB parts",
                 P(0.06f), P(0.915f), P(0.024f), CY[0],CY[1],CY[2], 0.7f);
    }

    // Status line (both modes).
    drawText(c, m_status, P(0.06f), P(0.865f), P(0.026f), AM[0],AM[1],AM[2], 0.95f);

    // ---- Upload + (re)point the glass entity. ----
    std::vector<uint8_t> rgba = canvasToRGBA(c);
    x3::rhi::TextureHandle nt = device.createTexture(rgba.data(), N, N, /*srgb*/true);
    if (!m_scene) { if (m_screenTex.id) device.destroyTexture(m_screenTex); m_screenTex = nt; return; }
    if (m_screenSlot == kNoLink) {
        // First bake: create the glass screen entity on the back wall, facing the lift.
        x3::prims::PrimMesh q = makeUnitQuad();
        Entity e;
        e.mesh = device.createMesh(q.verts.data(), (uint32_t)q.verts.size(),
                                   q.index.data(), (uint32_t)q.index.size());
        m_extraMeshes.push_back(e.mesh);
        e.tex = nt;
        e.baseColor[0]=1; e.baseColor[1]=1; e.baseColor[2]=1; e.baseColor[3]=0.96f;
        // Low flat glow (the engine emissive is a uniform add — high values flood
        // the baked UI into a cyan slab); the texture's hot strokes carry the read.
        e.emissive[0]=0.10f; e.emissive[1]=0.24f; e.emissive[2]=0.30f; e.emissive[3]=0.55f;
        e.transparent = true;
        e.glass.opacity = 0.96f;
        e.glass.tint[0]=0.7f; e.glass.tint[1]=0.9f; e.glass.tint[2]=1.0f;
        e.glass.roughness = 0.0f; e.glass.refraction = 0.01f; e.glass.specular = 0.5f;
        e.tag = (uint32_t)Tag::Prop;
        e.transform[0] = 3.2f; e.transform[5] = 2.0f; e.transform[10] = 1.0f;
        e.transform[12] = m_site[0] - 2.8f;
        e.transform[13] = m_site[1] + 2.3f;
        e.transform[14] = m_site[2] - 6.50f;
        m_screenSlot = m_scene->add(e);
        m_screenTex = nt;
    } else {
        Entity& e = m_scene->get(m_screenSlot);
        if (m_screenTex.id) device.destroyTexture(m_screenTex);
        m_screenTex = nt;
        e.tex = nt;
    }
}

} // namespace x3::game
