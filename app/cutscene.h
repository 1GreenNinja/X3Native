#pragma once
// x3.cutscene/1 — DATA-DRIVEN CUTSCENE SYSTEM (docs/design/CUTSCENE_FORMAT.md).
//
// Game/slice code only — engine/ stays pure. This module is PURE DATA + EVAL + a
// deterministic timeline player: no window / Vulkan / GLFW / audio handles — so
// --test-cutscene exercises the whole thing headless, and the windowed/headless
// CINEMATIC DRIVER in app/main.cpp consumes evaluated poses each frame.
//
// SPLIT (mirrors ui.cpp / intro_coldopen.cpp): data structs + a self-contained JSON
// parser (same stance as level_loader.cpp) + pure evaluation (Catmull-Rom camera +
// actor splines, FOV ramps, deterministic shake, fades / letterbox / title alphas)
// + CutscenePlayer (tick/seek/skip with exactly-once audio-cue + x3.fire-event
// semantics). The first authored consumer is THE COLD OPEN
// (assets/cutscenes/cold_open.cutscene.json): Jake's last flight -> ambushed by a
// capital ship -> shot down -> smash to black -> "ESCAPE FROM LAB ZERO" ->
// "SIX MONTHS LATER" -> wake in the Level-1 cell.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace x3::cut {

// ---------------------------------------------------------------------------
// Data model (parsed from *.cutscene.json)
// ---------------------------------------------------------------------------
struct Vec3 { float x = 0, y = 0, z = 0; };

struct CameraKey {
    float t   = 0.0f;
    Vec3  pos{};
    Vec3  look{};          // LOOK-AT target point (yaw/pitch derived at eval)
    float fov = 60.0f;     // degrees
    bool  cut = false;     // true = HARD CUT into this key (starts a new shot/spline span;
                           // interpolation never crosses a cut boundary)
};

struct ShakeBurst {
    float t = 0, dur = 0, amp = 0, freq = 10.0f;   // meters / Hz; linear decay over dur
};

struct ActorKey {
    float t = 0.0f;
    Vec3  pos{};
    Vec3  rotDeg{};        // yaw / pitch / roll (degrees)
    float scale = 1.0f;
};

struct Actor {
    std::string id;
    std::string model;            // GLB path relative to assetRoot(), or "builtin:beam"/"builtin:glow"
    float size = 0.0f;            // normalize longest AABB axis to this (m); 0 = raw
    Vec3  rotOffsetDeg{};         // static facing correction, applied before keyed rotation
    Vec3  stretch{1, 1, 1};       // per-axis scale multiplier (beams)
    float color[4]    = {1, 1, 1, 1};
    float emissive[4] = {0, 0, 0, 0};   // {r,g,b,strength}; strength>0 overrides model emissive
    float showAt = 0.0f, hideAt = -1.0f;   // hideAt < 0 => duration
    std::vector<ActorKey> keys;   // >= 1, t ascending
};

struct AudioCue {
    float t = 0.0f;
    std::string sound;    // driver-mapped name
    float gain = 1.0f;
    bool  music = false;  // route through playMusic (looped) instead of a one-shot
};

struct Fade {
    float t = 0, dur = 0;
    float from = 0, to = 0;          // alpha ramp over [t, t+dur], holds `to` after
    float color[3] = {0, 0, 0};
};

struct Letterbox {
    bool  present = false;
    float inAt = 0, inDur = 0.5f, outAt = 0, outDur = 0.5f;
    float frac = 0.11f;              // bar height as a fraction of screen height
};

struct TitleCard {
    float t = 0, dur = 0;
    std::string text;
    std::string font = "title";      // "title"|"menu"|"news"|"mono"
    float sizeFrac = 0.07f;          // glyph px = sizeFrac * min(W,H)
    float fadeIn = 0.5f, fadeOut = 0.5f;
    float color[3] = {0.92f, 0.93f, 0.96f};
};

struct Event {
    float t = 0.0f;
    std::string name;
    bool  endState = false;          // MUST still fire on skip
};

struct Cutscene {
    std::string name;
    float duration  = 0.0f;
    bool  skippable = true;
    float skipTo    = -1.0f;         // < 0 => duration
    std::vector<CameraKey>  camKeys;
    std::vector<ShakeBurst> shakes;
    std::vector<Actor>      actors;
    std::vector<AudioCue>   audio;
    std::vector<Fade>       fades;
    Letterbox               letterbox;
    std::vector<TitleCard>  titles;
    std::vector<Event>      events;

    float skipTarget() const { return skipTo >= 0.0f ? skipTo : duration; }
};

// ---------------------------------------------------------------------------
// Parse + validate
// ---------------------------------------------------------------------------
// Parse a *.cutscene.json document (text in memory). On success returns true and
// fills `out`; on failure returns false with human-readable messages in `errors`.
// Parsing also runs validate() — a returned cutscene is always structurally sound.
bool parseCutscene(std::string_view jsonText, Cutscene& out, std::vector<std::string>& errors);

// Read + parse a cutscene file from an absolute/relative filesystem path.
bool loadCutsceneFile(const std::string& path, Cutscene& out, std::vector<std::string>& errors);

// Structural validation (called by parse; exposed for tests): format invariants per
// docs/design/CUTSCENE_FORMAT.md. Appends messages; returns errors.empty().
bool validate(const Cutscene& cs, std::vector<std::string>& errors);

// ---------------------------------------------------------------------------
// Pure evaluation (no state; called with any t)
// ---------------------------------------------------------------------------
struct CamPose {
    Vec3  pos{};
    float yaw = 0, pitch = 0;   // radians, device convention
    float fov = 60.0f;          // degrees
};
// Camera spline (Catmull-Rom pos + look, lerped FOV) + summed deterministic shake.
CamPose evalCamera(const Cutscene& cs, float t);

