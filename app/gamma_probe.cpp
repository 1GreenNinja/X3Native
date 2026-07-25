// ===========================================================================
// GAMMA PROBE — the acceptance-gate MEASUREMENT for the LINEAR-vs-GAMMA fix
// (feat/club-gamma-fix, 2026-07-25).
//
// The root-cause fix flipped the swapchain + offscreen capture target from
// VK_FORMAT_B8G8R8A8_UNORM to VK_FORMAT_B8G8R8A8_SRGB (see engine/rhi/vk/
// vk_targets.cpp). With _SRGB, the GPU applies the sRGB OETF exactly once when
// a LINEAR value is stored — so a linear 0.5 the shaders emit lands on byte
// ~188, not 127. Screenshots REPRODUCE the darkness bug and cannot see it; the
// only valid proof is to MEASURE the stored byte for a known linear value.
//
// This probe is deliberately self-contained (its own throwaway headless Vulkan
// device) so it measures the EXACT format the swapchain now uses, independent of
// the render graph / tonemap. It clears a B8G8R8A8_SRGB image to a ramp of known
// LINEAR values, copies each to a host buffer, and reads the stored byte back.
// vkCmdClearColorImage on an sRGB image encodes the (linear) clear value per the
// Vulkan spec — the same store-time encode the swapchain composite relies on.
//
// PASS gate: linear 0.5 -> ~188 (188 = round(255 * srgb_encode(0.5))). A byte of
// 127 means the format is still UNORM (unfixed).
// ===========================================================================
#include "engine/core/x3_log.h"

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

// Reference sRGB OETF (IEC 61966-2-1) — what an _SRGB store must produce.
static int srgbByteRef(float lin) {
    float e = (lin <= 0.0031308f) ? (12.92f * lin)
                                  : (1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f);
    int b = (int)std::lround(e * 255.0f);
    return b < 0 ? 0 : (b > 255 ? 255 : b);
}

