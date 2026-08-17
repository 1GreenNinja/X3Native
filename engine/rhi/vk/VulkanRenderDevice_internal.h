// AUTO-CARVED from VulkanRenderDevice.cpp (#28 monolith split).
// Shared class declaration so its methods can be defined across vk/*.cpp TUs.
// Behavior-identical: method bodies are still inline here until moved out.
#pragma once

#include "../IRenderDevice.h"
#include "../RenderGraph.h"
#include "../Csm.h"               // cascaded-shadow-map fitting math (Vulkan-free, unit-tested)
#include "../VulkanRT.h"          // hardware ray-tracing AS manager (ray-query path)
#include "../../core/x3_log.h"
#include "../../core/x3_boot.h"   // [boot] timeline marks (device-init sub-phases)
#include "../../core/x3_cpuzones.h" // LANE 6: rdtsc CPU-zone attribution (r_speeds)
#include "../font8x8_basic.h"
#include "../font_robotomono.h"   // embedded Roboto Mono TTF (Apache-2.0) — modern HUD font

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

// Dear ImGui (docking) — EDITOR-ONLY. These headers + the GLFW/Vulkan backends are
// referenced ONLY by the editor-UI methods (initEditorUI/beginEditorUI/endEditorUI/
// shutdownEditorUI) and the editor-UI graph pass; nothing in the game/headless path
// touches them. ImGui types stay inside THIS .cpp — IRenderDevice.h never sees them.
// GLFW is forward-declared by imgui_impl_glfw.h (no GLFW include needed here; the
// host passes the GLFWwindow* as void* across the IRenderDevice boundary).
#include <imgui.h>
#include <imgui_impl_glfw.h>     // vcpkg installs the backends flat in include/ (no backends/ prefix)
#include <imgui_impl_vulkan.h>

// PNG writer for captureFrame() (--screenshot). This is the ONLY translation unit
// that defines STB_IMAGE_WRITE_IMPLEMENTATION (ModelLoader.cpp owns the matching
// STB_IMAGE_IMPLEMENTATION for the reader — they are separate stb headers).
// (STB_IMAGE_WRITE_IMPLEMENTATION defined in VulkanRenderDevice.cpp only)
#include <stb_image_write.h>

// stb_truetype: bake a crisp glyph atlas from a real TTF at device init (modern
// HUD/menu font). This is the ONLY translation unit that defines the impl macro.
// (STB_TRUETYPE_IMPLEMENTATION defined in VulkanRenderDevice.cpp only)
#include <stb_truetype.h>

#include <vector>
#include <string>
#include <cmath>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <cassert>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <chrono>
#include <mutex>   // m_uploadMu: parallel boot-time model preload (docs/BOOT_TIME.md)

// Both are now PUBLIC compile definitions on the x3core target (see
// engine/CMakeLists.txt) so EVERY translation unit agrees on the convention —
// defining it only here meant any other TU that included glm first built
// projections in OpenGL's [-1,1] Z. Kept (guarded) for standalone readers.
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#ifndef GLM_FORCE_RADIANS
#define GLM_FORCE_RADIANS
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "../FrustumCull.h"   // CPU per-object frustum cull (r_frustumcull, D15 baseline)
#include "../GpuCull.h"       // D15 GPU-driven culling (r_cullpath: Tier 0/1/2 + HZB)
#include "../ClusterLights.h" // clustered forward lighting (r_clusterlights): froxel grid + assignment

namespace x3::rhi {

namespace vkdetail {

// Compute a model-space bounding sphere from a vertex list for the CPU frustum cull:
// AABB center, then the radius is the max vertex distance from that center (a tight
// conservative sphere). Empty/null -> zero radius (treated as unbounded -> never culled).
inline void computeLocalSphere(const MeshVertex* verts, uint32_t vcount,
                               glm::vec3& outCenter, float& outRadius) {
    outCenter = glm::vec3(0.0f);
    outRadius = 0.0f;
    if (!verts || vcount == 0) return;
    glm::vec3 lo(verts[0].pos[0], verts[0].pos[1], verts[0].pos[2]);
    glm::vec3 hi = lo;
    for (uint32_t i = 1; i < vcount; ++i) {
        const glm::vec3 p(verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    outCenter = 0.5f * (lo + hi);
    float r2 = 0.0f;
    for (uint32_t i = 0; i < vcount; ++i) {
        const glm::vec3 p(verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]);
        r2 = std::max(r2, glm::dot(p - outCenter, p - outCenter));
    }
    outRadius = std::sqrt(r2);
}

// Transform a model-space bounding sphere to world space by an instance model matrix:
// center -> (model * center); radius -> radius * (max scale axis), so non-uniform
// scale stays conservative (the largest axis bounds the whole sphere). Returns the
// CullSphere (xyz = world center, w = world radius) the GPU-equivalent test consumes.
inline CullSphere worldSphere(const glm::mat4& model, const glm::vec3& cLocal, float rLocal) {
    const glm::vec3 cWorld = glm::vec3(model * glm::vec4(cLocal, 1.0f));
    // Column lengths of the upper-left 3x3 = per-axis scale (incl. shear contribution).
    const float sx = glm::length(glm::vec3(model[0]));
    const float sy = glm::length(glm::vec3(model[1]));
    const float sz = glm::length(glm::vec3(model[2]));
    const float maxScale = std::max(sx, std::max(sy, sz));
    return CullSphere(cWorld, rLocal * maxScale);
}

constexpr uint32_t kFramesInFlight = 2;
// ---- LANE 6: per-pass GPU timestamps (r_speeds) ---------------------------
// The frame's timestamp query pool used to hold exactly TWO queries (frame start
// + frame end), which is why m_cullGpuMs / m_hzbGpuMs were never assigned and the
// HUD printed 0.00 on every machine. It now holds the frame bracket PLUS a pair
// per graph pass. The busiest echotropolis frame records ~30 passes; 48 leaves
// headroom without making the readback wide.
// LANE 6 REPLAY ON MAIN (2026-08): main carries clustered lights, CSM, geo-LOD and
// reflection denoise, all of which add graph passes the 0bc0d482 base never had. 48
// silently CLAMPED (std::min in vk_graph.cpp) — an over-cap pass would vanish from
// the breakdown and the "pass sum == frame bracket" invariant would quietly fail.
// 96 leaves headroom on the widest frame this engine records.
constexpr uint32_t kMaxTimedPasses = 96;
constexpr uint32_t kFrameTsQueries = 2 + kMaxTimedPasses * 2;   // = 194
// Glass frost (M4): number of progressively-downsampled blur levels of the scene
// copy. Each is a separate single-mip image (the render-graph tracks one layout per
// image). The glass shader samples the deepest level for the frosted look.
constexpr uint32_t kGlassFrostLevels = 3;

} // namespace vkdetail
using namespace vkdetail;  // class body + out-of-line defs use helpers unqualified

class VulkanRenderDevice final : public IRenderDevice {
public:
    bool init(const DeviceDesc& desc) override;

    void shutdown() override;

    // =====================================================================
    // Editor UI (Dear ImGui, docking) — EDITOR-ONLY (Level Architect P0).
    // None of these allocate or run unless the host calls initEditorUI() (only
    // app/main.cpp does, and only with --editor). The game/headless paths never
    // call them, so the shipping game is byte-for-byte unchanged. resolved imgui
    // == vcpkg 1.92.8 (>= 1.90): fonts AUTO-UPLOAD on the first
    // ImGui_ImplVulkan_RenderDrawData and PipelineRenderingCreateInfo is an
    // InitInfo struct field — so NO explicit ImGui_ImplVulkan_CreateFontsTexture()
    // call is needed (the pre-1.90 branch).
    // =====================================================================
    void initEditorUI(void* glfwWindow) override;

    void beginEditorUI() override;

    void endEditorUI() override;

    void shutdownEditorUI() override;

    void editorWantsInput(bool& mouse, bool& kbd) const override;

    bool editorUIActive() const override;

    void onResize(uint32_t w, uint32_t h) override;

    void setVsync(bool enabled) override;

    void setCamera(float x, float y, float z, float yaw, float pitch, float fovDeg) override;
    void setCameraBasis(float x, float y, float z,
                        const float fwd[3], const float up[3], float fovDeg) override;

    void setCameraRoll(float rollRadians) override;

    void setCameraFar(float farMeters) override;   // W8-3: far-plane override

    void setAmbient(float r, float g, float b) override;

    // CPU per-object frustum cull toggle (r_frustumcull). Default ON. When OFF the
    // draw path is byte-identical to before this feature (objectsDrawn == list.size()).
    void setFrustumCullEnabled(bool enabled) override;

    // D15 GPU cull host requests (resolved per frame in prepareFrameData).
    void setCullPath(int path) override;
    void setHzbEnabled(bool enabled) override;
    void setGpuCullEquivalenceCheck(bool enabled) override;

    void setBloom(float intensity) override;

    // Whole-scene brightness: pre-tonemap exposure multiplier in the composite pass.
    // With auto-exposure ON this is the compensation BIAS on the adapted value.
    void setExposure(float e) override;
    void setDebugView(int mode) override { m_debugView = m_cvarOv.applyDebugView(mode); }

    // ---- CLI `--set` OVERRIDE LATCH (IRenderDevice::RenderCVarOverrides) ----
    // Armed once by the --world host dispatch from the parsed --set list; every
    // render-param setter above re-stamps the overridden FIELDS on its way
    // through, so a host's own write (or a per-frame one) can never silently
    // undo the command line. Disarmed (active == false) by default: a run with
    // no --set takes the identical code path it always did.
    void setCVarOverrides(const RenderCVarOverrides& ov) override { m_cvarOv = ov; }

    // Painterly levers (ART_BIBLE §5): per-zone depth fog + filmic grade. Host
    // opt-in state, deliberately outside PostFXParams (setPostFX re-applies from
    // cvars live and must not clobber a zone's atmosphere).
    void setFog(const FogParams& f) override;
    void setGrade(const GradeParams& g) override;
    // Cinematic filmic post (vignette/grain/split-tone in the composite pass,
    // post-tonemap). Cutscene-owned; enabled=false (the default) never enters
    // the shader block -> byte-identical composite.
    void setFilmic(const FilmicParams& f) override;
    // Underwater caustics (mesh.frag; rides the SsaoControl caustics lane).
    void setCaustics(const CausticsParams& c) override;
    void setWetness(const WetnessParams& w) override;
    void setSnowCover(float cover) override;

    // Metal ambient-specular floor strength (mesh.frag IBL path; rides ssao ctrl ibl.w).
    void setMetalAmbient(float s) override;
    void setClusterLights(bool enable) override;
    void setIblIntensity(float s) override;
    void setIblSpecular(float s) override;

    // HDR post-stack settings (r_tonemap / r_bloom* / r_autoexposure / r_ae*),
    // synced per frame by the app. Toggling AE on re-arms the adaptation snap so
    // the first adapted frame lands on target instantly (no multi-second crawl).
    void setPostFX(const PostFXParams& p) override;
    bool velocityEnabled() const override;
    bool velocityAvailable() const override;

    void setShadowBounds(float cx, float cy, float cz, float halfExtent) override;
    void setShadowCutout(bool enable) override;
    void setCsmParams(const CsmParams& p) override;

    // Interior reflection probe: when ON, the IBL environment cube is baked from the
    // SCENE geometry (around the camera) instead of the analytic sky, so glossy metals
    // reflect the dim interior rather than the bright open sky (which blows them out).
    void setIblProbe(bool enable) override;

    void setPointLights(const PointLight* lights, uint32_t count) override;

    void setSkyParams(const SkyParams& sp) override;

    // Global sky-animation time (seconds). Cached + written into the sky UBO's
    // params.z by prepareFrameData(), driving the starfield rotation + any future
    // time-driven celestial motion. Live loop passes elapsed; screenshots a fixed value.
    void setSkyTime(float t) override;

    // Project a world point -> HUD pixel coords (top-left origin) using the cached render
    // viewProj. false if behind the camera / well off-screen. For monster health bars etc.
    bool rayTracingSupported() const override;

    void setRtaoParams(const RtaoParams& p) override;

    void setReflectionParams(const ReflectionParams& p) override;

    void setDdgiParams(const DdgiParams& p) override;

    void setRtShadowParams(const RtShadowParams& p) override;

    void setSkinnedRtEnabled(bool enabled) override;
    bool skinnedRtEnabled() const override;
    uint32_t skinnedRtInstanceCount() const override;   // skinned chars in the TLAS this frame

    // RT RESIDENCY (see IRenderDevice::setRtOnlyDraws): sticky submission mode.
    // Records taken while this is set go to the TLAS only, never to raster.
    void setRtOnlyDraws(bool on) override { m_rtOnlyDraws = on; }

    // vis-unify: host-injected per-frame PVS numbers (room/portal skips + flood ms).
    void setVisHostStats(uint32_t roomsCulled, float pvsMs) override {
        m_visRoomsCulled = roomsCulled; m_visPvsMs = pvsMs;
    }

    void setGlassDevParams(const GlassDevParams& p) override;

    bool worldToScreen(float wx, float wy, float wz, float& sx, float& sy) const override;

    void setSsaoParams(const SsaoParams& sp) override;

    void setWaterParams(const WaterParams& wp) override;

    void setGiParams(const GiParams& gp) override;

    // ---- Particles + decals (combat juice) ---------------------------------
    // Append this frame's particle instances into the additive / alpha CPU staging
    // buffers (cleared each beginFrame). The buffers are FIXED-capacity (reserved
    // once at init to kMaxParticles); appends past the cap are dropped — NO per-
    // frame heap alloc. prepareFrameData() uploads them into the per-frame instance
    // ring and buildAndExecuteGraph adds the particle pass when any are present.
    void submitParticles(const ParticleInstance* instances, uint32_t count,
                         ParticleBlend mode) override;
    void submitDecals(const DecalInstance* decals, uint32_t count) override;

    FrameContext beginFrame() override;

    // LEGACY single-cascade sun ortho viewProj (r_csm 0). An ortho box of
    // half-extent kShadowOrtho centered on the camera position, with the light
    // positioned kShadowDepthHalf back along the sun direction. The box follows
    // the camera so the visible ~60 m level is always covered. Matches the sun L
    // in mesh.frag: normalize(0.4, 1.0, 0.3). Also honours r_shadowforward, which
    // slides the box forward along the camera axis (0 = historical, bit-exact).
    glm::mat4 computeLightViewProj() const;

    // CSM (r_csm 1): fit kCsmCascades cascades to this frame's camera, write them
    // into m_csm + the per-frame CSM UBO, and return the number of cascades that
    // must be rasterized. Returns 0 when CSM is inactive (cvar off, or a host
    // pinned the box with setShadowBounds) — the caller then runs the legacy
    // single-cascade path into layer 0 exactly as before.
    uint32_t prepareCsmCascades();

    // Record the BODY of the main color pass into `cmd` (the graph has already
    // begun dynamic rendering + emitted the swapchain/depth/shadow barriers). This
    // draws: the analytic sky (if enabled), the deferred mesh multidraw, then the
    // deferred HUD overlay — in exactly the order the hand-coded path used.
    void recordMainPassBody(VkCommandBuffer cmd);

    // Record the queued planet body draws into the (already-open) main color pass.
    // Reuses the mesh path's set0 (bindless textures) + set1 (object SSBO + camera
    // UBO) descriptor sets — rebinding them to the planet pipeline layout (which
    // declares the SAME set0/set1 layouts) so the bind is valid even though we just
    // bound them for the mesh path. Per planet: push the model + texture indices,
    // bind the sphere's vertex/index buffers, and one indexed draw.
    void recordPlanetDraws(VkCommandBuffer cmd);

    void endFrame(const FrameContext& fc) override;

    RenderStats stats() const override;
    void dumpPerfBreakdown(const char* why) override { logPerfBreakdown(why ? why : "manual"); resetPerfWindow(); }
    void setPassTimers(bool on) override { m_passTimersOn = on; x3::perf::zonesEnabled() = on; }
    bool passTimers() const override { return m_passTimersOn; }

    // ---- ZERO-STUTTER telemetry snapshot (r_frametelemetry / --test-framepacing).
    // Percentiles over the post-warmup ring; the late-creation counters are the
    // strict-PSO audit receipts (see the x3Create* wrappers + recordFramePacing).
    FramePacing framePacing() const override;

    void setPacingParams(const PacingParams& pp) override;

    // ---- Offscreen capture (--screenshot) ----------------------------------
    // Step 1: arm a capture for the NEXT frame. endFrame() will record the color-
    // image -> host readback copy inside that frame's live command buffer, reading
    // the freshly-rendered, properly-acquired swapchain image (no non-acquired
    // image is ever touched). Allocates the host-visible readback buffer up front.
    void armCapture(const char* path) override;

    // (The in-frame capture copy is now expressed as graph passes — see
    // buildAndExecuteGraph()'s "capture-copy"/"present" nodes. The graph derives
    // the COLOR_ATTACHMENT->TRANSFER_SRC->PRESENT_SRC transitions automatically.)

    // Step 3: finalize an armed capture. Wait on the captured frame's inFlight
    // fence (its copy has retired), map the readback buffer, swizzle (BGRA->RGBA)
    // and write the PNG. If no capture was armed/recorded, fall back to the legacy
    // self-contained "last-presented image" copy (idles the device first). Returns
    // true on success.
    bool captureFrame(const char* path) override;

    // Swizzle the mapped readback bytes to tightly-packed RGBA8 and write the PNG.
    bool writeCapturePng(const char* path, const void* mapped, uint32_t W, uint32_t H);

    // Legacy: copy the LAST presented image via a self-contained one-time-submit.
    // Idles the device, transitions PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC.
    // Retained as a fallback for callers that don't arm a capture in-frame.
    bool legacyCaptureLastPresented(const char* path);

    bool supportsDescriptorIndexing() const override;
    bool supportsMeshShaders() const override;

    // ---- Mesh / texture resource API (S1) ----------------------------------
    MeshHandle createMesh(const MeshVertex* verts, uint32_t vcount,
                          const uint32_t* idx, uint32_t icount) override;

    // Lane 5: N LOD levels sharing ONE vertex buffer. See IRenderDevice.h.
    uint32_t createMeshLodChain(const MeshVertex* verts, uint32_t vcount,
                                const uint32_t* const* idx, const uint32_t* icount,
                                uint32_t levels, MeshHandle* outMeshes) override;

    uint32_t meshVertexStride() const override { return m_vtxStride; }
    uint64_t meshVertexBytes() const override;

    void cameraLodInfo(float outEye[3], float& outFovYDeg, uint32_t& outHeightPx) const override;

    bool meshBounds(MeshHandle h, float outMin[3], float outMax[3]) const override;

    void destroyMesh(MeshHandle h) override;

    // ---- Dynamic mesh re-upload (CPU skinning, J1; fix 2 frames-in-flight) ---
    // Overwrite an existing mesh's vertices in place for the frame CURRENTLY being
    // recorded (m_frameIdx). On the first call the mesh is promoted to dynamic:
    // kFramesInFlight HOST_VISIBLE, persistently-mapped vertex buffers are
    // allocated and seeded from the static buffer's source so every frame slot has
    // valid contents even before its first per-slot write. Each call then writes
    // ONLY the buffer for m_frameIdx — which the GPU finished reading kFramesInFlight
    // frames ago (guaranteed by the inFlight fence waited in beginFrame), so there
    // is NO device wait and NO write-while-read hazard. The draw path binds the
    // matching per-frame buffer (Mesh::drawVbo(m_frameIdx)). Called per skinned
    // character per frame; scales to many NPCs without a GPU stall.
    void updateMesh(MeshHandle h, const MeshVertex* verts, uint32_t vcount) override;

    TextureHandle createTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb) override;

    void destroyTexture(TextureHandle h) override;

    // ---- Terrain material splat (open-world ground) -----------------------
    // Resolve the four detail textures' bindless indices, cache them, and hand
    // back a synthetic MARKER handle. drawMeshEmissive() recognises this exact
    // handle id and flags the per-object SSBO row as terrain, packing the four
    // indices into the previously-reserved pad fields. No GPU resource is created
    // here (the marker is purely a CPU sentinel), so there is nothing to destroy.
    TextureHandle registerTerrainMaterial(TextureHandle grass, TextureHandle rock,
                                          TextureHandle snow,  TextureHandle sand,
                                          TextureHandle rockHigh,
                                          TextureHandle grassN, TextureHandle rockN,
                                          TextureHandle snowN,  TextureHandle sandN) override;

    void drawMesh(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                  const float baseColorFactor[4], const float model[16]) override;

    // Glass / transparent draw. Same payload as drawMeshEmissive plus a GlassMaterial:
    // the per-object row is flagged GLASS so mesh.frag DISCARDs it (opaque pass) and
    // the transparent glass pass draws it (glass.frag). M1 uses opacity (-> alpha) +
    // tint/refraction/roughness/specular carried for later milestones. POD only.
    void drawMeshGlass(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                       const float baseColorFactor[4], const float emissive[4],
                       const GlassMaterial& glass, const float model[16],
                       bool alphaBlend = false) override;

    void drawMeshEmissive(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                          const float baseColorFactor[4], const float emissive[4],
                          const float model[16]) override;

    // PBR public entry: forwards to the shared internal builder with no glass.
    void drawMeshPBR(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                     TextureHandle normal, TextureHandle metalRough,
                     const float baseColorFactor[4], const float emissive[4],
                     const float model[16], bool alphaMask = false, bool alphaBlend = false, TextureHandle emissiveTex = {},
                     TextureHandle detailTex = {}, float detailUvScale = 1.0f,
                     float clearcoat = 0.0f, float clearcoatRough = 0.05f,
                     float selfLight = 0.0f, float metallicScale = 1.0f,
                     float foliage = 0.0f) override {
        drawMeshInternal(fc, mesh, baseColor, normal, metalRough, baseColorFactor, emissive,
                         model, alphaMask, alphaBlend, emissiveTex, detailTex, detailUvScale,
                         /*extraFlags=*/0u, /*glass=*/nullptr, clearcoat, clearcoatRough,
                         selfLight, metallicScale, foliage);
    }

    // Shared draw record append. The opaque/emissive/PBR/glass paths differ only by
    // the optional PBR maps, the alpha mode, `extraFlags` (GLASS bit), and the
    // optional GlassMaterial (nullptr for every non-glass path).
    void drawMeshInternal(const FrameContext& fc, MeshHandle mesh, TextureHandle baseColor,
                          TextureHandle normal, TextureHandle metalRough,
                          const float baseColorFactor[4], const float emissive[4],
                          const float model[16], bool alphaMask, bool alphaBlend,
                          TextureHandle emissiveTex, TextureHandle detailTex, float detailUvScale,
                          uint32_t extraFlags, const GlassMaterial* glass,
                          float clearcoat = 0.0f, float clearcoatRough = 0.05f,
                          float selfLight = 0.0f, float metallicScale = 1.0f,
                          float foliage = 0.0f);

    // ---- Procedural planet body (FORGE3D port) -----------------------------
    // Queue a planet draw for THIS frame: resolve each TextureHandle to its
    // bindless index (same map lookup drawMeshPBR uses), confirm the mesh exists,
    // and push a PlanetDraw entry. The actual draw is recorded in recordMainPassBody
    // AFTER the opaque mesh multidraw (dedicated planet pipeline + push constant).
    void drawPlanet(const FrameContext& fc, MeshHandle mesh, const float model[16],
                    uint32_t typeIndex, const TextureHandle* maps, uint32_t mapCount,
                    float uTime) override;

    // ---- Screen-space 2D HUD overlay (S7) ----------------------------------
    void drawHudQuad(const FrameContext& fc, float xPx, float yPx,
                     float wPx, float hPx, const float rgba[4]) override;

    // Textured HUD rectangle sampling an app-created texture (world-map tiles).
    // Same vertex ring / deferred-record path as drawHudQuad; the record carries
    // the texture id so recordHudDraws binds it instead of the white texel.
    // Arbitrary-corner variant (rotated map tiles — see IRenderDevice.h).
    void drawHudImageQuad(const FrameContext& fc, TextureHandle tex,
                          const float xyPx[8], const float rgba[4]) override;
    void drawHudImage(const FrameContext& fc, TextureHandle tex,
                      float xPx, float yPx, float wPx, float hPx,
                      const float rgba[4],
                      float u0, float v0, float u1, float v1) override;

    // Back-compat: render with the DEFAULT mono role (Console/HudMono — embedded
    // Roboto Mono). All existing non-UI callers route here unchanged.
    void drawHudText(const FrameContext& fc, const char* text, float xPx,
                     float yPx, float pxPerGlyph, const float rgba[4]) override;

    // Role-aware HUD text. Picks the role's baked atlas; PROPORTIONAL roles advance
    // by each glyph's real width, monospace roles advance by a fixed cell. Falls
    // back to the 8x8 bitmap path only if no TTF baked at all (NEVER blank text).
    void drawHudTextF(const FrameContext& fc, x3::rhi::FontRole role, const char* text,
                      float xPx, float yPx, float px, const float rgba[4]) override;

    // Render `text` from role `role`'s baked TTF atlas. For PROPORTIONAL roles the
    // pen advances by each glyph's real advance; for MONOSPACE roles every glyph
    // advances by a fixed cell and the (already-fixed-pitch) shape is placed
    // directly. `px` is the cap pixel size; the role index is also the texFont to
    // bind. (Takes the index, not a FontAtlas&, so the signature needs no early type
    // visibility — FontAtlas is declared with the other members further below.)
    void drawHudTextAtlas(int role, const char* text,
                          float xPx, float yPx, float px, const float c[4]);

    // The TRUE rendered width of `text` for `role` at glyph size `px`. Pure metrics
    // (no GPU work, safe before a frame). Proportional roles sum per-glyph advances;
    // mono roles (and the bitmap fallback) return N*px so legacy layout stays exact.
    float textAdvance(x3::rhi::FontRole role, const char* text, float px) const override;

    void hudSize(uint32_t& outW, uint32_t& outH) const override;

private:
    struct Mesh {
        VkBuffer vbo = VK_NULL_HANDLE; VmaAllocation vboAlloc = nullptr;
        VkBuffer ibo = VK_NULL_HANDLE; VmaAllocation iboAlloc = nullptr;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;       // vertices the vbo was sized for
        // Local-space bounding sphere (model space), computed from the mesh's
        // vertices at create/update time. Used by the CPU per-object frustum cull
        // (r_frustumcull): transformed to world space per draw instance via the
        // instance model matrix (center * model; radius * maxScale). See
        // FrustumCull.h. boundsRadius == 0 means "no bounds" -> never culled.
        glm::vec3 boundsCenter{0.0f};   // model-space sphere center
        float     boundsRadius = 0.0f;  // model-space sphere radius (0 = unbounded)
        // CPU local-space AABB from createMesh's submitted vertices (meshBounds).
        float bmin[3] = {0, 0, 0};
        float bmax[3] = {0, 0, 0};
        // CPU-skinning support (J1, scaled for fix 2): a mesh that has been updated
        // at least once becomes DYNAMIC and holds kFramesInFlight HOST_VISIBLE,
        // persistently-mapped vertex buffers — one per frame-in-flight — instead of
        // one shared buffer. updateMesh() writes the buffer for the frame being
        // recorded; the draw path (recordShadowPass/flushMeshDraws) binds the same
        // per-frame buffer. Because each frame's GPU work has retired (its inFlight
        // fence was waited in beginFrame) before that slot is reused, we never
        // overwrite a buffer the GPU may still be reading — NO vkDeviceWaitIdle and
        // no write-while-read hazard. Static meshes leave dynamic=false and use
        // only vbo (the device-local buffer); dynVbo[]/dynMapped[] stay null.
        bool     dynamic = false;
        // ---- LOD-chain shared vertex buffer (Lane 5) -------------------------
        // A mesh created through createMeshLodChain() gets its OWN index buffer
        // but ALIASES the chain's single vertex buffer. `vboShare` names the
        // chain (a share id minted per chain; 0 == "I own my vbo exclusively",
        // which is every mesh that existed before this feature). destroyMesh
        // decrements the share's refcount and frees the vbo only when it reaches
        // zero, so LOD levels may be destroyed in any order. Nothing in the draw
        // path can tell the difference: drawVbo() still returns `vbo`.
        uint32_t vboShare = 0;
        VkBuffer      dynVbo[kFramesInFlight]    = {};   // per-frame HOST_VISIBLE vbos
        VmaAllocation dynVboAlloc[kFramesInFlight] = {};
        void*         dynMapped[kFramesInFlight] = {};   // persistent maps

