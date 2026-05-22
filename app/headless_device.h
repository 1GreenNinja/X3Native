#pragma once
// Shared headless IRenderDevice test-double (game layer).
//
// A single no-op implementation of x3::rhi::IRenderDevice for the headless
// self-tests (--test-*). It hands out monotonically-increasing valid opaque
// handles wherever a handle is required (preserving the per-test handle-id
// expectations), and treats every draw / frame / camera / param call as a
// no-op. The headless tests build levels, drive systems, and assert gameplay
// state without a window or Vulkan.
//
// WHY THIS EXISTS: each self-test .cpp used to define its own near-identical
// anonymous-namespace stub. Every time IRenderDevice gained a pure-virtual
// (water, GI, terrain-material, ...) ALL of them had to add the override or
// they went abstract and the build broke. This consolidates the stub into ONE
// place: adding a new IRenderDevice method now means editing only this base.
//
// A test that needs to OBSERVE calls (e.g. count mesh create/destroy for a leak
// check) derives a tiny local subclass and overrides just the method(s) it
// cares about; `m_next` is protected so such an override can still mint the same
// incrementing handle ids the base would have. See app/terrain.cpp.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::game {

// Single no-op IRenderDevice for all headless self-tests. Mints incrementing
// valid handles; everything else is a no-op. Match IRenderDevice EXACTLY so a
// new pure-virtual added to the interface is implemented here once.
class HeadlessRenderDevice : public x3::rhi::IRenderDevice {
public:
    bool init(const x3::rhi::DeviceDesc&) override { return true; }
    void shutdown() override {}
    void onResize(uint32_t, uint32_t) override {}
    void setVsync(bool) override {}
    void setCamera(float, float, float, float, float, float) override {}

    x3::rhi::FrameContext beginFrame() override { return {}; }
    void endFrame(const x3::rhi::FrameContext&) override {}

    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex*, uint32_t,
                                   const uint32_t*, uint32_t) override {
        return x3::rhi::MeshHandle{ m_next++ };
    }
    void destroyMesh(x3::rhi::MeshHandle) override {}
    void updateMesh(x3::rhi::MeshHandle, const x3::rhi::MeshVertex*, uint32_t) override {}

    x3::rhi::TextureHandle createTexture(const void*, uint32_t, uint32_t, bool) override {
        return x3::rhi::TextureHandle{ m_next++ };
    }
    void destroyTexture(x3::rhi::TextureHandle) override {}

    x3::rhi::TextureHandle registerTerrainMaterial(x3::rhi::TextureHandle,
                                                   x3::rhi::TextureHandle,
                                                   x3::rhi::TextureHandle,
                                                   x3::rhi::TextureHandle) override {
        return x3::rhi::TextureHandle{ m_next++ };
    }

    void drawMesh(const x3::rhi::FrameContext&, x3::rhi::MeshHandle,
                  x3::rhi::TextureHandle, const float[4], const float[16]) override {}
    void drawMeshEmissive(const x3::rhi::FrameContext&, x3::rhi::MeshHandle,
                          x3::rhi::TextureHandle, const float[4], const float[4],
                          const float[16]) override {}

    void submitParticles(const x3::rhi::IRenderDevice::ParticleInstance*, uint32_t,
                         x3::rhi::IRenderDevice::ParticleBlend) override {}
    void submitDecals(const x3::rhi::IRenderDevice::DecalInstance*, uint32_t) override {}

    // GPU-compute debris world (K-T2). No-op in the headless stub; the real
    // compute/sim path is exercised against the live Vulkan device in --test-debris.
    void gpuDebrisConfig(const x3::rhi::IRenderDevice::GpuDebrisParams&) override {}
    uint32_t gpuDebrisSpawnBurst(const float[3], uint32_t count, float, float, float,
                                 uint32_t) override { return count; }
    void gpuDebrisStep(float) override {}
    void gpuDebrisDraw(const x3::rhi::FrameContext&, const float[4]) override {}
    uint32_t gpuDebrisAliveCount() const override { return 0; }
    uint32_t gpuDebrisCapacity() const override { return 0; }
    x3::rhi::IRenderDevice::GpuDebrisStats gpuDebrisReadback(float) const override { return {}; }

    void setPointLights(const x3::rhi::PointLight*, uint32_t) override {}
    void setSkyParams(const x3::rhi::IRenderDevice::SkyParams&) override {}
    void setSsaoParams(const x3::rhi::IRenderDevice::SsaoParams&) override {}
    void setGiParams(const x3::rhi::IRenderDevice::GiParams&) override {}
    void setWaterParams(const x3::rhi::IRenderDevice::WaterParams&) override {}

    void drawHudQuad(const x3::rhi::FrameContext&, float, float, float, float, const float[4]) override {}
    void drawHudText(const x3::rhi::FrameContext&, const char*, float, float, float, const float[4]) override {}
    void hudSize(uint32_t& w, uint32_t& h) const override { w = 0; h = 0; }

    x3::rhi::RenderStats stats() const override { return {}; }

    void armCapture(const char*) override {}                   // headless: no swapchain
    bool captureFrame(const char*) override { return false; }  // headless: no swapchain

    bool supportsDescriptorIndexing() const override { return false; }
    bool supportsMeshShaders() const override { return false; }

protected:
    // Next opaque handle id to mint. Protected so a test-specific subclass that
    // overrides createMesh/createTexture (e.g. to count them) can preserve the
    // exact incrementing-handle behavior of the base.
    uint32_t m_next = 1;
};

} // namespace x3::game
