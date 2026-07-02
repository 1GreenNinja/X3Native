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
    // Enemy VOCALIZATIONS (enemy-SFX pass): taunt/attack/take-hit/death creature
    // sounds resolved from the purchased packs (graceful: an absent WAV loads to an
    // invalid handle and just plays silent). The host wires these onto the new
    // EnemyTaunt/EnemyAttack/EnemyHit/EnemyDeath cue kinds so enemies have a voice.
    x3::audio::SoundHandle enemyTaunt, enemyAttack, enemyHit, enemyDeath;
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
    // Enemy creature vocalizations (resolved from the sci-fi packs; silent if absent).
    ba.enemyAttack = ba.audio->load(x3::game::resolveAudio(
        "Free Pack/Monster Bite.wav"));
    ba.enemyHit = ba.audio->load(x3::game::resolveAudio(
        "Sci-fi Evolution Gift Pack/Alien Game Tech Hit.wav"));
    ba.enemyTaunt = ba.audio->load(x3::game::resolveAudio(
        "Sci-fi Evolution Gift Pack/Alien Egg Sac Open 1.wav"));
    ba.enemyDeath = ba.audio->load(x3::game::resolveAudio(
        "Sci-fi Evolution Gift Pack/Fictional Game Goo Kill Smash 3.wav"));
    return ba;
}

}} // namespace x3::apphost
