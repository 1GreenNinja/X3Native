#pragma once
// Render Device (RHI) interface — D1.
// Spec: specs/D1-render-device.spec.md
// IMPORTANT: this header must NOT include <vulkan.h>. All Vulkan types stay
// hidden in the .cpp so game/Lua code never sees the graphics API.
#include <cstdint>
#include <cmath>

namespace x3::rhi {

struct DeviceDesc {
    void*    nativeWindowHandle = nullptr; // HWND on Windows (from GLFW)
    uint32_t width  = 0;
    uint32_t height = 0;
    bool     vsync       = true;
    bool     validation  = false;          // Vulkan validation layers
    // SYNCHRONIZATION VALIDATION (VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_
    // VALIDATION_EXT). NOT part of standard validation — without this flag the
    // layers check API usage (VUIDs) but never look for missing barriers, so a
    // "0 VUID" run says NOTHING about whether the frame's sync is correct.
    // Expensive (the layer shadow-tracks every access), so it is opt-in:
    // `--vksync` on the CLI or X3_VK_SYNC_VALIDATION=1 in the environment.
    // Implies `validation` (the feature is meaningless without the layers).
    bool     syncValidation = false;
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
    // BOOT-TIME hook (docs/BOOT_TIME.md): invoked ONCE as soon as the upload path
    // is live (upload pool + bindless set + VMA — right after the core graphics
    // objects exist), i.e. several hundred ms BEFORE init() returns. The boot uses
    // it to kick the async GLB preload so the warmup overlaps the remaining
    // device init (post stack, SSAO/GI, RT precompile…). May be null.
    void (*onUploadReady)(void* user) = nullptr;
    void* onUploadReadyUser           = nullptr;
    // ---- VERTEX COMPRESSION (Lane 5; --vtxfmt N) ---------------------------
    // Which packed mesh-vertex layout every PSO's vertex input is built with:
    //   0 = the legacy 32 B float3/float3/float2 (today, BIT-EXACT)
    //   1 = 24 B, normal as A2B10G10R10_SNORM (UV still full precision)
    //   2 = 20 B, and UV as half2
    // See engine/rhi/VertexPack.h for the layouts and why this is a device-wide
    // startup decision rather than a per-mesh or runtime one. Silently falls
    // back to 0 (with a log line) if the device cannot use the packed formats as
    // vertex buffers. Non-zero disables GPU skinning (skin.comp writes the
    // legacy layout) — callers transparently take the CPU skinning path.
    uint32_t vertexFormat = 0;
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
    // RT RESIDENCY (setRtOnlyDraws): draws admitted to the TLAS but deliberately
    // kept OUT of the raster stream — the room/portal-PVS survivors that ray
    // tracing needs and the camera does not. Counted SEPARATELY from
    // objectsSubmitted precisely so the unified vis stats block (Visibility.h)
    // keeps meaning what it meant: `tested` is the raster cull's input, and a
    // PVS skip must stay a PVS skip, not become a phantom frustum kill.
    uint32_t rtResidencyDraws = 0;
    float    gpuFrameMs       = 0.0f; // GPU time for the main pass (timestamp queries)
    uint64_t frameCount       = 0;   // total frames presented since init

    // ---- D15 GPU cull (r_cullpath >= 1). Read back with frames-in-flight
    // latency (the counters describe the frame submitted kFramesInFlight frames
    // ago — same guarantee as gpuFrameMs). All zero when the GPU cull is off. ----
    int      gpuCullPath      = 0;   // ACTIVE path this frame (0 CPU, 1 Tier0, 2 Tier1 async, 3 Tier2 mesh)
    uint32_t gpuCullTested    = 0;   // instances the cull shader evaluated
    uint32_t gpuCullDrawn     = 0;   // survivors compacted into visibleInstance[]
    uint32_t gpuCullFrustum   = 0;   // culled by the frustum test
    uint32_t gpuCullHzb       = 0;   // culled by the HZB occlusion test (r_hzb 1)
    // Equivalence harness (setGpuCullEquivalenceCheck): per-readback CPU-evaluated
    // expected survivor count for the SAME frame + cumulative comparison counters.
    uint32_t gpuCullExpected  = 0;   // CPU-side expected `drawn` for the read-back frame
    uint32_t gpuCullEquivFrames     = 0; // frames compared since enable
    uint32_t gpuCullEquivMismatches = 0; // frames where drawn != expected (MUST stay 0)

    // ---- Unified visibility (r_vis / vis-unify; see Visibility.h) ----------
    // Caps the orchestrator resolves against (constant after init).
    bool gpuCullSupported   = false; // cull.comp pipelines live (Tier 0 at least)
    bool asyncCullSupported = false; // dedicated compute queue (Tier 1)
    bool hzbSupported       = false; // depth pyramid targets live
    // Host-injected PVS numbers (setVisHostStats): entities the room/portal PVS
    // skipped at submission this frame + the flood-fill CPU time. Zero when no
    // PVS is active (legacy levels / sandbox worlds).
    uint32_t visRoomsCulled = 0;
    float    visPvsMs       = 0.0f;
    // Per-stage times: the device emit/cull walk (CPU) and the cull.comp / HZB
    // reduce dispatch GPU times (graphics-queue timestamps; 0 on Tier 1 frames —
    // the async dispatch lives off the graphics timeline by design).
    float cullCpuMs = 0.0f;
    float cullGpuMs = 0.0f;
    float hzbGpuMs  = 0.0f;

    // ---- TLAS mutation instrumentation (vis-unify --test-visunify part-D) ---
    // RE-HOMED reconciliation: the empire RT stack ALREADY shipped the TLAS
    // double-buffer (VulkanRT m_tlasRing), so the per-frame scene-mutation
    // vkDeviceWaitIdle is GONE — only the ONE boot-time first-build wait remains.
    // These mirror the shipped VulkanRT counters so --test-visunify can assert the
    // REAL zero steady-state sync-waits (32/32) instead of the old "report" path.
    // tlasBuilds = real (re)builds since init; tlasSyncWaits = CPU-blocking idles
    // the mutation path paid (settles at 1 = boot); tlasCpuMs = CPU ms of the most
    // recent buildTlas. (tlasGrows/tlasCpuMsMax are not tracked by the shipped
    // double-buffer and stay 0 here — kept for HUD/diag layout stability.)
    // ---- Clustered forward lighting (r_clusterlights). All zero when off. ----
    // clusterOverflows is the one that MATTERS: a non-zero value means lights
    // were dropped from a froxel's list and are visibly not lighting it. The old
    // 64-light truncation was silent for a year; this one is not.
    bool     clusterActive      = false;
    uint32_t clusterLights      = 0;  // scene lights fed to the assignment
    uint32_t clusterVisible     = 0;  // lights that reached >= 1 froxel
    uint32_t clusterCulled      = 0;  // lights entirely outside the frustum (free)
    uint32_t clusterAssignments = 0;  // (light, froxel) pairs written
    uint32_t clusterOverflows   = 0;  // pairs DROPPED at the per-froxel cap
    uint32_t clusterOverflowed  = 0;  // distinct froxels that hit the cap
    uint32_t clusterMaxLoad     = 0;  // deepest froxel list this frame
    float    clusterCpuMs       = 0.0f; // CPU ms spent assigning

    uint32_t tlasBuilds    = 0;   // real TLAS (re)builds since init
    uint32_t tlasSyncWaits = 0;   // CPU-blocking idles in the mutation path (-> 1, boot)
    uint32_t tlasGrows     = 0;   // (unused on the double-buffer base; always 0)
    float    tlasCpuMs     = 0.0f; // CPU ms of the most recent mutation path run
    float    tlasCpuMsMax  = 0.0f; // (unused on the double-buffer base; always 0)
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

