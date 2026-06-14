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

const VulkanRenderDevice::RoleFontDesc* VulkanRenderDevice::roleFontTable() {
        // Indexed by FontRole. Console==HudMono share index 0 (embedded mono).
        static const RoleFontDesc kRoleFontPaths[kFontRoleCount] = {
            /* 0 Console/HudMono */ { "Consolas.ttf",                                 false, "Consolas" }, // matches the BabylonJS x3-console (Consolas,Courier New,monospace); embedded Roboto Mono is the fallback
            /* 1 Title           */ { "Orbitron/static/Orbitron-Bold.ttf",           true,  "Orbitron-Bold" },
            /* 2 Menu            */ { "Space_Grotesk/static/SpaceGrotesk-Medium.ttf", true,  "SpaceGrotesk-Medium" },
            /* 3 Enemy           */ { "Tektur/static/Tektur_Condensed-Bold.ttf",      true,  "Tektur_Condensed-Bold" },
            /* 4 News            */ { "Space_Mono/SpaceMono-Bold.ttf",                false, "SpaceMono-Bold" },
        };
        return kRoleFontPaths;
    }

bool VulkanRenderDevice::buildFontAtlas() {
        // Universal last-resort bitmap atlas (used if a role's TTF AND the embedded
        // fallback both fail — extremely unlikely, but never ship blank text).
        m_bitmapFontReady = buildBitmapFontAtlas(m_bitmapFontTex);
        if (!m_bitmapFontReady)
            logError("[rhi] HUD font: 8x8 bitmap fallback atlas failed to build");

        const RoleFontDesc* table = roleFontTable();
        int roleOk = 0;
        for (int r = 0; r < kFontRoleCount; ++r) {
            // HudMono is an ALIAS of Console (same enum value 0); the loop visits each
            // distinct index once, so no special-casing is needed.
            FontAtlas& fa = m_fonts[r];
            fa = FontAtlas{};
            fa.proportional = table[r].proportional;

            // Load the role's TTF bytes from assets/fonts/, else the embedded Roboto
            // Mono. (Index 0 has no file => always embedded.)
            std::vector<unsigned char> fileBytes;
            const unsigned char* ttf = kRobotoMonoTTF;
            size_t ttfSize = kRobotoMonoTTFSize;
            std::string loaded = "Roboto Mono (embedded)";
            if (table[r].path) {
                std::string resolved;
                fileBytes = readFontFile(table[r].path, resolved);
                if (!fileBytes.empty()) {
                    ttf = fileBytes.data(); ttfSize = fileBytes.size();
                    loaded = std::string(table[r].label) + " (" + resolved + ")";
                } else {
                    loaded = std::string("Roboto Mono (embedded; ") + table[r].path +
                             " not found)";
                }
            }

            if (bakeTtfAtlas(r, ttf, ttfSize)) {
                fa.ready = true; ++roleOk;
                logInfo("[rhi] HUD font role " + roleName(r) + " -> " + loaded +
                        (fa.proportional ? " [proportional]" : " [monospace]"));
            } else {
                // The role's chosen TTF failed — fall back to the EMBEDDED font so the
                // role still has a proper atlas (mono), rather than the 8x8 bitmap.
                fa = FontAtlas{}; fa.proportional = false;
                if (ttf != kRobotoMonoTTF && bakeTtfAtlas(r, kRobotoMonoTTF, kRobotoMonoTTFSize)) {
                    fa.ready = true; ++roleOk;
                    logError("[rhi] HUD font role " + roleName(r) + " -> " + loaded +
                             " FAILED to bake; using Roboto Mono (embedded) [monospace]");
                } else {
                    logError("[rhi] HUD font role " + roleName(r) +
                             ": TTF bake failed entirely — using 8x8 bitmap fallback");
                }
            }
        }
        logInfo("[rhi] HUD fonts: " + std::to_string(roleOk) + "/" +
                std::to_string(kFontRoleCount) + " role atlases baked from TTF");
        // Success as long as SOMETHING can render text (a role atlas or the bitmap).
        return roleOk > 0 || m_bitmapFontReady;
    }

std::string VulkanRenderDevice::roleName(int r) {
        switch (r) {
            case 0: return "Console/HudMono";
            case 1: return "Title";
            case 2: return "Menu";
            case 3: return "Enemy";
            case 4: return "News";
            default: return std::to_string(r);
        }
    }

bool VulkanRenderDevice::bakeTtfAtlas(int role, const unsigned char* ttf, size_t ttfSize) {
        FontAtlas& fa = m_fonts[role];
        if (!ttf || ttfSize < 4) { logError("[rhi] TTF: empty font data"); return false; }

        stbtt_fontinfo info{};
        const int off = stbtt_GetFontOffsetForIndex(ttf, 0);
        if (off < 0) { logError("[rhi] TTF: GetFontOffsetForIndex failed"); return false; }
        if (!stbtt_InitFont(&info, ttf, off)) { logError("[rhi] TTF: InitFont failed"); return false; }

        // Global vertical metrics at the bake size (consistent baseline/cell height).
        const float scale = stbtt_ScaleForPixelHeight(&info, kTtfBakePx);
        int asc = 0, desc = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &lineGap);
        fa.ascent = asc * scale;   // baseline distance from the cell top

        // Reference cell advance (drives bake-px -> requested-px scale `s`). For mono
        // fonts every glyph shares this; for proportional fonts it's just the scale
        // reference ('M' advance), with real per-glyph advances stored per glyph.
        {
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&info, 'M', &adv, &lsb);
            fa.cellAdvance = adv * scale;
            if (fa.cellAdvance <= 0.0f) fa.cellAdvance = kTtfBakePx; // sane fallback
        }

        // Pack the glyph range into an 8-bit coverage atlas.
        std::vector<unsigned char> coverage((size_t)kTtfAtlasW * kTtfAtlasH, 0);
        std::vector<stbtt_packedchar> packed(kTtfCharCount);
        stbtt_pack_context spc{};
        if (!stbtt_PackBegin(&spc, coverage.data(), kTtfAtlasW, kTtfAtlasH,
                             /*stride=*/0, /*padding=*/1, nullptr)) {
            logError("[rhi] TTF: PackBegin failed"); return false;
        }
        stbtt_PackSetOversampling(&spc, 2, 2);   // 2x2 supersample for crisp small text
        if (!stbtt_PackFontRange(&spc, ttf, 0, kTtfBakePx,
                                 kTtfFirstChar, kTtfCharCount, packed.data())) {
            logError("[rhi] TTF: PackFontRange failed (atlas too small?)");
            stbtt_PackEnd(&spc);
            return false;
        }
        stbtt_PackEnd(&spc);

        // Record per-glyph atlas rects + quad offsets + advance (bake-pixel units).
        for (int i = 0; i < kTtfCharCount; ++i) {
            const stbtt_packedchar& pc = packed[i];
            TtfGlyph& g = fa.glyphs[i];
            g.u0 = (float)pc.x0 / (float)kTtfAtlasW;
            g.v0 = (float)pc.y0 / (float)kTtfAtlasH;
            g.u1 = (float)pc.x1 / (float)kTtfAtlasW;
            g.v1 = (float)pc.y1 / (float)kTtfAtlasH;
            // pc.xoff/yoff are offsets from the pen (baseline) to the glyph's top-left.
            g.x0 = pc.xoff;  g.y0 = pc.yoff;
            g.x1 = pc.xoff2; g.y1 = pc.yoff2;
            g.advance = pc.xadvance;
        }

        // Expand coverage -> RGBA (white, alpha=coverage) and upload.
        std::vector<uint8_t> rgba((size_t)kTtfAtlasW * kTtfAtlasH * 4, 0);
        for (size_t p = 0; p < coverage.size(); ++p) {
            rgba[p*4+0] = 255; rgba[p*4+1] = 255; rgba[p*4+2] = 255;
            rgba[p*4+3] = coverage[p];
        }
        if (!createSampledTexture(rgba.data(), kTtfAtlasW, kTtfAtlasH, /*srgb=*/false, fa.tex)) {
            logError("[rhi] TTF: atlas texture upload failed"); return false;
        }

        // LINEAR + CLAMP sampler: smooth glyph edges (antialiased), no wrap bleed.
        if (fa.tex.sampler) vkDestroySampler(m_dev.device, fa.tex.sampler, nullptr);
        fa.tex.sampler = VK_NULL_HANDLE;
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &fa.tex.sampler) != VK_SUCCESS) {
            logError("[rhi] TTF font sampler create failed"); return false;
        }
        return true;
    }

