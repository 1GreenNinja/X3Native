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
    // AUDIO-ASSETS PASS (W2-B, 2026-07-05): repointed to the curated, COMMITTED
    // mirror under assets/audio/enemies/ (see AUDIO_MANIFEST.md) so these resolve
    // on a fresh clone with no D:/G: external mount — the same portability win the
    // per-weapon WAVs already got. resolveAudio() still tries the repo-local path
    // first and falls through to the (now nonexistent-on-this-box) external roots
    // if the repo file is ever missing, so behaviour degrades gracefully rather
    // than regressing on any machine that still has the old external packs.
    x3::audio::SoundHandle enemyTaunt, enemyAttack, enemyHit, enemyDeath;
    // Footstep resolution (audio-assets pass, W2-B). Previously footsteps had no
    // dedicated WAV at all — the host aliased sndStep = sndGun (a pitched-down
    // gunshot). Four varied hard-surface takes are committed under
    // assets/audio/footsteps/; the host should pick one per step (e.g. round-robin
    // or random) instead of the gunshot alias. See docs/design/WAVE2_PUNCHLIST.md
    // item 10 and the note in app_run.cpp next to `sndStep = sndGun` — replacing
    // that alias is the director's one-line change (app_run.cpp is out of scope
    // for this pass; boot_audio.h only loads the handles here).
    x3::audio::SoundHandle footstepConcrete[4];
    // Player pain / landing audio hooks (audio-assets pass, W2-B). Player emits
    // CueKind::PlayerPain (see Player::takeDamage) and CueKind::PlayerLand (see
    // Player::update's airborne->grounded edge); these are the handles a wired
    // cue sink would play. Two pain takes so back-to-back hits don't repeat the
    // exact same grunt. Loading here is the sound department's part; the host
    // still needs the one-line `player.setCueSink(...)` subscription (out of
    // scope: that line lives in app_run.cpp).
    x3::audio::SoundHandle playerPain[2];
    x3::audio::SoundHandle playerLand;
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
    // Enemy creature vocalizations: committed repo-local mirror (assets/audio/enemies/),
    // resolved first; external per-machine pack roots remain as the fallback chain
    // inside resolveAudio() itself (see audio_root.h).
    ba.enemyTaunt  = ba.audio->load(x3::game::resolveAudio("enemies/taunt.wav"));
    ba.enemyAttack = ba.audio->load(x3::game::resolveAudio("enemies/attack.wav"));
    ba.enemyHit    = ba.audio->load(x3::game::resolveAudio("enemies/hit.wav"));
    ba.enemyDeath  = ba.audio->load(x3::game::resolveAudio("enemies/death.wav"));
    // Footsteps: 4 varied concrete/hard-surface takes, committed repo-local.
    ba.footstepConcrete[0] = ba.audio->load(x3::game::resolveAudio("footsteps/step_concrete_1.wav"));
    ba.footstepConcrete[1] = ba.audio->load(x3::game::resolveAudio("footsteps/step_concrete_2.wav"));
    ba.footstepConcrete[2] = ba.audio->load(x3::game::resolveAudio("footsteps/step_concrete_3.wav"));
    ba.footstepConcrete[3] = ba.audio->load(x3::game::resolveAudio("footsteps/step_concrete_4.wav"));
    // Player pain (2 takes) + landing thud, committed repo-local.
    ba.playerPain[0] = ba.audio->load(x3::game::resolveAudio("player/pain_1.wav"));
    ba.playerPain[1] = ba.audio->load(x3::game::resolveAudio("player/pain_2.wav"));
    ba.playerLand    = ba.audio->load(x3::game::resolveAudio("player/land.wav"));
    return ba;
}

}} // namespace x3::apphost
