// ============================================================================
// CLUB LISTEN MODE — implementation. See club_listen.h for the design.
//
// miniaudio note: the single-header implementation (MINIAUDIO_IMPLEMENTATION)
// lives ONCE in engine/audio/MiniaudioSystem.cpp. Here we include <miniaudio.h>
// for DECLARATIONS ONLY and link against that TU. The engine TU compiles the
// full WASAPI backend (it does NOT define MA_NO_DEVICE_IO / MA_NO_WASAPI), so
// ma_device_type_loopback capture is available in the linked implementation.
// ============================================================================

#include "club_listen.h"

#include "engine/core/x3_log.h"
#include "engine/core/IConsole.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace x3::club_listen {

namespace {
constexpr float kPi = 3.14159265358979f;

// One-pole low-pass smoothing coefficient for a cutoff `fc` at sample rate `fs`.
inline float onePoleA(float fc, float fs) {
    return 1.0f - std::exp(-2.0f * kPi * fc / fs);
}
}  // namespace

// ===========================================================================
// BeatDetector
// ===========================================================================
struct BeatDetector::Impl {
    int   fs = 48000;

    // --- audio-thread filter state (only touched in processBlock) ----------
    float dc   = 0.0f;   // ~5 Hz LP  -> DC/rumble tracker (removed from x)
    float lpK  = 0.0f;   // ~150 Hz LP -> kick/bass band
    float lpM  = 0.0f;   // ~2 kHz LP  -> boundary for the mid band
    float aDc  = 0.0f, aK = 0.0f, aM = 0.0f;

    // smoothed band envelopes (fast attack / slow release), pre-AGC
    float envBass = 0.0f, envMid = 0.0f, envHigh = 0.0f;
    // slow "loudness" follower per band for AGC normalisation
    float agcBass = 1e-4f, agcMid = 1e-4f, agcHigh = 1e-4f;

    // hop-energy accumulation for onset picking
    int   hopLen = 480;        // ~10 ms at 48 kHz
    int   hopFill = 0;
    float hopAccum = 0.0f;     // sum of kick-band^2 over the current hop
    float prevHopE = 0.0f;     // previous hop energy
    float avgHopE  = 1e-6f;    // adaptive running mean of hop energy (threshold base)

    // broadband RMS follower (silence detection)
    float rmsAccum = 0.0f;
    int   rmsFill  = 0;

    // onset timing
    uint64_t sampleClock = 0;         // total samples seen
    uint64_t lastOnsetSample = 0;
    bool     haveLastOnset = false;
    int      refractory = 0;          // samples until another onset may fire

    // inter-onset intervals (seconds) ring for the tempo median
    static constexpr int kIoiN = 12;
    float ioi[kIoiN] = {0};
    int   ioiCount = 0;
    int   ioiHead  = 0;

    // --- published atomics (audio thread writes, frame thread reads) --------
    std::atomic<float>    a_bpm{0.0f};
    std::atomic<uint32_t> a_onsetSeq{0};
    std::atomic<uint32_t> a_onsetCount{0};
    std::atomic<float>    a_bass{0.0f}, a_mid{0.0f}, a_high{0.0f};
    std::atomic<float>    a_rms{0.0f};

    // --- frame-thread-only state (advance) ---------------------------------
    float    phase = 0.0f;      // continuous beat position
    uint32_t seenSeq = 0;       // last onset seq consumed by the frame thread
    float    thumpEnv = 0.0f;   // onset-retriggered decay envelope
    float    lastGoodBpm = 120.0f;

    void configure(int sampleRate) {
        fs = sampleRate > 0 ? sampleRate : 48000;
        aDc = onePoleA(5.0f,   (float)fs);
        aK  = onePoleA(150.0f, (float)fs);
        aM  = onePoleA(2000.0f,(float)fs);
        hopLen = fs / 100;               // 10 ms hops
        if (hopLen < 1) hopLen = 1;
        refractoryReset = fs / 5;        // 200 ms min gap -> <=300 BPM
    }
    int refractoryReset = 9600;