bool VulkanRenderDevice::buildBitmapFontAtlas(Texture& dst) {
        constexpr uint32_t kAtlasW = 128, kAtlasH = 64;
        std::vector<uint8_t> rgba((size_t)kAtlasW * kAtlasH * 4, 0);
        for (int ch = 0; ch < 128; ++ch) {
            int col = ch % 16, row = ch / 16;
            int baseX = col * 8, baseY = row * 8;
            for (int gy = 0; gy < 8; ++gy) {
                unsigned char bits = kFont8x8Basic[ch][gy];
                for (int gx = 0; gx < 8; ++gx) {
                    bool on = (bits >> gx) & 1u;  // bit0 = leftmost pixel
                    size_t px = ((size_t)(baseY + gy) * kAtlasW + (baseX + gx)) * 4;
                    uint8_t a = on ? 255 : 0;
                    rgba[px+0] = 255; rgba[px+1] = 255; rgba[px+2] = 255; rgba[px+3] = a;
                }
            }
        }
        // UNORM (data): the per-vertex color already carries the desired tint, so
        // no sRGB linearization of the mask is wanted. Nearest filtering keeps the
        // pixel font crisp.
        if (!createSampledTexture(rgba.data(), kAtlasW, kAtlasH, /*srgb=*/false, dst))
            return false;
        // Replace the linear sampler with a NEAREST, CLAMP one for crisp glyphs.
        if (dst.sampler) vkDestroySampler(m_dev.device, dst.sampler, nullptr);
        dst.sampler = VK_NULL_HANDLE;
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &dst.sampler) != VK_SUCCESS) {
            logError("[rhi] font sampler create failed"); return false;
        }
        return true;
    }

bool VulkanRenderDevice::createHud() {
        // Descriptor set layout: just the HUD texture at binding 0 (frag).
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorCount = 1;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 1; slci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_hudSetLayout) != VK_SUCCESS) {
            logError("[rhi] HUD set layout failed"); return false;
        }

        // Per-frame HUD descriptor pools + host-visible vertex rings.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            VkDescriptorPoolSize sz{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxHudDraws };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = kMaxHudDraws; pci.poolSizeCount = 1; pci.pPoolSizes = &sz;
            if (x3CreateDescriptorPool(&pci, nullptr, &fr.hudDescPool) != VK_SUCCESS) {
                logError("[rhi] HUD descriptor pool failed"); return false;
            }
            VkBufferCreateInfo vbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            vbci.size = (VkDeviceSize)kMaxHudVerts * sizeof(HudVertex);
            vbci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo vaci{};
            vaci.usage = VMA_MEMORY_USAGE_AUTO;
            vaci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo vinfo{};
            if (x3vmaCreateBuffer(&vbci, &vaci, &fr.hudVbo, &fr.hudVboAlloc, &vinfo) != VK_SUCCESS) {
                logError("[rhi] HUD vertex ring create failed"); return false;
            }
            fr.hudVboMapped = vinfo.pMappedData;
        }

        // Font atlas (uploaded once).
        if (!buildFontAtlas()) { logError("[rhi] font atlas build failed"); return false; }

        // Shaders.
        VkShaderModule vs = loadShaderModule("shaders\\hud.vert.spv");
        VkShaderModule fs = loadShaderModule("shaders\\hud.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkVertexInputBindingDescription bind{ 0, sizeof(HudVertex), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attrs[3]{
            { 0, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(HudVertex, pos)   },
            { 1, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(HudVertex, uv)    },
            { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(HudVertex, color) },
        };
        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // No depth test/write: the HUD draws on top of the 3D scene.
        VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;

        // Straight (non-premultiplied) alpha blend.
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_hudSetLayout; // no push constants
        if (vkCreatePipelineLayout(m_dev.device, &plci, nullptr, &m_hudLayout) != VK_SUCCESS) {
            logError("[rhi] HUD pipeline layout failed"); return false;
        }

        // HDR pipeline: the HUD is now drawn in the COMPOSITE pass (after tonemap),
        // which writes the LDR final target (m_format) and has NO depth attachment.
        // So the HUD pipeline declares the LDR color format + UNDEFINED depth.
        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &m_format;
        prci.depthAttachmentFormat = VK_FORMAT_UNDEFINED;  // composite pass has no depth

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = m_hudLayout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &m_hudPipeline);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] HUD pipeline create failed"); return false; }

        logInfo("[rhi] HUD 2D pipeline ready (NDC quads + TTF/bitmap glyph atlas, alpha-blended, no depth)");
        return true;
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

void VulkanRenderDevice::destroyHud() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            auto& fr = m_frames[i];
            if (fr.hudVbo) { vmaDestroyBuffer(m_alloc, fr.hudVbo, fr.hudVboAlloc);
                             fr.hudVbo = VK_NULL_HANDLE; fr.hudVboAlloc = nullptr; fr.hudVboMapped = nullptr; }
            if (fr.hudDescPool) { vkDestroyDescriptorPool(m_dev.device, fr.hudDescPool, nullptr); fr.hudDescPool = VK_NULL_HANDLE; }
        }
        for (int r = 0; r < kFontRoleCount; ++r) {
            destroyTextureObj(m_fonts[r].tex);
            m_fonts[r].ready = false;
        }
        destroyTextureObj(m_bitmapFontTex);
        m_bitmapFontReady = false;
        if (m_hudPipeline)  vkDestroyPipeline(m_dev.device, m_hudPipeline, nullptr);
        if (m_hudLayout)    vkDestroyPipelineLayout(m_dev.device, m_hudLayout, nullptr);
        if (m_hudSetLayout) vkDestroyDescriptorSetLayout(m_dev.device, m_hudSetLayout, nullptr);
        m_hudPipeline = VK_NULL_HANDLE; m_hudLayout = VK_NULL_HANDLE; m_hudSetLayout = VK_NULL_HANDLE;
    }

bool VulkanRenderDevice::createSwapchain(uint32_t w, uint32_t h) {
        vkb::SwapchainBuilder scb{ m_dev };
        scb.set_desired_format(VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
           .set_desired_present_mode(m_vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR)
           .set_desired_extent(w, h)
           // TRANSFER_SRC so captureFrame() can vkCmdCopyImageToBuffer the
           // presented color image to a host-visible readback buffer (--screenshot).
           .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (m_swapchain != VK_NULL_HANDLE) scb.set_old_swapchain(m_swapchain);
        auto ret = scb.build();
        if (!ret) { logError(std::string("[rhi] swapchain: ") + ret.error().message()); return false; }
        vkb::Swapchain sc = ret.value();

        // destroy old views/swapchain after building the new one
        destroySwapchain();

        m_swapchain = sc.swapchain;
        m_extent = sc.extent;
        m_format = sc.image_format;
        m_swapImages = sc.get_images().value();
        m_swapViews  = sc.get_image_views().value();

        // (re)create per-image renderFinished semaphores
        m_renderFinished.resize(m_swapImages.size());
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (auto& s : m_renderFinished) vkCreateSemaphore(m_dev.device, &si, nullptr, &s);

        // Depth buffer sized to the swapchain
        VkImageCreateInfo dici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        dici.imageType = VK_IMAGE_TYPE_2D;
        dici.format = m_depthFormat;
        dici.extent = { m_extent.width, m_extent.height, 1 };
        dici.mipLevels = 1; dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT;
        dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED so the SSAO pass can read view-space depth from this buffer (the
        // depth pre-pass writes it; SSAO reconstructs view position + normals).
        dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // GI snapshots depth for temporal reproject
        VmaAllocationCreateInfo daci{};
        daci.usage = VMA_MEMORY_USAGE_AUTO;
        daci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&dici, &daci, &m_depthImg, &m_depthAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] depth image create failed"); return false;
        }
        VkImageViewCreateInfo dvci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        dvci.image = m_depthImg; dvci.viewType = VK_IMAGE_VIEW_TYPE_2D; dvci.format = m_depthFormat;
        dvci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &dvci, nullptr, &m_depthView) != VK_SUCCESS) {
            logError("[rhi] depth view create failed"); return false;
        }
        return true;
    }

