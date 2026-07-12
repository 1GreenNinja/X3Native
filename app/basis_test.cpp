// ===========================================================================
// basis_test.cpp — --test-basis: THE MIRROR CANNOT COME BACK. (KNOWN_BUGS R3)
//
// The bug: a model instanced through a basis whose determinant is NEGATIVE. That
// is a reflection, not a rotation: winding reverses, back-face culling discards the
// outer shell, and you light the INSIDE of the object — which is to say, you cannot
// light it at all. It cost nine rounds of art on the rift gate before anyone thought
// to compute a determinant.
//
// This test makes the whole bug class impossible, and it is deliberately TOTAL: it
// does not check a list of known sites (a list rots the moment someone copy-pastes
// the idiom into a new file). It BUILDS THE WORLDS on a headless device and walks
// EVERY entity Scene ever received, asserting det(upper 3x3) > 0. Any new mirrored
// instancing site anywhere in the game goes red here on the next run.
//
// A test that cannot fail is worthless, so every assert ships with a NEGATIVE
// CONTROL: the exact legacy idiom (right = (-outZ, 0, outX)) is fed to the detector
// and to the scene scanner, and both must report MIRRORED. If someone "simplifies"
// det3() into something that always returns +1, the negative controls go red.
// ===========================================================================
#include "basis.h"

#include "descent_slide.h"
#include "headless_device.h"
#include "rifthub.h"          // pulls scene.h / trigger.h / IPhysicsWorld.h
#include "engine/core/x3_log.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

int bs_pass = 0, bs_fail = 0;

void bsCheck(bool ok, const std::string& what) {
    if (ok) { ++bs_pass; x3::logInfo("  [PASS] " + what); }
    else    { ++bs_fail; x3::logError("  [FAIL] " + what); }
}

// The LEGACY MIRROR IDIOM, preserved here on purpose and used ONLY as a negative
// control. Do not call it from game code. right = (-outward.z, 0, outward.x) with
// [right, up, outward] as columns => determinant -1.
void legacyMirroredBasis(const float outward[3], float X[3], float Y[3], float Z[3]) {
    X[0] = -outward[2]; X[1] = 0.0f; X[2] = outward[0];
    Y[0] = 0.0f;        Y[1] = 1.0f; Y[2] = 0.0f;
    Z[0] = outward[0];  Z[1] = 0.0f; Z[2] = outward[2];
}

// ---- THE SCANNER --------------------------------------------------------------
// Walk every drawable entity in a built Scene and count mirrored bases. Entities
// with no mesh, and collapsed (det == 0) transforms, draw nothing and are skipped.
uint32_t scanMirrored(const Scene& scene, const char* world,
                      std::vector<uint32_t>* outIds = nullptr) {
    uint32_t bad = 0;
    const std::vector<Entity>& es = scene.entities();
    for (uint32_t i = 0; i < (uint32_t)es.size(); ++i) {
        if (!es[i].mesh.valid()) continue;
        const float d = det3(es[i].transform);
        if (d < -1e-6f) {
            ++bad;
            if (outIds) outIds->push_back(i);
            if (bad <= 8) {
                x3::logError(std::string("    MIRRORED entity ") + std::to_string(i) +
                             " in '" + world + "' — det = " + std::to_string(d) +
                             " (inside-out, unlightable)");
            }
        }
    }
    return bad;
}

} // namespace

