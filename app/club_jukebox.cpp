// CLUB JUKEBOX — see club_jukebox.h for the design doc. Game/slice code only.
#include "club_jukebox.h"
#include "asset_root.h"     // assetRoot() — the repo-local committed-assets dir
#include "json_mini.h"      // jmini — the shared tiny JSON DOM (sidecar parse)

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <system_error>

namespace x3::game {

namespace fs = std::filesystem;

namespace {

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

std::string lowerName(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

constexpr float kToastSeconds = 4.0f;

} // namespace

// ---------------------------------------------------------------------------
// Folder resolution
// ---------------------------------------------------------------------------
std::string ClubJukebox::repoMusicDir() {
    return (fs::path(assetRoot()) / "audio" / "club_music").string();
}

std::string ClubJukebox::userMusicDir() {
    // <Documents>/X3Native/club_music — the personal library that never has to
    // live in the repo. USERPROFILE is authoritative on Windows; a missing env
    // (odd service context) just disables the user dir.
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) home = std::getenv("HOME");
    if (!home || !*home) return {};
    return (fs::path(home) / "Documents" / "X3Native" / "club_music").string();
}

std::string ClubJukebox::sidecarPathFor(const std::string& trackPath) {
    return fs::path(trackPath).replace_extension(".json").string();
}

// ---------------------------------------------------------------------------
// Sidecar parse
// ---------------------------------------------------------------------------
bool ClubJukebox::parseSidecarText(const std::string& jsonText,
                                   float& outBpm, float& outOffsetS) {
    jmini::JReader r(jsonText);
    jmini::JVal v = r.parse();
    if (!r.ok || v.t != jmini::JVal::Obj) return false;
    const float bpm = v.fnum("bpm", 0.0f);
    if (!(bpm > 0.0f)) return false;   // bpm is REQUIRED and must be positive
    outBpm     = bpm;
    outOffsetS = v.fnum("offset_s", 0.0f);
    return true;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------
void ClubJukebox::scanDirInto(const std::string& dir, std::vector<JukeboxTrack>& out) {
    if (dir.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file(ec)) continue;
        const fs::path& p = de.path();
        const std::string ext = lowerExt(p);
        if (ext != ".mp3" && ext != ".wav") continue;
        JukeboxTrack t;
        t.path = p.string();
        t.name = p.stem().string();
        // Optional sidecar: "<trackname>.json" beside the track.
        std::ifstream sf(sidecarPathFor(t.path));
        if (sf) {
            std::ostringstream ss; ss << sf.rdbuf();
            if (parseSidecarText(ss.str(), t.bpm, t.offsetS)) {
                t.sidecar = true;
            } else {
                x3::logWarn("[jukebox] sidecar unreadable (need {\"bpm\": <num>}), "
                            "using default BPM: " + sidecarPathFor(t.path));
            }
        }
        out.push_back(std::move(t));
    }
}

int ClubJukebox::scan() {
    m_tracks.clear();
    m_cur = 0;
    // Union of the three roots, highest priority first (the cvar-provided dir,
    // the user Documents library, the committed repo folder). Duplicate FILE
    // NAMES across roots keep the higher-priority root's copy.
    std::vector<JukeboxTrack> found;
    scanDirInto(m_cfg.extraDir, found);
    scanDirInto(userMusicDir(), found);
    scanDirInto(repoMusicDir(), found);
    std::vector<std::string> seen;
    for (auto& t : found) {
        const std::string key = lowerName(fs::path(t.path).filename().string());
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        m_tracks.push_back(std::move(t));
    }
    // Alphabetical by display name (case-insensitive), stable across machines.
    sortAlphabetical();
    if (m_cfg.shuffle && m_tracks.size() > 1) {
        uint32_t seed = m_cfg.shuffleSeed;
        if (seed == 0)
            seed = (uint32_t)std::chrono::steady_clock::now()
                       .time_since_epoch().count();
        std::mt19937 rng(seed);
        std::shuffle(m_tracks.begin(), m_tracks.end(), rng);
    }
    if (!m_tracks.empty())
        x3::logInfo("[jukebox] scan: " + std::to_string(m_tracks.size()) +
                    " track(s) (" + std::string(m_cfg.shuffle ? "shuffled" : "alphabetical") + ")");
    return (int)m_tracks.size();
}

void ClubJukebox::sortAlphabetical() {
    std::sort(m_tracks.begin(), m_tracks.end(),
              [](const JukeboxTrack& a, const JukeboxTrack& b) {
                  return lowerName(a.name) < lowerName(b.name);
              });
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------
void ClubJukebox::applyGridFor(const JukeboxTrack& t) {
    if (!m_club) return;
    const float bpm = t.bpm > 0.0f ? t.bpm : m_cfg.defaultBpm;
    m_club->setBeatGrid(bpm, t.offsetS);
}

bool ClubJukebox::playIndex(size_t idx) {
    if (m_tracks.empty() || !m_audio) return false;
    // Probe forward from idx; a missing/corrupt file is skipped with ONE log
    // line (marked so a later wrap doesn't re-log it) — bounded by the count.
    for (size_t n = 0; n < m_tracks.size(); ++n) {
        const size_t i = (idx + n) % m_tracks.size();
        JukeboxTrack& t = m_tracks[i];
        if (t.bad) continue;
        if (!m_audio->probeAudioFile(t.path)) {
            t.bad = true;
            x3::logWarn("[jukebox] skipping unplayable track: " + t.path);
            continue;
        }
        m_cur = i;
        // STREAMED through the single music channel (the elevator/ambient-bed
        // path): loop=false so musicAtEnd() gives us the auto-advance edge.
        m_audio->playMusic(t.path, /*loop*/false, m_cfg.volume);
        applyGridFor(t);
        const float gridBpm = t.bpm > 0.0f ? t.bpm : m_cfg.defaultBpm;
        char bpmTxt[32];
        std::snprintf(bpmTxt, sizeof(bpmTxt), "%.4g", gridBpm);
        m_toast  = "Now Playing: " + t.name + "  [" + bpmTxt + " BPM" +
                   (t.sidecar ? "]" : ", default]");
        m_toastT = kToastSeconds;
        x3::logInfo("[jukebox] now playing '" + t.name + "' (" + bpmTxt +
                    " BPM grid" + (t.sidecar ? ", sidecar" : ", default") +
                    ") — " + t.path);
        return true;
    }
    x3::logWarn("[jukebox] no playable tracks in the playlist");
    return false;
}

bool ClubJukebox::startPlayback() {
    m_playing = false;
    if (m_tracks.empty() || !m_audio) return false;
    m_playing = playIndex(0);
    return m_playing;
}

void ClubJukebox::step(int dir) {
    if (m_tracks.empty()) return;
    const size_t n = m_tracks.size();
    size_t idx = (m_cur + (size_t)((dir > 0) ? 1 : n - 1)) % n;
    if (dir < 0) {
        // prev: walk BACKWARD over bad tracks so Shift+N lands on the previous
        // playable one (playIndex itself probes forward).
        for (size_t k = 0; k < n && m_tracks[idx].bad; ++k)
            idx = (idx + n - 1) % n;
    }
    m_playing = playIndex(idx);
}

void ClubJukebox::update(float dt) {
    if (m_toastT > 0.0f) m_toastT -= dt;
    if (!m_playing || !m_audio) return;
    // Auto-advance: the streamed non-looping voice reached its end.
    if (m_audio->musicAtEnd()) step(+1);
}

void ClubJukebox::stopPlayback() {
    if (m_audio && m_playing) m_audio->stopMusic();
    m_playing = false;
    m_toastT  = 0.0f;
    // The house default grid comes back with the built-in track.
    if (m_club) m_club->setBeatGrid(Club1127World::kDefaultBpm, 0.0f);
}

// ---------------------------------------------------------------------------
// Toast ("Now Playing" HUD readout, bottom-center, fades out)
// ---------------------------------------------------------------------------
void ClubJukebox::drawToast(x3::rhi::IRenderDevice& device,
                            const x3::rhi::FrameContext& frame) const {
    if (m_toastT <= 0.0f || m_toast.empty()) return;
    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    if (w == 0 || h == 0) return;
    const float a  = std::min(1.0f, m_toastT);            // last second fades
    const float px = 15.0f;
    const float tw = device.textAdvance(x3::rhi::FontRole::HudMono,
                                        m_toast.c_str(), px);
    const float x = ((float)w - tw) * 0.5f;
    const float y = (float)h - 110.0f;
    const float plate[4]  = { 0.0f, 0.0f, 0.0f, 0.45f * a };
    const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.85f * a };
    const float neon[4]   = { 0.55f, 0.95f, 1.0f, 0.95f * a };   // club cyan
    device.drawHudQuad(frame, x - 12.0f, y - 7.0f, tw + 24.0f, px + 14.0f, plate);
    device.drawHudTextF(frame, x3::rhi::FontRole::HudMono, m_toast.c_str(),
                        x + 1.5f, y + 1.5f, px, shadow);
    device.drawHudTextF(frame, x3::rhi::FontRole::HudMono, m_toast.c_str(),
                        x, y, px, neon);
}

// ===========================================================================
// SELF-TEST (--test-jukebox) — headless; tiny generated WAVs in a temp dir.
// ===========================================================================
namespace {

// Minimal valid 16-bit mono PCM WAV (a short sine) for the scan/probe tests.
bool writeTinyWav(const std::string& path, int sampleRate = 8000,
                  int samples = 800) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const int dataBytes = samples * 2;
    auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1);
    u32((uint32_t)sampleRate); u32((uint32_t)sampleRate * 2); u16(2); u16(16);
    f.write("data", 4); u32((uint32_t)dataBytes);
    for (int i = 0; i < samples; ++i) {
        const int16_t s = (int16_t)(12000.0 * std::sin(2.0 * 3.14159265358979 *
                                                       440.0 * i / sampleRate));
        f.write((const char*)&s, 2);
    }
    return f.good();
}

} // namespace

