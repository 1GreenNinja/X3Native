#pragma once
// Shared include surface for the extracted --world host TUs (#28 deep split).
// These are the engine + app headers the lifted host bodies reference. Kept in
// one place so every host TU has the identical environment the inline body had.

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/x3_boot.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include "../host_context.h"
#include "../world_hosts.h"

#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace x3::apphost {

// ===========================================================================
// CONTENT WIRING (lane inspx/content-wiring) — OUTDOOR CASCADED SHADOWS.
//
// Cascaded shadow maps shipped behind `r_csm` and NOTHING TURNED THEM ON in a
// world host. The cvar is registered and pushed to the device in exactly one
// place — runDefaultHost's applyRtaoCVars — and the --world hosts run instead
// of that function, never alongside it. So `--world cliffs --set r_csm 1`
// silently rendered the legacy single 45 m camera-locked ortho box, which is
// the whole problem CSM exists to fix: at 250 m of view depth over streamed
// terrain, everything past 45 m simply has no shadow.
//
// Call this from any host with a SUN and a real view depth. Defaults to ON for
// those worlds (that is the point of the lane); `--set r_csm 0` restores the
// legacy single cascade exactly, which is the bit-exactness escape hatch.
//
// CONTRACT PRESERVED: a host that pins its shadow box with setShadowBounds()
// makes CSM stand down (vk_passes.cpp: `active = m_csmEnabled && !m_shadowOverride`).
// Do not call this from such a host without removing the pin first.
// ===========================================================================
inline void applyOutdoorCsm(const HostContext& hc, x3::rhi::IRenderDevice& device,
                            float distanceMeters, const char* who) {
    auto cv = [&](const char* name, const char* dflt) -> std::string {
        for (const auto& kv : hc.cliCVars) if (kv.first == name) return kv.second;
        return dflt;
    };
    x3::rhi::IRenderDevice::CsmParams c{};
    c.enabled     = cv("r_csm", "1") != "0";          // ON by default for outdoor hosts
    c.lambda      = (float)std::atof(cv("r_csm_lambda", "0.75").c_str());
    c.distance    = (float)std::atof(cv("r_csm_dist", "0").c_str());
    if (c.distance <= 0.0f) c.distance = distanceMeters;   // per-world default
    c.blend       = (float)std::atof(cv("r_csm_blend", "0.12").c_str());
    c.forwardBias = (float)std::atof(cv("r_shadowforward", "0").c_str());
    c.debug       = cv("r_csm_debug", "0") != "0";
    device.setCsmParams(c);
    x3::logInfo(std::string("[csm] --world ") + who + ": cascades " +
                (c.enabled ? "ON" : "OFF (r_csm 0 -- legacy single 45 m box)") +
                (c.enabled ? (" over " + std::to_string((int)c.distance) + " m of view depth")
                           : std::string()));
}

} // namespace x3::apphost
