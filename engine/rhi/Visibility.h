#pragma once
// Visibility.h — the ONE culling brain (vis-unify). Public, graphics-API-free.
//
// X3 grew FOUR visibility systems, each with its own cvar and its own stats:
//   (1) room/portal PVS        — app-side flood-fill (level_loader.h), r_roomcull
//   (2) CPU frustum cull       — device emit loop (FrustumCull.h), r_frustumcull
//   (3) GPU cull Tiers 0/1     — cull.comp compute (GpuCull.h), r_cullpath
//   (4) HZB occlusion          — depth pyramid riding the GPU path, r_hzb
// This header is the single POLICY that decides, per frame, which stages run
// and how they FEED each other:
//
//   PVS (app)  ->  room-visible instance SET  ->  GPU cull input (frustum+HZB)
//                                             \->  CPU frustum cull (fallback
//                                                  tier + equivalence reference)
//
// The PVS prefilter happens at SUBMISSION (Scene::render skips room-culled
// entities), so with the GPU path on, cull.comp's tested set is exactly the
// PVS survivor set — the stages COMPOSE instead of running independently.
//
// ONE cvar drives it (r_vis); the legacy cvars remain as compat aliases that
// map onto it (app/main.cpp logs a deprecation line when they're touched).
//
// POLICY TABLE (resolveVisPolicy):
//   r_vis | PVS | CPU frustum | GPU cull        | HZB | notes
//   ------+-----+-------------+-----------------+-----+---------------------------
//    0    | off | ON          | off             | off | reference floor ("off")
//    1    | ON  | ON          | off             | off | legacy default behaviour
//    2    | ON  | (GPU does)  | auto tier (0/1) | off | degrades to 1 w/o GPU cull
//    3    | ON  | (GPU does)  | auto tier (0/1) | ON  | degrades to 2 w/o HZB
//   -1    | auto: the best of the above the device supports (3 -> 2 -> 1)
//
// "CPU frustum (GPU does)": with the GPU path active the SAME normalized-plane
// sphere predicate runs in cull.comp (bit-equivalent — the D15 acceptance gate
// "GPU statDrawn == CPU objectsDrawn" stays the proof). r_frustumcull remains
// the independent predicate bypass (ALWAYS_VISIBLE everything) on BOTH paths.
//
// This is a pure-policy module: no Vulkan, no device, usable headless (tests).

#include "IRenderDevice.h"   // RenderStats (POD, graphics-API-free)

#include <cstdint>
#include <string>

namespace x3::rhi {

// What the device reports it can do (filled from RenderStats caps fields).
struct VisCaps {
    bool gpuCull   = false;   // cull.comp pipelines live (Tier 0 at least)
    bool asyncCull = false;   // dedicated compute queue (Tier 1)
    bool hzb       = false;   // depth pyramid targets live
};

// The per-frame resolved policy. Apply with:
//   app:    scene.setRoomCullEnabled(policy.pvs)  (+ the PVS flood-fill gate)
//   device: setCullPath(policy.cullPath); setHzbEnabled(policy.hzb);
struct VisPolicy {
    int  mode     = 1;       // RESOLVED r_vis level actually in effect (0..3)
    bool pvs      = true;    // room/portal PVS prefilters the submit set
    int  cullPath = 0;       // device cull path request (0 = CPU, -1 = auto GPU tier)
    bool hzb      = false;   // HZB occlusion phase on the GPU path
    const char* describe() const;   // "pvs+gpu" etc. (stable, for logs/HUD)
};

// Resolve r_vis (+ caps) into the per-frame policy. `rvis`: -1 auto, 0..3 as
// the table above; out-of-range clamps. Unsupported levels DEGRADE (3 -> 2 -> 1)
// and the resolved `mode` reports what is actually in effect. `pvsOverride`:
// -1 = none, 0 = force PVS off (the r_roomcull-0 noclip/debug alias), 1 = force
// on. The override only touches the PVS stage — the GPU path keeps running.
VisPolicy resolveVisPolicy(int rvis, const VisCaps& caps, int pvsOverride = -1);

// ---------------------------------------------------------------------------
// ONE stats block. The numbers CONSERVE:
//   roomsCulled + frustumCulled + hzbCulled + drawn == candidates
// where candidates = roomsCulled (PVS submission skips) + tested (instances
// the active cull stage evaluated). On the GPU path the cull counters are read
// back with frames-in-flight latency (steady on a still camera, momentarily
// offset right after a scene/policy change — same caveat as gpuFrameMs).
// ---------------------------------------------------------------------------
struct VisFrameStats {
    int      mode = 0;            // resolved r_vis level
    int      activePath = 0;      // 0 CPU, 1 Tier 0, 2 Tier 1 async
    uint32_t candidates    = 0;   // PVS input set (skips + tested)
    uint32_t roomsCulled   = 0;   // PVS: entities not submitted (room-invisible)
    uint32_t tested        = 0;   // instances the frustum/HZB stage evaluated
    uint32_t frustumCulled = 0;
    uint32_t hzbCulled     = 0;
    uint32_t drawn         = 0;
    float    pvsMs     = 0.0f;    // PVS flood-fill CPU time (host-injected)
    float    cullCpuMs = 0.0f;    // device emit/cull walk CPU time
    float    cullGpuMs = 0.0f;    // cull.comp dispatch GPU time (graphics queue)
    float    hzbGpuMs  = 0.0f;    // pyramid reduce GPU time (graphics queue)
    bool     conserves = false;   // the conservation identity above
};

// Assemble the unified block from a device stats snapshot. `resolvedMode` is
// the policy level the host applied (the device only knows the GPU sub-path).
VisFrameStats assembleVisStats(const RenderStats& rs, int resolvedMode);

// One-line render of the block for logs / the smoketest:
// "vis L2(tier1) cand C: rooms R -> frustum F -> hzb H -> drawn D | pvs p.pp cull c.cc[/g.gg gpu] ms [CONSERVED]"
std::string formatVisLine(const VisFrameStats& v);

} // namespace x3::rhi
