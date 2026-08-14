// entry_game.cpp — X3Engine.exe, the shipping game front-end.
//
// Deliberately trivial. Every line of host logic lives in x3app.dll so that the
// game and the Level Architect cannot diverge (see app/app_entry.h).
//
// If you are about to add anything to this file: don't. It belongs in the host
// layer, where BOTH front-ends get it. That is the entire point of the split.

#include "app_entry.h"

int main(int argc, char** argv) {
    return x3AppMain(argc, argv);
}
