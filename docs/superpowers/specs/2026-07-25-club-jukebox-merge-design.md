# Club 1127 Jukebox — Merge Design

**Date:** 2026-07-25
**Branch:** `feat/club-jukebox-merge` (worktree `D:\GameDev\x3-jukebox-merge`, parented to `X3Native`)
**Base:** `origin/feat/club-jukebox` @ `212d1ec1`
**Status:** design approved, ready for implementation plan

---

## 1. Context

Two independent jukebox implementations exist for Club 1127. They are not two commits on one
lineage — they are different modules written in parallel:

| | Local (`backup/club-jukebox-local-2026-07-25`) | Remote (`origin/feat/club-jukebox`) |
|---|---|---|
| Module | `app/club_jukebox.{h,cpp}` — `ClubJukebox` | `app/jukebox.{h,cpp}` — `Jukebox` |
| Size | 406 lines .cpp | 434 lines .cpp |
| Club | pre-canon-port | **includes the +1186-line `club1127.cpp` canon-port** |

The remote branch is 12 commits ahead and 2 behind; the local commits are preserved on
`backup/club-jukebox-local-2026-07-25` (pushed 2026-07-25).

`x3-jukebox`'s working tree reports 105 modified files. **All 105 are `assets/fonts/*` LFS
pointer churn (raw blobs committed before `.gitattributes` routed `*.ttf`/`*.otf` through LFS)
— zero real uncommitted work.** Nothing needs salvaging from that working tree.

## 2. Goal

One jukebox module that keeps the best ideas from both implementations, built on the remote's
canon-port club, and positioned so the two downstream subsystems (§10) drop in without
re-architecture.

## 3. Non-goals

Explicitly **out of scope** for this spec — each gets its own spec:

- **Spatial band-split audio** — routing the track's real low end to the corner subs.
- **Screen system + MilkDrop-style visualizer wall.**

This spec must not preclude either. Where a decision affects them, §10 records the constraint.

## 4. Decisions

### 4.1 Canonical module: the remote's `Jukebox`

`app/jukebox.{h,cpp}` is the base. It is what the canon-port `host_club.cpp` and
`club1127.cpp` already call, and it ships the `.gitignore`/README/test-tone hygiene.
`app/club_jukebox.{h,cpp}` are **not** ported wholesale; their ideas are folded in below.

### 4.2 Ideas folded in from the local implementation

| # | Idea | Rationale |
|---|---|---|
| F1 | **`offset_s` sidecar field + `Track::offsetS`** | Aligns beat 0 to the track's true downbeat. The remote gets tempo right and *phase* wrong — the light show lands off-beat. |
| F2 | **Three-root scan union with dedup** | Tim's MP3s live in `<Documents>/X3Native/club_music` and must never enter the repo. Remote scans only two roots. |
| F3 | **Decode probe + per-track `bad` flag** | Detects a corrupt/undecodable file *before* playback stalls; logged once, then skipped. |
| F4 | **`stopPlayback()`** | Restores the house default tempo on club exit. The remote has no clean stop. |
| F5 | **`Config` struct incl. `shuffleSeed`** | Seed 0 = derive from clock; a fixed seed makes shuffle deterministic and therefore testable. |
| F6 | **`drawToast(device, frame)`** | Actually renders the toast. Remote only exposes `toastRemaining()`/`toastText()` and leaves drawing to the host. Serves as the DJ-screen fallback until the screen system lands. |

### 4.3 Behaviour kept from the remote

- Non-recursive folder scan (a `samples/` subdir never auto-plays).
- `X3_SND_CLUBMUSIC_*` env-var overrides for each cvar.
- Respect for the Settings music on/off toggle and music volume.
- Dual sidecar naming: `<track>.mp3.json` **or** `<track>.json`.
- `isMusicFinished()`-driven auto-advance.
- `runJukeboxSelfTest()` and the `--test-jukebox` CLI hook.
- `.gitignore` coverage so user audio is never committed.

### 4.4 Scan-root precedence (resolves F2 against the remote's rule)

Roots scanned, in order:

1. `snd_clubmusic_dir` cvar / `Config::extraDir` (explicit override)
2. `<Documents>/X3Native/club_music` (user library)
3. `<assetRoot>/audio/club_music` (repo-local; README + test tone)

