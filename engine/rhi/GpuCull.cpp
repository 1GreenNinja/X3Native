// GpuCull.cpp — D15 implementation. See GpuCull.h for the tier map + test
// status of each piece. CLEAN-ROOM: Vulkan spec, GLSL spec, public GDC/SIGGRAPH
// material on GPU-driven rendering; meshlet limits follow the publicly
// documented 64-vert/124-tri guidance. No engine source consulted.
#include "engine/rhi/GpuCull.h"
#include "engine/rhi/RenderGraph.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace x3::rhi {

// ===========================================================================
// Tier detection
// ===========================================================================
CullDeviceCaps detectCullCaps(VkPhysicalDevice phys, int forceTier) {
    CullDeviceCaps caps{};

    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qCount, qs.data());
    for (uint32_t i = 0; i < qCount; ++i) {
        const bool compute  = qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT;
        const bool graphics = qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
        if (compute && !graphics) {            // dedicated compute family
            caps.hasDedicatedComputeQueue = true;
            caps.computeQueueFamily = i;
            break;
        }
    }

    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &mesh };
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    caps.hasMeshShader = mesh.meshShader == VK_TRUE && mesh.taskShader == VK_TRUE;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    // Heuristic, deliberately driver-agnostic: on NVIDIA, mesh-shader support
    // begins at Turing, so NVIDIA-without-mesh-shaders == Pascal or older —
    // exactly the GPUs whose "async compute" is weak preemption-based overlap.
    // (The 1080 Ti lands here.) Other vendors: trust the dedicated-queue probe.
    const bool isNvidia = props.vendorID == 0x10DE;
    caps.isPreTuring = isNvidia && !caps.hasMeshShader;

    // AUTO policy: Tier 1 when a dedicated compute queue exists on a non-Pascal
    // part; Tier 2 is opt-in only (r_cullpath 2) until the asset pipeline bakes
    // meshlet data for every mesh — auto-selecting it would draw nothing for
    // un-baked content. Tier 0 is the universal floor + safety fallback.
    if (forceTier >= 0 && forceTier <= 2) {
        caps.tier = (CullTier)forceTier;
        if (caps.tier == CullTier::Tier2_MeshShaders && !caps.hasMeshShader) {
            logWarn("[cull] r_cullpath 2 forced but VK_EXT_mesh_shader absent -> Tier 0");
            caps.tier = CullTier::Tier0_GraphicsCompute;
        }
    } else {
        caps.tier = (caps.hasDedicatedComputeQueue && !caps.isPreTuring)
                  ? CullTier::Tier1_AsyncCompute
                  : CullTier::Tier0_GraphicsCompute;
    }
    logInfo(std::string("[cull] tier ") + std::to_string((int)caps.tier) +
            (caps.isPreTuring ? " (pre-Turing pinned)" : "") +
            (caps.hasMeshShader ? " [mesh-shader capable]" : ""));
    return caps;
}

// ===========================================================================
// Pipelines
// ===========================================================================
static VkPipeline makeComputePipe(VkDevice dev, const std::vector<uint32_t>& spv,
                                  VkPipelineLayout layout, uint32_t specUseHzb) {
    VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = spv.size() * sizeof(uint32_t);
    smci.pCode = spv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkSpecializationMapEntry entry{ 0, 0, sizeof(uint32_t) };
    VkSpecializationInfo spec{ 1, &entry, sizeof(uint32_t), &specUseHzb };

    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", &spec };
    ci.layout = layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
    vkDestroyShaderModule(dev, mod, nullptr);
    return pipe;
}

