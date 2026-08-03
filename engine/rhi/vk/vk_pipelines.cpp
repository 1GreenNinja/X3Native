// #28 monolith split — VulkanRenderDevice pipeline/subsystem creation (out-of-line).
// Bodies moved verbatim from the inline class; only inline->out-of-line mechanics
// (VulkanRenderDevice:: qualification, default-arg/override stripping) changed.
// See VulkanRenderDevice_internal.h for the class declaration.
#include "VulkanRenderDevice_internal.h"

namespace x3::rhi {

std::vector<unsigned char> VulkanRenderDevice::readFontFile(const char* relPath, std::string& outResolved) {
        std::vector<unsigned char> bytes;
        if (!relPath) return bytes;
        std::filesystem::path exeDir(".");
#ifdef _WIN32
        {
            char buf[1024];
            DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
            if (n > 0 && n < sizeof(buf))
                exeDir = std::filesystem::path(std::string(buf, n)).parent_path();
        }
#endif
        const std::filesystem::path rel(relPath);
        const std::filesystem::path candidates[] = {
            std::filesystem::path("assets/fonts") / rel,                       // CWD = repo root
            exeDir / ".." / ".." / ".." / "assets" / "fonts" / rel,            // build/bin/<Config>
            exeDir / "assets" / "fonts" / rel,                                 // assets next to exe
            std::filesystem::path("../assets/fonts") / rel,                    // CWD = a subdir
        };
        for (const auto& c : candidates) {
            std::ifstream f(c, std::ios::binary | std::ios::ate);
            if (!f) continue;
            const std::streamsize sz = f.tellg();
            if (sz <= 4) continue;
            f.seekg(0);
            bytes.resize((size_t)sz);
            if (f.read(reinterpret_cast<char*>(bytes.data()), sz)) {
                std::error_code ec; auto norm = std::filesystem::weakly_canonical(c, ec);
                outResolved = (ec ? c : norm).string();
                return bytes;
            }
            bytes.clear();
        }
        return bytes;
    }


std::string VulkanRenderDevice::exeDir() {
        char buf[1024]; DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
        std::string p(buf, n);
        size_t slash = p.find_last_of("\\/");
        return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
    }

std::string VulkanRenderDevice::pipelineCachePath() { return exeDir() + "\\x3pipeline.cache"; }

void VulkanRenderDevice::createPipelineCache() {
        std::vector<char> blob;
        std::ifstream f(pipelineCachePath(), std::ios::binary | std::ios::ate);
        if (f) {
            size_t sz = (size_t)f.tellg(); f.seekg(0);
            blob.resize(sz);
            if (sz) f.read(blob.data(), sz);
        }
        if (blob.size() >= 32) {
            // VkPipelineCacheHeaderVersionOne: u32 headerSize, u32 headerVersion,
            // u32 vendorID, u32 deviceID, u8 uuid[16].
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(m_dev.physical_device, &props);
            uint32_t vendor = 0, device = 0;
            std::memcpy(&vendor, blob.data() + 8, 4);
            std::memcpy(&device, blob.data() + 12, 4);
            if (vendor != props.vendorID || device != props.deviceID ||
                std::memcmp(blob.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
                logInfo("[rhi] pipeline cache: on-disk blob is for a different GPU/driver — ignoring (cold boot)");
                blob.clear();
            }
        } else {
            blob.clear();
        }
        VkPipelineCacheCreateInfo pcc{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        pcc.initialDataSize = blob.size();
        pcc.pInitialData    = blob.empty() ? nullptr : blob.data();
        if (vkCreatePipelineCache(m_dev.device, &pcc, nullptr, &m_pipelineCache) != VK_SUCCESS) {
            // Defensive: retry without initial data, then give up (cache==NULL is legal).
            pcc.initialDataSize = 0; pcc.pInitialData = nullptr;
            if (vkCreatePipelineCache(m_dev.device, &pcc, nullptr, &m_pipelineCache) != VK_SUCCESS)
                m_pipelineCache = VK_NULL_HANDLE;
            blob.clear();
        }
        m_cacheLoadedBytes = blob.size();
        logInfo("[rhi] pipeline cache: " + (blob.empty()
            ? std::string("COLD (no usable on-disk cache; full compile this boot)")
            : std::string("WARM — loaded ") + std::to_string(blob.size()) + " bytes from " + pipelineCachePath()));
    }

void VulkanRenderDevice::savePipelineCache() {
        if (m_pipelineCache == VK_NULL_HANDLE) return;
        size_t sz = 0;
        if (vkGetPipelineCacheData(m_dev.device, m_pipelineCache, &sz, nullptr) != VK_SUCCESS || sz == 0) return;
        std::vector<char> blob(sz);
        if (vkGetPipelineCacheData(m_dev.device, m_pipelineCache, &sz, blob.data()) != VK_SUCCESS) return;
        std::ofstream f(pipelineCachePath(), std::ios::binary | std::ios::trunc);
        if (!f) { logError("[rhi] pipeline cache: save failed (cannot open " + pipelineCachePath() + ")"); return; }
        f.write(blob.data(), (std::streamsize)sz);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[rhi] pipeline cache: saved %zu bytes (this boot compiled %u pipelines in %.1f ms; loaded %llu bytes)",
            sz, m_psoTotal, m_psoCreateMs, (unsigned long long)m_cacheLoadedBytes);
        logInfo(buf);
    }

VkShaderModule VulkanRenderDevice::loadShaderModule(const std::string& relPath) {
        std::string full = exeDir() + "\\" + relPath;
        std::ifstream f(full, std::ios::binary | std::ios::ate);
        if (!f) { logError(std::string("[rhi] shader not found: ") + full); return VK_NULL_HANDLE; }
        size_t sz = (size_t)f.tellg(); f.seekg(0);
        std::vector<char> code(sz); f.read(code.data(), sz);
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = sz;
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_dev.device, &ci, nullptr, &m) != VK_SUCCESS)
            logError(std::string("[rhi] shader module create failed: ") + full);
        else
            noteCreate("shader module", m_modulesLate, m_modulesThisFrame);
        return m;
    }

std::vector<uint32_t> VulkanRenderDevice::loadSpvWords(const std::string& relPath) {
        std::string full = exeDir() + "\\" + relPath;
        std::ifstream f(full, std::ios::binary | std::ios::ate);
        if (!f) { logError(std::string("[rhi] shader not found: ") + full); return {}; }
        size_t sz = (size_t)f.tellg(); f.seekg(0);
        std::vector<uint32_t> words((sz + 3) / 4, 0u);
        f.read(reinterpret_cast<char*>(words.data()), sz);
        return words;
    }

bool VulkanRenderDevice::createGpuCull() {
        m_cullCaps = detectCullCaps(m_dev.physical_device, -1);

        std::vector<uint32_t> cullSpv = loadSpvWords("shaders\\cull.comp.spv");
        std::vector<uint32_t> hzbSpv  = loadSpvWords("shaders\\hzb_build.comp.spv");
        if (cullSpv.empty() || hzbSpv.empty()) return false;
        if (!m_gpuCull.init(m_dev.device, m_cullCaps, cullSpv, hzbSpv,
                            /*buildHzb=*/true, /*reversedZ=*/false)) // X3 = STANDARD Z (verified)
            return false;

        // HZB pyramid sampler (also bound as the harmless dummy when HZB is off):
        // NEAREST + clamp + nearest-mip with the full LOD range, as the cull's
        // textureLod(level) expects.
        {
            VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU = sci.addressModeV = sci.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = VK_LOD_CLAMP_NONE;
            if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_hzbSampler) != VK_SUCCESS)
                return false;
        }

        // Per-frame cull descriptor sets.
        {
            VkDescriptorPoolSize sizes[3]{};
            sizes[0] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kFramesInFlight };
            sizes[1] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight };
            sizes[2] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kFramesInFlight; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_cullPool) != VK_SUCCESS)
                return false;

            VkDescriptorSetLayout dsl = m_gpuCull.cullSetLayout();
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                auto& fr = m_frames[i];
                VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                dsai.descriptorPool = m_cullPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
                if (vkAllocateDescriptorSets(m_dev.device, &dsai, &fr.cullSet) != VK_SUCCESS)
                    return false;
                VkDescriptorBufferInfo bInst { fr.cullInstBuf,  0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo bCmds { fr.indirectBuf,  0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo bVis  { fr.visBuf,       0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo bStat { fr.cullStatsBuf, 0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo bPar  { fr.cullParamsBuf,0, sizeof(CullParamsGpu) };
                // Binding 5: the HZB pyramid once it exists; until then the 1x1
                // white default (never sampled with USE_HZB=0 — just keeps the
                // descriptor valid).
                VkDescriptorImageInfo iHzb{ m_hzbSampler, m_whiteTex.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                VkWriteDescriptorSet w[6]{};
                auto wb = [&](uint32_t bind, VkDescriptorType t, const VkDescriptorBufferInfo* bi) {
                    w[bind].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w[bind].dstSet = fr.cullSet; w[bind].dstBinding = bind;
                    w[bind].descriptorCount = 1; w[bind].descriptorType = t;
                    w[bind].pBufferInfo = bi;
                };
                wb(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bInst);
                wb(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bCmds);
                wb(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bVis);
                wb(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bStat);
                wb(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bPar);
                w[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[5].dstSet = fr.cullSet; w[5].dstBinding = 5; w[5].descriptorCount = 1;
                w[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[5].pImageInfo = &iHzb;
                vkUpdateDescriptorSets(m_dev.device, 6, w, 0, nullptr);
            }
        }
        // ---- Tier 1 (async compute) objects: timeline semaphore + per-frame
        // command pools on the dedicated compute family. Optional.
        if (m_computeQueue) {
            VkSemaphoreTypeCreateInfo tci{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            tci.initialValue = 0;
            VkSemaphoreCreateInfo sci2{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci };
            bool ok = vkCreateSemaphore(m_dev.device, &sci2, nullptr, &m_cullTimeline) == VK_SUCCESS;
            for (uint32_t i = 0; ok && i < kFramesInFlight; ++i) {
                auto& fr = m_frames[i];
                VkCommandPoolCreateInfo pci2{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                pci2.queueFamilyIndex = m_computeFamily;
                ok = vkCreateCommandPool(m_dev.device, &pci2, nullptr, &fr.cullPool) == VK_SUCCESS;
                if (ok) {
                    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                    cai.commandPool = fr.cullPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    cai.commandBufferCount = 1;
                    ok = vkAllocateCommandBuffers(m_dev.device, &cai, &fr.cullCmd) == VK_SUCCESS;
                }
            }
            m_asyncCullReady = ok;
            logInfo(m_asyncCullReady
                ? "[cull] Tier 1 async-compute path READY (timeline semaphore + compute pools)"
                : "[cull] Tier 1 setup failed — async path disabled (Tier 0 still available)");
        }

        logInfo(std::string("[cull] D15 GPU cull ready (caps tier ") +
                std::to_string((int)m_cullCaps.tier) + ", r_cullpath gates the path)");
        return true;
    }

bool VulkanRenderDevice::createHzbTargets() {
        destroyHzbTargets();
        if (!m_gpuCullReady) return false;
        m_hzbW = std::max(1u, m_extent.width  / 2u);
        m_hzbH = std::max(1u, m_extent.height / 2u);
        uint32_t mips = 1; { uint32_t w = std::max(m_hzbW, m_hzbH); while (w > 1) { w /= 2; ++mips; } }
        m_hzbMipCount = std::min(mips, kHzbMaxMips);

        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R32_SFLOAT;
        ici.extent = { m_hzbW, m_hzbH, 1 };
        ici.mipLevels = m_hzbMipCount; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(m_alloc, &ici, &aci, &m_hzbImg, &m_hzbAlloc, nullptr) != VK_SUCCESS) {
            logError("[cull] hzb pyramid create failed"); return false;
        }
        auto makeView = [&](uint32_t baseMip, uint32_t mipCount, VkImageView& out) {
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image = m_hzbImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R32_SFLOAT;
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1 };
            return vkCreateImageView(m_dev.device, &vci, nullptr, &out) == VK_SUCCESS;
        };
        if (!makeView(0, m_hzbMipCount, m_hzbViewAll)) return false;
        for (uint32_t m = 0; m < m_hzbMipCount; ++m)
            if (!makeView(m, 1, m_hzbMipView[m])) return false;

        // One-time UNDEFINED -> GENERAL for the whole chain.
        bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            VkImageMemoryBarrier2 ib{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            ib.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; ib.srcAccessMask = 0;
            ib.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            ib.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.image = m_hzbImg;
            ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_hzbMipCount, 0, 1 };
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cmd, &dep);
        });
        if (!ok) { logError("[cull] hzb layout init failed"); return false; }

        // Per-mip reduce sets: binding0 = src sampler (depth for mip 0, the
        // previous pyramid mip otherwise), binding1 = this mip as storage image.
        if (!m_hzbPool) {
            VkDescriptorPoolSize sizes[2]{};
            sizes[0] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kHzbMaxMips };
            sizes[1] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kHzbMaxMips };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kHzbMaxMips; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_hzbPool) != VK_SUCCESS)
                return false;
        } else {
            vkResetDescriptorPool(m_dev.device, m_hzbPool, 0);
        }
        VkDescriptorSetLayout hdsl = m_gpuCull.hzbSetLayout();
        for (uint32_t m = 0; m < m_hzbMipCount; ++m) {
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_hzbPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &hdsl;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_hzbMipSet[m]) != VK_SUCCESS)
                return false;
            VkDescriptorImageInfo src{};
            src.sampler = m_hzbSampler;
            if (m == 0) { src.imageView = m_depthView; src.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; }
            else        { src.imageView = m_hzbMipView[m - 1]; src.imageLayout = VK_IMAGE_LAYOUT_GENERAL; }
            VkDescriptorImageInfo dst{ VK_NULL_HANDLE, m_hzbMipView[m], VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_hzbMipSet[m]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &src;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_hzbMipSet[m]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &dst;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }

        // Point every frame's cull set (binding 5) at the live pyramid.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_frames[i].cullSet) continue;
            VkDescriptorImageInfo iHzb{ m_hzbSampler, m_hzbViewAll, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_frames[i].cullSet; w.dstBinding = 5; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &iHzb;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }

        m_depthValid = false;          // depth content is fresh/unrendered
        m_hzbReady = true;
        logInfo("[cull] HZB pyramid ready: " + std::to_string(m_hzbW) + "x" +
                std::to_string(m_hzbH) + " mips=" + std::to_string(m_hzbMipCount));
        return true;
    }

