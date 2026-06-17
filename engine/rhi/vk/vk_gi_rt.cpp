// experiment
#include "VulkanRenderDevice_internal.h"
namespace x3::rhi {
bool VulkanRenderDevice::createSsao() {
        // NEAREST sampler for reading the depth image as plain data (no compare).
        VkSamplerCreateInfo dsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        dsci.magFilter = VK_FILTER_NEAREST; dsci.minFilter = VK_FILTER_NEAREST;
        dsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        dsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dsci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        if (vkCreateSampler(m_dev.device, &dsci, nullptr, &m_depthSampler) != VK_SUCCESS) {
            logError("[rhi] ssao depth sampler failed"); return false;
        }
        // CLAMP linear sampler for up-sampling the half-res AO into mesh.frag.
        VkSamplerCreateInfo lsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        lsci.magFilter = VK_FILTER_LINEAR; lsci.minFilter = VK_FILTER_LINEAR;
        lsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        lsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        lsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        lsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        lsci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        if (vkCreateSampler(m_dev.device, &lsci, nullptr, &m_ssaoLinearSampler) != VK_SUCCESS) {
            logError("[rhi] ssao linear sampler failed"); return false;
        }

        // ---- Descriptor set layout (ssao.frag): binding0 = depth sampler,
        //      binding1 = SsaoUBO. ----
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 2; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_ssaoSetLayout) != VK_SUCCESS) {
                logError("[rhi] ssao set layout failed"); return false;
            }
        }
        // ---- Blur set layout: binding0 = raw AO, binding1 = depth. ----
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 2; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_ssaoBlurSetLayout) != VK_SUCCESS) {
                logError("[rhi] ssao blur set layout failed"); return false;
            }
        }
        // (mesh.frag set 3 layout m_meshAoSetLayout was created in createGraphics so
        //  the mesh pipeline layout could include it; we only ALLOCATE its sets here.)

        // ---- Descriptor pool: per-frame ssao sets + per-frame mesh-ao sets + 1
        //      blur set. Samplers + uniform buffers sized exactly. ----
        {
            const uint32_t nFrames = kFramesInFlight;
            VkDescriptorPoolSize sizes[3]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sizes[0].descriptorCount = nFrames /*ssao depth*/ + nFrames * 4 /*mesh ao + refl + ddgi irr/vis*/ + 2 /*blur*/;
            sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            sizes[1].descriptorCount = nFrames /*ssao ubo*/ + nFrames /*ctrl ubo*/;
            // RT devices: mesh set3 carries the TLAS at binding 5 (r_rtshadows).
            sizes[2].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            sizes[2].descriptorCount = nFrames;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nFrames + nFrames + 1;
            pci.poolSizeCount = m_rtSupported ? 3u : 2u; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_ssaoPool) != VK_SUCCESS) {
                logError("[rhi] ssao desc pool failed"); return false;
            }
        }

        // ---- Per-frame SSAO + control UBOs + their descriptor sets. ----
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(SsaoUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&ub, &aci, &m_ssaoUboBuf[i], &m_ssaoUboAlloc[i], &info) != VK_SUCCESS) {
                logError("[rhi] ssao ubo create failed"); return false;
            }
            m_ssaoUboMapped[i] = info.pMappedData;

            VkBufferCreateInfo cb{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            cb.size = sizeof(SsaoControl); cb.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationInfo cinfo{};
            if (x3vmaCreateBuffer(&cb, &aci, &m_ssaoCtrlBuf[i], &m_ssaoCtrlAlloc[i], &cinfo) != VK_SUCCESS) {
                logError("[rhi] ssao ctrl ubo create failed"); return false;
            }
            m_ssaoCtrlMapped[i] = cinfo.pMappedData;

            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_ssaoPool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_ssaoSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_ssaoSet[i]) != VK_SUCCESS) {
                logError("[rhi] ssao set alloc failed"); return false;
            }
            VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a1.descriptorPool = m_ssaoPool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_meshAoSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_meshAoSet[i]) != VK_SUCCESS) {
                logError("[rhi] mesh ao set alloc failed"); return false;
            }
        }
        {
            VkDescriptorSetAllocateInfo ab{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ab.descriptorPool = m_ssaoPool; ab.descriptorSetCount = 1; ab.pSetLayouts = &m_ssaoBlurSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ab, &m_ssaoBlurSet) != VK_SUCCESS) {
                logError("[rhi] ssao blur set alloc failed"); return false;
            }
        }

        // ---- Depth pre-pass pipeline (depth.vert; depth-only, camera viewProj,
        //      set0 = objSet via m_shadowLayout, writes m_depthImg). ----
        if (!createDepthPrePipeline()) return false;

        // ---- SSAO pipeline (ssao.frag -> R8). ----
        {
            VkPushConstantRange pcr{}; // none for ssao; params come from the UBO
            (void)pcr;
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_ssaoSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_ssaoLayout) != VK_SUCCESS) {
                logError("[rhi] ssao pipeline layout failed"); return false;
            }
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssao.frag.spv",
                                          m_ssaoLayout, kSsaoFormat, /*additiveBlend=*/false, m_ssaoPipe))
                return false;
        }
        // ---- SSAO blur pipeline (ssao_blur.frag -> R8, push constant). ----
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoBlurPush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_ssaoBlurSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_ssaoBlurLayout) != VK_SUCCESS) {
                logError("[rhi] ssao blur pipeline layout failed"); return false;
            }
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssao_blur.frag.spv",
                                          m_ssaoBlurLayout, kSsaoFormat, /*additiveBlend=*/false, m_ssaoBlurPipe))
                return false;
        }

        logInfo("[rhi] SSAO ready (half-res 32-tap hemisphere + depth-aware blur, depth-reconstruction)");
        return true;
    }

bool VulkanRenderDevice::createDepthPrePipeline() {
        VkShaderModule vs = loadShaderModule("shaders\\depth.vert.spv");
        if (!vs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT; stage.module = vs; stage.pName = "main";

        VkVertexInputBindingDescription bind{ 0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attrs[3]{
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)    },
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal) },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(MeshVertex, uv)     },
        };
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;
        // Same back-face cull + winding as the main mesh pass so the depth values
        // match EXACTLY (the color pass then runs depth-test EQUAL).
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 0; cb.pAttachments = nullptr;
        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 0;
        prci.depthAttachmentFormat = m_depthFormat;
        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 1; gpci.pStages = &stage;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_shadowLayout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_depthPrePipeline);
        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] depth pre-pass pipeline create failed"); return false; }

        // ---- ALPHA-CUTOUT variant (depth_cutout.vert/.frag) -----------------
        // Identical fixed-function state; adds a fragment stage that replicates
        // mesh.frag's alphaMode==MASK discard so billboard depth matches the color
        // pass texel-for-texel. Used per-draw for cutout groups on reflections
        // frames (recordDepthPrePassBody). Set 0 = objSet (same bindings as
        // depth.vert -> layout-compatible with m_shadowLayout), set 1 = bindless.
        // NON-FATAL on failure: the plain full-quad pre-pass still works.
        {
            VkDescriptorSetLayout cutSets[2] = { m_objSetLayout, m_bindlessLayout };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 2; plci.pSetLayouts = cutSets;
            if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_depthPreCutoutLayout) == VK_SUCCESS) {
                VkShaderModule cvs = loadShaderModule("shaders\\depth_cutout.vert.spv");
                VkShaderModule cfs = loadShaderModule("shaders\\depth_cutout.frag.spv");
                if (cvs && cfs) {
                    VkPipelineShaderStageCreateInfo cstages[2]{};
                    cstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   cstages[0].module = cvs; cstages[0].pName = "main";
                    cstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; cstages[1].module = cfs; cstages[1].pName = "main";
                    gpci.stageCount = 2; gpci.pStages = cstages;
                    gpci.layout = m_depthPreCutoutLayout;
                    if (x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_depthPreCutoutPipeline) != VK_SUCCESS)
                        m_depthPreCutoutPipeline = VK_NULL_HANDLE;
                }
                if (cvs) vkDestroyShaderModule(m_dev.device, cvs, nullptr);
                if (cfs) vkDestroyShaderModule(m_dev.device, cfs, nullptr);
            }
            if (!m_depthPreCutoutPipeline)
                logError("[rhi] depth pre-pass CUTOUT pipeline unavailable — billboards keep full-quad depth");
        }
        return true;
    }

bool VulkanRenderDevice::createSsaoTargets() {
        destroySsaoTargets();
        m_ssaoExtent = { std::max(1u, m_extent.width / 2), std::max(1u, m_extent.height / 2) };
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!createColorTarget(kSsaoFormat, m_ssaoExtent.width, m_ssaoExtent.height, usage,
                               m_ssaoRawImg, m_ssaoRawAlloc, m_ssaoRawView)) {
            logError("[rhi] ssao raw target create failed"); return false;
        }
        if (!createColorTarget(kSsaoFormat, m_ssaoExtent.width, m_ssaoExtent.height, usage,
                               m_ssaoBlurImg, m_ssaoBlurAlloc, m_ssaoBlurView)) {
            logError("[rhi] ssao blur target create failed"); return false;
        }
        return true;
    }

