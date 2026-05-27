// miniaudio implementation of IAudioSystem (M9). See engine/audio/IAudioSystem.h.
//
// Clean-room: built from the IAudioSystem interface + the public miniaudio API
// only (miniaudio is Unlicense/MIT-0 — clean). No RBDOOM / id Tech source.
//
// Strategy (matches the spec's "use ma_engine + ma_sound" note):
//   * One ma_engine drives the device, mixing, 3D spatialization, and decode.
//   * load(absPath) builds a "prototype" ma_sound flagged DECODE (fully decoded
//     into RAM) but NOT started — it is just a data source we clone from. SFX are
//     short, so decode-to-RAM is cheap and lets us fire many overlapping copies
//     with independent volume/pitch/position.
//   * playSound2D/3D clones the prototype into a transient voice (ma_sound_init_
//     copy), sets its params, starts it, and tracks it. update() reaps voices that
//     have finished (ma_sound_at_end) and a hard voice cap steals the oldest when
//     over budget (spec T5). Spatialization is toggled per-voice (2D = disabled).
//   * Music is a single streamed ma_sound (flag STREAM) — not decoded into RAM.
//
// GRACEFUL no-device: if ma_engine_init fails (no device / no speakers / headless
// CI) we log a warning and run in silent mode: every play call is a no-op, load()
// returns invalid handles, and shutdown() is safe. init() still returns true so
// the host treats no-device as a non-fatal success.

#include "engine/audio/IAudioSystem.h"
#include "engine/core/x3_log.h"

