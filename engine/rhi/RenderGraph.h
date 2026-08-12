#pragma once
// RenderGraph — a small, allocation-free-in-the-hot-path frame graph for the
// X3Native Vulkan renderer (perf-stack B). CLEAN-ROOM, original work: built from
// the Vulkan 1.3 spec (sync2 / dynamic rendering), Real-Time Rendering 4th ed.,
// and the public frame-graph design pattern described in GDC/SIGGRAPH talks
// (e.g. Frostbite's "FrameGraph", O'Donnell 2017). No game-engine source was
// consulted. // No GPL / id Tech / RBDOOM source consulted.
//
// PURPOSE
// -------
// The renderer used to hand-code a fixed pass sequence each frame (shadow depth
// pass -> main color pass -> optional capture copy) with manually-written sync2
// barriers + image-layout transitions. This graph formalizes those passes as
// NODES that declare, per-resource, the layout + pipeline-stage + access each one
// needs. The graph then DERIVES the correct VkImageMemoryBarrier2 (layout
// transition + execution/memory dependency) automatically from the difference
// between a resource's tracked current state and the state the executing pass
// requires, and drives vkCmdBeginRendering / vkCmdEndRendering for raster passes.
//
// This is the infrastructure later passes (depth pre-pass, bloom, GI, streaming
// terrain, async compute) plug into declaratively — adding a pass is a few lines
// (register resources you touch + a record lambda); the barriers fall out for
// free. See the worked examples at the bottom of this header.
//
// SCOPE / DELIBERATE LIMITS (honest)
// ----------------------------------
//  * This header includes <vulkan/vulkan.h> and is therefore an INTERNAL renderer
//    detail. It is consumed only by VulkanRenderDevice.cpp and never reaches any
//    public engine header (IRenderDevice.h stays graphics-API-free).
//  * Passes are added in dependency order by the caller (the renderer knows its
//    own order). The graph still does a real topological-feasibility check + can
//    auto-order, but the steady path adds passes in order and the barriers are
//    derived from declared reads/writes, not from a re-sort each frame — that
//    keeps the hot path allocation-free. A full data-flow auto-scheduler is the
//    documented next tier.
//  * One queue (graphics) today. A pass carries a `queue` tag so a compute/async
//    pass can be added later without reworking the model (not exercised yet).

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cassert>
#include <vector>

namespace x3::rhi {

// Max resource uses a single pass may declare (and the per-pass barrier batch
// size in execute()). Shared by RenderPassDesc::uses[] and execute()'s scratch
// array so they can never disagree. The busiest pass today (the GI terrain pass)
// declares 5; 8 leaves comfortable headroom. Bump here if a future pass needs
// more — both the inline array and the barrier batch grow together.
inline constexpr uint32_t kMaxUsesPerPass = 8;

// ---------------------------------------------------------------------------
// Resource model. A graph resource is an IMAGE (the only thing needing layout
// transitions); buffers in this renderer are persistent-mapped rings that do not
// need cross-pass image-style barriers, so they are not modeled as graph nodes
// (the per-frame ring write happens before recording and the indirect/SSBO reads
// are covered by the same submission's implicit ordering — documented). Each
// resource tracks a CURRENT STATE (layout + the stage/access that last touched
// it) so the next pass that touches it gets exactly the right barrier derived.
// ---------------------------------------------------------------------------
struct ResourceState {
    VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2        access = 0;
};

// A handle into the graph's resource table (index; 0xFFFFFFFF == invalid).
struct RgResource { uint32_t id = 0xFFFFFFFFu; bool valid() const { return id != 0xFFFFFFFFu; } };

// How a pass uses a resource: the layout it must be in, plus the stage+access
// that will touch it in that pass. The graph compares this against the resource's
// tracked current state and emits the transition barrier if anything differs.
struct ResourceUse {
    RgResource            res;
    VkImageLayout         layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2        access;
    VkImageAspectFlags    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    bool                  isWrite = false;   // affects future read/write hazard derivation
};

// Which queue a pass targets. Only Graphics is used today; the tag exists so an
// async-compute pass can be declared later without changing the model.
enum class RgQueue : uint8_t { Graphics, Compute };

// A pass node. Declares the resources it reads/writes (with the state each needs)
// and a record callback that issues the actual draw/dispatch/copy commands. The
// graph emits all required barriers BEFORE invoking `record`, and (for raster
// passes that set `render`) drives vkCmdBeginRendering/EndRendering around it.
struct RenderPassDesc {
    // The command recorder. A RAW function pointer + an opaque ctx pointer instead
    // of a std::function: a std::function whose captures exceed its small-buffer
    // size heap-allocates ON CONSTRUCTION, which for ~20 passes/frame is ~20 hidden
    // allocs/frame. A plain function pointer never allocates. `ctx` points at STABLE
    // per-pass storage the renderer already keeps (e.g. a per-pass member struct, or
    // simply `this`); the trampoline casts it back. Receives the live command buffer
    // (already inside vkCmdBeginRendering when usesDynamicRendering). Must not record
    // barriers for graph-tracked resources — those are derived. `record == nullptr`
    // is allowed (a pure layout-transition pass, e.g. present finalize).
    using RecordFn = void(*)(void* ctx, VkCommandBuffer cmd);

