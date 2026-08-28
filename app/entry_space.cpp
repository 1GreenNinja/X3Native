// entry_space.cpp — X3Space.exe, the standalone space front-end.
//
// Owner, 2026-08-24: "Can you have the space bit as a standalone, unconnected to
// the game at large?" Offered a dev shortcut, a separate exe, or a separate
// product, he picked the separate exe carrying the full space roster. This file
// is that exe, and it is thirty lines, because THAT IS THE WHOLE POINT.
//
// A LAUNCHER, not a fork. It injects "--space" into argv and calls the exact
// same x3AppMain() that X3Engine.exe and X3LevelArchitect.exe call, in the same
// x3app.dll — the identical pattern entry_editor.cpp uses for "--editor"
// (docs/design/LEVEL_ARCHITECT_EXE_PLAN.md, condition D3). There is no
// space-specific host code here and there must never be any: every engine fix
// has to keep flowing to all three binaries, and it only does that while there
// is physically ONE compiled copy of the host layer. A "standalone space game"
// built by copying files would be a second codebase within a week.
//
// What the flag does, in full (app/cli.cpp, the epilogue): pick the default
// world (x3::game::spaceDefaultWorld() — a SPACE world, never canonlevel) and
// put the product's name in the window title. That is it. An explicit --world
// still wins, every self-test still runs, the console/pause menu/FPS overlay/F7
// tuning panel all come along because the space host wires the shared HostShell
// like every other world does.
//
// The roster it carries is not listed here either — it is read off the
// destination registry (app/destinations.h, "THE SPACE ROSTER": every row
// grouped DestGroup::Space), and gated by D14-D16 inside --test-rifthub. A hand
// list in this file would be the first thing to drift.
//
// Direct entry to the dreadnought encounter, for tuning how flying FEELS
// without paying for a cinematic first:  X3Space.exe --dogfight

#include "app_entry.h"

#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    // Forward argv verbatim, adding "--space" unless the caller already asked
    // for it (parseCli would tolerate the duplicate, but an honest argv is
    // easier to read in a crash log). Byte-for-byte the entry_editor.cpp shape.
    bool alreadySpace = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--space") == 0) { alreadySpace = true; break; }
    }
    if (alreadySpace) return x3AppMain(argc, argv);

    static char kSpaceFlag[] = "--space";

    std::vector<char*> args;
    args.reserve((size_t)argc + 2);
    args.push_back(argv[0]);         // exe path stays argv[0]
    args.push_back(kSpaceFlag);      // the injection
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    args.push_back(nullptr);         // argv is null-terminated by standard

    return x3AppMain((int)args.size() - 1, args.data());
}