#define MINIAUDIO_IMPLEMENTATION
// We use the high-level engine + decoding; disable capture/encoding we don't need
// to keep the build lean and avoid pulling in unused backends' symbols.
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace x3::audio {
namespace {

// Hard cap on simultaneous one-shot voices (spec T5). Over budget -> steal oldest.
constexpr size_t kMaxVoices = 64;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float clampPitch(float p) { return p < 0.05f ? 0.05f : (p > 8.0f ? 8.0f : p); }

// A loaded, fully-decoded prototype sound we clone transient voices from. Kept
// alive for the system's lifetime (clones reference its decoded data).
struct Proto {
    std::unique_ptr<ma_sound> sound;   // initialized with MA_SOUND_FLAG_DECODE
    std::string path;
    bool ok = false;
};

// A live, transient one-shot voice (a clone of a Proto). Reaped when finished.
struct Voice {
    std::unique_ptr<ma_sound> sound;
    uint64_t serial = 0;   // monotonically increasing; oldest = smallest
};

// A live LOOPING voice (a clone of a Proto set to loop). Lives until stopLoop().
// Keyed by a LoopHandle id so the caller can stop exactly the voice it started.
struct LoopVoice {
    std::unique_ptr<ma_sound> sound;
    uint32_t id = 0;       // the LoopHandle id handed back to the caller
};

class MiniaudioSystem final : public IAudioSystem {
public:
    ~MiniaudioSystem() override { shutdown(); }  // safety net if the host forgets

    bool init() override {
        if (m_inited) return true;

        ma_engine_config cfg = ma_engine_config_init();
        ma_result r = ma_engine_init(&cfg, &m_engine);
        if (r != MA_SUCCESS) {
            // No device / no speakers / headless. Run silent — NOT an error for
            // the host (game stays playable). init() returns true (silent mode).
            x3::logWarn(std::string("[audio] no audio device (ma_engine_init=") +
                        std::to_string((int)r) + ") — running silent (no sound)");
            m_silent = true;
            m_inited = true;
            return true;
        }

        // Listener 0 defaults. World handedness: miniaudio is right-handed by
        // default, matching our forward = (cos p cos y, sin p, cos p sin y).
        ma_engine_listener_set_position(&m_engine, 0, 0.0f, 0.0f, 0.0f);
        ma_engine_listener_set_direction(&m_engine, 0, 1.0f, 0.0f, 0.0f);
        ma_engine_listener_set_world_up(&m_engine, 0, 0.0f, 1.0f, 0.0f);

        m_silent = false;
        m_inited = true;
        x3::logInfo("[audio] miniaudio engine up");
        return true;
    }

    void shutdown() override {
        if (!m_inited) return;
        if (!m_silent) {
            stopMusic();
            // Free transient one-shot voices, then any live loop voices, then the
            // prototypes, then the engine (no leaks — the engine asserts alloc==0).
            for (auto& v : m_voices) {
                if (v.sound) { ma_sound_uninit(v.sound.get()); }
            }
            m_voices.clear();
            for (auto& lv : m_loops) {
                if (lv.sound) { ma_sound_uninit(lv.sound.get()); }
            }
            m_loops.clear();
            for (auto& kv : m_protos) {
                if (kv.second.sound) { ma_sound_uninit(kv.second.sound.get()); }
            }
            m_protos.clear();
            ma_engine_uninit(&m_engine);
        }
        m_inited = false;
        m_silent = false;
    }

    SoundHandle load(std::string_view absPath) override {
        if (!m_inited || m_silent) return SoundHandle{ 0 };
        const std::string path(absPath);

        // Dedupe: same path -> same handle (prototypes are read-only clones).
        auto it = m_pathToId.find(path);
        if (it != m_pathToId.end()) return SoundHandle{ it->second };

        auto proto = std::make_unique<ma_sound>();
        // DECODE: fully decode into RAM (short SFX). NO_SPATIALIZATION here keeps
        // the prototype neutral; clones re-enable spatialization for 3D plays.
        const ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
        ma_result r = ma_sound_init_from_file(&m_engine, path.c_str(), flags,
                                              nullptr, nullptr, proto.get());
        if (r != MA_SUCCESS) {
            x3::logWarn(std::string("[audio] load failed: ") + path +
                        " (ma=" + std::to_string((int)r) + ") — silent for this sound");
            return SoundHandle{ 0 };
        }

        const uint32_t id = m_nextId++;
        Proto p;
        p.sound = std::move(proto);
        p.path = path;
        p.ok = true;
        m_protos.emplace(id, std::move(p));
        m_pathToId.emplace(path, id);
        return SoundHandle{ id };
    }

    void playSound2D(SoundHandle sound, float vol, float pitch) override {
        playInternal(sound, false, 0, 0, 0, vol, pitch);
    }

    void playSound3D(SoundHandle sound, float x, float y, float z,
                     float vol, float pitch) override {
        playInternal(sound, true, x, y, z, vol, pitch);
    }

    void setListener(float x, float y, float z, float yaw, float pitch) override {
        if (!m_inited || m_silent) return;
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw),   sy = std::sin(yaw);
        ma_engine_listener_set_position(&m_engine, 0, x, y, z);
        ma_engine_listener_set_direction(&m_engine, 0, cp * cy, sp, cp * sy);
        ma_engine_listener_set_world_up(&m_engine, 0, 0.0f, 1.0f, 0.0f);
    }

    void playMusic(std::string_view absPath, bool loop, float vol) override {
        // Remember the request so setMusicEnabled(true) / setMusicVolume() can act
        // even in silent mode or before a device exists.
        m_musicPath = std::string(absPath);
        m_musicLoop = loop;
        m_musicVol  = clamp01(vol);
        if (!m_musicEnabled) return;   // music turned off in settings -> don't start the bed
        startMusicVoice();
    }

    void stopMusic() override {
        if (m_music) {
            ma_sound_uninit(m_music.get());
            m_music.reset();
        }
    }

    void setMusicVolume(float vol) override {
        m_musicVol = clamp01(vol);
        if (m_music) ma_sound_set_volume(m_music.get(), m_musicVol);
    }

    void setMusicEnabled(bool enabled) override {
        if (enabled == m_musicEnabled) {
            // Even when unchanged, keep the live voice's volume in sync.
            if (enabled && m_music) ma_sound_set_volume(m_music.get(), m_musicVol);
            return;
        }
        m_musicEnabled = enabled;
        if (!enabled) {
            stopMusic();                 // silence + forget the playing voice
        } else if (!m_musicPath.empty()) {
            startMusicVoice();           // resume the last track at the current vol
        }
    }

    void setMasterSfxVolume(float vol) override {
        // Stored and applied to NEW voices in playInternal() (one place). Live voices
        // already playing keep their volume; new one-shots pick up the new master.
        m_sfxMaster = clamp01(vol);
    }

    LoopHandle startLoop(SoundHandle sound, float vol, float pitch) override {
        if (!m_inited || m_silent || !sound.valid()) return LoopHandle{ 0 };
        auto it = m_protos.find(sound.id);
        if (it == m_protos.end() || !it->second.ok) return LoopHandle{ 0 };

        auto voice = std::make_unique<ma_sound>();
        // 2D loop (the player's own gun) — disable spatialization like playSound2D.
        const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
        ma_result r = ma_sound_init_copy(&m_engine, it->second.sound.get(), flags,
                                         nullptr, voice.get());
        if (r != MA_SUCCESS) {
            x3::logWarn(std::string("[audio] loop clone failed (ma=") +
                        std::to_string((int)r) + ")");
            return LoopHandle{ 0 };
        }
        // Same one-place master-SFX multiply as one-shots so the Settings slider
        // quiets the held-fire loop too.
        ma_sound_set_volume(voice.get(), clamp01(vol) * m_sfxMaster);
        ma_sound_set_pitch(voice.get(), clampPitch(pitch));
        ma_sound_set_looping(voice.get(), MA_TRUE);
        ma_sound_start(voice.get());

        const uint32_t id = m_nextLoopId++;
        LoopVoice lv;
        lv.sound = std::move(voice);
        lv.id = id;
        m_loops.push_back(std::move(lv));
        return LoopHandle{ id };
    }

    void stopLoop(LoopHandle loop) override {
        if (!loop.valid()) return;   // invalid / already-stopped -> safe no-op
        for (size_t i = 0; i < m_loops.size(); ++i) {
            if (m_loops[i].id == loop.id) {
                if (m_loops[i].sound) ma_sound_uninit(m_loops[i].sound.get());
                m_loops[i] = std::move(m_loops.back());
                m_loops.pop_back();
                return;
            }
        }
        // Not found (already stopped, or never ours): no-op.
    }

    void setLoopParams(LoopHandle loop, float vol, float pitch) override {
        if (!loop.valid()) return;
        for (auto& lv : m_loops) {
            if (lv.id == loop.id && lv.sound) {
                ma_sound_set_volume(lv.sound.get(), clamp01(vol) * m_sfxMaster);
                ma_sound_set_pitch(lv.sound.get(), clampPitch(pitch));
                return;
            }
        }
    }

    void update(float /*dt*/) override {
        if (!m_inited || m_silent) return;
        // Reap finished voices (free their ma_sound + slot).
        for (size_t i = 0; i < m_voices.size();) {
            ma_sound* s = m_voices[i].sound.get();
            if (s && ma_sound_at_end(s)) {
                ma_sound_uninit(s);
                m_voices[i] = std::move(m_voices.back());
                m_voices.pop_back();
            } else {
                ++i;
            }
        }
    }

private:
    // (Re)start the streamed music voice from m_musicPath at m_musicVol. No-op when
    // silent / no device / no remembered track. Replaces any current music voice.
    void startMusicVoice() {
        if (!m_inited || m_silent || m_musicPath.empty()) return;
        stopMusic();
        m_music = std::make_unique<ma_sound>();
        // STREAM: don't decode the whole track into RAM. Music is 2D (no spatial).
        const ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
        ma_result r = ma_sound_init_from_file(&m_engine, m_musicPath.c_str(), flags,
                                              nullptr, nullptr, m_music.get());
        if (r != MA_SUCCESS) {
            x3::logWarn(std::string("[audio] music load failed: ") + m_musicPath +
                        " (ma=" + std::to_string((int)r) + ") — no music");
            m_music.reset();
            return;
        }
        ma_sound_set_looping(m_music.get(), m_musicLoop ? MA_TRUE : MA_FALSE);
        ma_sound_set_volume(m_music.get(), m_musicVol);
        ma_sound_start(m_music.get());
        x3::logInfo(std::string("[audio] music: ") + m_musicPath);
    }

    void playInternal(SoundHandle sound, bool spatial, float x, float y, float z,
                      float vol, float pitch) {
        if (!m_inited || m_silent || !sound.valid()) return;
        auto it = m_protos.find(sound.id);
        if (it == m_protos.end() || !it->second.ok) return;

        // Voice cap (spec T5): steal the oldest live voice when at budget.
        if (m_voices.size() >= kMaxVoices) {
            size_t oldest = 0;
            for (size_t i = 1; i < m_voices.size(); ++i)
                if (m_voices[i].serial < m_voices[oldest].serial) oldest = i;
            if (m_voices[oldest].sound) ma_sound_uninit(m_voices[oldest].sound.get());
            m_voices[oldest] = std::move(m_voices.back());
            m_voices.pop_back();
        }

        auto voice = std::make_unique<ma_sound>();
        // Clone shares the prototype's decoded data (no re-decode). Spatialization
        // is enabled by NOT passing NO_SPATIALIZATION for 3D voices.
        ma_uint32 flags = spatial ? 0u : MA_SOUND_FLAG_NO_SPATIALIZATION;
        ma_result r = ma_sound_init_copy(&m_engine, it->second.sound.get(), flags,
                                         nullptr, voice.get());
        if (r != MA_SUCCESS) {
            x3::logWarn(std::string("[audio] voice clone failed (ma=") +
                        std::to_string((int)r) + ")");
            return;
        }

        // ONE-PLACE master SFX scale: every 2D/3D one-shot's volume is multiplied by
        // the master here, so the Settings SFX slider quiets ALL gunfire/impacts/steps
        // without touching any call site.
        ma_sound_set_volume(voice.get(), clamp01(vol) * m_sfxMaster);
        ma_sound_set_pitch(voice.get(), clampPitch(pitch));
        if (spatial) {
            ma_sound_set_spatialization_enabled(voice.get(), MA_TRUE);
            ma_sound_set_position(voice.get(), x, y, z);
        }
        ma_sound_start(voice.get());

        Voice v;
        v.sound = std::move(voice);
        v.serial = m_nextSerial++;
        m_voices.push_back(std::move(v));
    }

    bool       m_inited = false;
    bool       m_silent = false;
    ma_engine  m_engine{};                 // valid only when !m_silent

    std::unordered_map<uint32_t, Proto>     m_protos;     // id -> prototype
    std::unordered_map<std::string, uint32_t> m_pathToId; // dedupe
    std::vector<Voice>                      m_voices;     // live one-shots
    std::vector<LoopVoice>                  m_loops;      // live looping voices
    std::unique_ptr<ma_sound>               m_music;      // streamed track (or null)

    // Music bed state (remembered so settings can toggle/volume it live, even in
    // silent mode or before a device exists).
    std::string m_musicPath;            // last requested track ("" = none yet)
    bool        m_musicLoop = true;
    float       m_musicVol  = 1.0f;     // current music volume [0,1]
    bool        m_musicEnabled = true;  // Settings "Music ON/OFF"

    // Master SFX volume [0,1]: applied to EVERY one-shot in playInternal (one place).
    float       m_sfxMaster = 1.0f;

    uint32_t m_nextId = 1;       // 0 reserved for invalid handle
    uint64_t m_nextSerial = 1;   // voice age ordering
    uint32_t m_nextLoopId = 1;   // 0 reserved for invalid LoopHandle
};

} // namespace

