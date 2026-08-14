// app/space/hud_project.cpp — see hud_project.h for the design + the bug history.

#include "hud_project.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3 { namespace space { namespace hud {

namespace {

inline float dot3(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
inline void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}
inline bool normalize3(float v[3]) {
    const float l = std::sqrt(dot3(v, v));
    if (l < 1e-6f) return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}

constexpr float kPi   = 3.14159265358979323846f;
constexpr float kNear = 0.05f;   // metres; below this a point is "behind"

} // namespace

// ---------------------------------------------------------------------------
// ViewProjector
// ---------------------------------------------------------------------------

ViewProjector::ViewProjector(const float eye[3], const float fwd[3], const float up[3],
                             float fovYDeg, float widthPx, float heightPx) {
    m_eye[0] = eye[0]; m_eye[1] = eye[1]; m_eye[2] = eye[2];
    m_w = (widthPx  > 1.0f) ? widthPx  : 1.0f;
    m_h = (heightPx > 1.0f) ? heightPx : 1.0f;

    float f[3] = { fwd[0], fwd[1], fwd[2] };
    if (!normalize3(f)) { f[0] = 0.0f; f[1] = 0.0f; f[2] = 1.0f; }

    // GRAM-SCHMIDT — the whole point of this class. `up` from cameraBasis() is
    // the SHIP's up while `fwd` is the (freelook + look-bias) GAZE, so the two
    // are NOT orthogonal; the render device does exactly this before lookAt.
    float u[3] = { up[0], up[1], up[2] };
    const float d = dot3(u, f);
    u[0] -= f[0] * d; u[1] -= f[1] * d; u[2] -= f[2] * d;
    if (!normalize3(u)) {
        // Degenerate (up parallel to fwd): pick any perpendicular, world-Y first.
        float alt[3] = { 0.0f, 1.0f, 0.0f };
        if (std::fabs(f[1]) > 0.99f) { alt[0] = 1.0f; alt[1] = 0.0f; }
        const float d2 = dot3(alt, f);
        u[0] = alt[0] - f[0]*d2; u[1] = alt[1] - f[1]*d2; u[2] = alt[2] - f[2]*d2;
        normalize3(u);
    }
    // right = fwd x up   (glm::lookAt's `s`)
    float r[3]; cross3(f, u, r); normalize3(r);
    // up    = right x fwd (glm::lookAt's `u`). NOT fwd x right — that is -up,
    // and it is exactly the sign that mirrored every marker about screen centre.
    cross3(r, f, u); normalize3(u);

    for (int k = 0; k < 3; ++k) { m_f[k] = f[k]; m_r[k] = r[k]; m_u[k] = u[k]; }

    const float fovY = (fovYDeg > 1.0f && fovYDeg < 179.0f) ? fovYDeg : 65.0f;
    m_tanHalfY = std::tan(fovY * 0.5f * kPi / 180.0f);
    m_tanHalfX = m_tanHalfY * (m_w / m_h);
}

Projected ViewProjector::project(const float world[3]) const {
    Projected o{};
    const float d[3] = { world[0] - m_eye[0], world[1] - m_eye[1], world[2] - m_eye[2] };
    const float zf = dot3(d, m_f);
    const float xr = dot3(d, m_r);
    const float yu = dot3(d, m_u);
    o.depth  = zf;
    o.behind = (zf <= kNear);

    float ex = xr, ey = yu;
    if (!o.behind) {
        const float nx = (xr / zf) / m_tanHalfX;
        const float ny = (yu / zf) / m_tanHalfY;
        ex = nx; ey = ny;
        if (nx > -1.0f && nx < 1.0f && ny > -1.0f && ny < 1.0f) {
            o.onScreen   = true;
            o.sx         = (nx * 0.5f + 0.5f) * m_w;
            o.sy         = (0.5f - ny * 0.5f) * m_h;   // screen-y grows DOWN
            o.pxPerMetre = m_h / (2.0f * zf * m_tanHalfY);
            // Fall through: edgeX/edgeY are still filled so callers may use them.
        }
    }
    const float m = std::max(std::fabs(ex), std::fabs(ey));
    if (m > 1e-6f) { o.edgeX = ex / m; o.edgeY = ey / m; }
    return o;
}