static uint32_t pickMemType(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

// Returns 0 on success (gate passed), 1 on any failure / gate miss.
int runGammaProbe() {
    const VkFormat kFmt = VK_FORMAT_B8G8R8A8_SRGB;   // the swapchain format after the fix
    const std::array<float, 5> ramp = { 0.05f, 0.10f, 0.25f, 0.50f, 0.75f };

    x3::logInfo("[gamma-probe] measuring VK_FORMAT_B8G8R8A8_SRGB store encode "
                "(swapchain format after the LINEAR-vs-GAMMA fix)...");

    // ---- headless instance + device (no surface, defer surface init) --------
    vkb::InstanceBuilder ib;
    ib.set_app_name("X3Native-gamma-probe").require_api_version(1, 1, 0)
      .request_validation_layers(false);
    auto instRet = ib.build();
    if (!instRet) { x3::logError(std::string("[gamma-probe] instance: ") + instRet.error().message()); return 1; }
    vkb::Instance inst = instRet.value();

    vkb::PhysicalDeviceSelector sel{ inst };
    sel.set_minimum_version(1, 1).defer_surface_initialization();
    auto physRet = sel.select();
    if (!physRet) { x3::logError(std::string("[gamma-probe] phys: ") + physRet.error().message()); vkb::destroy_instance(inst); return 1; }
    vkb::PhysicalDevice phys = physRet.value();
    vkb::DeviceBuilder db{ phys };
    auto devRet = db.build();
    if (!devRet) { x3::logError(std::string("[gamma-probe] device: ") + devRet.error().message()); vkb::destroy_instance(inst); return 1; }
    vkb::Device dev = devRet.value();
    VkDevice d = dev.device;
    auto q   = dev.get_queue(vkb::QueueType::graphics);
    auto qfi = dev.get_queue_index(vkb::QueueType::graphics);
    if (!q || !qfi) { x3::logError("[gamma-probe] no graphics queue"); vkb::destroy_device(dev); vkb::destroy_instance(inst); return 1; }
    VkQueue queue = q.value();
    uint32_t family = qfi.value();

    // Confirm the format supports the transfers the swapchain path uses.
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(phys.physical_device, kFmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ||
        !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
        x3::logError("[gamma-probe] B8G8R8A8_SRGB lacks transfer features on this GPU");
        vkb::destroy_device(dev); vkb::destroy_instance(inst); return 1;
    }

    int rc = 1;
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory imgMem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory bufMem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    const uint32_t W = 4, H = 4;

    do {
        // ---- sRGB image (device-local), TRANSFER_DST (clear) + TRANSFER_SRC (copy) ----
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = kFmt;
        ici.extent = { W, H, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(d, &ici, nullptr, &img) != VK_SUCCESS) { x3::logError("[gamma-probe] image create"); break; }
        VkMemoryRequirements ir{}; vkGetImageMemoryRequirements(d, img, &ir);
        uint32_t it = pickMemType(phys.physical_device, ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ia{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ia.allocationSize = ir.size; ia.memoryTypeIndex = it;
        if (vkAllocateMemory(d, &ia, nullptr, &imgMem) != VK_SUCCESS || vkBindImageMemory(d, img, imgMem, 0) != VK_SUCCESS) {
            x3::logError("[gamma-probe] image memory"); break;
        }

        // ---- host-visible readback buffer (W*H BGRA) ----
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = (VkDeviceSize)W * H * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(d, &bci, nullptr, &buf) != VK_SUCCESS) { x3::logError("[gamma-probe] buffer create"); break; }
        VkMemoryRequirements br{}; vkGetBufferMemoryRequirements(d, buf, &br);
        uint32_t bt = pickMemType(phys.physical_device, br.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo ba{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ba.allocationSize = br.size; ba.memoryTypeIndex = bt;
        if (vkAllocateMemory(d, &ba, nullptr, &bufMem) != VK_SUCCESS || vkBindBufferMemory(d, buf, bufMem, 0) != VK_SUCCESS) {
            x3::logError("[gamma-probe] buffer memory"); break;
        }

        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.queueFamilyIndex = family; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(d, &pci, nullptr, &pool) != VK_SUCCESS) { x3::logError("[gamma-probe] cmd pool"); break; }
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(d, &fci, nullptr, &fence);

        bool allOk = true;
        int measured[5] = { 0,0,0,0,0 };
        for (size_t i = 0; i < ramp.size(); ++i) {
            const float v = ramp[i];
            VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
            VkCommandBuffer cmd; vkAllocateCommandBuffers(d, &cai, &cmd);
            VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &cbi);

            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            // UNDEFINED -> TRANSFER_DST
            VkImageMemoryBarrier toDst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcAccessMask = 0; toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toDst.image = img; toDst.subresourceRange = range;
            toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toDst);

            // Clear to the LINEAR ramp value. On an _SRGB image the store encodes it.
            VkClearColorValue clr{}; clr.float32[0] = v; clr.float32[1] = v; clr.float32[2] = v; clr.float32[3] = 1.0f;
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &range);

            // TRANSFER_DST -> TRANSFER_SRC
            VkImageMemoryBarrier toSrc{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toSrc.image = img; toSrc.subresourceRange = range;
            toSrc.srcQueueFamilyIndex = toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toSrc);

            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { W, H, 1 };
            vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

            vkEndCommandBuffer(cmd);
            vkResetFences(d, 1, &fence);
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            vkQueueSubmit(queue, 1, &si, fence);
            vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX);
            vkFreeCommandBuffers(d, pool, 1, &cmd);

            void* map = nullptr;
            vkMapMemory(d, bufMem, 0, VK_WHOLE_SIZE, 0, &map);
            const uint8_t* px = static_cast<const uint8_t*>(map);   // BGRA
            const int b = px[0], g = px[1], r = px[2];
            vkUnmapMemory(d, bufMem);
            const int got = g;                          // grey clear: B==G==R
            measured[i] = got;
            const int expect = srgbByteRef(v);
            const bool ok = std::abs(got - expect) <= 2 && std::abs(b - r) <= 1 && std::abs(b - g) <= 1;
            allOk = allOk && ok;
            x3::logInfo("[gamma-probe]   linear " + std::to_string(v).substr(0, 4) +
                        " -> byte " + std::to_string(got) +
                        " (expect " + std::to_string(expect) + ", BGR " +
                        std::to_string(b) + "/" + std::to_string(g) + "/" + std::to_string(r) + ")" +
                        (ok ? "  OK" : "  <-- MISMATCH"));
        }

        // The acceptance gate: linear 0.5 MUST be ~188 (not 127).
        const bool gate = std::abs(measured[3] - 188) <= 2;
        x3::logInfo(std::string("[gamma-probe] ACCEPTANCE GATE: linear 0.5 -> byte ") +
                    std::to_string(measured[3]) + " (must be ~188; 127 = still UNORM/unfixed) -> " +
                    (gate ? "PASS" : "FAIL"));
        rc = (allOk && gate) ? 0 : 1;
    } while (false);

    if (fence) vkDestroyFence(d, fence, nullptr);
    if (pool)  vkDestroyCommandPool(d, pool, nullptr);
    if (buf)   vkDestroyBuffer(d, buf, nullptr);
    if (bufMem) vkFreeMemory(d, bufMem, nullptr);
    if (img)   vkDestroyImage(d, img, nullptr);
    if (imgMem) vkFreeMemory(d, imgMem, nullptr);
    vkb::destroy_device(dev);
    vkb::destroy_instance(inst);

    x3::logInfo(std::string("[gamma-probe] ") + (rc == 0 ? "PASS" : "FAIL"));
    return rc;
}

} // namespace x3::game
