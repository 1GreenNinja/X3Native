#pragma once
// [P3-5] LOG-1 (docs/design/SUBSYSTEM_HARDENING_PLAN.md): the combat path logs
// per-event lines — "[monster] hit for N — HP now M", "[player] took N damage",
// "[ai] entity X Advance -> Attack" — that spam hundreds of lines into a real
// run. They are gated here behind two cvars, DEFAULT QUIET:
//   combat_log 1  -> player-hit + monster-hit/kill/headshot lines
//   ai_log 1      -> [ai] state-transition lines
// app_run registers the cvars (console.registerCVar) and pushes their values
// into these flags each frame; everything is preserved for debugging — flip the
// cvar and the full stream returns. Headless self-tests assert on return
// values, not log text, so they run quiet by default. Header-only (C++17
// inline variables): any app/ file can gate a line with no new link deps.

namespace x3::game {

inline bool g_combatLogEnabled = false;  // combat_log — player/monster hit lines
inline bool g_aiLogEnabled     = false;  // ai_log     — [ai] state transitions

inline void setCombatLogEnabled(bool on) { g_combatLogEnabled = on; }
inline void setAiLogEnabled(bool on)     { g_aiLogEnabled     = on; }
inline bool combatLogEnabled()           { return g_combatLogEnabled; }
inline bool aiLogEnabled()               { return g_aiLogEnabled; }

} // namespace x3::game
