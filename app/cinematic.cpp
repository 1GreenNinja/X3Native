// ===========================================================================
// COLD-OPEN CINEMATIC + NIGHT-SKY PLANETS — free-function definitions moved
// VERBATIM out of app/main.cpp (#28 monolith split). Bodies unchanged; only
// the `inline` keyword dropped (now out-of-line in this TU) and the enclosing
// namespace set to x3::apphost.
// ===========================================================================
#include "cinematic.h"

// stb_image for loadNightSkyPlanets' stbi_load (FORGE3D planet PNGs). The engine
// already hosts a FILE-LOCAL STB_IMAGE_IMPLEMENTATION in ModelLoader.cpp, and
// app/main.cpp hosts its own STB_IMAGE_STATIC copy for the GIF tool, so this TU
// instantiates its OWN file-local copy (no symbol clash) — exactly as main.cpp
// did before the split.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <cstring>
#include <unordered_map>

namespace x3::apphost {

std::vector<NightSkyPlanet> loadNightSkyPlanets(
        x3::rhi::IRenderDevice* device, x3::rhi::MeshHandle& outMesh,
        int& nTexFail, const char* logTag,
        x3::rhi::MeshHandle* outRingMesh) {
    const std::string kPlanets = "C:/Users/Tim/X3/Assets/FORGE3D/Planets/";
    const std::string kAtmo    = kPlanets + "Atmosphere/";
    const std::string kMisc    = kPlanets + "Misc/Textures/";

    std::unordered_map<std::string, x3::rhi::TextureHandle> texCache;
    auto loadTex = [&](const std::string& path, bool srgb) -> x3::rhi::TextureHandle {
        std::string key = path + (srgb ? "#s" : "#l");
        auto it = texCache.find(key);
        if (it != texCache.end()) return it->second;
        int w = 0, h = 0, comp = 0;
        stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);   // force RGBA8
        x3::rhi::TextureHandle handle{};
        if (!px) {
            x3::logError(std::string(logTag) + ": FAILED to load " + path);
            ++nTexFail;
        } else {
            handle = device->createTexture(px, (uint32_t)w, (uint32_t)h, srgb);
            stbi_image_free(px);
            x3::logInfo(std::string(logTag) + ": loaded " + path + " (" + std::to_string(w) + "x" +
                        std::to_string(h) + (srgb ? ", srgb)" : ", linear)"));
        }
        texCache[key] = handle;
        return handle;
    };