void VulkanRenderDevice::destroySwapchain() {
        if (m_depthView) { vkDestroyImageView(m_dev.device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
        if (m_depthImg)  { vmaDestroyImage(m_alloc, m_depthImg, m_depthAlloc); m_depthImg = VK_NULL_HANDLE; m_depthAlloc = nullptr; }
        for (auto v : m_swapViews) if (v) vkDestroyImageView(m_dev.device, v, nullptr);
        m_swapViews.clear();
        for (auto s : m_renderFinished) if (s) vkDestroySemaphore(m_dev.device, s, nullptr);
        m_renderFinished.clear();
        if (m_swapchain) { vkDestroySwapchainKHR(m_dev.device, m_swapchain, nullptr); m_swapchain = VK_NULL_HANDLE; }
    }

void VulkanRenderDevice::recreateSwapchain() {
        // ZERO-STUTTER: declared recreate boundary (resize/vsync). Extent-tracking
        // targets are reallocated here — exempt from the strict late-create assert,
        // flagged for the spike log. No PIPELINES are created (dynamic rendering;
        // all formats are extent-independent), only images/views/descriptors.
        m_creationBoundary = true;
        m_recreatedThisFrame = true;
        vkDeviceWaitIdle(m_dev.device);
        createSwapchain(m_width, m_height);
        // HDR scene + bloom mips track the frame extent — rebuild + rewrite their
        // descriptor sets after the swapchain (and m_extent) are updated.
        createBloomTargets();
        writePostDescriptors();
        // Glass set 4 references the scene-copy view (recreated above) — rewrite it.
        writeGlassDescriptors();
        // SSAO half-res targets track the extent; the depth view also changed, so
        // rewrite every SSAO descriptor that references depth/AO views.
        createSsaoTargets();
        // Reflections (if built): the target tracks the extent and the depth + TAA
        // history views changed — recreate + rewrite BEFORE writeSsaoDescriptors so
        // mesh set3 binding2 picks up the NEW refl view (not the destroyed one).
        if (m_reflBuilt) {
            if (!createReflTargets()) { destroyRefl(); m_refl.ssr = false; }
            else writeReflDescriptors();
        }
        writeSsaoDescriptors();
        // GI half-res targets + prev-depth track the extent; the depth/AO/scene
        // views changed, so rebuild + rewrite. History is invalid after a resize.
        createGiTargets();
        writeGiDescriptors();
        m_giHistoryValid = false;
        // Water samples the scene depth: the depth view changed -> rewire it.
        writeWaterDescriptors();
        // Particles sample the scene depth (soft fade): rewire on the new depth view.
        writeParticleDescriptors();
        // HZB pyramid tracks the extent + samples the (new) depth view.
        if (m_gpuCullReady) createHzbTargets();
        // RT AO (if built): half-res target tracks the extent + the depth view
        // changed, so recreate the target + rewrite all RT-AO descriptors.
        if (m_rtaoBuilt) { createRtaoTargets(); writeRtaoDescriptors(); }
        // Editor UI (if active): tell ImGui the new swapchain image count. With
        // dynamic rendering ImGui holds no per-image framebuffers and m_format is
        // stable, so no font/pipeline rebuild is needed.
        if (m_imguiInit)
            ImGui_ImplVulkan_SetMinImageCount(
                static_cast<uint32_t>(m_swapImages.empty() ? 2u : m_swapImages.size()));
        m_creationBoundary = false;
    }

bool VulkanRenderDevice::createOffscreenTarget(uint32_t w, uint32_t h) {
        if (w == 0 || h == 0) { logError("[rhi] offscreen: zero extent"); return false; }
        // Destroy any prior target first (recreate path); on first create these are
        // all null and destroy is a no-op.
        destroyOffscreenTarget();

        m_extent = { w, h };
        m_format = VK_FORMAT_B8G8R8A8_UNORM;   // match the windowed swapchain format

        // Offscreen color image: COLOR_ATTACHMENT (graph target) + TRANSFER_SRC
        // (vkCmdCopyImageToBuffer for --screenshot), single sample, optimal tiling.
        VkImageCreateInfo cici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        cici.imageType = VK_IMAGE_TYPE_2D;
        cici.format = m_format;
        cici.extent = { w, h, 1 };
        cici.mipLevels = 1; cici.arrayLayers = 1;
        cici.samples = VK_SAMPLE_COUNT_1_BIT;
        cici.tiling = VK_IMAGE_TILING_OPTIMAL;
        cici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        cici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo caci{};
        caci.usage = VMA_MEMORY_USAGE_AUTO;
        caci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&cici, &caci, &m_offscreenColorImg, &m_offscreenColorAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] offscreen color image create failed"); return false;
        }
        VkImageViewCreateInfo cvci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        cvci.image = m_offscreenColorImg; cvci.viewType = VK_IMAGE_VIEW_TYPE_2D; cvci.format = m_format;
        cvci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &cvci, nullptr, &m_offscreenColorView) != VK_SUCCESS) {
            logError("[rhi] offscreen color view create failed"); return false;
        }

        // Depth buffer sized to the offscreen color (identical to createSwapchain).
        VkImageCreateInfo dici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        dici.imageType = VK_IMAGE_TYPE_2D;
        dici.format = m_depthFormat;
        dici.extent = { w, h, 1 };
        dici.mipLevels = 1; dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT;
        dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED so the SSAO pass can read view-space depth from this buffer (the
        // depth pre-pass writes it; SSAO reconstructs view position + normals).
        dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // GI snapshots depth for temporal reproject
        VmaAllocationCreateInfo daci{};
        daci.usage = VMA_MEMORY_USAGE_AUTO;
        daci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&dici, &daci, &m_depthImg, &m_depthAlloc, nullptr) != VK_SUCCESS) {
            logError("[rhi] offscreen depth image create failed"); return false;
        }
        VkImageViewCreateInfo dvci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        dvci.image = m_depthImg; dvci.viewType = VK_IMAGE_VIEW_TYPE_2D; dvci.format = m_depthFormat;
        dvci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &dvci, nullptr, &m_depthView) != VK_SUCCESS) {
            logError("[rhi] offscreen depth view create failed"); return false;
        }
        return true;
    }