void VulkanRenderDevice::destroySsaoTargets() {
        if (m_ssaoBlurView) { vkDestroyImageView(m_dev.device, m_ssaoBlurView, nullptr); m_ssaoBlurView = VK_NULL_HANDLE; }
        if (m_ssaoBlurImg)  { vmaDestroyImage(m_alloc, m_ssaoBlurImg, m_ssaoBlurAlloc); m_ssaoBlurImg = VK_NULL_HANDLE; m_ssaoBlurAlloc = nullptr; }
        if (m_ssaoRawView)  { vkDestroyImageView(m_dev.device, m_ssaoRawView, nullptr); m_ssaoRawView = VK_NULL_HANDLE; }
        if (m_ssaoRawImg)   { vmaDestroyImage(m_alloc, m_ssaoRawImg, m_ssaoRawAlloc); m_ssaoRawImg = VK_NULL_HANDLE; m_ssaoRawAlloc = nullptr; }
    }

void VulkanRenderDevice::writeSsaoDescriptors() {
        // ssao.frag set: binding0 = depth (NEAREST), binding1 = per-frame SsaoUBO.
        // The depth image is sampled while in DEPTH_READ_ONLY_OPTIMAL (the graph's
        // SSAO/blur passes transition it there), so the descriptor layout MUST be
        // DEPTH_READ_ONLY_OPTIMAL to satisfy VUID-vkCmdDraw-imageLayout-00344.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo di{ m_depthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo bi{ m_ssaoUboBuf[i], 0, sizeof(SsaoUBO) };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_ssaoSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &di;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_ssaoSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        // blur set: binding0 = raw AO (linear, SHADER_READ_ONLY), binding1 = depth
        // (NEAREST, DEPTH_READ_ONLY — same layout-match requirement as above).
        {
            VkDescriptorImageInfo da{ m_ssaoLinearSampler, m_ssaoRawView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo dd{ m_depthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_ssaoBlurSet; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &da;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_ssaoBlurSet; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &dd;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        // mesh.frag set3: binding0 = blurred AO (linear), binding1 = per-frame ctrl,
        // binding2 = the SSR/RT reflection buffer (refl.comp output). Before the
        // reflection chain is built, binding2 points at the blurred-AO view as a
        // LAYOUT-VALID placeholder (mesh.frag never samples it then — the
        // ssao.refl.x gate is 0 — but the descriptor must reference a real view).
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo da{ m_ssaoLinearSampler, m_ssaoBlurView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo dr{ m_ssaoLinearSampler,
                                      m_reflView ? m_reflView : m_ssaoBlurView,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            // DDGI atlases (bindings 3/4). Until/unless the DDGI chain is built,
            // the blurred-AO view is a LAYOUT-VALID placeholder (never sampled —
            // the ssao.ddgiCtrl.x gate is 0 — but descriptors must be real).
            VkDescriptorImageInfo dgi{ m_ssaoLinearSampler,
                                       m_ddgiIrrView ? m_ddgiIrrView : m_ssaoBlurView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo dgv{ m_ssaoLinearSampler,
                                       m_ddgiVisView ? m_ddgiVisView : m_ssaoBlurView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo bi{ m_ssaoCtrlBuf[i], 0, sizeof(SsaoControl) };
            VkWriteDescriptorSet w[5]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_meshAoSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &da;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_meshAoSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[2].dstSet = m_meshAoSet[i]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &dr;
            w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[3].dstSet = m_meshAoSet[i]; w[3].dstBinding = 3; w[3].descriptorCount = 1;
            w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo = &dgi;
            w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[4].dstSet = m_meshAoSet[i]; w[4].dstBinding = 4; w[4].descriptorCount = 1;
            w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[4].pImageInfo = &dgv;
            vkUpdateDescriptorSets(m_dev.device, 5, w, 0, nullptr);
        }
        // RT soft shadows (r_rtshadows): re-point set3 binding5 at the TLAS when
        // one exists (resize path — the sets were just rewritten above; keep the
        // AS binding live so the next RT-shadow frame doesn't trace a stale
        // descriptor). Before the first TLAS build there is nothing to write —
        // the plain pipelines never reference binding 5.
        writeMeshTlasDescriptor();
    }

void VulkanRenderDevice::writeMeshTlasDescriptor(uint32_t slot) {
        if (!m_rtSupported) return;
        VkAccelerationStructureKHR tlas = m_rt.tlas();
        if (!tlas || !m_meshAoSet[0]) return;
        const uint32_t lo = (slot == kAllFrameSlots) ? 0u : slot;
        const uint32_t hi = (slot == kAllFrameSlots) ? kFramesInFlight : slot + 1u;
        for (uint32_t i = lo; i < hi && i < kFramesInFlight; ++i) {
            if (!m_meshAoSet[i]) continue;
            VkWriteDescriptorSetAccelerationStructureKHR asW{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
            asW.accelerationStructureCount = 1; asW.pAccelerationStructures = &tlas;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.pNext = &asW; w.dstSet = m_meshAoSet[i]; w.dstBinding = 5;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
        m_meshTlasWritten = true;
    }

void VulkanRenderDevice::destroySsao() {
        destroySsaoTargets();
        if (m_ssaoBlurPipe)   { vkDestroyPipeline(m_dev.device, m_ssaoBlurPipe, nullptr); m_ssaoBlurPipe = VK_NULL_HANDLE; }
        if (m_ssaoPipe)       { vkDestroyPipeline(m_dev.device, m_ssaoPipe, nullptr); m_ssaoPipe = VK_NULL_HANDLE; }
        if (m_depthPrePipeline){ vkDestroyPipeline(m_dev.device, m_depthPrePipeline, nullptr); m_depthPrePipeline = VK_NULL_HANDLE; }
        if (m_depthPreCutoutPipeline){ vkDestroyPipeline(m_dev.device, m_depthPreCutoutPipeline, nullptr); m_depthPreCutoutPipeline = VK_NULL_HANDLE; }
        if (m_depthPreCutoutLayout)  { vkDestroyPipelineLayout(m_dev.device, m_depthPreCutoutLayout, nullptr); m_depthPreCutoutLayout = VK_NULL_HANDLE; }
        if (m_ssaoBlurLayout) { vkDestroyPipelineLayout(m_dev.device, m_ssaoBlurLayout, nullptr); m_ssaoBlurLayout = VK_NULL_HANDLE; }
        if (m_ssaoLayout)     { vkDestroyPipelineLayout(m_dev.device, m_ssaoLayout, nullptr); m_ssaoLayout = VK_NULL_HANDLE; }
        if (m_ssaoPool)       { vkDestroyDescriptorPool(m_dev.device, m_ssaoPool, nullptr); m_ssaoPool = VK_NULL_HANDLE; }
        if (m_meshAoSetLayout){ vkDestroyDescriptorSetLayout(m_dev.device, m_meshAoSetLayout, nullptr); m_meshAoSetLayout = VK_NULL_HANDLE; }
        if (m_ssaoBlurSetLayout){ vkDestroyDescriptorSetLayout(m_dev.device, m_ssaoBlurSetLayout, nullptr); m_ssaoBlurSetLayout = VK_NULL_HANDLE; }
        if (m_ssaoSetLayout)  { vkDestroyDescriptorSetLayout(m_dev.device, m_ssaoSetLayout, nullptr); m_ssaoSetLayout = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_ssaoUboBuf[i])  { vmaDestroyBuffer(m_alloc, m_ssaoUboBuf[i], m_ssaoUboAlloc[i]); m_ssaoUboBuf[i] = VK_NULL_HANDLE; }
            if (m_ssaoCtrlBuf[i]) { vmaDestroyBuffer(m_alloc, m_ssaoCtrlBuf[i], m_ssaoCtrlAlloc[i]); m_ssaoCtrlBuf[i] = VK_NULL_HANDLE; }
        }
        if (m_ssaoLinearSampler){ vkDestroySampler(m_dev.device, m_ssaoLinearSampler, nullptr); m_ssaoLinearSampler = VK_NULL_HANDLE; }
        if (m_depthSampler)   { vkDestroySampler(m_dev.device, m_depthSampler, nullptr); m_depthSampler = VK_NULL_HANDLE; }
    }

void VulkanRenderDevice::rtLogInfo(const char* m) { x3::logInfo(m ? m : ""); }

void VulkanRenderDevice::rtLogError(const char* m) { x3::logError(m ? m : ""); }

bool VulkanRenderDevice::ensureRtCore() {
        if (!m_rtSupported) return false;
        if (!m_rtInitTried) {
            m_rtInitTried = true;
            if (!m_rt.init(m_dev.device, m_dev.physical_device, m_alloc, m_gfxQueue,
                           m_gfxFamily, &rtLogInfo, &rtLogError)) {
                logError("[rhi] RT: AS module init failed — staying on raster/SSAO");
                m_rtSupported = false;   // disable RT entirely; never retry
                return false;
            }
            // Position-fetch tier: BLAS builds carry ALLOW_DATA_ACCESS so DDGI's
            // ray-query shader may read committed-triangle vertex positions.
            m_rt.setAllowDataAccess(m_rtPosFetch);
        }
        return m_rt.ready();
    }

bool VulkanRenderDevice::ensureRtaoReady() {
        if (!ensureRtCore()) return false;
        if (!m_rtaoBuilt) {
            if (!createRtao()) { logError("[rhi] RT AO: pipeline create failed"); m_rtSupported = false; return false; }
            if (!createRtaoTargets()) { logError("[rhi] RT AO: target create failed"); m_rtSupported = false; return false; }
            writeRtaoDescriptors();
            m_rtaoBuilt = true;
            logInfo("[rhi] RT AO ready (ray-query inline AO: BLAS/TLAS + half-res compute + multiply apply)");
        }
        return true;
    }

bool VulkanRenderDevice::createRtao() {
        // NEAREST sampler for reading depth as plain data; LINEAR for AO up-sample.
        VkSamplerCreateInfo n{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        n.magFilter = VK_FILTER_NEAREST; n.minFilter = VK_FILTER_NEAREST;
        n.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        n.addressModeU = n.addressModeV = n.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        n.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        if (vkCreateSampler(m_dev.device, &n, nullptr, &m_rtaoDepthSampler) != VK_SUCCESS) return false;
        VkSamplerCreateInfo l = n; l.magFilter = VK_FILTER_LINEAR; l.minFilter = VK_FILTER_LINEAR;
        if (vkCreateSampler(m_dev.device, &l, nullptr, &m_rtaoLinearSampler) != VK_SUCCESS) return false;

        // ---- Compute set layout: 0=depth sampler, 1=AO storage image, 2=TLAS,
        //      3=Rtao UBO. ----
        {
            VkDescriptorSetLayoutBinding b[4]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 4; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_rtaoSetLayout) != VK_SUCCESS) return false;
        }
        // ---- Apply set layout: 0=AO (linear), 1=depth (nearest). ----
        {
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 2; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_rtaoApplySetLayout) != VK_SUCCESS) return false;
        }
        // ---- Descriptor pool: per-frame compute + apply sets. ----
        {
            const uint32_t nF = kFramesInFlight;
            VkDescriptorPoolSize sizes[4]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = nF /*compute depth*/ + nF * 2 /*apply*/;
            sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          sizes[1].descriptorCount = nF;
            sizes[2].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; sizes[2].descriptorCount = nF;
            sizes[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[3].descriptorCount = nF;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nF * 2; pci.poolSizeCount = 4; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_rtaoPool) != VK_SUCCESS) return false;
        }
        // ---- Per-frame UBOs + sets. ----
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(RtaoUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&ub, &aci, &m_rtaoUboBuf[i], &m_rtaoUboAlloc[i], &info) != VK_SUCCESS) return false;
            m_rtaoUboMapped[i] = info.pMappedData;
            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_rtaoPool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_rtaoSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_rtaoSet[i]) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a1.descriptorPool = m_rtaoPool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_rtaoApplySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_rtaoApplySet[i]) != VK_SUCCESS) return false;
        }
        // ---- Compute pipeline (rtao.comp). ----
        {
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_rtaoSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_rtaoLayout) != VK_SUCCESS) return false;
            VkShaderModule cs = loadShaderModule("shaders\\rtao.comp.spv");
            if (!cs) return false;
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_rtaoLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_rtaoPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return false;
        }
        // ---- Apply pipeline (rtao_apply.frag -> HDR, MULTIPLY blend). ----
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RtaoApplyPush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_rtaoApplySetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_rtaoApplyLayout) != VK_SUCCESS) return false;
            if (!createMultiplyFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\rtao_apply.frag.spv",
                                                  m_rtaoApplyLayout, kHdrFormat, m_rtaoApplyPipe))
                return false;
        }
        return true;
    }