void VulkanRenderDevice::destroyHzbTargets() {
        m_hzbReady = false; m_depthValid = false;
        if (m_hzbViewAll) { vkDestroyImageView(m_dev.device, m_hzbViewAll, nullptr); m_hzbViewAll = VK_NULL_HANDLE; }
        for (uint32_t m = 0; m < kHzbMaxMips; ++m) {
            if (m_hzbMipView[m]) { vkDestroyImageView(m_dev.device, m_hzbMipView[m], nullptr); m_hzbMipView[m] = VK_NULL_HANDLE; }
            m_hzbMipSet[m] = VK_NULL_HANDLE;   // pool reset/destroy reclaims them
        }
        if (m_hzbImg) { vmaDestroyImage(m_alloc, m_hzbImg, m_hzbAlloc); m_hzbImg = VK_NULL_HANDLE; m_hzbAlloc = nullptr; }
        m_hzbMipCount = 0; m_hzbW = m_hzbH = 0;
    }

void VulkanRenderDevice::destroyGpuCull() {
        destroyHzbTargets();
        if (m_cullTimeline) { vkDestroySemaphore(m_dev.device, m_cullTimeline, nullptr); m_cullTimeline = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.cullPool) { vkDestroyCommandPool(m_dev.device, fr.cullPool, nullptr); fr.cullPool = VK_NULL_HANDLE; fr.cullCmd = VK_NULL_HANDLE; }
        }
        m_asyncCullReady = false;
        if (m_hzbPool)    { vkDestroyDescriptorPool(m_dev.device, m_hzbPool, nullptr); m_hzbPool = VK_NULL_HANDLE; }
        if (m_cullPool)   { vkDestroyDescriptorPool(m_dev.device, m_cullPool, nullptr); m_cullPool = VK_NULL_HANDLE; }
        if (m_hzbSampler) { vkDestroySampler(m_dev.device, m_hzbSampler, nullptr); m_hzbSampler = VK_NULL_HANDLE; }
        m_gpuCull.shutdown(m_dev.device);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.cullInstBuf)   { vmaDestroyBuffer(m_alloc, fr.cullInstBuf, fr.cullInstAlloc); fr.cullInstBuf = VK_NULL_HANDLE; fr.cullInstAlloc = nullptr; fr.cullInstMapped = nullptr; }
            if (fr.visBuf)        { vmaDestroyBuffer(m_alloc, fr.visBuf, fr.visAlloc); fr.visBuf = VK_NULL_HANDLE; fr.visAlloc = nullptr; fr.visMapped = nullptr; }
            if (fr.cullStatsBuf)  { vmaDestroyBuffer(m_alloc, fr.cullStatsBuf, fr.cullStatsAlloc); fr.cullStatsBuf = VK_NULL_HANDLE; fr.cullStatsAlloc = nullptr; fr.cullStatsMapped = nullptr; }
            if (fr.cullParamsBuf) { vmaDestroyBuffer(m_alloc, fr.cullParamsBuf, fr.cullParamsAlloc); fr.cullParamsBuf = VK_NULL_HANDLE; fr.cullParamsAlloc = nullptr; fr.cullParamsMapped = nullptr; }
            fr.cullSet = VK_NULL_HANDLE;
        }
        m_gpuCullReady = false;
    }