void VulkanRenderDevice::destroyOffscreenTarget() {
        if (m_depthView) { vkDestroyImageView(m_dev.device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
        if (m_depthImg)  { vmaDestroyImage(m_alloc, m_depthImg, m_depthAlloc); m_depthImg = VK_NULL_HANDLE; m_depthAlloc = nullptr; }
        if (m_offscreenColorView) { vkDestroyImageView(m_dev.device, m_offscreenColorView, nullptr); m_offscreenColorView = VK_NULL_HANDLE; }
        if (m_offscreenColorImg)  { vmaDestroyImage(m_alloc, m_offscreenColorImg, m_offscreenColorAlloc); m_offscreenColorImg = VK_NULL_HANDLE; m_offscreenColorAlloc = nullptr; }
    }

void VulkanRenderDevice::recreateOffscreenTarget() {
        // ZERO-STUTTER: declared recreate boundary (headless resize) — see
        // recreateSwapchain(). Images/views/descriptors only; no pipelines.
        m_creationBoundary = true;
        m_recreatedThisFrame = true;
        vkDeviceWaitIdle(m_dev.device);
        createOffscreenTarget(m_width, m_height);
        // The HDR scene + bloom mips are sized to the frame extent — rebuild them
        // (and rewrite the post descriptor sets) so they track the new size.
        createBloomTargets();
        writePostDescriptors();
        // Glass set 4 references the scene-copy view (recreated above) — rewrite it.
        writeGlassDescriptors();
        // SSAO half-res targets + depth-referencing descriptors track the extent.
        createSsaoTargets();
        // Reflections (if built): the target tracks the extent and the depth + TAA
        // history views were destroyed/recreated above (createOffscreenTarget +
        // createBloomTargets) — recreate + rewrite BEFORE writeSsaoDescriptors so
        // mesh set3 binding2 picks up the NEW refl view (not the destroyed one).
        // (Missing this was a live VUID-08114 source: the headless --smoketest
        // mid-run recreate left the refl compute set on destroyed views.)
        if (m_reflBuilt) {
            if (!createReflTargets()) { destroyRefl(); m_refl.ssr = false; }
            else writeReflDescriptors();
        }
        writeSsaoDescriptors();
        // GI half-res targets + prev-depth track the extent; rebuild + rewrite.
        createGiTargets();
        writeGiDescriptors();
        m_giHistoryValid = false;
        // Water samples the scene depth: the depth view changed -> rewire it.
        writeWaterDescriptors();
        // Particles sample the scene depth (soft fade): rewire on the new depth view.
        writeParticleDescriptors();
        // HZB pyramid tracks the extent + samples the (new) depth view.
        if (m_gpuCullReady) createHzbTargets();
        // RT AO (if built): the half-res target tracks the extent AND its compute +
        // apply sets reference the depth view destroyed/recreated above — recreate
        // + rewrite, exactly like the windowed recreateSwapchain() path. (Missing
        // this was a live VUID-08114 source: the headless --test-rt mid-run
        // recreate left the rtao sets on the destroyed depth view.)
        if (m_rtaoBuilt) { createRtaoTargets(); writeRtaoDescriptors(); }
        m_creationBoundary = false;
    }

bool VulkanRenderDevice::createBloomTargets() {
        destroyBloomTargets();
        const uint32_t W = m_extent.width, H = m_extent.height;
        if (W == 0 || H == 0) { logError("[rhi] bloom: zero extent"); return false; }

        // HDR scene target (full resolution). TRANSFER_SRC so the glass scene-copy
        // pass can vkCmdCopyImage it into the scene-color copy (glass refraction).
        if (!createColorTarget(kHdrFormat, W, H,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               m_hdrImg, m_hdrAlloc, m_hdrView)) {
            logError("[rhi] HDR scene target create failed"); return false;
        }

        // ---- TAA targets (full-res, HDR format match) -----------------------
        // OUTPUT: the resolve renders into it, AE/bloom/composite sample it, and
        // the history-copy reads it (TRANSFER_SRC). HISTORY: persists across
        // frames; written only by the history-copy (TRANSFER_DST), sampled by the
        // next frame's resolve. (Re)created with the extent -> history is invalid.
        if (!createColorTarget(kHdrFormat, W, H,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               m_taaOutImg, m_taaOutAlloc, m_taaOutView)) {
            logError("[rhi] TAA output target create failed"); return false;
        }
        if (!createColorTarget(kHdrFormat, W, H,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               m_taaHistImg, m_taaHistAlloc, m_taaHistView)) {
            logError("[rhi] TAA history target create failed"); return false;
        }
        m_taaHistState = ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
        m_taaHistoryValid = false;

        // Bloom mips: mip0 = half res, each subsequent halves again (min 1px).
        uint32_t mw = W, mh = H;
        for (uint32_t i = 0; i < kBloomMips; ++i) {
            mw = std::max(1u, mw / 2);
            mh = std::max(1u, mh / 2);
            m_bloomMips[i].extent = { mw, mh };
            if (!createColorTarget(kHdrFormat, mw, mh,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   m_bloomMips[i].img, m_bloomMips[i].alloc, m_bloomMips[i].view)) {
                logError("[rhi] bloom mip create failed"); return false;
            }
        }

        // ---- Scene-color COPY target (glass refraction/frost, spec §3.1) -----
        // A mip-chained HDR image: mip0 (full-res) receives a vkCmdCopyImage of the
        // opaque HDR scene; mips 1..N are progressively blurred (M4 frost). Usage:
        // TRANSFER_DST (copy into mip0) + SAMPLED (glass reads) + COLOR_ATTACHMENT
        // (frost blur passes render into mips 1..N). One image, per-mip + full-chain
        // views. Graceful: failure leaves it NULL (the glass pass falls back to
        // sampling without refraction).
        if (!createSceneCopyTarget(W, H)) {
            // Non-fatal: clean up partial state; glass will run without refraction.
            destroySceneCopyTarget();
            logError("[rhi] scene-color copy target create failed — glass refraction disabled");
        }
        return true;
    }

bool VulkanRenderDevice::createSceneCopyTarget(uint32_t W, uint32_t H) {
        // Full-res, single-mip copy: TRANSFER_DST (copy target), SAMPLED (refraction
        // read), TRANSFER_SRC (frost level 0 reads it as the blur source).
        if (!createColorTarget(kHdrFormat, W, H,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               m_sceneCopyImg, m_sceneCopyAlloc, m_sceneCopyView)) {
            return false;
        }
        // Frost levels: each half the previous (level0 = half-res). COLOR_ATTACHMENT
        // (blur render target) + SAMPLED (sampled by the next level + the glass shader).
        uint32_t mw = W, mh = H;
        for (uint32_t i = 0; i < kGlassFrostLevels; ++i) {
            mw = std::max(1u, mw / 2); mh = std::max(1u, mh / 2);
            m_glassFrostExt[i] = { mw, mh };
            if (!createColorTarget(kHdrFormat, mw, mh,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   m_glassFrostImg[i], m_glassFrostAlloc[i], m_glassFrostView[i])) {
                return false;
            }
        }
        return true;
    }

void VulkanRenderDevice::destroySceneCopyTarget() {
        if (m_sceneCopyView) { vkDestroyImageView(m_dev.device, m_sceneCopyView, nullptr); m_sceneCopyView = VK_NULL_HANDLE; }
        if (m_sceneCopyImg) { vmaDestroyImage(m_alloc, m_sceneCopyImg, m_sceneCopyAlloc); m_sceneCopyImg = VK_NULL_HANDLE; m_sceneCopyAlloc = nullptr; }
        for (uint32_t i = 0; i < kGlassFrostLevels; ++i) {
            if (m_glassFrostView[i]) { vkDestroyImageView(m_dev.device, m_glassFrostView[i], nullptr); m_glassFrostView[i] = VK_NULL_HANDLE; }
            if (m_glassFrostImg[i])  { vmaDestroyImage(m_alloc, m_glassFrostImg[i], m_glassFrostAlloc[i]); m_glassFrostImg[i] = VK_NULL_HANDLE; m_glassFrostAlloc[i] = nullptr; }
            m_glassFrostExt[i] = {};
        }
    }

void VulkanRenderDevice::destroyBloomTargets() {
        for (uint32_t i = 0; i < kBloomMips; ++i) {
            BloomMip& m = m_bloomMips[i];
            if (m.view)  { vkDestroyImageView(m_dev.device, m.view, nullptr); m.view = VK_NULL_HANDLE; }
            if (m.img)   { vmaDestroyImage(m_alloc, m.img, m.alloc); m.img = VK_NULL_HANDLE; m.alloc = nullptr; }
            m.extent = {};
        }
        destroySceneCopyTarget();
        if (m_taaOutView)  { vkDestroyImageView(m_dev.device, m_taaOutView, nullptr); m_taaOutView = VK_NULL_HANDLE; }
        if (m_taaOutImg)   { vmaDestroyImage(m_alloc, m_taaOutImg, m_taaOutAlloc); m_taaOutImg = VK_NULL_HANDLE; m_taaOutAlloc = nullptr; }
        if (m_taaHistView) { vkDestroyImageView(m_dev.device, m_taaHistView, nullptr); m_taaHistView = VK_NULL_HANDLE; }
        if (m_taaHistImg)  { vmaDestroyImage(m_alloc, m_taaHistImg, m_taaHistAlloc); m_taaHistImg = VK_NULL_HANDLE; m_taaHistAlloc = nullptr; }
        m_taaHistState = ResourceState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
        m_taaHistoryValid = false;
        if (m_hdrView) { vkDestroyImageView(m_dev.device, m_hdrView, nullptr); m_hdrView = VK_NULL_HANDLE; }
        if (m_hdrImg)  { vmaDestroyImage(m_alloc, m_hdrImg, m_hdrAlloc); m_hdrImg = VK_NULL_HANDLE; m_hdrAlloc = nullptr; }
    }

bool VulkanRenderDevice::createGlassResources() {
        // Mip-aware LINEAR clamp sampler: the frost lookup (M4) samples an explicit
        // LOD; CLAMP_TO_EDGE so a refraction offset near the screen edge doesn't wrap.
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0.0f; sci.maxLod = 0.0f;   // single-mip scene copy + frost levels
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_glassCopySampler) != VK_SUCCESS) {
            logError("[rhi] glass scene-copy sampler failed"); return false;
        }
        // Per-frame GlassControl UBOs (host-visible, persistently mapped).
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(GlassControl); bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo ainfo{};
            if (x3vmaCreateBuffer(&bci, &aci, &m_glassCtrlBuf[i], &m_glassCtrlAlloc[i], &ainfo) != VK_SUCCESS) {
                logError("[rhi] glass control UBO alloc failed"); return false;
            }
            m_glassCtrlMapped[i] = ainfo.pMappedData;
        }
        // Descriptor pool: per-frame glass sets (2 image samplers [scene copy +
        // frost] + 1 UBO each) PLUS the frost downsample src sets (one single-sampler
        // set per frost level, reusing m_postSetLayout1).
        VkDescriptorPoolSize ps[2]{
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 2 + kGlassFrostLevels },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight } };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = kFramesInFlight + kGlassFrostLevels; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        if (x3CreateDescriptorPool(&pci, nullptr, &m_glassPool) != VK_SUCCESS) {
            logError("[rhi] glass descriptor pool failed"); return false;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_glassPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_glassSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_glassSet[i]) != VK_SUCCESS) {
                logError("[rhi] glass descriptor set alloc failed"); return false;
            }
        }
        // Frost (M4) downsample src sets: one single-sampler set per output level
        // (set[i] samples the SOURCE of level i). Reuses the bloom-down pipeline +
        // layout (m_bloomDownPipe / m_postSetLayout1). Only built when the post
        // single-sampler layout + scene copy exist; otherwise frost stays off.
        if (m_postSetLayout1 && m_bloomDownPipe && m_sceneCopyImg) {
            bool ok = true;
            for (uint32_t i = 0; i < kGlassFrostLevels && ok; ++i) {
                VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                ai.descriptorPool = m_glassPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_postSetLayout1;
                ok = (vkAllocateDescriptorSets(m_dev.device, &ai, &m_glassFrostSrcSet[i]) == VK_SUCCESS);
            }
            // Reuse the existing bloom downsample pipeline for the frost blur (same
            // shader, layout, HDR format, dynamic viewport). NULL -> frost disabled.
            m_glassFrostPipe = ok ? m_bloomDownPipe : VK_NULL_HANDLE;
        }
        writeGlassDescriptors();
        return true;
    }

