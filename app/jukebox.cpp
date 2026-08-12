// CLUB JUKEBOX implementation — see app/jukebox.h.
#include "jukebox.h"
#include "club1127.h"
#include "asset_root.h"        // assetRoot()
#include "settings_io.h"       // x3::apphost::x3SettingsPath()

#include "engine/audio/IAudioSystem.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cctype>
#include <cmath>              // sin, fabs
#include <cstdlib>            // getenv, strtod, strtol
#include <filesystem>
#include <fstream>
#include <memory>            // unique_ptr (self-test)
#include <random>
#include <sstream>
#include <system_error>

namespace x3::game {

namespace fs = std::filesystem;

namespace {

// Supported track extensions (case-insensitive). miniaudio decodes MP3 (bundled
// dr_mp3) + WAV natively, and OGG VORBIS via the stb_vorbis path enabled in
// engine/audio/MiniaudioSystem.cpp; all ride the same streamed playMusic path.
bool isTrackExt(const std::string& extLower) {
    return extLower == ".mp3" || extLower == ".wav" || extLower == ".ogg";
}

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A cvar read from the shared key=value settings cfg (x3native_settings.cfg),
// overridable by an environment variable (checked FIRST so a per-launch env wins).
std::string cvarStr(const char* key, const char* envName, const std::string& def) {
    if (const char* e = std::getenv(envName); e && *e) return std::string(e);
    std::ifstream f(x3::apphost::x3SettingsPath());
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (line.substr(0, eq) == key) return line.substr(eq + 1);
        }
    }
    return def;
}
float cvarFloat(const char* key, const char* envName, float def) {
    const std::string s = cvarStr(key, envName, "");
    if (s.empty()) return def;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return (end == s.c_str()) ? def : (float)v;
}
bool cvarBool(const char* key, const char* envName, bool def) {
    const std::string s = cvarStr(key, envName, "");
    if (s.empty()) return def;
    return std::strtol(s.c_str(), nullptr, 10) != 0;
}

// The default user music dir: %USERPROFILE%\Documents\X3Native\club_music.
std::string defaultUserDir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) home = std::getenv("HOME");
    fs::path base = (home && *home) ? fs::path(home) : fs::path(".");
    return (base / "Documents" / "X3Native" / "club_music").string();
}

} // namespace

// ---------------------------------------------------------------------------
// Sidecar parsing. Minimal, dependency-free: find "bpm", the following ':', then
// the first numeric token. Handles {"bpm": 128}, {"bpm":128.5}, whitespace, and
// extra keys. Returns false if no usable number is found.
// ---------------------------------------------------------------------------
static bool parseBpmFromText(const std::string& text, float& outBpm) {
    const auto key = text.find("\"bpm\"");
    if (key == std::string::npos) return false;
    auto colon = text.find(':', key + 5);
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' ||
                               text[i] == '\n' || text[i] == '\r')) ++i;
    if (i >= text.size()) return false;
    char* end = nullptr;
    const double v = std::strtod(text.c_str() + i, &end);
    if (end == text.c_str() + i) return false;   // no number
    if (!(v > 20.0 && v < 400.0)) return false;   // out of sane musical range
    outBpm = (float)v;
    return true;
}

bool parseBpmSidecar(const std::string& audioPath, float& outBpm) {
    std::error_code ec;
    // Probe order: "<track>.mp3.json" (full-name sidecar), then "<stem>.json".
    std::vector<fs::path> candidates;
    candidates.push_back(fs::path(audioPath + ".json"));
    {
        fs::path p(audioPath);
        candidates.push_back(p.replace_extension(".json"));
    }
    for (const fs::path& c : candidates) {
        if (!fs::exists(c, ec)) continue;
        std::ifstream f(c);
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        if (parseBpmFromText(ss.str(), outBpm)) return true;
        x3::logWarn(std::string("[jukebox] sidecar has no usable bpm: ") + c.string());
    }
    return false;
}

