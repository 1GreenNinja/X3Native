// Rotor-spin prototype. See rotor_demo.h.
//
// Clean-room: built ONLY from X3Native's own Scene / mesh_prims + the engine RHI
// interface + hand-rolled matrix math. No RBDOOM / id Tech / Doom / Quake — or any
// other game-engine — source consulted. No renderer/engine changes.
#include "rotor_demo.h"
#include "headless_device.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace x3::game {

namespace {

constexpr float kTwoPi  = 6.28318530718f;
constexpr float kHalfPi = 1.57079632679f;

// Append `src`'s render geometry into `dst`, offsetting indices. Visual-only merge
// (collision arrays are not needed for a purely-rendered prop).
void appendPrim(x3::prims::PrimMesh& dst, const x3::prims::PrimMesh& src) {
    const uint32_t base = (uint32_t)dst.verts.size();
    dst.verts.insert(dst.verts.end(), src.verts.begin(), src.verts.end());
    for (uint32_t i : src.index) dst.index.push_back(base + i);
}

// Upload a PrimMesh and register a visual Scene entity at `transform` (column-major).
uint32_t addVisual(Scene& scene, x3::rhi::IRenderDevice& device, const x3::prims::PrimMesh& m,
                   float r, float g, float b, float emis, const float transform[16]) {
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    e.baseColor[0] = r; e.baseColor[1] = g; e.baseColor[2] = b; e.baseColor[3] = 1.0f;
    if (emis > 0.0f) { e.emissive[0] = 0.20f; e.emissive[1] = 0.55f; e.emissive[2] = 0.95f; e.emissive[3] = emis; }
    e.tag = (uint32_t)Tag::Prop;
    std::memcpy(e.transform, transform, sizeof(float) * 16);
    return scene.add(e);
}

} // namespace

void rotorSpinMatrix(const x3::phys::Vec3& hub, const x3::phys::Vec3& axis,
                     float angle, float out[16]) {
    // Normalize the axis (fall back to +Y for a degenerate input).
    float ax = axis.x, ay = axis.y, az = axis.z;
    float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len < 1e-6f) { ax = 0.0f; ay = 1.0f; az = 0.0f; len = 1.0f; }
    ax /= len; ay /= len; az /= len;

    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;

    // Column-major: column j is the image of basis vector e_j under the rotation.
    out[0]  = t * ax * ax + c;       out[1]  = t * ax * ay + s * az;  out[2]  = t * ax * az - s * ay;  out[3]  = 0.0f;
    out[4]  = t * ax * ay - s * az;  out[5]  = t * ay * ay + c;       out[6]  = t * ay * az + s * ax;  out[7]  = 0.0f;
    out[8]  = t * ax * az + s * ay;  out[9]  = t * ay * az - s * ax;  out[10] = t * az * az + c;       out[11] = 0.0f;
    out[12] = hub.x;                 out[13] = hub.y;                 out[14] = hub.z;                 out[15] = 1.0f;
}

uint32_t RotorSpin::addRotor(uint32_t entity, const x3::phys::Vec3& hub,
                             const x3::phys::Vec3& axis, float radiansPerSec) {
    Rotor r;
    r.entity = entity; r.hub = hub; r.axis = axis; r.radiansPerSec = radiansPerSec; r.angle = 0.0f;
    m_rotors.push_back(r);
    return (uint32_t)m_rotors.size() - 1;
}

