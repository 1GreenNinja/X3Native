#pragma once
// GPU object culling — D15. Internal renderer detail (includes vulkan.h, same
// quarantine rule as RenderGraph.h: consumed by VulkanRenderDevice.cpp only).
// Spec: specs/D15-gpucull.spec.md (write me)
//
// TIERS (selected once at device init; cvar r_cullpath overrides: -1 auto,
// 0/1/2 force; r_cullpath 0 also serves as the safety fallback):
//   Tier 0 — compute frustum(+HZB) cull on the GRAPHICS queue. Vulkan 1.2+.
//            Verified-by-design for Pascal (GTX 1080 Ti): no drawIndirectCount,
//            no async queue, no mesh shaders. TESTED: shaders validate to
//            SPIR-V; pass logic reviewed; needs on-GPU verification (checklist
//            in D15 handoff).
//   Tier 1 — same cull/HZB dispatches on a DEDICATED COMPUTE QUEUE overlapping
//            the previous frame's raster. Turing+/RDNA+. ⚠ UNTESTED — the
//            cross-queue semaphore + ownership plumbing in recordAsync* needs
//            validation-layer review on real hardware before enabling.
//   Tier 2 — VK_EXT_mesh_shader per-MESHLET culling (task: frustum + backface
//            cone + HZB per ~64-tri cluster). ⚠ UNTESTED on GPU — the CPU
//            meshlet BUILDER below IS tested (self-test); the task/mesh
//            pipeline + vertex-interface match against mesh.frag needs review.
//
// All tiers feed the SAME per-mesh indirect command + visible-instance buffers,
// so everything downstream of the cull is tier-agnostic.
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <vector>

namespace x3::rhi {

class RenderGraph;

enum class CullTier : int { Tier0_GraphicsCompute = 0,
                            Tier1_AsyncCompute    = 1,
                            Tier2_MeshShaders     = 2 };

struct CullDeviceCaps {
    CullTier tier = CullTier::Tier0_GraphicsCompute;
    bool     hasDedicatedComputeQueue = false;
    uint32_t computeQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    bool     hasMeshShader = false;
    bool     isPreTuring   = false;   // Pascal/older: pin Tier 0 (weak async)
};

// Inspect the physical device once at init. `forceTier` < 0 = auto.
CullDeviceCaps detectCullCaps(VkPhysicalDevice phys, int forceTier = -1);

// GPU-visible structs (must match cull.comp std430 layouts exactly).
struct CullInstanceGpu {
    float sphere[4];        // world center xyz + radius
    uint32_t meshSlot;
    uint32_t instanceData;
    uint32_t flags;         // bit0 ALWAYS_VISIBLE
    uint32_t _pad;
};
struct CullParamsGpu {
    float frustum[6][4];
    float viewProj[16];
    float hzbSize[2];
    uint32_t instanceCount;
    uint32_t _pad0;
};
struct CullStatsGpu { uint32_t tested, drawn, frustumCulled, hzbCulled; };

// Everything the cull pass touches each frame. The DEVICE owns/creates these
// (VMA lives there); this system only records commands against them.
struct CullFrameInputs {
    VkBuffer instances;        // CullInstanceGpu[instanceCount], CPU-written
    VkBuffer drawCmds;         // VkDrawIndexedIndirectCommand[meshSlots], CPU-
                               // written with instanceCount=0 each frame
    VkBuffer visibleInstances; // uint32[totalInstances], GPU-written
    VkBuffer stats;            // CullStatsGpu, GPU-written, host-readback ring
    VkBuffer params;           // CullParamsGpu UBO, CPU-written
    uint32_t instanceCount = 0;
    VkDescriptorSet cullSet = VK_NULL_HANDLE;   // bindings 0..5 per cull.comp
};

class GpuCullSystem {
public:
    // SPIR-V blobs are loaded by the device's existing shader path. When
    // `buildHzb` is true BOTH cull pipeline variants (frustum-only + frustum+HZB
    // spec-const) and the pyramid-reduce pipeline are created, so the HZB phase
    // can be toggled per frame (r_hzb) without pipeline recreation. reversedZ
    // selects the reduce direction (X3 uses STANDARD Z: GLM_FORCE_DEPTH_ZERO_TO_ONE
    // + glm::perspective -> depth grows with distance -> MAX-reduce).
    bool init(VkDevice dev, const CullDeviceCaps& caps,
              const std::vector<uint32_t>& cullSpv,
              const std::vector<uint32_t>& hzbSpv,
              bool buildHzb, bool reversedZ);
    void shutdown(VkDevice dev);

    // Descriptor-set layouts the DEVICE allocates its per-frame sets from
    // (bindings exactly as cull.comp / hzb_build.comp declare).
    VkDescriptorSetLayout cullSetLayout() const { return m_cullDsl; }
    VkDescriptorSetLayout hzbSetLayout()  const { return m_hzbDsl; }