bool runJukeboxSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; }
        else    { x3::logError(std::string("jukebox FAIL: ") + what); }
        return ok;
    };

    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) /
        ("x3_jukebox_test_" + std::to_string(
            (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp, ec);

    // ---- Fixture: 3 valid tiny WAVs + 1 corrupt "mp3" + 1 sidecar ----------
    writeTinyWav((tmp / "a_track.wav").string());
    writeTinyWav((tmp / "b_track.wav").string());
    writeTinyWav((tmp / "c_track.wav").string());
    { std::ofstream bad(tmp / "broken.mp3", std::ios::binary);
      bad << "this is not audio at all"; }
    { std::ofstream sc(tmp / "a_track.json");
      sc << "{ \"bpm\": 140.0, \"offset_s\": 0.25 }"; }

    // ---- T1: folder scan finds the files, alphabetical order ---------------
    ClubJukebox jb;
    ClubJukebox::Config cfg;
    cfg.extraDir   = tmp.string();
    cfg.defaultBpm = 120.0f;
    jb.configure(cfg);
    // NOTE: scan() also unions the user/repo dirs; assert the temp tracks are
    // present in order rather than an exact count (a dev machine may have a
    // library). scanDirInto is the pure per-dir seam.
    std::vector<JukeboxTrack> one;
    ClubJukebox::scanDirInto(tmp.string(), one);
    check(one.size() == 4, "T1 scanDirInto finds 4 files (3 wav + 1 mp3)");
    jb.scan();
    check(jb.count() >= 4, "T1b scan() unions the temp dir in");

    // ---- T2: sidecar parse -------------------------------------------------
    const JukeboxTrack* aTrack = nullptr;
    for (const auto& t : jb.tracks()) if (t.name == "a_track") aTrack = &t;
    check(aTrack && aTrack->sidecar && std::fabs(aTrack->bpm - 140.0f) < 1e-3f &&
              std::fabs(aTrack->offsetS - 0.25f) < 1e-3f,
          "T2 sidecar parsed (bpm 140 / offset 0.25)");
    float b = 0, o = 0;
    check(!ClubJukebox::parseSidecarText("{ nope", b, o),   "T2b malformed sidecar rejected");
    check(!ClubJukebox::parseSidecarText("{\"offset_s\":1}", b, o),
          "T2c sidecar without bpm rejected");
    check(ClubJukebox::parseSidecarText("{\"bpm\":171.5}", b, o) &&
              std::fabs(b - 171.5f) < 1e-3f && o == 0.0f,
          "T2d minimal sidecar ok (offset defaults 0)");

    // ---- T3: BPM grid retune on the CLUB (the machinery that rides it) -----
    Club1127World club;   // unbuilt is fine — the grid is plain state
    club.setBeatGrid(140.0f, 0.25f);
    check(std::fabs(club.beatBpm() - 140.0f) < 1e-3f, "T3 setBeatGrid applies 140");
    club.setBeatGrid(9999.0f);
    check(std::fabs(club.beatBpm() - 240.0f) < 1e-3f, "T3b bpm clamped high (240)");
    club.setBeatGrid(0.0f);
    check(std::fabs(club.beatBpm() - Club1127World::kDefaultBpm) < 1e-3f,
          "T3c bpm 0 -> house default");

    // ---- T4: playback start retunes the grid (headless audio is graceful) --
    std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
    const bool audioUp = audio && audio->init();
    check(audioUp, "T4 audio system init (silent/no-device counts as up)");
    // A jukebox pinned to JUST the temp dir (bypass the union for determinism).
    ClubJukebox jb2;
    jb2.configure(cfg);
    ClubJukebox::scanDirInto(tmp.string(), jb2.tracksMutable());
    jb2.sortAlphabetical();
    jb2.attach(audio.get(), &club);
    check(jb2.startPlayback(), "T4b startPlayback with valid tracks");
    check(jb2.current() && jb2.current()->name == "a_track",
          "T4c starts at the alphabetical head");
    check(std::fabs(club.beatBpm() - 140.0f) < 1e-3f,
          "T4d grid retuned to the sidecar BPM on play");

    // ---- T5: corrupt-file skip + next/prev wrap ----------------------------
    jb2.next();   // b_track
    check(jb2.current() && jb2.current()->name == "b_track", "T5 next -> b_track");
    jb2.next();   // broken.mp3 fails the probe -> skips to c_track
    check(jb2.current() && jb2.current()->name == "c_track",
          "T5b corrupt file skipped to c_track");
    check(std::fabs(club.beatBpm() - 120.0f) < 1e-3f,
          "T5c no-sidecar track uses the default BPM (120)");
    jb2.next();   // wraps to a_track
    check(jb2.current() && jb2.current()->name == "a_track", "T5d wrap to head");
    jb2.prev();   // back to c_track (skipping broken backward)
    check(jb2.current() && jb2.current()->name == "c_track",
          "T5e prev skips the corrupt file backward");

    // ---- T6: all-corrupt playlist refuses to start -------------------------
    const fs::path tmpBad = tmp / "only_bad";
    fs::create_directories(tmpBad, ec);
    { std::ofstream bad(tmpBad / "junk.mp3", std::ios::binary); bad << "junk"; }
    ClubJukebox jb3;
    ClubJukebox::Config cfg3 = cfg; cfg3.extraDir = tmpBad.string();
    jb3.configure(cfg3);
    ClubJukebox::scanDirInto(tmpBad.string(), jb3.tracksMutable());
    jb3.attach(audio.get(), &club);
    check(!jb3.startPlayback(), "T6 all-corrupt playlist does not start");

    // ---- T7: EMPTY folder -> jukebox inert (built-in club loop untouched) --
    const fs::path tmpEmpty = tmp / "empty";
    fs::create_directories(tmpEmpty, ec);
    ClubJukebox jb4;
    ClubJukebox::Config cfg4 = cfg; cfg4.extraDir = tmpEmpty.string();
    jb4.configure(cfg4);
    ClubJukebox::scanDirInto(tmpEmpty.string(), jb4.tracksMutable());
    check(jb4.empty() && !jb4.startPlayback(),
          "T7 empty folder -> inert (host keeps the default club loop)");

    // ---- T8: stopPlayback restores the house grid --------------------------
    jb2.stopPlayback();
    check(std::fabs(club.beatBpm() - Club1127World::kDefaultBpm) < 1e-3f,
          "T8 stopPlayback restores the default beat grid");

    if (audio) audio->shutdown();
    fs::remove_all(tmp, ec);

    x3::logInfo("jukebox: " + std::to_string(pass) + "/" + std::to_string(total) +
                " passed");
    return pass == total;
}

} // namespace x3::game
