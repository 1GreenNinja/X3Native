// --world space host — the Act-3 6DOF space-pilot showcase. RE-HOMED from the
// pre-split main() `if (worldMode == "space") { ... }` inline block (feat/
// cockpit-vattalus) into the #28 deep-split world-host registry. The body is the
// VERBATIM space host loop; the ONLY edits are reaching shared state via the
// HostContext (`hc.device` is a raw IRenderDevice*, so the pre-split
// `device.get()`/`device->` become `device`/`device->`), mirroring host_drive.cpp.
#include "world_host_common.h"
#include "host_shell.h"                 // console (~) + FPS (F3); this host keeps its OWN pause menu
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "../scene.h"
#include "../mesh_prims.h"
#include "../fx.h"
#include "../asset_root.h"
#include "../space_pilot.h"
#include "../space/wormhole.h"           // feat/wormholes: the space wormhole entities
#include "../space/space_layer.h"        // S0 spine — the transit state machine
#include "../space/wormhole_transit.h"   // S3 runner + the crystal-matrix ride
#include "../ship_comms.h"               // the AEGIS wormhole-stability advisory bus
#include "../star_systems.h"             // x3::starsys — the star a transit lands you under
#include "../settings_io.h"   // readFlightMode (persisted Settings-menu / console pick)
#include "../audio_root.h"    // resolveAudio(...) — flight engine hum / boost / mode blip WAVs
#include "engine/audio/IAudioSystem.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>   // std::getenv / std::atoi (X3_SUN_DIVE_TEST variant parse)
#include <cstring>   // std::strlen / std::strstr (the transit HUD overflow field)
#include <filesystem>
#include <vector>

