#pragma once
// RIVER RAPIDS (feat/river-rapids) — the owner: "Make the river rush...
// rapids, whitewater, foam, but have quiet calm areas too."
//
// Rev 11 shipped the underground river as ONE water pass: a clear, lamp-lit,
// glassy channel from the head grotto to the plunge pool. Glassy everywhere
// is the problem — 1.86 km of river that falls 16 m through a gorge cannot be
// a mirror end to end. This module gives that pass a FLOW FIELD and a REACH
// TABLE, and nothing else: the water is still drawn by shaders/water.{vert,
// frag} through the same WaterParams, extended (not duplicated) with:
//
//   * A 1-D FLOW LUT along the polyline (kFlowSamples samples over the run's
//     length): speed (m/s), turbulence (0..1), standing-wave amplitude (m)
//     and wavelength (m). The vertex stage recovers along-chain s and the
//     lateral offset from the SAME closest-segment search river mode already
//     does for the water level, so flow and level cannot disagree.
//   * The REACH TABLE — Calm / Riffle / Rapid / Plunge segments authored in
//     metres along worldUnderRiverChain(). The derived node table resolves the
//     bed at ~150 m (12 nodes); real rapids are made of steps and boulder
//     gardens far below that resolution, so the reach table is the authored
//     SUB-NODE bed roughness, and the flow model treats it as such: a reach's
//     kind contributes an effective gradient, and speed follows from gradient
//     and channel section exactly as it does between the nodes.
//   * The BOULDERS — a few rocks in the fast reaches, static Jolt bodies with
//     foam wakes the shader draws from their positions (rocks[] in the UBO).
//
// FLOW MODEL. Speed is continuity plus Chezy-flavoured gradient response:
//     v(s) = (v0 + v1 * sqrt(grade_eff / kURRushGrade)) * (Aref / A(s))
//     A(s) = 2 * hw(s) * depth(s)          (wet section; pools deeper & slower)
//     grade_eff = max(node-table grade, reach kind grade * intensity)
// so a pool at 4.5 m depth runs at ~0.45 m/s and the gorge at ~2.2 m/s. A
// Rapid's standing waves take their wavelength from that speed (Froude:
// lambda = 2*pi*v^2/g) clamped to what the 2.5 m water grid can carry, and
// their amplitude from intensity * speed.
//
// THE DOOR: X3_RIVER_RAPIDS=0 leaves every WaterParams byte Rev 11 wrote
// untouched and the new block at zero; the shader takes the legacy path on
// flowInfo.x == 0. --test-riverrapids proves both.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::game {

struct UnderRiverChain;

enum class RiverReachKind : uint8_t { Calm = 0, Riffle = 1, Rapid = 2, Plunge = 3 };

// One authored reach, in METRES along the chain (the polyline's own cum[]),
// s1 <= 0 means "to the end of the run" so the table cannot go stale when the
// route's length moves by a metre. `intensity` scales the kind's character.
struct RiverReach {
    float s0, s1;
    RiverReachKind kind;
    float intensity;
    const char* name;
};

// The door. X3_RIVER_RAPIDS unset or "1" = ON; "0" = OFF (Rev 11 look). Read
// once, like X3_WATER_CLARITY. The self-test flips it through
// riverRapidsForce() so both states are exercised in one process.
bool riverRapidsEnabled();
void riverRapidsForce(int state);   // -1 = env decides, 0 = off, 1 = on

// The authored reach table for the canon underground river.
const RiverReach* underRiverReaches(uint32_t& count);
const char* riverReachKindName(RiverReachKind k);

// Per-kind constants of the model (exported so the gate can quote them).
float riverReachKindGrade(RiverReachKind k);   // sub-node effective gradient
float riverReachKindTurb(RiverReachKind k);    // whitewater ceiling 0..1
constexpr float kRiverReachEdge = 12.0f;       // half-width of the reach ramp (m)

// Reach lookup at along-chain s (metres): the reach containing s, and the
// blended turbulence 0..1 (kind turb * intensity, ramped over kRiverReachEdge
// so a Rapid does not start on a line).
const RiverReach& riverReachAt(float s, float total);
float riverReachTurbulenceAt(float s, float total);
float riverReachGradeAt(float s, float total);

// THE FLOW MODEL — pure, so the gate can drive it directly:
// v = (v0 + v1 * sqrt(grade / kURRushGrade)) * (Aref / (2 * halfWidth * depth)).
float riverFlowSpeed(float grade, float halfWidth, float depth);

// The flow at along-chain s on the underground chain (grade from the node
// table, effective grade from the reach, section from hw/bedDrop).
struct RiverFlowSample {
    float speed = 0.0f;      // m/s along the downstream tangent
    float turbulence = 0.0f; // 0..1
    float waveAmp = 0.0f;    // standing-wave amplitude (m)
    float waveLen = 0.0f;    // standing-wave wavelength (m)
    float grade = 0.0f;      // effective gradient used
    float depth = 0.0f;      // wet depth used (m)
    float halfWidth = 0.0f;  // wet half-width used (m)
};
RiverFlowSample underRiverFlowAt(const UnderRiverChain& uc, float s);