        // The vertex buffer the draw path must bind for the frame currently being
        // recorded: the matching per-frame dynamic buffer when dynamic, else the
        // single static device-local vbo.
        VkBuffer drawVbo(uint32_t frameIdx) const {
            return dynamic ? dynVbo[frameIdx] : vbo;
        }
        // PREVIOUS-frame vertex buffer for the velocity pass (#4). Skinned meshes
        // keep the prior frame's deformed verts in the previous ring slot; static
        // meshes reuse vbo so the previous == current position (no skinning term,
        // only model/camera motion contributes). frames is kFramesInFlight.
        VkBuffer prevVbo(uint32_t frameIdx, uint32_t frames) const {
            if (!dynamic) return vbo;
            uint32_t prev = (frameIdx + frames - 1u) % frames;
            return dynVbo[prev] ? dynVbo[prev] : dynVbo[frameIdx];
        }
    };
    struct Texture {
        VkImage image = VK_NULL_HANDLE; VmaAllocation alloc = nullptr;
        VkImageView view = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE;
        uint32_t bindlessIndex = 0;   // slot in the bindless texture array
    };

    // ---- GPU compute skinning TYPES (GPU SKINNING OF MODELS) --------------
    // Declared here (with Mesh/Texture) so method signatures that take a SkinnedMesh&
    // are in complete-class context. The std430 row layouts match shaders/skin.comp
    // EXACTLY. The runtime member variables (maps, pool, pipeline) live in the
    // members section further down with the other subsystem state.
    struct SkinSrcVertex { glm::vec4 posPad; glm::vec4 nrmPad; glm::vec4 uvPad; }; // 48B
    struct SkinSrcInfluence { glm::uvec4 idx; glm::vec4 wt; };                     // 32B
    static_assert(sizeof(SkinSrcVertex) == 48, "SkinSrcVertex must be 3x vec4");
    static_assert(sizeof(SkinSrcInfluence) == 32, "SkinSrcInfluence must be uvec4+vec4");
    static constexpr uint32_t kMaxSkinJoints = 256;  // palette cap per instance
    struct SkinnedMesh {
        uint32_t      vertexCount = 0;
        VkBuffer      srcVbo   = VK_NULL_HANDLE; VmaAllocation srcAlloc = nullptr; // bind-pose verts (immutable)
        VkBuffer      infBuf   = VK_NULL_HANDLE; VmaAllocation infAlloc = nullptr; // influences (immutable)
        VkBuffer      palBuf[kFramesInFlight]   = {};  // per-frame palette SSBO (host-visible)
        VmaAllocation palAlloc[kFramesInFlight] = {};
        void*         palMapped[kFramesInFlight] = {};
        VkDescriptorSet set[kFramesInFlight] = {};     // per-frame compute set
        uint32_t      jointCount = 0;                  // palette joints set THIS frame
        uint32_t      lastSkinnedFrame = ~0u;          // m_frameIdx the output was last written
    };
    struct SkinPush { uint32_t vertexCount; uint32_t jointCount; };

    // 2D HUD vertex: position already in clip space (NDC), uv, rgba color.
    struct HudVertex { float pos[2]; float uv[2]; float color[4]; };

    // LEGACY forward point-light cap — the size of the FrameUBO's fixed light
    // array, looped in full for every fragment. This is the path r_clusterlights 0
    // still runs, and it MUST stay 64: the repo's md5 / screenshot gates are
    // pinned to its exact output.
    //
    // The real cap is now kMaxSceneLights (1024, engine/rhi/ClusterLights.h) via
    // the clustered path, which keeps the lights in an SSBO and has each fragment
    // iterate only its own froxel's list. setPointLights() accepts up to that many;
    // the FIRST 64 are what still land in this UBO array, so the legacy branch sees
    // byte-for-byte the set it always saw.
    static constexpr uint32_t kMaxPointLights = 64;

    // One GPU point light row (matches the GLSL std140 PointLight in mesh.frag).
    // std140 packs each as two vec4s: (pos.xyz, range) + (color.rgb, _pad) = 32 B.
    struct GpuPointLight {
        glm::vec4 posRange;     // xyz = world pos, w = range (meters)
        glm::vec4 colorPad;     // rgb = linear color * intensity, a = unused
    };
    static_assert(sizeof(GpuPointLight) == 32, "GpuPointLight must be two vec4 (std140)");

    // Per-frame UBO (matches shaders/mesh.{vert,frag} + shadow.vert). The camera
    // viewProj + the sun's lightViewProj (perf-stack E), plus the forward point-
    // light set: an ambient/count header vec4 then a fixed array. std140: the two
    // mat4s pack back-to-back (128 B); ambientCount is a vec4 (16 B, 16-aligned);
    // the GpuPointLight[] array of 32-B (vec4-aligned) elements follows tightly.
    struct FrameUBO {
        glm::mat4 viewProj;          // offset 0
        glm::mat4 lightViewProj;     // offset 64
        glm::vec4 ambientCount;      // offset 128: rgb = ambient color, w = light count (as float)
        GpuPointLight lights[kMaxPointLights]; // offset 144
        glm::vec4 camPos;            // xyz = camera world position (PBR view vector); w unused
        glm::vec4 sunDir;            // xyz = per-scene direction TOWARD the sun (lighting + shadows)
        // ---- CLUSTERED FORWARD LIGHTING tail (r_clusterlights) --------------
        // APPENDED, never inserted: mesh.vert declares this block only as far as
        // camPos and glass.frag as far as sunDir, and std140 makes a shorter
        // declaration a valid PREFIX of the same buffer. Adding at the end keeps
        // both of those valid without touching either shader's binding.
        // All four are ZERO'd when the feature is off, and clusterCfg.x == 0 is
        // what routes the shader's light loops back to the legacy array above.
        glm::vec4 camFwd;            // xyz = camera FORWARD (view basis), w = zNear
        glm::vec4 clusterCfg;        // x = active(0/1), y = scene light count, zw = 1/screenW, 1/screenH
        glm::vec4 clusterGrid;       // x = gridX, y = gridY, z = gridZ, w = max lights per froxel
        glm::vec4 clusterSlice;      // x = sliceScale, y = sliceBias, z = froxel count, w = reserved
    };
    static_assert(sizeof(FrameUBO) == 144 + kMaxPointLights * 32 + 32 + 64,
                  "FrameUBO must match the std140 layout in mesh.frag");

    // ---- Analytic sky UBO (open-world track, task A) -----------------------
    // Matches the SkyUBO block in shaders/sky.frag (std140). One mat4 (inverse
    // viewProj for per-pixel ray reconstruction) + four vec4s of sun/haze params.
    // Its own per-frame buffer + descriptor set (set 0 of the sky pipeline) — it
    // does NOT touch the shared mesh FrameUBO, so the locked mesh layout is safe.
    struct SkyUBO {
        glm::mat4 invViewProj;   // offset 0
        glm::vec4 camPos;        // offset 64: xyz = camera world pos
        glm::vec4 sunDir;        // offset 80: xyz = normalized toward-sun direction
        glm::vec4 sunColor;      // offset 96: rgb = sun color, a = intensity
        glm::vec4 params;        // offset 112: x = haze, y = exposure, z/w reserved
        glm::vec4 zenith;        // offset 128: rgb = overhead sky color (per-scene)
        glm::vec4 horizon;       // offset 144: rgb = horizon glow color (per-scene)
    };
    static_assert(sizeof(SkyUBO) == 160, "SkyUBO must match the std140 layout in sky.frag");

    // Per-object GPU row (matches shaders/mesh.vert ObjectData; std430). The 3
    // uint pads keep the struct 16-byte aligned after texIndex so std430's mat4
    // base alignment is satisfied for the next row. `emissive` (HDR pipeline) is
    // an extra vec4 (rgb = linear emissive color, a = strength) added on top of
    // the lit result in linear HDR so fixtures glow + feed the bloom chain.
    struct ObjectData {
        glm::mat4 model;
        glm::vec4 baseColorFactor;
        glm::vec4 emissive;          // rgb = linear color, a = strength
        uint32_t  texIndex;
        uint32_t  flags;             // bitfield (was _pad0): bit0 = TERRAIN, bit1 = GLASS
        uint32_t  _pad1, _pad2;      // terrain detail-index packs (only when TERRAIN bit set)
        // ---- PBR maps (0 = none; the fragment shader skips its PBR branch) -----
        uint32_t  normalTexIndex;            // 0 = none (PBR normal-map bindless idx)
        uint32_t  mrTexIndex;                // 0 = none (metallic-roughness bindless idx)
        uint32_t  emissiveTexIndex;          // 0 = none (emissive bindless idx)
        uint32_t  detailPacked;              // HDRP micro-detail: (uvScale*64 << 20) | bindlessIdx; 0 = none
        // ---- GLASS material (only meaningful when the GLASS flag is set) -------
        // Carried per-object so each glass instance keeps its OWN material (the
        // holo-terminal panel/rim/scanline all differ — material-instance style,
        // spec §3.2). Opaque draws leave these zeroed (the opaque path never reads
        // them). glass.frag (M2-M4) consumes: refraction (screen-space bend),
        // roughness (frost mip), specular (shimmer), tint (rgb body color).
        glm::vec4 glassParams;       // x = refraction, y = roughness, z = specular, w = additive glow (rim exponent; 0 = normal glass)
        glm::vec4 glassTint;         // rgb = glass tint color, a = emissiveMap (0 = flat glow, 1 = modulate by texel)
    };
    static_assert(sizeof(ObjectData) == 160, "ObjectData must match std430 layout");

    // ---- THE STABLE RT MATERIAL TABLE (ddgi_rays.comp / refl.comp) -----------
    // WHY THIS EXISTS. A ray query returns instanceCustomIndex; the ray shaders
    // use it to fetch the hit surface's material. That index used to be the hit
    // record's row in the per-object SSBO (ObjectData) — but that buffer is
    // COMPACTED BY VISIBILITY: only instances that survive the frustum cull get a
    // row. The TLAS, correctly, admits off-screen geometry too, because RT needs
    // what the camera cannot see. So every culled instance carried customIndex 0
    // and every DDGI/reflection ray that hit it shaded with object row 0's
    // albedo/emissive. On the steady-state city that was 62.7% of the TLAS.
    //
    // The fix is to stop indexing a cull-compacted table. This one has a row per
    // DRAW RECORD, visible or not, so a record's index is the same whether the
    // camera is looking at it or not. That also removes the row CHURN: with the
    // compacted index, moving the camera reshuffled ~25 k instanceCustomIndex
    // values per frame and forced their TLAS instance rows to be rewritten
    // despite nothing having moved.
    //
    // Only the two fields the ray shaders actually read live here — keeping this
    // 32 B (rather than mirroring all 160 B of ObjectData) is what makes the
    // per-record table cheaper than the compacted one it replaces.
    struct RtMaterialGpu {
        glm::vec4 baseColorFactor;
        glm::vec4 emissive;          // rgb = linear color, a = strength
    };
    static_assert(sizeof(RtMaterialGpu) == 32, "RtMaterialGpu must match std430 layout");

    // Per-object flag bits packed into ObjectData::flags. TERRAIN drives mesh.frag's
    // procedural splat (was the standalone terrainFlag); GLASS routes the fragment to
    // the transparent glass pass (mesh.frag DISCARDs it; glass.frag draws it). The
    // bits are mutually exclusive in practice but stored independently so the shaders
    // can test either without ambiguity.
    static constexpr uint32_t kFlagTerrain = 1u << 0;
    static constexpr uint32_t kFlagGlass   = 1u << 1;
    // CLEARCOAT (car paint): mesh.frag adds a second fixed-F0 specular lobe; the
    // packed {roughness<<8 | intensity} byte pair rides the SPARE terrain-pack1
    // lane (mutually exclusive with TERRAIN, which owns that lane when set).
    static constexpr uint32_t kFlagClearcoat = 1u << 2;
    // SHIP SELF-LIGHT (canon: ships are self-lit). mesh.frag adds a shaped
    // rim + form term on the side the star is NOT lighting. The intensity byte
    // rides the SPARE terrain-pack2 lane (a ship is never TERRAIN, which owns
    // that lane when set; clearcoat owns pack1, so the two never collide).
    static constexpr uint32_t kFlagShipSelfLit = 1u << 3;
    // FOLIAGE (trees/vegetation): mesh.frag softens the diffuse (wrap lighting) and
    // adds a warm back-translucency term so the low sun glows THROUGH the canopy
    // instead of leaving the away-side flat black. No packed lane needed — a fixed
    // shader intensity; the flag alone gates it. Every non-foliage draw is unchanged.
    static constexpr uint32_t kFlagFoliage = 1u << 4;

    // CPU-side per-draw record accumulated by drawMesh(), consumed by endFrame().
    struct DrawRecord {
        uint32_t meshId;
        uint32_t texIndex;
        float    model[16];
        float    factor[4];
        float    emissive[4];   // rgb = linear emissive color, a = strength
        uint32_t flags;         // bit0 = TERRAIN (mesh.frag splat), bit1 = GLASS (glass pass)
        uint32_t terrainPack1;  // grass<<16 | rock  (bindless detail indices)
        uint32_t terrainPack2;  // snow<<16  | sand
        uint32_t normalTexIndex = 0;  // 0 = none (PBR normal-map bindless idx)
        uint32_t mrTexIndex     = 0;  // 0 = none (metallic-roughness bindless idx)
        uint32_t emissiveTexIndex = 0; // 0 = none (emissive bindless idx)
        uint32_t detailPacked = 0;     // HDRP micro-detail: (uvScale*64 << 20) | bindlessIdx; 0 = none
        bool     alphaBlend = false;   // glTF BLEND -> blend batch (CPU partition)
        // ALWAYS_VISIBLE for the CPU frustum cull (r_frustumcull): when true this
        // instance is NEVER culled (sky / fullscreen / unbounded items). Mirrors
        // cull.comp's flags bit0. CPU-only — NOT uploaded to the object SSBO.
        bool     noCull = false;
        // RT RESIDENCY (setRtOnlyDraws): this record exists FOR the TLAS. It is
        // dropped by emitGroup at the same point a frustum-culled instance is
        // dropped — before a raster SSBO row is assigned — so group contiguity
        // (firstInstance = baseRow, instanceCount = drawn) is preserved for free.
        // CPU-only, like noCull: never uploaded to the object SSBO.
        bool     rtOnly = false;
        // GLASS material (only filled by drawMeshGlass; zeroed for opaque draws).
        float    glassParams[4]; // x = refraction, y = roughness, z = specular, w unused
        float    glassTint[4];   // rgb = tint, a = emissiveMap (GlassMaterial::emissiveMap)
    };

    // Per-frame mesh-draw capacity: sizes the per-object SSBO ring (one
    // ObjectData row per drawMesh) + the indirect-command buffer. 128k supports
    // the stress harness (spawn up to 100k cubes); records beyond this are
    // skipped safely. Cost: ~12 MB of SSBO ring per frame (96 B * cap).
    static constexpr uint32_t kMaxDrawsPerFrame = 131072;
    // Bindless texture array capacity (slot 0 == built-in white default). The
    // array is PARTIALLY_BOUND so only the slots actually created are written;
    // unused slots cost nothing. 4096 is ample for this game.
    static constexpr uint32_t kMaxTextures = 4096;
    // Per-frame HUD draw + vertex capacity. HUD draws are few (one per text/quad
    // flush); kept small + independent of the mesh cap so the HUD descriptor pool
    // stays tiny. 6 verts/quad; a full-screen console is well under this.
    static constexpr uint32_t kMaxHudDraws = 256;
    static constexpr uint32_t kMaxHudVerts = 24576;

    // ---- Directional shadow mapping (perf-stack E) ------------------------
    // Square depth map resolution, PER CASCADE. 2048 is a good quality/cost
    // balance (4x the area of 1024 for one extra mip of crispness; still cheap
    // on the A2000/1080 Ti).
    static constexpr uint32_t kShadowDim = 2048;
    // LEGACY single-cascade box (r_csm 0): the sun's ortho half-extent (meters)
    // centered on the camera, and the depth range along the sun direction. This
    // covered a ~60 m level and is kept EXACTLY as-is so the r_csm 0 path stays
    // bit-identical for the md5/screenshot gates.
    static constexpr float kShadowOrtho     = 45.0f;   // half-width/height
    static constexpr float kShadowDepthHalf = 80.0f;   // +/- along the sun dir
    // CSM (r_csm 1): the shadow map becomes a 2D ARRAY of this many layers. The
    // count lives in one place — engine/rhi/Csm.h — so retuning it resizes the
    // image, the UBO array and the shader loop together.
    static constexpr uint32_t kCsmCascades = (uint32_t)csm::kNumCascades;

    // Per-frame CSM UBO (set 2, binding 1). Mirrored EXACTLY by the `Csm` block
    // in shaders/mesh.frag and shaders/glass.frag. std140: mat4 and vec4 are both
    // 16-byte aligned with no padding needed here.
    struct CsmUBO {
        glm::mat4 viewProj[kCsmCascades];  // world -> cascade i shadow clip
        glm::vec4 splitFar;                // lane i = cascade i's far VIEW depth (m)
        glm::vec4 depthBias;               // lane i = constant bias, light-clip depth units
        glm::vec4 normalBias;              // lane i = world-space normal offset (m)
        glm::vec4 ctrl;                    // x = active cascade count (0 => LEGACY path),
                                           // y = blend-band fraction, z/w reserved
    };
    static_assert(sizeof(CsmUBO) == kCsmCascades * 64 + 64, "CsmUBO must match the GLSL Csm block");

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        // GPU-driven per-frame rings (persistent-mapped, written each frame):
        //   objBuf      : per-object SSBO  (set 1, binding 0) — kMaxDrawsPerFrame rows
        //   camBuf      : camera viewProj UBO (set 1, binding 1)
        //   indirectBuf : VkDrawIndexedIndirectCommand array (one per distinct mesh)
        //   objSet      : the set-1 descriptor (points at objBuf+camBuf; written once)
        VkBuffer      objBuf = VK_NULL_HANDLE;      VmaAllocation objAlloc = nullptr;  void* objMapped = nullptr;
        //   rtMatBuf    : THE STABLE RT MATERIAL TABLE — one RtMaterialGpu row per
        //                 DRAW RECORD (not per VISIBLE object). objBuf is compacted
        //                 by visibility, so a row index into it is only meaningful
        //                 for an on-screen instance; the TLAS deliberately contains
        //                 OFF-SCREEN geometry too, and that geometry must still
        //                 shade with its OWN material. Indexed by draw-record index,
        //                 which does not move when the camera does. See
        //                 prepareFrameData() and shaders/rt_material.glsl.
        VkBuffer      rtMatBuf = VK_NULL_HANDLE;    VmaAllocation rtMatAlloc = nullptr; void* rtMatMapped = nullptr;
        VkBuffer      camBuf = VK_NULL_HANDLE;      VmaAllocation camAlloc = nullptr;  void* camMapped = nullptr;
        VkBuffer      indirectBuf = VK_NULL_HANDLE; VmaAllocation indirectAlloc = nullptr; void* indirectMapped = nullptr;
        VkDescriptorSet objSet = VK_NULL_HANDLE;
        // ---- CLUSTERED FORWARD LIGHTING per-frame ring (r_clusterlights) ----
        //   lightBuf   : GpuPointLight[kMaxSceneLights] (set 1, binding 3) — the
        //                whole scene light set, not the legacy 64.
        //   clusterBuf : uint32[kClusterCount * (1 + kMaxLightsPerCluster)]
        //                (set 1, binding 4). Layout: per-froxel counts first, then
        //                the fixed-stride index lists. ONE binding, no prefix sum.
        // Both are allocated unconditionally (they are ~916 KB per frame slot) and
        // are simply NEVER READ while r_clusterlights is 0 — the shader's legacy
        // branch cannot reach them, so the fallback stays bit-exact.
        VkBuffer      lightBuf = VK_NULL_HANDLE;    VmaAllocation lightAlloc = nullptr;   void* lightMapped = nullptr;
        VkBuffer      clusterBuf = VK_NULL_HANDLE;  VmaAllocation clusterAlloc = nullptr; void* clusterMapped = nullptr;
        // Per-frame analytic-sky UBO (set 0 of the sky pipeline) + its descriptor.
        VkBuffer      skyBuf = VK_NULL_HANDLE;      VmaAllocation skyAlloc = nullptr;  void* skyMapped = nullptr;
        VkDescriptorSet skySet = VK_NULL_HANDLE;
        // Per-frame HUD descriptor pool (image-sampler only) + vertex ring.
        VkDescriptorPool hudDescPool = VK_NULL_HANDLE;
        VkBuffer      hudVbo = VK_NULL_HANDLE;
        VmaAllocation hudVboAlloc = nullptr;
        void*         hudVboMapped = nullptr;
        uint32_t      hudVertsUsed = 0;  // write cursor into the vertex ring
        // Per-frame GPU timestamp query pool (2 stamps: pass begin + pass end).
        // Reset + written each beginFrame/endFrame; results are read back when the
        // SAME ring slot comes around again (kFramesInFlight frames later, so the
        // fence has already guaranteed the timestamps are available — no stall).
        VkQueryPool   tsPool = VK_NULL_HANDLE;
        bool          tsPending = false; // a frame's timestamps await readback
        // ---- LANE 6 per-pass timing (r_speeds) -----------------------------
        // Queries [0,1] stay the whole-frame bracket. Queries [2 .. 2+2*N) are the
        // per-pass pairs the RenderGraph writes; tsPassNames/tsPassCpuMs capture
        // the pass identity + CPU record cost at RECORD time so the readback (which
        // happens kFramesInFlight frames later, when this slot's fence retires) can
        // name what it is reading. Pass names are string literals -> stable.
        uint32_t      tsPassCount = 0;
        const char*   tsPassNames[kMaxTimedPasses]{};
        float         tsPassCpuMs[kMaxTimedPasses]{};

        // ---- D15 GPU cull per-frame ring (all persistent-mapped) -----------
        //   cullInstBuf : CullInstanceGpu[kMaxDrawsPerFrame] (CPU-written when on)
        //   visBuf      : uint32[kMaxDrawsPerFrame] visible-instance indirection.
        //                 IDENTITY-filled at creation; cull.comp compacts into it
        //                 when the GPU path is on (visDirty tracks the scribble so
        //                 a path toggle back to CPU restores identity once).
        //   cullStatsBuf: CullStatsGpu, GPU-written, read back on slot reuse.
        //   cullParamsBuf: CullParamsGpu UBO (planes/viewProj/hzb/instanceCount).
        VkBuffer      cullInstBuf = VK_NULL_HANDLE;  VmaAllocation cullInstAlloc = nullptr;  void* cullInstMapped = nullptr;
        VkBuffer      visBuf = VK_NULL_HANDLE;       VmaAllocation visAlloc = nullptr;       void* visMapped = nullptr;
        VkBuffer      cullStatsBuf = VK_NULL_HANDLE; VmaAllocation cullStatsAlloc = nullptr; void* cullStatsMapped = nullptr;
        VkBuffer      cullParamsBuf = VK_NULL_HANDLE; VmaAllocation cullParamsAlloc = nullptr; void* cullParamsMapped = nullptr;
        VkDescriptorSet cullSet = VK_NULL_HANDLE;    // cull.comp bindings 0..5
        bool          visDirty = false;       // GPU compaction scribbled visBuf
        bool          cullStatsPending = false; // a frame's cull stats await readback
        uint32_t      cullExpected = 0;       // CPU-evaluated survivors (equiv mode)
        bool          cullExpectedValid = false;
        // Tier 1: this slot's DEDICATED-COMPUTE-QUEUE command pool/buffer (the
        // async cull dispatch). Reset on slot reuse (fence-guarded transitively:
        // compute work <= timeline wait <= graphics work <= inFlight fence).
        VkCommandPool   cullPool = VK_NULL_HANDLE;
        VkCommandBuffer cullCmd  = VK_NULL_HANDLE;
    };