bool VulkanRenderDevice::createMultiplyFullscreenPipeline(const char* vsPath, const char* fsPath,
                                      VkPipelineLayout layout, VkFormat colorFmt,
                                      VkPipeline& outPipe) {
        VkShaderModule vs = loadShaderModule(vsPath);
        VkShaderModule fs = loadShaderModule(fsPath);
        if (!vs || !fs) { if (vs) vkDestroyShaderModule(m_dev.device, vs, nullptr); if (fs) vkDestroyShaderModule(m_dev.device, fs, nullptr); return false; }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
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
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE; dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &colorFmt;
        prci.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci; gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = layout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &outPipe);
        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        return pr == VK_SUCCESS;
    }

bool VulkanRenderDevice::createRtaoTargets() {
        destroyRtaoTargets();
        m_rtaoExtent = { std::max(1u, m_extent.width / 2), std::max(1u, m_extent.height / 2) };
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!createColorTarget(kRtaoFormat, m_rtaoExtent.width, m_rtaoExtent.height, usage,
                               m_rtaoImg, m_rtaoAlloc, m_rtaoView)) return false;
        return true;
    }

void VulkanRenderDevice::destroyRtaoTargets() {
        if (m_rtaoView) { vkDestroyImageView(m_dev.device, m_rtaoView, nullptr); m_rtaoView = VK_NULL_HANDLE; }
        if (m_rtaoImg)  { vmaDestroyImage(m_alloc, m_rtaoImg, m_rtaoAlloc); m_rtaoImg = VK_NULL_HANDLE; m_rtaoAlloc = nullptr; }
    }

