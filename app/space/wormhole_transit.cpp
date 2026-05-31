// app/space/wormhole_transit.cpp — S3 wormhole transit implementation.
#include "wormhole_transit.h"

#include "space_layer.h"
#include "wormhole_vfx.h"

#include <algorithm>

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

} // namespace x3::space