    const char*               name = "pass";
    RgQueue                   queue = RgQueue::Graphics;

    // Fixed inline array of resource uses (NO per-frame heap allocation; the old
    // std::vector push_back'd fresh every frame for every pass). Fill via addUse().
    ResourceUse               uses[kMaxUsesPerPass]{};
    uint32_t                  useCount = 0;

    // Optional dynamic-rendering description. If `usesDynamicRendering` is true the
    // graph calls vkCmdBeginRendering(renderInfo) after barriers and
    // vkCmdEndRendering() after `record`. Leave false for non-raster passes
    // (e.g. a transfer/capture copy or a compute dispatch) that issue their own
    // commands directly. The VkRenderingInfo + attachment infos must outlive
    // execute() (the renderer keeps them in stable members across addPass+execute).
    bool                      usesDynamicRendering = false;
    VkRenderingInfo           renderInfo{};

    RecordFn                  record = nullptr;
    void*                     recordCtx = nullptr;

    // Append a resource use into the inline array. In Debug an over-cap append
    // trips the assert (catches a pass that outgrew the cap at the source); in
    // Release it is dropped (and execute()'s guard also logs) — kMaxUsesPerPass is
    // sized with headroom so this never fires in practice.
    void addUse(const ResourceUse& u) {
        assert(useCount < kMaxUsesPerPass &&
               "RenderPassDesc: too many uses — raise kMaxUsesPerPass in RenderGraph.h");
        if (useCount < kMaxUsesPerPass) uses[useCount++] = u;
    }
};

// ---------------------------------------------------------------------------
// The graph. Lifetime: ONE instance lives on the renderer; resources that persist
// (shadow map) are imported once, swapchain color/depth are (re)imported when the
// view/state changes. Per frame the renderer resets the transient pass list,
// imports this frame's swapchain image, adds the passes for the frame, and calls
// execute(cmd). The resource table + pass vector are reused across frames (their
// capacity persists) so the steady per-frame path performs NO heap allocation.
// ---------------------------------------------------------------------------
class RenderGraph {
public:
    // ---- Resource registration -------------------------------------------
    // Import an external image with a KNOWN current state (e.g. the shadow map
    // persists DEPTH_READ_ONLY between frames; the swapchain image is UNDEFINED at
    // acquire). Returns a stable handle for this build. Cleared each beginFrame().
    //
    // `arrayLayers` must be the image's REAL layer count: the derived barriers
    // transition [0, arrayLayers), and a layer left out of a transition is a layout
    // mismatch the validation layer will flag (and real hardware may honour). It
    // defaults to 1 because every image here except the CSM shadow array is a
    // single layer.
    RgResource importImage(const char* name, VkImage image,
                           const ResourceState& current,
                           uint32_t arrayLayers = 1);

    // ---- Per-frame build -------------------------------------------------
    // Reset the transient pass list + resource table for a new frame. Keeps the
    // backing vectors' capacity (no free/realloc) -> allocation-free steady state.
    void beginFrame();

    // Add a pass (declared reads/writes + record callback). Passes are recorded in
    // the order added (the renderer adds them in dependency order); the graph
    // derives + emits the barriers from each pass's declared resource states.
    void addPass(RenderPassDesc&& pass);

    // Execute the built graph into `cmd`: for each pass, emit the derived
    // sync2 barriers for its resource uses, (optionally) begin dynamic rendering,
    // invoke its record callback, end rendering, and advance each touched
    // resource's tracked state. After execute(), a resource's state reflects the
    // last pass that touched it (so the renderer can read e.g. the swapchain's
    // post-graph state to finalize the present transition outside the graph if it
    // chooses — though the present transition is itself expressible as a pass).
    void execute(VkCommandBuffer cmd);

    // Read back a resource's tracked state after execute() (e.g. to know the
    // swapchain's final layout). Returns UNDEFINED state for invalid handles.
    ResourceState stateOf(RgResource r) const;

    // Override a resource's tracked state without emitting a barrier (used to
    // record the post-execute layout that a manual present/cleanup step leaves the
    // resource in, keeping the persistent shadow-map state correct across frames).
    void setState(RgResource r, const ResourceState& s);

    // Diagnostics: number of barriers the LAST execute() emitted (for reporting /
    // sanity that the graph is doing the work the hand-code used to).
    uint32_t lastBarrierCount() const { return m_lastBarrierCount; }

