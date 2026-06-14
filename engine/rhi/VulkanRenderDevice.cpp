// Vulkan implementation of IRenderDevice — D1 (clean-room).
// Spec: specs/D1-render-device.spec.md
//
// #28 MONOLITH SPLIT: the ~13.8k-line inline class was carved into a shared
// declaration header (vk/VulkanRenderDevice_internal.h) so its method bodies can
// be defined across several focused translation units under engine/rhi/vk/:
//   * vk_resources.cpp — buffers/images/samplers/descriptor pools + the tracked
//     allocation wrappers (allocationCount=0 accounting lives there).
//   * vk_pipelines.cpp — ALL PSO creation (boot-time, r_strictpso, pipeline cache).
//   * vk_passes.cpp    — depth/cutout prepass, buildRtSceneAS, DDGI, reflections,
//     RT shadows, and the post stack in energy-conserving order.
//   * vk_stb_impl.cpp  — the stb single-header implementations (image-write + ttf).
//   * VulkanRenderDevice.cpp (THIS TU) — device/swapchain/frame lifecycle +
//     orchestration, plus the createRenderDevice() factory.
// The transform is behavior-preserving: every body was moved verbatim, only the
// inline->out-of-line mechanics (qualify names, hoist shared helpers) changed.

#include "vk/VulkanRenderDevice_internal.h"

namespace x3::rhi {

IRenderDevice* createRenderDevice() { return new VulkanRenderDevice(); }

} // namespace x3::rhi
