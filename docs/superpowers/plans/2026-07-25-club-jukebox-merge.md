# Club 1127 Jukebox Merge — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fold the good ideas from the local `ClubJukebox` into the canonical `Jukebox` module so Club 1127's beat grid locks to each track's true tempo *and downbeat*, scans the user's real music library, and shuts down cleanly.

**Architecture:** `app/jukebox.{h,cpp}` stays the single jukebox module. Five features (F1–F5) are folded in. F1 needs a new phase parameter on `Club1127World`'s beat clock; F3 needs one small backend-graceful hook on `IAudioSystem`. Everything else is contained in `jukebox.cpp`.

**Tech Stack:** C++20, CMake (`windows-vs2026` preset), miniaudio via `IAudioSystem`, headless self-test through `--test-jukebox`.

---

## Environment

**Worktree:** `D:\GameDev\x3-jukebox-merge` (branch `feat/club-jukebox-merge`)

**Build:**
```powershell
cmake --build --preset windows-vs2026 --config Release
```

**Test:**
```powershell
.\build\bin\Release\X3Engine.exe --test-jukebox
```
Exit code 0 = all passed. Final line reads `jukebox: X/Y passed`.

**CLAUDE.md rules that apply here:**
- Bounded runs only. **Never** `--smoketest`.
- Check `Get-Process X3Engine` before any engine launch; if one is running that isn't yours, build only.
- Commit locally. **Do not push** — the session lead reviews and pushes.
- `--test-jukebox` is headless and needs no GPU/audio device; it is safe to run while the owner plays.

## Scope change from the spec

Two corrections found while reading the real source. Both are recorded here and must be reflected in the spec when this lands.

- **F6 (`drawToast`) is DROPPED.** `app/world_hosts/host_club.cpp:316-323` already renders the toast from `toastRemaining()`/`toastText()`. Moving rendering into `Jukebox` would couple a game module to `IRenderDevice` for no user-visible gain, and spec #3's screen system supersedes it. YAGNI.
- **F3 touches the engine.** Remote's `IAudioSystem` has no `probeAudioFile`; it needs adding as a default-`true` virtual so backends opt in.

## File structure

| File | Responsibility | Change |
|---|---|---|
| `app/club1127.h` | Club world state + beat grid | Add `setBeatGrid`, `beatOffsetS()`, `m_beatOffsetS` |
| `app/club1127.cpp` | Beat clock | One line: subtract the phase offset |
| `app/jukebox.h` | Jukebox API | `Track::offsetS`, `Config`, `stopPlayback`, `currentOffsetS`, new sidecar signature |
| `app/jukebox.cpp` | Scan / playback / tests | F1–F5 + self-test cases |
| `engine/audio/IAudioSystem.h` | Audio interface | Add `probeAudioFile` (default `true`) |
| `engine/audio/MiniaudioSystem.cpp` | miniaudio backend | Implement the probe |
| `app/world_hosts/host_club.cpp` | Club host | Call `stopPlayback` on exit |
| `assets/audio/club_music/README.txt` | User docs | Document `offset_s` + root precedence |

---

## Task 1: Beat-grid phase on the club (F1a)

**Files:**
- Modify: `app/club1127.h:175-177`
- Modify: `app/club1127.cpp:1950`

- [ ] **Step 1: Write the failing test**

Add to `runJukeboxSelfTest()` in `app/jukebox.cpp`, immediately before the `fs::remove_all(tmp, ec);` line:

```cpp
    // (T3b) Beat-grid PHASE: setBeatGrid stores the downbeat offset; setBpm alone
    //       leaves it at zero; out-of-range offsets are rejected.
    {
        Club1127World club;
        club.setBeatGrid(128.0f, 0.35f);
        const bool a = std::fabs(club.bpm() - 128.0f) < 0.01f &&
                       std::fabs(club.beatOffsetS() - 0.35f) < 0.0001f;
        club.setBeatGrid(100.0f, 999.0f);          // absurd offset => clamped to 0
        const bool b = std::fabs(club.beatOffsetS()) < 0.0001f;
        Club1127World club2;
        const bool c = std::fabs(club2.beatOffsetS()) < 0.0001f;   // defaults to 0
        check(a && b && c, "club beat grid stores a downbeat phase offset");
    }
```

- [ ] **Step 2: Run to verify it fails**

```powershell
cmake --build --preset windows-vs2026 --config Release
```
Expected: **compile error** — `'setBeatGrid': is not a member of 'x3::game::Club1127World'`.

- [ ] **Step 3: Implement**