void VulkanRenderDevice::writeRtaoDescriptors() {
        VkAccelerationStructureKHR tlas = m_rt.tlas();
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo depthInfo{ m_rtaoDepthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo aoInfo{ VK_NULL_HANDLE, m_rtaoView, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorBufferInfo ubo{ m_rtaoUboBuf[i], 0, sizeof(RtaoUBO) };
            VkWriteDescriptorSetAccelerationStructureKHR asW{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
            asW.accelerationStructureCount = 1; asW.pAccelerationStructures = &tlas;
            VkWriteDescriptorSet w[4]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_rtaoSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &depthInfo;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = m_rtaoSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &aoInfo;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = m_rtaoSet[i]; w[2].dstBinding = 3; w[2].descriptorCount = 1;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[2].pBufferInfo = &ubo;
            // Always write the depth/AO/UBO bindings; add the TLAS binding only when
            // a TLAS exists (it's built later this frame, so rewriteRtaoTlas() fills
            // binding 2 once available — the set is never used before then).
            w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = m_rtaoSet[i]; w[3].dstBinding = 2; w[3].descriptorCount = 1;
            w[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w[3].pNext = &asW;
            vkUpdateDescriptorSets(m_dev.device, tlas ? 4u : 3u, w, 0, nullptr);

            VkDescriptorImageInfo aoSampled{ m_rtaoLinearSampler, m_rtaoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo depthSampled{ m_rtaoDepthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet a[2]{};
            a[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; a[0].dstSet = m_rtaoApplySet[i]; a[0].dstBinding = 0; a[0].descriptorCount = 1;
            a[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; a[0].pImageInfo = &aoSampled;
            a[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; a[1].dstSet = m_rtaoApplySet[i]; a[1].dstBinding = 1; a[1].descriptorCount = 1;
            a[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; a[1].pImageInfo = &depthSampled;
            vkUpdateDescriptorSets(m_dev.device, 2, a, 0, nullptr);
        }
    }

bool VulkanRenderDevice::ensureAudioRays() {
        if (m_audioRayBuilt)  return true;
        if (m_audioRayFailed) return false;   // failed once — don't retry/spam
        auto fail = [&](const char* what) {
            logError(std::string("[rta] audio-ray chain create failed: ") + what);
            m_audioRayFailed = true;
            destroyAudioRays();
            return false;
        };
        // Set layout.
        {
            VkDescriptorSetLayoutBinding b[3]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 3; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_audioRaySetLayout) != VK_SUCCESS)
                return fail("set layout");
        }
        // Pipeline (push constant = ray count).
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_audioRaySetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_audioRayLayout) != VK_SUCCESS)
                return fail("pipeline layout");
            VkShaderModule cs = loadShaderModule("shaders\\audio_rays.comp.spv");
            if (!cs) return fail("audio_rays.comp.spv load");
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_audioRayLayout;
            VkResult pr = vkCreateComputePipelines(m_dev.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_audioRayPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return fail("compute pipeline");
        }
        // Buffers: ray batch in (sequential write) + hit distances out (random read).
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = (VkDeviceSize)kAudioRayCapacity * sizeof(AudioRay);
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(m_alloc, &bci, &aci, &m_audioRayInBuf, &m_audioRayInAlloc, &info) != VK_SUCCESS)
                return fail("ray-in buffer");
            m_audioRayInMapped = info.pMappedData;
            bci.size = (VkDeviceSize)kAudioRayCapacity * sizeof(float);
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            if (vmaCreateBuffer(m_alloc, &bci, &aci, &m_audioRayOutBuf, &m_audioRayOutAlloc, &info) != VK_SUCCESS)
                return fail("hit-out buffer");
            m_audioRayOutMapped = info.pMappedData;
        }
        // Descriptor pool + set; write the two SSBO bindings now (TLAS is bound
        // per-call when the handle changes).
        {
            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; sizes[0].descriptorCount = 1;
            sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;             sizes[1].descriptorCount = 2;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_dev.device, &pci, nullptr, &m_audioRayPool) != VK_SUCCESS)
                return fail("descriptor pool");
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_audioRayPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_audioRaySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_audioRaySet) != VK_SUCCESS)
                return fail("descriptor set");
            VkDescriptorBufferInfo inB{ m_audioRayInBuf, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo outB{ m_audioRayOutBuf, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_audioRaySet; w[0].dstBinding = 1; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &inB;
            w[1] = w[0]; w[1].dstBinding = 2; w[1].pBufferInfo = &outB;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        // Transient command buffer + fence (own pool: self-contained, like VulkanRT).
        {
            VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpci.queueFamilyIndex = m_gfxFamily;
            if (vkCreateCommandPool(m_dev.device, &cpci, nullptr, &m_audioRayCmdPool) != VK_SUCCESS)
                return fail("command pool");
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = m_audioRayCmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(m_dev.device, &ai, &m_audioRayCmd) != VK_SUCCESS)
                return fail("command buffer");
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (vkCreateFence(m_dev.device, &fci, nullptr, &m_audioRayFence) != VK_SUCCESS)
                return fail("fence");
        }
        m_audioRayBuilt = true;
        logInfo("[rta] audio-ray chain ready (audio rays through the scene TLAS)");
        return true;
    }

void VulkanRenderDevice::destroyAudioRays() {
        if (!m_dev.device) return;
        if (m_audioRayInFlight && m_audioRayFence) {
            vkWaitForFences(m_dev.device, 1, &m_audioRayFence, VK_TRUE, UINT64_MAX);
            m_audioRayInFlight = false;
            m_audioRayInFlightCount = 0;
        }
        if (m_audioRayFence)   { vkDestroyFence(m_dev.device, m_audioRayFence, nullptr); m_audioRayFence = VK_NULL_HANDLE; }
        if (m_audioRayCmdPool) { vkDestroyCommandPool(m_dev.device, m_audioRayCmdPool, nullptr); m_audioRayCmdPool = VK_NULL_HANDLE; m_audioRayCmd = VK_NULL_HANDLE; }
        if (m_audioRayPool)    { vkDestroyDescriptorPool(m_dev.device, m_audioRayPool, nullptr); m_audioRayPool = VK_NULL_HANDLE; m_audioRaySet = VK_NULL_HANDLE; }
        if (m_audioRayInBuf)   { vmaDestroyBuffer(m_alloc, m_audioRayInBuf, m_audioRayInAlloc); m_audioRayInBuf = VK_NULL_HANDLE; m_audioRayInAlloc = nullptr; m_audioRayInMapped = nullptr; }
        if (m_audioRayOutBuf)  { vmaDestroyBuffer(m_alloc, m_audioRayOutBuf, m_audioRayOutAlloc); m_audioRayOutBuf = VK_NULL_HANDLE; m_audioRayOutAlloc = nullptr; m_audioRayOutMapped = nullptr; }
        if (m_audioRayPipe)      { vkDestroyPipeline(m_dev.device, m_audioRayPipe, nullptr); m_audioRayPipe = VK_NULL_HANDLE; }
        if (m_audioRayLayout)    { vkDestroyPipelineLayout(m_dev.device, m_audioRayLayout, nullptr); m_audioRayLayout = VK_NULL_HANDLE; }
        if (m_audioRaySetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_audioRaySetLayout, nullptr); m_audioRaySetLayout = VK_NULL_HANDLE; }
        m_audioRayTlasBound = VK_NULL_HANDLE;
        m_audioRayBuilt = false;
    }

bool VulkanRenderDevice::ensureReflReady() {
        if (!m_reflBuilt) {
            // mesh set3 binding2 is rewritten below for ALL frames in flight ->
            // those sets may still be referenced by executing frames. One-time
            // hitch on first enable only.
            vkDeviceWaitIdle(m_dev.device);
            if (!createRefl() || !createReflTargets()) {
                logError("[rhi] reflections: create failed — r_ssr disabled");
                destroyRefl();
                m_refl.ssr = false;
                return false;
            }
            writeReflDescriptors();
            writeSsaoDescriptors();   // re-point mesh set3 binding2 at the refl buffer
            m_reflBuilt = true;
            logInfo(m_reflPipeRt
                ? "[rhi] reflections ready (SSR depth-march vs prev-frame color + ray-query fallback)"
                : "[rhi] reflections ready (SSR depth-march vs prev-frame color; no RT fallback)");
        }
        // Live r_reflquality switch: recreate the target at the new resolution.
        if (m_reflFullRes != m_refl.fullRes) {
            vkDeviceWaitIdle(m_dev.device);
            if (!createReflTargets()) { m_refl.ssr = false; return false; }
            writeReflDescriptors();
            writeSsaoDescriptors();
        }
        return m_reflImg != VK_NULL_HANDLE && m_reflPipe != VK_NULL_HANDLE;
    }

bool VulkanRenderDevice::createRefl() {
        // ---- Set layouts: 0 = depth sampler, 1 = output storage image, 2 = prev
        // scene (TAA history) sampler, 3 = Refl UBO; the RT variant adds 4 = TLAS.
        {
            VkDescriptorSetLayoutBinding b[5]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 4; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_reflSetLayout) != VK_SUCCESS) return false;
            if (m_rtSupported) {
                b[4].binding = 4; b[4].descriptorCount = 1;
                b[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                b[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.bindingCount = 5;
                if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_reflSetLayoutRt) != VK_SUCCESS) return false;
            }
        }
        // ---- Pool + per-frame UBOs + sets (SSR always; RT sets on RT devices). ----
        {
            const uint32_t nFrames = kFramesInFlight;
            const uint32_t nVariants = m_rtSupported ? 2u : 1u;
            VkDescriptorPoolSize sizes[4]{};
            uint32_t nSizes = 3;
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sizes[0].descriptorCount = nFrames * nVariants * 2;   // depth + prevScene
            sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            sizes[1].descriptorCount = nFrames * nVariants;
            sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            sizes[2].descriptorCount = nFrames * nVariants;
            if (m_rtSupported) {
                sizes[3].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                sizes[3].descriptorCount = nFrames;
                nSizes = 4;
            }
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nFrames * nVariants; pci.poolSizeCount = nSizes; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_reflPool) != VK_SUCCESS) return false;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(ReflUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&ub, &aci, &m_reflUboBuf[i], &m_reflUboAlloc[i], &info) != VK_SUCCESS) return false;
            m_reflUboMapped[i] = info.pMappedData;

            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_reflPool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_reflSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_reflSet[i]) != VK_SUCCESS) return false;
            if (m_rtSupported) {
                VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                a1.descriptorPool = m_reflPool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_reflSetLayoutRt;
                if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_reflSetRt[i]) != VK_SUCCESS) return false;
            }
        }
        // ---- Compute pipelines: SSR-only (every device) + ray-query (RT only). ----
        {
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_reflSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_reflLayout) != VK_SUCCESS) return false;
            VkShaderModule cs = loadShaderModule("shaders\\refl.comp.spv");
            if (!cs) return false;
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_reflLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_reflPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return false;
        }
        if (m_rtSupported) {
            // Non-fatal: an RT-pipeline failure degrades to SSR-only (logged).
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_reflSetLayoutRt;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_reflLayoutRt) == VK_SUCCESS) {
                VkShaderModule cs = loadShaderModule("shaders\\refl_rt.comp.spv");
                if (cs) {
                    VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
                    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
                    cpci.layout = m_reflLayoutRt;
                    if (x3CreateComputePipelines(1, &cpci, nullptr, &m_reflPipeRt) != VK_SUCCESS)
                        m_reflPipeRt = VK_NULL_HANDLE;
                    vkDestroyShaderModule(m_dev.device, cs, nullptr);
                }
            }
            if (!m_reflPipeRt)
                logError("[rhi] reflections: ray-query pipeline unavailable — SSR-only");
        }
        return true;
    }

bool VulkanRenderDevice::createReflTargets() {
        destroyReflTargets();
        m_reflFullRes = m_refl.fullRes;
        const uint32_t div = m_reflFullRes ? 1u : 2u;
        m_reflExtent = { std::max(1u, m_extent.width / div), std::max(1u, m_extent.height / div) };
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!createColorTarget(kReflFormat, m_reflExtent.width, m_reflExtent.height, usage,
                               m_reflImg, m_reflAlloc, m_reflView)) return false;
        const bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            iblBarrierTex2D(cmd, m_reflImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!ok) { logError("[rhi] reflections: target init transition failed"); return false; }
        return true;
    }

