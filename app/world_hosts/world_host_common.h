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

// ===========================================================================
// RENDER-PASS A/B FOR --world HOSTS (diagnostic wiring).
//
// SAME BUG CLASS as applyOutdoorCsm above: the per-frame cvar sync hub
// (app_run.cpp applyRtaoCVars) belongs to runDefaultHost, and the --world
// hosts run INSTEAD of it — so `--world tunnel --set r_ssao 0` silently
// rendered the default and every A/B taken that way was a no-op.
//
// STRICTLY OPT-IN: a device setter is called ONLY when the matching cvar was
// actually passed on the command line, so a run with no --set is byte-for-byte
// the behaviour it always had. This is the screen-space-artifact bisect tool.
// ===========================================================================
inline void applyWorldHostRenderCVars(const HostContext& hc, x3::rhi::IRenderDevice& device) {
    if (hc.cliCVars.empty()) return;
    auto has = [&](const char* n) {
        for (const auto& kv : hc.cliCVars) if (kv.first == n) return true;
        return false;
    };
    auto s = [&](const char* n, const char* d) -> std::string {
        for (const auto& kv : hc.cliCVars) if (kv.first == n) return kv.second;
        return d;
    };
    auto i = [&](const char* n, const char* d) { return std::atoi(s(n, d).c_str()); };
    auto f = [&](const char* n, const char* d) { return (float)std::atof(s(n, d).c_str()); };

    if (has("r_debugview")) device.setDebugView(i("r_debugview", "0"));
    if (has("r_exposure"))  device.setExposure(f("r_exposure", "0.88"));

    if (has("r_ssao") || has("r_ssao_strength") || has("r_ssao_radius") ||
        has("r_ssao_bias") || has("r_ssao_intensity") || has("r_ssao_power")) {
        x3::rhi::IRenderDevice::SsaoParams p{};
        p.enabled   = i("r_ssao", "1") != 0;
        p.radius    = f("r_ssao_radius", "0.5");
        p.bias      = f("r_ssao_bias", "0.025");
        p.intensity = f("r_ssao_intensity", "1.0");
        p.power     = f("r_ssao_power", "1.5");
        p.strength  = f("r_ssao_strength", "0.9");
        device.setSsaoParams(p);
    }
    if (has("r_ssgi")) {
        x3::rhi::IRenderDevice::GiParams p{};
        p.enabled = i("r_ssgi", "1") != 0;
        device.setGiParams(p);
    }
    if (has("r_rtao")) {
        x3::rhi::IRenderDevice::RtaoParams p{};
        p.enabled  = i("r_rtao", "0") != 0;
        p.radius   = f("r_rtao_radius", "0.5");
        p.rays     = i("r_rtao_rays", "8");
        p.strength = f("r_rtao_strength", "0.85");
        device.setRtaoParams(p);
    }
    if (has("r_ssr") || has("r_rtreflections") || has("r_refldenoise") || has("r_reflquality")) {
        x3::rhi::IRenderDevice::ReflectionParams r{};
        r.ssr        = i("r_ssr", "1") != 0;
        r.rtFallback = i("r_rtreflections", "1") != 0;
        r.fullRes    = i("r_reflquality", "0") != 0;
        r.intensity  = f("r_reflintensity", "1");
        r.denoiseIters      = i("r_refldenoise", "4");
        r.denoiseDepthSigma = f("r_refldn_depth", "0.06");
        r.denoiseNormalPow  = f("r_refldn_normal", "16");
        r.denoiseDiscScale  = f("r_refldn_disc", "0.4");
        device.setReflectionParams(r);
    }
    if (has("r_bloom") || has("r_taa") || has("r_taasharpen") || has("r_tonemap") ||
        has("r_autoexposure") || has("r_filmic")) {
        x3::rhi::IRenderDevice::PostFXParams px{};
        px.tonemapMode    = i("r_tonemap", "1");
        px.bloomEnabled   = i("r_bloom", "1") != 0;
        px.bloomIntensity = f("r_bloomintensity", "-1");
        px.bloomThreshold = f("r_bloomthreshold", "1.10");
        px.autoExposure   = i("r_autoexposure", "1") != 0;
        px.aeSpeed        = f("r_aespeed", "1.5");
        px.aeMin          = f("r_aemin", "0.7");
        px.aeMax          = f("r_aemax", "2.2");
        px.aeKey          = f("r_aekey", "0.18");
        px.taa            = i("r_taa", "1") != 0;
        px.taaSharpen     = f("r_taasharpen", "0.25");
        px.filmicAllowed  = i("r_filmic", "1") != 0;
        device.setPostFX(px);
    }
    if (has("r_rtshadows")) {
        x3::rhi::IRenderDevice::RtShadowParams rs{};
        rs.tier        = i("r_rtshadows", "2");
        rs.sunSizeDeg  = f("r_rtsun_size", "0.5");
        rs.pointMax    = i("r_rtpoint_max", "4");
        rs.pointRadius = f("r_rtpoint_size", "0.10");
        device.setRtShadowParams(rs);
    }
}

} // namespace x3::apphost