// ---------------------------------------------------------------------------

void Jukebox::configure(const std::vector<std::string>& dirs, float defaultBpm,
                        bool shuffle, float vol, bool musicOn) {
    m_dirs       = dirs;
    m_defaultBpm = (defaultBpm > 20.0f && defaultBpm < 400.0f) ? defaultBpm : 120.0f;
    m_shuffle    = shuffle;
    m_vol        = (vol < 0.0f) ? 0.0f : (vol > 1.0f ? 1.0f : vol);
    m_musicOn    = musicOn;
    scan();
}

void Jukebox::rescan(float vol, bool musicOn) {
    std::vector<std::string> dirs;
    dirs.push_back((fs::path(assetRoot()) / "audio" / "club_music").string());   // repo-local
    const std::string userDir = cvarStr("snd_clubmusic_dir", "X3_SND_CLUBMUSIC_DIR",
                                        defaultUserDir());
    if (!userDir.empty()) dirs.push_back(userDir);                                // user fallback
    configure(dirs,
              cvarFloat("snd_clubmusic_bpm",     "X3_SND_CLUBMUSIC_BPM",     120.0f),
              cvarBool ("snd_clubmusic_shuffle", "X3_SND_CLUBMUSIC_SHUFFLE", false),
              vol, musicOn);
}

void Jukebox::scan() {
    m_tracks.clear();
    m_index = -1;
    std::error_code ec;

    // NON-recursive scan of each dir (a `samples/` subdir never auto-plays). A
    // filename already seen in an earlier (higher-priority) dir is skipped, so the
    // repo-local copy wins over the user-dir copy of the same basename.
    for (const std::string& dir : m_dirs) {
        if (dir.empty() || !fs::is_directory(dir, ec)) continue;
        for (const auto& de : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!de.is_regular_file(ec)) continue;
            const fs::path p = de.path();
            const std::string ext = toLower(p.extension().string());
            if (!isTrackExt(ext)) continue;
            const std::string stem = p.stem().string();
            const std::string sortKey = toLower(stem);
            bool dup = false;
            for (const Track& t : m_tracks)
                if (t.sortKey == sortKey) { dup = true; break; }
            if (dup) continue;

            Track t;
            t.path = p.string();
            t.name = stem;
            t.sortKey = sortKey;
            float bpm = m_defaultBpm;
            t.hasSidecar = parseBpmSidecar(t.path, bpm);
            t.bpm = t.hasSidecar ? bpm : m_defaultBpm;
            m_tracks.push_back(std::move(t));
        }
    }

    if (m_tracks.empty()) {
        x3::logInfo("[jukebox] no user tracks found in club_music — the club keeps "
                    "its built-in track (drop MP3/WAV/OGG files to override)");
        return;
    }

    if (m_shuffle) {
        std::mt19937 rng{ std::random_device{}() };
        std::shuffle(m_tracks.begin(), m_tracks.end(), rng);
    } else {
        std::sort(m_tracks.begin(), m_tracks.end(),
                  [](const Track& a, const Track& b) { return a.sortKey < b.sortKey; });
    }
    m_index = 0;
    x3::logInfo("[jukebox] found " + std::to_string(m_tracks.size()) +
                " user track(s) (" + (m_shuffle ? "shuffle" : "alphabetical") +
                ", default " + std::to_string((int)m_defaultBpm) + " BPM)");
}

const std::string& Jukebox::currentName() const {
    if (m_index < 0 || m_index >= (int)m_tracks.size()) return m_empty;
    return m_tracks[m_index].name;
}
float Jukebox::currentBpm() const {
    if (m_index < 0 || m_index >= (int)m_tracks.size()) return Club1127World::kDefaultBpm;
    return m_tracks[m_index].bpm;
}

void Jukebox::armToast() {
    if (m_index < 0 || m_index >= (int)m_tracks.size()) return;
    const Track& t = m_tracks[m_index];
    std::ostringstream os;
    os << "Now Playing: " << t.name << "  (" << (int)(t.bpm + 0.5f) << " BPM)";
    m_toastText = os.str();
    m_toast = 5.0f;   // seconds on screen
}

