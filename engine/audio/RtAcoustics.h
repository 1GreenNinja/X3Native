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
//     temporally smoothed (~200 ms) so successive gunshots glide rather than
//     zipper. The mixer (MiniaudioSystem) queries it once per 3D one-shot via
//     IAudioSystem::setOcclusionProvider and applies a volume duck + a
//     per-voice lowpass — stand behind a wall from gunfire and it muffles;
//     step through the door and it opens up.
//
//   * ROOM REVERB ESTIMATE: every ~0.5 s it fires a 64-ray deterministic
//     sphere from the LISTENER; the mean free path + miss fraction classify
//     the surrounding space (small / medium / large / outdoor) which maps to
//     reverb decay (T60) + wet level for the mixer's Schroeder insert
//     (IAudioSystem::setReverbParams), smoothed so doorway transitions sweep.
//
// The TRACER is injected (plain function pointer + user): the game wires it to
// IRenderDevice::traceAudioRays (the GPU ray-query batch vs the TLAS); the
// headless self-test (--test-acoustics) wires a deterministic CPU box-room
// tracer instead — same math, no GPU required. A tracer returning false means
// "no data this call" (e.g. the TLAS isn't built yet): all last values hold.
//
// Budget: <= 16 emitters * 9 rays + 64 room rays = 208 rays per update, one
// batched trace call — ~0.1 ms class, trivial next to DDGI's thousands.

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

// Batched tracer: fill outHitT[i] with the CLOSEST hit distance along ray i
// (< 0 = miss within tMax) and return true; return false = no data this call
// (caller keeps its last values). Plain function pointer + user (no
// <functional> across the engine boundary).
using AcousticTraceFn = bool (*)(void* user, const AcousticRay* rays, int count,
                                 float* outHitT);

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

    // Inject the ray tracer (game: IRenderDevice::traceAudioRays bridge;
    // test: CPU box tracer). fn=nullptr disables all tracing (occlusion 0).
    void setTracer(AcousticTraceFn fn, void* user);

    // Master gate (snd_rtacoustics): disabled -> occlusionAt returns 0, update
    // is a no-op, and the room estimate decays to dry (wet 0).
    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }

    // Listener (player camera) world position, set per frame.
    void setListener(float x, float y, float z);

    // Mixer occlusion provider entry: smoothed occlusion [0,1] for an emitter
    // at (x,y,z). Registers/refreshes the emitter slot; a NEW slot is traced
    // synchronously so the very first shot from a fresh spot is already
    // correct. Thunk form matches IAudioSystem::OcclusionFn.
    float occlusionAt(float x, float y, float z);
    static float occlusionThunk(void* self, float x, float y, float z) {
        return static_cast<RtAcoustics*>(self)->occlusionAt(x, y, z);
    }

    // Per-frame: re-trace active emitters (one batched call), advance the
    // ~200 ms occlusion smoothing, expire idle emitters (3 s), and every
    // ~0.5 s re-probe the room sphere + smooth the reverb targets.
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
        bool  used = false;
    };

    // Trace the (1 + kFanRays) occlusion fan for one emitter NOW; returns the
    // blocked fraction, or -1 when the tracer has no data.
    float traceEmitterFan(const Emitter& e);
    void  probeRoom();           // 64-ray sphere -> classify -> set targets
    static void classify(float meanFreePath, float missFrac,
                         RoomClass& cls, float& t60, float& wet);

    AcousticTraceFn m_trace = nullptr;
    void* m_traceUser = nullptr;
    bool  m_enabled = true;

    float m_lx = 0, m_ly = 0, m_lz = 0;   // listener
    Emitter m_emitters[kMaxEmitters];

    RoomEstimate m_room;
    float m_roomT60Target = 0.8f;
    float m_roomWetTarget = 0.0f;
    float m_roomTimer = 999.0f;           // fire the first probe immediately

    // Reused batch scratch (no per-frame heap churn after warm-up).
    std::string m_dbgScratch;
};

// Headless self-test (--test-acoustics): deterministic CPU box-room tracer;
// asserts wall-occlusion high vs line-of-sight ~0, the 200 ms smoothing,
// small-room vs walls-removed (outdoor) classification, determinism across
// instances, and prints a wall-walk transcript (the snd_rta_debug story).
bool runAcousticsSelfTest();

} // namespace x3::audio
