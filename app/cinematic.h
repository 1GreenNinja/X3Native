#pragma once
// ===========================================================================
// COLD-OPEN CINEMATIC + NIGHT-SKY PLANETS — host rendering helpers moved
// VERBATIM out of app/main.cpp (#28 monolith split). The NightSkyPlanet data +
// the CinematicScene/CinAudioMap classes are used by value in several main()
// world hosts, so their definitions stay here (inline, unchanged); the heavy
// free functions (loadNightSkyPlanets/drawNightSkyPlanets/runCutsceneWindowed)
// are declared here and defined in cinematic.cpp.
// ===========================================================================
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/core/x3_log.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "engine/audio/IAudioSystem.h"
#include "cutscene.h"
#include "mesh_prims.h"
#include "asset_root.h"
#include "audio_root.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace x3::apphost {

// ---- Shared NIGHT-SKY planet helper -------------------------------------
// One spot to build the UV-sphere + load the 6 FORGE3D planet types (Moon, Ice,
// Gas, Lava, Terrestrial, Sun) — the SAME files / slot order / srgb flags the
// --screenshot-nightsky block has always used — and draw them via drawPlanet().
// Used by BOTH --screenshot-nightsky and the NIGHT --screenshot-showroom path so
// the planet recipe isn't duplicated.
//
// CELESTIAL placement: each body is specified by a WORLD-SPACE sky DIRECTION
// (azimuth/elevation) + an APPARENT angular diameter — NOT a world position.
// drawNightSkyPlanets() re-anchors every body on the CAMERA EYE each draw:
//   pos = eye + dir(az,el) * kNightSkyDist,  radius = dist * tan(diam/2)
// so the bodies are TRANSLATION-INVARIANT (zero parallax — they never "approach"
// the building as the player walks) while still rotating correctly with the view,
// exactly like real celestial bodies. Azimuth 0 = -Z, +90 = +X (engine yaw - 90°).
struct NightSkyPlanet {
    uint32_t                            typeIndex;   // 0=Moon 1=Ice 2=Gas 3=Lava 4=Terrestrial 8=Sun
    std::vector<x3::rhi::TextureHandle> maps;        // pc.tex[] slot order for the type
    float                               azimuthDeg;   // world-space sky azimuth (0 = -Z, +90 = +X)
    float                               elevationDeg; // above the horizon (keep >= ~12 so nothing rides the roofline)
    float                               angularDiameterDeg; // apparent size (full disc, degrees)
    const char*                         name;        // log label
    // ---- TRANSPARENT glow layers (additive atmosphere / sun corona; alpha ring).
    // Each is OPTIONAL: a valid texture handle enables that layer for this body. The
    // shells reuse the SAME sphere mesh as the body, scaled up; the ring uses a flat
    // annulus mesh (passed separately to drawNightSkyPlanets). Type indices match
    // PlanetType: Atmosphere=9, SunCorona=10, Ring=11.
    x3::rhi::TextureHandle              atmoTex{};   // Atmosphere shell ramp (tex[0]); inflated sphere
    x3::rhi::TextureHandle              coronaTex{}; // SunCorona map (tex[0]); inflated sphere, animated
    x3::rhi::TextureHandle              ringTex{};   // Ring radial strip (tex[0]); flat annulus
};

// Anchor distance for every celestial body: 70% of the 200 m far plane (see the
// glm::perspective in VulkanRenderDevice). The largest body (gas giant, 9° ->
// r ~= 11 m, ring out to 2.5r ~= 27.5 m) stays comfortably inside the far plane
// (worst point ~168 m) and well past all world geometry, so depth-test LESS
// occludes it behind the spire/terrain at the horizon while it still draws OVER
// the far-depth sky dome.
inline constexpr float kNightSkyDist = 140.0f;