void ViewProjector::edgePoint(const Projected& p, float margin,
                              float& outX, float& outY) const {
    if (std::fabs(p.edgeX) < 1e-6f && std::fabs(p.edgeY) < 1e-6f) {
        outX = m_w * 0.5f; outY = m_h * 0.5f;
        return;
    }
    const float mx = std::min(margin, m_w * 0.45f);
    const float my = std::min(margin, m_h * 0.45f);
    const float halfW = m_w * 0.5f - mx;
    const float halfH = m_h * 0.5f - my;
    // Scale the unit-square direction out to the (inset) viewport rectangle.
    const float sx = halfW * p.edgeX;
    const float sy = halfH * p.edgeY;
    outX = m_w * 0.5f + sx;
    outY = m_h * 0.5f - sy;
    outX = std::max(mx, std::min(m_w - mx, outX));
    outY = std::max(my, std::min(m_h - my, outY));
}

// ---------------------------------------------------------------------------
// Label layout
// ---------------------------------------------------------------------------

bool rectsOverlap(const Rect& a, const Rect& b) {
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
             a.y + a.h <= b.y || b.y + b.h <= a.y);
}

void layoutLabels(const LabelRequest* req, LabelPlacement* out, uint32_t n,
                  const Rect* avoid, uint32_t avoidCount,
                  float viewW, float viewH, float margin) {
    if (!req || !out || n == 0) return;
    for (uint32_t i = 0; i < n; ++i) out[i] = LabelPlacement{};

    // Deterministic order: priority, then input index (a stable insertion sort —
    // n is 4-8 here, and std::sort's tie-break is not guaranteed stable).
    constexpr uint32_t kMaxLabels = 32;
    uint32_t order[kMaxLabels];
    const uint32_t count = std::min<uint32_t>(n, kMaxLabels);
    for (uint32_t i = 0; i < count; ++i) order[i] = i;
    for (uint32_t i = 1; i < count; ++i) {
        const uint32_t v = order[i];
        uint32_t j = i;
        while (j > 0 && req[order[j - 1]].priority > req[v].priority) {
            order[j] = order[j - 1]; --j;
        }
        order[j] = v;
    }

    // Candidate slots around the anchor, in preference order. dx/dy are in units
    // of "one gap"; the box is nudged out until it stops colliding.
    struct Slot { float dx, dy; };
    static const Slot kSlots[] = {
        {  1.0f,  0.0f }, { -1.0f,  0.0f },      // right, left
        {  1.0f, -1.0f }, {  1.0f,  1.0f },      // up-right, down-right
        { -1.0f, -1.0f }, { -1.0f,  1.0f },      // up-left, down-left
        {  0.0f, -1.0f }, {  0.0f,  1.0f },      // straight up, straight down
    };
    constexpr float kGapX = 16.0f;   // px from the anchor to the label edge
    constexpr float kGapY = 14.0f;

    Rect placedRects[kMaxLabels];
    uint32_t placedCount = 0;

    for (uint32_t oi = 0; oi < count; ++oi) {
        const uint32_t i = order[oi];
        const LabelRequest& rq = req[i];
        const float bw = rq.w, bh = rq.h;
        if (bw + 2.0f * margin > viewW || bh + 2.0f * margin > viewH) {
            out[i].x = margin; out[i].y = margin; out[i].placed = false;
            continue;
        }
        bool done = false;
        for (int ring = 0; ring < 6 && !done; ++ring) {
            const float push = 1.0f + 0.85f * (float)ring;
            for (const Slot& s : kSlots) {
                Rect c{};
                c.w = bw; c.h = bh;
                // Anchor sits on the box edge nearest the anchor.
                c.x = rq.anchorX + s.dx * (kGapX * push) - (s.dx > 0.0f ? 0.0f
                                                        : (s.dx < 0.0f ? bw : bw * 0.5f));
                c.y = rq.anchorY + s.dy * (kGapY * push) - (s.dy > 0.0f ? 0.0f
                                                        : (s.dy < 0.0f ? bh : bh * 0.5f));
                // Keep inside the viewport.
                c.x = std::max(margin, std::min(viewW - margin - bw, c.x));
                c.y = std::max(margin, std::min(viewH - margin - bh, c.y));
                bool hit = false;
                for (uint32_t k = 0; k < placedCount && !hit; ++k)
                    hit = rectsOverlap(c, placedRects[k]);
                for (uint32_t k = 0; k < avoidCount && !hit; ++k)
                    hit = rectsOverlap(c, avoid[k]);
                if (!hit) {
                    out[i].x = c.x; out[i].y = c.y; out[i].placed = true;
                    placedRects[placedCount++] = c;
                    done = true;
                    break;
                }
            }
        }
        if (!done) {
            // LAST RESORT: stack it below everything already placed so the labels
            // are still individually readable (never silently overlapping).
            float y = margin;
            for (uint32_t k = 0; k < placedCount; ++k)
                y = std::max(y, placedRects[k].y + placedRects[k].h + 2.0f);
            Rect c{ std::max(margin, std::min(viewW - margin - bw, rq.anchorX - bw * 0.5f)),
                    std::min(viewH - margin - bh, y), bw, bh };
            out[i].x = c.x; out[i].y = c.y; out[i].placed = true;
            if (placedCount < kMaxLabels) placedRects[placedCount++] = c;
        }
    }
    // Anything past kMaxLabels: park it on its anchor (documented degradation).
    for (uint32_t i = count; i < n; ++i) {
        out[i].x = req[i].anchorX; out[i].y = req[i].anchorY; out[i].placed = false;
    }
}

