// Visibility.cpp — the unified visibility policy (see Visibility.h).
#include "Visibility.h"

#include <cstdio>

namespace x3::rhi {

const char* VisPolicy::describe() const {
    switch (mode) {
        case 0:  return "cpu-only";
        case 1:  return pvs ? "pvs+cpu" : "cpu (pvs overridden off)";
        case 2:  return pvs ? "pvs+gpu" : "gpu (pvs overridden off)";
        case 3:  return pvs ? "pvs+gpu+hzb" : "gpu+hzb (pvs overridden off)";
        default: return "?";
    }
}

VisPolicy resolveVisPolicy(int rvis, const VisCaps& caps, int pvsOverride) {
    // -1 auto = the best supported level; out-of-range clamps into [0,3].
    int want = rvis;
    if (want < 0)      want = caps.hzb ? 3 : (caps.gpuCull ? 2 : 1);
    else if (want > 3) want = 3;

    // Degrade unsupported levels (3 -> 2 -> 1). Level 0/1 are always available
    // (the CPU frustum cull is the universal fallback tier).
    if (want == 3 && !caps.hzb)     want = caps.gpuCull ? 2 : 1;
    if (want == 2 && !caps.gpuCull) want = 1;

    VisPolicy p{};
    p.mode = want;
    p.pvs  = (want >= 1);
    if (pvsOverride == 0) p.pvs = false;        // r_roomcull 0 (noclip/debug)
    else if (pvsOverride == 1) p.pvs = true;
    p.cullPath = (want >= 2) ? -1 : 0;          // -1 = device picks the best tier
    p.hzb      = (want >= 3);
    return p;
}

VisFrameStats assembleVisStats(const RenderStats& rs, int resolvedMode) {
    VisFrameStats v{};
    v.mode       = resolvedMode;
    v.activePath = rs.gpuCullPath;
    if (rs.gpuCullPath >= 1) {
        // GPU path: cull.comp's own conserving counters (readback latency).
        v.tested        = rs.gpuCullTested;
        v.frustumCulled = rs.gpuCullFrustum;
        v.hzbCulled     = rs.gpuCullHzb;
        v.drawn         = rs.gpuCullDrawn;
    } else {
        // CPU path: the emit loop tests every submitted instance; survivors draw.
        v.tested        = rs.objectsSubmitted;
        v.drawn         = rs.objectsDrawn;
        v.frustumCulled = (rs.objectsSubmitted >= rs.objectsDrawn)
                              ? rs.objectsSubmitted - rs.objectsDrawn : 0;
        v.hzbCulled     = 0;
    }
    v.roomsCulled = rs.visRoomsCulled;
    v.candidates  = v.roomsCulled + v.tested;
    v.pvsMs       = rs.visPvsMs;
    v.cullCpuMs   = rs.cullCpuMs;
    v.cullGpuMs   = rs.cullGpuMs;
    v.hzbGpuMs    = rs.hzbGpuMs;
    v.conserves   = (v.roomsCulled + v.frustumCulled + v.hzbCulled + v.drawn)
                        == v.candidates;
    return v;
}

std::string formatVisLine(const VisFrameStats& v) {
    const char* path = (v.activePath == 2) ? "tier1"
                     : (v.activePath == 1) ? "tier0"
                     : (v.activePath == 3) ? "tier2" : "cpu";
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "vis L%d(%s) cand %u: rooms %u -> frustum %u -> hzb %u -> drawn %u | "
        "pvs %.2f cull %.2f/%.2f hzb %.2f ms %s",
        v.mode, path, v.candidates, v.roomsCulled, v.frustumCulled, v.hzbCulled,
        v.drawn, v.pvsMs, v.cullCpuMs, v.cullGpuMs, v.hzbGpuMs,
        v.conserves ? "[CONSERVED]" : "[NOT-CONSERVED]");
    return std::string(buf);
}

} // namespace x3::rhi
