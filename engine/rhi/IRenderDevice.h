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
};

struct FrameContext {
    uint32_t frameIndex = 0;  // ring index [0, kFramesInFlight)
    uint64_t cmd        = 0;  // opaque command-buffer handle
    uint32_t backbuffer = 0;  // swapchain image index
    bool     valid      = false;
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

    // Camera (FPS-style). Angles in radians. The device builds view+proj.
    // forward = (cos(pitch)*cos(yaw), sin(pitch), cos(pitch)*sin(yaw)).
    virtual void setCamera(float x, float y, float z, float yaw, float pitch, float fovDeg) = 0;

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
    };
    // Set the active sky parameters for subsequent frames (cached + re-applied
    // each frame, like setPointLights). Calling with enabled=false disables it.
    virtual void          setSkyParams(const SkyParams&) = 0;

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
    // drawHudText: render `text` starting at (xPx,yPx) with each glyph occupying
    // pxPerGlyph x pxPerGlyph pixels, sampling the embedded 8x8 bitmap font atlas.
    // Newlines advance a line; non-printable chars are skipped. `rgba` tints it.
    virtual void drawHudText(const FrameContext&, const char* text, float xPx,
                             float yPx, float pxPerGlyph, const float rgba[4]) = 0;
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
};

// Factory. Returns the (clean, original) Vulkan implementation of IRenderDevice.
IRenderDevice* createRenderDevice();

} // namespace x3::rhi