    // ROLL-CAPABLE camera: pass a full orientation basis (forward + up) instead of
    // yaw+pitch. This is what lets a space fighter LOOP and BANK — the plain
    // setCamera hardcodes up = (0,1,0), so the view can never roll and inverts past
    // vertical (owner: "the controls never felt RIGHT ... the view PINWHEELS").
    // fwd/up need not be exactly orthonormal; the device re-orthonormalizes.
    // Default impl derives yaw/pitch from fwd and calls setCamera (roll dropped) so
    // non-Vulkan / headless devices still work.
    virtual void setCameraBasis(float x, float y, float z,
                                const float fwd[3], const float up[3], float fovDeg) {
        const float yaw   = std::atan2(fwd[2], fwd[0]);
        const float pitch = std::asin(fwd[1] < -1.f ? -1.f : (fwd[1] > 1.f ? 1.f : fwd[1]));
        (void)up;
        setCamera(x, y, z, yaw, pitch, fovDeg);
    }
    // Camera ROLL about the view-forward axis (radians; 0 = upright, positive
    // rolls the horizon clockwise on screen). Same per-frame latch semantics as
    // setCamera. Additive: default is a no-op so headless/test devices and hosts
    // that never roll are byte-identical.
    virtual void setCameraRoll(float rollRadians) { (void)rollRadians; }

    // W8-3: camera FAR-PLANE override (meters). Default 200 m (the historic
    // hardcode — every existing host is pixel-identical without calling this).
    // Open-world vista hosts (surface start / terrain + city screenshot hosts /
    // the streamed world) raise it (~15000 m) so the mountain ranges on the
    // horizon actually draw. Standard-Z + D32F: distant depth precision
    // coarsens, acceptable for horizon geometry (continuous meshes, no distant
    // interleaving). Additive: default impl is a no-op (headless/test devices).
    virtual void setCameraFar(float farMeters) { (void)farMeters; }

    // Scene ambient (hemispheric floor lift in mesh.frag, also the crude IBL term for
    // PBR meshes). Default is a small cool constant; raise it for bright daylit/outdoor
    // scenes (e.g. the showroom) so metal/glass surfaces aren't black. Non-pure (no-op
    // default) so headless / other devices are unaffected.
    virtual void setAmbient(float r, float g, float b) {}

    // Frame the sun's shadow ortho box on a world center + half-extent, instead of the default
    // camera-following ~45 m box. For large scenes (the showroom) so the building + surrounding
    // geometry fall inside the shadow map and actually cast shadows. Non-pure (no-op default).
    virtual void setShadowBounds(float cx, float cy, float cz, float halfExtent) {}

    // ALPHA-CUTOUT SHADOWS. The shadow pass is depth-only (no fragment stage), so an
    // alphaMode==MASK billboard (a snow fir, a people sprite) casts the shadow of its
    // FULL QUAD — under a high sun that reads as hard black rectangles on the ground.
    // Enabling this routes cutout draw groups through a shadow pipeline that runs
    // mesh.frag's exact alpha discard, so a fir casts a fir-shaped shadow. OFF by
    // default: every existing world's shadow map stays bit-for-bit identical.
    virtual void setShadowCutout(bool enable) {}

    // CASCADED SHADOW MAPS (r_csm). The legacy sun shadow is a SINGLE ~45 m
    // ortho box locked to the camera, so sun shadows simply stop ~45 m out —
    // wrong for any open scene and a hard blocker for racing (at 200 km/h that
    // boundary sweeps past the car in 0.8 s). CSM splits the view frustum into
    // `Csm.h`'s kNumCascades slices and renders one shadow map per slice into a
    // 2D array, each fitted to a rotation-invariant bounding sphere and snapped
    // to the shadow texel grid so edges do not swim.
    //
    // enabled = false reproduces the legacy single cascade EXACTLY (same matrix,
    // same layer, same shader branch) so md5/screenshot gates stay bit-exact.
    //
    // `forwardBias` is independent of `enabled`: it slides the LEGACY box forward
    // along the camera axis by that many meters — the cheap interim for racing
    // and a useful A/B reference against real cascades. 0 = historical behaviour.
    //
    // NOTE: setShadowBounds() wins. A host that pins the box has explicitly
    // tuned its scene, so CSM stands down and the pinned box is used unchanged.
    struct CsmParams {
        bool  enabled     = false;
        float lambda      = 0.75f;   // practical-split blend: 0 = uniform, 1 = logarithmic
        float distance    = 250.0f;  // meters of view depth the cascades cover
        float blend       = 0.12f;   // cross-fade band width, as a fraction of a slice
        float forwardBias = 0.0f;    // LEGACY-path forward push along the camera axis (m)
        bool  debug       = false;   // step visibility per cascade so bands are visible
        // r_shadowsnap: quantise the LEGACY (r_csm 0) camera-following box's
        // centre to the world-anchored shadow-texel lattice. The legacy box slid
        // continuously with the camera, so every shadow edge crawled frame to
        // frame — read as SHIMMER at range, where one texel spans several pixels
        // of silhouette. Cascades have always snapped; this gives the one box the
        // legacy path draws the same treatment. false = the historical unsnapped
        // box, bit-for-bit. Independent of `enabled`.
        bool  snapLegacy  = true;
    };
    virtual void setCsmParams(const CsmParams&) {}

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

    // ---- RENDERER DEBUG VIEW (r_debugview) -------------------------------------
    // 0 = off (shipping; byte-identical). 1 = SHADING NORMAL as colour (N*0.5+0.5).
    // A surface whose normal points AWAY from the room cannot receive a single
    // photon from any light in it — it collects only ambient/IBL and reads BLUE,
    // and every symptom points at the art (KNOWN_BUGS R3). This is the instrument
    // that tells you in one frame whether the normal, or the light, is the liar.
    virtual void setDebugView(int mode) { (void)mode; }

    // ---- PAINTERLY LEVERS (ART_BIBLE.md §5) — per-zone atmosphere + grade. ----
    // Host opt-in ONLY: both default fully OFF, and the setters are deliberately
    // SEPARATE from PostFXParams/setPostFX (which app_run re-applies live from
    // cvars every frame) so a zone's opt-in is never clobbered by the cvar loop.
    // Depth fog: a fullscreen Beer-Lambert extinction pass over the HDR scene,
    // recorded ONLY when enabled && density > 0 — worlds that don't opt in render
    // byte-identical (no pass, no pipeline runs). Non-pure (no-op default).
    struct FogParams {
        bool  enabled = false;
        float color[3] = { 0.0f, 0.0f, 0.0f };  // linear HDR fog color
        float density  = 0.0f;                   // extinction per meter (0.002-0.004 = subtle)
        float start    = 0.0f;                   // meters of clean air (viewmodel guard)
        float maxOpacity = 0.85f;                // far-wall cap (no milky wash law)
        // ---- VOLUMETRIC LIGHT SCATTERING (opt-in; default OFF) ----------------
        // When `volumetric` is false the fog pass runs the ORIGINAL flat
        // Beer-Lambert shader and every frame is byte-identical to the pre-
        // volumetric build. When true, the pass raymarches the view ray and
        // accumulates in-scattering from the sun (shadow-mapped -> god rays) and
        // from the frame's forward point lights (-> neon / street-lamp haze),
        // through a Henyey-Greenstein phase function. The color/density/start/
        // maxOpacity above still drive extinction, so the two paths produce the
        // SAME image when scatterStrength == 0.
        bool  volumetric      = false;
        float scatterStrength = 0.0f;    // scattering coefficient (1/m). 0 == flat fog.
        float anisotropy      = 0.70f;   // Henyey-Greenstein g (0.6-0.8 = forward haze)
        int   steps           = 32;      // raymarch steps (clamped 4..64 in-shader)
        float maxDistance     = 500.0f;  // march clamp (m) — cost + far-field guard
        float sunScatter      = 1.0f;    // multiplier on the sun-shaft term
        float lightScatter    = 1.0f;    // multiplier on the point-light haze term
    };
    virtual void setFog(const FogParams& f) {}
    // UNDERWATER CAUSTICS (mesh.frag): dancing refracted-sun filaments on any
    // sunlit surface BELOW the local water plane — the riverbed, the fish, the
    // swimmer. Purely procedural in the fragment stage (no textures, no extra
    // pass): the params ride the per-frame mesh control UBO and the shader
    // modulates the DIRECT SUN term only, so shadowed water stays dark and
    // ambient / point lights are untouched; the effect fades with depth so the
    // shallows dance while the deep stays moody. `waterY` is the CAMERA-LOCAL
    // water surface height — the host queries its own water (river reach or
    // sea) and treats it as locally flat; the shader never evaluates a spline.
    // `time` is host-advanced (deterministic captures — the setWaterParams
    // convention). enabled=false (the default) is byte-identical: the whole
    // shader path is gated on a uniform flag, so worlds without water never
    // spend a single extra ALU.
    struct CausticsParams {
        bool  enabled   = false;
        float waterY    = 0.0f;   // world Y of the local water surface
        float time      = 0.0f;   // animation clock (seconds, host-advanced)
        float intensity = 1.0f;   // 0..1 master scale (1 = calibrated look)
    };
    virtual void setCaustics(const CausticsParams&) {}
    // SURFACE WETNESS (rain). A water film changes three things about a surface
    // and this is all of them: the diffuse DARKENS (light refracts into the film,
    // scatters in the substrate and comes back attenuated), the surface gets
    // SMOOTHER (water fills the micro-cavities that made it rough), and the
    // dielectric F0 rises toward the air-water interface. Together those turn a
    // matte street into a mirror — which is the whole reason the reflection and
    // DDGI work pays off in a Pacific-Northwest harbour that actually rains.
    //
    // `amount` is the GLOBAL soak, not the instantaneous rainfall: surfaces stay
    // wet long after a shower stops, so the host integrates precipitation into
    // this (see x3::game::WetnessModel) rather than passing rain straight through.
    // Per-fragment the shader modulates it by exposure — an upward-facing face
    // catches rain, a soffit or a tunnel bore does not — and by cavity occlusion,
    // because water pools where AO says the geometry dishes inward.
    //
    // amount = 0 (the default) is byte-identical: the shader gate never opens, so
    // every dry world and every existing screenshot is untouched.
    struct WetnessParams {
        float amount    = 0.0f;   // 0..1 global soak (0 = bone dry, gate closed)
        float porosity  = 1.0f;   // 0..1 how much this world's materials darken when wet
        float puddles   = 1.0f;   // 0..1 how strongly cavity/AO pooling reads
        float minRough  = 0.06f;  // roughness a fully-soaked surface converges to
    };
    virtual void setWetness(const WetnessParams&) {}