// Build the UV-sphere (writes `outMesh`) + load every planet's textures, returning
// the list of bodies with their default sky layout (azimuth/elevation/angular
// diameter — see the table below; THE one spot to tune the night sky). `nTexFail`
// is incremented per missing file. The texture cache de-dupes shared maps.
// `logTag` prefixes the load logs so the calling path is clear.
//
//   body         az(deg)  el(deg)  diam(deg)  role
//   Sun            +28       16       3.5     low-ish, clear of the terrain silhouette, 50° off the hero
//   Terrestrial    -22       22       7.0     THE HERO — big, upper-left, moonlit half-phase
//   Gas (rings)   -147       24       9.0     other side of the sky (125° from the hero)
//   Moon           -44       30       2.5     small, above-left of the hero
//   Ice            -85       45       2.0     small, high far-left
//   Lava           +47       27       1.5     small, upper-right (everything >= ~12° min elevation)

// Build the UV-sphere + load every planet's textures (see cinematic.cpp).
std::vector<NightSkyPlanet> loadNightSkyPlanets(
        x3::rhi::IRenderDevice* device, x3::rhi::MeshHandle& outMesh,
        int& nTexFail, const char* logTag,
        x3::rhi::MeshHandle* outRingMesh = nullptr);

// Draw every planet for the current frame (see cinematic.cpp).
void drawNightSkyPlanets(x3::rhi::IRenderDevice* device, const x3::rhi::FrameContext& fc,
                         x3::rhi::MeshHandle mesh,
                         const std::vector<NightSkyPlanet>& planets, float uTime,
                         float eyeX, float eyeY, float eyeZ,
                         x3::rhi::MeshHandle ringMesh = {});

// ============================================================================
// COLD-OPEN CINEMATIC DRIVER (x3.cutscene/1 — app/cutscene.h holds the pure
// data/eval/player; THIS is the windowed/headless 3D scene that renders it).
// The film: Jake's ship over the planet, the capital-ship ambush, the shoot-
// down, smash to black, the title cards — then the host hands off to the cell.
// ============================================================================

// One loaded cutscene actor: either a GLB (drawables + size-normalization) or a
// builtin emissive primitive (beam = unit box, glow = unit sphere).
struct CinActorState {
    const x3::cut::Actor* def = nullptr;
    x3::asset::Model model;                              // GLB actors only
    std::vector<x3::asset::ModelDrawable> drawables;
    float normScale = 1.0f;
    bool  builtin   = false;                             // uses the shared box/sphere
    bool  isBeam    = false;
};

class CinematicScene {
public:
    // Build everything the film needs on `device`: the FORGE3D planet bodies
    // (re-positioned for the cold-open sky), the ship GLBs, and the builtin FX
    // prims. Headless-safe (createMesh/createTexture work offscreen too).
    bool load(x3::rhi::IRenderDevice& device, const x3::cut::Cutscene& cs) {
        m_src.reset(x3::asset::createAssetSource());
        m_src->mountDir(x3::game::assetRoot(), 0);
        m_loader.reset(x3::asset::createModelLoader(&device, m_src.get()));

        // ---- Builtin FX prims (shared unit box + unit sphere) ----
        {
            x3::prims::PrimMesh box = x3::prims::makeBox(0.5f, 0.5f, 0.5f, 0, 0, 0);
            m_box = device.createMesh(box.verts.data(), (uint32_t)box.verts.size(),
                                      box.index.data(), (uint32_t)box.index.size());
            x3::prims::PrimMesh sph = x3::prims::makeUVSphere(24, 48);
            m_sphere = device.createMesh(sph.verts.data(), (uint32_t)sph.verts.size(),
                                         sph.index.data(), (uint32_t)sph.index.size());
        }

        // ---- Actors ----
        for (const x3::cut::Actor& a : cs.actors) {
            CinActorState st;
            st.def = &a;
            if (a.model.rfind("builtin:", 0) == 0) {
                st.builtin = true;
                st.isBeam = (a.model == "builtin:beam");
            } else {
                st.model = m_loader->load(a.model);
                if (st.model.ok) {
                    st.drawables = x3::asset::makeDrawables(st.model);
                    // Size casting: normalize the longest mesh-space axis to a.size,
                    // FOLDING IN any scale baked into the GLB node transforms (some
                    // converted ships carry e.g. a 0.1x node scale — without this the
                    // capital ship rendered 10x too small).
                    if (a.size > 0.0f) {
                        float mn[3], mx[3];
                        if (x3::cut::glbPositionExtent(x3::game::assetRoot() + "/" + a.model, mn, mx)) {
                            const float ext = std::max({ mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2] });
                            float nodeScale = 0.0f;
                            for (const auto& d : st.drawables) {
                                const float* m = d.nodeTransform;
                                for (int c = 0; c < 3; ++c) {
                                    const float len = std::sqrt(m[c*4+0]*m[c*4+0] + m[c*4+1]*m[c*4+1] +
                                                                m[c*4+2]*m[c*4+2]);
                                    nodeScale = std::max(nodeScale, len);
                                }
                            }
                            if (nodeScale <= 1e-4f) nodeScale = 1.0f;
                            const float effExt = ext * nodeScale;
                            if (effExt > 1e-4f) st.normScale = a.size / effExt;
                        }
                    }
                    x3::logInfo("[cutscene] actor '" + a.id + "' loaded " + a.model + " (" +
                                std::to_string(st.drawables.size()) + " drawables, normScale=" +
                                std::to_string(st.normScale) + ")");
                } else {
                    x3::logError("[cutscene] actor '" + a.id + "' FAILED to load " + a.model +
                                 " — drawn as a builtin glow stand-in");
                    st.builtin = true;
                }
            }
            m_actors.push_back(std::move(st));
        }