    // TIER 0 path (also the Tier 1 record body): appends one compute pass to
    // the graph. Buffer hazards (compute write -> DRAW_INDIRECT/vertex read,
    // and the previous frame's read -> this frame's CPU write) are emitted as
    // manual sync2 BUFFER barriers inside the record callback — the graph
    // tracks images only, per its documented scope. `useHzb` selects the
    // frustum-only or frustum+HZB pipeline variant for THIS frame (r_hzb).
    void addCullPass(RenderGraph& graph, const CullFrameInputs& frame, bool useHzb);

    // HZB pyramid passes (one dispatch per mip). The caller owns the pyramid
    // image + per-mip descriptor sets (binding0 = prev mip / depth sampler,
    // binding1 = this mip as storage image); mip 0's set samples the depth
    // buffer itself.
    struct HzbChain {
        VkImage  pyramid = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0, mipCount = 0;   // width/height = MIP 0 dims
        const VkDescriptorSet* mipSets = nullptr;   // [mipCount]
    };
    void addHzbPasses(RenderGraph& graph, const HzbChain& chain);
    // The reduce-loop body (entry barrier + one dispatch per mip + per-mip
    // write->read barriers). Public so the device can record it inside its own
    // graph pass (which declares the DEPTH image read so the graph derives the
    // depth layout transition — the pyramid's barriers stay manual/in here).
    void recordHzbBuild(VkCommandBuffer cmd, const HzbChain& chain);

    // ⚠ TIER 1 — UNTESTED, review required. Records the same dispatches into a
    // command buffer for the dedicated compute queue + returns the semaphore the
    // graphics submit must wait on (VERTEX_INPUT|DRAW_INDIRECT stages).
    VkSemaphore recordAsyncCull(VkCommandBuffer computeCmd,
                                const CullFrameInputs& frame);

    const CullDeviceCaps& caps() const { return m_caps; }

    // Record-callback plumbing (public: RenderGraph uses raw fn ptr + ctx, and
    // the trampolines are file-local free functions in the .cpp).
    struct PassCtx { GpuCullSystem* self; CullFrameInputs frame; bool useHzb; };
    struct HzbCtx  { GpuCullSystem* self; HzbChain chain; };
    void recordCullBody(VkCommandBuffer cmd, const CullFrameInputs& f, bool useHzb);

    VkPipeline       m_hzbPipe   = VK_NULL_HANDLE;   // HzbCtx record reads these
    VkPipelineLayout m_hzbLayout = VK_NULL_HANDLE;

private:
    CullDeviceCaps   m_caps{};
    VkPipelineLayout m_cullLayout = VK_NULL_HANDLE;
    VkPipeline       m_cullPipe      = VK_NULL_HANDLE;  // frustum-only (USE_HZB=0)
    VkPipeline       m_cullPipeHzb   = VK_NULL_HANDLE;  // frustum+HZB  (USE_HZB=1)
    VkDescriptorSetLayout m_cullDsl  = VK_NULL_HANDLE;  // owned; device allocs sets
    VkDescriptorSetLayout m_hzbDsl   = VK_NULL_HANDLE;
    VkSemaphore      m_asyncDone  = VK_NULL_HANDLE;   // Tier 1 (timeline)
    // stable record-callback ctx storage (RenderGraph uses raw fn ptr + ctx)
    PassCtx m_ctx{};
    HzbCtx  m_hzbCtx{};
};

// ---------------------------------------------------------------------------
// TIER 2 CPU side — meshlet building (TESTED via runMeshletSelfTest()).
// Greedy vertex-locality clustering: <=64 verts / <=124 tris per meshlet, with
// a bounding sphere and a backface cone (axis + cutoff; w=-1 when the cone is
// too wide to ever cull, e.g. double-sided foliage cards).
// ---------------------------------------------------------------------------
struct Meshlet {
    float sphere[4];
    float cone[4];          // xyz axis, w = cutoff (-1 disables)
    uint32_t vertexOffset, triangleOffset;
    uint32_t vertexCount,  triangleCount;
};
struct MeshletMesh {
    std::vector<Meshlet>  meshlets;
    std::vector<uint32_t> vertexIndices;   // meshlet-local -> mesh vertex index
    std::vector<uint8_t>  triangles;       // 3 bytes/tri, meshlet-local indices
};
MeshletMesh buildMeshlets(const float* positions /*xyz stride 3*/,
                          uint32_t vertexCount,
                          const uint32_t* indices, uint32_t indexCount);

// D15 CPU acceptance tests (meshlet invariants on generated meshes).
bool runMeshletSelfTest();

} // namespace x3::rhi