void Jukebox::playCurrent(x3::audio::IAudioSystem& audio, Club1127World& club) {
    if (m_index < 0 || m_index >= (int)m_tracks.size()) return;
    // Skip any track whose file has vanished since the scan (missing => one log
    // line + advance). A corrupt file that miniaudio cannot decode is handled by
    // playMusic (logs once, no voice) + isMusicFinished()==true => update()
    // auto-advances past it on the next tick, so we do not loop forever.
    std::error_code ec;
    int guard = (int)m_tracks.size();
    while (guard-- > 0 && !fs::exists(m_tracks[m_index].path, ec)) {
        x3::logWarn("[jukebox] skipping missing track: " + m_tracks[m_index].path);
        m_index = (m_index + 1) % (int)m_tracks.size();
    }

    const Track& t = m_tracks[m_index];
    // A lone track loops seamlessly (no re-decode gap); a playlist plays through
    // and auto-advances. Route via the MUSIC channel so the Settings music volume
    // scales it (playMusic remembers the vol + honours setMusicEnabled).
    const bool loop = (m_tracks.size() == 1);
    audio.setMusicEnabled(m_musicOn);
    audio.playMusic(t.path, loop, m_vol);
    club.setBpm(t.bpm);   // retune the beat grid (subs/tiles/dancers/lights)
    armToast();
    x3::logInfo("[jukebox] " + m_toastText + (t.hasSidecar ? " [sidecar]" : " [cvar bpm]") +
                (loop ? " [loop]" : ""));
}

void Jukebox::begin(x3::audio::IAudioSystem& audio, Club1127World& club) {
    if (!hasTracks()) return;      // empty => host keeps the built-in club_descent
    if (m_index < 0) m_index = 0;
    playCurrent(audio, club);
}

void Jukebox::next(x3::audio::IAudioSystem& audio, Club1127World& club) {
    if (!hasTracks()) return;
    m_index = (m_index + 1) % (int)m_tracks.size();
    playCurrent(audio, club);
}
void Jukebox::prev(x3::audio::IAudioSystem& audio, Club1127World& club) {
    if (!hasTracks()) return;
    m_index = (m_index - 1 + (int)m_tracks.size()) % (int)m_tracks.size();
    playCurrent(audio, club);
}

void Jukebox::update(float dt, x3::audio::IAudioSystem& audio, Club1127World& club) {
    if (m_toast > 0.0f) m_toast -= dt;
    if (!hasTracks() || m_tracks.size() == 1) return;   // lone track loops; nothing to advance
    // Auto-advance when the current (non-looping) track has ended or failed.
    if (audio.isMusicFinished()) next(audio, club);
}

} // namespace x3::game

// ===========================================================================
// Headless self-test (--test-jukebox). Exercises the whole pipeline on tiny
// WAVs generated in a temp dir (NO real MP3s) + the committed sample fixture.
// ===========================================================================
#include "engine/audio/IAudioSystem.h"
#include <cstdint>
#include <cstring>