        // ---- Planet sky (FORGE3D bodies as DIRECTION-ANCHORED celestial layers).
        // The camera far plane is 200 m, so real distant placement would clip the
        // bodies entirely. Instead each body stores a DIRECTION + apparent size and
        // drawWorld() re-projects it at a fixed 150 m from the live camera every
        // frame — parallax-free sky bodies (the fix/planets-sky technique). ----
        int nTexFail = 0;
        std::vector<NightSkyPlanet> all =
            loadNightSkyPlanets(&device, m_planetMesh, nTexFail, "[cutscene]", &m_ringMesh);
        auto anchor = [&](NightSkyPlanet& b, float dx, float dy, float dz, float angSin) {
            const float len = std::sqrt(dx*dx + dy*dy + dz*dz);
            CelAnchor a;
            a.dir[0] = dx / len; a.dir[1] = dy / len; a.dir[2] = dz / len;
            a.angSin = angSin;
            m_anchors.push_back(a);
            m_planets.push_back(b);
        };
        for (NightSkyPlanet& b : all) {
            const std::string n = b.name ? b.name : "";
            if (n == "Terrestrial")        // the HOME PLANET rising below the action (~30 deg wide)
                anchor(b, 0.0f, -0.55f, -0.84f, 0.26f);
            else if (n == "Sun")           // BEHIND the flight path — the ambush comes out of the sun
                anchor(b, 0.10f, 0.18f, 0.97f, 0.05f);
            else if (n == "Gas")           // ringed giant, ahead-right
                anchor(b, 0.40f, 0.11f, -0.91f, 0.085f);
            else if (n == "Moon")          // accent, ahead-left
                anchor(b, -0.39f, 0.13f, -0.91f, 0.05f);
            // Ice / Lava deliberately dropped — keep the sky composed, not cluttered.
        }
        if (nTexFail > 0)
            x3::logInfo("[cutscene] " + std::to_string(nTexFail) +
                        " planet texture(s) missing — bodies may render flat (graceful)");
        return true;
    }

    // The space look. Mirrors the nightsky recipe but with a REAL key sun from
    // behind the flight path (the silhouette beats) + a lifted cool fill so the
    // hulls read. SSAO/GI untouched (mostly-empty depth; harmless).
    void applyLook(x3::rhi::IRenderDevice& device) {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        // RAKING KEY SUN (menace-relight). The MESH directional sun's color is a
        // hardcoded full-strength constant (shaders/mesh.frag:156, kSunColor); only
        // its DIRECTION comes from here (SkyParams.sunDir), and sunIntensity scales
        // ONLY the analytic sky dome/disc — NOT the hulls. The old dir (0.10,0.18,0.97)
        // was nearly axial with the reveal camera, so the capital's camera-facing hull
        // got a flat, near-frontal light with no modeling (a pale slab). We now RAKE the
        // key up + to the right (still generally BEHIND, +Z, so the "ambush out of the
        // sun" backlight reads) so one flank/top edge takes a hard sunlit KICKER while
        // the bulk falls to shadow — a menacing dark mass with a bright rim. The visible
        // sun DISC is a separate FORGE3D body anchored in load() (unmoved), so raking the
        // lighting does not move the on-screen sun.
        sp.sunDir[0] = 0.70f; sp.sunDir[1] = 0.45f; sp.sunDir[2] = 0.55f;   // hard side-top rake
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.95f; sp.sunColor[2] = 0.86f;
        // sunIntensity scales ONLY the analytic sky disc/glow, NOT the hulls (kSunColor
        // is full-strength in mesh.frag). Set to 0 so raking the KEY direction for hull
        // sculpting does not spawn a stray second sun disc — the on-screen "sun" is the
        // FORGE3D body anchored in load(); the ambush still comes out of it.
        sp.sunIntensity = 0.0f;
        sp.haze = 0.0f; sp.exposure = 1.0f;
        sp.zenith[0]  = 0.004f; sp.zenith[1]  = 0.004f; sp.zenith[2]  = 0.010f;
        sp.horizon[0] = 0.008f; sp.horizon[1] = 0.010f; sp.horizon[2] = 0.020f;
        device.setSkyParams(sp);
        // Lower + cooler starlight fill so the SHADOW side reads as a dark hull (not a
        // pale wash) — the raking sun + the running lights carry the form. (Was 0.17/
        // 0.18/0.25, which — with the old flat emissive — flattened the ship to clay.)
        device.setAmbient(0.085f, 0.095f, 0.135f);     // cool starlight fill — dark-side lift only
        device.setBloom(0.34f);                        // hero glow: engines / red running lights / bolts
    }

    // Restore the device state the cutscene touched to the engine defaults the
    // cell build expects (sky OFF, default ambient/bloom, no point lights).
    static void restoreLook(x3::rhi::IRenderDevice& device) {
        x3::rhi::IRenderDevice::SkyParams sp{};       // enabled = false
        device.setSkyParams(sp);
        device.setAmbient(0.42f, 0.44f, 0.50f);       // device default
        device.setBloom(0.06f);                       // device default (kBloomIntensity)
        device.setSkyTime(0.0f);
        device.setPointLights(nullptr, 0);
    }

    // x3.fire event hook (driver-reserved fx.* names; everything else is host's).
    void onEvent(const std::string& name, const x3::cut::Cutscene& cs, float t) {
        if (name.rfind("fx.trail.start:", 0) == 0) {
            m_trailActor = name.substr(15);
            m_trailOn = true;
            m_lastPuff = t;
        } else if (name.rfind("fx.trail.stop:", 0) == 0) {
            m_trailOn = false;
        } else if (name.rfind("fx.impact:", 0) == 0) {
            const std::string id = name.substr(10);
            if (const x3::cut::Actor* a = findActor(cs, id)) {
                const x3::cut::ActorPose p = x3::cut::evalActor(cs, *a, t);
                // A deterministic burst of glowing debris around the impact point —
                // SMALL and scattered (big merged spheres read as a solid blob).
                for (int i = 0; i < 6; ++i) {
                    const float fi = (float)i;
                    Puff pf;
                    pf.born = t;
                    pf.x = p.pos.x + std::sin(fi * 2.4f) * 4.5f;
                    pf.y = p.pos.y + std::cos(fi * 1.7f) * 3.5f;
                    pf.z = p.pos.z + std::sin(fi * 3.1f + 1.0f) * 4.5f;
                    pf.hot = true;
                    pushPuff(pf);
                }
            }
        }
    }

    // Per-frame: advance the smoke/debris trail (gpu-light CPU puffs).
    void update(const x3::cut::Cutscene& cs, float t) {
        if (m_trailOn) {
            if (const x3::cut::Actor* a = findActor(cs, m_trailActor)) {
                const x3::cut::ActorPose p = x3::cut::evalActor(cs, *a, t);
                if (!p.visible) {
                    m_trailOn = false;
                } else {
                    while (t - m_lastPuff >= 0.12f) {
                        m_lastPuff += 0.12f;
                        Puff pf;
                        pf.born = m_lastPuff;
                        const x3::cut::ActorPose q = x3::cut::evalActor(cs, *a, m_lastPuff);
                        pf.x = q.pos.x; pf.y = q.pos.y; pf.z = q.pos.z;
                        pf.hot = false;
                        pushPuff(pf);
                    }
                }
            }
        }
        // Prune dead puffs (embers gutter at 2.4 s; impact bursts die hot at 1.3 s
        // so they never linger as unlit black balls).
        m_puffs.erase(std::remove_if(m_puffs.begin(), m_puffs.end(),
                                     [t](const Puff& p) { return t - p.born > (p.hot ? 1.3f : 2.4f); }),
                      m_puffs.end());
    }

    // Reset transient FX (trail/puffs) — used when scrubbing (--cuetime).
    void resetFx() { m_puffs.clear(); m_trailOn = false; }

    // Draw the 3D world for time t (camera already set by the caller).
    void drawWorld(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& fc,
                   const x3::cut::Cutscene& cs, float t) {
        if (!fc.valid) return;
        // Actors.
        for (CinActorState& st : m_actors) {
            const x3::cut::Actor& a = *st.def;
            const x3::cut::ActorPose pose = x3::cut::evalActor(cs, a, t);
            if (!pose.visible) continue;
            float obj[16];
            x3::cut::actorMatrix(a, pose, st.normScale, obj);
            if (st.builtin) {
                const x3::rhi::MeshHandle mesh = st.isBeam ? m_box : m_sphere;
                if (mesh.valid())
                    device.drawMeshEmissive(fc, mesh, {}, a.color, pose.emissive, obj);
            } else {
                for (const auto& d : st.drawables) {
                    float fin[16];
                    x3::asset::mulMat4(obj, d.nodeTransform, fin);
                    const float* emis = nullptr;
                    float emBuf[4];
                    // Time-evaluated emissive (the blob->detailed reveal ramp); when an
                    // actor has no ramp this equals the static a.emissive (legacy).
                    if (pose.emissive[3] > 0.0f) { std::copy(pose.emissive, pose.emissive + 4, emBuf); emis = emBuf; }
                    else {
                        emBuf[0] = d.emissiveFactor[0]; emBuf[1] = d.emissiveFactor[1];
                        emBuf[2] = d.emissiveFactor[2]; emBuf[3] = 1.0f;
                        emis = emBuf;
                    }
                    device.drawMeshPBR(fc, x3::rhi::MeshHandle{ d.meshId },
                                       x3::rhi::TextureHandle{ d.baseColorTexId },
                                       x3::rhi::TextureHandle{ d.normalTexId },
                                       x3::rhi::TextureHandle{ d.mrTexId },
                                       d.baseColorFactor, emis, fin,
                                       d.alphaMask, d.alphaBlend,
                                       x3::rhi::TextureHandle{ d.emissiveTexId },
                                       x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale);
                }
            }
        }
        // Burning-debris trail: SMALL ember specks that flare hot then SHRINK away
        // (growing opaque spheres read as a cartoon ball-chain — these gutter out).
        for (const Puff& p : m_puffs) {
            const float age = t - p.born;
            const float k = std::min(1.0f, age / 2.4f);
            const float s = std::max(0.05f, (p.hot ? 1.0f + k * 1.4f : 0.5f - 0.35f * k));
            const float m[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0, p.x, p.y, p.z, 1 };
            const float grey = 0.07f * (1.0f - 0.6f * k);
            const float col[4] = { grey, grey * 0.95f, grey * 0.9f, 1.0f };
            const float heat = p.hot ? std::max(0.0f, 1.0f - age * 1.4f) : std::max(0.0f, 1.0f - age * 0.9f);
            const float emis[4] = { 2.6f * heat, 1.0f * heat, 0.30f * heat, 3.2f * heat };
            if (m_sphere.valid()) device.drawMeshEmissive(fc, m_sphere, {}, col, emis, m);
        }
        // Planets LAST (depth-tested against the ships), re-projected each frame as
        // DIRECTION-ANCHORED sky bodies 150 m from the live camera (far plane 200 m).
        {
            const x3::cut::CamPose cam = x3::cut::evalCamera(cs, t);
            // FOLD FIX: fix/planets-sky reworked NightSkyPlanet to carry
            // azimuth/elevation/angularDiameter (the body is re-anchored on the
            // camera eye INSIDE drawNightSkyPlanets) — the old worldPos/radius
            // fields the cold-open set are gone. Convert each CelAnchor direction
            // to az/el (az 0 = -Z, +90 = +X; matches drawNightSkyPlanets) + an
            // apparent angular diameter from angSin, then call the eye-anchored API.
            constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
            for (size_t i = 0; i < m_planets.size() && i < m_anchors.size(); ++i) {
                const CelAnchor& a = m_anchors[i];
                m_planets[i].azimuthDeg         = std::atan2(a.dir[0], -a.dir[2]) * kRadToDeg;
                m_planets[i].elevationDeg       = std::asin(std::max(-1.0f, std::min(1.0f, a.dir[1]))) * kRadToDeg;
                m_planets[i].angularDiameterDeg = 2.0f * std::asin(std::max(0.0f, std::min(1.0f, a.angSin))) * kRadToDeg;
            }
            drawNightSkyPlanets(&device, fc, m_planetMesh, m_planets, 10.0f + t * 0.02f,
                                cam.pos.x, cam.pos.y, cam.pos.z, m_ringMesh);
        }
    }

    // Draw the 2D overlay for time t: letterbox, fade, title cards.
    void drawOverlay(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& fc,
                     const x3::cut::Cutscene& cs, float t) {
        if (!fc.valid) return;
        uint32_t W = 0, H = 0;
        device.hudSize(W, H);
        const float fw = (float)W, fh = (float)H;
        // Letterbox bars.
        const float lb = x3::cut::evalLetterbox(cs, t);
        if (lb > 0.0f) {
            const float black[4] = { 0, 0, 0, 1 };
            device.drawHudQuad(fc, 0, 0, fw, fh * lb, black);
            device.drawHudQuad(fc, 0, fh * (1.0f - lb), fw, fh * lb, black);
        }
        // Full-screen fade.
        float fade[4];
        x3::cut::evalFade(cs, t, fade);
        if (fade[3] > 0.001f) device.drawHudQuad(fc, 0, 0, fw, fh, fade);
        // Title cards (stacked in file order if simultaneous).
        int active = 0;
        for (const x3::cut::TitleCard& tc : cs.titles) {
            const float a = x3::cut::evalTitleAlpha(tc, t);
            if (a <= 0.001f) continue;
            x3::rhi::FontRole role = x3::rhi::FontRole::Title;
            if (tc.font == "menu") role = x3::rhi::FontRole::Menu;
            else if (tc.font == "news") role = x3::rhi::FontRole::News;
            else if (tc.font == "mono") role = x3::rhi::FontRole::Console;
            const float px = tc.sizeFrac * (fw < fh ? fw : fh);
            const float adv = device.textAdvance(role, tc.text.c_str(), px);
            const float col[4] = { tc.color[0], tc.color[1], tc.color[2], a };
            device.drawHudTextF(fc, role, tc.text.c_str(),
                                fw * 0.5f - adv * 0.5f,
                                fh * 0.5f - px * 0.5f + (float)active * px * 1.6f,
                                px, col);
            ++active;
        }
    }

    // Free everything we created (meshes; loader-owned GPU handles via unload).
    void destroy(x3::rhi::IRenderDevice& device) {
        for (CinActorState& st : m_actors)
            if (!st.builtin && st.model.ok) m_loader->unload(st.model);
        m_actors.clear();
        auto kill = [&](x3::rhi::MeshHandle& h) { if (h.valid()) { device.destroyMesh(h); h = {}; } };
        kill(m_box); kill(m_sphere); kill(m_planetMesh); kill(m_ringMesh);
        // NightSkyPlanet textures are device-owned createTexture handles; the device
        // frees them at shutdown — same lifetime stance as the nightsky/showroom paths.
        m_planets.clear();
        m_loader.reset();
        m_src.reset();
    }