bool VulkanRenderDevice::createGraphics() {
        // ---- Upload primitives: transient command pool + fence for staging copies ----
        VkCommandPoolCreateInfo upci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        upci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        upci.queueFamilyIndex = m_gfxFamily;
        if (vkCreateCommandPool(m_dev.device, &upci, nullptr, &m_uploadPool) != VK_SUCCESS) {
            logError("[rhi] upload pool create failed"); return false;
        }
        VkFenceCreateInfo ufi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(m_dev.device, &ufi, nullptr, &m_uploadFence) != VK_SUCCESS) {
            logError("[rhi] upload fence create failed"); return false;
        }

        // ====================================================================
        // BINDLESS set (set 0): one large COMBINED_IMAGE_SAMPLER array. Flags make
        // it partially-bound (only created slots written) + update-after-bind (we
        // write slots lazily at createTexture, even after the set is bound). The
        // pool carries UPDATE_AFTER_BIND_BIT to match.
        // ====================================================================
        {
            VkDescriptorSetLayoutBinding b{};
            b.binding = 0;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = kMaxTextures;
            b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorBindingFlags bf =
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bfci.bindingCount = 1; bfci.pBindingFlags = &bf;

            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.pNext = &bfci;
            slci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            slci.bindingCount = 1; slci.pBindings = &b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_bindlessLayout) != VK_SUCCESS) {
                logError("[rhi] bindless set layout failed"); return false;
            }

            VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
            pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_bindlessPool) != VK_SUCCESS) {
                logError("[rhi] bindless pool failed"); return false;
            }
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_bindlessPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_bindlessLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_bindlessSet) != VK_SUCCESS) {
                logError("[rhi] bindless set alloc failed"); return false;
            }
        }

        // ====================================================================
        // Object/camera set (set 1): SSBO (binding 0) + camera UBO (binding 1),
        // one set per frame pointing at that frame's persistent-mapped rings.
        // ====================================================================
        {
            VkDescriptorSetLayoutBinding binds[5]{};
            binds[0].binding = 0; binds[0].descriptorCount = 1;
            binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            binds[1].binding = 1; binds[1].descriptorCount = 1;
            binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            // VERTEX: camera viewProj (mesh.vert) + lightViewProj (shadow.vert).
            // FRAGMENT: lightViewProj for the per-pixel shadow projection (mesh.frag).
            binds[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            // D15 GPU cull: binding 2 = the visible-instance indirection SSBO
            // (identity when the cull is off; cull.comp's compaction when on).
            // All four objects[] vertex shaders read it.
            binds[2].binding = 2; binds[2].descriptorCount = 1;
            binds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            // CLUSTERED FORWARD LIGHTING (r_clusterlights): binding 3 = the scene
            // light array (up to kMaxSceneLights), binding 4 = the froxel light
            // lists (per-froxel counts followed by fixed-stride index lists).
            // FRAGMENT-only: mesh.frag + glass.frag read them via the shared
            // shaders/inc/mesh_lighting.glsl iterator. Every other shader on this
            // set simply never declares them (legal, and costs nothing).
            binds[3].binding = 3; binds[3].descriptorCount = 1;
            binds[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            binds[4].binding = 4; binds[4].descriptorCount = 1;
            binds[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.bindingCount = 5; slci.pBindings = binds;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_objSetLayout) != VK_SUCCESS) {
                logError("[rhi] object set layout failed"); return false;
            }

            VkDescriptorPoolSize sizes[2]{};
            // 4 storage buffers per frame: objects, visible-index, scene lights, froxel lists.
            sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[0].descriptorCount = 4 * kFramesInFlight;
            sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[1].descriptorCount = kFramesInFlight;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_objPool) != VK_SUCCESS) {
                logError("[rhi] object pool failed"); return false;
            }

            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                auto& fr = m_frames[i];
                // Object SSBO ring.
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = (VkDeviceSize)kMaxDrawsPerFrame * sizeof(ObjectData);
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_AUTO;
                aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo info{};
                if (x3vmaCreateBuffer(&bci, &aci, &fr.objBuf, &fr.objAlloc, &info) != VK_SUCCESS) {
                    logError("[rhi] object SSBO create failed"); return false;
                }
                fr.objMapped = info.pMappedData;

                // Frame UBO (camera viewProj + sun lightViewProj + point lights).
                VkBufferCreateInfo cbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                cbci.size = sizeof(FrameUBO);
                cbci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VmaAllocationInfo cinfo{};
                if (x3vmaCreateBuffer(&cbci, &aci, &fr.camBuf, &fr.camAlloc, &cinfo) != VK_SUCCESS) {
                    logError("[rhi] camera UBO create failed"); return false;
                }
                fr.camMapped = cinfo.pMappedData;

                // ---- CLUSTERED FORWARD LIGHTING rings (r_clusterlights) ------
                // Scene light array (set 1, binding 3): kMaxSceneLights rows.
                VkBufferCreateInfo lbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                lbci.size = (VkDeviceSize)kMaxSceneLights * sizeof(GpuPointLight);
                lbci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationInfo linfo{};
                if (x3vmaCreateBuffer(&lbci, &aci, &fr.lightBuf, &fr.lightAlloc, &linfo) != VK_SUCCESS) {
                    logError("[rhi] cluster light SSBO create failed"); return false;
                }
                fr.lightMapped = linfo.pMappedData;

                // Froxel lists (set 1, binding 4): [counts | fixed-stride lists].
                VkBufferCreateInfo clbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                clbci.size = (VkDeviceSize)kClusterCount * (1u + kMaxLightsPerCluster) * sizeof(uint32_t);
                clbci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationInfo clinfo{};
                if (x3vmaCreateBuffer(&clbci, &aci, &fr.clusterBuf, &fr.clusterAlloc, &clinfo) != VK_SUCCESS) {
                    logError("[rhi] cluster list SSBO create failed"); return false;
                }
                fr.clusterMapped = clinfo.pMappedData;
                // Zero the counts so a frame that never runs the assignment (the
                // very first frame, or any frame with r_clusterlights 0 followed by
                // a toggle ON mid-flight) cannot read stale list lengths.
                std::memset(fr.clusterMapped, 0, (size_t)clbci.size);

                // Indirect-command buffer (one VkDrawIndexedIndirectCommand per
                // distinct mesh; capped at kMaxTextures meshes which is plenty).
                // STORAGE usage added for D15: cull.comp atomically bumps each
                // command's instanceCount in place (binding 1).
                VkBufferCreateInfo ibci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                ibci.size = (VkDeviceSize)kMaxDrawMeshes * sizeof(VkDrawIndexedIndirectCommand);
                ibci.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                // Tier 1: the dedicated compute queue writes this buffer while the
                // graphics queue reads it -> CONCURRENT sharing across the two
                // families (simpler than ownership transfers; revisit if profiled).
                const uint32_t cullFamilies[2] = { m_gfxFamily, m_computeFamily };
                if (m_computeQueue) {
                    ibci.sharingMode = VK_SHARING_MODE_CONCURRENT;
                    ibci.queueFamilyIndexCount = 2;
                    ibci.pQueueFamilyIndices = cullFamilies;
                }
                VmaAllocationInfo iinfo{};
                if (x3vmaCreateBuffer(&ibci, &aci, &fr.indirectBuf, &fr.indirectAlloc, &iinfo) != VK_SUCCESS) {
                    logError("[rhi] indirect buffer create failed"); return false;
                }
                fr.indirectMapped = iinfo.pMappedData;

                // ---- D15 GPU cull per-frame ring ---------------------------
                // visible-instance indirection (vertex shaders read binding 2;
                // cull.comp writes binding 2 of the CULL set). Identity-filled so
                // the CPU path is byte-identical to pre-D15 behavior.
                {
                    VkBufferCreateInfo vbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    vbci.size = (VkDeviceSize)kMaxDrawsPerFrame * sizeof(uint32_t);
                    vbci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                    if (m_computeQueue) {
                        vbci.sharingMode = VK_SHARING_MODE_CONCURRENT;
                        vbci.queueFamilyIndexCount = 2;
                        vbci.pQueueFamilyIndices = cullFamilies;
                    }
                    VmaAllocationInfo vinfo{};
                    if (vmaCreateBuffer(m_alloc, &vbci, &aci, &fr.visBuf, &fr.visAlloc, &vinfo) != VK_SUCCESS) {
                        logError("[rhi] visible-instance buffer create failed"); return false;
                    }
                    fr.visMapped = vinfo.pMappedData;
                    uint32_t* ids = static_cast<uint32_t*>(fr.visMapped);
                    for (uint32_t k = 0; k < kMaxDrawsPerFrame; ++k) ids[k] = k;

                    VkBufferCreateInfo cbci2{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    cbci2.size = (VkDeviceSize)kMaxDrawsPerFrame * sizeof(CullInstanceGpu);
                    cbci2.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                    if (m_computeQueue) {
                        cbci2.sharingMode = VK_SHARING_MODE_CONCURRENT;
                        cbci2.queueFamilyIndexCount = 2;
                        cbci2.pQueueFamilyIndices = cullFamilies;
                    }
                    VmaAllocationInfo ciinfo{};
                    if (vmaCreateBuffer(m_alloc, &cbci2, &aci, &fr.cullInstBuf, &fr.cullInstAlloc, &ciinfo) != VK_SUCCESS) {
                        logError("[rhi] cull instance buffer create failed"); return false;
                    }
                    fr.cullInstMapped = ciinfo.pMappedData;

                    // Stats: GPU-written, host-READ on slot reuse -> RANDOM access
                    // (cached) memory so the readback isn't a WC-memory read.
                    VkBufferCreateInfo stbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    stbci.size = sizeof(CullStatsGpu);
                    stbci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                    if (m_computeQueue) {
                        stbci.sharingMode = VK_SHARING_MODE_CONCURRENT;
                        stbci.queueFamilyIndexCount = 2;
                        stbci.pQueueFamilyIndices = cullFamilies;
                    }
                    VmaAllocationCreateInfo staci{};
                    staci.usage = VMA_MEMORY_USAGE_AUTO;
                    staci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                    VmaAllocationInfo stinfo{};
                    if (vmaCreateBuffer(m_alloc, &stbci, &staci, &fr.cullStatsBuf, &fr.cullStatsAlloc, &stinfo) != VK_SUCCESS) {
                        logError("[rhi] cull stats buffer create failed"); return false;
                    }
                    fr.cullStatsMapped = stinfo.pMappedData;
                    std::memset(fr.cullStatsMapped, 0, sizeof(CullStatsGpu));

                    VkBufferCreateInfo pbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    pbci.size = sizeof(CullParamsGpu);
                    pbci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                    if (m_computeQueue) {
                        pbci.sharingMode = VK_SHARING_MODE_CONCURRENT;
                        pbci.queueFamilyIndexCount = 2;
                        pbci.pQueueFamilyIndices = cullFamilies;
                    }
                    VmaAllocationInfo pinfo{};
                    if (vmaCreateBuffer(m_alloc, &pbci, &aci, &fr.cullParamsBuf, &fr.cullParamsAlloc, &pinfo) != VK_SUCCESS) {
                        logError("[rhi] cull params buffer create failed"); return false;
                    }
                    fr.cullParamsMapped = pinfo.pMappedData;
                }

                // Allocate + write the set-1 descriptor (points at this frame's
                // SSBO + camera UBO + visible-index SSBO; written once, buffers
                // are persistent-mapped).
                VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                dsai.descriptorPool = m_objPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_objSetLayout;
                if (vkAllocateDescriptorSets(m_dev.device, &dsai, &fr.objSet) != VK_SUCCESS) {
                    logError("[rhi] object set alloc failed"); return false;
                }
                VkDescriptorBufferInfo sbi{ fr.objBuf, 0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo cbi{ fr.camBuf, 0, sizeof(FrameUBO) };
                VkDescriptorBufferInfo vbi{ fr.visBuf, 0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo lbi{ fr.lightBuf, 0, VK_WHOLE_SIZE };
                VkDescriptorBufferInfo clbi{ fr.clusterBuf, 0, VK_WHOLE_SIZE };
                VkWriteDescriptorSet writes[5]{};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = fr.objSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &sbi;
                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = fr.objSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[1].pBufferInfo = &cbi;
                writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[2].dstSet = fr.objSet; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &vbi;
                writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[3].dstSet = fr.objSet; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
                writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[3].pBufferInfo = &lbi;
                writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[4].dstSet = fr.objSet; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
                writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[4].pBufferInfo = &clbi;
                vkUpdateDescriptorSets(m_dev.device, 5, writes, 0, nullptr);
            }
        }

        // ---- Built-in 1x1 white default texture (sRGB) -> bindless slot 0 ----
        const uint8_t white[4] = { 255, 255, 255, 255 };
        if (!createSampledTexture(white, 1, 1, true, m_whiteTex)) {
            logError("[rhi] default white texture create failed"); return false;
        }
        if (!registerBindless(m_whiteTex) || m_whiteTex.bindlessIndex != 0) {
            logError("[rhi] white default must occupy bindless slot 0"); return false;
        }

        // ---- mesh.frag SSAO set (set 3): AO sampler + SsaoControl UBO + the
        // SSR/RT reflection buffer (binding 2, refl.comp output — bound to the
        // blurred-AO view as a layout-valid placeholder until the refl chain is
        // built). Created here (only needs the device) so the mesh pipeline
        // layout can include it; the rest is built later in createSsao(). ----
        {
            // Bindings 3/4 = the DDGI irradiance + visibility atlases (r_ddgi).
            // mesh.frag statically references them, so they are part of the
            // layout on EVERY device; non-DDGI paths point them at the blurred-AO
            // view as a layout-valid placeholder (never sampled — gate 0).
            // Binding 6 = the DENOISED reflection buffer (r_refldenoise). Like
            // bindings 3/4 it is on EVERY device because mesh.frag statically
            // references it; when the denoise stage is off it is pointed at the
            // RAW reflection view (or, before the refl chain exists at all, at
            // the blurred-AO placeholder), which is exactly what makes
            // r_refldenoise 0 bit-exact. NOTE the array ORDER: binding 6 sits at
            // index 5 and the RT-only TLAS at index 6, so the `? 7 : 6` count
            // below drops the TLAS and keeps binding 6 on non-RT devices.
            VkDescriptorSetLayoutBinding b[7]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[4].binding = 4; b[4].descriptorCount = 1;
            b[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            // Binding 5 = the scene TLAS for RT soft shadows (r_rtshadows) — in
            // the LAYOUT only on ray-query devices (the AS descriptor type needs
            // VK_KHR_acceleration_structure). Only the mesh_rt.frag variant
            // statically references it; the plain pipelines never touch it, so
            // it may stay unwritten until the first TLAS build lands.
            b[5].binding = 6; b[5].descriptorCount = 1;
            b[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[6].binding = 5; b[6].descriptorCount = 1;
            b[6].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = m_rtSupported ? 7u : 6u; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_meshAoSetLayout) != VK_SUCCESS) {
                logError("[rhi] mesh ao set layout failed"); return false;
            }
        }
        // ---- mesh.frag IBL set (set 4): irradiance cube + prefilter cube + BRDF LUT.
        // Created here (device-only) so the mesh pipeline layout can declare it; the
        // images/sets are built later in createIbl(). 3 combined image samplers. ----
        {
            VkDescriptorSetLayoutBinding b[3]{};
            for (int i = 0; i < 3; ++i) {
                b[i].binding = i; b[i].descriptorCount = 1;
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 3; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_iblMeshSetLayout) != VK_SUCCESS) {
                logError("[rhi] mesh ibl set layout failed"); return false;
            }
        }

        // ---- glass.frag set 4: scene-color copy sampler + GlassControl UBO -----
        // The glass pipeline's EXTRA set (beyond the 4 shared with the opaque mesh
        // path), so glass.frag can sample the scene behind it (refraction M2/frost
        // M4) + read the per-frame camera pos / time / dev overrides. Created here
        // (only needs the device) so the glass pipeline layout can include it; the
        // UBOs + descriptor sets are built later (after the scene-copy target exists)
        // in createGlassResources / writeGlassDescriptors.
        {
            // binding0 = scene-color copy (sharp, refraction M2); binding1 =
            // GlassControl UBO; binding2 = frostiest blur level (M4 frost lerp).
            VkDescriptorSetLayoutBinding b[3]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 3; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_glassSetLayout) != VK_SUCCESS) {
                logError("[rhi] glass set layout failed"); return false;
            }
        }

        // ---- Mesh pipeline (bindless texture + per-object SSBO + indirect) ----
        VkShaderModule vs = loadShaderModule("shaders\\mesh.vert.spv");
        VkShaderModule fs = loadShaderModule("shaders\\mesh.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        // VERTEX COMPRESSION (Lane 5): the layout comes from the ACTIVE format,
        // which is the legacy 32 B float3/float3/float2 unless --vtxfmt says
        // otherwise. The shader still declares vec3/vec3/vec2 either way — the
        // fixed-function vertex fetch does the unpack.
        VkVertexInputBindingDescription bind{};
        VkVertexInputAttributeDescription attrs[3]{};
        meshVertexInput(bind, attrs);
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth: the SSAO depth pre-pass already wrote the EXACT camera depth, so
        // the main pass tests EQUAL with depth-write OFF (no double depth write, no
        // z-fight — same geometry, same transform). This also lets SSAO read a
        // complete depth buffer before lighting.
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_EQUAL;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        // GPU-driven layout: set 0 = bindless textures, set 1 = object SSBO +
        // camera UBO, set 2 = the shadow map (perf-stack E), set 3 = the SSAO AO
        // texture + control UBO, set 4 = IBL (irradiance + prefilter cubes + BRDF
        // LUT). NO push constants (per-object data is in the SSBO).
        VkDescriptorSetLayout setLayouts[5] = { m_bindlessLayout, m_objSetLayout, m_shadowSetLayout, m_meshAoSetLayout, m_iblMeshSetLayout };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 5; plci.pSetLayouts = setLayouts;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_meshLayout) != VK_SUCCESS) {
            logError("[rhi] pipeline layout failed"); return false;
        }

        // HDR pipeline: the mesh pass now renders into the R16G16B16A16_SFLOAT
        // linear HDR scene target (NOT the LDR swapchain). Tonemap moved to the
        // composite pass. The depth attachment format is unchanged.
        const VkFormat hdrFmt = kHdrFormat;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &hdrFmt;
        prci.depthAttachmentFormat = m_depthFormat;

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_meshLayout;
        // SSAO path pipeline: depth-test EQUAL, depth-write OFF (the depth pre-pass
        // already wrote depth). Created with `dss` set above.
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipeline);
        if (pr != VK_SUCCESS) {
            vkDestroyShaderModule(m_dev.device, vs, nullptr);
            vkDestroyShaderModule(m_dev.device, fs, nullptr);
            logError("[rhi] graphics pipeline create failed"); return false;
        }

        // No-SSAO path pipeline: the ORIGINAL behavior (no depth pre-pass) — depth
        // test LESS + depth write ON, so the main pass clears + writes depth itself.
        // Selected at draw time when m_ssao.enabled is false (true zero SSAO cost).
        VkPipelineDepthStencilStateCreateInfo dssNo = dss;
        dssNo.depthWriteEnable = VK_TRUE;
        dssNo.depthCompareOp   = VK_COMPARE_OP_LESS;
        gpci.pDepthStencilState = &dssNo;
        pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineNoSsao);

        // ---- Reflection-PROBE scene PSO: mesh_probe.vert (per-face viewProj via push
        // constant) + the SAME mesh.frag, opaque depth LESS+write into the HDR env-cube
        // face. regenIblFromSky() uses it to bake interior geometry into the IBL env so
        // glossy metals reflect the room, not the open sky. gpci is in opaque (dssNo/cb/rs).
        {
            VkShaderModule pvs = loadShaderModule("shaders\\mesh_probe.vert.spv");
            if (pvs) {
                VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
                pcr.offset = 0; pcr.size = sizeof(glm::mat4);
                VkPipelineLayoutCreateInfo plp{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
                plp.setLayoutCount = 5; plp.pSetLayouts = setLayouts;       // same 5 sets as the mesh pass
                plp.pushConstantRangeCount = 1; plp.pPushConstantRanges = &pcr;
                if (vkCreatePipelineLayout(m_dev.device, &plp, nullptr, &m_meshProbeLayout) == VK_SUCCESS) {
                    VkPipelineShaderStageCreateInfo pst[2] = { stages[0], stages[1] };
                    pst[0].module = pvs;                                    // swap vertex -> probe (fs unchanged)
                    VkGraphicsPipelineCreateInfo ppgci = gpci;              // opaque state, HDR color + depth fmt
                    ppgci.pStages = pst; ppgci.layout = m_meshProbeLayout;
                    if (x3CreateGraphicsPipelines(1, &ppgci, nullptr, &m_meshProbePipe) != VK_SUCCESS)
                        logError("[rhi] reflection-probe pipeline create failed (probe disabled)");
                }
                vkDestroyShaderModule(m_dev.device, pvs, nullptr);
            } else {
                logError("[rhi] mesh_probe.vert.spv failed to load (reflection probe disabled)");
            }
        }

        // Transparent (BLEND/glass) variant — same shaders/layout/HDR target, vs/fs still alive.
        // src-alpha OVER blend, depth-test LESS_OR_EQUAL (works for both the EQUAL-prepass and the
        // LESS no-prepass opaque depth), NO depth-write, cull NONE (double-sided glass).
        VkResult prT = VK_SUCCESS;
        {
            VkPipelineColorBlendAttachmentState cbaT = cba;
            cbaT.blendEnable = VK_TRUE;
            cbaT.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cbaT.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cbaT.colorBlendOp = VK_BLEND_OP_ADD;
            cbaT.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cbaT.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cbaT.alphaBlendOp = VK_BLEND_OP_ADD;
            VkPipelineColorBlendStateCreateInfo cbT = cb; cbT.pAttachments = &cbaT;
            VkPipelineDepthStencilStateCreateInfo dssT = dss;   // depthTest ON, write OFF
            dssT.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineRasterizationStateCreateInfo rsT = rs; rsT.cullMode = VK_CULL_MODE_NONE;
            gpci.pColorBlendState = &cbT; gpci.pDepthStencilState = &dssT; gpci.pRasterizationState = &rsT;
            prT = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineTransparent);
            gpci.pColorBlendState = &cb; gpci.pDepthStencilState = &dssNo; gpci.pRasterizationState = &rs;  // restore
        }

        // ---- RT soft-shadow variants (r_rtshadows; ray-query devices only) ----
        // The SAME three mesh pipelines with mesh_rt.frag.spv (inline ray-query
        // shadow rays; TLAS at set3/binding5, which the layout above carries on
        // RT devices). Identical fixed-function state per-variant; bound at draw
        // time only on frames where the TLAS descriptor is valid. NON-FATAL on
        // failure: the want-gate checks the pipeline handle, so a load/create
        // failure simply leaves RT shadows off (plain raster path).
        if (m_rtSupported) {
            VkShaderModule fsRt = loadShaderModule("shaders\\mesh_rt.frag.spv");
            if (fsRt) {
                VkPipelineShaderStageCreateInfo rtStages[2] = { stages[0], stages[1] };
                rtStages[1].module = fsRt;
                gpci.pStages = rtStages;
                // EQUAL/no-write (depth pre-pass on) variant:
                gpci.pDepthStencilState = &dss;
                if (x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineRt) != VK_SUCCESS)
                    m_meshPipelineRt = VK_NULL_HANDLE;
                // LESS/write (no pre-pass) variant:
                gpci.pDepthStencilState = &dssNo;
                if (x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineNoSsaoRt) != VK_SUCCESS)
                    m_meshPipelineNoSsaoRt = VK_NULL_HANDLE;
                // Transparent (BLEND) variant — same state the block above used:
                {
                    VkPipelineColorBlendAttachmentState cbaT = cba;
                    cbaT.blendEnable = VK_TRUE;
                    cbaT.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                    cbaT.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    cbaT.colorBlendOp = VK_BLEND_OP_ADD;
                    cbaT.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaT.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    cbaT.alphaBlendOp = VK_BLEND_OP_ADD;
                    VkPipelineColorBlendStateCreateInfo cbT = cb; cbT.pAttachments = &cbaT;
                    VkPipelineDepthStencilStateCreateInfo dssT = dss;
                    dssT.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                    VkPipelineRasterizationStateCreateInfo rsT = rs; rsT.cullMode = VK_CULL_MODE_NONE;
                    gpci.pColorBlendState = &cbT; gpci.pDepthStencilState = &dssT; gpci.pRasterizationState = &rsT;
                    if (x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_meshPipelineTransparentRt) != VK_SUCCESS)
                        m_meshPipelineTransparentRt = VK_NULL_HANDLE;
                    gpci.pColorBlendState = &cb; gpci.pDepthStencilState = &dssNo; gpci.pRasterizationState = &rs;  // restore
                }
                gpci.pStages = stages;   // restore for the planet clones below
                vkDestroyShaderModule(m_dev.device, fsRt, nullptr);
                if (m_meshPipelineRt && m_meshPipelineNoSsaoRt)
                    logInfo("[rhi] RT soft-shadow mesh pipelines ready (mesh_rt.frag: inline ray-query sun + point shadows)");
                else
                    logError("[rhi] RT soft-shadow pipeline create failed — r_rtshadows stays raster-only");
            } else {
                logError("[rhi] mesh_rt.frag.spv failed to load — r_rtshadows stays raster-only");
            }
        }

        // ---- Planet body pipelines (FORGE3D port) — one OPAQUE PSO per type ----
        // Each is a CLONE of the OPAQUE mesh PSO (same MeshVertex input via `vin`,
        // depth LESS + write via `dssNo`, cull BACK via `rs`, no blend via `cb`, same
        // HDR target via `prci`) but with planet.vert + the per-type fragment shader,
        // all sharing ONE layout (set0 bindless + set1 object SSBO/camera UBO + a
        // 128-byte push-constant range, VERTEX|FRAGMENT). gpci is currently in the
        // restored opaque state (cb/dssNo/rs) — exactly the opaque depth/cull/blend.
        // Per-type fragment .spv in PlanetType enum order (Moon..Sun). Atmosphere /
        // suncorona / ring shells are DEFERRED (not wired this pass).
        VkResult prP = VK_SUCCESS;
        {
            static const char* kPlanetFrags[PT_OpaqueCount] = {
                "shaders\\planet_moon.frag.spv",
                "shaders\\planet_ice.frag.spv",
                "shaders\\planet_gas.frag.spv",
                "shaders\\planet_lava.frag.spv",
                "shaders\\planet_terrestrial.frag.spv",
                "shaders\\planet_oceanic.frag.spv",
                "shaders\\planet_sand.frag.spv",
                "shaders\\planet_thunderstorm.frag.spv",
                "shaders\\planet_sun.frag.spv",
            };
            VkShaderModule pvs = loadShaderModule("shaders\\planet.vert.spv");
            if (!pvs) {
                vkDestroyShaderModule(m_dev.device, vs, nullptr);
                vkDestroyShaderModule(m_dev.device, fs, nullptr);
                logError("[rhi] planet vertex shader module failed to load"); return false;
            }
            // Shared layout: SAME set0 (bindless) + set1 (object SSBO + camera UBO)
            // layouts the mesh pipeline uses, + a 128-byte push constant for both stages.
            VkDescriptorSetLayout planetSetLayouts[2] = { m_bindlessLayout, m_objSetLayout };
            VkPushConstantRange pcRange{};
            pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            pcRange.offset = 0; pcRange.size = 128;
            VkPipelineLayoutCreateInfo pplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pplci.setLayoutCount = 2; pplci.pSetLayouts = planetSetLayouts;
            pplci.pushConstantRangeCount = 1; pplci.pPushConstantRanges = &pcRange;
            if (vkCreatePipelineLayout(m_dev.device, &pplci, nullptr, &m_planetPipelineLayout) != VK_SUCCESS) {
                vkDestroyShaderModule(m_dev.device, pvs, nullptr);
                vkDestroyShaderModule(m_dev.device, vs, nullptr);
                vkDestroyShaderModule(m_dev.device, fs, nullptr);
                logError("[rhi] planet pipeline layout failed"); return false;
            }
            for (uint32_t pt = 0; pt < (uint32_t)PT_OpaqueCount && prP == VK_SUCCESS; ++pt) {
                VkShaderModule pfs = loadShaderModule(kPlanetFrags[pt]);
                if (!pfs) { logError(std::string("[rhi] planet frag failed to load: ") + kPlanetFrags[pt]);
                            prP = VK_ERROR_INITIALIZATION_FAILED; break; }
                VkPipelineShaderStageCreateInfo pstages[2]{};
                pstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   pstages[0].module = pvs; pstages[0].pName = "main";
                pstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; pstages[1].module = pfs; pstages[1].pName = "main";
                VkGraphicsPipelineCreateInfo pgci = gpci;   // copies the opaque state set above
                pgci.pStages = pstages; pgci.layout = m_planetPipelineLayout;
                prP = x3CreateGraphicsPipelines(1, &pgci, nullptr, &m_planetPipelines[pt]);
                vkDestroyShaderModule(m_dev.device, pfs, nullptr);
            }

            // ---- TRANSPARENT glow shells (DEFERRED layers, now wired) ----------
            // Three more PSOs sharing m_planetPipelineLayout + planet.vert, drawn
            // AFTER the opaque bodies. They override the opaque blend/depth/cull:
            //   Atmosphere / SunCorona : ADDITIVE (srcRGB=ONE, dstRGB=ONE), depth
            //       test LEQUAL + write OFF, cull NONE (far limb hemisphere shows).
            //   Ring : ALPHA (SRC_ALPHA / ONE_MINUS_SRC_ALPHA), depth LEQUAL +
            //       write OFF, cull NONE (annulus visible from both faces).
            // The frags emit PREMULTIPLIED glow (atmosphere/corona) so srcRGB=ONE.
            struct TransP { PlanetType pt; const char* frag; bool additive; };
            static const TransP kTrans[] = {
                { PT_Atmosphere, "shaders\\planet_atmosphere.frag.spv", true  },
                { PT_SunCorona,  "shaders\\planet_suncorona.frag.spv",  true  },
                { PT_Ring,       "shaders\\planet_ring.frag.spv",       false },
            };
            // Shared depth (LEQUAL, write OFF) + raster (cull NONE) for all three.
            VkPipelineDepthStencilStateCreateInfo dssGlow{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            dssGlow.depthTestEnable = VK_TRUE; dssGlow.depthWriteEnable = VK_FALSE;
            dssGlow.depthCompareOp  = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineRasterizationStateCreateInfo rsGlow{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rsGlow.polygonMode = VK_POLYGON_MODE_FILL; rsGlow.cullMode = VK_CULL_MODE_NONE;
            rsGlow.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rsGlow.lineWidth = 1.0f;
            for (const TransP& tp : kTrans) {
                if (prP != VK_SUCCESS) break;
                VkShaderModule pfs = loadShaderModule(tp.frag);
                if (!pfs) { logError(std::string("[rhi] planet (transparent) frag failed to load: ") + tp.frag);
                            prP = VK_ERROR_INITIALIZATION_FAILED; break; }
                VkPipelineShaderStageCreateInfo pstages[2]{};
                pstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   pstages[0].module = pvs; pstages[0].pName = "main";
                pstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; pstages[1].module = pfs; pstages[1].pName = "main";
                VkPipelineColorBlendAttachmentState cbaG{};
                cbaG.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
                cbaG.blendEnable = VK_TRUE;
                cbaG.colorBlendOp = VK_BLEND_OP_ADD; cbaG.alphaBlendOp = VK_BLEND_OP_ADD;
                if (tp.additive) {                       // premultiplied glow: ONE/ONE
                    cbaG.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaG.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaG.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaG.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                } else {                                 // ring: SRC_ALPHA over
                    cbaG.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                    cbaG.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    cbaG.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbaG.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                }
                VkPipelineColorBlendStateCreateInfo cbG = cb; cbG.pAttachments = &cbaG;
                VkGraphicsPipelineCreateInfo pgci = gpci;   // opaque base; override below
                pgci.pStages = pstages; pgci.layout = m_planetPipelineLayout;
                pgci.pColorBlendState = &cbG; pgci.pDepthStencilState = &dssGlow; pgci.pRasterizationState = &rsGlow;
                prP = x3CreateGraphicsPipelines(1, &pgci, nullptr, &m_planetPipelines[tp.pt]);
                vkDestroyShaderModule(m_dev.device, pfs, nullptr);
            }
            vkDestroyShaderModule(m_dev.device, pvs, nullptr);
        }

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] no-ssao graphics pipeline create failed"); return false; }
        if (prT != VK_SUCCESS) { logError("[rhi] transparent graphics pipeline create failed"); return false; }
        if (prP != VK_SUCCESS) { logError("[rhi] planet graphics pipeline create failed"); return false; }

        logInfo("[rhi] GPU-driven mesh pipeline ready (bindless textures + per-object SSBO + multidraw-indirect)");
        logInfo("[rhi] planet body pipeline ready (push-constant model + bindless triplanar PBR)");

        // ---- Translucent GLASS pipeline (transparent pass) ----
        // Shares mesh.vert + sets 0-3 with the opaque mesh path, but uses its OWN
        // pipeline layout m_glassLayout (sets 0-3 identical + set 4 = scene-color
        // copy sampler + GlassControl UBO) so glass.frag can sample the scene behind
        // it (refraction M2 / frost M4). glass.frag, alpha blend ON
        // (SRC_ALPHA/ONE_MINUS_SRC_ALPHA), depth-test LESS_OR_EQUAL with depth-write
        // OFF (composites over the opaque scene without disturbing depth), cull NONE
        // (double-sided glass). GRACEFUL FALLBACK (spec §5): on any failure the
        // pipeline stays NULL and the glass pass is skipped — opaque is never affected.
        {
            // Glass pipeline layout: the 4 shared mesh sets + the glass-only set 4
            // + the IBL set as set 5 (W8-2: the SAME set the opaque path binds at
            // set 4 — prefiltered env + BRDF LUT — so glass.frag mirrors the real
            // environment; m_iblMeshSetLayout was created above at mesh-layout time).
            VkDescriptorSetLayout glassSets[6] = {
                m_bindlessLayout, m_objSetLayout, m_shadowSetLayout,
                m_meshAoSetLayout, m_glassSetLayout, m_iblMeshSetLayout };
            VkPipelineLayoutCreateInfo gplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            gplci.setLayoutCount = 6; gplci.pSetLayouts = glassSets;
            if (vkCreatePipelineLayout(m_dev.device, &gplci, nullptr, &m_glassLayout) != VK_SUCCESS) {
                m_glassLayout = VK_NULL_HANDLE;
                logError("[rhi] glass pipeline layout failed — glass pass disabled (opaque unaffected)");
            }

            VkShaderModule gvs = m_glassLayout ? loadShaderModule("shaders\\mesh.vert.spv") : VK_NULL_HANDLE;
            VkShaderModule gfs = m_glassLayout ? loadShaderModule("shaders\\glass.frag.spv") : VK_NULL_HANDLE;
            if (!m_glassLayout || !gvs || !gfs) {
                if (gvs) vkDestroyShaderModule(m_dev.device, gvs, nullptr);
                if (gfs) vkDestroyShaderModule(m_dev.device, gfs, nullptr);
                if (m_glassLayout) logError("[rhi] glass shader load failed — glass pass disabled (opaque unaffected)");
            } else {
                VkPipelineShaderStageCreateInfo gst[2];
                gst[0] = stages[0]; gst[0].module = gvs;   // mesh.vert (shared)
                gst[1] = stages[1]; gst[1].module = gfs;   // glass.frag

                // Double-sided glass: no back-face cull.
                VkPipelineRasterizationStateCreateInfo grs = rs;
                grs.cullMode = VK_CULL_MODE_NONE;

                // Depth: test LESS_OR_EQUAL against the opaque depth, no write.
                VkPipelineDepthStencilStateCreateInfo gdss = dss;
                gdss.depthTestEnable  = VK_TRUE;
                gdss.depthWriteEnable = VK_FALSE;
                gdss.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

                // Standard straight-alpha blend.
                VkPipelineColorBlendAttachmentState gcba = cba;
                gcba.blendEnable         = VK_TRUE;
                gcba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                gcba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                gcba.colorBlendOp        = VK_BLEND_OP_ADD;
                gcba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                gcba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                gcba.alphaBlendOp        = VK_BLEND_OP_ADD;
                VkPipelineColorBlendStateCreateInfo gcb = cb;
                gcb.pAttachments = &gcba;

                VkGraphicsPipelineCreateInfo ggpci = gpci;
                ggpci.layout             = m_glassLayout;   // glass-specific (set 4)
                ggpci.pStages            = gst;
                ggpci.pRasterizationState = &grs;
                ggpci.pDepthStencilState  = &gdss;
                ggpci.pColorBlendState    = &gcb;
                VkResult gpr = x3CreateGraphicsPipelines(1,
                                                         &ggpci, nullptr, &m_glassPipeline);
                vkDestroyShaderModule(m_dev.device, gvs, nullptr);
                vkDestroyShaderModule(m_dev.device, gfs, nullptr);
                if (gpr != VK_SUCCESS) {
                    m_glassPipeline = VK_NULL_HANDLE;
                    logError("[rhi] glass pipeline create failed — glass pass disabled (opaque unaffected)");
                } else {
                    logInfo("[rhi] translucent glass pipeline ready (transparent pass)");
                }
            }
        }

        // Now that m_objSetLayout exists, build the depth-only shadow pipeline.
        if (!createShadowPipeline()) return false;
        return true;
    }

