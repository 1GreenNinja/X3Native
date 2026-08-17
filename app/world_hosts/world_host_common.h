#pragma once
// Shared include surface for the extracted --world host TUs (#28 deep split).
// These are the engine + app headers the lifted host bodies reference. Kept in
// one place so every host TU has the identical environment the inline body had.

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/x3_boot.h"
#include "engine/core/IConsole.h"       // live cvar apply (applyLiveHostRenderCVars)
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include "../host_context.h"
#include "../world_hosts.h"

#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <functional>

namespace x3::apphost {

// ===========================================================================
// THE `--set` AUDIT — "silence is what made this expensive".
//
// Every cvar that ACTUALLY reached the device claims its name here. The two
// host dispatches — dispatchWorldHost() for `--world`, dispatchScreenshotHosts()
// for `--screenshot-*` — then report, LOUDLY, every `--set` on the command line
// that nothing claimed, because the failure mode this whole file exists to kill
// is a run that quietly ignores `--set` and produces a plausible-looking frame
// that proves nothing. BOTH families run INSTEAD of runDefaultHost's per-frame
// cvar sync hub, so both had the identical hole.
// ===========================================================================
inline std::vector<std::string>& claimedHostCVars() {
    static std::vector<std::string> claimed;
    return claimed;
}
inline void claimHostCVar(const std::string& name) {
    auto& v = claimedHostCVars();
    for (const auto& s : v) if (s == name) return;
    v.push_back(name);
}
inline bool hostCVarClaimed(const std::string& name) {
    for (const auto& s : claimedHostCVars()) if (s == name) return true;
    return false;
}

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
    // --set AUDIT: this function IS the r_csm* consumer for outdoor hosts, so the
    // names it reads are applied — claim them or dispatchWorldHost would report
    // them as silently ignored. (An INDOOR host never calls this, and then
    // `--set r_csm 1` genuinely IS ignored — and now says so.)
    for (const char* n : { "r_csm", "r_csm_lambda", "r_csm_dist", "r_csm_blend",
                           "r_shadowforward", "r_csm_debug" }) {
        for (const auto& kv : hc.cliCVars) if (kv.first == n) claimHostCVar(n);
    }
    x3::logInfo(std::string("[csm] --world ") + who + ": cascades " +
                (c.enabled ? "ON" : "OFF (r_csm 0 -- legacy single 45 m box)") +
                (c.enabled ? (" over " + std::to_string((int)c.distance) + " m of view depth")
                           : std::string()));
}

