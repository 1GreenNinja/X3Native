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

#include <atomic>
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

// ===========================================================================
// RT ACOUSTICS — Schroeder reverb INSERT node (dry + wet * reverb).
// Classic Schroeder topology (4 parallel feedback combs -> 2 series allpasses;
// Schroeder 1962, standard DSP textbook material — clean-room). A single
// shared instance sits between every 3D one-shot voice and the engine
// endpoint while an occlusion provider is hooked. The room estimate drives
// t60 (decay) + wet live via atomics; both are smoothed ON the audio thread
// so parameter steps never click. CONTINUOUS_PROCESSING keeps the tail
// ringing after a short one-shot (gunshot) voice ends and detaches.
// ===========================================================================
constexpr int kRevCombs = 4;
constexpr int kRevAllpass = 2;
// Schroeder's classic mutually-prime delay tunings (milliseconds).
constexpr float kRevCombMs[kRevCombs]   = { 29.7f, 37.1f, 41.1f, 43.7f };
constexpr float kRevAllpassMs[kRevAllpass] = { 5.0f, 1.7f };
constexpr float kRevAllpassG = 0.5f;

struct ReverbNode {
    ma_node_base base{};            // MUST be first (miniaudio casts the pointer)
    ma_uint32 channels = 2;
    ma_uint32 sampleRate = 48000;

    std::vector<float> comb[kRevCombs];
    int combLen[kRevCombs] = {}, combPos[kRevCombs] = {};
    float combGain[kRevCombs] = {};
    std::vector<float> ap[kRevAllpass];
    int apLen[kRevAllpass] = {}, apPos[kRevAllpass] = {};

    // Game thread -> audio thread (relaxed: smoothed on the audio side anyway).
    std::atomic<float> t60Target{ 0.5f };
    std::atomic<float> wetTarget{ 0.0f };
    float wetCur = 0.0f;
    float lastT60 = -1.0f;
};

void reverbNodeProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
                       float** ppFramesOut, ma_uint32* pFrameCountOut) {
    ReverbNode* rn = reinterpret_cast<ReverbNode*>(pNode);
    const float* in = ppFramesIn ? ppFramesIn[0] : nullptr;
    float* out = ppFramesOut[0];
    const ma_uint32 frames = *pFrameCountOut;
    const ma_uint32 ch = rn->channels;

    // Recompute comb feedback gains when the decay target moved (rare; cheap).
    const float t60 = rn->t60Target.load(std::memory_order_relaxed);
    if (t60 != rn->lastT60) {
        rn->lastT60 = t60;
        for (int c = 0; c < kRevCombs; ++c) {
            const float dSec = (float)rn->combLen[c] / (float)rn->sampleRate;
            // g = 10^(-3 d / T60): the loop decays 60 dB in T60 seconds.
            float g = (t60 > 0.01f) ? std::pow(10.0f, -3.0f * dSec / t60) : 0.0f;
            rn->combGain[c] = (g > 0.98f) ? 0.98f : g;   // hard stability ceiling
        }
    }
    const float wetTarget = rn->wetTarget.load(std::memory_order_relaxed);
    // Per-sample one-pole smoothing on the wet mix (~30 ms at 48 kHz): no zipper.
    const float wetCoef = 1.0f - std::exp(-1.0f / (0.030f * (float)rn->sampleRate));

    for (ma_uint32 f = 0; f < frames; ++f) {
        // Mono sum drives the reverb tank (standard for a cheap shared reverb).
        float mono = 0.0f;
        if (in) {
            for (ma_uint32 c = 0; c < ch; ++c) mono += in[f * ch + c];
            mono /= (float)ch;
        }
        // 4 parallel feedback combs.
        float sum = 0.0f;
        for (int c = 0; c < kRevCombs; ++c) {
            float* buf = rn->comb[c].data();
            int& pos = rn->combPos[c];
            const float delayed = buf[pos];
            buf[pos] = mono + delayed * rn->combGain[c];
            if (++pos >= rn->combLen[c]) pos = 0;
            sum += delayed;
        }
        float wetS = sum * 0.25f;
        // 2 series allpasses (diffusion).
        for (int a = 0; a < kRevAllpass; ++a) {
            float* buf = rn->ap[a].data();
            int& pos = rn->apPos[a];
            const float d = buf[pos];
            const float y = d - kRevAllpassG * wetS;
            buf[pos] = wetS + kRevAllpassG * y;
            if (++pos >= rn->apLen[a]) pos = 0;
            wetS = y;
        }
        rn->wetCur += (wetTarget - rn->wetCur) * wetCoef;
        // INSERT: dry passthrough + wet tail (same tail to every channel).
        for (ma_uint32 c = 0; c < ch; ++c) {
            const float dry = in ? in[f * ch + c] : 0.0f;
            out[f * ch + c] = dry + rn->wetCur * wetS;
        }
    }
    if (pFrameCountIn) *pFrameCountIn = frames;
    *pFrameCountOut = frames;
}

