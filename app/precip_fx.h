#pragma once
// ============================================================================
// PRECIPITATION — falling snow and rain you drive through.
//
// THE ONE DECISION THAT MATTERS: the particles live in a BOX THAT FOLLOWS THE
// CAMERA, and a flake that leaves the box wraps to the opposite face. It is
// never simulated in world space. That single choice is what makes this
// possible at all — the world is fifteen miles across, and filling it with snow
// would cost millions of particles to put a few hundred where you can see them.
// A camera-local volume spends its entire budget inside the view, costs the
// same on a 15-mile ring as in a tunnel mouth, and never runs out of snow
// because you drove somewhere new.
//
// The wrap is also where a naive version gives itself away, in three places:
//
//   * BOUNDARY FADE. A flake that teleports at full opacity BLINKS, and once
//     you notice the blinking you cannot stop seeing it. Every particle fades
//     out toward the shell of the volume and back in on the far side, so the
//     recycle is invisible. This is the single most important line in the file.
//
//   * FLUTTER, not fall. Snow does not drop; it wanders down, each flake on its
//     own little spiral, because it is light enough for air to push around.
//     Give every flake the same straight vertical path and you have made WHITE
//     RAIN — which is exactly what most games ship. Per-flake phase and radius
//     on a lateral oscillation is the whole difference.
//
//   * DEPTH BY SIZE. Near flakes are big and soft, far ones small and faint.
//     Uniform size flattens the volume into a sheet of confetti hanging in
//     front of the lens; the size gradient is what gives it front-to-back.
//
// Rain is the same machine with the dial turned: seven times the fall speed,
// no flutter to speak of, smaller and dimmer and far more of it.
//
// DETERMINISM: an LCG seeded at init, stepped only when a particle respawns.
// No clock reads, no allocation after init, no rand().
// ============================================================================
#include <cstdint>
#include <vector>

namespace x3::rhi { class IRenderDevice; struct FrameContext; }

namespace x3::game {

// What is falling. The two differ by far more than colour, so this picks a
// whole behaviour rather than a tint.
enum class PrecipKind : uint32_t { None = 0, Rain = 1, Snow = 2 };

struct PrecipConfig {
    // Half-extent of the camera-local box, METRES. Big enough that the far face
    // is past the point where a flake is a single dim pixel, small enough that
    // the budget is spent where you are looking.
    float halfExtentM   = 26.0f;
    // Ceiling above the camera. Snow wants headroom -- you look UP into it.
    float heightM       = 34.0f;
    // Particle budget at full intensity. Snow needs fewer than rain because
    // each flake is bigger and lingers.
    uint32_t maxSnow    = 2400;   // 'Rain hardly drops a drip' — was 900; a storm needs a SKY of it
    uint32_t maxRain    = 5200;   // was 1600 — a few hundred live drops read as almost nothing
    // Fall speeds, m/s. Snow terminal velocity is genuinely about 1.2 m/s and
    // rain about 9 -- the gap is most of why they read as different weather.
    float snowFallMps   = 1.25f;
    float rainFallMps   = 9.0f;
    // Lateral flutter for snow: amplitude (m) and how fast a flake cycles it.
    float flutterAmpM   = 0.55f;
    float flutterRate   = 0.9f;
    // Billboard half-extent at the NEAR face and at the FAR face, metres.
    float snowNearSize  = 0.075f;
    float snowFarSize   = 0.022f;
    float rainNearSize  = 0.042f;
    float rainFarSize   = 0.010f;
};

class PrecipFx {
public:
    void init(const PrecipConfig& cfg, uint32_t seed = 0xC0FFEEu);

    // Advance and submit. `intensity` is WeatherSample::precipitation (0..1) and
    // scales how many particles are live -- NOT how fast they fall, because
    // heavier snow is not faster snow, there is just more of it.
    //
    // `windX/windZ` lean the whole column; feed it whatever the world's wind is
    // (0 is fine). camX/camY/camZ is the volume's centre.
    // `skyVisible` is 1 under open sky and 0 under a ROOF. Snow does not fall
    // inside a tunnel -- obvious the first time you drive under concrete in a
    // blizzard, and invisible in any design document. The host answers it with
    // ONE upward raycast per frame, which is why it is a parameter rather than a
    // per-flake query: 900 rays to answer a question with a single answer for
    // the whole volume is real cost for no extra truth.
    //
    // The FIRST attempt at this culled flakes above the roof plane, which was
    // exactly wrong -- the ones you see indoors are BELOW the roof, in the
    // tunnel's own air. Being under cover is a property of the volume, not a
    // height to clip at. Smoothed internally so a tunnel mouth crossfades over
    // about half a second instead of switching the weather like a light.
    void update(float dt, PrecipKind kind, float intensity,
                float camX, float camY, float camZ,
                float windX = 0.0f, float windZ = 0.0f,
                float skyVisible = 1.0f);

    // Submit this frame's live particles. Safe to call with nothing falling --
    // it submits zero and the device adds no particle pass at all.
    void submit(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    uint32_t liveCount() const { return m_live; }
    PrecipKind kind() const { return m_kind; }

private:
    struct Flake {
        float x, y, z;      // position RELATIVE to the volume centre
        float phase;        // flutter phase, radians
        float radius;       // flutter radius scale, 0..1
        float speedVar;     // per-flake fall-speed multiplier, ~0.8..1.2
    };

    uint32_t nextRand();
    float    randUnit();
    void     respawnTop(Flake& f);       // place at the ceiling, random XZ
    void     scatter(Flake& f);          // place anywhere in the volume (init)

    PrecipConfig m_cfg{};
    std::vector<Flake> m_flakes;
    // Rebuilt every frame from the live flakes; kept as a member so submit()
    // never allocates. Typed as raw floats so this header does not have to
    // include the whole render device.
    mutable std::vector<float> m_scratch;
    PrecipKind m_kind  = PrecipKind::None;
    uint32_t   m_live  = 0;
    uint32_t   m_rng   = 0xC0FFEEu;
    float      m_cx = 0.0f, m_cy = 0.0f, m_cz = 0.0f;
    float      m_t  = 0.0f;
    float      m_sky   = 1.0f;   // smoothed sky visibility, 0 = under a roof
};

// --test-precip. Headless, no device: asserts the volume stays camera-local
// (particles follow the camera rather than being left behind), that the wrap
// recycles instead of leaking, that snow flutters laterally while rain falls
// near-straight, that intensity scales COUNT and not speed, and that it is
// deterministic. Returns true on pass.
bool runPrecipSelfTest();

} // namespace x3::game