In `app/club1127.h`, replace lines 175-177:

```cpp
    static constexpr float kDefaultBpm = 85.5f;   // matches club_descent.wav
    void  setBpm(float bpm) { if (bpm > 20.0f && bpm < 400.0f) m_bpm = bpm; }
    float bpm() const { return m_bpm; }
```

with:

```cpp
    static constexpr float kDefaultBpm = 85.5f;   // matches club_descent.wav
    void  setBpm(float bpm) { if (bpm > 20.0f && bpm < 400.0f) m_bpm = bpm; }
    float bpm() const { return m_bpm; }

    // Beat-grid PHASE (Club Jukebox sidecar "offset_s"): seconds from the track's
    // start to its FIRST DOWNBEAT. The beat clock subtracts this so beat 0 lands on
    // the music's actual downbeat instead of on level-load time. Without it the grid
    // runs at the right tempo but the wrong phase — the subs punch off-beat.
    // Out-of-range values are ignored (treated as 0), matching setBpm's guard style.
    void  setBeatGrid(float bpm, float offsetS) {
        setBpm(bpm);
        m_beatOffsetS = (offsetS > -10.0f && offsetS < 10.0f) ? offsetS : 0.0f;
    }
    float beatOffsetS() const { return m_beatOffsetS; }
```

In the private members of the same header, immediately after the `m_bpm` member (line ~278):

```cpp
    float                                         m_bpm = kDefaultBpm;
    // Downbeat phase offset in seconds; see setBeatGrid(). 0 = beat 0 at t=0.
    float                                         m_beatOffsetS = 0.0f;
```

In `app/club1127.cpp`, replace line 1950:

```cpp
    const float beatCount = t * beatHz;                    // absolute beat position
```

with:

```cpp
    // Phase-shifted by the track's downbeat offset (Club Jukebox sidecar
    // "offset_s"), so beat 0 lands on the music's first downbeat, not on t=0.
    const float beatCount = (t - m_beatOffsetS) * beatHz;  // absolute beat position
```

- [ ] **Step 4: Run to verify it passes**

```powershell
cmake --build --preset windows-vs2026 --config Release
.\build\bin\Release\X3Engine.exe --test-jukebox
```
Expected: `PASS club beat grid stores a downbeat phase offset`, exit code 0.

- [ ] **Step 5: Commit**

```powershell
git add app/club1127.h app/club1127.cpp app/jukebox.cpp
git commit -m "feat(club1127): beat-grid downbeat phase offset (F1a)"
```

---

## Task 2: Sidecar `offset_s` → beat grid (F1b)

**Files:**
- Modify: `app/jukebox.h` (Track, sidecar signature, `currentOffsetS`)
- Modify: `app/jukebox.cpp:81-117` (parsing), `:170-172` (scan), `:234` (apply)

- [ ] **Step 1: Write the failing test**

Add to `runJukeboxSelfTest()` before `fs::remove_all(tmp, ec);`:

```cpp
    // (T3c) offset_s flows sidecar -> Track -> Club1127World::setBeatGrid.
    {
        const fs::path od = tmp / "offset";
        fs::create_directories(od, ec);
        writeTinyWav(od / "delta.wav", 60);
        { std::ofstream(od / "delta.wav.json") << "{ \"bpm\": 128, \"offset_s\": 0.35 }\n"; }
        writeTinyWav(od / "echo.wav", 60);
        { std::ofstream(od / "echo.wav.json") << "{ \"bpm\": 90 }\n"; }   // no offset => 0

        Jukebox jb;
        jb.configure({ od.string() }, 120.0f, false, 0.75f, true);
        const auto& tr = jb.tracks();
        const bool parsed = tr.size() == 2 &&
                            std::fabs(tr[0].offsetS - 0.35f) < 0.0001f &&
                            std::fabs(tr[1].offsetS) < 0.0001f;

        std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
        audio->init();
        Club1127World club;
        jb.begin(*audio, club);       // delta => 128 BPM, phase 0.35
        const bool applied = std::fabs(club.bpm() - 128.0f) < 0.01f &&
                             std::fabs(club.beatOffsetS() - 0.35f) < 0.0001f;
        jb.next(*audio, club);        // echo => 90 BPM, phase reset to 0
        const bool reset = std::fabs(club.bpm() - 90.0f) < 0.01f &&
                           std::fabs(club.beatOffsetS()) < 0.0001f;
        check(parsed && applied && reset,
              "sidecar offset_s parsed and applied to the beat grid (and reset per track)");
    }
```