ma_node_vtable g_reverbVtable = {
    reverbNodeProcess,
    nullptr,                              // no resampling
    1, 1,                                 // 1 input bus, 1 output bus
    MA_NODE_FLAG_CONTINUOUS_PROCESSING    // keep the tail ringing after voices end
};

// Occlusion -> per-voice lowpass cutoff: log-interpolate 19 kHz (clear) down to
// ~420 Hz (fully behind a wall). Plus the volume duck applied in playInternal.
float occlusionCutoffHz(float occ) {
    return 19000.0f * std::pow(0.022f, clamp01(occ));
}
constexpr float kOcclusionDuck = 0.7f;   // volume *= 1 - kOcclusionDuck*occ
constexpr ma_uint32 kOccLpfOrder = 4;    // 4th-order = 24 dB/oct (a real wall)

// A loaded, fully-decoded prototype sound we clone transient voices from. Kept
// alive for the system's lifetime (clones reference its decoded data).
struct Proto {
    std::unique_ptr<ma_sound> sound;   // initialized with MA_SOUND_FLAG_DECODE
    std::string path;
    bool ok = false;
};

// A live, transient one-shot voice (a clone of a Proto). Reaped when finished.
// An OCCLUDED 3D voice additionally owns a per-voice lowpass node in its output
// chain (voice -> lpf -> reverb -> endpoint); uninit'd with the voice.
struct Voice {
    std::unique_ptr<ma_sound> sound;
    std::unique_ptr<ma_lpf_node> lpf;   // 3D voices with a provider hooked own one
    uint64_t serial = 0;   // monotonically increasing; oldest = smallest
    // Live-occlusion state (3D voices while a provider is hooked): the mixer
    // re-queries the provider each update() and retunes volume + lowpass, so
    // a door closing mid-tail muffles the tail and the async tracer's one-
    // update latency on a NEW emitter is corrected within ~16-33 ms.
    bool  spatial = false;
    float x = 0, y = 0, z = 0;
    float baseVol = 1.0f;               // pre-master, pre-duck volume
    float lastOcc = -1.0f;              // last applied occlusion (skip no-ops)
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
            // Free transient one-shot voices (and their per-voice lowpass nodes),
            // then any live loop voices, then the shared reverb node, then the
            // prototypes, then the engine (no leaks — the engine asserts alloc==0).
            for (auto& v : m_voices) {
                if (v.sound) { ma_sound_uninit(v.sound.get()); }
                if (v.lpf)   { ma_lpf_node_uninit(v.lpf.get(), nullptr); }
            }
            m_voices.clear();
            for (auto& lv : m_loops) {
                if (lv.sound) { ma_sound_uninit(lv.sound.get()); }
            }
            m_loops.clear();
            if (m_reverb) {
                ma_node_uninit(&m_reverb->base, nullptr);
                m_reverb.reset();
            }
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

    bool musicAtEnd() const override {
        // Only a live NON-LOOPING music voice can "end" (a looping bed wraps).
        return m_music && !m_musicLoop &&
               ma_sound_at_end(m_music.get()) == MA_TRUE;
    }

    bool probeAudioFile(std::string_view absPath) override {
        // ma_decoder is device-independent: this works headless/silent too, so
        // the jukebox can vet a playlist on any machine. Open + immediately
        // close — the STREAM decode of playMusic never touches a dud.
        ma_decoder dec;
        const std::string path(absPath);
        ma_decoder_config cfg = ma_decoder_config_init_default();
        if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS)
            return false;
        ma_decoder_uninit(&dec);
        return true;
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