Union of all three, **deduplicated by filename**. On collision the **earlier root wins**
(explicit override beats user library beats repo). This inverts the remote's
"repo-local wins" rule, because the repo copy is a committed fixture and the user's copy is
the real track.

> Note: the remote's stated rule ("a track present in BOTH resolves to the repo-local copy")
> is deliberately reversed here. Any comment carrying the old rule must be updated.

### 4.5 Sidecar schema

```json
{ "bpm": 128.0, "offset_s": 0.35 }
```

- `bpm` — required for a sidecar to count. Non-positive/missing → treat as no sidecar.
- `offset_s` — optional, defaults `0.0`. Seconds from track start to the first downbeat.
- No auto-detection. Untagged tracks use the `snd_clubmusic_bpm` cvar (default 120).

## 5. Public API (merged `x3::game::Jukebox`)

```cpp
struct Track {
    std::string path, name, sortKey;
    float bpm        = 120.0f;
    float offsetS    = 0.0f;   // F1
    bool  hasSidecar = false;
    bool  bad        = false;  // F3
};

struct Config {                 // F5
    std::string extraDir;
    bool     shuffle     = false;
    float    defaultBpm  = 120.0f;
    float    volume      = 0.75f;
    uint32_t shuffleSeed = 0;   // 0 = derive from clock
};

void configure(const Config&);
void rescan(float vol, bool musicOn);
void begin(IAudioSystem&, Club1127World&);
void update(float dt, IAudioSystem&, Club1127World&);
void next(IAudioSystem&, Club1127World&);
void prev(IAudioSystem&, Club1127World&);
void stopPlayback(IAudioSystem&, Club1127World&);   // F4

bool hasTracks() const;  size_t count() const;  int index() const;
const std::string& currentName() const;
float currentBpm() const;  float currentOffsetS() const;   // F1
float toastRemaining() const;  const std::string& toastText() const;
void  drawToast(IRenderDevice&, const FrameContext&) const; // F6

bool parseBpmSidecar(const std::string& audioPath, float& outBpm, float& outOffsetS); // F1
```

## 6. Data flow

```
club entry
  └─ rescan(vol, musicOn)        scan 3 roots → dedup → sidecars → sort/shuffle(seed)
  └─ begin(audio, club)
       └─ playCurrent()          probe (F3) → skip if bad → playMusic()
            └─ applyGridFor(t)   club.setBeatGrid(t.bpm, t.offsetS)   ← F1
                                   ↳ corner subs, dance tiles, dancers, moving heads
  per frame
  └─ update(dt, audio, club)     toast countdown; isMusicFinished() → next()
  club exit
  └─ stopPlayback(audio, club)   stopMusic(); restore house tempo      ← F4
```

## 7. Error handling

| Condition | Behaviour |
|---|---|
| Track missing/undecodable | `bad = true`, one log line, advance to next. Never stalls. |
| All tracks bad | Behave as empty playlist. |
| Empty playlist | Jukebox never activates; built-in `club_descent` loop is byte-for-byte unchanged. |
| No audio device / silent mode | Inert; every call a safe no-op. |
| `Club1127World` null | Playback proceeds, no beat-grid retune. |
| Malformed sidecar | Ignored; fall back to default BPM, `offsetS = 0`. One log line. |
| Same filename in two roots | Earlier root wins (§4.4). |

## 8. Testing — `--test-jukebox`

Extends `runJukeboxSelfTest()`. All cases use tiny generated WAVs in a temp dir plus the
committed `samples/test_tone_120.wav` fixture. No real music touched, no device required.

| # | Case | Asserts |
|---|---|---|
| T1 | Folder scan | Alphabetical order; extension filter; non-recursive (a `samples/` subdir is not auto-played). |
| T2 | Sidecar parse | Both naming forms; `bpm` + `offset_s`; malformed → default. |
| T3 | **Phase alignment (F1)** | `setBeatGrid` receives the sidecar's `bpm` *and* `offsetS`. |
| T4 | **Three-root precedence (F2)** | Union across 3 temp roots; duplicate filename resolves to the earlier root. |
| T5 | **Deterministic shuffle (F5)** | Same `shuffleSeed` → identical order twice; different seed → different order. |
| T6 | **Corrupt-file skip (F3)** | A truncated file is flagged `bad`, skipped, playlist advances, logged once. |
| T7 | Empty-folder fallback | No retune issued; house tempo untouched. |
| T8 | **`stopPlayback` (F4)** | Music stopped and house tempo restored. |

