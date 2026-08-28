@echo off
REM ============================================================================
REM  X3 SPACE - the standalone space game. Double-click this from Explorer.
REM
REM  Owner, 2026-08-24: "Can you have the space bit as a standalone, unconnected
REM  to the game at large?"  This is that front door. It opens in the flight
REM  arena over Kethzar Prime - no facility, no cell, no cold open. The rest of
REM  the space roster (the ship interior, the deep-space station, the wormhole,
REM  the tractor capture, the cockpit) is on the world menu, same as always.
REM
REM  Running from Explorer means the game is owned by Windows, not by a Claude
REM  Code session, so nothing reaps it: it stays open until YOU close it (menu
REM  Quit, or the window X).
REM
REM  DEV SHORTCUT - straight into the dreadnought fight, no cinematic:
REM      "Play X3 Space.bat" --dogfight
REM  (that is the flag that makes tuning how flying FEELS cost one launch
REM  instead of one full intro playthrough).
REM
REM  HARD RULE, and the reason this file looks the way it does: a play-dir
REM  launcher NEVER points at a build tree. %~dp0 is THIS directory, the deploy
REM  directory - the exe next to this file, not one in build\bin\Release that a
REM  rebuild can delete out from under the shortcut. Same as Play X3.bat and
REM  Play Echo Harbor.bat.
REM
REM  CWD guard, same as its siblings: several world/region loaders are
REM  CWD-relative (assets/world/regions.json, assets/districts/districts.txt,
REM  map_pois.json) and the dialog resolver reads staging/ and
REM  docs/design/narrative/chat_trees/ relative to it too.
REM ============================================================================
cd /d "%~dp0"

if not exist "%~dp0X3Space.exe" (
  echo.
  echo   X3Space.exe is not in this directory yet:
  echo       %~dp0
  echo.
  echo   Deploy a GATED build into this play dir first - X3Space.exe ships
  echo   alongside X3Play.exe / X3LevelArchitect.exe and the SAME x3app.dll.
  echo   All three exes are thin launchers over that one DLL, so a mismatched
  echo   pair boots the wrong game rather than failing loudly.
  echo.
  pause
  exit /b 1
)

start "" "%~dp0X3Space.exe" %*