bool GpuCullSystem::init(VkDevice dev, const CullDeviceCaps& caps,
                         const std::vector<uint32_t>& cullSpv,
                         const std::vector<uint32_t>& hzbSpv,
                         bool useHzb, bool reversedZ) {
    m_caps = caps;
    m_useHzb = useHzb;

    // Descriptor set layout: bindings 0..5 exactly as cull.comp declares.
    VkDescriptorSetLayoutBinding b[6]{};
    for (uint32_t i = 0; i < 4; ++i)
        b[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[4] = { 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[5] = { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo dlci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlci.bindingCount = 6; dlci.pBindings = b;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &dsl) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &m_cullLayout) != VK_SUCCESS) return false;

    m_cullPipe = makeComputePipe(dev, cullSpv, m_cullLayout, useHzb ? 1u : 0u);
    if (!m_cullPipe) { logError("[cull] cull pipeline creation failed"); return false; }

    if (useHzb) {
        // HZB layout: sampler src + storage dst + push(dstSize). REDUCE_MAX
        // spec-const: standard-Z keeps the FARTHEST (max) depth; reversed-Z
        // flips to min. Must agree with cull.comp's compare (see hzbVisible).
        VkDescriptorSetLayoutBinding hb[2]{};
        hb[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        hb[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo hlci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        hlci.bindingCount = 2; hlci.pBindings = hb;
        VkDescriptorSetLayout hdsl = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(dev, &hlci, nullptr, &hdsl);
        VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
        VkPipelineLayoutCreateInfo hplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        hplci.setLayoutCount = 1; hplci.pSetLayouts = &hdsl;
        hplci.pushConstantRangeCount = 1; hplci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(dev, &hplci, nullptr, &m_hzbLayout);
        m_hzbPipe = makeComputePipe(dev, hzbSpv, m_hzbLayout, reversedZ ? 0u : 1u);
        if (!m_hzbPipe) { logError("[cull] hzb pipeline creation failed"); return false; }
    }
    return true;
}

void GpuCullSystem::shutdown(VkDevice dev) {
    if (m_cullPipe)   vkDestroyPipeline(dev, m_cullPipe, nullptr);
    if (m_cullLayout) vkDestroyPipelineLayout(dev, m_cullLayout, nullptr);
    if (m_hzbPipe)    vkDestroyPipeline(dev, m_hzbPipe, nullptr);
    if (m_hzbLayout)  vkDestroyPipelineLayout(dev, m_hzbLayout, nullptr);
    if (m_asyncDone)  vkDestroySemaphore(dev, m_asyncDone, nullptr);
}

// ===========================================================================
// TIER 0 — cull pass on the graphics queue, inside the RenderGraph.
// Buffer hazards are manual sync2 barriers HERE (the graph tracks images only,
// per its documented scope): compute writes -> DRAW_INDIRECT read (drawCmds)
// and -> VERTEX_SHADER read (visibleInstances).
// ===========================================================================
static void recordCull(void* ctxRaw, VkCommandBuffer cmd) {
    auto* ctx = (GpuCullSystem::PassCtx*)ctxRaw;
    ctx->self->recordCullBody(cmd, ctx->frame);
}

void GpuCullSystem::recordCullBody(VkCommandBuffer cmd, const CullFrameInputs& f) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullLayout,
                            0, 1, &f.cullSet, 0, nullptr);
    vkCmdDispatch(cmd, (f.instanceCount + 63u) / 64u, 1, 1);

    VkBufferMemoryBarrier2 bb[2]{};
    bb[0] = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
              f.drawCmds, 0, VK_WHOLE_SIZE };
    bb[1] = bb[0];
    bb[1].dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    bb[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    bb[1].buffer        = f.visibleInstances;
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.bufferMemoryBarrierCount = 2; dep.pBufferMemoryBarriers = bb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void GpuCullSystem::addCullPass(RenderGraph& graph, const CullFrameInputs& frame) {
    m_ctx = { this, frame };                  // stable storage for the raw-ptr ctx
    RenderPassDesc pass{};
    pass.name = "gpu-cull";
    pass.queue = RgQueue::Graphics;           // Tier 0: serialized, universal
    pass.record = &recordCull;
    pass.recordCtx = &m_ctx;
    graph.addPass(std::move(pass));
}

// ===========================================================================
// HZB pyramid build. One dispatch per mip; per-mip image barriers are manual
// (the graph tracks whole images, not subresources — documented limitation).
// Caller supplies the per-mip descriptor sets + the pyramid image it owns.
// ⚠ The mip-chain barrier sequence below is REVIEW-REQUIRED under validation
// layers (untestable here): each mip flips SHADER_WRITE->SHADER_READ before
// the next consumes it.
// ===========================================================================
void GpuCullSystem::addHzbPasses(RenderGraph& graph, const HzbChain& chain) {
    m_hzbCtx = { this, chain };
    RenderPassDesc pass{};
    pass.name = "hzb-build";
    pass.queue = RgQueue::Graphics;
    pass.record = +[](void* ctxRaw, VkCommandBuffer cmd) {
        auto* ctx = (GpuCullSystem::HzbCtx*)ctxRaw;
        GpuCullSystem* self = ctx->self;
        const HzbChain& c = ctx->chain;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self->m_hzbPipe);
        uint32_t w = c.width, h = c.height;
        for (uint32_t mip = 0; mip < c.mipCount; ++mip) {
            w = std::max(1u, w / 2u); h = std::max(1u, h / 2u);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    self->m_hzbLayout, 0, 1, &c.mipSets[mip], 0, nullptr);
            uint32_t push[2] = { w, h };
            vkCmdPushConstants(cmd, self->m_hzbLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, 8, push);
            vkCmdDispatch(cmd, (w + 7u) / 8u, (h + 7u) / 8u, 1);

            VkImageMemoryBarrier2 ib{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            ib.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            ib.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            ib.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            ib.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.image = c.pyramid;
            ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 };
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    };
    pass.recordCtx = &m_hzbCtx;
    graph.addPass(std::move(pass));
}