void VulkanRenderDevice::writeGlassDescriptors() {
        if (!m_glassPool || !m_glassCopySampler) return;
        VkImageView copyView = m_sceneCopyView ? m_sceneCopyView : m_hdrView;
        // Frostiest blur level for binding 2 (M4). Only bind the frost image when the
        // frost chain actually RUNS (m_glassFrostPipe set), so its layout is
        // transitioned to SHADER_READ_ONLY by the glass pass. Otherwise fall back to
        // the sharp copy (the shader's frostReady flag is also 0, so the lerp is a
        // no-op) — never bind an untransitioned image.
        VkImageView frostView = (m_glassFrostPipe && m_glassFrostView[kGlassFrostLevels - 1])
                                ? m_glassFrostView[kGlassFrostLevels - 1] : copyView;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_glassSet[i]) continue;
            VkDescriptorImageInfo di{ m_glassCopySampler, copyView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo df{ m_glassCopySampler, frostView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo bi{ m_glassCtrlBuf[i], 0, sizeof(GlassControl) };
            VkWriteDescriptorSet w[3]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = m_glassSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &di;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = m_glassSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[2].dstSet = m_glassSet[i]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &df;
            vkUpdateDescriptorSets(m_dev.device, 3, w, 0, nullptr);
        }
        // Frost downsample SOURCE sets: set[0] samples the scene copy, set[i] samples
        // frost level i-1 (the previous, larger level). Written here so they track the
        // recreated views on resize.
        for (uint32_t i = 0; i < kGlassFrostLevels; ++i) {
            if (!m_glassFrostSrcSet[i]) continue;
            VkImageView src = (i == 0) ? copyView : m_glassFrostView[i - 1];
            if (!src) src = copyView;
            VkDescriptorImageInfo si{ m_glassCopySampler, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_glassFrostSrcSet[i]; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &si;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        }
    }

void VulkanRenderDevice::destroyGlassResources() {
        if (m_glassPool) { vkDestroyDescriptorPool(m_dev.device, m_glassPool, nullptr); m_glassPool = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            m_glassSet[i] = VK_NULL_HANDLE;
            if (m_glassCtrlBuf[i]) { vmaDestroyBuffer(m_alloc, m_glassCtrlBuf[i], m_glassCtrlAlloc[i]); m_glassCtrlBuf[i] = VK_NULL_HANDLE; m_glassCtrlAlloc[i] = nullptr; m_glassCtrlMapped[i] = nullptr; }
        }
        // Frost src sets came from m_glassPool (freed above); the pipe is an ALIAS of
        // m_bloomDownPipe (owned by destroyPost) — clear, don't destroy.
        for (uint32_t i = 0; i < kGlassFrostLevels; ++i) m_glassFrostSrcSet[i] = VK_NULL_HANDLE;
        m_glassFrostPipe = VK_NULL_HANDLE;
        if (m_glassCopySampler) { vkDestroySampler(m_dev.device, m_glassCopySampler, nullptr); m_glassCopySampler = VK_NULL_HANDLE; }
    }

bool VulkanRenderDevice::createColorTarget(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                       VkImage& outImg, VmaAllocation& outAlloc, VkImageView& outView) {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = { w, h, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (x3vmaCreateImage(&ici, &aci, &outImg, &outAlloc, nullptr) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = outImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &vci, nullptr, &outView) != VK_SUCCESS) return false;
        return true;
    }

bool VulkanRenderDevice::createPost() {
        // CLAMP_TO_EDGE linear sampler so edge taps in the down/up filters do not
        // wrap (avoids bloom bleeding across screen edges).
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        if (vkCreateSampler(m_dev.device, &sci, nullptr, &m_postSampler) != VK_SUCCESS) {
            logError("[rhi] post sampler create failed"); return false;
        }

        // Descriptor set layout: 1 combined image sampler (down/up source).
        VkDescriptorSetLayoutBinding b0{};
        b0.binding = 0; b0.descriptorCount = 1;
        b0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b0.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo s1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        s1.bindingCount = 1; s1.pBindings = &b0;
        if (vkCreateDescriptorSetLayout(m_dev.device, &s1, nullptr, &m_postSetLayout1) != VK_SUCCESS) {
            logError("[rhi] post set layout (1) failed"); return false;
        }
        // Descriptor set layout: 2 combined image samplers (composite: HDR + bloom)
        // + the auto-exposure SSBO (binding 2, fragment-read).
        VkDescriptorSetLayoutBinding b2[3]{};
        b2[0].binding = 0; b2[0].descriptorCount = 1;
        b2[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b2[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b2[1].binding = 1; b2[1].descriptorCount = 1;
        b2[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b2[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b2[2].binding = 2; b2[2].descriptorCount = 1;
        b2[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b2[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo s2{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        s2.bindingCount = 3; s2.pBindings = b2;
        if (vkCreateDescriptorSetLayout(m_dev.device, &s2, nullptr, &m_postSetLayout2) != VK_SUCCESS) {
            logError("[rhi] post set layout (2) failed"); return false;
        }
        // Auto-exposure set layout: b0 = HDR scene sampler, b1 = exposure SSBO
        // (both compute-stage; the reduce/adapt runs in autoexposure.comp).
        VkDescriptorSetLayoutBinding ab[2]{};
        ab[0].binding = 0; ab[0].descriptorCount = 1;
        ab[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ab[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        ab[1].binding = 1; ab[1].descriptorCount = 1;
        ab[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ab[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo sa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        sa.bindingCount = 2; sa.pBindings = ab;
        if (vkCreateDescriptorSetLayout(m_dev.device, &sa, nullptr, &m_aeSetLayout) != VK_SUCCESS) {
            logError("[rhi] auto-exposure set layout failed"); return false;
        }
        // TAA resolve set layout: b0 = current HDR scene, b1 = history, b2 = depth
        // (all fragment samplers) + b3 = the per-frame TAA UBO (matrices/params).
        VkDescriptorSetLayoutBinding tb[4]{};
        for (uint32_t i = 0; i < 3; ++i) {
            tb[i].binding = i; tb[i].descriptorCount = 1;
            tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        tb[3].binding = 3; tb[3].descriptorCount = 1;
        tb[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        tb[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo st{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        st.bindingCount = 4; st.pBindings = tb;
        if (vkCreateDescriptorSetLayout(m_dev.device, &st, nullptr, &m_taaSetLayout) != VK_SUCCESS) {
            logError("[rhi] TAA set layout failed"); return false;
        }
        // NEAREST clamp sampler for reading the depth image as data in the TAA
        // resolve (same role as the SSAO/water depth samplers; own instance so the
        // post stack stays self-contained).
        VkSamplerCreateInfo tds{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        tds.magFilter = VK_FILTER_NEAREST; tds.minFilter = VK_FILTER_NEAREST;
        tds.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        tds.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        tds.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        tds.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_dev.device, &tds, nullptr, &m_taaDepthSampler) != VK_SUCCESS) {
            logError("[rhi] TAA depth sampler failed"); return false;
        }

        // Descriptor pool: (HDR set + kBloomMips mip sets + TAA-out set)
        // single-sampler sets + 2 composite sets (2 samplers + 1 SSBO each: raw-HDR
        // + TAA variants) + 2 auto-exposure sets (1 sampler + 1 SSBO each) + the
        // per-frame TAA resolve sets (3 samplers + 1 UBO each). Sized exactly; no
        // UPDATE_AFTER_BIND needed.
        const uint32_t single = 1 + kBloomMips + 1;     // HDR + each mip + TAA out
        VkDescriptorPoolSize ps[3]{
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              single + 2*2 + 1*2 + 3*kFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         4 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight },
        };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = single + 4 + kFramesInFlight; pci.poolSizeCount = 3; pci.pPoolSizes = ps;
        if (x3CreateDescriptorPool(&pci, nullptr, &m_postPool) != VK_SUCCESS) {
            logError("[rhi] post desc pool failed"); return false;
        }
        // Allocate the single-sampler sets (HDR + mips) and the composite set.
        auto alloc1 = [&](VkDescriptorSet& out) -> bool {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_postPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_postSetLayout1;
            return vkAllocateDescriptorSets(m_dev.device, &ai, &out) == VK_SUCCESS;
        };
        if (!alloc1(m_setHdr)) { logError("[rhi] post set alloc (hdr) failed"); return false; }
        for (uint32_t i = 0; i < kBloomMips; ++i)
            if (!alloc1(m_setMip[i])) { logError("[rhi] post set alloc (mip) failed"); return false; }
        if (!alloc1(m_setTaaOut)) { logError("[rhi] post set alloc (taa-out) failed"); return false; }
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_postPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_postSetLayout2;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_setComposite) != VK_SUCCESS) {
                logError("[rhi] post set alloc (composite) failed"); return false;
            }
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_setCompositeTaa) != VK_SUCCESS) {
                logError("[rhi] post set alloc (composite-taa) failed"); return false;
            }
        }
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_postPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_aeSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_aeSet) != VK_SUCCESS) {
                logError("[rhi] post set alloc (auto-exposure) failed"); return false;
            }
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_aeSetTaa) != VK_SUCCESS) {
                logError("[rhi] post set alloc (auto-exposure-taa) failed"); return false;
            }
        }
        // Per-frame TAA resolve sets + their host-mapped UBOs (matrices change
        // every frame; one buffer per frame-in-flight so a write never races the
        // GPU's read of the previous frame's set).
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = m_postPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_taaSetLayout;
            if (vkAllocateDescriptorSets(m_dev.device, &ai, &m_taaSet[i]) != VK_SUCCESS) {
                logError("[rhi] post set alloc (taa) failed"); return false;
            }
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = sizeof(TaaUBO); bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo ainfo{};
            if (x3vmaCreateBuffer(&bci, &aci, &m_taaUboBuf[i], &m_taaUboAlloc[i], &ainfo) != VK_SUCCESS) {
                logError("[rhi] TAA UBO alloc failed"); return false;
            }
            m_taaUboMapped[i] = ainfo.pMappedData;
        }

        // Auto-exposure SSBO: 16 bytes { adapted, avgLog, pad, pad }, persistent
        // across frames (the temporal adaptation state). Host-mapped so the initial
        // neutral value (exposure 1.0) is written without a staging submit; the GPU
        // then owns it (compute writes, composite reads). Tiny + once-per-frame —
        // host-visible memory is irrelevant to performance here.
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = 16; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (x3vmaCreateBuffer(&bci, &aci, &m_aeBuf, &m_aeAlloc, &info) != VK_SUCCESS) {
                logError("[rhi] auto-exposure buffer create failed"); return false;
            }
            float init[4] = { 1.0f, 0.0f, 0.0f, 0.0f };   // neutral exposure
            std::memcpy(info.pMappedData, init, sizeof(init));
        }

        // Auto-exposure compute pipeline (autoexposure.comp).
        {
            VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AePush) };
            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1; pl.pSetLayouts = &m_aeSetLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(m_dev.device, &pl, nullptr, &m_aeLayout) != VK_SUCCESS) {
                logError("[rhi] auto-exposure pipeline layout failed"); return false;
            }
            VkShaderModule cs = loadShaderModule("shaders\\autoexposure.comp.spv");
            if (!cs) { logError("[rhi] autoexposure.comp.spv load failed"); return false; }
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = cs; cpci.stage.pName = "main";
            cpci.layout = m_aeLayout;
            VkResult pr = x3CreateComputePipelines(1, &cpci, nullptr, &m_aePipe);
            vkDestroyShaderModule(m_dev.device, cs, nullptr);
            if (pr != VK_SUCCESS) { logError("[rhi] auto-exposure pipeline create failed"); return false; }
        }

        // Pipeline layouts (push constants for tunables).
        VkPushConstantRange pcBloom{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPush) };
        VkPipelineLayoutCreateInfo bl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        bl.setLayoutCount = 1; bl.pSetLayouts = &m_postSetLayout1;
        bl.pushConstantRangeCount = 1; bl.pPushConstantRanges = &pcBloom;
        if (vkCreatePipelineLayout(m_dev.device, &bl, nullptr, &m_bloomLayout) != VK_SUCCESS) {
            logError("[rhi] bloom pipeline layout failed"); return false;
        }
        VkPushConstantRange pcComp{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CompositePush) };
        VkPipelineLayoutCreateInfo cl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        cl.setLayoutCount = 1; cl.pSetLayouts = &m_postSetLayout2;
        cl.pushConstantRangeCount = 1; cl.pPushConstantRanges = &pcComp;
        if (vkCreatePipelineLayout(m_dev.device, &cl, nullptr, &m_compositeLayout) != VK_SUCCESS) {
            logError("[rhi] composite pipeline layout failed"); return false;
        }

        // Build the three full-screen-triangle pipelines.
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\bloom_down.frag.spv",
                                      m_bloomLayout, kHdrFormat, /*additiveBlend=*/false, m_bloomDownPipe))
            return false;
        // Upsample is ADDITIVELY blended onto the larger mip (ONE,ONE) so the
        // graph's load-op keeps the existing content and the driver combines.
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\bloom_up.frag.spv",
                                      m_bloomLayout, kHdrFormat, /*additiveBlend=*/true, m_bloomUpPipe))
            return false;
        // Composite writes the LDR final target (m_format).
        if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\composite.frag.spv",
                                      m_compositeLayout, m_format, /*additiveBlend=*/false, m_compositePipe))
            return false;

        // TAA resolve: full-screen pass writing the HDR-format TAA output. All
        // parameters ride in the per-frame UBO -> no push constants.
        {
            VkPipelineLayoutCreateInfo tl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            tl.setLayoutCount = 1; tl.pSetLayouts = &m_taaSetLayout;
            if (vkCreatePipelineLayout(m_dev.device, &tl, nullptr, &m_taaLayout) != VK_SUCCESS) {
                logError("[rhi] TAA pipeline layout failed"); return false;
            }
            if (!createFullscreenPipeline("shaders\\fullscreen.vert.spv", "shaders\\taa_resolve.frag.spv",
                                          m_taaLayout, kHdrFormat, /*additiveBlend=*/false, m_taaPipe))
                return false;
        }

        logInfo("[rhi] HDR post pipeline ready (R16G16B16A16_SFLOAT scene + " +
                std::to_string(kBloomMips) + "-mip bloom + TAA resolve + ACES composite)");
        return true;
    }