bool VulkanRenderDevice::createShadowImage() {
        // CSM (Lane 3): the map is a 2D ARRAY of kCsmCascades layers. Layer 0 is
        // the legacy cascade — with r_csm 0 ONLY layer 0 is rendered and ONLY
        // layer 0 is sampled, so the image is bit-identical to the pre-cascade
        // renderer. The extra layers cost VRAM only (2048^2 * 4 B * 4 = 64 MB),
        // never time, when CSM is off.
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = m_shadowFormat;
        ici.extent = { kShadowDim, kShadowDim, 1 };
        ici.mipLevels = 1; ici.arrayLayers = kCsmCascades;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&ici, &aci, &m_shadowImg, &m_shadowAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] shadow image create failed"); return false;
        }
        // SAMPLING view: the whole array (sampler2DArrayShadow in mesh/glass.frag).
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = m_shadowImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = m_shadowFormat;
        vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kCsmCascades };
        if (vkCreateImageView(m_dev.device, &vci, nullptr, &m_shadowView) != VK_SUCCESS) {
            logError("[rhi] shadow view create failed"); return false;
        }
        // Per-cascade RENDER views: dynamic rendering attaches ONE layer per
        // cascade pass, so each layer needs its own single-layer 2D view.
        for (uint32_t i = 0; i < kCsmCascades; ++i) {
            VkImageViewCreateInfo lv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            lv.image = m_shadowImg; lv.viewType = VK_IMAGE_VIEW_TYPE_2D; lv.format = m_shadowFormat;
            lv.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1 };
            if (vkCreateImageView(m_dev.device, &lv, nullptr, &m_shadowLayerView[i]) != VK_SUCCESS) {
                logError("[rhi] shadow layer view create failed"); return false;
            }
        }

        // Compare-enabled sampler: hardware PCF. LESS_OR_EQUAL means texture()
        // returns 1 where refDepth <= storedDepth (lit). CLAMP_TO_EDGE + a white
        // border avoids spurious shadowing at the map edges.
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside == lit
        sci.compareEnable = VK_TRUE;
        sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        sci.maxLod = 0.0f;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_shadowSampler) != VK_SUCCESS) {
            logError("[rhi] shadow sampler create failed"); return false;
        }

        // Set-2 layout: binding 0 = the shadow array sampler (sampler2DArrayShadow),
        // binding 1 = the per-frame CSM UBO (cascade matrices, splits, biases).
        // The UBO is per-frame-in-flight, so set 2 becomes an ARRAY of sets and the
        // binding site picks m_shadowSet[m_frameIdx].
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorCount = 1;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorCount = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 2; slci.pBindings = b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_shadowSetLayout) != VK_SUCCESS) {
            logError("[rhi] shadow set layout failed"); return false;
        }
        VkDescriptorPoolSize ps[2] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight },
        };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        if (x3CreateDescriptorPool(&pci, nullptr, &m_shadowDescPool) != VK_SUCCESS) {
            logError("[rhi] shadow desc pool failed"); return false;
        }
        for (uint32_t f = 0; f < kFramesInFlight; ++f) {
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_shadowDescPool; dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &m_shadowSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_shadowSet[f]) != VK_SUCCESS) {
                logError("[rhi] shadow set alloc failed"); return false;
            }
            // Persistent-mapped CSM UBO (tiny: 4 mat4 + 4 vec4).
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(CsmUBO);
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo bac{};
            bac.usage = VMA_MEMORY_USAGE_AUTO;
            bac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo ai{};
            if (x3vmaCreateBuffer(&bci, &bac, &m_csmUbo[f], &m_csmUboAlloc[f], &ai) != VK_SUCCESS) {
                logError("[rhi] csm ubo create failed"); return false;
            }
            m_csmUboMapped[f] = ai.pMappedData;
            // Zero it so a frame sampled before the first prepareFrameData sees
            // ctrl.x == 0 (the legacy single-cascade branch), never garbage.
            if (m_csmUboMapped[f]) std::memset(m_csmUboMapped[f], 0, sizeof(CsmUBO));

            // The shadow map's sampled layout is DEPTH_READ_ONLY_OPTIMAL (it's never
            // a color/general image); write the descriptor once with that layout. The
            // per-frame barrier leaves the image in exactly this layout before the
            // main pass samples it.
            VkDescriptorImageInfo dii{};
            dii.sampler = m_shadowSampler;
            dii.imageView = m_shadowView;
            dii.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            VkDescriptorBufferInfo dbi{ m_csmUbo[f], 0, sizeof(CsmUBO) };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_shadowSet[f]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &dii;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_shadowSet[f]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &dbi;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        return true;
    }

