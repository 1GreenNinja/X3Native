// entry_editor.cpp — X3LevelArchitect.exe, the Level Architect front-end.
//
// A LAUNCHER, not a fork. It injects "--editor" into argv and calls the exact
// same x3AppMain() that X3Engine.exe calls, in the same x3app.dll. There is no
// editor-specific host path here and there must never be one: the editor has to
// boot levels through the shipping code, or the two binaries drift and the split
// buys us nothing (docs/design/LEVEL_ARCHITECT_EXE_PLAN.md, condition D3).
//
// Everything else — worlds, self-tests, screenshots, --world fromdoc round-trip
// — still works from this exe, because it IS the same entry. Passing extra args
// on the command line composes normally.

#include "app_entry.h"

#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    // Forward argv verbatim, adding "--editor" unless the caller already asked
    // for it (parseCli would tolerate the duplicate, but an honest argv is
    // easier to read in a crash log).
    bool alreadyEditor = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--editor") == 0) { alreadyEditor = true; break; }
    }
    if (alreadyEditor) return x3AppMain(argc, argv);

    static char kEditorFlag[] = "--editor";

    std::vector<char*> args;
    args.reserve((size_t)argc + 2);
    args.push_back(argv[0]);          // exe path stays argv[0]
    args.push_back(kEditorFlag);      // the injection
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    args.push_back(nullptr);          // argv is null-terminated by standard

    return x3AppMain((int)args.size() - 1, args.data());
}
