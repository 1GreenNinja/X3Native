#pragma once
// Audio system interface (M9).
//
// Spec: specs/M9-audio-backend.spec.md. The spec sketches a richer IAudioBackend
// (buses, crossfade, IAssetSource-backed VFS). For the vertical slice we expose
// the smaller, app-facing IAudioSystem the host actually needs to make the slice
// SOUND: 2D/3D one-shots, a positional listener, and a single looping music/
// ambient track. miniaudio's high-level ma_engine covers all of it.
//
// Clean-room: this header is implementation-agnostic — NO miniaudio types leak
// out. The concrete MiniaudioSystem keeps every ma_* type inside its .cpp.
//
// Design intent matched from the spec:
//   * init() is GRACEFUL: if there is no audio device (headless CI, RDP session,
//     no speakers) it logs a warning and runs as a silent no-op. It never throws
//     and never crashes; every play call becomes a no-op. init() still returns
//     true in that case so the host treats "no device" as a non-fatal success.
//   * load() returns a 0/invalid SoundHandle on failure (logged once, non-fatal).
//   * Sounds are loaded from REAL absolute G:\ paths (like the purchased GLBs);
//     no WAV/music is copied into the public repo.

#include <cstdint>
#include <string_view>

namespace x3::audio {

// Opaque handle to a loaded sound. id==0 is the invalid/failed handle.
struct SoundHandle {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
};

class IAudioSystem {
public:
    virtual ~IAudioSystem() = default;

    // Bring up the audio device. Returns true on success OR on graceful no-device
    // (silent mode). Returns false only on a genuine, unexpected failure. Never
    // crashes. Safe to call once.
    virtual bool init() = 0;

    // Tear down: stop everything, free voices/sounds, close the device. Idempotent.
    virtual void shutdown() = 0;

    // Load a sound from an ABSOLUTE filesystem path (e.g. "G:/.../shot.wav").
    // Returns an invalid handle (id==0) on failure (missing/undecodable file);
    // the failure is logged once and is non-fatal. miniaudio decodes WAV natively.
    virtual SoundHandle load(std::string_view absPath) = 0;

    // Play a previously-loaded sound as a non-positional 2D one-shot. Volume and
    // pitch are clamped/sane; invalid handle or silent mode -> no-op.
    virtual void playSound2D(SoundHandle sound, float vol = 1.0f, float pitch = 1.0f) = 0;

    // Play a one-shot at world position (x,y,z), spatialized against the current
    // listener (attenuation + panning). Invalid handle or silent mode -> no-op.
    virtual void playSound3D(SoundHandle sound, float x, float y, float z,
                             float vol = 1.0f, float pitch = 1.0f) = 0;

    // Set the listener (driven by the player camera each frame). yaw/pitch are in
    // radians, in the engine's forward convention:
    //   forward = (cos pitch cos yaw, sin pitch, cos pitch sin yaw).
    virtual void setListener(float x, float y, float z, float yaw, float pitch) = 0;

    // Start a streamed music/ambient track from an absolute path. Replaces any
    // currently-playing music. loop=true loops indefinitely. Missing file or
    // silent mode -> no-op (logged).
    virtual void playMusic(std::string_view absPath, bool loop = true, float vol = 1.0f) = 0;

    // Stop the music track (if any).
    virtual void stopMusic() = 0;

    // Per-frame tick: advances any internal bookkeeping (voice cleanup). Cheap.
    virtual void update(float dt) = 0;
};

// Factory. Always returns a valid object; if no device is present the object runs
// in silent no-op mode (call init() to find out — it logs which mode it is in).
IAudioSystem* createAudioSystem();

// Headless self-test (--test-audio). Exercises init -> load -> playSound2D ->
// update -> shutdown with no device required (no-device is treated as PASS). The
// point is to prove the system initializes, loads, and tears down cleanly with no
// crash/leak on a machine that may or may not have an audio device. Returns true
// unless something genuinely fails.
bool runAudioSelfTest();

} // namespace x3::audio