bool VulkanRenderDevice::createShadowPipeline() {
        VkShaderModule vs = loadShaderModule("shaders\\shadow.vert.spv");
        if (!vs) return false;

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT; stage.module = vs; stage.pName = "main";

        // VERTEX COMPRESSION (Lane 5): the layout comes from the ACTIVE format,
        // which is the legacy 32 B float3/float3/float2 unless --vtxfmt says
        // otherwise. The shader still declares vec3/vec3/vec2 either way — the
        // fixed-function vertex fetch does the unpack.
        VkVertexInputBindingDescription bind{};
        VkVertexInputAttributeDescription attrs[3]{};
        meshVertexInput(bind, attrs);
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;

        // Front-face cull renders back faces into the shadow map: the depth values
        // come from surfaces facing AWAY from the light, which pushes self-shadow
        // acne behind the lit geometry (a standard, robust acne mitigation). A
        // constant + slope depth bias supplements it.
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_FRONT_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 1.25f;
        rs.depthBiasSlopeFactor = 1.75f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS;

        // No color attachment in the shadow pass.
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 0; cb.pAttachments = nullptr;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        // set 0 = the object SSBO + camera UBO (shadow.vert reads both).
        // CSM: a 64-byte push constant carries the CURRENT CASCADE's light
        // viewProj, so the same pass body can be replayed once per cascade
        // without touching the per-frame UBO (which the mesh/glass/planet shaders
        // all share and whose std140 layout is mirrored in ~20 GLSL files). 64 B
        // is inside the 128 B every Vulkan implementation guarantees. With
        // r_csm 0 the pushed matrix IS the legacy computeLightViewProj() result,
        // so the rasterized depth is unchanged.
        VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, (uint32_t)sizeof(glm::mat4) };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_objSetLayout;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_shadowLayout) != VK_SUCCESS) {
            logError("[rhi] shadow pipeline layout failed"); vkDestroyShaderModule(m_dev.device, vs, nullptr); return false;
        }

        // Dynamic rendering: depth-only (no color formats).
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 0;
        prci.depthAttachmentFormat = m_shadowFormat;

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 1; gpci.pStages = &stage;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_shadowLayout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_shadowPipeline);
        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] shadow pipeline create failed"); return false; }

        // ---- ALPHA-CUTOUT shadow variant (shadow_cutout.vert + depth_cutout.frag) ----
        // The depth-only pipeline above has NO fragment stage, so an alphaMode==MASK
        // billboard (snow firs, people sprites) casts the shadow of its FULL QUAD —
        // on snow under a high sun that reads as hard black RECTANGLES around every
        // tree. This variant adds the fragment stage that replicates mesh.frag's
        // exact cutout discard, so a fir casts a fir-shaped shadow. Same fixed-
        // function state (front-face cull + the same bias) so shadow depths are
        // otherwise unchanged. Engaged PER DRAW GROUP only when the host opts in via
        // setShadowCutout(true) — every other world keeps the historical shadow
        // bit-for-bit. NON-FATAL on failure.
        if (m_objSetLayout && m_bindlessLayout) {
            VkDescriptorSetLayout cutSets[2] = { m_objSetLayout, m_bindlessLayout };
            VkPipelineLayoutCreateInfo cplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            cplci.setLayoutCount = 2; cplci.pSetLayouts = cutSets;
            // Same per-cascade matrix push constant as the plain shadow layout, at
            // the same offset/stage — so recordShadowPassBody can push ONCE per
            // cascade even when it swaps between the two pipelines mid-cascade.
            cplci.pushConstantRangeCount = 1; cplci.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &cplci, nullptr, &m_shadowCutoutLayout) == VK_SUCCESS) {
                VkShaderModule cvs = loadShaderModule("shaders\\shadow_cutout.vert.spv");
                VkShaderModule cfs = loadShaderModule("shaders\\depth_cutout.frag.spv");
                if (cvs && cfs) {
                    VkPipelineShaderStageCreateInfo cstages[2]{};
                    cstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   cstages[0].module = cvs; cstages[0].pName = "main";
                    cstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; cstages[1].module = cfs; cstages[1].pName = "main";
                    gpci.stageCount = 2; gpci.pStages = cstages;
                    gpci.layout = m_shadowCutoutLayout;
                    if (x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_shadowCutoutPipeline) != VK_SUCCESS)
                        m_shadowCutoutPipeline = VK_NULL_HANDLE;
                }
                if (cvs) vkDestroyShaderModule(m_dev.device, cvs, nullptr);
                if (cfs) vkDestroyShaderModule(m_dev.device, cfs, nullptr);
            }
            if (!m_shadowCutoutPipeline)
                logError("[rhi] shadow CUTOUT pipeline unavailable — foliage keeps full-quad shadows");
        }

        logInfo("[rhi] directional shadow pipeline ready (2048^2 depth, depth-only, PCF compare sampler)");
        return true;
    }

void VulkanRenderDevice::destroyGraphics() {
        // Mesh + texture registries (created by the app via the public API).
        //
        // LOD-CHAIN FIX (mines lane): chain member meshes ALIAS one shared vertex
        // buffer (Mesh::vboShare != 0 — see createMeshLodChain). The old loop
        // vmaDestroyBuffer'd m.vbo once PER LEVEL here, double-freeing the shared
        // buffer and segfaulting shutdown the first time a world kept live chains
        // until exit (no shipping world did before --world mines).
        // destroyMesh() already refcounts via m_vboShares; mirror that here by
        // skipping aliased vbos in the loop and freeing each share exactly once.
        for (auto& kv : m_meshes) {
            Mesh& m = kv.second;
            if (m.dynamic) {
                for (uint32_t i = 0; i < kFramesInFlight; ++i)
                    if (m.dynVbo[i]) vmaDestroyBuffer(m_alloc, m.dynVbo[i], m.dynVboAlloc[i]);
            } else if (m.vboShare != 0) {
                // shared with the chain's other levels — freed once, below
            } else if (m.vbo) {
                vmaDestroyBuffer(m_alloc, m.vbo, m.vboAlloc);
            }
            vmaDestroyBuffer(m_alloc, m.ibo, m.iboAlloc);
        }
        m_meshes.clear();
        // Free each still-registered shared chain vertex buffer exactly once.
        for (auto& kv : m_vboShares)
            if (kv.second.buf) vmaDestroyBuffer(m_alloc, kv.second.buf, kv.second.alloc);
        m_vboShares.clear();
        // Drain any still-pending deferred frees (buffers AND images/views/samplers
        // queued by destroyMesh/destroyTexture). shutdown() waited idle first, so
        // every referencing frame has retired and it is safe to free immediately.
        flushPendingFrees();
        // Free the persistent capture readback buffer (fix 1).
        if (m_captureBuf) {
            vmaDestroyBuffer(m_alloc, m_captureBuf, m_captureAlloc);
            m_captureBuf = VK_NULL_HANDLE; m_captureAlloc = nullptr; m_captureMapped = nullptr;
        }
        for (auto& kv : m_textures) destroyTextureObj(kv.second);
        m_textures.clear();
        destroyTextureObj(m_whiteTex);

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.objBuf)      { vmaDestroyBuffer(m_alloc, fr.objBuf, fr.objAlloc); fr.objBuf = VK_NULL_HANDLE; fr.objAlloc = nullptr; fr.objMapped = nullptr; }
            if (fr.camBuf)      { vmaDestroyBuffer(m_alloc, fr.camBuf, fr.camAlloc); fr.camBuf = VK_NULL_HANDLE; fr.camAlloc = nullptr; fr.camMapped = nullptr; }
            if (fr.lightBuf)    { vmaDestroyBuffer(m_alloc, fr.lightBuf, fr.lightAlloc); fr.lightBuf = VK_NULL_HANDLE; fr.lightAlloc = nullptr; fr.lightMapped = nullptr; }
            if (fr.clusterBuf)  { vmaDestroyBuffer(m_alloc, fr.clusterBuf, fr.clusterAlloc); fr.clusterBuf = VK_NULL_HANDLE; fr.clusterAlloc = nullptr; fr.clusterMapped = nullptr; }
            if (fr.indirectBuf) { vmaDestroyBuffer(m_alloc, fr.indirectBuf, fr.indirectAlloc); fr.indirectBuf = VK_NULL_HANDLE; fr.indirectAlloc = nullptr; fr.indirectMapped = nullptr; }
        }

        if (m_objPool)        { vkDestroyDescriptorPool(m_dev.device, m_objPool, nullptr); m_objPool = VK_NULL_HANDLE; }
        if (m_objSetLayout)   { vkDestroyDescriptorSetLayout(m_dev.device, m_objSetLayout, nullptr); m_objSetLayout = VK_NULL_HANDLE; }
        if (m_bindlessPool)   { vkDestroyDescriptorPool(m_dev.device, m_bindlessPool, nullptr); m_bindlessPool = VK_NULL_HANDLE; }
        if (m_bindlessLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_bindlessLayout, nullptr); m_bindlessLayout = VK_NULL_HANDLE; }

        // Shadow mapping resources (perf-stack E).
        if (m_shadowPipeline)  { vkDestroyPipeline(m_dev.device, m_shadowPipeline, nullptr); m_shadowPipeline = VK_NULL_HANDLE; }
        if (m_shadowCutoutPipeline) { vkDestroyPipeline(m_dev.device, m_shadowCutoutPipeline, nullptr); m_shadowCutoutPipeline = VK_NULL_HANDLE; }
        if (m_shadowCutoutLayout)   { vkDestroyPipelineLayout(m_dev.device, m_shadowCutoutLayout, nullptr); m_shadowCutoutLayout = VK_NULL_HANDLE; }
        if (m_shadowLayout)    { vkDestroyPipelineLayout(m_dev.device, m_shadowLayout, nullptr); m_shadowLayout = VK_NULL_HANDLE; }
        if (m_shadowDescPool)  { vkDestroyDescriptorPool(m_dev.device, m_shadowDescPool, nullptr); m_shadowDescPool = VK_NULL_HANDLE; }
        if (m_shadowSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_shadowSetLayout, nullptr); m_shadowSetLayout = VK_NULL_HANDLE; }
        if (m_shadowSampler)   { vkDestroySampler(m_dev.device, m_shadowSampler, nullptr); m_shadowSampler = VK_NULL_HANDLE; }
        if (m_shadowView)      { vkDestroyImageView(m_dev.device, m_shadowView, nullptr); m_shadowView = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kCsmCascades; ++i)
            if (m_shadowLayerView[i]) { vkDestroyImageView(m_dev.device, m_shadowLayerView[i], nullptr); m_shadowLayerView[i] = VK_NULL_HANDLE; }
        for (uint32_t f = 0; f < kFramesInFlight; ++f)
            if (m_csmUbo[f]) { vmaDestroyBuffer(m_alloc, m_csmUbo[f], m_csmUboAlloc[f]);
                               m_csmUbo[f] = VK_NULL_HANDLE; m_csmUboAlloc[f] = nullptr; m_csmUboMapped[f] = nullptr; }
        if (m_shadowImg)       { vmaDestroyImage(m_alloc, m_shadowImg, m_shadowAlloc); m_shadowImg = VK_NULL_HANDLE; m_shadowAlloc = nullptr; }

        if (m_meshPipeline)  vkDestroyPipeline(m_dev.device, m_meshPipeline, nullptr);
        if (m_meshPipelineNoSsao) vkDestroyPipeline(m_dev.device, m_meshPipelineNoSsao, nullptr);
        if (m_meshProbePipe) { vkDestroyPipeline(m_dev.device, m_meshProbePipe, nullptr); m_meshProbePipe = VK_NULL_HANDLE; }
        if (m_meshProbeLayout) { vkDestroyPipelineLayout(m_dev.device, m_meshProbeLayout, nullptr); m_meshProbeLayout = VK_NULL_HANDLE; }
        if (m_meshPipelineTransparent) vkDestroyPipeline(m_dev.device, m_meshPipelineTransparent, nullptr);
        if (m_meshPipelineRt)            vkDestroyPipeline(m_dev.device, m_meshPipelineRt, nullptr);
        if (m_meshPipelineNoSsaoRt)      vkDestroyPipeline(m_dev.device, m_meshPipelineNoSsaoRt, nullptr);
        if (m_meshPipelineTransparentRt) vkDestroyPipeline(m_dev.device, m_meshPipelineTransparentRt, nullptr);
        m_meshPipelineRt = VK_NULL_HANDLE; m_meshPipelineNoSsaoRt = VK_NULL_HANDLE;
        m_meshPipelineTransparentRt = VK_NULL_HANDLE;
        for (uint32_t pt = 0; pt < (uint32_t)PT_Count; ++pt) {
            if (m_planetPipelines[pt]) { vkDestroyPipeline(m_dev.device, m_planetPipelines[pt], nullptr); m_planetPipelines[pt] = VK_NULL_HANDLE; }
        }
        if (m_planetPipelineLayout) vkDestroyPipelineLayout(m_dev.device, m_planetPipelineLayout, nullptr);
        if (m_glassPipeline) vkDestroyPipeline(m_dev.device, m_glassPipeline, nullptr);
        if (m_glassLayout)   vkDestroyPipelineLayout(m_dev.device, m_glassLayout, nullptr);
        if (m_glassSetLayout) vkDestroyDescriptorSetLayout(m_dev.device, m_glassSetLayout, nullptr);
        if (m_meshLayout)    vkDestroyPipelineLayout(m_dev.device, m_meshLayout, nullptr);
        // mesh.frag set 4 layout (IBL): created in createGraphics, baked into m_meshLayout.
        if (m_iblMeshSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_iblMeshSetLayout, nullptr); m_iblMeshSetLayout = VK_NULL_HANDLE; }
        if (m_uploadFence)   vkDestroyFence(m_dev.device, m_uploadFence, nullptr);
        for (int s = 0; s < 2; ++s) {   // boot-time upload-batch fences (double-buffered)
            if (m_batchFences[s]) { vkDestroyFence(m_dev.device, m_batchFences[s], nullptr); m_batchFences[s] = VK_NULL_HANDLE; }
            m_batchCmds[s] = VK_NULL_HANDLE;   // freed with m_uploadPool
        }
        if (m_uploadPool)    vkDestroyCommandPool(m_dev.device, m_uploadPool, nullptr);
        m_meshPipeline = VK_NULL_HANDLE; m_meshPipelineNoSsao = VK_NULL_HANDLE;
        m_meshPipelineTransparent = VK_NULL_HANDLE; m_meshLayout = VK_NULL_HANDLE;
        m_planetPipelineLayout = VK_NULL_HANDLE;
        m_glassPipeline = VK_NULL_HANDLE;
        m_glassLayout = VK_NULL_HANDLE; m_glassSetLayout = VK_NULL_HANDLE;
        m_uploadFence = VK_NULL_HANDLE; m_uploadPool = VK_NULL_HANDLE;
    }