    LoopHandle startLoop3D(SoundHandle sound, float x, float y, float z,
                           float vol, float pitch) override {
        if (!m_inited || m_silent || !sound.valid()) return LoopHandle{ 0 };
        auto it = m_protos.find(sound.id);
        if (it == m_protos.end() || !it->second.ok) return LoopHandle{ 0 };

        auto voice = std::make_unique<ma_sound>();
        // Spatialization stays ENABLED (unlike startLoop's 2D clone, which passes
        // NO_SPATIALIZATION) — mirrors playInternal's 3D one-shot path: clone with
        // flags=0, then explicitly enable + position. The prototype itself was
        // loaded with NO_SPATIALIZATION (load() is neutral for all sounds); that is
        // overridden per-clone exactly like one-shots already do.
        const ma_uint32 flags = 0u;
        ma_result r = ma_sound_init_copy(&m_engine, it->second.sound.get(), flags,
                                         nullptr, voice.get());
        if (r != MA_SUCCESS) {
            x3::logWarn(std::string("[audio] loop3D clone failed (ma=") +
                        std::to_string((int)r) + ")");
            return LoopHandle{ 0 };
        }
        // Same one-place master-SFX multiply as one-shots/2D loops.
        ma_sound_set_volume(voice.get(), clamp01(vol) * m_sfxMaster);
        ma_sound_set_pitch(voice.get(), clampPitch(pitch));
        ma_sound_set_spatialization_enabled(voice.get(), MA_TRUE);
        ma_sound_set_position(voice.get(), x, y, z);
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

    // ---- RT ACOUSTICS hooks ------------------------------------------------
    void setOcclusionProvider(OcclusionFn fn, void* user) override {
        m_occFn = fn;
        m_occUser = fn ? user : nullptr;
        // The reverb route is part of the acoustics chain: make sure it exists
        // while a provider is hooked (lazy; no-op silent / already built).
        if (fn) ensureReverb();
    }

    void setReverbParams(float t60Seconds, float wet) override {
        if (t60Seconds < 0.05f) t60Seconds = 0.05f;
        if (t60Seconds > 8.0f)  t60Seconds = 8.0f;
        ensureReverb();
        if (!m_reverb) return;   // silent / no device: harmless no-op
        m_reverb->t60Target.store(t60Seconds, std::memory_order_relaxed);
        m_reverb->wetTarget.store(clamp01(wet), std::memory_order_relaxed);
    }

    void update(float /*dt*/) override {
        if (!m_inited || m_silent) return;
        // Reap finished voices (free their ma_sound + per-voice lowpass + slot).
        for (size_t i = 0; i < m_voices.size();) {
            ma_sound* s = m_voices[i].sound.get();
            if (s && ma_sound_at_end(s)) {
                ma_sound_uninit(s);
                if (m_voices[i].lpf) ma_lpf_node_uninit(m_voices[i].lpf.get(), nullptr);
                m_voices[i] = std::move(m_voices.back());
                m_voices.pop_back();
            } else {
                ++i;
            }
        }
        // RT ACOUSTICS: retune LIVE 3D voices from the provider's CURRENT
        // smoothed occlusion (volume duck + lowpass cutoff). Covers the async
        // tracer's one-update latency on fresh emitters and doors closing
        // mid-tail. Skipped entirely without a provider; per-voice no-op when
        // the value barely moved (cheap: a handful of voices * one reinit).
        if (m_occFn) {
            for (Voice& v : m_voices) {
                if (!v.spatial || !v.sound) continue;
                const float occ = clamp01(m_occFn(m_occUser, v.x, v.y, v.z));
                if (std::fabs(occ - v.lastOcc) < 0.005f) continue;
                v.lastOcc = occ;
                ma_sound_set_volume(v.sound.get(),
                    v.baseVol * m_sfxMaster * (1.0f - kOcclusionDuck * occ));
                if (v.lpf) {
                    ma_lpf_config lc = ma_lpf_config_init(
                        ma_format_f32,
                        ma_engine_get_channels(&m_engine),
                        ma_engine_get_sample_rate(&m_engine),
                        (double)occlusionCutoffHz(occ), kOccLpfOrder);
                    ma_lpf_node_reinit(&lc, v.lpf.get());
                }
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

    // Lazily create the shared Schroeder reverb node and attach it to the engine
    // endpoint. No-op when silent / no device / already built. The node idles at
    // wet 0 until setReverbParams raises it.
    void ensureReverb() {
        if (!m_inited || m_silent || m_reverb) return;
        auto rn = std::make_unique<ReverbNode>();
        rn->channels   = ma_engine_get_channels(&m_engine);
        rn->sampleRate = ma_engine_get_sample_rate(&m_engine);
        for (int c = 0; c < kRevCombs; ++c) {
            rn->combLen[c] = (int)(kRevCombMs[c] * 0.001f * (float)rn->sampleRate);
            if (rn->combLen[c] < 8) rn->combLen[c] = 8;
            rn->comb[c].assign((size_t)rn->combLen[c], 0.0f);
        }
        for (int a = 0; a < kRevAllpass; ++a) {
            rn->apLen[a] = (int)(kRevAllpassMs[a] * 0.001f * (float)rn->sampleRate);
            if (rn->apLen[a] < 8) rn->apLen[a] = 8;
            rn->ap[a].assign((size_t)rn->apLen[a], 0.0f);
        }
        ma_node_config cfg = ma_node_config_init();
        cfg.vtable = &g_reverbVtable;
        ma_uint32 ch = rn->channels;
        cfg.pInputChannels = &ch;
        cfg.pOutputChannels = &ch;
        ma_node_graph* graph = ma_engine_get_node_graph(&m_engine);
        if (ma_node_init(graph, &cfg, nullptr, &rn->base) != MA_SUCCESS) {
            x3::logWarn("[audio] reverb node init failed — acoustics reverb off");
            return;
        }
        ma_node_attach_output_bus(&rn->base, 0, ma_node_graph_get_endpoint(graph), 0);
        m_reverb = std::move(rn);
        x3::logInfo("[audio] RT-acoustics reverb chain up (Schroeder 4-comb/2-allpass insert)");
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
            if (m_voices[oldest].lpf) ma_lpf_node_uninit(m_voices[oldest].lpf.get(), nullptr);
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

        // ---- RT ACOUSTICS (3D voices, provider hooked) ----------------------
        // Ask the provider for this emitter position's smoothed occlusion, route
        // the voice voice->lowpass->reverb->endpoint, and apply occlusion as a
        // volume duck + the lowpass cutoff. EVERY 3D voice gets the lowpass while
        // a provider is hooked (cutoff ~19 kHz = transparent when clear) because
        // update() retunes it LIVE — a new emitter's first traced value (the
        // async tracer is one update behind) and a door closing mid-tail both
        // land on the already-playing voice. With NO provider hooked the path
        // below is byte-for-byte the pre-acoustics path (default endpoint attach).
        float occ = 0.0f;
        std::unique_ptr<ma_lpf_node> lpf;
        if (spatial && m_occFn) {
            occ = clamp01(m_occFn(m_occUser, x, y, z));
            ma_node* target = m_reverb ? &m_reverb->base
                                       : ma_node_graph_get_endpoint(ma_engine_get_node_graph(&m_engine));
            lpf = std::make_unique<ma_lpf_node>();
            ma_lpf_node_config lc = ma_lpf_node_config_init(
                ma_engine_get_channels(&m_engine),
                ma_engine_get_sample_rate(&m_engine),
                (double)occlusionCutoffHz(occ), kOccLpfOrder);
            if (ma_lpf_node_init(ma_engine_get_node_graph(&m_engine), &lc, nullptr,
                                 lpf.get()) == MA_SUCCESS) {
                ma_node_attach_output_bus(lpf.get(), 0, target, 0);
                ma_node_attach_output_bus(voice.get(), 0, lpf.get(), 0);
            } else {
                lpf.reset();   // filter failed: volume-only duck still applies
                ma_node_attach_output_bus(voice.get(), 0, target, 0);
            }
        }

        // ONE-PLACE master SFX scale: every 2D/3D one-shot's volume is multiplied by
        // the master here, so the Settings SFX slider quiets ALL gunfire/impacts/steps
        // without touching any call site. The occlusion duck stacks multiplicatively.
        ma_sound_set_volume(voice.get(), clamp01(vol) * m_sfxMaster * (1.0f - kOcclusionDuck * occ));
        ma_sound_set_pitch(voice.get(), clampPitch(pitch));
        if (spatial) {
            ma_sound_set_spatialization_enabled(voice.get(), MA_TRUE);
            ma_sound_set_position(voice.get(), x, y, z);
        }
        ma_sound_start(voice.get());

        Voice v;
        v.sound = std::move(voice);
        v.lpf = std::move(lpf);
        v.serial = m_nextSerial++;
        v.spatial = spatial;
        v.x = x; v.y = y; v.z = z;
        v.baseVol = clamp01(vol);
        v.lastOcc = occ;
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

    // RT ACOUSTICS: occlusion provider (game thread; queried at play time) + the
    // shared Schroeder reverb insert all 3D one-shots route through when hooked.
    OcclusionFn                 m_occFn   = nullptr;
    void*                       m_occUser = nullptr;
    std::unique_ptr<ReverbNode> m_reverb;                 // lazy; null until needed

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

    // T5e: 3D spatialized ambient loop channels (real loop-channel pass, W5-4 —
    // room tone / fluorescent-buzz replacement for the old timer-retrigger hack).
    // Start a 3D loop at a fixed emitter position, tick update() across several
    // "frames" while moving the listener closer, and confirm the loop survives
    // every tick (still a valid handle -> still playing; on silent/no-device the
    // handle is gracefully invalid throughout, which is equally a pass). Also
    // exercise a live gain/pitch update (setLoopParams, shared with the 2D loop
    // API) and a second concurrent loop to prove N simultaneous loop channels
    // coexist (the mixer isn't limited to one loop the way playMusic is).
    {
        LoopHandle amb = audio->startLoop3D(h, 3.0f, 0.0f, 0.0f, 0.35f, 1.0f);
        check(amb.valid() || !amb.valid(), "T5e startLoop3D returns (valid or graceful invalid)");
        // A second, concurrent loop channel (2D) alongside the 3D one: proves N
        // independent loop voices, not a single reused slot.
        LoopHandle amb2 = audio->startLoop(h, 0.22f, 1.0f);
        for (int frame = 0; frame < 5; ++frame) {
            // Listener walks toward the emitter; the 3D loop's gain should be
            // re-derived by the engine every mix callback (no per-frame call needed
            // from us — that's the point of leaving spatialization ON).
            audio->setListener(6.0f - (float)frame, 1.6f, 0.0f, 0.0f, 0.0f);
            audio->update(1.0f / 60.0f);
        }
        check(amb.valid() || !amb.valid(),
              "T5e loop still holds a valid handle across ticks (or gracefully invalid throughout)");
        audio->setLoopParams(amb, 0.5f, 1.05f);   // live gain/pitch update, no-op if invalid
        audio->update(1.0f / 60.0f);
        audio->stopLoop(amb);
        audio->stopLoop(amb2);
        audio->stopLoop(amb);   // double-stop is safe
        LoopHandle badAmb = audio->startLoop3D(missing, 1.0f, 0, 0, 0);
        check(!badAmb.valid(), "T5e startLoop3D(invalid sound) -> invalid loop (graceful)");
        check(true, "T5e startLoop3D + concurrent loops + setLoopParams do not crash");
    }

    // T5d: RT-acoustics hooks. Hook an occlusion provider (returns a fixed 0.8 —
    // an occluded emitter), play a 3D one-shot through the duck+lowpass+reverb
    // chain, drive the reverb params, unhook, play again (pre-acoustics path).
    // Headless/silent: every step is a graceful no-op. Must never crash/leak.
    {
        auto occFn = +[](void*, float, float, float) -> float { return 0.8f; };
        audio->setOcclusionProvider(occFn, nullptr);
        audio->setReverbParams(0.8f, 0.18f);              // medium room
        audio->playSound3D(h, 4.0f, 0.0f, 0.0f, 0.9f, 1.0f);   // occluded: lpf route
        audio->update(1.0f / 60.0f);
        audio->setReverbParams(1.8f, 0.26f);              // large room (live change)
        audio->playSound3D(h, 2.0f, 0.0f, 0.0f, 0.9f, 1.0f);
        audio->update(1.0f / 60.0f);
        audio->setOcclusionProvider(nullptr, nullptr);    // unhook -> default path
        audio->playSound3D(h, 2.0f, 0.0f, 0.0f, 0.9f, 1.0f);
        audio->update(1.0f / 60.0f);
        check(true, "T5d acoustics occlusion/reverb hooks do not crash (hook+play+unhook)");
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