// ===========================================================================
// ⚠ TIER 1 — UNTESTED, REVIEW REQUIRED (cross-queue submission). Records the
// cull into a compute-queue command buffer; the graphics submit must WAIT on
// the returned timeline semaphore at VERTEX_INPUT|DRAW_INDIRECT. Queue-family
// OWNERSHIP TRANSFER is intentionally avoided by creating the cull buffers
// VK_SHARING_MODE_CONCURRENT across {graphics, compute} families — simpler,
// slightly slower; revisit after it works. Validation-layer pass on Turing+
// hardware is mandatory before enabling (r_cullpath 1).
// ===========================================================================
VkSemaphore GpuCullSystem::recordAsyncCull(VkCommandBuffer computeCmd,
                                           const CullFrameInputs& frame) {
    recordCullBody(computeCmd, frame);        // identical dispatch + barriers
    return m_asyncDone;                       // device init creates the timeline
}

// ===========================================================================
// TIER 2 CPU SIDE — meshlet builder (TESTED — runMeshletSelfTest below).
// Greedy clustering preserving index-buffer locality (glTF meshes from the
// pipeline are already vertex-cache ordered, so adjacency in the index stream
// approximates spatial adjacency well).
// ===========================================================================
MeshletMesh buildMeshlets(const float* pos, uint32_t vertexCount,
                          const uint32_t* idx, uint32_t indexCount) {
    constexpr uint32_t kMaxVerts = 64, kMaxTris = 124;
    MeshletMesh out;
    std::unordered_map<uint32_t, uint8_t> local;   // mesh vert -> meshlet-local
    Meshlet cur{};
    cur.vertexOffset = 0; cur.triangleOffset = 0;

    auto flush = [&]() {
        if (cur.triangleCount == 0) return;
        // Bounding sphere: centroid of the meshlet's verts + max distance.
        double cx = 0, cy = 0, cz = 0;
        for (uint32_t v = 0; v < cur.vertexCount; ++v) {
            const float* p = pos + out.vertexIndices[cur.vertexOffset + v] * 3;
            cx += p[0]; cy += p[1]; cz += p[2];
        }
        cx /= cur.vertexCount; cy /= cur.vertexCount; cz /= cur.vertexCount;
        double r2 = 0;
        for (uint32_t v = 0; v < cur.vertexCount; ++v) {
            const float* p = pos + out.vertexIndices[cur.vertexOffset + v] * 3;
            double dx = p[0]-cx, dy = p[1]-cy, dz = p[2]-cz;
            r2 = std::max(r2, dx*dx + dy*dy + dz*dz);
        }
        cur.sphere[0]=(float)cx; cur.sphere[1]=(float)cy; cur.sphere[2]=(float)cz;
        cur.sphere[3]=(float)std::sqrt(r2);

        // Backface cone: average face normal; cutoff from the worst deviation.
        // Conservative: if any face strays past ~84° from the axis, disable
        // (w = -1) — wide clusters can never be safely back-face culled.
        double ax=0, ay=0, az=0;
        std::vector<std::array<double,3>> normals(cur.triangleCount);
        for (uint32_t t = 0; t < cur.triangleCount; ++t) {
            const uint8_t* tri = &out.triangles[(cur.triangleOffset + t) * 3];
            const float* a = pos + out.vertexIndices[cur.vertexOffset + tri[0]] * 3;
            const float* b = pos + out.vertexIndices[cur.vertexOffset + tri[1]] * 3;
            const float* c = pos + out.vertexIndices[cur.vertexOffset + tri[2]] * 3;
            double e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
            double e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
            double n[3] = { e1[1]*e2[2]-e1[2]*e2[1],
                            e1[2]*e2[0]-e1[0]*e2[2],
                            e1[0]*e2[1]-e1[1]*e2[0] };
            double len = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            if (len > 1e-12) { n[0]/=len; n[1]/=len; n[2]/=len; }
            normals[t] = { n[0], n[1], n[2] };
            ax += n[0]; ay += n[1]; az += n[2];
        }
        double alen = std::sqrt(ax*ax+ay*ay+az*az);
        if (alen < 1e-9) { cur.cone[0]=0; cur.cone[1]=0; cur.cone[2]=1; cur.cone[3]=-1.f; }
        else {
            ax/=alen; ay/=alen; az/=alen;
            double minDot = 1.0;
            for (auto& n : normals)
                minDot = std::min(minDot, n[0]*ax + n[1]*ay + n[2]*az);
            cur.cone[0]=(float)ax; cur.cone[1]=(float)ay; cur.cone[2]=(float)az;
            // meshopt-style cutoff: cull when dot(view, axis) >= w + r/d.
            cur.cone[3] = (minDot > 0.1) ? (float)std::sqrt(1.0 - minDot*minDot)
                                         : -1.f;
        }
        out.meshlets.push_back(cur);
        local.clear();
        cur = {};
        cur.vertexOffset   = (uint32_t)out.vertexIndices.size();
        cur.triangleOffset = (uint32_t)(out.triangles.size() / 3);
    };

    for (uint32_t t = 0; t + 2 < indexCount; t += 3) {
        uint32_t v[3] = { idx[t], idx[t+1], idx[t+2] };
        uint32_t newVerts = 0;
        for (uint32_t k = 0; k < 3; ++k)
            if (!local.count(v[k])) ++newVerts;
        if (cur.vertexCount + newVerts > kMaxVerts || cur.triangleCount + 1 > kMaxTris)
            flush();
        for (uint32_t k = 0; k < 3; ++k) {
            auto it = local.find(v[k]);
            uint8_t l;
            if (it == local.end()) {
                l = (uint8_t)cur.vertexCount++;
                local[v[k]] = l;
                out.vertexIndices.push_back(v[k]);
            } else l = it->second;
            out.triangles.push_back(l);
        }
        cur.triangleCount++;
    }
    flush();
    (void)vertexCount;
    return out;
}

