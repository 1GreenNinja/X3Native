// Combat FX: crosshair + shot tracers + muzzle flash. See app/fx.h.
//
// Clean-room: built from the IRenderDevice + Vec3 interfaces only. No id Tech /
// RBDOOM source consulted.
#include "fx.h"
#include "mesh_prims.h"

#include <cmath>

namespace x3::game {

namespace {

// Build a column-major 4x4 from a 3x3 basis (columns bx,by,bz), PER-AXIS scale
// (sx,sy,sz applied to the corresponding basis column), and translation t.
void composeTRS3(float m[16],
                 const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                 float sx, float sy, float sz, const x3::phys::Vec3& t) {
    m[0]  = bx.x * sx; m[1]  = bx.y * sx; m[2]  = bx.z * sx; m[3]  = 0.0f;
    m[4]  = by.x * sy; m[5]  = by.y * sy; m[6]  = by.z * sy; m[7]  = 0.0f;
    m[8]  = bz.x * sz; m[9]  = bz.y * sz; m[10] = bz.z * sz; m[11] = 0.0f;
    m[12] = t.x;       m[13] = t.y;       m[14] = t.z;       m[15] = 1.0f;
}

x3::phys::Vec3 cross(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return x3::phys::Vec3{ a.y * b.z - a.z * b.y,
                           a.z * b.x - a.x * b.z,
                           a.x * b.y - a.y * b.x };
}

x3::phys::Vec3 normalize(const x3::phys::Vec3& v) {
    float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l < 1e-6f) return x3::phys::Vec3{ 0.0f, 0.0f, 1.0f };
    return x3::phys::Vec3{ v.x / l, v.y / l, v.z / l };
}

} // namespace

// ---------------------------------------------------------------------------
// init / shutdown: own one shared centered unit box (half-extent 0.5).
// ---------------------------------------------------------------------------
void CombatFx::init(x3::rhi::IRenderDevice& device) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f);
    m_box = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                              geo.index.data(), (uint32_t)geo.index.size());
    for (auto& t : m_tracers) t.life = 0.0f;
    m_muzzleFlash = 0.0f;
    m_nextTracer = 0;
}

void CombatFx::shutdown(x3::rhi::IRenderDevice& device) {
    if (m_box.valid()) {
        device.destroyMesh(m_box);
        m_box = x3::rhi::MeshHandle{};
    }
}

// ---------------------------------------------------------------------------
// addTracer: drop a beam into the pool (round-robin) + light the muzzle flash.
// ---------------------------------------------------------------------------
void CombatFx::addTracer(const x3::phys::Vec3& from, const x3::phys::Vec3& to) {
    Tracer& t = m_tracers[m_nextTracer];
    t.from = from;
    t.to   = to;
    t.life = kTracerTime;
    m_nextTracer = (m_nextTracer + 1) % kMaxTracers;

    m_muzzlePos   = from;
    m_muzzleFlash = kMuzzleFlashTime;
}

