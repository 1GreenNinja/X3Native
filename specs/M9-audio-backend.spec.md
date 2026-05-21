# Spec: Audio Backend (miniaudio)  (M9)

> Clean-room — implement from THIS FILE + public refs ONLY. No RBDOOM source.
> miniaudio is MIT/public-domain — clean. Zero 14900K dependency.

- **Implements interface:** `IAudioBackend` (`engine/audio/IAudioBackend.h`)
- **Status:** SPEC (ready)
- **Library:** miniaudio (single-header).

## 1. Purpose
3D-positional + 2D audio: spatialized SFX (HRTF), streaming music with crossfade, ambient beds, and a small bus mixer (Master/SFX/Music/Voice). Replaces id's sound system entirely.

## 2. Interface contract
```cpp
// engine/audio/IAudioBackend.h — clean; miniaudio hidden in .cpp
#include <cstdint>
#include <string_view>

namespace x3::audio {

struct Vec3 { float x=0,y=0,z=0; };
struct SoundId { uint32_t id=0; bool valid() const { return id!=0; } };
enum class Bus : uint8_t { Master, SFX, Music, Voice };

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual bool init() = 0;
    virtual void shutdown() = 0;

    // Listener (driven by camera each frame)
    virtual void setListener(Vec3 pos, Vec3 forward, Vec3 up) = 0;

    // One-shot + looping. Path resolved via IAssetSource.
    virtual SoundId play2D(std::string_view virtualPath, Bus bus, float volume = 1.0f) = 0;
    virtual SoundId play3D(std::string_view virtualPath, Vec3 pos, Bus bus, float volume = 1.0f) = 0;
    virtual void    setSoundPosition(SoundId, Vec3) = 0;
    virtual void    stop(SoundId) = 0;

    // Music: streamed, with crossfade between tracks.
    virtual void playMusic(std::string_view virtualPath, float crossfadeSeconds = 2.0f) = 0;
    virtual void stopMusic(float fadeSeconds = 1.0f) = 0;

    // Bus volumes (0..1)
    virtual void setBusVolume(Bus, float volume) = 0;

    virtual void update(float dt) = 0;       // advances fades, ducking, cleanup
};

IAudioBackend* createAudioBackend(class asset::IAssetSource* assets);

} // namespace x3::audio
```

## 3. Behavior
- Decode: WAV (and OGG/MP3 via miniaudio's optional decoders) from `IAssetSource` blobs (decode-from-memory).
- 3D: per-source position + listener → attenuation + panning; HRTF where miniaudio supports it.
- Music: streamed (not fully decoded into RAM); crossfade old↔new over N seconds.
- Buses: each sound routed to a bus; bus volume multiplies into Master. Optional ducking: Music ducks under Voice/combat (simple gain envelope).
- Threading: miniaudio runs its own audio thread; the interface marshals safely (lock-free ring or miniaudio's API).

## 4. Edge cases & error handling
- Missing/undecodable file → no sound, log once, return invalid SoundId.
- play on a stopped/invalid SoundId → no-op.
- Many simultaneous sounds → cap voices (e.g., 64); steal the oldest/quietest when over budget.
- Device lost / no audio device → init logs + continues silently (game still playable).
- Volume out of range → clamp 0..1.

## 5. Performance targets
- Mixing 64 voices on the audio thread well within real-time budget.
- play2D/play3D return immediately (no main-thread decode stall for short SFX; stream for long).
- No glitches/pops on crossfade (equal-power fade curve).

## 6. Acceptance tests
1. **T1 — 2D play:** `play2D("sfx/click.wav", Bus::SFX)` is audible.
2. **T2 — 3D pan:** a looping 3D source to the listener's left pans left; moving it right pans right; distance attenuates.
3. **T3 — Music crossfade:** `playMusic(A)` then `playMusic(B, 2.0)` crossfades A→B over 2 s with no silence gap.
4. **T4 — Bus volume:** `setBusVolume(Music, 0.0)` silences music but not SFX.
5. **T5 — Voice cap:** trigger 200 one-shots in a frame → no crash; voice count capped; oldest stolen.
6. **T6 — Missing file:** play a nonexistent path → invalid SoundId, single log line, engine fine.
7. **T7 — Ducking (if implemented):** playing a Voice sound dips Music by the configured amount, restores after.

## 7. Public references
- miniaudio documentation (engine API, sound groups, 3D spatialization, decoding-from-memory).
- General 3D audio attenuation + equal-power crossfade references (public).

## 8. Suggested permissive libraries
- **miniaudio** (MIT/public-domain) — device, decode, mix, 3D, streaming. Single header; covers nearly all of this.

## 9. Notes for the clean-room implementer
- Use miniaudio's high-level `ma_engine` + `ma_sound` to start; buses map to `ma_sound_group`.
- Decode SFX from memory (`IAssetSource` blob) so everything ships in `.x3pak`. Stream music from a pak entry (miniaudio supports custom VFS read callbacks — wire to IAssetSource).
- Keep `ma_*` types in the .cpp. Interface is plain structs + opaque SoundId.
- Match the bus model to Marble TTT's mixer concept (Master/SFX/Music/Voice) so settings UI is consistent across games.
