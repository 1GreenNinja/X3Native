#pragma once
// ============================================================================
// Showroom DAY<->NIGHT lighting helpers (#28 deep split).
// Moved VERBATIM out of main()'s anon namespace so BOTH main.cpp's screenshot
// handlers AND the extracted --world showroom host (app/world_hosts/host_
// showroom.cpp) share one definition. Behaviour is byte-identical; main() keeps
// its call sites unqualified via using-declarations.
// ============================================================================

#include "engine/rhi/IRenderDevice.h"
#include <vector>
#include <cstdlib>
#include <cstdint>

namespace x3 { namespace apphost {

// ---- SHOWROOM DAY<->NIGHT lighting STATES (one helper, two looks) -----------
// Drives sky/sun/ambient/bloom/interior-point-lights for --world showroom and
// its headless proofs through ONE switch. NIGHT reproduces the exact values the
// showroom has always used; the planet draw + setSkyTime wheeling are gated to
// NIGHT by the CALLER. `interiorLights` (nullable) holds the FULL-intensity
// NIGHT point lights (color already pre-multiplied by intensity); DAY pushes a
// x0.3-scaled copy, NIGHT pushes them unchanged.
inline void applyShowroomTimeOfDay(
        x3::rhi::IRenderDevice* device, bool day,
        const std::vector<x3::rhi::PointLight>* interiorLights = nullptr) {
    x3::rhi::IRenderDevice::SkyParams sp{};
    sp.enabled = true;
    if (day) {
        // DAY — Unity-match: high winter sun, bright pale winter-blue sky.
        sp.sunDir[0]   = -0.0595f; sp.sunDir[1] = 0.9355f; sp.sunDir[2] = -0.3483f; // TOWARD the sun
        sp.sunColor[0] = 1.00f;  sp.sunColor[1] = 0.98f; sp.sunColor[2] = 0.95f;    // warm-neutral
        sp.sunIntensity = 3.4f;   // bright key (winter midday)
        sp.haze = 0.5f; sp.exposure = 0.92f;   // just under 1.0 so the bright floors don't blow out
        sp.zenith[0]  = 0.20f; sp.zenith[1]  = 0.34f; sp.zenith[2]  = 0.62f;        // pale winter-blue
        sp.horizon[0] = 0.72f; sp.horizon[1] = 0.80f; sp.horizon[2] = 0.92f;        // warm-grey/white haze
        device->setSkyParams(sp);
        device->setAmbient(0.48f, 0.52f, 0.62f);   // BRIGHT cool snow-bounce high-key fill (pulled from 0.55 so floors don't blow)
        device->setBloom(0.12f);                    // low: let white panels bloom only slightly
    } else {
        // NIGHT — UNCHANGED from the original showroom recipe.
        sp.sunDir[0] = 0.6f; sp.sunDir[1] = 0.42f; sp.sunDir[2] = -0.2f;   // low raking MOON
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.96f; sp.sunColor[2] = 0.90f;
        sp.sunIntensity = 0.25f;   // cool moonlight (still casts shadows)
        sp.haze = 0.15f; sp.exposure = 0.62f;
        sp.zenith[0]  = 0.012f; sp.zenith[1]  = 0.012f; sp.zenith[2]  = 0.028f;   // near-black zenith
        sp.horizon[0] = 0.10f;  sp.horizon[1] = 0.13f;  sp.horizon[2] = 0.20f;    // faint cool horizon
        device->setSkyParams(sp);
        device->setAmbient(0.09f, 0.10f, 0.16f);   // cool dim moonlight fill
        device->setBloom(0.22f);                    // HERO glow on the HDR-emissive windows/fixtures
    }
    // Interior point lights: DAY dims them (x0.3) so snow-bounce dominates; NIGHT
    // uses the full-intensity set.  Color channels are pre-multiplied by intensity.
    if (interiorLights) {
        if (day) {
            std::vector<x3::rhi::PointLight> dim = *interiorLights;
            for (x3::rhi::PointLight& pl : dim) {
                pl.color[0] *= 0.3f; pl.color[1] *= 0.3f; pl.color[2] *= 0.3f;
            }
            device->setPointLights(dim.data(), (uint32_t)dim.size());
        } else {
            device->setPointLights(interiorLights->data(), (uint32_t)interiorLights->size());
        }
    }
    // Interior reflection probe: bake the IBL env from the showroom geometry (around the
    // camera) instead of the open sky, so the glossy/metallic Unity panels reflect the
    // dim interior rather than the bright sky (which blows them out to white).
    device->setIblProbe(true);
}

// Read the DAY-vs-NIGHT selection for the SHOWROOM. Default = NIGHT (unchanged).
// DAY is opted into via the env X3_SHOWROOM_DAY=1 OR the in-game 'T' toggle.
inline bool showroomDayDefault() {
    const char* e = std::getenv("X3_SHOWROOM_DAY");
    return e != nullptr && e[0] != '0' && e[0] != '\0';
}

}} // namespace x3::apphost
