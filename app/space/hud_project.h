// app/space/hud_project.h
//
// WORLD -> SCREEN projection for the space-combat HUD, plus collision-aware
// label layout. PURE (no GPU, no GLFW, no engine state) so it is testable
// headlessly — see runSpaceHudSelfTest() / --test-spacehud.
//
// WHY THIS EXISTS (the bug it was extracted to kill):
// The dogfight HUD used to build its own camera basis inline:
//     right = fwd x camUp
//     up    = fwd x right        <-- WRONG: that is MINUS up
// and it never normalized either axis nor orthogonalized camUp against fwd.
// SpacePilotController::cameraBasis() deliberately returns a GAZE (ship-forward
// blended with hold-to-freelook + the target-keeping look bias) together with
// the SHIP's up — two vectors that are NOT orthogonal whenever the assist is
// active, which is every frame there is a target. The render device fixes that
// up with Gram-Schmidt inside setCameraBasis(); the HUD did not. Net effect: the
// markers were mirrored vertically AND skewed by a factor that changed with
// every mouse movement — brackets floating in empty space, "MOVES WILDLY with
// the mouse" (owner playtest, 2026-08).
//
// So the ONE rule this header enforces: build the basis EXACTLY the way
// glm::lookAt (and therefore VulkanRenderDevice::setCameraBasis) builds it —
//     f = normalize(fwd)
//     u = normalize(up - f * dot(up, f))     (Gram-Schmidt)
//     r = f x u                              (== glm::lookAt's `s`)
//     u = r x f                              (== glm::lookAt's `u`)
// and project through the SAME vertical FOV + aspect the device rasters with.
#pragma once

#include <cstdint>

namespace x3 { namespace space { namespace hud {

// One world point resolved into HUD/framebuffer pixels.
struct Projected {
    bool  onScreen   = false;  // in front of the eye AND inside the frustum
    bool  behind     = false;  // at/behind the eye plane (depth <= near)
    float sx         = 0.0f;   // framebuffer px, origin TOP-LEFT (valid iff onScreen)
    float sy         = 0.0f;
    float depth      = 0.0f;   // camera-forward depth in metres (negative = behind)
    float pxPerMetre = 0.0f;   // screen px per world metre AT `depth` (0 when behind)
    // Unit-square direction toward the point for the off-screen indicator:
    // +x right, +y UP (screen-y is flipped when this is turned into pixels).
    // max(|edgeX|,|edgeY|) == 1 when resolvable, both 0 when degenerate.
    float edgeX      = 0.0f;
    float edgeY      = 0.0f;
};

// Projects world points into one framebuffer. Cheap to build (one Gram-Schmidt);
// build it once per frame and project as many points as you like.
class ViewProjector {
public:
    // `fwd` / `up` may be any non-parallel pair of non-unit vectors — exactly what
    // SpacePilotController::cameraBasis() hands out. fovYDeg is the VERTICAL fov
    // handed to setCamera/setCameraBasis (glm::perspective takes vertical fov).
    // widthPx/heightPx are FRAMEBUFFER pixels (the space drawHudQuad works in).
    ViewProjector(const float eye[3], const float fwd[3], const float up[3],
                  float fovYDeg, float widthPx, float heightPx);

    Projected project(const float world[3]) const;

    // Where to draw the off-screen indicator for `p`: pinned `margin` px inside
    // the viewport edge, along p.edgeX/edgeY. Safe (returns the screen centre)
    // when the direction is degenerate.
    void edgePoint(const Projected& p, float margin, float& outX, float& outY) const;

    float width()  const { return m_w; }
    float height() const { return m_h; }
    // The orthonormal view basis actually used (for tests / callers that need it).
    const float* forward() const { return m_f; }
    const float* right()   const { return m_r; }
    const float* upAxis()  const { return m_u; }

private:
    float m_eye[3]{};
    float m_f[3]{ 0, 0, 1 };
    float m_r[3]{ 1, 0, 0 };
    float m_u[3]{ 0, 1, 0 };
    float m_tanHalfY = 1.0f;
    float m_tanHalfX = 1.0f;
    float m_w = 1.0f, m_h = 1.0f;
};

// ---------------------------------------------------------------------------
// Collision-aware label layout (the subsystem callouts).
//
// Tim's frame had TURRETS / ENGINES / SENSORS / SHIELD GEN stacked on top of
// each other AND on the hull, at a range where the whole 450 m dreadnought is
// ~200 px wide. Subsystem targeting IS the feature, so the labels get laid out
// properly: each is pushed to the first candidate slot around its hardpoint that
// collides with nothing already placed, and a LEADER LINE ties it back.
// ---------------------------------------------------------------------------
struct Rect { float x = 0, y = 0, w = 0, h = 0; };   // top-left + size, px

struct LabelRequest {
    float anchorX = 0, anchorY = 0;   // the hardpoint, in px
    float w = 0, h = 0;               // the label box size, in px
    int   priority = 0;               // lower = placed first (gets the best slot)
};

struct LabelPlacement {
    float x = 0, y = 0;    // top-left of the placed box, px
    bool  placed = false;  // false only if the viewport is smaller than the box
};

// Deterministic (no rand, no time). Places `n` labels, honouring `priority` then
// input order, avoiding every previously-placed label and every `avoid` rect,
// clamped inside the viewport by `margin`. `out` must hold `n` entries.
void layoutLabels(const LabelRequest* req, LabelPlacement* out, uint32_t n,
                  const Rect* avoid, uint32_t avoidCount,
                  float viewW, float viewH, float margin = 6.0f);

// True iff the two rects overlap (touching edges do NOT count).
bool rectsOverlap(const Rect& a, const Rect& b);

// The elbow leader line from a placed label back to its anchor: a HORIZONTAL
// run at the label's vertical centre out to `elbowX`, then a VERTICAL run to the
// anchor. Two axis-aligned rects, because drawHudQuad has no rotated primitive.
// Returns the number of segments written (0, 1 or 2) into `outSegs`.
uint32_t leaderSegments(const LabelPlacement& p, const LabelRequest& req,
                        float thickness, Rect* outSegs);

// ---- --test-spacehud self-test (pure, no window/Vulkan) --------------------
// Covers the projection basis (the mirrored-up regression), aim-independence
// (the marker tracks the TARGET, not the crosshair), off-screen edge direction,
// and the label layout's non-overlap guarantee. Logs PASS/FAIL T#.
bool runSpaceHudSelfTest();

}}} // namespace x3::space::hud