- [ ] **Step 2: Run to verify it fails**

```powershell
cmake --build --preset windows-vs2026 --config Release
```
Expected: **compile error** — `'offsetS': is not a member of 'x3::game::Jukebox::Track'`.

- [ ] **Step 3: Implement**

In `app/jukebox.h`, in `struct Track`, after the `bpm` member:

```cpp
        float       bpm        = 120;  // resolved BPM (sidecar, else default cvar)
        float       offsetS    = 0.0f; // sidecar downbeat offset in seconds (F1)
        bool        hasSidecar = false;
```

In `app/jukebox.h`, add next to `currentBpm()`:

```cpp
    float       currentBpm()  const;
    float       currentOffsetS() const;   // sidecar downbeat offset of the current track
```

In `app/jukebox.h`, replace the free-function declaration:

```cpp
bool parseBpmSidecar(const std::string& audioPath, float& outBpm);
```

with:

```cpp
// Resolve BPM (and optional downbeat offset) for `audioPath`: probe
// "<audioPath>.json" then "<stem>.json" for {"bpm": <float>, "offset_s": <float>}.
// Returns true + sets outBpm on a valid sidecar. outOffsetS is set to the sidecar's
// "offset_s" when present and sane, otherwise 0. Returns false (outputs untouched)
// when there is no sidecar / no usable "bpm" number.
bool parseBpmSidecar(const std::string& audioPath, float& outBpm, float& outOffsetS);
```

In `app/jukebox.cpp`, add after `parseBpmFromText` (after line 96):

```cpp
// Optional "offset_s": seconds from track start to the first downbeat. Absent or
// out of range => 0 (the beat grid then starts its phase at t=0, as before).
static bool parseOffsetFromText(const std::string& text, float& outOffsetS) {
    const auto key = text.find("\"offset_s\"");
    if (key == std::string::npos) return false;
    auto colon = text.find(':', key + 10);
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' ||
                               text[i] == '\n' || text[i] == '\r')) ++i;
    if (i >= text.size()) return false;
    char* end = nullptr;
    const double v = std::strtod(text.c_str() + i, &end);
    if (end == text.c_str() + i) return false;
    if (!(v > -10.0 && v < 10.0)) return false;   // sane downbeat window
    outOffsetS = (float)v;
    return true;
}
```

In `app/jukebox.cpp`, replace `parseBpmSidecar` (lines 98-117) with:

```cpp
bool parseBpmSidecar(const std::string& audioPath, float& outBpm, float& outOffsetS) {
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
        const std::string text = ss.str();
        if (parseBpmFromText(text, outBpm)) {
            outOffsetS = 0.0f;                       // absent offset_s => no phase shift
            parseOffsetFromText(text, outOffsetS);   // present + sane => use it
            return true;
        }
        x3::logWarn(std::string("[jukebox] sidecar has no usable bpm: ") + c.string());
    }
    return false;
}
```

In `app/jukebox.cpp::scan()`, replace lines 170-172:

```cpp
            float bpm = m_defaultBpm;
            t.hasSidecar = parseBpmSidecar(t.path, bpm);
            t.bpm = t.hasSidecar ? bpm : m_defaultBpm;
```

with:

```cpp
            float bpm = m_defaultBpm, offsetS = 0.0f;
            t.hasSidecar = parseBpmSidecar(t.path, bpm, offsetS);
            t.bpm     = t.hasSidecar ? bpm : m_defaultBpm;
            t.offsetS = t.hasSidecar ? offsetS : 0.0f;
```

In `app/jukebox.cpp::playCurrent()`, replace line 234:

```cpp
    club.setBpm(t.bpm);   // retune the beat grid (subs/tiles/dancers/lights)
```

with:

```cpp
    // Retune the beat grid (subs/tiles/dancers/lights) to this track's tempo AND
    // downbeat phase. A track with no offset_s resets the phase to 0.
    club.setBeatGrid(t.bpm, t.offsetS);
```

In `app/jukebox.cpp`, add after `currentBpm()` (after line 203):

```cpp
float Jukebox::currentOffsetS() const {
    if (m_index < 0 || m_index >= (int)m_tracks.size()) return 0.0f;
    return m_tracks[m_index].offsetS;
}
```

In `app/jukebox.cpp`, in the existing self-test helper `writeSidecar`, leave as-is (it writes bpm only; the new test writes its own sidecars inline).

- [ ] **Step 4: Run to verify it passes**