void VulkanRenderDevice::destroyReflTargets() {
        if (m_reflView) { vkDestroyImageView(m_dev.device, m_reflView, nullptr); m_reflView = VK_NULL_HANDLE; }
        if (m_reflImg)  { vmaDestroyImage(m_alloc, m_reflImg, m_reflAlloc); m_reflImg = VK_NULL_HANDLE; m_reflAlloc = nullptr; }
    }

void VulkanRenderDevice::writeReflDescriptors() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo depthInfo{ m_depthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, m_reflView, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo histInfo{ m_ssaoLinearSampler,
                                            m_taaHistView ? m_taaHistView : m_reflView,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo ubo{ m_reflUboBuf[i], 0, sizeof(ReflUBO) };
            VkDescriptorSet targets[2] = { m_reflSet[i], m_reflSetRt[i] };
            for (int s = 0; s < 2; ++s) {
                if (!targets[s]) continue;
                VkWriteDescriptorSet w[4]{};
                w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = targets[s]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
                w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &depthInfo;
                w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = targets[s]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
                w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &outInfo;
                w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = targets[s]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
                w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &histInfo;
                w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = targets[s]; w[3].dstBinding = 3; w[3].descriptorCount = 1;
                w[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[3].pBufferInfo = &ubo;
                vkUpdateDescriptorSets(m_dev.device, 4, w, 0, nullptr);
            }
        }
        // The TLAS (RT sets, binding 4) may already exist (e.g. RT AO enabled
        // first): point the fresh sets at it now; otherwise rewriteRtaoTlas()
        // fills it after the first build.
        if (m_rt.tlasBuilt()) rewriteRtaoTlas();
    }

void VulkanRenderDevice::destroyRefl() {
        if (!m_dev.device) return;
        destroyReflTargets();
        if (m_reflPipeRt)      { vkDestroyPipeline(m_dev.device, m_reflPipeRt, nullptr); m_reflPipeRt = VK_NULL_HANDLE; }
        if (m_reflPipe)        { vkDestroyPipeline(m_dev.device, m_reflPipe, nullptr); m_reflPipe = VK_NULL_HANDLE; }
        if (m_reflLayoutRt)    { vkDestroyPipelineLayout(m_dev.device, m_reflLayoutRt, nullptr); m_reflLayoutRt = VK_NULL_HANDLE; }
        if (m_reflLayout)      { vkDestroyPipelineLayout(m_dev.device, m_reflLayout, nullptr); m_reflLayout = VK_NULL_HANDLE; }
        if (m_reflPool)        { vkDestroyDescriptorPool(m_dev.device, m_reflPool, nullptr); m_reflPool = VK_NULL_HANDLE; }
        if (m_reflSetLayoutRt) { vkDestroyDescriptorSetLayout(m_dev.device, m_reflSetLayoutRt, nullptr); m_reflSetLayoutRt = VK_NULL_HANDLE; }
        if (m_reflSetLayout)   { vkDestroyDescriptorSetLayout(m_dev.device, m_reflSetLayout, nullptr); m_reflSetLayout = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_reflUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_reflUboBuf[i], m_reflUboAlloc[i]); m_reflUboBuf[i] = VK_NULL_HANDLE; m_reflUboMapped[i] = nullptr; }
            m_reflSet[i] = VK_NULL_HANDLE; m_reflSetRt[i] = VK_NULL_HANDLE;
        }
        m_reflBuilt = false;
        m_reflActiveThisFrame = false;
        m_reflRtThisFrame = false;
    }

bool VulkanRenderDevice::ensureDdgiReady() {
        if (!ensureRtCore() || !m_rtPosFetch) return false;
        // The ray pass's MISS shading binds the IBL env cube; without the IBL
        // chain there is no cube view to bind (rare init failure) — stay off.
        if (m_iblEnvCubeView == VK_NULL_HANDLE) return false;
        if (!m_ddgiBuilt) {
            // mesh set3 bindings 3/4 are rewritten for ALL frames in flight ->
            // those sets may be referenced by executing frames. One-time hitch.
            vkDeviceWaitIdle(m_dev.device);
            if (!createDdgi() || !createDdgiTargets()) {
                logError("[rhi] DDGI: create failed — r_ddgi disabled");
                destroyDdgi();
                m_ddgi.enabled = false;
                return false;
            }
            writeDdgiDescriptors();
            writeSsaoDescriptors();   // re-point mesh set3 bindings 3/4 at the atlases
            m_ddgiBuilt = true;
            logInfo("[rhi] DDGI ready (ray-query probe grid: octahedral irradiance + Chebyshev visibility atlases)");
        }
        // Live grid-dimension change (r_ddgi_n*): recreate the atlases + ray buffer.
        if (m_ddgiCountX != m_ddgi.countX || m_ddgiCountY != m_ddgi.countY ||
            m_ddgiCountZ != m_ddgi.countZ) {
            vkDeviceWaitIdle(m_dev.device);
            if (!createDdgiTargets()) { m_ddgi.enabled = false; return false; }
            writeDdgiDescriptors();
            writeSsaoDescriptors();
            m_ddgiVolumeValid = false;   // spacing depends on counts
        }
        if (!m_ddgiVolumeValid) computeDdgiVolume();
        return m_ddgiRayPipe != VK_NULL_HANDLE && m_ddgiUpPipe != VK_NULL_HANDLE
            && m_ddgiIrrImg != VK_NULL_HANDLE;
    }

bool VulkanRenderDevice::createDdgi() {
        // LINEAR/CLAMP sampler for the atlas reads (compute feedback + nothing else;
        // mesh.frag set3 uses m_ssaoLinearSampler like its other bindings).
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_ddgiSampler) != VK_SUCCESS) return false;

        // ---- RAY set layout: 0=TLAS, 1=object SSBO, 2=ray SSBO, 3=UBO,
        //      4=prev irradiance, 5=prev visibility, 6=env cube. ----
        {
            VkDescriptorSetLayoutBinding b[7]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[4].binding = 4; b[4].descriptorCount = 1;
            b[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[5].binding = 5; b[5].descriptorCount = 1;
            b[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[6].binding = 6; b[6].descriptorCount = 1;
            b[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            for (int i = 0; i < 7; ++i) b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 7; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_ddgiRaySetLayout) != VK_SUCCESS) return false;
        }
        // ---- UPDATE set layout: 0=ray SSBO, 1=irr storage image, 2=vis storage
        //      image, 3=UBO. ----
        {
            VkDescriptorSetLayoutBinding b[4]{};
            b[0].binding = 0; b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[1].binding = 1; b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[2].binding = 2; b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[3].binding = 3; b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            for (int i = 0; i < 4; ++i) b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = 4; ci.pBindings = b;
            if (vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &m_ddgiUpSetLayout) != VK_SUCCESS) return false;
        }
        // ---- Pool + per-frame UBOs + sets. ----
        {
            const uint32_t nF = kFramesInFlight;
            VkDescriptorPoolSize sizes[5]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     sizes[0].descriptorCount = nF * 3;
            sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;             sizes[1].descriptorCount = nF * 3;
            sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;             sizes[2].descriptorCount = nF * 2;
            sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;              sizes[3].descriptorCount = nF * 2;
            sizes[4].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; sizes[4].descriptorCount = nF;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nF * 2; pci.poolSizeCount = 5; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_ddgiPool) != VK_SUCCESS) return false;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(DdgiUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&ub, &aci, &m_ddgiUboBuf[i], &m_ddgiUboAlloc[i], &info) != VK_SUCCESS) return false;
            m_ddgiUboMapped[i] = info.pMappedData;
            VkDescriptorSetAllocateInfo a0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a0.descriptorPool = m_ddgiPool; a0.descriptorSetCount = 1; a0.pSetLayouts = &m_ddgiRaySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a0, &m_ddgiRaySet[i]) != VK_SUCCESS) return false;
            VkDescriptorSetAllocateInfo a1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            a1.descriptorPool = m_ddgiPool; a1.descriptorSetCount = 1; a1.pSetLayouts = &m_ddgiUpSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &a1, &m_ddgiUpSet[i]) != VK_SUCCESS) return false;
        }
        // ---- Compute pipelines: ddgi_rays + ddgi_update (mode push constant). ----
        {
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_ddgiRaySetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_ddgiRayLayout) != VK_SUCCESS) return false;
            VkShaderModule cs = loadShaderModule("shaders\\ddgi_rays.comp.spv");
            if (!cs) return false;
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_ddgiRayLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_ddgiRayPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return false;
        }
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_ddgiUpSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_ddgiUpLayout) != VK_SUCCESS) return false;
            VkShaderModule cs = loadShaderModule("shaders\\ddgi_update.comp.spv");
            if (!cs) return false;
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_ddgiUpLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_ddgiUpPipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) return false;
        }
        return true;
    }

