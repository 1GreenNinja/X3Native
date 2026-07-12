// app/space/wormhole_transit.cpp — S3 wormhole transit implementation.
#include "wormhole_transit.h"

#include "space_layer.h"
#include "wormhole_vfx.h"
#include "../headless_device.h"     // HeadlessRenderDevice for the self-test
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace x3::space {

void WormholeTransit::init(rhi::IRenderDevice& dev, SpaceLayer& layer,
                           float durationSec) {
    duration_ = (durationSec > 0.0f) ? durationSec : 6.0f;
    elapsed_  = 0.0f;
    progress_ = 0.0f;
    active_   = false;

    // Bring up the crystal-matrix tunnel VFX (GPU). Owned for our lifetime.
    if (!vfx_) vfx_ = new WormholeVfx();
    vfx_->init(dev);

    // Register the runner with the S0 spine. requestWormhole() arms the pending
    // transition; each SpaceLayer.update(dt) ticks THIS lambda until it returns
    // true. The lambda owns the transit timer + progress ramp; SpaceLayer owns
    // the Context state machine (WormholeTransit -> DeepSpace on completion).
    layer.registerWormholeRunner([this](float dt) -> bool {
        // First tick of a fresh transit: the runner was just (re)armed, so reset
        // the timer. `active_` flips false on the completing tick below, so a
        // false->arm here marks the start of a new jump.
        if (!active_) {
            elapsed_  = 0.0f;
            progress_ = 0.0f;
            active_   = true;
        }
        elapsed_ += dt;
        progress_ = std::clamp(elapsed_ / duration_, 0.0f, 1.0f);
        if (progress_ >= 1.0f) {
            // Jump complete: SpaceLayer lands back in DeepSpace at the dest.
            active_ = false;
            return true;
        }
        return false;
    });
}

void WormholeTransit::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                             const float* viewProj16, float timeSec) {
    if (!vfx_ || !vfx_->initialized()) return;
    // Draw the tunnel at the current progress so the core blooms to white-hot
    // convergence as the jump finishes. The host owns the camera transform.
    vfx_->render(dev, fr, viewProj16, timeSec, progress_);
}

float WormholeTransit::progress() const { return progress_; }
bool  WormholeTransit::active() const   { return active_; }

void WormholeTransit::shutdown(rhi::IRenderDevice& dev) {
    if (vfx_) {
        vfx_->shutdown(dev);
        delete vfx_;
        vfx_ = nullptr;
    }
    active_   = false;
    progress_ = 0.0f;
    elapsed_  = 0.0f;
}

// ---------------------------------------------------------------------------
// --test-wormhole-transit: S3 wormhole transit self-test. Headless -- wires a
// WormholeTransit runner into a SpaceLayer and drives the S0 spine:
// requestWormhole(dest) -> Context::WormholeTransit + active(); stepping ramps
// progress 0->1 monotonically; at 1.0 the layer lands back in DeepSpace and the
// transit completes; a second jump re-arms cleanly. Ported byte-faithfully from
// the pre-split monolith main() inline block into this lane TU (integration
// feast fold of the 14900K's feat/wormhole-transit lane).
// ---------------------------------------------------------------------------
bool runWormholeTransitSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
        else   {          x3::logError(std::string("  [FAIL] ") + name); }
    };

    x3::game::HeadlessRenderDevice hdev;

    // T1: init brings up the owned VFX and the transit starts idle.
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, /*durationSec=*/6.0f);
        check(!wt.active(), "T1 init() -> not active (no transit armed yet)");
        check(wt.progress() == 0.0f, "T1b init() -> progress 0");
        wt.shutdown(hdev);
    }

    // T2: requestWormhole -> WormholeTransit + active(); stepping ramps
    // progress 0->1; at 1.0 the context lands in DeepSpace and active==false.
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        const float dur = 6.0f;
        wt.init(hdev, L, dur);

        L.requestWormhole(/*destSystemId=*/42u);
        check(L.context() == Context::WormholeTransit,
              "T2 requestWormhole -> Context::WormholeTransit");

        L.update(1.0f);
        check(wt.active(), "T2b after first update -> active()");
        check(wt.progress() > 0.0f && wt.progress() < 1.0f,
              "T2c progress ramps into (0,1)");
        check(L.context() == Context::WormholeTransit,
              "T2d still in WormholeTransit mid-jump");

        float prev = wt.progress();
        bool monotonic = true;
        for (int i = 0; i < 5; ++i) {
            L.update(1.0f);
            if (wt.progress() < prev) monotonic = false;
            prev = wt.progress();
        }
        check(monotonic, "T2e progress is monotonic non-decreasing");
        check(wt.progress() >= 1.0f, "T2f progress reaches 1.0 at duration");
        check(L.context() == Context::DeepSpace,
              "T2g transit complete -> back in DeepSpace (arrived at dest)");
        check(!wt.active(), "T2h transit complete -> active()==false");

        wt.shutdown(hdev);
    }

    // T3: progress() never exceeds 1.0 even if over-stepped past duration.
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, /*durationSec=*/2.0f);
        L.requestWormhole(7u);
        L.update(100.0f); // wildly over-step
        check(wt.progress() == 1.0f, "T3 progress clamps to 1.0 on over-step");
        check(L.context() == Context::DeepSpace, "T3b over-step still lands DeepSpace");
        check(!wt.active(), "T3c over-step completes the transit");
        wt.shutdown(hdev);
    }

    // T4: a second jump after the first re-arms cleanly (timer resets).
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, /*durationSec=*/4.0f);
        L.requestWormhole(1u);
        for (int i = 0; i < 4; ++i) L.update(1.0f);
        check(L.context() == Context::DeepSpace && !wt.active(),
              "T4 first jump completes");
        L.requestWormhole(2u);
        L.update(1.0f);
        check(wt.active() && wt.progress() > 0.0f && wt.progress() < 1.0f,
              "T4b second jump re-arms with a fresh progress ramp");
        wt.shutdown(hdev);
    }

    // T5: render() before/after init is VUID-safe (no crash; no-op pre-init).
    {
        SpaceLayer L; L.init();
        WormholeTransit wt;
        wt.init(hdev, L, 6.0f);
        rhi::FrameContext fr = hdev.beginFrame();
        const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        wt.render(hdev, fr, idM, /*timeSec=*/0.5f); // active()==false here -> still safe
        L.requestWormhole(3u);
        L.update(1.0f);
        wt.render(hdev, fr, idM, 1.0f);             // mid-transit draw
        hdev.endFrame(fr);
        check(true, "T5 render() is crash-free pre- and mid-transit");
        wt.shutdown(hdev);
        rhi::FrameContext fr2 = hdev.beginFrame();
        wt.render(hdev, fr2, idM, 2.0f);
        hdev.endFrame(fr2);
        check(!wt.active() && wt.progress() == 0.0f,
              "T5b shutdown() resets active()/progress() and render() stays safe");
    }

    x3::logInfo("wormhole-transit: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    std::printf("wormhole-transit: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