// ===========================================================================
// RENDER-PASS A/B FOR --world HOSTS — `--world X --set r_bloom 0` etc.
//
// THE BUG THIS CLOSES. The per-frame cvar->device sync hub (app_run.cpp's
// applyRtaoCVars) belongs to runDefaultHost, and every `--world` host runs
// INSTEAD of it, never alongside it. So `--world tunnel --set r_bloom 0`
// SILENTLY RENDERED THE DEFAULT WORLD with the cvar unapplied — it failed
// quietly and the results looked completely plausible. It has already voided
// real evidence: a lane concluded an artifact "survives r_bloom 0 / r_taa 0 /
// r_taasharpen 0" when none of those three tests had ever run. applyOutdoorCsm
// above documented exactly this gap for r_csm; nobody generalized it.
//
// NOT A PER-HOST OPT-IN. This is called from dispatchWorldHost() for EVERY
// route in kHostRoutes, before the host body runs — a per-host call that has to
// be remembered is the same trap one layer up. A new --world added to the route
// table is wired the moment it is dispatchable.
//
// TWO LAYERS, because one is not enough:
//   1. HERE, at host entry: push the requested state onto the device, so it
//      lands even in hosts that never touch that setter at all.
//   2. THE LATCH (IRenderDevice::setCVarOverrides): hosts overwrite these params
//      themselves AFTER entry — host_showroom disables SSAO, host_club pins
//      exposure, host_echotropolis re-pushes setPostFX EVERY FRAME. Entry-apply
//      alone would be silently undone by all of them. The latch re-stamps the
//      overridden FIELDS inside every subsequent setter call, so the command
//      line wins for the whole run no matter who writes last.
//
// STRICTLY OPT-IN. A setter fires, and a latch field is armed, ONLY when that
// cvar was actually passed on the command line. A run with no `--set` returns
// on the first line and the latch stays inactive — byte-for-byte the behaviour
// it always had. That is the property that makes this safe to hoist.
//
// EVERY APPLIED CVAR IS LOGGED, so a run's own output proves what it tested;
// anything on the --set list that is NOT here (or claimed by applyOutdoorCsm)
// gets a LOUD end-of-run report from reportUnappliedWorldHostCVars().
// ===========================================================================
inline void applyHostRenderCVars(const HostContext& hc, x3::rhi::IRenderDevice& device,
                                 const std::string& who) {
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

    // Claim + announce one cvar. Returns true when it was on the command line.
    auto take = [&](const char* n) {
        if (!has(n)) return false;
        claimHostCVar(n);
        x3::logInfo("[cvar] " + who + ": APPLIED  --set " + n + " " + s(n, ""));
        return true;
    };

    // ---- Build the run-long override latch (field-level, opt-in) -----------
    x3::rhi::IRenderDevice::RenderCVarOverrides ov{};
    auto ovI = [&](const char* n, auto& slot) { if (take(n)) { slot.set(i(n, "0")); ov.active = true; } };
    auto ovF = [&](const char* n, auto& slot) { if (take(n)) { slot.set(f(n, "0")); ov.active = true; } };
    auto ovB = [&](const char* n, auto& slot) { if (take(n)) { slot.set(i(n, "0") != 0); ov.active = true; } };

    ovI("r_debugview",     ov.debugView);
    ovF("r_exposure",      ov.exposure);

    ovB("r_ssao",          ov.ssaoEnabled);
    ovF("r_ssao_radius",   ov.ssaoRadius);
    ovF("r_ssao_bias",     ov.ssaoBias);
    ovF("r_ssao_intensity",ov.ssaoIntensity);
    ovF("r_ssao_power",    ov.ssaoPower);
    ovF("r_ssao_strength", ov.ssaoStrength);

    ovB("r_ssgi",          ov.giEnabled);
    ovF("r_ssgi_intensity",ov.giIntensity);
    ovF("r_ssgi_strength", ov.giStrength);

    ovB("r_rtao",          ov.rtaoEnabled);
    ovF("r_rtao_radius",   ov.rtaoRadius);
    ovI("r_rtao_rays",     ov.rtaoRays);
    ovF("r_rtao_strength", ov.rtaoStrength);

    ovB("r_ssr",           ov.reflSsr);
    ovB("r_rtreflections", ov.reflRt);
    ovB("r_reflquality",   ov.reflFullRes);
    ovF("r_reflintensity", ov.reflIntensity);
    ovI("r_refldenoise",   ov.reflDenoiseIters);
    ovF("r_refldn_depth",  ov.reflDnDepth);
    ovF("r_refldn_normal", ov.reflDnNormal);
    ovF("r_refldn_disc",   ov.reflDnDisc);

    ovI("r_tonemap",         ov.tonemapMode);
    ovB("r_bloom",           ov.bloom);
    ovF("r_bloomintensity",  ov.bloomIntensity);
    ovF("r_bloomthreshold",  ov.bloomThreshold);
    ovB("r_autoexposure",    ov.autoExposure);
    ovF("r_aespeed",         ov.aeSpeed);
    ovF("r_aemin",           ov.aeMin);
    ovF("r_aemax",           ov.aeMax);
    ovF("r_aekey",           ov.aeKey);
    ovB("r_taa",             ov.taa);
    ovF("r_taasharpen",      ov.taaSharpen);
    ovB("r_velocity",        ov.velocity);
    ovB("r_filmic",          ov.filmicAllowed);

    ovI("r_rtshadows",     ov.rtsTier);
    ovF("r_rtsun_size",    ov.rtsSunSize);
    ovI("r_rtpoint_max",   ov.rtsPointMax);
    ovF("r_rtpoint_size",  ov.rtsPointRadius);

    ovB("r_ddgi",           ov.ddgiEnabled);
    ovI("r_ddgi_debug",     ov.ddgiDebug);
    ovI("r_ddgi_rays",      ov.ddgiRays);
    ovF("r_ddgi_intensity", ov.ddgiIntensity);
    ovI("r_ddgi_nx",        ov.ddgiNx);
    ovI("r_ddgi_ny",        ov.ddgiNy);
    ovI("r_ddgi_nz",        ov.ddgiNz);
    ovF("r_ddgi_hyst",      ov.ddgiHyst);

    ovF("r_metalambient",   ov.metalAmbient);
    ovB("r_clusterlights",  ov.clusterLights);

    if (!ov.active) return;   // nothing this layer owns was passed: touch nothing

    // LAYER 2 — arm the latch for the rest of the run (survives every later
    // host write, including per-frame ones). Announce it, so the run says what
    // it pinned.
    device.setCVarOverrides(ov);
    x3::logInfo("[cvar] " + who +
                ": override latch ARMED — the values above are pinned for the whole "
                "run and cannot be undone by a later host write");

    // LAYER 1 — push the requested state now, so it lands even where the host
    // never calls the setter at all. Group-gated: a group is only touched when
    // at least one of its cvars was passed (the opt-in property).
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
    if (has("r_ssgi") || has("r_ssgi_intensity") || has("r_ssgi_strength")) {
        x3::rhi::IRenderDevice::GiParams p{};
        p.enabled   = i("r_ssgi", "1") != 0;
        p.intensity = f("r_ssgi_intensity", "1.0");
        p.strength  = f("r_ssgi_strength", "1.0");
        device.setGiParams(p);
    }
    if (has("r_rtao") || has("r_rtao_radius") || has("r_rtao_rays") || has("r_rtao_strength")) {
        x3::rhi::IRenderDevice::RtaoParams p{};
        p.enabled  = i("r_rtao", "0") != 0;
        p.radius   = f("r_rtao_radius", "0.5");
        p.rays     = i("r_rtao_rays", "8");
        p.strength = f("r_rtao_strength", "0.85");
        device.setRtaoParams(p);
    }
    if (has("r_ssr") || has("r_rtreflections") || has("r_refldenoise") ||
        has("r_reflquality") || has("r_reflintensity") || has("r_refldn_depth") ||
        has("r_refldn_normal") || has("r_refldn_disc")) {
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
        has("r_autoexposure") || has("r_filmic") || has("r_bloomintensity") ||
        has("r_bloomthreshold") || has("r_aespeed") || has("r_aemin") ||
        has("r_aemax") || has("r_aekey") || has("r_velocity")) {
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
        px.velocity       = i("r_velocity", "0") != 0;
        px.filmicAllowed  = i("r_filmic", "1") != 0;
        device.setPostFX(px);
    }
    if (has("r_rtshadows") || has("r_rtsun_size") || has("r_rtpoint_max") ||
        has("r_rtpoint_size")) {
        x3::rhi::IRenderDevice::RtShadowParams rs{};
        rs.tier        = i("r_rtshadows", "2");
        rs.sunSizeDeg  = f("r_rtsun_size", "0.5");
        rs.pointMax    = i("r_rtpoint_max", "4");
        rs.pointRadius = f("r_rtpoint_size", "0.10");
        device.setRtShadowParams(rs);
    }
    if (has("r_ddgi") || has("r_ddgi_debug") || has("r_ddgi_rays") ||
        has("r_ddgi_intensity") || has("r_ddgi_nx") || has("r_ddgi_ny") ||
        has("r_ddgi_nz") || has("r_ddgi_hyst")) {
        x3::rhi::IRenderDevice::DdgiParams dg{};
        dg.enabled      = i("r_ddgi", "0") != 0;
        dg.debug        = i("r_ddgi_debug", "0");
        dg.raysPerProbe = i("r_ddgi_rays", "96");
        dg.intensity    = f("r_ddgi_intensity", "1.0");
        dg.countX       = i("r_ddgi_nx", "24");
        dg.countY       = i("r_ddgi_ny", "8");
        dg.countZ       = i("r_ddgi_nz", "24");
        dg.hysteresis   = f("r_ddgi_hyst", "0.97");
        device.setDdgiParams(dg);
    }
    if (has("r_metalambient"))  device.setMetalAmbient(f("r_metalambient", "1"));
    if (has("r_clusterlights")) device.setClusterLights(i("r_clusterlights", "0") != 0);
}

