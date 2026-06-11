#pragma once
// Render Device (RHI) interface — D1.
// Spec: specs/D1-render-device.spec.md
// IMPORTANT: this header must NOT include <vulkan.h>. All Vulkan types stay
// hidden in the .cpp so game/Lua code never sees the graphics API.
#include <cstdint>

namespace x3::rhi {

struct DeviceDesc {
    void*    nativeWindowHandle = nullptr; // HWND on Windows (from GLFW)
    uint32_t width  = 0;
    uint32_t height = 0;
    bool     vsync       = true;
    bool     validation  = false;          // Vulkan validation layers
    // HEADLESS / OFFSCREEN mode (validation + screenshot paths). When true the
    // device creates NO surface and NO swapchain — instead it renders into an
    // offscreen color image (same format/extent the swapchain used) that the
    // render graph targets. "Present" is a no-op; screenshot readback copies the
    // offscreen image. nativeWindowHandle is ignored. The interactive windowed
    // path (headless=false) is byte-for-byte unchanged. See the .cpp.
    bool     headless    = false;
    // Supersample AA (HEADLESS/screenshot only): render at width*ssaa x height*ssaa and
    // box-downscale to width x height on capture. 1 = off. The live windowed path ignores it.
    uint32_t ssaa        = 1;
};

struct FrameContext {
    uint32_t frameIndex = 0;  // ring index [0, kFramesInFlight)
    uint64_t cmd        = 0;  // opaque command-buffer handle
    uint32_t backbuffer = 0;  // swapchain image index
    bool     valid      = false;
};

// ---------------------------------------------------------------------------
// Role-based HUD/UI fonts. Each role binds its own baked glyph atlas (loaded
// from assets/fonts/ at init, with the embedded Roboto Mono as the guaranteed
// fallback). Some roles are PROPORTIONAL (per-glyph advance widths); the mono
// roles keep a fixed cell advance so the legacy N*px layout math stays exact.
//
//   Title   -> Orbitron-Bold         (proportional) — menu titles, big banners
//   Menu    -> SpaceGrotesk-Medium    (proportional) — buttons/toggles/objective/labels
//   Enemy   -> Tektur_Condensed-Bold  (proportional) — enemy nameplates / threat tags
//   News    -> SpaceMono-Bold         (monospace)    — event popups, "AREA CLEAR", tickers
//   Console -> Roboto Mono (embedded) (monospace)    — dev `~` console + HP/ammo numerics
//   HudMono -> alias of Console (the standard game-console mono — "is GOOD")
//
// To reassign a role's font: edit the ONE path string in kRoleFontPaths[] in
// VulkanRenderDevice.cpp and restart (no rebuild of the rest needed). To ship a
// role embedded in the single binary, run tools/embed_font.ps1 on its .ttf (see
// that file's header) and point that role at the generated embedded blob.
enum class FontRole : uint32_t {
    Console = 0,   // embedded Roboto Mono (mono) — default for drawHudText()
    HudMono = 0,   // alias: HP/ammo numerics use the same mono atlas as Console
    Title   = 1,   // Orbitron (proportional)
    Menu    = 2,   // Space Grotesk (proportional)
    Enemy   = 3,   // Tektur Condensed (proportional)
    News    = 4,   // Space Mono (monospace)
    Count   = 5,
};

// ---------------------------------------------------------------------------
// Mesh + texture API (S1). Vulkan types stay hidden in the .cpp; the public
// boundary uses POD vertices and small opaque handles (id == 0 means invalid).
// ---------------------------------------------------------------------------
struct MeshVertex { float pos[3]; float normal[3]; float uv[2]; };
struct MeshHandle    { uint32_t id = 0; bool valid() const { return id != 0; } };
struct TextureHandle { uint32_t id = 0; bool valid() const { return id != 0; } };

// ---------------------------------------------------------------------------
// Point lights (forward, fixed-array). A small per-frame set of omni lights the
// mesh fragment shader accumulates ON TOP of the directional sun + shadow term.
// POD only — no Vulkan types cross the boundary. `pos` is world-space; `color`
// is linear RGB pre-multiplied by intensity (so caller controls brightness);
// `range` is the falloff radius in meters (attenuation reaches ~0 at `range`).
// A point light contributes NO shadow (the single shadow map is the sun's); it
// is a cheap unshadowed fill — exactly right for corridor ceiling fixtures.
// ---------------------------------------------------------------------------
struct PointLight {
    float pos[3]   = { 0.0f, 0.0f, 0.0f };
    float range    = 6.0f;                  // meters; attenuation -> 0 at range
    float color[3] = { 1.0f, 1.0f, 1.0f };  // linear RGB * intensity
    float _pad     = 0.0f;                  // keep 16-byte friendly for the GPU
};

// ---------------------------------------------------------------------------
// Per-frame performance counters (perf instrumentation layer). No Vulkan types
// here — the device fills this from its own counters + GPU timestamp queries so
// the HUD / console can report draw calls / triangles / GPU+CPU ms without ever
// touching the graphics API. Snapshot it with IRenderDevice::stats().
//
// Counters (drawCalls / triangles / objectsSubmitted / objectsDrawn) reflect the
// frame that was most recently submitted via endFrame(). gpuFrameMs is the GPU
// time of an EARLIER frame (read back with the frames-in-flight latency, so the
// timestamps are guaranteed available without a stall) — see the .cpp notes.
// ---------------------------------------------------------------------------
struct RenderStats {
    uint32_t drawCalls        = 0;   // drawMesh() calls that recorded a draw this frame
    uint32_t triangles        = 0;   // total triangles submitted this frame
    uint32_t objectsSubmitted = 0;   // drawMesh() calls attempted (incl. skipped)
    uint32_t objectsDrawn     = 0;   // drawMesh() calls that actually drew (== drawCalls)
    float    gpuFrameMs       = 0.0f; // GPU time for the main pass (timestamp queries)
    uint64_t frameCount       = 0;   // total frames presented since init
};

class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;

    virtual bool init(const DeviceDesc&) = 0;
    virtual void shutdown() = 0;
    virtual void onResize(uint32_t w, uint32_t h) = 0;

    // Toggle vertical sync at runtime (settings UI). Switches the swapchain present
    // mode (FIFO when on, MAILBOX when off) by flagging a swapchain recreate on the
    // next beginFrame(); a no-op in headless mode (no swapchain) and when the value
    // is unchanged. Additive: the interactive windowed path is otherwise unchanged.
    virtual void setVsync(bool enabled) = 0;

    // Camera (FPS-style). Angles in radians. The device builds view+proj.
    // forward = (cos(pitch)*cos(yaw), sin(pitch), cos(pitch)*sin(yaw)).
    virtual void setCamera(float x, float y, float z, float yaw, float pitch, float fovDeg) = 0;

    // Scene ambient (hemispheric floor lift in mesh.frag, also the crude IBL term for
    // PBR meshes). Default is a small cool constant; raise it for bright daylit/outdoor
    // scenes (e.g. the showroom) so metal/glass surfaces aren't black. Non-pure (no-op
    // default) so headless / other devices are unaffected.
    virtual void setAmbient(float r, float g, float b) {}