struct ActorPose {
    Vec3  pos{};
    Vec3  rotDeg{};
    float scale = 1.0f;
    bool  visible = false;
};
ActorPose evalActor(const Cutscene& cs, const Actor& a, float t);

// Full-screen fade overlay at t: returns {r,g,b,a} (a == 0 -> nothing to draw).
void evalFade(const Cutscene& cs, float t, float outRgba[4]);

// Letterbox bar height fraction of screen height at t (0 -> no bars).
float evalLetterbox(const Cutscene& cs, float t);

// Title-card alpha at t (0 when inactive / outside [t, t+dur]).
float evalTitleAlpha(const TitleCard& tc, float t);

// Compose the actor's column-major model matrix at a pose:
//   T(pos) * Ryaw * Rpitch * Rroll * R(rotOffset) * S(scale * normScale * stretch)
// `normScale` is the driver's size-normalization factor (1 when Actor::size == 0).
void actorMatrix(const Actor& a, const ActorPose& p, float normScale, float out[16]);

// ---------------------------------------------------------------------------
// CutscenePlayer — the deterministic timeline runtime
// ---------------------------------------------------------------------------
class CutscenePlayer {
public:
    // Event callback: `seeked` is true when the event is delivered by seek()/skip()
    // catch-up (drivers rebuild FX state; hosts still honor endState flags).
    using EventFn = std::function<void(const Event& e, bool seeked)>;
    using AudioFn = std::function<void(const AudioCue& cue)>;

    explicit CutscenePlayer(const Cutscene& cs) : m_cs(&cs) { rebuildOrder(); }

    void onEvent(EventFn fn) { m_onEvent = std::move(fn); }
    void onAudio(AudioFn fn) { m_onAudio = std::move(fn); }

    // Advance the playhead; fires audio cues + events crossing (prev, now]. Clamps
    // at duration. Idempotent once done.
    void tick(float dt);

    // Jump the playhead to `t` (clamped [0, duration]). Forward seeks deliver the
    // jumped-over EVENTS with seeked=true (audio cues are NOT fired). A backward
    // seek rewinds the fired-state so the timeline can replay deterministically.
    void seek(float t);

    // Skip (no-op unless skippable): jump to skipTarget(); events in the skipped
    // range fire ONLY if endState (delivered seeked=true); others are dropped.
    // At/past the target a second skip jumps to duration the same way.
    void skip();

    // ---- Queries -----------------------------------------------------------
    float time() const { return m_t; }
    bool  done() const { return m_t >= m_cs->duration; }
    bool  skipped() const { return m_skipped; }
    const Cutscene& cutscene() const { return *m_cs; }

    // Eval conveniences at the current playhead.
    CamPose   camera() const { return evalCamera(*m_cs, m_t); }
    ActorPose actor(const Actor& a) const { return evalActor(*m_cs, a, m_t); }

    // Test hooks: how many events/audio cues have fired so far.
    uint32_t firedEventCount() const;
    uint32_t firedAudioCount() const;

private:
    void rebuildOrder();
    // Deliver everything in (from, to]; mode: 0=play (audio+events), 1=seek
    // (events only, seeked=true), 2=skip (endState events only, seeked=true).
    void fireRange(float from, float to, int mode);

    const Cutscene* m_cs = nullptr;
    float m_t = 0.0f;
    bool  m_skipped = false;
    std::vector<uint32_t> m_evOrder;    // event indices sorted by (t, file order)
    std::vector<bool>     m_evFired;
    std::vector<bool>     m_auFired;
    EventFn m_onEvent;
    AudioFn m_onAudio;
};

// ---------------------------------------------------------------------------
// StoryFlags — tiny persisted string-flag set (first consumer: intro_complete)
// ---------------------------------------------------------------------------
class StoryFlags {
public:
    bool has(const std::string& flag) const;
    void set(const std::string& flag);
    void clear(const std::string& flag);
    bool load(const std::string& path);   // missing file == empty set (returns false)
    bool save(const std::string& path) const;
    size_t count() const { return m_flags.size(); }
private:
    std::vector<std::string> m_flags;     // sorted unique
};

// %LOCALAPPDATA%/X3Native/story_flags.txt (dir created on save), else cwd fallback.
std::string defaultStoryFlagsPath();

// ---------------------------------------------------------------------------
// GLB mesh-space POSITION extent (the Actor::size normalization input).
// Reads the GLB's JSON chunk only (no geometry decode): unions the spec-mandated
// min/max of every accessor referenced as a primitive POSITION attribute. Node
// transforms are NOT applied (mesh-space extent — fine for uniform-size casting).
// Returns false on any I/O / parse / missing-min-max failure.
// ---------------------------------------------------------------------------
bool glbPositionExtent(const std::string& path, float outMin[3], float outMax[3]);

// The cold-open completion flag name (shared between the boot gate + the player).
inline constexpr const char* kFlagIntroComplete = "intro_complete";

// ---------------------------------------------------------------------------
// --test-cutscene: headless self-test (no window / Vulkan / audio).
// Validates the format (parse + reject paths), the spline/eval math, exactly-once
// event + audio firing, seek/skip semantics, StoryFlags round-trip, and parses +
// validates the real shipped assets/cutscenes/cold_open.cutscene.json.
// Returns true iff all checks pass.
// ---------------------------------------------------------------------------
bool runCutsceneSelfTest();

} // namespace x3::cut