namespace x3 { namespace apphost {

// ---------------------------------------------------------------------------
// Local render-only helpers for the space HUD + sense-of-speed FX layer. These
// are PURE PRESENTATION (never touch the pilot sim state that --test-space
// hashes), so the determinism gate is unaffected. Kept file-local (anon ns).
// ---------------------------------------------------------------------------
namespace {
// Column-major 4x4 from three (already-scaled-direction) basis columns + origin.
inline void composeBasis(float m[16],
                         const x3::phys::Vec3& cx, const x3::phys::Vec3& cy,
                         const x3::phys::Vec3& cz, float sx, float sy, float sz,
                         const x3::phys::Vec3& t) {
    m[0]=cx.x*sx; m[1]=cx.y*sx; m[2]=cx.z*sx; m[3]=0;
    m[4]=cy.x*sy; m[5]=cy.y*sy; m[6]=cy.z*sy; m[7]=0;
    m[8]=cz.x*sz; m[9]=cz.y*sz; m[10]=cz.z*sz; m[11]=0;
    m[12]=t.x;    m[13]=t.y;    m[14]=t.z;     m[15]=1;
}
inline x3::phys::Vec3 vcross(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
inline x3::phys::Vec3 vnorm(const x3::phys::Vec3& a) {
    float l = std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z);
    if (l < 1e-6f) return { 0, 0, 1 };
    return { a.x/l, a.y/l, a.z/l };
}
// Cheap integer hash -> [0,1) for deterministic dust-field seeding / twinkle.
inline uint32_t hashU(uint32_t x) { x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16; return x; }
inline float    hashF(uint32_t x) { return (hashU(x) & 0xFFFFFFu) / 16777216.0f; }
inline float smoothstepLocal(float e0, float e1, float x) {
    float d = e1 - e0; if (std::fabs(d) < 1e-6f) d = (d < 0.0f) ? -1e-6f : 1e-6f;
    float t = (x - e0) / d; t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// ---- SUN SURFACE bake helpers (SAME 2D tileable value-noise pattern as
//      ship_windows.cpp's bakePortalRGBA — copied here rather than shared
//      across TUs since it's a handful of lines). `cell` texel-cells tile
//      seamlessly around the sphere's U (longitude) seam. ----------------
inline float sunHash2(uint32_t x, uint32_t y, uint32_t cell, uint32_t salt) {
    uint32_t h = (x % cell) * 374761393u + (y % cell) * 668265263u + salt * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}
inline float sunValueNoise(float u, float v, uint32_t cell, uint32_t salt) {
    const float fx = u * (float)cell, fy = v * (float)cell;
    const uint32_t x0 = (uint32_t)std::floor(fx), y0 = (uint32_t)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    auto sm = [](float t){ return t * t * (3.0f - 2.0f * t); };
    const float sx = sm(tx), sy = sm(ty);
    const float a = sunHash2(x0,     y0,     cell, salt);
    const float b = sunHash2(x0 + 1, y0,     cell, salt);
    const float c = sunHash2(x0,     y0 + 1, cell, salt);
    const float d = sunHash2(x0 + 1, y0 + 1, cell, salt);
    const float ab = a + (b - a) * sx;
    const float cd = c + (d - c) * sx;
    return ab + (cd - ab) * sy;
}
// Bake an n x n RGBA sun-surface texture: 5-octave granulation (warm white-gold
// -> deep-orange cells), a sparse bright faculae/vein layer, and 3-7 hash-placed
// dark sunspots (soft penumbra falloff, wrapping the U seam). Used as BOTH the
// baseColor and the emissiveTex for the core sphere so the "living surface"
// detail actually modulates the bloom, not just unlit albedo.
inline std::vector<uint8_t> bakeSunRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4, 0);
    struct Spot { float u, v, r; };
    const int nSpots = 3 + (int)(sunHash2(1, 1, n, 777u) * 5.0f);   // 3-7
    Spot spots[7];
    for (int i = 0; i < nSpots; ++i) {
        spots[i].u = hashF((uint32_t)(i * 7 + 1000));
        spots[i].v = 0.18f + 0.64f * hashF((uint32_t)(i * 7 + 2000));   // keep off the poles
        // BIGGER, higher-contrast sunspots (owner: "visible sunspots, bigger") — ~1.7x
        // the old radius so they survive the bloom as discrete dark blotches.
        spots[i].r = 0.050f + 0.075f * hashF((uint32_t)(i * 7 + 3000));
    }
    for (uint32_t y = 0; y < n; ++y) {
        const float v = (y + 0.5f) / (float)n;
        for (uint32_t x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / (float)n;
            // Granulation: 5 octaves of tileable value noise.
            float gran = 0.0f, amp = 0.5f; uint32_t cell = 6;
            for (int o = 0; o < 5; ++o) {
                gran += amp * sunValueNoise(u, v, cell, 5000u + (uint32_t)o * 37u);
                amp *= 0.55f; cell *= 2u;
            }
            // Contrast remap: raw multi-octave value noise averages ~0.5 with a
            // narrow spread — stretch it (STRONGER amplitude now, owner: "real
            // contrast, deep orange cells + brighter cell centres") so the
            // granulation reads as boiling plasma cells through the bloom, not a
            // flat wash.
            // v3: HIGHER contrast + a gamma bias so the dark inter-granular lanes
            // occupy MORE of the surface (real granulation is a network of bright
            // cells separated by thin dark lanes, not a 50/50 wash). Squaring after
            // the stretch deepens the valleys without dimming the cell centres.
            gran = std::clamp((gran - 0.40f) * 2.9f, 0.0f, 1.0f);
            gran = gran * gran * (0.55f + 0.45f * gran);   // stronger gamma: wide dark lanes, sparse bright peaks
            // Faculae/plasma veins: a higher-frequency layer, bright above a threshold.
            const float vein = sunValueNoise(u, v, 40, 6600u);
            const float veinBright = std::max(0.0f, vein - 0.74f) / 0.26f;
            // v3 palette: DEEP orange-red inter-granular lanes as the DOMINANT tone ->
            // WHITE-GOLD only at the sparse bright cell cores (owner: "deep orange
            // cells / white-gold centres"). The disc must read as a rich ORANGE star,
            // so the base (gran=0) is a saturated deep orange, not a pale yellow; blue
            // stays near-zero until the very brightest cores so hot spots go white-gold
            // while everything else holds orange. Contrast now comes from the wide dark
            // lane network, not from a bright wash.
            // v5 (owner: "sun still doesnt look so sunny" — a DARK granulated band
            // ringed the white core). Raise the FLOOR so the whole disc reads bright
            // YELLOW-GOLD; granulation now modulates brightness as a subtle texture
            // rather than dropping the inter-granular lanes into a dark orange ring.
            // Real sun photos have no dark band between the saturated centre and limb.
            float r = 0.96f + 0.04f * gran;
            float g = 0.62f + 0.26f * gran;
            float b = 0.22f + 0.26f * gran * gran;   // luminous gold everywhere -> white-gold at the cell cores
            r += veinBright * 0.26f; g += veinBright * 0.22f; b += veinBright * 0.10f;
            // Sunspots: NEAR-BLACK core + soft penumbra, U wraps at the longitude
            // seam. Pushed much darker than a "natural" sunspot so the blotch still
            // reads as a discrete dark feature after the core's bloom bleeds into it.
            for (int i = 0; i < nSpots; ++i) {
                float du = u - spots[i].u; du -= std::round(du);
                const float dv = v - spots[i].v;
                const float d = std::sqrt(du * du + dv * dv);
                const float k = 1.0f - smoothstepLocal(spots[i].r * 0.5f, spots[i].r, d);
                r *= (1.0f - 0.94f * k); g *= (1.0f - 0.96f * k); b *= (1.0f - 0.98f * k);
            }
            auto u8 = [](float c){ c = std::clamp(c, 0.0f, 1.0f); return (uint8_t)std::lround(c * 255.0f); };
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            p[0] = u8(r); p[1] = u8(g); p[2] = u8(b); p[3] = 255;
        }
    }
    return px;
}
} // namespace

int hostSpace(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    // ==== VERBATIM host body (re-homed; device is now a raw pointer) ====
    if (worldMode == "space") {
        x3::logInfo("--world space: building the Act-3 space-pilot showcase");
        std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
        if (!sphys->init()) {
            x3::logError("--world space: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }

        // W3-3 (AD-2 red-line): deep-space STARFIELD. The old host disabled the
        // sky entirely, leaving the flat navy clear color behind the fleet. The
        // analytic sky's procedural starfield is gated to DARK skies and, at
        // haze == 0, paints stars on the FULL sphere (spaceW: a space scene
        // looking "down" sees stars, not a ground plane) — so deep space is the
        // sky ENABLED at near-black with zero haze, exactly like the nightsky
        // host but with no horizon band at all.
        // TWIN-SUN FIX (owner: "a bright white glow-orb floating beside the real
        // sun"): the analytic sky paints its sun DISC along skyP.sunDir at INFINITY,
        // while the real emissive body sits 20 km away — so from off-axis positions
        // they visibly SEPARATE (parallax). We keep this SkyParams PERSISTENT and, in
        // the windowed loop, re-aim skyP.sunDir every frame down the ship→body ray so
        // the painted disc always sits directly BEHIND the real (larger, opaque,
        // depth-nearer) body and is fully occluded by it — the body becomes the ONLY
        // visible sun, while the directional LIGHT (sunIntensity) still shades hulls
        // from the physically-correct direction. Seeded here to the spawn ray.
        x3::rhi::IRenderDevice::SkyParams skyP{};
        skyP.enabled = true;
        skyP.sunDir[0] = 0.6f; skyP.sunDir[1] = 0.5f; skyP.sunDir[2] = 0.62f;   // matches the key light corner
        skyP.sunColor[0] = 0.75f; skyP.sunColor[1] = 0.82f; skyP.sunColor[2] = 1.0f;
        // W6-2: 0.02 -> 0.55 — a cool DIRECTIONAL starlight key. The sky sun feeds
        // the PBR path (the surface tower proves it), and it's the only way hulls
        // get real shading gradients out here: point rigs vanish at capital-ship
        // scale, and a flat ambient floor reads as clay. Still far below daylight.
        skyP.sunIntensity = 0.55f;
        skyP.haze = 0.0f;                              // haze 0 == DEEP SPACE (stars on the full sphere)
        skyP.exposure = 1.0f;
        skyP.zenith[0]  = 0.003f; skyP.zenith[1]  = 0.003f; skyP.zenith[2]  = 0.008f;
        skyP.horizon[0] = 0.004f; skyP.horizon[1] = 0.005f; skyP.horizon[2] = 0.011f;
        device->setSkyParams(skyP);
        device->setSkyTime(10.0f);                     // non-zero -> starfield twinkle/rotation phase;
                                                       // the windowed loop advances this per-frame below
        // R2: enabling the sky at near-zero sun replaced whatever ambient the
        // disabled-sky path implied — the fleet went silhouette-black. Explicit
        // cool ambient so hulls read while space stays dark (nightsky's trick).
        device->setAmbient(0.11f, 0.12f, 0.16f);
        // SSAO + SSGI screen-space passes raster the whole scene to black on a
        // black/empty space background (no nearby geometry to bounce off) -- the
        // 1080 Ti / no-RT fallback path documented in the memory bank. Disable
        // both for the space showcase so the ships actually read against the
        // dark backdrop.
        { x3::rhi::IRenderDevice::SsaoParams ap{}; ap.enabled = false;
          device->setSsaoParams(ap); }
        { x3::rhi::IRenderDevice::GiParams gp{}; gp.enabled = false;
          device->setGiParams(gp); }
        // Sun = the directional sun baked into mesh.frag at +Y-ish; layer on a
        // few BRIGHT point lights NEAR the fleet so the ships read (the analytic
        // sky is OFF -> no atmospheric tint; light only comes from these point
        // lights + the hardcoded sun, attenuated by 1/r^2). The point-light
        // ranges + intensities are intentionally cranked: deep space has zero
        // bounced light, so anything subtle would render the ships as silhouettes.
        // R3: with the REAL black sky in (starfield), the old light rig left the
        // hulls as silhouettes — the navy "readability" of the old shot was just
        // the clear color. Roughly doubled key/fill/rim so the fleet reads as lit
        // metal against the stars.
        // Persistent 5-slot rig: [0..2] STATIC fleet key/fill/rim (set once); [3] a
        // PLAYER-KEY light that FOLLOWS the ship each frame (so its silhouette reads
        // while flying — the static rig doesn't move with the player); [4] a warm
        // SUN-HEAT light that ramps up as you dive toward the star. Slots 3-4 are
        // refreshed every frame via updateDynamicLights() below, which re-uploads all
        // five with setPointLights (the device caches its own copy).
        // feat/wormholes: the rig grows from 5 to 5 + kMaxWormholeLights. A
        // wormhole that does not light the world is a sticker, so the field's
        // spill is a FIRST-CLASS member of this rig, not an afterthought — and it
        // is BOUNDED (WormholeField::collectLights can never return more than
        // kMaxWormholeLights, which --test-wormholes asserts). Device cap is 64;
        // this rig is 9.
        constexpr int kStaticLights = 5;
        constexpr int kTotalLights  = kStaticLights + x3::space::kMaxWormholeLights;
        x3::rhi::PointLight plights[kTotalLights];
        int liveLightCount = kStaticLights;
        // Key light: a "sun" anchored near the fleet so attenuation is gentle.
        plights[0].pos[0] =  120.0f; plights[0].pos[1] = 120.0f; plights[0].pos[2] = 120.0f;
        plights[0].range  =  600.0f;
        plights[0].color[0] = 130.0f; plights[0].color[1] = 121.0f; plights[0].color[2] = 104.0f;
        // Fill light from -X/+Y to bring out the camera-facing side.
        plights[1].pos[0] = -80.0f; plights[1].pos[1] =  60.0f; plights[1].pos[2] =  20.0f;
        plights[1].range  = 400.0f;
        plights[1].color[0] = 40.0f; plights[1].color[1] = 46.0f; plights[1].color[2] = 60.0f;
        // Rim/back light from +X/-Y to give the ships shape.
        plights[2].pos[0] =  200.0f; plights[2].pos[1] = -30.0f; plights[2].pos[2] = -50.0f;
        plights[2].range  = 500.0f;
        plights[2].color[0] = 24.0f; plights[2].color[1] = 19.0f; plights[2].color[2] = 13.0f;
        // Slots 3-4 start dark; updateDynamicLights fills them once the pilot exists.
        plights[3] = {}; plights[4] = {};
        device->setPointLights(plights, (uint32_t)liveLightCount);

        // ===================== THE LOCAL STAR ================================
        // `--world space` is set in KETHZAR PRIME (x3::starsys::kDogfightSystemId):
        // the amber hypergiant the sun disc, the key/fill/rim rig and the sky were
        // all tuned against. A wormhole transit LANDS YOU SOMEWHERE ELSE, and the
        // single cue that makes that read in one glance is the star — a different
        // colour, a different brightness, and a key light on the hull that changes
        // with it. A retinted skybox alone is wallpaper; a retinted KEY LIGHT is a
        // different place.
        //
        // Everything below is INERT until a transit arrives: `starChanged` is false,
        // so every colour and every light is exactly the authored Kethzar value and
        // the departure frame is byte-identical to what shipped.
        const x3::starsys::StarSystem* homeSys =
            x3::starsys::findSystem(x3::starsys::kDogfightSystemId);
        float       starRGB[3]   = { 1.0f, 0.62f, 0.20f };  // current local star colour
        float       starLumRatio = 1.0f;                    // x the reference star
        bool        starChanged  = false;                   // has a transit re-dressed us?
        std::string systemName   = homeSys ? homeSys->name : "Kethzar Prime";
        std::string systemClass  = homeSys ? homeSys->starClass : "K0 hypergiant";
        if (homeSys) {
            starRGB[0] = homeSys->starColor[0];
            starRGB[1] = homeSys->starColor[1];
            starRGB[2] = homeSys->starColor[2];
        }
        // The AUTHORED Kethzar key/fill/rim, kept so the per-system retint is a
        // ratio against the tuned rig rather than a replacement for it.
        const float baseKey[3]  = { plights[0].color[0], plights[0].color[1], plights[0].color[2] };
        const float baseFill[3] = { plights[1].color[0], plights[1].color[1], plights[1].color[2] };
        const float baseRim[3]  = { plights[2].color[0], plights[2].color[1], plights[2].color[2] };
        const float baseSkyInt  = skyP.sunIntensity;
        // The AMBIENT is deep space's stand-in for "every other light in the sky",
        // and out here the overwhelming majority of that is the LOCAL STAR. Leaving
        // it fixed was measurable and wrong: with only the key/fill/rim retinted, a
        // decor hull's mean pixel moved by less than 2/255 between an amber
        // hypergiant and a red dwarf, because at these ranges the hull is mostly
        // ambient-lit. The star has to own this term too or "a different star"
        // never reaches the hull at all.
        float ambientRGB[3] = { 0.11f, 0.12f, 0.16f };
        const float baseAmbient[3] = { 0.11f, 0.12f, 0.16f };

        // Re-chromatise an AUTHORED Kethzar-amber colour for the current star.
        //
        // It would be easy and wrong to multiply the authored triple by a colour
        // ratio: the amber ramp has near-zero blue, so any ratio big enough to make
        // a blue star blue destroys the carefully tuned limb/shell falloff. Instead
        // this preserves the authored LUMINANCE exactly — every gradient, every
        // limb-darkening cue, every shell step survives — and swaps only the CHROMA
        // for the local star's. The result is normalised by its own max channel
        // rather than clamped per channel, so a hot star cannot flatten to pure
        // white: it lands ON the star's colour instead of clipping past it.
        auto starCol = [&](float r, float g, float b, float out3[3]) {
            if (!starChanged) { out3[0] = r; out3[1] = g; out3[2] = b; return; }
            const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            const float sl  = std::max(0.05f, 0.2126f * starRGB[0] +
                                              0.7152f * starRGB[1] +
                                              0.0722f * starRGB[2]);
            const float k = lum * std::min(1.35f, std::max(0.55f, starLumRatio)) / sl;
            float v[3] = { k * starRGB[0], k * starRGB[1], k * starRGB[2] };
            const float mx = std::max(v[0], std::max(v[1], v[2]));
            if (mx > 1.0f) { v[0] /= mx; v[1] /= mx; v[2] /= mx; }
            out3[0] = v[0]; out3[1] = v[1]; out3[2] = v[2];
        };
        // Emissive DRIVE multiplier for the star's body. Bounded hard: the sun's
        // own notes record that pushing the core emissive past ~1.7 blows the whole
        // disc over the bloom knee into a flat pale wafer with no granulation left.
        // A brighter star gets a brighter star, not a white hole in the frame.
        auto starEmMul = [&]() -> float {
            return starChanged ? std::min(1.25f, std::max(0.55f, starLumRatio)) : 1.0f;
        };

        // ARRIVE somewhere. Re-dresses the star colour, the star's LIGHT (key/fill/
        // rim + the sky's directional key, which is what actually shades the hull),
        // and the starfield phase, so the sky overhead is a different arrangement of
        // stars under a different-coloured sun. Called once per arrival.
        auto applySystem = [&](const char* sysId) -> bool {
            const x3::starsys::StarSystem* s = sysId ? x3::starsys::findSystem(sysId) : nullptr;
            if (!s || !homeSys) return false;
            starRGB[0] = s->starColor[0];
            starRGB[1] = s->starColor[1];
            starRGB[2] = s->starColor[2];
            starLumRatio = (homeSys->starLuminosity > 0.01f)
                         ? s->starLuminosity / homeSys->starLuminosity : 1.0f;
            systemName   = s->name;
            systemClass  = s->starClass;
            starChanged  = true;
            // THE SKY. sunColor is the analytic sky's own star disc + its
            // directional key; sunIntensity is how hard that key shades hulls.
            const float ei = std::min(1.6f, std::max(0.35f, starLumRatio));
            skyP.sunColor[0] = starRGB[0];
            skyP.sunColor[1] = starRGB[1];
            skyP.sunColor[2] = starRGB[2];
            skyP.sunIntensity = baseSkyInt * ei;
            // A whisper of the star's chroma into the deep-space floor, so even the
            // black between the stars is not the black of the system you left.
            skyP.zenith[0]  = 0.0022f + 0.0016f * starRGB[0];
            skyP.zenith[1]  = 0.0022f + 0.0016f * starRGB[1];
            skyP.zenith[2]  = 0.0030f + 0.0020f * starRGB[2];
            skyP.horizon[0] = 0.0030f + 0.0020f * starRGB[0];
            skyP.horizon[1] = 0.0034f + 0.0022f * starRGB[1];
            skyP.horizon[2] = 0.0044f + 0.0028f * starRGB[2];
            device->setSkyParams(skyP);
            // THE AMBIENT, re-chromatised the same way (authored intensity kept,
            // chroma swapped, scaled by the star's luminosity).
            {
                const float mx = std::max(baseAmbient[0], std::max(baseAmbient[1], baseAmbient[2]));
                float c[3]; starCol(baseAmbient[0] / mx, baseAmbient[1] / mx,
                                    baseAmbient[2] / mx, c);
                const float gA = std::min(1.7f, std::max(0.40f, starLumRatio));
                ambientRGB[0] = c[0] * mx * gA;
                ambientRGB[1] = c[1] * mx * gA;
                ambientRGB[2] = c[2] * mx * gA;
                device->setAmbient(ambientRGB[0], ambientRGB[1], ambientRGB[2]);
            }
            // THE STARFIELD. The analytic sky's procedural stars rotate with sky
            // time, so a large jump in phase is a genuinely DIFFERENT arrangement of
            // stars overhead — not the same sky with a filter over it. Derived from
            // the system id, so a given destination always has its own sky.
            uint32_t hsh = 2166136261u;
            for (const char* c = s->id; *c; ++c) { hsh ^= (uint32_t)(unsigned char)*c; hsh *= 16777619u; }
            device->setSkyTime(10.0f + (float)(hsh % 9973u) * 0.037f);
            x3::logInfo(std::string("--world space: ARRIVED in ") + s->name + " (" + s->starClass +
                        ") star=(" + std::to_string(starRGB[0]) + "," + std::to_string(starRGB[1]) +
                        "," + std::to_string(starRGB[2]) + ") keyLum x" + std::to_string(starLumRatio));
            return true;
        };

        // ---- Player ship (the SpacePilotController) -----------------------
        // FLIGHT MODE: seed from the persisted Settings-menu / console pick (the
        // cfg file is the only bridge into this standalone host — see below), fall
        // back to the shared latch, then apply the mode's feel preset. Live
        // switching is bound to the 1/2/3 keys in the windowed loop.
        {
            x3::game::FlightMode fm{};
            if (x3::game::parseFlightMode(std::to_string(x3::apphost::readFlightMode()), fm))
                x3::game::setRequestedFlightMode(fm);
        }
        x3::game::SpacePilotController pilot;
        pilot.spawn(*sphys, 0.0f, 0.0f, 0.0f);
        pilot.setMode(x3::game::requestedFlightMode());
        x3::logInfo(std::string("--world space: flight mode = ") +
                    x3::game::flightModeName(pilot.mode()) +
                    "  (press 1=Arcade 2=Assist 3=Loose to hot-swap)");

        // ---- Try to load an actual ship GLB. SpaceShip*.glb don't ship in
        //      assets/rigged_glb yet (per the task brief: "4 SpaceShip*.glb
        //      already in rigged_glb" was aspirational — the dir has none on
        //      this baseline). We fall back to DroneOscillating.glb as a
        //      stand-in flying object; if even that fails, we draw the ship
        //      as a procedural box.
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> mloader(x3::asset::createModelLoader(device, asrc.get()));
        // Probe candidates in order of preference: real ship asset first, drone fallback.
        const char* kShipCandidates[] = {
            "SpaceShip.glb", "SpaceShip2.glb", "SpaceShip3.glb", "SpaceShip4.glb",
            "DroneOscillating.glb", "DroneExportWMotion.glb"
        };
        x3::asset::Model shipModel{};
        std::string shipFile;
        for (const char* c : kShipCandidates) {
            shipModel = mloader->load(c);
            if (shipModel.ok) { shipFile = c; break; }
        }
        std::vector<x3::asset::ModelDrawable> shipDrawables;
        if (shipModel.ok) shipDrawables = x3::asset::makeDrawables(shipModel);
        x3::logInfo(std::string("--world space: ship model=") + (shipModel.ok ? shipFile : "<procedural-box-fallback>"));

        // Procedural-box fallback (in case the GLB load fails entirely).
        x3::prims::PrimMesh sbm = x3::prims::makeBox(2.0f, 0.6f, 1.2f, 0, 0, 0, 0.25f);
        auto shipBoxMesh = device->createMesh(sbm.verts.data(), (uint32_t)sbm.verts.size(),
                                              sbm.index.data(), (uint32_t)sbm.index.size());
        auto sbTexD = x3::prims::makeCheckerRGBA(64, 8, 180, 190, 210, 60, 70, 90);
        auto shipBoxTex = device->createTexture(sbTexD.data(), 64, 64, true);

        // ---- SENSE-OF-SPEED near-field streak/dust layer -------------------
        // CINEMATIC WARP GRACE (owner playtest: the old stretched-box streaks read
        // "clunky" — hard-edged uniform bars, giant beams, faceted dust chips). The
        // field is now kDust NEAR-CAMERA specks in a cube around the ship that drift
        // OPPOSITE velocity and wrap when they leave the box. Each renders as a SOFT
        // ROUND billboard-ish sphere (low-poly UV sphere, not a box → no facets) that
        // is a tiny dot at rest and, at speed, becomes a TAPERED COMET: 3 collinear
        // emissive segments of decreasing strength+thickness head→tail fake the
        // bright-head/transparent-tail alpha ramp (the emissive path has no per-vertex
        // alpha, so we layer diminishing segments instead). Length follows a SMOOTHSTEP
        // of speed (graceful ramp, capped so no giant beams), per-particle hash drives
        // size (0.5-1.5x)/brightness/colour-temperature, and a boundary-shell fade
        // stops any pop-in/out at the wrap. Pure render (no sim state) → determinism
        // untouched.
        x3::prims::PrimMesh strm = x3::prims::makeUVSphere(8, 12);   // soft round speck / streak seg
        auto dustMesh = device->createMesh(strm.verts.data(), (uint32_t)strm.verts.size(),
                                           strm.index.data(), (uint32_t)strm.index.size());
        const int   kDust = 220;        // <=400 (×3 segments only when moving fast)
        const float kDustR = 65.0f;     // wrap half-box (m) centered on the ship
        std::vector<x3::phys::Vec3> dust((size_t)kDust);
        for (int i = 0; i < kDust; ++i) {
            dust[(size_t)i] = x3::phys::Vec3{
                (hashF((uint32_t)(i*3+1))*2.0f - 1.0f) * kDustR,
                (hashF((uint32_t)(i*3+2))*2.0f - 1.0f) * kDustR,
                (hashF((uint32_t)(i*3+3))*2.0f - 1.0f) * kDustR };
        }
        bool boostActive = false;   // Shift-boost this frame (drives HUD + streak punch)
        float throttle01 = 0.0f;    // max(0, W-thrust) this frame (drives the thruster audio layer)

        // ===================================================================
        // REAL SUN — a physical star you can fly to (and die in). ============
        // ===================================================================
        // The analytic sky paints a sun disc along sp.sunDir (0.6,0.5,0.62). We put a
        // PHYSICAL emissive body along that SAME ray so the painted disc and the real
        // star coincide, and it visibly GROWS as you close (a real destination, not
        // sky paint). Layered emissive spheres beat a fancy shader here (cheap): a
        // white-gold CORE driven well past the bloom threshold (0.92) so it blooms,
        // plus additive translucent corona shells (warm gradient) around it.
        //
        // CHOSEN CONSTANTS (tuned for maxSpeed ~200-340 m/s ships; the dive from the
        // 15 km heat warning to the ~2.5 km surface should take ~1 min at cruise):
        //   kSunDist   20000 m   — spawn→star centre. NB: pulled in from the owner's
        //                          50 km starting point — at 50 km the disc's depth
        //                          compresses into the analytic-sky far-depth and the
        //                          sky overwrites it (empirically: renders < ~25 km,
        //                          vanishes at 50 km with far=60 km). 20 km keeps the
        //                          disc visible from spawn AND a ~90 s cruise journey.
        //   kSunRadius  2000 m   — core radius (real disc from spawn, fills frame near)
        // Heat/death use SURFACE distance  dSurf = |ship-centre| - kSunRadius:
        //   kHeatStart 15000 m   — hull temp begins to climb (NOMINAL→)
        //   kWarnDist   8000 m   — WARNING  (orange, "HULL TEMP RISING")
        //   kCritDist   4000 m   — CRITICAL (red flashing, "PULL AWAY", beep)
        //   kLightDist 10000 m   — warm proximity point-light ramps up (hull heat-glow)
        // Crossing the BODY (dSurf < 0) no longer kills — a shield engages and a 17 s
        // countdown + kill-cam sequence runs (see the Phase machine below).
        const x3::phys::Vec3 kSunDir    = vnorm(x3::phys::Vec3{ 0.6f, 0.5f, 0.62f });
        const float          kSunDist   = 20000.0f;
        const x3::phys::Vec3 kSunCenter = { kSunDir.x*kSunDist, kSunDir.y*kSunDist, kSunDir.z*kSunDist };
        const float          kSunRadius = 2000.0f;
        const float          kHeatStart = 15000.0f;
        const float          kWarnDist  = 8000.0f;
        const float          kCritDist  = 4000.0f;
        const float          kLightDist = 10000.0f;
        // Push the far plane past the star (50 km + radius) so it renders at range.
        device->setCameraFar(60000.0f);
        // Big smooth sphere for the sun core + corona shells + shield + shockwaves.
        x3::prims::PrimMesh sunm = x3::prims::makeUVSphere(48, 96);
        auto sunMesh = device->createMesh(sunm.verts.data(), (uint32_t)sunm.verts.size(),
                                          sunm.index.data(), (uint32_t)sunm.index.size());
        // CAMERA-FACING GLOW DISC (unit radius, XZ plane, normal +Y) — the building
        // block for the CENTER HOTSPOT + GLARE HALO (owner: "the sun should be
        // BLINDING — white-hot centre, big glare"). NB on WHY discs, not the literal
        // "additive billboard w/ radial texture": glass.frag adds a FLAT per-object
        // emissive (no emissive-map on the glass path — drawMeshGlass binds none), so
        // a radial TEXTURE can't shape the bloom (it'd bloom the whole quad). Instead
        // the radial gradient comes from GEOMETRY: several concentric camera-facing
        // discs of graduated size whose additive glass contributions STACK (same
        // no-scene additive accumulation the 30 corona shells already rely on) — most
        // overlap at the centre => blinding white core, fewest at the rim => soft
        // falloff. Flat + camera-facing => a clean CIRCLE (no square, no ring), and —
        // unlike a sphere — it can be SMALLER than the core without being occluded, so
        // the hotspot sits INSIDE the disc silhouette where the owner wants it.
        std::vector<x3::rhi::MeshVertex> discV; std::vector<uint32_t> discI;
        {
            const int kSeg = 64;
            discV.push_back({{0,0,0},{0,1,0},{0.5f,0.5f}});
            for (int i = 0; i <= kSeg; ++i) {
                const float a = (float)i / (float)kSeg * 6.2831853f;
                const float cx = std::cos(a), cz = std::sin(a);
                discV.push_back({{cx,0,cz},{0,1,0},{cx*0.5f+0.5f, cz*0.5f+0.5f}});
            }
            for (int i = 1; i <= kSeg; ++i) {   // CCW from +Y (the camera-facing side)
                discI.push_back(0); discI.push_back((uint32_t)(i+1)); discI.push_back((uint32_t)i);
            }
        }
        auto glowDiscMesh = device->createMesh(discV.data(), (uint32_t)discV.size(),
                                               discI.data(), (uint32_t)discI.size());
        // LIVING SUN SURFACE (owner: "sunspots, plasma flares, little variations as
        // the hydrogen fusion occurs" — was a flat emissive disc). One-time bake:
        // granulation + faculae + 3-7 sunspots (bakeSunRGBA, above), bound as BOTH
        // the core's baseColor AND its emissiveTex so the detail modulates the
        // bloom (mesh.frag multiplies the flat emissive uniform by the emissive
        // texture per-texel — see IRenderDevice::drawMeshPBR's emissiveTex arg).
        auto sunTexPx = bakeSunRGBA(384);
        auto sunTex = device->createTexture(sunTexPx.data(), 384, 384, /*srgb=*/true);
        // ---- INSIDE-THE-SUN plasma dome (deliverable B) --------------------------
        // Two low-res INVERTED spheres (winding flipped so the INNER surface faces a
        // camera sitting inside them), camera-anchored during InsideSun. Both sample
        // the same granulation bake (sunTex) as an emissive map; their UVs PAN every
        // frame (updateMesh — the ship_windows portal recipe) in OPPOSITE directions
        // and rates so the plasma churns. Layer A is an opaque warm backdrop; layer B
        // is a slightly smaller additive glass shell (alphaBlend=true so it skips the
        // depth pre-pass) counter-rotating over it. Cheap: 2 small meshes + UV re-up.
        auto makeInvertedSphere = [](uint32_t stacks, uint32_t slices) {
            x3::prims::PrimMesh m = x3::prims::makeUVSphere(stacks, slices);
            for (size_t t = 0; t + 2 < m.index.size(); t += 3)
                std::swap(m.index[t + 1], m.index[t + 2]);   // flip winding -> inward
            return m;
        };
        x3::prims::PrimMesh plasmaA = makeInvertedSphere(20, 40);
        x3::prims::PrimMesh plasmaB = makeInvertedSphere(20, 40);
        auto plasmaMeshA = device->createMesh(plasmaA.verts.data(), (uint32_t)plasmaA.verts.size(),
                                              plasmaA.index.data(), (uint32_t)plasmaA.index.size());
        auto plasmaMeshB = device->createMesh(plasmaB.verts.data(), (uint32_t)plasmaB.verts.size(),
                                              plasmaB.index.data(), (uint32_t)plasmaB.index.size());
        // Scratch vertex buffers reused each frame for the two panned UV re-uploads.
        std::vector<x3::rhi::MeshVertex> plasmaVA = plasmaA.verts;
        std::vector<x3::rhi::MeshVertex> plasmaVB = plasmaB.verts;
        // PLASMA FLARES / PROMINENCES: kFlareCount hash-seeded arcs on the limb,
        // each on its OWN rise->arch->collapse->fade lifecycle (period 20-60s,
        // staggered phases so 1-2 are typically live at once). `base0`/`base1` are
        // the two surface feet of the loop; height (and therefore visibility)
        // rides a single sin(pi*t) envelope per lifecycle — see drawFlares below.
        struct SunFlare { x3::phys::Vec3 base0, base1; float maxH, period, phase; };
        const int kFlareCount = 5;
        SunFlare flares[kFlareCount];
        for (int i = 0; i < kFlareCount; ++i) {
            const float ph  = hashF((uint32_t)(i * 41 + 1)) * 6.2831853f;
            const float th  = (0.22f + 0.56f * hashF((uint32_t)(i * 41 + 2))) * 3.14159265f;  // avoid poles
            const float ph2 = ph + 0.12f + 0.10f * hashF((uint32_t)(i * 41 + 3));
            const x3::phys::Vec3 n0{ std::sin(th)*std::cos(ph),  std::cos(th), std::sin(th)*std::sin(ph)  };
            const x3::phys::Vec3 n1{ std::sin(th)*std::cos(ph2), std::cos(th), std::sin(th)*std::sin(ph2) };
            flares[i].base0  = { kSunCenter.x + n0.x*kSunRadius, kSunCenter.y + n0.y*kSunRadius, kSunCenter.z + n0.z*kSunRadius };
            flares[i].base1  = { kSunCenter.x + n1.x*kSunRadius, kSunCenter.y + n1.y*kSunRadius, kSunCenter.z + n1.z*kSunRadius };
            flares[i].maxH   = kSunRadius * (0.05f + 0.10f * hashF((uint32_t)(i * 41 + 4)));
            flares[i].period = 20.0f + 40.0f * hashF((uint32_t)(i * 41 + 5));
            flares[i].phase  = hashF((uint32_t)(i * 41 + 6)) * flares[i].period;
        }
        flares[0].phase = flares[0].period * 0.5f;   // one flare guaranteed mid-arch at t=0 (proof screenshot)
        {
            // Force flare 0 onto the CAMERA-FACING LIMB (the hash angles are
            // otherwise free to land anywhere, including dead-centre where an
            // arc popping toward the camera reads as just more sun-coloured
            // bloom) so the guaranteed-active flare above actually shows as a
            // bump on the edge of the disc against black space in a straight-on
            // proof screenshot.
            const x3::phys::Vec3 wup = (std::fabs(kSunDir.y) < 0.95f) ? x3::phys::Vec3{0,1,0} : x3::phys::Vec3{1,0,0};
            const x3::phys::Vec3 latDir = vnorm(vcross(kSunDir, wup));   // perpendicular to view (screen-"horizontal")
            const x3::phys::Vec3 nF0 = vnorm(x3::phys::Vec3{
                latDir.x*0.95f - kSunDir.x*0.28f, latDir.y*0.95f - kSunDir.y*0.28f + 0.10f, latDir.z*0.95f - kSunDir.z*0.28f });
            const x3::phys::Vec3 nF1 = vnorm(x3::phys::Vec3{
                latDir.x*0.80f - kSunDir.x*0.15f, latDir.y*0.80f - kSunDir.y*0.15f - 0.15f, latDir.z*0.80f - kSunDir.z*0.15f });
            flares[0].base0 = { kSunCenter.x + nF0.x*kSunRadius, kSunCenter.y + nF0.y*kSunRadius, kSunCenter.z + nF0.z*kSunRadius };
            flares[0].base1 = { kSunCenter.x + nF1.x*kSunRadius, kSunCenter.y + nF1.y*kSunRadius, kSunCenter.z + nF1.z*kSunRadius };
        }

        // ---- Local math helpers (presentation only) -----------------------
        auto smooth01 = [](float e0, float e1, float x) -> float {
            // NB: e0>e1 is a valid INVERTED ramp (used by the heat/light curves), so
            // the zero-guard must preserve the denominator's SIGN, not force positive.
            float d = e1 - e0;
            if (std::fabs(d) < 1e-6f) d = (d < 0.0f) ? -1e-6f : 1e-6f;
            float t = (x - e0) / d;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            return t * t * (3.0f - 2.0f * t);
        };
        auto vlen = [](const x3::phys::Vec3& a) {
            return std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z);
        };
        // Build the +90°-yaw-corrected ship matrix (columns -r, u, f) at a pose.
        auto shipMatrix = [](const x3::phys::Vec3& p, const x3::phys::Vec3& f,
                             const x3::phys::Vec3& u, const x3::phys::Vec3& r, float m[16]) {
            m[0]=-r.x; m[1]=-r.y; m[2]=-r.z; m[3]=0;
            m[4]= u.x; m[5]= u.y; m[6]= u.z; m[7]=0;
            m[8]= f.x; m[9]= f.y; m[10]=f.z; m[11]=0;
            m[12]=p.x; m[13]=p.y; m[14]=p.z; m[15]=1;
        };
        // Translate+uniform-scale matrix (for the sun/corona/shield/shockwave spheres).
        auto sphereMatrix = [](const x3::phys::Vec3& c, float s, float m[16]) {
            m[0]=s; m[1]=0; m[2]=0; m[3]=0;  m[4]=0; m[5]=s; m[6]=0; m[7]=0;
            m[8]=0; m[9]=0; m[10]=s; m[11]=0; m[12]=c.x; m[13]=c.y; m[14]=c.z; m[15]=1;
        };
        // Same as sphereMatrix but also rotates about +Y (the UV sphere's pole
        // axis) — used to slowly spin the sun's surface texture (granulation
        // drift) without ever re-uploading the mesh.
        auto sphereMatrixYaw = [](const x3::phys::Vec3& c, float s, float yaw, float m[16]) {
            const float cy = std::cos(yaw), sy = std::sin(yaw);
            m[0]=cy*s; m[1]=0; m[2]=-sy*s; m[3]=0;
            m[4]=0;    m[5]=s; m[6]=0;     m[7]=0;
            m[8]=sy*s; m[9]=0; m[10]=cy*s; m[11]=0;
            m[12]=c.x; m[13]=c.y; m[14]=c.z; m[15]=1;
        };

        // ---- SUN-DEATH cinematic PHASE MACHINE (host-side; controller untouched) --
        // Flying → (cross body) → InsideSun[17s shield drain] → Detonation[antimatter
        // blast + coronal ejection, external kill-cam, sun ~half screen] → Rewind[~1s
        // backwards-scrub stinger of the ejection] → TitleCard["30 SECONDS EARLIER…"
        // film card on near-black] → Replay[forward re-entry from the external vantage:
        // the ship cruises in, breaches, entry flash] → Respawn[fade to black, re-seed
        // pose, "HULL LOST TO THE SUN — SHIELD HELD 17.0s", fade in] → Flying. Any key
        // skips straight to Respawn.
        enum class Phase { Flying, InsideSun, Detonation, Rewind, TitleCard, Replay, Respawn };
        Phase phase   = Phase::Flying;
        float phaseT  = 0.0f;                 // seconds elapsed in the current phase
        float shieldPct = 100.0f;
        bool  respawned = false;              // Respawn phase re-seed latch
        // INVULNERABILITY CHEAT (owner: "shield option that is invulnerable..
        // toggle with I"). Host-side only, default OFF, never persisted — see
        // the I-key edge-trigger in the windowed loop below (mirrors the V
        // camera-toggle pattern) and the ESC menu row. While true, InsideSun's
        // phaseT is continuously reset each frame (see advanceSequence), so the
        // 17s countdown never advances, Detonation can never trigger, and
        // shieldPct reads 100 — the molten-interior/plasma-swirl visuals are
        // untouched, only survivability changes.
        bool  g_invulnerable = false;
        float invulnFlashT = -1.0f;            // one-shot "INVULNERABILITY ON/OFF" caption; <0 = idle
        bool  invulnFlashOn = false;            // which caption text invulnFlashT is currently showing
        const float kInvulnFlashSecs = 1.5f;
        // AUDIT (owner playtest: "shield engaged at SUN 0.7km, still outside"):
        // the HUD's "SUN <km>" readout, the InsideSun trigger below (distC <
        // kSunRadius), and the HULL TEMP bands already shared ONE measure —
        // g_sunSurf = dist-to-centre - kSunRadius, i.e. distance to the OPAQUE
        // CORE surface, never a corona-shell radius — so they can't numerically
        // disagree. The real bug was that the pilot was FROZEN the instant
        // InsideSun began (no sphys->step, no pilot.update()), so there was no
        // graze/abort path at all AND no discrete cue at the crossing instant —
        // the ship was already gliding through the bright corona haze (shells
        // reach out to kSunRadius*2.6) for ~3 km before the hard core edge, with
        // nothing marking the actual moment, so it read as arbitrary/early. Fix:
        // keep the pilot LIVE through InsideSun (below) so a graze can actually
        // pull back out, and add a one-shot flash + caption at the exact instant
        // of crossing so it's unmistakable.
        float shieldFlashT = -1.0f;           // one-shot "SHIELD ENGAGED" cue; <0 = idle
        const float kShieldFlashSecs    = 1.5f;   // flash + caption hang time
        const float kShieldRechargeSecs = 5.0f;   // graze-abort: shield restores over this long
        // SUN-DEATH DIAGNOSTIC (owner: "hull STILL lost to the sun BEFORE entering",
        // 3rd report). Flip true only while instrumenting: logs the exact distC /
        // g_sunSurf / phase at every Flying->InsideSun and InsideSun->Detonation
        // transition, plus a 2 s heartbeat inside kWarnDist, so a scripted dive
        // (X3_SUN_DIVE_TEST=1, below) can PROVE where death actually fires. Ships
        // false — pure logging, zero effect on sim/render state.
        const bool  kSunDebugLog = false;
        float dbgLogTimer = 0.0f;             // heartbeat accumulator for kSunDebugLog
        const float kShieldSecs   = 17.0f;    // shield holds this long inside the body
        const float kDetonateSecs = 4.5f;     // blast + coronal ejection duration
        const float kRewindSecs   = 1.0f;     // short backwards-scrub stinger
        const float kTitleSecs    = 2.6f;     // "30 SECONDS EARLIER…" card (incl. fades)
        const float kReplaySecs   = 6.5f;     // forward re-entry replay (whole buffer)
        const float kFadeSecs     = 1.0f;     // fade-to-black → respawn → fade-in
        const int   kDebrisCount  = 32;       // coronal-ejection emissive fragments
        // ---- FIERY ENTRY-BURN SHEATH (owner: "fiery fringe around the ship as it
        // enters"). g_burnFactor is a 0..~1.5 proximity ramp read by drawBurnSheath/
        // drawBurnEmbers below: 0 outside ~1.5x the core radius (surface distance >=
        // kBurnStartSurf), 1.0 at/inside the core surface, PLUS a one-shot +50% flare
        // for kBurnFlareSecs at the exact surface-crossing instant (paired with the
        // existing SHIELD ENGAGED flash — same trigger point). Render-only/time+hash
        // driven — no sim-state, so --test-space determinism is unaffected.
        const float kBurnStartSurf = kSunRadius * 0.5f;   // surface-dist at distC == 1.5x radius
        const float kBurnFlareSecs = 2.0f;
        float burnFlareT = -1.0f;             // one-shot crossing flare; <0 = idle
        float g_burnFactor = 0.0f;            // loop-shared: live-ship burn intensity this frame
        x3::phys::Vec3 entryPos{ 0,0,0 };     // surface impact point (debris origin)
        x3::phys::Vec3 entryNrm{ 0,1,0 };     // outward normal at the impact point
        x3::phys::Vec3 cineCamPos{ 0,0,0 };   // frozen external kill-cam position
        float cineYaw = 0.0f, cinePit = 0.0f; // frozen external kill-cam look
        // Trajectory RING BUFFER — record the ship pose continuously so the kill-cam
        // can scrub/replay the fatal approach. 15 Hz × 32 s (>= the 30 s the title card
        // promises), render-only.
        struct TrajSample { x3::phys::Vec3 p, f, u, r; };
        const float kTrajHz = 15.0f;
        const int   kTrajLen = (int)(kTrajHz * 32.0f);   // 480 samples = 32 s
        std::vector<TrajSample> trajRing((size_t)kTrajLen);
        int   trajHead = 0, trajCount = 0;
        float trajTimer = 0.0f;
        std::vector<TrajSample> trajPlay;     // linearised oldest→entry at detonation
        // Loop-shared telemetry the HUD/overlay read (updated each frame in the loop).
        float g_heat = 0.0f;        // 0..1 hull-heat fraction (raw proximity)
        // DISPLAYED hull heat: eases toward g_heat normally, but toward 0 while
        // g_invulnerable (owner: "with invuln.. the hull temp should stay 0" — the
        // shield perfectly insulates). The ease (not a snap) means toggling invuln
        // OFF near the star ramps the temp up from ambient over ~2 s instead of
        // jumping to 3200C. tempC + the WARNING/CRITICAL states all derive from
        // this, so the whole temperature system goes quiet under invuln.
        float g_heatShown = 0.0f;
        float g_sunSurf = 1e9f;     // distance to the SUN SURFACE (m)
        float g_clock = 0.0f;       // presentation clock (blink/pulse/flash phases)

        // ---- Static decor fleet: a wing formation a few dozen meters out
        //      Each is a static placement transform (rotation around +Y for variety).
        //      Coordinates chosen so the headless screenshot camera (at -X behind
        //      the player ship, looking toward +X) sees a tight cluster of ships
        //      filling a good portion of the frame.
        struct DecorShip { float x, y, z, yaw, scale; };
        const DecorShip decor[] = {
            {   30.0f,   2.0f,    8.0f,  0.2f, 18.0f },  // close right
            {   35.0f,   4.0f,  -10.0f, -0.3f, 20.0f },  // close left, up
            {   45.0f,  -2.0f,   18.0f,  0.4f, 22.0f },  // mid-right, down
            {   55.0f,   8.0f,   -4.0f,  0.0f, 24.0f },  // mid lead, up
            {   80.0f,   0.0f,  -25.0f,  0.6f, 28.0f },  // far escort left
            {   80.0f,   2.0f,   25.0f, -0.6f, 28.0f },  // far escort right
        };
        const int kDecorCount = (int)(sizeof(decor) / sizeof(decor[0]));

        // CombatFx for laser tracers + impact decals. Heap-allocated because
        // CombatFx carries ~256 KB of mutable scratch instance arrays; piling
        // another stack copy into main() (which already holds one for the
        // canonplay/level1 paths) overflows the 1 MB default thread stack.
        auto combatFxOwned = std::make_unique<x3::game::CombatFx>();
        x3::game::CombatFx& combatFx = *combatFxOwned;
        combatFx.init(*device);

        // ================= feat/wormholes — THE WORMHOLES =====================
        // Two of them (one stable, one not), seeded from the shared roster in
        // app/space/wormhole.cpp so the world and the self-test cannot drift.
        // They are HELD OPEN by default, because the player should be able to fly
        // out and find them. X3_WORMHOLE_OPEN=1 instead runs the staged opening
        // live from Dormant — that is how the opening sequence gets captured.
        x3::space::WormholeField wormholes;
        wormholes.init(*device);
        // X3_WORMHOLE_OFF=1 seeds NOTHING — an A/B control for measuring exactly
        // what this feature costs (draw calls, lights, sync hazards) against the
        // same binary, instead of against a differently-built one.
        {
            const char* off = std::getenv("X3_WORMHOLE_OFF");
            if (!(off && off[0] == '1')) {
                x3::space::seedSpaceWormholes(wormholes);
                const char* staged = std::getenv("X3_WORMHOLE_OPEN");
                const bool  stage  = staged && staged[0] == '1';
                for (int i = 0; i < wormholes.count(); ++i) {
                    if (stage) wormholes.at(i).open();
                    else       wormholes.at(i).forceHeld();
                }
            }
        }
        // X3_WORMHOLE_T=<seconds>: pre-roll the field before the capture frame so
        // a HEADLESS shot can land anywhere on the staged opening. The capture
        // path renders ONE frame, so a staged event can only be judged honestly
        // as a SERIES of runs at different t — this is the knob that makes that
        // series possible. Pre-rolled at a fixed 1/165 s step, so the frames the
        // series produces are the frames the 165 Hz game actually shows.
        float wormholePreroll = 0.0f;
        if (const char* wt = std::getenv("X3_WORMHOLE_T"))
            wormholePreroll = (float)std::atof(wt);
        // X3_WORMHOLE_ENTER=<index>: park the ship IN that wormhole's mouth so a
        // headless run actually takes the transit. The capture path never flies,
        // so without this the transit leg could only ever be asserted by a unit
        // test — and "entering one means something" is a claim about the world,
        // not about a test.
        int wormholeEnterIdx = -1;
        if (const char* we = std::getenv("X3_WORMHOLE_ENTER"))
            wormholeEnterIdx = std::atoi(we);

        // The S0/S3 transit spine. Flying into an open throat engages the SAME
        // SpaceLayer state machine and the SAME crystal-matrix WormholeVfx that
        // `--world wormhole-transit` drives — one implementation, two entrances.
        x3::space::SpaceLayer spaceLayer;
        spaceLayer.init();
        x3::space::WormholeTransit transit;
        // 5 s was a fade with a world swap behind it. 8.5 s is a RIDE: a violent
        // entry punch, a long stretch inside the throat with the instruments off
        // their scales, and a bloom out the far side. Long enough to be a journey,
        // short enough that the player never wants the helm back.
        transit.init(*device, spaceLayer, /*durationSec=*/8.5f);
        bool  transitEngaged = false;   // true from entry until arrival
        int   transitFrom    = -1;      // which wormhole swallowed us
        float transitClock   = 0.0f;
        float commsPublishAcc = 0.0f;   // the advisory is a ~10 Hz duty, not per-frame

        // ================= WHERE EACH CORRIDOR ACTUALLY GOES ==================
        // The field seeds two holes (wormhole.cpp seedSpaceWormholes): THE GAMMA
        // CORRIDOR (stable, id 900) and THE DERELICT APERTURE (unstable, id 901).
        // Until now requestWormhole() DISCARDED its destination argument and both
        // ends of the jump were the same star system - the transit was a loop.
        //
        //   THE GAMMA CORRIDOR    -> SIRIUS. Amber K0 hypergiant to A1V BLUE-WHITE:
        //     the far end of the spectrum from home, so "we are somewhere else" is
        //     legible in a single frame from the colour of the star alone.
        //   THE DERELICT APERTURE -> plotted for Tau Ceti, ARRIVES at WOLF 359.
        //     An unstable corridor does not put you where you aimed. A dim little
        //     M6.5V red dwarf is as far from a blue-white A-type as the registry
        //     goes, so the two destinations cannot be confused with each other
        //     either.
        auto planFor = [&](const x3::space::Wormhole& w) {
            x3::space::TransitPlan pl;
            pl.corridorName = w.name();
            pl.fromSystem   = systemName.c_str();
            pl.stable       = w.stable();
            const char* wantId = (w.id() == 901) ? "tau_ceti" : "sirius";
            // An UNSTABLE corridor lands you somewhere other than the plot. The
            // AEGIS script says so; the sky proves it.
            const char* gotId  = pl.stable ? wantId : "wolf_359";
            const x3::starsys::StarSystem* want = x3::starsys::findSystem(wantId);
            const x3::starsys::StarSystem* got  = x3::starsys::findSystem(gotId);
            pl.toSystem   = want ? want->name : "Sirius";     // what AEGIS PLOTTED
            pl.toSystemId = got  ? got->id    : "sirius";     // where you ARRIVE
            pl.distanceLy = want ? want->distanceLy : 8.6f;
            return pl;
        };

        // ================= THE RIDE FRAME ====================================
        // The transit plays in its own stretch of world, far from the fleet and the
        // star, with the world scene NOT drawn: for eight and a half seconds the
        // throat IS the frame. Assembling the tunnel around a fixed anchor (rather
        // than around wherever the player happened to enter) keeps the coordinates
        // small, so the walls never lose precision no matter how far out the
        // wormhole sat.
        const x3::phys::Vec3 kTransitAnchor{ 0.0f, 9000.0f, 0.0f };
        // Engine yaw convention (docs/COORDINATES.md): forward = (cos yaw, 0, sin
        // yaw), so yaw = pi/2 looks down +Z - which is the axis the crystal tube is
        // authored along. Looking DOWN the tunnel, not across it. (The old
        // --world wormhole-transit showcase used yaw 0: it stood inside the tube
        // and looked sideways at the wall.)
        const float kTransitYaw = 1.57079633f;

        const float dt = 1.0f / 60.0f;

        // Draw a ship at a placement matrix (yaw-only for decor; full quat for
        // the player ship). W6-2: ships ride the FULL PBR path now (normal/MR/
        // authored emissive — the same conversion monsters got in Wave 1), so
        // hulls catch the light rig as lit metal instead of the old basic-path
        // "×4 + 0.45 floor" albedo hack that flattened them to silhouettes.
        // `bright` is a gentle exposure assist for deep space (no bounce light),
        // applied as a modest albedo scale, not a floor.
        auto drawShipAt = [&](const x3::rhi::FrameContext& frame,
                              const float xform[16], float bright) {
            if (shipModel.ok) {
                for (const auto& dr : shipDrawables) {
                    float fin[16];
                    x3::asset::mulMat4(xform, dr.nodeTransform, fin);
                    // GAMMA-RECAL round 2: assist 1.2x -> 0.5x — the 2.2-2.8x base-
                    // colour push was most of the 'overall glow' on the honest curve.
                    const float b = 1.0f + 0.5f * bright;   // exposure assist (no floor)
                    const float tint[4] = {
                        dr.baseColorFactor[0] * b,
                        dr.baseColorFactor[1] * b,
                        dr.baseColorFactor[2] * b,
                        dr.baseColorFactor[3]
                    };
                    // Authored emissive (canopies/engine glow) at full strength, PLUS a
                    // faint cool STARLIGHT AMBIENT floor: deep space has no bounce term,
                    // so pure PBR renders near-black hulls invisible. The floor keeps the
                    // silhouette readable as dim metal while normals/MR still shade from
                    // the real light rig. Kept far below bloom threshold (bible: no blobs).
                    const float amb = 0.006f * (1.0f + bright);   // GAMMA-RECAL round 2:
                                                                  // near-killed — the star's
                                                                  // GGX highlight + authored
                                                                  // interior/engine emissive
                                                                  // carry the hull now
                    const float emis[4] = { dr.emissiveFactor[0] + amb,
                                            dr.emissiveFactor[1] + amb * 1.05f,
                                            dr.emissiveFactor[2] + amb * 1.25f, 1.0f };
                    device->drawMeshPBR(frame, x3::rhi::MeshHandle{ dr.meshId },
                                        x3::rhi::TextureHandle{ dr.baseColorTexId },
                                        x3::rhi::TextureHandle{ dr.normalTexId },
                                        x3::rhi::TextureHandle{ dr.mrTexId },
                                        tint, emis, fin,
                                        dr.alphaMask, dr.alphaBlend,
                                        x3::rhi::TextureHandle{ dr.emissiveTexId },
                                        x3::rhi::TextureHandle{ dr.detailTexId },
                                        dr.detailUvScale);
                }
            } else {
                const float white[4] = { bright, bright, bright, 1.0f };
                device->drawMesh(frame, shipBoxMesh, shipBoxTex, white, xform);
            }
        };

        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            // Player ship: build a 4x4 from quaternion + position (visible only
            // in 3P; in 1P it would clip the near plane — host gates visuals).
            if (pilot.isThirdPerson()) {
                const x3::phys::Vec3 p = pilot.pos();
                const x3::phys::Vec3 f = pilot.forward();
                const x3::phys::Vec3 r = pilot.right();
                const x3::phys::Vec3 u = pilot.up();
                // +90deg yaw correction: the Minerva GLB is authored with its
                // NOSE along model +Z (where the engine convention puts the right
                // wing), so mapping model +X->forward rendered it yawed 90deg to
                // starboard. Remap so the model's +Z nose points along forward():
                // columns (f,u,r) -> (-r,u,f). This is a proper rotation (det +1,
                // (-r)x u = f), so the hull is rotated, NOT mirrored.
                float m[16] = {
                    -r.x, -r.y, -r.z, 0,  // col 0 = model +X  (<- -right)
                    u.x,  u.y,  u.z,  0,  // col 1 = model +Y  (up)
                    f.x,  f.y,  f.z,  0,  // col 2 = model +Z = nose -> forward
                    p.x,  p.y,  p.z,  1
                };
                drawShipAt(frame, m, 1.5f);
            }
            // Decor fleet.
            for (int i = 0; i < kDecorCount; ++i) {
                const float c = std::cos(decor[i].yaw), s = std::sin(decor[i].yaw);
                const float S = decor[i].scale;
                float m[16] = {
                    c*S, 0,  -s*S, 0,
                    0,   S,  0,   0,
                    s*S, 0,  c*S, 0,
                    decor[i].x, decor[i].y, decor[i].z, 1
                };
                drawShipAt(frame, m, 1.0f);
            }
        };

        // ---- Advance the dust field (drift opposite velocity + wrap) -------
        auto updateDust = [&](float d) {
            const x3::phys::Vec3 sp = pilot.pos();
            const x3::phys::Vec3 vel = pilot.velocity();
            for (int i = 0; i < kDust; ++i) {
                x3::phys::Vec3& p = dust[(size_t)i];
                p.x -= vel.x * d; p.y -= vel.y * d; p.z -= vel.z * d;
                auto wrap = [&](float& c, float center) {
                    float dd = c - center;
                    while (dd >  kDustR) { c -= 2*kDustR; dd -= 2*kDustR; }
                    while (dd < -kDustR) { c += 2*kDustR; dd += 2*kDustR; }
                };
                wrap(p.x, sp.x); wrap(p.y, sp.y); wrap(p.z, sp.z);
            }
        };

        // ---- Draw the near-field streak/dust specks ------------------------
        auto drawSpeedFx = [&](const x3::rhi::FrameContext& frame) {
            const x3::phys::Vec3 sp   = pilot.pos();
            const x3::phys::Vec3 vel  = pilot.velocity();
            const float spd  = pilot.speed();
            const float maxS = std::max(1.0f, pilot.tuning().maxSpeed);
            const float sf   = std::min(1.0f, spd / maxS);
            // EASED response: length/brightness ride a smoothstep of speed (graceful
            // ramp-in), plus a soft extra kick on boost. Capped so no giant beams.
            // FIX (owner: streaks read as static bars at low speed) — the ramp used
            // to start at sf==0 (any motion at all began stretching specks into
            // streaks). Now it stays essentially pointlike below ~35% of max speed
            // and only reaches full stretch near max speed, so cruise reads as a
            // twinkling starfield and streaking is reserved for genuinely fast flight.
            const float resp = smooth01(0.35f, 0.95f, sf) * (boostActive ? 1.30f : 1.0f);
            x3::phys::Vec3 vd{ 1, 0, 0 };
            if (spd > 0.3f) vd = x3::phys::Vec3{ vel.x/spd, vel.y/spd, vel.z/spd };
            const x3::phys::Vec3 ref = (std::fabs(vd.y) < 0.95f)
                                     ? x3::phys::Vec3{ 0, 1, 0 } : x3::phys::Vec3{ 1, 0, 0 };
            const x3::phys::Vec3 uax = vnorm(vcross(ref, vd));
            const x3::phys::Vec3 vax = vcross(vd, uax);
            const bool moving = resp > 0.03f;
            // Head→mid→tail segment layout (fakes a bright-head/transparent-tail taper;
            // the emissive path has no per-vertex alpha). offset·halfLen, thick·, str·.
            const float segOff[3]  = { -0.55f, 0.0f, 0.55f };
            const float segThk[3]  = {  1.00f, 0.72f, 0.50f };
            const float segStr[3]  = {  1.00f, 0.55f, 0.28f };
            const float kMaxHalf   = 1.15f;   // cap: total streak <= ~2.5 m at scale
            for (int i = 0; i < kDust; ++i) {
                const x3::phys::Vec3& p = dust[(size_t)i];
                // Per-particle hash variance: size 0.5-1.5, temp (below), MAGNITUDE-
                // distributed brightness (owner: "all the same white... some very
                // faint points"). pow(hashF, 2.2) skews most particles dim (real star
                // fields are mostly faint), with a rarer bright tail.
                const float hsz    = 0.5f + hashF((uint32_t)(i*7 + 5));
                const float hbrRaw = hashF((uint32_t)(i*11 + 3));
                const float hmag   = std::pow(hbrRaw, 2.2f);
                const float hbr    = 0.16f + 1.15f * hmag;             // mostly dim, few bright
                const float htmp   = hashF((uint32_t)(i*13 + 9));
                // Boundary-shell fade: nothing pops in/out at the wrap (fades over the
                // outer ~18% of the box). Chebyshev distance to the wrap boundary.
                const float dcx = std::fabs(p.x - sp.x), dcy = std::fabs(p.y - sp.y),
                            dcz = std::fabs(p.z - sp.z);
                const float dedge = std::max(dcx, std::max(dcy, dcz)) / kDustR;
                const float fade = 1.0f - smooth01(0.82f, 1.0f, dedge);
                if (fade < 0.02f) continue;
                // COLOUR VARIANCE (owner: push the spread so it's visible): bucketed
                // palette instead of one narrow blend — cool blue-white -> pure white
                // (most common), warm yellow-white, and a rare faint amber/orange.
                float base[4];
                if (htmp < 0.55f) {
                    const float k = htmp / 0.55f;
                    base[0] = 0.72f + 0.28f*k; base[1] = 0.82f + 0.18f*k; base[2] = 1.00f;
                } else if (htmp < 0.85f) {
                    const float k = (htmp - 0.55f) / 0.30f;
                    base[0] = 1.00f; base[1] = 0.92f - 0.10f*k; base[2] = 0.80f - 0.25f*k;
                } else {
                    const float k = (htmp - 0.85f) / 0.15f;
                    base[0] = 1.00f; base[1] = 0.70f - 0.15f*k; base[2] = 0.35f - 0.15f*k;
                }
                base[3] = 1.0f;
                // TWINKLE: slow per-star brightness oscillation, faint stars twinkle
                // MORE than bright ones; fades out as streaks form (resp) so a fully
                // stretched comet doesn't shimmer.
                const float twF     = 0.6f + 2.0f * hashF((uint32_t)(i*17 + 21));
                const float twPhase = hashF((uint32_t)(i*19 + 33)) * 6.2831853f;
                const float twDepth = (0.40f - 0.15f * hmag) * (1.0f - resp);
                const float twinkle = 1.0f + twDepth * std::sin(g_clock * twF * 6.2831853f + twPhase);
                const float halfLen = std::min(kMaxHalf, 0.06f + kMaxHalf * resp * hsz);
                const float thick   = (0.012f + 0.020f * sf) * hsz;
                const float S = (0.22f + 2.6f * resp) * hbr * fade * twinkle;
                const int segs = moving ? 3 : 1;
                for (int s = 0; s < segs; ++s) {
                    const float str = S * segStr[s];
                    if (str < 0.015f) continue;
                    const x3::phys::Vec3 c{ p.x + vd.x*halfLen*segOff[s],
                                            p.y + vd.y*halfLen*segOff[s],
                                            p.z + vd.z*halfLen*segOff[s] };
                    float m[16];
                    composeBasis(m, uax, vax, vd, thick*segThk[s], thick*segThk[s],
                                 std::max(thick, halfLen*0.42f), c);
                    const float emis[4] = { base[0], base[1], base[2], str };
                    device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, base, emis, m);
                }
            }
        };

        // ---- REAL SUN render: granulated orange core + tight additive corona ----
        // `camPos` (v4): the render camera this frame. Two uses — (1) the CENTER
        // HOTSPOT + GLARE HALO discs face it; (2) INTERIOR-BULLSEYE fix: each
        // concentric corona/churn SHELL fades out as the camera enters it, so you
        // never see a shell's inner surface edge-on as a dark-red "porthole" ring
        // when engulfed (owner bug at SUN 0.8 km) — the molten interior takes over.
        auto drawSun = [&](const x3::rhi::FrameContext& frame, const x3::phys::Vec3& camPos) {
            float m[16];
            const float camDistC = vlen(x3::phys::Vec3{ camPos.x-kSunCenter.x,
                                        camPos.y-kSunCenter.y, camPos.z-kSunCenter.z });
            // Per-shell camera fade: 1 when the camera is comfortably OUTSIDE a shell
            // of world radius R, ramping to 0 as it crosses inside ~1.05x R (kills the
            // edge-on inner-surface ring). Used by the corona + churn shells below.
            auto shellCamFade = [&](float R) { return smooth01(R * 1.05f, R * 1.28f, camDistC); };
            // ============ SUN VISUAL v3 (owner: "make it LOOK like a sun") =========
            // The old look was a FRIED EGG: three hard-edged corona shells out to
            // 2.6x = 5.2 km (a flat gold disc), a bright WHITE RING at the tight gold
            // shell, and a pale washed core. Two coupled problems: it read as
            // concentric rings (not a star), and the visible edge (5.2 km) sat far
            // outside the 2.0 km shield trigger, so the sun "started" nowhere near
            // where it killed you (the "died before entering" perception bug — the
            // phase machine itself is correct; a scripted X3_SUN_DIVE_TEST proved
            // death only fires 17 s AFTER distC<kSunRadius, deep in the core).
            //
            // v3 = a granulated ORANGE core (the dominant body) + a TIGHT additive
            // corona hugging it (visible edge ~1.34x = 2.7 km ≈ the trigger, so the
            // bright surface and the kill radius now agree). Fixes that got here:
            //
            //  * All glass draws pass alphaBlend=true -> the BLEND partition, which the
            //    depth PRE-PASS skips (SSAO/SSGI/reflections on by default): the shells
            //    share sunMesh with the opaque core, and in the OPAQUE partition the
            //    fragment-less pre-pass would write the nearest shell's depth across the
            //    disc and occlude the core. BLEND keeps the core visible.
            //  * GlassMaterial.refraction MUST be 0 on every shell. The default 0.03
            //    refracts the backdrop at each shell's limb into a glassy "bubble" rim
            //    — a pale ring hugging the core. Zeroed = pure additive glow, no lens.
            //  * The opaque core occludes the shells at screen-radius <1.0, so just
            //    OUTSIDE the core silhouette the full SUM of every shell appears at once
            //    — a bright step-up = a RING — while the core's own textured limb sat
            //    DARKER than that sum (the dark band). Two fixes: (a) keep the halo DIM
            //    enough that its brightest point (the sum at the core edge) stays BELOW
            //    the core surface brightness, so the step at the silhouette is a gentle
            //    DOWNWARD fade (a glow), never a bright ring; (b) redden the halo
            //    outward so the base reads as a chromosphere rim (cheap limb-darkening
            //    cue) melting into the corona.
            //  * Also: bloom rim. Core emissive was 2.9 (WHOLE disc over the 0.92 bloom
            //    threshold -> pale wafer + a pale bloom ring). Dropped to ~1.05 and the
            //    bright cell cores toned to GOLD not white, so only sparse faculae bloom
            //    and the surface holds its orange granulation.
            // MANY thin shells (was 14 — individually visible as concentric BANDS in
            // captures). 30 halves the radius step between neighbours so the sum reads
            // as a smooth gradient, not stacked rings. Start at 1.012x (NOT 1.0x — a
            // shell coincident with the opaque core z-fights it and punched the thin
            // DARK BAND seen at the limb in v3-close).
            const int kCoronaShells = 30;
            for (int i = kCoronaShells - 1; i >= 0; --i) {
                const float f  = (float)i / (float)(kCoronaShells - 1);   // 0=limb .. 1=outer
                // v5b: pull the corona in from 1.34x -> 1.16x so it HUGS the core. Each
                // glass shell limb-brightens into a ring at its own projected radius; at
                // 1.34x (now exposed by the tightened halo) the outermost shell floated a
                // hard orange ring out in a dark gap (owner "dark ring"). Hugging the core
                // + tapering the outer op to 0 (see below) removes that free-floating ring.
                const float s  = 1.012f + f * (1.16f - 1.012f);
                // INTERIOR-BULLSEYE fix: fade this shell out once the camera is inside
                // it (no edge-on inner-surface ring when engulfed).
                const float sfade = shellCamFade(kSunRadius * s);
                if (sfade < 0.01f) continue;
                // MONOTONIC exponential falloff, TAPERED to exactly 0 at the outer edge
                // ((1-f^2)) so the outermost shell has no grazing limb ring — the corona
                // just melts into space. Per-shell op is LOW so the sum stays a faint glow.
                const float op = 0.024f * std::exp(-2.6f * f) * (1.0f - f * f) * sfade;
                // Deep orange at the limb -> red -> dark red as it fades out. Kept
                // SATURATED (low green/blue) on purpose: a pale/gold halo reads as a
                // bright ring hugging the core, a saturated-orange one melts into the
                // corona (learned the hard way — the gold version popped a ring).
                // THE LOCAL STAR: starCol() preserves this authored luminance ramp
                // exactly (limb -> outer falloff, the whole reason the corona melts
                // instead of ringing) and swaps only the CHROMA for the star we are
                // actually under. Before any transit it is the identity.
                float cc[3];
                starCol(1.0f, std::max(0.0f, 0.40f - 0.30f * f),
                              std::max(0.0f, 0.11f - 0.09f * f), cc);
                const float r = cc[0], g = cc[1], b = cc[2];
                sphereMatrix(kSunCenter, kSunRadius * s, m);
                const float bc[4] = { r, std::max(0.0f,g), std::max(0.0f,b), 1.0f };
                const float em[4] = { r, std::max(0.0f,g), std::max(0.0f,b), 0.7f * starEmMul() };
                x3::rhi::IRenderDevice::GlassMaterial gm{};
                gm.opacity = op; gm.roughness = 1.0f; gm.specular = 0.0f;
                gm.refraction = 0.0f;   // NO screen-space distortion — the default 0.03 refracts
                                        // the backdrop at each shell limb = a glassy "bubble" rim
                                        // (the pale ring hugging the core). Zeroing it leaves a
                                        // pure additive glow, no lens edge.
                gm.tint[0] = bc[0]; gm.tint[1] = bc[1]; gm.tint[2] = bc[2];
                device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{}, bc, em, gm, m,
                                      /*alphaBlend=*/true);   // BLEND partition: skip depth pre-pass
            }
            // CHURN SHELL: a faint textured copy counter-rotating a touch faster than
            // the core — cheap parallax "boil". Hugs the core (1.015x) and kept dim so
            // it modulates the surface rather than forming its own mid-tone annulus.
            const float churnYaw = -g_clock * 0.026f;
            const float churnFade = shellCamFade(kSunRadius * 1.006f);   // bullseye fix
            if (churnFade > 0.01f) {
                sphereMatrixYaw(kSunCenter, kSunRadius * 1.006f, churnYaw, m);
                float ch[3]; starCol(1.0f, 0.55f, 0.22f, ch);
                const float bc[4] = { ch[0], ch[1], ch[2], 1.0f };
                const float em[4] = { ch[0], ch[1], ch[2], 0.35f * churnFade * starEmMul() };
                x3::rhi::IRenderDevice::GlassMaterial gm{};
                gm.opacity = 0.09f * churnFade; gm.roughness = 1.0f; gm.specular = 0.0f;
                gm.refraction = 0.0f;   // no lens edge (see corona note above)
                gm.tint[0]=ch[0]; gm.tint[1]=ch[1]; gm.tint[2]=ch[2];
                device->drawMeshGlass(frame, sunMesh, sunTex, bc, em, gm, m,
                                      /*alphaBlend=*/true);   // BLEND partition: skip depth pre-pass
            }
            // GRANULATED CORE — the dominant body. Baked granulation/sunspot/faculae
            // texture bound as BOTH baseColor and emissiveTex (detail modulates the
            // bloom), slowly rotating, with a FUSION SHIMMER (slow sum-of-sines,
            // +-~8%). The ONLY opaque sun draw. v3 drops emissive 2.9 -> 1.7: at 2.9
            // the WHOLE disc cleared the ~0.92 bloom threshold and blew to a flat pale
            // wafer; at 1.7 only the white-gold cell centres + faculae bloom, so the
            // deep-orange lanes and sunspots survive and the disc reads as textured
            // boiling plasma (SDO look) instead of a washed circle.
            const float coreYaw = g_clock * 0.010f;
            sphereMatrixYaw(kSunCenter, kSunRadius, coreYaw, m);
            const float shimmer = 1.0f
                + 0.05f * std::sin(g_clock * (6.2831853f / 5.3f))
                + 0.03f * std::sin(g_clock * (6.2831853f / 7.7f) + 1.7f);
            float coreC[3]; starCol(1.0f, 0.62f, 0.28f, coreC);
            const float cbc[4] = { coreC[0], coreC[1], coreC[2], 1.0f };
            // v4: nudge 1.05 -> 1.20. Slightly hotter base so the gold cell cores +
            // faculae bloom a touch harder, while the deep-orange lanes still hold the
            // granulation (kept well under the 2.9 "white wafer" regime). The BLINDING
            // white centre is NOT from the core emissive (that would wafer the whole
            // disc again) — it comes from the hotspot disc stack below.
            // v5: the granulation floor is now much brighter (yellow-gold everywhere),
            // so drop the emissive 1.20 -> 0.95 to hold the limb JUST below the bloom
            // knee — the disc reads bright saturated gold (no dark ring), while the
            // white-gold cell cores + faculae + the hotspot stack own the blinding core.
            const float cem[4] = { coreC[0], coreC[1], coreC[2], 0.95f * shimmer * starEmMul() };
            device->drawMeshPBR(frame, sunMesh, sunTex, x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                cbc, cem, m, /*alphaMask=*/false, /*alphaBlend=*/false, sunTex);

            // ================= CENTER HOTSPOT + GLARE HALO (v4) ==================
            // Real-sun photo look: a BLINDING white-hot centre grading to gold, with a
            // big soft glare that dominates the sky — while the limb keeps its orange
            // granulation. Built from camera-facing additive glow discs (see glowDiscMesh
            // note): the radial gradient is GEOMETRIC (concentric discs of graduated size
            // whose additive glass contributions STACK — most overlap dead-centre).
            const x3::phys::Vec3 toCamV{ camPos.x - kSunCenter.x, camPos.y - kSunCenter.y, camPos.z - kSunCenter.z };
            if (camDistC > kSunRadius * 1.02f) {   // once basically inside, the interior dome takes over
                const x3::phys::Vec3 toCam = vnorm(toCamV);
                // Camera-facing basis (the discs are radially symmetric, so any basis
                // perpendicular to toCam works — roll is irrelevant).
                const x3::phys::Vec3 wup = (std::fabs(toCam.y) < 0.95f) ? x3::phys::Vec3{0,1,0} : x3::phys::Vec3{1,0,0};
                const x3::phys::Vec3 bru = vnorm(vcross(wup, toCam));
                const x3::phys::Vec3 bup = vcross(toCam, bru);
                // Anchor the disc plane JUST in front of the near pole (tangent plane of
                // the core) so the opaque core can never occlude it, with a hair of
                // clearance to avoid z-fighting the pole.
                const float dB = camDistC - kSunRadius;                 // near-pole distance from camera
                const x3::phys::Vec3 sprC{ kSunCenter.x + toCam.x*kSunRadius*1.006f,
                                           kSunCenter.y + toCam.y*kSunRadius*1.006f,
                                           kSunCenter.z + toCam.z*kSunRadius*1.006f };
                // Apparent disc angular radius ~ kSunRadius/camDistC. Sizing a disc's
                // world half-extent as frac*discAng*dB holds its APPARENT size at a
                // constant fraction of the sun disc across the whole 18 km -> 3 km
                // approach (frac is that fraction).
                const float discAng = kSunRadius / std::max(1.0f, camDistC);
                // FADE: ramp in with proximity (a tiny far disc shouldn't be a blazing
                // dot), and fade OUT again as the ship enters the body (~1.02x..1.5x)
                // so the hotspot/halo don't clip weirdly once the molten interior owns
                // the frame.
                const float nearFade = smooth01(kSunRadius * 1.02f, kSunRadius * 1.5f, camDistC);
                auto glowDisc = [&](float frac, float emStr, float cr, float cg, float cb) {
                    const float half = frac * discAng * dB;
                    if (half < 1.0f) return;
                    // THE LOCAL STAR, at the single choke point every halo/hotspot
                    // disc goes through: the gold-to-white glare ramp keeps its
                    // authored brightness curve and takes the star's chroma.
                    { float gc[3]; starCol(cr, cg, cb, gc); cr = gc[0]; cg = gc[1]; cb = gc[2];
                      emStr *= starEmMul(); }
                    float mm[16];
                    composeBasis(mm, bru, toCam, bup, half, 1.0f, half, sprC);
                    const float bcf[4] = { cr, cg, cb, 1.0f };
                    const float emf[4] = { cr, cg, cb, emStr * nearFade };
                    x3::rhi::IRenderDevice::GlassMaterial gm{};
                    gm.opacity = 0.0f;            // pure additive glow (no see-through body)
                    gm.roughness = 1.0f; gm.specular = 0.0f; gm.refraction = 0.0f;
                    gm.tint[0]=cr; gm.tint[1]=cg; gm.tint[2]=cb;
                    device->drawMeshGlass(frame, glowDiscMesh, x3::rhi::TextureHandle{}, bcf, emf, gm, mm,
                                          /*alphaBlend=*/true);
                };
                // GLARE HALO (big, soft, warm white-gold): a DENSE stack of pure-additive
                // camera-facing discs (opacity 0 => no lit-diffuse brown, flat => no
                // grazing-limb ring — the two failure modes sphere shells had). MANY discs
                // with a smooth exponential emissive falloff so they blend into one soft
                // radial wash that fades to black within ~2.5x the disc (a glare that
                // dominates near the sun, not a flat brown fill of the whole frame).
                {
                    const int kHalo = 44;
                    for (int i = 0; i < kHalo; ++i) {
                        const float t = (float)i / (float)(kHalo - 1);       // 0 outer .. 1 inner
                        // v5b (owner: "the glare halo reads BROWN/MUDDY — it darkens the
                        // sky"). A BIG soft halo over black space is fatal: every disc in
                        // its dim mid-region adds a low warm value that reads as brown mud,
                        // never as light. Fix = a TIGHT, BRIGHT corona hugging the disc:
                        // pull the outer reach in from 2.3x -> 1.5x and use a STEEP cubic
                        // ramp so the outer discs are ~0 (true transparent, no smudge) and
                        // the brightness is concentrated near the limb where it reads as
                        // luminous gold AIR, not a dim brown wash spread across the frame.
                        const float frac  = 1.5f - t * (1.5f - 0.55f);       // 1.5x outer -> meets the hotspot
                        const float ramp  = t * t * t;                       // steep: outer ~0, bright near-disc
                        const float emStr = 1.7f * ramp;
                        // warm gold outer -> warm-white inner; blue LIFTED so the glow
                        // reads as bright gold, never the muddy orange-brown a low-blue
                        // dim mix produced.
                        glowDisc(frac, emStr, 1.0f, 0.80f + 0.16f*t, 0.54f + 0.28f*t);
                    }
                }
                // CENTER HOTSPOT (small, blinding): graduated discs from ~0.60x down to
                // ~0.06x, emissive ramping WELL past the 0.92 bloom threshold at the
                // centre so the core of the disc blows to white-hot, grading gold — the
                // limb (outside these) keeps its granulation + orange.
                {
                    const int kHot = 16;
                    for (int i = 0; i < kHot; ++i) {
                        const float t = (float)i / (float)(kHot - 1);        // 0 outer .. 1 inner
                        // v5b: SPREAD the hotspot glow across nearly the WHOLE disc (outer
                        // disc ~0.95x) with a gentle cubic ramp, so a gold wash feathers
                        // all the way to the limb and there is NO dark annulus between the
                        // blinding white centre and the granulated limb (owner: "dark
                        // granulated band" ringing the core). The centre still blows to
                        // white; the wash only lifts the mid/limb into continuous gold.
                        const float frac = 1.02f - t * (1.02f - 0.05f);      // reach the limb so no dark edge band
                        const float emStr = 0.46f + 3.8f * t * t * t;        // wide soft gold wash -> blinding centre
                        // gold at the rim -> white at the very centre
                        glowDisc(frac, emStr, 1.0f, 0.86f + 0.14f*t, 0.60f + 0.40f*t);
                    }
                }
            }
        };