    std::vector<NightSkyPlanet> bodies;
    // Moon (type 0): tex[0]=Albedo(s) [1]=Normal(l) [2]=Detail(s) [3]=Spec(l) [4]=Scatter(s)
    {
        const std::string p = kPlanets + "Moon/Textures/";
        bodies.push_back({ 0u, {
            loadTex(p + "moon_02.png",        true),
            loadTex(p + "moon_02_normal.png", false),
            loadTex(p + "moon_01_detail.png", true),
            loadTex(p + "moon_02_spec.png",   false),
            loadTex(kAtmo + "sunset_yellow_05.png", true),
        }, /*az*/ -44.0f, /*el*/ 30.0f, /*diam*/ 2.5f, "Moon" });
    }
    // Ice (type 1): tex[0]=ColorMap(s) [1]=Normal(l) [2]=Height(l) [3]=Detail(l) [4]=Scatter(s)
    {
        const std::string p = kPlanets + "Ice/Textures/";
        bodies.push_back({ 1u, {
            loadTex(p + "ColorMapSqr.png",  true),
            loadTex(p + "ice_01_normal.png", false),
            loadTex(p + "ice_01.png",        false),
            loadTex(p + "icedetail_01.png",  false),
            loadTex(kAtmo + "sunset_blue_03.png", true),
        }, /*az*/ -85.0f, /*el*/ 45.0f, /*diam*/ 2.0f, "Ice" });
    }
    // Gas (type 2): tex[0]=HeightBands(s) [1]=UVDistortion(l) [2]=Scatter(s)
    {
        const std::string p = kPlanets + "Gas/Textures/";
        bodies.push_back({ 2u, {
            loadTex(p + "planet_gas_03.png",  true),
            loadTex(p + "planet_gas_08.png",  false),
            loadTex(kAtmo + "sunset_yellow_01.png", true),
        }, /*az*/ -147.0f, /*el*/ 24.0f, /*diam*/ 9.0f, "Gas" });
    }
    // Lava (type 3): tex[0]=Height(s) [1]=Detail(l) [2]=Magma(l) [3]=Normal(l) [4]=Distortion(s) [5]=Scatter(s)
    {
        const std::string lp = kPlanets + "Lava/Textures/";
        const std::string ip = kPlanets + "Ice/Textures/";
        bodies.push_back({ 3u, {
            loadTex(lp + "lava_01.png",        true),
            loadTex(lp + "lavadetail_01.png",  false),
            loadTex(lp + "lavadetail_01.png",  false),
            loadTex(ip + "ice_04_normal.png",  false),
            loadTex(lp + "lavadistmap.png",    true),
            loadTex(kAtmo + "sunset_red_04.png", true),
        }, /*az*/ 47.0f, /*el*/ 27.0f, /*diam*/ 1.5f, "Lava" });
    }
    // Terrestrial (type 4): tex[0]=Height(s) [1]=LandMask(l) [2]=Normal(l) [3]=Scatter(s)
    //   [4]=Gradient(l) [5]=CloudsTop(s) [6]=CloudsMiddle(s) [7]=CityLight(l)
    //   [8]=CityLightUV(l) [9]=CityLightMask(l)
    {
        const std::string p = kPlanets + "Terrestrial/Textures/";
        bodies.push_back({ 4u, {
            loadTex(p + "terrestrialdetail_01.png",        true),
            loadTex(p + "landmask_01.png",                 false),
            loadTex(p + "terrestrialdetail_01_normal.png", false),
            loadTex(kAtmo + "sunset_yellow_05.png",        true),
            loadTex(kMisc + "polegradient_01.png",         false),
            loadTex(p + "cloudscap_01.png",                true),
            loadTex(p + "clouds_01.png",                   true),
            loadTex(p + "lights_01.png",                   false),
            loadTex(p + "lights_01_uv.png",                false),
            loadTex(p + "lights_01_mask.png",              false),
        }, /*az*/ -22.0f, /*el*/ 22.0f, /*diam*/ 7.0f, "Terrestrial" });
    }
    // Sun (type 8): tex[0]=SurfaceMap(l) [1]=DistortionMap(l) — emissive, small+bright.
    {
        const std::string sp = kPlanets + "Sun/Textures/";
        const std::string tp = kPlanets + "Thunderstorm/Textures/";
        bodies.push_back({ 8u, {
            loadTex(sp + "sunsurface_01.png", false),
            loadTex(tp + "storm_02.png",      false),
        }, /*az*/ 28.0f, /*el*/ 16.0f, /*diam*/ 3.5f, "Sun" });
    }

    // ---- TRANSPARENT glow layers (additive atmosphere + sun corona; alpha ring) ----
    // Load the three extra maps + attach them to the matching bodies so the shells
    // draw with the same world position as their body. Texture roles per the port
    // headers + TEXTURE_MANIFEST:
    //   Atmosphere shell : _AtmosphereSample = Atmosphere/Atmosphere_01.png horizon
    //                      gradient ramp (sRGB, CLAMP_TO_EDGE). On the Terrestrial.
    //   Sun corona       : _CoronaMap = Sun/Textures/suncorona_01.png grayscale flow
    //                      atlas (LINEAR). On the Sun (animated via uTime).
    //   Ring             : _DetailMap = Gas/Textures/ring_01.png radial strip (sRGB,
    //                      CLAMP_TO_EDGE; RGB=color, R=alpha). Around the Gas giant.
    x3::rhi::TextureHandle atmoTex   = loadTex(kAtmo + "Atmosphere_01.png", true);
    x3::rhi::TextureHandle coronaTex = loadTex(kPlanets + "Sun/Textures/suncorona_01.png", false);
    x3::rhi::TextureHandle ringTex   = loadTex(kPlanets + "Gas/Textures/ring_01.png", true);
    for (NightSkyPlanet& b : bodies) {
        if (b.typeIndex == 4u) b.atmoTex   = atmoTex;    // Terrestrial -> atmosphere shell
        if (b.typeIndex == 8u) b.coronaTex = coronaTex;  // Sun         -> corona halo
        if (b.typeIndex == 2u) b.ringTex   = ringTex;    // Gas         -> ring annulus
    }

    // ONE UV-sphere mesh (unit radius; pos == normal for the triplanar shading).
    x3::prims::PrimMesh sphere = x3::prims::makeUVSphere(64, 128);
    outMesh = device->createMesh(
        sphere.verts.data(), (uint32_t)sphere.verts.size(),
        sphere.index.data(),  (uint32_t)sphere.index.size());