bool VulkanRenderDevice::createDdgiTargets() {
        destroyDdgiTargets();
        m_ddgiCountX = m_ddgi.countX; m_ddgiCountY = m_ddgi.countY; m_ddgiCountZ = m_ddgi.countZ;
        const uint32_t tilesU = (uint32_t)(m_ddgiCountX * m_ddgiCountY);
        const uint32_t tilesV = (uint32_t)m_ddgiCountZ;
        const uint32_t probeCount = (uint32_t)(m_ddgiCountX * m_ddgiCountY * m_ddgiCountZ);
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                      | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!createColorTarget(kDdgiIrrFormat, tilesU * 8u, tilesV * 8u, usage,
                               m_ddgiIrrImg, m_ddgiIrrAlloc, m_ddgiIrrView)) return false;
        if (!createColorTarget(kDdgiVisFormat, tilesU * 16u, tilesV * 16u, usage,
                               m_ddgiVisImg, m_ddgiVisAlloc, m_ddgiVisView)) return false;

        // Ray results SSBO: probeCount * 128 (max rays) * vec4, device-local.
        m_ddgiRayBufSize = (VkDeviceSize)probeCount * 128u * 16u;
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = m_ddgiRayBufSize;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (x3vmaCreateBuffer(&bci, &aci, &m_ddgiRayBuf, &m_ddgiRayAlloc, nullptr) != VK_SUCCESS)
            return false;

        // Clear both atlases to zero + park them SHADER_READ_ONLY so the very
        // first ddgi-rays pass (which samples them as "previous frame") reads
        // defined black and the always-bound mesh set3 descriptors are valid.
        const bool ok = oneTimeSubmit([&](VkCommandBuffer cmd){
            VkImage imgs[2] = { m_ddgiIrrImg, m_ddgiVisImg };
            for (int i = 0; i < 2; ++i) {
                iblBarrierTex2D(cmd, imgs[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkClearColorValue zero{};
                VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(cmd, imgs[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
                iblBarrierTex2D(cmd, imgs[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
        });
        if (!ok) { logError("[rhi] DDGI: atlas init transition failed"); return false; }
        const ResourceState ready{ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
        m_ddgiIrrState = ready;
        m_ddgiVisState = ready;
        m_ddgiFrameCount = 0;
        m_ddgiVolumeValid = false;   // new counts -> refit spacing
        return true;
    }

void VulkanRenderDevice::destroyDdgiTargets() {
        if (m_ddgiIrrView) { vkDestroyImageView(m_dev.device, m_ddgiIrrView, nullptr); m_ddgiIrrView = VK_NULL_HANDLE; }
        if (m_ddgiIrrImg)  { vmaDestroyImage(m_alloc, m_ddgiIrrImg, m_ddgiIrrAlloc); m_ddgiIrrImg = VK_NULL_HANDLE; m_ddgiIrrAlloc = nullptr; }
        if (m_ddgiVisView) { vkDestroyImageView(m_dev.device, m_ddgiVisView, nullptr); m_ddgiVisView = VK_NULL_HANDLE; }
        if (m_ddgiVisImg)  { vmaDestroyImage(m_alloc, m_ddgiVisImg, m_ddgiVisAlloc); m_ddgiVisImg = VK_NULL_HANDLE; m_ddgiVisAlloc = nullptr; }
        if (m_ddgiRayBuf)  { vmaDestroyBuffer(m_alloc, m_ddgiRayBuf, m_ddgiRayAlloc); m_ddgiRayBuf = VK_NULL_HANDLE; m_ddgiRayAlloc = nullptr; }
        m_ddgiCountX = m_ddgiCountY = m_ddgiCountZ = 0;
    }

void VulkanRenderDevice::writeDdgiDescriptors() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorBufferInfo objInfo{ m_frames[i].objBuf, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo rayInfo{ m_ddgiRayBuf, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo uboInfo{ m_ddgiUboBuf[i], 0, sizeof(DdgiUBO) };
            VkDescriptorImageInfo irrSampled{ m_ddgiSampler, m_ddgiIrrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo visSampled{ m_ddgiSampler, m_ddgiVisView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo envSampled{ m_iblCubeSampler, m_iblEnvCubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo irrStorage{ VK_NULL_HANDLE, m_ddgiIrrView, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo visStorage{ VK_NULL_HANDLE, m_ddgiVisView, VK_IMAGE_LAYOUT_GENERAL };

            VkWriteDescriptorSet w[10]{};
            uint32_t n = 0;
            auto add = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                           const VkDescriptorImageInfo* ii, const VkDescriptorBufferInfo* bi) {
                w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[n].dstSet = set; w[n].dstBinding = binding; w[n].descriptorCount = 1;
                w[n].descriptorType = type; w[n].pImageInfo = ii; w[n].pBufferInfo = bi;
                ++n;
            };
            add(m_ddgiRaySet[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objInfo);
            add(m_ddgiRaySet[i], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &rayInfo);
            add(m_ddgiRaySet[i], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);
            add(m_ddgiRaySet[i], 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &irrSampled, nullptr);
            add(m_ddgiRaySet[i], 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &visSampled, nullptr);
            add(m_ddgiRaySet[i], 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &envSampled, nullptr);
            add(m_ddgiUpSet[i], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &rayInfo);
            add(m_ddgiUpSet[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &irrStorage, nullptr);
            add(m_ddgiUpSet[i], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &visStorage, nullptr);
            add(m_ddgiUpSet[i], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);
            vkUpdateDescriptorSets(m_dev.device, n, w, 0, nullptr);
        }
        if (m_rt.tlasBuilt()) rewriteRtaoTlas();   // fills ray-set binding 0
    }

void VulkanRenderDevice::destroyDdgi() {
        if (!m_dev.device) return;
        destroyDdgiTargets();
        if (m_ddgiUpPipe)       { vkDestroyPipeline(m_dev.device, m_ddgiUpPipe, nullptr); m_ddgiUpPipe = VK_NULL_HANDLE; }
        if (m_ddgiRayPipe)      { vkDestroyPipeline(m_dev.device, m_ddgiRayPipe, nullptr); m_ddgiRayPipe = VK_NULL_HANDLE; }
        if (m_ddgiUpLayout)     { vkDestroyPipelineLayout(m_dev.device, m_ddgiUpLayout, nullptr); m_ddgiUpLayout = VK_NULL_HANDLE; }
        if (m_ddgiRayLayout)    { vkDestroyPipelineLayout(m_dev.device, m_ddgiRayLayout, nullptr); m_ddgiRayLayout = VK_NULL_HANDLE; }
        if (m_ddgiPool)         { vkDestroyDescriptorPool(m_dev.device, m_ddgiPool, nullptr); m_ddgiPool = VK_NULL_HANDLE; }
        if (m_ddgiUpSetLayout)  { vkDestroyDescriptorSetLayout(m_dev.device, m_ddgiUpSetLayout, nullptr); m_ddgiUpSetLayout = VK_NULL_HANDLE; }
        if (m_ddgiRaySetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_ddgiRaySetLayout, nullptr); m_ddgiRaySetLayout = VK_NULL_HANDLE; }
        if (m_ddgiSampler)      { vkDestroySampler(m_dev.device, m_ddgiSampler, nullptr); m_ddgiSampler = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_ddgiUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_ddgiUboBuf[i], m_ddgiUboAlloc[i]); m_ddgiUboBuf[i] = VK_NULL_HANDLE; m_ddgiUboMapped[i] = nullptr; }
            m_ddgiRaySet[i] = VK_NULL_HANDLE; m_ddgiUpSet[i] = VK_NULL_HANDLE;
        }
        m_ddgiBuilt = false;
        m_ddgiWantThisFrame = false;
        m_ddgiActiveThisFrame = false;
        m_ddgiVolumeValid = false;
        m_ddgiFrameCount = 0;
    }

void VulkanRenderDevice::destroyRt() {
        if (!m_dev.device) { m_rt.shutdown(); return; }
        destroyRtaoTargets();
        if (m_rtaoApplyPipe) { vkDestroyPipeline(m_dev.device, m_rtaoApplyPipe, nullptr); m_rtaoApplyPipe = VK_NULL_HANDLE; }
        if (m_rtaoPipe)      { vkDestroyPipeline(m_dev.device, m_rtaoPipe, nullptr); m_rtaoPipe = VK_NULL_HANDLE; }
        if (m_rtaoApplyLayout){ vkDestroyPipelineLayout(m_dev.device, m_rtaoApplyLayout, nullptr); m_rtaoApplyLayout = VK_NULL_HANDLE; }
        if (m_rtaoLayout)    { vkDestroyPipelineLayout(m_dev.device, m_rtaoLayout, nullptr); m_rtaoLayout = VK_NULL_HANDLE; }
        if (m_rtaoPool)      { vkDestroyDescriptorPool(m_dev.device, m_rtaoPool, nullptr); m_rtaoPool = VK_NULL_HANDLE; }
        if (m_rtaoApplySetLayout){ vkDestroyDescriptorSetLayout(m_dev.device, m_rtaoApplySetLayout, nullptr); m_rtaoApplySetLayout = VK_NULL_HANDLE; }
        if (m_rtaoSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_rtaoSetLayout, nullptr); m_rtaoSetLayout = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_rtaoUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_rtaoUboBuf[i], m_rtaoUboAlloc[i]); m_rtaoUboBuf[i] = VK_NULL_HANDLE; }
        }
        if (m_rtaoLinearSampler){ vkDestroySampler(m_dev.device, m_rtaoLinearSampler, nullptr); m_rtaoLinearSampler = VK_NULL_HANDLE; }
        if (m_rtaoDepthSampler) { vkDestroySampler(m_dev.device, m_rtaoDepthSampler, nullptr); m_rtaoDepthSampler = VK_NULL_HANDLE; }
        m_rtaoBuilt = false;
        m_rt.shutdown();
    }

void VulkanRenderDevice::buildSsaoKernelAndNoise() {
        if (m_ssaoKernelBuilt) return;
        m_ssaoKernelBuilt = true;
        // Deterministic LCG so the look is identical across runs (and clean-room).
        uint32_t s = 0x13572468u;
        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / (float)(1u << 24); };
        for (int i = 0; i < kSsaoKernel; ++i) {
            glm::vec3 v(rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f, rnd()); // hemisphere (+z)
            v = glm::normalize(v) * rnd();
            float t = (float)i / (float)kSsaoKernel;
            float scale = 0.1f + 0.9f * t * t;            // accelerate toward 1 (cluster near origin)
            v *= scale;
            m_ssaoKernelCPU[i] = glm::vec4(v, 0.0f);
        }
        for (int i = 0; i < 16; ++i) {
            // Rotation vectors in the XY plane (z=0), uniform in [-1,1].
            m_ssaoNoiseCPU[i] = glm::vec4(rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f, 0.0f, 0.0f);
        }
    }

void VulkanRenderDevice::buildGiKernelAndNoise() {
        if (m_giKernelBuilt) return;
        m_giKernelBuilt = true;
        uint32_t s = 0x9E3779B9u;
        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / (float)(1u << 24); };
        for (int i = 0; i < kGiKernel; ++i) {
            // Cosine-weighted hemisphere about +z (Malley): r = sqrt(u1), phi = 2pi u2.
            float u1 = rnd(), u2 = rnd();
            float r = std::sqrt(u1);
            float phi = 6.2831853f * u2;
            glm::vec3 v(r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u1)));
            // Vary the radial reach so taps spread across the hemisphere shell, with
            // a mild bias toward mid-range (broad soft bounce, not just contact).
            float t = (float)i / (float)kGiKernel;
            float scale = 0.25f + 0.75f * t;
            m_giKernelCPU[i] = glm::vec4(v * scale, 0.0f);
        }
        for (int i = 0; i < 16; ++i)
            m_giNoiseCPU[i] = glm::vec4(rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f, 0.0f, 0.0f);
    }

bool VulkanRenderDevice::createGi() {
        // ---- Descriptor set layouts ----
        auto makeLayout = [&](uint32_t nImg, bool hasUbo, VkDescriptorSetLayout& out) -> bool {
            VkDescriptorSetLayoutBinding b[8]{};
            uint32_t n = 0;
            for (uint32_t i = 0; i < nImg; ++i) {
                b[n].binding = n; b[n].descriptorCount = 1;
                b[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++n;
            }
            if (hasUbo) {
                b[n].binding = n; b[n].descriptorCount = 1;
                b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++n;
            }
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = n; ci.pBindings = b;
            return vkCreateDescriptorSetLayout(m_dev.device, &ci, nullptr, &out) == VK_SUCCESS;
        };
        // gather: depth + scene + GiUBO ; temporal: cur+hist+depth+prevDepth + UBO ;
        // blur: gi + depth (push const) ; apply: gi + depth + ao (push const).
        if (!makeLayout(2, true,  m_giGatherSetLayout))   { logError("[rhi] gi gather layout failed"); return false; }
        if (!makeLayout(4, true,  m_giTemporalSetLayout)) { logError("[rhi] gi temporal layout failed"); return false; }
        if (!makeLayout(2, false, m_giBlurSetLayout))     { logError("[rhi] gi blur layout failed"); return false; }
        if (!makeLayout(3, false, m_giApplySetLayout))    { logError("[rhi] gi apply layout failed"); return false; }

        // ---- Descriptor pool ----
        {
            const uint32_t nF = kFramesInFlight;
            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sizes[0].descriptorCount = nF * 2 /*gather*/ + nF * 4 /*temporal*/ + nF * 2 /*blur*/ + nF * 3 /*apply*/;
            sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            sizes[1].descriptorCount = nF /*gather ubo*/ + nF /*temporal ubo*/;
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = nF * 4; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
            if (x3CreateDescriptorPool(&pci, nullptr, &m_giPool) != VK_SUCCESS) {
                logError("[rhi] gi desc pool failed"); return false;
            }
        }
        // ---- Per-frame UBOs + gather/temporal sets ----
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VkBufferCreateInfo ub{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ub.size = sizeof(GiUBO); ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&ub, &aci, &m_giUboBuf[i], &m_giUboAlloc[i], &info) != VK_SUCCESS) {
                logError("[rhi] gi ubo create failed"); return false;
            }
            m_giUboMapped[i] = info.pMappedData;
            VkBufferCreateInfo tb{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            tb.size = sizeof(GiTemporalUBO); tb.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationInfo tinfo{};
            if (x3vmaCreateBuffer(&tb, &aci, &m_giTempUboBuf[i], &m_giTempUboAlloc[i], &tinfo) != VK_SUCCESS) {
                logError("[rhi] gi temporal ubo create failed"); return false;
            }
            m_giTempUboMapped[i] = tinfo.pMappedData;

            VkDescriptorSetAllocateInfo ag{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ag.descriptorPool = m_giPool; ag.descriptorSetCount = 1; ag.pSetLayouts = &m_giGatherSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ag, &m_giGatherSet[i]) != VK_SUCCESS) {
                logError("[rhi] gi gather set alloc failed"); return false;
            }
            VkDescriptorSetAllocateInfo at{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            at.descriptorPool = m_giPool; at.descriptorSetCount = 1; at.pSetLayouts = &m_giTemporalSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &at, &m_giTemporalSet[i]) != VK_SUCCESS) {
                logError("[rhi] gi temporal set alloc failed"); return false;
            }
            VkDescriptorSetAllocateInfo ab{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ab.descriptorPool = m_giPool; ab.descriptorSetCount = 1; ab.pSetLayouts = &m_giBlurSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ab, &m_giBlurSet[i]) != VK_SUCCESS) {
                logError("[rhi] gi blur set alloc failed"); return false;
            }
            VkDescriptorSetAllocateInfo aa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            aa.descriptorPool = m_giPool; aa.descriptorSetCount = 1; aa.pSetLayouts = &m_giApplySetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &aa, &m_giApplySet[i]) != VK_SUCCESS) {
                logError("[rhi] gi apply set alloc failed"); return false;
            }
        }
        // ---- Pipeline layouts ----
        auto makePipeLayout = [&](VkDescriptorSetLayout setL, uint32_t pcSize, VkPipelineLayout& out) -> bool {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &setL;
            if (pcSize > 0) { pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr; }
            return vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &out) == VK_SUCCESS;
        };
        if (!makePipeLayout(m_giGatherSetLayout,   0, m_giGatherLayout))   { logError("[rhi] gi gather pl failed"); return false; }
        if (!makePipeLayout(m_giTemporalSetLayout, 0, m_giTemporalLayout)) { logError("[rhi] gi temporal pl failed"); return false; }
        if (!makePipeLayout(m_giBlurSetLayout,  sizeof(GiBlurPush),  m_giBlurLayout))  { logError("[rhi] gi blur pl failed"); return false; }
        if (!makePipeLayout(m_giApplySetLayout, sizeof(GiApplyPush), m_giApplyLayout)) { logError("[rhi] gi apply pl failed"); return false; }

        // ---- Pipelines (full-screen triangle). Apply uses ADDITIVE blend into the
        //      HDR target; the rest write their own half-res buffers (no blend). ----
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssgi_gather.frag.spv",
                                      m_giGatherLayout, kGiFormat, /*additiveBlend=*/false, m_giGatherPipe)) return false;
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssgi_temporal.frag.spv",
                                      m_giTemporalLayout, kGiFormat, /*additiveBlend=*/false, m_giTemporalPipe)) return false;
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssgi_blur.frag.spv",
                                      m_giBlurLayout, kGiFormat, /*additiveBlend=*/false, m_giBlurPipe)) return false;
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\ssgi_apply.frag.spv",
                                      m_giApplyLayout, kHdrFormat, /*additiveBlend=*/true, m_giApplyPipe)) return false;
        return true;
    }

