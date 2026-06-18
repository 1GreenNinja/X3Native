// app/space/space_layer.cpp — S0 SpaceLayer spine implementation.
#include "space_layer.h"

#include <cassert>
#include <cmath>
#include <cstring>

namespace x3::space {

void SpaceLayer::init() {
    context_ = Context::DeepSpace;
    pending_ = Pending::None;
    const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    std::memcpy(env_, I, sizeof(env_));
    linVel_[0] = linVel_[1] = linVel_[2] = 0.0f;
    angVel_[0] = angVel_[1] = angVel_[2] = 0.0f;
    slots_.clear();
    freeList_.clear();
    live_.clear();
    liveCount_ = 0;
    // Runners intentionally preserved across init() so a subsystem can
    // register once at startup; nothing here re-arms a transition.
}

Context SpaceLayer::context() const { return context_; }

// ---- Transitions -----------------------------------------------------------

void SpaceLayer::requestWormhole(uint32_t /*destSystemId*/) {
    // Arm a wormhole transit. The intermediate visible state is
    // WormholeTransit; the runner ticks until the jump completes, then we
    // arrive back in DeepSpace at the destination system.
    pending_ = Pending::Wormhole;
    context_ = Context::WormholeTransit;
}

void SpaceLayer::requestDescent(uint32_t /*planetId*/) {
    // Arm a cinematic descent. Intermediate state AtmoDescent; on completion
    // we hand off to the Surface --world.
    pending_ = Pending::Descent;
    context_ = Context::AtmoDescent;
}

void SpaceLayer::requestAscent() {
    // Ascent has no intermediate cinematic of its own here; it simply returns
    // to DeepSpace once ticked. (A descent runner is reused if present so the
    // same on-rails sequence can play in reverse.)
    pending_ = Pending::Ascent;
    // Leave context as-is (typically Surface) until the transition completes.
}

void SpaceLayer::tickTransition(float dt) {
    if (pending_ == Pending::None) return;

    bool complete = true;
    switch (pending_) {
        case Pending::Wormhole:
            if (wormholeRunner_) complete = wormholeRunner_(dt);
            if (complete) context_ = Context::DeepSpace; // arrived at dest system
            break;
        case Pending::Descent:
            if (descentRunner_) complete = descentRunner_(dt);
            if (complete) context_ = Context::Surface;   // handed to --world
            break;
        case Pending::Ascent:
            if (descentRunner_) complete = descentRunner_(dt);
            if (complete) context_ = Context::DeepSpace;
            break;
        case Pending::None:
            break;
    }
    if (complete) pending_ = Pending::None;
}

// ---- Environment transform -------------------------------------------------

void SpaceLayer::setEnvironmentVelocity(const float vel[3], const float angVel[3]) {
    linVel_[0] = vel[0]; linVel_[1] = vel[1]; linVel_[2] = vel[2];
    angVel_[0] = angVel[0]; angVel_[1] = angVel[1]; angVel_[2] = angVel[2];
}

void SpaceLayer::environmentTransform(float out16[16]) const {
    std::memcpy(out16, env_, sizeof(env_));
}

// Multiply column-major 4x4 matrices: out = a * b.
static void mul4x4(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int c = 0; c < 4; ++c) {       // column of b / result
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    }
    std::memcpy(out, r, sizeof(r));
}

void SpaceLayer::update(float dt) {
    // Advance any active staged transition first.
    tickTransition(dt);

    // Integrate the environment transform from linear + angular velocity.
    // Ship-static / environment-moves model: build a per-step delta and
    // pre-multiply so accumulated motion compounds frame over frame.
    const float wx = angVel_[0] * dt;
    const float wy = angVel_[1] * dt;
    const float wz = angVel_[2] * dt;

    // Small-angle-friendly per-axis rotation, composed Rz*Ry*Rx. Using exact
    // sin/cos here keeps it correct for large dt too, but the model is
    // explicitly fine with small-angle approximation.
    const float cx = std::cos(wx), sx = std::sin(wx);
    const float cy = std::cos(wy), sy = std::sin(wy);
    const float cz = std::cos(wz), sz = std::sin(wz);

    // Rx (column-major)
    const float Rx[16] = {
        1,   0,   0,   0,
        0,   cx,  sx,  0,
        0,  -sx,  cx,  0,
        0,   0,   0,   1,
    };
    // Ry (column-major)
    const float Ry[16] = {
        cy,  0,  -sy,  0,
        0,   1,   0,   0,
        sy,  0,   cy,  0,
        0,   0,   0,   1,
    };
    // Rz (column-major)
    const float Rz[16] = {
        cz,  sz,  0,  0,
       -sz,  cz,  0,  0,
        0,   0,   1,  0,
        0,   0,   0,  1,
    };

    float Rzy[16];
    mul4x4(Rz, Ry, Rzy);
    float dR[16];
    mul4x4(Rzy, Rx, dR);
    // Translation delta lives in column 3 (column-major: indices 12,13,14).
    dR[12] = linVel_[0] * dt;
    dR[13] = linVel_[1] * dt;
    dR[14] = linVel_[2] * dt;

    // Compose the delta onto the accumulated transform.
    float next[16];
    mul4x4(dR, env_, next);
    std::memcpy(env_, next, sizeof(env_));
}

// ---- Proxy registry --------------------------------------------------------

void SpaceLayer::rebuildLive() {
    live_.clear();
    for (uint32_t i = 0; i < (uint32_t)slots_.size(); ++i)
        if (slots_[i].live) live_.push_back(i);
}

uint32_t SpaceLayer::addProxy(const Proxy& p) {
    uint32_t id;
    if (!freeList_.empty()) {
        id = freeList_.back();
        freeList_.pop_back();
        slots_[id].proxy = p;
        slots_[id].live  = true;
    } else {
        id = (uint32_t)slots_.size();
        slots_.push_back(Slot{ p, true });
    }
    ++liveCount_;
    rebuildLive();
    return id;
}

void SpaceLayer::updateProxy(uint32_t id, const Proxy& p) {
    if (id < slots_.size() && slots_[id].live)
        slots_[id].proxy = p;
}

void SpaceLayer::removeProxy(uint32_t id) {
    if (id < slots_.size() && slots_[id].live) {
        slots_[id].live = false;
        freeList_.push_back(id);
        --liveCount_;
        rebuildLive();
    }
}

uint32_t SpaceLayer::proxyCount() const { return liveCount_; }

const Proxy& SpaceLayer::proxy(uint32_t i) const {
    assert(i < live_.size() && "proxy(i): index out of live range");
    return slots_[live_[i]].proxy;
}

// ---- Runner registration ---------------------------------------------------

void SpaceLayer::registerWormholeRunner(TransitionFn fn) { wormholeRunner_ = std::move(fn); }
void SpaceLayer::registerDescentRunner(TransitionFn fn)  { descentRunner_  = std::move(fn); }

} // namespace x3::space