    void reset() {
        dc = lpK = lpM = 0.0f;
        envBass = envMid = envHigh = 0.0f;
        agcBass = agcMid = agcHigh = 1e-4f;
        hopFill = 0; hopAccum = 0.0f; prevHopE = 0.0f; avgHopE = 1e-6f;
        rmsAccum = 0.0f; rmsFill = 0;
        sampleClock = 0; lastOnsetSample = 0; haveLastOnset = false; refractory = 0;
        ioiCount = 0; ioiHead = 0;
        a_bpm = 0.0f; a_onsetSeq = 0; a_onsetCount = 0;
        a_bass = a_mid = a_high = 0.0f; a_rms = 0.0f;
        phase = 0.0f; seenSeq = 0; thumpEnv = 0.0f; lastGoodBpm = 120.0f;
    }

    // median of the collected inter-onset intervals -> a BPM in [70,180]
    float estimateBpm() const {
        if (ioiCount < 3) return 0.0f;
        float tmp[kIoiN];
        for (int i = 0; i < ioiCount; ++i) tmp[i] = ioi[i];
        // insertion sort (tiny N)
        for (int i = 1; i < ioiCount; ++i) {
            float v = tmp[i]; int j = i - 1;
            while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; --j; }
            tmp[j + 1] = v;
        }
        float med = tmp[ioiCount / 2];
        if (med <= 1e-4f) return 0.0f;
        float bpm = 60.0f / med;
        // Fold into a musical range so half/double-time onset patterns agree.
        while (bpm < 70.0f)  bpm *= 2.0f;
        while (bpm > 180.0f) bpm *= 0.5f;
        return bpm;
    }

    void onOnset() {
        const uint64_t now = sampleClock;
        if (haveLastOnset) {
            const float dtSec = (float)(now - lastOnsetSample) / (float)fs;
            if (dtSec > 0.05f && dtSec < 2.0f) {   // plausible beat interval
                ioi[ioiHead] = dtSec;
                ioiHead = (ioiHead + 1) % kIoiN;
                if (ioiCount < kIoiN) ++ioiCount;
                a_bpm.store(estimateBpm(), std::memory_order_relaxed);
            }
        }
        lastOnsetSample = now;
        haveLastOnset = true;
        a_onsetCount.fetch_add(1, std::memory_order_relaxed);
        a_onsetSeq.fetch_add(1, std::memory_order_relaxed);
    }
};

BeatDetector::BeatDetector(int sampleRate) : m_(new Impl) {
    m_->configure(sampleRate);   // filter coeffs + hop/refractory lengths
    m_->reset();                 // zero the running state (leaves coeffs intact)
}
BeatDetector::~BeatDetector() { delete m_; }

void BeatDetector::reset() { const int fs = m_->fs; m_->reset(); m_->configure(fs); }