// ===========================================================================
// THE LOUD END. Called by dispatchWorldHost() AFTER the host returns (so a
// late claimer like applyOutdoorCsm has had its chance): every `--set` on the
// command line that nothing applied is named, at ERROR level, with what that
// means. Silence is the thing that cost a lane its conclusions; a run that
// ignored a flag must SAY SO in its own output.
// ===========================================================================
// ===========================================================================
// LIVE CONSOLE -> DEVICE APPLY (D-CONSOLE fold). applyHostRenderCVars ABOVE is
// BOOT-TIME ONLY — it reads hc.cliCVars (the `--set` list parsed once off the
// command line). It has no idea a console exists: typing `r_exposure 0.5` at
// an interactive --world host's console (HostShell — app/world_hosts/
// host_shell.h/.cpp) after boot never reached the device, because nothing
// ever read the LIVE cvar value again. That is the second half of the
// owner's complaint ("do we have the console with EVERY COMMAND ACTIVE") —
// the render cvars now exist on every host's console (app/engine_console.h),
// but typing a new value did nothing until this.
//
// This is the per-frame sibling: read the SAME watched cvars straight off a
// live x3::con::IConsole and re-push them to the device — the world-host
// equivalent of app_run.cpp's applyRtaoCVars (its own per-frame cvar sync
// hub; setCsmParams there is pushed from live console cvars every frame the
// exact same way, so this does not introduce a new pattern, it generalizes
// the existing one). Driven automatically from HostShell::draw() — see
// host_shell.cpp — so every host that attaches a HostShell gets it "for free".
//
// DELIBERATELY NOT HERE: r_wetness*. Unlike every field below (which the
// --world hosts here only ever set ONCE at boot — see host_tunnel.cpp's
// applyHostRenderCVars() call site), wetness is a PER-FRAME AUTHORITATIVE
// value a host's own weather sim drives (host_tunnel.cpp: x3::game::
// WetnessModel -> device.setWetness() every frame, from real rain
// accumulation, not from a cvar). Pushing r_wetness here every ~15 frames
// would fight that — periodically stomping live rain-soak back to the cvar's
// resting value — which is the exact "must not fight" failure this fold's
// noclip/chase-cam care was written against, just for a different system.
//
// COST: IConsole (engine/core/Console.cpp) exposes no dirty/change counter,
// so "cheap" here means a string hash of the watched values, gated to run
// every ~15th frame, and the device push only fires when that hash actually
// changed. An untouched console costs one hash build every 15 frames — a
// couple dozen string reads and an XOR-fold, not fifteen no-op device calls.
// ===========================================================================
inline size_t hashLiveHostCVars(const x3::con::IConsole& console) {
    static const char* const kWatched[] = {
        "r_exposure", "r_debugview", "r_metalambient", "r_clusterlights",
        "r_tonemap", "r_bloom", "r_bloomintensity", "r_bloomthreshold",
        "r_autoexposure", "r_aespeed", "r_aemin", "r_aemax", "r_aekey",
        "r_taa", "r_taasharpen", "r_velocity", "r_filmic",
        "r_ssao", "r_ssao_radius", "r_ssao_bias", "r_ssao_intensity", "r_ssao_power", "r_ssao_strength",
        "r_ssgi", "r_ssgi_intensity", "r_ssgi_strength",
        "r_rtao", "r_rtao_radius", "r_rtao_rays", "r_rtao_strength",
        "r_ssr", "r_rtreflections", "r_reflquality", "r_reflintensity",
        "r_refldenoise", "r_refldn_depth", "r_refldn_normal", "r_refldn_disc",
        "r_rtshadows", "r_rtsun_size", "r_rtpoint_max", "r_rtpoint_size",
        "r_ddgi", "r_ddgi_debug", "r_ddgi_rays", "r_ddgi_intensity",
        "r_ddgi_nx", "r_ddgi_ny", "r_ddgi_nz", "r_ddgi_hyst",
        "r_csm", "r_csm_lambda", "r_csm_dist", "r_csm_blend", "r_shadowforward", "r_csm_debug",
    };
    std::hash<std::string> hasher;
    size_t acc = 0;
    for (const char* n : kWatched)
        acc ^= hasher(console.getString(n)) + 0x9e3779b97f4a7c15ULL + (acc << 6) + (acc >> 2);
    return acc;
}

