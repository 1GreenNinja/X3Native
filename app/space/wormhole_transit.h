// app/space/wormhole_transit.h
//
// S3 wormhole transit — the Salvari crystal-matrix interstellar jump.
//
// This is the autopilot interstellar-jump sequence: it hooks into the S0
// SpaceLayer spine by registering a wormhole RUNNER (a SpaceLayer::TransitionFn)
// that ramps a transit timer 0..1 over `durationSec`. The runner drives the
// owned WormholeVfx `progress`, so the crystalline tunnel blooms to white-hot
// convergence as the jump finishes. When progress reaches 1.0 the runner returns
// true and the SpaceLayer lands back in DeepSpace at the destination system.
//
// Owns the WormholeVfx (GPU). The state machine itself lives in SpaceLayer; this
// class only contributes the runner + the per-frame VFX draw at the current
// progress.
#pragma once

#include "engine/rhi/IRenderDevice.h"

namespace x3::space {

class SpaceLayer;       // S0 spine (frozen interface)
class WormholeVfx;      // crystal-matrix tunnel VFX (feat/wormhole-vfx)

class WormholeTransit {
public:
    // Wire a wormhole runner into the SpaceLayer: each update(dt) the runner
    // advances an internal timer, progress = clamp(elapsed/durationSec). The
    // runner returns true when progress reaches 1.0 (transit complete). Brings
    // up the owned WormholeVfx (GPU). durationSec must be > 0.
    void init(rhi::IRenderDevice&, SpaceLayer&, float durationSec = 6.0f);

    // Draw the owned WormholeVfx at the CURRENT transit progress. `timeSec`
    // animates the energy flow; the progress drives the white-hot core bloom.
    // A no-op (nothing drawn) while no transit is active.
    void render(rhi::IRenderDevice&, const rhi::FrameContext&,
                const float* viewProj16, float timeSec);

    float progress() const;   // 0..1, current transit completion
    bool  active() const;     // true while a transit is running

    // Release the owned WormholeVfx. Idempotent.
    void  shutdown(rhi::IRenderDevice&);

private:
    WormholeVfx* vfx_ = nullptr;     // owned (heap; fwd-decl keeps header light)
    float duration_   = 6.0f;
    float elapsed_    = 0.0f;
    float progress_   = 0.0f;
    bool  active_     = false;
};

// --test-wormhole-transit: headless self-test of the S3 transit driving the S0
// SpaceLayer spine (integration-feast fold; body lives in wormhole_transit.cpp).
bool runWormholeTransitSelfTest();

} // namespace x3::space
