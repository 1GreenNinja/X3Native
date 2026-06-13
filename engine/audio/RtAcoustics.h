#pragma once
// RT ACOUSTICS — audio rays through the render TLAS.
//
// The piece almost nobody ships: gunfire MUFFLES through the actual level
// geometry and rooms reverb from their ACTUAL shape, using the same scene TLAS
// the RT AO / reflections / DDGI passes trace. This module is the CPU brain:
//
//   * OCCLUSION: for every active 3D sound emitter it fires a primary
//     listener->emitter ray plus a small deterministic jittered fan (8 rays)
//     against the tracer; the blocked-ray fraction is the occlusion factor,
//     temporally smoothed (~200 ms) so transitions glide rather than zipper.
//     The mixer (MiniaudioSystem) queries it via IAudioSystem::
//     setOcclusionProvider — at play time AND live per update for every
//     in-flight 3D voice — and applies a volume duck + a per-voice lowpass:
//     stand behind a wall from gunfire and it muffles; step through the door
//     and it opens up.
//
//   * ROOM REVERB ESTIMATE: every ~0.5 s it fires a 64-ray deterministic
//     sphere from the LISTENER; the mean free path + miss fraction classify
//     the surrounding space (small / medium / large / outdoor) which maps to
//     reverb decay (T60) + wet level for the mixer's Schroeder insert
//     (IAudioSystem::setReverbParams), smoothed so doorway transitions sweep.
//
// The TRACER is injected as an ASYNC submit/harvest pair (plain function
// pointers + user): the game wires it to IRenderDevice::traceAudioRaysSubmit/
// Harvest (the GPU ray-query batch vs the TLAS — async so the game thread
// never fence-waits behind frame GPU work); the headless self-test
// (--test-acoustics) wires a deterministic CPU box-room tracer with the same
// one-update latency. Each update HARVESTS the previous batch (applying it
// with saved slot metadata, so expired/reused slots never mis-apply) and
// SUBMITS the next. No data yet -> all last values hold.
//
// Budget: <= 16 emitters * 9 rays + 64 room rays = 208 rays per batch, one
// submit per update — ~0.1 ms class GPU, ~microseconds CPU.

#include <string>

namespace x3::audio {

// One audio ray. LAYOUT-COMPATIBLE with IRenderDevice::AudioRay and the
// std430 ARay in shaders/audio_rays.comp (two vec4s; static_asserted where
// the game bridges the two types).
struct AcousticRay {
    float ox = 0, oy = 0, oz = 0;  // world-space origin
    float tMax = 1.0f;             // ray length (meters)
    float dx = 0, dy = 1, dz = 0;  // direction (normalized)
    float pad = 0;
};

// Async tracer pair (mirrors IRenderDevice::traceAudioRaysSubmit/Harvest):
//  * submit: kick a batch; false = busy / no TLAS yet / unsupported.
//  * harvest: poll the last batch; returns its ray count and fills outHitT
//    (closest hit, < 0 = miss) when done; 0 = still pending; -1 = none/lost.
using AcousticSubmitFn  = bool (*)(void* user, const AcousticRay* rays, int count);
using AcousticHarvestFn = int  (*)(void* user, float* outHitT, int capacity);

enum class RoomClass : int { Small = 0, Medium = 1, Large = 2, Outdoor = 3 };

struct RoomEstimate {
    RoomClass cls = RoomClass::Medium;
    float meanFreePath = 8.0f;   // average listener->surface distance (m)
    float missFrac = 0.0f;       // fraction of room rays that escaped (sky)
    float t60 = 0.8f;            // smoothed reverb decay handed to the mixer
    float wet = 0.0f;            // smoothed reverb wet mix handed to the mixer
};

class RtAcoustics {
public:
    static constexpr int kMaxEmitters = 16;  // active occlusion-tracked emitters
    static constexpr int kFanRays     = 8;   // jitter fan around each primary ray
    static constexpr int kRoomRays    = 64;  // listener room-probe sphere
    static constexpr int kBatchCap    = kMaxEmitters * (1 + kFanRays) + kRoomRays;

    // Inject the async tracer (game: IRenderDevice bridge; test: CPU box
    // tracer). nullptrs disable all tracing (occlusion 0, room stays dry).
    void setTracer(AcousticSubmitFn submit, AcousticHarvestFn harvest, void* user);

    // Master gate (snd_rtacoustics): disabled -> occlusionAt returns 0 and
    // update() is a no-op.
    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }

    // Listener (player camera) world position, set per frame.
    void setListener(float x, float y, float z);

    // Mixer occlusion provider entry: smoothed occlusion [0,1] for an emitter
    // at (x,y,z). Registers/refreshes the emitter slot. A NEW slot starts at 0
    // and SNAPS to its first traced value (one update of latency); the mixer's
    // live per-voice re-query makes that inaudible (~16-33 ms). Thunk form
    // matches IAudioSystem::OcclusionFn.
    float occlusionAt(float x, float y, float z);
    static float occlusionThunk(void* self, float x, float y, float z) {
        return static_cast<RtAcoustics*>(self)->occlusionAt(x, y, z);
    }

    // Per-frame: HARVEST the previous batch (apply occlusion targets + the
    // room estimate via saved metadata), advance the ~200 ms smoothing,
    // expire idle emitters (3 s), and SUBMIT the next batch (with the room
    // sphere appended every ~0.5 s).
    void update(float dt);

    const RoomEstimate& room() const { return m_room; }
    int activeEmitters() const;

    // snd_rta_debug line: per-emitter occlusion + room class/mfp/t60/wet.
    std::string debugString() const;

private:
    struct Emitter {
        float x = 0, y = 0, z = 0;
        float occ = 0.0f;        // smoothed (what the mixer hears)
        float occTarget = 0.0f;  // latest traced blocked-ray fraction
        float idle = 0.0f;       // seconds since last occlusionAt() touch
        unsigned gen = 0;        // bumped on (re)allocation — pending-apply guard
        bool  used = false;
        bool  newborn = true;    // first traced value SNAPS (no smoothing ramp)
    };

    void probeApply(const float* hitT, int roomStart);  // room rays -> estimate
    static void classify(float meanFreePath, float missFrac,
                         RoomClass& cls, float& t60, float& wet);

    AcousticSubmitFn  m_submit  = nullptr;
    AcousticHarvestFn m_harvest = nullptr;
    void* m_traceUser = nullptr;
    bool  m_enabled = true;

    float m_lx = 0, m_ly = 0, m_lz = 0;   // listener
    Emitter m_emitters[kMaxEmitters];

    RoomEstimate m_room;
    float m_roomT60Target = 0.8f;
    float m_roomWetTarget = 0.0f;
    float m_roomTimer = 999.0f;           // fire the first probe immediately

    // In-flight batch metadata (what harvest applies results against).
    struct Pending {
        bool active = false;
        int  count = 0;
        int  fanStart[kMaxEmitters] = {};
        int  fanCount[kMaxEmitters] = {};
        unsigned gen[kMaxEmitters] = {};  // slot generation at submit time
        int  roomStart = -1;              // -1 = no room sphere in this batch
    } m_pending;
};

// Headless self-test (--test-acoustics): deterministic CPU box-room tracer
// (same async submit/harvest contract, one-update latency); asserts
// wall-occlusion high vs line-of-sight ~0, the 200 ms smoothing,
// small-room vs walls-removed (outdoor) classification, determinism across
// instances, and prints a wall-walk transcript (the snd_rta_debug story).
bool runAcousticsSelfTest();

} // namespace x3::audio
