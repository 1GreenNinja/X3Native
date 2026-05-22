#pragma once
// SimClock — the deterministic fixed-step simulation accumulator (§3.1).
// Spec: specs/NETCODE-architecture.spec.md §3.1 / §3.4 (Fiedler "Fix Your
// Timestep!"). Header-only so main.cpp's ONE accumulator block and the
// --test-net determinism test use BYTE-IDENTICAL logic.
//
// The sim advances at a fixed kSimDt (1/60, aligned to Jolt's internal kFixedDt
// so physics steps exactly once per tick). Render stays uncapped; leftover time is
// carried in the accumulator. Same input + same dt sequence => same tick count =>
// deterministic single-machine sim (the prerequisite for prediction re-sim, demo
// replay, and reproducible tests).

#include "engine/net/NetTypes.h"

namespace x3::net {

// Fixed simulation cadence. 60 Hz to start (open decision §10.2.1), and it ALIGNS
// with Jolt's internal kFixedDt = 1/60 so a single step(kSimDt) maps to exactly
// one internal physics Update.
constexpr float kSimHz = 60.0f;
constexpr float kSimDt = 1.0f / kSimHz;

// Spiral-of-death clamp: never accumulate more than this much real time of catch-up
// in one frame (matches JoltPhysicsWorld's own 0.25 s accumulator cap).
constexpr float kMaxAccum = 0.25f;

// Carries leftover real time between frames and yields whole fixed steps.
struct SimAccumulator {
    float accum = 0.0f;     // unconsumed real time (seconds)
    NetTick tick = 0;       // monotone sim tick number

    // Feed wall-clock dt; returns how many fixed kSimDt steps to run THIS frame.
    // Clamps the accumulator first (anti-spiral), then drains whole steps. The
    // caller loops `for (uint32_t i=0;i<n;++i) serverTick(kSimDt);`. Deterministic:
    // the same dt sequence yields the same total step count regardless of frame
    // boundaries (leftover always carries forward).
    uint32_t advance(float realDt) {
        if (realDt > 0.0f) accum += realDt;
        if (accum > kMaxAccum) accum = kMaxAccum;
        uint32_t steps = 0;
        while (accum >= kSimDt) {
            accum -= kSimDt;
            ++tick;
            ++steps;
        }
        return steps;
    }
};

} // namespace x3::net
