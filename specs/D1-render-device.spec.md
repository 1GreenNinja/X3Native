# Spec: Render Device (RHI)  (D1)

> Written by the SPEC TEAM (14900K). Implemented by the CLEAN-ROOM TEAM (13700K) from THIS FILE + public refs ONLY.
> ❌ No GPL source, no transcribed function bodies, no RBDOOM identifiers/paths below this line.
> **This is a WORKED EXAMPLE / STUB** showing the format. Fill in real targets during D1.

- **Ledger ID:** D1
- **Implements interface:** `IRenderDevice` (`engine/rhi/IRenderDevice.h`)
- **Status:** SPEC (example stub — not yet authoritative)
- **Spec author machine:** 14900K
- **Clean-room target machine:** 13700K

## 1. Purpose
The render device owns the GPU connection: it creates the Vulkan instance, picks a physical device, creates the logical device + queues, manages the swapchain (incl. resize), and hands out command buffers for per-frame recording. Everything above it (materials, scene, shadows) talks only through this interface and never touches raw Vulkan handles directly.

## 2. Interface contract
```cpp
// engine/rhi/IRenderDevice.h — clean, authored fresh
namespace x3::rhi {

struct DeviceDesc {
    void*  nativeWindowHandle = nullptr;  // HWND on Windows
    uint32_t width  = 0;
    uint32_t height = 0;
    bool   vsync     = true;
    bool   validation = false;            // Vulkan validation layers
};

struct FrameContext {
    uint32_t frameIndex = 0;              // ring index [0, kFramesInFlight)
    uint64_t cmd        = 0;              // opaque command-buffer handle
    uint32_t backbuffer = 0;             // swapchain image index
};

class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;

    virtual bool init(const DeviceDesc&) = 0;
    virtual void shutdown() = 0;
    virtual void onResize(uint32_t w, uint32_t h) = 0;

    // Per-frame
    virtual FrameContext beginFrame() = 0;       // acquires swapchain image, begins cmd
    virtual void         endFrame(const FrameContext&) = 0;  // submit + present

    // Capability query
    virtual bool supportsDescriptorIndexing() const = 0;
    virtual bool supportsMeshShaders() const = 0;
};

IRenderDevice* createRenderDevice();  // factory — returns the (clean, original) Vulkan impl
} // namespace x3::rhi
```

## 3. Behavior
- Inputs: a native window handle + initial size + vsync/validation flags.
- Outputs: a working device; per-frame command-buffer handles via `beginFrame`.
- Lifecycle: `init` once → N×(`beginFrame`/record/`endFrame`) → `shutdown`. `onResize` may interleave; must recreate swapchain safely (wait-idle, destroy, recreate).
- Threading: `init`/`shutdown`/`onResize` main-thread only. `beginFrame`/`endFrame` main-thread; recording into the returned cmd handle may be multi-threaded in a later phase (out of scope for v1).
- Invariants: exactly `kFramesInFlight` (target 2-3) frames in flight; no GPU handle leaks across resize.

## 4. Edge cases & error handling
- Swapchain out-of-date / suboptimal (window resize, minimize): recreate; skip the frame cleanly, no crash.
- Device lost: log + attempt one recreate; if it fails, surface a fatal error to the host.
- Zero-size window (minimized): no-op frames until restored.
- Validation enabled but layers absent: warn, continue without validation.

## 5. Performance targets
- `beginFrame`+`endFrame` overhead ≤ 0.2 ms/frame on RTX 3060 at 1080p (excludes actual draw work).
- No per-frame heap allocation in the begin/end path (pre-allocate per-frame ring resources at init).
- Triple-buffer by default; vsync toggle without device recreate where possible.

## 6. Acceptance tests
1. **T1 — Init/shutdown:** `init` then `shutdown` with a 1280×720 window leaks no Vulkan objects (validate with VK_LAYER_KHRONOS_validation + VMA leak report).
2. **T2 — Clear+present:** 600 frames of beginFrame → clear backbuffer to a known color → endFrame, presents without validation errors; pixel readback matches clear color.
3. **T3 — Resize storm:** programmatically resize the swapchain 100× across frames; no crash, no validation error, final image correct.
4. **T4 — Caps query:** `supportsDescriptorIndexing()` returns true on an RTX 3060 / 5090.
5. **T5 — Vsync toggle:** flip vsync at runtime; frame pacing changes, no device recreate crash.

## 7. Public references
- Vulkan 1.3 Specification — Devices and Queues, WSI Swapchain (`VK_KHR_swapchain`), Synchronization2.
- vk-bootstrap docs (instance/device/swapchain bring-up).
- Vulkan Memory Allocator (VMA) docs.
- "Vulkan Programming Guide" (Sellers) ch. 2-3; vkguide.dev.

## 8. Suggested permissive libraries
- **vk-bootstrap** (MIT) — instance/physical-device/logical-device/swapchain boilerplate.
- **VMA — Vulkan Memory Allocator** (MIT) — all device memory.
- **volk** (MIT) — Vulkan function loader (optional).

## 9. Notes for the clean-room implementer
- Target Vulkan 1.3 dynamic rendering (`VK_KHR_dynamic_rendering`) to avoid VkRenderPass/VkFramebuffer boilerplate.
- Use timeline semaphores (Synchronization2) for frame pacing.
- Keep the public interface tiny; hide ALL Vulkan types in the .cpp. The header must not include `vulkan.h` (so game/Lua never see Vulkan).
