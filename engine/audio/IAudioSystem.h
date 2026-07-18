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

// Opaque handle to a live LOOPING voice (startLoop/stopLoop). id==0 = invalid/none
// (e.g. headless/silent mode, or an unstarted loop). Stopping an invalid handle is
// a safe no-op, so a caller can keep a single LoopHandle (0 = "no loop running").
struct LoopHandle {
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

    // Playlist support (Club Jukebox): true when the CURRENT music request has
    // finished and a playlist driver should advance to the next track. Precisely:
    // a non-looping track that has reached its end, OR a non-looping track that
    // was requested-and-enabled but has no live voice (missing/corrupt file, or
    // silent/no-device mode) — both mean "done, move on". Returns false while a
    // track is actively playing, while it loops, when music is disabled, and when
    // no track has ever been requested. Default no-op returns false so backends
    // opt in (the miniaudio backend overrides it).
    virtual bool isMusicFinished() const { return false; }

    // ---- Live volume controls (Settings menu) -----------------------------
    // Set the music bed's volume in [0,1]. Applied immediately to the playing
    // music voice (and remembered for any music started later). Silent/no-music
    // -> harmlessly stored. setMusicEnabled(true) resumes at THIS volume.
    virtual void setMusicVolume(float vol) = 0;

    // Enable/disable the music bed live. false stops/silences the current bed
    // (the track + its loop position are forgotten); true (re)starts the last
    // music track at the current music volume. No-op if no track was ever set.
    virtual void setMusicEnabled(bool enabled) = 0;

    // Master SFX volume in [0,1]: scales ALL playSound2D/playSound3D output. The
    // scale is applied INTERNALLY in one place (every voice's volume is multiplied
    // by this), so call sites need not change. Applied to NEW voices going forward.
    virtual void setMasterSfxVolume(float vol) = 0;

    // ---- Sustained LOOP voices (held-fire weapons) ------------------------
    // Start a 2D LOOPING voice of `sound` (e.g. a held auto-fire whine) and return a
    // LoopHandle to control/stop it. The voice loops until stopLoop() is called. vol
    // is scaled by the master SFX volume (same one-place multiply as one-shots), so
    // the audio settings slider quiets it too. Invalid handle / silent / no-device ->
    // returns an invalid LoopHandle (graceful no-op). 2D is intentional: the loop is
    // the player's OWN weapon (no spatialization needed). Pure-virtual so every
    // backend implements it (mirrors how the volume setters were added).
    virtual LoopHandle startLoop(SoundHandle sound, float vol = 1.0f, float pitch = 1.0f) = 0;

    // Start a 3D SPATIALIZED looping voice of `sound` at world position (x,y,z) and
    // return a LoopHandle to control/stop it (same handle family as startLoop() —
    // stopLoop()/setLoopParams() work on either). Use this for a fixed ambient
    // emitter (a buzzing light fixture, a machine hum) instead of retriggering a
    // one-shot on a timer: the loop wraps seamlessly (no pop/gap at the seam,
    // mirroring how playMusic loops) AND, because spatialization is left ON and the
    // position is set once here, miniaudio's engine re-derives distance attenuation
    // + panning EVERY mix callback against the live listener (setListener) for as
    // long as the loop runs — the same continuous mechanism a playSound3D one-shot
    // rides while it plays, just sustained indefinitely instead of for one clip's
    // duration. No occlusion routing (that stays a one-shot-only feature via
    // setOcclusionProvider); ambient loops sit on the plain spatializer path.
    // Invalid handle / silent / no-device -> invalid LoopHandle (graceful no-op).
    virtual LoopHandle startLoop3D(SoundHandle sound, float x, float y, float z,
                                    float vol = 1.0f, float pitch = 1.0f) = 0;

    // Stop + free a loop voice started by startLoop()/startLoop3D(). Stopping an
    // invalid/already-stopped handle is a safe no-op (double-stop is fine).
    // Silent/no-device -> no-op.
    virtual void stopLoop(LoopHandle loop) = 0;

    // Update a live loop voice's volume/pitch (vol is master-SFX-scaled, like
    // startLoop). Default no-op so backends may opt out; invalid handle -> no-op.
    virtual void setLoopParams(LoopHandle /*loop*/, float /*vol*/, float /*pitch*/) {}

    // ---- RT ACOUSTICS hooks (occlusion + room reverb) ----------------------
    // Occlusion provider: a callback the mixer invokes ONCE per 3D one-shot at
    // play time with the emitter's world position, returning the smoothed
    // occlusion factor in [0,1] (0 = clear line of sound, 1 = fully behind
    // geometry). The mixer applies it as a volume duck + a per-voice one-pole
    // lowpass chain (muffled-through-the-wall). Pass fn=nullptr to unhook —
    // the play path is then byte-for-byte the pre-acoustics path for new
    // voices. Plain function pointer + user so no <functional> crosses the
    // interface. Default no-op (backends may opt out).
    using OcclusionFn = float (*)(void* user, float x, float y, float z);
    virtual void setOcclusionProvider(OcclusionFn /*fn*/, void* /*user*/) {}

    // Room reverb (RT-acoustics room estimate -> mixer): a single shared
    // Schroeder reverb INSERT (4 comb + 2 allpass, dry + wet*reverb) that all
    // 3D one-shots route through while an occlusion provider is hooked.
    // `t60Seconds` is the decay time (RT60); `wet` is the wet mix [0..1]
    // (0 = audibly dry). Both are smoothed inside the audio thread (no zipper).
    // The reverb chain is created lazily on the first call; before that (or in
    // silent mode) this is a harmless store. Default no-op.
    virtual void setReverbParams(float /*t60Seconds*/, float /*wet*/) {}

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