void VulkanRenderDevice::iblFaceBasis(int face, glm::vec3& fwd, glm::vec3& right, glm::vec3& up) {
        switch (face) {
            case 0: fwd={ 1, 0, 0}; right={ 0, 0,-1}; up={0,-1,0}; break; // +X
            case 1: fwd={-1, 0, 0}; right={ 0, 0, 1}; up={0,-1,0}; break; // -X
            case 2: fwd={ 0, 1, 0}; right={ 1, 0, 0}; up={0, 0,1}; break; // +Y
            case 3: fwd={ 0,-1, 0}; right={ 1, 0, 0}; up={0, 0,-1}; break;// -Y
            case 4: fwd={ 0, 0, 1}; right={ 1, 0, 0}; up={0,-1,0}; break; // +Z
            default:fwd={ 0, 0,-1}; right={-1, 0, 0}; up={0,-1,0}; break; // -Z
        }
    }

bool VulkanRenderDevice::createIblCube(uint32_t size, uint32_t mipLevels, VkImageUsageFlags usage,
                   VkImage& outImg, VmaAllocation& outAlloc,
                   VkImageView& outCubeView, VkImageView* outFaceViews, uint32_t rtMip) {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = kIblCubeFormat;
        ici.extent = { size, size, 1 }; ici.mipLevels = mipLevels; ici.arrayLayers = 6;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&ici, &aci, &outImg, &outAlloc, nullptr) != VK_SUCCESS) return false;
        VkImageViewCreateInfo cv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        cv.image = outImg; cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE; cv.format = kIblCubeFormat;
        cv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 6 };
        if (vkCreateImageView(m_dev.device, &cv, nullptr, &outCubeView) != VK_SUCCESS) return false;
        if (outFaceViews) {
            for (uint32_t f = 0; f < 6; ++f) {
                VkImageViewCreateInfo fv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                fv.image = outImg; fv.viewType = VK_IMAGE_VIEW_TYPE_2D; fv.format = kIblCubeFormat;
                fv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, rtMip, 1, f, 1 };
                if (vkCreateImageView(m_dev.device, &fv, nullptr, &outFaceViews[f]) != VK_SUCCESS) return false;
            }
        }
        return true;
    }