```powershell
cmake --build --preset windows-vs2026 --config Release
.\build\bin\Release\X3Engine.exe --test-jukebox
```
Expected: `PASS sidecar offset_s parsed and applied to the beat grid (and reset per track)`, exit 0.

- [ ] **Step 5: Commit**

```powershell
git add app/jukebox.h app/jukebox.cpp
git commit -m "feat(jukebox): sidecar offset_s drives beat-grid phase (F1b)"
```

---

## Task 3: Three-root scan with inverted precedence (F2)

**Files:**
- Modify: `app/jukebox.cpp:131-141` (`rescan`), `:148-151` (scan comment)

- [ ] **Step 1: Write the failing test**

Add to `runJukeboxSelfTest()` before `fs::remove_all(tmp, ec);`:

```cpp
    // (T4b) Root precedence: the SAME basename in two roots resolves to the
    //       EARLIER root (explicit override > user library > repo fixture).
    {
        const fs::path r1 = tmp / "root1", r2 = tmp / "root2";
        fs::create_directories(r1, ec);
        fs::create_directories(r2, ec);
        writeTinyWav(r1 / "shared.wav", 60);
        { std::ofstream(r1 / "shared.wav.json") << "{ \"bpm\": 111 }\n"; }
        writeTinyWav(r2 / "shared.wav", 60);
        { std::ofstream(r2 / "shared.wav.json") << "{ \"bpm\": 222 }\n"; }
        writeTinyWav(r2 / "only2.wav", 60);

        Jukebox jb;
        jb.configure({ r1.string(), r2.string() }, 120.0f, false, 0.75f, true);
        const auto& tr = jb.tracks();
        // union = shared + only2; shared must come from root1 (bpm 111, not 222)
        bool sharedFromR1 = false, hasOnly2 = false;
        for (const auto& t : tr) {
            if (t.name == "shared" && std::fabs(t.bpm - 111.0f) < 0.01f) sharedFromR1 = true;
            if (t.name == "only2") hasOnly2 = true;
        }
        check(tr.size() == 2 && sharedFromR1 && hasOnly2,
              "scan unions roots and resolves duplicate basenames to the EARLIER root");
    }
```

- [ ] **Step 2: Run to verify it fails**

```powershell
cmake --build --preset windows-vs2026 --config Release
.\build\bin\Release\X3Engine.exe --test-jukebox
```
Expected: this case **PASSES already** — `scan()` already skips duplicates from later dirs. The test is a regression guard locking the behaviour in before `rescan()` changes root order. Confirm it passes, then proceed.

- [ ] **Step 3: Implement**

In `app/jukebox.cpp`, replace `rescan` (lines 131-141):

```cpp
void Jukebox::rescan(float vol, bool musicOn) {
    // THREE roots, highest priority FIRST (scan() keeps the first basename it sees):
    //   1. snd_clubmusic_dir cvar  — explicit per-machine override
    //   2. <Documents>/X3Native/club_music — the user library (Tim's real MP3s;
    //      these never enter the repo)
    //   3. <assetRoot>/audio/club_music — the committed fixture (README + test tone)
    // The user's copy deliberately WINS over the repo fixture: the repo copy is a
    // test artifact, the user's is the real track.
    std::vector<std::string> dirs;
    const std::string cvarDir = cvarStr("snd_clubmusic_dir", "X3_SND_CLUBMUSIC_DIR", "");
    if (!cvarDir.empty()) dirs.push_back(cvarDir);
    const std::string userDir = defaultUserDir();
    if (!userDir.empty()) dirs.push_back(userDir);
    dirs.push_back((fs::path(assetRoot()) / "audio" / "club_music").string());
    configure(dirs,
              cvarFloat("snd_clubmusic_bpm",     "X3_SND_CLUBMUSIC_BPM",     120.0f),
              cvarBool ("snd_clubmusic_shuffle", "X3_SND_CLUBMUSIC_SHUFFLE", false),
              vol, musicOn);
}
```

In `app/jukebox.cpp::scan()`, replace the comment at lines 148-150:

```cpp
    // NON-recursive scan of each dir (a `samples/` subdir never auto-plays). A
    // filename already seen in an earlier (higher-priority) dir is skipped, so the
    // repo-local copy wins over the user-dir copy of the same basename.
```

with:

```cpp
    // NON-recursive scan of each dir (a `samples/` subdir never auto-plays). A
    // filename already seen in an EARLIER (higher-priority) dir is skipped — see
    // rescan() for the root order: cvar override > user library > repo fixture.
```

In `app/jukebox.h`, update the folder documentation block near the top to match the three roots and the new precedence (replace the "Folders (scanned NON-recursively...)" paragraph):