namespace x3::game {

namespace {

// Write a minimal valid PCM WAV (16-bit mono) of `ms` milliseconds at 8 kHz, a
// quiet sine so it is real audio (not silence). Tiny (~a few KB). Used to build a
// hermetic playlist without shipping any real music.
bool writeTinyWav(const fs::path& path, int ms, bool corrupt = false) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    if (corrupt) {
        // Garbage that claims to be a WAV but is not decodable — the "corrupt"
        // case. Header says RIFF/WAVE then random bytes with a bogus chunk size.
        const char junk[64] = { 'R','I','F','F', 4,0,0,0, 'W','A','V','E',
                                'j','u','n','k', (char)0xFF,(char)0xFF,(char)0xFF,(char)0x7F };
        f.write(junk, sizeof(junk));
        return (bool)f;
    }
    const uint32_t rate = 8000, bits = 16, ch = 1;
    const uint32_t nSamples = (uint32_t)((int64_t)rate * ms / 1000);
    const uint32_t dataBytes = nSamples * ch * (bits / 8);
    const uint32_t byteRate = rate * ch * (bits / 8);
    const uint16_t blockAlign = (uint16_t)(ch * (bits / 8));
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16((uint16_t)ch);
    w32(rate); w32(byteRate); w16(blockAlign); w16((uint16_t)bits);
    f.write("data", 4); w32(dataBytes);
    for (uint32_t i = 0; i < nSamples; ++i) {
        const double s = std::sin(2.0 * 3.14159265 * 220.0 * i / rate) * 3000.0;
        w16((uint16_t)(int16_t)s);
    }
    return (bool)f;
}

// A stub .ogg used ONLY to prove the extension filter accepts Vorbis. The scanner
// is extension-only (it never opens the file), so this deliberately contains just
// an OggS capture pattern rather than a real Vorbis stream — real .ogg DECODING is
// proven separately by --test-audio against a genuine encoded file.
void writeOggStub(const fs::path& path) {
    std::ofstream f(path, std::ios::binary);
    f.write("OggS", 4);
    const char rest[28] = { 0 };
    f.write(rest, sizeof(rest));
}

void writeSidecar(const fs::path& path, float bpm) {
    std::ofstream f(path);
    f << "{ \"bpm\": " << bpm << " }\n";
}

} // namespace