bool VulkanRenderDevice::createFullscreenPipeline(const char* vsPath, const char* fsPath,
                              VkPipelineLayout layout, VkFormat colorFmt,
                              bool additiveBlend, VkPipeline& outPipe) {
        VkShaderModule vs = loadShaderModule(vsPath);
        VkShaderModule fs = loadShaderModule(fsPath);
        if (!vs || !fs) return false;

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
        dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        if (additiveBlend) {
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        } else {
            cba.blendEnable = VK_FALSE;
        }
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &colorFmt;
        prci.depthAttachmentFormat = VK_FORMAT_UNDEFINED;   // no depth in post passes

        VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpci.pNext = &prci;
        gpci.stageCount = 2; gpci.pStages = stages;
        gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp; gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms; gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb; gpci.pDynamicState = &ds; gpci.layout = layout;
        VkResult pr = x3CreateGraphicsPipelines(1, &gpci, nullptr, &outPipe);

        vkDestroyShaderModule(m_dev.device, vs, nullptr);
        vkDestroyShaderModule(m_dev.device, fs, nullptr);
        if (pr != VK_SUCCESS) { logError("[rhi] post pipeline create failed"); return false; }
        return true;
    }

void VulkanRenderDevice::writePostDescriptors() {
        auto write1 = [&](VkDescriptorSet set, VkImageView view) {
            VkDescriptorImageInfo dii{ m_postSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
            vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        };
        write1(m_setHdr, m_hdrView);
        for (uint32_t i = 0; i < kBloomMips; ++i) write1(m_setMip[i], m_bloomMips[i].view);
        write1(m_setTaaOut, m_taaOutView);   // bloom bright-pass source when TAA is on

        // Composite set: binding 0 = HDR scene, binding 1 = bloom mip0,
        // binding 2 = auto-exposure SSBO.
        VkDescriptorImageInfo d0{ m_postSampler, m_hdrView,           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo d1{ m_postSampler, m_bloomMips[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo db{ m_aeBuf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet cw[3]{};
        cw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cw[0].dstSet = m_setComposite; cw[0].dstBinding = 0; cw[0].descriptorCount = 1;
        cw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; cw[0].pImageInfo = &d0;
        cw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cw[1].dstSet = m_setComposite; cw[1].dstBinding = 1; cw[1].descriptorCount = 1;
        cw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; cw[1].pImageInfo = &d1;
        cw[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cw[2].dstSet = m_setComposite; cw[2].dstBinding = 2; cw[2].descriptorCount = 1;
        cw[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cw[2].pBufferInfo = &db;
        vkUpdateDescriptorSets(m_dev.device, 3, cw, 0, nullptr);

        // Auto-exposure set: b0 = HDR scene (sampled by the compute reduce; the
        // view changes on resize, hence rewritten here), b1 = the exposure SSBO.
        VkDescriptorImageInfo a0{ m_postSampler, m_hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet aw[2]{};
        aw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        aw[0].dstSet = m_aeSet; aw[0].dstBinding = 0; aw[0].descriptorCount = 1;
        aw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; aw[0].pImageInfo = &a0;
        aw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        aw[1].dstSet = m_aeSet; aw[1].dstBinding = 1; aw[1].descriptorCount = 1;
        aw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; aw[1].pBufferInfo = &db;
        vkUpdateDescriptorSets(m_dev.device, 2, aw, 0, nullptr);

        // ---- TAA variants + per-frame resolve sets ---------------------------
        // Composite-TAA set: binding 0 = the TAA RESOLVE OUTPUT (instead of the
        // raw HDR scene), binding 1 = bloom mip0, binding 2 = AE SSBO.
        VkDescriptorImageInfo t0{ m_postSampler, m_taaOutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet tw[3]{};
        tw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[0].dstSet = m_setCompositeTaa; tw[0].dstBinding = 0; tw[0].descriptorCount = 1;
        tw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw[0].pImageInfo = &t0;
        tw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[1].dstSet = m_setCompositeTaa; tw[1].dstBinding = 1; tw[1].descriptorCount = 1;
        tw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw[1].pImageInfo = &d1;
        tw[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[2].dstSet = m_setCompositeTaa; tw[2].dstBinding = 2; tw[2].descriptorCount = 1;
        tw[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; tw[2].pBufferInfo = &db;
        vkUpdateDescriptorSets(m_dev.device, 3, tw, 0, nullptr);

        // AE-TAA set: meter the TAA output (b0) + the same exposure SSBO (b1).
        VkWriteDescriptorSet atw[2]{};
        atw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        atw[0].dstSet = m_aeSetTaa; atw[0].dstBinding = 0; atw[0].descriptorCount = 1;
        atw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; atw[0].pImageInfo = &t0;
        atw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        atw[1].dstSet = m_aeSetTaa; atw[1].dstBinding = 1; atw[1].descriptorCount = 1;
        atw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; atw[1].pBufferInfo = &db;
        vkUpdateDescriptorSets(m_dev.device, 2, atw, 0, nullptr);

        // Per-frame TAA resolve sets: b0 = current HDR scene, b1 = history,
        // b2 = scene depth (sampled as data in DEPTH_READ_ONLY), b3 = that
        // frame's TAA UBO. Views change on resize -> rewritten here every time.
        VkDescriptorImageInfo r0{ m_postSampler,     m_hdrView,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo r1{ m_postSampler,     m_taaHistView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo r2{ m_taaDepthSampler, m_depthView,   VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!m_taaSet[i] || !m_taaUboBuf[i]) continue;
            VkDescriptorBufferInfo rb{ m_taaUboBuf[i], 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet rw[4]{};
            for (uint32_t b = 0; b < 3; ++b) {
                rw[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                rw[b].dstSet = m_taaSet[i]; rw[b].dstBinding = b; rw[b].descriptorCount = 1;
                rw[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            rw[0].pImageInfo = &r0; rw[1].pImageInfo = &r1; rw[2].pImageInfo = &r2;
            rw[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rw[3].dstSet = m_taaSet[i]; rw[3].dstBinding = 3; rw[3].descriptorCount = 1;
            rw[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; rw[3].pBufferInfo = &rb;
            vkUpdateDescriptorSets(m_dev.device, 4, rw, 0, nullptr);
        }
    }

void VulkanRenderDevice::destroyPost() {
        destroyBloomTargets();
        if (m_taaPipe)        { vkDestroyPipeline(m_dev.device, m_taaPipe, nullptr); m_taaPipe = VK_NULL_HANDLE; }
        if (m_taaLayout)      { vkDestroyPipelineLayout(m_dev.device, m_taaLayout, nullptr); m_taaLayout = VK_NULL_HANDLE; }
        if (m_taaSetLayout)   { vkDestroyDescriptorSetLayout(m_dev.device, m_taaSetLayout, nullptr); m_taaSetLayout = VK_NULL_HANDLE; }
        if (m_taaDepthSampler){ vkDestroySampler(m_dev.device, m_taaDepthSampler, nullptr); m_taaDepthSampler = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (m_taaUboBuf[i]) { vmaDestroyBuffer(m_alloc, m_taaUboBuf[i], m_taaUboAlloc[i]); m_taaUboBuf[i] = VK_NULL_HANDLE; m_taaUboAlloc[i] = nullptr; m_taaUboMapped[i] = nullptr; }
        }
        if (m_aePipe)         { vkDestroyPipeline(m_dev.device, m_aePipe, nullptr); m_aePipe = VK_NULL_HANDLE; }
        if (m_aeLayout)       { vkDestroyPipelineLayout(m_dev.device, m_aeLayout, nullptr); m_aeLayout = VK_NULL_HANDLE; }
        if (m_aeBuf)          { vmaDestroyBuffer(m_alloc, m_aeBuf, m_aeAlloc); m_aeBuf = VK_NULL_HANDLE; m_aeAlloc = nullptr; }
        if (m_aeSetLayout)    { vkDestroyDescriptorSetLayout(m_dev.device, m_aeSetLayout, nullptr); m_aeSetLayout = VK_NULL_HANDLE; }
        if (m_compositePipe)  { vkDestroyPipeline(m_dev.device, m_compositePipe, nullptr); m_compositePipe = VK_NULL_HANDLE; }
        if (m_bloomUpPipe)    { vkDestroyPipeline(m_dev.device, m_bloomUpPipe, nullptr); m_bloomUpPipe = VK_NULL_HANDLE; }
        if (m_bloomDownPipe)  { vkDestroyPipeline(m_dev.device, m_bloomDownPipe, nullptr); m_bloomDownPipe = VK_NULL_HANDLE; }
        if (m_compositeLayout){ vkDestroyPipelineLayout(m_dev.device, m_compositeLayout, nullptr); m_compositeLayout = VK_NULL_HANDLE; }
        if (m_bloomLayout)    { vkDestroyPipelineLayout(m_dev.device, m_bloomLayout, nullptr); m_bloomLayout = VK_NULL_HANDLE; }
        if (m_postPool)       { vkDestroyDescriptorPool(m_dev.device, m_postPool, nullptr); m_postPool = VK_NULL_HANDLE; }
        if (m_postSetLayout2) { vkDestroyDescriptorSetLayout(m_dev.device, m_postSetLayout2, nullptr); m_postSetLayout2 = VK_NULL_HANDLE; }
        if (m_postSetLayout1) { vkDestroyDescriptorSetLayout(m_dev.device, m_postSetLayout1, nullptr); m_postSetLayout1 = VK_NULL_HANDLE; }
        if (m_postSampler)    { vkDestroySampler(m_dev.device, m_postSampler, nullptr); m_postSampler = VK_NULL_HANDLE; }
    }

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

void VulkanRenderDevice::writeMeshTlasDescriptor() {
        if (!m_rtSupported) return;
        VkAccelerationStructureKHR tlas = m_rt.tlas();
        if (!tlas || !m_meshAoSet[0]) return;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
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
            VkDescriptorSetLayoutBinding binds[3]{};
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
            VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            slci.bindingCount = 3; slci.pBindings = binds;
            if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_objSetLayout) != VK_SUCCESS) {
                logError("[rhi] object set layout failed"); return false;
            }

            VkDescriptorPoolSize sizes[2]{};
            sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[0].descriptorCount = 2 * kFramesInFlight;
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
                VkWriteDescriptorSet writes[3]{};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = fr.objSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &sbi;
                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = fr.objSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[1].pBufferInfo = &cbi;
                writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[2].dstSet = fr.objSet; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &vbi;
                vkUpdateDescriptorSets(m_dev.device, 3, writes, 0, nullptr);
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
            VkDescriptorSetLayoutBinding b[6]{};
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
            b[5].binding = 5; b[5].descriptorCount = 1;
            b[5].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            ci.bindingCount = m_rtSupported ? 6u : 5u; ci.pBindings = b;
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
            // Glass pipeline layout: the 4 shared mesh sets + the glass-only set 4.
            VkDescriptorSetLayout glassSets[5] = {
                m_bindlessLayout, m_objSetLayout, m_shadowSetLayout,
                m_meshAoSetLayout, m_glassSetLayout };
            VkPipelineLayoutCreateInfo gplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            gplci.setLayoutCount = 5; gplci.pSetLayouts = glassSets;
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
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = m_shadowFormat;
        ici.extent = { kShadowDim, kShadowDim, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1;
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
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = m_shadowImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = m_shadowFormat;
        vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_dev.device, &vci, nullptr, &m_shadowView) != VK_SUCCESS) {
            logError("[rhi] shadow view create failed"); return false;
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

        // Set-2 layout: a single combined-image-sampler (sampler2DShadow) in frag.
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorCount = 1;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 1; slci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(m_dev.device, &slci, nullptr, &m_shadowSetLayout) != VK_SUCCESS) {
            logError("[rhi] shadow set layout failed"); return false;
        }
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (x3CreateDescriptorPool(&pci, nullptr, &m_shadowDescPool) != VK_SUCCESS) {
            logError("[rhi] shadow desc pool failed"); return false;
        }
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = m_shadowDescPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_shadowSetLayout;
        if (vkAllocateDescriptorSets(m_dev.device, &dsai, &m_shadowSet) != VK_SUCCESS) {
            logError("[rhi] shadow set alloc failed"); return false;
        }
        // The shadow map's sampled layout is DEPTH_READ_ONLY_OPTIMAL (it's never a
        // color/general image); write the descriptor once with that layout. The
        // per-frame barrier leaves the image in exactly this layout before the
        // main pass samples it.
        VkDescriptorImageInfo dii{};
        dii.sampler = m_shadowSampler;
        dii.imageView = m_shadowView;
        dii.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_shadowSet; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
        vkUpdateDescriptorSets(m_dev.device, 1, &w, 0, nullptr);
        return true;
    }

bool VulkanRenderDevice::createShadowPipeline() {
        VkShaderModule vs = loadShaderModule("shaders\\shadow.vert.spv");
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
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_objSetLayout;
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

        logInfo("[rhi] directional shadow pipeline ready (2048^2 depth, depth-only, PCF compare sampler)");
        return true;
    }

void VulkanRenderDevice::destroyGraphics() {
        // Mesh + texture registries (created by the app via the public API).
        for (auto& kv : m_meshes) {
            Mesh& m = kv.second;
            if (m.dynamic) {
                for (uint32_t i = 0; i < kFramesInFlight; ++i)
                    if (m.dynVbo[i]) vmaDestroyBuffer(m_alloc, m.dynVbo[i], m.dynVboAlloc[i]);
            } else if (m.vbo) {
                vmaDestroyBuffer(m_alloc, m.vbo, m.vboAlloc);
            }
            vmaDestroyBuffer(m_alloc, m.ibo, m.iboAlloc);
        }
        m_meshes.clear();
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
            if (fr.indirectBuf) { vmaDestroyBuffer(m_alloc, fr.indirectBuf, fr.indirectAlloc); fr.indirectBuf = VK_NULL_HANDLE; fr.indirectAlloc = nullptr; fr.indirectMapped = nullptr; }
        }

        if (m_objPool)        { vkDestroyDescriptorPool(m_dev.device, m_objPool, nullptr); m_objPool = VK_NULL_HANDLE; }
        if (m_objSetLayout)   { vkDestroyDescriptorSetLayout(m_dev.device, m_objSetLayout, nullptr); m_objSetLayout = VK_NULL_HANDLE; }
        if (m_bindlessPool)   { vkDestroyDescriptorPool(m_dev.device, m_bindlessPool, nullptr); m_bindlessPool = VK_NULL_HANDLE; }
        if (m_bindlessLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_bindlessLayout, nullptr); m_bindlessLayout = VK_NULL_HANDLE; }

        // Shadow mapping resources (perf-stack E).
        if (m_shadowPipeline)  { vkDestroyPipeline(m_dev.device, m_shadowPipeline, nullptr); m_shadowPipeline = VK_NULL_HANDLE; }
        if (m_shadowLayout)    { vkDestroyPipelineLayout(m_dev.device, m_shadowLayout, nullptr); m_shadowLayout = VK_NULL_HANDLE; }
        if (m_shadowDescPool)  { vkDestroyDescriptorPool(m_dev.device, m_shadowDescPool, nullptr); m_shadowDescPool = VK_NULL_HANDLE; }
        if (m_shadowSetLayout) { vkDestroyDescriptorSetLayout(m_dev.device, m_shadowSetLayout, nullptr); m_shadowSetLayout = VK_NULL_HANDLE; }
        if (m_shadowSampler)   { vkDestroySampler(m_dev.device, m_shadowSampler, nullptr); m_shadowSampler = VK_NULL_HANDLE; }
        if (m_shadowView)      { vkDestroyImageView(m_dev.device, m_shadowView, nullptr); m_shadowView = VK_NULL_HANDLE; }
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

} // namespace x3::rhi