```cpp
// Folders (scanned NON-recursively, so a `samples/` subdir never auto-plays), in
// PRIORITY order — the first copy of a given filename wins:
//   1) the `snd_clubmusic_dir` cvar dir          — explicit per-machine override
//   2) <Documents>/X3Native/club_music           — the user library (real music,
//                                                  never enters the repo)
//   3) <assetRoot>/audio/club_music/             — committed fixture (README +
//                                                  one tiny test tone)
// A track present in several roots resolves to the HIGHEST-priority one: the
// user's real file beats the repo's test fixture.
```

- [ ] **Step 4: Run to verify it passes**

```powershell
cmake --build --preset windows-vs2026 --config Release
.\build\bin\Release\X3Engine.exe --test-jukebox
```
Expected: all cases PASS, exit 0.

- [ ] **Step 5: Commit**

```powershell
git add app/jukebox.h app/jukebox.cpp
git commit -m "feat(jukebox): three scan roots, user library beats repo fixture (F2)"
```

---

## Task 4: `Config` struct with deterministic shuffle (F5)

**Files:**
- Modify: `app/jukebox.h` (add `Config`, overload `configure`)
- Modify: `app/jukebox.cpp:121-129` (`configure`), `:183-185` (shuffle)

- [ ] **Step 1: Write the failing test**

Add to `runJukeboxSelfTest()` before `fs::remove_all(tmp, ec);`:

```cpp
    // (T5b) Deterministic shuffle: the same seed reproduces the same order; a
    //       different seed produces a different one (with enough tracks).
    {
        const fs::path sd = tmp / "shuffle";
        fs::create_directories(sd, ec);
        for (const char* n : { "a","b","c","d","e","f","g","h" })
            writeTinyWav(sd / (std::string(n) + ".wav"), 20);

        auto orderWithSeed = [&](uint32_t seed) {
            Jukebox::Config cfg;
            cfg.dirs        = { sd.string() };
            cfg.defaultBpm  = 120.0f;
            cfg.shuffle     = true;
            cfg.volume      = 0.75f;
            cfg.musicOn     = true;
            cfg.shuffleSeed = seed;
            Jukebox jb;
            jb.configure(cfg);
            std::string s;
            for (const auto& t : jb.tracks()) s += t.name;
            return s;
        };
        const std::string s1 = orderWithSeed(1234);
        const std::string s2 = orderWithSeed(1234);
        const std::string s3 = orderWithSeed(9876);
        check(s1.size() == 8 && s1 == s2 && s1 != s3,
              "seeded shuffle is deterministic (same seed => same order)");
    }
```

- [ ] **Step 2: Run to verify it fails**

```powershell
cmake --build --preset windows-vs2026 --config Release
```
Expected: **compile error** — `'Config': is not a member of 'x3::game::Jukebox'`.

- [ ] **Step 3: Implement**

In `app/jukebox.h`, inside `class Jukebox`, immediately after `struct Track { ... };`:

```cpp
    // Full configuration in one struct. `shuffleSeed` 0 means "derive from the
    // clock" (a fresh order every boot); any non-zero value makes the shuffle
    // reproducible, which is what lets the self-test assert on it.
    struct Config {
        std::vector<std::string> dirs;
        float    defaultBpm  = 120.0f;
        bool     shuffle     = false;
        float    volume      = 0.75f;
        bool     musicOn     = true;
        uint32_t shuffleSeed = 0;
    };
```

In `app/jukebox.h`, add next to the existing `configure` declaration:

```cpp
    void configure(const std::vector<std::string>& dirs, float defaultBpm,
                   bool shuffle, float vol, bool musicOn);
    // Preferred form. The 5-arg overload above forwards to this with seed 0.
    void configure(const Config& cfg);
```

In `app/jukebox.h`, add the private member next to `m_shuffle`:

```cpp
    bool   m_shuffle    = false;              // snd_clubmusic_shuffle
    uint32_t m_shuffleSeed = 0;               // 0 = derive from clock (F5)
```

In `app/jukebox.cpp`, replace `configure` (lines 121-129):

