#pragma once
// ============================================================================
// Boot-time async audio bring-up (#28 deep split, Phase C). The BootAudio struct
// + makeBootAudio() factory overlap the miniaudio device + WAV loads (~80-100ms)
// with the Vulkan device init. Moved VERBATIM out of main()'s body so it is
// shared between main() (which launches the std::async) and the default host
// (app/app_run.cpp, which joins/consumes it). Behaviour is byte-identical.
// ============================================================================

#include "engine/audio/IAudioSystem.h"
#include "audio_root.h"   // x3::game::resolveAudio
#include <memory>

namespace x3 { namespace apphost {

struct BootAudio {
    std::unique_ptr<x3::audio::IAudioSystem> audio;
    x3::audio::SoundHandle gun, door, pickup, death;
};

inline BootAudio makeBootAudio() {
    BootAudio ba;
    ba.audio.reset(x3::audio::createAudioSystem());
    ba.audio->init();
    ba.gun = ba.audio->load(x3::game::resolveAudio(
        "Sci-Fi_Guns_Game-Of-Weapons/Audio/SFX/Wave/Single_Gunshots/"
        "Single_Gunshot_Sci-Fi_Gun-01.wav"));
    ba.door = ba.audio->load(x3::game::resolveAudio(
        "ModularScifiInterior/Sound/S_ScifiDoor_A.WAV"));
    ba.pickup = ba.audio->load(x3::game::resolveAudio(
        "Sci-fi Evolution Gift Pack/Health or Energy Game Recharge 2.wav"));
    ba.death = ba.audio->load(x3::game::resolveAudio(
        "Free Pack/Explosion 1.wav"));
    return ba;
}

}} // namespace x3::apphost
