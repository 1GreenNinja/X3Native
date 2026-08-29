// ===========================================================================
// motion_blur_test.cpp — --test-motionblur. See motion_blur_test.h for the case
// list and why each one exists.
// ===========================================================================
#include "motion_blur_test.h"

#include "motion_rig.h"
#include "mesh_prims.h"
#include "engine/rhi/MotionBlur.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::game {

namespace {

using namespace x3::motionrig;

int g_pass = 0, g_total = 0;

void check(bool ok, const std::string& what) {
    ++g_total;
    if (ok) { ++g_pass; x3::logInfo("  [PASS] " + what); }
    else                x3::logError("  [FAIL] " + what);
}

std::string fmt(double v, int dp = 4) {
    char b[64]; std::snprintf(b, sizeof(b), "%.*f", dp, v); return b;
}

// ---------------------------------------------------------------------------
// THE PROBE SCENE.
//
// Three boxes, chosen so one scene can answer every question:
//
//   pillar  z = -2  (NEAR the camera)  static, narrow, centred
//   slab    z = +1  (BEHIND the pillar) optionally translating along X
//   wall    z = +6  (far)               static backdrop
//
// A checkerboard albedo under flat ambient. THE CHECK SIZE IS LOAD-BEARING and
// was calibrated against a first run that measured the wrong thing:
//
//   at ~13 px per check against a ~17 px blur, the blur exceeded one full check
//   PERIOD and washed the checkerboard to flat grey -- destroying the horizontal
//   edges as thoroughly as the vertical ones. Both gradients collapsed 3.7x
//   together, the ANISOTROPY barely moved (1.032 -> 0.947), and a real, large,
//   correct blur scored DBI 0.08. The metric was right; the probe was wrong.
//
// So the checks are sized WELL ABOVE the blur length (~55 px per check vs ~17 px
// of blur at the slab's depth). Blur along X then spreads each vertical edge over
// 17 px -- collapsing gradient energy along the motion axis -- while leaving the
// horizontal edges, and therefore the across-axis reference, untouched. That is
// the regime in which directional anisotropy actually measures direction.
//
// With camSpeed > 0 and objSpeed 0 the whole frame moves (the camera cases).
// With camSpeed 0 and objSpeed > 0 the slab moves behind a stationary pillar,
// which is the depth-ordering case: the slab must blur, the pillar must not.
// ---------------------------------------------------------------------------
struct ProbeScene {
    x3::rhi::MeshHandle    pillar{}, slab{}, wall{};
    x3::rhi::TextureHandle checker{};
    float objSpeed = 0.0f;      // slab speed, m/s along +X
};

const float kIdentity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

void drawProbe(void* ctx, x3::rhi::IRenderDevice& device,
               x3::rhi::FrameContext& frame, float t) {
    auto* s = static_cast<ProbeScene*>(ctx);
    const float white[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float noEmis[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    // Slab: the ONLY moving object. Column-major translation, matching the
    // engine's model-matrix convention (the prev-model SSBO reads these rows, so
    // a stable draw ORDER is what makes per-object velocity correct here).
    float slabT[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    slabT[12] = s->objSpeed * t;

    // Draw order is FIXED for the whole test: the velocity prev-model ring is
    // indexed by SSBO row, and rows are assigned in emit order.
    device.drawMeshEmissive(frame, s->wall,   s->checker, white, noEmis, kIdentity);
    device.drawMeshEmissive(frame, s->slab,   s->checker, white, noEmis, slabT);
    device.drawMeshEmissive(frame, s->pillar, s->checker, white, noEmis, kIdentity);
}

// One post configuration for the whole battery, varying only the two knobs the
// cases actually test. Everything else is pinned so nothing else can move:
//   * auto-exposure OFF  — it would react to the blur's effect on mean luminance
//   * bloom OFF          — it would bleed detail across the measured gradients
//   * taasharpen 0       — a sharpen pass fights the very measurement being made
//   * mbDt fixed         — the framerate under test, stated explicitly
x3::rhi::IRenderDevice::PostFXParams makePost(bool velocity, bool motionBlur, float mbDt) {
    x3::rhi::IRenderDevice::PostFXParams px{};
    px.tonemapMode  = 1;
    px.bloomEnabled = false;
    px.autoExposure = false;
    px.taa          = true;      // required: velocity (and so the blur) rides on it
    px.taaSharpen   = 0.0f;
    px.velocity     = velocity;
    px.motionBlur   = motionBlur;
    px.mbShutter    = x3::rhi::kMotionBlurDefaultShutter;
    px.mbRefFps     = x3::rhi::kMotionBlurDefaultRefFps;
    px.mbSamples    = 15;        // above the default: a cleaner measurement
    px.mbMaxBlur    = 0.0f;      // the dilation's exact bound
    px.mbSoftZ      = 0.05f;
    px.mbDt         = mbDt;
    return px;
}

// Apply a post config AND force a TAA-history invalidation, so every series
// starts from the same renderer state. The off->on transition is the engine's
// own documented reset (VulkanRenderDevice::setPostFX).
void applyPostAndResetHistory(x3::rhi::IRenderDevice& device,
                              const x3::rhi::IRenderDevice::PostFXParams& px) {
    x3::rhi::IRenderDevice::PostFXParams off = px;
    off.taa = false;
    device.setPostFX(off);
    device.setPostFX(px);
}

void logMetrics(const char* label, const SeriesMetrics& m) {
    x3::logInfo(std::string("  [metric] ") + label +
                "  gradAlong " + fmt(m.gradAlong, 3) +
                "  gradAcross " + fmt(m.gradAcross, 3) +
                "  anisotropy " + fmt(m.anisotropy) +
                "  temporalRms " + fmt(m.temporalRms, 3));
}

} // namespace

int runMotionBlurTest(x3::rhi::IRenderDevice& device, const std::string& outDirIn) {
    g_pass = g_total = 0;
    const std::string outDir = outDirIn.empty() ? std::string("captures/motionblur") : outDirIn;
    x3::logInfo("=== --test-motionblur: MOTION-DOMAIN RIG + MOTION BLUR RESOLVE PASS ===");
    x3::logInfo("    rig: deterministic camera-on-rails, fixed dt, N-frame series;");
    x3::logInfo("    metric: RMS luminance-gradient ENERGY along vs across the motion axis");
    x3::logInfo("            (translation is an isometry; directional blur is not).");

    // ---- Scene + lighting. Flat ambient only: the image IS the albedo, which
    // maximises the gradient signal and removes every lighting variable.
    device.setAmbient(1.0f, 1.0f, 1.0f);
    device.setIblIntensity(0.0f);
    x3::rhi::IRenderDevice::SkyParams sky{};
    sky.enabled = false; sky.sunLight = 0.0f;
    device.setSkyParams(sky);
    device.setExposure(1.0f);
    device.setPointLights(nullptr, 0);
    // SSAO on for ONE structural reason, stated so nobody removes it: the
    // velocity pre-pass requires the DEPTH PRE-PASS, and the depth pre-pass only
    // runs when SSAO, SSGI, RT-AO or reflections are enabled (vk_graph.cpp
    // prologue). With all four off, r_velocity 1 is a silent no-op and this whole
    // test would measure a blur that never ran.
    x3::rhi::IRenderDevice::SsaoParams ss{};
    ss.enabled = true; ss.strength = 0.0f;   // enabled for the pre-pass, contributing nothing
    device.setSsaoParams(ss);
    // EVERY OTHER GI/RT CONSUMER OFF, and this is not tidiness -- it is what makes
    // the probe reproducible. Measured while building this gate: with the ray-traced
    // stack at its device defaults, TWO IDENTICAL STATIC-CAMERA RUNS WERE NOT
    // BIT-IDENTICAL, with motion blur entirely disabled. The TLAS is built into a
    // 3-slot ring with occasional device waits ([tlas-db] in the log), so which
    // acceleration structure a frame samples depends on ring timing; on a MOVING
    // camera TAA's neighbourhood clamp discards the difference within a frame or
    // two, but on a STATIC camera the history accumulates it and it never washes
    // out. That is a pre-existing property of the RT path, not of this lane -- but
    // a rig whose baseline drifts cannot make a bit-identity claim about anything,
    // so the probe scene switches the ray-traced stack off entirely.
    // SSAO stays ON above because the depth PRE-PASS (and therefore the velocity
    // pass, and therefore this whole test) is gated on one of these four being
    // enabled. Remove that line and the test silently measures nothing.
    {
        x3::rhi::IRenderDevice::GiParams gi{};          gi.enabled = false;
        device.setGiParams(gi);
        x3::rhi::IRenderDevice::RtaoParams rtao{};      rtao.enabled = false;
        device.setRtaoParams(rtao);
        x3::rhi::IRenderDevice::ReflectionParams rf{};  rf.ssr = false; rf.rtFallback = false;
        device.setReflectionParams(rf);
        x3::rhi::IRenderDevice::DdgiParams dg{};        dg.enabled = false;
        device.setDdgiParams(dg);
        x3::rhi::IRenderDevice::RtShadowParams rs{};    rs.tier = 0;
        device.setRtShadowParams(rs);
        device.setSkinnedRtEnabled(false);
    }

    device.beginUploadBatch();
    auto checkerPx = x3::prims::makeCheckerRGBA(256, 32);
    ProbeScene scene{};
    scene.checker = device.createTexture(checkerPx.data(), 256, 256, /*srgb*/true);
    {
        // half-extents, centre, uvScale (UV tiles per world metre)
        auto mkBox = [&](float hx, float hy, float hz, float cz, float uv) {
            x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, 0.0f, 0.0f, cz, uv);
            return device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                     m.index.data(), (uint32_t)m.index.size());
        };
        // uvScale = UV tiles per world metre; the 256px texture holds 8 checks
        // per tile, so 0.22 tiles/m == 0.57 m per check == ~57 px at the slab's
        // 10 m depth. The pillar is nearer (7 m) and narrower, so it gets a finer
        // scale to keep enough checks inside its measurement region.
        scene.pillar = mkBox(1.2f,  6.0f,  0.3f, -2.0f, 0.45f);
        scene.slab   = mkBox(14.0f, 14.0f, 0.3f,  1.0f, 0.22f);
        scene.wall   = mkBox(40.0f, 40.0f, 0.3f,  6.0f, 0.22f);
    }
    device.endUploadBatch();

    // ---- Rails configurations ------------------------------------------------
    // 30 m/s lateral at ~15 m from the backdrop puts roughly 33 px/frame of
    // screen motion on the slab at 60 Hz, which a 0.5 shutter turns into ~17 px
    // of blur -- comfortably inside the dilation's 40 px bound and comfortably
    // larger than the ~13 px checker cell, so the pattern is genuinely destroyed
    // along the motion axis rather than merely smeared.
    const float kCamSpeed = 30.0f;
    const float kObjSpeed = 20.0f;
    // settle + frames is a multiple of 8 so two series start on the same TAA
    // Halton phase. See RailsConfig.
    auto baseRails = [&](const char* sub, float camSpeed, float dt, int settle, int frames) {
        RailsConfig c{};
        c.frames = frames; c.settle = settle; c.dt = dt; c.t0 = 0.0f;
        c.camSpeed = camSpeed;
        c.camX = 0.0f; c.camY = 0.0f; c.camZ = -9.0f;
        c.dir = outDir + "/" + sub;
        c.prefix = "f";
        return c;
    };

    auto shoot = [&](const RailsConfig& cfg, bool velocity, bool motionBlur,
                     float mbDt, float objSpeed) {
        scene.objSpeed = objSpeed;
        applyPostAndResetHistory(device, makePost(velocity, motionBlur, mbDt));
        return captureRails(device, cfg, &drawProbe, &scene);
    };

    const float kDt60  = 1.0f / 60.0f;
    const float kDt165 = 1.0f / 165.0f;

    // =======================================================================
    // R1 — RIG DETERMINISM. Guard the instrument before measuring with it.
    // =======================================================================
    const RailsConfig movOffA = baseRails("mov_off_a", kCamSpeed, kDt60, 16, 8);
    const RailsConfig movOffB = baseRails("mov_off_b", kCamSpeed, kDt60, 16, 8);
    const auto sMovOffA = shoot(movOffA, /*vel*/true, /*blur*/false, kDt60, 0.0f);
    const auto sMovOffB = shoot(movOffB, /*vel*/true, /*blur*/false, kDt60, 0.0f);
    check(sMovOffA.size() == 8 && sMovOffB.size() == 8,
          "R1a rails wrote 8 numbered frames per series");
    check(seriesBitIdentical(sMovOffA, sMovOffB),
          "R1 RIG DETERMINISM: two identical rails runs are BIT-IDENTICAL "
          "(fixed dt + analytic path + aligned TAA phase + history reset)");

    // =======================================================================
    // R2 / R3 — THE METRIC ITSELF, on pixels, with no renderer in the loop.
    // =======================================================================
    {
        std::vector<RgbaImage> sharp, blurred;
        for (const auto& p : sMovOffA) {
            RgbaImage img = loadImage(p);
            if (!img.valid()) { sharp.clear(); break; }
            // 8 px radius = a 17 px box along X, the same order as the blur the
            // shader is asked to produce.
            blurred.emplace_back(applyDirectionalBlur(img, MotionAxis::Horizontal, 8));
            sharp.emplace_back(std::move(img));
        }
        const SeriesMetrics mSharp = measureFrames(sharp,   MotionAxis::Horizontal);
        const SeriesMetrics mBlur  = measureFrames(blurred, MotionAxis::Horizontal);
        logMetrics("R2 sharp   ", mSharp);
        logMetrics("R2 synthetic-blur", mBlur);
        const double dbiSynth = directionalBlurIndex(mBlur, mSharp);
        check(dbiSynth > 0.20,
              "R2 METRIC SELF-CHECK (positive): a synthetic 17 px horizontal box blur "
              "scores DBI " + fmt(dbiSynth) + " > 0.20 — the metric CAN see directional blur");
        const double dbiSelf = directionalBlurIndex(mSharp, mSharp);
        check(std::fabs(dbiSelf) < 1e-9,
              "R3 METRIC SELF-CHECK (negative): a series against itself scores DBI "
              + fmt(dbiSelf) + " = 0 — the metric does not manufacture a reading");
    }

    const SeriesMetrics mMovOff = measureSeries(sMovOffA, MotionAxis::Horizontal);
    logMetrics("moving/blur-OFF", mMovOff);
    check(mMovOff.valid && mMovOff.gradAcross > 3.0,
          "R4 PROBE SANITY: the reference series carries real detail across the "
          "motion axis (RMS gradient " + fmt(mMovOff.gradAcross, 2) + " > 3) — nothing below can pass vacuously");

    // =======================================================================
    // N1 / N2 — THE NEGATIVE CONTROL. A STATIC CAMERA IS NOT BLURRED.
    // This is the case the tree's existing still-frame gates cannot express, and
    // the one that would have caught "motion blur is missing" as easily as it
    // catches "motion blur fires when it must not".
    // =======================================================================
    const RailsConfig statOff = baseRails("static_off", 0.0f, kDt60, 16, 8);
    const RailsConfig statOn  = baseRails("static_on",  0.0f, kDt60, 16, 8);
    const auto sStatOff = shoot(statOff, true, /*blur*/false, kDt60, 0.0f);
    const auto sStatOn  = shoot(statOn,  true, /*blur*/true,  kDt60, 0.0f);
    const SeriesMetrics mStatOff = measureSeries(sStatOff, MotionAxis::Horizontal);
    const SeriesMetrics mStatOn  = measureSeries(sStatOn,  MotionAxis::Horizontal);
    logMetrics("static/blur-OFF", mStatOff);
    logMetrics("static/blur-ON ", mStatOn);
    const double dbiStatic = directionalBlurIndex(mStatOn, mStatOff);
    check(std::fabs(dbiStatic) < 0.02,
          "N1 NEGATIVE CONTROL: static camera, blur ON vs OFF scores DBI "
          + fmt(dbiStatic) + " (|DBI| < 0.02) — a still frame does NOT read as blurred");
    check(seriesBitIdentical(sStatOn, sStatOff),
          "N1b and it is BIT-IDENTICAL: with no motion the pass is exactly the "
          "identity function (mb_blur.frag early-outs below half a pixel of velocity)");
    // N2. NOT "temporalRms == 0", and the reason is a real property of this
    // renderer worth stating: under TAA a STATIC CAMERA IS NOT TEMPORALLY STATIC.
    // The Halton jitter moves the projection sub-pixel every frame and the resolve
    // blends 10% of that jittered current frame in, so consecutive frames of a
    // dead-still camera differ by a small residual on high-contrast edges
    // (measured: RMS ~7.6 on this checkerboard). What the case can assert -- and
    // what actually matters -- is that this residual is an ORDER OF MAGNITUDE
    // below real scene motion, so N1 is a statement about a genuinely still frame
    // and not about a slowly-drifting one.
    const double staticVsMoving = (mMovOff.temporalRms > 1e-9)
                                ? (mStatOff.temporalRms / mMovOff.temporalRms) : 1.0;
    check(staticVsMoving < 0.15,
          "N2 NOTHING MOVED: static series frame-to-frame energy "
          + fmt(mStatOff.temporalRms, 2) + " is " + fmt(staticVsMoving * 100.0, 1) +
          "% of the moving series' " + fmt(mMovOff.temporalRms, 2) +
          " (< 15%) — the residual is TAA's sub-pixel jitter, not scene motion");

    // =======================================================================
    // P1 / P2 — THE POSITIVE CASE. A MOVING CAMERA IS BLURRED, ALONG THE AXIS.
    // =======================================================================
    const RailsConfig movOn = baseRails("moving_on", kCamSpeed, kDt60, 16, 8);
    const auto sMovOn = shoot(movOn, true, /*blur*/true, kDt60, 0.0f);
    const SeriesMetrics mMovOn = measureSeries(sMovOn, MotionAxis::Horizontal);
    logMetrics("moving/blur-ON ", mMovOn);
    check(mMovOff.temporalRms > 5.0 && mMovOn.temporalRms > 5.0,
          "P1 THE SCENE MOVED in both series (frame-to-frame energy OFF "
          + fmt(mMovOff.temporalRms, 2) + " / ON " + fmt(mMovOn.temporalRms, 2) +
          ", both > 5) — P2 compares two series that really are in motion");
    const double dbiMoving = directionalBlurIndex(mMovOn, mMovOff);
    check(dbiMoving > 0.20,
          "P2 BLUR ON vs OFF SEPARATES: moving camera scores DBI " + fmt(dbiMoving) +
          " > 0.20 — detail is lost specifically ALONG the motion axis");

    // =======================================================================
    // D1 / D2 — THE dt RULE, arithmetically, on the SHIPPED function.
    // =======================================================================
    {
        // Identical physical motion sampled at two framerates: the per-frame UV
        // displacement is proportional to dt.
        const float physicalUvPerSecond = 2.0f;
        const float v60  = physicalUvPerSecond * kDt60;
        const float v165 = physicalUvPerSecond * kDt165;
        const float s60  = x3::rhi::motionBlurVelocityScale(kDt60,  0.5f, 60.0f);
        const float s165 = x3::rhi::motionBlurVelocityScale(kDt165, 0.5f, 60.0f);
        const double blur60  = (double)v60  * s60;
        const double blur165 = (double)v165 * s165;
        const double rel = std::fabs(blur60 - blur165) / std::max(blur60, 1e-9);
        check(rel < 1e-4,
              "D1 dt NORMALISATION (arithmetic): the same physical motion yields the "
              "same exposure displacement at 60 Hz (" + fmt(blur60, 6) + ") and 165 Hz ("
              + fmt(blur165, 6) + "), " + fmt(rel * 100.0, 4) + "% apart");
        // The bug the guide warns about: without the normalisation, blur strength
        // would follow the per-frame delta and so shrink by the framerate ratio --
        // vanishing at high framerate and overwhelming at low. If this control
        // ever stopped separating, D1 and D3 would be measuring nothing.
        const double relRaw = std::fabs((double)v60 - (double)v165) / (double)v60;
        check(relRaw > 0.5,
              "D2 dt NEGATIVE CONTROL: the UN-normalised per-frame velocity differs by "
              + fmt(relRaw * 100.0, 1) + "% between 60 and 165 Hz — the rule is doing "
              "real work, and D1/D3 would go red without it");
    }

    // =======================================================================
    // D3 — THE dt RULE, RENDERED. Same physical instant, two framerates.
    //
    // 4/60 s == 11/165 s exactly, so frame 4 of the 60 Hz series and frame 11 of
    // the 165 Hz series are the SAME camera pose on the SAME path. Compared as
    // blur STRENGTH (DBI against each run's own blur-off reference) rather than
    // as raw pixels, because TAA converges per FRAME and is therefore legitimately
    // framerate-dependent; this isolates the motion blur from that.
    // =======================================================================
    {
        const RailsConfig r165off = baseRails("hz165_off", kCamSpeed, kDt165, 24, 16);
        const RailsConfig r165on  = baseRails("hz165_on",  kCamSpeed, kDt165, 24, 16);
        const auto s165off = shoot(r165off, true, false, kDt165, 0.0f);
        const auto s165on  = shoot(r165on,  true, true,  kDt165, 0.0f);

        // Sample the matching instant from each run: t = 4/60 = 11/165 s.
        auto oneFrame = [](const std::vector<std::string>& v, size_t i) {
            std::vector<std::string> o;
            if (i < v.size()) o.push_back(v[i]);
            return o;
        };
        const SeriesMetrics m60off  = measureSeries(oneFrame(sMovOffA, 4),  MotionAxis::Horizontal);
        const SeriesMetrics m60on   = measureSeries(oneFrame(sMovOn,   4),  MotionAxis::Horizontal);
        const SeriesMetrics m165off = measureSeries(oneFrame(s165off, 11),  MotionAxis::Horizontal);
        const SeriesMetrics m165on  = measureSeries(oneFrame(s165on,  11),  MotionAxis::Horizontal);
        logMetrics("D3  60Hz/OFF", m60off);
        logMetrics("D3  60Hz/ON ", m60on);
        logMetrics("D3 165Hz/OFF", m165off);
        logMetrics("D3 165Hz/ON ", m165on);
        const double dbi60  = directionalBlurIndex(m60on,  m60off);
        const double dbi165 = directionalBlurIndex(m165on, m165off);
        x3::logInfo("  [metric] D3 blur strength at t = 4/60 s = 11/165 s:  60 Hz DBI "
                    + fmt(dbi60) + "   165 Hz DBI " + fmt(dbi165));
        const double denom = std::max(std::max(dbi60, dbi165), 1e-6);
        const double rel   = std::fabs(dbi60 - dbi165) / denom;
        check(dbi165 > 0.20,
              "D3a AT 165 Hz THE BLUR IS STILL THERE: DBI " + fmt(dbi165) +
              " > 0.20 — the failure this rule prevents is the effect VANISHING at high framerate");
        check(rel < 0.15,
              "D3b IDENTICAL RESULT AT 60 AND 165 Hz: measured blur strength "
              + fmt(dbi60) + " vs " + fmt(dbi165) + ", " + fmt(rel * 100.0, 1) + "% apart (< 15%)");
    }

    // =======================================================================
    // Z1 / Z2 — DEPTH ORDERING. Static camera, fast slab behind a static pillar.
    // The slab must blur. The pillar must NOT be painted with the slab's colour.
    // =======================================================================
    {
        const RailsConfig objOff = baseRails("obj_off", 0.0f, kDt60, 16, 8);
        const RailsConfig objOn  = baseRails("obj_on",  0.0f, kDt60, 16, 8);
        const auto sObjOff = shoot(objOff, true, false, kDt60, kObjSpeed);
        const auto sObjOn  = shoot(objOn,  true, true,  kDt60, kObjSpeed);

        // The pillar (half-width 1.2 m at 7 m, 65 deg fov) subtends about 27% of
        // the half-frame, so the middle 12% of the frame is pillar and nothing
        // else, with margin on both sides for the 40 px dilation reach.
        const Region pillarRoi{ 0.44f, 0.35f, 0.56f, 0.65f };
        const Region slabRoi  { 0.75f, 0.35f, 0.95f, 0.65f };

        const SeriesMetrics pOff = measureSeries(sObjOff, MotionAxis::Horizontal, pillarRoi);
        const SeriesMetrics pOn  = measureSeries(sObjOn,  MotionAxis::Horizontal, pillarRoi);
        const SeriesMetrics bOff = measureSeries(sObjOff, MotionAxis::Horizontal, slabRoi);
        const SeriesMetrics bOn  = measureSeries(sObjOn,  MotionAxis::Horizontal, slabRoi);
        logMetrics("Z pillar/OFF", pOff);
        logMetrics("Z pillar/ON ", pOn);
        logMetrics("Z slab/OFF  ", bOff);
        logMetrics("Z slab/ON   ", bOn);

        const double dbiPillar = directionalBlurIndex(pOn, pOff);
        const double dbiSlab   = directionalBlurIndex(bOn, bOff);
        check(dbiSlab > 0.20,
              "Z2 DEPTH ORDERING, positive half: the moving background slab IS blurred "
              "(DBI " + fmt(dbiSlab) + " > 0.20) — Z1 is about ordering, not about a frame "
              "where nothing blurred");
        check(dbiPillar < 0.08,
              "Z1 DEPTH ORDERING: the STATIC FOREGROUND pillar stays sharp in front of the "
              "fast background (DBI " + fmt(dbiPillar) + " < 0.08 while the slab reads "
              + fmt(dbiSlab) + ") — the background does not smear over the foreground");
    }

    // =======================================================================
    // V1 — VELOCITY ABSENT. The graceful-degradation contract, MEASURED.
    // =======================================================================
    {
        const RailsConfig nvOff = baseRails("novel_off", kCamSpeed, kDt60, 16, 8);
        const RailsConfig nvOn  = baseRails("novel_on",  kCamSpeed, kDt60, 16, 8);
        const auto sNvOff = shoot(nvOff, /*vel*/false, /*blur*/false, kDt60, 0.0f);
        const auto sNvOn  = shoot(nvOn,  /*vel*/false, /*blur*/true,  kDt60, 0.0f);
        check(seriesBitIdentical(sNvOff, sNvOn),
              "V1 VELOCITY ABSENT: with r_velocity 0, r_motionblur 1 is BIT-IDENTICAL to "
              "r_motionblur 0 — the chain is never built, and the fallback is exact");
    }

    x3::logInfo("=== --test-motionblur: " + std::to_string(g_pass) + "/" +
                std::to_string(g_total) + " passed ===");
    std::printf("motionblur: %d/%d passed\n", g_pass, g_total);
    return (g_pass == g_total) ? 0 : 1;
}

} // namespace x3::game
