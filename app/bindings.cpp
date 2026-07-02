// ===========================================================================
// D14 SCRIPT BOOT + GAME BINDINGS — host side (factored from app/main.cpp,
// #28 monolith split). Bodies moved VERBATIM; only the enclosing namespace
// changed (anonymous/static file scope -> x3::apphost) so main.cpp + the
// --test-hatch self-test can share them.
// ===========================================================================
#include "bindings.h"

#include "engine/core/x3_log.h"
#include "asset_root.h"
#include "level1_game.h"
#include "holo_terminal.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace x3::apphost {

// Load every scripts/*.lua found under the asset root (or repo root) into the
// script system — the exact boot-load the app performs. Returns count loaded.
int loadBootScripts(x3::script::IScriptSystem& scripts) {
    namespace fs = std::filesystem;
    std::error_code sec;
    // scripts/ lives at the repo root (peer to assets/). Resolve via the
    // asset root's parent, then fall back to ./scripts for repo-root runs.
    const fs::path candidates[] = {
        fs::path(x3::game::assetRoot()).parent_path() / "scripts",
        fs::path("scripts"),
    };
    int loaded = 0;
    for (const fs::path& dir : candidates) {
        if (!fs::is_directory(dir, sec)) continue;
        for (const auto& ent : fs::directory_iterator(dir, sec)) {
            if (!ent.is_regular_file() || ent.path().extension() != ".lua") continue;
            std::ifstream f(ent.path(), std::ios::binary);
            if (!f) continue;
            std::string src((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
            const std::string name = ent.path().filename().string();
            if (scripts.load(name, src) != x3::script::kInvalidScript) ++loaded;
        }
        break; // first existing scripts dir wins
    }
    return loaded;
}

// D14 trigger/objective bindings (app-side, per the D14 handoff: the engine
// ships a generic registerFunction(); the APP wires the real game systems here
// so engine/ never learns about doors/objectives). A SMALL, focused surface —
// just the trigger/secret-room mechanic. `game` (the Level1Game) owns the
// objective HUD line, the DoorSystem (doors A-E + the secret-room floor hatch),
// and the secret room. Captured by pointer; these are only called during
// scripts->update()/fire() while `game` is alive.
void registerGameBindings(x3::script::IScriptSystem& scripts,
                          x3::game::Level1Game& game) {
    using x3::script::ScriptValue;
    x3::game::Level1Game* gp = &game;

    // x3.setObjective(text) -> the GTA-style under-minimap objective line.
    scripts.registerFunction("setObjective",
        [gp](const std::vector<ScriptValue>& a) -> ScriptValue {
            if (!a.empty()) gp->objectives().setText(a[0].asString());
            return ScriptValue();
        });

    // x3.openDoor(id) / x3.closeDoor(id) -> the DoorSystem by door index.
    // Idempotent (startOpening/toggle no-op when already in that state).
    scripts.registerFunction("openDoor",
        [gp](const std::vector<ScriptValue>& a) -> ScriptValue {
            if (a.empty()) return ScriptValue(false);
            uint32_t i = (uint32_t)a[0].asInt();
            if (i >= gp->doors().count()) return ScriptValue(false);
            x3::game::Door& d = gp->doors().at(i);
            gp->doors().unlock(d);                 // scripted opens bypass the lock
            return ScriptValue(gp->doors().startOpening(d));
        });
    scripts.registerFunction("closeDoor",
        [gp](const std::vector<ScriptValue>& a) -> ScriptValue {
            if (a.empty()) return ScriptValue(false);
            uint32_t i = (uint32_t)a[0].asInt();
            if (i >= gp->doors().count()) return ScriptValue(false);
            x3::game::Door& d = gp->doors().at(i);
            // toggle() only closes an open door; refuse if already closed/closing.
            if (d.state == x3::game::DoorState::Closed ||
                d.state == x3::game::DoorState::Closing) return ScriptValue(false);
            return ScriptValue(gp->doors().toggle(d));
        });
    // x3.setDoorState(id, open) -> explicit open/close (convenience over the two above).
    scripts.registerFunction("setDoorState",
        [gp](const std::vector<ScriptValue>& a) -> ScriptValue {
            if (a.size() < 2) return ScriptValue(false);
            uint32_t i = (uint32_t)a[0].asInt();
            if (i >= gp->doors().count()) return ScriptValue(false);
            x3::game::Door& d = gp->doors().at(i);
            if (a[1].asBool()) { gp->doors().unlock(d); return ScriptValue(gp->doors().startOpening(d)); }
            if (d.state == x3::game::DoorState::Closed ||
                d.state == x3::game::DoorState::Closing) return ScriptValue(false);
            return ScriptValue(gp->doors().toggle(d));
        });

    // x3.openTrapdoor() -> the secret-room floor hatch (DoorSpec.floorHatch).
    // The hatch lives in the same DoorSystem; index from the secret room. The
    // id arg is optional/ignored (there's one hatch); accepted for symmetry.
    scripts.registerFunction("openTrapdoor",
        [gp](const std::vector<ScriptValue>&) -> ScriptValue {
            if (!gp->secret().hatchBuilt()) return ScriptValue(false);
            uint32_t i = gp->secret().hatchDoorIndex();
            if (i >= gp->doors().count()) return ScriptValue(false);
            x3::game::Door& d = gp->doors().at(i);
            gp->doors().unlock(d);
            return ScriptValue(gp->doors().startOpening(d));
        });

    x3::logInfo("D14 script bindings: setObjective/openDoor/closeDoor/"
                "setDoorState/openTrapdoor wired to Level1Game");
}

// The cell-terminal Enter glue: fire the typed code INTO Lua before submit()
// clears the input line (scripts/secret_room.lua listens for terminal_code and
// on the secret code calls x3.openTrapdoor()+x3.setObjective()), then run the
// terminal's own submit sink (idempotent with the script). Factored so the
// in-game Enter handler and --test-hatch run the SAME keypad->fire link.
bool submitTerminalToScripts(x3::script::IScriptSystem* scripts,
                             x3::game::HoloTerminal& term) {
    const std::string entered = term.input();
    if (scripts) scripts->fire("terminal_code", {{"code", entered}});
    return term.submit();   // fires the sink -> opens the trapdoor on the code
}

} // namespace x3::apphost