uint32_t RotorSpin::buildGrayboxQuad(Scene& scene, x3::rhi::IRenderDevice& device,
                                     const x3::phys::Vec3& origin) {
    const float ox = origin.x, oy = origin.y, oz = origin.z;
    const float arm  = 0.60f;   // hub distance from center along each axis
    const float hubY = oy + 0.10f;

    // ---- Body: a hull box + a crossed "+" frame, baked at the world origin (static). ----
    {
        x3::prims::PrimMesh body;
        appendPrim(body, x3::prims::makeBox(0.25f, 0.08f, 0.25f, ox, oy, oz));         // hull
        appendPrim(body, x3::prims::makeBox(arm,   0.04f, 0.05f, ox, oy, oz));         // X arm
        appendPrim(body, x3::prims::makeBox(0.05f, 0.04f, arm,   ox, oy, oz));         // Z arm
        float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        m_body = addVisual(scene, device, body, 0.60f, 0.62f, 0.66f, 0.0f, ident);
    }

    // ---- Rotor blade mesh: a 4-blade cross authored at the LOCAL origin, SHARED by
    // all 4 rotor entities (each gets its own transform). ----
    x3::prims::PrimMesh prop;
    appendPrim(prop, x3::prims::makeBox(0.30f, 0.015f, 0.04f, 0.0f, 0.0f, 0.0f));      // blade along X
    appendPrim(prop, x3::prims::makeBox(0.04f, 0.015f, 0.30f, 0.0f, 0.0f, 0.0f));      // blade along Z

    const x3::phys::Vec3 yAxis{ 0.0f, 1.0f, 0.0f };
    // "+" config: front/back (along X) spin one way; left/right (along Z) the other —
    // counter-rotating like a real quad.
    struct Hub { x3::phys::Vec3 pos; float rps; };
    const Hub hubs[kQuadRotorCount] = {
        { { ox + arm, hubY, oz       },  kRotorDemoRps },  // front
        { { ox - arm, hubY, oz       },  kRotorDemoRps },  // back
        { { ox,       hubY, oz + arm }, -kRotorDemoRps },  // right (counter)
        { { ox,       hubY, oz - arm }, -kRotorDemoRps },  // left  (counter)
    };
    for (const Hub& h : hubs) {
        float m[16];
        rotorSpinMatrix(h.pos, yAxis, 0.0f, m);
        uint32_t e = addVisual(scene, device, prop, 0.14f, 0.16f, 0.20f, 1.2f, m);
        addRotor(e, h.pos, yAxis, h.rps);
    }

    x3::logInfo("RotorSpin::buildGrayboxQuad — graybox quadcopter (box body + 4 counter-"
                "rotating blade rotors @ " + std::to_string(kRotorDemoRps) + " rad/s) built");
    return m_body;
}

void RotorSpin::tick(float dt, Scene& scene) {
    for (Rotor& r : m_rotors) {
        if (r.entity >= scene.size()) continue;
        r.angle += r.radiansPerSec * dt;
        // Keep the accumulated angle in [0, 2π) (numerically stable over long runs).
        r.angle = std::fmod(r.angle, kTwoPi);
        if (r.angle < 0.0f) r.angle += kTwoPi;
        rotorSpinMatrix(r.hub, r.axis, r.angle, scene.get(r.entity).transform);
    }
}

// ===========================================================================
// Headless self-test (--test-rotorspin).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[rotorspin-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[rotorspin-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;
using HeadlessDevice = x3::game::HeadlessRenderDevice;

bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Transform a direction (ignore translation) by a column-major 3x3 (mat·vec).
void rot3(const float m[16], float x, float y, float z, float out[3]) {
    out[0] = m[0] * x + m[4] * y + m[8]  * z;
    out[1] = m[1] * x + m[5] * y + m[9]  * z;
    out[2] = m[2] * x + m[6] * y + m[10] * z;
}

} // namespace

