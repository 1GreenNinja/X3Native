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
#include <cstdio>

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
        const std::vector<x3::rhi::PointLight>* interiorLights = nullptr,
        bool interiorProbe = true) {
    x3::rhi::IRenderDevice::SkyParams sp{};
    sp.enabled = true;
    if (day) {
        // DAY / DUSK — the Unity-reference exterior grade: a LOW winter sun raking
        // across the tower's camera-facing faces (the old rig put the sun almost
        // straight overhead, sunDir.y = 0.94, so every vertical panel got a grazing
        // N.L ~ 0 and the whole tower read as a flat dark slab no matter how bright
        // the key was). Value, not lumens: the sun DIRECTION is the fix; intensity
        // came DOWN, and the 0.48 ambient wash came down with it.
        // Elevation is a HARD constraint here, not taste: the pack's terrain is a BOWL
        // ringed by 400 m peaks. Below ~35 deg the ridge on the sun side really does
        // put the whole valley in shadow (verified by tracing it with RT shadows, not
        // guessed) — the snow goes navy and no amount of key fixes it. ~48 deg keeps
        // the valley floor lit while still raking the tower's camera-facing panels.
        sp.sunDir[0]   = -0.30f; sp.sunDir[1] = 0.75f; sp.sunDir[2] = 0.59f;  // TOWARD the sun: behind-left of the hero cam
        sp.sunColor[0] = 1.00f;  sp.sunColor[1] = 0.95f; sp.sunColor[2] = 0.88f;  // late-afternoon warm-white
        sp.sunIntensity = 2.6f;   // SKY DISK + glow only (this is NOT the key — see sunLight)
        // THE KEY. mesh.frag's directional radiance is SkyParams::sunLight, NOT
        // sunIntensity (which only scales the sky disk). The old DAY preset set
        // sunIntensity = 3.4 and left sunLight at its 1.0 default — so the "bright
        // winter day" was lit by exactly the same 1.0 sun as every interior in the
        // game, and snow (albedo 0.73, PBR 1/pi) could only ever resolve to ~0.34
        // sRGB: a dark blue-grey. THAT is why day looked like an underexposed night.
        // 4.2 is what puts sunlit snow at ~0.75 sRGB with no ambient wash and no
        // exposure hack.
        sp.sunLight = 4.2f;
        sp.haze = 0.55f; sp.exposure = 0.95f;
        sp.zenith[0]  = 0.16f; sp.zenith[1]  = 0.27f; sp.zenith[2]  = 0.52f;  // cool dusk blue
        sp.horizon[0] = 0.74f; sp.horizon[1] = 0.79f; sp.horizon[2] = 0.88f;  // pale overcast haze
        // Tuning hooks (art pass): X3_SHOWROOM_SUN="x,y,z" re-aims the key,
        // X3_SHOWROOM_SUNLIGHT scales its radiance. No effect when unset.
        if (const char* sd = std::getenv("X3_SHOWROOM_SUN")) {
            float x = 0, y = 0, z = 0;
            if (std::sscanf(sd, "%f,%f,%f", &x, &y, &z) == 3) { sp.sunDir[0] = x; sp.sunDir[1] = y; sp.sunDir[2] = z; }
        }
        if (const char* sl = std::getenv("X3_SHOWROOM_SUNLIGHT")) sp.sunLight = (float)std::atof(sl);
        device->setSkyParams(sp);
        device->setAmbient(0.26f, 0.29f, 0.36f);   // HONEST cool sky+snow bounce (was a 0.48 wash that killed contrast)
        device->setBloom(0.10f);                    // low: let white trim bloom only slightly
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
    // Reflection probe. INTERIOR (default): bake the IBL env from the showroom geometry
    // around the camera, so the glossy/metallic panels reflect the dim interior rather
    // than the bright sky (which blows them out to white).
    // EXTERIOR (interiorProbe = false): the tower IS in the open — its metal wants the
    // SKY as its environment. Baking the interior probe out there hands a ~0.6-metallic
    // panel atlas a dark box to mirror, and a metal with nothing to reflect is flat by
    // construction. This is what was killing the reference's SHEEN on the hero shot.
    device->setIblProbe(interiorProbe);
}

// Read the DAY-vs-NIGHT selection for the SHOWROOM. Default = NIGHT (unchanged).
// DAY is opted into via the env X3_SHOWROOM_DAY=1 OR the in-game 'T' toggle.
inline bool showroomDayDefault() {
    const char* e = std::getenv("X3_SHOWROOM_DAY");
    return e != nullptr && e[0] != '0' && e[0] != '\0';
}

}} // namespace x3::apphost
