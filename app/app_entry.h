#pragma once
// app_entry.h — the ONE entry point shared by every X3Native front-end.
//
// The host layer (app/, ~200 TUs: app_run.cpp, world_hosts/*, editor/*) is built
// as x3app.dll. Both executables are thin launchers over it:
//
//   X3Engine.exe          entry_game.cpp    -> x3AppMain(argc, argv)
//   X3LevelArchitect.exe  entry_editor.cpp  -> x3AppMain(argc, argv + "--editor")
//
// This is the point of the split. Today's two worst bugs (the tunnel lit in
// headless capture but black when driven; assetRoot() resolving differently per
// build layout) were HOST-layer divergence, not engine divergence — a DLL
// boundary drawn around engine/ alone would have caught neither. Drawing it
// around app/ as well makes it impossible for the editor to run different host
// code than the game, because there is exactly one compiled copy.
//
// See docs/design/LEVEL_ARCHITECT_EXE_PLAN.md.

// Defined in app/main.cpp. Exported from x3app.dll via
// CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS.
int x3AppMain(int argc, char** argv);