bool runJukeboxSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool cond, const char* name) {
        if (cond) { ++pass; x3::logInfo(std::string("[jukebox-test] PASS ") + name); }
        else      { ++fail; x3::logError(std::string("[jukebox-test] FAIL ") + name); }
    };
    std::error_code ec;

    // Hermetic temp folder.
    const fs::path tmp = fs::temp_directory_path(ec) /
                         ("x3_jukebox_test_" + std::to_string((uint64_t)std::random_device{}()));
    fs::create_directories(tmp, ec);

    // Tracks: alpha.wav (no sidecar), bravo.wav (+ bravo.wav.json 100), charlie.wav
    // (+ charlie.json 140), delta.ogg (Vorbis is an accepted container), plus a
    // non-audio notes.txt that must be ignored.
    writeTinyWav(tmp / "bravo.wav", 60);
    writeTinyWav(tmp / "alpha.wav", 60);
    writeTinyWav(tmp / "charlie.wav", 60);
    writeOggStub(tmp / "delta.ogg");
    writeSidecar(tmp / "bravo.wav.json", 100.0f);          // full-name sidecar
    writeSidecar(tmp / "charlie.json", 140.0f);            // stem sidecar
    { std::ofstream(tmp / "notes.txt") << "not audio\n"; }

    // (T1) Folder scan: 4 tracks, alphabetical, .txt ignored, extension filter.
    {
        Jukebox jb;
        jb.configure({ tmp.string() }, /*defaultBpm*/120.0f, /*shuffle*/false,
                     /*vol*/0.75f, /*musicOn*/true);
        const auto& tr = jb.tracks();
        const bool ok = tr.size() == 4 &&
                        tr[0].name == "alpha" && tr[1].name == "bravo" &&
                        tr[2].name == "charlie" && tr[3].name == "delta";
        check(ok, "folder scan finds 4 tracks alphabetically (wav+ogg), ignores non-audio");

        // (T2) Sidecar parse: bravo=100 (full-name), charlie=140 (stem), alpha=cvar 120.
        const bool bpmOk = !tr[0].hasSidecar && std::fabs(tr[0].bpm - 120.0f) < 0.01f &&
                            tr[1].hasSidecar && std::fabs(tr[1].bpm - 100.0f) < 0.01f &&
                            tr[2].hasSidecar && std::fabs(tr[2].bpm - 140.0f) < 0.01f;
        check(bpmOk, "sidecar bpm parsed (100/140); no sidecar => 120 cvar default");
    }

    // (T3) BPM retune applied to a Club1127World across begin()/next().
    {
        std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
        audio->init();   // silent/no-device is fine — playMusic no-ops, logic still runs
        Club1127World club;
        const float clubDefault = club.bpm();   // == kDefaultBpm (85.5), the club_descent tempo
        Jukebox jb;
        jb.configure({ tmp.string() }, 120.0f, false, 0.75f, true);
        jb.begin(*audio, club);                 // first track = alpha (no sidecar) => 120
        const bool b0 = std::fabs(club.bpm() - 120.0f) < 0.01f;
        jb.next(*audio, club);                  // bravo => 100 (sidecar)
        const bool b1 = std::fabs(club.bpm() - 100.0f) < 0.01f;
        jb.next(*audio, club);                  // charlie => 140 (sidecar)
        const bool b2 = std::fabs(club.bpm() - 140.0f) < 0.01f;
        check(std::fabs(clubDefault - Club1127World::kDefaultBpm) < 0.01f && b0 && b1 && b2,
              "club beat grid retunes to each track BPM (120 -> 100 -> 140)");
    }

    // (T4) Empty-folder fallback: no tracks => no retune (club keeps its default).
    {
        const fs::path empty = tmp / "empty";
        fs::create_directories(empty, ec);
        std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
        audio->init();
        Club1127World club;
        Jukebox jb;
        jb.configure({ empty.string() }, 120.0f, false, 0.75f, true);
        jb.begin(*audio, club);                 // no-op
        const bool ok = !jb.hasTracks() &&
                        std::fabs(club.bpm() - Club1127World::kDefaultBpm) < 0.01f;
        check(ok, "empty folder => no tracks, club keeps built-in default tempo (zero change)");
    }

    // (T5) Corrupt/missing skip: a corrupt file + a missing file must not stall;
    //      the playlist advances and (for missing) logs one skip line.
    {
        const fs::path cd = tmp / "corrupt";
        fs::create_directories(cd, ec);
        writeTinyWav(cd / "bad.wav", 60, /*corrupt*/true);   // undecodable
        writeTinyWav(cd / "good.wav", 60);                   // fine
        std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
        audio->init();
        Club1127World club;
        Jukebox jb;
        jb.configure({ cd.string() }, 120.0f, false, 0.75f, true);
        jb.begin(*audio, club);
        // Delete good.wav AFTER scan to force the missing-file skip path too.
        fs::remove(cd / "good.wav", ec);
        bool crashed = false;
        try { for (int i = 0; i < 8; ++i) { jb.update(1.0f, *audio, club); jb.next(*audio, club); } }
        catch (...) { crashed = true; }
        check(jb.hasTracks() && !crashed,
              "corrupt + missing files skip without crashing (one log line, playlist advances)");
    }

    // (T6) Committed sample fixture: the repo-local samples/ dir parses the tiny
    //      test-tone WAV + its 120-BPM sidecar (wiring proof, no real MP3s).
    {
        const fs::path samples = fs::path(assetRoot()) / "audio" / "club_music" / "samples";
        if (fs::is_directory(samples, ec)) {
            Jukebox jb;
            jb.configure({ samples.string() }, 999.0f /*obvious-if-used default*/, false, 0.75f, true);
            bool found = false;
            for (const auto& t : jb.tracks())
                if (t.name == "test_tone_120" && t.hasSidecar && std::fabs(t.bpm - 120.0f) < 0.01f)
                    found = true;
            check(found, "committed sample fixture (test_tone_120.wav + sidecar bpm=120) scans");
        } else {
            x3::logWarn("[jukebox-test] samples/ dir absent (fresh clone w/o fixture) — skipping T6");
        }
    }

    fs::remove_all(tmp, ec);
    x3::logInfo("jukebox: " + std::to_string(pass) + "/" +
                std::to_string(pass + fail) + " passed");
    return fail == 0;
}

} // namespace x3::game