    // Flat annulus for the ring (object-space radii match planet_ring.frag's
    // hardcoded inner=1.3 / outer=2.5; the model matrix sizes it to the gas giant).
    if (outRingMesh) {
        x3::prims::PrimMesh ring = x3::prims::makeRing(1.3f, 2.5f, 128);
        *outRingMesh = device->createMesh(
            ring.verts.data(), (uint32_t)ring.verts.size(),
            ring.index.data(),  (uint32_t)ring.index.size());
    }

    return bodies;
}

// Draw every planet for the current frame (call AFTER the scene's own draws so the
// depth buffer occludes correctly). Each body uses its per-type planet pipeline.
//
// CELESTIAL anchoring: takes the CAMERA EYE and re-derives each body's world
// position from its sky direction every draw — pos = eye + dir(az,el)*kNightSkyDist,
// radius = kNightSkyDist*tan(diam/2) — so the bodies are translation-invariant
// (zero parallax as the player moves; they read as OUT in the night sky, never
// encroaching on the building) while still rotating correctly with the view.
// The atmosphere/corona shells + the ring annulus reuse the SAME derived position,
// so the companion layers track their parent body through the celestial transform.
void drawNightSkyPlanets(x3::rhi::IRenderDevice* device, const x3::rhi::FrameContext& fc,
                                x3::rhi::MeshHandle mesh,
                                const std::vector<NightSkyPlanet>& planets, float uTime,
                                float eyeX, float eyeY, float eyeZ,
                                x3::rhi::MeshHandle ringMesh) {
    if (!fc.valid) return;
    // PlanetType transparent indices (see VulkanRenderDevice PlanetType enum).
    constexpr uint32_t kAtmosphere = 9u, kSunCorona = 10u, kRing = 11u;
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    for (const NightSkyPlanet& b : planets) {
        // Sky direction from the body's azimuth/elevation (az 0 = -Z, +90 = +X).
        const float az = b.azimuthDeg * kDegToRad, el = b.elevationDeg * kDegToRad;
        const float ce = std::cos(el);
        const float px = eyeX + std::sin(az) * ce * kNightSkyDist;
        const float py = eyeY + std::sin(el)      * kNightSkyDist;
        const float pz = eyeZ - std::cos(az) * ce * kNightSkyDist;
        // Apparent angular diameter -> world radius at the anchor distance.
        const float r = kNightSkyDist * std::tan(b.angularDiameterDeg * 0.5f * kDegToRad);
        // OPAQUE body: uniform scale by the apparent radius, translated to world pos.
        const float model[16] = {
            r, 0, 0, 0,
            0, r, 0, 0,
            0, 0, r, 0,
            px, py, pz, 1,
        };
        device->drawPlanet(fc, mesh, model, b.typeIndex,
                           b.maps.data(), (uint32_t)b.maps.size(), uTime);

        // --- ADDITIVE atmosphere shell: same sphere, inflated ~1.06x the body. ---
        if (b.atmoTex.valid()) {
            const float s = r * 1.06f;
            const float m[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0, px, py, pz, 1 };
            x3::rhi::TextureHandle t[1] = { b.atmoTex };
            device->drawPlanet(fc, mesh, m, kAtmosphere, t, 1u, uTime);
        }
        // --- ADDITIVE sun corona: same sphere, big shell ~2.2x the Sun, animated. ---
        if (b.coronaTex.valid()) {
            const float s = r * 2.2f;
            const float m[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0, px, py, pz, 1 };
            x3::rhi::TextureHandle t[1] = { b.coronaTex };
            device->drawPlanet(fc, mesh, m, kSunCorona, t, 1u, uTime);
        }
        // --- ALPHA ring: flat annulus, tilted, uniformly scaled by the body radius. ---
        // The ring mesh is authored in object space at inner=1.3 / outer=2.5 (the
        // frag's hardcoded radii). A uniform scale by `r` keeps object-space radii
        // intact (the frag works object-space) and sizes the ring to 1.3r..2.5r in
        // world. A small tilt about X gives the disc some perspective.
        if (b.ringTex.valid() && ringMesh.valid()) {
            const float s = r;
            const float ct = 0.92f, st = 0.39f;   // ~23 deg tilt about X (cos,sin)
            // column-major: tilt(X) * scale(s), translated to the body position.
            const float m[16] = {
                 s,      0,      0,     0,
                 0,    s*ct,   s*st,    0,
                 0,   -s*st,   s*ct,    0,
                 px, py, pz, 1,
            };
            x3::rhi::TextureHandle t[1] = { b.ringTex };
            device->drawPlanet(fc, ringMesh, m, kRing, t, 1u, uTime);
        }
    }
}