    // LYING SNOW, 0..1 ground cover (app/wetness.h WetnessModel::snowCover()).
    // Brings the terrain's SNOWLINE down rather than whitening the world flat:
    // the tops go first and the valleys go last, because height is cold, and
    // watching the white come down the range is the effect. 0 (the default)
    // leaves the permanent altitude-only snow cap exactly as it was.
    virtual void setSnowCover(float) {}

    // ---- TERRAIN MATERIAL + TEMPORAL STABILITY (outdoor-polish lane) --------
    // Two problems, one uniform, because they are the same problem seen twice:
    // what the ground DOES with light, and what it does with light when a pixel
    // is wider than the detail it is showing.
    //
    // `antiAlias` — at range one screen pixel spans several METRES of ground.
    //   That is wider than the stochastic-tiling hex lattice (~16 m) and the
    //   fine splat-mask noise (~28 m), so their per-pixel weights alias no
    //   matter how complete the mip chain is: the taps are filtered, the
    //   WEIGHTS BETWEEN THEM are not. Sub-pixel camera motion then re-rolls
    //   them and the mountain sparkles. This fades those terms — and the normal
    //   relief, whose detail is likewise below the footprint — out with
    //   distance. 0 = the historical (aliasing) math, exactly.
    //
    // `perBand` — terrain used ONE flat dielectric roughness (0.5) for grass,
    //   cliff, snow and sand alike. This blends in an authored per-band
    //   roughness/specular instead. 0 = that flat 0.5, exactly.
    //
    // `sparkle` — the owner's calibration: hard-baked sand should GLINT in a low
    //   evening sun (1) or stay matte (0). It is a MATERIAL property, not an
    //   aliasing artefact — which is the whole point of fixing the aliasing
    //   first, so a deliberate sparkle can be told apart from a broken one.
    struct TerrainMatParams {
        float antiAlias   = 1.0f;   // 0 = historical, 1 = full footprint fade
        float perBand     = 1.0f;   // 0 = flat dielectric 0.5, 1 = authored bands
        float sparkle     = 1.0f;   // sand-band gloss 0..1
        float roughScale  = 1.0f;   // multiplier on the authored band roughness
    };
    virtual void setTerrainMaterial(const TerrainMatParams&) {}

    // Filmic grade + split-tone + vignette in the composite pass, master-lerped by
    // `strength` (0 = bit-identical passthrough — the shader never enters the block).
    struct GradeParams {
        float strength = 0.0f;                          // 0 = off (identity)
        float shadowTint[3]    = { 1.0f, 1.0f, 1.0f };  // multiplied into shadows (teal)
        float highlightTint[3] = { 1.0f, 1.0f, 1.0f };  // multiplied into highlights (warm)
        float saturation = 1.0f;                        // 1 = unchanged
        float vignette   = 0.0f;                        // 0..~0.12 per the bible
    };
    virtual void setGrade(const GradeParams& g) {}
    // CINEMATIC FILMIC POST (feat/filmic-post): the cutscene FILM LOOK — vignette
    // + animated luma-weighted film grain + split-tone grade + saturation, applied
    // in the composite pass AFTER tonemap and AFTER the zone grade (the last word
    // on the frame). Owned by cinematic playback (CinematicScene::applyLook sets
    // it, restoreLook clears it — the look never leaks into gameplay). Defaults
    // are OFF and mathematically identity: enabled=false never enters the shader
    // block (byte-identical output), and enabled-with-defaults is exact identity
    // too (every sub-op self-gates; see --test-filmic). PostFXParams.filmicAllowed
    // (r_filmic) is the live kill-switch for A/B.
    struct FilmicParams {
        bool  enabled   = false;
        float vignette  = 0.0f;   // 0..~0.35 corner darkening (film: felt, not seen)
        float grain     = 0.0f;   // 0..~0.15 grain amplitude (luma-weighted in-shader)
        float grainSeed = 0.0f;   // seed OFFSET; the device advances a per-frame
                                  // counter on top so the grain always crawls
        float shadowTint[3]    = { 1.0f, 1.0f, 1.0f };  // shadows pulled toward (teal)
        float highlightTint[3] = { 1.0f, 1.0f, 1.0f };  // highlights pulled toward (warm)
        float saturation = 1.0f;                        // 1 = unchanged
    };
    virtual void setFilmic(const FilmicParams&) {}

    // CPU per-object frustum cull toggle (live r_frustumcull cvar; default ON). When
    // disabled the draw path is byte-identical to before the cull existed
    // (objectsDrawn == every submitted instance). Conservative world-sphere vs
    // frustum test that mirrors the GPU cull.comp (D15 equivalence baseline).
    // Non-pure (no-op default) so headless / other devices are unaffected.
    virtual void setFrustumCullEnabled(bool enabled) {}

    // ---- D15 GPU-driven culling (r_cullpath) -------------------------------
    // path: -1 = auto (best supported GPU tier), 0 = CPU cull exactly as today,
    // 1 = Tier 0 (compute cull on the graphics queue), 2 = Tier 1 (async compute
    // queue), 3 = Tier 2 (mesh-shader meshlets, opt-in). Unsupported requests
    // clamp DOWN to the best available tier; 0 is always honored. Non-pure
    // (no-op default) so headless / other devices are unaffected.
    virtual void setCullPath(int path) {}
    // HZB occlusion phase on top of the GPU frustum cull (r_hzb; needs path >= 1).
    virtual void setHzbEnabled(bool enabled) {}
    // Equivalence harness (--test-gpucull + soak): when on, the CPU evaluates the
    // IDENTICAL cull predicate per instance each frame and the device compares the
    // GPU statDrawn readback against it (stats().gpuCullEquiv*). Costs a CPU cull
    // walk per frame — test/diagnostic only.
    virtual void setGpuCullEquivalenceCheck(bool enabled) {}