bool VulkanRenderDevice::createIbl() {
        m_iblEnvMips = (uint32_t)std::floor(std::log2((float)kIblEnvSize)) + 1u;

        // ---- Images + views ----
        const VkImageUsageFlags cubeUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!createIblCube(kIblEnvSize, m_iblEnvMips, cubeUsage,
                           m_iblEnvImg, m_iblEnvAlloc, m_iblEnvCubeView, m_iblEnvFaceView, 0)) {
            logError("[rhi] IBL env cube create failed"); return false;
        }
        if (!createIblCube(kIblIrradSize, 1, cubeUsage,
                           m_iblIrradImg, m_iblIrradAlloc, m_iblIrradCubeView, m_iblIrradFaceView, 0)) {
            logError("[rhi] IBL irradiance cube create failed"); return false;
        }
        // Prefilter: kIblPrefilterMips mips; one RT view per (mip,face).
        {
            VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            ici.imageType = VK_IMAGE_TYPE_2D; ici.format = kIblCubeFormat;
            ici.extent = { kIblPrefilterSize, kIblPrefilterSize, 1 };
            ici.mipLevels = kIblPrefilterMips; ici.arrayLayers = 6;
            ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = cubeUsage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            if (x3vmaCreateImage(&ici, &aci, &m_iblPrefImg, &m_iblPrefAlloc, nullptr) != VK_SUCCESS) {
                logError("[rhi] IBL prefilter cube create failed"); return false;
            }
            VkImageViewCreateInfo cv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            cv.image = m_iblPrefImg; cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE; cv.format = kIblCubeFormat;
            cv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, kIblPrefilterMips, 0, 6 };
            if (vkCreateImageView(m_dev.device, &cv, nullptr, &m_iblPrefCubeView) != VK_SUCCESS) return false;
            for (uint32_t mip = 0; mip < kIblPrefilterMips; ++mip)
                for (uint32_t f = 0; f < 6; ++f) {
                    VkImageViewCreateInfo fv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                    fv.image = m_iblPrefImg; fv.viewType = VK_IMAGE_VIEW_TYPE_2D; fv.format = kIblCubeFormat;
                    fv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, f, 1 };
                    if (vkCreateImageView(m_dev.device, &fv, nullptr, &m_iblPrefFaceView[mip][f]) != VK_SUCCESS) return false;
                }
        }
        // BRDF LUT (2D RG16F).
        if (!createColorTarget(kIblBrdfFormat, kIblBrdfSize, kIblBrdfSize,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               m_iblBrdfImg, m_iblBrdfAlloc, m_iblBrdfView)) {
            logError("[rhi] IBL BRDF LUT create failed"); return false;
        }

        // ---- Samplers ----
        {
            VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = VK_LOD_CLAMP_NONE;
            if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_iblCubeSampler) != VK_SUCCESS) return false;
            VkSamplerCreateInfo bs = sci; bs.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; bs.maxLod = 0.0f;
            if (vkCreateSampler(m_dev.device, &bs, nullptr, &m_iblBrdfSampler) != VK_SUCCESS) return false;
        }

        // ---- Descriptor set layouts ----
        // set0 for env capture: one UBO (IblSkyUBO).
        {
            VkDescriptorSetLayoutBinding b{}; b.binding = 0; b.descriptorCount = 1;
            b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 1; ci.pBindings = &b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_iblSkyUboSetLayout) != VK_SUCCESS) return false;
        }
        // set0 for convolve passes: one cube sampler.
        {
            VkDescriptorSetLayoutBinding b{}; b.binding = 0; b.descriptorCount = 1;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 1; ci.pBindings = &b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_iblCubeSetLayout) != VK_SUCCESS) return false;
        }
        // (mesh.frag set 4 layout m_iblMeshSetLayout was created in createGraphics so
        //  the mesh pipeline layout could include it; we only ALLOCATE its set here.)

        // ---- Sky UBO buffer (host-mapped, written before each bake) ----
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(IblSkyUBO); bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&bci, &aci, &m_iblSkyUboBuf, &m_iblSkyUboAlloc, &info) != VK_SUCCESS) return false;
            m_iblSkyUboMapped = info.pMappedData;
        }

        // ---- Descriptor pool + sets (bake-side: sky UBO set + env-cube set) ----
        {
            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1;
            sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[1].descriptorCount = 2;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_iblBakePool) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_iblBakePool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_iblSkyUboSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_iblSkyUboSet) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a1.descriptorPool = m_iblBakePool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_iblCubeSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_iblEnvCubeSet) != VK_SUCCESS) return false;
            // Write the sky UBO into the env set; the env cube into the convolve set.
            VkDescriptorBufferInfo dbi{ m_iblSkyUboBuf, 0, sizeof(IblSkyUBO) };
            VkDescriptorImageInfo  dci{ m_iblCubeSampler, m_iblEnvCubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_iblSkyUboSet;
            w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &dbi;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = m_iblEnvCubeSet;
            w[1].dstBinding = 0; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &dci;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }

        // ---- mesh.frag set 4 pool + set (written after the first bake) ----
        {
            VkDescriptorPoolSize sz{}; sz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sz.descriptorCount = 3;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &sz;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_iblMeshPool) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_iblMeshPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_iblMeshSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_iblMeshSet) != VK_SUCCESS) return false;
        }

        // ---- Pipelines (fullscreen-triangle vertex shader) ----
        // Env capture: set0 = sky UBO, push = IblFacePush.
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(IblFacePush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_iblSkyUboSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_iblEnvLayout) != VK_SUCCESS) return false;
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ibl_env.frag.spv",
                                          m_iblEnvLayout, kIblCubeFormat, false, m_iblEnvPipe)) return false;
        }
        // Convolve (irradiance + prefilter share this layout): set0 = cube sampler, push = IblFacePush.
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(IblFacePush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_iblCubeSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_iblCubeLayout) != VK_SUCCESS) return false;
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ibl_irradiance.frag.spv",
                                          m_iblCubeLayout, kIblCubeFormat, false, m_iblIrradPipe)) return false;
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ibl_prefilter.frag.spv",
                                          m_iblCubeLayout, kIblCubeFormat, false, m_iblPrefPipe)) return false;
        }
        // BRDF LUT: no sets, no push.
        {
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_iblBrdfLayout) != VK_SUCCESS) return false;
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ibl_brdf_lut.frag.spv",
                                          m_iblBrdfLayout, kIblBrdfFormat, false, m_iblBrdfPipe)) return false;
        }

        // ---- Initialize all sampled images to SHADER_READ_ONLY (contents undefined)
        // so mesh.frag set 4 is bindable from the very first draw even if a bake has
        // not run yet. A plain UNDEFINED->SHADER_READ layout transition is enough
        // (no clear / no TRANSFER_DST needed) since the gate flag (ssao.ibl.x) keeps
        // the shader from sampling these until a real bake completes; the first-frame
        // bake overwrites the contents regardless.
        bool clr = oneTimeSubmit([&](VkCommandBuffer cmd){
            iblBarrier(cmd, m_iblEnvImg, 0, m_iblEnvMips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            iblBarrier(cmd, m_iblIrradImg, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            iblBarrier(cmd, m_iblPrefImg, 0, kIblPrefilterMips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            iblBarrierTex2D(cmd, m_iblBrdfImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!clr) { logError("[rhi] IBL initial layout transition failed"); return false; }
        // Write mesh.frag set 4 now (points at the cleared cubes/LUT); regenIblFromSky
        // re-points it after each bake (identical views, so this is just safety).
        {
            VkDescriptorImageInfo di0{ m_iblCubeSampler, m_iblIrradCubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo di1{ m_iblCubeSampler, m_iblPrefCubeView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo di2{ m_iblBrdfSampler, m_iblBrdfView,      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w[3]{};
            for (int i = 0; i < 3; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = m_iblMeshSet;
                w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; }
            w[0].pImageInfo = &di0; w[1].pImageInfo = &di1; w[2].pImageInfo = &di2;
            vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
        }

        // Reflection-probe depth target (env-face sized) for the optional scene bake.
        {
            VkImageCreateInfo di{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            di.imageType = VK_IMAGE_TYPE_2D; di.format = m_depthFormat;
            di.extent = { kIblEnvSize, kIblEnvSize, 1 }; di.mipLevels = 1; di.arrayLayers = 1;
            di.samples = VK_SAMPLE_COUNT_1_BIT; di.tiling = VK_IMAGE_TILING_OPTIMAL;
            di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            VmaAllocationCreateInfo da{}; da.usage = VMA_MEMORY_USAGE_AUTO;
            da.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            if (x3vmaCreateImage(&di, &da, &m_probeDepthImg, &m_probeDepthAlloc, nullptr) == VK_SUCCESS) {
                VkImageViewCreateInfo dv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                dv.image = m_probeDepthImg; dv.viewType = VK_IMAGE_VIEW_TYPE_2D; dv.format = m_depthFormat;
                dv.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                vkCreateImageView(m_dev.device, &dv, nullptr, &m_probeDepthView);
            } else {
                logError("[rhi] probe depth image create failed (reflection probe disabled)");
            }
        }

        m_iblReady = true;
        logInfo("[rhi] IBL ready (env 256 + irradiance 32 + prefilter 128/5mip + BRDF LUT 256, split-sum)");
        return true;
    }

void VulkanRenderDevice::destroyIbl() {
        auto killView = [&](VkImageView& v){ if (v) { vkDestroyImageView(m_dev.device, v, nullptr); v = VK_NULL_HANDLE; } };
        auto killImg  = [&](VkImage& i, VmaAllocation& a){ if (i) { vmaDestroyImage(m_alloc, i, a); i = VK_NULL_HANDLE; a = nullptr; } };
        auto killPipe = [&](VkPipeline& p){ if (p) { vkDestroyPipeline(m_dev.device, p, nullptr); p = VK_NULL_HANDLE; } };
        auto killPl   = [&](VkPipelineLayout& p){ if (p) { vkDestroyPipelineLayout(m_dev.device, p, nullptr); p = VK_NULL_HANDLE; } };
        auto killSl   = [&](VkDescriptorSetLayout& s){ if (s) { vkDestroyDescriptorSetLayout(m_dev.device, s, nullptr); s = VK_NULL_HANDLE; } };
        killPipe(m_iblEnvPipe); killPipe(m_iblIrradPipe); killPipe(m_iblPrefPipe); killPipe(m_iblBrdfPipe);
        killPl(m_iblEnvLayout); killPl(m_iblCubeLayout); killPl(m_iblBrdfLayout);
        if (m_iblMeshPool) { vkDestroyDescriptorPool(m_dev.device, m_iblMeshPool, nullptr); m_iblMeshPool = VK_NULL_HANDLE; }
        if (m_iblBakePool) { vkDestroyDescriptorPool(m_dev.device, m_iblBakePool, nullptr); m_iblBakePool = VK_NULL_HANDLE; }
        // m_iblMeshSetLayout is owned by createGraphics/destroyGraphics (it's baked
        // into the mesh pipeline layout), so it is NOT destroyed here.
        killSl(m_iblCubeSetLayout); killSl(m_iblSkyUboSetLayout);
        if (m_iblSkyUboBuf) { vmaDestroyBuffer(m_alloc, m_iblSkyUboBuf, m_iblSkyUboAlloc); m_iblSkyUboBuf = VK_NULL_HANDLE; m_iblSkyUboAlloc = nullptr; m_iblSkyUboMapped = nullptr; }
        if (m_iblCubeSampler) { vkDestroySampler(m_dev.device, m_iblCubeSampler, nullptr); m_iblCubeSampler = VK_NULL_HANDLE; }
        if (m_iblBrdfSampler) { vkDestroySampler(m_dev.device, m_iblBrdfSampler, nullptr); m_iblBrdfSampler = VK_NULL_HANDLE; }
        for (int f = 0; f < 6; ++f) { killView(m_iblEnvFaceView[f]); killView(m_iblIrradFaceView[f]); }
        for (uint32_t m = 0; m < kIblPrefilterMips; ++m) for (int f = 0; f < 6; ++f) killView(m_iblPrefFaceView[m][f]);
        killView(m_iblEnvCubeView); killView(m_iblIrradCubeView); killView(m_iblPrefCubeView); killView(m_iblBrdfView);
        killImg(m_iblEnvImg, m_iblEnvAlloc); killImg(m_iblIrradImg, m_iblIrradAlloc);
        killImg(m_iblPrefImg, m_iblPrefAlloc); killImg(m_iblBrdfImg, m_iblBrdfAlloc);
        killView(m_probeDepthView); killImg(m_probeDepthImg, m_probeDepthAlloc);  // reflection-probe depth
        m_iblReady = false; m_iblBaked = false;
    }

bool VulkanRenderDevice::createSky() {
        // Set-0 layout: a single UBO (the SkyUBO) read by the fragment stage.
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorCount = 1;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 1; slci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_skySetLayout) != VK_SUCCESS) {
            logError("[rhi] sky set layout failed"); return false;
        }

        // One UBO descriptor per frame-in-flight.
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (x3CreateDescriptorPool(&pci, nullptr, &m_skyPool) != VK_SUCCESS) {
            logError("[rhi] sky desc pool failed"); return false;
        }

        // Per-frame UBO buffer (persistently mapped) + its descriptor set.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(SkyUBO);
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&bci, &aci, &fr.skyBuf, &fr.skyAlloc, &info) != VK_SUCCESS) {
                logError("[rhi] sky UBO create failed"); return false;
            }
            fr.skyMapped = info.pMappedData;

            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_skyPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_skySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &fr.skySet) != VK_SUCCESS) {
                logError("[rhi] sky set alloc failed"); return false;
            }
            VkDescriptorBufferInfo dbi{ fr.skyBuf, 0, sizeof(SkyUBO) };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = fr.skySet; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &dbi;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }

        // Shaders.
        VkShaderModule vs = loadShaderModule("shaders\\sky.vert.spv");
        VkShaderModule fs = loadShaderModule("shaders\\sky.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        // No vertex input: the vertex shader builds the triangle from gl_VertexIndex.
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Far-depth fill: the sky's gl_Position.z == w (depth 1.0). LESS_OR_EQUAL
        // passes only where the cleared depth (1.0) still stands (no nearer
        // geometry). depthWrite OFF leaves the depth buffer untouched for later use.
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_FALSE; // opaque background fill
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_skySetLayout; // no push constants
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_skyLayout) != VK_SUCCESS) {
            logError("[rhi] sky pipeline layout failed"); return false;
        }

        // HDR pipeline: the sky also renders into the linear HDR scene target.
        const VkFormat hdrFmt = kHdrFormat;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &hdrFmt;
        prci.depthAttachmentFormat = m_depthFormat;  // pass has a depth attachment

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_skyLayout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_skyPipeline);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] sky pipeline create failed"); return false; }

        logInfo("[rhi] analytic sky pipeline ready (full-screen tri, far-depth, depth-test no-write)");
        return true;
    }

void VulkanRenderDevice::destroySky() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.skyBuf) { vmaDestroyBuffer(m_alloc, fr.skyBuf, fr.skyAlloc);
                             fr.skyBuf = VK_NULL_HANDLE; fr.skyAlloc = nullptr; fr.skyMapped = nullptr; }
        }
        if (m_skyPipeline)  { vkDestroyPipeline(m_dev.device, m_skyPipeline, nullptr); m_skyPipeline = VK_NULL_HANDLE; }
        if (m_skyLayout)    { vkDestroyPipelineLayout(m_dev.device, m_skyLayout, nullptr); m_skyLayout = VK_NULL_HANDLE; }
        if (m_skyPool)      { vkDestroyDescriptorPool(m_dev.device, m_skyPool, nullptr); m_skyPool = VK_NULL_HANDLE; }
        if (m_skySetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_skySetLayout, nullptr); m_skySetLayout = VK_NULL_HANDLE; }
    }

bool VulkanRenderDevice::createWater() {
        // --- Unit-patch grid mesh (device-local; built once via staging). ---
        const uint32_t dim = kWaterGridDim;            // verts per edge
        std::vector<glm::vec2> verts; verts.reserve((size_t)dim * dim);
        for (uint32_t z = 0; z < dim; ++z) {
            for (uint32_t x = 0; x < dim; ++x) {
                float fx = (float)x / (float)(dim - 1) * 2.0f - 1.0f; // [-1,1]
                float fz = (float)z / (float)(dim - 1) * 2.0f - 1.0f;
                verts.emplace_back(fx, fz);
            }
        }
        std::vector<uint32_t> idx; idx.reserve((size_t)(dim - 1) * (dim - 1) * 6);
        for (uint32_t z = 0; z < dim - 1; ++z) {
            for (uint32_t x = 0; x < dim - 1; ++x) {
                uint32_t i0 = z * dim + x;
                uint32_t i1 = z * dim + (x + 1);
                uint32_t i2 = (z + 1) * dim + x;
                uint32_t i3 = (z + 1) * dim + (x + 1);
                // CCW from above (+Y); winding matches the device's front face.
                idx.push_back(i0); idx.push_back(i2); idx.push_back(i1);
                idx.push_back(i1); idx.push_back(i2); idx.push_back(i3);
            }
        }
        m_waterIndexCount = (uint32_t)idx.size();
        if (!createDeviceLocalBuffer(verts.data(), verts.size() * sizeof(glm::vec2),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_waterVbo, m_waterVboAlloc)) {
            logError("[rhi] water vbo create failed"); return false;
        }
        if (!createDeviceLocalBuffer(idx.data(), idx.size() * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_waterIbo, m_waterIboAlloc)) {
            logError("[rhi] water ibo create failed"); return false;
        }

        // --- Scene-depth sampler (LINEAR, clamp): samples the depth buffer as data
        // for the depth-based water color. ---
        VkSamplerCreateInfo dsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        dsci.magFilter = VK_FILTER_LINEAR; dsci.minFilter = VK_FILTER_LINEAR;
        dsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        dsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &dsci, nullptr, &m_waterDepthSampler) != VK_SUCCESS) {
            logError("[rhi] water depth sampler failed"); return false;
        }

        // --- Set-0 layout: WaterUBO (b0, VS+FS) + scene-depth sampler (b1, FS). ---
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorCount = 1;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorCount = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 2; slci.pBindings = b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_waterSetLayout) != VK_SUCCESS) {
            logError("[rhi] water set layout failed"); return false;
        }

        // --- Descriptor pool: UBO + sampler per frame-in-flight. ---
        VkDescriptorPoolSize ps[2]{
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight } };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        if (x3CreateDescriptorPool(&pci, nullptr, &m_waterPool) != VK_SUCCESS) {
            logError("[rhi] water desc pool failed"); return false;
        }

        // --- Per-frame UBO + descriptor set (depth binding written later). ---
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(WaterUBO);
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&bci, &aci, &m_waterUboBuf[i], &m_waterUboAlloc[i], &info) != VK_SUCCESS) {
                logError("[rhi] water UBO create failed"); return false;
            }
            m_waterUboMapped[i] = info.pMappedData;

            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_waterPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_waterSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_waterSet[i]) != VK_SUCCESS) {
                logError("[rhi] water set alloc failed"); return false;
            }
            VkDescriptorBufferInfo dbi{ m_waterUboBuf[i], 0, sizeof(WaterUBO) };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_waterSet[i]; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &dbi;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
        writeWaterDescriptors();   // wire the scene-depth binding (depth view)

        // --- Pipeline: vec2 grid vertex; depth-test LESS_OR_EQUAL, no write. ---
        VkShaderModule vs = loadShaderModule("shaders\\water.vert.spv");
        VkShaderModule fs = loadShaderModule("shaders\\water.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkVertexInputBindingDescription vib{ 0, sizeof(glm::vec2), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription via{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 1; vin.pVertexAttributeDescriptions = &via;

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; // sea seen from both sides
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Test against the opaque depth (so terrain in front of the water occludes
        // it) but DON'T write — the depth buffer is also sampled for the depth color
        // and post passes expect it unchanged. LESS_OR_EQUAL is robust at the seam.
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_FALSE; // opaque water (depth-color carries the look)
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_waterSetLayout;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_waterLayout) != VK_SUCCESS) {
            logError("[rhi] water pipeline layout failed"); return false;
        }

        const VkFormat hdrFmt = kHdrFormat;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &hdrFmt;
        prci.depthAttachmentFormat = m_depthFormat;

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_waterLayout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_waterPipeline);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] water pipeline create failed"); return false; }

        logInfo("[rhi] water pipeline ready (Gerstner grid, sky-reflection + depth-refraction + sun glint)");
        return true;
    }

void VulkanRenderDevice::writeWaterDescriptors() {
        if (!m_depthView) return;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_waterSet[i]) continue;
            VkDescriptorImageInfo di{ m_waterDepthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_waterSet[i]; w.dstBinding = 1; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &di;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
    }