// Run a cutscene WINDOWED to completion (blocking). Returns false only if the
// window was closed mid-film (host should quit cleanly). `startAt` scrubs the
// playhead before the first frame (--cuetime). Restores the device look state
// it touched before returning, so the world build that follows is unaffected.
bool runCutsceneWindowed(x3::rhi::IRenderDevice& device, GLFWwindow* window,
                                x3::audio::IAudioSystem* audio,
                                const x3::cut::Cutscene& cs, float startAt,
                                const std::function<void(const std::string&)>& hostEvent,
                                float stopAt) {
    if (!window) return true;   // headless guard (smoketests etc.) — no-op
    // CLIP-SPLIT: a positive in-range stopAt bounds this run to a span [startAt, stopAt).
    const bool   clipped  = (stopAt > 0.0f && stopAt < cs.duration);
    const float  clipEndT = clipped ? stopAt : cs.duration;
    x3::logInfo("[cutscene] playing '" + cs.name + "' " +
                (clipped ? ("clip [" + std::to_string(startAt) + ".." + std::to_string(stopAt) + "] s")
                         : ("(" + std::to_string(cs.duration) + " s)")) +
                " — press K to skip");

    CinematicScene scene;
    device.beginUploadBatch();
    scene.load(device, cs);
    device.endUploadBatch();
    scene.applyLook(device);

    CinAudioMap amap;
    amap.init(audio);

    x3::cut::CutscenePlayer player(cs);
    player.onAudio([&](const x3::cut::AudioCue& cue) { amap.fire(cue); });
    player.onEvent([&](const x3::cut::Event& e, bool) {
        scene.onEvent(e.name, cs, e.t);   // authored time: deterministic FX state incl. scrubs
        if (hostEvent) hostEvent(e.name); // host hook (StoryFlags: intro_complete etc.)
        x3::logInfo("[cutscene] x3.fire " + e.name + " @ " + std::to_string(e.t));
    });
    if (startAt > 0.0f) player.seek(startAt);

    bool prevAnyKey = true;     // swallow a key still held from before the film
    double prevTime = glfwGetTime();
    bool completed = true;
    // A clip is "done" at the clip end; the whole film is done at duration.
    auto reachedEnd = [&]() {
        return clipped ? (player.time() >= clipEndT) : player.done();
    };
    while (!glfwWindowShouldClose(window) && !reachedEnd()) {
        glfwPollEvents();
        const double now = glfwGetTime();
        float dt = (float)(now - prevTime);
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 0.0f) dt = 0.0f;

        // Skip ONLY on a rising-edge K press (Tim) — movement/look/mouse no longer skip the film.
        // In a CLIP, K jumps to the clip end (seek-only, audio cues NOT re-fired) so a
        // skipped clip still hands off cleanly to the next beat; the un-clipped film uses
        // the authored skipTarget.
        const bool skipKey = (glfwGetKey(window, GLFW_KEY_K) == GLFW_PRESS);
        if (skipKey && !prevAnyKey) { if (clipped) player.seek(clipEndT); else player.skip(); }
        prevAnyKey = skipKey;

        player.tick(dt);
        const float t = player.time();
        scene.update(cs, t);

        const x3::cut::CamPose cam = x3::cut::evalCamera(cs, t);
        device.setCamera(cam.pos.x, cam.pos.y, cam.pos.z, cam.yaw, cam.pitch, cam.fov);
        device.setSkyTime(10.0f + t * 0.02f);

        auto frame = device.beginFrame();
        if (frame.valid) {
            scene.drawWorld(device, frame, cs, t);
            scene.drawOverlay(device, frame, cs, t);
        }
        device.endFrame(frame);
    }
    if (!reachedEnd()) completed = false;   // window closed mid-clip/film

    // Stop music only when the WHOLE film ends (or the window closed): a clip beat
    // returns control to the orchestrator mid-timeline, so the looped music bed must
    // carry across the interactive gap into the next clip (the director wants a
    // continuous score, not silence between beats). The music.stop cue at the tail
    // (and the next playMusic on re-entry) handles the start/stop within the timeline.
    if (audio && (!clipped || !completed)) audio->stopMusic();
    scene.destroy(device);
    CinematicScene::restoreLook(device);
    x3::logInfo(std::string("[cutscene] '") + cs.name + "' " +
                (completed ? (player.skipped() ? "skipped" : "complete") : "aborted (window closed)"));
    return completed;
}

} // namespace x3::apphost