    void imageBarrier(VkCommandBuffer cmd, VkImage img,
                      VkImageLayout oldL, VkImageLayout newL,
                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

    void depthBarrier(VkCommandBuffer cmd, VkImage img,
                      VkImageLayout oldL, VkImageLayout newL,
                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

    // ---- HUD helpers --------------------------------------------------------
    // Fill 6 vertices (two triangles) for a pixel-space rect, converting to NDC
    // using the current framebuffer extent (origin top-left -> NDC y flips).
    void emitQuad(HudVertex out[6], float xPx, float yPx, float wPx, float hPx,
                  float u0, float v0, float u1, float v1, const float c[4]);

    // ---- GPU-driven per-frame data prep (Subsystem D + shadows E) ----------
    // Build the camera+light UBO, group this frame's drawMesh() records by mesh,
    // write the per-object SSBO (instances of a mesh contiguous), and fill one
    // VkDrawIndexedIndirectCommand per mesh. Pure data; records NO commands. Runs
    // once per frame (m_framePrepared) because BOTH the shadow depth pass and the
    // main color pass consume the same SSBO + indirect buffer. Caches the produced
    // command count in m_frameCmdCount.
    void prepareFrameData();

    // ---- Directional shadow depth pass (perf-stack E) ----------------------
    // Render the scene depth from the sun's POV into the shadow map using the SAME
    // SSBO + indirect commands as the color pass (prepared by prepareFrameData),
    // with the depth-only pipeline (set 0 = object SSBO + camera UBO; shadow.vert
    // reads model rows + lightViewProj). Always runs (even with zero occluders) so
    // the map is cleared + left in DEPTH_READ_ONLY_OPTIMAL for the main pass to
    // sample. Recorded BEFORE the main color pass begins (no nested rendering).
    // Record the BODY of the shadow depth pass into `cmd` (the graph has begun
    // dynamic rendering on the shadow map + emitted its UNDEFINED->DEPTH_ATTACHMENT
    // barrier). Same SSBO + indirect draws as the color pass, from the sun's POV.
    void recordShadowPassBody(VkCommandBuffer cmd);

    // ---- SSAO depth pre-pass body ------------------------------------------
    // Record the camera depth-only pre-pass into the (already-open) depth pass:
    // bind the depth pre-pass pipeline (depth.vert, set0 = objSet) and replay the
    // SAME indirect draws as the main pass, so the depth buffer holds the exact
    // camera depth before lighting. Runs only when SSAO is enabled.
    void recordDepthPrePassBody(VkCommandBuffer cmd);

    // ---- GPU-driven mesh multidraw (Subsystem D) ---------------------------
    // Record the mesh multidraw into the (already-open) main color pass: bind the
    // mesh pipeline + descriptor sets and issue ONE vkCmdDrawIndexedIndirect per
    // distinct mesh. Called from recordMainPassBody (inside the graph's color pass).
    void recordMeshDraws(VkCommandBuffer cmd);

    // A deferred HUD draw: a span of this frame's HUD vertex ring + which texture
    // to bind. `texFont` selects the atlas: -1 => the 1x1 white texel (filled
    // quads); >=0 => that FontRole's glyph atlas (or the 8x8 bitmap fallback if the
    // role didn't bake). drawHudQuad/drawHudText(F) append these; they are replayed
    // (in order, after the meshes) by recordHudDraws inside the graph's color pass —
    // keeping HUD-on-top ordering identical to the hand-coded path.
    struct HudRecord { uint32_t first; uint32_t count; int texFont; uint32_t userTex = 0; };

    // Append `count` vertices to this frame's HUD ring and queue a deferred HUD
    // draw record (replayed inside the graph's color pass). No command recording
    // here — the color pass is not open yet (commands are recorded in endFrame).
    // `texFont`: -1 = white texel; otherwise a FontRole index whose atlas to bind.
    void flushHud(const HudVertex* verts, uint32_t count, int texFont, uint32_t userTex = 0);

    // Resolve a HudRecord's texFont to the Texture to bind: a live app texture for
    // userTex != 0 (drawHudImage — the world-map tile compositor), the white texel
    // for -1, the role's baked atlas if ready, else the 8x8 bitmap fallback, else white.
    const Texture* hudRecordTexture(int texFont, uint32_t userTex = 0) const;

    // Replay the frame's deferred HUD draws into the (already-open) color pass.
    // Allocates the per-draw descriptor from the frame's HUD pool here, exactly as
    // the old in-line flushHud did (same descriptor lifetime: recycled next reuse).
    void recordHudDraws(VkCommandBuffer cmd);

    // ===================================================================
    // Build the per-frame render graph + execute it (perf-stack B). This is the
    // single place the frame's GPU command sequence is assembled. Each pass
    // declares the resources it reads/writes (layout + stage + access); the graph
    // derives + emits all sync2 barriers + layout transitions and drives
    // begin/endRendering. Replaces every previously hand-coded barrier.
    //
    //   Pass 1  shadow-depth : WRITE shadow map (DEPTH_ATTACHMENT). The graph
    //                          transitions it from its persistent prior-frame
    //                          state (DEPTH_READ_ONLY, or UNDEFINED on first use).
    //   Pass 2  main-color   : WRITE swapchain color (COLOR_ATTACHMENT) + depth
    //                          (DEPTH_ATTACHMENT) + READ shadow map
    //                          (DEPTH_READ_ONLY). The graph emits the
    //                          shadow DEPTH_ATTACHMENT->DEPTH_READ_ONLY transition
    //                          (the #1 VUID-sensitive barrier) automatically, plus
    //                          the swapchain/depth UNDEFINED->attachment transitions.
    //   Pass 3  present OR capture : transitions the swapchain color to
    //                          PRESENT_SRC, or (if a capture is armed) to
    //                          TRANSFER_SRC, copies to the readback buffer, then to
    //                          PRESENT_SRC — all derived from declared uses.
    // ===================================================================
    // Add the bloom downsample + upsample passes to the graph for this frame.
    // `rgHdr` is the linear HDR scene target (already written by the main pass);
    // `rgMip[]` are the kBloomMips bloom targets. Each pass is a full-screen
    // triangle; the graph derives the COLOR_ATTACHMENT<->SHADER_READ_ONLY
    // transitions between mips. No per-frame heap alloc: the push payloads live in
    // member arrays captured by value into the record lambdas.
    void addBloomPasses(RgResource rgHdr, RgResource* rgMip);

    // ---- Glass frost-blur chain (M4) --------------------------------------
    // Downsample the scene copy into m_glassFrostImg[] — separate single-mip images
    // (the render-graph tracks one layout per image, so distinct images, not mip
    // levels), each half the previous. level0 samples the scene copy; level i samples
    // level i-1. Reuses the bloom-down 13-tap filter (m_bloomDownPipe / m_bloomLayout,
    // BloomPush, firstPass=0 = plain downsample). The glass shader lerps the sharp
    // copy toward the deepest level by roughness. Stable per-level storage lives in
    // member arrays. The caller passes the scene-copy resource (rgSceneCopy) so the
    // first level orders after the copy; each frost level is imported here.
    void addGlassFrostPasses(RgResource rgSceneCopy);

    // Set dynamic viewport+scissor to a post target's extent.
    void postViewport(VkCommandBuffer c, VkExtent2D ext);

    void buildAndExecuteGraph(VkCommandBuffer cmd, uint32_t imageIndex, bool wantCapture);

    // ---- ROLE -> FONT FILE MAP (the ONE place to reassign a role's font) -------
    // To change which typeface a role uses: edit the path string here and restart
    // (no rebuild of the engine logic needed — the file ships in assets/). The
    // *static* (non-VariableFont) weights are used. `proportional=false` forces a
    // monospace cell (News/Console are genuinely mono; the proportional roles get
    // real per-glyph advances). The embedded Roboto Mono is the guaranteed fallback
    // for ANY role whose file is missing/unreadable, so text is NEVER blank.
    //
    // To EMBED a role's font for a single-binary ship: run
    //   tools/embed_font.ps1 -InFont assets/fonts/<dir>/<file>.ttf -Symbol k<Name>TTF
    // and point that role's load at the generated blob (see kRobotoMonoTTF usage).
    // Read a font file by trying a few roots so the load works regardless of the
    // process CWD (run-from-repo-root, run-from-build-dir, assets-next-to-exe). The
    // relative path (e.g. "Orbitron/static/Orbitron-Bold.ttf") is joined onto each
    // candidate prefix; the first readable hit wins. Returns the resolved path in
    // `outResolved`. Mirrors app/asset_root.h's candidate order but stays self-
    // contained in the engine layer (no app dependency). Empty vector => not found.
    std::vector<unsigned char> readFontFile(const char* relPath, std::string& outResolved);

    struct RoleFontDesc { const char* path; bool proportional; const char* label; };
    static const RoleFontDesc* roleFontTable();

    // Font atlas builder. Bakes ONE crisp glyph atlas PER FontRole from a real TTF
    // (assets/fonts/, embedded Roboto Mono fallback) via stb_truetype, and bakes the
    // legacy 8x8 bitmap font as the universal last-resort so text is NEVER blank.
    // Logs which typeface actually loaded for each role.
    bool buildFontAtlas();

    static std::string roleName(int r);

    // Bake ASCII 32..126 from `ttf` into an antialiased R8 coverage atlas via
    // stb_truetype's packer (2x2 oversample for crisp small text), then expand to
    // RGBA (white, alpha=coverage) so the existing LINEAR-sampler HUD path renders
    // smooth, alpha-blended, per-vertex-tinted glyphs. Records per-glyph atlas UVs +
    // offsets + advance (bake-pixel units) into m_fonts[role]. Does NOT set ready.
    // (Takes the role index, not a FontAtlas&, so the signature needs no early type
    // visibility — FontAtlas is declared with the other members further below.)
    bool bakeTtfAtlas(int role, const unsigned char* ttf, size_t ttfSize);

    // Build a 128x64 RGBA atlas (16 cols x 8 rows of 8x8 glyphs) from the embedded
    // public-domain font8x8_basic bits (white texel, alpha = pixel-on) into `dst`.
    // Universal last-resort fallback (NEVER ship blank text).
    bool buildBitmapFontAtlas(Texture& dst);

    // Create the 2D HUD pipeline: NDC quads, no depth, alpha blend, one combined-
    // image-sampler set, per-frame HUD descriptor pools + vertex rings.
    bool createHud();

    // =====================================================================
    // IMAGE-BASED LIGHTING (IBL) — split-sum environment reflections.
    //
    // Bakes (from the analytic sky) a diffuse irradiance cube + a roughness-mipped
    // specular prefilter cube + a scene-independent BRDF LUT, then exposes them as
    // mesh.frag set 4. createIbl() builds all GPU objects (idempotent w.r.t. the
    // sky); regenIblFromSky() runs the four fullscreen bake passes on a one-time
    // submit (init + whenever the sky changes). Everything is additive: if the IBL
    // resources fail to build, m_iblReady stays false and mesh.frag falls back to
    // its old flat ambient term (the descriptor set is still bound but the shader
    // gates on an "iblValid" flag carried in the SSAO control UBO's spare lane).
    // =====================================================================

    // std140 mirror of IblSkyUBO in ibl_env.frag (5 vec4s = 80 bytes).
    struct IblSkyUBO {
        glm::vec4 sunDir;
        glm::vec4 sunColor;
        glm::vec4 params;   // x=haze y=exposure z=time w=enabled
        glm::vec4 zenith;
        glm::vec4 horizon;
    };
    // Per-face push constant for the env/irradiance pass (3 vec4) + prefilter (4 vec4).
    struct IblFacePush {
        glm::vec4 faceFwd;
        glm::vec4 faceRight;
        glm::vec4 faceUp;
        glm::vec4 misc;     // prefilter: x=roughness y=resolution; ignored by env/irradiance
    };

    // The six cube-face direction bases (matches Vulkan cube face order +X,-X,+Y,-Y,+Z,-Z).
    // fwd = face center dir; right/up span the face so dir = fwd + (2u-1)*right + (2v-1)*up.
    // Chosen so the assembled cube is consistent (standard GL/Vulkan cubemap convention).
    static void iblFaceBasis(int face, glm::vec3& fwd, glm::vec3& right, glm::vec3& up);

    // Create one cubemap image (6 layers) + a CUBE view + per-face single-mip RT
    // views (mip `rtMip` of each face). When mipLevels>1 the image is mip-complete.
    bool createIblCube(uint32_t size, uint32_t mipLevels, VkImageUsageFlags usage,
                       VkImage& outImg, VmaAllocation& outAlloc,
                       VkImageView& outCubeView, VkImageView* outFaceViews, uint32_t rtMip);

    // Build all IBL GPU objects (images, views, samplers, descriptor layouts/sets,
    // and the four bake pipelines). Does NOT bake (that's regenIblFromSky()).
    bool createIbl();

    // Small helper: render the fullscreen triangle into a single image-view attachment.
    void iblRenderTo(VkCommandBuffer cmd, VkImageView target, uint32_t w, uint32_t h,
                     VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set,
                     const IblFacePush* push);

    // sync2 image-layout barrier for an IBL image subresource range.
    void iblBarrier(VkCommandBuffer cmd, VkImage img, uint32_t baseMip, uint32_t mipCount,
                    VkImageLayout oldL, VkImageLayout newL,
                    VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 ds, VkAccessFlags2 da);

    // Reflection-probe scene bake: render opaque scene geometry into all 6 env-cube
    // faces from m_iblProbePos (90deg/face), so the IBL env captures the dim INTERIOR
    // instead of the bright sky. The env image is already in COLOR_ATTACHMENT (mip0, 6
    // layers). Each face clears to a dim ambient backdrop (window openings / gaps) then
    // draws the scene via the probe PSO (push-constant per-face viewProj). One shared
    // depth target, WAW-barriered between faces. Lighting reuses mesh.frag (direct +
    // fallback ambient on the first bake, since IBL isn't valid yet) — no recursion.
    void bakeProbeSceneIntoEnv(VkCommandBuffer cmd);

    // Bake the IBL chain from the CURRENT sky params. Runs the BRDF LUT (only once),
    // then env capture (6 faces) -> env mip generation -> irradiance convolve (6
    // faces) -> prefilter (5 mips x 6 faces), all on one one-time submit. Then
    // writes mesh.frag set 4 to point at the fresh irradiance/prefilter/LUT.
    bool regenIblFromSky();

    // 2D version of iblBarrier (BRDF LUT, 1 layer).
    void iblBarrierTex2D(VkCommandBuffer cmd, VkImage img, VkImageLayout oldL, VkImageLayout newL,
                         VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da);

    void destroyIbl();

    // ---- Analytic sky (open-world track, task A) ---------------------------
    // Create the per-frame sky UBO buffers + descriptor sets and the full-screen
    // sky pipeline. The pipeline has NO vertex input (the vertex shader generates a
    // covering triangle from gl_VertexIndex), draws at far depth with depth test
    // LESS_OR_EQUAL + depth write OFF so opaque geometry occludes it and the sky
    // writes nothing to depth, and does not blend (opaque background fill).
    bool createSky();

    void destroySky();

    // ---- Water (undersea-world foundation) ---------------------------------
    // Build the unit-patch grid mesh (vec2 grid coord per vertex, [-1,1]), the
    // per-frame water UBO buffers + descriptor sets (set0: UBO + scene-depth
    // sampler), and the water graphics pipeline (water.vert/.frag; depth-test
    // LESS_OR_EQUAL, depth-write OFF — the water reads the depth the opaque pass
    // produced and never writes it, so post passes are unharmed). Created once;
    // the depth descriptor binding is (re)written by writeWaterDescriptors() at
    // init + on resize (the depth view changes).
    bool createWater();

    // (Re)write the scene-depth binding of each frame's water set. Called at init
    // + on resize (the depth image view changes). The depth is sampled in the
    // DEPTH_READ_ONLY layout — the SAME layout it is bound as a read-only depth
    // attachment in the water pass, so simultaneous test + sample is valid.
    void writeWaterDescriptors();

    void destroyWater();

    // Record the water surface into the (already-open) water pass: bind the water
    // pipeline + this frame's set (UBO + scene depth) and draw the grid mesh. The
    // VS displaces the grid with Gerstner waves; the FS does sky-reflection +
    // depth-refraction + sun glint into the linear HDR target.
    void recordWaterPassBody(VkCommandBuffer cmd);

    // ---- Translucent GLASS pass body (transparent meshes) ------------------
    // Recorded into the (already-open) post-opaque glass pass. Binds the glass
    // pipeline + the SAME descriptor sets the opaque mesh pass uses (bindless
    // textures, object SSBO + camera UBO, shadow map, SSAO) and REPLAYS the SAME
    // per-mesh indirect multidraw. glass.frag DISCARDs non-glass fragments and
    // mesh.frag DISCARDs glass fragments, so the two passes cleanly partition the
    // draw list — no separate glass index/SSBO buffer needed for M1. Only reached
    // when m_frameGlassCount > 0 (the pass isn't added otherwise).
    void recordGlassPassBody(VkCommandBuffer cmd);

    // ---- Particles + impact decals (combat juice) --------------------------
    // Build: the shared unit quad (4 corners, triangle strip), the per-frame
    // instance rings (additive/alpha particles + decals) + UBOs + descriptor sets,
    // and the three graphics pipelines (additive particles, alpha particles, alpha
    // decals). All depth-TEST LESS_OR_EQUAL, depth-write OFF (the billboards/decals
    // read the opaque depth the main pass produced and never overwrite it). The
    // particle FS samples the scene depth for the soft-particle fade (set0,b1).
    bool createParticles();

    // (Re)write the scene-depth binding (b1) of each frame's particle set. Called at
    // init + on resize (the depth image view changes). Sampled in DEPTH_READ_ONLY —
    // the SAME layout it is bound as a read-only depth attachment in the pass.
    void writeParticleDescriptors();

    void destroyParticles();

    // =======================================================================
    // GPU-compute persistent debris world (Subsystem K, tier T2).
    // -----------------------------------------------------------------------
    // CLEAN-ROOM, original work. Built ONLY from the K spec (§5/§7), the engine's
    // own renderer patterns (the SSBO + persistent-mapped buffer + instanced-draw
    // path used by particles/multidraw above), and the Vulkan 1.3 spec (compute
    // pipelines, SSBOs, vkCmdDispatch). NO id Tech / RBDOOM source consulted.
    //
    // A fixed-size pool of debris fragments lives in ONE host-visible DEVICE_LOCAL
    // SSBO (VMA HOST_ACCESS_RANDOM -> BAR/host-visible device memory; reads + writes
    // both work, no staging needed for spawn/readback). A COMPUTE shader integrates
    // the whole pool each step (gravity, ground + AABB collision, damping, sleep,
    // lifetime free). The pool draws via ONE instanced unit-cube draw (instanceCount
    // == capacity); each instance reads its row from the same SSBO and dead slots
    // collapse to nothing in the vertex shader (no per-frame compaction).
    //
    // This is the SYNCHRONOUS path (graphics queue): correct on Pascal (1080 Ti)
    // where async-compute overlap is weak. The compute dispatch and the draw run in
    // the same frame's command buffer, ordered by an SSBO memory barrier.

    // Mirror of shaders/debris.comp's Fragment (std430; four 16-byte vec4 rows).
    struct GpuDebrisFragment {
        glm::vec4 posLife;    // xyz position, w remaining lifetime (s)
        glm::vec4 velScale;   // xyz velocity, w half-extent
        glm::vec4 spinState;  // xyz angular velocity, w packed state+sleepCtr
        glm::vec4 rot;        // orientation quaternion (x,y,z,w)
    };
    static_assert(sizeof(GpuDebrisFragment) == 64, "GpuDebrisFragment must be 4x vec4");

    // Mirror of shaders/debris.comp's Params UBO (std140).
    struct GpuDebrisParamsUBO {
        glm::vec4 gravityDt;     // xyz gravity, w dt
        glm::vec4 groundDamp;    // x groundY, y restitution, z friction, w linDamp
        glm::vec4 sleepCap;      // x sleepLin, y sleepAng, z sleepFrames, w capacity
        glm::vec4 aabbCount;     // x aabb count
        glm::vec4 aabbMin[4];
        glm::vec4 aabbMax[4];
    };

    // Mirror of shaders/debris.{vert} DebrisDrawUBO (std140).
    struct GpuDebrisDrawUBO { glm::mat4 viewProj; glm::vec4 color; };

    static constexpr uint32_t kDebrisCapacity = 65536;  // pool size (spec §11 target: 50k+)
    static constexpr float    kDebrisDeadState = 0.0f;
    static constexpr float    kDebrisActiveState = 1.0f;
    // Vertices per shard mesh slot in the shard-set SSBO (padded with degenerate
    // repeats of the last vertex). MUST match kShardVerts in shaders/debris.vert.
    static constexpr uint32_t kDebrisShardVertsMax = 36;

    bool createDebris();

    void destroyDebris();

    void gpuDebrisConfig(const IRenderDevice::GpuDebrisParams& p) override;

    uint32_t gpuDebrisAliveCount() const override;
    uint32_t gpuDebrisCapacity() const override;

    uint32_t gpuDebrisSpawnBurst(const float pos[3], uint32_t count, float speed,
                                 float lifetime, float halfExtent, uint32_t seed) override;

    void gpuDebrisStep(float dt) override;

    void gpuDebrisDraw(const FrameContext& fc, const float tint[4]) override;

    IRenderDevice::GpuDebrisStats gpuDebrisReadback(float boundsLimit) const override;

    // Record the debris compute dispatch (synchronous, graphics queue). Called by the
    // graph's debris-compute pass (before the draw). An SSBO write->read barrier after
    // the dispatch lets the instanced draw + the next-frame readback see the result.
    void recordDebrisComputeBody(VkCommandBuffer cmd);

    // Record the debris instanced cube draw into the (already-open) particle/transparent
    // pass. ONE instanced draw over the whole pool; dead slots collapse in the VS.
    void recordDebrisDrawBody(VkCommandBuffer cmd);

    // ======================================================================
    // GPU compute skinning of models (GPU SKINNING OF MODELS).
    //
    // Clean-room, original work. Built ONLY from the engine's own RHI/loader
    // interfaces, the Vulkan 1.3 spec (compute pipelines, SSBOs, barriers), and
    // public glTF 2.0 linear-blend-skinning math. No id Tech / RBDOOM source.
    //
    // Create the compute pipeline + descriptor pool/layout once at init. Each
    // registered skinned mesh allocates its own buffers + per-frame descriptor sets.
    // ======================================================================
    bool createSkinning();

    void destroySkinning();

    // Free one skinned mesh's GPU resources (immediate; callers ensure the GPU is
    // idle for this buffer set — registration-time rollback, unregister waits idle,
    // and shutdown already waited idle).
    void destroySkinnedMeshResources(SkinnedMesh& sm);

    // Promote a mesh to a compute-skinned dynamic mesh: allocate kFramesInFlight
    // skinned-output vertex buffers with STORAGE usage (so the compute can write them
    // and the draw can read them as vertex buffers) and seed them from the bind pose.
    // Mirrors updateMesh's dynamic promotion but the buffers are GPU-written, so they
    // are DEVICE_LOCAL + STORAGE | VERTEX (no host mapping needed).
    bool promoteMeshForSkinning(Mesh& m, const MeshVertex* bindVerts, uint32_t vcount);

    bool registerSkinnedMesh(MeshHandle mesh, const MeshVertex* bindVerts, uint32_t vcount,
                             const uint16_t* jointIdx4, const float* jointWt4) override;

    void unregisterSkinnedMesh(MeshHandle mesh) override;

    void setSkinnedPalette(MeshHandle mesh, const float* palette, uint32_t jointCount) override;

    bool readbackSkinnedMesh(MeshHandle mesh, MeshVertex* out, uint32_t vcount) override;

    bool supportsGpuSkinning() const override;

    // Record the skinning compute pass: ONE dispatch per pending skinned instance,
    // each writing its mesh's per-frame skinned-output vbo. A single SSBO write ->
    // vertex-read barrier after all dispatches lets the depth/shadow/color passes
    // read the skinned vertices. Recorded as the FIRST graphics-queue pass each frame
    // (before shadow/depth/color), so all three passes draw the skinned geometry.
    void recordSkinComputeBody(VkCommandBuffer cmd);

    // Record the particle + decal draws into the (already-open) particle pass.
    // Order: DECALS first (alpha, on surfaces) then ALPHA particles (smoke/dust)
    // then ADDITIVE particles (sparks/muzzle — glow last so they sit brightest).
    void recordParticlePassBody(VkCommandBuffer cmd);

    void destroyHud();

    bool createSwapchain(uint32_t w, uint32_t h);

    void destroySwapchain();

    void recreateSwapchain();

    // ---- HEADLESS offscreen render target (no window, no swapchain) ---------
    // Creates the offscreen COLOR image the render graph targets in place of an
    // acquired swapchain image, plus the matching depth image. The color image
    // uses the SAME default format the windowed swapchain used (B8G8R8A8_UNORM)
    // and COLOR_ATTACHMENT | TRANSFER_SRC usage, so the existing capture readback
    // + BGRA->RGBA swizzle produce byte-identical PNGs. This mirrors the depth
    // image creation in createSwapchain() so both modes size their depth the same.
    bool createOffscreenTarget(uint32_t w, uint32_t h);

    void destroyOffscreenTarget();

    // Headless analogue of recreateSwapchain(): idle the device, destroy + recreate
    // the offscreen color + depth images at the new size, and re-point the graph
    // (the graph re-imports the offscreen image by handle each frame in
    // buildAndExecuteGraph, so simply recreating the images here is enough — the
    // next beginFrame/endFrame targets the new images). This exercises the same
    // resize/recreate code path the windowed swapchain recreate does, validated.
    void recreateOffscreenTarget();

    // ---- HDR scene + bloom render targets ----------------------------------
    // Create (or recreate) the linear HDR scene target + the bloom mip chain at
    // the current frame extent. Called after the swapchain/offscreen target exists
    // (so m_extent is valid) and on every resize. Images: COLOR_ATTACHMENT (render
    // target) | SAMPLED (read by the next post pass) | TRANSFER_DST is not needed.
    bool createBloomTargets();

    // Create the single-mip scene-color copy image (refraction, M2) + the separate
    // downsampled frost-blur level images (M4). Returns false on any failure (caller
    // treats it as non-fatal: glass still draws, just without refraction/frost).
    bool createSceneCopyTarget(uint32_t W, uint32_t H);

    void destroySceneCopyTarget();

    void destroyBloomTargets();

    // ---- Glass resources (set 4 UBO + descriptor sets + scene-copy sampler) ----
    // Built once at init AFTER createBloomTargets (so the scene-copy view exists):
    // a mip-aware LINEAR sampler, per-frame GlassControl UBOs, a descriptor pool +
    // one glass set per frame. The set is (re)written by writeGlassDescriptors at
    // init + on every resize (the scene-copy view changes). The glass set layout
    // itself is built in createGraphics (needed by the glass pipeline layout).
    // Graceful: any failure leaves m_glassSet[*] null; recordGlassPassBody falls
    // back to binding nothing and the glass pass is skipped (opaque unaffected).
    bool createGlassResources();

    // (Re)point the glass sets at the current scene-copy view + per-frame UBO.
    // Called at init + after every resize (the scene-copy view is recreated). If
    // the scene-copy target failed to create, the sampler points at the HDR view
    // as a harmless stand-in (glass simply won't refract — opacity path still reads).
    void writeGlassDescriptors();

    void destroyGlassResources();

    // Helper: create a single-mip 2D color image + view (used for HDR + bloom mips).
    bool createColorTarget(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                           VkImage& outImg, VmaAllocation& outAlloc, VkImageView& outView);

    // ---- Post-process pipelines (bloom down/up + composite) ----------------
    // Build the sampler, descriptor-set layouts (1 sampler for down/up, 2 for the
    // composite), descriptor pool + sets, and the three full-screen-triangle
    // pipelines. The pipelines are extent-independent (dynamic viewport/scissor),
    // so they are created ONCE; only the target IMAGES + descriptor set writes are
    // recreated on resize (via createBloomTargets + writePostDescriptors).
    bool createPost();

    // Build a full-screen-triangle post pipeline (no vertex input, no depth, single
    // color attachment of `colorFmt`). `additiveBlend` selects ONE,ONE additive
    // blending (bloom upsample accumulation) vs. opaque write.
    bool createFullscreenPipeline(const char* vsPath, const char* fsPath,
                                  VkPipelineLayout layout, VkFormat colorFmt,
                                  bool additiveBlend, VkPipeline& outPipe,
                                  bool alphaBlend = false,
                                  // PREMULTIPLIED over-blend (ONE, ONE_MINUS_SRC_ALPHA) —
                                  // the volumetric pass emits fogColor*f + inscatter with
                                  // f in alpha, so it both extinguishes and ADDS light.
                                  bool premultipliedBlend = false);

    // (Re)write the post descriptor sets to point at the current HDR + bloom mip
    // image views. Called after createBloomTargets() at init + every resize. The
    // images are in SHADER_READ_ONLY when sampled (the graph transitions them), so
    // the descriptor imageLayout is SHADER_READ_ONLY_OPTIMAL.
    void writePostDescriptors();

    void destroyPost();

    // =====================================================================
    // SSAO setup. Builds: a NEAREST depth sampler (sample the depth image as
    // data), a CLAMP linear sampler (up-sample the AO), the depth pre-pass
    // pipeline (depth.vert, reuses m_shadowLayout = objSet), the SSAO + blur
    // full-screen pipelines, descriptor layouts/pool/sets, and the per-frame
    // SSAO + control UBOs. Extent-dependent images are made in createSsaoTargets.
    // =====================================================================
    bool createSsao();

    // Depth-only CAMERA pre-pass pipeline (depth.vert): writes the main depth
    // buffer from the camera's POV before lighting so SSAO has a full depth image.
    // Reuses m_shadowLayout (set0 = objSet); renders to m_depthFormat, no color.
    bool createDepthPrePipeline();

    // ---- Velocity pre-pass (#4) ---------------------------------------------
    // Build the velocity pipeline (velocity.vert/.frag -> RG16F MV target) and
    // its per-frame descriptor sets + UBO/prev-model buffers. Graceful: returns
    // true even if the velocity .spv is missing (the pass just stays disabled).
    bool createVelocityResources();
    void destroyVelocityResources();
    // Record the velocity pre-pass: re-rasterize the opaque draws (depth EQUAL),
    // binding dynVbo[frameIdx] as current + dynVbo[prevSlot] (or the static VBO)
    // as previous, writing the screen-space motion vector. Mirrors
    // recordDepthPrePassBody's draw loop.
    void recordVelocityPassBody(VkCommandBuffer cmd);

    // Create (or recreate) the half-res SSAO raw + blurred R8 targets at the
    // current frame extent. Called after createBloomTargets() at init + on resize.
    bool createSsaoTargets();

    void destroySsaoTargets();

    // (Re)write the SSAO descriptor sets that reference the depth + AO image views
    // (these change on resize). Called after createSsaoTargets() at init + resize.
    void writeSsaoDescriptors();

    // Write the scene TLAS into mesh set3 binding5 for ALL frames in flight
    // (the r_rtshadows ray origin). DOUBLE-BUFFER (#5): `slot` selects which
    // frame-in-flight descriptor set to re-point. The per-frame rebuild path passes
    // the CURRENT frame slot ONLY (safe: beginFrame waited that slot's inFlight
    // fence, so it is not referenced by pending work — no device wait needed). The
    // boot/resize/first-build paths pass kAllFrameSlots to rewrite every slot (idle
    // by construction). No-op without RT support / before the first TLAS exists.
    static constexpr uint32_t kAllFrameSlots = 0xFFFFFFFFu;
    void writeMeshTlasDescriptor(uint32_t slot = kAllFrameSlots);

    void destroySsao();

    // ======================================================================
    // RT AMBIENT OCCLUSION (hardware ray query) — lazy init / targets /
    // descriptors / per-frame TLAS build / compute dispatch / teardown.
    // ----------------------------------------------------------------------
    // Built LAZILY the first time RT AO is enabled on an RT-capable device (so a
    // run that never turns r_rtao on pays zero init cost). All teardown is in
    // destroyRt(), called from shutdown(). The compute pass writes a half-res R8 AO
    // image (GENERAL layout) via rayQueryEXT; a full-screen MULTIPLY apply pass then
    // darkens the linear HDR scene by the up-sampled AO before bloom.

    // Static trampolines so the C-style VulkanRT logger pointers reach x3::log*.
    static void rtLogInfo(const char* m);
    static void rtLogError(const char* m);

    // Lazily init the shared AS module (BLAS/TLAS manager) — used by BOTH the
    // RT-AO chain and the RT-reflections fallback. Returns true when the module
    // is ready. No-op (returns false) without RT support.
    bool ensureRtCore();

    // Lazily init the AS module + RT-AO pipelines/targets. Returns true when the RT
    // chain is ready to use this frame. No-op (returns false) without RT support.
    bool ensureRtaoReady();

    bool createRtao();

    // Full-screen-triangle pipeline whose single color attachment uses a MULTIPLY
    // blend (dstColor * srcColor): the apply pass outputs the AO darkening factor
    // and the blender multiplies it into the existing HDR target (no read-back).
    bool createMultiplyFullscreenPipeline(const char* vsPath, const char* fsPath,
                                          VkPipelineLayout layout, VkFormat colorFmt,
                                          VkPipeline& outPipe);

    // Create (or recreate) the half-res RT-AO R8 storage target at the current
    // extent. STORAGE (compute write) | SAMPLED (apply read).
    bool createRtaoTargets();

    void destroyRtaoTargets();

    // (Re)write the RT-AO descriptor sets (depth/AO image views + TLAS + UBO).
    // The TLAS write is refreshed each time the TLAS handle changes; called once
    // at build + whenever targets/TLAS are recreated.
    void writeRtaoDescriptors();

    // Re-point the TLAS binding into the RTAO / refl-RT / DDGI-ray compute sets.
    // DOUBLE-BUFFER (#5): `slot` selects which frame-in-flight set(s) to rewrite —
    // the per-frame ring rebuild passes the CURRENT slot only (no device wait), the
    // boot/resize paths pass kAllFrameSlots. With the TLAS ring the handle changes
    // on every build, so this runs each rebuild (re-pointing to the fresh slot).
    void rewriteRtaoTlas(uint32_t slot = kAllFrameSlots);

    // Build (or refit) the scene acceleration structures for THIS frame from the
    // per-frame draw list (m_drawRecords, still valid in endFrame after
    // prepareFrameData). Ensures a BLAS exists for every distinct mesh, then
    // rebuilds the TLAS with one instance per draw. Returns true if a usable TLAS
    // is ready. Called from endFrame BEFORE the graph records the frame.
    bool buildRtSceneAS();

    // X3_TLAS_VERIFY=1 only: byte-compare the partially-updated instance buffer
    // against a full naive repack of every row. See m_rtRowMirror.
    void verifyRtInstanceRows(uint32_t instCount);

    // X3_TLAS_VERIFY=2 only: drive the synthetic streaming-churn window.
    void rtChurnWindow(uint32_t recordCount);


    // =====================================================================
    // RT ACOUSTICS — ASYNC batched ray queries against the SAME scene TLAS
    // the RT screen effects use. Submit records + queues ONE small compute
    // dispatch (audio_rays.comp) with a fence and returns immediately;
    // harvest polls the fence next update and copies the hit distances out.
    // Async on purpose: a synchronous fence wait on the graphics queue would
    // stall the game thread behind the in-flight frame's GPU work (tens of
    // ms on heavy scenes); submit+harvest costs ~microseconds and audio
    // tolerates one update of latency. The first submit ARMS the per-frame
    // TLAS build (m_audioRaysWantFrames) and returns false until it exists.
    // =====================================================================
    bool traceAudioRaysSubmit(const AudioRay* rays, int count) override;

    int traceAudioRaysHarvest(float* outHitT, int capacity) override;

    // Lazily build the audio-ray batch chain: pipeline (audio_rays.comp) +
    // descriptor set {0 = TLAS, 1 = ray SSBO in, 2 = hit SSBO out} + the two
    // persistent host-visible buffers + a transient command buffer + fence.
    bool ensureAudioRays();

    void destroyAudioRays();

    // Fill the per-frame RT-AO compute UBO (invViewProj + camPos + tunables). Uses
    // the SAME viewProj prepareFrameData cached this frame.
    void prepareRtaoUbo();

    // Record the RT-AO compute dispatch body (the graph has emitted the AO-image
    // GENERAL transition + the depth READ_ONLY transition). Traces rayQueryEXT
    // against the TLAS and writes the half-res AO image.
    void recordRtaoComputeBody(VkCommandBuffer c);

    // ======================================================================
    // SSR / RAY-QUERY REFLECTIONS (refl.comp) — lazy init / targets /
    // descriptors / per-frame UBO / compute dispatch / teardown.
    // ----------------------------------------------------------------------
    // Built LAZILY the first time r_ssr activates (TAA on + history targets
    // exist), so runs that never enable reflections pay zero init cost. The SSR
    // pipeline (refl.comp.spv) is created on EVERY device; the ray-query variant
    // (refl_rt.comp.spv, TLAS at binding 4) only on RT devices — Pascal-class
    // hardware is automatically SSR-only. The first build does a device wait:
    // the ALWAYS-BOUND mesh set3 (binding 2 = this pass's output) must be
    // rewritten for all frames in flight.
    // ======================================================================
    bool ensureReflReady();

    bool createRefl();

    // (Re)create the reflection storage target at the current extent and quality
    // (r_reflquality: half- or full-res). The image is transitioned ONCE to
    // SHADER_READ_ONLY so the always-bound mesh set3 binding2 is layout-valid even
    // on frames where the refl pass doesn't run (it is then never sampled — the
    // ssao.refl.x gate is 0 — but the descriptor must still match the layout).
    bool createReflTargets();

    void destroyReflTargets();

    // (Re)write the refl compute sets (depth + output + TAA history + UBO; the RT
    // set also gets the TLAS when one exists). Called after target creation and on
    // resize (the depth/history/output views all change with the extent).
    void writeReflDescriptors();

    // Fill the per-frame reflection UBO. Uses the SAME (jittered) viewProj this
    // frame's depth was rasterized with (m_lastViewProj, cached by
    // prepareFrameData) + the previous frame's UNJITTERED viewProj captured there.
    void prepareReflUbo();

    // Record the reflections compute dispatch (the graph already emitted the
    // output GENERAL + depth READ_ONLY + history READ_ONLY transitions). Binds
    // the ray-query pipeline when the TLAS was built this frame, else SSR-only.
    void recordReflComputeBody(VkCommandBuffer c);

    // Record the DENOISE AUX dispatch (shaders/refl_aux.comp): reconstruct the
    // geometric normal + view distance from the depth buffer at the reflection
    // buffer's own grid, which is what refl_denoise.comp's edge stops read. It is
    // a SEPARATE pass rather than ~10 lines inside refl.comp specifically so
    // refl.comp's SPIR-V stays untouched and `r_refldenoise 0` stays bit-exact —
    // see the header of shaders/refl_aux.comp for the measurement that forced it.
    void recordReflAuxBody(VkCommandBuffer c);

    // Record ONE a-trous denoise iteration (shaders/refl_denoise.comp). `iter` is
    // the 0-based iteration index; the tap spacing is 1 << iter and the ping-pong
    // set is chosen from the same parity rule the graph uses, so the LAST write
    // always lands in m_reflDnImg[0] and mesh.frag's set3 binding 6 is a FIXED
    // descriptor with no per-frame churn.
    void recordReflDenoiseBody(VkCommandBuffer c, int iter);

    // Which of the two ping-pong images iteration `iter` writes, for a chain of
    // `total` iterations. Chosen so the final write is always index 0.
    static int reflDenoiseDstIdx(int iter, int total) { return (total - 1 - iter) & 1; }

    // Is the denoise stage both REQUESTED (r_refldenoise > 0) and BUILDABLE
    // (pipeline + all three targets present)? Everything that can fail during
    // creation is non-fatal, so this is the single predicate the graph, the UBO
    // and the descriptor writes all agree on.
    bool reflDenoiseWanted() const {
        return m_refl.denoiseIters > 0 && m_reflDnPipe != VK_NULL_HANDLE
            && m_reflAuxPipe != VK_NULL_HANDLE && m_reflAuxView != VK_NULL_HANDLE
            && m_reflDnView[0] != VK_NULL_HANDLE && m_reflDnView[1] != VK_NULL_HANDLE;
    }

    void destroyRefl();

    // ======================================================================
    // DDGI (r_ddgi) — lazy init / probe-volume fit / targets / descriptors /
    // per-frame UBO / compute dispatch / teardown. Tier: ray query + position
    // fetch ONLY (m_rtPosFetch); everything else never touches this.
    // ======================================================================
    bool ensureDdgiReady();

    // Fit the probe volume: an explicit volume from the params when given,
    // otherwise an AABB over THIS frame's static draw-record origins + padding
    // (instance origins approximate the playable volume well for the built
    // levels; cvar override available for exotic scenes). Sticky once fitted —
    // probes must be world-stable for the hysteresis to converge.
    void computeDdgiVolume();

    bool createDdgi();

    // (Re)create the octahedral atlases + the per-frame ray buffer for the
    // CURRENT grid counts. Atlas tile layout: tileU = px + py*countX (a
    // countX*countY tile row), tileV = pz; irradiance tiles are 8x8 texels
    // (6x6 interior + border), visibility 16x16 (14x14 + border). Both are
    // cleared to zero and left SHADER_READ_ONLY (the warm-up hysteresis ramp
    // fully overwrites them on the first update).
    bool createDdgiTargets();

    void destroyDdgiTargets();

    // (Re)write the per-frame DDGI compute sets. The TLAS binding (ray set,
    // binding 0) is written when one exists; otherwise rewriteRtaoTlas() fills
    // it after the first build (the set is never dispatched before then).
    void writeDdgiDescriptors();

    // Fill the per-frame DDGI UBO: grid geometry, the per-frame random ray
    // rotation, sun/ambient/lights for hit shading, and the EFFECTIVE
    // hysteresis — a cumulative-moving-average ramp (h = n/(n+1), capped at
    // the target) so a fresh/black probe field converges in a handful of
    // frames instead of fading in over seconds.
    void prepareDdgiUbo();

    // Record the DDGI ray dispatch (the graph already parked both atlases
    // SHADER_READ_ONLY for the feedback sample). The ray buffer is NOT a graph
    // resource — emit its WAR barrier (last frame's update READ it) here.
    void recordDdgiRaysBody(VkCommandBuffer c);

    // Record the DDGI update dispatches (the graph already transitioned both
    // atlases to GENERAL). Ray-buffer write -> read barrier first, then one
    // dispatch per atlas (push-constant mode selects irradiance/visibility).
    void recordDdgiUpdateBody(VkCommandBuffer c);

    void destroyDdgi();

    // Record the auto-exposure reduce/adapt dispatch (single 16x16 workgroup; the
    // graph has already transitioned the HDR scene to SHADER_READ_ONLY for the
    // compute sample). The exposure SSBO is NOT a graph resource, so this body
    // owns its hazards: a PRE barrier orders the dispatch after the previous
    // frame's composite fragment read (and any prior compute access — same
    // submission queue), and a POST barrier makes the new adapted value visible
    // to this frame's composite fragment read.
    void recordAutoExposureBody(VkCommandBuffer c);

    void destroyRt();

    // Build the baked hemisphere kernel + 4x4 rotation-noise tables ONCE (CPU,
    // deterministic) and stash them; prepareFrameData copies them into each
    // frame's SSAO UBO. Kernel: cosine-ish hemisphere samples, biased toward the
    // origin so near-surface occlusion dominates (standard SSAO weighting).
    void buildSsaoKernelAndNoise();

    // ======================================================================
    // GI — creation / targets / descriptors / teardown.
    // ======================================================================
    // Baked cosine-weighted hemisphere kernel (Malley's method: project a uniform
    // disk to the hemisphere) so the gather is importance-sampled toward the normal
    // — exactly the cosine weighting an irradiance integral wants. Deterministic
    // LCG (clean-room, identical across runs). Plus a 4x4 rotation-noise table.
    void buildGiKernelAndNoise();

    // Build the GI samplers, descriptor-set layouts, pool + sets, per-frame UBOs,
    // and the four full-screen GI pipelines. Extent-independent (dynamic viewport);
    // the half-res IMAGES + per-frame descriptor writes are (re)done in
    // createGiTargets()/writeGiDescriptors(). Reuses m_depthSampler (NEAREST) for
    // depth and m_ssaoLinearSampler (CLAMP linear) for colour/AO — both created by
    // createSsao(), which runs first.
    bool createGi();

    // Create (or recreate) the half-res GI targets + the full-res prev-depth copy
    // at the current frame extent. Called after createGi() at init + on resize.
    bool createGiTargets();

    void destroyGiTargets();

    // (Re)write the GI descriptor sets to point at the current image views. The
    // gather set (depth + scene + UBO) is stable per resize; the temporal/blur/
    // apply sets reference ping-pong accum views and are rewritten each frame in
    // prepareFrameData (cheap). Here we set the per-resize-stable bindings + the
    // gather/temporal UBOs. Sampler reuse: m_depthSampler (NEAREST), m_ssaoLinearSampler (LINEAR).
    void writeGiDescriptors();

    // Per-frame: point the temporal/blur/apply descriptor sets at the right
    // ping-pong accum buffers for this frame. `writeIdx` = accum we write this
    // frame, `histIdx` = accum we read as history. Cheap vkUpdateDescriptorSets;
    // no allocation. Called from prepareFrameData after m_giAccumWrite is chosen.
    void writeGiFrameDescriptors(uint32_t writeIdx, uint32_t histIdx, VkImageView aoView);

    void destroyGi();

    bool createPerFrame();

    void destroyPerFrame();

    // ---- Graphics: VMA + triangle pipeline (first geometry) ----
    static std::string exeDir();

    // =========================================================================
    // ZERO-STUTTER instrumentation (docs/ZERO_STUTTER.md).
    //
    // EVERY pipeline / shader-module / descriptor-pool creation and every VMA
    // buffer/image allocation in this file funnels through the x3Create* /
    // x3vmaCreate* wrappers below so the engine can PROVE nothing expensive is
    // created mid-frame:
    //   * total + per-frame counters feed the spike log + framePacing(),
    //   * any creation AFTER the first frame began counts as LATE and (under
    //     r_strictpso) logs a validation-style "[stutter]" error line,
    //   * pipeline creation goes through the persistent VkPipelineCache
    //     (loaded from disk at boot, saved at shutdown -> warm second boots).
    // Declared recreate boundaries (swapchain resize, editor-UI init, live
    // quality-setting rebuilds) set m_creationBoundary so they are exempt from
    // the strict assert — they are explicit hitch points, never mid-gameplay.
    // =========================================================================
    void noteCreate(const char* what, uint32_t& lateCounter, uint32_t& frameCounter);

    VkResult x3CreateGraphicsPipelines(uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                       const VkAllocationCallbacks* ac, VkPipeline* out);
    VkResult x3CreateComputePipelines(uint32_t n, const VkComputePipelineCreateInfo* ci,
                                      const VkAllocationCallbacks* ac, VkPipeline* out);
    VkResult x3CreateDescriptorPool(const VkDescriptorPoolCreateInfo* ci,
                                    const VkAllocationCallbacks* ac, VkDescriptorPool* out);
    VkResult x3vmaCreateBuffer(const VkBufferCreateInfo* bci, const VmaAllocationCreateInfo* aci,
                               VkBuffer* buf, VmaAllocation* alloc, VmaAllocationInfo* info);
    VkResult x3vmaCreateImage(const VkImageCreateInfo* ici, const VmaAllocationCreateInfo* aci,
                              VkImage* img, VmaAllocation* alloc, VmaAllocationInfo* info);

    // ---- VkPipelineCache persistence (ZERO-STUTTER step 2) -----------------
    static std::string pipelineCachePath();

    // Load the on-disk pipeline cache (if any) and create the VkPipelineCache all
    // x3Create*Pipelines calls feed. A stale/foreign blob (different GPU/driver)
    // is detected via the spec'd 32-byte header and ignored — cold boot compiles
    // everything and the save below replaces the file.
    void createPipelineCache();

    // Persist the pipeline cache beside the exe (called from shutdown(), after
    // waitIdle and before the device dies). Second boots then compile near-zero.
    void savePipelineCache();

    // ---- Frame-pacing ring + spike log (ZERO-STUTTER step 3) ---------------
    // Called at the very end of endFrame(): records the endFrame->endFrame CPU
    // wall delta + the latest GPU timestamp time, and logs ONE attribution line
    // for any post-warmup frame above the spike threshold. Warmup frames are not
    // recorded (boot compile / first-bake noise stays out of the percentiles).
    void recordFramePacing();

    VkShaderModule loadShaderModule(const std::string& relPath);

    // Raw SPIR-V words off disk (GpuCullSystem creates its own modules).
    std::vector<uint32_t> loadSpvWords(const std::string& relPath);

    // ---- D15 GPU-driven culling bring-up -----------------------------------
    // Caps detect + cull/HZB pipelines + the per-frame cull descriptor sets
    // (bindings 0..5 of cull.comp). Non-fatal: on any failure m_gpuCullReady
    // stays false and r_cullpath >= 1 silently falls back to the CPU path.
    bool createGpuCull();

    // ---- D15 stage 2: HZB depth pyramid (extent-tracking) -------------------
    // Mip 0 = half the render extent; full chain down to 1x1. Image lives in
    // GENERAL forever (transitioned once here); the per-frame write->read flips
    // are sync2 barriers inside recordHzbBuild. Rebuilt on resize (depth view
    // changes); marks last-frame depth invalid so the first post-resize frame
    // culls frustum-only.
    bool createHzbTargets();

    void destroyHzbTargets();

    void destroyGpuCull();

    // One-time staging copy into a fresh DEVICE_LOCAL buffer (transient cmd + fence).
    bool createDeviceLocalBuffer(const void* data, VkDeviceSize bytes,
                                 VkBufferUsageFlags usage,
                                 VkBuffer& outBuf, VmaAllocation& outAlloc);

    // Staging-upload an RGBA8 image into a single-mip sampled texture (+view+sampler).
    bool createSampledTexture(const void* rgba8, uint32_t w, uint32_t h, bool srgb, Texture& out);

    void destroyTextureObj(Texture& t);

    // ---- BOOT-TIME upload batching (docs/BOOT_TIME.md) ----------------------
    // Begin (or continue) the shared batch command buffer — DOUBLE-BUFFERED so a
    // new batch can record while the previous submit is still executing (the CPU
    // only blocks if BOTH slots are in flight). Returns VK_NULL_HANDLE only if
    // allocation fails (callers then fall back to oneTimeSubmit).
    VkCommandBuffer batchCmd();

    // Retire one submitted batch slot: wait its fence (or skip if not signaled and
    // non-blocking), free its staging buffers, reset its command buffer.
    void retireBatchSlot(uint32_t s, bool blocking);

    // SUBMIT every recorded batched upload in ONE submit — WITHOUT waiting. The
    // graphics queue executes in submission order, so any later frame/one-shot
    // submission sees the uploads complete on the GPU timeline; a trailing global
    // TRANSFER -> ALL_COMMANDS barrier makes the writes visible (the texture path
    // already ends in per-mip barriers). The staging buffers + fence stay pending
    // until waitUploadBatch() (forced before the fence/cmd are reused; opportunistic
    // non-blocking retire in beginFrame).
    void submitUploadBatch();

    // Retire previously submitted batches: wait their fences (blocking=true) or
    // only those already signaled, freeing staging buffers + resetting cmds.
    void waitUploadBatch(bool blocking = true);

    // Submit + fully retire (the original blocking semantics). Used by anything
    // that reuses the fence/cmd next (oneTimeSubmit) or tears down (shutdown).
    void flushUploadBatch();

    void beginUploadBatch() override;

    void endUploadBatch() override;

    // Record + submit a transient command buffer, wait on a one-shot fence.
    template <class Fn>
    bool oneTimeSubmit(Fn&& record) {
        std::lock_guard<std::recursive_mutex> lk(m_uploadMu);   // parallel preload safe
        // Ordering safety: any one-shot GPU op (BLAS build, readback, IBL bake…)
        // may depend on batched uploads — SUBMIT those first (no CPU wait needed:
        // same-queue submission order + the batch's trailing barrier make the
        // uploads land before this submit executes; this op's own fence wait
        // transitively covers them).
        submitUploadBatch();
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_uploadPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_dev.device, &ai, &cmd) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        record(cmd);
        vkEndCommandBuffer(cmd);

        VkCommandBufferSubmitInfo cmdS{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdS.commandBuffer = cmd;
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &cmdS;
        VkResult sr = vkQueueSubmit2(m_gfxQueue, 1, &submit, m_uploadFence);
        if (sr == VK_SUCCESS) {
            vkWaitForFences(m_dev.device, 1, &m_uploadFence, VK_TRUE, UINT64_MAX);
            vkResetFences(m_dev.device, 1, &m_uploadFence);
        }
        vkFreeCommandBuffers(m_dev.device, m_uploadPool, 1, &cmd);
        return sr == VK_SUCCESS;
    }

    bool createGraphics();

    // ---- Directional shadow mapping (perf-stack E) -------------------------
    // Create the shadow depth texture (+ view + compare sampler) and the set-2
    // descriptor (sampler2DShadow) the mesh fragment shader reads. Resolution is
    // fixed + swapchain-independent, so this is created once at init.
    bool createShadowImage();

    // Depth-only pipeline for the shadow pass: shadow.vert (lightViewProj*model),
    // no fragment shader, no color attachment, depth write/test LESS. set 0 =
    // the object SSBO + camera UBO (shadow.vert reads model rows + lightViewProj).
    // A small rasterizer depth bias supplements the shader's slope-scaled bias.
    bool createShadowPipeline();

    // Write one bindless array slot to point at `tex` (combined image+sampler).
    // update-after-bind means this is legal even while the set is bound: it only
    // rewrites the descriptor the NEXT frame reads (createTexture happens at load /
    // between frames; destroyTexture repoints the slot to white immediately, then
    // DEFERS the old image/view destruction until the in-flight frames retire).
    void writeBindlessSlot(uint32_t slot, const Texture& tex);

    // Assign `tex` the next free bindless slot + write the descriptor. Returns
    // false (and leaves bindlessIndex 0) if the array is full.
    bool registerBindless(Texture& tex);

    void destroyGraphics();

    // Core objects
    vkb::Instance m_inst{};
    vkb::Device   m_dev{};
    VkSurfaceKHR  m_surface = VK_NULL_HANDLE;
    VkQueue       m_gfxQueue = VK_NULL_HANDLE;
    uint32_t      m_gfxFamily = 0;
    bool          m_descriptorIndexing = false;
    bool          m_rtSupported = false;   // VK_KHR_ray_query + acceleration_structure enabled (RT P0)

    // ======================================================================
    // Hardware ray tracing (RT Phase 1+: BLAS/TLAS + ray-query RT AO).
    // ----------------------------------------------------------------------
    // The AS manager (BLAS per mesh + per-frame TLAS) lives in VulkanRT; the
    // ray-query RT-AO compute pass + its full-screen MULTIPLY apply pass live in
    // this class. EVERYTHING is gated behind m_rtao.enabled (the r_rtao cvar) AND
    // m_rtSupported; when off, none of this is built/dispatched and the raster +
    // SSAO/SSGI path is byte-for-byte unchanged.
    VulkanRT          m_rt;                                  // AS manager (ray-query)
    bool              m_rtInitTried = false;                 // lazy one-time module init
    // ---- SKINNED-CHARACTER TLAS REFIT (#3, r_skinnedrt) ------------------
    // When ON (default) AND m_rtSupported, buildRtSceneAS builds/refits a per-frame
    // BLAS for each visible skinned character and adds it to the multi-consumer
    // TLAS (RT shadows + reflections + DDGI + RT acoustics all then see monsters).
    // OFF (or non-RT GPU) -> skinned chars stay raster-only; static RT path is
    // byte-identical to the pre-feature behavior. Set via setSkinnedRtEnabled
    // (the r_skinnedrt cvar; CLI string wiring lives in app/ — engine exposes the
    // toggle + introspection so the feature is self-gating + testable here).
    bool              m_skinnedRtEnabled = true;             // r_skinnedrt (default ON)
    bool              m_skinnedRtThisFrame = false;          // a skinned BLAS was (re)built this frame
    uint32_t          m_skinnedRtInstances = 0;              // skinned chars added to the TLAS this frame
    bool              m_skinnedRtLogged = false;             // one-shot 0->N edge log latch
    RtaoParams        m_rtao{};                              // cached tunables (default OFF)
    bool              m_rtaoBuilt = false;                   // RT-AO pipelines created
    uint32_t          m_rtFrameSeed = 0;                     // per-frame noise seed
    // RT-AO half-res target (R8 storage image written by the compute pass; sampled
    // by the apply pass). GENERAL layout while the compute writes it.
    VkImage           m_rtaoImg   = VK_NULL_HANDLE; VmaAllocation m_rtaoAlloc = nullptr; VkImageView m_rtaoView = VK_NULL_HANDLE;
    VkExtent2D        m_rtaoExtent{};
    static constexpr VkFormat kRtaoFormat = VK_FORMAT_R8_UNORM;
    // RT-AO compute pass (rayQueryEXT): set0 = depth sampler + AO storage image +
    // TLAS + Rtao UBO.
    VkSampler             m_rtaoDepthSampler  = VK_NULL_HANDLE; // NEAREST depth
    VkSampler             m_rtaoLinearSampler = VK_NULL_HANDLE; // LINEAR AO up-sample
    VkDescriptorSetLayout m_rtaoSetLayout     = VK_NULL_HANDLE;
    VkPipelineLayout      m_rtaoLayout        = VK_NULL_HANDLE;
    VkPipeline            m_rtaoPipe          = VK_NULL_HANDLE;  // compute
    VkDescriptorSetLayout m_rtaoApplySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_rtaoApplyLayout    = VK_NULL_HANDLE;
    VkPipeline            m_rtaoApplyPipe      = VK_NULL_HANDLE; // full-screen MULTIPLY
    VkDescriptorPool      m_rtaoPool           = VK_NULL_HANDLE;
    VkDescriptorSet       m_rtaoSet[kFramesInFlight]      = {};  // compute set (per frame)
    VkDescriptorSet       m_rtaoApplySet[kFramesInFlight] = {};  // apply set (per frame)
    // Rtao compute UBO (std140; matches shaders/rtao.comp).
    struct RtaoUBO {
        glm::mat4 invViewProj;   // clip -> world
        glm::vec4 camPos;        // xyz = camera world pos
        glm::vec4 params0;       // x = radius, y = numRays, z = bias, w = strength
        glm::vec4 params1;       // x = outW, y = outH, z = frameSeed, w = power
    };
    static_assert(sizeof(RtaoUBO) == 64 + 16 * 3, "RtaoUBO std140 layout");
    VkBuffer      m_rtaoUboBuf[kFramesInFlight]  = {}; VmaAllocation m_rtaoUboAlloc[kFramesInFlight] = {}; void* m_rtaoUboMapped[kFramesInFlight] = {};
    // Apply-pass push constant (matches shaders/rtao_apply.frag).
    struct RtaoApplyPush { float aoTexel[2]; float strength; float pad0; };
    RtaoApplyPush m_rtaoApplyPush{};
    // ---- STATIC/DYNAMIC TLAS SPLIT (2026-08-11) ---------------------------
    // CPU-SIDE SHADOW of the TLAS instance buffer: everything that feeds one
    // packed VkAccelerationStructureInstanceKHR row, in the row's own order.
    // buildRtSceneAS compares each draw record against its shadow entry and
    // writes the mapped (write-combined) instance buffer ONLY on a mismatch, so
    // per frame the CPU touches the handful of rows that MOVED instead of all
    // 96,076. The mapped buffer is never read back — this is the readable copy.
    // Layout note: `model` first keeps the 64-byte memcmp 16-byte aligned.
    struct RtRowSrc {
        float           model[16];
        VkDeviceAddress blasAddr = 0;
        uint32_t        custom   = 0;
        uint32_t        pad      = 0;
    };
    std::vector<RtRowSrc> m_rtRowShadow;
    // ---- X3_TLAS_VERIFY=1: exhaustive proof that the partition is exact --------
    // A partially-updated TLAS input cannot be checked by a smoketest: a stale row
    // renders as slightly-wrong GI or a shadow from a car that already drove away,
    // and only sometimes. So the invariant is checked DIRECTLY, every frame, over
    // a live run: m_rtRowMirror is the exact byte image of what we wrote into the
    // (unreadable, write-combined) device instance buffer, and the verifier
    // independently repacks EVERY row the naive way and memcmps the two. Any
    // mismatch is a hard [ERROR] naming the row. Off by default and then neither
    // vector is touched, so this costs nothing in a normal run.
    bool m_rtVerify = false;                                   // X3_TLAS_VERIFY=1|2
    bool m_rtChurn  = false;                                   // X3_TLAS_VERIFY=2
    uint32_t m_rtChurnLo = 0, m_rtChurnHi = 0, m_rtChurnFrame = 0;
    bool m_rtVerifyChecked = false;                            // env read once
    uint64_t m_rtVerifyFrames = 0, m_rtVerifyBad = 0;
    // Material-lookup arm of the harness: instances checked / instances whose
    // instanceCustomIndex resolved to somebody else's material.
    uint32_t m_rtVerifyMatChecked = 0, m_rtVerifyMatBad = 0;
    std::vector<VkAccelerationStructureInstanceKHR> m_rtRowMirror;   // = device buffer
    std::vector<VkAccelerationStructureInstanceKHR> m_rtRowExpect;   // naive full repack
    uint32_t m_rtLastInstCount = 0;   // instance count of the last-built TLAS
    uint32_t m_rtStaticRows    = 0;   // rows left untouched this frame (telemetry)
    uint32_t m_rtDynamicRows   = 0;   // rows actually rewritten this frame (telemetry)
    uint32_t m_rtRowShifts     = 0;   // of those, ones whose OBJECT did not move -
                                      // only its compacted object-SSBO row index did
    bool m_rtaoActiveThisFrame = false;   // RT-AO chain added to the graph this frame
    // Stable storage for the RT-AO apply pass's VkRenderingInfo + attachment (the
    // graph holds pointers into these across execute()).
    VkRenderingAttachmentInfo m_rtaoApplyAttach{};
    VkRenderingInfo           m_rtaoApplyRenderInfo{};

    // ======================================================================
    // RT ACOUSTICS (traceAudioRays) — batched audio ray queries vs the TLAS.
    static constexpr uint32_t kAudioRayCapacity = 1024;   // rays per call (hard cap)
    static_assert(sizeof(AudioRay) == 32, "AudioRay must match audio_rays.comp std430 (two vec4s)");
    VkDescriptorSetLayout m_audioRaySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_audioRayLayout    = VK_NULL_HANDLE;
    VkPipeline            m_audioRayPipe      = VK_NULL_HANDLE;
    VkDescriptorPool      m_audioRayPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_audioRaySet       = VK_NULL_HANDLE;
    VkBuffer        m_audioRayInBuf  = VK_NULL_HANDLE; VmaAllocation m_audioRayInAlloc  = nullptr; void* m_audioRayInMapped  = nullptr;
    VkBuffer        m_audioRayOutBuf = VK_NULL_HANDLE; VmaAllocation m_audioRayOutAlloc = nullptr; void* m_audioRayOutMapped = nullptr;
    VkCommandPool   m_audioRayCmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_audioRayCmd     = VK_NULL_HANDLE;
    VkFence         m_audioRayFence   = VK_NULL_HANDLE;
    VkAccelerationStructureKHR m_audioRayTlasBound = VK_NULL_HANDLE; // descriptor currency
    bool m_audioRayBuilt  = false;
    bool m_audioRayFailed = false;        // one-shot create failure latch (no retry spam)
    bool m_audioRayInFlight = false;      // a submitted batch awaits harvest
    int  m_audioRayInFlightCount = 0;     // ray count of the in-flight batch
    int  m_audioRaysWantFrames = 0;       // >0: keep the scene TLAS built for audio

    // ======================================================================
    // SSR / RAY-QUERY REFLECTIONS (r_ssr / r_rtreflections) — refl.comp.
    // ----------------------------------------------------------------------
    // A half-res (or full-res, r_reflquality) compute pass after the depth
    // pre-pass: marches each pixel's reflection ray against the depth buffer and
    // samples LAST frame's lit scene (the TAA history image), with an optional
    // inline ray-query fallback into the SAME BLAS/TLAS the RT-AO path builds.
    // Output rgba16f (radiance + confidence) is sampled by mesh.frag (set 3,
    // binding 2) and blended into the split-sum IBL specular. Built LAZILY on
    // first enable; everything gated — off = zero passes, render unchanged.
    // Cached tunables. DEVICE default = ON (like the PostFXParams TAA default):
    // headless screenshot/smoketest paths that never push cvars still get the
    // shipped look. The r_ssr/r_rtreflections cvars + the --norefl/--notaa/
    // --legacypost pins override it via setReflectionParams. Reflections only
    // ever ACTIVATE when TAA is on, so every TAA-off path is still bit-identical.
    ReflectionParams m_refl{ /*ssr=*/true, /*rtFallback=*/true, /*fullRes=*/false, /*intensity=*/1.0f };
    bool       m_reflBuilt = false;                  // pipelines/targets created
    bool       m_reflFullRes = false;                // resolution the targets were built at
    bool       m_reflActiveThisFrame = false;        // refl pass in the graph this frame
    bool       m_reflRtThisFrame = false;            // ray-query fallback pipeline this frame
    bool       m_reflHistValid = false;              // TAA history usable as color source
    glm::mat4  m_reflPrevVP{ 1.0f };                 // prev frame's UNJITTERED viewProj
    VkImage    m_reflImg = VK_NULL_HANDLE; VmaAllocation m_reflAlloc = nullptr; VkImageView m_reflView = VK_NULL_HANDLE;
    VkExtent2D m_reflExtent{};
    static constexpr VkFormat kReflFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkDescriptorSetLayout m_reflSetLayout   = VK_NULL_HANDLE;  // SSR-only (no TLAS binding)
    VkDescriptorSetLayout m_reflSetLayoutRt = VK_NULL_HANDLE;  // + TLAS at binding 4 (RT devices)
    VkPipelineLayout      m_reflLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      m_reflLayoutRt    = VK_NULL_HANDLE;
    VkPipeline            m_reflPipe        = VK_NULL_HANDLE;  // refl.comp.spv (SSR-only)
    VkPipeline            m_reflPipeRt      = VK_NULL_HANDLE;  // refl_rt.comp.spv (+ ray query)
    VkDescriptorPool      m_reflPool        = VK_NULL_HANDLE;
    VkDescriptorSet       m_reflSet[kFramesInFlight]   = {};   // SSR set (per frame)
    VkDescriptorSet       m_reflSetRt[kFramesInFlight] = {};   // RT set (per frame; RT devices)
    // Refl compute UBO (std140; matches shaders/refl.comp).
    struct ReflUBO {
        glm::mat4 invViewProj;   // CURRENT (jittered) clip -> world
        glm::mat4 viewProj;      // CURRENT (jittered) world -> clip
        glm::mat4 prevViewProj;  // PREVIOUS frame's UNJITTERED world -> clip
        glm::vec4 camPos;        // xyz = camera, w = history valid
        glm::vec4 sunDir;        // xyz = toward sun, w = frame seed
        glm::vec4 ambient;       // rgb = scene ambient, w = unused
        glm::vec4 params0;       // x = outW, y = outH, z = max march dist, w = thickness
        glm::vec4 params1;       // x = march steps, y/z/w = unused
    };
    static_assert(sizeof(ReflUBO) == 64 * 3 + 16 * 5, "ReflUBO std140 layout");
    VkBuffer m_reflUboBuf[kFramesInFlight] = {}; VmaAllocation m_reflUboAlloc[kFramesInFlight] = {}; void* m_reflUboMapped[kFramesInFlight] = {};

    // ---- REFLECTION DENOISE (r_refldenoise; shaders/refl_denoise.comp) ----
    // The stage the reflection chain was missing. refl.comp wrote and mesh.frag
    // consumed RAW, while GI had a whole gather -> temporal -> denoise -> apply
    // chain for the same class of problem. Measured defect: on CTR.glb's
    // CTR_Body (base rough 0.4 / metal 0.8) the reflection arrives as blotchy
    // mottling — mean |px - 9x9 local mean| on flat door skin 5.53 (reflections
    // off) vs 7.69 (shipped) — and sweeping mesh.frag's consumer disc across
    // radii 0/6/14/24 moved it only 7.70/7.92/7.69/7.56, proving the noise is in
    // the BUFFER, not in the consumer kernel.
    //
    // The filter is an edge-aware a-trous wavelet with depth AND normal edge
    // stops and PREMULTIPLIED-confidence accumulation; its device-independent
    // definition (and the full rationale, including why roughness is NOT packed
    // into the buffer) lives in engine/rhi/ReflDenoise.h, which is what
    // --test-refldenoise asserts against.
    //
    // RESOURCES. m_reflAuxImg carries the per-texel geometry (rgb = world
    // normal, a = view distance) that refl_aux.comp reconstructs from depth, so
    // an edge stop costs ONE fetch per tap instead of re-deriving a normal from
    // depth 25 times per iteration. m_reflDnImg[2] are the a-trous ping-pong
    // targets; the iteration parity (reflDenoiseDstIdx) is chosen so the FINAL
    // write always lands in index 0, which is why mesh.frag's set3 binding 6 is
    // a fixed descriptor and no per-frame descriptor rewrite is needed. All
    // three are created with the refl targets — no per-frame allocation.
    //
    // BIT-EXACT OFF, and it is enforced structurally rather than by argument:
    //   * the aux + denoise passes are simply not added to the graph;
    //   * set3 binding 6 points at m_reflView — the SAME image binding 2 has;
    //   * SsaoControl.refl.w stays 0, which routes mesh.frag through
    //     sampleReflGlossy(), whose source text is unchanged;
    //   * refl.comp and refl_aux.comp are separate shaders, so refl.comp's
    //     SPIR-V is byte-identical to the pre-lane build.
    // The first cut of this lane did NOT do the last two — it folded a
    // `* discScale` (exactly 1.0 when off) into sampleReflGlossy and put the aux
    // store inside refl.comp — and the A/B against the pre-lane build came back
    // with +-1 LSB on ~0.17% of subpixels purely from shifted FMA contraction.
    static constexpr int kReflDenoiseMaxIters = 5;
    static constexpr VkFormat kReflAuxFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    int        m_reflDenoiseIters = 0;               // iterations the targets/descriptors were built for
    int        m_reflDenoiseThisFrame = 0;           // iterations actually in the graph this frame
    VkImage    m_reflAuxImg = VK_NULL_HANDLE; VmaAllocation m_reflAuxAlloc = nullptr; VkImageView m_reflAuxView = VK_NULL_HANDLE;
    VkImage    m_reflDnImg[2] = {}; VmaAllocation m_reflDnAlloc[2] = {}; VkImageView m_reflDnView[2] = {};
    VkDescriptorSetLayout m_reflDnSetLayout = VK_NULL_HANDLE;  // src sampler + dst storage + aux sampler
    VkPipelineLayout      m_reflDnLayout    = VK_NULL_HANDLE;
    VkPipeline            m_reflDnPipe      = VK_NULL_HANDLE;
    VkDescriptorPool      m_reflDnPool      = VK_NULL_HANDLE;
    // AUX pass (refl_aux.comp): depth sampler + aux storage image.
    VkDescriptorSetLayout m_reflAuxSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_reflAuxLayout    = VK_NULL_HANDLE;
    VkPipeline            m_reflAuxPipe      = VK_NULL_HANDLE;
    VkDescriptorSet       m_reflAuxSet       = VK_NULL_HANDLE;   // static views only -> written once
    struct ReflAuxPush {
        glm::mat4 invViewProj;   // CURRENT (jittered) clip -> world
        glm::vec4 camPos;        // xyz = camera world position
        glm::vec4 params0;       // x = W, y = H, zw = unused
    };
    static_assert(sizeof(ReflAuxPush) == 96, "ReflAuxPush must match refl_aux.comp");
    // FOUR static sets cover every (src, dst) pair the ping-pong can produce:
    //   [0] src = refl (raw)     -> dst = dn[0]      (first iteration, even count)
    //   [1] src = refl (raw)     -> dst = dn[1]      (first iteration, odd count)
    //   [2] src = dn[1]          -> dst = dn[0]
    //   [3] src = dn[0]          -> dst = dn[1]
    // They reference only static views, so they are written ONCE at build time.
    VkDescriptorSet m_reflDnSet[4] = {};
    // Push constants (matches the Push block in shaders/refl_denoise.comp).
    struct ReflDnPush {
        glm::vec4 params0;   // x = W, y = H, z = tap spacing, w = depthSigma
        glm::vec4 params1;   // x = normalPow, yzw = reserved
    };
    static_assert(sizeof(ReflDnPush) == 32, "ReflDnPush must match refl_denoise.comp");
    // Per-iteration record context. RenderPassDesc::record is a RAW function
    // pointer (deliberately, to keep std::function heap traffic out of the frame
    // loop), so the iteration index travels through a stable member the same way
    // the glass-frost mip chain passes its level. Fixed array = no per-frame
    // allocation.
    struct ReflDnPassCtx { VulkanRenderDevice* self = nullptr; int iter = 0; };
    ReflDnPassCtx m_reflDnCtx[kReflDenoiseMaxIters]{};

    // ======================================================================
    // DDGI — dynamic diffuse global illumination (r_ddgi; ddgi_rays.comp +
    // ddgi_update.comp + mesh.frag set3 bindings 3/4).
    // ----------------------------------------------------------------------
    // Probe-grid GI (Majercik et al. 2019): per-frame inline-ray-query rays
    // from every probe against the SHARED scene TLAS, simple per-object hit
    // shading (instanceCustomIndex -> ObjectData SSBO row), hysteresis-blended
    // into octahedral irradiance (8x8/probe, RGBA16F) + visibility-depth
    // (16x16/probe, RG16F) atlases that mesh.frag interpolates with Chebyshev
    // leak rejection. Tier-gated on ray query + position fetch; built lazily.
    bool       m_rtPosFetch = false;          // VK_KHR_ray_tracing_position_fetch enabled
    DdgiParams m_ddgi{};                      // cached tunables (default OFF)
    bool       m_ddgiBuilt = false;           // pipelines/targets created
    bool       m_ddgiWantThisFrame = false;   // decided in prepareFrameData (UBO gate)
    bool       m_ddgiActiveThisFrame = false; // passes in the graph this frame (TLAS ok)
    uint32_t   m_ddgiFrameCount = 0;          // updates since (re)activation (warm-up ramp)
    // The fitted/configured probe volume (sticky once activated; refit on toggle
    // or when the caller passes an explicit volume).
    glm::vec3  m_ddgiOrigin{ 0.0f };          // grid min corner (world)
    glm::vec3  m_ddgiSpacing{ 1.0f };         // probe spacing (m)
    int        m_ddgiCountX = 0, m_ddgiCountY = 0, m_ddgiCountZ = 0; // built atlas dims
    bool       m_ddgiVolumeValid = false;
    float      m_ddgiVisMaxDist = 4.0f;       // visibility clamp (1.5 * spacing diagonal)
    // Octahedral atlases (persistent across frames; graph-imported with tracked
    // state like taa.hist). Irradiance: 8x8 texels/probe; visibility: 16x16.
    VkImage    m_ddgiIrrImg = VK_NULL_HANDLE; VmaAllocation m_ddgiIrrAlloc = nullptr; VkImageView m_ddgiIrrView = VK_NULL_HANDLE;
    VkImage    m_ddgiVisImg = VK_NULL_HANDLE; VmaAllocation m_ddgiVisAlloc = nullptr; VkImageView m_ddgiVisView = VK_NULL_HANDLE;
    ResourceState m_ddgiIrrState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
    ResourceState m_ddgiVisState{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
    static constexpr VkFormat kDdgiIrrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDdgiVisFormat = VK_FORMAT_R16G16_SFLOAT;
    // Per-frame ray results: probeCount * 128 * vec4 (radiance + signed distance).
    VkBuffer   m_ddgiRayBuf = VK_NULL_HANDLE; VmaAllocation m_ddgiRayAlloc = nullptr;
    VkDeviceSize m_ddgiRayBufSize = 0;
    VkSampler  m_ddgiSampler = VK_NULL_HANDLE;            // LINEAR/CLAMP atlas sampler
    VkDescriptorSetLayout m_ddgiRaySetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ddgiUpSetLayout  = VK_NULL_HANDLE;
    VkPipelineLayout      m_ddgiRayLayout    = VK_NULL_HANDLE;
    VkPipelineLayout      m_ddgiUpLayout     = VK_NULL_HANDLE;
    VkPipeline            m_ddgiRayPipe      = VK_NULL_HANDLE;  // ddgi_rays.comp
    VkPipeline            m_ddgiUpPipe       = VK_NULL_HANDLE;  // ddgi_update.comp
    VkDescriptorPool      m_ddgiPool         = VK_NULL_HANDLE;
    VkDescriptorSet       m_ddgiRaySet[kFramesInFlight] = {};
    VkDescriptorSet       m_ddgiUpSet[kFramesInFlight]  = {};
    // DDGI compute UBO (std140; matches the Ddgi block in ddgi_rays.comp AND
    // ddgi_update.comp — one layout, two consumers).
    struct DdgiUBO {
        glm::vec4  gridOrigin;       // xyz = grid min corner, w = raysPerProbe
        glm::vec4  gridSpacing;      // xyz = spacing (m), w = visMaxDist (m)
        glm::ivec4 gridCounts;       // xyz = probe counts, w = frame index
        glm::vec4  rotation0;        // per-frame random rotation rows (3x3)
        glm::vec4  rotation1;
        glm::vec4  rotation2;
        glm::vec4  sunDirIntensity;  // xyz = toward sun, w = sun diffuse scale
        glm::vec4  ambientSky;       // rgb = flat ambient, w = env cube valid
        glm::vec4  params;           // x = hystIrr, y = lightCount, z = bounceGain, w = hystVis
        GpuPointLight lights[kMaxPointLights];
    };
    static_assert(sizeof(DdgiUBO) == 16 * 9 + kMaxPointLights * 32, "DdgiUBO std140 layout");
    VkBuffer m_ddgiUboBuf[kFramesInFlight] = {}; VmaAllocation m_ddgiUboAlloc[kFramesInFlight] = {}; void* m_ddgiUboMapped[kFramesInFlight] = {};
    // SSBO row of each draw record this frame (TLAS instanceCustomIndex source —
    // the grouped SSBO write order differs from the draw-record order).
    std::vector<uint32_t> m_recordSsboRow;
    // ---- Stable RT material table (see RtMaterialGpu) ------------------------
    // CPU shadow of what each frame slot's rtMatBuf already holds, so a frame
    // only writes the rows whose material actually CHANGED. Materials are near
    // static, so in the steady state this write is ~free — which is the whole
    // reason a per-record table is affordable where a per-record ObjectData
    // (160 B/row) would not have been. One shadow per frame-in-flight because
    // the slots are written independently.
    std::vector<RtMaterialGpu> m_rtMatShadow[kFramesInFlight];
    uint32_t                   m_rtMatRowsWritten = 0;   // telemetry: rows written this frame

    // Graphics
    VmaAllocator  m_alloc = nullptr;
    VkPipeline       m_meshPipeline = VK_NULL_HANDLE;        // SSAO path: depth EQUAL, no write
    VkPipeline       m_meshPipelineNoSsao = VK_NULL_HANDLE;  // no-SSAO path: depth LESS, write
    // Interior reflection probe: a clone of the no-SSAO mesh PSO using mesh_probe.vert
    // (per-face viewProj via push constant) to bake scene geometry into the IBL env cube.
    VkPipeline       m_meshProbePipe = VK_NULL_HANDLE;
    VkPipelineLayout m_meshProbeLayout = VK_NULL_HANDLE;     // 5 mesh sets + mat4 push (VERTEX)
    VkImage          m_probeDepthImg = VK_NULL_HANDLE; VmaAllocation m_probeDepthAlloc = nullptr;
    VkImageView      m_probeDepthView = VK_NULL_HANDLE;      // env-face-sized depth for the bake
    bool             m_iblProbeScene = false;                // bake scene into env (vs sky-only)
    glm::vec3        m_iblProbePos{ 0.0f, 1.6f, 0.0f };      // probe world position (set to cam at bake)
    VkPipeline       m_meshPipelineTransparent = VK_NULL_HANDLE;  // BLEND: src-alpha over, LEQUAL, no depth-write, cull NONE
    // RT soft-shadow variants (mesh_rt.frag.spv; created only on ray-query
    // devices, bound only on frames where the TLAS descriptor is valid). Same
    // pipeline LAYOUT as the plain set (set3 just has one extra binding on RT
    // devices); identical fixed-function state per-variant.
    VkPipeline       m_meshPipelineRt            = VK_NULL_HANDLE; // EQUAL/no-write (pre-pass on)
    VkPipeline       m_meshPipelineNoSsaoRt      = VK_NULL_HANDLE; // LESS/write (no pre-pass)
    VkPipeline       m_meshPipelineTransparentRt = VK_NULL_HANDLE; // BLEND variant
    RtShadowParams   m_rtShadows{};                    // cached r_rtshadows tunables (default tier 2)
    bool             m_rtShadowsWantThisFrame   = false; // decided in prepareFrameData (UBO gate)
    bool             m_rtShadowsActiveThisFrame = false; // decided in endFrame (TLAS ready + descriptor written)
    bool             m_meshTlasWritten = false;          // mesh set3 binding5 points at a real TLAS
    uint32_t         m_rtshFrameSeed = 0;                // per-frame jitter rotation counter
    VkPipelineLayout m_meshLayout = VK_NULL_HANDLE;
    // Translucent GLASS pipeline (transparent pass). Shares mesh.vert + sets 0-3
    // with the opaque mesh path, but uses its OWN pipeline layout m_glassLayout
    // (sets 0-3 identical, plus set 4 = glass-specific: scene-color copy sampler +
    // glass control UBO) so glass.frag can sample the scene behind it (refraction
    // M2 / frost M4) without touching the locked mesh layout. Depth-test
    // LESS_OR_EQUAL, depth-write OFF, alpha blend, cull NONE (double-sided glass).
    // Created in createMeshPipeline after the opaque ones. Graceful fallback
    // (spec §5): if any glass object fails to create it stays NULL and the glass
    // pass is skipped (the opaque path is never broken).
    VkPipeline       m_glassPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_glassLayout   = VK_NULL_HANDLE;
    // Glass set 4: binding0 = scene-color copy (mip-aware sampler), binding1 =
    // GlassControl UBO (per-frame: camera world pos, time, screen size, dev cvar
    // overrides). The layout is created once; the per-frame UBO + descriptor set
    // are written each frame / on resize (the scene-copy view changes on resize).
    VkDescriptorSetLayout m_glassSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_glassPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_glassSet[kFramesInFlight] = {};
    VkBuffer      m_glassCtrlBuf[kFramesInFlight]    = {};
    VmaAllocation m_glassCtrlAlloc[kFramesInFlight]  = {};
    void*         m_glassCtrlMapped[kFramesInFlight] = {};
    // Frost (M4): a fullscreen-triangle downsample pipeline + per-level descriptor
    // sets (one per blur-source image) that blur the scene copy into m_glassFrostImg[].
    // Reuses the bloom-down shader/layout (m_bloomLayout). NULL until M4 builds it,
    // which keeps the frost passes off (glassFrostOn=false) for M2/M3.
    VkPipeline      m_glassFrostPipe = VK_NULL_HANDLE;
    VkDescriptorSet m_glassFrostSrcSet[kGlassFrostLevels + 1] = {}; // [0]=scene copy, [1..]=levels
    // Per-frame GlassControl UBO (std140; matches glass.frag GlassControl block).
    // camPos.xyz = camera world position (fresnel/specular view vector), .w = time
    // (animated glint). screen.xy = 1/width, 1/height (gl_FragCoord -> screen UV);
    // .z = scene-copy max mip level (frost lod clamp), .w unused. ctrl = dev cvar
    // OVERRIDES: x = refraction scale, y = roughness add, z = specular scale, w = 1
    // when any override is active (else 0 -> shader uses the per-object material).
    struct GlassControl {
        glm::vec4 camPos;     // xyz = camera world pos, w = time (seconds)
        glm::vec4 screen;     // x = 1/W, y = 1/H, z = maxMip, w = sceneCopyValid (0/1)
        glm::vec4 ctrl;       // x = refractScale, y = roughAdd, z = specScale, w = overrideOn
        glm::vec4 camRight;   // xyz = camera RIGHT axis (world) — screen-space normal.x
        glm::vec4 camUp;      // xyz = camera UP axis (world)    — screen-space normal.y
    };
    // Live dev-cvar glass overrides (r_glass_*), pushed via setGlassDevParams.
    GlassDevParams m_glassDev{};
    // Wall-clock seconds since device init, for the animated specular glint (M3).
    std::chrono::steady_clock::time_point m_glassClockStart = std::chrono::steady_clock::now();

    // ---- Procedural planet body (FORGE3D port) — dedicated per-TYPE pipelines.
    // Each clones the OPAQUE mesh PSO (same MeshVertex input, depth LESS+write, cull
    // BACK, no blend) but with planet.vert + a per-type fragment shader, all sharing
    // the SAME layout (set0 bindless + set1 camera UBO + a 128-byte push constant).
    // The 9 OPAQUE planet types, in pipeline-index order:
    enum PlanetType : uint32_t {
        PT_Moon = 0, PT_Ice, PT_Gas, PT_Lava, PT_Terrestrial,
        PT_Oceanic, PT_Sand, PT_Thunderstorm, PT_Sun,
        // TRANSPARENT glow layers (drawn AFTER the opaque bodies, compositing OVER
        // them). Atmosphere/SunCorona = ADDITIVE shells; Ring = ALPHA annulus.
        PT_Atmosphere, PT_SunCorona, PT_Ring,
        PT_Count,
        PT_OpaqueCount = PT_Atmosphere   // [0..PT_OpaqueCount) are the 9 opaque bodies
    };
    VkPipeline       m_planetPipelines[PT_Count] = {};   // [0] is the old m_planetPipeline
    VkPipelineLayout m_planetPipelineLayout = VK_NULL_HANDLE;
    // Generalized push-constant block (mirrors PC {} in planet.vert + every per-type
    // frag): mat4 model (64B) + uint tex[12] (48B) + float uTime (4B) + float _p0 +
    // uint _p1 + uint _p2 (12B) = 128B exactly (the full push-constant range).
    struct PlanetPush {
        float    model[16];   // 64B
        uint32_t tex[12];     // 48B — bindless texture indices, per-type slot mapping
        float    uTime;       //  4B
        float    _p0;         //  4B
        uint32_t _p1;         //  4B
        uint32_t _p2;         //  4B
    };
    static_assert(sizeof(PlanetPush) == 128, "PlanetPush must be 128 bytes (push-constant range)");
    // One queued planet draw for THIS frame (resolved bindless indices + mesh id +
    // which per-type pipeline to bind). tex[] holds up to 12 resolved bindless idx.
    struct PlanetDraw {
        float    model[16];
        uint32_t tex[12];
        float    uTime;
        uint32_t typeIndex;   // PlanetType -> which m_planetPipelines[] to bind
        uint32_t meshId;
    };
    std::vector<PlanetDraw> m_planetDraws;

    // GPU-driven descriptor objects:
    //   set 0 = bindless texture array (one shared set, update-after-bind)
    //   set 1 = per-frame object SSBO + camera UBO (allocated in createGraphics)
    VkDescriptorSetLayout m_bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_bindlessPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_bindlessSet    = VK_NULL_HANDLE;
    uint32_t              m_nextBindless   = 0;   // next free bindless slot
    // Terrain material splat: the marker handle id returned by
    // registerTerrainMaterial() + the five resolved detail bindless indices
    // (grass, rock, snow, sand, high-altitude rock — [4] optional, 0 = absent).
    // 0 marker id == no terrain material registered.
    uint32_t              m_terrainMarkerId = 0;
    uint32_t              m_terrainTexIdx[5] = { 0, 0, 0, 0, 0 };
    uint32_t              m_terrainNrmIdx[4] = { 0, 0, 0, 0 };
    VkDescriptorSetLayout m_objSetLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      m_objPool        = VK_NULL_HANDLE;

    // Max distinct meshes per frame (sizes the indirect-command buffer).
    static constexpr uint32_t kMaxDrawMeshes = 4096;

    // Per-frame draw accumulation (GPU-driven). m_drawRecords is filled by
    // drawMesh(); prepareFrameData() groups it (m_groups/m_groupOrder reused to
    // avoid per-frame allocs) into the SSBO + indirect buffer, and the graph's
    // shadow/color passes replay the multidraw from it.
    std::vector<DrawRecord> m_drawRecords;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_groups;
    std::vector<uint32_t>   m_groupOrder;
    uint32_t                m_drawMeshOrder[kMaxDrawMeshes] = {};
    // Per-indirect-command flag: group contains >=1 alphaMode==MASK (cutout)
    // instance -> the reflections depth pre-pass uses the alpha-testing pipeline
    // for it (recordDepthPrePassBody). Parallel to m_drawMeshOrder.
    uint8_t                 m_drawMeshCutout[kMaxDrawMeshes] = {};
    // Per-frame data preparation (camera/light UBO + SSBO + indirect) is shared by
    // the shadow depth pass AND the main color pass, so it runs once (guarded) and
    // caches the number of indirect commands produced.
    bool                    m_framePrepared = false;
    uint32_t                m_frameCmdCount = 0;
    uint32_t                m_frameCmdOpaque = 0;  // [0,opaque)=opaque cmds, [opaque,count)=BLEND (glass)
    // Number of GLASS-flagged instances submitted this frame (counted in the SSBO
    // fill). When 0 the transparent glass pass is skipped entirely (spec §5: zero
    // cost when no glass is visible).
    uint32_t                m_frameGlassCount = 0;

    // ---- Analytic sky pipeline (open-world track, task A) ------------------
    // A vertexless full-screen triangle drawn at the START of the main color pass
    // (after the clear, before meshes) at far depth with depthTest=LESS_OR_EQUAL +
    // depthWrite=OFF, so it fills only un-covered pixels and geometry occludes it.
    // set 0 = the per-frame sky UBO (invViewProj + sun/haze params). Disabled by
    // default; only drawn when setSkyParams(enabled=true).
    VkPipeline            m_skyPipeline  = VK_NULL_HANDLE;
    VkPipelineLayout      m_skyLayout    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_skySetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_skyPool      = VK_NULL_HANDLE;
    SkyParams             m_sky{};   // cached params (enabled=false by default)
    float                 m_skyTime = 0.0f;  // sky-animation time (seconds), -> sky UBO params.z

    // ---- Image-based lighting (IBL) — split-sum environment reflections ----
    // Foundation for SSR + the RT tier. Three cube/2D resources baked from the
    // analytic sky: a diffuse IRRADIANCE cube (32px), a roughness-mipped specular
    // PREFILTER cube (128px base, kIblPrefilterMips), and a scene-independent BRDF
    // LUT (256px RG16F). An intermediate ENV cube (256px, mipped) is captured from
    // the analytic-sky math, then convolved. mesh.frag set 4 samples these for
    // split-sum IBL (replacing the flat ambient*Fresnel constant). The bakes run
    // on a one-time submit at init + whenever setSkyParams changes the sky, so the
    // per-frame cost is zero (only the three texture fetches in the shader).
    static constexpr uint32_t kIblEnvSize        = 256;  // env cube face edge
    static constexpr uint32_t kIblIrradSize      = 32;   // irradiance cube face edge
    static constexpr uint32_t kIblPrefilterSize  = 128;  // prefilter cube mip0 edge
    static constexpr uint32_t kIblPrefilterMips  = 5;    // roughness mip count (128->8)
    static constexpr uint32_t kIblBrdfSize       = 256;  // BRDF LUT edge
    static constexpr VkFormat kIblCubeFormat     = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kIblBrdfFormat     = VK_FORMAT_R16G16_SFLOAT;

    bool m_iblReady = false;          // all resources created (mesh.frag may sample)
    bool m_iblBaked = false;          // a valid environment was baked at least once
    bool m_iblDirty = true;           // sky changed -> rebake on next frame prep
    // Env (intermediate) cube: captured analytic sky, full mip chain for prefilter.
    VkImage     m_iblEnvImg = VK_NULL_HANDLE;  VmaAllocation m_iblEnvAlloc = nullptr;
    VkImageView m_iblEnvCubeView = VK_NULL_HANDLE;            // CUBE view (all mips) for sampling
    VkImageView m_iblEnvFaceView[6] = {};                    // per-face mip0 RT views (capture)
    uint32_t    m_iblEnvMips = 1;
    // Irradiance cube (diffuse IBL).
    VkImage     m_iblIrradImg = VK_NULL_HANDLE; VmaAllocation m_iblIrradAlloc = nullptr;
    VkImageView m_iblIrradCubeView = VK_NULL_HANDLE;
    VkImageView m_iblIrradFaceView[6] = {};
    // Prefilter cube (specular IBL), one RT view per (mip,face).
    VkImage     m_iblPrefImg = VK_NULL_HANDLE;  VmaAllocation m_iblPrefAlloc = nullptr;
    VkImageView m_iblPrefCubeView = VK_NULL_HANDLE;
    VkImageView m_iblPrefFaceView[kIblPrefilterMips][6] = {};
    // BRDF LUT (2D RG16F).
    VkImage     m_iblBrdfImg = VK_NULL_HANDLE;  VmaAllocation m_iblBrdfAlloc = nullptr;
    VkImageView m_iblBrdfView = VK_NULL_HANDLE;
    // Samplers: cube (linear+mip, clamp) shared by env/irradiance/prefilter; 2D for the LUT.
    VkSampler   m_iblCubeSampler = VK_NULL_HANDLE;
    VkSampler   m_iblBrdfSampler = VK_NULL_HANDLE;
    // Pipelines (fullscreen-triangle, render to one cube face / mip at a time).
    VkPipeline       m_iblEnvPipe = VK_NULL_HANDLE;
    VkPipeline       m_iblIrradPipe = VK_NULL_HANDLE;
    VkPipeline       m_iblPrefPipe = VK_NULL_HANDLE;
    VkPipeline       m_iblBrdfPipe = VK_NULL_HANDLE;
    VkPipelineLayout m_iblEnvLayout = VK_NULL_HANDLE;     // UBO set0 + face push
    VkPipelineLayout m_iblCubeLayout = VK_NULL_HANDLE;    // cube sampler set0 + face/misc push
    VkPipelineLayout m_iblBrdfLayout = VK_NULL_HANDLE;    // no sets, no push
    // set0 for the IBL UBO (env capture) + set0 for a cube sampler (convolve passes).
    VkDescriptorSetLayout m_iblSkyUboSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_iblCubeSetLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      m_iblBakePool = VK_NULL_HANDLE;
    VkDescriptorSet       m_iblSkyUboSet = VK_NULL_HANDLE;   // -> env capture
    VkDescriptorSet       m_iblEnvCubeSet = VK_NULL_HANDLE;  // env cube -> irradiance/prefilter
    VkBuffer m_iblSkyUboBuf = VK_NULL_HANDLE; VmaAllocation m_iblSkyUboAlloc = nullptr; void* m_iblSkyUboMapped = nullptr;
    // mesh.frag set 4 (IBL): binding0 irradiance cube, binding1 prefilter cube, binding2 BRDF LUT.
    VkDescriptorSetLayout m_iblMeshSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_iblMeshPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_iblMeshSet = VK_NULL_HANDLE;

    // 2D HUD overlay pipeline (NDC quads, no depth, alpha-blended)
    VkPipeline       m_hudPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_hudLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_hudSetLayout = VK_NULL_HANDLE;
    std::vector<HudVertex> m_hudScratch;        // CPU scratch for text batching

    // ---- Role-based glyph atlases (modern HUD/UI fonts) --------------------
    // One baked atlas per FontRole (see IRenderDevice.h). Each is baked once at
    // init from a TTF in assets/fonts/ (kRoleFontPaths) via stb_truetype, with the
    // embedded Roboto Mono as the guaranteed fallback so text is NEVER blank.
    // Proportional roles advance the pen by each glyph's real width; monospace
    // roles advance by a fixed cell (cellAdvance) so legacy N*px layout stays exact.
    // Per-glyph atlas rects/offsets/advance are stored in bake-pixel units so they
    // scale to any requested px at draw time.
    static constexpr int   kTtfFirstChar = 32;   // ASCII space
    static constexpr int   kTtfCharCount = 95;   // 32..126 inclusive
    static constexpr int   kTtfAtlasW    = 1024;  // fits 95 glyphs @ 48px w/ 2x2 oversampling
    static constexpr int   kTtfAtlasH    = 1024;
    static constexpr float kTtfBakePx    = 48.0f; // bake size (oversampled for crispness)
    struct TtfGlyph {
        float u0, v0, u1, v1;   // atlas UVs
        float x0, y0, x1, y1;   // quad offsets in bake-pixel units (relative to pen, baseline at y=0)
        float advance;          // horizontal advance in bake-pixel units
    };
    struct FontAtlas {
        Texture  tex{};                       // R8-coverage-as-RGBA glyph atlas
        TtfGlyph glyphs[kTtfCharCount]{};
        float    cellAdvance  = kTtfBakePx;   // monospace cell width (bake px); proportional uses per-glyph advance
        float    ascent       = kTtfBakePx;   // baseline offset from cell top (bake px)
        bool     proportional = false;        // true => advance per glyph; false => fixed cell
        bool     ready        = false;        // a TTF atlas baked OK for this role
    };
    static constexpr int kFontRoleCount = (int)x3::rhi::FontRole::Count;  // 5
    FontAtlas m_fonts[kFontRoleCount]{};
    bool      m_bitmapFontReady = false;       // legacy 8x8 fallback baked (no TTF at all)
    Texture   m_bitmapFontTex{};               // 8x8 atlas used when a role has no TTF

    // One-time staging upload (transient pool + fence)
    VkCommandPool m_uploadPool = VK_NULL_HANDLE;
    VkFence       m_uploadFence = VK_NULL_HANDLE;

    // ---- BOOT-TIME upload batching (docs/BOOT_TIME.md) ----------------------
    // While m_batchActive, createDeviceLocalBuffer/createSampledTexture record
    // into m_batchCmd instead of one blocking oneTimeSubmit each; staging buffers
    // are kept alive in m_batchStagings until the single flush. oneTimeSubmit and
    // beginFrame auto-flush a pending batch first, so any GPU op that could read
    // batched data executes strictly after the uploads land.
    bool          m_batchActive = false;            // begin/endUploadBatch window
    bool          m_batchOpen   = false;            // current slot has recorded work
    uint32_t      m_batchSlot   = 0;                // recording slot (double-buffered)
    bool          m_batchSubmittedSlot[2] = { false, false };
    VkCommandBuffer m_batchCmds[2]  = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkFence         m_batchFences[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    std::vector<std::pair<VkBuffer, VmaAllocation>> m_batchStagings;        // recording
    std::vector<std::pair<VkBuffer, VmaAllocation>> m_batchInFlightSlot[2]; // submitted
    uint32_t      m_batchOps    = 0;                // uploads in the current batch
    uint32_t      m_batchFlushes = 0;               // receipts: flushes this batch window
    double        m_batchMs     = 0.0;              // receipts: wall ms spent flushing
    // Upload-path guard: createMesh/createTexture/destroyMesh/destroyTexture/
    // oneTimeSubmit may be called from the BOOT-TIME parallel model-preload
    // threads (x3::asset::preloadModels). Recursive: createMesh -> oneTimeSubmit
    // re-enters on the unbatched path. Frame/draw/skinning paths remain main-
    // thread-only (unchanged contract).
    std::recursive_mutex m_uploadMu;

    // ---- Offscreen capture (fix 1: in-frame, acquired-image copy) ----------
    // armCapture() arms a request; endFrame() then records the color-image -> host
    // readback copy INSIDE the live frame (operating on the freshly-rendered,
    // properly-acquired swapchain image, before the PRESENT_SRC transition). The
    // copy completes when that frame's inFlight fence signals. captureFrame()
    // (called after that endFrame) waits on the captured frame's fence, maps the
    // readback buffer, swizzles + writes the PNG. No non-acquired image is read.
    bool          m_captureArmed = false;     // a capture is pending for this frame
    bool          m_captureReady = false;     // the in-frame copy was recorded
    VkBuffer      m_captureBuf   = VK_NULL_HANDLE;
    VmaAllocation m_captureAlloc = nullptr;
    void*         m_captureMapped = nullptr;
    uint32_t      m_captureW = 0, m_captureH = 0;
    uint32_t      m_captureFrameSlot = 0;     // m_frameIdx the copy was recorded into
    VkFence       m_captureFence = VK_NULL_HANDLE; // inFlight fence of that frame
    // Stable storage for the capture-copy pass's source image (the function-pointer
    // record path needs the handle in a member, not a lambda capture).
    VkImage       m_captureColorImg = VK_NULL_HANDLE;

    // Resource registries (created via the public mesh/texture API)
    std::unordered_map<uint32_t, Mesh>    m_meshes;
    std::unordered_map<uint32_t, Texture> m_textures;
    uint32_t m_nextMeshId = 1;
    uint32_t m_nextTexId  = 1;
    Texture  m_whiteTex{};   // built-in 1x1 white default

    // ---- LOD-chain shared vertex buffers (Lane 5) --------------------------
    // Keyed by Mesh::vboShare (never 0). refs is the number of live meshes still
    // aliasing this vertex buffer; the last destroyMesh to release it defers the
    // buffer free. Meshes created the ordinary way have vboShare == 0 and never
    // touch this map, so their lifetime handling is byte-for-byte as before.
    struct VboShare { VkBuffer buf = VK_NULL_HANDLE; VmaAllocation alloc = nullptr; uint32_t refs = 0; };
    std::unordered_map<uint32_t, VboShare> m_vboShares;
    uint32_t m_nextVboShare = 1;

    // ---- VERTEX COMPRESSION (Lane 5) ---------------------------------------
    // The ACTIVE packed mesh-vertex layout (engine/rhi/VertexPack.h), resolved
    // ONCE in init() from DeviceDesc::vertexFormat and then immutable: the
    // vertex input is baked into every PSO. 0 / 32 is the legacy layout, and
    // every byte the upload path writes in that case is identical to before.
    uint32_t m_vtxFmt    = 0;
    uint32_t m_vtxStride = 32;
    // Fill the mesh vertex input for the ACTIVE format. One helper so the four
    // places that declare it (opaque, shadow, depth pre-pass, velocity) cannot
    // drift apart. Always 3 attributes at locations 0/1/2.
    void meshVertexInput(VkVertexInputBindingDescription& bind,
                         VkVertexInputAttributeDescription attrs[3]) const;
    // Pack `verts` into `staging` using the active format and return the byte
    // count. For the legacy format this is a plain memcpy.
    size_t packMeshVertices(const MeshVertex* verts, uint32_t vcount,
                            std::vector<uint8_t>& staging) const;

    // Depth (sized to swapchain)
    VkFormat      m_depthFormat = VK_FORMAT_D32_SFLOAT;
    VkImage       m_depthImg = VK_NULL_HANDLE;
    VkImageView   m_depthView = VK_NULL_HANDLE;
    VmaAllocation m_depthAlloc = nullptr;

    // ======================================================================
    // HDR pipeline + bloom (task: HDR scene target + bloom + emissive).
    // ----------------------------------------------------------------------
    // The lit scene (sky + meshes) renders into an offscreen R16G16B16A16_SFLOAT
    // target in LINEAR HDR (no tonemap). A bloom chain (bright-pass -> N-mip
    // progressive Karis downsample -> tent upsample, all full-screen-triangle
    // fragment passes in the render graph) extracts + blurs the bright HDR
    // radiance. A final composite pass reads the HDR scene + bloom mip0, combines
    // them, applies the shared ACES tonemap ONCE, and writes the LDR
    // swapchain/offscreen image (which the HUD then draws on top of). All images
    // are sized to the frame extent + recreated on resize; the descriptor sets are
    // written once at create time so the per-frame path does NO heap allocation.
    static constexpr uint32_t kBloomMips      = 5;     // 5 progressively-smaller mips
    static constexpr VkFormat kHdrFormat      = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Bloom tunables (subtle/filmic defaults; could be promoted to cvars).
    static constexpr float kBloomThreshold    = 1.10f; // bright-pass luminance threshold
    static constexpr float kBloomKnee         = 0.55f; // soft-knee width
    static constexpr float kBloomUpScale      = 0.85f; // per-mip upsample contribution
    static constexpr float kBloomIntensity    = 0.06f; // final additive bloom strength

    // Linear HDR scene target (sky + meshes render here; sampled by bloom mip0
    // bright-pass + the composite). COLOR_ATTACHMENT | SAMPLED.
    VkImage       m_hdrImg   = VK_NULL_HANDLE;
    VmaAllocation m_hdrAlloc = nullptr;
    VkImageView   m_hdrView  = VK_NULL_HANDLE;

    // ---- Scene-color COPY target (translucent GLASS, spec §3.1) ------------
    // A full-res, SINGLE-MIP copy of the opaque HDR scene, captured AFTER the main
    // (+water) pass and BEFORE the glass pass, so glass.frag can SAMPLE the scene
    // behind it (screen-space refraction M2) while WRITING the same HDR target — you
    // cannot sample + write one image in a single pass. HDR format; usage
    // TRANSFER_DST (vkCmdCopyImage target) + SAMPLED + TRANSFER_SRC (M4 frost reads
    // it as the blur source). The render-graph tracks ONE layout per image (mip0
    // only), so the frost blur uses SEPARATE images (m_glassFrostImg[], like the
    // bloom mips) rather than this image's deeper mips. Created/destroyed alongside
    // the bloom targets (tracks the swapchain extent; keeps allocationCount=0).
    VkImage       m_sceneCopyImg   = VK_NULL_HANDLE;
    VmaAllocation m_sceneCopyAlloc = nullptr;
    VkImageView   m_sceneCopyView  = VK_NULL_HANDLE;    // single-mip view (refraction sample)
    VkSampler     m_glassCopySampler = VK_NULL_HANDLE;  // LINEAR, CLAMP
    // Frost (M4): a short chain of separately-allocated, progressively-downsampled
    // blur images of the scene copy (kGlassFrostLevels). Each is single-mip (the
    // graph tracks per-image layout), like the bloom mips. The glass shader samples
    // the deepest level for the frosted look and lerps to the sharp copy by roughness.
    VkImage       m_glassFrostImg[kGlassFrostLevels]   = {};
    VmaAllocation m_glassFrostAlloc[kGlassFrostLevels] = {};
    VkImageView   m_glassFrostView[kGlassFrostLevels]  = {};
    VkExtent2D    m_glassFrostExt[kGlassFrostLevels]   = {};

    // One bloom mip (its own image so each can be both a render target AND a
    // sampled source for the next/prev pass). mip[0] is half the frame extent;
    // each subsequent mip halves again.
    struct BloomMip {
        VkImage       img   = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VkImageView   view  = VK_NULL_HANDLE;
        VkExtent2D    extent{};
    };
    BloomMip      m_bloomMips[kBloomMips];
    VkSampler     m_postSampler = VK_NULL_HANDLE;   // CLAMP linear sampler (post passes)

    // Post (HDR/bloom/composite) pipelines + layouts.
    VkDescriptorSetLayout m_postSetLayout1 = VK_NULL_HANDLE; // 1 sampler (down/up)
    VkDescriptorSetLayout m_postSetLayout2 = VK_NULL_HANDLE; // 2 samplers (composite)
    VkPipelineLayout      m_bloomLayout    = VK_NULL_HANDLE; // set0=1 sampler + push
    VkPipelineLayout      m_compositeLayout = VK_NULL_HANDLE;// set0=2 samplers + push
    VkPipeline            m_bloomDownPipe  = VK_NULL_HANDLE;
    VkPipeline            m_bloomUpPipe    = VK_NULL_HANDLE;
    VkPipeline            m_compositePipe  = VK_NULL_HANDLE;

    // Descriptor pool + pre-written sets for the post passes. One "1-sampler" set
    // per distinct sampled source (HDR scene + each bloom mip) used by the down/up
    // passes, plus one "2-sampler" composite set. Written once at create time.
    VkDescriptorPool      m_postPool        = VK_NULL_HANDLE;
    VkDescriptorSet       m_setHdr          = VK_NULL_HANDLE;             // samples HDR scene
    VkDescriptorSet       m_setMip[kBloomMips] = {};                      // samples mip i
    VkDescriptorSet       m_setComposite    = VK_NULL_HANDLE;            // HDR scene + bloom mip0

    // Stable storage for the post passes' VkRenderingInfo + attachments (must
    // outlive execute(); the graph holds pointers into these). One per bloom mip
    // for down + up, plus the composite color attach.
    VkRenderingAttachmentInfo m_bloomAttach[kBloomMips]{};       // downsample targets
    VkRenderingInfo           m_bloomRenderInfo[kBloomMips]{};
    VkRenderingAttachmentInfo m_bloomUpAttach[kBloomMips]{};     // upsample targets (LOAD)
    VkRenderingInfo           m_bloomUpRenderInfo[kBloomMips]{};
    VkRenderingAttachmentInfo m_hdrColorAttach{};   // main pass -> HDR target
    VkRenderingAttachmentInfo m_compositeAttach{};
    VkRenderingInfo           m_compositeRenderInfo{};
    // Push-constant payloads for the post passes (stable storage referenced by the
    // record lambdas; no per-frame heap alloc).
    struct BloomPush { float srcTexel[2]; float threshold, knee, intensity; int firstPass; float pad0, pad1; };
    struct CompositePush { float bloomIntensity, exposure; int32_t tonemapMode, aeEnabled;
                           float sharpen, texelW, texelH, gradeStrength;
                           // vec4-aligned tails (GLSL push layout): rgb tint + packed extra.
                           float shadowTint[4];      // rgb = shadow tint, w = saturation
                           float highlightTint[4];   // rgb = highlight tint, w = vignette
                           // Cinematic filmic post (feat/filmic-post) — mirrors the
                           // pc.filmic* vec4s in composite.frag. 112 B total, under
                           // the 128 B push-constant guarantee.
                           float filmic[4];          // x=enabled y=vignette z=grain w=seed
                           float filmicShadow[4];    // rgb = shadow tint, w = saturation
                           float filmicHighlight[4]; // rgb = highlight tint, w = grain px scale
                         };
    // Depth-fog fullscreen pass (ART_BIBLE §5). Push mirrors shaders/fog.frag.
    struct FogPush { glm::mat4 invProj; glm::vec4 colorDensity; glm::vec4 startMax; };
    // Volumetric light-scattering variant of the same pass (shaders/volumetric.frag).
    // Separate push (and separate pipeline/layout) so the FLAT fog path above is
    // never touched: FogParams::volumetric == false records the original pipeline
    // with the original push, byte-identical to the pre-volumetric build.
    // Exactly 128 bytes = the Vulkan-guaranteed minimum maxPushConstantsSize.
    struct VolPush { glm::mat4 invViewProj; glm::vec4 colorDensity; glm::vec4 startMax;
                     glm::vec4 vol0; glm::vec4 vol1; };
    static_assert(sizeof(VolPush) == 128, "VolPush must fit the guaranteed 128-byte push range");
    BloomPush m_bloomDownPush[kBloomMips]{};
    BloomPush m_bloomUpPush[kBloomMips]{};

    // ---- TAA (temporal anti-aliasing) ---------------------------------------
    // Full-res HDR resolve OUTPUT (render target; AE/bloom/composite sample it;
    // TRANSFER_SRC for the history refresh) + persistent HISTORY (TRANSFER_DST +
    // sampled by the next frame's resolve). The history's cross-frame layout is
    // tracked in m_taaHistState (shadow-map pattern). Jitter phase is a pure
    // frame counter (deterministic headless captures); the previous UNJITTERED
    // view-proj + camera pose feed reprojection + camera-cut detection.
    VkImage       m_taaOutImg   = VK_NULL_HANDLE;
    VmaAllocation m_taaOutAlloc = nullptr;
    VkImageView   m_taaOutView  = VK_NULL_HANDLE;
    VkImage       m_taaHistImg   = VK_NULL_HANDLE;
    VmaAllocation m_taaHistAlloc = nullptr;
    VkImageView   m_taaHistView  = VK_NULL_HANDLE;
    ResourceState m_taaHistState{ VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
    bool          m_taaHistoryValid     = false;  // false -> resolve passes through
    bool          m_taaActiveThisFrame  = false;  // set per frame in buildAndExecuteGraph
    uint32_t      m_taaFrameNum         = 0;      // Halton phase (frame counter)
    glm::mat4     m_taaPrevVP{ 1.0f };            // previous UNJITTERED view-proj
    glm::vec3     m_taaPrevCamPos{ 0.0f };
    float         m_taaPrevYaw = 0.0f, m_taaPrevPitch = 0.0f, m_taaPrevFov = 0.0f;
    bool          m_taaPrevCamValid = false;
    struct TaaUBO {
        glm::mat4 invViewProjCur;   // current JITTERED clip -> world
        glm::mat4 viewProjPrev;     // world -> previous UNJITTERED clip
        glm::vec4 params0;          // texelW, texelH, historyValid, blend
        glm::vec4 params1;          // jitterX, jitterY (px, debug), unused
    };
    VkDescriptorSetLayout m_taaSetLayout = VK_NULL_HANDLE;  // b0-2 samplers + b3 UBO
    VkPipelineLayout      m_taaLayout    = VK_NULL_HANDLE;
    VkPipeline            m_taaPipe      = VK_NULL_HANDLE;
    VkSampler             m_taaDepthSampler = VK_NULL_HANDLE;  // NEAREST clamp (depth as data)
    VkDescriptorSet       m_taaSet[kFramesInFlight] = {};      // per-frame resolve inputs
    VkBuffer              m_taaUboBuf[kFramesInFlight] = {};
    VmaAllocation         m_taaUboAlloc[kFramesInFlight] = {};
    void*                 m_taaUboMapped[kFramesInFlight] = {};
    VkDescriptorSet       m_setTaaOut       = VK_NULL_HANDLE;  // bloom src (TAA output)
    VkDescriptorSet       m_setCompositeTaa = VK_NULL_HANDLE;  // composite b0 = TAA output
    VkDescriptorSet       m_aeSetTaa        = VK_NULL_HANDLE;  // AE b0 = TAA output
    VkRenderingAttachmentInfo m_taaAttach{};                   // stable storage for the graph
    VkRenderingInfo           m_taaRenderInfo{};

    // ---- PER-OBJECT VELOCITY BUFFER (#4: velocity buffer + DLSS input) -------
    // An RG16F screen-space motion-vector target written by a velocity pre-pass
    // (velocity.vert/.frag) that re-rasterizes the opaque geometry right after
    // the depth pre-pass (depth EQUAL). Each pixel stores (prevUV - curUV) with
    // jitter removed, so the TAA resolve reprojects DYNAMIC + SKINNED motion
    // directly (drone, monsters) instead of relying on the neighborhood clamp.
    // It is also the required input for DLSS (PART 2 seam). Created alongside the
    // TAA targets; gated by m_post.velocity (r_velocity) AND the pipeline/target
    // existing — graceful: if velocity.*.spv is absent (not yet registered in the
    // app shader list) the pass is never built and TAA falls back to camera-only
    // reprojection, byte-identical to the pre-velocity path.
    VkImage       m_velImg   = VK_NULL_HANDLE;
    VmaAllocation m_velAlloc = nullptr;
    VkImageView   m_velView  = VK_NULL_HANDLE;
    static constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
    VkPipelineLayout m_velLayout  = VK_NULL_HANDLE;  // set0 = objSet+velUBO (shadow-style + b4)
    VkPipeline       m_velPipe    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_velSetLayout = VK_NULL_HANDLE;  // objSet b0..b3 + velUBO b4
    VkDescriptorPool m_velPool = VK_NULL_HANDLE;           // dedicated (own the velocity sets)
    VkDescriptorSet  m_velSet[kFramesInFlight] = {};        // per-frame obj/prev/vis/cam/velUBO
    // Velocity UBO: unjittered cur/prev viewProj + the two frames' jitter (NDC).
    struct VelUBO {
        glm::mat4 viewProjCurUnjit;
        glm::mat4 viewProjPrevUnjit;
        glm::vec4 jitter;   // xy = cur jitter (NDC), zw = prev jitter (NDC)
    };
    VkBuffer      m_velUboBuf[kFramesInFlight]    = {};
    VmaAllocation m_velUboAlloc[kFramesInFlight]  = {};
    void*         m_velUboMapped[kFramesInFlight] = {};
    // Previous-frame per-object model matrices (one mat4 per object SSBO row),
    // double-buffered so the velocity vertex shader reads last frame's transforms
    // while this frame writes the current ones. Indexed identically to objBuf.
    VkBuffer      m_prevModelBuf[kFramesInFlight]   = {};
    VmaAllocation m_prevModelAlloc[kFramesInFlight] = {};
    void*         m_prevModelMapped[kFramesInFlight]= {};
    bool          m_velActiveThisFrame = false;   // set per frame in buildAndExecuteGraph
    glm::vec2     m_velPrevJitterNdc{ 0.0f };      // previous frame's jitter (NDC) for the UBO
    // CPU-side per-row model history for the velocity prev-model SSBO. Row order
    // is the grouped emit order (stable for a static scene -> a row maps to the
    // same object across frames); on a topology change a few rows get a one-frame
    // stale MV, which the TAA neighborhood clamp contains. Filled in prepareFrameData.
    std::vector<glm::mat4> m_velPrevModels;        // models from the previous frame
    std::vector<glm::mat4> m_velCurModels;         // scratch: this frame's models
    ResourceState m_velState{ VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
    VkRenderingAttachmentInfo m_velAttach{};       // stable storage for the graph
    VkRenderingAttachmentInfo m_velDepthAttach{};  // read-only depth (EQUAL test)
    VkRenderingInfo           m_velRenderInfo{};

    // ---- AUTO-EXPOSURE (eye adaptation) ------------------------------------
    // A single-workgroup compute pass (autoexposure.comp) reduces a fixed 64x64
    // log-luminance sample grid over the HDR scene to an average, maps it to a
    // target exposure (key/avg, clamped), and temporally adapts a persistent
    // 16-byte SSBO value the composite multiplies in (exposure = adapted *
    // r_exposure bias). Buffers are not graph-tracked (documented model), so the
    // record body emits its own tiny SSBO barriers. Determinism: the adaptation
    // SNAPS to the target on the first frame, whenever AE is toggled on, and on
    // EVERY headless frame (reproducible --screenshot*/--test captures).
    VkDescriptorSetLayout m_aeSetLayout = VK_NULL_HANDLE;  // b0 = HDR sampler, b1 = SSBO (compute)
    VkDescriptorSet       m_aeSet       = VK_NULL_HANDLE;
    VkPipelineLayout      m_aeLayout    = VK_NULL_HANDLE;
    VkPipeline            m_aePipe      = VK_NULL_HANDLE;
    VkBuffer              m_aeBuf       = VK_NULL_HANDLE;  // { adapted, avgLog, pad, pad }
    VmaAllocation         m_aeAlloc     = nullptr;
    bool                  m_aeSnap      = true;            // snap-to-target on next AE frame
    double                m_aePrevTime  = -1.0;            // steady_clock seconds (dt source)
    struct AePush { float dt, speed, minExp, maxExp, key; int32_t snap; float pad0, pad1; };
    AePush                m_aePush{};
    PostFXParams          m_post{};                        // live r_* post-stack settings
    // Stable per-pass record context for the bloom down/up passes. The graph's
    // record path is a raw function pointer + void* ctx (no std::function, no
    // per-frame heap alloc); each pass's per-mip parameters (which descriptor set
    // to bind, the dst extent, the mip index) live here, NOT in a lambda capture.
    struct BloomPassCtx {
        VulkanRenderDevice* self = nullptr;
        VkDescriptorSet     srcSet = VK_NULL_HANDLE;
        VkExtent2D          dstExt{};
        uint32_t            mip = 0;
    };
    BloomPassCtx m_bloomDownCtx[kBloomMips]{};
    BloomPassCtx m_bloomUpCtx[kBloomMips]{};
    // Glass frost-blur chain (M4): stable per-level storage (one per frost level).
    VkRenderingAttachmentInfo m_glassFrostAttach[kGlassFrostLevels]{};
    VkRenderingInfo           m_glassFrostRenderInfo[kGlassFrostLevels]{};
    BloomPush                 m_glassFrostPush[kGlassFrostLevels]{};
    BloomPassCtx              m_glassFrostCtx[kGlassFrostLevels]{};
    RgResource                m_glassFrostRg[kGlassFrostLevels]{};   // per-frame graph handles

    // ======================================================================
    // SSAO — screen-space ambient occlusion (idTech-8 grounding/contact AO).
    // ----------------------------------------------------------------------
    // A half-res hemisphere-kernel pass reconstructs each pixel's VIEW-SPACE
    // position + normal FROM THE DEPTH BUFFER (no G-buffer), accumulates occlusion
    // against a noise-rotated 32-sample kernel, and writes a single-channel R8 AO
    // image; a depth-aware bilateral blur removes the 4x4 noise tiling. The result
    // modulates ONLY mesh.frag's ambient term. Depth comes from a NEW depth
    // pre-pass (depth.vert) so the full depth buffer exists before lighting; the
    // main pass then runs depth-test EQUAL with depth-write off. Half-res + a
    // 4x4-tap blur keep the cost well under ~1 ms at 1080p on a 1080 Ti.
    static constexpr int      kSsaoKernel = 32;          // hemisphere samples
    static constexpr VkFormat kSsaoFormat = VK_FORMAT_R8_UNORM;  // single-channel AO
    // SSAO UBO (std140; matches the Ssao block in ssao.frag): proj + invProj, two
    // param vec4s, then the baked kernel[32] + noise[16] tables.
    struct SsaoUBO {
        glm::mat4 proj;
        glm::mat4 invProj;
        glm::vec4 params0;             // x=radius, y=bias, z=intensity, w=power
        glm::vec4 params1;             // x=screenW, y=screenH, z=noiseScale.x, w=noiseScale.y
        glm::vec4 kernel[kSsaoKernel];
        glm::vec4 noise[16];
    };
    static_assert(sizeof(SsaoUBO) == 128 + 32 + (kSsaoKernel + 16) * 16,
                  "SsaoUBO must match the std140 layout in ssao.frag");
    // Tiny control block fed to mesh.frag (set3/binding1): ctrl = {x=enabled,
    // y=strength, z=1/screenW, w=1/screenH}; ibl = {x=IBL valid(0/1), y=IBL
    // intensity, z=prefilter max mip, w=reserved}. The ibl lane lets mesh.frag fall
    // back to the old flat ambient when no environment is baked (other paths/headless
    // failure) without a separate UBO.
    // mesh.frag set3/binding1 control block. The ddgi* lanes (r_ddgi) carry the
    // probe-grid gate + geometry so the fragment stage can interpolate the DDGI
    // atlases; all zero when inactive (the ambient math is then byte-identical).
    struct SsaoControl {
        glm::vec4 ctrl; glm::vec4 ibl; glm::vec4 refl;
        glm::vec4 ddgiCtrl;     // x = active, y = intensity (ramped), z = debug, w = bias scale
        glm::vec4 ddgiOrigin;   // xyz = grid min corner, w = visMaxDist
        glm::vec4 ddgiSpacing;  // xyz = probe spacing
        glm::vec4 ddgiCounts;   // xyz = probe counts (float)
        // RT soft shadows (r_rtshadows) — read ONLY by the mesh_rt.frag variant;
        // all zero when inactive (the plain variant never references them).
        glm::vec4 rtsh0;        // x = tier, y = tan(sun angular radius), z = point ray budget K, w = light source radius (m)
        glm::vec4 rtsh1;        // x = frame seed (0 when TAA is off -> static dither)
        // Underwater caustics (setCaustics): all zero when no host opted in ->
        // the mesh.frag gate never opens (dry worlds byte-identical).
        glm::vec4 caustics;     // x = enabled, y = water surface Y, z = time (s), w = intensity
        // TERRAIN NORMAL MAPS (registerTerrainMaterial). Packed like the albedo
        // pack: x = grass<<16|rock, y = snow<<16|sand. All zero -> mesh.frag's
        // terrainNormal() returns the geometry normal (pre-relief behaviour).
        glm::uvec4 terrainNrm{ 0u, 0u, 0u, 0u };
        // Surface wetness (setWetness): all zero when no host opted in -> the
        // mesh.frag gate never opens (dry worlds byte-identical).
        glm::vec4 wetness;      // x = amount, y = porosity, z = puddles, w = min roughness
        glm::vec4 precip;       // x = lying-snow cover 0..1, yzw reserved
        // CLOUD SHADOWS (task #27; mesh.frag `cloudShad` — keep in sync): the
        // ground shade of the sky.frag cloud deck. x = strength (0 = gate
        // shut -> dry/indoor worlds byte-identical), y = cover
        // (SkyParams::cloud), z = sky time (m_skyTime, the shared drift
        // clock), w = reserved. Filled from the CACHED sky params — no new
        // host API: a host that pushes clouds into the sky gets their shade
        // on the ground for free (NO_SLOP rule 6: defaults ON).
        glm::vec4 cloudShadow;
    };
    // Half-res AO targets: raw (ssao.frag output) + blurred (ssao_blur output,
    // sampled by mesh.frag). Both R8, recreated with the frame extent.
    VkImage       m_ssaoRawImg  = VK_NULL_HANDLE; VmaAllocation m_ssaoRawAlloc  = nullptr; VkImageView m_ssaoRawView  = VK_NULL_HANDLE;
    VkImage       m_ssaoBlurImg = VK_NULL_HANDLE; VmaAllocation m_ssaoBlurAlloc = nullptr; VkImageView m_ssaoBlurView = VK_NULL_HANDLE;
    VkExtent2D    m_ssaoExtent{};                       // half the frame extent
    // Depth pre-pass pipeline (depth.vert; set0 = objSet, reuses m_shadowLayout).
    VkPipeline    m_depthPrePipeline = VK_NULL_HANDLE;
    // Alpha-cutout depth pre-pass variant (depth_cutout.vert/.frag): fragment-
    // stage alpha test so billboard depth matches mesh.frag's cutout discard.
    // Own layout (set0 = objSet, set1 = bindless). Reflections frames only.
    VkPipeline       m_depthPreCutoutPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_depthPreCutoutLayout   = VK_NULL_HANDLE;
    // SSAO + blur pipelines (full-screen-triangle fragment passes).
    VkSampler             m_depthSampler  = VK_NULL_HANDLE;  // NEAREST, sample depth as data
    VkDescriptorSetLayout m_ssaoSetLayout = VK_NULL_HANDLE;  // depth + SsaoUBO (ssao.frag)
    VkDescriptorSetLayout m_ssaoBlurSetLayout = VK_NULL_HANDLE; // aoRaw + depth (blur)
    VkPipelineLayout      m_ssaoLayout    = VK_NULL_HANDLE;
    VkPipelineLayout      m_ssaoBlurLayout = VK_NULL_HANDLE;
    VkPipeline            m_ssaoPipe      = VK_NULL_HANDLE;
    VkPipeline            m_ssaoBlurPipe  = VK_NULL_HANDLE;
    VkDescriptorPool      m_ssaoPool      = VK_NULL_HANDLE;
    // Per-frame SSAO UBO + sets (the SsaoUBO is filled in prepareFrameData; the
    // depth-sampling sets are rewritten on resize when the depth/AO views change).
    VkBuffer      m_ssaoUboBuf[kFramesInFlight] = {}; VmaAllocation m_ssaoUboAlloc[kFramesInFlight] = {}; void* m_ssaoUboMapped[kFramesInFlight] = {};
    VkDescriptorSet m_ssaoSet[kFramesInFlight]   = {};  // ssao.frag: depth + UBO
    VkDescriptorSet m_ssaoBlurSet                = VK_NULL_HANDLE; // blur: aoRaw + depth
    // mesh.frag set 3: AO texture + SsaoControl UBO (per-frame control buffer).
    // (The mesh-AO descriptor sets are allocated from m_ssaoPool — see createSsao();
    //  there is no separate pool, so no m_meshAoPool member exists.)
    VkDescriptorSetLayout m_meshAoSetLayout = VK_NULL_HANDLE;
    VkBuffer      m_ssaoCtrlBuf[kFramesInFlight] = {}; VmaAllocation m_ssaoCtrlAlloc[kFramesInFlight] = {}; void* m_ssaoCtrlMapped[kFramesInFlight] = {};
    VkDescriptorSet m_meshAoSet[kFramesInFlight] = {};  // mesh.frag set3: AO + ctrl
    VkSampler     m_ssaoLinearSampler = VK_NULL_HANDLE; // CLAMP linear (sample AO)
    // Stable storage for the SSAO/blur/depth-prepass VkRenderingInfo + attachments.
    VkRenderingAttachmentInfo m_depthPreAttach{};
    VkRenderingInfo           m_depthPreRenderInfo{};
    VkRenderingAttachmentInfo m_ssaoAttach{};
    VkRenderingInfo           m_ssaoRenderInfo{};
    VkRenderingAttachmentInfo m_ssaoBlurAttach{};
    VkRenderingInfo           m_ssaoBlurRenderInfo{};
    struct SsaoBlurPush { float aoTexel[2]; float depthSigma, pad0; };
    SsaoBlurPush m_ssaoBlurPush{};
    SsaoParams   m_ssao{};   // cached tunables (setSsaoParams)
    // Baked (deterministic) hemisphere kernel + 4x4 rotation noise (CPU-side),
    // copied into each frame's SSAO UBO by prepareFrameData.
    bool      m_ssaoKernelBuilt = false;
    glm::vec4 m_ssaoKernelCPU[kSsaoKernel]{};
    glm::vec4 m_ssaoNoiseCPU[16]{};

    // ======================================================================
    // GI — real-time dynamic global illumination (screen-space indirect diffuse).
    // CLEAN-ROOM, original. Built from public, non-engine references only: the
    // Vulkan 1.3 spec; Real-Time Rendering 4th ed.; the screen-space directional
    // occlusion / SSGI formulation (Ritschel/Grosch/Seidel, I3D 2009) where screen
    // occluders carry surface radiance for indirect colour bleeding; temporal-
    // reprojection / EMA accumulation + a-trous/bilateral denoise write-ups. No
    // game-engine source consulted.
    // ----------------------------------------------------------------------
    // Chain (all added to the render graph after the main color pass, before the
    // bloom chain): GATHER (half-res, ssgi_gather.frag — reconstruct view pos/normal
    // from the SSAO depth pre-pass, march a cosine hemisphere, sample the lit HDR
    // scene as incoming radiance) -> TEMPORAL (ssgi_temporal.frag — camera-reproject
    // last frame's GI + EMA blend, reject disocclusion) -> DENOISE (ssgi_blur.frag —
    // depth-aware bilateral) -> APPLY (full-res, ssgi_apply.frag — depth-aware
    // up-sample + ADDITIVE into the linear HDR target, modulated by the SSAO AO).
    // Half-res + a few taps + temporal target well under ~2 ms at 720p on a 1080 Ti.
    static constexpr int      kGiKernel = 24;                       // hemisphere taps
    static constexpr VkFormat kGiFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // HDR indirect radiance
    // GI gather UBO (std140; matches the Gi block in ssgi_gather.frag).
    struct GiUBO {
        glm::mat4 proj;
        glm::mat4 invProj;
        glm::vec4 params0;             // x=radius, y=intensity, z=maxRadiance, w=falloffPower
        glm::vec4 params1;             // x=screenW, y=screenH, z=numSamples, w=bias
        glm::vec4 kernel[kGiKernel];
        glm::vec4 noise[16];
    };
    static_assert(sizeof(GiUBO) == 128 + 32 + (kGiKernel + 16) * 16,
                  "GiUBO must match the std140 layout in ssgi_gather.frag");
    // GI temporal UBO (matches GiTemporal in ssgi_temporal.frag).
    struct GiTemporalUBO {
        glm::mat4 invViewProjCur;
        glm::mat4 viewProjPrev;
        glm::vec4 params0;             // x=alpha, y=depthRejectScale, z=valid, w=unused
    };
    // Push constants for the denoise blur + apply passes.
    struct GiBlurPush  { float giTexel[2]; float depthSigma, stepScale; };
    struct GiApplyPush { float giTexel[2]; float strength, aoAmount; };
    GiBlurPush  m_giBlurPush{};
    GiApplyPush m_giApplyPush{};
    // Half-res GI targets: raw gather, two ping-pong accumulation buffers (one is
    // the live history across frames), and a denoised buffer. All RGBA16F.
    VkImage m_giRawImg = VK_NULL_HANDLE; VmaAllocation m_giRawAlloc = nullptr; VkImageView m_giRawView = VK_NULL_HANDLE;
    VkImage m_giAccumImg[2] = {}; VmaAllocation m_giAccumAlloc[2] = {}; VkImageView m_giAccumView[2] = {};
    VkImage m_giDenoiseImg = VK_NULL_HANDLE; VmaAllocation m_giDenoiseAlloc = nullptr; VkImageView m_giDenoiseView = VK_NULL_HANDLE;
    // Full-res previous-frame depth copy (for temporal disocclusion reject). Kept
    // across frames; the main depth is overwritten each frame so we snapshot it.
    VkImage m_giPrevDepthImg = VK_NULL_HANDLE; VmaAllocation m_giPrevDepthAlloc = nullptr; VkImageView m_giPrevDepthView = VK_NULL_HANDLE;
    VkExtent2D m_giExtent{};                  // half the frame extent
    uint32_t   m_giAccumWrite = 0;            // which accum buffer this frame writes (ping-pong)
    bool       m_giHistoryValid = false;      // false on first frame / after resize
    glm::mat4  m_giPrevViewProj{1.0f};        // last frame's camera viewProj (for reproject)
    // Pipelines + layouts.
    VkDescriptorSetLayout m_giGatherSetLayout   = VK_NULL_HANDLE; // depth + scene + GiUBO
    VkDescriptorSetLayout m_giTemporalSetLayout = VK_NULL_HANDLE; // cur + hist + depth + prevDepth + UBO
    VkDescriptorSetLayout m_giBlurSetLayout     = VK_NULL_HANDLE; // gi + depth
    VkDescriptorSetLayout m_giApplySetLayout    = VK_NULL_HANDLE; // gi + depth + ao
    VkPipelineLayout m_giGatherLayout   = VK_NULL_HANDLE;
    VkPipelineLayout m_giTemporalLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_giBlurLayout     = VK_NULL_HANDLE;
    VkPipelineLayout m_giApplyLayout    = VK_NULL_HANDLE;
    VkPipeline m_giGatherPipe   = VK_NULL_HANDLE;
    VkPipeline m_giTemporalPipe = VK_NULL_HANDLE;
    VkPipeline m_giBlurPipe     = VK_NULL_HANDLE;
    VkPipeline m_giApplyPipe    = VK_NULL_HANDLE;
    VkDescriptorPool m_giPool = VK_NULL_HANDLE;
    // Per-frame UBOs + their gather/temporal descriptor sets. The temporal +
    // blur/apply sets reference ping-pong views, so they are rewritten each frame
    // (cheap vkUpdateDescriptorSets, no allocation) to point at the right buffers.
    VkBuffer m_giUboBuf[kFramesInFlight] = {}; VmaAllocation m_giUboAlloc[kFramesInFlight] = {}; void* m_giUboMapped[kFramesInFlight] = {};
    VkBuffer m_giTempUboBuf[kFramesInFlight] = {}; VmaAllocation m_giTempUboAlloc[kFramesInFlight] = {}; void* m_giTempUboMapped[kFramesInFlight] = {};
    VkDescriptorSet m_giGatherSet[kFramesInFlight] = {};
    VkDescriptorSet m_giTemporalSet[kFramesInFlight] = {};
    VkDescriptorSet m_giBlurSet[kFramesInFlight]  = {};   // per-frame: rewritten each frame (ping-pong)
    VkDescriptorSet m_giApplySet[kFramesInFlight] = {};   // per-frame: rewritten each frame
    // Stable storage for the GI VkRenderingInfo + attachments (one per pass).
    VkRenderingAttachmentInfo m_giGatherAttach{};   VkRenderingInfo m_giGatherRenderInfo{};
    VkRenderingAttachmentInfo m_giTemporalAttach{}; VkRenderingInfo m_giTemporalRenderInfo{};
    VkRenderingAttachmentInfo m_giBlurAttach{};     VkRenderingInfo m_giBlurRenderInfo{};
    VkRenderingAttachmentInfo m_giApplyAttach{};    VkRenderingInfo m_giApplyRenderInfo{};
    GiParams  m_gi{};                  // cached tunables (setGiParams)
    bool      m_giKernelBuilt = false;
    glm::vec4 m_giKernelCPU[kGiKernel]{};
    glm::vec4 m_giNoiseCPU[16]{};

    // ======================================================================
    // WATER — animated ocean surface (undersea-world foundation).
    // CLEAN-ROOM, original work. Wave model = the standard public Gerstner /
    // sum-of-sines formulation (Tessendorf "Simulating Ocean Water", the GPU Gems
    // Gerstner-wave chapter, public ocean-rendering articles); shading from public
    // water-rendering references. No GPL / id Tech / RBDOOM source consulted.
    // ----------------------------------------------------------------------
    // A large tessellated grid plane centered + snapped under the camera at
    // `seaLevel`, displaced by a sum of Gerstner waves (water.vert) with analytic
    // normals, drawn in the MAIN pass AFTER opaque meshes into the linear HDR
    // target. water.frag Fresnel-blends an analytic-sky reflection with a
    // depth-based refraction color (reads the scene depth buffer — the SAME depth
    // the SSAO pre-pass / main pass produced) and adds a sharp sun glint (HDR ->
    // bloom). Gated by setWaterParams(enabled): default OFF, zero cost when off.
    // The grid is a unit-patch mesh uploaded once; its own per-frame UBO + set.
    static constexpr uint32_t kWaterGridDim = 192;   // verts per edge (191^2 quads)
    static constexpr float    kWaterPatchHalf = 240.0f; // world half-extent (m)
    struct WaterUBO {
        glm::mat4 viewProj;       // 0
        glm::vec4 camPos;         // 64
        glm::vec4 sunDir;         // 80
        glm::vec4 deepColor;      // 96
        glm::vec4 shallowColor;   // 112
        glm::vec4 p0;             // 128: x=seaLevel,y=time,z=amplitude,w=steepness
        glm::vec4 p1;             // 144: x=baseWavelength,y=speed,z=specular,w=fresnelBase
        glm::vec4 p2;             // 160: x=patchHalfExtent,y=1/W,z=1/H,w=camera far (0 => legacy 200)
        glm::vec4 p3;             // 176: xyz=horizonColor (linear), w=1 when supplied else 0
        glm::vec4 p4;             // 192: x=clarity (0 = legacy opaque), yzw reserved
        // RIVER MODE (task #32): x=riverNodeCount (0 = legacy flat sea),
        // y=riverHalfWidth (m); zw reserved.
        glm::vec4 riverInfo;      // 208
        // xy=ocean basin centre (world XZ), z=basin radius (0 = no sea
        // fallback), w=oceanLevel (the sea surface Y the estuary hands off to).
        glm::vec4 riverBasin;     // 224
        // Per node: x=world x, y=world z, z=waterY (w unused). Count above.
        glm::vec4 riverNodes[20]; // 240
    };
    static_assert(sizeof(WaterUBO) == 560, "WaterUBO must match the std140 layout in water.{vert,frag}");
    WaterParams m_water{};   // cached tunables (setWaterParams)
    // Unit-patch grid mesh (vec2 grid coord per vertex), built once at init.
    VkBuffer      m_waterVbo = VK_NULL_HANDLE; VmaAllocation m_waterVboAlloc = nullptr;
    VkBuffer      m_waterIbo = VK_NULL_HANDLE; VmaAllocation m_waterIboAlloc = nullptr;
    uint32_t      m_waterIndexCount = 0;
    VkDescriptorSetLayout m_waterSetLayout = VK_NULL_HANDLE; // UBO + scene depth
    VkPipelineLayout      m_waterLayout    = VK_NULL_HANDLE;
    VkPipeline            m_waterPipeline  = VK_NULL_HANDLE;
    VkDescriptorPool      m_waterPool      = VK_NULL_HANDLE;
    // Per-frame UBO + descriptor set (set0: WaterUBO + scene-depth sampler). The
    // depth-binding write is refreshed each frame (depth view persists, but rewire
    // on resize); the UBO is mapped + filled in prepareFrameData.
    VkBuffer      m_waterUboBuf[kFramesInFlight] = {}; VmaAllocation m_waterUboAlloc[kFramesInFlight] = {}; void* m_waterUboMapped[kFramesInFlight] = {};
    VkDescriptorSet m_waterSet[kFramesInFlight] = {};
    VkSampler     m_waterDepthSampler = VK_NULL_HANDLE; // LINEAR clamp, samples scene depth
    // Stable storage for the water pass's VkRenderingInfo + attachments.
    VkRenderingAttachmentInfo m_waterColorAttach{};
    VkRenderingAttachmentInfo m_waterDepthAttach{};
    VkRenderingInfo           m_waterRenderInfo{};

    // Stable storage for the GLASS (transparent) pass's VkRenderingInfo +
    // attachments (post-opaque; color = HDR LOAD, depth = read-only LEQUAL).
    VkRenderingAttachmentInfo m_glassColorAttach{};
    VkRenderingAttachmentInfo m_glassDepthAttach{};
    VkRenderingInfo           m_glassRenderInfo{};

    // ---- GPU-instanced billboard particles + impact decals (combat juice) ---
    // A bounded, per-frame stream of camera-facing billboards drawn into the linear
    // HDR target AFTER opaque + water + GI, BEFORE bloom (so bright additive sparks
    // feed the bloom chain), depth-TESTED against the scene (no depth-write), with
    // a SOFT-PARTICLE fade vs. the scene depth (the SSAO/water depth buffer). Impact
    // decals (oriented quads on the hit surface) are drawn in the SAME pass, alpha-
    // blended, depth-tested, no write. CPU owns the sim (app/fx.*) + submits each
    // frame; the device streams the instances into a per-frame ring + draws ONE
    // instanced quad per sub-batch. The whole pass is added only when something was
    // submitted this frame (zero GPU cost when idle).
    static constexpr uint32_t kMaxParticles = 16384; // per-frame instance cap (each mode)
    static constexpr uint32_t kMaxDecals    = 256;    // per-frame decal instance cap
    // ParticleUBO (set0,b0): camera viewProj + screen-aligned billboard basis +
    // depth-reconstruction params. std140; matches particle.{vert,frag}.
    struct ParticleUBO {
        glm::mat4 viewProj;   // 0
        glm::vec4 camRight;   // 64
        glm::vec4 camUp;      // 80
        glm::vec4 camPos;     // 96
        glm::vec4 params;     // 112: x=1/W, y=1/H, z=near, w=far
    };
    static_assert(sizeof(ParticleUBO) == 128, "ParticleUBO must match the std140 layout");
    // DecalUBO (set0,b0): camera viewProj. std140; matches decal.{vert,frag}.
    struct DecalUBO {
        glm::mat4 viewProj;   // 0
        glm::vec4 params;     // 64: reserved
    };
    static_assert(sizeof(DecalUBO) == 80, "DecalUBO must match the std140 layout");
    // Per-instance GPU layout for the billboard pipeline: pos.xyz+halfSize, rgba.
    struct ParticleGpu { glm::vec4 posSize; glm::vec4 color; };
    // Per-instance GPU layout for the decal pipeline: center+halfSize, normal+angle, rgba.
    struct DecalGpu { glm::vec4 centerSize; glm::vec4 normalAngle; glm::vec4 color; };

    // CPU staging (fixed capacity; cleared each beginFrame -> no per-frame heap churn).
    std::vector<ParticleInstance> m_partAdd;    // additive batch (sparks/fire/muzzle)
    std::vector<ParticleInstance> m_partAlpha;  // alpha batch (smoke/dust/blood)
    std::vector<DecalInstance>    m_decals;     // impact decals this frame
    uint32_t m_partAddCount = 0, m_partAlphaCount = 0, m_decalCount = 0; // uploaded counts

    // Shared unit quad (4 corners in [-0.5,0.5], triangle strip) for both pipelines.
    VkBuffer      m_partQuadVbo = VK_NULL_HANDLE; VmaAllocation m_partQuadAlloc = nullptr;
    // Per-frame instance rings (persistent-mapped): additive + alpha particles, decals.
    VkBuffer m_partInstAddBuf[kFramesInFlight] = {};   VmaAllocation m_partInstAddAlloc[kFramesInFlight] = {};   void* m_partInstAddMapped[kFramesInFlight] = {};
    VkBuffer m_partInstAlphaBuf[kFramesInFlight] = {}; VmaAllocation m_partInstAlphaAlloc[kFramesInFlight] = {}; void* m_partInstAlphaMapped[kFramesInFlight] = {};
    VkBuffer m_decalInstBuf[kFramesInFlight] = {};     VmaAllocation m_decalInstAlloc[kFramesInFlight] = {};     void* m_decalInstMapped[kFramesInFlight] = {};
    // Per-frame UBOs (particle + decal) + descriptor sets.
    VkBuffer m_partUboBuf[kFramesInFlight] = {};  VmaAllocation m_partUboAlloc[kFramesInFlight] = {};  void* m_partUboMapped[kFramesInFlight] = {};
    VkBuffer m_decalUboBuf[kFramesInFlight] = {}; VmaAllocation m_decalUboAlloc[kFramesInFlight] = {}; void* m_decalUboMapped[kFramesInFlight] = {};
    VkDescriptorSet m_partSet[kFramesInFlight] = {};   // UBO + scene-depth sampler
    VkDescriptorSet m_decalSet[kFramesInFlight] = {};  // UBO only
    VkSampler             m_partDepthSampler = VK_NULL_HANDLE;  // LINEAR clamp, scene depth
    VkDescriptorSetLayout m_partSetLayout    = VK_NULL_HANDLE;  // b0 UBO + b1 depth
    VkDescriptorSetLayout m_decalSetLayout   = VK_NULL_HANDLE;  // b0 UBO
    VkDescriptorPool      m_partPool         = VK_NULL_HANDLE;
    VkPipelineLayout      m_partLayout       = VK_NULL_HANDLE;
    VkPipelineLayout      m_decalLayout      = VK_NULL_HANDLE;
    VkPipeline            m_partAddPipeline  = VK_NULL_HANDLE;  // additive blend
    VkPipeline            m_partAlphaPipeline= VK_NULL_HANDLE;  // alpha blend
    VkPipeline            m_decalPipeline    = VK_NULL_HANDLE;  // alpha blend
    // Stable storage for the particle pass's VkRenderingInfo + attachments.
    VkRenderingAttachmentInfo m_partColorAttach{};
    VkRenderingAttachmentInfo m_partDepthAttach{};
    VkRenderingInfo           m_partRenderInfo{};

    // ---- GPU-compute persistent debris world (Subsystem K, tier T2) -------
    // A host-visible DEVICE_LOCAL pool SSBO of GpuDebrisFragment rows integrated by
    // a compute shader each step + drawn via ONE instanced unit-cube draw. The pool
    // + counters persist across frames (debris is persistent); the params/draw UBOs
    // are per-frame-in-flight. Spawn writes free slots on the CPU (mapped); readback
    // summarizes the mapped pool. Pending flags gate this frame's compute/draw pass.
    IRenderDevice::GpuDebrisParams m_debrisParams{};
    VkBuffer      m_debrisPoolBuf   = VK_NULL_HANDLE; VmaAllocation m_debrisPoolAlloc  = nullptr; void* m_debrisPoolMapped  = nullptr;
    VkBuffer      m_debrisCountBuf  = VK_NULL_HANDLE; VmaAllocation m_debrisCountAlloc = nullptr; void* m_debrisCountMapped = nullptr;
    VkBuffer      m_debrisParamsBuf[kFramesInFlight]  = {}; VmaAllocation m_debrisParamsAlloc[kFramesInFlight]  = {}; void* m_debrisParamsMapped[kFramesInFlight]  = {};
    VkBuffer      m_debrisDrawUboBuf[kFramesInFlight] = {}; VmaAllocation m_debrisDrawUboAlloc[kFramesInFlight] = {}; void* m_debrisDrawUboMapped[kFramesInFlight] = {};
    // Shard mesh set SSBO (fix/gib-meshes): kGpuDebrisShardCount distinct low-poly
    // irregular shard meshes, each padded to kDebrisShardVertsMax vertices (two vec4
    // rows per vertex: position, flat normal). The draw fetches vertices from this
    // buffer per instance (no vertex-input bindings) so ONE instanced draw renders
    // several distinct gib meshes.
    VkBuffer      m_debrisShardBuf = VK_NULL_HANDLE; VmaAllocation m_debrisShardAlloc = nullptr;
    VkDescriptorPool      m_debrisPool             = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_debrisComputeSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_debrisDrawSetLayout    = VK_NULL_HANDLE;
    VkDescriptorSet       m_debrisComputeSet[kFramesInFlight] = {};  // per-frame (params UBO is per-frame)
    VkDescriptorSet       m_debrisDrawSet[kFramesInFlight] = {};
    VkPipelineLayout      m_debrisComputeLayout    = VK_NULL_HANDLE;
    VkPipelineLayout      m_debrisDrawLayout       = VK_NULL_HANDLE;
    VkPipeline            m_debrisComputePipeline  = VK_NULL_HANDLE;
    VkPipeline            m_debrisDrawPipeline     = VK_NULL_HANDLE;
    uint32_t              m_debrisSpawnCursor      = 0;   // recycle ring cursor
    uint32_t              m_debrisAlive            = 0;   // CPU mirror (counters[0] is authoritative)
    bool                  m_debrisStepPending      = false; // dispatch the compute pass this frame
    bool                  m_debrisDrawPending      = false; // record the instanced draw this frame
    glm::mat4             m_lastViewProj{ 1.0f };           // cached viewProj for the debris draw UBO
    VkRenderingAttachmentInfo m_debrisColorAttach{};
    VkRenderingAttachmentInfo m_debrisDepthAttach{};
    VkRenderingInfo           m_debrisRenderInfo{};

    // ---- GPU compute skinning of models (GPU SKINNING OF MODELS) ----------
    // A compute pre-pass that linear-blend-skins registered meshes into their
    // per-frame dynamic vbo BEFORE the depth/shadow/color passes (which then draw
    // that buffer with their unchanged vertex shaders). Each registered skinned mesh
    // owns: an IMMUTABLE bind-pose+attrs SSBO + an IMMUTABLE influences SSBO
    // (uploaded once), a per-frame-in-flight palette SSBO (host-visible; the CPU
    // Skinner uploads the joint-matrix palette into it each frame), and a per-frame
    // compute descriptor set. The skinned OUTPUT is the Mesh's dynVbo[frameIdx]
    // (promoted to dynamic + given STORAGE usage so the compute can write it AND the
    // draw can read it as a vertex buffer). One vkCmdDispatch per pending instance.
    // (The SkinSrcVertex / SkinSrcInfluence / SkinnedMesh / SkinPush TYPES are
    // declared up near the Mesh/Texture structs so the method signatures that take
    // them are visible — see the private struct block after Texture.)
    std::unordered_map<uint32_t, SkinnedMesh> m_skinnedMeshes;       // keyed by mesh id
    std::vector<uint32_t>  m_skinPending;                            // mesh ids to dispatch this frame
    VkDescriptorPool       m_skinPool          = VK_NULL_HANDLE;
    VkDescriptorSetLayout  m_skinSetLayout     = VK_NULL_HANDLE;
    VkPipelineLayout       m_skinPipelineLayout= VK_NULL_HANDLE;
    VkPipeline             m_skinPipeline      = VK_NULL_HANDLE;
    bool                   m_skinStepPending   = false;              // any skinning to dispatch this frame

    // ---- Directional shadow mapping (perf-stack E) ------------------------
    // A single fixed-resolution depth texture rendered from the sun's POV each
    // frame (depth-only pipeline, same SSBO + indirect draws as the color pass),
    // then sampled by mesh.frag via a compare sampler (sampler2DShadow). One map
    // (no CSM); the ortho box follows the camera. Created once (resolution is
    // swapchain-independent); the per-frame work is the depth pass + a barrier.
    VkFormat              m_shadowFormat   = VK_FORMAT_D32_SFLOAT;
    VkImage               m_shadowImg      = VK_NULL_HANDLE;
    VmaAllocation         m_shadowAlloc    = nullptr;
    VkImageView           m_shadowView     = VK_NULL_HANDLE;
    VkSampler             m_shadowSampler  = VK_NULL_HANDLE;   // compare-enabled
    VkPipeline            m_shadowPipeline = VK_NULL_HANDLE;   // depth-only
    VkPipelineLayout      m_shadowLayout   = VK_NULL_HANDLE;   // set0 = objSet
    // ALPHA-CUTOUT shadow variant (shadow_cutout.vert + depth_cutout.frag): a
    // fir billboard casts a FIR-shaped shadow instead of its full quad. Opt-in
    // per host via setShadowCutout(); off = the historical shadow, bit-for-bit.
    VkPipeline            m_shadowCutoutPipeline = VK_NULL_HANDLE;
    VkPipelineLayout      m_shadowCutoutLayout   = VK_NULL_HANDLE;  // set0 = objSet, set1 = bindless
    bool                  m_shadowCutout = false;
    VkDescriptorSetLayout m_shadowSetLayout = VK_NULL_HANDLE;  // set2: b0 shadow array, b1 CSM UBO
    VkDescriptorPool      m_shadowDescPool = VK_NULL_HANDLE;
    VkDescriptorSet       m_shadowSet[kFramesInFlight]{};      // per-frame (b1 is per-frame data)
    glm::mat4             m_lightViewProj{ 1.0f };  // computed each frame
    bool                  m_shadowOverride = false;        // setShadowBounds: fixed shadow box
    glm::vec3             m_shadowCenter{ 0.0f };
    float                 m_shadowOrtho = kShadowOrtho;
    float                 m_shadowDepthHalf = kShadowDepthHalf;

    // ---- CASCADED SHADOW MAPS (r_csm; Lane 3) -----------------------------
    // Per-cascade single-layer render views into m_shadowImg (dynamic rendering
    // attaches one layer per cascade pass) + the per-frame CSM UBO that mesh.frag
    // and glass.frag read at set2/binding1.
    VkImageView           m_shadowLayerView[kCsmCascades]{};
    VkBuffer              m_csmUbo[kFramesInFlight]{};
    VmaAllocation         m_csmUboAlloc[kFramesInFlight]{};
    void*                 m_csmUboMapped[kFramesInFlight]{};
    // Host-facing knobs (setCsmParams; driven by r_csm / r_csm_lambda / r_csm_dist
    // / r_shadowforward). enabled=false reproduces the legacy single cascade
    // EXACTLY — same matrix, same layer, same shader branch.
    bool                  m_csmEnabled = false;
    float                 m_csmLambda  = csm::kDefaultLambda;
    float                 m_csmDistance = csm::kDefaultShadowDistance;
    float                 m_csmBlend   = 0.12f;   // cross-fade band as a fraction of each slice
    bool                  m_csmDebug   = false;  // r_csm_debug: step visibility per cascade
    // Cheap interim / A-B reference (r_shadowforward, meters): push the LEGACY
    // single-cascade ortho box forward along the camera forward vector so the
    // shadowed region leads the car instead of being centred on it. 0 = the
    // historical camera-centred box, bit-for-bit.
    float                 m_shadowForward = 0.0f;
    // This frame's fitted cascades (filled in prepareFrameData, consumed by
    // recordShadowPassBody). count == 0 means the legacy path ran.
    csm::Result           m_csm{};
    uint32_t              m_csmCascadesThisFrame = 0;
    uint32_t              m_curImageIndex  = 0;

    // ---- Render graph (perf-stack B) --------------------------------------
    // ONE persistent graph; rebuilt cheaply each frame in endFrame() from the
    // passes' declared resource reads/writes (capacity persists -> no per-frame
    // heap churn). It derives + emits every sync2 barrier + layout transition and
    // drives begin/endRendering. The shadow map's state PERSISTS across frames
    // (DEPTH_READ_ONLY after the main pass samples it; UNDEFINED on first use) and
    // is fed back in via importImage so the cross-frame WAR barrier is derived.
    RenderGraph           m_graph;
    ResourceState         m_shadowState{ VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0 };
    // Deferred HUD draws (appended by drawHudQuad/drawHudText, replayed inside the
    // graph's color pass). Capacity persists across frames.
    std::vector<HudRecord> m_hudRecords;
    // Stable storage for the VkRenderingInfo + attachment structs the graph passes
    // reference (must outlive execute(); the graph holds pointers into these).
    VkRenderingInfo           m_shadowRenderInfo{};
    VkRenderingAttachmentInfo m_shadowDepthAttach{};
    VkRenderingInfo           m_mainRenderInfo{};
    VkRenderingAttachmentInfo m_mainDepthAttach{};

    // Swapchain (windowed mode only; all null/empty in headless mode)
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkExtent2D     m_extent{};
    VkFormat       m_format = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage>     m_swapImages;
    std::vector<VkImageView> m_swapViews;
    std::vector<VkSemaphore> m_renderFinished; // per swapchain image

    // ---- Headless offscreen render target (headless mode only) -------------
    // The single color image the render graph targets in place of an acquired
    // swapchain image. Same format/extent the swapchain used (B8G8R8A8_UNORM,
    // COLOR_ATTACHMENT | TRANSFER_SRC) so the capture readback is byte-identical.
    // m_extent / m_format / m_depthImg / m_depthView above are shared with the
    // windowed path (only ONE of the two target sets is ever live).
    bool          m_headless = false;
    VkImage       m_offscreenColorImg   = VK_NULL_HANDLE;
    VmaAllocation m_offscreenColorAlloc = nullptr;
    VkImageView   m_offscreenColorView  = VK_NULL_HANDLE;

    // ---- Editor UI (Dear ImGui, docking) — EDITOR-ONLY ---------------------
    // All default null/false so a NON-editor run (no initEditorUI call) allocates
    // nothing and the editor-UI graph pass is never added. The descriptor pool is
    // DEDICATED to ImGui (never shared with the bindless/HUD pools — sharing would
    // corrupt them). m_editorDrawData is filled by endEditorUI() (CPU draw-data
    // build, OUTSIDE the command buffer) and consumed by the editor-UI graph pass
    // inside buildAndExecuteGraph() — mirrors prepareFrameData()->recordMeshDraws().
    VkDescriptorPool m_imguiPool      = VK_NULL_HANDLE;
    bool             m_imguiInit      = false;
    ImDrawData*      m_editorDrawData = nullptr;
    // Stable storage for the editor-UI pass's VkRenderingInfo + color attachment so
    // they outlive execute() (the graph holds pointers into these — same pattern as
    // the composite pass's m_compositeRenderInfo / m_compositeAttach above).
    VkRenderingInfo           m_editorUiRenderInfo{};
    VkRenderingAttachmentInfo m_editorUiAttach{};

    // Per-frame-in-flight
    Frame    m_frames[kFramesInFlight];
    uint32_t m_frameIdx = 0;
    uint64_t m_totalFrames = 0;

    // ---- Deferred buffer destruction (fix 2) ------------------------------
    // When updateMesh() promotes a static mesh to dynamic, its old device-local
    // vbo may still be referenced by command buffers from up to kFramesInFlight-1
    // earlier frames. Rather than vkDeviceWaitIdle, we queue the buffer for
    // destruction tagged with the frame index it became unreferenced, and free it
    // once kFramesInFlight more frames have started (all referencing frames retired).
    struct PendingFree { VkBuffer buf; VmaAllocation alloc; uint64_t retireAtFrame; };
    std::vector<PendingFree> m_pendingFrees;
    void deferDestroyBuffer(VkBuffer buf, VmaAllocation alloc);

    // ---- Deferred image/view/sampler destruction (fix 2) ------------------
    // destroyTexture() / destroyMesh() used to vkDeviceWaitIdle before freeing —
    // a full GPU stall PER destroy. During terrain-streaming eviction that is
    // dozens of stalls per boundary-cross. Instead we queue the GPU objects with
    // the frame index they became unreferenced and free them once kFramesInFlight
    // frames have begun (every in-flight cmd buffer that could reference them has
    // retired — same retirement rule as deferDestroyBuffer). The bindless-slot
    // write-back to white (host-side vkUpdateDescriptorSets) is done IMMEDIATELY
    // by the caller; only the actual destroy is deferred. Any of the handles may
    // be VK_NULL_HANDLE (e.g. a mesh defers only buffers; a texture defers
    // image+view+sampler) — the drain skips null handles.
    struct PendingImageFree {
        VkImage       image; VmaAllocation alloc;
        VkImageView   view;  VkSampler     sampler;
        uint64_t      retireAtFrame;
    };
    std::vector<PendingImageFree> m_pendingImageFrees;
    void deferDestroyImage(VkImage image, VmaAllocation alloc,
                           VkImageView view, VkSampler sampler);
    void drainPendingFrees();
    // Force-free EVERYTHING still queued (used at shutdown after a final
    // vkDeviceWaitIdle, when all frames are guaranteed retired).
    void flushPendingFrees();

    // ---- Perf instrumentation ---------------------------------------------
    // GPU timestamps: the device's valid bit count + ns-per-tick (timestampPeriod).
    // Both come from VkPhysicalDeviceLimits; if timestamps are unsupported on the
    // graphics queue the pool is never created and gpuFrameMs stays 0.
    bool     m_tsSupported = false;
    float    m_tsPeriodNs  = 0.0f;       // nanoseconds per timestamp tick
    uint64_t m_tsValidMask = 0;          // mask of meaningful timestamp bits
    float    m_lastGpuMs   = 0.0f;       // most recent GPU pass time (ms)
    // Counters being accumulated for the in-flight frame (reset each beginFrame),
    // and the snapshot of the last completed frame (returned by stats()).
    RenderStats m_building{};            // accumulates during the current frame
    RenderStats m_lastStats{};           // snapshot taken at endFrame()

    // ---- ZERO-STUTTER state (docs/ZERO_STUTTER.md) -------------------------
    // Persistent pipeline cache + the late-creation audit + frame-pacing ring.
    VkPipelineCache m_pipelineCache    = VK_NULL_HANDLE;
    uint64_t        m_cacheLoadedBytes = 0;       // bytes accepted from disk at boot
    double          m_psoCreateMs      = 0.0;     // wall ms spent in vkCreate*Pipelines
    bool            m_firstFrameBegun  = false;   // set by the first beginFrame()
    bool            m_creationBoundary = false;   // inside a declared recreate boundary
    PacingParams    m_pacing{};                   // cvar-driven (setPacingParams)
    // Totals + late-creation audit counters (late = after the first frame began,
    // outside a declared boundary — these are the zero-stutter receipts).
    uint32_t m_psoTotal = 0, m_psoLate = 0, m_modulesLate = 0, m_poolsLate = 0;
    // Per-frame attribution counters (reset in beginFrame, read by the spike log).
    uint32_t m_psoThisFrame = 0, m_modulesThisFrame = 0, m_poolsThisFrame = 0,
             m_allocsThisFrame = 0, m_asBuildsThisFrame = 0;
    bool m_iblBakedThisFrame = false, m_recreatedThisFrame = false;
    // Frame-time ring (post-warmup samples only; CPU = endFrame->endFrame wall).
    static constexpr uint32_t kPaceRingCap = 4096;
    struct PaceSample { float cpuMs; float gpuMs; };
    std::vector<PaceSample> m_paceRing;
    uint32_t m_paceWrite = 0;
    bool     m_paceHaveLast = false;
    std::chrono::steady_clock::time_point m_paceLast{};
    uint32_t m_spikeCount = 0;       // all post-warmup spikes (logged)
    uint32_t m_spikeCleanCount = 0;  // spikes with NO attributed cause (the gate)
    bool     m_tlasDbReceiptLogged = false;  // one-shot TLAS double-buffer proof line (#5)

    bool m_vsync = true;
    bool m_needsRecreate = false;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_ssaa = 1, m_outW = 0, m_outH = 0;   // SSAA: m_width = m_outW*ssaa; downscale on capture

    // Camera (FPS); defaults frame the cube at origin
    glm::vec3 m_camPos{ 0.0f, 1.5f, 4.0f };
    float m_camYaw = -1.5708f;   // look toward -Z
    float m_camPitch = -0.30f;   // slightly down
    float m_camFov = 60.0f;
    float m_camRoll = 0.0f;      // roll about view-forward (radians; 0 = upright)
    float m_camFar = 200.0f;     // W8-3: far plane (the historic hardcode as default)
    // Roll-capable camera basis (set by setCameraBasis). When m_camHasBasis, the
    // view uses these directly (up != world-up => the view rolls). setCamera (yaw/
    // pitch) clears it, so every existing host is pixel-identical.
    glm::vec3 m_camFwd{ 0.0f, 0.0f, -1.0f };
    glm::vec3 m_camUp{ 0.0f, 1.0f, 0.0f };
    bool      m_camHasBasis = false;

    // ---- Forward point lights (interior fill) -----------------------------
    // CPU-side cache set by setPointLights(); re-uploaded into each frame's UBO.
    // m_ambient is a small constant lift so shadowed/back-faces aren't pure black
    // (a touch cool/blue to read as a sci-fi interior without washing out).
    std::vector<PointLight> m_pointLights;
    // Hemispheric ambient FLOOR (mesh.frag ambientCount.rgb). The original 0.10/0.11/
    // 0.14 was tuned far too dark for a SUNLESS indoor scene (B1 has no directional
    // sun, so unlit areas fell to near-black — "incredibly dark"). Lifted ~2.6x to a
    // readable cool-sci-fi floor. PERF: render-cost-neutral (a shader constant; no
    // extra draws/work). See docs/PERF_LOG.md.
    // 2nd lift (still "couldn't see"): 0.26 -> 0.42. Sunless B1 needs a real ambient
    // floor; point lights only pool under fixtures, leaving floor/walls black between.
    // NOT the dial for an outdoor world: mesh.frag's iblAmbient() prefers the baked
    // environment and only falls back to this flat constant when none is valid, so
    // setAmbient() is a no-op anywhere the sky probe is live (measured under the
    // cloud deck — see host_tunnel.cpp applySky()). Sky-lit worlds change the fill
    // by changing the SKY; m_iblDirty rebakes the probe from it.
    glm::vec3               m_ambient{ 0.42f, 0.44f, 0.50f };
    int                     m_debugView = 0;   // r_debugview: 0 = off, 1 = shading normals
    // CLI --set override latch (see setCVarOverrides above). Default-constructed
    // = inactive = every apply() is an immediate no-op.
    RenderCVarOverrides     m_cvarOv{};
    // Final additive bloom strength; defaults to the global subtle value, per-scene
    // override via setBloom() (the showroom raises it for the glowing-spire hero look).
    float                   m_bloomIntensity = kBloomIntensity;
    float                   m_exposure = 1.0f;   // whole-scene brightness (composite pre-tonemap)
    // ---- Painterly levers (ART_BIBLE §5): host-opted zone atmosphere + grade ----
    FogParams               m_fogParams{};       // enabled=false -> fog pass never recorded
    CausticsParams          m_caustics{};        // enabled=false -> mesh.frag caustics gate stays shut
    WetnessParams           m_wetness{};         // amount=0 -> mesh.frag wetness gate stays shut
    float                   m_snowCover = 0.0f;  // 0 -> terrain snow band is altitude-only
    GradeParams             m_gradeParams{};     // strength=0 -> composite grade block inert
    FilmicParams            m_filmic{};          // enabled=false -> composite filmic block inert
    uint32_t                m_filmicFrame = 0;   // per-frame grain-seed advance (the crawl)
    glm::mat4               m_fogInvProjCPU{ 1.0f };  // frame inverse-projection for fog.frag
    VkPipelineLayout        m_fogLayout = VK_NULL_HANDLE;
    VkPipeline              m_fogPipe   = VK_NULL_HANDLE;
    VkDescriptorSet         m_setFog    = VK_NULL_HANDLE;   // b0 = main depth (TAA depth sampler)
    // ---- Volumetric scattering variant of the fog pass ---------------------
    // set0 = m_setFog (depth), set1 = the frame's objSet (Camera UBO b1 -> sun +
    // lightViewProj + the 64 point lights), set2 = m_shadowSet (sampler2DShadow).
    // Reuses the EXISTING sets/layouts so nothing new is allocated or written.
    glm::mat4               m_volInvViewProjCPU{ 1.0f };  // jittered clip -> world
    float                   m_volFrameSeed = 0.0f;        // dither rotation (TAA-friendly)
    bool                    m_volActive = false;          // this frame's fog pass took the raymarch branch
    VkPipelineLayout        m_volLayout = VK_NULL_HANDLE;
    VkPipeline              m_volPipe   = VK_NULL_HANDLE;
    VkRenderingAttachmentInfo m_fogAttach{};
    VkRenderingInfo         m_fogRenderInfo{};
    // CPU per-object frustum cull (r_frustumcull). Default ON. m_frameFrustum is the
    // 6 normalized world-space planes for the frame being prepared (filled from the
    // camera viewProj in prepareFrameData, consumed by emitGroup).
    bool                    m_frustumCull = true;
    FrustumPlanes           m_frameFrustum{};
    // RT RESIDENCY (setRtOnlyDraws): sticky while the host fans its room-invisible
    // geometry. Cleared at beginFrame so a host that forgets cannot leak it.
    bool                    m_rtOnlyDraws = false;

    // ---- D15 GPU-driven culling (r_cullpath) -------------------------------
    // m_cullPathReq is the host request (-1 auto / 0 CPU / 1 Tier0 / 2 Tier1 /
    // 3 Tier2); m_cullPathActive is the per-frame RESOLVED path (clamped to what
    // the device + bring-up stage support), latched once in prepareFrameData and
    // consumed by emitGroup + the graph build so a mid-frame cvar change can
    // never tear the frame. Default 0: byte-identical to pre-D15 behavior.
    GpuCullSystem           m_gpuCull;
    CullDeviceCaps          m_cullCaps{};
    bool                    m_gpuCullReady = false;   // pipelines + sets built
    VkDescriptorPool        m_cullPool = VK_NULL_HANDLE;
    VkSampler               m_hzbSampler = VK_NULL_HANDLE;  // NEAREST, clamp, all mips
    int                     m_cullPathReq = 0;
    int                     m_cullPathActive = 0;
    uint32_t                m_frameCullInstances = 0;  // rows the cull pass dispatches over
    // Tier 1 (async compute): the dedicated queue + a timeline semaphore the
    // graphics submit waits on at DRAW_INDIRECT|VERTEX_SHADER.
    VkQueue                 m_computeQueue = VK_NULL_HANDLE;
    uint32_t                m_computeFamily = VK_QUEUE_FAMILY_IGNORED;
    bool                    m_asyncCullReady = false;
    VkSemaphore             m_cullTimeline = VK_NULL_HANDLE;
    uint64_t                m_cullTimelineValue = 0;
    bool                    m_asyncCullThisFrame = false;
    CullFrameInputs         m_asyncCullInputs{};
    bool                    m_hzbEnabled = false;     // r_hzb (needs path >= 1)
    bool                    m_hzbActiveThisFrame = false; // resolved per frame
    // HZB depth pyramid (R32_SFLOAT, full mip chain, GENERAL layout forever).
    // Built from LAST frame's depth at the top of this frame's command buffer
    // (one-frame occlusion latency — disocclusions resolve next frame).
    static constexpr uint32_t kHzbMaxMips = 16;
    VkImage                 m_hzbImg = VK_NULL_HANDLE; VmaAllocation m_hzbAlloc = nullptr;
    VkImageView             m_hzbViewAll = VK_NULL_HANDLE;          // all mips (cull samples)
    VkImageView             m_hzbMipView[kHzbMaxMips] = {};        // one mip each (reduce dst/src)
    VkDescriptorSet         m_hzbMipSet[kHzbMaxMips] = {};
    VkDescriptorPool        m_hzbPool = VK_NULL_HANDLE;
    uint32_t                m_hzbMipCount = 0;
    uint32_t                m_hzbW = 0, m_hzbH = 0;                 // mip 0 dims
    bool                    m_hzbReady = false;
    // Last frame's depth: tracked post-graph state + "has been rendered once"
    // (the pyramid must never reduce an UNDEFINED depth image).
    ResourceState           m_depthState{};
    bool                    m_depthValid = false;
    GpuCullSystem::HzbChain m_hzbChain{};   // stable record-ctx storage
    bool                    m_cullEquivCheck = false; // --test-gpucull harness
    // Latest read-back cull counters (frames-in-flight latency) + equivalence.
    CullStatsGpu            m_lastCullStats{};
    uint32_t                m_lastCullExpected = 0;
    uint32_t                m_cullEquivFrames = 0;
    uint32_t                m_cullEquivMismatches = 0;
    // ---- CLUSTERED FORWARD LIGHTING (r_clusterlights) ----------------------
    // OFF by default: the legacy 64-light UBO loop stays the shipping path until
    // a host opts in, so every existing md5 / screenshot gate keeps holding
    // without touching a single world. setClusterLights() is the only writer.
    bool                    m_clusterLights = false;
    // CPU-SIDE STAGING for the assignment. The froxel lists live in a
    // persistent-mapped HOST_ACCESS_SEQUENTIAL_WRITE (write-combined) buffer, and
    // the assignment does a read-modify-write on every froxel's count. READING
    // write-combined memory is uncached and un-prefetched, and it cost 5.0 ms for
    // a 48-light frame (measured) — 8x the entire rest of the pass. Assign into
    // ordinary cached memory here, then do ONE linear memcpy into the mapped
    // buffer, which is exactly the access pattern write-combining is built for.
    std::vector<uint32_t>   m_clusterCounts;           // kClusterCount
    std::vector<uint32_t>   m_clusterIndices;          // kClusterCount * kMaxLightsPerCluster
    ClusterBuildResult      m_clusterStats{};          // last frame's assignment counters
    float                   m_clusterCpuMs = 0.0f;     // CPU ms spent assigning last frame
    uint32_t                m_clusterOverflowLogged = 0; // rate-limiter for the overflow warning
    float                   m_metalAmbient = 1.0f; // metal ambient-spec floor strength (mesh.frag ibl.w; r_metalambient)
    float                   m_iblIntensity = 1.0f; // IBL ambient scale (mesh.frag ibl.y; SEAM 2 interior/exterior balance)
    float                   m_iblSpecular  = -1.0f; // ABSOLUTE env-specular scale (mesh.frag refl.z); <0 = unset -> shader falls back to m_iblIntensity (pre-R10 math exactly)
    // ---- vis-unify: host-injected PVS numbers + per-stage timing -----------
    uint32_t                m_visRoomsCulled = 0;   // setVisHostStats (this frame's room/portal skips)
    float                   m_visPvsMs = 0.0f;      // setVisHostStats (flood-fill ms)
    float                   m_cullCpuMs = 0.0f;     // device emit/cull walk CPU time (LANE 6: now assigned from cpu zones)
    float                   m_cullGpuMs = 0.0f;     // cull.comp dispatch GPU time (LANE 6: now assigned from the "gpu-cull" pass timestamp pair)
    float                   m_hzbGpuMs = 0.0f;      // HZB reduce GPU time (LANE 6: now assigned from the "hzb-build" pass timestamp pair)

    // ---- LANE 6 PER-PASS / PER-ZONE PERF BREAKDOWN ------------------------
    // Rolling accumulator over a window of frames so a dump is stable instead of
    // a single noisy frame. Pass identity is the string-literal pointer the
    // RenderPassDesc carries; passes that appear in only some frames (RT, GI,
    // capture) are averaged over the frames they actually ran, and `rows[].frames`
    // records that so the dump can say so.
    struct PerfPassAccum { const char* name = nullptr; double gpuMs = 0.0; double cpuMs = 0.0; uint32_t frames = 0; };
    PerfPassAccum           m_perfPass[kMaxTimedPasses]{};
    uint32_t                m_perfPassCount = 0;
    uint64_t                m_perfCpuTicks[x3::perf::Z_Count]{};   // summed rdtsc ticks per CPU zone
    uint32_t                m_perfCpuCalls[x3::perf::Z_Count]{};
    double                  m_perfFrameCpuMs = 0.0;   // summed wall CPU frame time (endFrame->endFrame)
    double                  m_perfFrameGpuMs = 0.0;   // summed whole-frame GPU bracket
    uint32_t                m_perfFrames = 0;         // frames whose per-pass GPU times landed in the window
    uint32_t                m_perfCpuFrames = 0;      // frames whose CPU zones landed in the window
    bool                    m_passTimersOn = true;    // X3_PASSTIMERS=0 disables (the A/B for measurement overhead)
    float                   m_perfAutoDumpS = 0.0f;   // X3_PASSDUMP=<seconds>: auto-log the breakdown (0 = off)
    double                  m_perfNextDumpS = 0.0;    // next auto-dump time (s since first frame)
    std::chrono::steady_clock::time_point m_perfT0{};      // first-frame clock origin
    std::chrono::steady_clock::time_point m_perfLastEnd{}; // endFrame->endFrame wall delta
    bool                    m_perfHaveLastEnd = false;
    bool                    m_perfEnvRead = false;
    void                    accumulatePerfFrame(const Frame& fr, const float* passGpuMs);
    void                    accumulateCpuZones();
    void                    logPerfBreakdown(const char* why);
    void                    resetPerfWindow();
};

} // namespace x3::rhi
