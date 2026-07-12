// Game-feel CUE hooks. See app/cues.h.
//
// Clean-room: just a tiny dispatch + a throttled-log fallback. No engine audio
// dependency (that's the whole point — the host wires the sink).
#include "cues.h"

#include "engine/core/x3_log.h"

#include <string>

namespace x3::game {

const char* cueKindName(CueKind k) {
    switch (k) {
        case CueKind::Footstep:     return "footstep";
        case CueKind::BulletImpact: return "bullet-impact";
        case CueKind::MeleeImpact:  return "melee-impact";
        case CueKind::EnemyTaunt:   return "enemy-taunt";
        case CueKind::EnemyAttack:  return "enemy-attack";
        case CueKind::EnemyHit:     return "enemy-hit";
        case CueKind::EnemyDeath:   return "enemy-death";
        case CueKind::PlayerPain:   return "player-pain";
        case CueKind::PlayerLand:   return "player-land";
        case CueKind::PlayerSplash: return "player-splash";
    }
    return "?";
}

void emitCueOrLog(const GameCueFn& sink, const GameCue& cue) {
    if (sink) { sink(cue); return; }   // host handles it (audio/FX) — no log spam.

    // No sink wired: emit a THROTTLED log so the trigger point is observable in
    // headless tests / console without a 60 Hz footstep stream flooding output.
    // One static counter per kind; log every Nth fall-through. The throttle is a
    // dev convenience for the no-audio path only (the real path never logs here).
    static uint32_t s_count[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    const uint32_t i = (uint32_t)cue.kind < 10u ? (uint32_t)cue.kind : 0u;
    constexpr uint32_t kEvery = 8;     // log 1 in 8 fall-through cues per kind
    if ((s_count[i]++ % kEvery) == 0) {
        x3::logInfo(std::string("[cue] ") + cueKindName(cue.kind) +
                    " @ (" + std::to_string(cue.pos.x) + "," +
                    std::to_string(cue.pos.y) + "," + std::to_string(cue.pos.z) + ")" +
                    " i=" + std::to_string(cue.intensity) + " [no sink — stub log]");
    }
}

} // namespace x3::game