// THE WHITEWATER, mirrored from water.frag one expression each (PAIRED — the
// gate evaluates these, the GPU draws those; a drift between them is a red):
//   riverStandingWaveCrest — the primary crest train's normalized height at
//     (s, lateral/hw): -1 trough .. +1 crest, the crest-sharpened sine the
//     vertex stage displaces by (chan.z to the fragment stage).
//   riverWhitewaterMask — WHERE foam forms, 0..1: the standing-wave crest
//     caps (bands across the channel), the banks, the boulder bow piles and
//     wakes, and a little of the reach's own turbulence everywhere else. The
//     fast water between those is meant to stay dark and glossy.
//   riverWhitewaterLace — the foam pattern itself at ONE flow-frame
//     coordinate q = (along / kLaceStretch, across), i.e. stretched
//     kLaceStretch x along the current: four octaves down to ~0.2 m plus a
//     1-D across-flow streak term.
//   riverWhitewaterCover — the near-binary cover of one phase: lace over a
//     threshold that falls with the mask (bubbles are foam or not; no milk).
//   riverWhitewaterCoverBlend — the two advected phases (qA/qB/wA from
//     riverFlowAdvect) each THRESHOLDED, each faded hard around its own
//     half-weight, the brighter one kept: a blend of the two lace fields
//     first (v2) halved the contrast whenever both phases carried weight
//     and every edge went soft; a linear blend of the covers (v3) showed
//     the fading phase as a grey ghost of every raft.
constexpr float kLaceStretch = 4.0f;
float riverStandingWaveCrest(float s, float latN, float waveLen, float time);
float riverWhitewaterMask(float turb, float latN, float crest, float wake);
float riverWhitewaterLace(const float q[2], float fine);
float riverWhitewaterThreshold(float mask);
float riverWhitewaterCover(float mask, float lace);
float riverWhitewaterCoverBlend(float mask, float laceA, float laceB, float wA);
// FOAM BASE at (s, lateral): the mask at a crest cap with no rock near — the
// noise-free propensity the gate asserts "0 in calm, > 0.5 mid-rapid" on.
float riverFoamBaseAt(const UnderRiverChain& uc, float s, float lat);
// FOAM COVERAGE of a reach at along-chain s: the mean cover over a patch of
// three wavelengths by the middle 60% of the wet width, 0.1 m samples, at
// time t — the number the owner sees as "how white is the rapid" (gate R9
// wants 0.25..0.50 mid-Rapid: whitewater, not a milk bath).
float riverWhitewaterCoverage(const UnderRiverChain& uc, float s, float time);

// THE BOULDERS. Authored as (s, lateral, radius); world placement derived
// from the chain + the carved bed (terrainHeightAtWorld) so the rock sits on
// the bed and breaks the surface by kBoulderShow metres whatever the bed does.
struct RiverBoulder {
    float x = 0, y = 0, z = 0;   // centre, world
    float radius = 0;            // horizontal radius (m); vertical = radius * kBoulderSquash
    float show = 0;              // crown height above the water (m)
    float dirX = 0, dirZ = 0;    // downstream unit tangent at the rock
    float wakeLen = 0;           // foam wake length downstream (m)
    float s = 0, lat = 0;        // where on the chain it sits
};
constexpr float kBoulderSquash = 0.80f;   // river cobble is flatter than a sphere
constexpr float kBoulderShow   = 1.25f;   // DEFAULT crown above the water (m); 0.55 vanished under the standing waves. Specs may stand taller.
uint32_t underRiverBoulders(const UnderRiverChain& uc, RiverBoulder* out, uint32_t maxN);

// Fill WaterParams' flow block for the channel the params already describe.
// No-op (block stays zero) when the door is shut. The underground bake reads
// the chain; the surface bake gets the risen nodes it was built from and is
// Calm end to end (the approved shoreline stays glassy; the ripples merely
// drift with the current).
void bakeUnderRiverFlow(x3::rhi::IRenderDevice::WaterParams& wp);
void bakeSurfaceRiverFlow(x3::rhi::IRenderDevice::WaterParams& wp);

// THE ADVECTION LAW, mirrored from water.frag (PAIRED — see the shader's
// flowAdvect()): two phases of period kFlowCycle, each carrying the ripple
// pattern downstream by speed*(phase-0.5)*kFlowCycle and cross-faded so the
// wrap of one lands where its weight is zero. Exposed so the dt-scaling gate
// can show that the pattern moves speed*dt metres for a dt-second step.
constexpr float kFlowCycle = 2.4f;   // seconds per advection phase
void riverFlowAdvect(float time, float speed, float dirX, float dirZ,
                     float offA[2], float offB[2], float& weightA);

// Headless gate: --test-riverrapids.
bool runRiverRapidsSelfTest();

} // namespace x3::game