private:
    struct Puff { float born = 0; float x = 0, y = 0, z = 0; bool hot = false; };

    static const x3::cut::Actor* findActor(const x3::cut::Cutscene& cs, const std::string& id) {
        for (const auto& a : cs.actors) if (a.id == id) return &a;
        return nullptr;
    }
    void pushPuff(const Puff& p) {
        if (m_puffs.size() >= 160) m_puffs.erase(m_puffs.begin());
        m_puffs.push_back(p);
    }

    std::unique_ptr<x3::asset::IAssetSource> m_src;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    std::vector<CinActorState> m_actors;
    x3::rhi::MeshHandle m_box{}, m_sphere{};
    x3::rhi::MeshHandle m_planetMesh{}, m_ringMesh{};
    struct CelAnchor { float dir[3] = {0, 0, -1}; float angSin = 0.05f; };
    std::vector<NightSkyPlanet> m_planets;
    std::vector<CelAnchor> m_anchors;     // parallel to m_planets (direction + apparent size)
    std::vector<Puff> m_puffs;
    std::string m_trailActor;
    bool  m_trailOn  = false;
    float m_lastPuff = 0.0f;
};

// Map the cutscene's named audio cues onto loaded sounds (graceful misses: a
// missing pack WAV plays silent — same stance as the rest of the slice).
struct CinAudioMap {
    x3::audio::IAudioSystem* audio = nullptr;
    x3::audio::SoundHandle alarm{}, rumble{}, charge{}, bolt{}, boom{};
    std::string musicPath;