// ---------------------------------------------------------------------------
// update: decay tracer lifetimes + the muzzle flash.
// ---------------------------------------------------------------------------
void CombatFx::update(float dt) {
    if (dt <= 0.0f) return;
    for (auto& t : m_tracers) {
        if (t.life > 0.0f) {
            t.life -= dt;
            if (t.life < 0.0f) t.life = 0.0f;
        }
    }
    if (m_muzzleFlash > 0.0f) {
        m_muzzleFlash -= dt;
        if (m_muzzleFlash < 0.0f) m_muzzleFlash = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// drawBeam: stretch the unit box along segment a->b with `thickness` cross-
// section. Build an orthonormal basis with the segment as the local Z axis.
// ---------------------------------------------------------------------------
void CombatFx::drawBeam(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                        float thickness, const float color[4]) const {
    if (!m_box.valid()) return;
    x3::phys::Vec3 seg{ b.x - a.x, b.y - a.y, b.z - a.z };
    float len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    if (len < 1e-5f) return;
    x3::phys::Vec3 dir = normalize(seg);

    // Two perpendiculars to dir. Pick a reference axis not parallel to dir.
    x3::phys::Vec3 ref = (std::fabs(dir.y) < 0.99f) ? x3::phys::Vec3{ 0, 1, 0 }
                                                    : x3::phys::Vec3{ 1, 0, 0 };
    x3::phys::Vec3 u = normalize(cross(ref, dir));   // perp 1
    x3::phys::Vec3 v = cross(dir, u);                // perp 2 (already unit)

    // Midpoint of the segment is the box center.
    x3::phys::Vec3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };

    // Unit box is half-extent 0.5: scale local axes by (2*thickness) cross-
    // section and `len` along the segment so it exactly spans a->b.
    float model[16];
    composeTRS3(model, u, v, dir, thickness * 2.0f, thickness * 2.0f, len, mid);
    device.drawMesh(frame, m_box, x3::rhi::TextureHandle{}, color, model);
}

// ---------------------------------------------------------------------------
// draw: crosshair (always) + active tracers + muzzle flash.
// ---------------------------------------------------------------------------
void CombatFx::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    float eyeX, float eyeY, float eyeZ, float yaw, float pitch) const {
    if (!m_box.valid()) return;

    // Bright FX colors (baseColorFactor multiplies the default white texel).
    const float crosshairColor[4] = { 0.2f, 1.0f, 0.3f, 1.0f };  // bright green
    const float tracerColor[4]    = { 1.0f, 0.95f, 0.4f, 1.0f }; // hot yellow
    const float muzzleColor[4]    = { 1.0f, 0.85f, 0.4f, 1.0f }; // muzzle orange

    // ---- Camera basis (device convention: fwd = (cp*cy, sp, cp*sy)). ----
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    x3::phys::Vec3 right{ -sy, 0.0f, cy };
    x3::phys::Vec3 up = cross(right, forward);

    // ---- Crosshair "+": two thin boxes at screen center, kCrosshairDist ahead,
    // oriented to the camera (horizontal arm along right, vertical along up). ----
    x3::phys::Vec3 center{
        eyeX + forward.x * kCrosshairDist,
        eyeY + forward.y * kCrosshairDist,
        eyeZ + forward.z * kCrosshairDist };
    const float arm  = kCrosshairSize;          // half-length of each arm
    const float thin = kCrosshairSize * 0.18f;  // half-thickness of each arm
    {
        // Horizontal bar: long along right, thin along up, thin along forward.
        float m[16];
        composeTRS3(m, right, up, forward, arm * 2.0f, thin * 2.0f, thin * 2.0f, center);
        device.drawMesh(frame, m_box, x3::rhi::TextureHandle{}, crosshairColor, m);
    }
    {
        // Vertical bar: thin along right, long along up, thin along forward.
        float m[16];
        composeTRS3(m, right, up, forward, thin * 2.0f, arm * 2.0f, thin * 2.0f, center);
        device.drawMesh(frame, m_box, x3::rhi::TextureHandle{}, crosshairColor, m);
    }

    // ---- Tracers: thin bright beams, fading thinner as they age. ----
    for (const auto& t : m_tracers) {
        if (t.life <= 0.0f) continue;
        float k = (kTracerTime > 0.0f) ? (t.life / kTracerTime) : 1.0f; // 1->0
        // Slightly taper the beam as it fades so it reads as a fast streak.
        float thick = kTracerThickness * (0.5f + 0.5f * k);
        drawBeam(device, frame, t.from, t.to, thick, tracerColor);
    }

    // ---- Muzzle flash: a brief bright box at the muzzle. ----
    if (m_muzzleFlash > 0.0f) {
        float k = (kMuzzleFlashTime > 0.0f) ? (m_muzzleFlash / kMuzzleFlashTime) : 1.0f;
        float s = kMuzzleFlashSize * (0.6f + 0.4f * k) * 2.0f; // full extent
        float m[16];
        composeTRS3(m, right, up, forward, s, s, s, m_muzzlePos);
        device.drawMesh(frame, m_box, x3::rhi::TextureHandle{}, muzzleColor, m);
    }
}

} // namespace x3::game