    // ---- PER-PASS TIMING (LANE 6 / r_speeds) -----------------------------
    // Before this existed the engine had exactly TWO GPU timestamps — frame start
    // and frame end (vk_resources.cpp) — so `m_cullGpuMs` / `m_hzbGpuMs` were
    // declared, never assigned, and the HUD printed 0.00 forever. EVERY per-pass
    // millisecond quoted in this repo's docs was therefore a whole-frame delta,
    // not a pass cost. This is the fix.
    //
    // enableTiming() hands the graph a slice of the frame's timestamp query pool:
    // pass i writes ALL_COMMANDS timestamps into queries [firstQuery + 2i] and
    // [firstQuery + 2i + 1]. The caller resets the whole pool before recording and
    // reads the results back kFramesInFlight frames later (no stall), then maps
    // them onto the pass names captured by timedPassName().
    //
    // MEASUREMENT HONESTY: both stamps use ALL_COMMANDS, so pass i's start latches
    // only after every previously-submitted command has completed. That makes the
    // per-pass durations non-overlapping and their sum ~= the frame total — which
    // is exactly what a cost breakdown needs — at the price of discouraging some
    // inter-pass overlap the GPU might otherwise find. That cost is itself
    // measurable: r_passtimers 0 turns the whole thing off for the A/B.
    void enableTiming(VkQueryPool pool, uint32_t firstQuery, uint32_t maxPasses) {
        m_tsPool = pool; m_tsFirst = firstQuery; m_tsMaxPasses = maxPasses;
    }
    void disableTiming() { m_tsPool = VK_NULL_HANDLE; m_tsMaxPasses = 0; }

    // Passes that got a timestamp pair in the LAST execute() (<= maxPasses).
    uint32_t    timedPassCount() const { return (uint32_t)m_timed.size(); }
    const char* timedPassName(uint32_t i) const { return m_timed[i].name; }
    // CPU milliseconds spent INSIDE that pass's record callback (+ its derived
    // barrier emit). This is the CPU-side half of the breakdown: on a CPU-bound
    // frame the expensive pass is the one whose record walk is expensive, which is
    // not necessarily the one whose GPU time is largest.
    float       timedPassCpuMs(uint32_t i) const { return m_timed[i].cpuMs; }

private:
    struct Resource {
        const char*   name;
        VkImage       image;
        ResourceState state;
        uint32_t      arrayLayers;   // barriers must span EVERY layer (CSM: 4)
    };
    struct TimedPass { const char* name; float cpuMs; };
    std::vector<Resource>       m_resources;   // capacity persists across frames
    std::vector<RenderPassDesc> m_passes;      // capacity persists across frames
    std::vector<TimedPass>      m_timed;       // capacity persists across frames
    uint32_t                    m_lastBarrierCount = 0;
    VkQueryPool                 m_tsPool = VK_NULL_HANDLE;
    uint32_t                    m_tsFirst = 0;
    uint32_t                    m_tsMaxPasses = 0;
};

// ===========================================================================
// HOW TO ADD A NEW PASS (declarative — a few lines). Examples:
//
//  // (a) A DEPTH PRE-PASS writing the main depth buffer before the color pass.
//  //     Declare depth as a write in DEPTH_ATTACHMENT_OPTIMAL; the graph emits
//  //     the (UNDEFINED|prev)->DEPTH_ATTACHMENT barrier automatically, and the
//  //     color pass that later reads/writes depth gets its barrier derived too.
//  RenderPassDesc pre{};
//  pre.name = "depth-prepass";
//  pre.uses = {{ depthRes, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
//                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
//              | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
//                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
//                VK_IMAGE_ASPECT_DEPTH_BIT, /*isWrite=*/true }};
//  pre.usesDynamicRendering = true; pre.renderInfo = depthOnlyInfo;
//  pre.record = [&](VkCommandBuffer c){ /* bind depth pipeline, draw occluders */ };
//  graph.addPass(std::move(pre));
//
//  // (b) A COMPUTE pass (e.g. frustum culling or bloom downsample) on the async
//  //     queue later — tag it RgQueue::Compute, declare the storage image it
//  //     writes as GENERAL layout with COMPUTE_SHADER stage; no renderInfo:
//  RenderPassDesc cull{};
//  cull.name = "cull"; cull.queue = RgQueue::Compute;
//  cull.uses = {{ visBuf, VK_IMAGE_LAYOUT_GENERAL,
//                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
//                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
//                 VK_IMAGE_ASPECT_COLOR_BIT, true }};
//  cull.record = [&](VkCommandBuffer c){ vkCmdDispatch(c, gx, gy, 1); };
//  graph.addPass(std::move(cull));
// ===========================================================================

} // namespace x3::rhi
