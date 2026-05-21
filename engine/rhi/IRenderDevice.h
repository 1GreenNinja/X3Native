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
    // Create a sampled texture from tightly-packed RGBA8 (w*h*4 bytes). `srgb`
    // selects the storage format (sRGB for color, UNORM for data/linear).
    virtual TextureHandle createTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb) = 0;
    virtual void          destroyTexture(TextureHandle) = 0;
    // Submit a draw between beginFrame/endFrame. An invalid baseColor falls back
    // to the built-in 1x1 white texture, so baseColorFactor alone gives a flat
    // color. `model` is a column-major 4x4. baseColorFactor multiplies the texel.
    virtual void          drawMesh(const FrameContext&, MeshHandle, TextureHandle baseColor,
                                   const float baseColorFactor[4], const float model[16]) = 0;

    // Capability query
    virtual bool supportsDescriptorIndexing() const = 0;
    virtual bool supportsMeshShaders() const = 0;
};

// Factory. Returns the GPL v0 impl when X3_USE_GPL_SCAFFOLD is defined and
// available; otherwise returns the clean Vulkan impl. (For now: clean impl.)
IRenderDevice* createRenderDevice();

} // namespace x3::rhi