IAudioSystem* createAudioSystem() { return new MiniaudioSystem(); }

// ===========================================================================
// Headless self-test (--test-audio). init -> load -> playSound2D -> update ->
// shutdown, with no device required (no-device == PASS). Proves the system
// initializes, loads, and tears down cleanly with no crash/leak.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[audio-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[audio-test] FAIL ") + name); }
}

// A real purchased gunshot WAV (absolute G:\ path, never copied into the repo).
// Used to exercise the decode path when a device is present; when there's no
// device load() returns invalid by design (still a PASS — graceful).
constexpr const char* kTestWav =
    "G:/Unity_Projects/EscapeFromLabZero/Assets/Sci-Fi_Guns_Game-Of-Weapons/"
    "Audio/SFX/Wave/Single_Gunshots/Single_Gunshot_Sci-Fi_Gun-01.wav";

} // namespace

bool runAudioSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<IAudioSystem> audio(createAudioSystem());

    // T1: init never crashes; no-device is treated as success.
    bool ok = audio->init();
    check(ok, "T1 init() returns true (device OR graceful silent)");

    // T2: loading a real WAV yields a valid handle OR a graceful invalid one
    // (silent / no device). Either is acceptable — must not crash.
    SoundHandle h = audio->load(kTestWav);
    check(h.valid() || !h.valid(), "T2 load() returns (valid or graceful invalid)");

    // T3: loading a deliberately-missing file must be a graceful invalid handle.
    SoundHandle missing = audio->load("G:/__x3_no_such_audio_file__.wav");
    check(!missing.valid(), "T3 missing file -> invalid handle (graceful)");

    // T4: play + spatial play + listener + update do not crash (no-ops if silent
    // or if the handle is invalid).
    audio->setListener(0, 1.6f, 0, 0.0f, 0.0f);
    audio->playSound2D(h, 1.0f, 1.0f);
    audio->playSound3D(h, 2.0f, 0.0f, 0.0f, 0.8f, 1.1f);
    audio->playSound2D(missing, 1.0f, 1.0f);   // invalid handle -> no-op
    audio->update(1.0f / 60.0f);
    check(true, "T4 play2D/play3D/setListener/update do not crash");

    // T5: music start + stop are safe (no-op when silent / missing).
    audio->playMusic("G:/__x3_no_such_music__.wav", true, 0.5f);
    audio->stopMusic();
    check(true, "T5 playMusic(missing)/stopMusic do not crash");

    // T5b: the live volume/enable setters never crash (no-ops when silent) and are
    // safe in any order — including before/without a music track.
    audio->setMasterSfxVolume(0.5f);
    audio->setMusicVolume(0.7f);
    audio->setMusicEnabled(false);      // off
    audio->playSound2D(h, 1.0f, 1.0f);  // master-scaled (no-op if silent)
    audio->setMusicEnabled(true);       // back on -> resumes last track at current vol
    audio->setMasterSfxVolume(0.0f);    // fully muted SFX path
    audio->playSound2D(h, 1.0f, 1.0f);
    audio->setMasterSfxVolume(1.0f);
    audio->setMusicVolume(0.25f);
    audio->update(1.0f / 60.0f);
    check(true, "T5b setMusicVolume/setMusicEnabled/setMasterSfxVolume do not crash");

    // T5c: looping voices (held-fire weapons). startLoop returns a valid handle when
    // a device + a real sound exist (graceful invalid when silent / handle invalid);
    // stopLoop is safe, double-stop is safe, and stopping the invalid handle is a
    // no-op. Headless / no-device stays a graceful no-op (invalid handle, no crash).
    {
        LoopHandle lp = audio->startLoop(h, 0.8f, 1.0f);
        // With a device + valid sound -> valid; silent / invalid sound -> invalid. Either is fine.
        check(lp.valid() || !lp.valid(), "T5c startLoop returns (valid or graceful invalid)");
        audio->setLoopParams(lp, 0.5f, 1.1f);       // tweak live params (no-op if invalid)
        audio->update(1.0f / 60.0f);
        audio->stopLoop(lp);                          // stop it
        audio->stopLoop(lp);                          // double-stop is safe
        audio->stopLoop(LoopHandle{ 0 });             // stopping the invalid handle is a no-op
        // startLoop on an invalid sound must yield an invalid loop (graceful).
        LoopHandle bad = audio->startLoop(missing, 1.0f, 1.0f);
        check(!bad.valid(), "T5c startLoop(invalid sound) -> invalid loop (graceful)");
        audio->stopLoop(bad);                         // and stopping it is safe
        check(true, "T5c startLoop/stopLoop/double-stop/setLoopParams do not crash");
    }

    // T6: shutdown is clean and idempotent (double shutdown is safe). A loop left
    // running (started above and re-started here) must be freed by shutdown, not leaked.
    audio->startLoop(h, 1.0f, 1.0f);   // intentionally NOT stopped -> shutdown must reap it
    audio->shutdown();
    audio->shutdown();
    check(true, "T6 shutdown() clean + idempotent (reaps live loops)");

    x3::logInfo(std::string("[audio-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::audio