    // Frame the sun's shadow ortho box on a world center + half-extent, instead of the default
    // camera-following ~45 m box. For large scenes (the showroom) so the building + surrounding
    // geometry fall inside the shadow map and actually cast shadows. Non-pure (no-op default).
    virtual void setShadowBounds(float cx, float cy, float cz, float halfExtent) {}

    // Enable/disable the interior reflection probe: bake the IBL environment from the
    // SCENE geometry (around the camera) instead of the analytic sky, so glossy metals
    // reflect the dim interior rather than the bright open sky. Non-pure (no-op default).
    virtual void setIblProbe(bool enable) {}

    // Final additive bloom strength (composite pass). Default is a subtle global value;
    // raise it for "hero" scenes (e.g. the showroom spire glowing against a dark sky) so
    // HDR-emissive surfaces bloom strongly, WITHOUT touching other scenes. Non-pure no-op default.
    virtual void setBloom(float intensity) {}

    // Whole-scene brightness multiplier applied pre-tonemap in the composite pass
    // (1.0 = unchanged). Drives the live r_exposure cvar / showroom brightness slider.
    // With auto-exposure ON this acts as exposure COMPENSATION (bias) on top of the
    // adapted value; with it OFF it is the absolute exposure, exactly as before.
    virtual void setExposure(float e) {}

    // HDR post-stack settings (tonemap / bloom gate / auto-exposure), synced per
    // frame from the r_* cvars. Defaults preserve the device-side behavior when the
    // app never calls this (headless screenshot/test paths included).
    struct PostFXParams {
        int   tonemapMode    = 1;      // r_tonemap: 0 = passthrough clamp (A/B), 1 = ACES
        bool  bloomEnabled   = true;   // r_bloom: 0 skips the whole bloom chain
        float bloomIntensity = -1.0f;  // r_bloomintensity: <0 = keep the scene-tuned setBloom()
        float bloomThreshold = 1.10f;  // r_bloomthreshold: bright-pass knee point (linear)
        bool  autoExposure   = true;   // r_autoexposure: eye adaptation on/off
        float aeSpeed        = 1.5f;   // r_aespeed: adaptation rate (1/s)
        float aeMin          = 0.70f;  // r_aemin: adapted-exposure clamp floor
        float aeMax          = 2.20f;  // r_aemax: adapted-exposure clamp ceiling
        float aeKey          = 0.18f;  // r_aekey: target middle-grey key
        bool  taa            = true;   // r_taa: temporal AA (Halton jitter + history
                                       // resolve). 0 = jitter fully off + resolve pass
                                       // skipped -> byte-identical to the pre-TAA path.
        float taaSharpen     = 0.25f;  // r_taasharpen: post-resolve RCAS-style sharpen
                                       // amount (0 = off). Only applied when taa is on.
    };
    virtual void setPostFX(const PostFXParams&) {}
    // Metal ambient-specular floor strength (mesh.frag IBL path): metals in a DARK
    // baked environment keep an F0-tinted ambient response instead of going black.
    // 1.0 = default ON, 0.0 = off. Drives the live r_metalambient cvar.
    virtual void setMetalAmbient(float s) {}

    // Per-frame
    virtual FrameContext beginFrame() = 0;
    virtual void         endFrame(const FrameContext&) = 0;

