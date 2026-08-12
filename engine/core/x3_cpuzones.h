#pragma once
// x3_cpuzones — LANE 6 (streaming & perf). Allocation-free, header-only CPU cost
// attribution for the render frame.
//
// WHY THIS EXISTS
// ---------------
// docs/ZERO_STUTTER.md:166 records CPU p50 72.91 ms vs GPU p50 45.05 ms on a 5090:
// the frame is CPU-bound by ~28 ms and NOTHING in this engine said where that CPU
// time goes. `docs/screenshots/gpucull/RESULTS.md:46-54` names the immediate-mode
// drawMesh() submission walk as the suspect but never measured it against the rest
// of the frame. These zones close that gap: every microsecond the frame spends
// inside the render device is bucketed, and whatever is left over is host time
// (game tick, physics, AI, EnvArt iteration).
//
// DESIGN
// ------
//  * rdtsc, not steady_clock. The hot zone (drawMesh) fires thousands of times per
//    frame; QueryPerformanceCounter is ~20-30 ns a call, so 2 calls x 4000 draws is
//    ~0.2 ms of measurement overhead ON the thing being measured. __rdtsc() is ~10
//    cycles. TSC is invariant on every CPU this engine targets (Zen2+/Skylake+).
//  * Ticks are converted to ms with a frequency calibrated ONCE, lazily, against
//    steady_clock over a 1 ms spin. If calibration produces nonsense the whole
//    facility reports 0 and never lies.
//  * Single-threaded by contract: these zones are for the RENDER thread only. The
//    accumulator is a plain function-local static; no atomics, no locks, no cost.
//  * Zones NEST. A scope adds its own elapsed ticks to its own bucket only, so
//    the buckets of an outer and an inner zone both include the inner time. The
//    reporting side (VulkanRenderDevice::perfBreakdown) knows which buckets are
//    leaves and does not double-count.
//
// Clean-room / original work. No GPL, id Tech or RBDOOM source consulted.

#include <cstdint>
#include <chrono>

#if defined(_MSC_VER)
  #include <intrin.h>
#endif