    void init(x3::audio::IAudioSystem* a) {
        audio = a;
        if (!audio) return;
        alarm  = audio->load(x3::game::resolveAudio("Sci-fi Evolution Gift Pack/Alarm.wav"));
        rumble = audio->load(x3::game::resolveAudio("Free Pack/Explosion 2.wav"));
        charge = audio->load(x3::game::resolveAudio("weapons/loops/Vefects_Zap_Medium_01.wav"));
        bolt   = audio->load(x3::game::resolveAudio("weapons/single/Single_Gunshot_Sci-Fi_Gun-66.wav"));
        boom   = audio->load(x3::game::resolveAudio("Free Pack/Explosion 1.wav"));
        musicPath = x3::game::resolveAudio("Sci-Fi Music Pack 1/Loops/SMP1_LOOP_Zero8 _1.wav");
    }
    void fire(const x3::cut::AudioCue& cue) {
        if (!audio) return;
        if (cue.music) {
            if (cue.sound == "music.stop") audio->stopMusic();
            else audio->playMusic(musicPath, /*loop=*/true, cue.gain);
            return;
        }
        if (cue.sound == "music.stop")           { audio->stopMusic(); return; }
        x3::audio::SoundHandle h{};
        if      (cue.sound == "alarm")           h = alarm;
        else if (cue.sound == "rumble.capital")  h = rumble;
        else if (cue.sound == "charge")          h = charge;
        else if (cue.sound == "bolt")            h = bolt;
        else if (cue.sound == "explosion")       h = boom;
        audio->playSound2D(h, cue.gain);
    }
};

// Run a cutscene WINDOWED to completion (blocking) — see cinematic.cpp.
//
// CLIP-SPLIT (Phase 5): play only the span [startAt, stopAt) of the timeline, then
// return — this is how the Intro Orchestrator carves the single cold-open timeline
// into named clip BEATS (cine.flight / cine.reveal / cine.charge / cine.outcome)
// with interactive windows occupying the gaps. stopAt <= 0 (or > duration) plays to
// the end as before (the passive-film path is unchanged). K still skips: a skip in
// a clip jumps to the clip end (the span's skip target) rather than the whole film.
bool runCutsceneWindowed(x3::rhi::IRenderDevice& device, GLFWwindow* window,
                         x3::audio::IAudioSystem* audio,
                         const x3::cut::Cutscene& cs, float startAt = 0.0f,
                         const std::function<void(const std::string&)>& hostEvent = {},
                         float stopAt = 0.0f);

} // namespace x3::apphost