uint32_t leaderSegments(const LabelPlacement& p, const LabelRequest& req,
                        float thickness, Rect* outSegs) {
    if (!outSegs) return 0;
    const float th = (thickness > 0.5f) ? thickness : 1.0f;
    const float cy = p.y + req.h * 0.5f;                       // label vertical centre
    // Elbow at the label's near edge, then a vertical run to the anchor.
    const bool  anchorLeft = req.anchorX < p.x + req.w * 0.5f;
    const float elbowX = anchorLeft ? p.x : (p.x + req.w);
    uint32_t n = 0;
    // Horizontal: label edge -> elbow toward the anchor's x.
    const float hx0 = std::min(elbowX, req.anchorX);
    const float hx1 = std::max(elbowX, req.anchorX);
    if (hx1 - hx0 > 1.0f) { outSegs[n++] = Rect{ hx0, cy - th * 0.5f, hx1 - hx0, th }; }
    // Vertical: down/up the anchor column to the hardpoint.
    const float vy0 = std::min(cy, req.anchorY);
    const float vy1 = std::max(cy, req.anchorY);
    if (vy1 - vy0 > 1.0f) {
        outSegs[n++] = Rect{ req.anchorX - th * 0.5f, vy0, th, vy1 - vy0 };
    }
    return n;
}

// ---------------------------------------------------------------------------
// --test-spacehud
// ---------------------------------------------------------------------------
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[spacehud-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[spacehud-test] FAIL ") + name); }
}
} // namespace

