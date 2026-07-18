#pragma once
// CLUB JUKEBOX (feat/club-jukebox) — Tim's "Self Radio" for Club 1127.
//
// Drop your own MP3s/WAVs in a folder, and THE DEEP plays them — with the ENTIRE
// light show (sub-cone thumps, corner pulse lights, dance-tile breathe, OLED
// thump, dancers' bounce/sway) retuned to each track's BPM via
// Club1127World::setBeatGrid(). Personal-use music: nothing but a README and a
// tiny generated test tone ships in the repo (.gitignore covers real audio).
//
// FOLDERS scanned (union of all three; alphabetical by filename, or shuffled):
//   1. snd_clubmusic_dir cvar / configure(extraDir)  — an explicit library dir
//   2. <Documents>/X3Native/club_music               — the user-level library
//                                                      (Tim's MP3s never live in
//                                                      the repo)
//   3. <repo assets>/audio/club_music                — the committed folder
//                                                      (README + test tone)
//
// SIDECAR: each track may carry "<trackname>.json" beside it:
//   { "bpm": 128.0, "offset_s": 0.35 }     // offset_s optional (first-beat offset)
// With a sidecar the club's beat grid locks to that BPM (offset aligns beat 0 to
// the track's downbeat). No sidecar -> the default BPM (snd_clubmusic_bpm, 120).
// v1 is sidecar-or-default ONLY — no auto BPM detection (simple + reliable).
//
// PLAYBACK: streamed (MA_SOUND_FLAG_STREAM) through IAudioSystem::playMusic —
// the SAME single music channel the game's ambient bed uses, so the Settings
// music volume/enable controls apply unchanged. Auto-advance on track end
// (musicAtEnd), N / Shift+N next/prev in-club, missing/corrupt files skip with
// one log line (probeAudioFile), and an EMPTY folder means the club's built-in
// looping club_descent.wav behavior is byte-for-byte what it was: the jukebox
// simply never activates.
//
// Game/slice code only — engine/ stays pure (the two tiny IAudioSystem hooks,
// musicAtEnd + probeAudioFile, are interface-level and backend-graceful).

#include "club1127.h"
#include "engine/audio/IAudioSystem.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

struct JukeboxTrack {
    std::string path;         // absolute file path (.mp3 / .wav)
    std::string name;         // display name (filename without extension)
    float       bpm     = 0.0f;   // sidecar BPM; 0 = none -> use the default
    float       offsetS = 0.0f;   // sidecar first-beat offset (seconds)
    bool        sidecar = false;  // a sidecar JSON was found + parsed
    bool        bad     = false;  // failed the decode probe (skip; logged once)
};

class ClubJukebox {
public:
    struct Config {
        std::string extraDir;       // snd_clubmusic_dir ("" = none)
        bool        shuffle    = false;  // snd_clubmusic_shuffle
        float       defaultBpm = 120.0f; // snd_clubmusic_bpm (no-sidecar tracks)
        float       volume     = 0.75f;  // music-channel start volume
        uint32_t    shuffleSeed = 0;     // 0 = derive from clock (fresh order/boot)
    };

    void configure(const Config& cfg) { m_cfg = cfg; }
    const Config& config() const { return m_cfg; }

    // (Re)scan the folders into the playlist. Call on club entry — that IS the
    // hot-rescan (no live file-watch). Returns the number of tracks found.
    int scan();

    bool   empty() const { return m_tracks.empty(); }
    size_t count() const { return m_tracks.size(); }
    const std::vector<JukeboxTrack>& tracks() const { return m_tracks; }

    // Bind the live systems. `audio` may be null (silent/headless -> inert),
    // `club` may be null (no grid to retune; playback still works).
    void attach(x3::audio::IAudioSystem* audio, Club1127World* club) {
        m_audio = audio; m_club = club;
    }

    // Start playing the playlist from the top. False if the playlist is empty or
    // no audio system is attached (the caller keeps the built-in club loop).
    bool startPlayback();

    // Manual track control (DJ booth: N = next, Shift+N = previous). Wraps.
    void next() { if (playing()) step(+1); }
    void prev() { if (playing()) step(-1); }

    // Per-frame: auto-advance when the streamed track ends + toast timer.
    void update(float dt);

    // Stop the jukebox: music off, beat grid back to the house default tempo.
    void stopPlayback();

    bool playing() const { return m_playing && !m_tracks.empty(); }
    const JukeboxTrack* current() const {
        return playing() ? &m_tracks[m_cur] : nullptr;
    }

    // "Now Playing: <name>  (<bpm> BPM)" — shown for a few seconds on track
    // change. Draw bottom-center; a no-op when the toast has faded.
    void drawToast(x3::rhi::IRenderDevice& device,
                   const x3::rhi::FrameContext& frame) const;

    // ---- folder resolution (exposed for the self-test / README truth) ------
    static std::string repoMusicDir();   // <assets>/audio/club_music
    static std::string userMusicDir();   // <Documents>/X3Native/club_music
    static std::string sidecarPathFor(const std::string& trackPath);

    // Parse a sidecar JSON text -> bpm/offset. Returns false on malformed JSON
    // or a missing/nonpositive "bpm" (caller falls back to the default BPM).
    static bool parseSidecarText(const std::string& jsonText,
                                 float& outBpm, float& outOffsetS);

    // Scan ONE directory (test seam). Appends found tracks (sorted later).
    static void scanDirInto(const std::string& dir, std::vector<JukeboxTrack>& out);

    // Test seams (--test-jukebox): pin the playlist to exactly one temp dir
    // (bypassing the three-root union) + apply the canonical alphabetical sort.
    std::vector<JukeboxTrack>& tracksMutable() { return m_tracks; }
    void sortAlphabetical();

private:
    void step(int dir);          // advance cur by +-1 (wraps) + play
    bool playIndex(size_t idx);  // probe/skip-forward from idx; false if none playable
    void applyGridFor(const JukeboxTrack& t);

    Config                    m_cfg{};
    std::vector<JukeboxTrack> m_tracks;
    size_t                    m_cur = 0;
    bool                      m_playing = false;
    x3::audio::IAudioSystem*  m_audio = nullptr;
    Club1127World*            m_club  = nullptr;
    std::string               m_toast;         // "Now Playing: ..."
    float                     m_toastT = 0.0f; // seconds left on the toast
};

// Headless self-test (--test-jukebox): folder scan, sidecar parse, per-track
// beat-grid retune, empty-folder fallback, corrupt-file skip — tiny generated
// WAVs in a temp dir, no real music touched. Logs "jukebox: X/Y passed".
bool runJukeboxSelfTest();

} // namespace x3::game
