#pragma once
// ============================================================================
// ENGINE CONSOLE — the ONE shared command/cvar registry (D-CONSOLE fold).
//
// Owner, twice in one day: "we are MISSING ALL THE CONSOLE COMMANDS FROM THE
// GAME!!!" / "do we have the console with EVERY COMMAND ACTIVE". His
// screenshot: typing noclip/idclip into a --world host's console -> "unknown".
//
// THE BUG THIS CLOSES: app_run.cpp (the campaign/default host) registered its
// whole cvar + cheat-command catalog on ITS OWN x3::con::IConsole instance,
// built and populated once, deep inside runDefaultHost(). Every --world host
// (tunnel, drive, ...) that stood up its own console (or none at all) started
// from EMPTY — `help` showed nothing, `noclip` was unrecognized, `r_exposure`
// didn't exist to type. Two call sites hand-copying ~90 cvars would just drift
// again (NO_SLOP rule 4: one registry, no drift) — so this is the ONE place.
//
// registerEngineConsoleCVars() is pure data: renderer/tuning cvars (r_*,
// vm_*, grip_*, combat_log/ai_log, boot_budget_ms, ...). No campaign state,
// safe to call on ANY console (campaign or a bare --world host).
//
// registerEngineConsoleCommands() is the cheat/utility command set (noclip /
// idclip, god, iddqd, idkfa, idfa, vigil_link, intro_play, flightmode,
// restart) plus a grouped, paged 'help'. Most of the cheats need live
// CAMPAIGN state (the Player entity, the weapon Arsenal, chat-tree story
// flags, ...) that a bare --world host does not have. EngineConsoleHooks
// carries optional std::function callbacks for exactly that state; leave a
// hook null and the command still EXISTS (never "unknown: idkfa") but prints
// "<cmd>: campaign only" instead of doing anything — the owner's second
// complaint was commands going missing outright, and a stub is not missing.
//
// noclip/idclip is NOT optional. Every caller supplies a real toggle: the
// campaign wires it to Player::setNoclip (unchanged), a --world host wires it
// to its own HostShell freefly camera (app/world_hosts/host_shell.h). A
// working freefly is deliverable #3 of this fold, not a nice-to-have.
// ============================================================================
#include "engine/core/IConsole.h"

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace x3::game {

struct EngineConsoleHooks {
    // ---- REQUIRED — every caller supplies a real toggle. ----
    std::function<void(bool)> setNoclip;
    std::function<bool()>     getNoclip;

    // ---- OPTIONAL — null = the command still exists, and prints "campaign
    // only" instead of acting. ----
    std::function<void(bool)> setGod;
    std::function<bool()>     getGod;
    std::function<void()>     healPlayer;
    std::function<void()>     armAllWeapons;        // idfa / idkfa: hand every weapon over
    std::function<void(bool)> setInfiniteAmmo;       // idfa / idkfa
    std::function<void(bool)> vigilLink;             // vigil_link [0|1]
    std::function<void()>     introPlay;             // intro_play
};

// The pure-data cvar catalog (renderer + tuning). Safe on any console; no
// hooks needed. (Formerly app_run.cpp's file-local registerViewmodelCVars —
// same body, same defaults, byte-identical registration order.)
void registerEngineConsoleCVars(x3::con::IConsole& console);

// The cheat/utility command set + the grouped/paged 'help'. `window` (may be
// null in headless contexts) backs the generic 'restart' command; `hooks`
// supplies the campaign-only wiring (leave fields null for a bare host).
void registerEngineConsoleCommands(x3::con::IConsole& console, GLFWwindow* window,
                                   const EngineConsoleHooks& hooks);

// Both of the above, in order — the one call every console owner makes.
void registerEngineConsole(x3::con::IConsole& console, GLFWwindow* window,
                           const EngineConsoleHooks& hooks);

// Headless self-test (--test-engineconsole): registers the shared set with a
// stub hook set (real noclip only, everything else absent -> "campaign only"
// stubs), then execs r_exposure / noclip / help and asserts none of it
// crashes or falls through to "unknown: <cmd>". See app/engine_console.cpp.
bool runEngineConsoleSelfTest();

} // namespace x3::game