        // ---- PLASMA FLARES / PROMINENCES: kFlareCount limb arcs, each riding its
        //      own sin(pi*t) rise->arch->collapse->fade envelope (t = phase-shifted
        //      elapsed time / period). White-gold roots (surface feet) fading to
        //      deep-orange tips (the arc's apex) — small emissive spheres chained
        //      along the loop (reuses dustMesh; no new mesh). --------------------
        auto drawFlares = [&](const x3::rhi::FrameContext& frame) {
            constexpr int kSegs = 6;
            for (int i = 0; i < kFlareCount; ++i) {
                const SunFlare& fl = flares[i];
                float t = std::fmod(g_clock + fl.phase, fl.period) / fl.period;
                if (t < 0.0f) t += 1.0f;
                const float env = std::max(0.0f, std::pow(std::sin(3.14159265f * t), 1.15f));
                if (env < 0.02f) continue;
                for (int s = 0; s <= kSegs; ++s) {
                    const float u = (float)s / (float)kSegs;
                    const x3::phys::Vec3 baseline{
                        fl.base0.x + (fl.base1.x - fl.base0.x) * u,
                        fl.base0.y + (fl.base1.y - fl.base0.y) * u,
                        fl.base0.z + (fl.base1.z - fl.base0.z) * u };
                    const x3::phys::Vec3 bn = vnorm(x3::phys::Vec3{
                        baseline.x - kSunCenter.x, baseline.y - kSunCenter.y, baseline.z - kSunCenter.z });
                    const float archShape = std::sin(3.14159265f * u);   // 0 at both feet, 1 at the apex
                    const float h = fl.maxH * env * archShape;
                    const x3::phys::Vec3 p{ baseline.x + bn.x*h, baseline.y + bn.y*h, baseline.z + bn.z*h };
                    // White-gold roots -> deep-orange tip as the arc rises off the surface.
                    const float r0 = 1.0f, g0 = 0.93f - 0.48f*archShape, b0 = 0.75f - 0.63f*archShape;
                    float mm[16]; sphereMatrix(p, fl.maxH * 0.22f, mm);
                    const float bc[4] = { r0, g0, b0, 1.0f };
                    const float em[4] = { r0, g0, b0, 2.2f * env };
                    device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, bc, em, mm);
                }
            }
        };

        // ---- INSIDE-THE-SUN: swirling plasma dome (deliverable B) ----------------
        // Wraps the camera in churning plasma while the ship is inside the core. Two
        // counter-panning inverted domes anchored at `camC`; the granulation bake is
        // the emissive map (near-black albedo so the lit term never washes it flat).
        // UVs slide every frame (updateMesh) — layer A one way, layer B faster the
        // other — so the plasma boils. Bright warm white-orange; the molten wash +
        // shield countdown draw OVER it (HUD/cinematic passes) and stay readable.
        auto drawInterior = [&](const x3::rhi::FrameContext& frame, const x3::phys::Vec3& camC) {
            // Pan offsets: u swirls (longitude), v drifts a touch (latitude). Layer B
            // counter-rotates ~1.7x faster for a churning, non-locked parallax.
            // TILE the granulation several times around each dome so many small
            // plasma cells show (a single wrap over a 300 m dome = huge soft blobs).
            // Different tiling per layer adds cross-scale parallax; pan swirls them.
            const float tAu = 4.0f, tAv = 3.0f, tBu = 2.5f, tBv = 2.0f;
            const float uA =  g_clock * 0.035f, vA = std::sin(g_clock * 0.11f) * 0.05f;
            const float uB = -g_clock * 0.060f, vB = std::sin(g_clock * 0.09f + 2.1f) * 0.06f;
            for (size_t i = 0; i < plasmaVA.size(); ++i) {
                plasmaVA[i].uv[0] = plasmaA.verts[i].uv[0] * tAu + uA;
                plasmaVA[i].uv[1] = plasmaA.verts[i].uv[1] * tAv + vA;
            }
            for (size_t i = 0; i < plasmaVB.size(); ++i) {
                plasmaVB[i].uv[0] = plasmaB.verts[i].uv[0] * tBu + uB;
                plasmaVB[i].uv[1] = plasmaB.verts[i].uv[1] * tBv + vB;
            }
            device->updateMesh(plasmaMeshA, plasmaVA.data(), (uint32_t)plasmaVA.size());
            device->updateMesh(plasmaMeshB, plasmaVB.data(), (uint32_t)plasmaVB.size());
            float m[16];
            // Layer A: opaque warm plasma backdrop, radius 320 m around the camera.
            sphereMatrix(camC, 320.0f, m);
            const float aBc[4] = { 0.06f, 0.03f, 0.01f, 1.0f };           // near-black albedo
            const float aEm[4] = { 1.0f, 0.60f, 0.26f, 2.3f };            // warm white-orange glow
            device->drawMeshPBR(frame, plasmaMeshA, sunTex, x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                aBc, aEm, m, /*alphaMask=*/false, /*alphaBlend=*/false, sunTex);
            // Layer B: smaller additive churn shell (glass, BLEND partition), radius
            // 250 m, counter-rotating, hotter/whiter — reads as roiling flame fronts.
            sphereMatrix(camC, 250.0f, m);
            const float bBc[4] = { 1.0f, 0.82f, 0.5f, 1.0f };
            const float bEm[4] = { 1.0f, 0.78f, 0.42f, 1.7f };
            x3::rhi::IRenderDevice::GlassMaterial gm{};
            gm.opacity = 0.5f; gm.roughness = 1.0f; gm.specular = 0.0f;
            gm.tint[0] = 1.0f; gm.tint[1] = 0.8f; gm.tint[2] = 0.5f;
            device->drawMeshGlass(frame, plasmaMeshB, sunTex, bBc, bEm, gm, m, /*alphaBlend=*/true);
        };

        // ---- Dynamic point lights: player-key (follows ship) + sun-heat ---------
        auto updateDynamicLights = [&](const x3::phys::Vec3& sPos, const x3::phys::Vec3& f,
                                       const x3::phys::Vec3& u, const x3::phys::Vec3& r) {
            // Restore the deep-space ambient floor + sky the transit turns off.
            // The floor is the LOCAL STAR'S ambient, not a constant.
            device->setAmbient(ambientRGB[0], ambientRGB[1], ambientRGB[2]);
            device->setSkyParams(skyP);
            // [0..2] THE STAR'S LIGHT. This is the claim "you arrived somewhere else"
            // stands or falls on: the key/fill/rim rig is what SHADES THE HULL, so a
            // different star has to change these, not just the skybox. Same authored
            // intensities and positions; the chroma and the luminosity ratio come from
            // the star. Identity until a transit lands (starChanged == false).
            {
                const float base[3][3] = {
                    { baseKey[0],  baseKey[1],  baseKey[2]  },
                    { baseFill[0], baseFill[1], baseFill[2] },
                    { baseRim[0],  baseRim[1],  baseRim[2]  },
                };
                for (int li = 0; li < 3; ++li) {
                    if (!starChanged) {
                        plights[li].color[0] = base[li][0];
                        plights[li].color[1] = base[li][1];
                        plights[li].color[2] = base[li][2];
                        continue;
                    }
                    // Preserve each light's authored INTENSITY (its max channel) and
                    // re-chromatise: the rig keeps its exposure, the scene changes hue.
                    const float mx = std::max(base[li][0], std::max(base[li][1], base[li][2]));
                    float c[3]; starCol(base[li][0] / std::max(1e-4f, mx),
                                        base[li][1] / std::max(1e-4f, mx),
                                        base[li][2] / std::max(1e-4f, mx), c);
                    const float gainL = std::min(1.7f, std::max(0.40f, starLumRatio));
                    plights[li].color[0] = c[0] * mx * gainL;
                    plights[li].color[1] = c[1] * mx * gainL;
                    plights[li].color[2] = c[2] * mx * gainL;
                }
            }
            // [3] PLAYER KEY — offset up + behind + to the camera side so the hull
            // silhouette reads while flying. Subtle (lit metal, not a floodlight).
            const x3::phys::Vec3 kp{ sPos.x + u.x*6.0f - f.x*4.0f + r.x*5.0f,
                                     sPos.y + u.y*6.0f - f.y*4.0f + r.y*5.0f,
                                     sPos.z + u.z*6.0f - f.z*4.0f + r.z*5.0f };
            plights[3].pos[0]=kp.x; plights[3].pos[1]=kp.y; plights[3].pos[2]=kp.z;
            plights[3].range = 42.0f;
            plights[3].color[0]=16.0f; plights[3].color[1]=17.0f; plights[3].color[2]=22.0f;
            // [4] SUN HEAT — warm light between ship and sun that ramps up only within
            // kLightDist of the surface (far-away look unchanged), so hulls/cockpit
            // visibly heat-glow on the dive. Independent smoothstep on surface range.
            const float lightRamp = smooth01(kLightDist, kSunRadius, g_sunSurf);
            const x3::phys::Vec3 hp{ sPos.x + kSunDir.x*30.0f,
                                     sPos.y + kSunDir.y*30.0f,
                                     sPos.z + kSunDir.z*30.0f };
            const float hi = lightRamp * lightRamp * 110.0f;
            plights[4].pos[0]=hp.x; plights[4].pos[1]=hp.y; plights[4].pos[2]=hp.z;
            plights[4].range = 120.0f;
            plights[4].color[0]=hi*1.0f; plights[4].color[1]=hi*0.45f; plights[4].color[2]=hi*0.12f;
            // [5..] THE WORMHOLE SPILL. This is the claim the whole effect stands
            // or falls on: the wormhole is not merely bright, it LIGHTS things.
            // Its lights go into the same rig as the key/fill/rim, so the same
            // mesh.frag point-light loop that shades the hull sees them — which
            // means the spill reaches the ship's PBR material by exactly the path
            // every other light in this world already takes. Bounded by
            // collectLights (never more than kMaxWormholeLights), so the count
            // cannot run away as wormholes are added.
            const int wn = wormholes.collectLights(&plights[kStaticLights],
                                                   kTotalLights - kStaticLights);
            for (int i = kStaticLights + wn; i < kTotalLights; ++i) plights[i] = {};
            liveLightCount = kStaticLights + wn;
            device->setPointLights(plights, (uint32_t)liveLightCount);
        };

        // ---- feat/wormholes: one tick + one draw, shared by BOTH loops -------
        // Defined once and called from the headless capture path and the windowed
        // loop alike, so a capture cannot diverge from what the player sees. That
        // divergence is exactly how a "staged event" ends up judged from a still
        // that the game never actually renders.

        // TICK. dt-correct end to end: the phase machine drains `fdt`, the comms
        // advisory runs on a wall-clock accumulator rather than a frame count, and
        // the transit clock is integrated, never incremented.
        auto tickWormholes = [&](float fdt, const x3::phys::Vec3& shipPos) {
            wormholes.update(fdt);

            // THE COMMS HOOK. feat/ship-comms left exactly this seam: build the
            // rows, publish them with the player's eye, and the AEGIS director
            // does the proximity test and names the stability out loud. One call.
            // Duty-cycled to ~10 Hz because the advisory is edge-triggered with
            // hysteresis — publishing at 165 Hz would be 16x the work for the
            // identical result.
            commsPublishAcc += fdt;
            if (commsPublishAcc >= 0.1f) {
                commsPublishAcc = 0.0f;
                const float eye[3] = { shipPos.x, shipPos.y, shipPos.z };
                wormholes.publishToComms(eye);
            }

            // THE TRANSIT. Flying into an open throat engages the S0 SpaceLayer
            // spine; the S3 WormholeTransit runner drives it to completion and the
            // ship ARRIVES somewhere else. Entering one means something.
            if (!transitEngaged) {
                const float p[3] = { shipPos.x, shipPos.y, shipPos.z };
                const int hit = wormholes.entered(p);
                if (hit >= 0) {
                    transitEngaged = true;
                    transitFrom    = hit;
                    transitClock   = 0.0f;
                    // Tell the transit WHERE IT IS GOING before arming the spine.
                    transit.begin(planFor(wormholes.at(hit)));
                    spaceLayer.requestWormhole((uint32_t)wormholes.at(hit).id());
                    // Log the plan we just STAGED, not transit.plan() - the runner
                    // latches the staged plan on its first tick (a jump in flight
                    // cannot be retargeted), so plan() here is still the old one.
                    x3::logInfo(std::string("--world space: WORMHOLE TRANSIT engaged -> ") +
                                wormholes.at(hit).name() + " -> " +
                                planFor(wormholes.at(hit)).toSystemId);
                }
            } else {
                transitClock += fdt;
                spaceLayer.update(fdt);
            }

            // AEGIS narrates the jump. The transit AUTHORS the lines (departure,
            // mid-corridor, the extra collapse warning an unstable hole earns, and
            // arrival); the host only relays them onto the bus, so the windowed loop
            // and the headless capture path cannot drift apart on what was said.
            {
                x3::space::TransitCommsLine cl;
                while (transit.popComms(cl)) {
                    x3::game::commsBus().post(x3::game::CommsSender::ShipAI,
                                              x3::game::kCommsShipAiName, cl.text);
                    x3::logInfo(std::string("[AEGIS] ") + cl.text);
                }
            }

            // ARRIVAL. The ride is over and the ship is somewhere new. pilot.spawn()
            // is the same public reposition the Respawn beat already uses, so no
            // space_pilot internals are touched -- but it takes a Tuning, and passing
            // the DEFAULT one (as this did) silently reset the player's flight feel
            // on every jump. Carry the mode and the tuning across.
            if (transit.arrivalPending()) {
                transit.consumeArrival();
                transitEngaged = false;
                const bool aborted = transit.lastArrivalAborted();
                const x3::game::FlightMode keepMode = pilot.mode();
                const x3::game::SpacePilotController::Tuning keepTune = pilot.tuning();
                if (aborted) {
                    // An aborted transit must leave the player in a VALID world, not
                    // in limbo: spat back out beside the mouth they entered, in the
                    // system they departed from, with the sky untouched.
                    float bx = 40.0f, by = 30.0f, bz = 40.0f;
                    if (transitFrom >= 0) {
                        const float* wp = wormholes.at(transitFrom).pos();
                        bx = wp[0] - 90.0f; by = wp[1] + 26.0f; bz = wp[2] - 90.0f;
                    }
                    pilot.spawn(*sphys, bx, by, bz, keepTune);
                    x3::logInfo("--world space: WORMHOLE TRANSIT ABORTED - "
                                "returned to the departure side");
                } else {
                    // THE JOURNEY LANDS. Re-dress the local star (colour, luminosity,
                    // and the key light that actually shades the hull) and the
                    // starfield for the arrival system, then put the ship down a long
                    // way from where it left.
                    applySystem(transit.plan().toSystemId);
                    pilot.spawn(*sphys, -1420.0f, 260.0f, 1180.0f, keepTune);
                    // The corridor you came through does not follow you: close the
                    // field behind the jump so the arrival sky belongs to the new
                    // system rather than carrying the departure system's furniture.
                    for (int wi = 0; wi < wormholes.count(); ++wi) wormholes.at(wi).close();
                }
                pilot.setMode(keepMode);
                transitFrom = -1;
            }
        };

        // DRAW. The wormholes themselves always; the crystal-matrix throat rushing
        // past on top of them while a transit is running.
        auto drawWormholes = [&](const x3::rhi::FrameContext& frame,
                                 const x3::phys::Vec3& eyePos, float tSec) {
            const float eye[3] = { eyePos.x, eyePos.y, eyePos.z };
            wormholes.render(*device, frame, eye);
            (void)tSec;   // the transit owns its own frame now (drawTransitFrame)
        };

        // =====================================================================
        // THE RIDE. Everything below is what turns the transit from a fade into a
        // journey you are INSIDE of.
        //
        // Old behaviour, for the record: the tunnel mesh was drawn ONCE, at the
        // WORLD ORIGIN, along +Z, with setOrigin() never called and the camera
        // still on the player's chase cam somewhere else entirely. The player kept
        // full control of a ship that was nowhere near the tube. Nothing about it
        // was a ride.
        //
        // Now: the camera and the ship are placed on the tunnel axis at a dedicated
        // anchor, the throat is assembled AROUND them out of scrolling multi-shell
        // copies, the tunnel's own light rig lights the hull (you are inside the
        // source, so the spill is the dominant light in frame), and the streak
        // layer runs at full stretch. The world scene is NOT drawn: for the length
        // of the jump, the throat is the entire frame.
        // =====================================================================

        // Where the camera and the ship sit during a transit, and how wide the lens
        // is. The camera stays ON the axis and the TUNNEL banks around it (see
        // WormholeTransit::bankX/bankY), which is the read the brief asked for:
        // the tunnel rolls around the ship rather than the ship wandering the tube.
        auto transitCamera = [&](float& cx, float& cy, float& cz,
                                 float& cyaw, float& cpit, float& cfov) {
            const float el = transit.elapsed();
            cx = kTransitAnchor.x;
            cy = kTransitAnchor.y;
            cz = kTransitAnchor.z;
            // A very small drift so the shot is never mechanically locked. Kept
            // tiny: big camera moves inside a tunnel read as nausea, not speed.
            cyaw = kTransitYaw + 0.026f * std::sin(el * 0.47f);
            cpit = 0.030f * std::sin(el * 0.83f + 1.3f);
            // FOV PUNCHES WIDE under acceleration. The pilot's own fovBase/fovMax
            // are the vocabulary here (space_pilot.h) - read, not fought: the ride
            // starts from the pilot's resting FOV and pushes past its speed ceiling,
            // because this is faster than the ship can fly.
            const float fovBase = pilot.tuning().fovBase;
            cfov = fovBase + transit.fovPunchDeg();
        };

        // The ship, riding the corridor ahead of the camera. It BANKS with the
        // tunnel but lags it (secondary motion trailing primary), and rolls on its
        // own slower rhythm, so hull and walls never move in lockstep.
        auto transitShipMatrix = [&](float m[16]) {
            const float el = transit.elapsed();
            const float ride = transit.ride01();
            // Ship roll: its own period, and it LAGS the tunnel's bank.
            const float br = (0.42f * std::sin(el * 0.58f) +
                              0.19f * std::sin(el * 1.29f + 1.2f)) * ride;
            // Nose drift, small, on yet another period.
            const x3::phys::Vec3 f = vnorm(x3::phys::Vec3{
                0.055f * std::sin(el * 0.91f), 0.042f * std::sin(el * 0.68f + 1.0f), 1.0f });
            const x3::phys::Vec3 upRef{ std::sin(br), std::cos(br), 0.0f };
            const x3::phys::Vec3 r = vnorm(vcross(f, upRef));
            const x3::phys::Vec3 u = vnorm(vcross(r, f));
            // Sit the hull down-and-ahead of the eye so the convergence point is
            // visible past it: the tunnel needs a readable CENTRE, and the ship
            // silhouetted against that centre is the composition.
            const float lag = 0.55f;   // the hull trails the tunnel's swing
            const x3::phys::Vec3 pos{
                kTransitAnchor.x + transit.bankX() * lag,
                kTransitAnchor.y + transit.bankY() * lag - 2.6f,
                kTransitAnchor.z + 16.0f
            };
            shipMatrix(pos, f, u, r, m);
        };

        // THE TUNNEL'S LIGHT. The exterior lane's headline claim was that a wormhole
        // does not merely glow, it LIGHTS things (its spill measured +7.41% on hull
        // pixels). Inside the throat you are surrounded by the source, so the spill
        // must be the DOMINANT light in frame, not a rim accent: eight lights on a
        // ring at the wall radius, plus one hot light at the convergence ahead, all
        // riding the ride curve and the corridor's spectrum. They go into the same
        // plights rig every other light in this world uses, so they reach the hull's
        // PBR material by exactly the same path.
        auto transitLights = [&]() {
            // KILL THE AMBIENT FLOOR. Deep space here runs a cool ambient so hulls
            // do not render as silhouettes against black - but inside the throat the
            // walls fill the frame, and an ambient floor multiplies into every one
            // of them, which is a uniform lift over the whole image by another name.
            // Inside the tunnel the LIGHT COMES FROM THE TUNNEL: the ring rig below
            // is the light source, so the floor goes away and the dark lanes return.
            device->setAmbient(0.012f, 0.014f, 0.022f);
            // KILL THE SKY, TOO. `skyP.sunIntensity` is a DIRECTIONAL key that
            // shades every mesh in the frame - including the tunnel walls, which
            // are drawn through the LIT emissive path. Inside the throat there is
            // no sky to see and no star to be lit by, so leaving the key on just
            // adds a broad flat term across every wall facing it. This was the
            // single biggest contributor to the first pass reading as a white sheet.
            { x3::rhi::IRenderDevice::SkyParams off = skyP; off.enabled = false;
              off.sunIntensity = 0.0f; device->setSkyParams(off); }
            const float el   = transit.elapsed();
            const float ride = transit.ride01();
            const int kRing = 8;
            int n = 0;
            // Salvari blue-violet, pushed toward violet on an unstable corridor.
            const bool unst = !transit.plan().stable;
            const float cr = unst ? 0.78f : 0.42f;
            const float cg = unst ? 0.34f : 0.62f;
            const float cb = 1.00f;
            for (int i = 0; i < kRing && n < kTotalLights; ++i) {
                // The ring ROTATES with the throat, so the highlights sweep around
                // the hull instead of sitting still on it.
                const float ang = (float)i / (float)kRing * 6.2831853f + transit.rollRad();
                // TIGHT to the ship, NOT out at the wall. The first rig put the ring
                // at 9.5 m, which sat closer to the tube shells than to the hull: the
                // walls go through drawMeshEmissive (a LIT path - albedo x lighting
                // PLUS the HDR base term), so a bright ring out there floodlit the
                // tunnel itself and washed the frame to a flat lavender sheet. The
                // ring belongs around the SHIP; the walls carry their own glow.
                const float rad = 5.6f;
                // Each light also slides along the axis at its own rate, so the hull
                // reads a travelling wash rather than a static ring.
                const float za = 8.0f + 22.0f * (0.5f + 0.5f * std::sin(el * 1.7f + (float)i));
                x3::rhi::PointLight& L = plights[n++];
                L = {};
                L.pos[0] = kTransitAnchor.x + std::cos(ang) * rad;
                L.pos[1] = kTransitAnchor.y + std::sin(ang) * rad;
                L.pos[2] = kTransitAnchor.z + za;
                L.range  = 26.0f;   // reaches the hull, dies well short of the wall
                // Per-light flicker on its own phase: the walls are not a lamp.
                const float fl = 0.72f + 0.28f * std::sin(el * 3.1f + (float)i * 1.9f);
                const float I  = 52.0f * ride * fl;
                L.color[0] = cr * I; L.color[1] = cg * I; L.color[2] = cb * I;
            }
            if (n < kTotalLights) {
                // THE CONVERGENCE. The bright point down the axis: the composition's
                // centre, and a hard key that rims the hull from dead ahead.
                x3::rhi::PointLight& L = plights[n++];
                L = {};
                L.pos[0] = kTransitAnchor.x;
                L.pos[1] = kTransitAnchor.y;
                L.pos[2] = kTransitAnchor.z + 150.0f;
                L.range  = 220.0f;
                const float I = 55.0f * ride;
                L.color[0] = 0.86f * I; L.color[1] = 0.90f * I; L.color[2] = 1.00f * I;
            }
            for (int i = n; i < kTotalLights; ++i) plights[i] = {};
            device->setPointLights(plights, (uint32_t)n);
        };

        // Near-field specks streaming down the corridor. This REUSES the host's
        // existing sense-of-speed machinery (dustMesh + the 3-segment comet taper
        // that fakes a bright-head/faint-tail ramp), driven off the transit's ride
        // curve instead of the pilot's speed, rather than standing up a second
        // streak system that would inevitably look like a different effect.
        auto drawTransitStreaks = [&](const x3::rhi::FrameContext& frame) {
            const float ride = transit.streakDrive();
            if (ride < 0.02f) return;
            const x3::phys::Vec3 vd{ 0.0f, 0.0f, -1.0f };   // toward the camera
            const x3::phys::Vec3 uax{ 1.0f, 0.0f, 0.0f };
            const x3::phys::Vec3 vax{ 0.0f, 1.0f, 0.0f };
            const float segOff[3] = { -0.55f, 0.0f, 0.55f };
            const float segThk[3] = {  1.00f, 0.72f, 0.50f };
            const float segStr[3] = {  1.00f, 0.55f, 0.28f };
            const int   kN = 190;
            const float span = 260.0f;        // axial length of the speck field
            for (int i = 0; i < kN; ++i) {
                const float h1 = hashF((uint32_t)(i * 3 + 11));
                const float h2 = hashF((uint32_t)(i * 5 + 23));
                const float h3 = hashF((uint32_t)(i * 7 + 37));
                // Specks live in an annulus between the ship and the wall, so they
                // read as material rushing past rather than as stars on the surface.
                const float ang = h1 * 6.2831853f + transit.rollRad() * 0.35f;
                const float rad = 2.6f + h2 * 8.2f;
                // Scroll: each speck rides the SAME integrated axial distance the
                // walls do (so nothing drifts out of sync), at its own rate.
                const float rate = 1.4f + h3 * 2.2f;
                float z = span * h3 + 40.0f - std::fmod(transit.axialDistance() * rate, span);
                while (z < 18.0f) z += span;   // always AHEAD: a speck beside the eye
                                               // reads as a scratch across the frame,
                                               // not as something rushing past
                const x3::phys::Vec3 c{
                    kTransitAnchor.x + std::cos(ang) * rad + transit.bankX() * 0.4f,
                    kTransitAnchor.y + std::sin(ang) * rad + transit.bankY() * 0.4f,
                    kTransitAnchor.z + z };
                // Fade at both ends of the field: nothing pops into or out of being.
                const float fadeIn  = smooth01(16.0f, 46.0f, z);
                const float fadeOut = 1.0f - smooth01(span * 0.78f, span, z);
                const float fade = fadeIn * fadeOut;
                if (fade < 0.02f) continue;
                // Kept SHORT and THIN. The first interior pass ran these at 7.5 m
                // half-length and they became bright bars across the whole frame,
                // which is most of what buried the tunnel structure underneath.
                const float halfLen = 0.5f + 3.6f * ride * (0.5f + h1);
                const float thick   = 0.022f + 0.038f * h2;
                // Cool blue-white, with a violet minority - the corridor's palette.
                const float base[4] = { (h2 < 0.75f) ? 0.72f : 0.86f,
                                        (h2 < 0.75f) ? 0.86f : 0.62f,
                                        1.0f, 1.0f };
                const float S = (0.10f + 0.52f * ride) * (0.35f + 0.9f * h1) * fade;
                for (int sg = 0; sg < 3; ++sg) {
                    const float str = S * segStr[sg];
                    if (str < 0.02f) continue;
                    const x3::phys::Vec3 cc{ c.x, c.y, c.z + halfLen * segOff[sg] };
                    float m[16];
                    composeBasis(m, uax, vax, vd, thick * segThk[sg], thick * segThk[sg],
                                 std::max(thick, halfLen * 0.45f), cc);
                    const float emis[4] = { base[0], base[1], base[2], str };
                    device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, base, emis, m);
                }
            }
        };

        // THE CONVERGENCE. A tunnel seen from inside, on its axis, is BRIGHTEST at
        // the periphery and darkest at the vanishing point - the wall normals face
        // the camera at the edge of frame and graze it at the centre. That is what a
        // cylinder does, and it is the exact inverse of the composition a tunnel
        // shot needs: without something AT the vanishing point the middle of frame
        // is a hole, and the eye has nothing to fly toward.
        //
        // So the corridor gets a real convergence: a stack of concentric additive
        // glow spheres far down the axis, blinding white at the centre and grading
        // out through the corridor's blue. This is the SAME technique the star's
        // hotspot uses (graduated additive discs whose contributions stack, most
        // overlapping dead-centre) - a proven radial gradient built out of geometry,
        // because the glass path has no emissive map to shape one with.
        auto drawTransitCore = [&](const x3::rhi::FrameContext& frame) {
            const float ride = transit.ride01();
            if (ride < 0.02f) return;
            const float zc   = kTransitAnchor.z + 760.0f;
            // The core BREATHES with the ride and flares as the jump completes.
            // Bounded. The core is the brightest thing in the frame and it is meant
            // to bloom - but a region that reaches PURE WHITE has no gradient inside
            // it and reads as cheap. Measured at the first tuning: 0.0215% of pixels
            // were exactly 255/255/255. Held under the knee it is 0.0000% with the
            // falloff still visible all the way in.
            const float flare = std::min(1.55f,
                0.45f + 0.55f * ride + 0.55f * transit.progress() * transit.progress());
            const int   kL = 14;
            for (int i = 0; i < kL; ++i) {
                const float u = (float)i / (float)(kL - 1);          // 0 outer .. 1 inner
                const float rad = 210.0f - u * (210.0f - 9.0f);
                // Steep ramp: the outer shells are almost nothing (no brown smudge
                // spread across the frame - the star's halo learned that the hard
                // way), the brightness concentrates into the last few degrees.
                const float ramp = u * u * u;
                // NB the stack SUMS at the centre (all 14 shells overlap there), so
                // the centre value is ~sum(str), not max(str). Tuned so that sum
                // sits under the tonemapper's knee after auto-exposure adapts.
                const float str  = (0.030f + 0.40f * ramp) * flare;
                if (str < 0.02f) continue;
                // Blue at the rim -> white-hot at the centre.
                const float cr = 0.42f + 0.58f * u;
                const float cg = 0.58f + 0.42f * u;
                const float cb = 1.0f;
                float m[16];
                sphereMatrix(x3::phys::Vec3{ kTransitAnchor.x + transit.bankX() * 1.6f,
                                             kTransitAnchor.y + transit.bankY() * 1.6f, zc },
                             rad, m);
                const float bc[4] = { cr, cg, cb, 1.0f };
                const float em[4] = { cr, cg, cb, str };
                x3::rhi::IRenderDevice::GlassMaterial gm{};
                gm.opacity = 0.0f;            // pure additive glow, no see-through body
                gm.roughness = 1.0f; gm.specular = 0.0f; gm.refraction = 0.0f;
                gm.additive  = 0.05f;         // flat: no grazing-limb ring
                gm.tint[0]=cr; gm.tint[1]=cg; gm.tint[2]=cb;
                device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{}, bc, em, gm, m,
                                      /*alphaBlend=*/true);
            }
        };

        // The whole transit frame: walls, streaks, ship. Called INSTEAD of the world
        // draw, from both the windowed loop and the headless capture path, so a
        // capture cannot show a ride the player never gets.
        auto drawTransitWorld = [&](const x3::rhi::FrameContext& frame, float tSec) {
            const float cam[3] = { kTransitAnchor.x, kTransitAnchor.y, kTransitAnchor.z };
            transit.renderTunnel(*device, frame, cam, tSec);
            drawTransitCore(frame);
            drawTransitStreaks(frame);
            float sm[16]; transitShipMatrix(sm);
            drawShipAt(frame, sm, 1.5f);
        };

        // ---- THE INSTRUMENTS OFF THEIR SCALES ------------------------------
        // The detail the owner named, and the one that sells the shot: the readouts
        // tell the player the game KNOWS this is not normal flight. Same HUD path as
        // the flight telemetry (drawHudTextF / drawHudQuad), same corner, so it
        // reads as the ship's own instruments failing to cope rather than as a
        // separate title card.
        //
        // OVERFLOW IS LITERAL. Each value is formatted into a FIXED-WIDTH field; if
        // the number does not fit, the field is filled with '#' and tagged OVR -
        // which is what an instrument with no scale left actually does.
        auto drawTransitHud = [&](const x3::rhi::FrameContext& frame, float W, float H) {
            using x3::rhi::FontRole;
            const float wash = transit.membraneWash01();
            // MEMBRANE WASH at the two crossings, in the corridor's own colour.
            if (wash > 0.002f) {
                const bool unst = !transit.plan().stable;
                const float c[4] = { unst ? 0.62f : 0.42f,
                                     unst ? 0.30f : 0.64f, 1.0f, wash };
                device->drawHudQuad(frame, 0, 0, W, H, c);
            }

            const bool  unst = !transit.plan().stable;
            const float acc[4]  = { unst ? 1.00f : 0.42f, unst ? 0.42f : 0.86f, 1.0f, 1.0f };
            const float dim[4]  = { acc[0]*0.75f, acc[1]*0.75f, acc[2]*0.85f, 0.92f };
            const float bg[4]   = { 0.02f, 0.02f, 0.05f, 0.55f };

            // Banner: what is happening and where it is meant to end.
            {
                char t[96];
                std::snprintf(t, sizeof(t), "TRANSLATION  %s", transit.plan().corridorName);
                const float px = 30.0f;
                const float tw = device->textAdvance(FontRole::Title, t, px);
                const float tx = W * 0.5f - tw * 0.5f;
                device->drawHudQuad(frame, tx - 18.0f, 16.0f, tw + 36.0f, px + 26.0f, bg);
                device->drawHudTextF(frame, FontRole::Title, t, tx, 24.0f, px, acc);
                char sub[128];
                std::snprintf(sub, sizeof(sub), "%s  ->  %s      AUTOPILOT HAS THE HELM",
                              transit.plan().fromSystem, transit.plan().toSystem);
                const float sp = 14.0f;
                const float sw = device->textAdvance(FontRole::HudMono, sub, sp);
                device->drawHudTextF(frame, FontRole::HudMono, sub,
                                     W * 0.5f - sw * 0.5f, 24.0f + px + 6.0f, sp, dim);
            }

            // Fixed-width field with real overflow behaviour.
            auto field = [](char* out, size_t n, double v, int width, const char* unit) {
                char num[64];
                std::snprintf(num, sizeof(num), "%.0f", v);
                if ((int)std::strlen(num) > width) {
                    // No scale left. Fill the field and say so.
                    for (int i = 0; i < width; ++i) num[i] = '#';
                    num[width] = '\0';
                    std::snprintf(out, n, "%s %s OVR", num, unit);
                } else {
                    std::snprintf(out, n, "%*s %s", width, num, unit);
                }
            };

            double pos[3]; transit.readPos(pos);
            char l[6][96];
            char f1[64], f2[64], f3[64], f4[64];
            field(f1, sizeof(f1), pos[0], 9, "m");
            field(f2, sizeof(f2), pos[1], 9, "m");
            field(f3, sizeof(f3), pos[2], 9, "m");
            field(f4, sizeof(f4), transit.readSpeedMs(), 9, "m/s");
            std::snprintf(l[0], sizeof(l[0]), "POS X %s", f1);
            std::snprintf(l[1], sizeof(l[1]), "POS Y %s", f2);
            std::snprintf(l[2], sizeof(l[2]), "POS Z %s", f3);
            std::snprintf(l[3], sizeof(l[3]), "VEL   %s", f4);
            std::snprintf(l[4], sizeof(l[4]), "      %8.1f c", transit.readSpeedC());
            std::snprintf(l[5], sizeof(l[5]), "DIST  %8.4f ly   ETA %4.1fs",
                          (double)transit.distanceRemainLy(), (double)transit.etaSec());
            const float tpx = 17.0f;
            const float bx = 20.0f, by = H - 158.0f;
            const float tbg[4] = { 0.02f, 0.03f, 0.05f, 0.62f };
            device->drawHudQuad(frame, bx - 10.0f, by - 10.0f, 360.0f, 152.0f, tbg);
            for (int i = 0; i < 6; ++i) {
                // The three POS lines and VEL flash once they are past legibility -
                // the instrument admitting it cannot render the number.
                const bool over = (std::strstr(l[i], "OVR") != nullptr);
                float col[4] = { dim[0], dim[1], dim[2], 0.95f };
                if (over) {
                    const float fl = (std::sin(g_clock * 11.0f + (float)i) > 0.0f) ? 1.0f : 0.55f;
                    col[0] = 1.0f; col[1] = 0.55f; col[2] = 0.25f; col[3] = fl;
                }
                device->drawHudText(frame, l[i], bx, by + 21.0f * (float)i, tpx, col);
            }
            // Corridor-progress bar: the one readout that stays sane, so the player
            // can see the ride END even while everything else is off its scale.
            const float barW = 330.0f, barH = 6.0f, barY = by + 21.0f * 6.0f + 6.0f;
            const float bbg[4] = { 0.10f, 0.15f, 0.20f, 0.7f };
            device->drawHudQuad(frame, bx, barY, barW, barH, bbg);
            const float fill[4] = { acc[0], acc[1], acc[2], 0.95f };
            device->drawHudQuad(frame, bx, barY, barW * transit.progress(), barH, fill);
            // Stage tag, right of the bar.
            {
                char st[48];
                std::snprintf(st, sizeof(st), "%s", x3::space::transitStageName(transit.stage()));
                device->drawHudTextF(frame, FontRole::HudMono, st, bx + barW + 14.0f,
                                     barY - 6.0f, 15.0f, dim);
            }
        };

        // ---- PLAYER-SHIP lights: engine glow + red/green/white nav beacons ------
        // Anchored to the pilot basis (forward/right/up). The Minerva's wings run
        // along ±right() in world (model +X → -right in the compose above), so the
        // wingtip nav dots sit on the real wings. All emissive/render-only.
        auto drawShipLights = [&](const x3::rhi::FrameContext& frame, float thrust01,
                                  float blinkT) {
            const x3::phys::Vec3 p = pilot.pos();
            const x3::phys::Vec3 f = pilot.forward();
            const x3::phys::Vec3 r = pilot.right();
            const x3::phys::Vec3 u = pilot.up();
            auto dot = [&](const x3::phys::Vec3& c, float rad, float cr, float cg,
                           float cb, float str) {
                float m[16]; sphereMatrix(c, rad, m);
                const float bc[4] = { cr, cg, cb, 1.0f };
                const float em[4] = { cr, cg, cb, str };
                device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, bc, em, m);
            };
            // ENGINE GLOW: twin nozzles at the rear (−forward), brightness scales with
            // thrust/speed (+ boost handled by the caller folding it into thrust01).
            const float eStr = 0.8f + 4.5f * thrust01;
            const float eRad = 0.16f + 0.10f * thrust01;
            for (float side : { -1.0f, 1.0f }) {
                const x3::phys::Vec3 e{ p.x - f.x*1.4f + r.x*0.55f*side - u.x*0.1f,
                                        p.y - f.y*1.4f + r.y*0.55f*side - u.y*0.1f,
                                        p.z - f.z*1.4f + r.z*0.55f*side - u.z*0.1f };
                dot(e, eRad, 0.45f, 0.85f, 1.0f, eStr);   // cyan-white engine plume
            }
            // NAV/RUNNING lights at the wingtips: RED port (left = −right), GREEN
            // starboard (right = +right); WHITE tail beacon that blinks slowly.
            const float span = 1.6f, back = 0.6f;
            const x3::phys::Vec3 portTip{ p.x - r.x*span - f.x*back, p.y - r.y*span - f.y*back, p.z - r.z*span - f.z*back };
            const x3::phys::Vec3 stbdTip{ p.x + r.x*span - f.x*back, p.y + r.y*span - f.y*back, p.z + r.z*span - f.z*back };
            dot(portTip, 0.11f, 1.0f, 0.10f, 0.10f, 2.6f);   // RED  (port)
            dot(stbdTip, 0.11f, 0.10f, 1.0f, 0.20f, 2.6f);   // GREEN(starboard)
            // WHITE tail beacon — slow deterministic blink (on ~half the cycle).
            const float blink = (std::sin(blinkT * 3.2f) > 0.2f) ? 1.0f : 0.12f;
            const x3::phys::Vec3 tail{ p.x - f.x*1.7f + u.x*0.5f, p.y - f.y*1.7f + u.y*0.5f, p.z - f.z*1.7f + u.z*0.5f };
            dot(tail, 0.10f, 1.0f, 1.0f, 1.0f, 3.0f * blink);
        };

        // ---- SHIELD shell (engages when inside the sun body) --------------------
        // No engine shield-material path is reachable from this host, so the shield is
        // an additive translucent emissive sphere around the ship (drains with % ).
        auto drawShield = [&](const x3::rhi::FrameContext& frame, float pct, float pulseT) {
            const x3::phys::Vec3 p = pilot.pos();
            const float k = std::max(0.0f, pct) / 100.0f;
            const float pulse = 0.75f + 0.25f * std::sin(pulseT * 6.0f);
            float m[16]; sphereMatrix(p, 2.6f, m);
            const float bc[4] = { 0.35f, 0.75f, 1.0f, 1.0f };
            const float em[4] = { 0.35f, 0.75f, 1.0f, (0.6f + 2.2f * k) * pulse };
            x3::rhi::IRenderDevice::GlassMaterial gm{};
            gm.opacity = 0.18f + 0.22f * k; gm.roughness = 0.4f; gm.specular = 0.4f;
            gm.tint[0]=0.4f; gm.tint[1]=0.75f; gm.tint[2]=1.0f;
            device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{}, bc, em, gm, m);
        };

        // ---- FIERY ENTRY-BURN SHEATH: 3 slightly-scaled-up emissive glass shells
        //      hugging the hull (reuses sunMesh — no new mesh alloc — same additive-
        //      glass trick as the sun corona's BLEND-partition fix above), white-gold
        //      innermost -> orange -> deep red-orange outer, with a deterministic
        //      sum-of-sines flicker. `burn` is g_burnFactor (0..~1.5). -------------
        auto drawBurnSheath = [&](const x3::rhi::FrameContext& frame, const x3::phys::Vec3& p,
                                  const x3::phys::Vec3& camPos, float burn) {
            if (burn < 0.02f) return;
            const float flick = 0.85f + 0.10f * std::sin(g_clock * 11.0f) + 0.05f * std::sin(g_clock * 23.0f + 1.3f);
            const struct { float s, r, g, b, str; } shells[3] = {
                { 1.15f, 1.00f, 0.92f, 0.55f, 2.2f },   // white-gold innermost
                { 1.30f, 1.00f, 0.55f, 0.15f, 1.6f },   // orange
                { 1.50f, 0.85f, 0.20f, 0.05f, 1.0f },   // deep red-orange outer
            };
            const float camToShip = vlen(x3::phys::Vec3{ camPos.x-p.x, camPos.y-p.y, camPos.z-p.z });
            float m[16];
            for (const auto& sh : shells) {
                const float shR = 2.2f * sh.s;
                if (camToShip < shR * 1.05f) continue;   // bullseye fix: skip shells the camera sits inside (3P-close)
                sphereMatrix(p, shR, m);
                const float bc[4] = { sh.r, sh.g, sh.b, 1.0f };
                const float em[4] = { sh.r, sh.g, sh.b, sh.str * burn * flick };
                x3::rhi::IRenderDevice::GlassMaterial gm{};
                gm.opacity = 0.10f * burn; gm.roughness = 1.0f; gm.specular = 0.0f;
                gm.tint[0] = sh.r; gm.tint[1] = sh.g; gm.tint[2] = sh.b;
                device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{}, bc, em, gm, m,
                                      /*alphaBlend=*/true);   // BLEND partition: skip depth pre-pass
            }
        };
        // ---- Shedding-fire embers trailing AFT of the ship (opposite forward()),
        //      hash-seeded positions drifting slowly on g_clock, fading toward the
        //      tail — reuses dustMesh (no new mesh). Cheap deterministic accent to
        //      the sheath above; skipped entirely below a small burn threshold. ----
        auto drawBurnEmbers = [&](const x3::rhi::FrameContext& frame, const x3::phys::Vec3& p,
                                  const x3::phys::Vec3& f, const x3::phys::Vec3& u,
                                  const x3::phys::Vec3& r, float burn) {
            if (burn < 0.05f) return;
            constexpr int kEmbers = 8;
            for (int i = 0; i < kEmbers; ++i) {
                const float h1 = hashF((uint32_t)(i * 29 + 1)), h2 = hashF((uint32_t)(i * 29 + 2)),
                            h3 = hashF((uint32_t)(i * 29 + 3));
                const float back = 1.6f + 3.2f * h1 + 1.4f * std::sin(g_clock * 2.0f + (float)i * 1.7f);
                const float side = (h2 * 2.0f - 1.0f) * 0.9f;
                const float upOff = (h3 * 2.0f - 1.0f) * 0.7f;
                const x3::phys::Vec3 c{ p.x - f.x*back + r.x*side + u.x*upOff,
                                        p.y - f.y*back + r.y*side + u.y*upOff,
                                        p.z - f.z*back + r.z*side + u.z*upOff };
                const float life = 1.0f - std::min(1.0f, back / 4.8f);   // fades toward the tail
                const float str = burn * (1.4f + 1.6f * h1) * life;
                if (str < 0.05f) continue;
                float m[16]; sphereMatrix(c, 0.10f + 0.10f * h2, m);
                const float col[4] = { 1.0f, 0.55f + 0.3f * h3, 0.15f, 1.0f };
                const float em[4]  = { col[0], col[1], col[2], str };
                device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, col, em, m);
            }
        };

        // ---- CORONAL EJECTION: superheated ship debris + shockwave shells -------
        // Deterministic (hash-seeded direction/speed), render-only. `t` = seconds into
        // the Detonation phase; `rev` reverses time for the backwards-scrub stinger.
        auto drawEjecta = [&](const x3::rhi::FrameContext& frame, float t) {
            // Expanding emissive shockwave shells at the impact point.
            for (int s = 0; s < 2; ++s) {
                const float t0 = (float)s * 0.6f;
                const float lt = t - t0;
                if (lt < 0.0f || lt > 2.4f) continue;
                const float rad = kSunRadius * (0.25f + lt * 0.9f);
                const float a   = (1.0f - lt / 2.4f);
                float m[16]; sphereMatrix(entryPos, rad, m);
                const float bc[4] = { 1.0f, 0.7f, 0.35f, 1.0f };
                const float em[4] = { 1.0f, 0.7f, 0.35f, 2.5f * a };
                x3::rhi::IRenderDevice::GlassMaterial gm{};
                gm.opacity = 0.14f * a; gm.roughness = 1.0f; gm.specular = 0.0f;
                gm.tint[0]=1.0f; gm.tint[1]=0.7f; gm.tint[2]=0.35f;
                device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{}, bc, em, gm, m);
            }
            // Debris fragments launched outward in a cone about the surface normal,
            // decelerating; each drawn as a warm emissive streak along its velocity.
            const x3::phys::Vec3 up = (std::fabs(entryNrm.y) < 0.95f)
                                    ? x3::phys::Vec3{ 0,1,0 } : x3::phys::Vec3{ 1,0,0 };
            const x3::phys::Vec3 ta = vnorm(vcross(up, entryNrm));
            const x3::phys::Vec3 tb = vcross(entryNrm, ta);
            for (int i = 0; i < kDebrisCount; ++i) {
                const float h1 = hashF((uint32_t)(i*5+1)), h2 = hashF((uint32_t)(i*5+2));
                const float h3 = hashF((uint32_t)(i*5+3)), h4 = hashF((uint32_t)(i*5+4));
                const float ang  = h1 * 6.2831853f;
                const float cone = 0.35f + 0.5f * h2;              // spread off the normal
                x3::phys::Vec3 dir = vnorm(x3::phys::Vec3{
                    entryNrm.x + (ta.x*std::cos(ang) + tb.x*std::sin(ang)) * cone,
                    entryNrm.y + (ta.y*std::cos(ang) + tb.y*std::sin(ang)) * cone,
                    entryNrm.z + (ta.z*std::cos(ang) + tb.z*std::sin(ang)) * cone });
                const float v0   = kSunRadius * (0.9f + 1.6f * h3);  // launch speed
                // Decelerating travel: distance = v0*(t - 0.5*k*t^2), clamped.
                const float tt = std::min(t, 3.5f);
                const float dist = v0 * (tt - 0.12f * tt * tt);
                const x3::phys::Vec3 c{ entryPos.x + dir.x*dist,
                                        entryPos.y + dir.y*dist,
                                        entryPos.z + dir.z*dist };
                const float life = std::min(1.0f, t / 3.5f);
                const float str  = (3.5f - 3.0f * life) * (0.6f + 0.6f * h4);
                if (str < 0.05f) continue;
                const x3::phys::Vec3 uax = vnorm(vcross(dir, up));
                const x3::phys::Vec3 vax = vcross(dir, uax);
                float m[16];
                composeBasis(m, uax, vax, dir, 40.0f, 40.0f, 130.0f + 90.0f*h3, c);
                const float col[4] = { 1.0f, 0.55f + 0.35f*h4, 0.20f, 1.0f };
                const float em[4]  = { col[0], col[1], col[2], str };
                device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, col, em, m);
            }
        };

        // ---- Flight HUD: active-mode readout + telemetry (reuses the engine's
        //      FontRole HUD text path — device->drawHudTextF / drawHudQuad, the
        //      SAME mechanism the ship-windows cockpit uses). Neon cyberpunk feel.
        auto drawHud = [&](const x3::rhi::FrameContext& frame, float W, float H) {
            using x3::rhi::FontRole;
            const x3::game::FlightMode fm = pilot.mode();
            const char* nm = "ARCADE"; const char* tag = "RESPONSIVE";
            float acc[4] = { 0.30f, 0.95f, 1.0f, 1.0f };            // cyan (Arcade)
            if (fm == x3::game::FlightMode::Assist) {
                nm = "ASSIST"; tag = "WEIGHTY";
                acc[0]=1.0f; acc[1]=0.35f; acc[2]=0.85f;            // magenta (Assist)
            } else if (fm == x3::game::FlightMode::Loose) {
                nm = "LOOSE"; tag = "DRIFT";
                acc[0]=0.75f; acc[1]=0.55f; acc[2]=1.0f;            // violet (Loose)
            }
            // Active-mode readout: prominent, top-center, Orbitron (Title role).
            const float titlePx = 40.0f;
            const float tw = device->textAdvance(FontRole::Title, nm, titlePx);
            const float tx = W * 0.5f - tw * 0.5f;
            const float ty = 22.0f;
            const float bg[4] = { 0.02f, 0.03f, 0.05f, 0.55f };
            device->drawHudQuad(frame, tx - 20.0f, ty - 8.0f, tw + 40.0f, titlePx + 30.0f, bg);
            const float ul[4] = { acc[0], acc[1], acc[2], 0.9f };
            device->drawHudQuad(frame, tx - 20.0f, ty + titlePx + 14.0f, tw + 40.0f, 3.0f, ul);
            device->drawHudTextF(frame, FontRole::Title, nm, tx, ty, titlePx, acc);
            // FIX (Integrator): drop the tagline a few px BELOW the underline so it no
            // longer clips into it (underline bottom ≈ ty+titlePx+17; tag now at +22).
            const float tagPx = 14.0f;
            const float tgw = device->textAdvance(FontRole::HudMono, tag, tagPx);
            const float dim[4] = { acc[0]*0.85f, acc[1]*0.85f, acc[2]*0.85f, 0.85f };
            device->drawHudTextF(frame, FontRole::HudMono, tag,
                                 W * 0.5f - tgw * 0.5f, ty + titlePx + 22.0f, tagPx, dim);

            // Telemetry corner (bottom-left): speed, heading, hull temp, sun range, pos.
            const float spd = pilot.speed();
            const x3::phys::Vec3 pp = pilot.pos();
            // FIX (Integrator): wrap heading to [0,360) so it never reads "+406".
            float hdg = std::fmod(pilot.yaw() * 57.29578f, 360.0f);
            if (hdg < 0.0f) hdg += 360.0f;
            // Hull temp: ambient baseline far away → climbs with the DISPLAYED heat
            // fraction (g_heatShown, which is pinned toward 0 under invuln + eases on
            // toggle). WARNING/CRITICAL derive from an "effective" surface distance
            // that reads infinite while invulnerable, so the whole temperature system
            // (orange/red flash, PULL AWAY, and — below — the beep) goes quiet.
            const float tempC = 22.0f + g_heatShown * 3180.0f;
            const float effSurf = g_invulnerable ? 1e9f : g_sunSurf;
            const bool  warn = (effSurf < kWarnDist);
            const bool  crit = (effSurf < kCritDist);
            char l1[64], l2[64], l3[80], l4[64], l5[64];
            std::snprintf(l1, sizeof(l1), "SPD %6.1f m/s", spd);
            std::snprintf(l2, sizeof(l2), "HDG %03.0f   BOOST %s", (double)hdg, boostActive ? "ON " : "off");
            const char* tState = crit ? "CRITICAL  PULL AWAY" : warn ? "RISING" : "NOMINAL";
            std::snprintf(l3, sizeof(l3), "HULL TEMP %4.0fC  %s", (double)tempC, tState);
            const float sunKm = std::max(0.0f, g_sunSurf) / 1000.0f;
            std::snprintf(l4, sizeof(l4), "SUN %7.1f km", (double)sunKm);
            std::snprintf(l5, sizeof(l5), "POS %5.0f %5.0f %5.0f", (double)pp.x, (double)pp.y, (double)pp.z);
            const float tpx = 16.0f;
            const float bx = 20.0f, by = H - 132.0f;
            const float tbg[4] = { 0.02f, 0.03f, 0.05f, 0.5f };
            device->drawHudQuad(frame, bx - 10.0f, by - 8.0f, 320.0f, 124.0f, tbg);
            const float cyan[4] = { 0.60f, 0.90f, 1.0f, 0.95f };
            // Temp line colour shifts orange (WARNING) → flashing red (CRITICAL).
            float tcol[4] = { 0.60f, 0.90f, 1.0f, 0.95f };
            if (crit) { const float fl = (std::sin(g_clock * 9.0f) > 0.0f) ? 1.0f : 0.45f;
                        tcol[0]=1.0f; tcol[1]=0.20f; tcol[2]=0.15f; tcol[3]=fl; }
            else if (warn) { tcol[0]=1.0f; tcol[1]=0.55f; tcol[2]=0.15f; }
            device->drawHudText(frame, l1, bx, by,          tpx, cyan);
            device->drawHudText(frame, l2, bx, by + 20.0f,  tpx, cyan);
            device->drawHudText(frame, l3, bx, by + 40.0f,  tpx, tcol);
            device->drawHudText(frame, l4, bx, by + 60.0f,  tpx, cyan);
            device->drawHudText(frame, l5, bx, by + 80.0f,  tpx, cyan);
            const float barW = 290.0f, barH = 6.0f, barY = by + 108.0f;
            const float bbg[4] = { 0.10f, 0.15f, 0.20f, 0.7f };
            device->drawHudQuad(frame, bx, barY, barW, barH, bbg);
            const float frac = std::min(1.0f, spd / std::max(1.0f, pilot.tuning().maxSpeed));
            const float fill[4] = { acc[0], acc[1], acc[2], 0.95f };
            device->drawHudQuad(frame, bx, barY, barW * frac, barH, fill);
            // SHIELD readout: engaged (draining inside the core), recharging after
            // a graze-abort, or — if the I-key invulnerability cheat is on — a
            // PERSISTENT gold "INVULNERABLE" readout in the same slot so the two
            // never stack/overlap.
            if (g_invulnerable) {
                const float gold[4] = { 1.0f, 0.85f, 0.20f, 1.0f };   // distinct gold, per the ask
                device->drawHudTextF(frame, FontRole::HudMono, "SHIELD: INVULNERABLE", bx, by - 30.0f, 18.0f, gold);
            } else if (phase == Phase::InsideSun || shieldPct < 99.95f) {
                char sl[48];
                if (phase == Phase::InsideSun)
                    std::snprintf(sl, sizeof(sl), "SHIELD %3.0f%%", (double)std::max(0.0f, shieldPct));
                else
                    std::snprintf(sl, sizeof(sl), "SHIELD %3.0f%% RECHARGING", (double)std::max(0.0f, shieldPct));
                const float sc[4] = { 0.4f, 0.8f, 1.0f, 1.0f };
                device->drawHudTextF(frame, FontRole::HudMono, sl, bx, by - 30.0f, 18.0f, sc);
            }
        };

        // ---- CINEMATIC OVERLAY: shield-engaged flash, molten wash, shield
        //      countdown, kill-cam flash, rewind tag, "30 SECONDS EARLIER…" title
        //      card, fade-to-black. Drawn after the HUD; the shield-engaged flash
        //      is the only bit that can render outside the death phases (it plays
        //      out over kShieldFlashSecs even across a very fast graze). ---------
        auto drawCinematic = [&](const x3::rhi::FrameContext& frame, float W, float H) {
            using x3::rhi::FontRole;
            auto full = [&](float r, float g, float b, float a) {
                if (a <= 0.001f) return; const float c[4] = { r, g, b, a };
                device->drawHudQuad(frame, 0, 0, W, H, c);
            };
            auto center = [&](FontRole role, const char* s, float px, float y, const float col[4]) {
                const float w = device->textAdvance(role, s, px);
                device->drawHudTextF(frame, role, s, W*0.5f - w*0.5f, y, px, col);
            };
            // ONE-SHOT "SHIELD ENGAGED" cue at the exact instant the core surface
            // is crossed: a quick bright flash + caption so the crossing is
            // unmistakable (owner playtest: the corona haze made the actual core
            // edge hard to read against the ambient glow).
            if (shieldFlashT >= 0.0f) {
                const float decay = 1.0f - smooth01(0.0f, kShieldFlashSecs, shieldFlashT);
                full(0.55f, 0.80f, 1.0f, decay * decay * 0.85f);
                const float capA = 1.0f - smooth01(kShieldFlashSecs * 0.55f, kShieldFlashSecs, shieldFlashT);
                if (capA > 0.02f) {
                    const float col[4] = { 0.55f, 0.85f, 1.0f, capA };
                    center(FontRole::Title, "SHIELD ENGAGED", 30.0f, H*0.30f, col);
                }
            }
            // INVULNERABILITY TOGGLE confirmation cue — brief, ~kInvulnFlashSecs,
            // plays regardless of phase (owner: flash "INVULNERABILITY ON/OFF").
            if (invulnFlashT >= 0.0f) {
                const float capA = 1.0f - smooth01(kInvulnFlashSecs * 0.55f, kInvulnFlashSecs, invulnFlashT);
                if (capA > 0.02f) {
                    float col[4];
                    if (invulnFlashOn) { col[0]=1.0f; col[1]=0.85f; col[2]=0.20f; col[3]=capA; }   // gold: ON
                    else               { col[0]=0.65f; col[1]=0.75f; col[2]=0.85f; col[3]=capA; }  // dim slate: OFF
                    center(FontRole::Title, invulnFlashOn ? "INVULNERABILITY ON" : "INVULNERABILITY OFF",
                           26.0f, H*0.20f, col);
                }
            }
            if (phase == Phase::InsideSun) {
                // Molten warm wash, gently pulsing; keep the HUD readable underneath.
                const float pulse = 0.42f + 0.10f * std::sin(g_clock * 5.0f);
                full(1.0f, 0.45f, 0.12f, pulse);
                // Big centre-low shield-failing countdown, red-shifting as it drops.
                const float rem = std::max(0.0f, kShieldSecs - phaseT);
                const float k = rem / kShieldSecs;
                char cd[48]; std::snprintf(cd, sizeof(cd), "SHIELD FAILING IN %4.1fs", (double)rem);
                const float col[4] = { 1.0f, 0.30f + 0.55f*k, 0.20f*k, 1.0f };
                center(FontRole::Title, cd, 40.0f, H*0.62f, col);
            } else if (phase == Phase::Detonation) {
                // Blinding white flash on the blast, decaying over ~0.6s.
                const float fl = 1.0f - smooth01(0.0f, 0.6f, phaseT);
                full(1.0f, 0.96f, 0.9f, fl * 0.95f);
            } else if (phase == Phase::Rewind) {
                // Short backwards-scrub stinger: desaturating dark vignette + tag.
                full(0.0f, 0.0f, 0.02f, 0.28f);
                const float col[4] = { 0.8f, 0.85f, 1.0f, 0.9f };
                center(FontRole::Title, "<< REWIND", 30.0f, H*0.12f, col);
            } else if (phase == Phase::TitleCard) {
                // Film card on near-black: fade in, hold, fade out.
                const float aIn  = smooth01(0.0f, 0.6f, phaseT);
                const float aOut = 1.0f - smooth01(kTitleSecs - 0.6f, kTitleSecs, phaseT);
                const float a = std::min(aIn, aOut);
                full(0.0f, 0.0f, 0.0f, 0.72f + 0.28f * a);
                const float col[4] = { 0.92f, 0.90f, 0.85f, a };
                center(FontRole::Title, "3 0   S E C O N D S   E A R L I E R", 30.0f, H*0.46f, col);
            } else if (phase == Phase::Replay) {
                // Entry flash near the end as the replayed ship breaches the surface.
                const float fl = smooth01(kReplaySecs - 0.5f, kReplaySecs, phaseT);
                full(1.0f, 0.9f, 0.7f, fl * 0.85f);
            } else if (phase == Phase::Respawn) {
                // Fade to black → hold (respawn) → fade back in.
                float a;
                if (phaseT < kFadeSecs)                 a = smooth01(0.0f, kFadeSecs, phaseT);
                else if (phaseT < kFadeSecs + 0.8f)     a = 1.0f;
                else                                    a = 1.0f - smooth01(kFadeSecs + 0.8f, 2.0f*kFadeSecs + 0.8f, phaseT);
                full(0.0f, 0.0f, 0.0f, a);
                if (phaseT > kFadeSecs * 0.7f) {
                    const float col[4] = { 0.95f, 0.55f, 0.35f, std::min(1.0f, a) };
                    center(FontRole::Title, "HULL LOST TO THE SUN", 26.0f, H*0.44f, col);
                    const float col2[4] = { 0.8f, 0.85f, 1.0f, std::min(1.0f, a) };
                    center(FontRole::HudMono, "SHIELD HELD 17.0s", 16.0f, H*0.44f + 40.0f, col2);
                }
            }
        };

        // ---- Draw the recorded ship pose (kill-cam rewind/replay). `g` 0→1 walks the
        //      snapshot oldest→entry; lerp position + basis for smooth playback. ----
        auto lerp3 = [](const x3::phys::Vec3& a, const x3::phys::Vec3& b, float t) {
            return x3::phys::Vec3{ a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t };
        };
        // Sample the recorded trajectory at g∈[0,1] (oldest→entry), interpolating
        // position + basis. Shared by the replay ship draw AND the tracking kill-cam.
        struct ReplayPose { x3::phys::Vec3 p, f, u, r; bool ok; };
        auto replayPoseAt = [&](float g) -> ReplayPose {
            ReplayPose rp{}; rp.ok = false;
            if (trajPlay.size() < 2) return rp;
            g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
            const float fi = g * (float)(trajPlay.size() - 1);
            int i0 = (int)fi; if (i0 < 0) i0 = 0;
            int i1 = std::min(i0 + 1, (int)trajPlay.size() - 1);
            const float fr = fi - (float)i0;
            const auto& A = trajPlay[(size_t)i0]; const auto& B = trajPlay[(size_t)i1];
            rp.p = lerp3(A.p, B.p, fr);
            rp.f = vnorm(lerp3(A.f, B.f, fr));
            rp.u = vnorm(lerp3(A.u, B.u, fr));
            rp.r = vnorm(lerp3(A.r, B.r, fr));
            rp.ok = true;
            return rp;
        };
        // TRACKING KILL-CAM (owner: the 30 s replay "doesnt show the ship flying into
        // the sun" — from the old frozen 5.2 km vantage the ~5 m ship was sub-pixel).
        // Ride ~85 m behind / above / beside the replayed ship, looking along the
        // ship→sun ray so the 2 km star looms ahead and fills the frame as the ship
        // closes and burns into the surface. Returns the camera pose for parameter g.
        auto replayTrackCam = [&](float g, float& ox, float& oy, float& oz,
                                  float& oyaw, float& opit) -> bool {
            const ReplayPose rp = replayPoseAt(g);
            if (!rp.ok) return false;
            const x3::phys::Vec3 toSun = vnorm(x3::phys::Vec3{
                kSunCenter.x - rp.p.x, kSunCenter.y - rp.p.y, kSunCenter.z - rp.p.z });
            const x3::phys::Vec3 camP{
                rp.p.x - toSun.x*68.0f + rp.u.x*20.0f + rp.r.x*24.0f,
                rp.p.y - toSun.y*68.0f + rp.u.y*20.0f + rp.r.y*24.0f,
                rp.p.z - toSun.z*68.0f + rp.u.z*20.0f + rp.r.z*24.0f };
            const x3::phys::Vec3 look{
                rp.p.x + toSun.x*14.0f, rp.p.y + toSun.y*14.0f, rp.p.z + toSun.z*14.0f };
            const x3::phys::Vec3 d = vnorm(x3::phys::Vec3{
                look.x - camP.x, look.y - camP.y, look.z - camP.z });
            ox = camP.x; oy = camP.y; oz = camP.z;
            opit = std::asin(std::max(-1.0f, std::min(1.0f, d.y)));
            oyaw = std::atan2(d.z, d.x);
            return true;
        };
        auto drawReplayShip = [&](const x3::rhi::FrameContext& frame, float g,
                                  const x3::phys::Vec3& viewPos) {
            const ReplayPose rp = replayPoseAt(g);
            if (!rp.ok) return;
            const x3::phys::Vec3 p = rp.p, f = rp.f, u = rp.u, r = rp.r;
            float m[16]; shipMatrix(p, f, u, r, m);
            drawShipAt(frame, m, 1.5f);
            // Entry-burn: derive the sheath directly from THIS recorded position's
            // distance to the sun (not the live pilot, which is frozen elsewhere
            // during the cinematic) — the replayed ship visibly catches fire as it
            // closes and crosses the surface wreathed in flame. `viewPos` is the
            // ACTUAL render camera so the sheath billboards face the tracking cam.
            const float surfDist = vlen(x3::phys::Vec3{ p.x-kSunCenter.x, p.y-kSunCenter.y, p.z-kSunCenter.z }) - kSunRadius;
            const float replayBurn = smooth01(kBurnStartSurf, 0.0f, surfDist);
            drawBurnSheath(frame, p, viewPos, replayBurn);
            drawBurnEmbers(frame, p, f, u, r, replayBurn);
        };

        // ---- Snapshot the ring buffer (oldest→newest) into trajPlay. ------------
        auto snapshotTraj = [&]() {
            trajPlay.clear();
            for (int i = 0; i < trajCount; ++i) {
                const int idx = ((trajHead - trajCount + i) % kTrajLen + kTrajLen) % kTrajLen;
                trajPlay.push_back(trajRing[(size_t)idx]);
            }
        };

        // ---- Frame the external kill-cam so the sun fills ~half the screen and the
        //      impact point is centred (camera ~1.6 sun-radii off the surface). -----
        auto setupKillCam = [&](const x3::phys::Vec3& shipPos) {
            entryNrm = vnorm(x3::phys::Vec3{ shipPos.x - kSunCenter.x,
                                             shipPos.y - kSunCenter.y,
                                             shipPos.z - kSunCenter.z });
            entryPos = x3::phys::Vec3{ kSunCenter.x + entryNrm.x*kSunRadius,
                                       kSunCenter.y + entryNrm.y*kSunRadius,
                                       kSunCenter.z + entryNrm.z*kSunRadius };
            const x3::phys::Vec3 wup = (std::fabs(entryNrm.y) < 0.95f)
                                     ? x3::phys::Vec3{ 0,1,0 } : x3::phys::Vec3{ 1,0,0 };
            const x3::phys::Vec3 lat = vnorm(vcross(entryNrm, wup));
            const x3::phys::Vec3 cdir = vnorm(x3::phys::Vec3{
                entryNrm.x*0.55f + lat.x*0.83f, entryNrm.y*0.55f + lat.y*0.83f,
                entryNrm.z*0.55f + lat.z*0.83f });
            const float camDist = kSunRadius + 3200.0f;   // ~5200 m from centre
            cineCamPos = x3::phys::Vec3{ kSunCenter.x + cdir.x*camDist,
                                         kSunCenter.y + cdir.y*camDist,
                                         kSunCenter.z + cdir.z*camDist };
            const x3::phys::Vec3 d = vnorm(x3::phys::Vec3{ entryPos.x - cineCamPos.x,
                                                           entryPos.y - cineCamPos.y,
                                                           entryPos.z - cineCamPos.z });
            cinePit = std::asin(std::max(-1.0f, std::min(1.0f, d.y)));
            cineYaw = std::atan2(d.z, d.x);
        };

        // ---- Advance the sun-death PHASE MACHINE (windowed only). Computes heat,
        //      records the trajectory, drives transitions/timers, respawns. `skip`
        //      (any key during InsideSun or the frozen cinematic) jumps straight to
        //      Respawn. NOTE: the pilot is only FROZEN from Detonation onward — see
        //      the AUDIT note above the enum. Through Flying AND InsideSun the ship
        //      keeps flying (sphys->step/pilot.update() still run in the host loop),
        //      so InsideSun is a real graze-abort window: pull back across the core
        //      edge before the timer expires and the countdown cancels. Return value
        //      is a convenience mirror of "phase != Flying" (unused for control flow
        //      — the host branches on phase directly). ---------------------------
        auto advanceSequence = [&](float fdt, bool skip) -> bool {
            const x3::phys::Vec3 sPos = pilot.pos();
            const float distC = vlen(x3::phys::Vec3{ sPos.x-kSunCenter.x,
                                     sPos.y-kSunCenter.y, sPos.z-kSunCenter.z });
            g_sunSurf = distC - kSunRadius;
            // DIAGNOSTIC heartbeat: while within the warning band, log the raw
            // numbers every 2 s so a scripted dive reveals exactly what the trigger
            // sees vs. the HUD "SUN km" readout (both derive from g_sunSurf here).
            if (kSunDebugLog) {
                dbgLogTimer += fdt;
                if (g_sunSurf < kWarnDist && dbgLogTimer >= 2.0f) {
                    dbgLogTimer = 0.0f;
                    char lb[192];
                    std::snprintf(lb, sizeof(lb),
                        "[sun-dbg] hb phase=%d distC=%.1f g_sunSurf=%.1f (HUD SUN %.2fkm) shield=%.1f pos=(%.0f,%.0f,%.0f)",
                        (int)phase, (double)distC, (double)g_sunSurf,
                        (double)(std::max(0.0f, g_sunSurf) / 1000.0f), (double)shieldPct,
                        (double)sPos.x, (double)sPos.y, (double)sPos.z);
                    x3::logInfo(lb);
                }
            }
            // Heat curve: 0 far, smoothstep up as the surface distance falls from
            // kHeatStart to the body; a touch of inverse-square bite near the surface.
            const float prox = smooth01(kHeatStart, kSunRadius, g_sunSurf);
            g_heat = prox * prox;
            // Ease the DISPLAYED heat toward 0 while invulnerable, else toward the
            // real proximity heat (~2 s time-constant so an invuln toggle glides the
            // hull temp instead of snapping it — see g_heatShown decl).
            {
                const float heatTgt = g_invulnerable ? 0.0f : g_heat;
                const float hk = 1.0f - std::exp(-2.2f * fdt);
                g_heatShown += (heatTgt - g_heatShown) * hk;
            }
            // One-shot "SHIELD ENGAGED" flash/caption timer — ticks regardless of
            // phase so it plays out even across a very fast graze.
            if (shieldFlashT >= 0.0f) {
                shieldFlashT += fdt;
                if (shieldFlashT > kShieldFlashSecs) shieldFlashT = -1.0f;
            }
            // Fiery entry-burn: tick the one-shot crossing flare, then derive this
            // frame's live-ship burn intensity (proximity ramp + flare boost) for
            // drawBurnSheath/drawBurnEmbers. Ticks regardless of phase, like the
            // shield flash above, so it decays correctly across a fast graze too.
            if (burnFlareT >= 0.0f) {
                burnFlareT += fdt;
                if (burnFlareT > kBurnFlareSecs) burnFlareT = -1.0f;
            }
            {
                const float burnBase = smooth01(kBurnStartSurf, 0.0f, g_sunSurf);
                const float flareBoost = (burnFlareT >= 0.0f)
                    ? 0.5f * (1.0f - smooth01(0.0f, kBurnFlareSecs, burnFlareT)) : 0.0f;
                g_burnFactor = burnBase * (1.0f + flareBoost);
            }
            // Trajectory ring buffer: record while the ship is under live player
            // control (Flying AND the graze-able InsideSun window) so a later real
            // detonation still replays the WHOLE approach, including any earlier graze.
            if (phase == Phase::Flying || phase == Phase::InsideSun) {
                trajTimer += fdt;
                if (trajTimer >= 1.0f / kTrajHz) {
                    trajTimer = 0.0f;
                    trajRing[(size_t)trajHead] = { sPos, pilot.forward(), pilot.up(), pilot.right() };
                    trajHead = (trajHead + 1) % kTrajLen;
                    if (trajCount < kTrajLen) ++trajCount;
                }
            }
            if (phase == Phase::Flying) {
                // GRAZE-ABORT recharge: shield restores over kShieldRechargeSecs once
                // back in free flight (no-op once it's back to full).
                if (shieldPct < 100.0f)
                    shieldPct = std::min(100.0f, shieldPct + fdt * (100.0f / kShieldRechargeSecs));
                if (distC < kSunRadius) {           // breached the CORE → shield engages
                    if (kSunDebugLog) {
                        char lb[192];
                        std::snprintf(lb, sizeof(lb),
                            "[sun-dbg] >>> Flying->InsideSun  distC=%.2f (kSunRadius=%.0f) g_sunSurf=%.2f pos=(%.1f,%.1f,%.1f) center=(%.1f,%.1f,%.1f)",
                            (double)distC, (double)kSunRadius, (double)g_sunSurf,
                            (double)sPos.x, (double)sPos.y, (double)sPos.z,
                            (double)kSunCenter.x, (double)kSunCenter.y, (double)kSunCenter.z);
                        x3::logInfo(lb);
                    }
                    phase = Phase::InsideSun; phaseT = 0.0f; shieldPct = 100.0f;
                    shieldFlashT = 0.0f;
                    burnFlareT = 0.0f;   // pairs the +50% entry-burn flare with the SHIELD ENGAGED cue
                    setupKillCam(sPos);
                }
                return false;
            }
            // --- Sequence active ---
            phaseT += fdt;
            // "Any key" skip jumps to Respawn — but ONLY from the FROZEN cinematic
            // (Detonation onward). InsideSun is still LIVE flight (the graze-abort
            // window): a movement key there means "fly", NOT "skip my death", so it
            // must never short-circuit to Respawn (owner bug: "with Invuln on, Hull
            // lost to the sun" — pressing W/Space/Shift to fly around inside the star
            // was consumed as a cinematic skip -> Respawn, bypassing invuln entirely).
            if (skip && phase != Phase::Respawn && phase != Phase::InsideSun) {
                phase = Phase::Respawn; phaseT = 0.0f; respawned = false;
            }
            switch (phase) {
                case Phase::InsideSun:
                    // INVULNERABLE: continuously reset the countdown clock each frame
                    // (owner's own suggested alternative to "does not run") — shield
                    // reads 100%, and since phaseT never accumulates, the kShieldSecs
                    // expiry below can never fire. Loiter/exit freely; visuals untouched.
                    if (g_invulnerable) {
                        phaseT = 0.0f;
                        shieldPct = 100.0f;
                    } else {
                        shieldPct = 100.0f * std::max(0.0f, 1.0f - phaseT / kShieldSecs);
                    }
                    if (distC >= kSunRadius) {   // GRAZE: pulled back out before the timer expired
                        phase = Phase::Flying; phaseT = 0.0f;
                        break;
                    }
                    if (!g_invulnerable && phaseT >= kShieldSecs) {
                        if (kSunDebugLog) {
                            char lb[192];
                            std::snprintf(lb, sizeof(lb),
                                "[sun-dbg] >>> InsideSun->Detonation  held=%.2fs distC=%.2f g_sunSurf=%.2f pos=(%.1f,%.1f,%.1f)",
                                (double)phaseT, (double)distC, (double)g_sunSurf,
                                (double)sPos.x, (double)sPos.y, (double)sPos.z);
                            x3::logInfo(lb);
                        }
                        snapshotTraj(); phase = Phase::Detonation; phaseT = 0.0f;
                    }
                    break;
                case Phase::Detonation:
                    if (phaseT >= kDetonateSecs) { phase = Phase::Rewind; phaseT = 0.0f; }
                    break;
                case Phase::Rewind:
                    if (phaseT >= kRewindSecs) { phase = Phase::TitleCard; phaseT = 0.0f; }
                    break;
                case Phase::TitleCard:
                    if (phaseT >= kTitleSecs) { phase = Phase::Replay; phaseT = 0.0f; }
                    break;
                case Phase::Replay:
                    if (phaseT >= kReplaySecs) { phase = Phase::Respawn; phaseT = 0.0f; respawned = false; }
                    break;
                case Phase::Respawn:
                    if (!respawned && phaseT >= kFadeSecs) {   // re-seed at black
                        const x3::game::FlightMode keep = pilot.mode();
                        pilot.spawn(*sphys, 0.0f, 0.0f, 0.0f);
                        pilot.setMode(keep);
                        trajHead = trajCount = 0; trajTimer = 0.0f;
                        respawned = true;
                    }
                    if (phaseT >= 2.0f * kFadeSecs + 0.8f) { phase = Phase::Flying; phaseT = 0.0f; g_heat = 0.0f; }
                    break;
                default: break;
            }
            return true;
        };

        // ---- Pause menu overlay (ESC). RESUME / FLIGHT MODE / QUIT. Same HUD
        //      text mechanism; keyboard-navigated (sel = highlighted row). ------
        auto drawPauseMenu = [&](const x3::rhi::FrameContext& frame, float W, float H, int sel) {
            using x3::rhi::FontRole;
            const float ov[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
            device->drawHudQuad(frame, 0, 0, W, H, ov);
            const float pw = 440.0f, ph = 296.0f;   // +46px row for the invulnerability toggle
            const float px = W * 0.5f - pw * 0.5f, py = H * 0.5f - ph * 0.5f;
            const float pbg[4] = { 0.03f, 0.05f, 0.08f, 0.92f };
            device->drawHudQuad(frame, px, py, pw, ph, pbg);
            const float acc[4] = { 0.30f, 0.95f, 1.0f, 1.0f };
            device->drawHudQuad(frame, px, py, pw, 4.0f, acc);
            const char* title = "PAUSED";
            const float tw = device->textAdvance(FontRole::Title, title, 30.0f);
            device->drawHudTextF(frame, FontRole::Title, title, px + pw*0.5f - tw*0.5f, py + 18.0f, 30.0f, acc);
            char midItem[64];
            std::snprintf(midItem, sizeof(midItem), "FLIGHT MODE:  %s", x3::game::flightModeName(pilot.mode()));
            char invItem[64];
            std::snprintf(invItem, sizeof(invItem), "SHIELD: INVULNERABLE %s", g_invulnerable ? "ON" : "OFF");
            const char* items[4] = { "RESUME", midItem, invItem, "QUIT TO DESKTOP" };
            float iy = py + 78.0f;
            for (int i = 0; i < 4; ++i) {
                const bool on = (i == sel);
                if (on) {
                    const float selbg[4] = { acc[0]*0.25f, acc[1]*0.25f, acc[2]*0.30f, 0.55f };
                    device->drawHudQuad(frame, px + 20.0f, iy - 5.0f, pw - 40.0f, 30.0f, selbg);
                }
                float col[4];
                if (on) { col[0]=1.0f; col[1]=0.90f; col[2]=0.40f; col[3]=1.0f; }        // amber highlight
                else    { col[0]=0.60f; col[1]=0.80f; col[2]=0.95f; col[3]=0.85f; }
                device->drawHudTextF(frame, FontRole::Menu, items[i], px + 34.0f, iy, 22.0f, col);
                iy += 46.0f;
            }
            const float hint[4] = { 0.5f, 0.6f, 0.7f, 0.8f };
            device->drawHudTextF(frame, FontRole::HudMono, "UP/DOWN + ENTER   ESC=RESUME",
                                 px + 34.0f, py + ph - 26.0f, 12.0f, hint);
        };

        // ===== INSTRUMENTED SUN-DIVE REPRODUCTION (X3_SUN_DIVE_TEST=1) =======
        // Owner bug, 3rd report: "hull STILL lost to the sun BEFORE entering the
        // sun." Two prior code audits declared the trigger correct; this drives the
        // REAL controller (pilot.update) + REAL phase machine (advanceSequence) on a
        // scripted straight-line dive at the star and LOGS every transition so we
        // can read — not assert — where death actually fires. Pair with
        // kSunDebugLog=true. No window / no GPU needed: it only steps the sim.
        if (const char* diveEnv = std::getenv("X3_SUN_DIVE_TEST")) {
            // Variant: =1 (default) the death-repro dive (must reach Detonation at 17s);
            // =2 the INVULN regression (owner bug "with Invuln on, Hull lost to the sun")
            // — force g_invulnerable ON before entry; the ship must sit INSIDE the core
            // >30 s with NO detonation, shield pinned 100, and HULL TEMP at ambient.
            const int   diveVariant = std::atoi(diveEnv);
            const bool  invulnTest  = (diveVariant == 2);
            g_invulnerable = invulnTest;
            x3::logInfo(std::string("[sun-dive] === INSTRUMENTED SUN-DIVE REPRODUCTION begin (variant ")
                        + std::to_string(diveVariant) + (invulnTest ? ", INVULN ON) ===" : ") ==="));
            char lb[192];
            std::snprintf(lb, sizeof(lb),
                "[sun-dive] kSunCenter=(%.0f,%.0f,%.0f) kSunRadius=%.0f corona visible edge~=%.0f (1.34x) kWarnDist=%.0f kCritDist=%.0f",
                (double)kSunCenter.x, (double)kSunCenter.y, (double)kSunCenter.z,
                (double)kSunRadius, (double)(kSunRadius * 1.34f), (double)kWarnDist, (double)kCritDist);
            x3::logInfo(lb);
            // Loose preset (high accel/top speed, 340 m/s) so the dive actually
            // reaches the core within the sim cap. Start 6 km off the core surface,
            // on the sun ray, and fly straight in.
            pilot.setMode(x3::game::FlightMode::Loose);
            const float startDistC = kSunRadius + 6000.0f;
            const x3::phys::Vec3 startPos{ kSunCenter.x - kSunDir.x*startDistC,
                                           kSunCenter.y - kSunDir.y*startDistC,
                                           kSunCenter.z - kSunDir.z*startDistC };
            pilot.spawn(*sphys, startPos.x, startPos.y, startPos.z, pilot.tuning());
            const float fdt = 1.0f / 60.0f;
            const float kSteerToLook = 1.0f / (1.9f * 0.00132f);   // rad -> lookDX units (kMouseSens*kPxToRad)
            auto angWrap = [](float a){ while (a >  3.14159265f) a -= 6.2831853f;
                                        while (a < -3.14159265f) a += 6.2831853f; return a; };
            Phase prevPhase = phase;
            bool  reachedDeton = false;
            float insideTime = 0.0f;     // seconds spent with distC < kSunRadius (inside the core)
            float hbTimer    = 0.0f;     // heartbeat log accumulator
            float minShield  = 100.0f;   // lowest shield seen while inside (invuln proof)
            float maxTempC   = 22.0f;    // hottest HULL TEMP seen (invuln proof — must stay ~ambient)
            for (int step = 0; step < 60 * 100; ++step) {   // 100 s sim cap
                const x3::phys::Vec3 p = pilot.pos();
                const x3::phys::Vec3 toC{ kSunCenter.x-p.x, kSunCenter.y-p.y, kSunCenter.z-p.z };
                const float distC = vlen(toC);
                const x3::phys::Vec3 dir = vnorm(toC);
                // Nose onto the star (controller convention: fwd=(cospcosy,sinp,cospsiny)).
                const float tgtYaw = std::atan2(dir.z, dir.x);
                const float tgtPit = std::asin(std::max(-1.0f, std::min(1.0f, dir.y)));
                x3::game::PlayerInput in{};
                // GENTLE damped steer (0.15 of the residual/frame) so the nose eases
                // onto the star without a huge one-frame delta spiking auto-bank into
                // a tumble (which misdirects thrust and stalls the dive).
                const float yawErr = angWrap(tgtYaw - pilot.yaw());
                const float pitErr = tgtPit - pilot.pitch();
                in.lookDX =  yawErr * kSteerToLook * 0.15f;
                in.lookDY = -pitErr * kSteerToLook * 0.15f;
                const bool  aimed = (std::fabs(yawErr) < 0.05f && std::fabs(pitErr) < 0.05f);
                // Throttle: once roughly aimed, dive hard until ~800 m inside the core,
                // then reverse-thrust to brake and hover so the 17 s shield timer
                // expires (reproduces the death) rather than grazing straight through.
                const x3::phys::Vec3 v = pilot.velocity();
                const float radialIn = v.x*dir.x + v.y*dir.y + v.z*dir.z;   // + = closing
                if (!aimed)                 in.moveFwd = 0.0f;              // finish turning first
                else if (distC > 1200.0f)   in.moveFwd = 1.0f;             // full dive
                else                        in.moveFwd = std::max(-1.0f, std::min(1.0f,
                                                0.004f*(distC - 800.0f) - 0.25f*radialIn));
                in.sprint  = aimed && distC > kSunRadius;   // boost only diving in
                pilot.update(in, fdt, *sphys);
                advanceSequence(fdt, false);
                // Telemetry: hull temp derives from the DISPLAYED heat (g_heatShown),
                // which is pinned to ambient under invuln — the =2 proof watches it.
                const float tempC = 22.0f + g_heatShown * 3180.0f;
                if (distC < kSunRadius) {
                    insideTime += fdt;
                    if (phase == Phase::InsideSun) minShield = std::min(minShield, shieldPct);
                    maxTempC = std::max(maxTempC, tempC);
                }
                if (phase != prevPhase) {
                    std::snprintf(lb, sizeof(lb), "[sun-dive] t=%.2fs phase %d->%d  distC=%.1f g_sunSurf=%.1f speed=%.1f shield=%.0f tempC=%.0f",
                        (double)(step*fdt), (int)prevPhase, (int)phase,
                        (double)distC, (double)g_sunSurf, (double)pilot.speed(),
                        (double)shieldPct, (double)tempC);
                    x3::logInfo(lb);
                    prevPhase = phase;
                }
                // Heartbeat every 2 s while inside/near the core so the =2 run PROVES
                // the ship loiters with shield 100 + temp ambient and never detonates.
                hbTimer += fdt;
                if (phase == Phase::InsideSun && hbTimer >= 2.0f) {
                    hbTimer = 0.0f;
                    std::snprintf(lb, sizeof(lb), "[sun-dive] hb t=%.1fs InsideSun inside=%.1fs shield=%.0f%% HULL TEMP=%.0fC%s",
                        (double)(step*fdt), (double)insideTime, (double)shieldPct, (double)tempC,
                        invulnTest ? "  (invuln: expect 100%% + ambient)" : "");
                    x3::logInfo(lb);
                }
                if (phase == Phase::Detonation) { reachedDeton = true;
                    x3::logInfo("[sun-dive] reached Detonation — full death reproduced; stopping."); break; }
                // INVULN variant: no death can come, so run long enough to PROVE a >30 s
                // loiter inside the core, then exit cleanly.
                if (invulnTest && insideTime > 35.0f) {
                    x3::logInfo("[sun-dive] invuln: survived >35 s inside the core — no detonation; stopping."); break;
                }
            }
            if (invulnTest) {
                const bool survived = !reachedDeton && insideTime > 30.0f &&
                                      minShield > 99.5f && maxTempC < 40.0f;
                std::snprintf(lb, sizeof(lb),
                    "[sun-dive] INVULN RESULT: %s  (detonated=%s inside=%.1fs minShield=%.0f%% maxTempC=%.0fC)",
                    survived ? "PASS — survived, shield pinned, temp ambient" : "FAIL",
                    reachedDeton ? "yes" : "no", (double)insideTime, (double)minShield, (double)maxTempC);
                x3::logInfo(lb);
            } else if (!reachedDeton) {
                x3::logInfo("[sun-dive] cap hit without Detonation (grazed/loitered) — see transitions above.");
            }
            g_invulnerable = false;   // don't leak the cheat state past the test
            x3::logInfo("[sun-dive] === INSTRUMENTED SUN-DIVE REPRODUCTION end ===");
            sphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return 0;
        }

        // ===== Headless capture (--world space --screenshot <path>) ========
        if (headless) {
            // The frustum-cull pass on this baseline tests AABBs that may be
            // wrong for a deeply-nested GLB drawable transform. Disable for the
            // capture so the test screenshot is robust against that; it is a
            // VISUAL gate, not a perf gate. (Windowed path leaves it default-on.)
            device->setFrustumCullEnabled(false);
            // Camera behind the player ship looking toward +X (yaw=0 -> +X is
            // the device's "forward 0" per Player::camera()), slight downward
            // pitch to catch the slight-Y staggered decor ships. The fleet
            // cluster sits at x=60..200 with +/-Z flanks within FOV.
            float cam[5] = { -25.0f, 6.0f, 0.0f, 0.0f, -0.05f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            // X3_SUN_INSIDE=1: teleport the capture camera to the star's core and
            // render the InsideSun plasma dome so the interior experience (deliverable
            // B) is capturable headless (the live phase machine never runs here). The
            // camera sits at the sun centre; the swirling dome surrounds it.
            const bool insideShot = std::getenv("X3_SUN_INSIDE") != nullptr;
            if (insideShot && !shotCamOverride) {
                cam[0] = kSunCenter.x; cam[1] = kSunCenter.y; cam[2] = kSunCenter.z;
                cam[3] = 0.8018f; cam[4] = 0.0f;
                g_clock = 8.0f;   // seed the swirl off its origin
            }
            // X3_KILLCAM=1: synthesize a straight dive into the star, then frame it with
            // the LIVE tracking kill-cam (replayTrackCam) so the '30 SECONDS EARLIER'
            // replay framing is verifiable headless — the ship must read LARGE, burning,
            // flying into the looming sun (the live windowed replay uses the same math).
            const bool killcamShot = std::getenv("X3_KILLCAM") != nullptr;
            if (killcamShot && !shotCamOverride) {
                trajPlay.clear();
                const int kN = 24;
                for (int s = 0; s < kN; ++s) {
                    const float fr = (float)s / (float)(kN - 1);        // 0 far .. 1 at surface
                    const float distC = kSunRadius + 5000.0f * (1.0f - fr);
                    TrajSample ts{};
                    ts.p = x3::phys::Vec3{ kSunCenter.x - kSunDir.x*distC,
                                           kSunCenter.y - kSunDir.y*distC,
                                           kSunCenter.z - kSunDir.z*distC };
                    ts.f = kSunDir;                                     // nosing into the star
                    ts.r = vnorm(vcross(ts.f, x3::phys::Vec3{ 0,1,0 }));
                    ts.u = vnorm(vcross(ts.r, ts.f));
                    trajPlay.push_back(ts);
                }
                // Sample mid-approach: the ship reads LARGE against a big (but framed)
                // sun disc with space around it — a representative replay frame.
                float kx, ky, kz, kyaw, kpit;
                if (replayTrackCam(0.7f, kx, ky, kz, kyaw, kpit)) {
                    cam[0] = kx; cam[1] = ky; cam[2] = kz; cam[3] = kyaw; cam[4] = kpit;
                }
                g_clock = 3.0f;   // seed the burn flicker off its origin
            }
            const std::string outPath = screenshot ? screenshotPath : std::string("G:/X3Native/captures/space.png");
            // `--set <cvar> <value>` pairs, read straight off the command line:
            // this capture path has no IConsole (HostContext carries the raw
            // pairs, and the console only exists inside the interactive loop).
            auto commsCVar = [&](const char* name) -> int {
                for (const auto& kv : hc.cliCVars)
                    if (kv.first == name) return std::atoi(kv.second.c_str());
                return 0;
            };
            // The comms device's capture-path instance (drawn with the HUD below).
            x3::game::CommsDevice   shotComms;
            x3::game::CommsDirector shotCommsDirector;
            x3::ui::UiContext     shotCommsUi;
            bool                  shotCommsSeeded = false;
            // Heat telemetry for the HUD (the sequence NEVER runs in headless — the
            // spawn is 48 km off the surface, so heat is ~0 and no death triggers).
            {
                const x3::phys::Vec3 sp = pilot.pos();
                g_sunSurf = vlen(x3::phys::Vec3{ sp.x-kSunCenter.x, sp.y-kSunCenter.y, sp.z-kSunCenter.z }) - kSunRadius;
                const float prox = smooth01(kHeatStart, kSunRadius, g_sunSurf); g_heat = prox*prox;
                g_heatShown = g_heat;   // headless: no invuln, show the real value directly
            }
            // TWIN-SUN FIX (headless): aim the painted sky sun down the capture
            // camera→body ray so the disc hides behind the real body from THIS
            // vantage too (matters for off-axis verification shots). Camera is
            // static in headless, so set it once here.
            if (!insideShot) {
                const x3::phys::Vec3 toSun{ kSunCenter.x - cam[0], kSunCenter.y - cam[1], kSunCenter.z - cam[2] };
                const float tl = vlen(toSun);
                if (tl > 1.0f) {
                    skyP.sunDir[0] = toSun.x / tl; skyP.sunDir[1] = toSun.y / tl; skyP.sunDir[2] = toSun.z / tl;
                    device->setSkyParams(skyP);
                }
            }
            // feat/wormholes: PRE-ROLL the staged opening to X3_WORMHOLE_T before
            // the settle frames, at the real 165 Hz step. The capture path renders
            // a single frame, so this is what lets a SERIES of runs walk the
            // opening phase by phase and be judged as the event it is rather than
            // as one still. The pre-roll also feeds the light rig, so the spill on
            // the hull in the captured frame is the spill at that exact moment.
            if (wormholeEnterIdx >= 0 && wormholeEnterIdx < wormholes.count()) {
                const float* wp = wormholes.at(wormholeEnterIdx).pos();
                pilot.spawn(*sphys, wp[0], wp[1], wp[2]);
                x3::logInfo("--world space: X3_WORMHOLE_ENTER — ship parked in the mouth of "
                            + std::string(wormholes.at(wormholeEnterIdx).name()));
            }
            if (wormholePreroll > 0.0f) {
                const float wh = 1.0f / 165.0f;
                const int steps = (int)(wormholePreroll / wh);
                // g_clock rides the pre-roll too: every time-driven part of the look
                // (the shell flow pulses, the streak scroll, the flicker) is a
                // function of accumulated time, so a frame captured at t must be the
                // frame the 165 Hz game shows at t - not the same scene at t = 0.
                for (int i = 0; i < steps; ++i) { tickWormholes(wh, pilot.pos()); g_clock += wh; }
            }
            // Settle: a few frames so the lights register + the meshes upload.
            const int kFrames = 16;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                sphys->step(dt);
                combatFx.update(dt);
                // Ticked BEFORE the light rig upload so the spill in this frame is
                // this frame's spill, not the previous one's.
                tickWormholes(dt, pilot.pos());
                // THE RIDE, HEADLESS. A single still cannot judge a ride, so the
                // capture path plays the SAME transit the windowed loop does and
                // X3_WORMHOLE_T walks the series across it (entry -> mid -> exit).
                // Same camera, same lights, same draw - a capture that diverged
                // from the live ride would be worth nothing.
                if (transitEngaged) {
                    transitLights();
                    float tcx, tcy, tcz, tcyaw, tcpit, tcfov;
                    transitCamera(tcx, tcy, tcz, tcyaw, tcpit, tcfov);
                    device->setCamera(tcx, tcy, tcz, tcyaw, tcpit, tcfov);
                } else {
                updateDynamicLights(pilot.pos(), pilot.forward(), pilot.up(), pilot.right());
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                }
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                if (insideShot) g_clock += (float)dt;   // pan the plasma across settle frames
                auto frame = device->beginFrame();
                if (frame.valid) {
                    if (transitEngaged) {
                        // INSIDE THE THROAT. The world scene is not drawn: for the
                        // length of the jump the tunnel IS the frame.
                        drawTransitWorld(frame, g_clock);
                        uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                        drawTransitHud(frame, (float)hw, (float)hh);
                    } else if (insideShot) {
                        // Interior-only capture: the plasma dome wrapping the camera,
                        // with the molten wash over it (as the live InsideSun does).
                        drawInterior(frame, x3::phys::Vec3{ cam[0], cam[1], cam[2] });
                        uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                        const float wash[4] = { 1.0f, 0.45f, 0.12f, 0.42f };
                        device->drawHudQuad(frame, 0, 0, (float)hw, (float)hh, wash);
                    } else if (killcamShot) {
                        // Tracking kill-cam framing: the star + the burning replay ship
                        // sailing into it (mirrors the live Replay draw at g=0.6).
                        drawSun(frame, x3::phys::Vec3{ cam[0], cam[1], cam[2] });
                        drawFlares(frame);
                        drawReplayShip(frame, 0.7f, x3::phys::Vec3{ cam[0], cam[1], cam[2] });
                    } else {
                        drawScene(frame);
                        combatFx.submit(*device, frame);
                        drawSpeedFx(frame);
                        drawSun(frame, x3::phys::Vec3{ cam[0], cam[1], cam[2] });
                        drawFlares(frame);
                        drawWormholes(frame, x3::phys::Vec3{ cam[0], cam[1], cam[2] },
                                      wormholePreroll);
                        drawShipLights(frame, 0.15f, 1.0f);
                        uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                        drawHud(frame, (float)hw, (float)hh);

                        // THE SHIP COMMS DEVICE. This headless capture path never
                        // enters the interactive loop (where HostShell draws it),
                        // so it is drawn here too — otherwise the one world the
                        // device was designed for could never be reviewed from a
                        // frame. `--set comms_demo 1` stages a representative feed;
                        // without it this draws the idle surface.
                        {
                            if (!shotCommsSeeded && commsCVar("comms_demo")) {
                                shotCommsSeeded = true;
                                using CS = x3::game::CommsSender;
                                shotComms.post(CS::ShipAI, x3::game::kCommsShipAiName,
                                    "Comms online. I have the channel, Commander.");
                                shotComms.post(CS::ShipAI, x3::game::kCommsShipAiName,
                                    "STABLE WORMHOLE 640m - THE RIFT HUB. Transit corridor is holding.");
                                shotComms.post(CS::Hostile, x3::game::kCommsHostileName,
                                    "You are a long way from anything that will miss you.");
                                shotComms.post(CS::ShipAI, x3::game::kCommsShipAiName,
                                    "Contact. Capital-class signature, bearing two-seven-zero.");
                                shotComms.post(CS::Hostile, x3::game::kCommsHostileName,
                                    "First blood. You fly like something that has never been hunted.");
                                shotComms.post(CS::ShipAI, x3::game::kCommsShipAiName,
                                    "Shields at thirty percent. Break contact and let them cycle.");
                                shotComms.post(CS::Hostile, x3::game::kCommsHostileName,
                                    "Launching. You will not out-fly the whole wing.");
                                shotComms.post(CS::ShipAI, x3::game::kCommsShipAiName,
                                    "UNSTABLE WORMHOLE 880m - THE MAGMA ZONE. Aperture is "
                                    "fluctuating - transit not advised.");
                            }
                            // feat/wormholes: DRAIN THE REAL BUS into the capture
                            // device. Until now this panel could only ever show
                            // the eight hard-coded demo strings above — two of
                            // which were WORMHOLE ADVISORIES written by hand,
                            // because no wormhole existed to generate one. The
                            // field publishes live rows every tick now, so the
                            // captured panel shows the advisory the AEGIS director
                            // actually produced for the wormhole actually in the
                            // frame, at its actually-measured range and stability.
                            {
                                x3::game::CommsSnapshot snap;
                                x3::game::commsBus().drain(shotComms, snap);
                                shotCommsDirector.update(shotComms, snap, (float)dt);
                            }
                            if (commsCVar("comms_focus") && !shotComms.focused())
                                shotComms.setFocused(true);
                            x3::ui::UiInput cin{};
                            shotCommsUi.begin(*device, frame, cin);
                            shotComms.draw(shotCommsUi, *device, frame, 1.0f / 60.0f);
                            shotCommsUi.end();
                        }
                    }
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world space: wrote " + outPath);
            else       x3::logError("--world space: capture FAILED");
            combatFx.shutdown(*device);
            transit.shutdown(*device);
            wormholes.shutdown(*device);
            device->destroyMesh(dustMesh); device->destroyMesh(sunMesh); device->destroyMesh(glowDiscMesh);
            device->destroyMesh(plasmaMeshA); device->destroyMesh(plasmaMeshB);
            device->destroyTexture(sunTex);
            device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
            if (shipModel.ok) mloader->unload(shipModel);
            sphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: 6DOF pilot, mouse + WASD + Q/E + V ==
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevV = false, prevLmb = false, prevI = false;

        // ---- Flight AUDIO — SPACESHIP ENGINE (owner: "sounds like a car engine,
        //      SHIFTING even... needs to sound like a SpaceShip!"). Root cause: this
        //      reused assets/audio/vehicles/engine_loop.wav (the DRIVE world's
        //      combustion-car loop) and rode speed as PITCH — a pitch sweep reads as
        //      gear shifts. Replaced with two synthesized loops (tools/gen_space_
        //      audio.py: a deep reactor thrum + a broadband thruster whoosh), both
        //      run CONTINUOUSLY at a FIXED pitch (1.0) and crossfaded by VOLUME
        //      instead — hum tracks speed, thrust tracks throttle/boost. No pitch
        //      scaling anywhere, so it can never read as shifting. MODE BLIP is
        //      unchanged (a one-shot chime at a per-mode pitch).
        std::unique_ptr<x3::audio::IAudioSystem> saudio(x3::audio::createAudioSystem());
        saudio->init();
        x3::audio::SoundHandle humSnd    = saudio->load(x3::game::resolveAudio("space/engine_hum.wav"));
        x3::audio::SoundHandle thrustSnd = saudio->load(x3::game::resolveAudio("space/engine_thrust.wav"));
        x3::audio::SoundHandle blipSnd   = saudio->load(x3::game::resolveAudio("interact/chime.wav"));
        x3::audio::LoopHandle  humLoop{};
        x3::audio::LoopHandle  thrustLoop{};
        x3::audio::LoopHandle  warnLoop{};   // CRITICAL hull-temp / shield warning beep
        if (humSnd.valid())    humLoop    = saudio->startLoop(humSnd, 0.25f, 1.0f);      // FIXED pitch
        if (thrustSnd.valid()) thrustLoop = saudio->startLoop(thrustSnd, 0.0f, 1.0f);    // FIXED pitch
        x3::logInfo(std::string("--world space: engine audio ") +
                    ((humSnd.valid() && thrustSnd.valid()) ? "ON (reactor hum + thruster whoosh)" : "absent (silent)"));

        // ---- PAUSE MENU state (ESC opens it; it NO LONGER exits) -----------
        bool paused = false;
        int  menuSel = 0;
        bool prevUp = false, prevDown = false, prevEnter = false;   // prevEsc gone: ESC is a callback edge now

        // Console (~) and the FPS/stats overlay. This host is the ONE exception
        // to the shell owning ESC: it already has a bespoke four-row pause menu
        // AND a rule that ESC is a cinematic SKIP, not a pause, during the death
        // sequence. Replacing that with the shell's generic menu would lose
        // both, so the escape handler always consumes ESC and just latches the
        // edge for the host's existing logic below — the shell's own menu never
        // opens here. What this host gains is the console and the frame times.
        //
        // The latch is a real key EVENT now; the polled prevEsc pair it replaces
        // dropped any press shorter than a frame.
        HostShell shell;
        shell.attach(hc);
        bool escEdge = false;
        shell.setEscapeHandler([&]() -> bool { escEdge = true; return true; });
        bool prevAnyKey = false;   // rising-edge latch so a held key can't insta-skip

        x3::logInfo("--world space: WASD thrust, mouse look, Q/E roll, Space/Ctrl up/down, Shift boost, V camera, LMB laser, 1/2/3 mode, I=invulnerable, Esc=pause menu");
        while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
            // Cleared before the poll: the escape handler runs inside
            // glfwPollEvents(), so clearing after would wipe what it just set.
            escEdge = false;
            glfwPollEvents();
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            g_clock += fdt;                       // presentation clock (blink/pulse/flash)
            // Toggle-confirmation caption decay (independent of phase, ticks always).
            if (invulnFlashT >= 0.0f) {
                invulnFlashT += fdt;
                if (invulnFlashT > kInvulnFlashSecs) invulnFlashT = -1.0f;
            }
            // CINEMATIC STARS: the sky time used to be set ONCE (static) — the engine
            // ties it to the starfield's twinkle/rotation phase, so a fixed value
            // meant a fully frozen background. Advance it slowly (subtle: ~0.02
            // units/sec) so the backdrop drifts/twinkles too, not just the near-field
            // dust. Sim-state untouched (visual only).
            device->setSkyTime(10.0f + g_clock * 0.02f);
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };

            // Is the sun-death SEQUENCE running at all (any non-Flying phase)? Used
            // for the skip-key/pause gating below. NOTE: this is broader than the
            // FROZEN window — InsideSun is part of `seq` but keeps the pilot live
            // (graze-abort), so it is excluded from `pilotFrozen`.
            const bool seq = (phase != Phase::Flying);
            const bool pilotFrozen = seq && (phase != Phase::InsideSun);
            // "Any key" skips straight to Respawn — but ONLY from the FROZEN
            // cinematic (Detonation onward). During InsideSun the ship is still
            // live-flying (graze-abort window), so advanceSequence() ignores the
            // skip there (see its guard) and these keys steer the ship instead.
            const bool anyKey = seq && (kd(GLFW_KEY_SPACE) || kd(GLFW_KEY_ENTER) ||
                kd(GLFW_KEY_ESCAPE) || kd(GLFW_KEY_W) || kd(GLFW_KEY_A) || kd(GLFW_KEY_S) ||
                kd(GLFW_KEY_D) || kd(GLFW_KEY_Q) || kd(GLFW_KEY_E) || kd(GLFW_KEY_LEFT_SHIFT) ||
                glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);

            // ESC toggles the pause menu (rising edge) ONLY while flying — during the
            // death cinematic (incl. InsideSun) ESC is a skip, not a pause. Resync the
            // mouse anchor so resume doesn't jump the view.
            if (!seq && escEdge) {
                paused = !paused;
                glfwSetInputMode(window, GLFW_CURSOR, paused ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                lastMX = mx; lastMY = my;
            }

            if (pilotFrozen) {
                // Detonation..Respawn: freeze the pilot, advance the phase machine
                // (blast, rewind, title card, replay, respawn) + run the external
                // kill-cam. Skip is EDGE-triggered so a key still held from flying-in
                // can't insta-skip.
                boostActive = false; throttle01 = 0.0f; lastMX = mx; lastMY = my;
                advanceSequence(fdt, anyKey && !prevAnyKey);
            } else if (!paused) {
                float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
                lastMX = mx; lastMY = my;

                // Build a PlayerInput for the controller. jumpPressed re-purposed as
                // "up impulse this frame" while Space is held; sprint = boost.
                x3::game::PlayerInput in{};
                in.moveFwd    = (kd(GLFW_KEY_W) ?  1.0f : 0.0f) + (kd(GLFW_KEY_S) ? -1.0f : 0.0f);
                in.moveStrafe = (kd(GLFW_KEY_D) ?  1.0f : 0.0f) + (kd(GLFW_KEY_A) ? -1.0f : 0.0f);
                in.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
                in.jumpPressed= kd(GLFW_KEY_SPACE);   // held = rise (ship up axis)
                in.diveHeld   = kd(GLFW_KEY_C);       // held = drop (ship -up axis)
                in.lookDX     = ddx;
                in.lookDY     = ddy;
                boostActive   = in.sprint;
                throttle01    = std::max(0.0f, in.moveFwd);   // ENGINE AUDIO: thrust-layer gate

                // Q/E roll axis: +1 for Q, -1 for E (or the other way; either is fine).
                float rollAxis = (kd(GLFW_KEY_Q) ? -1.0f : 0.0f) + (kd(GLFW_KEY_E) ? 1.0f : 0.0f);
                pilot.setRollInput(rollAxis);

                // FLIGHT MODE hot-swap while flying: 1=Arcade, 2=Assist, 3=Loose (the
                // in-space equivalent of the `flightmode` console command). Writes the
                // shared latch; the change is applied + blipped below (common path).
                if (kd(GLFW_KEY_1)) x3::game::setRequestedFlightMode(x3::game::FlightMode::Arcade);
                if (kd(GLFW_KEY_2)) x3::game::setRequestedFlightMode(x3::game::FlightMode::Assist);
                if (kd(GLFW_KEY_3)) x3::game::setRequestedFlightMode(x3::game::FlightMode::Loose);

                pilot.update(in, fdt, *sphys);

                // V to toggle 1P / 3P (rising edge).
                bool vNow = kd(GLFW_KEY_V);
                if (vNow && !prevV) pilot.toggleCameraMode();
                prevV = vNow;

                // I to toggle the invulnerability cheat (rising edge — mirrors the V
                // pattern above so a held key can't re-toggle every frame). Confirmation
                // caption + a pitched mode-blip echo (higher pitch = ON, lower = OFF).
                bool iNow = kd(GLFW_KEY_I);
                if (iNow && !prevI) {
                    g_invulnerable = !g_invulnerable;
                    invulnFlashOn = g_invulnerable;
                    invulnFlashT = 0.0f;
                    x3::logInfo(std::string("--world space: invulnerability -> ") + (g_invulnerable ? "ON" : "OFF"));
                    if (blipSnd.valid()) saudio->playSound2D(blipSnd, 0.7f, g_invulnerable ? 1.6f : 0.6f);
                }
                prevI = iNow;

                // X ABORTS A TRANSIT IN FLIGHT. The abort path has to be REACHABLE,
                // not just unit-tested: a jump you cannot get out of is the exact
                // shape of a soft-lock. The transit completes on its next tick, the
                // S0 spine lands back in DeepSpace (never in limbo), AEGIS says so,
                // and the host puts the ship back beside the mouth it entered - in
                // the departure system, with the sky untouched, because you never
                // reached the far side.
                {
                    static bool prevAbort = false;
                    const bool xNow = kd(GLFW_KEY_X);
                    if (xNow && !prevAbort && transitEngaged) {
                        transit.abort();
                        x3::logInfo("--world space: transit ABORT requested (X)");
                    }
                    prevAbort = xNow;
                }

                // LMB laser (rising edge -> fire one bolt, log on success).
                bool lmbNow = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                if (lmbNow && !prevLmb && pilot.fireLaser(fdt)) {
                    const x3::phys::Vec3 pos = pilot.pos();
                    const x3::phys::Vec3 fwd = pilot.forward();
                    x3::phys::Vec3 muzzle{ pos.x + fwd.x * 2.5f,
                                           pos.y + fwd.y * 2.5f,
                                           pos.z + fwd.z * 2.5f };
                    x3::phys::Vec3 hit{ pos.x + fwd.x * 400.0f,
                                        pos.y + fwd.y * 400.0f,
                                        pos.z + fwd.z * 400.0f };
                    combatFx.addTracer(muzzle, hit);
                }
                prevLmb = lmbNow;

                sphys->step(fdt);
                combatFx.update(fdt);
                updateDust(fdt);
                // feat/wormholes: the phase machine, the AEGIS advisory and the
                // transit trigger, all off the real wall-clock fdt.
                tickWormholes(fdt, pilot.pos());
                // Flying: records the approach + detects crossing INTO the sun core
                // (flips to InsideSun, engages the shield, fires the flash). InsideSun:
                // keeps recording/heat/HUD live and detects either the 17s expiry
                // (-> Detonation) or a GRAZE-ABORT (pulled back out -> Flying). `skip`
                // only matters while phase != Flying (anyKey requires seq==true).
                advanceSequence(fdt, anyKey && !prevAnyKey);
            } else {
                // Paused: keep the mouse anchor synced (no view drift), no flight
                // sim / physics step (visuals freeze behind the menu). Menu nav:
                lastMX = mx; lastMY = my;
                boostActive = false;
                const bool upNow = kd(GLFW_KEY_UP) || kd(GLFW_KEY_W);
                const bool dnNow = kd(GLFW_KEY_DOWN) || kd(GLFW_KEY_S);
                const bool enNow = kd(GLFW_KEY_ENTER) || kd(GLFW_KEY_SPACE);
                if (upNow && !prevUp)   menuSel = (menuSel + 3) % 4;
                if (dnNow && !prevDown) menuSel = (menuSel + 1) % 4;
                if (enNow && !prevEnter) {
                    if (menuSel == 0) {                 // RESUME
                        paused = false;
                        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                        lastMX = mx; lastMY = my;
                    } else if (menuSel == 1) {          // FLIGHT MODE: cycle (same setMode path)
                        int nxt = ((int)pilot.mode() + 1) % 3;
                        x3::game::setRequestedFlightMode((x3::game::FlightMode)nxt);
                    } else if (menuSel == 2) {          // SHIELD: INVULNERABLE ON/OFF (same flag as I)
                        g_invulnerable = !g_invulnerable;
                        invulnFlashOn = g_invulnerable;
                        invulnFlashT = 0.0f;
                        if (blipSnd.valid()) saudio->playSound2D(blipSnd, 0.7f, g_invulnerable ? 1.6f : 0.6f);
                    } else {                            // QUIT TO DESKTOP
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                }
                prevUp = upNow; prevDown = dnNow; prevEnter = enNow;
            }
            prevAnyKey = anyKey;

            // ---- Common: apply a mode change from EITHER source (1/2/3 keys or the
            //      menu cycle) and BLIP a per-mode chime so the switch is audible. --
            if (x3::game::requestedFlightMode() != pilot.mode()) {
                pilot.setMode(x3::game::requestedFlightMode());
                x3::logInfo(std::string("--world space: flight mode -> ") +
                            x3::game::flightModeName(pilot.mode()));
                if (blipSnd.valid()) {
                    const float bp = (pilot.mode() == x3::game::FlightMode::Assist) ? 0.7f
                                   : (pilot.mode() == x3::game::FlightMode::Loose)  ? 1.4f : 1.0f;
                    saudio->playSound2D(blipSnd, 0.7f, bp);
                }
            }

            // ---- Flight audio: reactor hum tracks SPEED (volume only); thruster
            //      whoosh tracks THROTTLE + boost (volume only). Both loops run at a
            //      FIXED pitch of 1.0 always — no pitch sweep anywhere, so it can
            //      never read as a car shifting gears.
            {
                const float spd  = pilot.speed();
                const float maxS = std::max(1.0f, pilot.tuning().maxSpeed);
                const float sf   = std::min(1.0f, spd / maxS);
                if (humLoop.valid()) {
                    const float hv = paused ? 0.05f : (0.25f + 0.35f * sf);
                    saudio->setLoopParams(humLoop, hv, 1.0f);   // FIXED pitch
                }
                if (thrustLoop.valid()) {
                    const float thrustResp = smooth01(0.0f, 1.0f, throttle01);
                    const float tv = paused ? 0.0f
                        : std::min(0.85f, thrustResp * 0.55f + (boostActive ? 0.30f : 0.0f));
                    saudio->setLoopParams(thrustLoop, tv, 1.0f);   // FIXED pitch
                }
                // CRITICAL warning beep: a fast high chime loop when hull-temp is
                // critical on approach, or while the shield drains inside the star.
                // Suppressed entirely while invulnerable (owner: the shield insulates
                // — temp/warnings all go quiet), so no critical beep on approach or
                // while loitering inside the star.
                const bool wantWarn = !g_invulnerable &&
                                      ((phase == Phase::Flying && g_sunSurf < kCritDist) ||
                                       phase == Phase::InsideSun);
                if (wantWarn && blipSnd.valid()) {
                    if (!warnLoop.valid()) warnLoop = saudio->startLoop(blipSnd, 0.35f, 1.9f);
                } else if (warnLoop.valid()) {
                    saudio->stopLoop(warnLoop); warnLoop = {};
                }
            }

            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0 && chh>0) device->onResize((uint32_t)cw, (uint32_t)chh);

            // Re-read the kill-cam flag from the LIVE phase (advanceSequence may have
            // transitioned this frame) so the camera never lags a frame at a boundary.
            const bool cineNow = (phase == Phase::Detonation || phase == Phase::Rewind ||
                                  phase == Phase::TitleCard  || phase == Phase::Replay);
            // Camera: the EXTERNAL kill-cam during the blast/rewind/card/replay, else
            // the pilot camera (FOV widens with speed + boost per mode).
            float cx, cy, cz, cyaw, cpit;
            if (cineNow) {
                // Only the '30 SECONDS EARLIER' Replay becomes a tracking shot; the
                // Detonation/Rewind/TitleCard phases keep the frozen wide half-screen
                // vantage (the blast is big enough to read from 5 km).
                bool tracked = false;
                if (phase == Phase::Replay) {
                    tracked = replayTrackCam(phaseT / kReplaySecs, cx, cy, cz, cyaw, cpit);
                }
                if (!tracked) { cx = cineCamPos.x; cy = cineCamPos.y; cz = cineCamPos.z; cyaw = cineYaw; cpit = cinePit; }
                device->setCamera(cx, cy, cz, cyaw, cpit, tracked ? 55.0f : 60.0f);
            } else if (transitEngaged) {
                // THE RIDE CAMERA. Autopilot has the helm, so the camera does too:
                // on the tunnel axis, looking down the corridor, lens punching wide
                // under acceleration. The pilot's own camera/aim path is untouched -
                // this host simply does not ask it for a camera while the ship is
                // inside a wormhole.
                float tfov = 65.0f;
                transitCamera(cx, cy, cz, cyaw, cpit, tfov);
                device->setCamera(cx, cy, cz, cyaw, cpit, tfov);
            } else {
                pilot.camera(cx, cy, cz, cyaw, cpit);
                device->setCamera(cx, cy, cz, cyaw, cpit, pilot.fov());
            }
            saudio->setListener(cx, cy, cz, cyaw, cpit);
            saudio->update(fdt);

            // TWIN-SUN FIX: re-aim the painted sky sun down the CAMERA→body ray so its
            // infinite disc projects to the real body's screen position and is fully
            // occluded by that opaque, depth-nearer body (no more parallax orb). The
            // directional light now shades hulls from the true sun direction too. When
            // the camera is basically at the body (engulfed) the ray degenerates — keep
            // the last good dir. Cheap: SkyParams is cached POD, re-applied like lights.
            {
                const x3::phys::Vec3 toSun{ kSunCenter.x - cx, kSunCenter.y - cy, kSunCenter.z - cz };
                const float tl = vlen(toSun);
                if (tl > 1.0f) {
                    skyP.sunDir[0] = toSun.x / tl; skyP.sunDir[1] = toSun.y / tl; skyP.sunDir[2] = toSun.z / tl;
                    device->setSkyParams(skyP);
                }
            }

            // Player-key + sun-heat follow lights + the WORMHOLE SPILL (refreshed
            // each frame; the spill is why the effect lights the hull at all).
            if (transitEngaged) transitLights();
            else updateDynamicLights(pilot.pos(), pilot.forward(), pilot.up(), pilot.right());

            auto frame = device->beginFrame();
            if (frame.valid) {
                // THE TRANSIT OWNS THE FRAME. For the length of a jump the world
                // scene, the star and the wormhole field are all somewhere the
                // player no longer is: they are inside the throat. Drawing the
                // departure system's furniture over the tunnel is exactly what made
                // the old transit read as a fade with a world behind it.
                if (transitEngaged) {
                    drawTransitWorld(frame, g_clock);
                    drawTransitHud(frame, (float)cw, (float)chh);
                    if (paused) drawPauseMenu(frame, (float)cw, (float)chh, menuSel);
                    shell.draw(frame);
                    device->endFrame(frame);
                    continue;
                }
                // World: the near-field scene only when NOT on the external kill-cam.
                if (!cineNow) {
                    drawScene(frame);
                    combatFx.submit(*device, frame);
                    drawSpeedFx(frame);
                }
                drawSun(frame, x3::phys::Vec3{ cx, cy, cz });   // the star renders in every phase
                drawFlares(frame);                     // limb prominences, also every phase
                // Phase-specific world elements.
                if (phase == Phase::Flying) {
                    const float thrust01 = std::min(1.0f,
                        std::min(1.0f, pilot.speed()/std::max(1.0f, pilot.tuning().maxSpeed))
                        + (boostActive ? 0.45f : 0.0f));
                    drawShipLights(frame, thrust01, g_clock);
                    // Entry-burn sheath: only meaningful in 3rd person (the ship itself
                    // is on-screen); skipped in 1P per the ask (cockpit view has no hull
                    // to wreathe in flame).
                    if (pilot.isThirdPerson()) {
                        drawBurnSheath(frame, pilot.pos(), x3::phys::Vec3{ cx, cy, cz }, g_burnFactor);
                        drawBurnEmbers(frame, pilot.pos(), pilot.forward(), pilot.up(), pilot.right(), g_burnFactor);
                    }
                } else if (phase == Phase::InsideSun) {
                    // Swirling plasma wraps the camera; ship + shield draw over it.
                    drawInterior(frame, x3::phys::Vec3{ cx, cy, cz });
                    drawShield(frame, shieldPct, g_clock);
                    drawShipLights(frame, 0.2f, g_clock);
                    if (pilot.isThirdPerson()) {
                        drawBurnSheath(frame, pilot.pos(), x3::phys::Vec3{ cx, cy, cz }, g_burnFactor);
                        drawBurnEmbers(frame, pilot.pos(), pilot.forward(), pilot.up(), pilot.right(), g_burnFactor);
                    }
                } else if (phase == Phase::Detonation) {
                    drawEjecta(frame, phaseT);
                } else if (phase == Phase::Rewind) {
                    // Blast retracts (reverse) while the ship sits at the entry point.
                    drawEjecta(frame, kDetonateSecs * (1.0f - phaseT / kRewindSecs));
                    drawReplayShip(frame, 1.0f, cineCamPos);
                } else if (phase == Phase::Replay) {
                    drawReplayShip(frame, phaseT / kReplaySecs, x3::phys::Vec3{ cx, cy, cz });   // fly the approach in
                }
                // feat/wormholes: drawn in EVERY phase (like the sun and flares) —
                // a wormhole opening out in the black does not stop happening
                // because the player is mid-killcam.
                drawWormholes(frame, x3::phys::Vec3{ cx, cy, cz }, g_clock);
                drawHud(frame, (float)cw, (float)chh);
                if (paused) drawPauseMenu(frame, (float)cw, (float)chh, menuSel);
                drawCinematic(frame, (float)cw, (float)chh);       // no-op unless a flash/phase overlay is active
            }
            shell.draw(frame);   // console + FPS over this host's own HUD and menu
            device->endFrame(frame);
        }
        if (warnLoop.valid())  saudio->stopLoop(warnLoop);
        if (thrustLoop.valid()) saudio->stopLoop(thrustLoop);
        if (humLoop.valid())    saudio->stopLoop(humLoop);
        saudio->shutdown();
        combatFx.shutdown(*device);
        transit.shutdown(*device);
        wormholes.shutdown(*device);
        // glowDiscMesh was destroyed only on the headless path; this list had
        // been missing it since the sun pass. Added while adding ours, because
        // allocationCount=0 at teardown is a gate condition.
        device->destroyMesh(glowDiscMesh);
        device->destroyMesh(dustMesh); device->destroyMesh(sunMesh);
        device->destroyMesh(plasmaMeshA); device->destroyMesh(plasmaMeshB);
        device->destroyTexture(sunTex);
        device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
        if (shipModel.ok) mloader->unload(shipModel);
        sphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }
    return -1;
}

}} // namespace x3::apphost