bool VulkanRenderDevice::createGiTargets() {
        destroyGiTargets();
        m_giExtent = { std::max(1u, m_extent.width / 2), std::max(1u, m_extent.height / 2) };
        const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!createColorTarget(kGiFormat, m_giExtent.width, m_giExtent.height, use,
                               m_giRawImg, m_giRawAlloc, m_giRawView)) { logError("[rhi] gi raw target failed"); return false; }
        for (int i = 0; i < 2; ++i)
            if (!createColorTarget(kGiFormat, m_giExtent.width, m_giExtent.height, use,
                                   m_giAccumImg[i], m_giAccumAlloc[i], m_giAccumView[i])) { logError("[rhi] gi accum target failed"); return false; }
        if (!createColorTarget(kGiFormat, m_giExtent.width, m_giExtent.height, use,
                               m_giDenoiseImg, m_giDenoiseAlloc, m_giDenoiseView)) { logError("[rhi] gi denoise target failed"); return false; }
        // Prev-depth: a full-res copy of the depth buffer (TRANSFER_DST + SAMPLED).
        // Same format as the main depth so vkCmdCopyImage is a straight blit.
        VkImageCreateInfo dici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        dici.imageType = VK_IMAGE_TYPE_2D; dici.format = m_depthFormat;
        dici.extent = { m_extent.width, m_extent.height, 1 }; dici.mipLevels = 1; dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT; dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        dici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        dici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo daci{}; daci.usage = VMA_MEMORY_USAGE_AUTO;
        daci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&dici, &daci, &m_giPrevDepthImg, &m_giPrevDepthAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] gi prev-depth image failed"); return false;
        }
        VkImageViewCreateInfo dvci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        dvci.image = m_giPrevDepthImg; dvci.viewType = VK_IMAGE_VIEW_TYPE_2D; dvci.format = m_depthFormat;
        dvci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &dvci, nullptr, &m_giPrevDepthView) != VK_SUCCESS) {
            logError("[rhi] gi prev-depth view failed"); return false;
        }
        m_giHistoryValid = false;   // no usable history until the second frame
        m_giAccumWrite = 0;
        return true;
    }