    // ---- Unified visibility host stats (vis-unify; see Visibility.h) -------
    // The app-side room/portal PVS runs BEFORE submission, so the device can't
    // see what it skipped. The host injects the per-frame PVS numbers here so
    // stats() carries the whole conserving pipeline (rooms -> frustum -> hzb ->
    // drawn). Call once per frame (before endFrame); sticky until re-set.
    virtual void setVisHostStats(uint32_t roomsCulled, float pvsMs) {}

    // ---- RT RESIDENCY: geometry ray tracing needs and raster does not -------
    // STICKY submission mode. While ON, every drawMesh*/drawMeshPBR/... call is
    // recorded as an RT-ONLY draw: it enters the scene TLAS (with its own row in
    // the stable RT material table) but is EXCLUDED from the raster stream — no
    // object-SSBO row, no indirect instance, no shadow/depth/colour replay.
    //
    // WHY THIS EXISTS. The room/portal PVS runs at SUBMISSION (Scene::render
    // skips room-invisible entities), so PVS-culled geometry never became a draw
    // record and therefore never entered the TLAS at all. Frustum culling had the
    // same consequence and was fixed downstream (the cull-compacted material index
    // -> the stable per-record table); the PVS half was not, because there was
    // nothing downstream to fix: the geometry simply was not there. DDGI,
    // reflections, RT shadows and RT acoustics all behaved as if the wall in the
    // next room did not exist — it cast no shadow and bounced no light.
    //
    // The raster path emits a group as `firstInstance = baseRow, instanceCount =
    // drawn`, so a group's visible instances MUST stay contiguous. That is why an
    // RT-only record is dropped at exactly the same point as a frustum-culled one
    // (before its SSBO row is assigned): `row` never advances for it, so the
    // survivors it sits between stay adjacent and no indirect command changes.
    //
    // Non-pure (no-op default) so headless / other devices are unaffected; a
    // device that ignores it simply keeps today's behaviour. Reset to false at
    // beginFrame so a host that forgets to clear it cannot leak into next frame.
    virtual void setRtOnlyDraws(bool on) {}

    // Metal ambient-specular floor strength (mesh.frag IBL path): metals in a DARK
    // baked environment keep an F0-tinted ambient response instead of going black.
    // 1.0 = default ON, 0.0 = off. Drives the live r_metalambient cvar.
    virtual void setMetalAmbient(float s) {}

    // ---- CLUSTERED (froxel) FORWARD LIGHTING — r_clusterlights -------------
    // OFF: the legacy path. Every fragment loops the fixed 64-entry point-light
    //      UBO array in full, so the scene light cap is 64 and the per-pixel cost
    //      is O(64) regardless of how many lights actually reach that pixel.
    // ON:  the view frustum is diced into a froxel grid (16x9x24 by default; see
    //      engine/rhi/ClusterLights.h) and each light is assigned to the froxels
    //      its sphere of influence overlaps. A fragment iterates ONLY its own
    //      froxel's list, out of a light set that can now hold kMaxSceneLights
    //      (1024) instead of 64.
    //
    // The two paths are deliberately NOT unified: OFF must stay bit-for-bit the
    // legacy render, because the repo's md5 / screenshot gates are pinned to it.
    // Live — safe to toggle mid-frame-loop.
    virtual void setClusterLights(bool enable) {}

    // IBL ambient intensity (mesh.frag ssao.ibl.y): scales the ENTIRE image-based
    // ambient term (diffuse irradiance + prefiltered specular) once a sky env is
    // baked. 1.0 = default (unchanged behavior). SEAM 2 (world merge): a host with
    // a bright outdoor sky wrapped around a mood-calibrated INTERIOR (canonlevel's
    // facility tower) dials this down so the sky's irradiance doesn't wash the
    // interior rooms white — the analytic-sky background, the direct sun and the
    // glass pass's own env reflections are all untouched.
    virtual void setIblIntensity(float s) {}
    // ENV-SPECULAR SCALE (r_iblspec, default 1.0 = unchanged). setIblIntensity()
    // scales the environment's DIFFUSE and SPECULAR lobes together; this scales the
    // specular lobe ALONE, so a dark interior can still contain bright, reflective
    // metal. (Metals have no diffuse lobe -- their whole ambient response IS the
    // prefiltered env specular -- while concrete/plaster are almost pure diffuse.)
    virtual void setIblSpecular(float s) {}

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
        bool  velocity       = false;  // r_velocity: per-object screen-space motion
                                       // vectors feed the TAA reprojection (fixes
                                       // fast dynamic/skinned ghosting; also the
                                       // DLSS input). DEFAULT OFF so the A/B
                                       // determinism basins (default/notaa/
                                       // legacypost/norefl screenshots) are
                                       // byte-identical to the pre-velocity build;
                                       // set r_velocity 1 to enable. 0 -> TAA uses
                                       // camera-only reprojection (pre-velocity
                                       // behavior). No-op when taa is off or
                                       // velocity.spv is absent (graceful fallback).
        bool  filmicAllowed  = true;   // r_filmic: master gate on the cinematic
                                       // filmic post (setFilmic). Default TRUE so
                                       // headless/screenshot paths that never call
                                       // setPostFX still honor a host's setFilmic;
                                       // the look itself stays OFF until a host
                                       // enables it, so worlds are byte-identical.
    };
    virtual void setPostFX(const PostFXParams&) {}

    // Introspection for the app cvar layer (the r_velocity string-binding lives in
    // app/main.cpp; the engine exposes the live value + availability so the cvar
    // can read them back). See docs/VELOCITY_DLSS_REPORT.md for the ~5-line app
    // follow-up that wires the "r_velocity"/"--velocity" string. Base defaults
    // match the OFF default; the Vulkan device overrides with live state.
    virtual bool velocityEnabled() const { return false; }
    virtual bool velocityAvailable() const { return false; }  // pipeline+target exist

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