Pass criterion: `jukebox: X/Y passed` with `X == Y`, non-zero exit on failure.

**Not covered by the headless self-test:** F6 (`drawToast`) needs a render device and a frame
context, so it cannot run in `--test-jukebox`. It is verified by entering the club and
confirming the toast draws on track change (N / Shift+N). Its *state* (`toastRemaining()`,
`toastText()`) is exercised by T3/T6 via `update(dt)`; only the draw call itself is untested.

## 9. Files touched

| File | Change |
|---|---|
| `app/jukebox.h` | `Track::offsetS`, `Config`, `stopPlayback`, `drawToast`, `currentOffsetS`, extended sidecar signature |
| `app/jukebox.cpp` | Three-root scan + dedup, probe/`bad`, seeded shuffle, offset plumbed to `setBeatGrid`, stop path, toast render |
| `app/club1127.h/.cpp` | `setBeatGrid(bpm, offsetS)` — phase parameter |
| `app/world_hosts/host_club.cpp` | Call `stopPlayback()` on club exit |
| `assets/audio/club_music/README.txt` | Document `offset_s` + the three roots and their precedence |
| `app/test_registry.cpp` | Register the new self-test cases |

## 10. Downstream requirements (recorded, not built here)

Committed decisions from the 2026-07-25 design session. This spec must not preclude them.

**Spec #2 — spatial band-split audio.** The track's *own* low end must come from the corner
subs, not a synthesized thump (explicitly chosen over the cheaper beat-locked layer).
Technique to validate: decode once into a shared buffer, run N spatialized voices over it —
high-passed cabinet voice at the DJ booth, low-passed voices at the 4 SVS corners and 4 JBL
18" positions. All voices advance in the same mix callback, so they stay sample-locked
without a custom crossover node. Trades `MA_SOUND_FLAG_STREAM` for a full decode (~25–50 MB
per track). `IAudioSystem` already provides `startLoop3D`/`stopLoop`/`setLoopParams`,
`setListener`, per-voice `ma_lpf_node`, and a shared reverb — positional emitters need no new
engine work; the *band split* does.

**Spec #3 — screens + visualizer wall.** One screen system, mode enum, three renderers over
shared plumbing (offscreen target per OLED entity, driven from `oledEntities()`):

| Screens | Mode |
|---|---|
| 6 × POE multiplex (80/85/75/65/55/55") | MilkDrop-style visualizers |
| 2 × DJ booth OLED | Now-playing card (name, BPM, index, elapsed) |
| back-bar + lounge | Album art |

Visualizers are **hand-ported from the presets Tim actually ran in the real club** (files live
on the real DJ booth desktop — not on this machine or `\\p13700\G`). A `.milk` interpreter is
explicitly out of scope; the data model should leave it possible later.

**Both downstream specs depend on F1.** Because the sidecar supplies true BPM *and* the
downbeat offset, the visualizers run **phase-locked and zero-latency** rather than
beat-inferred — categorically better than MilkDrop, which must guess. Build spec #2 before #3:
the crossover produces the bass/mid/treble envelopes the visualizers consume.

## 11. Known repo issue (out of scope, worth a follow-up)

105 `assets/fonts/*.ttf|.otf` files are committed as raw blobs while `.gitattributes` routes
them through LFS, so every worktree reports them permanently modified. Fix is
`git add --renormalize` on those paths in one dedicated commit (or `git lfs migrate import`,
which rewrites history). Not part of this work, but it is why every X3 worktree looks dirty
and it corrupts any "is this directory clean?" audit.

## 12. Open questions

None blocking. Deferred to their own specs:

- Crossover frequency and slope for the band split (spec #2).
- Which specific MilkDrop presets to port (spec #3) — needs the files off the DJ booth desktop.
- Whether the 6-screen multiplex renders one visualizer across all six as a single surface, or
  six independent instances (spec #3).