bool runRotorSpinSelfTest() {
    g_pass = g_fail = 0;

    HeadlessDevice device;
    Scene scene;
    RotorSpin quad;
    quad.buildGrayboxQuad(scene, device, x3::phys::Vec3{ 0.0f, 1.0f, 0.0f });

    // ---- R0: 4 rotors built; body + rotor entities valid, bodyless, visible, meshed. ----
    {
        bool ok = quad.built() && quad.count() == kQuadRotorCount &&
                  quad.bodyEntity() < scene.size();
        for (uint32_t i = 0; i < quad.count(); ++i) {
            const Rotor& r = quad.rotor(i);
            if (r.entity >= scene.size()) { ok = false; break; }
            const Entity& e = scene.get(r.entity);
            if (!e.mesh.valid() || !e.visible || e.body.valid()) ok = false;  // bodyless visual
        }
        check(ok, "R0 graybox quad built: 4 bodyless visible rotor entities + a body");
    }

    // ---- R1: at load every rotor angle is 0 and its transform = Translate(hub)·Identity. ----
    {
        bool ok = true;
        for (uint32_t i = 0; i < quad.count(); ++i) {
            const Rotor& r = quad.rotor(i);
            const float* m = scene.get(r.entity).transform;
            ok = ok && approx(r.angle, 0.0f);
            ok = ok && approx(m[12], r.hub.x) && approx(m[13], r.hub.y) && approx(m[14], r.hub.z);
            ok = ok && approx(m[0], 1.0f) && approx(m[5], 1.0f) && approx(m[10], 1.0f);  // identity rot
            ok = ok && approx(m[1], 0.0f) && approx(m[4], 0.0f) && approx(m[2], 0.0f);
        }
        check(ok, "R1 at load: angle 0 + transform = hub translation + identity rotation");
    }

    // ---- R4: rotorSpinMatrix correctness (axis-invariant, 90° rotates as expected,
    // columns orthonormal). About +Y: (1,0,0) -> (cos90,0,-sin90) = (0,0,-1). ----
    {
        float m[16];
        rotorSpinMatrix(x3::phys::Vec3{ 0,0,0 }, x3::phys::Vec3{ 0,1,0 }, kHalfPi, m);
        float onAxis[3];  rot3(m, 0, 1, 0, onAxis);   // axis point preserved
        float offAxis[3]; rot3(m, 1, 0, 0, offAxis);  // +X -> -Z at 90°
        const bool axisOk = approx(onAxis[0], 0.0f) && approx(onAxis[1], 1.0f) && approx(onAxis[2], 0.0f);
        const bool rotOk  = approx(offAxis[0], 0.0f) && approx(offAxis[1], 0.0f) && approx(offAxis[2], -1.0f);
        // Columns unit length + orthogonal (a proper rotation).
        const float l0 = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
        const float l1 = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
        const float dot01 = m[0]*m[4] + m[1]*m[5] + m[2]*m[6];
        const bool orthoOk = approx(l0, 1.0f) && approx(l1, 1.0f) && approx(dot01, 0.0f);
        check(axisOk && rotOk && orthoOk, "R4 spin matrix: axis-invariant + correct 90° + orthonormal");
    }

    // ---- R2/R3: tick 0.5 s; angles accumulate by rps·t; counter-rotating signs differ. ----
    {
        const int steps = 30;  // 0.5 s @ 60 Hz
        for (int i = 0; i < steps; ++i) quad.tick(kFixedDt, scene);
        // Independently accumulate the expected wrapped angle per rotor.
        auto expectAngle = [&](float rps) {
            float a = 0.0f;
            for (int i = 0; i < steps; ++i) { a = std::fmod(a + rps * kFixedDt, kTwoPi); if (a < 0) a += kTwoPi; }
            return a;
        };
        bool acc = true;
        for (uint32_t i = 0; i < quad.count(); ++i)
            acc = acc && approx(quad.rotor(i).angle, expectAngle(quad.rotor(i).radiansPerSec), 1e-3f);
        check(acc, "R2 ticking accumulates each rotor angle by rps·dt (wrapped)");

        // Rotor 0 (+rps) and rotor 2 (-rps) progress oppositely -> different angles.
        const bool counter = quad.rotor(0).radiansPerSec > 0 && quad.rotor(2).radiansPerSec < 0 &&
                             !approx(quad.rotor(0).angle, quad.rotor(2).angle, 1e-3f) &&
                              approx(quad.rotor(0).angle, kTwoPi - quad.rotor(2).angle, 1e-3f);
        check(counter, "R3 counter-rotating pair: +rps and -rps angles are mirror-opposite");

        // Transform actually reflects the angle (rotor 0 off-axis point moved off +X).
        float v[3]; rot3(scene.get(quad.rotor(0).entity).transform, 1, 0, 0, v);
        check(!approx(v[0], 1.0f) || !approx(v[2], 0.0f),
              "R3b rotor entity transform reflects the accumulated spin");
    }

    // ---- R5: a full 2π revolution returns the spin matrix to its start. ----
    {
        float m0[16], m2pi[16];
        rotorSpinMatrix(x3::phys::Vec3{ 1, 2, 3 }, x3::phys::Vec3{ 0, 1, 0 }, 0.0f,    m0);
        rotorSpinMatrix(x3::phys::Vec3{ 1, 2, 3 }, x3::phys::Vec3{ 0, 1, 0 }, kTwoPi,  m2pi);
        bool same = true;
        for (int i = 0; i < 16; ++i) same = same && approx(m0[i], m2pi[i], 1e-3f);
        check(same, "R5 a full 2π revolution returns the transform to its start");
    }

    // ---- R6: deterministic — a second identical run yields identical angles. ----
    {
        HeadlessDevice device2;
        Scene scene2;
        RotorSpin quad2;
        quad2.buildGrayboxQuad(scene2, device2, x3::phys::Vec3{ 0.0f, 1.0f, 0.0f });
        for (int i = 0; i < 30; ++i) quad2.tick(kFixedDt, scene2);
        bool det = quad2.count() == quad.count();
        for (uint32_t i = 0; det && i < quad.count(); ++i)
            det = det && approx(quad.rotor(i).angle, quad2.rotor(i).angle, 1e-6f);
        check(det, "R6 deterministic: identical tick sequence -> identical angles");
    }

    x3::logInfo(std::string("rotorspin: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