```cpp
void Jukebox::configure(const Config& cfg) {
    m_dirs        = cfg.dirs;
    m_defaultBpm  = (cfg.defaultBpm > 20.0f && cfg.defaultBpm < 400.0f) ? cfg.defaultBpm : 120.0f;
    m_shuffle     = cfg.shuffle;
    m_vol         = (cfg.volume < 0.0f) ? 0.0f : (cfg.volume > 1.0f ? 1.0f : cfg.volume);
    m_musicOn     = cfg.musicOn;
    m_shuffleSeed = cfg.shuffleSeed;
    scan();
}

void Jukebox::configure(const std::vector<std::string>& dirs, float defaultBpm,
                        bool shuffle, float vol, bool musicOn) {
    Config cfg;
    cfg.dirs       = dirs;
    cfg.defaultBpm = defaultBpm;
    cfg.shuffle    = shuffle;
    cfg.volume     = vol;
    cfg.musicOn    = musicOn;
    cfg.shuffleSeed = 0;          // clock-seeded: a fresh order every launch
    configure(cfg);
}
```

In `app/jukebox.cpp::scan()`, replace lines 183-185:

```cpp
    if (m_shuffle) {
        std::mt19937 rng{ std::random_device{}() };
        std::shuffle(m_tracks.begin(), m_tracks.end(), rng);
```

with:

```cpp
    if (m_shuffle) {
        // Sort first so the shuffle input is deterministic too — otherwise the
        // filesystem's directory order would leak into a "seeded" result.
        std::sort(m_tracks.begin(), m_tracks.end(),
                  [](const Track& a, const Track& b) { return a.sortKey < b.sortKey; });
        std::mt19937 rng{ m_shuffleSeed ? m_shuffleSeed
                                        : (uint32_t)std::random_device{}() };
        std::shuffle(m_tracks.begin(), m_tracks.end(), rng);
```

- [ ] **Step 4: Run to verify it passes**

```powershell
cmake --build --preset windows-vs2026 --config Release
.\build\bin\Release\X3Engine.exe --test-jukebox
```
Expected: `PASS seeded shuffle is deterministic (same seed => same order)`, exit 0.

- [ ] **Step 5: Commit**

```powershell
git add app/jukebox.h app/jukebox.cpp
git commit -m "feat(jukebox): Config struct + deterministic seeded shuffle (F5)"
```

---

## Task 5: Decode probe and `bad` flag (F3)

**Files:**
- Modify: `engine/audio/IAudioSystem.h` (add `probeAudioFile`)
- Modify: `engine/audio/MiniaudioSystem.cpp` (implement it)
- Modify: `app/jukebox.h` (`Track::bad`), `app/jukebox.cpp` (`playCurrent`)

- [ ] **Step 1: Write the failing test**

Add to `runJukeboxSelfTest()` before `fs::remove_all(tmp, ec);`:

```cpp
    // (T6b) A corrupt file is flagged `bad` and SKIPPED without being played, so
    //       the playlist lands on the good track instead of a silent stall.
    {
        const fs::path pd = tmp / "probe";
        fs::create_directories(pd, ec);
        writeTinyWav(pd / "aaa_bad.wav", 60, /*corrupt*/true);
        writeTinyWav(pd / "bbb_good.wav", 60);
        std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
        audio->init();
        Club1127World club;
        Jukebox jb;
        jb.configure({ pd.string() }, 120.0f, false, 0.75f, true);
        jb.begin(*audio, club);
        // With a real device the probe rejects aaa_bad and we land on bbb_good.
        // In silent/no-device mode probeAudioFile returns true (cannot decode
        // without an engine), so accept either — what must NOT happen is a hang
        // or a crash, and `bad` must be observable when the probe ran.
        const bool landedOk = jb.currentName() == "bbb_good" || jb.currentName() == "aaa_bad";
        check(jb.hasTracks() && landedOk,
              "corrupt track is probed and skipped (or tolerated in silent mode)");
    }
```

- [ ] **Step 2: Run to verify it fails**

```powershell
cmake --build --preset windows-vs2026 --config Release
```
Expected: compiles and **passes trivially** — this case only becomes meaningful once the probe exists. Proceed to Step 3; the value is the skip path added below.

- [ ] **Step 3: Implement**

In `engine/audio/IAudioSystem.h`, add immediately before `virtual void update(float dt) = 0;`:

```cpp
    // Cheap decode PROBE: can this file be opened and decoded at all? Used by the
    // Club Jukebox to skip a corrupt track BEFORE trying to play it (otherwise the
    // player hears a gap while the playlist works out the track is dead).
    // Default returns true so backends opt in and silent/no-device mode never
    // rejects a track it simply cannot test.
    virtual bool probeAudioFile(std::string_view /*absPath*/) { return true; }
```

In `engine/audio/MiniaudioSystem.cpp`, add inside the `MiniaudioSystem` class (next to the other overrides):

