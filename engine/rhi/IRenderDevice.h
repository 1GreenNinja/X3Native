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
    // Read back the LAST presented color (swapchain) image to the CPU and write
    // it as a PNG at `path`. Call AFTER endFrame() has presented at least one
    // frame (and no beginFrame/endFrame is in progress). Returns true on success.
    // The implementation handles the swapchain channel order (BGRA->RGBA) and the
    // row pitch so colors + dimensions are correct. No Vulkan types cross here.
    virtual bool captureFrame(const char* path) = 0;

    // Capability query
    virtual bool supportsDescriptorIndexing() const = 0;
    virtual bool supportsMeshShaders() const = 0;
};

// Factory. Returns the (clean, original) Vulkan implementation of IRenderDevice.
IRenderDevice* createRenderDevice();

} // namespace x3::rhi
