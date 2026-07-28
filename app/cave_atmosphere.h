#pragma once
// CAVE / TUNNEL ATMOSPHERE (feat/cave-atmosphere) — the runtime that turns the
// 800 m fall-shaft + side-shoot cave system into a living, eerie, crystal-lit
// underworld. The GEOMETRY (shaft, strata walls, lumpy side-shoot TUBES, biomes,
// Salvari shrine, house-sized landmark crystal, branches) is authored in
// club_bedrock.cpp::buildEarthTunnels; THIS module drives the per-frame ATMOSPHERE
// on top of it:
//
//   #1 CRYSTAL-ONLY LIGHTING: while the camera is in the caves, the club's global
//      ambient + IBL fill is dropped to near-zero so the ONLY light is the glowing
//      blue Salvari crystals (eerie pools in the dark, doubling as wayfinding). The
//      club's own violet ambient is RESTORED the instant you step back inside it.
//   #2 CLUB BASS FROM BELOW + CRYSTALS PULSING: the crystals BREATHE to the club's
//      beat clock (Club1127World::beatThump), and LOUDER/brighter the deeper you are
//      — the party throbbing up through the rock from beneath your feet, luring you
//      down. (The felt bass + singing hum are the audio half, below.)
//   #5 VOLUMETRIC FOG: a dark-blue depth-fog haze hangs in the caverns so the crystal
//      light throws shafts through it (reuses IRenderDevice::setFog).
//
// AUDIO (live path only; headless = graceful silence): a low BASS-THUMP emitter at
// the club floor whose volume rises as you descend, + "singing crystal" resonant
// hums near the nearest crystals, + a cave room REVERB. All from procedurally-
// generated tone WAVs (no pack asset needed). See cave_atmosphere.cpp.

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/audio/IAudioSystem.h"
#include "club_bedrock.h"          // DescentFallLayout

#include <vector>

namespace x3::game {

class Club1127World;   // for the beat clock (fwd-decl; host passes beatThump)

class CaveAtmosphere {
public:
    struct State {
        bool  inCave  = false;   // camera is in the shaft / side-shoot cave zone
        float depth01 = 0.0f;    // 0 at the surface mouth .. 1 at the club floor
        float blend   = 0.0f;    // smoothed crystal-only weight (0 club .. 1 cave)
    };

    // Configure the cave band from the descent layout + club footprint.
    void configure(const DescentFallLayout& L, float clubMaxX,
                   float clubFloorY, float clubCeilY, float mouthY);

    // Remember the club's OWN look (host's ambient + IBL) so we RESTORE it out of the
    // caves. Call after the host applies its club atmosphere.
    void setClubLook(float ambR, float ambG, float ambB, float iblIntensity);

    // Pure classifier: is world (x,y) in the cave zone, and how deep is it?
    State classify(float x, float y) const;

    // Per-frame visual drive: blend ambient/IBL/fog between the club look and the
    // crystal-only cave look by camera position (smoothed). Returns the state (the
    // host uses depth01 + inCave for the crystal pulse + audio).
    State update(float dt, const x3::phys::Vec3& cam, x3::rhi::IRenderDevice& device);

    // Beat-pulse brightness multiplier for a crystal at cave-depth `depth01` given the
    // club beat envelope (0 rest .. 1 kick): 1 at rest, swelling toward the kick,
    // stronger the deeper you are. Static + pure so the host can apply it in pushLights.
    static float crystalPulse(float beatThump, float depth01);

    // Is this point light a (pulseable) blue Salvari crystal? (blue channel dominant)
    static bool isCrystal(const x3::rhi::PointLight& l);

    // ---- AUDIO (live path) ----
    // Bind the audio system + generate the procedural bass/hum tones. Safe/no-op if
    // audio is null or the device is silent.
    void bindAudio(x3::audio::IAudioSystem* audio, float clubFloorY,
                   float shaftX, float shaftZ);
    // Drive the bass-from-below emitter (volume by depth + beat) + singing-crystal
    // hum + cave reverb each frame. Graceful no-op when unbound.
    void updateAudio(const x3::phys::Vec3& cam, const State& st, float beatThump);
    void shutdownAudio();

    // Headless self-test (--test-caveatmos): the classifier + pulse + fog band logic.
    static bool runSelfTest();

private:
    // cave band
    float m_clubMaxX  = 0.0f, m_clubFloorY = 0.0f, m_clubCeilY = 0.0f;
    float m_mouthY    = 0.0f, m_shaftBotY  = 0.0f;   // shaftBotY = landing-room ceiling (top of infra)
    // look to restore
    float m_ambR = 0.0f, m_ambG = 0.0f, m_ambB = 0.0f, m_ibl = 0.2f;
    float m_blend = 0.0f;
    bool  m_first = true;
    // audio
    x3::audio::IAudioSystem* m_audio = nullptr;
    x3::audio::SoundHandle   m_bassSnd{};
    x3::audio::SoundHandle   m_humSnd{};
    x3::audio::LoopHandle    m_bassLoop{};
    x3::audio::LoopHandle    m_humLoop{};
    float m_clubFloorY_a = 0.0f, m_shaftX = 0.0f, m_shaftZ = 0.0f;
};

} // namespace x3::game