    // ---- Mesh / texture resources (static; uploaded once, no per-frame churn) ----
    // Create a device-local mesh from interleaved pos/normal/uv vertices + 32-bit
    // indices. Returns an invalid handle on failure.
    virtual MeshHandle    createMesh(const MeshVertex* verts, uint32_t vcount,
                                     const uint32_t* idx, uint32_t icount) = 0;
    virtual void          destroyMesh(MeshHandle) = 0;
    // Re-upload the vertices of an EXISTING mesh in place (CPU skinning path, J1).
    // The vertex count must match the count the mesh was created with; the index
    // buffer is untouched. The mesh's vertex storage becomes HOST_VISIBLE on first
    // update so subsequent re-uploads are cheap mapped memcpys (no staging). Used a
    // handful of times per frame for the animated characters only; the static
    // environment art never calls this. Validation-clean: the call waits for any
    // in-flight GPU use of the buffer before overwriting it. No-op on bad input.
    virtual void          updateMesh(MeshHandle, const MeshVertex* verts, uint32_t vcount) = 0;
    // Create a sampled texture from tightly-packed RGBA8 (w*h*4 bytes). `srgb`
    // selects the storage format (sRGB for color, UNORM for data/linear).
    virtual TextureHandle createTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb) = 0;
    virtual void          destroyTexture(TextureHandle) = 0;

    // ---- Terrain material splat (open-world ground) -------------------------
    // Register a set of four already-created tiling DETAIL textures as the GROUND
    // material set used for procedural height+slope splatting in mesh.frag. The
    // call returns an opaque MARKER TextureHandle: any mesh subsequently drawn
    // with this handle as its baseColor is flagged as TERRAIN in the per-object
    // SSBO, and the fragment shader blends grass/rock/snow/sand by world height +
    // slope + noise instead of sampling a single texture. NON-terrain meshes
    // (anything drawn with any other texture) are completely unaffected — the
    // terrain branch is gated by this per-object flag, so existing levels render
    // byte-for-byte as before.
    //
    // This is a thin, additive boundary: it reuses the existing bindless array
    // (the four textures keep their own bindless slots) and the existing draw
    // path; only a previously-reserved pad field in the SSBO row is now used to
    // carry the terrain flag + the four packed detail-texture indices. Passing
    // any invalid handle returns an invalid marker (terrain falls back to flat).
    // Each of grass/rock/snow/sand should be a small seamless RGBA8 sRGB tile.
    virtual TextureHandle registerTerrainMaterial(TextureHandle grass, TextureHandle rock,
                                                  TextureHandle snow,  TextureHandle sand) = 0;

    // Submit a draw between beginFrame/endFrame. An invalid baseColor falls back
    // to the built-in 1x1 white texture, so baseColorFactor alone gives a flat
    // color. `model` is a column-major 4x4. baseColorFactor multiplies the texel.
    virtual void          drawMesh(const FrameContext&, MeshHandle, TextureHandle baseColor,
                                   const float baseColorFactor[4], const float model[16]) = 0;

    // Emissive draw (HDR pipeline). Same as drawMesh() plus a per-object EMISSIVE
    // term: `emissive` is { r, g, b, strength } — a LINEAR emissive color (rgb)
    // scaled by `strength` (a) and added on top of the lit result in linear HDR,
    // independent of incoming light (so the surface glows even in shadow). With
    // strength > 1 the surface becomes a bright HDR source that drives the bloom
    // chain — exactly right for light fixtures / light strips. POD only (no Vulkan
    // types cross the boundary). The 5-arg drawMesh() above is equivalent to this
    // with emissive = {0,0,0,0}. emissive == nullptr is treated as all-zero.
    virtual void          drawMeshEmissive(const FrameContext&, MeshHandle, TextureHandle baseColor,
                                           const float baseColorFactor[4], const float emissive[4],
                                           const float model[16]) = 0;

    // PBR draw: drawMeshEmissive plus a NORMAL map + METALLIC-ROUGHNESS texture
    // (glTF packing: metallic in B, roughness in G). Invalid normal/mr handles
    // (id 0) make this identical to drawMeshEmissive (the shader skips its PBR
    // branch for that object), so the default impl below simply forwards — only
    // the GLB drawable path (env_art / models carrying PBR maps) needs real PBR.
    virtual void          drawMeshPBR(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                                      TextureHandle /*normal*/, TextureHandle /*metalRough*/,
                                      const float baseColorFactor[4], const float emissive[4],
                                      const float model[16], bool /*alphaMask*/ = false,
                                      bool /*alphaBlend*/ = false, TextureHandle /*emissiveTex*/ = {},
                                      TextureHandle /*detailTex*/ = {}, float /*detailUvScale*/ = 1.0f) {
        drawMeshEmissive(fc, mesh, baseColor, baseColorFactor, emissive, model);
    }

    // ---- Procedural planet body (FORGE3D port) -----------------------------
    // Draw a UV-SPHERE planet with a per-TYPE planet pipeline (a push-constant
    // model matrix + up to 12 bindless texture indices + uTime; no per-object SSBO).
    // The per-type fragment shader does its own object-space triplanar / banded /
    // emissive look. Drawn AFTER the opaque mesh multidraw, reusing the mesh's set0
    // bindless + set1 Camera UBO descriptor sets. ADDITIVE: non-pure no-op default so
    // the headless stub / other devices are unaffected and existing levels are
    // byte-for-byte unchanged.
    //   model     : column-major 4x4 object->world.
    //   typeIndex : PlanetType (0=Moon,1=Ice,2=Gas,3=Lava,4=Terrestrial,5=Oceanic,
    //               6=Sand,7=Thunderstorm,8=Sun) — selects the pipeline.
    //   maps      : up to `mapCount` TextureHandles in the type's pc.tex[] slot order
    //               (resolved to bindless indices; >12 clamped, unused slots zeroed).
    //   uTime     : animation time in seconds (for animated types).
    virtual void drawPlanet(const FrameContext& fc, MeshHandle mesh, const float model[16],
                            uint32_t typeIndex, const TextureHandle* maps, uint32_t mapCount,
                            float uTime) {}

    // ---- Celestial / sky animation time ------------------------------------
    // A global time source (seconds) for sky-driven animation: the starfield
    // rotation + any future time-driven celestial motion read it via the sky UBO
    // (params.z). The live game loop passes the elapsed time each frame; headless
    // screenshots pass a fixed non-zero value so animated state is captured.
    // Per-planet animation (e.g. the sun corona) still flows via drawPlanet's
    // uTime arg — this only feeds the full-screen sky. Non-pure no-op default.
    virtual void          setSkyTime(float) {}

    // ---- Translucent GLASS material (transparent pass) ---------------------
    // A general, reusable translucent-glass material (design spec
    // docs/superpowers/specs/2026-05-25-glass-material-design.md). A mesh drawn
    // via drawMeshGlass() is NOT rendered in the opaque pass — it is flagged GLASS
    // and drawn in a dedicated post-opaque, depth-tested (LEQUAL, no depth write),
    // alpha-blended transparent pass so it reads as see-through over the lit scene.
    //
    // All params are runtime-tunable per draw (material-instance style). POD only —
    // no Vulkan types cross the boundary. MILESTONES: M1 uses `opacity` (-> blend
    // alpha) so glass renders see-through; `tint`/`refraction`/`roughness`/`specular`
    // are carried now and consumed by later milestones (refraction, fresnel/specular,
    // frost). `opacity` 0 = crystal clear, 1 = fully opaque (the primary dial).
    struct GlassMaterial {
        float opacity    = 0.35f;            // 0 = clear .. 1 = opaque (blend alpha)
        float refraction = 0.03f;            // screen-space distortion strength (M2)
        float roughness  = 0.0f;             // 0 = polished .. 1 = frosted (M4)
        float specular   = 0.6f;             // shimmer / specular strength (M3)
        float tint[3]    = { 1.0f, 1.0f, 1.0f }; // glass color; white = colorless
    };

    // Submit a translucent glass draw. `glass.opacity` overrides baseColorFactor's
    // alpha (the see-through dial). `emissive` is the same per-object HDR glow term
    // as drawMeshEmissive (holo glass keeps its glow); pass nullptr for none. The
    // device flags the per-object row GLASS so it routes to the transparent pass.
    virtual void          drawMeshGlass(const FrameContext&, MeshHandle, TextureHandle baseColor,
                                        const float baseColorFactor[4], const float emissive[4],
                                        const GlassMaterial& glass, const float model[16]) = 0;

    // ---- Analytic sky (open-world track, task A) ---------------------------
    // Parameters for the physically-plausible analytic sky drawn as the far-depth
    // backdrop wherever no opaque geometry covers a pixel (it composites against
    // the depth buffer, so geometry occludes it). POD only — no Vulkan types.
    //
    // `enabled` toggles the full-screen sky pass (default OFF, so indoor levels +
    // every existing flag look exactly as before — the sky only shows when turned
    // on for an outdoor vantage). `sunDir` is the direction TOWARD the sun; pass
    // the SAME normalize(0.4,1,0.3) the lighting/shadow pass uses so the sun disk
    // sits where the world is lit from. `sunColor` is linear RGB (match the
    // directional light's color); `sunIntensity` scales the disk + glow.
    // `haze` (0..1) controls the horizon haze strength; `exposure` scales the
    // pre-tonemap sky radiance (the sky shares mesh.frag's ACES response).
    struct SkyParams {
        bool  enabled       = false;
        float sunDir[3]     = { 0.4f, 1.0f, 0.3f };   // toward the sun (normalized internally)
        float sunColor[3]   = { 1.0f, 0.97f, 0.92f }; // linear RGB; matches the sun light
        float sunIntensity  = 1.0f;
        float haze          = 0.5f;
        float exposure      = 1.0f;
        float zenith[3]     = { 0.10f, 0.28f, 0.66f }; // overhead sky color (linear); per-scene (default = old global)
        float horizon[3]    = { 0.62f, 0.74f, 0.92f }; // horizon glow color (linear); per-scene (default = old global)
    };
    // Set the active sky parameters for subsequent frames (cached + re-applied
    // each frame, like setPointLights). Calling with enabled=false disables it.
    virtual void          setSkyParams(const SkyParams&) = 0;

    // Project a world point to HUD pixel coords (top-left origin); false if behind the
    // camera / off-screen. Uses the most recent render viewProj. Non-pure (headless: false).
    virtual bool          worldToScreen(float, float, float, float& sx, float& sy) const { sx = sy = 0.0f; return false; }

    // Hardware ray tracing (RT Phase 0): true once VK_KHR_ray_query +
    // acceleration_structure are enabled on the device. The future RT
    // shadow/reflection/GI passes gate on this (SSAO/CSM raster fallback when
    // false). Default false (headless / non-RT devices).
    virtual bool          rayTracingSupported() const { return false; }

    // ---- Ray-traced ambient occlusion (RT AO — hardware ray query) ----------
    // GROUND-TRUTH ambient occlusion via the Vulkan ray-query path (rayQueryEXT):
    // the device builds a BLAS per static mesh + a per-frame TLAS from the scene
    // draw list, then an inline-ray-query compute pass casts short cosine-hemisphere
    // rays against the TLAS from each pixel's depth-reconstructed world position;
    // the resulting occlusion MULTIPLIES the linear HDR scene before bloom/tonemap.
    //
    // GATED + DEFAULT OFF (the `r_rtao` cvar maps 1:1 onto `enabled`). When OFF, or
    // when the device has no RT support (rayTracingSupported()==false), the whole RT
    // chain is skipped — the existing rasterized + SSAO/SSGI path is byte-for-byte
    // unchanged and costs nothing. This is purely additive: it never replaces SSAO,
    // it darkens the final HDR scene with real ray-traced contact occlusion. POD
    // only — no Vulkan types cross the boundary. Mirrors setSsaoParams: the device
    // caches a snapshot and re-applies it each frame.
    //
    // Tunables: `enabled` gates the whole RT-AO chain. `radius` is the ray length in
    // meters (a hit within `radius` occludes; larger = broader, softer AO). `rays`
    // is the hemisphere rays per pixel per frame (1..32; the half-res buffer + a
    // depth-aware up-sample keep it cheap; raise for less noise). `bias` offsets the
    // ray origin off the surface to avoid self-intersection. `strength` lerps the
    // applied darkening (1 = full AO, 0 = none). `power` is a contrast exponent on
    // the AO. `rebuildTlasEachFrame` forces a TLAS rebuild every frame (correct for
    // moving geometry; the static-first path rebuilds only when the scene changes).
    struct RtaoParams {
        bool  enabled  = false;   // DEFAULT OFF (gated by r_rtao)
        float radius   = 0.5f;    // ray length (meters) — short = contact AO, not whole-room
        int   rays     = 8;       // hemisphere rays / pixel / frame (1..32)
        float bias     = 0.03f;   // surface offset (meters) — avoid self-intersection
        float strength = 0.85f;   // applied AO darkening (1 = full, 0 = off)
        float power    = 1.5f;    // contrast exponent on the AO
        bool  rebuildTlasEachFrame = false; // static-first: rebuild only on scene change
    };
    // Set the active RT-AO parameters for subsequent frames (cached + re-applied
    // each frame, like setSsaoParams). Calling with enabled=false disables RT AO.
    // No-op on a device without ray tracing. Default no-op (headless / base).
    virtual void          setRtaoParams(const RtaoParams&) {}

    // ---- SSR / ray-traced REFLECTIONS (r_ssr / r_rtreflections) ------------
    // Hybrid reflections: a half-res (or full-res) compute pass marches each
    // pixel's reflection ray against the depth buffer and samples LAST frame's
    // lit scene (the TAA history image) — reflections therefore REQUIRE TAA
    // (its history is the color source and its accumulation is the temporal
    // denoiser; with r_taa 0 the whole chain is off and the render is
    // byte-for-byte unchanged). On ray-query hardware, screen-space misses fall
    // back to ONE inline ray query into the scene TLAS (rtFallback; auto-
    // disabled — SSR-only — when rayTracingSupported() is false, e.g. Pascal).
    // mesh.frag blends the result INTO its split-sum IBL specular by confidence
    // (replace-where-confident through the same F0/roughness env-BRDF weighting
    // — energy-conserving, never additive on top of full IBL specular).
    struct ReflectionParams {
        bool  ssr        = false;   // master gate (r_ssr; OFF until the app enables it)
        bool  rtFallback = true;    // ray-query fallback where SSR misses (r_rtreflections)
        bool  fullRes    = false;   // r_reflquality: false = half-res (default), true = full
        float intensity  = 1.0f;    // blend-weight scale on the composed reflection [0..1]
    };
    // Cached + re-applied each frame, like setRtaoParams. Default no-op.
    virtual void          setReflectionParams(const ReflectionParams&) {}

    // ---- DDGI — dynamic diffuse global illumination (r_ddgi) ----------------
    // Classic probe-grid DDGI (Majercik et al. 2019, the public paper): an
    // axis-aligned grid of light probes; a per-frame inline-ray-query compute
    // pass traces N rays/probe against the SAME scene TLAS RT AO/reflections
    // use, shades hits simply (per-object albedo/emissive from the draw SSBO,
    // sun via a shadow ray, point lights, plus the previous frame's probe field
    // for infinite bounce), and blends the results into octahedral irradiance +
    // mean/mean^2 visibility-depth atlases with hysteresis. mesh.frag then
    // REPLACES its ambient DIFFUSE term (flat ambient or IBL irradiance cube)
    // with an 8-probe trilinear + Chebyshev-visibility-weighted (leak-free)
    // interpolation of that field, by grid confidence — outside the grid the
    // existing ambient path remains. Specular is untouched (IBL/reflections).
    //
    // TIER-GATED + DEFAULT OFF: requires ray-query hardware AND
    // VK_KHR_ray_tracing_position_fetch (hit normals). Non-RT devices (Pascal)
    // ignore this entirely — their ambient path is byte-for-byte unchanged.
    // Probes converge over ~1-2 s (hysteresis); emissive/sun changes propagate.
    struct DdgiParams {
        bool  enabled = false;     // master gate (r_ddgi)
        int   debug   = 0;         // r_ddgi_debug: 0 off, 1 irradiance field, 2 confidence
        int   countX = 24, countY = 8, countZ = 24;   // probe grid dimensions
        // Grid volume (world AABB). sizeX <= 0 -> AUTO-FIT to this frame's
        // static draw list (instance-origin AABB + padding) at activation.
        float originX = 0, originY = 0, originZ = 0;
        float sizeX = -1, sizeY = -1, sizeZ = -1;
        int   raysPerProbe = 96;   // rays/probe/frame (16..128)
        float hysteresis    = 0.97f;  // irradiance temporal blend (toward history)
        float hysteresisVis = 0.98f;  // visibility temporal blend
        float intensity = 1.0f;    // applied GI scale on the replaced diffuse term
        float bounceGain = 0.95f;  // recursive probe-field feedback gain (<1: stable)
        float normalBias = 1.0f;   // self-shadow bias scale (fraction of spacing/4)
    };
    // Cached + re-applied each frame, like setRtaoParams. Default no-op.
    virtual void          setDdgiParams(const DdgiParams&) {}

    // ---- Glass DEV overrides (live r_glass_* cvars, spec §2/§3.2) ----------
    // A dev-time SCALE/OVERRIDE applied to EVERY glass fragment this frame so the
    // glass look can be scrubbed live in the console without re-authoring each
    // material. When `override` is false the shader uses each object's own
    // GlassMaterial unchanged (the production path). When true: refraction is
    // SCALED by refractScale, roughness is ADDED by roughAdd (clamped), specular is
    // SCALED by specScale. Cached + re-applied each frame like the other dev params.
    // Default no-op (headless / base) so existing devices/tests are unaffected.
    struct GlassDevParams {
        bool  override     = false;  // false = use per-object materials as authored
        float refractScale = 1.0f;   // multiplies GlassMaterial.refraction
        float roughAdd     = 0.0f;   // added to GlassMaterial.roughness (clamped 0..1)
        float specScale    = 1.0f;   // multiplies GlassMaterial.specular
    };
    virtual void          setGlassDevParams(const GlassDevParams&) {}

    // ---- Screen-space ambient occlusion (SSAO, idTech-8 grounding/contact) --
    // SSAO darkens the AMBIENT/indirect lighting term in corners, crevices, and
    // contact points so objects feel grounded (fixes the "floating/flat" look,
    // esp. tall arena corners). Computed in VIEW space before tonemap: a half-res
    // hemisphere-kernel pass reconstructs view-space position + normal FROM THE
    // DEPTH BUFFER (no G-buffer), accumulates occlusion, then a depth-aware blur
    // removes banding. The result modulates ONLY the ambient term in mesh.frag
    // (direct sun + point lights stay full-strength). POD only — no Vulkan types.
    //
    // Tunables (the renderer applies them each frame; an app's console cvars map
    // 1:1 onto these): `enabled` gates the whole SSAO chain (default ON; off ==
    // the pre-SSAO look + no SSAO GPU cost). `radius` is the view-space sample
    // hemisphere radius in meters (larger = broader, softer occlusion). `bias`
    // offsets the depth compare to suppress self-occlusion acne on flat surfaces.
    // `intensity` scales the raw occlusion. `power` is a contrast exponent on the
    // final AO. `strength` lerps the APPLIED AO (1 = full effect, 0 = none) so the
    // ambient is never over-crushed.
    struct SsaoParams {
        bool  enabled   = true;
        float radius    = 0.5f;   // view-space hemisphere radius (meters)
        float bias      = 0.025f; // depth-compare bias (view-space units)
        float intensity = 1.0f;   // raw occlusion scale
        float power     = 1.5f;   // contrast exponent on final AO
        float strength  = 0.9f;   // lerp the applied AO (1 = full, 0 = off)
    };
    // Set the active SSAO parameters for subsequent frames (cached + re-applied
    // each frame, like setSkyParams). Calling with enabled=false disables SSAO.
    virtual void          setSsaoParams(const SsaoParams&) = 0;

    // ---- Real-time dynamic global illumination (SSGI — indirect diffuse) -----
    // A fully-dynamic, screen-space one-bounce indirect-diffuse pass so light
    // BOUNCES: coloured bounce light, lit ambient in shadow, colour bleeding — no
    // baking. Reuses the SSAO depth pre-pass + view-space reconstruction. The chain
    // is: a half-res GATHER (reconstruct view pos/normal from depth, march a
    // cosine-weighted hemisphere, sample the lit HDR scene colour as incoming
    // radiance weighted by cosine + range/visibility) -> TEMPORAL accumulation
    // (camera-reproject the previous frame's GI + EMA blend, reject on depth
    // disocclusion, to kill noise) -> a depth-aware DENOISE blur -> APPLY (depth-
    // aware up-sample + additive into the LINEAR HDR scene, modulated by the SSAO
    // AO) BEFORE bloom/tonemap. Direct sun + point lights stay full-strength; GI
    // lifts the bounce/ambient so shadowed areas get believable colour instead of
    // flat ambient. POD only — no Vulkan types cross the boundary. Mirrors
    // setSsaoParams: cache a snapshot, re-applied each frame.
    //
    // Tunables (an app's console cvars map 1:1): `enabled` gates the whole GI chain
    // (default ON; off == no GI GPU cost + the pre-GI look). `intensity` scales the
    // raw gathered radiance. `radius` is the view-space gather hemisphere radius in
    // meters (larger = broader, softer bounce). `strength` scales the APPLIED GI in
    // HDR (final knob). `numSamples` is the hemisphere taps per pixel per frame
    // (1..24; lower = cheaper + noisier, temporal cleans it up). `temporalAlpha`
    // is the history weight in the EMA (0 = no accumulation/most responsive,
    // ~0.9 = very smooth but laggier). `maxRadiance` clamps a single bounce sample
    // (firefly suppression). `aoModulate` (0..1) lerps how strongly the SSAO AO
    // gates the bounce. `falloffPower` shapes the range falloff curve.
    //
    // LIMITATIONS (honest): screen-space only — light from surfaces NOT on screen
    // (behind the camera / occluded) cannot bounce (the documented next tier is a
    // voxel/probe GI grid). Camera-only reprojection can ghost fast dynamic objects.
    struct GiParams {
        bool  enabled       = true;
        float intensity     = 1.0f;    // raw gathered-radiance scale
        float radius        = 1.6f;    // view-space gather hemisphere radius (m)
        float strength      = 1.0f;    // applied GI strength (HDR, final knob)
        int   numSamples    = 16;      // hemisphere taps per pixel per frame (1..24)
        float temporalAlpha = 0.90f;   // history weight (EMA); 0 = off
        float maxRadiance   = 4.0f;    // per-sample radiance clamp (firefly suppress)
        float aoModulate    = 0.7f;    // 0..1: how strongly SSAO AO gates the bounce
        float falloffPower  = 1.5f;    // range-falloff curve exponent
    };
    // Set the active GI parameters for subsequent frames (cached + re-applied each
    // frame, like setSsaoParams). Calling with enabled=false disables the GI chain.
    virtual void          setGiParams(const GiParams&) = 0;

    // ---- Animated water / ocean surface (undersea-world foundation) --------
    // A large animated water plane drawn in the MAIN pass (after opaque meshes,
    // before bloom/composite) into the linear HDR scene target, so it composes
    // with terrain, sky, shadows, point lights, HDR/bloom, and SSAO. The surface
    // is a tessellated grid centered + clamped under the camera at `seaLevel`,
    // displaced by a sum of Gerstner/trochoidal waves (a few summed directions)
    // with analytic normals, scrolling over time. Shading (all pre-tonemap, in
    // linear HDR): a Fresnel blend between a cheap REFLECTION (the analytic sky
    // color sampled in the reflected direction — same gradient/sun as sky.frag)
    // and a depth-based REFRACTION color (shallow->deep gradient from scene depth
    // vs. the water surface depth, reusing the SSAO depth pre-pass's depth buffer),
    // plus a sharp SUN GLINT specular (feeds bloom) and a subtle ripple normal
    // perturbation; a distance/horizon fog blends the far water into the sky.
    // POD only — no Vulkan types cross the boundary. Mirrors setSkyParams /
    // setSsaoParams: cache a snapshot, re-applied each frame; enabled=false
    // (default) means the whole water pass is skipped (zero GPU cost) so every
    // existing flag/level looks exactly as before.
    //
    // Tunables: `enabled` gates the pass. `seaLevel` is the water plane height (m,
    // world +Y). `time` is the animation clock (seconds) the host advances each
    // frame (the device does NOT keep its own clock, so headless captures are
    // deterministic). `amplitude` scales overall wave height (m); `steepness`
    // (0..1) is the Gerstner sharpness (0 = round sine swell, ~1 = peaked chop);
    // `waveLength` is the base wavelength (m) of the largest wave; `speed` scales
    // the scroll/phase rate. `deepColor` / `shallowColor` are LINEAR RGB for the
    // deep- and shallow-water refraction tint (the depth gradient lerps shallow->
    // deep). `sunDir` is the direction TOWARD the sun (pass the same one as the
    // sky/lighting). `specular` scales the sun-glint highlight (HDR, drives bloom).
    // `fresnel` biases the base (face-on) reflectance. The camera + sun + sky come
    // from the camera set via setCamera and the sky params; only water-specific
    // knobs live here.
    struct WaterParams {
        bool  enabled        = false;
        float seaLevel       = 0.0f;
        float time           = 0.0f;
        float amplitude      = 0.45f;
        float steepness      = 0.55f;
        float waveLength     = 14.0f;
        float speed          = 1.0f;
        float deepColor[3]   = { 0.02f, 0.07f, 0.11f };  // linear deep-water tint
        float shallowColor[3]= { 0.10f, 0.32f, 0.38f };  // linear shallow tint
        float sunDir[3]      = { 0.4f, 1.0f, 0.3f };      // toward the sun (normalized internally)
        float specular       = 12.0f;                     // sun-glint strength (HDR -> bloom)
        float fresnel        = 0.02f;                     // base (face-on) reflectance
    };
    // Set the active water parameters for subsequent frames (cached + re-applied
    // each frame, like setSkyParams). Calling with enabled=false disables water.
    virtual void          setWaterParams(const WaterParams&) = 0;

    // ---- GPU-instanced billboard particles + impact decals (combat juice) ---
    // A bounded, per-frame stream of CAMERA-FACING BILLBOARD particles drawn in
    // the HDR pipeline AFTER opaque + water + GI, BEFORE bloom — so bright sparks /
    // muzzle flashes feed the bloom chain. The CPU owns the simulation (a fixed
    // pool; see app/fx.*) and each frame submits the live particles' POD instances;
    // the device uploads them into a per-frame instance ring (NO per-frame heap
    // alloc) and draws ONE instanced quad. Soft particles: the fragment shader
    // fades each billboard against the scene depth buffer (the SSAO/water depth) so
    // sparks/smoke don't hard-intersect geometry. Depth-TEST against the scene, no
    // depth-WRITE (particles never occlude each other in depth). POD only.
    //
    // `mode` selects the blend: ADDITIVE (sparks/fire/muzzle — glows, feeds bloom)
    // or ALPHA (smoke/dust/blood — translucent over the scene). Submit each batch
    // with one call; the two modes are drawn in separate sub-batches so blend state
    // is correct. Submitting count==0 (or never calling it) means NO particle pass
    // is added that frame — zero GPU cost when idle. Call between beginFrame and
    // endFrame, like drawMesh. The device COPIES the instances (does not retain the
    // pointer). `count` is clamped to the device's per-frame particle cap.
    struct ParticleInstance {
        float pos[3]   = { 0.0f, 0.0f, 0.0f }; // world-space center
        float size     = 0.1f;                 // billboard half-extent (meters)
        float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // linear RGB * intensity, A = opacity
    };
    enum class ParticleBlend : uint32_t { Additive = 0, Alpha = 1 };
    // Append a batch of particle instances for THIS frame, all drawn with `mode`.
    // Multiple calls accumulate (additive + alpha batches kept separate internally).
    virtual void submitParticles(const ParticleInstance* instances, uint32_t count,
                                 ParticleBlend mode) = 0;

    // Impact decals: small oriented quads projected onto a hit surface (bullet
    // holes / scorch marks). Each is a world-space quad at `center`, oriented so
    // its face normal is `normal` (the surface normal from the weapon raycast),
    // sized `halfSize` (meters), rotated about the normal by `angle` (radians) for
    // variety, tinted by `color` (linear RGBA; A = opacity for fade-out). Drawn in
    // the same HDR transparent pass as particles, alpha-blended, depth-TESTED (so a
    // decal behind geometry is occluded) with NO depth-write, slightly pushed along
    // the normal to avoid z-fighting with the surface it sits on. The CPU owns a
    // bounded ring (oldest recycled) and submits the live decals each frame. POD
    // only; the device copies them. Submitting count==0 adds no decal draw.
    struct DecalInstance {
        float center[3] = { 0.0f, 0.0f, 0.0f }; // world-space hit point
        float halfSize  = 0.1f;                  // quad half-extent (meters)
        float normal[3] = { 0.0f, 1.0f, 0.0f };  // surface normal (face direction)
        float angle     = 0.0f;                  // spin about the normal (radians)
        float color[4]  = { 0.0f, 0.0f, 0.0f, 1.0f }; // linear RGBA; A = opacity
    };
    virtual void submitDecals(const DecalInstance* decals, uint32_t count) = 0;

    // ---- GPU-compute persistent debris world (Subsystem K, tier T2) ---------
    // A GPU-resident pool of thousands of cheap debris fragments simulated entirely
    // in a COMPUTE shader and drawn via the existing GPU-driven INSTANCED path. This
    // is an ADDITIVE, opt-in layer ON TOP of the Jolt chunk destruction (K-T0/T1):
    // the authoritative ~256 Jolt bodies stay exactly as they are; this carries the
    // far cheaper, far larger persistent rubble (settles + sleeps so it stays cheap).
    //
    // Lifecycle: configure once (gpuDebrisConfig), spawn bursts at fracture/explosion
    // events (gpuDebrisSpawnBurst), step the sim each frame AFTER beginFrame and
    // BEFORE the draw (gpuDebrisStep dispatches the compute pass), then draw the live
    // pool between beginFrame/endFrame (gpuDebrisDraw issues ONE instanced cube draw
    // over the whole pool capacity; dead slots collapse to nothing in the shader).
    //
    // PASCAL (1080 Ti): plain compute only — NO hardware ray tracing. The compute is
    // a synchronous pass on the graphics queue (the spec's async path is a 5090
    // optimization; the synchronous fallback is correct everywhere). POD only — no
    // Vulkan types cross this boundary.

    // Simulation tunables (spec §5/§15). All cached by the device; re-applied each
    // gpuDebrisStep. Defaults are sensible for a ~1m-scale ground world.
    struct GpuDebrisParams {
        float gravity[3]   = { 0.0f, -9.81f, 0.0f }; // m/s^2
        float groundY      = 0.0f;    // infinite ground plane height (world +Y)
        float restitution  = 0.30f;   // bounce on ground/AABB contact (0..1)
        float friction     = 0.40f;   // tangential ground friction per contact (0..1)
        float linearDamping= 0.20f;   // per-second linear velocity damping
        float sleepLinSpeed= 0.20f;   // below this linear speed -> sleep-eligible (m/s)
        float sleepAngSpeed= 0.30f;   // below this angular speed -> sleep-eligible (rad/s)
        uint32_t sleepFrames = 12;    // consecutive slow steps before settling to sleep
        // Optional small set of world AABBs the fragments collide against (besides the
        // ground plane). Up to 4; aabbCount==0 = ground plane only.
        uint32_t aabbCount = 0;
        float aabbMin[4][3] = {};
        float aabbMax[4][3] = {};
    };
    // Per-fragment readback snapshot (tests / HUD). Computed by gpuDebrisReadback()
    // from the live GPU pool (the device reads the pool back to host memory; intended
    // for verification, not the hot path).
    struct GpuDebrisStats {
        uint32_t alive    = 0;        // active + sleeping fragments
        uint32_t settled  = 0;        // fragments in the SLEEP state
        uint32_t capacity = 0;        // pool capacity
        float    minY     = 0.0f;     // lowest live-fragment y (settling proof)
        float    maxY     = 0.0f;     // highest live-fragment y
        float    maxSpeed = 0.0f;     // largest live linear speed (boundedness proof)
        uint32_t nanCount = 0;        // live fragments with any NaN/Inf component (must be 0)
        uint32_t outOfBounds = 0;     // live fragments outside +/- boundsLimit (must be 0)
    };

    // Configure the debris simulation. Cheap; cache + re-apply each step.
    virtual void gpuDebrisConfig(const GpuDebrisParams&) = 0;
    // Emit a burst of `count` fragments at `pos` with an outward velocity spread of
    // magnitude ~`speed` (+ an upward bias) and a random spin, each living `lifetime`
    // seconds with half-extent `halfExtent`. `seed` makes the burst deterministic.
    // Recycles the oldest fragments if the pool is full (never blocks, never allocs).
    // Returns the number actually spawned (== count unless clamped to capacity).
    virtual uint32_t gpuDebrisSpawnBurst(const float pos[3], uint32_t count, float speed,
                                         float lifetime, float halfExtent, uint32_t seed) = 0;
    // Advance the debris sim by `dt` (dispatches the compute pass over the pool).
    // Call once per frame between beginFrame and gpuDebrisDraw.
    virtual void gpuDebrisStep(float dt) = 0;
    // Draw the live debris pool (one instanced cube draw over the pool capacity).
    // Call between beginFrame/endFrame, after gpuDebrisStep. `tint` is linear RGBA.
    virtual void gpuDebrisDraw(const FrameContext&, const float tint[4]) = 0;
    // Current alive (active + sleeping) fragment count (cheap; no GPU readback).
    virtual uint32_t gpuDebrisAliveCount() const = 0;
    // Pool capacity (the configured maximum simultaneous fragments).
    virtual uint32_t gpuDebrisCapacity() const = 0;
    // Read the live pool back to host memory + summarize (tests). `boundsLimit` is
    // the absolute-coordinate bound a live fragment must stay within. Returns a
    // zeroed snapshot if the device has no debris pool (e.g. a headless stub).
    virtual GpuDebrisStats gpuDebrisReadback(float boundsLimit = 1.0e5f) const = 0;

    // ---- GPU compute skinning of models (GPU SKINNING OF MODELS) -----------
    // Moves skeletal skinning from the CPU to a COMPUTE pre-pass so crowds of
    // animated NPCs scale: the CPU keeps computing the (cheap) joint-matrix
    // palette, but instead of CPU linear-blend-skinning every vertex + re-uploading
    // the whole vertex buffer each frame (the J1 updateMesh path that "doesn't scale
    // past a handful of NPCs"), the host UPLOADS the per-instance palette and a
    // compute shader skins on the GPU into a per-frame skinned-output buffer that
    // the existing depth/shadow/color passes draw unchanged. POD only — no Vulkan
    // types cross this boundary. Pascal-safe (plain compute, no hardware RT).
    //
    // Lifecycle:
    //   1) Create the drawable mesh as usual (createMesh with the BIND-POSE verts).
    //   2) registerSkinnedMesh() ONCE: hands the device the bind-pose verts again
    //      plus per-vertex joint indices (4x u16) + weights (4x f). The device keeps
    //      an immutable bind-pose+attrs buffer and allocates a per-frame, compute-
    //      written SKINNED-OUTPUT vertex buffer for this mesh (double-buffered for
    //      frames-in-flight). The mesh now draws from that output buffer.
    //   3) setSkinnedPalette() EACH FRAME (between beginFrame and endFrame, BEFORE
    //      the draw): upload the mat4 joint palette (column-major, 16 floats/joint)
    //      this instance should be skinned with this frame. This flags the mesh for
    //      a compute dispatch in the skinning pre-pass added BEFORE the depth/shadow/
    //      color passes (with the correct SSBO->vertex-read barrier).
    //   4) drawMesh()/drawMeshEmissive() the SAME mesh handle as always — it now
    //      renders the GPU-skinned output. No per-frame CPU LBS, no updateMesh.
    //   5) unregisterSkinnedMesh() (or destroyMesh) frees the skinning resources.
    //
    // A device that cannot do compute skinning returns false from registerSkinnedMesh
    // (and supportsGpuSkinning()), so callers transparently fall back to CPU skinning.
    virtual bool registerSkinnedMesh(MeshHandle mesh, const MeshVertex* bindVerts,
                                     uint32_t vcount, const uint16_t* jointIdx4,
                                     const float* jointWt4) = 0;
    // Free the GPU-skinning resources for a mesh (idempotent; no-op if unregistered).
    // destroyMesh() also releases them, so an explicit call is optional.
    virtual void unregisterSkinnedMesh(MeshHandle mesh) = 0;
    // Upload this instance's joint-matrix palette for THIS frame and flag the mesh
    // for the compute skinning pre-pass. `palette` is jointCount column-major mat4s
    // (16 floats each). No-op if the mesh was not registered. The device copies the
    // palette (does not retain the pointer).
    virtual void setSkinnedPalette(MeshHandle mesh, const float* palette,
                                   uint32_t jointCount) = 0;
    // Test/diagnostic readback: wait for in-flight skinning to retire, then copy the
    // most-recently-skinned output vertices of `mesh` into `out` (vcount MeshVertex).
    // Returns false if the mesh is not registered or vcount mismatches. NOT a hot
    // path (it stalls the GPU); used by --test-gpuskin to verify the compute output.
    virtual bool readbackSkinnedMesh(MeshHandle mesh, MeshVertex* out, uint32_t vcount) = 0;
    // True if this device supports the GPU compute-skinning path (a real Vulkan
    // device with the skin compute pipeline; false for the headless stub).
    virtual bool supportsGpuSkinning() const = 0;

    // ---- Forward point lights (interior fill) ------------------------------
    // Set the active point lights for subsequent frames. The device copies the
    // array (does NOT retain the pointer) and uploads them into the per-frame
    // lights UBO inside beginFrame/endFrame, so mesh.frag accumulates them with
    // distance attenuation on top of the directional sun. Static lights only need
    // ONE call (the device re-uploads its cached copy each frame); call again to
    // change them. `count` is clamped to the device's cap (extra lights ignored).
    // Passing count==0 clears all point lights (sun + ambient only).
    virtual void          setPointLights(const PointLight* lights, uint32_t count) = 0;

    // ---- Screen-space 2D HUD overlay (S7) ----------------------------------
    // A pixel-space overlay drawn over the 3D scene, inside the same dynamic-
    // rendering pass (issue these AFTER drawMesh, before endFrame). Coordinates
    // are in framebuffer pixels with the origin at the TOP-LEFT; +x right, +y
    // down. Colors are linear rgba in [0,1]. No depth test; alpha-blended.
    //
    // drawHudQuad: a filled rectangle (backed by the built-in 1x1 white texture).
    virtual void drawHudQuad(const FrameContext&, float xPx, float yPx,
                             float wPx, float hPx, const float rgba[4]) = 0;
    // drawHudText: render `text` starting at (xPx,yPx) at glyph size `pxPerGlyph`.
    // Uses the DEFAULT mono role (FontRole::Console / HudMono — the embedded Roboto
    // Mono). Each glyph advances by a fixed cell (== pxPerGlyph), so the legacy
    // N*px layout math (centering / right-align) stays exact. Newlines advance a
    // line; non-printable chars are skipped; `rgba` tints it. Non-UI callers that
    // do not care about role keep using this unchanged.
    virtual void drawHudText(const FrameContext&, const char* text, float xPx,
                             float yPx, float pxPerGlyph, const float rgba[4]) = 0;
    // drawHudTextF: role-aware HUD text. `role` selects the baked atlas. PROPORTIONAL
    // roles advance the pen by each glyph's REAL width; monospace roles advance by a
    // fixed cell. To keep centering/right-align exact, query the true rendered width
    // with textAdvance(role, ...) (UiContext::textWidth wraps this). `px` is the cap
    // pixel height; `rgba` tints it.
    virtual void drawHudTextF(const FrameContext&, FontRole role, const char* text,
                              float xPx, float yPx, float px, const float rgba[4]) = 0;
    // textAdvance: the TRUE rendered pixel width `text` occupies for `role` at glyph
    // size `px` — sums per-glyph advances (proportional) or N*cell (mono). The UI
    // layer's textWidth() reads this so centering/right-alignment is pixel-exact.
    // Safe to call before a frame is begun (pure metrics; no GPU work). If the role's
    // atlas is unavailable it falls back to the mono cell metric.
    virtual float textAdvance(FontRole role, const char* text, float px) const = 0;
    // Current framebuffer size in pixels (for HUD layout / centering).
    virtual void hudSize(uint32_t& outW, uint32_t& outH) const = 0;

    // ---- Perf instrumentation (stats / r_speeds) --------------------------
    // Snapshot of the most recently completed frame's counters + the GPU frame
    // time (read back with frames-in-flight latency). Cheap; copy it each frame
    // for the HUD/console overlay. No Vulkan types cross this boundary.
    virtual RenderStats stats() const = 0;

    // ---- Offscreen capture (--screenshot) ---------------------------------
    // Two-step, validation-clean capture of a FRESHLY-RENDERED, properly-acquired
    // frame (avoids reading a non-acquired/last-presented swapchain image):
    //
    //   1) Call armCapture(path) BEFORE the beginFrame() of the frame you want to
    //      grab. This arms a pending request (the device copies the acquired color
    //      image into a host-visible readback buffer INSIDE that frame's command
    //      buffer, with correct sync2 barriers, as part of the live frame).
    //   2) Render that one frame normally (beginFrame -> drawMesh... -> endFrame).
    //   3) Call captureFrame(path) AFTER that endFrame() to wait for the armed
    //      copy to retire, then swizzle (BGRA->RGBA) + write the PNG. Returns true
    //      on success. If no capture was armed, captureFrame() falls back to the
    //      legacy "last-presented image" copy (still functional, kept for safety).
    //
    // The implementation handles the swapchain channel order (BGRA->RGBA) and the
    // row pitch so colors + dimensions are correct. No Vulkan types cross here.
    virtual void armCapture(const char* path) = 0;
    virtual bool captureFrame(const char* path) = 0;

    // Capability query
    virtual bool supportsDescriptorIndexing() const = 0;
    virtual bool supportsMeshShaders() const = 0;

    // ---- Editor UI (Dear ImGui, docking) — EDITOR-ONLY (Level Architect P0) ----
    // The native level editor draws its panels with Dear ImGui INTO the swapchain
    // image, in a dedicated render pass AFTER the game composite/HUD pass. ImGui is
    // gated to `--editor` mode: with no `--editor` flag none of these are called and
    // the device allocates NOTHING (zero cost in the shipping game path). The game's
    // own FontRole HUD (drawHudQuad/drawHudText) is untouched — ImGui is a SEPARATE,
    // editor-only overlay. All five are NON-PURE no-op defaults so the headless stub,
    // the game, and any other IRenderDevice implementation are unaffected; only the
    // Vulkan device overrides them (ImGui/Vulkan types stay hidden in the .cpp).
    //
    // initEditorUI: one-time ImGui init (context + docking flag + dedicated descriptor
    //   pool + GLFW/Vulkan backends bound to the device's own queue/format). `glfwWindow`
    //   is the live GLFWwindow* (passed as void* so this header needs no GLFW include).
    //   Safe to call once after device init succeeds; a second call is a no-op.
    virtual void initEditorUI(void* /*glfwWindow*/) {}
    // beginEditorUI: start a new ImGui frame (call once per frame BEFORE issuing the
    //   editor's Begin/End/widget calls). For P0 the device itself submits a dockspace
    //   + the ImGui demo window as a proof of the integration.
    virtual void beginEditorUI() {}
    // endEditorUI: finalize the ImGui frame (ImGui::Render) and stash the draw data so
    //   the device records ImGui_ImplVulkan_RenderDrawData inside the next endFrame()'s
    //   graph pass. Call AFTER all editor widget calls and BEFORE endFrame().
    virtual void endEditorUI() {}
    // shutdownEditorUI: tear down ImGui (backends + context + the dedicated pool).
    //   Idempotent; also invoked automatically from the device shutdown if still init.
    virtual void shutdownEditorUI() {}
    // editorWantsInput: reports ImGui's WantCaptureMouse / WantCaptureKeyboard so the
    //   host can gate game/camera input when the cursor/keyboard is over an ImGui panel.
    //   Both false when the editor UI is not initialized (game path keeps all input).
    virtual void editorWantsInput(bool& mouse, bool& kbd) const { mouse = false; kbd = false; }
    // editorUIActive: true once initEditorUI has succeeded (the editor overlay is live).
    virtual bool editorUIActive() const { return false; }
};

// Factory. Returns the (clean, original) Vulkan implementation of IRenderDevice.
IRenderDevice* createRenderDevice();

} // namespace x3::rhi