bool runBasisSelfTest() {
    bs_pass = bs_fail = 0;

    // =======================================================================
    // T1 — basisFromOutward() is right-handed and orthonormal, for EVERY outward.
    // =======================================================================
    {
        // 8 rifthub portal radials + the awkward ones: the two poles (where the
        // world-up reference collapses) and a fistful of skewed directions.
        std::vector<std::array<float, 3>> dirs;
        for (int i = 0; i < 8; ++i) {
            const float a = (float)i * (6.28318530718f / 8.0f);
            dirs.push_back({ std::cos(a), 0.0f, std::sin(a) });     // the gate radials
        }
        dirs.push_back({ 0.0f,  1.0f, 0.0f });                      // straight up
        dirs.push_back({ 0.0f, -1.0f, 0.0f });                      // straight down
        dirs.push_back({ 0.0f,  0.0f, -1.0f });                     // -Z: engine forward
        dirs.push_back({ 0.577f, 0.577f, 0.577f });                 // skew
        dirs.push_back({ -0.3f, 0.9f, -0.31f });                    // near-vertical skew
        dirs.push_back({ 3.0f, 0.0f, 4.0f });                       // NOT normalized

        bool detOk = true, orthoOk = true, zOk = true;
        for (const auto& d : dirs) {
            float X[3], Y[3], Z[3];
            basisFromOutward(d.data(), X, Y, Z);
            const float det = det3(X, Y, Z);
            if (std::fabs(det - 1.0f) > 1e-4f) detOk = false;       // a ROTATION: det == +1

            // Orthonormal: unit columns, zero dot products.
            auto len = [](const float v[3]) {
                return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); };
            auto dot = [](const float a[3], const float b[3]) {
                return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
            if (std::fabs(len(X) - 1.0f) > 1e-4f) orthoOk = false;
            if (std::fabs(len(Y) - 1.0f) > 1e-4f) orthoOk = false;
            if (std::fabs(len(Z) - 1.0f) > 1e-4f) orthoOk = false;
            if (std::fabs(dot(X, Y)) > 1e-4f) orthoOk = false;
            if (std::fabs(dot(X, Z)) > 1e-4f) orthoOk = false;
            if (std::fabs(dot(Y, Z)) > 1e-4f) orthoOk = false;

            // local +Z IS the outward axis (normalized).
            const float dl = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            if (std::fabs(Z[0] - d[0]/dl) > 1e-4f ||
                std::fabs(Z[1] - d[1]/dl) > 1e-4f ||
                std::fabs(Z[2] - d[2]/dl) > 1e-4f) zOk = false;
        }
        bsCheck(detOk,   "T1a basisFromOutward: determinant is +1 for every outward (a ROTATION)");
        bsCheck(orthoOk, "T1b basisFromOutward: columns are orthonormal (incl. the poles)");
        bsCheck(zOk,     "T1c basisFromOutward: local +Z is the outward axis");
    }

    // =======================================================================
    // T2 — NEGATIVE CONTROL. The detector must FIRE on the legacy idiom.
    //      If this passes silently, det3()/isMirroredBasis() are broken and every
    //      other assert in this file is worthless.
    // =======================================================================
    {
        bool allCaught = true, anyPositive = false;
        for (int i = 0; i < 8; ++i) {
            const float a = (float)i * (6.28318530718f / 8.0f);
            const float out[3] = { std::cos(a), 0.0f, std::sin(a) };
            float X[3], Y[3], Z[3];
            legacyMirroredBasis(out, X, Y, Z);
            const float det = det3(X, Y, Z);
            if (det >= 0.0f) anyPositive = true;                    // it IS -1. Always.
            float m[16];
            makeBasisXform(m, X, Y, Z, 0, 0, 0);
            if (!isMirroredBasis(m)) allCaught = false;             // the detector must fire
        }
        bsCheck(!anyPositive, "T2a NEGATIVE CONTROL: the legacy [right=(-outZ,0,outX), up, outward] "
                              "basis has determinant -1 at every portal angle");
        bsCheck(allCaught,    "T2b NEGATIVE CONTROL: isMirroredBasis() FIRES on it (the test can go red)");

        // ...and the fixed basis for the SAME outward must be clean.
        bool fixedClean = true;
        for (int i = 0; i < 8; ++i) {
            const float a = (float)i * (6.28318530718f / 8.0f);
            const float out[3] = { std::cos(a), 0.0f, std::sin(a) };
            float X[3], Y[3], Z[3], m[16];
            basisFromOutward(out, X, Y, Z);
            makeBasisXform(m, X, Y, Z, 0, 0, 0);
            if (isMirroredBasis(m)) fixedClean = false;
        }
        bsCheck(fixedClean, "T2c ...and basisFromOutward() on the same outward is NOT mirrored");
    }

    // =======================================================================
    // T3 — NEGATIVE CONTROL for the SCENE SCANNER. Plant a mirrored entity in a
    //      scene and prove the scanner catches it. (A scanner that always returns
    //      0 would make T4/T5 pass forever.)
    // =======================================================================
    {
        HeadlessRenderDevice device;
        Scene scene;

        Entity clean;
        clean.mesh = device.createMesh(nullptr, 0, nullptr, 0);
        // Identity (det +1).
        scene.add(clean);

        Entity mirrored;
        mirrored.mesh = device.createMesh(nullptr, 0, nullptr, 0);
        const float out[3] = { 1.0f, 0.0f, 0.0f };                  // rift portal 0
        float X[3], Y[3], Z[3];
        legacyMirroredBasis(out, X, Y, Z);
        makeBasisXform(mirrored.transform, X, Y, Z, 0, 0, 0);
        scene.add(mirrored);

        std::vector<uint32_t> ids;
        const uint32_t bad = scanMirrored(scene, "synthetic-negative-control", &ids);
        bsCheck(bad == 1 && ids.size() == 1 && ids[0] == 1,
                "T3 NEGATIVE CONTROL: the scene scanner catches a planted mirrored entity "
                "(and does not flag the clean one)");
    }

    // =======================================================================
    // T4 — THE RIFT HUB. Every entity, every portal. det > 0.
    // =======================================================================
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
        phys->init();
        HeadlessRenderDevice device;
        Scene scene;
        TriggerSystem triggers;
        Rifthub hub;
        hub.build(scene, device, *phys, triggers);
        const uint32_t bad = scanMirrored(scene, "rifthub");
        bsCheck(scene.size() > 100, "T4a rifthub built (scene is populated: " +
                                    std::to_string(scene.size()) + " entities)");
        bsCheck(bad == 0, "T4b rifthub: ZERO mirrored entities (gate ring, cradle, membrane, "
                          "conduits, deck, consoles) — " + std::to_string(bad) + " found");
        hub.shutdown(device);
        phys->shutdown();
    }

    // =======================================================================
    // T5 — THE DESCENT SLIDE. Its track frame was [right, up, tan] with
    //      right = cross(tan, up) => det -1. EVERY prop on the ride was mirrored.
    // =======================================================================
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
        phys->init();
        HeadlessRenderDevice device;
        Scene scene;
        DescentSlide slide;
        const bool built = slide.build(device, scene, *phys);
        bsCheck(built && scene.size() > 50,
                "T5a descent slide built (scene is populated: " +
                std::to_string(scene.size()) + " entities)");
        const uint32_t bad = scanMirrored(scene, "descentslide");
        bsCheck(bad == 0, "T5b descent slide: ZERO mirrored entities (channel, ribs, beams, "
                          "trestles, shoulder lights) — " + std::to_string(bad) + " found");
        slide.shutdown(*phys);
        phys->shutdown();
    }

    x3::logInfo("basis/mirror self-test: " + std::to_string(bs_pass) + " passed, " +
                std::to_string(bs_fail) + " failed");
    return bs_fail == 0;
}

} // namespace x3::game