```cpp
    bool probeAudioFile(std::string_view absPath) override {
        if (!m_ok) return true;              // silent/no-device: cannot judge, accept
        ma_decoder dec;
        const std::string p(absPath);
        if (ma_decoder_init_file(p.c_str(), nullptr, &dec) != MA_SUCCESS) return false;
        ma_decoder_uninit(&dec);
        return true;
    }
```

In `app/jukebox.cpp::playCurrent()`, replace the missing-file guard (lines 220-225):

```cpp
    std::error_code ec;
    int guard = (int)m_tracks.size();
    while (guard-- > 0 && !fs::exists(m_tracks[m_index].path, ec)) {
        x3::logWarn("[jukebox] skipping missing track: " + m_tracks[m_index].path);
        m_index = (m_index + 1) % (int)m_tracks.size();
    }
```

with:

```cpp
    // Skip any track that has vanished since the scan, or that the audio backend
    // cannot decode (F3). Each is logged ONCE via the per-track `bad` flag so a
    // wrapping playlist does not spam the log every lap.
    std::error_code ec;
    int guard = (int)m_tracks.size();
    while (guard-- > 0) {
        Track& t = m_tracks[m_index];
        if (!fs::exists(t.path, ec)) {
            if (!t.bad) { t.bad = true; x3::logWarn("[jukebox] skipping missing track: " + t.path); }
        } else if (!t.bad && !audio.probeAudioFile(t.path)) {
            t.bad = true;
            x3::logWarn("[jukebox] skipping undecodable track: " + t.path);
        }
        if (!t.bad) break;                                   // playable
        m_index = (m_index + 1) % (int)m_tracks.size();
    }
    if (m_tracks[m_index].bad) {
        x3::logWarn("[jukebox] every track is unplayable — leaving the club's built-in loop alone");
        return;
    }
```

- [ ] **Step 4: Run to verify it passes**

```powershell
cmake --build --preset windows-vs2026 --config Release
.\build\bin\Release\X3Engine.exe --test-jukebox
```
Expected: all cases PASS including the pre-existing corrupt/missing case, exit 0.

- [ ] **Step 5: Commit**

```powershell
git add engine/audio/IAudioSystem.h engine/audio/MiniaudioSystem.cpp app/jukebox.cpp
git commit -m "feat(audio,jukebox): probeAudioFile hook; skip undecodable tracks once (F3)"
```

---

## Task 6: `stopPlayback` and host wiring (F4)

**Files:**
- Modify: `app/jukebox.h` (declare), `app/jukebox.cpp` (define)
- Modify: `app/world_hosts/host_club.cpp` (call on exit)

- [ ] **Step 1: Write the failing test**

Add to `runJukeboxSelfTest()` before `fs::remove_all(tmp, ec);`:

```cpp
    // (T8) stopPlayback restores the club's house tempo and clears the phase.
    {
        std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
        audio->init();
        Club1127World club;
        Jukebox jb;
        jb.configure({ tmp.string() }, 120.0f, false, 0.75f, true);
        jb.begin(*audio, club);                       // retunes away from kDefaultBpm
        const bool moved = std::fabs(club.bpm() - Club1127World::kDefaultBpm) > 0.01f;
        jb.stopPlayback(*audio, club);
        const bool restored = std::fabs(club.bpm() - Club1127World::kDefaultBpm) < 0.01f &&
                              std::fabs(club.beatOffsetS()) < 0.0001f;
        check(moved && restored,
              "stopPlayback restores the house tempo and zeroes the beat phase");
    }
```

- [ ] **Step 2: Run to verify it fails**

```powershell
cmake --build --preset windows-vs2026 --config Release
```
Expected: **compile error** — `'stopPlayback': is not a member of 'x3::game::Jukebox'`.

- [ ] **Step 3: Implement**

In `app/jukebox.h`, add after `prev(...)`:

```cpp
    // Stop the jukebox: silence the music voice and hand the beat grid back to the
    // club's built-in tempo/phase. Safe to call when nothing is playing.
    void stopPlayback(x3::audio::IAudioSystem& audio, Club1127World& club);
```

In `app/jukebox.cpp`, add after `prev(...)` (after line 255):

```cpp
void Jukebox::stopPlayback(x3::audio::IAudioSystem& audio, Club1127World& club) {
    audio.stopMusic();
    club.setBeatGrid(Club1127World::kDefaultBpm, 0.0f);   // back to club_descent
    m_toast = 0.0f;
    m_toastText.clear();
    x3::logInfo("[jukebox] stopped — club back to its house tempo");
}
```