void VulkanRenderDevice::destroyGiTargets() {
        auto killImg = [&](VkImage& im, VmaAllocation& al, VkImageView& v) {
            if (v)  { vkDestroyImageView(m_dev.device, v, nullptr); v = VK_NULL_HANDLE; }
            if (im) { vmaDestroyImage(m_alloc, im, al); im = VK_NULL_HANDLE; al = nullptr; }
        };
        killImg(m_giPrevDepthImg, m_giPrevDepthAlloc, m_giPrevDepthView);
        killImg(m_giDenoiseImg, m_giDenoiseAlloc, m_giDenoiseView);
        killImg(m_giAccumImg[0], m_giAccumAlloc[0], m_giAccumView[0]);
        killImg(m_giAccumImg[1], m_giAccumAlloc[1], m_giAccumView[1]);
        killImg(m_giRawImg, m_giRawAlloc, m_giRawView);
    }

void VulkanRenderDevice::writeGiDescriptors() {
        // Gather: 0=depth(NEAREST), 1=scene(LINEAR), 2=GiUBO. Stable per resize.
        // Depth is sampled in DEPTH_READ_ONLY_OPTIMAL (the layout the graph leaves it
        // in for the GI passes); colour images use SHADER_READ_ONLY_OPTIMAL.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorImageInfo d{ m_depthSampler, m_depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo s{ m_ssaoLinearSampler, m_hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo u{ m_giUboBuf[i], 0, sizeof(GiUBO) };
            VkWriteDescriptorSet w[3]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_giGatherSet[i];
            w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &d;
            w[1] = w[0]; w[1].dstBinding = 1; w[1].pImageInfo = &s;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = m_giGatherSet[i];
            w[2].dstBinding = 2; w[2].descriptorCount = 1;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[2].pBufferInfo = &u;
            vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
        }
        // The temporal/blur/apply sets are written per-frame (ping-pong) by
        // writeGiFrameDescriptors(); nothing stable to do here for them.
    }

void VulkanRenderDevice::writeGiFrameDescriptors(uint32_t writeIdx, uint32_t histIdx, VkImageView aoView) {
        const VkImageLayout RO  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // colour
        const VkImageLayout DRO = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;   // depth
        // Temporal set (this frame's m_frameIdx): 0=cur(raw), 1=hist(accum[hist]),
        // 2=depth, 3=prevDepth, 4=GiTemporalUBO.
        {
            VkDescriptorImageInfo cur { m_ssaoLinearSampler, m_giRawView,          RO  };
            VkDescriptorImageInfo hist{ m_ssaoLinearSampler, m_giAccumView[histIdx], RO };
            VkDescriptorImageInfo dep { m_depthSampler,      m_depthView,          DRO };
            VkDescriptorImageInfo pdep{ m_depthSampler,      m_giPrevDepthView,    DRO };
            VkDescriptorBufferInfo ub { m_giTempUboBuf[m_frameIdx], 0, sizeof(GiTemporalUBO) };
            VkWriteDescriptorSet w[5]{};
            for (int k = 0; k < 4; ++k) {
                w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = m_giTemporalSet[m_frameIdx];
                w[k].dstBinding = (uint32_t)k; w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            w[0].pImageInfo = &cur; w[1].pImageInfo = &hist; w[2].pImageInfo = &dep; w[3].pImageInfo = &pdep;
            w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[4].dstSet = m_giTemporalSet[m_frameIdx];
            w[4].dstBinding = 4; w[4].descriptorCount = 1;
            w[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[4].pBufferInfo = &ub;
            vkUpdateDescriptorSets(m_dev.device, 5, w, 0, nullptr);
        }
        // Blur set: 0 = accum[writeIdx] (the temporal output), 1 = depth.
        {
            VkDescriptorImageInfo gi { m_ssaoLinearSampler, m_giAccumView[writeIdx], RO  };
            VkDescriptorImageInfo dep{ m_depthSampler,      m_depthView,            DRO };
            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_giBlurSet[m_frameIdx];
            w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &gi;
            w[1] = w[0]; w[1].dstBinding = 1; w[1].pImageInfo = &dep;
            vkUpdateDescriptorSets(m_dev.device, 2, w, 0, nullptr);
        }
        // Apply set: 0 = denoised GI, 1 = depth, 2 = AO (the blurred SSAO when on,
        // otherwise a harmless valid image — apply forces aoAmount=0 in that case).
        {
            VkDescriptorImageInfo gi { m_ssaoLinearSampler, m_giDenoiseView, RO  };
            VkDescriptorImageInfo dep{ m_depthSampler,      m_depthView,     DRO };
            VkDescriptorImageInfo ao { m_ssaoLinearSampler, aoView,          RO  };
            VkWriteDescriptorSet w[3]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_giApplySet[m_frameIdx];
            w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &gi;
            w[1] = w[0]; w[1].dstBinding = 1; w[1].pImageInfo = &dep;
            w[2] = w[0]; w[2].dstBinding = 2; w[2].pImageInfo = &ao;
            vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
        }
    }

void VulkanRenderDevice::destroyGi() {
        destroyGiTargets();
        auto killPipe = [&](VkPipeline& p){ if (p) { vkDestroyPipeline(m_dev.device, p, nullptr); p = VK_NULL_HANDLE; } };
        killPipe(m_giApplyPipe); killPipe(m_giBlurPipe); killPipe(m_giTemporalPipe); killPipe(m_giGatherPipe);
        auto killPL = [&](VkPipelineLayout& l){ if (l) { vkDestroyPipelineLayout(m_dev.device, l, nullptr); l = VK_NULL_HANDLE; } };
        killPL(m_giApplyLayout); killPL(m_giBlurLayout); killPL(m_giTemporalLayout); killPL(m_giGatherLayout);
        if (m_giPool) { vkDestroyDescriptorPool(m_dev.device, m_giPool, nullptr); m_giPool = VK_NULL_HANDLE; }
        auto killSL = [&](VkDescriptorSetLayout& l){ if (l) { vkDestroyDescriptorSetLayout(m_dev.device, l, nullptr); l = VK_NULL_HANDLE; } };
        killSL(m_giApplySetLayout); killSL(m_giBlurSetLayout); killSL(m_giTemporalSetLayout); killSL(m_giGatherSetLayout);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_giUboBuf[i])     { vmaDestroyBuffer(m_alloc, m_giUboBuf[i], m_giUboAlloc[i]); m_giUboBuf[i] = VK_NULL_HANDLE; }
            if (m_giTempUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_giTempUboBuf[i], m_giTempUboAlloc[i]); m_giTempUboBuf[i] = VK_NULL_HANDLE; }
        }
    }

} // namespace x3::rhi