    // ---- DISCRETE MESH LOD chain (Lane 5) -----------------------------------
    // Create N meshes that SHARE ONE device-local vertex buffer and differ only
    // in their index buffer. `idx[i]` / `icount[i]` are the index list of level
    // i (0 = full detail); every index must address `verts`. Returns the number
    // of levels actually created and writes their handles to outMeshes[0..n).
    //
    // WHY THIS SHAPE: the renderer already keys draw GROUPS on the mesh id and
    // emits exactly one VkDrawIndexedIndirectCommand per group (firstIndex 0,
    // vertexOffset 0). Handing each LOD its own mesh id therefore needs ZERO
    // changes to the group/indirect path, the GPU cull, or the CSM shadow loop —
    // selecting a level is just submitting a different handle, and all instances
    // that picked the same level batch into one indirect draw automatically.
    // A single mesh carrying multiple index RANGES would instead need (mesh,lod)
    // group keys and a per-LOD firstIndex threaded through cull.comp.
    // Sharing the vertex buffer is what keeps the chain cheap: the decimator uses
    // SUBSET placement (app/mesh_decimate.h), so coarse levels only ever
    // reference vertices level 0 already uploaded.
    //
    // Each level gets its own bounding sphere/AABB, computed from the vertices it
    // actually references. Destroying the levels in any order is safe (the shared
    // vertex buffer is refcounted). Default impl creates INDEPENDENT meshes via
    // createMesh so a null/stub device still works.
    virtual uint32_t createMeshLodChain(const MeshVertex* verts, uint32_t vcount,
                                        const uint32_t* const* idx, const uint32_t* icount,
                                        uint32_t levels, MeshHandle* outMeshes) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < levels; ++i) {
            outMeshes[i] = createMesh(verts, vcount, idx[i], icount[i]);
            if (!outMeshes[i].valid()) break;
            ++n;
        }
        return n;
    }

    // Byte stride of the mesh vertex format this device actually resolved at
    // init (see DeviceDesc::vertexFormat / engine/rhi/VertexPack.h). 32 unless a
    // packed format was requested AND the device supports it — so a caller that
    // wants to REPORT the compression must read this, not the requested value.
    virtual uint32_t meshVertexStride() const { return 32; }

    // Total bytes of mesh VERTEX buffer memory currently allocated, counting a
    // shared LOD-chain vertex buffer once. This is the part of the compression
    // win that is guaranteed rather than hardware-dependent: it is exactly
    // (sum of vertex counts) * meshVertexStride().
    virtual uint64_t meshVertexBytes() const { return 0; }

    // Camera state the app-side LOD selector needs (app/mesh_lod.h): the eye
    // position and vertical FOV last handed to setCamera/setCameraBasis, plus the
    // render height in pixels. Kept as a query rather than plumbed through every
    // host so LOD selection cannot drift out of sync with the camera the frame is
    // actually drawn with. Default reports a benign 60 deg / 1080p at the origin.
    virtual void cameraLodInfo(float outEye[3], float& outFovYDeg, uint32_t& outHeightPx) const {
        outEye[0] = outEye[1] = outEye[2] = 0.0f;
        outFovYDeg = 60.0f;
        outHeightPx = 1080;
    }

    // Create a sampled texture from tightly-packed RGBA8 (w*h*4 bytes). `srgb`
    // selects the storage format (sRGB for color, UNORM for data/linear).
    virtual TextureHandle createTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb) = 0;
    virtual void          destroyTexture(TextureHandle) = 0;

    // CPU-side local-space AABB of a mesh, computed once at createMesh from the
    // submitted vertices (zero per-frame cost; no readback). Returns false if the
    // handle is unknown or the device does not track bounds (default). The world
    // map's top-down tile bake reads this to rasterize entity footprints from the
    // REAL geometry (outMin/outMax are {x,y,z} in the mesh's local space; combine
    // with the entity transform for the world AABB).
    virtual bool meshBounds(MeshHandle, float /*outMin*/[3], float /*outMax*/[3]) const {
        return false;
    }

    // ---- BOOT-TIME upload batching (docs/BOOT_TIME.md) ----------------------
    // Between beginUploadBatch()/endUploadBatch(), createMesh/createTexture record
    // their staging copies into ONE shared command buffer instead of doing a
    // blocking submit + fence wait EACH (~ms of fixed cost per call — the 16 s
    // world-build pole was ~2000 tiny submits). The batch is flushed (single
    // submit + single wait) by endUploadBatch, and AUTOMATICALLY by beginFrame or
    // by any other one-shot GPU op, so ordering/visibility semantics are identical
    // to the unbatched path. Nestable-safe: extra calls are no-ops. Default no-op
    // (headless/null devices keep the plain blocking path).
    virtual void beginUploadBatch() {}
    virtual void endUploadBatch() {}

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
    // (the textures keep their own bindless slots) and the existing draw
    // path; only a previously-reserved pad field in the SSBO row is now used to
    // TWO independent optional groups, both defaulted, so every existing caller
    // keeps compiling:
    //   rockHigh  — a 5th ALBEDO: a second rock set the splat blends in with
    //               ALTITUDE, so the high massif reads as different stone from
    //               the road cuttings. Invalid = absent; the shader falls back to
    //               tinting the one rock set (identical to the 4-texture era).
    //   grassN..sandN — per-layer NORMAL maps: the splat's RELIEF. Must be
    //               created with srgb=false. Invalid = that layer has no relief
    //               and uses the geometry normal; all four invalid = the exact
    //               pre-relief renderer.
    virtual TextureHandle registerTerrainMaterial(TextureHandle grass, TextureHandle rock,
                                                  TextureHandle snow,  TextureHandle sand,
                                                  TextureHandle rockHigh = {},
                                                  TextureHandle grassN = {}, TextureHandle rockN = {},
                                                  TextureHandle snowN  = {}, TextureHandle sandN = {}) = 0;

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
    // `clearcoat`/`clearcoatRough` (car paint): a second fixed-F0 (0.04) specular
    // lobe over the base layer (mesh.frag), carried per-object in a spare SSBO
    // lane (the terrain-pack field — mutually exclusive with TERRAIN). 0 = none;
    // every existing call site keeps the default and shades byte-identically.
    //
    // `selfLight` (0..1) — CANON: SHIPS ARE SELF-LIT (Star Trek convention). A hull
    // must never die to a black silhouette just because the star is on the far side.
    // This is a SHAPED self-illumination, NOT an ambient/emissive floor: mesh.frag
    // builds it from a Fresnel rim + an N.V form term and MULTIPLIES it by
    // (1 - N.L*shadow), so it exists only on the side the star is NOT lighting and
    // fades to zero on the lit side. The hull still shades honestly from the sun;
    // this only keeps the dark side off the floor. 0 = none (every existing call
    // site keeps the default and shades byte-identically).
    virtual void          drawMeshPBR(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                                      TextureHandle /*normal*/, TextureHandle /*metalRough*/,
                                      const float baseColorFactor[4], const float emissive[4],
                                      const float model[16], bool /*alphaMask*/ = false,
                                      bool /*alphaBlend*/ = false, TextureHandle /*emissiveTex*/ = {},
                                      TextureHandle /*detailTex*/ = {}, float /*detailUvScale*/ = 1.0f,
                                      float /*clearcoat*/ = 0.0f, float /*clearcoatRough*/ = 0.05f,
                                      float /*selfLight*/ = 0.0f,
                                      // BLACK-PROP FIX: per-object metallic CLAMP for dark-albedo kit
                                      // props whose MR map bakes metallic=1 (which zeroes the diffuse
                                      // lobe and renders them black in low-IBL interiors). 1.0 = no
                                      // clamp (every existing call site shades byte-identically).
                                      // Rides the spare glass .w lane; selfLight rides terrainPack2 —
                                      // orthogonal, so a ship hull can carry both.
                                      float /*metallicScale*/ = 1.0f,
                                      // FOLIAGE (>0): mesh.frag wraps the diffuse + adds warm
                                      // back-translucency so the sun glows through canopies.
                                      float /*foliage*/ = 0.0f) {
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
        // STREET LIGHT (additive glow mode): 0 (default) = normal glass, byte-
        // identical for every existing pane. > 0 flags this draw as an ADDITIVE
        // VOLUMETRIC GLOW surface (fake light cones / ground light pools): the
        // fragment shader skips refraction/specular and instead adds
        // emissive * texel * pow(max(dot(N,V),0), additive) over the scene —
        // the VALUE is the view-angle rim-fade exponent (higher = softer
        // silhouette edges; ~1.5 for light cones, ~0.05 for flat ground pools
        // that must survive grazing views). Back faces self-extinguish
        // (dot(N,V) <= 0), so the double-sided glass pipeline draws one soft
        // front layer and overlapping glows ACCUMULATE (no replace artifact).
        float additive   = 0.0f;
        // EMISSIVE MAP (display glass): 0 (default) = the per-object emissive is a
        // FLAT glow over the whole pane — byte-identical for every existing surface.
        // 1 = the emissive is MODULATED BY THE BOUND BASE-COLOR TEXEL, so the pane
        // glows only WHERE ITS TEXTURE IS BRIGHT and black texels stay black. This is
        // the glass-pass twin of the opaque PBR route's `emissiveTex` (the club OLED
        // move: "black texels stay dark"), and it is what lets a BLACK GLASS holo
        // screen carry CRISP GLOWING TEXT instead of a flat blue flood — the flat
        // term can only wash the panel, because it cannot see the readout.
        // Intermediate values cross-fade. Uses the SAME texture already bound as
        // baseColor (a display's image IS its emission mask), so it costs no extra
        // binding and no extra sample.
        float emissiveMap = 0.0f;
    };

    // Submit a translucent glass draw. `glass.opacity` overrides baseColorFactor's
    // alpha (the see-through dial). `emissive` is the same per-object HDR glow term
    // as drawMeshEmissive (holo glass keeps its glow); pass nullptr for none. The
    // device flags the per-object row GLASS so it routes to the transparent pass.
    // `alphaBlend` (default false = unchanged): when true the glass draw is placed in
    // the BLEND partition so it is EXCLUDED from the depth pre-pass (which has no
    // fragment stage and would otherwise write the glass geometry's depth, wrongly
    // occluding real opaque geometry behind it under the EQUAL color pass). The
    // dedicated glass pass still renders it identically — this only changes which
    // CPU partition it lands in. Use for translucent shells that sit IN FRONT of an
    // opaque body you must still see through them (e.g. a sun's corona over its core).
    virtual void          drawMeshGlass(const FrameContext&, MeshHandle, TextureHandle baseColor,
                                        const float baseColorFactor[4], const float emissive[4],
                                        const GlassMaterial& glass, const float model[16],
                                        bool alphaBlend = false) = 0;

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
        // CLOUD COVER, 0..1. 0 = the clear analytic sky exactly as before (the
        // cloud term multiplies out, so every existing scene is unchanged);
        // ~0.35 = scattered fair-weather cumulus; 1 = overcast. Rides the sky
        // UBO's last reserved param lane, so no new binding.
        float cloud         = 0.0f;
        float zenith[3]     = { 0.10f, 0.28f, 0.66f }; // overhead sky color (linear); per-scene (default = old global)
        float horizon[3]    = { 0.62f, 0.74f, 0.92f }; // horizon glow color (linear); per-scene (default = old global)
        // SCENE SUN RADIANCE for mesh.frag's directional key (separate from
        // `sunIntensity`, which only scales the SKY DISK + glow). Multiplies the
        // shader's kSunColor. 1.0 == the historical hardcoded sun, so every world
        // that never touches this field is byte-identical. Space scenes raise it:
        // a STAR is the only light out there, and the ship hulls are near-black
        // paint, so an honest star has to be hot to shade them without crutches.
        float sunLight      = 1.0f;
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
    //
    // DENOISE STAGE (r_refldenoise). refl.comp used to write and mesh.frag used
    // to consume RAW — reflections had no equivalent of the GI chain's
    // gather -> temporal -> denoise -> apply. On real car paint that showed as
    // blotchy mottling (mean |px - 9x9 local mean| on flat door skin: 5.53 with
    // reflections off vs 7.69 shipped) which the consumer-side blur could not
    // reach, because the noise is IN THE BUFFER. `denoiseIters` a-trous
    // iterations of an edge-aware (depth + normal) filter now run between the
    // two; see engine/rhi/ReflDenoise.h for the filter and for the honest
    // roughness decision. 0 = OFF and BIT-EXACT to the pre-denoise renderer.
    struct ReflectionParams {
        bool  ssr        = false;   // master gate (r_ssr; OFF until the app enables it)
        bool  rtFallback = true;    // ray-query fallback where SSR misses (r_rtreflections)
        bool  fullRes    = false;   // r_reflquality: false = half-res (default), true = full
        float intensity  = 1.0f;    // blend-weight scale on the composed reflection [0..1]
        // ---- denoise stage ----
        int   denoiseIters      = 4;      // r_refldenoise: a-trous iterations; 0 = OFF (bit-exact)
        float denoiseDepthSigma = 0.06f;  // r_refldn_depth: depth stop, RELATIVE to view distance
        float denoiseNormalPow  = 16.0f;  // r_refldn_normal: normal stop exponent
        // r_refldn_disc: scale applied to mesh.frag's roughness-driven glossy
        // disc when the stage ran. The wide averaging has moved into a pass that
        // CAN reject across a depth/normal edge, so the un-depth-tested consumer
        // disc — the source of the silhouette bleed — shrinks. Ignored (treated
        // as exactly 1.0) when denoiseIters is 0.
        float denoiseDiscScale  = 0.40f;
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

    // ---- RAY-TRACED SOFT SHADOWS (r_rtshadows) ------------------------------
    // Per-pixel inline ray-query shadows in the mesh fragment stage, traced
    // against the SAME scene TLAS RT-AO/reflections/DDGI share:
    //   tier 0: CSM-only — today's path, bit-identical (the plain mesh.frag
    //           pipeline is bound; none of this code exists in its SPIR-V).
    //   tier 1: SUN — one cone-jittered ray per pixel (angular radius
    //           sunSizeDeg) min()-combined with the CSM term, so static
    //           geometry gets soft distance-scaled penumbra while skinned
    //           characters (absent from the static TLAS) keep their raster
    //           shadows. Per-frame jitter rotation; TAA accumulates the noise.
    //   tier 2 (default): sun + POINT LIGHTS — lamps finally cast: the first
    //           `pointMax` lights with a non-negligible contribution at the
    //           pixel each get one ray toward a jittered point on the light's
    //           spherical source (radius pointRadius); penumbra widens with
    //           occluder distance. Beyond the budget: unshadowed (existing
    //           behavior).
    // TIER-GATED: requires ray-query hardware; on anything else (Pascal) the
    // stored tier is ignored and the raster path is byte-for-byte unchanged
    // (the same auto-0 gating DDGI/reflections use). Opaque-only rays v1
    // (alpha-cutout occludes as the full quad — documented, same as RT AO).
    struct RtShadowParams {
        int   tier        = 2;      // r_rtshadows (0/1/2; auto-0 without ray query)
        float sunSizeDeg  = 0.5f;   // r_rtsun_size: sun angular RADIUS (degrees)
        int   pointMax    = 4;      // r_rtpoint_max: point shadow rays per pixel
        float pointRadius = 0.10f;  // r_rtpoint_size: light source radius (m)
    };
    // Cached + re-applied each frame, like setRtaoParams. Default no-op.
    virtual void          setRtShadowParams(const RtShadowParams&) {}

    // ---- SKINNED-CHARACTER TLAS REFIT (r_skinnedrt) -------------------------
    // Toggle whether visible skinned characters (monsters/NPCs) are added to the
    // scene TLAS, so RT shadows + reflections + DDGI + RT acoustics all see them.
    // Per-frame the backend builds/refits a BLAS for each skinned char from its
    // current pose (the same compute-skinned vertices the raster path draws) and
    // adds its instance to the multi-consumer TLAS. BUDGETED (a per-frame cap) and
    // REFIT-preferred (cheap VK_..._MODE_UPDATE after the first build). Reads the
    // most-recently-completed skinned output -> RT lags the raster pose by ONE
    // frame (intentional, imperceptible for shadows/AO/audio; avoids a new mid-
    // frame stall). DEFAULT ON. GATED: a non-RT GPU (Pascal) or this toggle OFF ->
    // skinned chars stay raster-only and the static RT path is byte-identical.
    virtual void          setSkinnedRtEnabled(bool /*enabled*/) {}
    virtual bool          skinnedRtEnabled() const { return false; }
    // Introspection (tests/telemetry): how many skinned characters were present in
    // the TLAS on the last built frame (0 when off / unsupported / none visible).
    virtual uint32_t      skinnedRtInstanceCount() const { return 0; }

    // ---- RT ACOUSTICS — audio rays through the render TLAS (snd_rtacoustics) --
    // ASYNC batched ray queries against the SAME scene TLAS the RT AO /
    // reflections / DDGI passes use. The audio layer (engine/audio/RtAcoustics)
    // batches per-emitter occlusion fans + a periodic listener room-probe sphere
    // into one submit — a few hundred rays. ASYNC because a synchronous fence
    // wait on the graphics queue would stall behind the in-flight frame's GPU
    // work (tens of ms on heavy scenes); submit + next-update harvest costs the
    // game thread ~microseconds and audio tolerates one update of latency.
    // POD only — no Vulkan types cross the boundary.
    //
    // traceAudioRaysSubmit: kick a batch. Returns false when ray tracing is
    // unsupported, the TLAS is not built yet (the first call ARMS the per-frame
    // TLAS build; data begins a frame later), a previous batch is still in
    // flight/unharvested, or count exceeds the internal capacity (1024).
    //
    // traceAudioRaysHarvest: poll the last submitted batch. Returns its ray
    // count and fills outHitT[i] with each ray's CLOSEST hit distance in meters
    // (< 0 = miss within tMax) once the GPU finished; 0 = still in flight;
    // -1 = nothing in flight / unsupported / capacity too small (batch dropped).
    // Defaults: inert no-ops (headless / non-RT devices).
    struct AudioRay {
        float ox = 0, oy = 0, oz = 0;  // world-space origin
        float tMax = 1.0f;             // ray length (meters)
        float dx = 0, dy = 1, dz = 0;  // direction (normalized by the caller)
        float pad = 0;                 // std430 vec4-pair alignment
    };
    virtual bool          traceAudioRaysSubmit(const AudioRay*, int /*count*/) { return false; }
    virtual int           traceAudioRaysHarvest(float* /*outHitT*/, int /*capacity*/) { return -1; }

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
        // HORIZON HANDOFF COLOR (linear). The Gerstner patch is a FINITE square
        // centred on the camera; a world that also draws a far-ocean mesh beyond
        // it (Echo Harbor bakes a 28 km flat quad into the island GLB) shows a
        // hard, dead-straight, camera-locked seam wherever the two meet, because
        // the patch fades to the ANALYTIC SKY while the far mesh is shaded as a
        // dielectric. Set this to the colour that far mesh actually renders as
        // and the patch fades into IT instead of into the sky, so the seam has
        // nothing left to reveal. Negative red (the default) keeps the historic
        // sky-fade behavior byte-for-byte, for every world that has no far mesh.
        float horizonColor[3] = { -1.0f, -1.0f, -1.0f };
        // WATER CLARITY (0..1; default 0 = the historic OPAQUE surface,
        // byte-identical for every existing world). Above 0 the surface is
        // alpha-blended over the lit scene: face-on SHALLOW water turns
        // translucent (you see the bed, the fish, a swimmer's body through
        // it), going opaque again with depth (the shallow->deep gradient) and
        // at grazing angles (Fresnel — distant water stays a mirror). This is
        // what makes a river read as WATER instead of a painted ribbon: the
        // shoreline shows its bed, and life under the surface is visible.
        float clarity = 0.0f;
        // ---- RIVER MODE (task #32 — ONE water truth) -----------------------
        // riverNodeCount == 0 (the default) keeps the historic FLAT plane at
        // `seaLevel`, byte-identical for every ocean world. When >= 2, the
        // patch's surface Y follows the closest-approach interpolation of this
        // polyline's per-node water level (the SAME table the terrain carve
        // and worldWaterLevelAt use — the drawn plane and the query can no
        // longer split truth). Outside `riverHalfWidth` of the spine the
        // surface alpha-fades out (the waterline the ribbon mesh would have),
        // EXCEPT inside the ocean basin disc (basinCenter/basinRadius), where
        // the level hands off to `oceanLevel` instead (the estuary meets a
        // real sea; terrain above oceanLevel clips it into shoreline). Nodes
        // are (x, z, waterY) in world metres.
        static constexpr uint32_t kMaxRiverNodes = 20;
        uint32_t riverNodeCount = 0;
        float    riverHalfWidth = 0.0f;
        float    riverNodes[kMaxRiverNodes][3] = {};
        float    basinCenter[2] = { 0.0f, 0.0f };
        float    basinRadius    = 0.0f;      // 0 => no ocean fallback disc
        float    oceanLevel     = 0.0f;
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
    // drawHudImage: a textured HUD rectangle sampling an app-created texture
    // (createTexture) — the world-map tile compositor's primitive. Same pixel
    // space / blending / ordering as drawHudQuad; `rgba` tints (1,1,1,1 = as-is);
    // u0..v1 select the sampled sub-rect (defaults = the whole texture). Non-pure
    // no-op default so the headless stub and non-Vulkan devices are unaffected.
    virtual void drawHudImage(const FrameContext&, TextureHandle /*tex*/,
                              float /*xPx*/, float /*yPx*/, float /*wPx*/, float /*hPx*/,
                              const float /*rgba*/[4],
                              float /*u0*/ = 0.0f, float /*v0*/ = 0.0f,
                              float /*u1*/ = 1.0f, float /*v1*/ = 1.0f) {}
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

    // ---- LANE 6: named frame cost breakdown (per-pass GPU + per-zone CPU) ----
    // dumpPerfBreakdown() logs a sorted table of every render-graph pass's REAL
    // GPU timestamp cost plus the CPU zone partition of the frame, averaged over
    // the frames accumulated since the last dump/reset, then resets the window.
    // `why` is a short tag printed in the header ("console", "auto", ...).
    // Env: X3_PASSDUMP=<seconds> auto-dumps on an interval; X3_PASSTIMERS=0
    // disables the timestamps entirely (the A/B for measurement overhead).
    virtual void dumpPerfBreakdown(const char* why) { (void)why; }
    virtual void setPassTimers(bool on) { (void)on; }
    virtual bool passTimers() const { return false; }

    // ---- ZERO-STUTTER frame-pacing telemetry (r_frametelemetry / --test-framepacing)
    // Snapshot of the device's frame-time ring buffer + "late creation" audit
    // counters. CPU times are endFrame-to-endFrame wall deltas; GPU times come
    // from the existing per-frame timestamp queries. Percentiles cover the
    // POST-WARMUP samples only (warmup = PacingParams::warmupFrames). The late-
    // creation counters are the receipts for the zero-stutter guarantee: every
    // pipeline/shader-module/descriptor-pool must be created at boot (or at a
    // declared recreate boundary) — anything created mid-frame after the first
    // frame began increments these and, under strictPso, logs a [stutter] error.
    struct FramePacing {
        // CPU frame-time percentiles (ms) over the post-warmup ring window.
        float cpuP50 = 0, cpuP95 = 0, cpuP99 = 0, cpuP999 = 0, cpuMax = 0;
        // GPU frame-time percentiles (ms) — same window, timestamp-query times.
        float gpuP50 = 0, gpuP95 = 0, gpuP99 = 0, gpuP999 = 0, gpuMax = 0;
        uint32_t samples = 0;          // post-warmup frames measured
        uint32_t spikes  = 0;          // post-warmup frames > max(spikeFactor*median, median+floorMs)
        // Spikes with NO attributed cause (no pipeline/module/pool/alloc/AS-build/
        // IBL-bake/recreate event that frame). Attributed spikes are declared
        // scene-mutation boundaries (e.g. a door opening triggers a TLAS rebuild —
        // logged, known, TODO: async double-buffered TLAS build); UNattributed
        // spikes are unexplained pacing failures and gate --test-framepacing.
        uint32_t spikesUnattributed = 0;
        // Late-creation audit (the strict-PSO gate; all must stay 0 in steady state):
        uint32_t psoLate     = 0;      // pipelines created after the first frame began
        uint32_t modulesLate = 0;      // shader modules created after the first frame began
        uint32_t poolsLate   = 0;      // descriptor pools created after the first frame began
        // Boot receipts:
        uint32_t psoTotal    = 0;      // pipelines created since init (boot precompile count)
        float    psoBootMs   = 0;      // wall-clock ms spent in pipeline creation
        uint64_t cacheLoaded = 0;      // VkPipelineCache bytes loaded from disk at boot
        // ---- TLAS DOUBLE-BUFFER receipts (#5 PART 1) -----------------------
        // The proof the ring removed the per-frame WAR-hazard device wait. With a
        // double-buffered TLAS, the steady-state per-frame device wait around the
        // TLAS rebuild must be ZERO even with skinned-RT on + skinned chars visible.
        uint32_t tlasBuilds       = 0; // total TLAS (re)builds since init
        uint32_t tlasSyncWaits    = 0; // device waits the TLAS rebuild path paid (boot=1)
        uint32_t tlasWaitsPerKBuild = 0; // 1000*syncWaits/builds — drives to 0 (was ~1000)
        float    tlasCpuMs        = 0; // CPU ms of the most recent buildTlas (no device wait)
    };
    virtual FramePacing framePacing() const { return {}; }

    // Tunables for the telemetry/guarantee (cvar-driven so CI can tighten):
    //   warmupFrames — frames excluded from percentiles/spike counting (default 60)
    //   spikeFactor  — spike when cpuMs > spikeFactor * rolling median (default 2)
    //   floorMs      — absolute slack: spike also requires cpuMs > median+floorMs
    //                  (filters OS-scheduler noise on sub-ms headless frames)
    //   strictPso    — log a [stutter] error line on ANY pipeline/module/pool
    //                  creation after the first frame begins (default ON in Debug)
    struct PacingParams {
        int   warmupFrames = 60;
        float spikeFactor  = 2.0f;
        float floorMs      = 3.0f;
        bool  strictPso    = false;
    };
    virtual void setPacingParams(const PacingParams&) {}

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

    // ========================================================================
    // ---- CLI `--set` OVERRIDE LATCH ---------------------------------------
    //
    // WHY THIS EXISTS (it cost real evidence). The per-frame cvar->device sync
    // hub lives in the DEFAULT host (app_run.cpp applyRtaoCVars). Every
    // `--world <name>` host runs INSTEAD of that function, never alongside it,
    // so `--set` NEVER REACHED THE DEVICE on a --world run: the frame rendered
    // the default state, silently, and looked entirely plausible. A lane
    // concluded an artifact "survives r_bloom 0 / r_taa 0 / r_taasharpen 0" —
    // none of those three tests had run.
    //
    // Applying the --set once at host entry is NOT enough on its own: hosts
    // overwrite these params themselves AFTER entry (host_showroom's
    // setSsaoParams(enabled=false), host_club's setExposure(1.0),
    // host_echotropolis's per-frame setPostFX/setExposure/setRtaoParams). That
    // is the same silent lie one layer down. So the app installs the parsed
    // --set list ONCE and every subsequent setter call on this device re-stamps
    // the overridden FIELDS on its way through: whoever writes last, the
    // COMMAND LINE WINS, for the whole run.
    //
    // STRICTLY OPT-IN, AND FIELD-LEVEL. A field is stamped only if that cvar
    // was actually passed. `active == false` — the default, and the state of
    // any run with no `--set` — makes every apply() an immediate no-op, so a
    // run without `--set` executes exactly the code it always did.
    //
    // Installed by the --world host dispatch only (app/world_hosts). The
    // default host is deliberately NOT latched: it seeds its console from
    // --set and syncs per frame, so latching it would freeze in-game console
    // edits.
    // ========================================================================
    template <class T> struct CVarOpt {
        bool has = false;
        T    v{};
        void set(T x) { has = true; v = x; }
        void stamp(T& dst) const { if (has) dst = v; }
    };
    struct RenderCVarOverrides {
        bool active = false;   // false = every apply() below returns immediately

        CVarOpt<int>   debugView;      // r_debugview
        CVarOpt<float> exposure;       // r_exposure

        // SSAO (r_ssao*)
        CVarOpt<bool>  ssaoEnabled;
        CVarOpt<float> ssaoRadius, ssaoBias, ssaoIntensity, ssaoPower, ssaoStrength;

        // SSGI (r_ssgi*)
        CVarOpt<bool>  giEnabled;
        CVarOpt<float> giIntensity, giStrength;

        // RT AO (r_rtao*)
        CVarOpt<bool>  rtaoEnabled;
        CVarOpt<float> rtaoRadius, rtaoStrength;
        CVarOpt<int>   rtaoRays;

        // Reflections (r_ssr / r_rtreflections / r_reflquality / r_refl*)
        CVarOpt<bool>  reflSsr, reflRt, reflFullRes;
        CVarOpt<float> reflIntensity;
        CVarOpt<int>   reflDenoiseIters;
        CVarOpt<float> reflDnDepth, reflDnNormal, reflDnDisc;

        // HDR post stack (r_tonemap / r_bloom* / r_autoexposure / r_ae* /
        // r_taa / r_taasharpen / r_velocity / r_filmic)
        CVarOpt<int>   tonemapMode;
        CVarOpt<bool>  bloom;
        CVarOpt<float> bloomIntensity, bloomThreshold;
        CVarOpt<bool>  autoExposure;
        CVarOpt<float> aeSpeed, aeMin, aeMax, aeKey;
        CVarOpt<bool>  taa;
        CVarOpt<float> taaSharpen;
        CVarOpt<bool>  velocity, filmicAllowed;

        // RT soft shadows (r_rtshadows / r_rtsun_size / r_rtpoint_*)
        CVarOpt<int>   rtsTier;
        CVarOpt<float> rtsSunSize;
        CVarOpt<int>   rtsPointMax;
        CVarOpt<float> rtsPointRadius;

        // DDGI probe-grid GI (r_ddgi / r_ddgi_debug / r_ddgi_rays /
        // r_ddgi_intensity / r_ddgi_n[xyz] / r_ddgi_hyst)
        CVarOpt<bool>  ddgiEnabled;
        CVarOpt<int>   ddgiDebug, ddgiRays, ddgiNx, ddgiNy, ddgiNz;
        CVarOpt<float> ddgiIntensity, ddgiHyst;

        // Scalar levers with their own setters (r_metalambient / r_clusterlights)
        CVarOpt<float> metalAmbient;
        CVarOpt<bool>  clusterLights;

        void apply(SsaoParams& p) const {
            if (!active) return;
            ssaoEnabled.stamp(p.enabled);   ssaoRadius.stamp(p.radius);
            ssaoBias.stamp(p.bias);         ssaoIntensity.stamp(p.intensity);
            ssaoPower.stamp(p.power);       ssaoStrength.stamp(p.strength);
        }
        void apply(GiParams& p) const {
            if (!active) return;
            giEnabled.stamp(p.enabled); giIntensity.stamp(p.intensity);
            giStrength.stamp(p.strength);
        }
        void apply(RtaoParams& p) const {
            if (!active) return;
            rtaoEnabled.stamp(p.enabled); rtaoRadius.stamp(p.radius);
            rtaoRays.stamp(p.rays);       rtaoStrength.stamp(p.strength);
        }
        void apply(ReflectionParams& p) const {
            if (!active) return;
            reflSsr.stamp(p.ssr);           reflRt.stamp(p.rtFallback);
            reflFullRes.stamp(p.fullRes);   reflIntensity.stamp(p.intensity);
            reflDenoiseIters.stamp(p.denoiseIters);
            reflDnDepth.stamp(p.denoiseDepthSigma);
            reflDnNormal.stamp(p.denoiseNormalPow);
            reflDnDisc.stamp(p.denoiseDiscScale);
        }
        void apply(PostFXParams& p) const {
            if (!active) return;
            tonemapMode.stamp(p.tonemapMode);   bloom.stamp(p.bloomEnabled);
            bloomIntensity.stamp(p.bloomIntensity);
            bloomThreshold.stamp(p.bloomThreshold);
            autoExposure.stamp(p.autoExposure); aeSpeed.stamp(p.aeSpeed);
            aeMin.stamp(p.aeMin);               aeMax.stamp(p.aeMax);
            aeKey.stamp(p.aeKey);               taa.stamp(p.taa);
            taaSharpen.stamp(p.taaSharpen);     velocity.stamp(p.velocity);
            filmicAllowed.stamp(p.filmicAllowed);
        }
        void apply(RtShadowParams& p) const {
            if (!active) return;
            rtsTier.stamp(p.tier);          rtsSunSize.stamp(p.sunSizeDeg);
            rtsPointMax.stamp(p.pointMax);  rtsPointRadius.stamp(p.pointRadius);
        }
        void apply(DdgiParams& p) const {
            if (!active) return;
            ddgiEnabled.stamp(p.enabled);   ddgiDebug.stamp(p.debug);
            ddgiRays.stamp(p.raysPerProbe); ddgiIntensity.stamp(p.intensity);
            ddgiNx.stamp(p.countX);         ddgiNy.stamp(p.countY);
            ddgiNz.stamp(p.countZ);         ddgiHyst.stamp(p.hysteresis);
        }
        float applyExposure(float e)  const { if (active) exposure.stamp(e);  return e; }
        int   applyDebugView(int m)   const { if (active) debugView.stamp(m); return m; }
        float applyMetalAmbient(float s) const { if (active) metalAmbient.stamp(s); return s; }
        bool  applyClusterLights(bool b) const { if (active) clusterLights.stamp(b); return b; }
    };
    // Install the latch. Default no-op (headless stub / any other backend); the
    // Vulkan device stores it and stamps every setter above. Calling with a
    // default-constructed (active == false) value disarms it.
    virtual void setCVarOverrides(const RenderCVarOverrides&) {}

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