void BeatDetector::processBlock(const float* samples, size_t frameCount, int channels) {
    if (!samples || frameCount == 0 || channels < 1) return;
    Impl& d = *m_;
    const float invCh = 1.0f / (float)channels;

    for (size_t i = 0; i < frameCount; ++i) {
        // downmix to mono
        float x = 0.0f;
        const float* fr = samples + i * channels;
        for (int c = 0; c < channels; ++c) x += fr[c];
        x *= invCh;

        // DC/rumble removal
        d.dc += d.aDc * (x - d.dc);
        const float xc = x - d.dc;

        // band split via one-pole low-passes
        d.lpK += d.aK * (xc - d.lpK);          // <=150 Hz  (kick/bass)
        d.lpM += d.aM * (xc - d.lpM);          // <=2 kHz
        const float bass = d.lpK;
        const float mid  = d.lpM - d.lpK;      // 150..2000 Hz
        const float high = xc - d.lpM;         // >2 kHz

        // per-sample rectified envelopes (fast attack, slow release)
        auto follow = [](float& env, float mag) {
            if (mag > env) env += 0.30f * (mag - env);   // fast attack
            else           env += 0.002f * (mag - env);  // slow release
        };
        follow(d.envBass, std::fabs(bass));
        follow(d.envMid,  std::fabs(mid));
        follow(d.envHigh, std::fabs(high));

        // slow AGC follower per band (keeps published energies ~0..1)
        d.agcBass += 0.0005f * (std::fabs(bass) - d.agcBass);
        d.agcMid  += 0.0005f * (std::fabs(mid)  - d.agcMid);
        d.agcHigh += 0.0005f * (std::fabs(high) - d.agcHigh);

        // hop energy for onset picking (kick band power)
        d.hopAccum += bass * bass;
        if (d.refractory > 0) --d.refractory;

        // broadband RMS (silence detection) over ~50 ms
        d.rmsAccum += xc * xc;
        if (++d.rmsFill >= d.fs / 20) {
            const float rms = std::sqrt(d.rmsAccum / (float)d.rmsFill);
            d.a_rms.store(rms, std::memory_order_relaxed);
            d.rmsAccum = 0.0f; d.rmsFill = 0;
        }

        if (++d.hopFill >= d.hopLen) {
            const float e = d.hopAccum / (float)d.hopFill;   // mean kick power this hop
            // adaptive threshold: a rising hop that clears 1.6x the running mean
            // (and a real jump over the previous hop) is an onset.
            const bool rising = (e > d.prevHopE * 1.25f);
            const bool overAvg = (e > d.avgHopE * 1.6f);
            const bool loud    = (e > 1e-7f);
            if (rising && overAvg && loud && d.refractory == 0) {
                d.onOnset();
                d.refractory = d.refractoryReset;
            }
            // update running mean (slow), publish band energies
            d.avgHopE += 0.10f * (e - d.avgHopE);
            d.prevHopE = e;
            d.hopAccum = 0.0f; d.hopFill = 0;

            const float nb = d.envBass / (d.agcBass * 4.0f + 1e-4f);
            const float nm = d.envMid  / (d.agcMid  * 4.0f + 1e-4f);
            const float nh = d.envHigh / (d.agcHigh * 4.0f + 1e-4f);
            auto clamp01 = [](float v){ return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
            d.a_bass.store(clamp01(nb), std::memory_order_relaxed);
            d.a_mid.store (clamp01(nm), std::memory_order_relaxed);
            d.a_high.store(clamp01(nh), std::memory_order_relaxed);
        }

        ++d.sampleClock;
    }
}

BeatFrame BeatDetector::advance(float dt, float offsetSec, float gain) {
    Impl& d = *m_;
    BeatFrame f;

    float bpm = d.a_bpm.load(std::memory_order_relaxed);
    if (bpm > 40.0f && bpm < 220.0f) d.lastGoodBpm = bpm;
    bpm = d.lastGoodBpm;

    // advance the continuous phase at the detected tempo
    const float beatsPerSec = bpm / 60.0f;
    d.phase += dt * beatsPerSec;

    // PLL: on a fresh onset, nudge the fractional phase toward the beat grid so
    // the show re-locks to what is actually playing.
    const uint32_t seq = d.a_onsetSeq.load(std::memory_order_relaxed);
    bool beatNow = false;
    if (seq != d.seenSeq) {
        d.seenSeq = seq;
        beatNow = true;
        float frac = d.phase - std::floor(d.phase);
        float corr = (frac < 0.5f) ? -frac : (1.0f - frac);
        d.phase += corr * 0.20f;      // gentle lock (0 = ignore onsets, 1 = snap)
    }

    // thump: retrigger to 1 on a beat, exponential decay otherwise; blended with
    // the live bass energy so the subs/cones also pump between hard onsets.
    if (beatNow) d.thumpEnv = 1.0f;
    else         d.thumpEnv *= std::exp(-dt / 0.11f);
    const float bass = d.a_bass.load(std::memory_order_relaxed) * gain;
    const float mid  = d.a_mid.load(std::memory_order_relaxed)  * gain;
    const float high = d.a_high.load(std::memory_order_relaxed) * gain;
    auto clamp01 = [](float v){ return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const float thump = clamp01(std::max(d.thumpEnv, bass));

    f.bpm       = bpm;
    f.beatCount = d.phase + offsetSec * beatsPerSec;   // visual alignment offset
    f.thump     = thump;
    f.bass      = clamp01(bass);
    f.mid       = clamp01(mid);
    f.high      = clamp01(high);
    f.beatNow   = beatNow;
    f.active    = false;   // set by the module singleton from signalRms()
    return f;
}

float BeatDetector::estimatedBpm() const { return m_->a_bpm.load(std::memory_order_relaxed); }
int   BeatDetector::onsetCount()  const { return (int)m_->a_onsetCount.load(std::memory_order_relaxed); }
float BeatDetector::signalRms()   const { return m_->a_rms.load(std::memory_order_relaxed); }

// ===========================================================================
// Deterministic self-test (--test-listen). No audio device: synthesize a click
// track at a known BPM (60 Hz kick bursts on the beat + a low noise floor),
// push it through a BeatDetector, and assert the recovered onset count + tempo.
// ===========================================================================
bool runListenSelfTest() {
    const int   fs      = 48000;
    const float bpm     = 128.0f;                 // house tempo under test
    const float period  = 60.0f / bpm;            // seconds per beat (~0.469 s)
    const int   beats   = 20;
    const float dur     = beats * period;         // total signal length
    const int   nTotal  = (int)(dur * fs);

    // Build a mono click track: each beat is an ~80 ms 60 Hz sine burst with an
    // exponential decay (a stand-in kick), over a faint deterministic hum so the
    // detector must clear an adaptive threshold rather than pure silence.
    std::vector<float> buf((size_t)nTotal, 0.0f);
    uint32_t lcg = 0x1234567u;                    // deterministic dither
    for (int n = 0; n < nTotal; ++n) {
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = ((float)(lcg >> 9) / (float)0x7fffff - 1.0f) * 0.01f;
        buf[(size_t)n] = noise;
    }
    for (int b = 0; b < beats; ++b) {
        const int start = (int)(b * period * fs);
        const int len   = (int)(0.08f * fs);
        for (int k = 0; k < len && (start + k) < nTotal; ++k) {
            const float t = (float)k / fs;
            const float env = std::exp(-t * 35.0f);
            buf[(size_t)(start + k)] += 0.8f * env * std::sin(2.0f * kPi * 60.0f * t);
        }
    }

    // Stream the buffer through the detector in small blocks, advancing the
    // frame-thread phase in lockstep and counting the beat pulses it emits.
    BeatDetector det(fs);
    const int block = 512;
    int pulses = 0;
    for (int off = 0; off < nTotal; off += block) {
        const int n = std::min(block, nTotal - off);
        det.processBlock(buf.data() + off, (size_t)n, 1);
        BeatFrame f = det.advance((float)n / fs, 0.0f, 1.0f);
        if (f.beatNow) ++pulses;
    }

    const int   onsets  = det.onsetCount();
    const float estBpm  = det.estimatedBpm();

    x3::logInfo("[test-listen] synthetic click track: " + std::to_string(beats) +
                " beats @ " + std::to_string((int)bpm) + " BPM -> detected " +
                std::to_string(onsets) + " onsets, est BPM " +
                std::to_string(estBpm) + ", frame pulses " + std::to_string(pulses));

    // Tolerances: onsets within +/-1 of the beats (edge burst may be clipped),
    // tempo within +/-4 BPM, and the frame-thread pulse must have fired.
    const bool onsetsOk = std::abs(onsets - beats) <= 1;
    const bool bpmOk     = std::fabs(estBpm - bpm) <= 4.0f;
    const bool pulseOk   = pulses >= beats - 2 && pulses > 0;

    if (!onsetsOk) x3::logError("[test-listen] onset count out of tolerance");
    if (!bpmOk)    x3::logError("[test-listen] BPM estimate out of tolerance");
    if (!pulseOk)  x3::logError("[test-listen] beat pulse did not fire as expected");

    const bool pass = onsetsOk && bpmOk && pulseOk;
    x3::logInfo(std::string("[test-listen] ") + (pass ? "PASS" : "FAIL"));
    return pass;
}

}  // namespace x3::club_listen


// ===========================================================================
// Module singleton — real WASAPI loopback capture. Kept in its own translation
// section below the pure detector so the detector compiles/links even if the
// miniaudio header were ever unavailable to this TU.
// ===========================================================================
#include <miniaudio.h>

namespace x3::club_listen {
namespace {

constexpr float kSilenceRms = 1.0e-4f;   // below this the club falls back to its internal beat

struct Module {
    std::unique_ptr<BeatDetector> detector;
    ma_device       device{};
    bool            deviceOpen  = false;
    bool            deviceFailed = false;   // open failed once -> disabled for the session
    x3::con::IConsole* console  = nullptr;
    std::mutex      lifecycle;
};

Module& mod() { static Module m; return m; }

// miniaudio loopback callback (audio thread): captured system output arrives in
// pInput as interleaved f32 frames.
void dataCallback(ma_device* dev, void* /*pOutput*/, const void* pInput, ma_uint32 frameCount) {
    Module& m = mod();
    if (!m.detector || !pInput || frameCount == 0) return;
    m.detector->processBlock(reinterpret_cast<const float*>(pInput),
                             (size_t)frameCount, (int)dev->capture.channels);
}

// Open the loopback device once. Logs a single line on failure and disables the
// feature for the rest of the session (the club keeps its internal beat).
void startCapture() {
    Module& m = mod();
    std::lock_guard<std::mutex> lk(m.lifecycle);
    if (m.deviceOpen || m.deviceFailed) return;

    if (!m.detector) m.detector = std::make_unique<BeatDetector>(48000);

    ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
    cfg.capture.format   = ma_format_f32;
    cfg.capture.channels = 0;            // native channel count
    cfg.sampleRate       = 48000;
    cfg.dataCallback     = dataCallback;
    cfg.pUserData        = &m;

    if (ma_device_init(nullptr, &cfg, &m.device) != MA_SUCCESS) {
        x3::logWarn("[club-listen] WASAPI loopback device open FAILED — "
                    "Listen Mode disabled; club uses its internal beat.");
        m.deviceFailed = true;
        return;
    }
    // reconfigure the detector to the device's real sample rate
    m.detector = std::make_unique<BeatDetector>((int)m.device.sampleRate);
    if (ma_device_start(&m.device) != MA_SUCCESS) {
        x3::logWarn("[club-listen] loopback device start FAILED — Listen Mode disabled.");
        ma_device_uninit(&m.device);
        m.deviceFailed = true;
        return;
    }
    m.deviceOpen = true;
    x3::logInfo("[club-listen] WASAPI loopback capturing system audio @ " +
                std::to_string(m.device.sampleRate) + " Hz, " +
                std::to_string(m.device.capture.channels) + " ch — the club now "
                "dances to whatever is playing.");
}

}  // namespace

void registerCVars(x3::con::IConsole& console) {
    console.registerCVar("snd_listen", "0",
        "CLUB LISTEN MODE: 1 = the club light show rides the LIVE beat of whatever "
        "audio is playing on the PC (WASAPI loopback); 0 = fixed house tempo.");
    console.registerCVar("snd_listen_offset_ms", "0",
        "Listen Mode visual<->audio alignment offset in milliseconds (+ leads audio).");
    console.registerCVar("snd_listen_gain", "1.0",
        "Listen Mode band-energy / thump gain (scales how hard the show pumps).");
}

void bindConsole(x3::con::IConsole* console) { mod().console = console; }

bool sample(float dt, BeatFrame& out) {
    Module& m = mod();
    out = BeatFrame{};

    const bool enabled = m.console && m.console->getInt("snd_listen") != 0;
    if (!enabled) return false;
    if (m.deviceFailed) return false;

    if (!m.deviceOpen) startCapture();   // lazy open on first enable
    if (!m.deviceOpen || !m.detector) return false;

    const float offMs = m.console ? m.console->getFloat("snd_listen_offset_ms") : 0.0f;
    float gain        = m.console ? m.console->getFloat("snd_listen_gain") : 1.0f;
    if (!(gain > 0.0f)) gain = 1.0f;

    BeatFrame f = m.detector->advance(dt, offMs * 0.001f, gain);
    f.active = (m.detector->signalRms() > kSilenceRms);
    out = f;
    return f.active;
}

void shutdown() {
    Module& m = mod();
    std::lock_guard<std::mutex> lk(m.lifecycle);
    if (m.deviceOpen) { ma_device_uninit(&m.device); m.deviceOpen = false; }
    m.detector.reset();
}

}  // namespace x3::club_listen
