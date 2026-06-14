#pragma once
// ===========================================================================
// D14 SCRIPT BOOT + GAME BINDINGS (host side).
//
// Factored out of app/main.cpp (#28 monolith split) VERBATIM. The engine ships
// a generic registerFunction(); the APP wires the real game systems here so
// engine/ never learns about doors/objectives. Declared here so BOTH the live
// game host (main.cpp) AND the headless --test-hatch chain self-test exercise
// the IDENTICAL script loading + binding registration (no drift).
// ===========================================================================
#include "engine/script/IScriptSystem.h"

namespace x3 { namespace game { class Level1Game; class HoloTerminal; } }

namespace x3::apphost {

// Load every scripts/*.lua found under the asset root (or repo root) into the
// script system — the exact boot-load the app performs. Returns count loaded.
int loadBootScripts(x3::script::IScriptSystem& scripts);

// D14 trigger/objective bindings (app-side). Wires setObjective / openDoor /
// closeDoor / setDoorState / openTrapdoor to the live Level1Game.
void registerGameBindings(x3::script::IScriptSystem& scripts,
                          x3::game::Level1Game& game);

// The cell-terminal Enter glue: fire the typed code INTO Lua before submit()
// clears the input line, then run the terminal's own submit sink.
bool submitTerminalToScripts(x3::script::IScriptSystem* scripts,
                             x3::game::HoloTerminal& term);

} // namespace x3::apphost