// ===========================================================================
// D15 CPU acceptance tests.
// ===========================================================================
bool runMeshletSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        (ok ? pass : fail)++;
        log(ok ? LogLevel::Info : LogLevel::Error,
            std::string(ok ? "  [PASS] " : "  [FAIL] ") + what);
    };

    // Generated grid mesh: (N+1)^2 verts, 2*N^2 tris — forces many meshlets.
    constexpr uint32_t N = 40;
    std::vector<float> pos;
    for (uint32_t y = 0; y <= N; ++y)
        for (uint32_t x = 0; x <= N; ++x) {
            pos.push_back((float)x); pos.push_back(0.f); pos.push_back((float)y);
        }
    std::vector<uint32_t> idx;
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            uint32_t a = y*(N+1)+x, b = a+1, c = a+N+1, d = c+1;
            idx.insert(idx.end(), { a,c,b,  b,c,d });
        }
    const uint32_t triCount = (uint32_t)idx.size() / 3;

    MeshletMesh mm = buildMeshlets(pos.data(), (uint32_t)pos.size()/3,
                                   idx.data(), (uint32_t)idx.size());

    check(!mm.meshlets.empty() && mm.meshlets.size() > 10, "builder: grid splits into many meshlets");

    bool budgets = true, localsOk = true;
    uint32_t totalTris = 0;
    for (const Meshlet& m : mm.meshlets) {
        totalTris += m.triangleCount;
        if (m.vertexCount > 64 || m.triangleCount > 124) budgets = false;
        for (uint32_t t = 0; t < m.triangleCount * 3; ++t)
            if (mm.triangles[m.triangleOffset*3 + t] >= m.vertexCount) localsOk = false;
    }
    check(budgets,               "builder: vertex/triangle budgets respected");
    check(localsOk,              "builder: local indices in range");
    check(totalTris == triCount, "builder: every input triangle emitted exactly once");

    // Sphere containment: every meshlet vertex inside its sphere (+epsilon).
    bool spheres = true;
    for (const Meshlet& m : mm.meshlets)
        for (uint32_t v = 0; v < m.vertexCount; ++v) {
            const float* p = pos.data() + mm.vertexIndices[m.vertexOffset+v]*3;
            float dx=p[0]-m.sphere[0], dy=p[1]-m.sphere[1], dz=p[2]-m.sphere[2];
            if (std::sqrt(dx*dx+dy*dy+dz*dz) > m.sphere[3] + 1e-4f) spheres = false;
        }
    check(spheres, "builder: bounding spheres contain all meshlet verts");

    // Flat grid: all faces share a normal -> every cone should be enabled and
    // tight (cutoff near 0 == minDot near 1).
    bool cones = true;
    for (const Meshlet& m : mm.meshlets)
        if (m.cone[3] < 0.f || m.cone[3] > 0.05f) cones = false;
    check(cones, "builder: coplanar cluster -> tight backface cone");

    // Degenerate input: empty mesh -> no meshlets, no crash.
    MeshletMesh empty = buildMeshlets(pos.data(), 0, idx.data(), 0);
    check(empty.meshlets.empty(), "builder: empty input handled");

    log(fail == 0 ? LogLevel::Info : LogLevel::Error,
        "[meshlet selftest] " + std::to_string(pass) + " passed, " +
        std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::rhi
