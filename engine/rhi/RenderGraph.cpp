// RenderGraph implementation (perf-stack B). CLEAN-ROOM, original work — built
// from the Vulkan 1.3 spec (synchronization2 / dynamic rendering) + the public
// frame-graph design pattern. // No GPL / id Tech / RBDOOM source consulted.
//
// The core idea is tiny and robust: each graph resource carries a CURRENT STATE
// (layout + the stage/access that last touched it). When a pass declares it needs
// a resource in some (layout, stage, access), execute() compares that against the
// tracked state and, if ANY of layout/stage/access implies a hazard or a layout
// change, emits exactly one VkImageMemoryBarrier2 transitioning old->new with the
// correct src/dst scopes — then advances the tracked state. This is precisely the
// set of barriers the renderer used to hand-write, but now DERIVED from the
// declared reads/writes, so the validation layer (which is what proves it) sees
// the same correct sync it did before.

#include "RenderGraph.h"
#include "../core/x3_log.h"
#include <cassert>

namespace x3::rhi {

RgResource RenderGraph::importImage(const char* name, VkImage image,
                                    const ResourceState& current,
                                    uint32_t arrayLayers) {
    RgResource h{ (uint32_t)m_resources.size() };
    m_resources.push_back(Resource{ name, image, current, arrayLayers ? arrayLayers : 1u });
    return h;
}

void RenderGraph::beginFrame() {
    // Reuse capacity; only the element count resets. No heap free/realloc, so the
    // steady per-frame path allocates nothing. (Resources are re-imported each
    // frame with their correct entry state — the renderer keeps the persistent
    // shadow-map state in a member and feeds it back in via importImage.)
    m_resources.clear();
    m_passes.clear();
    m_lastBarrierCount = 0;
}

void RenderGraph::addPass(RenderPassDesc&& pass) {
    m_passes.push_back(std::move(pass));
}

ResourceState RenderGraph::stateOf(RgResource r) const {
    if (!r.valid() || r.id >= m_resources.size()) return ResourceState{};
    return m_resources[r.id].state;
}

void RenderGraph::setState(RgResource r, const ResourceState& s) {
    if (!r.valid() || r.id >= m_resources.size()) return;
    m_resources[r.id].state = s;
}

void RenderGraph::execute(VkCommandBuffer cmd) {
    // Stack scratch for the per-pass barrier batch. A pass touches at most a
    // handful of resources, so a small fixed array avoids any per-frame heap use.
    // kMaxUsesPerPass (shared from RenderGraph.h) bounds both a pass's declared
    // uses[] and this barrier batch, so they can never disagree.
    VkImageMemoryBarrier2 barriers[kMaxUsesPerPass];
    // Per-use record of whether this pass's use CONTINUED an existing read epoch
    // (read-after-read, same layout, nothing written since). Filled in the barrier
    // loop, consumed by the state-advance loop below so the two agree.
    bool continuesRead[kMaxUsesPerPass];

    for (RenderPassDesc& pass : m_passes) {
        uint32_t bcount = 0;
        // Clear the whole array (not just the iterations reached): the batch-cap
        // guard below can `break` out early, which would otherwise leave stale
        // flags from the previous pass driving this pass's state advance.
        for (uint32_t i = 0; i < kMaxUsesPerPass; ++i) continuesRead[i] = false;

        // ---- Derive the barriers for this pass from its declared resource uses.
        // For each used resource compare the required (layout,stage,access) with
        // the tracked current state; emit a transition when the layout differs OR
        // there is a write involved (RAW/WAR/WAW) OR a read after a write. We
        // always wait on the resource's last stage/access (src scope) and make the
        // memory available/visible to this pass's stage/access (dst scope) — this
        // is the same dependency the hand-code expressed, derived generically.
        for (uint32_t ui = 0; ui < pass.useCount; ++ui) {
            continuesRead[ui] = false;   // default: the state advance overwrites
            const ResourceUse& use = pass.uses[ui];
            if (!use.res.valid() || use.res.id >= m_resources.size()) continue;
            // Fix 5: a pass that needs MORE barriers than the batch can hold is a
            // silent sync hazard if we just `break`. bcount can never exceed
            // useCount, and useCount is capped at kMaxUsesPerPass by addUse(), so
            // this is structurally impossible — but guard it loudly anyway: assert
            // in Debug, logError + skip (don't emit garbage) in Release.
            if (bcount >= kMaxUsesPerPass) {
                assert(bcount < kMaxUsesPerPass &&
                       "RenderGraph: pass exceeded kMaxUsesPerPass barriers — raise the cap");
                x3::logError("[rhi] RenderGraph: pass barrier batch exceeded "
                             "kMaxUsesPerPass — a sync barrier was dropped (raise the cap)");
                break;
            }
            Resource& r = m_resources[use.res.id];
            const ResourceState& cur = r.state;

            const bool layoutChange = (cur.layout != use.layout);
            const bool prevWasWrite = (cur.access & (
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_2_TRANSFER_WRITE_BIT |
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                VK_ACCESS_2_SHADER_WRITE_BIT)) != 0;

            // A transition is needed when the layout must change, when this pass
            // writes (WAR/WAW must order against the prior reader/writer), or when
            // this pass reads something a prior pass wrote (RAW).
            //
            // SYNC FIX — read-after-read is NOT unconditionally safe to skip. The
            // old rule ("skip read-after-read in the SAME layout with no prior
            // write — there is no hazard there") is only true about the two READS.
            // It silently drops the dependency on the WRITE that came before them:
            // that write was made visible by ONE barrier, whose dstScope named only
            // the FIRST reader. A second reader in the same layout but with a
            // different stage/access never gets the write made visible to IT.
            // Concretely: depth-prepass writes depth -> SSAO samples it
            // (FRAGMENT_SHADER / SHADER_SAMPLED_READ, layout DEPTH_READ_ONLY) ->
            // the velocity / particle / debris pass then uses the SAME layout as a
            // LOAD_OP_LOAD depth attachment (EARLY_FRAGMENT_TESTS /
            // DEPTH_STENCIL_ATTACHMENT_READ). Same layout, both reads, no write in
            // between -> no barrier was emitted, and the prepass's write was never
            // made visible to the fragment-test read. Sync validation reports it as
            // "vkCmdBeginRendering ... READ_AFTER_WRITE ... current synchronization
            // allows SHADER_SAMPLED_READ at FRAGMENT_SHADER, but ... must allow
            // DEPTH_STENCIL_ATTACHMENT_READ at EARLY_FRAGMENT_TESTS".
            //
            // So: also emit when this read needs stage or access bits the tracked
            // (already-made-visible) scope does not already cover. The barrier's
            // src scope is the previous reader, which CHAINS the original write's
            // availability forward per the spec's memory-dependency chaining. Read
            // scopes are then accumulated (below) rather than overwritten, so the
            // set only ever grows until the next write and this stays O(1) barriers
            // per new scope — not one per pass.
            const bool widerScope = ((use.stage  & ~cur.stage)  != 0) ||
                                    ((use.access & ~cur.access) != 0);
            const bool needBarrier = layoutChange || use.isWrite || prevWasWrite || widerScope;
            // Record whether this is a continuing read epoch (same layout, this use
            // reads, nothing written since) so the state advance accumulates instead
            // of overwriting.
            continuesRead[ui] = !layoutChange && !use.isWrite && !prevWasWrite;
            if (!needBarrier) continue;

            VkImageMemoryBarrier2& b = barriers[bcount++];
            b = VkImageMemoryBarrier2{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            // Src scope = whatever last touched the resource. On first use this is
            // TOP_OF_PIPE / access 0 (the imported entry state), which is the
            // correct "no prior work" scope and pairs with an UNDEFINED old layout.
            b.srcStageMask  = cur.stage;
            b.srcAccessMask = cur.access;
            b.dstStageMask  = use.stage;
            b.dstAccessMask = use.access;
            b.oldLayout     = cur.layout;
            b.newLayout     = use.layout;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = r.image;
            // Span EVERY array layer of the resource (1 for all images here except
            // the CSM shadow array, whose 4 cascade layers are all written by the
            // shadow pass and all sampled by the main pass). Mip 0 only: no graph
            // pass transitions a non-base mip.
            b.subresourceRange = { use.aspect, 0, 1, 0, r.arrayLayers };
        }

        if (bcount > 0) {
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = bcount;
            dep.pImageMemoryBarriers = barriers;
            vkCmdPipelineBarrier2(cmd, &dep);
            m_lastBarrierCount += bcount;
        }

        // ---- Advance tracked state for every used resource (even reads with no
        // barrier) so a later pass derives its src scope correctly. For a pure
        // read-after-read we still record the latest reader's stage/access so a
        // subsequent writer waits on ALL readers.
        for (uint32_t ui = 0; ui < pass.useCount; ++ui) {
            const ResourceUse& use = pass.uses[ui];
            if (!use.res.valid() || use.res.id >= m_resources.size()) continue;
            Resource& r = m_resources[use.res.id];
            r.state.layout = use.layout;
            if (continuesRead[ui]) {
                // Read epoch continues: ACCUMULATE. The comment above always
                // claimed "we still record the latest reader's stage/access so a
                // subsequent writer waits on ALL readers" — but an overwrite keeps
                // only the LAST reader, so a following writer could race an earlier
                // one (WAR), and a following reader saw a scope narrower than what
                // had actually been made visible. OR-ing is what "all readers"
                // means, and it also makes widerScope above settle: once a scope has
                // been covered, later uses inside the same epoch add no barrier.
                r.state.stage  |= use.stage;
                r.state.access |= use.access;
            } else {
                r.state.stage  = use.stage;
                r.state.access = use.access;
            }
        }

        // ---- Drive dynamic rendering + record the pass body (function pointer +
        // ctx — no std::function, no per-frame heap alloc).
        if (pass.usesDynamicRendering)
            vkCmdBeginRendering(cmd, &pass.renderInfo);
        if (pass.record)
            pass.record(pass.recordCtx, cmd);
        if (pass.usesDynamicRendering)
            vkCmdEndRendering(cmd);
    }
}

} // namespace x3::rhi
