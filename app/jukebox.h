#pragma once
// ============================================================================
// CLUB JUKEBOX — Tim's personal-use "Self Radio" for Club 1127 (GTA-style).
//
// Benign personal-use feature: Tim drops his OWN music (MP3/WAV) into a folder,
// Club 1127 streams it, and the club's beat grid (subs / dance tiles / dancers /
// moving-head lights — one clock in Club1127World::update) rides each track's
// BPM. Nothing is bundled or redistributed; the audio files are .gitignored (only
// a README + one tiny generated test-tone sample ship).
//
// Folders (scanned NON-recursively, so a `samples/` subdir never auto-plays), in
// PRIORITY order — the FIRST copy of a given filename wins:
//   1) the `snd_clubmusic_dir` cvar dir         — explicit per-machine override
//   2) %USERPROFILE%/Documents/X3Native/club_music — the user library (real music,
//                                                 never enters the repo)
//   3) <assetRoot>/audio/club_music/            — committed fixture (README + one
//                                                 tiny generated test tone)
// A track present in several roots resolves to the HIGHEST-priority one: the
// user's real file beats the repo's test fixture.
//
// Per-track BPM: an optional sidecar JSON next to the track — `<track>.mp3.json`
// or `<track>.json` — of the form {"bpm": <float>} retunes the club. No sidecar =>
// the `snd_clubmusic_bpm` cvar (default 120). NO auto-detect (Tim tags his tracks).
//
// Playback rides the EXISTING real-music path: IAudioSystem::playMusic() (a
// streamed ma_sound — miniaudio decodes MP3 via its bundled dr_mp3, and WAV
// natively) on the MUSIC volume channel, so the Settings music volume scales it.
// Multi-track playlists auto-advance on track end (isMusicFinished); a single
// track loops seamlessly. Alphabetical by default, or shuffled via the
// `snd_clubmusic_shuffle` cvar. Empty folder => the host keeps the built-in
// club_descent track exactly as before (zero behavior change).
//
// Robust: a missing/corrupt file is skipped with ONE log line and the playlist
// advances; rescan() hot-reloads the folder (called on club re-entry).
//
// Clean-room: built from the C++ standard library + the project's own headers
// (IAudioSystem, Club1127World, asset_root, settings_io). No foreign source.
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>

namespace x3 { namespace audio { class IAudioSystem; } }

namespace x3::game {

class Club1127World;

class Jukebox {
public:
    struct Track {
        std::string path;              // absolute path to the audio file
        std::string name;              // display name (filename stem, no extension)
        std::string sortKey;           // lowercased name for stable alpha ordering
        float       bpm        = 120;  // resolved BPM (sidecar, else default cvar)
        float       offsetS    = 0.0f; // sidecar downbeat offset, seconds (F1)
        bool        hasSidecar = false;
    };

    // --- Configuration --------------------------------------------------------
    // Read the cvars (snd_clubmusic_dir / _shuffle / _bpm, each overridable by
    // the matching X3_SND_CLUBMUSIC_* env var), resolve the scan folders, and
    // (re)build the playlist. Idempotent + cheap: safe to call on every club
    // entry for a hot rescan. `vol`/`musicOn` come from the audio settings so the
    // jukebox respects the player's music volume + on/off.
    void rescan(float vol, bool musicOn);

    // Test / explicit hook: scan a specific set of folders with explicit config
    // (bypasses the cvar/settings file so the self-test is hermetic).
    void configure(const std::vector<std::string>& dirs, float defaultBpm,
                   bool shuffle, float vol, bool musicOn);

    // --- Playback -------------------------------------------------------------
    // Start the current track through the MUSIC channel and retune `club` to its
    // BPM. No-op (leaves the club's default tempo + the host's built-in track
    // alone) when the playlist is empty.
    void begin(x3::audio::IAudioSystem& audio, Club1127World& club);

    // Per-frame tick: counts down the "Now Playing" toast and, for a multi-track
    // playlist, auto-advances to the next track (retuning the club) when the
    // current one ends (or failed to start). Cheap; safe when empty.
    void update(float dt, x3::audio::IAudioSystem& audio, Club1127World& club);

    // Manual transport (N / Shift+N in the club). No-op when empty.
    void next(x3::audio::IAudioSystem& audio, Club1127World& club);
    void prev(x3::audio::IAudioSystem& audio, Club1127World& club);

    // --- Queries --------------------------------------------------------------
    bool        hasTracks()   const { return !m_tracks.empty(); }
    size_t      count()       const { return m_tracks.size(); }
    int         index()       const { return m_index; }
    const std::string& currentName() const;
    float       currentBpm()  const;
    float       currentOffsetS() const;   // downbeat offset of the current track
    bool        shuffle()     const { return m_shuffle; }
    float       defaultBpm()  const { return m_defaultBpm; }
    const std::vector<std::string>& scanDirs() const { return m_dirs; }
    const std::vector<Track>&       tracks()   const { return m_tracks; }

    // "Now Playing" toast: seconds remaining (>0 => the host should draw it) +
    // the line to draw. Refreshed on every track change.
    float              toastRemaining() const { return m_toast; }
    const std::string& toastText()      const { return m_toastText; }

private:
    void scan();                                        // (re)build m_tracks from m_dirs
    void playCurrent(x3::audio::IAudioSystem& audio, Club1127World& club);
    void armToast();

    std::vector<Track>       m_tracks;
    std::vector<std::string> m_dirs;          // resolved scan folders (repo, then user)
    int    m_index      = -1;                 // playing index (-1 = none)
    float  m_defaultBpm = 120.0f;             // snd_clubmusic_bpm
    bool   m_shuffle    = false;              // snd_clubmusic_shuffle
    float  m_vol        = 0.75f;              // music-channel volume for the club
    bool   m_musicOn    = true;               // Settings "Music ON/OFF"
    float  m_toast      = 0.0f;               // seconds remaining on the toast
    std::string m_toastText;
    std::string m_empty;                      // returned by currentName() when empty
};

// --- Sidecar helper (exposed for the self-test) -----------------------------
// Resolve the BPM for `audioPath`: probe `<audioPath>.json` then `<stem>.json`
// for {"bpm": <float>}. Returns true + sets outBpm on a valid sidecar; false
// (outBpm untouched) when there is no sidecar / it has no usable "bpm" number.
// Also resolves the OPTIONAL "offset_s" (seconds from the file's start to its first
// downbeat). outOffsetS is set to 0 when the key is absent or out of range, so a
// track without an offset always resets the phase rather than inheriting the
// previous track's.
bool parseBpmSidecar(const std::string& audioPath, float& outBpm, float& outOffsetS);

// Headless self-test for `--test-jukebox`: folder scan (alpha order + extension
// filter), sidecar parse, BPM retune applied to a Club1127World, empty-folder
// fallback (no retune), and a missing/corrupt-file skip — all on tiny WAVs
// generated in a temp dir (no real MP3s), plus the committed sample fixture.
// Logs "jukebox: X/Y passed" and returns true iff all pass.
bool runJukeboxSelfTest();

} // namespace x3::game