namespace x3 { namespace perf {

// ---------------------------------------------------------------------------
// Zone ids. Order here is the order the breakdown prints. Keep leaves and
// composites grouped; see kZoneIsLeaf below.
// ---------------------------------------------------------------------------
enum Zone : uint32_t {
    Z_BeginFrame = 0,   // vkWaitForFences + acquire + per-frame reset  (GPU back-pressure lives here)
    Z_DrawMesh,         // IRenderDevice::drawMesh* submission walk     (the gpucull/RESULTS.md suspect)
    Z_Skin,             // setSkinnedPalette (bone palette upload)
    Z_Hud,              // drawHudQuad / drawText record
    Z_Prepare,          // prepareFrameData: camera/light UBO + object SSBO + indirect fill
    Z_AsBuild,          // buildRtSceneAS: BLAS refit + TLAS build INCLUDING its blocking fence waits
    Z_GraphRecord,      // buildAndExecuteGraph: graph build + all pass record callbacks
    Z_Submit,           // vkQueueSubmit2 + vkQueuePresentKHR
    Z_EndFrameTotal,    // the WHOLE of endFrame (prepare + asbuild + graph + submit + everything else)
    Z_HostDrawFan,      // HOST time BETWEEN two consecutive drawMesh calls (see DrawScope)
    // ---- HOST spans (LANE 6 REPLAY 2026-08): the 41% residual, split ------------
    // Wired in app/world_hosts/host_echotropolis.cpp's frame loop. EXCLUSIVE: a
    // HostScope reports its span MINUS everything already attributed inside it, so
    // a host span that contains drawMesh/hud/beginFrame does not double-count them
    // and these rows join the leaf partition directly (see kHostZoneFirst below).
    Z_HostInput,        // poll + console/menu/mode keys + camera rig + car/walk drive
    Z_HostPhysics,      // phys->step (nested inside Z_HostInput; subtracted from it)
    Z_HostStream,       // Tier-2 streamer tick (region residency wants/loads)
    Z_HostSim,          // tod/atmosphere + crowd + npcLife + AI + audio listener + ocean
    Z_HostDrawFans,     // the host's draw-fan + HUD-record section, device zones removed
    Z_HostFrameCap,     // r_maxfps sleep/spin (MUST be ~0 for a valid measurement)
    // ---- sub-buckets of Z_AsBuild (nested; NOT summed into the leaf partition) --
    Z_AsBlas,           // BLAS refit record + endBlasBatch's blocking fence wait
    Z_AsInstances,      // repack every draw record into TLAS instance rows + signature
    Z_AsTlas,           // VulkanRT::buildTlas incl. ITS blocking fence wait
    // ---- LEAF SPLIT of the three above (TLAS-split lane 2026-08) ---------------
    // The three buckets above each mix O(96k) CPU work with a BLOCKING FENCE WAIT.
    // Which half dominates decides the whole optimisation, so both halves are named.
    Z_AsBlasWait,       // endBlasBatch(): submit + vkWaitForFences  (inside as.blas_refit)
    Z_AsSig,            // tlasSignature() over every instance       (inside as.instance_pack)
    Z_AsTlasPack,       // buildTlas row pack + memcpy into the instance buffer
    Z_AsTlasWait,       // buildTlas oneTimeSubmit: submit + vkWaitForFences
    // ---- LEAF (TLAS-split lane): the AS batch's fence wait, RELOCATED ---------
    // The AS batch is submitted inside cpu.rt_as_build but WAITED at the end of
    // endFrame, so its stall is no longer inside that bucket. It gets its own leaf
    // so the frame partition stays honest and nobody reads the smaller
    // cpu.rt_as_build as the whole story.
    Z_AsDrain,          // endFrame: vkWaitForFences on the AS batch, pre-submit
    Z_Count
};

inline const char* zoneName(uint32_t z) {
    switch (z) {
        case Z_BeginFrame:    return "cpu.beginframe";
        case Z_DrawMesh:      return "cpu.drawmesh";
        case Z_Skin:          return "cpu.skinpalette";
        case Z_Hud:           return "cpu.hud";
        case Z_Prepare:       return "cpu.preparedata";
        case Z_AsBuild:       return "cpu.rt_as_build";
        case Z_GraphRecord:   return "cpu.graph_record";
        case Z_Submit:        return "cpu.submit_present";
        case Z_EndFrameTotal: return "cpu.endframe_total";
        case Z_HostDrawFan:   return "cpu.host_drawfan";
        case Z_HostInput:     return "cpu.host_input";
        case Z_HostPhysics:   return "cpu.host_physics";
        case Z_HostStream:    return "cpu.host_stream";
        case Z_HostSim:       return "cpu.host_sim";
        case Z_HostDrawFans:  return "cpu.host_drawfans";
        case Z_HostFrameCap:  return "cpu.host_framecap";
        case Z_AsBlas:        return "  as.blas_refit";
        case Z_AsInstances:   return "  as.instance_pack";
        case Z_AsTlas:        return "  as.tlas_build";
        case Z_AsBlasWait:    return "    as.blas_wait";
        case Z_AsSig:         return "    as.signature";
        case Z_AsTlasPack:    return "    as.tlas_pack";
        case Z_AsTlasWait:    return "    as.tlas_wait";
        case Z_AsDrain:       return "cpu.rt_as_wait";
        default:              return "cpu.?";
    }
}

// ---------------------------------------------------------------------------
// Tick source + calibration.
// ---------------------------------------------------------------------------
inline uint64_t rdtscNow() {
#if defined(_MSC_VER)
    return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// Ticks per millisecond. Calibrated once against steady_clock with a 1 ms spin
// (no sleep: a sleep can be preempted and would poison the ratio). Returns 0.0 if
// the measurement is implausible, which makes every zone report 0.0 ms rather
// than a fabricated number.
inline double ticksPerMs() {
    static const double kTpms = []() -> double {
        using clock = std::chrono::steady_clock;
        const auto  w0 = clock::now();
        const uint64_t t0 = rdtscNow();
        // Busy-spin ~1 ms. Volatile sink so the loop is not optimized away.
        volatile uint64_t sink = 0;
        for (;;) {
            for (int i = 0; i < 512; ++i) sink += i;
            if (std::chrono::duration<double, std::milli>(clock::now() - w0).count() >= 1.0)
                break;
        }
        const uint64_t t1 = rdtscNow();
        const double ms  = std::chrono::duration<double, std::milli>(clock::now() - w0).count();
        if (ms <= 0.0 || t1 <= t0) return 0.0;
        const double tpms = (double)(t1 - t0) / ms;
        // Sanity window: 0.1 GHz .. 20 GHz effective. Anything outside means the
        // TSC is not usable here (VM, non-invariant part) -> report nothing.
        if (tpms < 1.0e5 || tpms > 2.0e7) return 0.0;
        return tpms;
    }();
    return kTpms;
}

inline float ticksToMs(uint64_t ticks) {
    const double tpms = ticksPerMs();
    return (tpms > 0.0) ? (float)((double)ticks / tpms) : 0.0f;
}

// ---------------------------------------------------------------------------
// Per-frame accumulator. Render thread only.
// ---------------------------------------------------------------------------
struct FrameAccum {
    uint64_t ticks[Z_Count]{};
    uint32_t calls[Z_Count]{};
    // Timestamp of the last drawMesh EXIT — the anchor DrawScope uses to charge
    // the host's between-draws work (see Z_HostDrawFan).
    uint64_t lastDrawExit = 0;
    // LANE 6 REPLAY: running sum of every tick already charged to a LEAF bucket
    // this frame. A HostScope snapshots this on entry/exit and reports only the
    // difference its own span did NOT already account for, which is what makes
    // the host spans exclusive (and therefore safe to add to the leaf partition).
    // Only leaves contribute — composites (Z_AsBuild, Z_EndFrameTotal) must not,
    // or their children would be subtracted twice.
    uint64_t attributed = 0;
    void reset() {
        for (uint32_t i = 0; i < Z_Count; ++i) { ticks[i] = 0; calls[i] = 0; }
        lastDrawExit = 0;
        attributed   = 0;
    }
};

// Leaves that partition the frame. Composites (Z_AsBuild, Z_EndFrameTotal) and the
// as.* sub-buckets are excluded so `attributed` counts each tick exactly once.
inline bool zoneIsLeaf(uint32_t z) {
    switch (z) {
        case Z_BeginFrame: case Z_DrawMesh: case Z_Skin: case Z_Hud:
        case Z_Prepare:    case Z_GraphRecord: case Z_Submit:
        case Z_HostDrawFan: case Z_AsDrain:
            return true;
        default:
            return false;   // host spans self-report via HostScope; composites never
    }
}

inline FrameAccum& frameAccum() {
    static FrameAccum a;
    return a;
}

// Master enable. Off => Scope is a pair of predictable branches and two rdtsc
// reads are skipped entirely, so `r_passtimers 0` truly removes the overhead.
inline bool& zonesEnabled() {
    static bool on = true;
    return on;
}

// ---------------------------------------------------------------------------
// Console <-> device control block.
//
// The echotropolis world registers its console commands inside
// app/world_hosts/host_echotropolis.cpp, which LANE 7 owns and this lane must
// not touch (see docs/plans/SESSION_LANES.md). The HUD's own console
// registration (app/hud.cpp) has no IRenderDevice reference. So the console
// side sets a request here and the device consumes it at the end of the next
// frame — no host edit, works in every world host at once.
// ---------------------------------------------------------------------------
struct Control {
    bool dumpRequest = false;   // console `r_passdump` -> device logs the breakdown
    int  setTimers   = -1;      // console `r_passtimers 0|1` -> -1 = no change
};
inline Control& control() { static Control c; return c; }

// RAII scope. Adds its elapsed ticks to its bucket on destruction.
struct Scope {
    uint32_t z;
    uint64_t t0;
    explicit Scope(uint32_t zone) : z(zone), t0(zonesEnabled() ? rdtscNow() : 0) {}
    ~Scope() {
        if (!t0) return;
        FrameAccum& a = frameAccum();
        const uint64_t d = rdtscNow() - t0;
        a.ticks[z] += d;
        a.calls[z] += 1;
        if (zoneIsLeaf(z)) a.attributed += d;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// ---------------------------------------------------------------------------
// HostScope — an EXCLUSIVE span of host code (LANE 6 REPLAY 2026-08).
//
// The 0bc0d482 measurement left 41 % of the frame in an unattributed
// `cpu.host_outside` residual because nothing outside the render device was
// timed. Timing host spans naively double-counts: the draw-fan section contains
// ~92,000 drawMesh calls that Z_DrawMesh/Z_HostDrawFan already own.
//
// HostScope charges its bucket `span - (everything attributed inside the span)`.
// Because a HostScope publishes only its EXCLUSIVE result into `attributed`,
// host spans NEST correctly too: an inner Z_HostPhysics inside Z_HostInput is
// subtracted from the outer exactly once. The result is a true partition, so
// these rows go straight into the leaf list and `cpu.host_outside` shrinks by
// exactly what they capture.
// ---------------------------------------------------------------------------
struct HostScope {
    uint32_t z;
    uint64_t t0;
    uint64_t a0;
    explicit HostScope(uint32_t zone)
        : z(zone), t0(zonesEnabled() ? rdtscNow() : 0), a0(frameAccum().attributed) {}
    // Close the span early. The frame loop's spans are DELIMITED, not nested in
    // braces (the loop body declares locals that later spans still read), so the
    // host declares one HostScope per span and stop()s it at the boundary. Safe to
    // call twice; the destructor becomes a no-op afterwards.
    void stop() {
        if (!t0) return;
        FrameAccum& a = frameAccum();
        const uint64_t span  = rdtscNow() - t0;
        const uint64_t inner = a.attributed - a0;
        const uint64_t excl  = (span > inner) ? (span - inner) : 0;
        a.ticks[z]   += excl;
        a.calls[z]   += 1;
        a.attributed += excl;
        t0 = 0;
    }
    ~HostScope() { stop(); }
    HostScope(const HostScope&) = delete;
    HostScope& operator=(const HostScope&) = delete;
};

// ---------------------------------------------------------------------------
// DrawScope — the zone for drawMesh, plus the trick that attributes HOST cost
// without touching a single host file.
//
// The echotropolis frame issues ~92,000 drawMesh calls. The device's own work
// per call is small; the EXPENSIVE part is what the host does BETWEEN two calls
// (walk the next EnvArtSystem instance, build its 4x4, resolve its textures,
// distance/frustum test it). That work is unreachable from here — but the GAP
// between one drawMesh returning and the next one entering measures it exactly.
//
// Gaps longer than kGapCutoffMs are NOT draw-fan work (they are the game tick /
// physics / AI / streaming that runs before or after the draw fans), so they are
// excluded and fall out in the residual "cpu.host_outside" row instead. That
// keeps the two host rows honest and separable.
// ---------------------------------------------------------------------------
inline constexpr double kGapCutoffMs = 0.05;   // 50 us

struct DrawScope {
    uint64_t t0;
    explicit DrawScope() : t0(zonesEnabled() ? rdtscNow() : 0) {
        if (!t0) return;
        FrameAccum& a = frameAccum();
        if (a.lastDrawExit && t0 > a.lastDrawExit) {
            const uint64_t gap = t0 - a.lastDrawExit;
            if ((double)gap < kGapCutoffMs * ticksPerMs()) {
                a.ticks[Z_HostDrawFan] += gap;
                a.calls[Z_HostDrawFan] += 1;
                a.attributed           += gap;
            }
        }
    }
    ~DrawScope() {
        if (!t0) return;
        FrameAccum& a = frameAccum();
        const uint64_t t1 = rdtscNow();
        a.ticks[Z_DrawMesh] += t1 - t0;
        a.calls[Z_DrawMesh] += 1;
        a.attributed        += t1 - t0;
        a.lastDrawExit = t1;
    }
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;
};

#define X3_CPU_ZONE_CAT2(a, b) a##b
#define X3_CPU_ZONE_CAT(a, b)  X3_CPU_ZONE_CAT2(a, b)
// Usage:  X3_CPU_ZONE(Z_DrawMesh);   // times to end of enclosing scope
#define X3_CPU_ZONE(zone) ::x3::perf::Scope X3_CPU_ZONE_CAT(_x3zone_, __LINE__)(::x3::perf::zone)
// Usage:  X3_HOST_ZONE(Z_HostSim);   // EXCLUSIVE host span (see HostScope)
#define X3_HOST_ZONE(zone) ::x3::perf::HostScope X3_CPU_ZONE_CAT(_x3host_, __LINE__)(::x3::perf::zone)

}} // namespace x3::perf