In `app/world_hosts/host_club.cpp`, after the main loop ends and before the host returns (locate the teardown that follows the loop containing `jukebox.update(dt, *caudio, club);` at line ~265), add:

```cpp
    // Hand the beat grid back to the club's built-in track on the way out, so a
    // later re-entry does not inherit the last user track's tempo/phase.
    if (caudio && jukebox.hasTracks()) jukebox.stopPlayback(*caudio, club);
```

- [ ] **Step 4: Run to verify it passes**

```powershell
cmake --build --preset windows-vs2026 --config Release
.\build\bin\Release\X3Engine.exe --test-jukebox
```
Expected: `PASS stopPlayback restores the house tempo and zeroes the beat phase`, exit 0.

- [ ] **Step 5: Commit**

```powershell
git add app/jukebox.h app/jukebox.cpp app/world_hosts/host_club.cpp
git commit -m "feat(jukebox): stopPlayback restores house tempo on club exit (F4)"
```

---

## Task 7: User documentation and spec amendment

**Files:**
- Modify: `assets/audio/club_music/README.txt`
- Modify: `docs/superpowers/specs/2026-07-25-club-jukebox-merge-design.md`

- [ ] **Step 1: Update the README**

In `assets/audio/club_music/README.txt`, replace the sidecar section body with:

```
TELLING THE CLUB YOUR TRACK'S BPM AND DOWNBEAT (optional)
---------------------------------------------------------
  The beat grid rides each track's BPM. There is NO auto-detect — tag it yourself
  with a tiny sidecar JSON next to the track:

      my_song.mp3
      my_song.mp3.json      <-  { "bpm": 128, "offset_s": 0.35 }
        (or my_song.json)

  "bpm"       required. 20-400.
  "offset_s"  OPTIONAL. Seconds from the start of the file to the FIRST DOWNBEAT.
              Without it the grid runs at the right tempo but the wrong phase —
              the subs punch between your beats instead of on them. Range -10..10.

  No sidecar?  The club uses the snd_clubmusic_bpm cvar (default 120) and phase 0.

WHERE YOUR MUSIC CAN LIVE (highest priority first)
---------------------------------------------------
  1. the snd_clubmusic_dir cvar folder    - explicit override
  2. %USERPROFILE%\Documents\X3Native\club_music  - your library (recommended)
  3. THIS folder (in the repo)            - README + one test tone only

  The same filename in more than one place resolves to the HIGHEST-priority copy:
  your real track beats the repo's test fixture.
```

- [ ] **Step 2: Amend the spec**

In `docs/superpowers/specs/2026-07-25-club-jukebox-merge-design.md`, append to §4.2:

```markdown
### Amendment (2026-07-25, during planning)

- **F6 (`drawToast`) DROPPED.** `app/world_hosts/host_club.cpp:316-323` already renders the
  toast from `toastRemaining()`/`toastText()`. Porting the local `drawToast()` would couple a
  game module to `IRenderDevice` for no user-visible gain, and spec #3's screen system replaces
  this surface anyway.
- **F3 requires an engine change** not listed in §9: `IAudioSystem::probeAudioFile` (default
  `true`, overridden by the miniaudio backend). §9's file list is extended with
  `engine/audio/IAudioSystem.h` and `engine/audio/MiniaudioSystem.cpp`.
- **F1 requires a new club API.** The real interface is `setBpm(float)`/`bpm()`; there was no
  `setBeatGrid`. It is added, and `club1127.cpp:1950`'s `beatCount` subtracts the offset.
```

- [ ] **Step 3: Verify the full suite still passes**

```powershell
cmake --build --preset windows-vs2026 --config Release
.\build\bin\Release\X3Engine.exe --test-jukebox
```
Expected: `jukebox: N/N passed`, exit 0.

- [ ] **Step 4: Commit**

```powershell
git add assets/audio/club_music/README.txt docs/superpowers/specs/2026-07-25-club-jukebox-merge-design.md
git commit -m "docs(jukebox): document offset_s + root precedence; amend spec (F6 dropped)"
```

---

## Definition of done

- [ ] `--test-jukebox` reports `jukebox: N/N passed` with exit code 0
- [ ] A sidecar with `offset_s` visibly shifts the beat grid's phase in-club (eyes-on: enter `--world club`, N to a tagged track, confirm the sub cones punch on the beat)
- [ ] Leaving the club restores the `club_descent` tempo
- [ ] All commits are local; **nothing is pushed** (session lead reviews and pushes)