// Unconditionally pushes the watched set to the device (no hashing/gating —
// callers wanting the cheap per-frame version want applyLiveHostRenderCVars
// below). Exposed directly too, so a host can force one push right after
// HostShell::attach(), before the first hashed cycle, so the registered
// DEFAULTS take visible effect even if the player never types anything.
inline void pushLiveHostCVarsToDevice(const x3::con::IConsole& console, x3::rhi::IRenderDevice& device) {
    auto f = [&](const char* n) { return console.getFloat(n); };
    auto i = [&](const char* n) { return console.getInt(n); };
    auto b = [&](const char* n) { return console.getInt(n) != 0; };

    device.setDebugView(i("r_debugview"));
    device.setExposure(f("r_exposure"));
    device.setMetalAmbient(f("r_metalambient"));
    device.setClusterLights(b("r_clusterlights"));

    x3::rhi::IRenderDevice::SsaoParams sp{};
    sp.enabled = b("r_ssao"); sp.radius = f("r_ssao_radius"); sp.bias = f("r_ssao_bias");
    sp.intensity = f("r_ssao_intensity"); sp.power = f("r_ssao_power"); sp.strength = f("r_ssao_strength");
    device.setSsaoParams(sp);

    x3::rhi::IRenderDevice::GiParams gp{};
    gp.enabled = b("r_ssgi"); gp.intensity = f("r_ssgi_intensity"); gp.strength = f("r_ssgi_strength");
    device.setGiParams(gp);

    x3::rhi::IRenderDevice::RtaoParams rp{};
    rp.enabled = b("r_rtao"); rp.radius = f("r_rtao_radius"); rp.rays = i("r_rtao_rays"); rp.strength = f("r_rtao_strength");
    device.setRtaoParams(rp);

    x3::rhi::IRenderDevice::ReflectionParams rf{};
    rf.ssr = b("r_ssr"); rf.rtFallback = b("r_rtreflections"); rf.fullRes = b("r_reflquality");
    rf.intensity = f("r_reflintensity"); rf.denoiseIters = i("r_refldenoise");
    rf.denoiseDepthSigma = f("r_refldn_depth"); rf.denoiseNormalPow = f("r_refldn_normal");
    rf.denoiseDiscScale = f("r_refldn_disc");
    device.setReflectionParams(rf);

    x3::rhi::IRenderDevice::PostFXParams px{};
    px.tonemapMode = i("r_tonemap"); px.bloomEnabled = b("r_bloom");
    px.bloomIntensity = f("r_bloomintensity"); px.bloomThreshold = f("r_bloomthreshold");
    px.autoExposure = b("r_autoexposure"); px.aeSpeed = f("r_aespeed"); px.aeMin = f("r_aemin");
    px.aeMax = f("r_aemax"); px.aeKey = f("r_aekey"); px.taa = b("r_taa"); px.taaSharpen = f("r_taasharpen");
    px.velocity = b("r_velocity"); px.filmicAllowed = b("r_filmic");
    device.setPostFX(px);

    x3::rhi::IRenderDevice::RtShadowParams rs{};
    rs.tier = i("r_rtshadows"); rs.sunSizeDeg = f("r_rtsun_size");
    rs.pointMax = i("r_rtpoint_max"); rs.pointRadius = f("r_rtpoint_size");
    device.setRtShadowParams(rs);

    x3::rhi::IRenderDevice::DdgiParams dg{};
    dg.enabled = b("r_ddgi"); dg.debug = i("r_ddgi_debug"); dg.raysPerProbe = i("r_ddgi_rays");
    dg.intensity = f("r_ddgi_intensity"); dg.countX = i("r_ddgi_nx"); dg.countY = i("r_ddgi_ny");
    dg.countZ = i("r_ddgi_nz"); dg.hysteresis = f("r_ddgi_hyst");
    device.setDdgiParams(dg);

    x3::rhi::IRenderDevice::CsmParams cs{};
    cs.enabled = b("r_csm"); cs.lambda = f("r_csm_lambda"); cs.distance = f("r_csm_dist");
    cs.blend = f("r_csm_blend"); cs.forwardBias = f("r_shadowforward"); cs.debug = b("r_csm_debug");
    device.setCsmParams(cs);
}