void VulkanRenderDevice::destroyWater() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_waterUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_waterUboBuf[i], m_waterUboAlloc[i]);
                                    m_waterUboBuf[i] = VK_NULL_HANDLE; m_waterUboAlloc[i] = nullptr; m_waterUboMapped[i] = nullptr; }
        }
        if (m_waterVbo) { vmaDestroyBuffer(m_alloc, m_waterVbo, m_waterVboAlloc); m_waterVbo = VK_NULL_HANDLE; m_waterVboAlloc = nullptr; }
        if (m_waterIbo) { vmaDestroyBuffer(m_alloc, m_waterIbo, m_waterIboAlloc); m_waterIbo = VK_NULL_HANDLE; m_waterIboAlloc = nullptr; }
        if (m_waterPipeline)  { vkDestroyPipeline(m_dev.device, m_waterPipeline, nullptr); m_waterPipeline = VK_NULL_HANDLE; }
        if (m_waterLayout)    { vkDestroyPipelineLayout(m_dev.device, m_waterLayout, nullptr); m_waterLayout = VK_NULL_HANDLE; }
        if (m_waterPool)      { vkDestroyDescriptorPool(m_dev.device, m_waterPool, nullptr); m_waterPool = VK_NULL_HANDLE; }
        if (m_waterSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_waterSetLayout, nullptr); m_waterSetLayout = VK_NULL_HANDLE; }
        if (m_waterDepthSampler) { vkDestroySampler(m_dev.device, m_waterDepthSampler, nullptr); m_waterDepthSampler = VK_NULL_HANDLE; }
    }

bool VulkanRenderDevice::createParticles() {
        // Reserve the CPU staging vectors ONCE so per-frame submitParticles/Decals
        // appends never reallocate (the bounded "no per-frame heap alloc" promise).
        m_partAdd.reserve(kMaxParticles);
        m_partAlpha.reserve(kMaxParticles);
        m_decals.reserve(kMaxDecals);

        // --- Shared unit quad: 4 corners in [-0.5,0.5], drawn as a triangle strip. ---
        const glm::vec2 quad[4] = {
            { -0.5f, -0.5f }, {  0.5f, -0.5f }, { -0.5f,  0.5f }, {  0.5f,  0.5f } };
        if (!createDeviceLocalBuffer(quad, sizeof(quad),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_partQuadVbo, m_partQuadAlloc)) {
            logError("[rhi] particle quad vbo create failed"); return false;
        }

        // --- Scene-depth sampler (LINEAR clamp) for the soft-particle fade. ---
        VkSamplerCreateInfo dsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        dsci.magFilter = VK_FILTER_LINEAR; dsci.minFilter = VK_FILTER_LINEAR;
        dsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        dsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &dsci, nullptr, &m_partDepthSampler) != VK_SUCCESS) {
            logError("[rhi] particle depth sampler failed"); return false;
        }

        // --- Set-0 layouts. Particle: UBO(b0,VS+FS) + scene-depth(b1,FS). Decal: UBO(b0). ---
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.bindingCount = 2; slci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_partSetLayout) != VK_SUCCESS) {
                logError("[rhi] particle set layout failed"); return false;
            }
            VkDescriptorSetLayoutBinding db{};
            db.binding = 0; db.descriptorCount = 1;
            db.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            db.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.bindingCount = 1; dslci.pBindings = &db;
            if (vkCreateDescriptorSetLayout(m_dev.device, &dslci, nullptr, &m_decalSetLayout) != VK_SUCCESS) {
                logError("[rhi] decal set layout failed"); return false;
            }
        }

        // --- Descriptor pool: per frame-in-flight, 2 UBO sets + 1 sampler. ---
        VkDescriptorPoolSize ps[2]{
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight * 2 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight } };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight * 2; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        if (x3CreateDescriptorPool(&pci, nullptr, &m_partPool) != VK_SUCCESS) {
            logError("[rhi] particle desc pool failed"); return false;
        }

        // --- Per-frame instance rings + UBOs + descriptor sets. ---
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto makeMapped = [&](VkDeviceSize bytes, VkBufferUsageFlags usage,
                                  VkBuffer& buf, VmaAllocation& alloc, void*& mapped) -> bool {
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = bytes; bci.usage = usage;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_AUTO;
                aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo info{};
                if (x3vmaCreateBuffer(&bci, &aci, &buf, &alloc, &info) != VK_SUCCESS) return false;
                mapped = info.pMappedData;
                return true;
            };
            if (!makeMapped(sizeof(ParticleGpu) * kMaxParticles, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            m_partInstAddBuf[i], m_partInstAddAlloc[i], m_partInstAddMapped[i])) {
                logError("[rhi] particle add ring create failed"); return false; }
            if (!makeMapped(sizeof(ParticleGpu) * kMaxParticles, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            m_partInstAlphaBuf[i], m_partInstAlphaAlloc[i], m_partInstAlphaMapped[i])) {
                logError("[rhi] particle alpha ring create failed"); return false; }
            if (!makeMapped(sizeof(DecalGpu) * kMaxDecals, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            m_decalInstBuf[i], m_decalInstAlloc[i], m_decalInstMapped[i])) {
                logError("[rhi] decal ring create failed"); return false; }
            if (!makeMapped(sizeof(ParticleUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            m_partUboBuf[i], m_partUboAlloc[i], m_partUboMapped[i])) {
                logError("[rhi] particle UBO create failed"); return false; }
            if (!makeMapped(sizeof(DecalUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            m_decalUboBuf[i], m_decalUboAlloc[i], m_decalUboMapped[i])) {
                logError("[rhi] decal UBO create failed"); return false; }

            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_partPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_partSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_partSet[i]) != VK_SUCCESS) {
                logError("[rhi] particle set alloc failed"); return false; }
            VkDescriptorSetAllocateInfo dsai2{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai2.descriptorPool = m_partPool; dsai2.descriptorSetCount = 1; dsai2.pSetLayouts = &m_decalSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &dsai2, &m_decalSet[i]) != VK_SUCCESS) {
                logError("[rhi] decal set alloc failed"); return false; }

            // Bind the UBO (b0) of each set now; the depth (b1) is wired below/on resize.
            VkDescriptorBufferInfo pbi{ m_partUboBuf[i], 0, sizeof(ParticleUBO) };
            VkDescriptorBufferInfo dbi{ m_decalUboBuf[i], 0, sizeof(DecalUBO) };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_partSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &pbi;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_decalSet[i]; w[1].dstBinding = 0; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &dbi;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        writeParticleDescriptors();   // wire the scene-depth binding (depth view)

        // --- Pipelines. Shared: triangle strip, depth-test LESS_OR_EQUAL no write,
        // vertex input = quad corner (binding 0, per-vertex) + instance (binding 1). ---
        const VkFormat hdrFmt = kHdrFormat;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &hdrFmt;
        prci.depthAttachmentFormat = m_depthFormat;

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        // Pipeline layouts.
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_partSetLayout;
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_partLayout) != VK_SUCCESS) {
            logError("[rhi] particle pipeline layout failed"); return false; }
        VkPipelineLayoutCreateInfo dplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        dplci.setLayoutCount = 1; dplci.pSetLayouts = &m_decalSetLayout;
        if (vkCreatePipelineLayout(m_dev.device, &dplci, nullptr, &m_decalLayout) != VK_SUCCESS) {
            logError("[rhi] decal pipeline layout failed"); return false; }

        // ---- Particle pipelines (additive + alpha): quad corner + ParticleGpu. ----
        {
            VkShaderModule vs = loadShaderModule("shaders\\particle.vert.spv");
            VkShaderModule fs = loadShaderModule("shaders\\particle.frag.spv");
            if (!vs || !fs) return false;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

            VkVertexInputBindingDescription vibs[2]{
                { 0, sizeof(glm::vec2),   VK_VERTEX_INPUT_RATE_VERTEX   },   // quad corner
                { 1, sizeof(ParticleGpu), VK_VERTEX_INPUT_RATE_INSTANCE } }; // instance
            VkVertexInputAttributeDescription vias[3]{
                { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },                              // inCorner
                { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleGpu, posSize) },// inPosSize
                { 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleGpu, color)   }};// inColor
            VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vin.vertexBindingDescriptionCount = 2; vin.pVertexBindingDescriptions = vibs;
            vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = vias;

            // Additive blend (sparks/fire/muzzle): src*srcA + dst (glow, feeds bloom).
            VkPipelineColorBlendAttachmentState addBlend{};
            addBlend.blendEnable = VK_TRUE;
            addBlend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            addBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            addBlend.colorBlendOp = VK_BLEND_OP_ADD;
            addBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            addBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            addBlend.alphaBlendOp = VK_BLEND_OP_ADD;
            addBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
            // Alpha blend (smoke/dust/blood): standard over.
            VkPipelineColorBlendAttachmentState aBlend = addBlend;
            aBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

            auto buildPart = [&](const VkPipelineColorBlendAttachmentState& cba, VkPipeline& out) -> bool {
                VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
                cb.attachmentCount = 1; cb.pAttachments = &cba;
                VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
                gpci.pNext = &prci; gpci.stageCount = 2; gpci.pStages = stages;
                gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
                gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
                gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
                gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_partLayout;
                return x3CreateGraphicsPipelines(1, &gpci, nullptr, &out) == VK_SUCCESS;
            };
            bool ok = buildPart(addBlend, m_partAddPipeline) && buildPart(aBlend, m_partAlphaPipeline);
            vkDestroyShaderModule(m_dev.device, vs, nullptr);
            vkDestroyShaderModule(m_dev.device, fs, nullptr);
            if (!ok) { logError("[rhi] particle pipeline create failed"); return false; }
        }

        // ---- Decal pipeline (alpha): quad corner + DecalGpu. ----
        {
            VkShaderModule vs = loadShaderModule("shaders\\decal.vert.spv");
            VkShaderModule fs = loadShaderModule("shaders\\decal.frag.spv");
            if (!vs || !fs) return false;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

            VkVertexInputBindingDescription vibs[2]{
                { 0, sizeof(glm::vec2), VK_VERTEX_INPUT_RATE_VERTEX   },
                { 1, sizeof(DecalGpu),  VK_VERTEX_INPUT_RATE_INSTANCE } };
            VkVertexInputAttributeDescription vias[4]{
                { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },
                { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DecalGpu, centerSize)  },
                { 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DecalGpu, normalAngle) },
                { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DecalGpu, color)       }};
            VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vin.vertexBindingDescriptionCount = 2; vin.pVertexBindingDescriptions = vibs;
            vin.vertexAttributeDescriptionCount = 4; vin.pVertexAttributeDescriptions = vias;

            VkPipelineColorBlendAttachmentState cba{};
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 1; cb.pAttachments = &cba;

            VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            gpci.pNext = &prci; gpci.stageCount = 2; gpci.pStages = stages;
            gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
            gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
            gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
            gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_decalLayout;
            VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_decalPipeline);
            vkDestroyShaderModule(m_dev.device, vs, nullptr);
            vkDestroyShaderModule(m_dev.device, fs, nullptr);
            if (pr != VK_SUCCESS) { logError("[rhi] decal pipeline create failed"); return false; }
        }

        logInfo("[rhi] particle + decal pipelines ready (instanced billboards, additive/alpha, soft vs scene depth)");
        return true;
    }

void VulkanRenderDevice::writeParticleDescriptors() {
        if (!m_depthView) return;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_partSet[i]) continue;
            VkDescriptorImageInfo di{ m_partDepthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_partSet[i]; w.dstBinding = 1; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &di;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
    }

void VulkanRenderDevice::destroyParticles() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto kill = [&](VkBuffer& b, VmaAllocation& a, void*& m) {
                if (b) { vmaDestroyBuffer(m_alloc, b, a); b = VK_NULL_HANDLE; a = nullptr; m = nullptr; } };
            kill(m_partInstAddBuf[i],   m_partInstAddAlloc[i],   m_partInstAddMapped[i]);
            kill(m_partInstAlphaBuf[i], m_partInstAlphaAlloc[i], m_partInstAlphaMapped[i]);
            kill(m_decalInstBuf[i],     m_decalInstAlloc[i],     m_decalInstMapped[i]);
            kill(m_partUboBuf[i],       m_partUboAlloc[i],       m_partUboMapped[i]);
            kill(m_decalUboBuf[i],      m_decalUboAlloc[i],      m_decalUboMapped[i]);
        }
        if (m_partQuadVbo) { vmaDestroyBuffer(m_alloc, m_partQuadVbo, m_partQuadAlloc); m_partQuadVbo = VK_NULL_HANDLE; m_partQuadAlloc = nullptr; }
        if (m_partAddPipeline)   { vkDestroyPipeline(m_dev.device, m_partAddPipeline, nullptr);   m_partAddPipeline = VK_NULL_HANDLE; }
        if (m_partAlphaPipeline) { vkDestroyPipeline(m_dev.device, m_partAlphaPipeline, nullptr); m_partAlphaPipeline = VK_NULL_HANDLE; }
        if (m_decalPipeline)     { vkDestroyPipeline(m_dev.device, m_decalPipeline, nullptr);     m_decalPipeline = VK_NULL_HANDLE; }
        if (m_partLayout)    { vkDestroyPipelineLayout(m_dev.device, m_partLayout, nullptr);  m_partLayout = VK_NULL_HANDLE; }
        if (m_decalLayout)   { vkDestroyPipelineLayout(m_dev.device, m_decalLayout, nullptr); m_decalLayout = VK_NULL_HANDLE; }
        if (m_partPool)      { vkDestroyDescriptorPool(m_dev.device, m_partPool, nullptr); m_partPool = VK_NULL_HANDLE; }
        if (m_partSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_partSetLayout, nullptr);  m_partSetLayout = VK_NULL_HANDLE; }
        if (m_decalSetLayout){ vkDestroyDescriptorSetLayout(m_dev.device, m_decalSetLayout, nullptr); m_decalSetLayout = VK_NULL_HANDLE; }
        if (m_partDepthSampler) { vkDestroySampler(m_dev.device, m_partDepthSampler, nullptr); m_partDepthSampler = VK_NULL_HANDLE; }
    }


} // namespace x3::rhi