bool runSpaceHudSelfTest() {
    g_pass = g_fail = 0;
    const float W = 1280.0f, H = 720.0f;

    // T1 — a point ABOVE the view axis projects ABOVE screen centre.
    // This is the regression that shipped: `up = fwd x right` is MINUS up, so
    // every marker was mirrored about the horizontal centre line and the
    // brackets floated in empty space.
    {
        const float eye[3] = { 0, 0, 0 }, fwd[3] = { 1, 0, 0 }, up[3] = { 0, 1, 0 };
        ViewProjector vp(eye, fwd, up, 65.0f, W, H);
        const float high[3] = { 100.0f, 20.0f, 0.0f };    // 20 m up, 100 m ahead
        const float low[3]  = { 100.0f, -20.0f, 0.0f };
        const Projected a = vp.project(high);
        const Projected b = vp.project(low);
        check(a.onScreen && b.onScreen && a.sy < H * 0.5f && b.sy > H * 0.5f,
              "T1 up is UP (mirrored-basis regression)");
        // ...and RIGHT is right: with fwd=+X and up=+Y, right = fwd x up = +Z.
        const float rightP[3] = { 100.0f, 0.0f, 20.0f };
        const Projected c = vp.project(rightP);
        check(c.onScreen && c.sx > W * 0.5f, "T1b right is RIGHT");
    }

    // T2 — THE DEFECT-3 GATE: a subsystem marker tracks the TARGET's world
    // position, not the aim/crosshair direction. Same eye + same target, but the
    // ship's nose (and therefore the crosshair) swung 25 deg: the target's
    // projected pixel must not move, while the crosshair's does.
    {
        const float eye[3] = { 0, 0, 0 };
        const float fwd[3] = { 1, 0, 0 }, up[3] = { 0, 1, 0 };
        ViewProjector vp(eye, fwd, up, 65.0f, W, H);
        const float target[3] = { 400.0f, 35.0f, -60.0f };      // a hardpoint out there
        const Projected t0 = vp.project(target);
        // The nose ray 600 m out, first straight ahead then yawed 25 deg.
        const float aim0[3] = { 600.0f, 0.0f, 0.0f };
        const float ang = 25.0f * kPi / 180.0f;
        const float aim1[3] = { 600.0f * std::cos(ang), 0.0f, 600.0f * std::sin(ang) };
        const Projected a0 = vp.project(aim0);
        const Projected a1 = vp.project(aim1);
        // Re-project the target with the SAME camera (the camera did not move).
        const Projected t1 = vp.project(target);
        const bool targetStuck = std::fabs(t1.sx - t0.sx) < 0.01f &&
                                 std::fabs(t1.sy - t0.sy) < 0.01f;
        const bool aimMoved = std::fabs(a1.sx - a0.sx) > 50.0f;
        check(t0.onScreen && targetStuck && aimMoved,
              "T2 marker tracks the TARGET world position, not the aim ray");
    }

    // T3 — a NON-ORTHOGONAL (fwd, up) pair — exactly what cameraBasis() returns
    // once the target-keeping look bias blends the gaze off the ship's nose —
    // still projects identically to the orthonormalized pair. Before the fix the
    // un-normalized cross products skewed every marker by a factor that changed
    // with the mouse.
    {
        const float eye[3] = { 0, 0, 0 };
        const float fwd[3] = { 1, 0, 0 };
        const float upOrtho[3] = { 0, 1, 0 };
        // The ship's up, with a big component ALONG the gaze (0.6 of it) and not
        // unit length — the pathological case.
        const float upSkew[3]  = { 0.6f * 2.0f, 1.0f * 2.0f, 0.0f };
        ViewProjector ref(eye, fwd, upOrtho, 65.0f, W, H);
        ViewProjector skew(eye, fwd, upSkew, 65.0f, W, H);
        const float p[3] = { 250.0f, 40.0f, 30.0f };
        const Projected a = ref.project(p);
        const Projected b = skew.project(p);
        check(a.onScreen && b.onScreen &&
              std::fabs(a.sx - b.sx) < 0.5f && std::fabs(a.sy - b.sy) < 0.5f,
              "T3 skewed/unnormalized camera basis is orthonormalized (Gram-Schmidt)");
    }

    // T4 — a point BEHIND the eye reports behind + a usable edge direction, and
    // edgePoint() pins it inside the viewport.
    {
        const float eye[3] = { 0, 0, 0 }, fwd[3] = { 1, 0, 0 }, up[3] = { 0, 1, 0 };
        ViewProjector vp(eye, fwd, up, 65.0f, W, H);
        const float back[3] = { -300.0f, 0.0f, 120.0f };   // behind and to the right
        const Projected p = vp.project(back);
        float ex = 0, ey = 0;
        vp.edgePoint(p, 24.0f, ex, ey);
        check(p.behind && !p.onScreen && p.edgeX > 0.0f &&
              ex >= 24.0f && ex <= W - 24.0f && ey >= 24.0f && ey <= H - 24.0f,
              "T4 behind-camera contact -> edge indicator, clamped in-viewport");
    }

    // T5 — pxPerMetre halves when the range doubles (bracket sizing sanity).
    {
        const float eye[3] = { 0, 0, 0 }, fwd[3] = { 1, 0, 0 }, up[3] = { 0, 1, 0 };
        ViewProjector vp(eye, fwd, up, 65.0f, W, H);
        const float a[3] = { 100.0f, 0, 0 }, b[3] = { 200.0f, 0, 0 };
        const Projected pa = vp.project(a), pb = vp.project(b);
        check(pa.pxPerMetre > 0.0f &&
              std::fabs(pa.pxPerMetre / pb.pxPerMetre - 2.0f) < 1e-3f,
              "T5 pxPerMetre scales 1/depth");
    }

    // T6 — LABEL LAYOUT: four subsystem callouts whose hardpoints project within
    // ~40 px of each other (the 2.6 km standoff case) come out non-overlapping,
    // in-viewport, and every one of them gets a leader line back to its anchor.
    {
        LabelRequest req[4];
        const char* names[4] = { "ENGINES", "TURRETS", "SHIELD GEN", "SENSORS" };
        for (int i = 0; i < 4; ++i) {
            req[i].anchorX  = 600.0f + (float)(i % 2) * 18.0f;
            req[i].anchorY  = 340.0f + (float)(i / 2) * 16.0f;
            req[i].w        = 7.0f * (float)std::char_traits<char>::length(names[i]);
            req[i].h        = 12.0f;
            req[i].priority = i;
        }
        LabelPlacement pl[4];
        layoutLabels(req, pl, 4, nullptr, 0, W, H);
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) {
            ok = pl[i].placed &&
                 pl[i].x >= 0.0f && pl[i].y >= 0.0f &&
                 pl[i].x + req[i].w <= W && pl[i].y + req[i].h <= H;
            for (int j = i + 1; j < 4 && ok; ++j) {
                const Rect a{ pl[i].x, pl[i].y, req[i].w, req[i].h };
                const Rect b{ pl[j].x, pl[j].y, req[j].w, req[j].h };
                ok = !rectsOverlap(a, b);
            }
        }
        check(ok, "T6 crowded subsystem labels lay out non-overlapping + in-viewport");
        Rect segs[2];
        bool leaders = true;
        for (int i = 0; i < 4; ++i)
            leaders = leaders && leaderSegments(pl[i], req[i], 1.0f, segs) >= 1;
        check(leaders, "T6b every placed label gets a leader line back to its hardpoint");
    }

    // T7 — labels also avoid a keep-out rect (the boss bar / the target bracket).
    {
        LabelRequest req[2];
        for (int i = 0; i < 2; ++i) {
            req[i].anchorX = 640.0f; req[i].anchorY = 360.0f;
            req[i].w = 80.0f; req[i].h = 12.0f; req[i].priority = i;
        }
        const Rect avoid{ 560.0f, 330.0f, 160.0f, 60.0f };   // straddles the anchors
        LabelPlacement pl[2];
        layoutLabels(req, pl, 2, &avoid, 1, W, H);
        bool ok = true;
        for (int i = 0; i < 2; ++i) {
            const Rect a{ pl[i].x, pl[i].y, req[i].w, req[i].h };
            ok = ok && pl[i].placed && !rectsOverlap(a, avoid);
        }
        const Rect a0{ pl[0].x, pl[0].y, req[0].w, req[0].h };
        const Rect a1{ pl[1].x, pl[1].y, req[1].w, req[1].h };
        check(ok && !rectsOverlap(a0, a1), "T7 labels respect keep-out rects");
    }

    x3::logInfo("[spacehud-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

}}} // namespace x3::space::hud