// Call EVERY FRAME from an interactive host with a live console (HostShell
// drives this — app/world_hosts/host_shell.cpp — so a host wires it once and
// gets it "for free"). `frameCounter` is any monotonically increasing
// per-frame counter; every 15th frame the watched cvars are hashed and, ONLY
// if the hash changed since the last push, re-applied to the device.
// `lastHash` is caller-owned state (HostShell keeps one per instance).
inline void applyLiveHostRenderCVars(const x3::con::IConsole& console, x3::rhi::IRenderDevice& device,
                                     unsigned frameCounter, size_t& lastHash) {
    if (frameCounter != 0 && (frameCounter % 15u) != 0u) return;
    const size_t h = hashLiveHostCVars(console);
    if (frameCounter != 0 && h == lastHash) return;
    lastHash = h;
    pushLiveHostCVarsToDevice(console, device);
}

inline void reportUnappliedHostCVars(const HostContext& hc, const std::string& who) {
    if (hc.cliCVars.empty()) return;
    std::vector<std::string> unapplied;
    for (const auto& kv : hc.cliCVars)
        if (!hostCVarClaimed(kv.first)) unapplied.push_back(kv.first + " " + kv.second);
    if (unapplied.empty()) {
        x3::logInfo("[cvar] " + who + ": all " + std::to_string(hc.cliCVars.size()) +
                    " --set override(s) were applied");
        return;
    }
    x3::logError("[cvar] ================================================================");
    for (const auto& u : unapplied)
        x3::logError("[cvar] !!! --set " + u + "  was NOT APPLIED on " + who);
    x3::logError("[cvar] !!! THIS RUN DID NOT TEST THOSE. Any A/B conclusion resting on");
    x3::logError("[cvar] !!! them is VOID. Either add the cvar to applyHostRenderCVars");
    x3::logError("[cvar] !!! (app/world_hosts/world_host_common.h) or run the A/B on the");
    x3::logError("[cvar] !!! default host, which syncs the full cvar set every frame.");
    x3::logError("[cvar] ================================================================");
}

} // namespace x3::apphost
